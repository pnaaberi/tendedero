#include "panel.hpp"
#include "clipboard.hpp"
#include <LayerShellQt/Window>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDrag>
#include <QEnterEvent>
#include <QImageReader>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QUrl>
#include <algorithm>
#include <cmath>

static constexpr qint64 pulseDurationMs = 1800;

static qreal recoil(qint64 age) {
  if (age < 0 || age > pulseDurationMs)
    return 0;
  qreal seconds = age / 1000.0;
  return std::exp(-3.0 * seconds) * std::sin(10 * seconds);
}

Panel::Panel(QScreen *output) {
  assignedScreenName = output->name();
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
    if (!dragging && !currentMenu && !captureTimer.isActive())
      setRevealed(false);
  });
  captureTimer.setSingleShot(true);
  captureTimer.setTimerType(Qt::PreciseTimer);
  captureTimer.setInterval(10000);
  QObject::connect(&captureTimer, &QTimer::timeout, this, [this] {
    if (dragging || currentMenu || !pressed.isEmpty())
      captureTimer.start(1000);
    else
      setRevealed(false);
  });
  arrival.setStartValue(0.0);
  arrival.setEndValue(1.0);
  arrival.setDuration(800);
  arrival.setEasingCurve(QEasingCurve::InOutCubic);
  QObject::connect(&arrival, &QVariantAnimation::valueChanged, this,
                   [this] { update(); });
  QObject::connect(&arrival, &QVariantAnimation::finished, this,
                   [this] { stopFlight(true); });
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
  breezeTimer.setTimerType(Qt::PreciseTimer);
  QObject::connect(&breezeTimer, &QTimer::timeout, this, [this] {
    motionTime = clock.elapsed();
    if (motionTime - flexAt >= pulseDurationMs && breezeTimer.interval() != 40)
      breezeTimer.setInterval(40);
    updateInput();
    update();
  });
  QObject::connect(&slide, &QVariantAnimation::valueChanged, this,
                   [this](const QVariant &value) {
                     motionTime = clock.elapsed();
                     progress = value.toReal();
                     updateInput();
                     update();
                   });
  QObject::connect(
      output, &QScreen::geometryChanged, this, [this](const QRect &geometry) {
        resize(geometry.width(), isFlying() ? geometry.height() : 210);
      });
  updateInput();
}

void Panel::setItems(const QJsonArray &items) {
  if (retiring || dragging)
    return;
  if (!initialized) {
    initialItems = items;
    initialized = true;
  }
  if (items == lastItems && thumbnailsComplete)
    return;
  QList<Card> next;
  QString newCapture;
  bool complete = true;
  for (const auto &value : items) {
    auto item = value.toObject();
    QString path = item["path"].toString();
    QString stamp = item["modified"].toString() + ":" + item["size"].toString();
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
    if (!image.isNull()) {
      next.append({path, stamp, image});
      if (!initialItems.contains(value) && item["new"].toBool())
        newCapture = path;
    } else
      complete = false;
  }
  cards = next;
  lastItems = items;
  thumbnailsComplete = complete;
  updateInput();
  update();
  if (!newCapture.isEmpty())
    showCapture(newCapture);
  else if (!flightPath.isEmpty() &&
           !std::any_of(cards.begin(), cards.end(), [this](const Card &card) {
             return card.path == flightPath;
           }))
    stopFlight();
  if (cards.isEmpty() && captureTimer.isActive())
    setRevealed(false);
}

void Panel::showCapture(const QString &path) {
  stopFlight();
  setRevealed(true, false);
  captureTimer.start(10000);
  QImageReader reader(path);
  reader.setAutoTransform(true);
  QSize size = reader.size();
  if (size.isValid())
    reader.setScaledSize(size.scaled(600, 420, Qt::KeepAspectRatio));
  flightImage = reader.read();
  if (flightImage.isNull())
    return;
  flightPath = path;
  resize(width(), screen()->geometry().height());
  QSizeF preview = flightImage.size().scaled(
      int(width() * .65), int(height() * .55), Qt::KeepAspectRatio);
  flightStart = QRectF(QPointF(width() / 2.0 - preview.width() / 2,
                               height() / 2.0 - preview.height() / 2),
                       preview);
  arrival.start();
  updateInput();
}

void Panel::stopFlight(bool landed) {
  QString destination = flightPath;
  arrival.stop();
  flightPath.clear();
  flightImage = {};
  resize(width(), 210);
  if (landed && !destination.isEmpty())
    nudge(destination, 5);
  updateInput();
  update();
}

