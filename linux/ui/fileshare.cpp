#include "fileshare.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qint64 maxBytes = qint64(4) * 1024 * 1024 * 1024;
constexpr qint64 maxReplyBytes = 256 * 1024;
constexpr int historyLimit = 10;
bool regular(const QString &path) {
  struct stat info {};
  auto native = QFile::encodeName(path);
  return ::lstat(native.constData(), &info) == 0 && S_ISREG(info.st_mode) &&
         info.st_size > 0 && info.st_size <= maxBytes;
}
}

bool FileShare::validEndpoint(const QUrl &url) {
  // HTTP is accepted only on loopback for the local integration fixture.
  bool transport = url.scheme() == "https" ||
      (url.scheme() == "http" && (url.host() == "127.0.0.1" || url.host() == "::1"));
  return url.isValid() && transport && !url.host().isEmpty() &&
      url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() &&
      (url.path().isEmpty() || url.path() == "/");
}

bool FileShare::accepts(const QList<QUrl> &urls) {
  return !urls.isEmpty() && std::all_of(urls.begin(), urls.end(), [](const QUrl &url) {
    return url.isLocalFile() && url.host().isEmpty() && regular(url.toLocalFile());
  });
}

FileShare::FileShare(QUrl endpoint, QString history, QObject *parent)
    : QObject(parent), base(std::move(endpoint)), historyPath(std::move(history)) {
  if (validEndpoint(base)) base.setPath("");
  else base = QUrl();
  network.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
  QFile stored(historyPath);
  if (stored.open(QIODevice::ReadOnly) && stored.size() <= maxReplyBytes) {
    auto rows = QJsonDocument::fromJson(stored.readAll()).array();
    for (const auto &row : rows)
      if (validReceipt(row.toObject()) && recent.size() < historyLimit)
        recent.append(row);
  }
  if (!recent.isEmpty()) {
    confirmed = recent.first().toObject();
    activeName = confirmed["original_name"].toString();
    label = "Uploaded";
  }
}

QNetworkRequest FileShare::request(const QString &path) const {
  QNetworkRequest request(base.resolved(QUrl(path)));
  request.setTransferTimeout(30000);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  return request;
}

void FileShare::changed() {
  if (onChanged) onChanged();
}

void FileShare::enqueue(const QList<QUrl> &urls) {
  if (!enabled() || !accepts(urls) || pending.size() + urls.size() > 100) {
    description = "Choose regular local files from 1 byte to 4 GiB (up to 100 queued).";
    if (!active) {
      label = "Cannot upload";
      confirmed = {};
      activeName.clear();
    }
    changed();
    return;
  }
  for (const auto &url : urls) {
    QString path = url.toLocalFile();
    if ((!active || path != file.fileName()) && !pending.contains(path))
      pending.enqueue(path);
  }
  if (!active) next();
  else changed();
}

void FileShare::next() {
  if (active || pending.isEmpty()) return;
  file.setFileName(pending.dequeue());
  activeName = QFileInfo(file.fileName()).fileName();
  confirmed = {};
  active = true;
  posting = false;
  percent = -1;
  description.clear();
  auto path = QFile::encodeName(file.fileName());
  int descriptor = ::open(path.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  struct stat info {};
  if (descriptor < 0 || ::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size <= 0 || info.st_size > maxBytes) {
    if (descriptor >= 0) ::close(descriptor);
    settle("Cannot read file", "File changed or is no longer a regular readable file.");
    return;
  }
  if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
    ::close(descriptor);
    settle("Cannot read file", file.errorString());
    return;
  }
  label = "Connecting…";
  auto current = network.get(request("/upload.html"));
  reply = current;
  // Bound response buffering; uploaded file data is streamed directly from QFile.
  QObject::connect(current, &QNetworkReply::readyRead, this, [current] {
    if (current->bytesAvailable() > maxReplyBytes) current->abort();
  });
  QObject::connect(current, &QNetworkReply::finished, this, [this, current] {
    current->deleteLater();
    if (reply != current) return;
    reply.clear();
    auto body = current->read(maxReplyBytes + 1);
    QRegularExpression token("<meta\\s+name=[\"']csrf-token[\"']\\s+content=[\"']([A-Za-z0-9_-]+)[\"']");
    auto match = token.match(QString::fromUtf8(body));
    if (current->error() != QNetworkReply::NoError ||
        current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
        body.size() > maxReplyBytes || !match.hasMatch()) {
      settle("Cannot connect", "Check Tailscale, the FileShare address and owner access.");
      return;
    }
    post(match.captured(1).toLatin1());
  });
  changed();
}

