// Explicit, synthetic end-to-end check; creates one upload at the supplied endpoint.
#include "../ui/fileshare.hpp"
#include "../ui/panel.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QJsonDocument>
#include <QMimeData>
#include <QMouseEvent>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  try {
    require(argc == 3, "usage: pegline-fileshare-live HTTPS_ENDPOINT EVIDENCE_DIRECTORY");
    QDir evidence(QString::fromLocal8Bit(argv[2]));
    require(evidence.mkpath("."), "create private evidence directory");
    QTemporaryDir temp;
    QFile fixture(temp.filePath("pegline-integration-fixture.txt"));
    QByteArray bytes("Synthetic Pegline FileShare integration fixture. No personal contents.\n");
    require(fixture.open(QIODevice::WriteOnly) && fixture.write(bytes) == bytes.size(), "write synthetic fixture");
    fixture.close();
    FileShare share(QUrl(QString::fromLocal8Bit(argv[1])), evidence.filePath("receipt.json"));
    Panel panel(app.primaryScreen());
    panel.setFileShare(&share);
    share.onChanged = [&] { panel.refreshFileShare(); };
    panel.show();
    require(QTest::qWaitForWindowExposed(&panel), "native panel exposed");
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(fixture.fileName())});
    auto center = panel.fileShareRect().center();
    QDragEnterEvent enter(center, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&panel, &enter);
    require(enter.isAccepted(), "native card accepts regular-file drag");
    QDropEvent drop(center, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&panel, &drop);
    require(drop.isAccepted() && drop.dropAction() == Qt::CopyAction, "native card accepts Copy drop");
    require(QTest::qWaitFor([&] { return !share.busy(); }, 60000), "upload completed within one minute");
    if (share.status() != "Uploaded")
      std::cerr << share.status().toStdString() << ": " << share.detail().toStdString() << '\n';
    require(share.status() == "Uploaded", "server confirmed receipt");
    auto receipt = share.currentReceipt();
    QNetworkAccessManager network;
    auto reply = network.get(QNetworkRequest(QUrl(receipt["download_url"].toString())));
    require(QTest::qWaitFor([&] { return reply->isFinished(); }, 30000), "private download completed");
    require(reply->error() == QNetworkReply::NoError && reply->readAll() == bytes, "private download bytes equal upload");
    require(reply->rawHeader("Cache-Control") == "no-store" &&
            reply->rawHeader("Content-Type") == "application/octet-stream", "private download headers");
    require(receipt["sha256"].toString().toLatin1() == QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
            "receipt hash equals fixture hash");
    reply->deleteLater();
    for (const auto &key : {QString("download_url"), QString("stored_path")}) {
      auto box = panel.fileShareRect();
      QPoint click(box.left() + (key == "download_url" ? 43 : 126), box.top() + 93);
      QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, click);
      QProcess paste;
      paste.start("wl-paste", {"--no-newline", "--type", "text/plain"});
      require(QTest::qWaitFor([&] { return paste.state() == QProcess::NotRunning; }, 4000), "Wayland clipboard transfer completed");
      require(paste.exitCode() == 0 && QString::fromUtf8(paste.readAllStandardOutput()) == receipt[key].toString(),
              "copied result matches displayed upload");
    }
    require(panel.renderFrame().save(evidence.filePath("card.png")), "save own card render");
    require(QFile::exists(fixture.fileName()), "source retained");
    std::cout << "Native drop events, Tailnet upload/download/hash, and both Wayland clipboard actions passed.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
