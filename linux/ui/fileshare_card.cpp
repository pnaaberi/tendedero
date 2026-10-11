#include "panel.hpp"
#include "fileshare.hpp"
#include "clipboard.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <cmath>

namespace {
constexpr int cardWidth = 170, cardHeight = 108, rightMargin = 22;
const QColor paper("#e9f1eb"), ink("#213d30"), accent("#28684e"), peg("#dec198");
}

void Panel::setFileShare(FileShare *share) {
  fileShare = share;
  refreshFileShare();
}

void Panel::setCaptureSuppressed(bool suppressed) {
  captureSuppressed = suppressed;
  uploadHover = false;
  refreshFileShare();
}

void Panel::refreshFileShare() {
  if (revealed || !fileShareRect().isEmpty()) breezeTimer.start();
  else breezeTimer.stop();
  updateInput();
  update();
}

QRect Panel::fileShareRect() const {
  if (!fileShare || !fileShare->enabled() || captureSuppressed) return {};
  return QRect(std::max(8, width() - cardWidth - rightMargin), 36, cardWidth, cardHeight);
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
  p.setPen(QPen(QColor(175, 190, 180, 220), 1.5));
  p.drawLine(box.left() - 10, 20, width(), 20);
  p.setTransform(fileShareTransform(), true);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0, 0, 0, 60));
  p.drawRoundedRect(box.adjusted(-3, 4, 3, 7), 10, 10);
  p.setPen(QPen(uploadHover ? accent : QColor(255, 255, 255, 150), uploadHover ? 2 : 1));
  p.setBrush(paper);
  p.drawRoundedRect(box, 9, 9);
  p.setPen(Qt::NoPen);
  p.setBrush(peg);
  p.drawRoundedRect(QRect(box.center().x() - 5, box.top() - 14, 10, 22), 2, 2);
  p.setPen(ink);
  auto font = p.font();
  font.setPixelSize(14);
  font.setBold(true);
  p.setFont(font);
  p.drawText(box.adjusted(10, 12, -10, -72), Qt::AlignLeft | Qt::AlignVCenter, "↑  FileShare");
  font.setPixelSize(11);
  font.setBold(false);
  p.setFont(font);
  QString state = uploadHover ? "Release to upload" :
      !shareCopyFeedback.isEmpty() ? shareCopyFeedback : fileShare->status();
  if (fileShare->queued()) state += QString(" · +%1").arg(fileShare->queued());
  p.drawText(box.adjusted(10, 39, -10, -46), Qt::AlignLeft | Qt::AlignVCenter,
             p.fontMetrics().elidedText(state, Qt::ElideRight, box.width() - 20));
  QString detail = fileShare->name().isEmpty() ? fileShare->endpoint().host() : fileShare->name();
  p.drawText(box.adjusted(10, 57, -10, -28), Qt::AlignLeft | Qt::AlignVCenter,
             p.fontMetrics().elidedText(detail, Qt::ElideMiddle, box.width() - 20));
  p.setPen(accent);
  if (fileShare->busy()) {
    if (fileShare->progress() >= 0) {
      p.fillRect(box.left() + 10, box.top() + 78,
                 (box.width() - 20) * fileShare->progress() / 100, 2, accent);
    }
    p.drawText(box.adjusted(10, 82, -10, -4), Qt::AlignCenter, "Cancel uploads");
  } else if (!fileShare->currentReceipt().isEmpty()) {
    p.drawText(QRect(box.left() + 6, box.top() + 81, 80, 23), Qt::AlignCenter, "Copy link");
    p.drawText(QRect(box.left() + 88, box.top() + 81, 76, 23), Qt::AlignCenter, "Copy path");
  } else {
    p.drawText(box.adjusted(10, 81, -10, -4), Qt::AlignCenter,
               fileShare->detail().isEmpty() ? "Choose files…" : "Details / recent uploads…");
  }
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

void Panel::fileShareMenu(QPoint position) {
  if (!fileShare) return;
  QMenu menu;
  if (!fileShare->detail().isEmpty()) {
    menu.addAction(fileShare->detail())->setEnabled(false);
    menu.addSeparator();
  }
  QObject::connect(menu.addAction("Upload files…"), &QAction::triggered, this, [this] {
    auto selected = QFileDialog::getOpenFileUrls(nullptr, "Upload to FileShare");
    if (fileShare && !selected.isEmpty()) fileShare->enqueue(selected);
  });
  if (fileShare->busy())
    QObject::connect(menu.addAction("Cancel uploads"), &QAction::triggered, fileShare, &FileShare::cancel);
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
}

bool Panel::fileShareEvent(QEvent *event) {
  if (fileShareRect().isEmpty()) return false;
  if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop) {
    auto drop = static_cast<QDropEvent *>(event);
    bool accepts = fileShareAt(drop->position()) && (drop->possibleActions() & Qt::CopyAction) &&
                   FileShare::accepts(drop->mimeData()->urls());
    uploadHover = accepts && event->type() != QEvent::Drop;
    if (accepts) {
      drop->setDropAction(Qt::CopyAction);
      drop->accept();
      if (event->type() == QEvent::Drop) fileShare->enqueue(drop->mimeData()->urls());
    } else drop->ignore();
    refreshFileShare();
    return true;
  }
  if (event->type() == QEvent::DragLeave) {
    uploadHover = false;
    refreshFileShare();
    event->accept();
    return true;
  }
  if (event->type() == QEvent::MouseButtonPress) {
    auto mouse = static_cast<QMouseEvent *>(event);
    if (!fileShareAt(mouse->position())) return false;
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
    if (local.y() >= box.top() + 80) {
      if (fileShare->busy()) fileShare->cancel();
      else if (!fileShare->currentReceipt().isEmpty()) {
        auto receipt = fileShare->currentReceipt();
        copyFileShare(receipt[local.x() < box.center().x() ? "download_url" : "stored_path"].toString());
      } else fileShareMenu(mouse->globalPosition().toPoint());
    } else fileShareMenu(mouse->globalPosition().toPoint());
    return true;
  }
  return false;
}
