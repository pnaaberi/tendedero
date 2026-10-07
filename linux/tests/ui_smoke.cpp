#include "../ui/panel.hpp"
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
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
    require(!render.isNull() && render.pixelColor(card.center()).alpha() > 0,
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
                 "image clipboard, cancelled drag, dismissal, hide, menu retirement.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Native UI smoke FAILED: " << e.what() << '\n';
    return 1;
  }
}
