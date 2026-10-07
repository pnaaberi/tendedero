#include "../ui/panel.hpp"
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QJsonObject>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

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
    std::cout << "Native UI smoke passed: reveal, click-through, rendering, "
                 "image clipboard, cancelled drag, dismissal, hide.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Native UI smoke FAILED: " << e.what() << '\n';
    return 1;
  }
}
