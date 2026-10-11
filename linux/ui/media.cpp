#include "media.hpp"
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QApplication>
#include <QCache>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <sys/stat.h>

static bool videoContainer(const QString &path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  return suffix == "mp4" || suffix == "webm";
}

static QString videoIdentity(const QString &path) {
  struct stat info;
  if (::lstat(QFile::encodeName(path).constData(), &info) != 0 ||
      !S_ISREG(info.st_mode) || info.st_size == 0)
    return {};
  return path + ':' + QString::number(info.st_ino) + ':' + QString::number(info.st_size) +
      ':' + QString::number(info.st_mtim.tv_sec) + ':' + QString::number(info.st_mtim.tv_nsec);
}

class VideoPreviews : public QObject {
  struct Job { QString path, identity; };
  QCache<QString, QImage> cache{16};
  QList<Job> queue;
  QSet<QString> pending;
  QProcess process;
  QTimer timeout;
  QByteArray bytes;
  Job current;
  QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
  bool busy = false, firstFrame = false;

  void complete(const QImage &image) {
    timeout.stop();
    if (videoIdentity(current.path) == current.identity)
      cache.insert(current.identity, new QImage(image));
    pending.remove(current.identity);
    busy = false;
    ++revision;
    QTimer::singleShot(0, this, [this] { next(); });
  }

  void extract() {
    bytes.clear();
    process.start(ffmpeg, {"-v", "error", "-nostdin", "-threads", "1",
        "-protocol_whitelist", "file,pipe", "-f",
        QFileInfo(current.path).suffix().compare("mp4", Qt::CaseInsensitive) == 0 ? "mov" : "matroska",
        "-ss", firstFrame ? "0" : "0.5", "-i", current.path,
        "-map", "0:v:0", "-frames:v", "1", "-an", "-sn", "-dn",
        "-filter_threads", "1", "-vf", "scale=600:420:force_original_aspect_ratio=decrease",
        "-threads", "1", "-c:v", "png", "-f", "image2pipe", "pipe:1"});
    timeout.start(10000);
  }

  void next() {
    if (busy || queue.isEmpty()) return;
    current = queue.takeFirst();
    busy = true;
    firstFrame = false;
    if (videoIdentity(current.path) != current.identity) {
      complete({});
      return;
    }
    extract();
  }

public:
  quint64 revision = 0;
  VideoPreviews() : QObject(qApp) {
    process.setStandardErrorFile(QProcess::nullDevice());
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, this, [this] { process.kill(); });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
      bytes += process.readAllStandardOutput();
      if (bytes.size() > 2 * 1024 * 1024) process.kill();
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) complete({});
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int code, QProcess::ExitStatus status) {
      timeout.stop();
      bytes += process.readAllStandardOutput();
      QImage image;
      if (code == 0 && status == QProcess::NormalExit && bytes.size() <= 2 * 1024 * 1024)
        image.loadFromData(bytes, "PNG");
      // A recording shorter than the seek offset still gets its first frame.
      if (image.isNull() && code == 0 && status == QProcess::NormalExit && !firstFrame) {
        firstFrame = true;
        extract();
      } else complete(image);
    });
  }
  ~VideoPreviews() override {
    process.blockSignals(true);
    if (process.state() != QProcess::NotRunning) {
      process.kill();
      process.waitForFinished(1000);
    }
  }
  QImage request(const QString &path) {
    const QString identity = videoIdentity(path);
    if (identity.isEmpty()) return {};
    if (auto image = cache.object(identity)) return *image;
    if (!ffmpeg.isEmpty() && !pending.contains(identity) && pending.size() < 16) {
      pending.insert(identity);
      queue.append({path, identity});
      next();
    }
    return {};
  }
};

static VideoPreviews &videoPreviews() {
  static auto previews = new VideoPreviews;
  return *previews;
}

quint64 capturePreviewRevision() { return videoPreviews().revision; }

static bool readableVideo(const QString &path) {
  QFile file(path);
  if (videoIdentity(path).isEmpty() || !file.open(QIODevice::ReadOnly)) return false;
  const QByteArray header = file.read(12);
  return QFileInfo(path).suffix().compare("mp4", Qt::CaseInsensitive) == 0
      ? header.size() >= 12 && header.mid(4, 4) == "ftyp"
      : header.startsWith(QByteArray::fromHex("1a45dfa3"));
}

static void playBadge(QImage &image) {
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing);
  const qreal size = qMin(image.width(), image.height()) * .28;
  painter.translate(image.width() - size * .7, image.height() - size * .7);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(37, 55, 77, 220));
  painter.drawEllipse(QPointF(0, 0), size / 2, size / 2);
  painter.setBrush(Qt::white);
  QPainterPath play;
  play.moveTo(-size * .1, -size * .2);
  play.lineTo(size * .22, 0);
  play.lineTo(-size * .1, size * .2);
  play.closeSubpath();
  painter.drawPath(play);
}

bool captureValid(const QString &path) {
  // Validation does not enqueue thumbnails for the entire watched history.
  return videoContainer(path) ? readableVideo(path) : !capturePreview(path, QSize(480, 480)).isNull();
}

bool isVideoCapture(const QString &path) {
  if (videoContainer(path))
    return true;
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (suffix != "gif" && suffix != "webp")
    return false;
  QImageReader reader(path);
  return reader.supportsAnimation() && reader.imageCount() > 1;
}

QImage capturePreview(const QString &path, QSize bounds) {
  if (videoContainer(path)) {
    if (!readableVideo(path)) return {};
    QImage preview = videoPreviews().request(path);
    if (!preview.isNull()) {
      preview = preview.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      playBadge(preview);
      return preview;
    }
    QImage image(QSize(480, 320).scaled(bounds, Qt::KeepAspectRatio), QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor("#e8edf3"));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(image.width() / 480.0, image.height() / 320.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#354d68"));
    painter.drawRoundedRect(QRectF(198, 68, 84, 84), 24, 24);
    painter.setBrush(Qt::white);
    QPainterPath play;
    play.moveTo(230, 91);
    play.lineTo(256, 110);
    play.lineTo(230, 129);
    play.closeSubpath();
    painter.drawPath(play);
    painter.setPen(QColor("#25374d"));
    QFont font = painter.font();
    font.setPixelSize(30);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRectF(20, 174, 440, 42), Qt::AlignCenter, "Recording");
    font.setPixelSize(25);
    font.setBold(false);
    painter.setFont(font);
    const QString name = painter.fontMetrics().elidedText(QFileInfo(path).fileName(), Qt::ElideMiddle, 432);
    painter.drawText(QRectF(24, 223, 432, 38), Qt::AlignCenter, name);
    return image;
  }
  QImageReader reader(path);
  const QSize size = reader.size();
  if (size.isValid())
    reader.setScaledSize(size.scaled(bounds, Qt::KeepAspectRatio));
  reader.setAutoTransform(true);
  QImage image = reader.read();
  if (!image.isNull() && isVideoCapture(path)) playBadge(image);
  return image;
}
