#include "../ui/fileshare.hpp"
#include "../ui/panel.hpp"
#include <QApplication>
#include <QAbstractButton>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
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
  QApplication app(argc, argv);
  try {
    auto clipboardText = [&] {
      if (QApplication::platformName() != "wayland") return app.clipboard()->text();
      QProcess paste;
      paste.start("wl-paste", {"--no-newline", "--type", "text/plain"});
      require(QTest::qWaitFor([&] { return paste.state() == QProcess::NotRunning; }, 4000) && paste.exitCode() == 0,
              "Wayland clipboard transfer completes");
      return QString::fromUtf8(paste.readAllStandardOutput());
    };
    QTemporaryDir temp;
    QFile file(temp.filePath("test ä.txt"));
    require(file.open(QIODevice::WriteOnly), "fixture opens");
    file.write("synthetic upload\n");
    file.close();
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "test HTTP listener");
    QUrl base(QString("http://127.0.0.1:%1").arg(server.serverPort()));
    int posts = 0, requests = 0;
    int rejectPost = -1;
    QStringList receivedNames;
    QList<QByteArray> receivedBodies;
    QList<QByteArray> receivedHeaders;
    int responseStatus = 0;
    int pageStatus = 200;
    bool disconnectPage = false;
    QByteArray responseBody;
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
          ++requests;
          QByteArray response;
          int status = 200;
          if (data.startsWith("GET /upload.html ")) {
            if (disconnectPage) {
              socket->disconnectFromHost();
              return;
            }
            status = pageStatus;
            response = "<meta name=\"csrf-token\" content=\"fixture-token\">";
          } else {
            ++posts;
            QString name;
            for (auto line : headers.split('\n'))
              if (line.toLower().startsWith("x-file-name:"))
                name = QUrl::fromPercentEncoding(line.mid(12).trimmed());
            receivedNames.append(name);
            receivedBodies.append(data.mid(end + 4));
            receivedHeaders.append(headers);
            validRequest = headers.toLower().contains("x-csrf-token: fixture-token") &&
                headers.contains("Origin: " + base.toEncoded()) &&
                headers.contains("X-File-Name: test%20%C3%A4.txt") &&
                data.mid(end + 4) == "synthetic upload\n";
            if (stall) return;
            status = reject || posts == rejectPost ? 403 : 201;
            QString id = QString("up-20261011-%1").arg(posts, 16, 16, QLatin1Char('0'));
            QJsonObject receipt{{"status", "received_untrusted"},
                {"upload_id", id},
                {"stored_path", "/incoming/files/" + name}, {"size", length},
                {"sha256", QString::fromLatin1(QCryptographicHash::hash(data.mid(end + 4), QCryptographicHash::Sha256).toHex())},
                {"download_url", base.toString() + "/api/uploads/" + id + "/download"}};
            if (malformed) receipt.remove("stored_path");
            if (status == 403) receipt = {{"status", "error"}, {"message", "Owner required"}};
            response = QJsonDocument(receipt).toJson(QJsonDocument::Compact);
            if (responseStatus) {
              status = responseStatus;
              response = responseBody;
            }
          }
          socket->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Length: " +
                        QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
          socket->disconnectFromHost();
        });
      }
    });
    QString history = temp.filePath("receipts.json");
    FileShare share(base, history);
    Panel panel(app.primaryScreen());
    panel.setFileShare(&share);
    share.onChanged = [&] { panel.refreshFileShare(); };
    panel.show();
    panel.setRevealed(true, false);
    auto render = [&](const QString &name) {
      QString folder = qEnvironmentVariable("PEGLINE_TEST_RENDER_DIR");
      if (!folder.isEmpty()) {
        require(QDir().mkpath(folder), "create render directory");
        require(panel.renderFrame().save(folder + "/" + name + ".png"), "save synthetic card render");
      }
    };
    render("idle");
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
    require(share.status() == "Upload unconfirmed" && share.receipts().size() == 1,
            "incomplete acknowledgement is not success and is not retried");
    require(share.detail() == "The file may already be on FileShare. Check uploads before sending it again.",
            "unconfirmed receipt explains the uncertain outcome and avoids duplicate uploads");
    require(share.currentReceipt().isEmpty(), "failed attempt must not copy an earlier file's result");
    malformed = false;
    reject = true;
    share.enqueue({QUrl::fromLocalFile(file.fileName())});
    require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "rejection settles");
    require(share.status() == "Upload rejected" && share.detail().contains("File was not uploaded.") &&
            share.detail().contains("Owner required"), "structured rejection explains the outcome and retains the server reason");
    reject = false;
    stall = true;
    share.enqueue({QUrl::fromLocalFile(file.fileName())});
    require(QTest::qWaitFor([&] { return posts == 4; }, 4000), "test reached active POST");
    share.cancel();
    require(!share.busy() && share.status() == "Upload unconfirmed", "cancel during POST retains uncertainty");
    require(share.detail() == "Transfer stopped. The file may already be on FileShare. Check uploads before sending it again.",
            "cancel after sending warns that the file may already exist");
    require(share.currentReceipt().isEmpty(), "cancelled attempt cannot expose a stale copy action");
    stall = false;
    QList<QUrl> batch;
    for (int i = 0; i < 3; ++i) {
      QFile next(temp.filePath(QString("batch %1 ä.txt").arg(i + 1)));
      require(next.open(QIODevice::WriteOnly), "multi-file fixture opens");
      next.write(QByteArray(i + 1, 'a' + i));
      next.close();
      batch.append(QUrl::fromLocalFile(next.fileName()));
    }
    share.enqueue({batch[0], QUrl::fromLocalFile(temp.path())});
    require(!share.busy() && posts == 4, "mixed file/directory drop rejects the entire selection");
    QMimeData invalid;
    invalid.setUrls({batch[0], QUrl::fromLocalFile(temp.path())});
    QDragEnterEvent invalidDrag(panel.fileShareRect().center(), Qt::CopyAction, &invalid, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&panel, &invalidDrag);
    require(!invalidDrag.isAccepted() && posts == 4, "invalid mixed selection has no accepted card drop");
    render("invalid-selection");
    require(!share.canEnqueue(QList<QUrl>(101, batch[0])), "oversized selections are rejected before upload");
    auto dropFiles = [&](const QList<QUrl> &urls, const QString &preview) {
      QMimeData mime;
      mime.setUrls(urls);
      QPoint center = panel.fileShareRect().center();
      QDragEnterEvent enter(center, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
      QCoreApplication::sendEvent(&panel, &enter);
      require(enter.isAccepted() && enter.dropAction() == Qt::CopyAction, "multi-file drag is accepted as Copy");
      render(preview);
      QDropEvent drop(center, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
      QCoreApplication::sendEvent(&panel, &drop);
      require(drop.isAccepted() && drop.dropAction() == Qt::CopyAction, "multi-file drop is accepted as Copy");
    };
    dropFiles({batch[0], batch[1]}, "drop-two");
    dropFiles(batch, "queue-more");
    if (share.queued() != 2)
      std::cerr << "batch queued=" << share.queued() << " total=" << share.batchTotal()
                << " posts=" << posts << " status=" << share.status().toStdString() << '\n';
    require(share.queued() == 2, "adding files during an upload queues each file only once");
    require(share.batchTotal() == 3, "batch total counts actual queued files");
    render("queued");
    panel.setRevealed(false, false);
    require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "multi-file batch completes");
    require(panel.fileShareRect().isEmpty(), "background upload completion keeps the shelf hidden");
    panel.setRevealed(true, false);
    require(posts == 7 && share.receipts().size() == 4, "every distinct file has its own confirmed receipt");
    require(share.batchReceipts().size() == 3, "batch copy includes every confirmed file in this batch");
    render("uploaded");
    for (const QString &key : {QString("download_url"), QString("stored_path")}) {
      QStringList expected;
      for (auto receipt : share.batchReceipts()) expected.append(receipt.toObject()[key].toString());
      QRect box = panel.fileShareRect();
      QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
          QPoint(box.left() + (key == "download_url" ? box.width() / 4 : box.width() * 3 / 4), box.bottom() - 14));
      require(clipboardText() == expected.join('\n'), "card copies exactly this batch's links or paths, in order");
    }
    for (int i = 0; i < 3; ++i) {
      require(receivedNames[i + 4] == QString("batch %1 ä.txt").arg(i + 1) &&
              receivedBodies[i + 4] == QByteArray(i + 1, 'a' + i), "batch preserves file order, Unicode names and bytes");
      require(QFile::exists(batch[i].toLocalFile()), "multi-file upload retains each source");
    }
    QList<QUrl> largeBatch = batch;
    for (int i = 3; i < 13; ++i) {
      QFile next(temp.filePath(QString("batch %1.txt").arg(i + 1)));
      require(next.open(QIODevice::WriteOnly) && next.write("test") == 4, "large batch fixture opens");
      largeBatch.append(QUrl::fromLocalFile(next.fileName()));
    }
    int beforeFailure = posts;
    rejectPost = posts + 12;
    share.enqueue(largeBatch);
    require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "partial failure stops batch");
    require(posts == beforeFailure + 12 && share.receipts().size() == 10 &&
            share.batchReceipts().size() == 11 && share.currentReceipt().isEmpty(),
            "batch failure keeps confirmed history and never sends later files");
    render("partial-failure");
    for (const QString &key : {QString("download_url"), QString("stored_path")}) {
      QStringList expected;
      for (auto receipt : share.batchReceipts()) expected.append(receipt.toObject()[key].toString());
      bool foundFailure = false, copiedConfirmed = false;
      QTimer::singleShot(0, [&] {
        auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        for (auto action : menu->actions()) {
          if (action->text() == "Stopped at: batch 12.txt") foundFailure = true;
          if (action->text() == QString("Copy 11 confirmed %1").arg(key == "download_url" ? "links" : "paths")) {
            action->trigger();
            copiedConfirmed = true;
          }
        }
        menu->close();
      });
      QTest::mouseClick(&panel, Qt::RightButton, Qt::NoModifier, panel.fileShareRect().center());
      require(foundFailure, "failure menu identifies exactly which file stopped the batch");
      require(copiedConfirmed && clipboardText() == expected.join('\n'),
              "partial batch can copy all eleven confirmed results beyond the ten-item history");
    }
    rejectPost = -1;
    stall = true;
    int beforeCancel = posts;
    share.enqueue(batch);
    require(QTest::qWaitFor([&] { return posts == beforeCancel + 1; }, 4000), "queued cancellation reaches first POST");
    const auto busyHistory = share.receipts();
    require(!share.canClear() && !share.clearHistory() && share.busy() && share.queued() == 2 &&
            share.receipts() == busyHistory, "clearing cannot interrupt an upload or discard its queued files");
    share.cancel();
    QTest::qWait(100);
    require(posts == beforeCancel + 1 && !share.busy() && share.queued() == 0, "cancel stops active upload and clears the batch");
    render("cancelled");
    int beforeClear = requests;
    bool clearedFromMenu = false;
    QTimer::singleShot(0, [&] {
      auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu) return;
      for (auto action : menu->actions()) {
        if (action->text() == "Clear history" && action->isEnabled()) {
          action->trigger();
          clearedFromMenu = true;
        }
      }
      menu->close();
    });
    QTest::mouseClick(&panel, Qt::RightButton, Qt::NoModifier, panel.fileShareRect().center());
    require(clearedFromMenu, "card menu can clear local upload history");
    require(share.receipts().isEmpty() && share.batchReceipts().isEmpty() && share.currentReceipt().isEmpty() &&
            share.batchTotal() == 0 && share.name().isEmpty() && share.detail().isEmpty() &&
            share.status() == "Drop to upload", "clearing resets receipts, failed batch details and card contents");
    FileShare cleared(base, history);
    require(cleared.receipts().isEmpty() && cleared.currentReceipt().isEmpty(), "cleared contents stay empty after restart");
    QTest::qWait(20);
    require(share.enabled() && share.endpoint() == base && requests == beforeClear && !share.canClear(),
            "clearing retains settings and sends no server requests");
    render("cleared");
    stall = false;
    dropFiles({batch[0], batch[1]}, "after-clear-drop");
    require(QTest::qWaitFor([&] { return !share.busy(); }, 4000) && share.batchTotal() == 2 &&
            share.batchReceipts().size() == 2, "new uploads work after clearing with fresh batch counts");
    render("clear-button");
    const auto beforeInaccessible = share.receipts();
    const auto folderPermissions = QFileInfo(temp.path()).permissions();
    require(QFile::exists(history) && QFile::setPermissions(temp.path(), {}), "hide persisted history behind inaccessible folder");
    // Privileged test runners may still traverse a directory without permissions.
    const bool inaccessible = !QFileInfo::exists(history);
    const bool clearedInaccessible = inaccessible && share.clearHistory();
    require(QFile::setPermissions(temp.path(), folderPermissions), "restore fixture folder permissions");
    require(!clearedInaccessible && share.receipts() == beforeInaccessible && QFile::exists(history),
            "inaccessible history must not report a successful clear or discard in-memory results");
    auto clearButton = [&] {
      const QRect box = panel.fileShareRect();
      return QPoint(box.right() - 31, box.top() + 23);
    };
    // A press that leaves the control must not accidentally clear the results.
    QTest::mousePress(&panel, Qt::LeftButton, Qt::NoModifier, clearButton());
    QTest::mouseRelease(&panel, Qt::LeftButton, Qt::NoModifier, panel.fileShareRect().center());
    require(share.canClear() && share.receipts().size() == 2, "dragging off Clear leaves history intact");
    beforeClear = requests;
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, clearButton());
    require(!share.canClear() && share.receipts().isEmpty() && share.currentReceipt().isEmpty(),
            "visible Clear button resets a successful batch");
    FileShare clearedAgain(base, history);
    require(clearedAgain.receipts().isEmpty() && requests == beforeClear, "button clearing persists without network traffic");
    render("cleared-button");

    const QString blockedHistory = temp.filePath("history-is-a-directory");
    require(QDir().mkpath(blockedHistory), "create unwritable history target");
    FileShare blocked(base, blockedHistory);
    blocked.enqueue({batch[0]});
    require(QTest::qWaitFor([&] { return !blocked.busy(); }, 4000) && !blocked.currentReceipt().isEmpty(),
            "upload remains usable if local history cannot be saved");
    const auto retained = blocked.receipts();
    require(!blocked.clearHistory() && blocked.receipts() == retained && !blocked.currentReceipt().isEmpty(),
            "failed persistent clear keeps current results available");
    require(QFile::exists(file.fileName()), "uploads never remove originals");

    for (int status : {401, 403}) {
      pageStatus = status;
      const int beforeDenied = posts;
      share.enqueue({batch[0], batch[1]});
      require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "page access denial settles");
      require(share.status() == "Access denied" && share.detail().contains("File was not sent.") &&
              share.detail().contains("owner access") && posts == beforeDenied && share.queued() == 0,
              "denied upload-page access explains that no file was sent and stops the batch");
    }
    pageStatus = 200;
    disconnectPage = true;
    const int beforeDisconnect = posts;
    share.enqueue({batch[0]});
    require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "page connection loss settles");
    require(share.status() == "Cannot connect" && share.detail().contains("File was not sent.") &&
            posts == beforeDisconnect, "connection loss before POST explicitly confirms no file was sent");
    disconnectPage = false;
    responseStatus = 400;
    responseBody = R"({"status":"error","message":"  "})";
    share.enqueue({batch[0]});
    require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "empty rejection reason settles");
    require(share.status() == "Upload rejected" && share.detail().contains("File was not uploaded.") &&
            share.detail().contains("rejected the request"), "blank structured rejection has a visible meaningful fallback");

    auto inspectDetails = [&](bool stoppedBatch = true) {
      QMessageBox *details = nullptr;
      for (auto widget : QApplication::topLevelWidgets())
        if (auto box = qobject_cast<QMessageBox *>(widget); box && box->isVisible()) details = box;
      if (!details || details->text() != share.status())
        std::cerr << "details: expected=" << share.status().toStdString() << " actual="
                  << (details ? details->text().toStdString() : "none") << " revealed=" << panel.isRevealed()
                  << " card-empty=" << panel.fileShareRect().isEmpty() << '\n';
      require(details, "clicking a failed upload opens a visible details dialog");
      require(QTest::qWaitForWindowExposed(details), "upload details are exposed by the desktop");
      require(!details->isModal(), "upload details do not block the panel");
      require(details->textFormat() == Qt::PlainText, "upload details treat server text as plain text");
      require(details->text() == share.status(), "upload details show the current result");
      require(details->informativeText().contains(share.name()) && details->informativeText().contains(share.detail()),
              "upload details show the filename and full reason");
      bool openFileShare = false;
      for (auto button : details->buttons()) if (button->text() == "Open FileShare") openFileShare = true;
      require(openFileShare && details->button(QMessageBox::Close), "upload details offer Open FileShare and Close actions");
      require(details->informativeText().contains("Batch stopped:") == stoppedBatch,
              "details distinguish a stopped batch from rejected selections and ongoing uploads");
      const QString renderFolder = qEnvironmentVariable("PEGLINE_TEST_RENDER_DIR");
      if (!renderFolder.isEmpty() && stoppedBatch)
        require(details->grab().save(renderFolder + "/upload-details.png"), "save synthetic upload details render");
      details->close();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };
    for (const QString &code : {QString("disk_reserve"), QString("intake_full")}) {
      responseStatus = 507;
      responseBody = QJsonDocument(QJsonObject{{"status", "error"}, {"code", code},
          {"message", "Levytilan turvaraja estää lähetyksen."}}).toJson(QJsonDocument::Compact);
      const int beforeRejection = posts;
      const auto savedReceipts = share.receipts();
      share.enqueue({batch[0], batch[1]});
      require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "storage rejection settles");
      require(share.status() == (code == "disk_reserve" ? "Server storage low" : "Upload storage full"),
              "known HTTP 507 storage refusal identifies the cause on the card");
      require(share.detail() == (code == "disk_reserve" ?
          "File was not uploaded. Free space on the server, then try again." :
          "File was not uploaded. Remove unneeded uploads on the server, then try again."),
          "storage rejection explains the cause and server action in English");
      require(posts == beforeRejection + 1 && share.queued() == 0 && share.currentReceipt().isEmpty() &&
              share.receipts() == savedReceipts, "storage refusal stops the batch without retrying or inventing a receipt");
      panel.setRevealed(true, false);
      render(code == "disk_reserve" ? "storage-low" : "storage-full");
      if (code == "disk_reserve") {
        for (QPoint point : {panel.fileShareRect().center(),
             QPoint(panel.fileShareRect().center().x(), panel.fileShareRect().bottom() - 14)}) {
          // The old card opens a blocking menu here; close it so the regression fails instead of hanging.
          QTimer::singleShot(0, [&] {
            if (auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) menu->close();
          });
          QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, point);
          QCoreApplication::processEvents();
          inspectDetails();
        }
        bool hasDetailsAction = false;
        QTimer::singleShot(0, [&] {
          auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
          if (!menu) return;
          for (auto action : menu->actions()) if (action->text() == "Upload details…" && action->isEnabled()) {
            hasDetailsAction = true;
            action->trigger();
          }
          menu->close();
        });
        QTest::mouseClick(&panel, Qt::RightButton, Qt::NoModifier, panel.fileShareRect().center());
        require(hasDetailsAction, "failed upload menu offers an enabled Upload details action");
        inspectDetails();
      }
    }
    const QList<QPair<int, QByteArray>> uncertainResponses = {
        {507, R"({"status":"error","code":"unknown","message":"Failure"})"},
        {507, R"({"status":"error","message":"Failure"})"},
        {507, R"({"status":"received_untrusted","code":"disk_reserve"})"},
        {507, R"({"status":"error","code":"disk_reserve"}) trailing garbage)"},
        {500, R"({"status":"error","code":"disk_reserve"})"},
        {503, "Service unavailable"}};
    for (const auto &failure : uncertainResponses) {
      responseStatus = failure.first;
      responseBody = failure.second;
      const int beforeUncertain = posts;
      share.enqueue({batch[0], batch[1]});
      require(QTest::qWaitFor([&] { return !share.busy(); }, 4000), "unconfirmed server error settles");
      require(share.status() == "Upload unconfirmed" && share.currentReceipt().isEmpty() &&
              posts == beforeUncertain + 1 && share.queued() == 0,
              "unknown or malformed server errors remain uncertain and are never retried");
      require(share.detail() == "The file may already be on FileShare. Check uploads before sending it again.",
              "unconfirmed server error explains that another upload could create a duplicate");
    }
    render("upload-unconfirmed");
    responseStatus = 0;

    // Transport fixtures are opaque: suffixes and binary contents must never select a different upload path.
    const QStringList formatNames = {"document ä % #.pdf", "archive ä % #.zip", "image ä % #.png",
                                     "text ä % #.txt", "extensionless ä % #"};
    QByteArray binary(1024 * 1024 + 17, '\0');
    for (qsizetype i = 0; i < binary.size(); ++i) binary[i] = char(i % 256);
    const QList<QByteArray> formatBodies = {
        QByteArray("%PDF-1.7\n% Synthetic transport fixture\n") + binary.left(65537) + "\n%%EOF\n",
        QByteArray::fromHex("504b0506000000000000000000000000000000000000"),
        QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII="),
        QByteArray("Synthetic UTF-8 text: ä\n"), binary};
    QList<QUrl> formats;
    for (int i = 0; i < formatNames.size(); ++i) {
      QFile fixture(temp.filePath(formatNames[i]));
      require(fixture.open(QIODevice::WriteOnly) && fixture.write(formatBodies[i]) == formatBodies[i].size(),
              "synthetic format fixture opens");
      fixture.close();
      formats.append(QUrl::fromLocalFile(fixture.fileName()));
    }
    const int beforeFormats = posts;
    dropFiles(formats, "mixed-formats");
    require(QTest::qWaitFor([&] { return !share.busy(); }, 10000) && share.status() == "Uploaded" &&
            share.batchReceipts().size() == formats.size() && posts == beforeFormats + formats.size(),
            "mixed PDF ZIP PNG text and extensionless files receive individual confirmations");
    for (int i = 0; i < formats.size(); ++i) {
      const auto headers = receivedHeaders[beforeFormats + i].toLower();
      require(receivedNames[beforeFormats + i] == formatNames[i] && receivedBodies[beforeFormats + i] == formatBodies[i],
              "mixed-format uploads preserve encoded names and every binary byte beyond 64 KiB and 1 MiB");
      require(headers.contains("\ncontent-length: " + QByteArray::number(formatBodies[i].size()) + "\r") &&
              headers.contains("\ncontent-type: application/octet-stream\r") &&
              headers.contains("\nx-file-type: application/octet-stream\r") &&
              headers.contains("%c3%a4%20%25%20%23"), "every format uses raw bytes, exact length and safely encoded filename headers");
      const auto receipt = share.batchReceipts()[i].toObject();
      require(receipt["size"].toInteger() == formatBodies[i].size() &&
              receipt["sha256"].toString().toLatin1() == QCryptographicHash::hash(formatBodies[i], QCryptographicHash::Sha256).toHex() &&
              QFile::exists(formats[i].toLocalFile()), "mixed-format receipts match the original sizes and hashes and retain sources");
    }
    share.enqueue({QUrl::fromLocalFile(temp.path())});
    panel.setRevealed(true, false);
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, panel.fileShareRect().center());
    inspectDetails(false);
    stall = true;
    const int beforeInvalidSelection = posts;
    share.enqueue({formats[0], formats[1]});
    require(QTest::qWaitFor([&] { return posts == beforeInvalidSelection + 1; }, 4000), "active batch reaches POST");
    share.enqueue({QUrl::fromLocalFile(temp.path())});
    panel.setRevealed(true, false);
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, panel.fileShareRect().center());
    inspectDetails(false);
    require(share.busy() && share.queued() == 1, "rejected selection and viewing details do not stop the active batch");
    share.cancel();
    std::cout << "FileShare client checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
