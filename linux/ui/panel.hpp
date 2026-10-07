#pragma once
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QRasterWindow>
#include <QTimer>
#include <QTransform>
#include <QVariantAnimation>
#include <functional>
class QMenu;

class Panel : public QRasterWindow {
public:
  explicit Panel(QScreen *screen);
  void setItems(const QJsonArray &items);
  void setRevealed(bool revealed, bool animate = true);
  bool isRevealed() const;
  bool isFlying() const {
    return arrival.state() == QVariantAnimation::Running;
  }
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
    qint64 nudgedAt = 0;
    qreal nudgeStrength = 0;
  };
  QList<Card> cards;
  QJsonArray lastItems, initialItems;
  QTimer hideTimer, holdTimer, hoverTimer, breezeTimer, captureTimer;
  QVariantAnimation slide, arrival;
  QImage flightImage;
  QString flightPath;
  QRectF flightStart;
  bool initialized = false, thumbnailsComplete = false;
  QElapsedTimer clock;
  bool revealed = false, dragging = false, held = false;
  bool retiring = false;
  QMenu *currentMenu = nullptr;
  QString assignedScreenName;
  qreal progress = 0;
  QString pressed, copied;
  QPoint down;
  qint64 copiedUntil = 0;
  qint64 motionTime = 0;
  qint64 flexAt = 0;
  qreal flexStrength = 0;
  QRect sensor() const;
  bool overSensor(QPointF point) const;
  int cardAt(QPoint point) const;
  qreal sag() const;
  QTransform cardTransform(int index) const;
  void nudge(const QString &path, qreal strength);
  void updateInput();
  void showCapture(const QString &path);
  void stopFlight(bool landed = false);
};