void FileShare::post(const QByteArray &csrf) {
  auto upload = request("/api/uploads");
  upload.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
  upload.setHeader(QNetworkRequest::ContentLengthHeader, file.size());
  upload.setRawHeader("Origin", base.toEncoded());
  upload.setRawHeader("X-CSRF-Token", csrf);
  upload.setRawHeader("X-File-Name", QUrl::toPercentEncoding(activeName));
  upload.setRawHeader("X-File-Type", "application/octet-stream");
  upload.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
  posting = true;
  label = "Uploading…";
  percent = 0;
  auto current = network.post(upload, &file);
  reply = current;
  QObject::connect(current, &QNetworkReply::uploadProgress, this, [this, current](qint64 sent, qint64 total) {
    if (reply != current) return;
    percent = total > 0 ? int(std::min(qint64(100), sent * 100 / total)) : -1;
    label = percent == 100 ? "Confirming…" : "Uploading…";
    changed();
  });
  QObject::connect(current, &QNetworkReply::readyRead, this, [current] {
    if (current->bytesAvailable() > maxReplyBytes) current->abort();
  });
  QObject::connect(current, &QNetworkReply::finished, this, [this, current] {
    current->deleteLater();
    if (reply != current) return;
    reply.clear();
    auto body = current->read(maxReplyBytes + 1);
    auto receipt = QJsonDocument::fromJson(body).object();
    int code = current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (current->error() == QNetworkReply::NoError && code == 201 &&
        body.size() <= maxReplyBytes && validReceipt(receipt) &&
        receipt["size"].toInteger() == file.size()) {
      receipt["original_name"] = activeName;
      confirmed = receipt;
      recent.prepend(receipt);
      while (recent.size() > historyLimit) recent.removeLast();
      saveHistory();
      settle("Uploaded");
    } else if (code >= 400 && code < 500 && receipt["status"] == "error") {
      settle("Upload rejected", receipt["message"].toString().left(400));
    } else {
      settle("Receipt uncertain", "Check the FileShare upload page before sending this file again.");
    }
  });
  changed();
}

bool FileShare::validReceipt(const QJsonObject &r) const {
  static const QRegularExpression id("^up-[0-9]{8}-[0-9a-f]{16}$");
  static const QRegularExpression hash("^[0-9a-f]{64}$");
  QString uploadId = r["upload_id"].toString();
  QString path = r["stored_path"].toString();
  return r["status"] == "received_untrusted" && id.match(uploadId).hasMatch() &&
      path.startsWith('/') && !path.contains('\n') && !path.contains('\r') &&
      r["size"].toInteger() > 0 && r["size"].toInteger() <= maxBytes &&
      hash.match(r["sha256"].toString()).hasMatch() &&
      QUrl(r["download_url"].toString()) ==
          base.resolved(QUrl("/api/uploads/" + uploadId + "/download"));
}

void FileShare::settle(const QString &state, const QString &message) {
  active = false;
  posting = false;
  file.close();
  label = state;
  description = message;
  percent = -1;
  // Stop the batch on failure; never silently bury a failed/uncertain file.
  if (state != "Uploaded") pending.clear();
  changed();
  if (!pending.isEmpty()) QTimer::singleShot(0, this, [this] { next(); });
}

void FileShare::cancel() {
  pending.clear();
  if (!active) return;
  bool uncertain = posting;
  auto current = reply;
  reply.clear();
  if (current) current->abort();
  settle(uncertain ? "Receipt uncertain" : "Cancelled",
         uncertain ? "Transfer stopped. Check FileShare before uploading again." : QString());
}

void FileShare::saveHistory() {
  QDir directory(QFileInfo(historyPath).absolutePath());
  if (!directory.mkpath(".")) return;
  QFile::setPermissions(directory.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QSaveFile target(historyPath);
  if (target.open(QIODevice::WriteOnly)) {
    target.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    target.write(QJsonDocument(recent).toJson(QJsonDocument::Compact));
    target.commit();
  }
}
