#include "media.hpp"
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>

static bool videoContainer(const QString &path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  return suffix == "mp4" || suffix == "webm";
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
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
      return {};
    // Recognize the container without loading or decoding a potentially large
    // recording on the UI thread. Playback belongs to the default player.
    const QByteArray header = file.read(12);
    const bool mp4 = QFileInfo(path).suffix().compare("mp4", Qt::CaseInsensitive) == 0;
    if (mp4 ? (header.size() < 12 || header.mid(4, 4) != "ftyp")
            : !header.startsWith(QByteArray::fromHex("1a45dfa3")))
      return {};
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
  if (!image.isNull() && isVideoCapture(path)) {
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal size = qMin(image.width(), image.height()) * .4;
    painter.translate(image.width() / 2.0, image.height() / 2.0);
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
  return image;
}
