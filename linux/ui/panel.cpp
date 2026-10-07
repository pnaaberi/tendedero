#include "panel.hpp"
#include <LayerShellQt/Window>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDrag>
#include <QImageReader>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QUrl>
#include <cmath>

Panel::Panel(QScreen *output) {
  setScreen(output);
  setTitle("Pegline");
  setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
  QSurfaceFormat format;
  format.setAlphaBufferSize(8);
  setFormat(format);
  resize(output->geometry().width(), 210);
  if (QApplication::platformName() == "wayland") {
    auto layer = LayerShellQt::Window::get(this);
    layer->setScope("pegline");
    layer->setScreen(output);
    layer->setAnchors(
        LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) |
        LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
    layer->setLayer(LayerShellQt::Window::LayerTop);
    layer->setExclusiveZone(0);
    layer->setKeyboardInteractivity(
        LayerShellQt::Window::KeyboardInteractivityNone);
    layer->setActivateOnShow(false);
  } else {
    setFlags(flags() | Qt::WindowStaysOnTopHint);
    setPosition(output->geometry().topLeft());
  }
  clock.start();
  hideTimer.setSingleShot(true);
  hideTimer.setInterval(650);
  QObject::connect(&hideTimer, &QTimer::timeout, this, [this] {
    if (!dragging)
      setRevealed(false);
  });
  hoverTimer.setSingleShot(true);
  hoverTimer.setInterval(100);
  QObject::connect(&hoverTimer, &QTimer::timeout, this,
                   [this] { setRevealed(true); });
  holdTimer.setSingleShot(true);
  holdTimer.setInterval(450);
  QObject::connect(&holdTimer, &QTimer::timeout, this, [this] {
    if (!pressed.isEmpty() && !dragging) {
      held = true;
      action("annotate", pressed);
    }
  });
  breezeTimer.setInterval(40);
  QObject::connect(&breezeTimer, &QTimer::timeout, this, [this] { update(); });
  QObject::connect(&slide, &QVariantAnimation::valueChanged, this,
                   [this](const QVariant &value) {
                     progress = value.toReal();
                     updateInput();
                     update();
                   });
  QObject::connect(
      output, &QScreen::geometryChanged, this,
      [this](const QRect &geometry) { resize(geometry.width(), 210); });
  updateInput();
}

void Panel::setItems(const QJsonArray &items) {
  if (items == lastItems || dragging)
    return;
  QList<Card> next;
  for (const auto &value : items) {
    auto item = value.toObject();
    QString path = item["path"].toString(), stamp = item["modified"].toString();
    auto previous =
        std::find_if(cards.begin(), cards.end(), [&](const Card &card) {
          return card.path == path && card.stamp == stamp;
        });
    if (previous != cards.end()) {
      next.append(*previous);
      continue;
    }
    QImageReader reader(path);
    QSize size = reader.size();
    if (size.isValid())
      reader.setScaledSize(size.scaled(156, 112, Qt::KeepAspectRatio));
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (!image.isNull())
      next.append({path, stamp, image});
  }
  cards = next;
  lastItems = items;
  updateInput();
  update();
}

void Panel::setRevealed(bool value, bool animate) {
  hideTimer.stop();
  hoverTimer.stop();
  revealed = value;
  if (value)
    breezeTimer.start();
  else
    breezeTimer.stop();
  slide.stop();
  if (animate) {
    slide.setStartValue(progress);
    slide.setEndValue(value ? 1.0 : 0.0);
    slide.setDuration(value ? 300 : 180);
    slide.setEasingCurve(value ? QEasingCurve::OutCubic
                               : QEasingCurve::InCubic);
    slide.start();
  } else {
    progress = value ? 1.0 : 0.0;
    updateInput();
    update();
  }
}

bool Panel::isRevealed() const { return revealed; }
QRect Panel::sensor() const { return QRect((width() - 360) / 2, 0, 360, 3); }

QRect Panel::cardRect(int index) const {
  if (index < 0 || index >= cards.size())
    return {};
  qreal spacing =
      std::min(174.0, (width() - 48.0) / std::max(1, int(cards.size())));
  qreal x = width() / 2.0 + (index - (cards.size() - 1) / 2.0) * spacing;
  QSize size = cards[index].image.size().scaled(
      int(std::min(156.0, spacing - 18)), 112, Qt::KeepAspectRatio);
  qreal fraction = x / std::max(1, width());
  int y =
      int(18 + 4 * std::min(26.0, width() * .015) * fraction * (1 - fraction) +
          12 - (1 - progress) * 215);
  return QRect(int(x) - size.width() / 2 - 5, y, size.width() + 10,
               size.height() + 10);
}

