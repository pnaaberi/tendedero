#include "../ui/fileshare.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir temp;
    QFile file(temp.filePath("test ä.txt"));
    require(file.open(QIODevice::WriteOnly), "fixture opens");
    file.write("synthetic upload\n");
    file.close();
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "test HTTP listener");
    QUrl base(QString("http://127.0.0.1:%1").arg(server.serverPort()));
    int posts = 0;
    bool validRequest = false, malformed = false, reject = false, stall = false;
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
      while (auto socket = server.nextPendingConnection()) {
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
          auto data = socket->property("request").toByteArray() + socket->readAll();
          socket->setProperty("request", data);
          int end = data.indexOf("\r\n\r\n");
          if (end < 0 || socket->property("handled").toBool()) return;
          auto headers = data.left(end);
          qint64 length = 0;
          for (auto line : headers.split('\n'))
            if (line.toLower().startsWith("content-length:"))
              length = line.mid(15).trimmed().toLongLong();
          if (data.size() - end - 4 < length) return;
          socket->setProperty("handled", true);
          QByteArray response;
          int status = 200;
          if (data.startsWith("GET /upload.html ")) {
            response = "<meta name=\"csrf-token\" content=\"fixture-token\">";
          } else {
            ++posts;
            validRequest = headers.toLower().contains("x-csrf-token: fixture-token") &&
                headers.contains("Origin: " + base.toEncoded()) &&
                headers.contains("X-File-Name: test%20%C3%A4.txt") &&
                data.mid(end + 4) == "synthetic upload\n";
            if (stall) return;
            status = reject ? 403 : 201;
            QJsonObject receipt{{"status", "received_untrusted"},
                {"upload_id", "up-20261011-0123456789abcdef"},
                {"stored_path", "/incoming/files/fixture.txt"}, {"size", 17},
                {"sha256", QString(64, 'a')},
                {"download_url", base.toString() + "/api/uploads/up-20261011-0123456789abcdef/download"}};
            if (malformed) receipt.remove("stored_path");
            if (reject) receipt = {{"status", "error"}, {"message", "Owner required"}};
            response = QJsonDocument(receipt).toJson(QJsonDocument::Compact);
          }
          socket->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Length: " +
                        QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
          socket->disconnectFromHost();
        });
      }
    });
    QString history = temp.filePath("receipts.json");
    FileShare share(base, history);
    share.enqueue({QUrl("https://example.invalid/file"), QUrl::fromLocalFile(temp.path())});
    require(!share.busy() && posts == 0, "nonlocal URLs and directories cannot upload");
    share.enqueue({QUrl::fromLocalFile(file.fileName())});
    require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "upload completes asynchronously");
    if (posts != 1 || !validRequest || share.receipts().size() != 1)
      std::cerr << "posts=" << posts << " valid=" << validRequest << " receipts="
                << share.receipts().size() << " status=" << share.status().toStdString()
                << " detail=" << share.detail().toStdString() << '\n';
    require(posts == 1 && validRequest && share.receipts().size() == 1, "raw upload and complete receipt");
    require(share.status() == "Uploaded", "success only after receipt");
    FileShare restored(base, history);
    require(restored.receipts().size() == 1, "confirmed receipt survives restart");
    malformed = true;
    share.enqueue({QUrl::fromLocalFile(file.fileName())});
    require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "malformed response settles");
    require(share.status() == "Receipt uncertain" && share.receipts().size() == 1,
            "incomplete acknowledgement is not success and is not retried");
    require(share.currentReceipt().isEmpty(), "failed attempt must not copy an earlier file's result");
    malformed = false;
    reject = true;
    share.enqueue({QUrl::fromLocalFile(file.fileName())});
    require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "rejection settles");
    require(share.status() == "Upload rejected", "structured rejection stays distinct");
    reject = false;
    stall = true;
    share.enqueue({QUrl::fromLocalFile(file.fileName())});
    require(QTest::qWaitFor([&] { return posts == 4; }, 4000), "test reached active POST");
    share.cancel();
    require(!share.busy() && share.status() == "Receipt uncertain", "cancel during POST retains uncertainty");
    require(share.currentReceipt().isEmpty(), "cancelled attempt cannot expose a stale copy action");
    require(QFile::exists(file.fileName()), "uploads never remove originals");
    std::cout << "FileShare client checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
