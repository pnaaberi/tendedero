#include "../ui/panel.hpp"
#include <QApplication>
#include <QClipboard>
#include <QEnterEvent>
#include <QFile>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

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