int Panel::cardAt(QPoint point) const {
  for (int i = 0; i < cards.size(); ++i)
    if (cardRect(i).adjusted(-4, -12, 4, 4).contains(point))
      return i;
  return -1;
}

void Panel::updateInput() {
  QRegion region(sensor());
  if (progress > .01)
    for (int i = 0; i < cards.size(); ++i)
      region |= cardRect(i).adjusted(-6, -14, 6, 6);
  setMask(region.intersected(QRect(0, 0, width(), height())));
}

QImage Panel::renderFrame() const {
  QImage frame(size() * devicePixelRatio(),
               QImage::Format_ARGB32_Premultiplied);
  frame.setDevicePixelRatio(devicePixelRatio());
  frame.fill(Qt::transparent);
  QPainter painter(&frame);
  painter.setRenderHint(QPainter::Antialiasing);
  if (progress < .01) {
    painter.setPen(QPen(QColor(119, 203, 171, 170), 2));
    painter.drawLine(width() / 2 - 24, 1, width() / 2 + 24, 1);
    return frame;
  }
  painter.setOpacity(progress);
  painter.save();
  painter.translate(0, -(1 - progress) * 215);
  QPainterPath rope;
  rope.moveTo(0, 18);
  rope.quadTo(width() / 2.0, 18 + 2 * std::min(26.0, width() * .015), width(),
              18);
  QLinearGradient gradient(0, 0, width(), 0);
  gradient.setColorAt(0, Qt::transparent);
  gradient.setColorAt(.1, QColor(175, 190, 180, 220));
  gradient.setColorAt(.9, QColor(175, 190, 180, 220));
  gradient.setColorAt(1, Qt::transparent);
  painter.setPen(QPen(gradient, 1.5));
  painter.drawPath(rope);
  painter.restore();
  if (cards.isEmpty()) {
    QRect hint(width() / 2 - 230, 62 - int((1 - progress) * 215), 460, 35);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(25, 36, 35, 230));
    painter.drawRoundedRect(hint, 17, 17);
    painter.setPen(QColor("#e2ece7"));
    painter.drawText(hint, Qt::AlignCenter,
                     "Take a screenshot — it will hang here");
  }
  for (int i = 0; i < cards.size(); ++i) {
    const auto &card = cards[i];
    QRect box = cardRect(i);
    QPointF peg(box.center().x(), box.top() - 10);
    painter.save();
    painter.translate(peg);
    qreal tilt = (int(qHash(card.path) % 7) - 3) * .5;
    qreal breeze = std::sin(clock.elapsed() / 1150.0 + i) * .8;
    painter.rotate(tilt + breeze);
    painter.translate(-peg);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 60));
    painter.drawRoundedRect(box.adjusted(-3, 4, 3, 9), 11, 11);
    painter.setPen(QPen(QColor(255, 255, 255, 150), 1));
    painter.setBrush(QColor(233, 241, 235, 238));
    painter.drawRoundedRect(box, 9, 9);
    QPainterPath imageClip;
    imageClip.addRoundedRect(box.adjusted(5, 5, -5, -5), 5, 5);
    painter.save();
    painter.setClipPath(imageClip);
    painter.drawImage(box.adjusted(5, 5, -5, -5), card.image);
    painter.restore();
    QRectF pin(peg.x() - 5, peg.y() - 4, 10, 22);
    painter.setPen(QPen(QColor("#ad8558"), .7));
    painter.setBrush(QColor("#dec198"));
    painter.drawRoundedRect(pin, 2, 2);
    painter.setPen(QPen(QColor("#8c7151"), 1));
    painter.drawLine(QPointF(peg.x() - 3, peg.y() + 6),
                     QPointF(peg.x() + 3, peg.y() + 6));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(30, 40, 35, 195));
    painter.drawEllipse(QRect(box.left() + 3, box.top() + 3, 19, 19));
    painter.setPen(QPen(Qt::white, 1.3));
    painter.drawLine(box.topLeft() + QPoint(9, 9),
                     box.topLeft() + QPoint(16, 16));
    painter.drawLine(box.topLeft() + QPoint(16, 9),
                     box.topLeft() + QPoint(9, 16));
    if (copied == card.path && clock.elapsed() < copiedUntil) {
      QRect badge(box.center().x() - 36, box.bottom() + 5, 72, 23);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor("#28684e"));
      painter.drawRoundedRect(badge, 11, 11);
      painter.setPen(Qt::white);
      painter.drawText(badge, Qt::AlignCenter, "Copied ✓");
    }
    painter.restore();
  }
  return frame;
}