void Panel::setRevealed(bool value, bool animate) {
  if (retiring)
    return;
  bool opening = value && !revealed && animate;
  if (!value) {
    flexStrength = 0;
    for (auto &card : cards)
      card.nudgeStrength = 0;
    captureTimer.stop();
    stopFlight();
  }
  hideTimer.stop();
  hoverTimer.stop();
  revealed = value;
  if (opening)
    for (const auto &card : cards)
      nudge(card.path, 9);
  if (value)
    breezeTimer.start();
  else
    breezeTimer.stop();
  slide.stop();
  if (animate) {
    slide.setStartValue(progress);
    slide.setEndValue(value ? 1.0 : 0.0);
    slide.setDuration(value ? 480 : 180);
    QEasingCurve easing(value ? QEasingCurve::OutBack : QEasingCurve::InCubic);
    easing.setOvershoot(1.1);
    slide.setEasingCurve(easing);
    slide.start();
  } else {
    progress = value ? 1.0 : 0.0;
    updateInput();
    update();
  }
}

bool Panel::isRevealed() const { return revealed; }
void Panel::retire() {
  if (retiring)
    return;
  retiring = true;
  hide();
  hideTimer.stop();
  hoverTimer.stop();
  holdTimer.stop();
  breezeTimer.stop();
  captureTimer.stop();
  arrival.stop();
  slide.stop();
  if (currentMenu)
    currentMenu->close();
  if (dragging)
    QDrag::cancel();
  if (!currentMenu && !dragging)
    deleteLater();
}
QRect Panel::sensor() const { return QRect((width() - 32) / 2, 0, 32, 4); }
bool Panel::overSensor(QPointF point) const {
  return sensor().contains(
      QPoint(int(std::floor(point.x())), int(std::floor(point.y()))));
}

qreal Panel::sag() const {
  return std::min(26.0, width() * .015) +
         flexStrength * recoil(motionTime - flexAt);
}

void Panel::nudge(const QString &path, qreal strength) {
  for (auto &card : cards)
    if (card.path == path) {
      qint64 now = clock.elapsed();
      // Let the displayed swing settle before starting another pulse.
      if (card.nudgeStrength != 0 &&
          motionTime - card.nudgedAt < pulseDurationMs)
        return;
      card.nudgedAt = now;
      card.nudgeStrength = strength;
      flexAt = card.nudgedAt;
      flexStrength = std::min(6.0, std::abs(strength) * .75);
      breezeTimer.setInterval(16);
      break;
    }
}

QTransform Panel::cardTransform(int index) const {
  QRect box = cardRect(index);
  QPointF peg(box.center().x(), box.top() - 10);
  const auto &card = cards[index];
  qreal tilt = (int(qHash(card.path) % 7) - 3) * .5;
  qreal breeze = std::sin(motionTime / 1150.0 + index) * 1.1;
  qreal swing = card.nudgeStrength * recoil(motionTime - card.nudgedAt);
  QTransform transform;
  transform.translate(peg.x(), peg.y());
  transform.rotate(tilt + breeze + swing);
  transform.translate(-peg.x(), -peg.y());
  return transform;
}

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
      int(18 + 4 * sag() * fraction * (1 - fraction) +
          12 - (1 - progress) * 215);
  return QRect(int(x) - size.width() / 2 - 5, y, size.width() + 10,
               size.height() + 10);
}

int Panel::cardAt(QPointF point) const {
  // Visible cards take priority over a neighbor's transparent click padding.
  for (bool padded : {false, true})
    for (int i = int(cards.size()) - 1; i >= 0; --i) {
      if (cards[i].path == flightPath)
        continue;
      QRect box = cardRect(i);
      QPointF local = cardTransform(i).inverted().map(point);
      if (padded) {
        if (box.adjusted(-4, -12, 4, 4).contains(local.toPoint()))
          return i;
      } else {
        QPainterPath visible;
        visible.setFillRule(Qt::WindingFill);
        visible.addRoundedRect(box, 9, 9);
        QPointF peg(box.center().x(), box.top() - 10);
        visible.addRoundedRect(QRectF(peg.x() - 5, peg.y() - 4, 10, 22), 2, 2);
        if (visible.contains(local))
          return i;
      }
    }
  return -1;
}

void Panel::updateInput() {
  QRegion region(sensor());
  if (progress > .01)
    for (int i = 0; i < cards.size(); ++i)
      if (cards[i].path != flightPath)
        region |= QRegion(cardTransform(i).map(
            QPolygon(cardRect(i).adjusted(-6, -14, 6, 6))));
  setMask(region.intersected(QRect(0, 0, width(), height())));
}

