#include "../ui/panel.hpp"
#include <QApplication>
#include <QClipboard>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

class PaintedPanel : public Panel {
public:
  using Panel::Panel;
  int frames = 0;

protected:
  void paintEvent(QPaintEvent *event) override {
    ++frames;
    Panel::paintEvent(event);
  }
};

class TrackedPanel : public Panel {
public:
  TrackedPanel(QScreen *screen, bool &active) : Panel(screen), active(active) {}

protected:
  void mousePressEvent(QMouseEvent *event) override {
    bool *tracking = &active;
    *tracking = true;
    Panel::mousePressEvent(event);
    *tracking = false;
  }

private:
  bool &active;
};

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  try {
    QTemporaryDir directory;
    QString path = directory.filePath("fixture screenshot.png");
    QImage fixture(120, 80, QImage::Format_ARGB32);
    fixture.fill(QColor("#3266dc"));
    require(fixture.save(path), "create real PNG fixture");
    if (app.arguments().contains("--motion")) {
      PaintedPanel motion(app.primaryScreen());
      motion.setItems(QJsonArray{QJsonObject{{"path", path}, {"modified", "1"}}});
      motion.show();
      motion.setRevealed(true, false);
      int restingY = motion.cardRect(0).top();
      motion.setRevealed(false, false);
      motion.setRevealed(true);
      QElapsedTimer reveal;
      reveal.start();
      int furthestY = restingY;
      while (reveal.elapsed() < 650) {
        QTest::qWait(20);
        furthestY = std::max(furthestY, motion.cardRect(0).top());
      }
      require(furthestY >= restingY + 3,
              "spring reveal must visibly overshoot its resting position");
      require(std::abs(motion.cardRect(0).top() - restingY) <= 2,
              "spring reveal must settle back onto the line");
      QTest::qWait(1200);
      require(motion.action("copy", path), "copy must start its feedback motion");
      QImage displayed = motion.renderFrame();
      QRegion displayedInput = motion.mask();
      QThread::msleep(110);
      require(motion.renderFrame() == displayed && motion.mask() == displayedInput,
              "cards must retain their displayed input pose between animation ticks");
      QElapsedTimer wobble;
      wobble.start();
      int smallestHeight = 10000, largestHeight = 0;
      int sampledFrames = 0;
      bool clickedMovingEdge = false;
      while (wobble.elapsed() < QApplication::doubleClickInterval() + 500 ||
             sampledFrames < 3) {
        QTest::qWait(20);
        QImage frame = motion.renderFrame();
        ++sampledFrames;
        QRect blueBounds;
        QPoint movingEdge(-1, -1);
        int edgeDisplacement = 0;
        QRect restingHit = motion.cardRect(0).adjusted(-4, -12, 4, 4);
        for (int y = 0; y < frame.height(); ++y)
          for (int x = 0; x < frame.width(); ++x) {
            QColor color = frame.pixelColor(x, y);
            QPoint logical(int(x / frame.devicePixelRatio()),
                           int(y / frame.devicePixelRatio()));
            if (color == QColor("#3266dc")) {
              blueBounds = blueBounds.united(QRect(x, y, 1, 1));
              require(motion.mask().contains(logical),
                      "the moving image must remain inside its input region");
            }
            if (color.alpha() > 200 &&
                logical.y() > restingHit.center().y() &&
                logical.y() < restingHit.bottom() - 5 &&
                !restingHit.contains(logical)) {
              int displacement = std::max(restingHit.left() - logical.x(),
                                          logical.x() - restingHit.right());
              if (displacement > edgeDisplacement) {
                edgeDisplacement = displacement;
                movingEdge = logical;
              }
            }
          }
        smallestHeight = std::min(smallestHeight, blueBounds.height());
        largestHeight = std::max(largestHeight, blueBounds.height());
        if (!clickedMovingEdge && movingEdge.x() >= 0) {
          require(motion.mask().contains(movingEdge),
                  "the moving card edge must receive input");
          // Delayed event handling must still hit the card currently displayed.
          QThread::msleep(QApplication::doubleClickInterval() + 50);
          auto sendMouse = [&](QEvent::Type type, Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(movingEdge), QPointF(movingEdge),
                              motion.mapToGlobal(movingEdge), Qt::LeftButton,
                              buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(&motion, &event);
          };
          app.clipboard()->clear();
          sendMouse(QEvent::MouseButtonPress, Qt::LeftButton);
          sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
          const auto *copied = app.clipboard()->mimeData();
          require(copied && copied->hasImage(),
                  "clicking the moving card edge must still copy its image");
          require(motion.mask().contains(movingEdge),
                  "copy feedback must preserve the swinging edge's input region");
          bool opened = false;
          motion.onAction = [&](QString operation, QString openedPath) {
            opened = operation == "open" && openedPath == path;
          };
          sendMouse(QEvent::MouseButtonDblClick, Qt::LeftButton);
          sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
          require(opened, "double-clicking the swinging edge must open its image");
          motion.onAction = {};
          clickedMovingEdge = true;
        }
      }
      if (largestHeight - smallestHeight < 4)
        std::cerr << "Motion sampled " << sampledFrames << " frames over "
                  << wobble.elapsed() << " ms; height delta "
                  << largestHeight - smallestHeight << '\n';
      require(largestHeight - smallestHeight >= 4,
              "copy feedback must visibly swing the card");
      require(clickedMovingEdge, "motion check must exercise a displaced card edge");
      QTest::qWait(2000);
      motion.frames = 0;
      QTest::qWait(320);
      require(motion.frames <= 11,
              "settled motion must return to the quieter frame rate");
      require(motion.action("copy", path), "start a fresh swing for frame pacing");
      QTest::qWait(20);
      motion.frames = 0;
      QTest::qWait(600);
      require(motion.frames >= 24,
              "active swings must draw frequently enough for smooth motion");
      motion.setRevealed(false, false);
      require(motion.mask().boundingRect().width() == 32 &&
                  motion.mask().boundingRect().height() == 4,
              "after motion only the visible trigger may receive input");
      QTest::qWait(40);
      motion.frames = 0;
      QTest::qWait(120);
      require(motion.frames == 0, "hidden cards must stop animation work");
      Panel later(app.primaryScreen());
      later.setItems(QJsonArray{QJsonObject{{"path", path}, {"modified", "1"}}});
      later.show();
      later.setRevealed(true, false);
      require(later.action("copy", path), "start a later swing regression");
      QTest::qWait(QApplication::doubleClickInterval() + 40);
      QRegion swingingInput = later.mask();
      require(later.action("copy", path), "copy again late in the swing");
      require(later.mask() == swingingInput,
              "copying late in an active swing must not snap the input pose");
      later.setRevealed(false, false);
      const QColor targetColor("#3266dc");
      for (int crowdedWidth : {800, 1080})
        for (int sample = 0; sample < 4; ++sample) {
          QJsonArray crowdedItems;
          QString crowdedTarget;
          int targetIndex = crowdedWidth == 800 ? 4 : 0;
          for (int i = 0; i < 8; ++i) {
            QString crowdedPath = directory.filePath(
                QString("crowded-%1-%2-%3.png").arg(crowdedWidth).arg(sample).arg(i));
            QImage square(120, 120, QImage::Format_ARGB32);
            square.fill(i == targetIndex ? targetColor : QColor("#cc0000"));
            require(square.save(crowdedPath), "create crowded line fixture");
            crowdedItems.append(QJsonObject{{"path", crowdedPath}, {"modified", "1"}});
            if (i == targetIndex)
              crowdedTarget = crowdedPath;
          }
          Panel crowded(app.primaryScreen());
          crowded.resize(crowdedWidth, 210);
          crowded.setItems(crowdedItems);
          require(crowded.cardRect(targetIndex).isValid(),
                  "crowded target thumbnail must load");
          crowded.onError = [](QString message) {
            std::cerr << message.toStdString() << '\n';
          };
          crowded.show();
          crowded.setRevealed(true, false);
          require(crowded.action("copy", crowdedTarget), "swing a card on a crowded line");
          QTest::qWait(crowdedWidth == 800 ? 145 : 445);
          QImage crowdedFrame = crowded.renderFrame();
          QList<QPoint> edges;
          int firstRow = int((crowded.cardRect(targetIndex).center().y() + 1) *
                             crowdedFrame.devicePixelRatio());
          for (int y = firstRow; y < crowdedFrame.height(); ++y) {
            int first = -1, last = -1;
            for (int x = 0; x < crowdedFrame.width(); ++x)
              if (crowdedFrame.pixelColor(x, y) == targetColor) {
                if (first < 0)
                  first = x;
                last = x;
              }
            if (first >= 0)
              edges.append({QPoint(first, y), QPoint(last, y)});
          }
          require(!edges.isEmpty(), "crowded line must show the target screenshot");
          for (QPoint edge : edges) {
            QPointF point((edge.x() + .5) / crowdedFrame.devicePixelRatio(),
                          (edge.y() + .5) / crowdedFrame.devicePixelRatio());
            QMouseEvent press(QEvent::MouseButtonPress, point, point,
                              crowded.mapToGlobal(point.toPoint()), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, point,
                                crowded.mapToGlobal(point.toPoint()), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            app.clipboard()->clear();
            QCoreApplication::sendEvent(&crowded, &press);
            QCoreApplication::sendEvent(&crowded, &release);
            QImage copied = app.clipboard()->image();
            require(!copied.isNull(), "visible card body edge must copy an image");
            require(copied.pixelColor(copied.width() / 2,
                                      copied.height() / 2) == targetColor,
                    "clicking a visible overlapping image must copy that image");
          }
        }
      std::cout << "Motion check passed: spring reveal, settling, copy swing, "
                   "moving input regions, edge clicks, hidden trigger.\n";
      return 0;
    }
    if (app.arguments().contains("--arrival")) {
      Panel restored(app.primaryScreen());
      restored.setItems({});
      restored.setItems(QJsonArray{
          QJsonObject{{"path", path}, {"modified", "1"}, {"new", false}}});
      require(
          !restored.isRevealed() && !restored.isFlying(),
          "files imported after startup stability checks must restore quietly");
      Panel arrivals(app.primaryScreen());
      arrivals.setItems({});
      arrivals.show();
      require(!arrivals.isRevealed(),
              "restored history must not open the line");
      QElapsedTimer sinceCapture;
      sinceCapture.start();
      arrivals.setItems(QJsonArray{
          QJsonObject{{"path", path}, {"modified", "1"}, {"new", true}}});
      require(arrivals.isRevealed(),
              "a new screenshot must reveal Pegline automatically");
      require(arrivals.isFlying(),
              "a new screenshot must start its flight into the line");
      auto blueBounds = [](const QImage &frame) {
        QRect bounds;
        for (int y = 0; y < frame.height(); ++y)
          for (int x = 0; x < frame.width(); ++x)
            if (frame.pixelColor(x, y) == QColor("#3266dc"))
              bounds = bounds.united(QRect(x, y, 1, 1));
        return bounds;
      };
      QImage start = arrivals.renderFrame();
      QRect startBounds = blueBounds(start);
      require(startBounds.center().y() > 210,
              "preview must begin below the line");
      require(!arrivals.mask().contains(startBounds.center()),
              "flying preview must pass input through");
      require(!arrivals.mask().contains(arrivals.cardRect(0).center()),
              "empty destination peg must pass input through during flight");
      QTest::qWait(400);
      QRect midway = blueBounds(arrivals.renderFrame());
      require(midway.center().y() < startBounds.center().y() - 30,
              "preview must visibly travel upward");
      QTest::qWait(550);
      require(!arrivals.isFlying() && arrivals.height() == 210,
              "flight must land and release the full-screen buffer");
      require(blueBounds(arrivals.renderFrame()).center().y() < 210,
              "landed image must appear on its peg");
      require(arrivals.mask().contains(arrivals.cardRect(0).center()),
              "landed card must receive input");
      QEvent leave(QEvent::Leave);
      QCoreApplication::sendEvent(&arrivals, &leave);
      QTest::qWait(900);
      require(
          arrivals.isRevealed(),
          "pointer leave must not shorten the capture's ten-second preview");
      QString second = directory.filePath("second.png");
      require(fixture.save(second), "create second capture");
      auto items = QJsonArray{
          QJsonObject{{"path", path}, {"modified", "1"}, {"new", true}},
          QJsonObject{{"path", second}, {"modified", "2"}, {"new", true}}};
      QElapsedTimer sinceSecond;
      sinceSecond.start();
      arrivals.setItems(items);
      QTest::qWait(int(10200 - sinceCapture.elapsed()));
      require(arrivals.isRevealed(),
              "another capture must reset the ten-second preview");
      QTest::qWait(int(9900 - sinceSecond.elapsed()));
      require(arrivals.isRevealed(),
              "capture must get the full ten-second preview");
      QTest::qWait(500);
      require(!arrivals.isRevealed(),
              "preview must hide after ten seconds without dismissal");
      items[1] =
          QJsonObject{{"path", second}, {"modified", "3"}, {"new", true}};
      arrivals.setItems(items);
      require(arrivals.isRevealed(), "next capture must reveal again");
      arrivals.setRevealed(false, false);
      require(!arrivals.isFlying() && arrivals.height() == 210,
              "manual hide must cancel the flight");
      QTest::qWait(1000);
      require(!arrivals.isRevealed(),
              "cancelled flight must not reveal the line again");
      require(QFile::exists(path) && QFile::exists(second),
              "auto-hide must retain captures");
      std::cout << "Capture arrival check passed: animated travel, input "
                   "passthrough, ten-second preview, reset, early hide.\n";
      return 0;
    }
    Panel panel(app.primaryScreen());
    panel.setItems(QJsonArray{QJsonObject{{"path", path}, {"modified", "1"}}});
    if (app.arguments().contains("--clipboard-failure")) {
      require(app.platformName() == "wayland",
              "failure check must use real Wayland");
      qputenv("WAYLAND_DISPLAY", "pegline-deliberately-unavailable");
      QString error;
      panel.onError = [&](QString message) { error = message; };
      require(!panel.action("copy", path),
              "unavailable native clipboard must not report success");
      require(!error.isEmpty(), "clipboard failure must explain the error");
      std::cout << "Wayland clipboard failure check passed.\n";
      return 0;
    }
    panel.show();
    panel.setRevealed(true, false);
    app.processEvents();
    require(panel.isRevealed(), "show command must reveal the line");
    QRect card = panel.cardRect(0);
    require(panel.mask().contains(card.center()), "card must receive input");
    require(!panel.mask().contains(QPoint(4, 170)),
            "empty space must pass input through");
    QImage render = panel.renderFrame();
    QPoint cardPixel(int((card.center().x() + .5) * render.devicePixelRatio()),
                     int((card.center().y() + .5) * render.devicePixelRatio()));
    require(!render.isNull() && render.pixelColor(cardPixel).alpha() > 0,
            "revealed screenshot must render");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, card.center());
    const QMimeData *clipboard = app.clipboard()->mimeData();
    require(clipboard && clipboard->hasImage(), "click must copy image MIME");
    require(clipboard->hasFormat("image/png") && clipboard->hasUrls(),
            "clipboard must offer PNG and real file URI");
    require(app.clipboard()->image().size() == QSize(120, 80),
            "copy must use full image, not thumbnail");
    QTest::mousePress(&panel, Qt::LeftButton, Qt::NoModifier, card.center());
    QTest::mouseMove(&panel, card.center() + QPoint(30, 30));
    QTest::mouseRelease(&panel, Qt::LeftButton, Qt::NoModifier,
                        card.center() + QPoint(30, 30));
    require(QFile::exists(path), "cancelled drag must retain source file");
    panel.action("dismiss", path);
    require(QFile::exists(path), "taking a card down must retain source file");
    panel.setRevealed(false, false);
    require(!panel.isRevealed(), "hide command must tuck the line away");
    require(!panel.mask().contains(card.center()),
            "hidden card must not intercept clicks");
    require(panel.mask().contains(QPoint(panel.width() / 2, 1)),
            "hidden line must keep its hover sensor");

    QImage hidden = panel.renderFrame();
    QRegion visibleTrigger;
    for (int y = 0; y < panel.height(); ++y)
      for (int x = 0; x < panel.width(); ++x)
        if (hidden
                .pixelColor(int((x + .5) * hidden.devicePixelRatio()),
                            int((y + .5) * hidden.devicePixelRatio()))
                .alpha())
          visibleTrigger |= QRect(x, y, 1, 1);
    require(visibleTrigger.boundingRect().width() <= 32 &&
                visibleTrigger.boundingRect().height() == 4,
            "hidden trigger must be narrower and four pixels thick");
    require(panel.mask() == visibleTrigger,
            "only visible trigger pixels must receive input when hidden");
    auto enterAt = [&](QPointF point) {
      QEnterEvent enter(point, point, panel.mapToGlobal(point.toPoint()));
      QCoreApplication::sendEvent(&panel, &enter);
    };
    QPoint outside(panel.width() / 2 + 100, 1);
    enterAt(outside);
    QTest::qWait(150);
    require(!panel.isRevealed(),
            "hover beside the visible trigger must not summon the line");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, outside);
    require(!panel.isRevealed(),
            "click beside the visible trigger must not summon the line");
    QPoint trigger = visibleTrigger.boundingRect().center();
    enterAt(trigger);
    QTest::qWait(150);
    require(panel.isRevealed(), "hover over the visible trigger must reveal");
    panel.setRevealed(false, false);
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, trigger);
    require(panel.isRevealed(), "click on the visible trigger must reveal");
    panel.setRevealed(false, false);

    QRect triggerBounds = visibleTrigger.boundingRect();
    QPointF triggerCenter(trigger);
    for (const auto &edge : QList<QPair<QPointF, bool>>{
             {{triggerCenter.x(), triggerBounds.bottom() + .75}, true},
             {{triggerBounds.right() + .75, triggerCenter.y()}, true},
             {{triggerBounds.left() - .25, triggerCenter.y()}, false},
             {{triggerCenter.x(), triggerBounds.top() - .25}, false},
             {{triggerBounds.right() + 1.0, triggerCenter.y()}, false},
             {{triggerCenter.x(), triggerBounds.bottom() + 1.0}, false}}) {
      enterAt(edge.first);
      QTest::qWait(150);
      require(panel.isRevealed() == edge.second,
              "fractional hover must match the visible trigger boundary");
      panel.setRevealed(false, false);
      QMouseEvent press(QEvent::MouseButtonPress, edge.first, edge.first,
                        panel.mapToGlobal(edge.first.toPoint()), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
      QCoreApplication::sendEvent(&panel, &press);
      require(panel.isRevealed() == edge.second,
              "fractional click must match the visible trigger boundary");
      panel.setRevealed(false, false);
      QMouseEvent move(QEvent::MouseMove, edge.first, edge.first,
                       panel.mapToGlobal(edge.first.toPoint()), Qt::NoButton,
                       Qt::NoButton, Qt::NoModifier);
      QCoreApplication::sendEvent(&panel, &move);
      QTest::qWait(150);
      require(panel.isRevealed() == edge.second,
              "fractional movement must match the visible trigger boundary");
      panel.setRevealed(false, false);
    }

    QString delayedPath = directory.filePath("delayed.png");
    QString unavailablePath = directory.filePath("unavailable.png");
    require(fixture.save(delayedPath), "create delayed thumbnail");
    auto delayedItems = QJsonArray{QJsonObject{
        {"path", delayedPath}, {"modified", "1"},
        {"size", QString::number(QFileInfo(delayedPath).size())}, {"new", true}}};
    require(QFile::rename(delayedPath, unavailablePath), "make thumbnail temporarily unavailable");
    Panel delayed(app.primaryScreen());
    delayed.setItems(delayedItems);
    require(!delayed.cardRect(0).isValid(), "unavailable thumbnail must not create a card");
    require(QFile::rename(unavailablePath, delayedPath), "restore thumbnail without changing its fingerprint");
    delayed.setItems(delayedItems);
    require(delayed.cardRect(0).isValid(),
            "unchanged snapshots must retry a previously unreadable thumbnail");
    require(!delayed.isRevealed() && !delayed.isFlying(),
            "retrying the first snapshot must still restore history quietly");

    Panel liveRetry(app.primaryScreen());
    liveRetry.setItems({});
    require(QFile::rename(delayedPath, unavailablePath), "temporarily hide live thumbnail");
    liveRetry.setItems(delayedItems);
    require(QFile::rename(unavailablePath, delayedPath), "restore live thumbnail");
    liveRetry.setItems(delayedItems);
    require(liveRetry.isRevealed() && liveRetry.isFlying(),
            "a new live capture must fly in once its thumbnail can be read");
    liveRetry.setItems(delayedItems);
    liveRetry.setRevealed(false, false);

    QString editedPath = directory.filePath("edited.png");
    require(fixture.save(editedPath), "create editable image");
    QString oldSize = QString::number(QFileInfo(editedPath).size());
    Panel edited(app.primaryScreen());
    edited.setItems(QJsonArray{QJsonObject{{"path", editedPath},
                                         {"modified", "1"},
                                         {"size", oldSize},
                                         {"new", false}}});
    QImage replacement(480, 100, QImage::Format_ARGB32);
    replacement.fill(QColor("#be8b58"));
    require(replacement.save(editedPath), "replace editable image");
    QString newSize = QString::number(QFileInfo(editedPath).size());
    require(newSize != oldSize, "replacement must have a different file size");
    edited.setItems(QJsonArray{QJsonObject{{"path", editedPath},
                                         {"modified", "1"},
                                         {"size", newSize},
                                         {"new", false}}});
    edited.setRevealed(true, false);
    QImage refreshed = edited.renderFrame();
    QPoint refreshedCenter = edited.cardRect(0).center();
    QPoint refreshedPixel(int((refreshedCenter.x() + .5) * refreshed.devicePixelRatio()),
                          int((refreshedCenter.y() + .5) * refreshed.devicePixelRatio()));
    require(refreshed.pixelColor(refreshedPixel) == QColor("#be8b58"),
            "a different-size image with unchanged timestamp must refresh its thumbnail");

    panel.setRevealed(true, false);
    bool revealedDuringMenu = false;
    QTimer::singleShot(10, &app, [&] {
      QEvent leave(QEvent::Leave);
      QCoreApplication::sendEvent(&panel, &leave);
    });
    QTimer::singleShot(900, &app, [&] {
      revealedDuringMenu = panel.isRevealed();
      for (QWidget *widget : QApplication::topLevelWidgets())
        if (auto menu = qobject_cast<QMenu *>(widget))
          menu->close();
    });
    QTest::mousePress(&panel, Qt::RightButton, Qt::NoModifier,
                     panel.cardRect(0).center());
    require(revealedDuringMenu,
            "pointer leave must not hide the line under an active card menu");
    QEvent afterMenuLeave(QEvent::Leave);
    QCoreApplication::sendEvent(&panel, &afterMenuLeave);
    QTest::qWait(900);
    require(!panel.isRevealed(),
            "manual reveal must hide when the pointer leaves after menu closure");

    bool handlerActive = false, destroyedDuringInteraction = false;
    QPointer<Panel> retiring =
        new TrackedPanel(app.primaryScreen(), handlerActive);
    retiring->setItems(
        QJsonArray{QJsonObject{{"path", path}, {"modified", "1"}}});
    retiring->show();
    retiring->setRevealed(true, false);
    QObject::connect(retiring, &QObject::destroyed, &app,
                     [&] { destroyedDuringInteraction = handlerActive; });
    QTimer::singleShot(10, &app, [&] {
      if (retiring)
        retiring->retire();
    });
    QTimer::singleShot(20, &app, [] {
      for (QWidget *widget : QApplication::topLevelWidgets())
        if (auto menu = qobject_cast<QMenu *>(widget))
          menu->close();
    });
    QTest::mousePress(retiring, Qt::RightButton, Qt::NoModifier,
                      retiring->cardRect(0).center());
    require(!destroyedDuringInteraction,
            "panel must outlive its nested context-menu interaction");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(retiring.isNull(),
            "retired panel must be released after interaction returns");
    std::cout << "Native UI smoke passed: reveal, click-through, rendering, "
                 "image clipboard, cancelled drag, dismissal, hide, menu "
                 "retirement.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Native UI smoke FAILED: " << e.what() << '\n';
    return 1;
  }
}
