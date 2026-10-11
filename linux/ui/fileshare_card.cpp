#include "panel.hpp"
#include "fileshare.hpp"
#include "clipboard.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonObject>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <cmath>

namespace {
constexpr int cardWidth = 208, cardHeight = 150, rightMargin = 22;
const QColor paper("#e9f1eb"), ink("#213d30"), accent("#28684e"), peg("#dec198");
}

void Panel::setFileShare(FileShare *share) {
  fileShare = share;
  refreshFileShare();
}

void Panel::setCaptureSuppressed(bool suppressed) {
  captureSuppressed = suppressed;
  uploadHover = false;
  uploadRejected = false;
  refreshFileShare();
}

void Panel::refreshFileShare() {
  shareCopyFeedback.clear();
  if (revealed) breezeTimer.start();
  else breezeTimer.stop();
  updateInput();
  update();
}

QRect Panel::fileShareRect() const {
  if (!fileShare || !fileShare->enabled() || captureSuppressed || progress < .01) return {};
  return QRect(std::max(8, width() - cardWidth - rightMargin),
               int(36 - (1 - progress) * 215), cardWidth, cardHeight);
}

QTransform Panel::fileShareTransform() const {
  auto box = fileShareRect();
  QTransform transform;
  transform.translate(box.center().x(), box.top() - 10);
  transform.rotate(uploadHover ? 0 : std::sin(motionTime / 1500.0) * 0.8);
  transform.translate(-box.center().x(), -box.top() + 10);
  return transform;
}

bool Panel::fileShareAt(QPointF point) const {
  return !fileShareRect().isEmpty() &&
      fileShareRect().contains(fileShareTransform().inverted().map(point).toPoint());
}

