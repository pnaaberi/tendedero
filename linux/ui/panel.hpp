#pragma once
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QRasterWindow>
#include <QTimer>
#include <QVariantAnimation>
#include <functional>
class QMenu;

class Panel : public QRasterWindow {
public:
  explicit Panel(QScreen *screen);
  void setItems(const QJsonArray &items);
  void setRevealed(bool revealed, bool animate = true);
  bool isRevealed() const;
  QRect cardRect(int index) const;
  QImage renderFrame() const;
  bool action(const QString &operation, const QString &path);
  std::function<void(QString, QString)> onAction;
  std::function<void(QString)> onError;
  bool isDragging() const { return dragging; }
  void retire();
  QString screenName() const { return assignedScreenName; }

protected:
  void paintEvent(QPaintEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseDoubleClickEvent(QMouseEvent *) override;
  bool event(QEvent *) override;

private:
  struct Card {
    QString path, stamp;
    QImage image;
  };
  QList<Card> cards;
  QJsonArray lastItems;
  QTimer hideTimer, holdTimer, hoverTimer, breezeTimer;
  QVariantAnimation slide;
  QElapsedTimer clock;
  bool revealed = false, dragging = false, held = false;
  bool retiring = false;
  QMenu *currentMenu = nullptr;
  QString assignedScreenName;
  qreal progress = 0;
  QString pressed, copied;
  QPoint down;
  qint64 copiedUntil = 0;
  QRect sensor() const;
  int cardAt(QPoint point) const;
  void updateInput();
};