QImage Panel::renderFrame() const {
  QImage frame(size() * devicePixelRatio(),
               QImage::Format_ARGB32_Premultiplied);
  frame.setDevicePixelRatio(devicePixelRatio());
  frame.fill(Qt::transparent);
  QPainter painter(&frame);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillRect(sensor(), QColor(119, 203, 171, 170));
  if (progress < .01)
    return frame;
  painter.setOpacity(std::clamp(progress, qreal(0), qreal(1)));
  painter.save();
  painter.translate(0, -(1 - progress) * 215);
  QPainterPath rope;
  rope.moveTo(0, 18);
  rope.quadTo(width() / 2.0, 18 + 2 * sag(), width(), 18);
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
    if (card.path == flightPath)
      continue;
    QRect box = cardRect(i);
    QPointF peg(box.center().x(), box.top() - 10);
    painter.save();
    painter.setTransform(cardTransform(i), true);
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
    painter.restore();
    if (copied == card.path && clock.elapsed() < copiedUntil) {
      QRectF movingBox = cardTransform(i).mapRect(box);
      QRect badge(int(movingBox.center().x()) - 36,
                  std::min(int(movingBox.bottom()) + 5, height() - 25), 72, 23);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor("#28684e"));
      painter.drawRoundedRect(badge, 11, 11);
      painter.setPen(Qt::white);
      painter.drawText(badge, Qt::AlignCenter, "Copied ✓");
    }
  }
  if (isFlying()) {
    int index = 0;
    while (index < cards.size() && cards[index].path != flightPath)
      ++index;
    QRectF target = cardRect(index).adjusted(5, 5, -5, -5);
    qreal t = arrival.currentValue().toReal();
    QPointF center = flightStart.center() * (1 - t) + target.center() * t;
    center.rx() += std::sin(t * M_PI) * 65;
    QSizeF size = flightStart.size() * (1 - t) + target.size() * t;
    QRectF box(center - QPointF(size.width() / 2, size.height() / 2), size);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 70));
    painter.drawRoundedRect(box.adjusted(-7, -2, 7, 13), 12, 12);
    painter.setBrush(QColor(233, 241, 235, 245));
    painter.drawRoundedRect(box.adjusted(-5, -5, 5, 5), 9, 9);
    QPainterPath clip;
    clip.addRoundedRect(box, 5, 5);
    painter.setClipPath(clip);
    painter.drawImage(box, flightImage);
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
  if (retiring)
    return false;
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
    if (QApplication::platformName() == "wayland") {
      if (!setNativeClipboard(png,
                              QUrl::fromLocalFile(path).toEncoded() + "\r\n")) {
        if (onError)
          onError("Could not copy screenshot to the Wayland clipboard");
        return false;
      }
    } else {
      auto mime = new QMimeData;
      mime->setImageData(image);
      mime->setData("image/png", png);
      mime->setUrls({QUrl::fromLocalFile(path)});
      QApplication::clipboard()->setMimeData(mime);
    }
    copied = path;
    copiedUntil = clock.elapsed() + 1400;
    nudge(path, 9);
    updateInput();
    update();
  } else if (onAction)
    onAction(operation, path);
  return true;
}

void Panel::mousePressEvent(QMouseEvent *event) {
  hideTimer.stop();
  int index = cardAt(event->position());
  if (index < 0) {
    if (overSensor(event->position()))
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
    QPointer<Panel> guard(this);
    currentMenu = &menu;
    menu.exec(event->globalPosition().toPoint());
    if (!guard)
      return;
    currentMenu = nullptr;
    if (retiring)
      deleteLater();
    else
      hideTimer.start();
    return;
  }
  if (event->button() != Qt::LeftButton)
    return;
  QRect box = cardRect(index);
  if (QRect(box.topLeft(), QSize(26, 26))
          .contains(cardTransform(index).inverted().map(event->position()).toPoint())) {
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
  int index = cardAt(event->position());
  if (index >= 0 && event->button() == Qt::LeftButton)
    action("open", cards[index].path);
}

void Panel::mouseMoveEvent(QMouseEvent *event) {
  hideTimer.stop();
  if (!revealed) {
    if (!overSensor(event->position()))
      hoverTimer.stop();
    else if (!hoverTimer.isActive())
      hoverTimer.start();
  }
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
  QPointer<Panel> guard(this);
  QPointer<QDrag> dragGuard(drag);
  drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
  if (!guard)
    return;
  // Never delete a source based on the reported action: file managers own
  // moves.
  if (dragGuard)
    dragGuard->deleteLater();
  dragging = false;
  if (retiring)
    deleteLater();
  else
    hideTimer.start();
}

bool Panel::event(QEvent *event) {
  if (retiring)
    return QRasterWindow::event(event);
  if (event->type() == QEvent::Enter) {
    hideTimer.stop();
    if (!revealed &&
        overSensor(static_cast<QEnterEvent *>(event)->position()))
      hoverTimer.start();
    else
      hoverTimer.stop();
  }
  if (event->type() == QEvent::Leave) {
    hoverTimer.stop();
    if (!dragging)
      hideTimer.start();
  }
  return QRasterWindow::event(event);
}