void Panel::paintFileShare(QPainter &p) const {
  QRect box = fileShareRect();
  if (box.isEmpty()) return;
  p.save();
  p.setTransform(fileShareTransform(), true);
  const bool warning = uploadRejected || (!uploadHover && !fileShare->busy() && !fileShare->detail().isEmpty());
  const QColor tone = warning ? QColor("#9b482e") : accent;
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0, 0, 0, 60));
  p.drawRoundedRect(box.adjusted(-3, 4, 3, 7), 10, 10);
  p.setPen(QPen(QColor(255, 255, 255, 150), 1));
  p.setBrush(paper);
  p.drawRoundedRect(box, 9, 9);
  p.setPen(Qt::NoPen);
  p.setBrush(peg);
  p.drawRoundedRect(QRect(box.center().x() - 5, box.top() - 14, 10, 22), 2, 2);
  p.setPen(ink);
  auto font = p.font();
  font.setPixelSize(13);
  font.setBold(true);
  p.setFont(font);
  p.drawText(box.adjusted(12, 12, -12, -114), Qt::AlignLeft | Qt::AlignVCenter, "FileShare");
  int completed = fileShare->batchReceipts().size();
  int total = fileShare->batchTotal();
  font.setPixelSize(10);
  font.setBold(false);
  p.setFont(font);
  QString badge = fileShare->busy() ? QString("%1 / %2").arg(std::min(completed + 1, total)).arg(total) :
      warning && !fileShare->name().isEmpty() && total > 1 ? QString("%1 / %2").arg(completed).arg(total) :
      !fileShare->currentReceipt().isEmpty() && total > 1 ? QString("%1 files").arg(completed) : "PRIVATE";
  p.drawText(box.adjusted(100, 12, -12, -114), Qt::AlignRight | Qt::AlignVCenter, badge);

  QRect zone(box.left() + 10, box.top() + 40, box.width() - 20, 72);
  p.setPen(QPen(tone, uploadHover || uploadRejected ? 2 : 1, Qt::DashLine));
  p.setBrush(uploadRejected ? QColor("#fae9e2") : uploadHover ? QColor("#cce8d5") : QColor("#f5f9f5"));
  p.drawRoundedRect(zone, 7, 7);
  // A stack of files with a downward arrow makes the whole area a drop target.
  p.setPen(QPen(tone, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(Qt::NoBrush);
  QRectF sheet(zone.left() + 13, zone.top() + 22, 22, 29);
  p.drawRoundedRect(sheet.translated(4, -4), 3, 3);
  p.setBrush(uploadHover ? QColor("#cce8d5") : QColor("#f5f9f5"));
  p.drawRoundedRect(sheet, 3, 3);
  QPointF arrow(sheet.center().x(), sheet.top() + 8);
  p.drawLine(arrow, arrow + QPointF(0, 12));
  p.drawLine(arrow + QPointF(-4, 8), arrow + QPointF(0, 12));
  p.drawLine(arrow + QPointF(4, 8), arrow + QPointF(0, 12));

  QString title = "Drop files here", detail = "One or many · private";
  if (fileShare->busy()) {
    title = fileShare->status();
    detail = fileShare->name();
  } else if (!fileShare->currentReceipt().isEmpty()) {
    title = completed > 1 ? QString("%1 files uploaded").arg(completed) : "File uploaded";
    detail = completed > 1 ? "Ready to share" : fileShare->name();
  } else if (!fileShare->detail().isEmpty()) {
    title = fileShare->status();
    detail = fileShare->name().isEmpty() ? "Open details to review" : fileShare->name();
  }
  if (!shareCopyFeedback.isEmpty()) title = shareCopyFeedback;
  if (uploadHover || uploadRejected) {
    title = uploadDropTitle;
    detail = uploadDropDetail;
  }
  font.setPixelSize(12);
  font.setBold(true);
  p.setFont(font);
  p.setPen(tone);
  int textWidth = zone.width() - 57;
  p.drawText(QRect(zone.left() + 49, zone.top() + 14, textWidth, 22), Qt::AlignVCenter,
             p.fontMetrics().elidedText(title, Qt::ElideRight, textWidth));
  font.setPixelSize(10);
  font.setBold(false);
  p.setFont(font);
  p.setPen(ink);
  p.drawText(QRect(zone.left() + 49, zone.top() + 37, textWidth, 20), Qt::AlignVCenter,
             p.fontMetrics().elidedText(detail, Qt::ElideMiddle, textWidth));
  if (fileShare->busy() && fileShare->progress() >= 0)
    p.fillRect(zone.left() + 4, zone.bottom() - 4,
               (zone.width() - 8) * fileShare->progress() / 100, 2, accent);

  font.setPixelSize(11);
  p.setFont(font);
  p.setPen(accent);
  QRect footer(box.left() + 6, box.bottom() - 27, box.width() - 12, 23);
  if (uploadHover)
    p.drawText(footer, Qt::AlignCenter, "Release to upload");
  else if (uploadRejected)
    p.drawText(footer, Qt::AlignCenter, "Selection will not upload");
  else if (fileShare->busy())
    p.drawText(footer, Qt::AlignCenter, total > 1 ? "Cancel remaining uploads" : "Cancel upload");
  else if (!fileShare->currentReceipt().isEmpty()) {
    QRect left = footer; left.setWidth(footer.width() / 2);
    QRect right = left.translated(left.width(), 0);
    p.drawText(left, Qt::AlignCenter, completed > 1 ? "Copy links" : "Copy link");
    p.drawText(right, Qt::AlignCenter, completed > 1 ? "Copy paths" : "Copy path");
  } else
    p.drawText(footer, Qt::AlignCenter, fileShare->detail().isEmpty() ? "Choose files…" : "Details / recent uploads…");
  p.restore();
}

void Panel::copyFileShare(const QString &text) {
  if (QApplication::platformName() == "wayland") {
    if (!setNativeClipboardText(text.toUtf8())) {
      if (onError) onError("Could not copy the FileShare result.");
      return;
    }
  } else QApplication::clipboard()->setText(text);
  shareCopyFeedback = "Copied ✓";
  update();
  QTimer::singleShot(1500, this, [this] { shareCopyFeedback.clear(); update(); });
}

void Panel::chooseFileShare() {
  QPointer<FileShare> target = fileShare;
  auto selected = QFileDialog::getOpenFileUrls(nullptr, "Upload to FileShare");
  if (target && !selected.isEmpty()) target->enqueue(selected);
}

void Panel::fileShareMenu(QPoint position) {
  if (!fileShare) return;
  QMenu menu;
  if (!fileShare->detail().isEmpty()) {
    if (!fileShare->name().isEmpty())
      menu.addAction("Stopped at: " + fileShare->name().left(100).replace('&', "&&"))->setEnabled(false);
    menu.addAction(fileShare->detail())->setEnabled(false);
    menu.addSeparator();
  }
  QObject::connect(menu.addAction("Upload files…"), &QAction::triggered, this, &Panel::chooseFileShare);
  if (fileShare->busy())
    QObject::connect(menu.addAction("Cancel uploads"), &QAction::triggered, fileShare, &FileShare::cancel);
  auto confirmed = fileShare->batchReceipts();
  if (!confirmed.isEmpty()) {
    menu.addSeparator();
    menu.addAction(QString("Last batch: %1 of %2 confirmed").arg(confirmed.size()).arg(fileShare->batchTotal()))->setEnabled(false);
    for (const QString &key : {QString("download_url"), QString("stored_path")}) {
      QString label = QString("Copy %1 confirmed %2").arg(confirmed.size()).arg(key == "download_url" ? "links" : "paths");
      QObject::connect(menu.addAction(label), &QAction::triggered, this, [this, confirmed, key] {
        QStringList results;
        for (auto receipt : confirmed) results.append(receipt.toObject()[key].toString());
        copyFileShare(results.join('\n'));
      });
    }
    menu.addSeparator();
  }
  for (auto value : fileShare->receipts()) {
    auto receipt = value.toObject();
    auto name = receipt["original_name"].toString();
    auto submenu = menu.addMenu(name.left(70).replace('&', "&&"));
    QObject::connect(submenu->addAction("Copy private link"), &QAction::triggered, this,
                     [this, receipt] { copyFileShare(receipt["download_url"].toString()); });
    QObject::connect(submenu->addAction("Copy server path"), &QAction::triggered, this,
                     [this, receipt] { copyFileShare(receipt["stored_path"].toString()); });
  }
  menu.addSeparator();
  QObject::connect(menu.addAction("Open FileShare upload page"), &QAction::triggered, this, [this] {
    if (fileShare) QDesktopServices::openUrl(fileShare->endpoint().resolved(QUrl("/upload.html")));
  });
  QPointer<Panel> guard(this);
  currentMenu = &menu;
  menu.exec(position);
  if (!guard) return;
  currentMenu = nullptr;
  if (retiring) deleteLater();
  else hideTimer.start();
}

bool Panel::fileShareEvent(QEvent *event) {
  if (!fileShare || !fileShare->enabled() || captureSuppressed) return false;
  if (event->type() == QEvent::DragLeave) {
    uploadHover = false;
    uploadRejected = false;
    if (!dragging) hideTimer.start();
    refreshFileShare();
    event->accept();
    return true;
  }
  if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
    auto drop = static_cast<QDropEvent *>(event);
    if (overSensor(drop->position()) && (drop->possibleActions() & Qt::CopyAction) &&
        fileShare->canEnqueue(drop->mimeData()->urls())) {
      if (!revealed) setRevealed(true);
      hideTimer.stop();
      drop->setDropAction(Qt::CopyAction);
      drop->accept();
      return true;
    }
  }
  if (fileShareRect().isEmpty()) return false;
  if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop) {
    auto drop = static_cast<QDropEvent *>(event);
    auto urls = drop->mimeData()->urls();
    bool over = fileShareAt(drop->position());
    bool accepts = over && (drop->possibleActions() & Qt::CopyAction) && fileShare->canEnqueue(urls);
    uploadHover = accepts && event->type() != QEvent::Drop;
    uploadRejected = over && !accepts && event->type() != QEvent::Drop;
    if (over) hideTimer.stop();
    if (uploadHover) {
      qint64 bytes = 0;
      for (const auto &url : urls) bytes += QFileInfo(url.toLocalFile()).size();
      uploadDropTitle = fileShare->busy() ? QString("Queue %1 file%2").arg(urls.size()).arg(urls.size() == 1 ? "" : "s") :
          QString("Drop %1 file%2").arg(urls.size()).arg(urls.size() == 1 ? "" : "s");
      uploadDropDetail = QLocale().formattedDataSize(bytes) + " total";
    } else if (uploadRejected) {
      uploadDropTitle = "Cannot drop these";
      uploadDropDetail = FileShare::accepts(urls) || urls.size() > 100 ? "100 files per batch" : "Local files · 1 B–4 GiB";
    }
    if (accepts) {
      drop->setDropAction(Qt::CopyAction);
      drop->accept();
      if (event->type() == QEvent::Drop) fileShare->enqueue(drop->mimeData()->urls());
    } else drop->ignore();
    refreshFileShare();
    return true;
  }
  if (event->type() == QEvent::MouseButtonPress) {
    auto mouse = static_cast<QMouseEvent *>(event);
    if (!fileShareAt(mouse->position())) return false;
    hideTimer.stop();
    if (mouse->button() == Qt::RightButton) fileShareMenu(mouse->globalPosition().toPoint());
    else if (mouse->button() == Qt::LeftButton) uploadPressed = true;
    return true;
  }
  if (event->type() == QEvent::MouseButtonRelease && uploadPressed) {
    uploadPressed = false;
    auto mouse = static_cast<QMouseEvent *>(event);
    if (!fileShareAt(mouse->position())) return true;
    auto local = fileShareTransform().inverted().map(mouse->position());
    auto box = fileShareRect();
    if (local.y() >= box.bottom() - 30) {
      if (fileShare->busy()) fileShare->cancel();
      else if (!fileShare->currentReceipt().isEmpty()) {
        QStringList results;
        QString key = local.x() < box.center().x() ? "download_url" : "stored_path";
        for (auto receipt : fileShare->batchReceipts()) results.append(receipt.toObject()[key].toString());
        copyFileShare(results.join('\n'));
      } else if (!fileShare->detail().isEmpty()) fileShareMenu(mouse->globalPosition().toPoint());
      else chooseFileShare();
    } else if (!fileShare->detail().isEmpty()) fileShareMenu(mouse->globalPosition().toPoint());
    else chooseFileShare();
    return true;
  }
  return false;
}
