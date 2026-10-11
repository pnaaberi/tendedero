#pragma once
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QQueue>
#include <QUrl>
#include <functional>

class FileShare : public QObject {
public:
  explicit FileShare(QUrl endpoint, QString historyPath, QObject *parent = nullptr);
  static bool validEndpoint(const QUrl &url);
  static bool accepts(const QList<QUrl> &urls);
  void enqueue(const QList<QUrl> &urls);
  void cancel();
  bool busy() const { return active || !pending.isEmpty(); }
  bool enabled() const { return validEndpoint(base); }
  QUrl endpoint() const { return base; }
  QString status() const { return label; }
  QString detail() const { return description; }
  QString name() const { return activeName; }
  int progress() const { return percent; }
  int queued() const { return pending.size(); }
  QJsonArray receipts() const { return recent; }
  QJsonObject currentReceipt() const { return confirmed; }
  std::function<void()> onChanged;

private:
  QUrl base;
  QString historyPath;
  QNetworkAccessManager network;
  QPointer<QNetworkReply> reply;
  QFile file;
  QQueue<QString> pending;
  QJsonArray recent;
  QJsonObject confirmed;
  QString label = "Drop to upload", description, activeName;
  int percent = -1;
  bool active = false, posting = false;
  void next();
  void post(const QByteArray &csrf);
  void settle(const QString &state, const QString &message = {});
  void changed();
  QNetworkRequest request(const QString &path) const;
  bool validReceipt(const QJsonObject &receipt) const;
  void saveHistory();
};