void Panel::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.drawImage(QPoint(0, 0), renderFrame());
}

void Panel::resizeEvent(QResizeEvent *event) {
  QRasterWindow::resizeEvent(event);
  updateInput();
}

bool Panel::action(const QString &operation, const QString &path) {
  auto card = std::find_if(cards.begin(), cards.end(),
                           [&](const Card &item) { return item.path == path; });
  if (card == cards.end())
    return false;
  if (operation == "copy") {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) {
      if (onError)
        onError("Cannot read image: " + reader.errorString());
      return false;
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
      if (onError)
        onError("Cannot encode screenshot for clipboard");
      return false;
    }
    auto mime = new QMimeData;
    mime->setImageData(image);
    mime->setData("image/png", png);
    mime->setUrls({QUrl::fromLocalFile(path)});
    QApplication::clipboard()->setMimeData(mime);
    copied = path;
    copiedUntil = clock.elapsed() + 1400;
    update();
  } else if (onAction)
    onAction(operation, path);
  return true;
}

void Panel::mousePressEvent(QMouseEvent *event) {
  hideTimer.stop();
  int index = cardAt(event->position().toPoint());
  if (index < 0) {
    setRevealed(true);
    return;
  }
  QString path = cards[index].path;
  if (event->button() == Qt::RightButton) {
    QMenu menu;
    for (const auto &entry :
         QList<QPair<QString, QString>>{{"Copy image", "copy"},
                                        {"Open", "open"},
                                        {"Annotate in Spectacle", "annotate"},
                                        {"Show in folder", "reveal"},
                                        {"Save a copy to Pictures", "save"},
                                        {"Take down (keep file)", "dismiss"}}) {
      auto item = menu.addAction(entry.first);
      QObject::connect(item, &QAction::triggered, this,
                       [this, entry, path] { action(entry.second, path); });
    }
    menu.exec(event->globalPosition().toPoint());
    hideTimer.start();
    return;
  }
  if (event->button() != Qt::LeftButton)
    return;
  QRect box = cardRect(index);
  if (QRect(box.topLeft(), QSize(26, 26))
          .contains(event->position().toPoint())) {
    action("dismiss", path);
    return;
  }
  pressed = path;
  down = event->position().toPoint();
  held = false;
  holdTimer.start();
}

void Panel::mouseReleaseEvent(QMouseEvent *event) {
  holdTimer.stop();
  QString path = pressed;
  pressed.clear();
  if (!path.isEmpty() && !held && !dragging &&
      event->button() == Qt::LeftButton)
    action("copy", path);
  held = false;
}

void Panel::mouseDoubleClickEvent(QMouseEvent *event) {
  holdTimer.stop();
  pressed.clear();
  int index = cardAt(event->position().toPoint());
  if (index >= 0 && event->button() == Qt::LeftButton)
    action("open", cards[index].path);
}

void Panel::mouseMoveEvent(QMouseEvent *event) {
  hideTimer.stop();
  if (!revealed && sensor().contains(event->position().toPoint()) &&
      !hoverTimer.isActive())
    hoverTimer.start();
  if (pressed.isEmpty() || held || dragging ||
      (event->position().toPoint() - down).manhattanLength() <
          QApplication::startDragDistance())
    return;
  holdTimer.stop();
  QString path = pressed;
  pressed.clear();
  dragging = true;
  auto drag = new QDrag(this);
  auto mime = new QMimeData;
  mime->setUrls({QUrl::fromLocalFile(path)});
  drag->setMimeData(mime);
  auto card = std::find_if(cards.begin(), cards.end(),
                           [&](const Card &item) { return item.path == path; });
  if (card != cards.end()) {
    drag->setPixmap(QPixmap::fromImage(card->image));
    drag->setHotSpot(QPoint(card->image.width() / 2, card->image.height() / 2));
  }
  drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
  // Never delete a source based on the reported action: file managers own
  // moves.
  drag->deleteLater();
  dragging = false;
  hideTimer.start();
}

bool Panel::event(QEvent *event) {
  if (event->type() == QEvent::Enter) {
    hideTimer.stop();
    if (!revealed)
      hoverTimer.start();
  }
  if (event->type() == QEvent::Leave) {
    hoverTimer.stop();
    if (!dragging)
      hideTimer.start();
  }
  return QRasterWindow::event(event);
}
