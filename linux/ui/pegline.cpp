#include "panel.hpp"
#include <KGlobalAccel>
#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QDesktopServices>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QUrl>

extern "C" char *pg_snapshot();
extern "C" char *pg_action(const char *, const char *);
extern "C" void pg_string_free(char *);

extern "C" bool pg_image_valid(const char *path) {
  QImageReader reader(QString::fromUtf8(path));
  QSize size = reader.size();
  if (size.isValid())
    reader.setScaledSize(size.scaled(480, 480, Qt::KeepAspectRatio));
  return !reader.read().isNull();
}

static QJsonObject snapshot() {
  char *value = pg_snapshot();
  auto document = QJsonDocument::fromJson(value);
  pg_string_free(value);
  return document.object();
}

static QString json(const QJsonObject &value) {
  return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

static QIcon ownIcon() {
  QPixmap image(64, 64);
  image.fill(Qt::transparent);
  QPainter p(&image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor("#70b795"), 3));
  p.drawLine(4, 15, 60, 15);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor("#78aaf0"));
  p.drawRoundedRect(10, 21, 20, 28, 4, 4);
  p.setBrush(QColor("#9ad4b1"));
  p.drawRoundedRect(35, 25, 20, 25, 4, 4);
  p.setBrush(QColor("#dfbd89"));
  p.drawRoundedRect(18, 10, 5, 18, 2, 2);
  p.drawRoundedRect(42, 10, 5, 23, 2, 2);
  return QIcon(image);
}

class Desktop : public QObject {
public:
  QJsonObject state;
  QList<QPointer<Panel>> panels;
  QSystemTrayIcon tray;
  QMenu menu;
  QTimer scanTimer;
  QString lastError;
  QString lastWarning;
  QAction shortcut;
  QAction captureShortcut;
  bool capturing = false;

  Desktop()
      : shortcut("Show or hide Pegline", this),
        captureShortcut("Capture a region to Pegline", this) {
    tray.setIcon(ownIcon());
    tray.setToolTip("Pegline — screenshots within reach");
    tray.setContextMenu(&menu);
    auto add = [this](const QString &label,
                      const std::function<void()> &action) {
      QObject::connect(menu.addAction(label), &QAction::triggered, this,
                       action);
    };
    add("Show / hide line    Meta+Alt+T", [this] { control("Toggle"); });
    menu.addSeparator();
    add("Capture a region…    Meta+Shift+S", [this] { capture("--region"); });
    add("Capture current screen", [this] { capture("--current"); });
    add("Open screenshot folder", [this] {
      QDesktopServices::openUrl(QUrl::fromLocalFile(state["watch"].toString()));
    });
    add("Take all down (keep files)", [this] {
      for (auto value : state["items"].toArray())
        perform("dismiss", value.toObject()["path"].toString());
    });
    menu.addSeparator();
    add("Quit Pegline", [] { QApplication::quit(); });
    QObject::connect(&tray, &QSystemTrayIcon::activated, this,
                     [this](QSystemTrayIcon::ActivationReason reason) {
                       if (reason == QSystemTrayIcon::Trigger)
                         control("Toggle");
                     });
    shortcut.setObjectName("ToggleLine");
    shortcut.setProperty("componentName", "org.choppy.Pegline");
    shortcut.setProperty("componentDisplayName", "Pegline");
    const QKeySequence keys("Meta+Alt+T");
    // Autoload this action's saved binding. KDE drops conflicting keys without
    // taking another application's shortcut, and preserves user customizations.
    if (!KGlobalAccel::setGlobalShortcut(&shortcut, keys))
      report("Could not register Pegline's shortcut. Use its tray icon or KDE "
             "shortcut settings.");
    QObject::connect(&shortcut, &QAction::triggered, this,
                     [this] { control("Toggle"); });
    captureShortcut.setObjectName("CaptureRegion");
    captureShortcut.setProperty("componentName", "org.choppy.Pegline");
    captureShortcut.setProperty("componentDisplayName", "Pegline");
    captureShortcut.setAutoRepeat(false);
    if (!KGlobalAccel::setGlobalShortcut(&captureShortcut,
                                         QKeySequence("Meta+Shift+S")))
      report("Could not register Meta+Shift+S. Set Pegline's capture shortcut "
             "in KDE settings.");
    QObject::connect(&captureShortcut, &QAction::triggered, this,
                     [this] { capture("--region"); });
    refresh();
    for (QScreen *screen : QApplication::screens())
      addScreen(screen);
    QObject::connect(qApp, &QGuiApplication::screenAdded, this,
                     [this](QScreen *screen) { addScreen(screen); });
    QObject::connect(qApp, &QGuiApplication::screenRemoved, this,
                     [this](QScreen *screen) {
                       for (auto panel : panels)
                         if (panel && panel->screenName() == screen->name())
                           panel->retire();
                     });
    scanTimer.setInterval(500);
    QObject::connect(&scanTimer, &QTimer::timeout, this, [this] { refresh(); });
    scanTimer.start();
    tray.show();
  }

  void addScreen(QScreen *screen) {
    auto panel = new Panel(screen);
    panels.append(panel);
    panel->setItems(state["items"].toArray());
    panel->onAction = [this](QString operation, QString path) {
      perform(operation, path);
    };
    panel->onError = [this](QString error) { report(error); };
    panel->show();
  }

  void refresh() {
    state = snapshot();
    for (auto panel : panels)
      if (panel)
        panel->setItems(state["items"].toArray());
    QString error = state["error"].toString();
    if (!error.isEmpty() && error != lastError) {
      report(error);
    }
    lastError = error;
    QString warning = state["warning"].toString();
    if (!warning.isEmpty() && warning != lastWarning)
      report(warning);
    lastWarning = warning;
  }

  void report(const QString &message) {
    qWarning().noquote() << "Pegline:" << message;
    tray.showMessage("Pegline", message, QSystemTrayIcon::Warning);
  }

  void capture(const QString &mode) {
    if (capturing)
      return;
    capturing = true;
    control("Hide");
    QTimer::singleShot(250, this, [this, mode] {
      auto process = new QProcess(this);
      QObject::connect(process, &QProcess::errorOccurred, this,
                       [this, process](QProcess::ProcessError error) {
                         if (error == QProcess::FailedToStart) {
                           capturing = false;
                           report("Could not launch Spectacle: " +
                                  process->errorString());
                           process->deleteLater();
                         }
                       });
      QObject::connect(
          process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this, process](int code, QProcess::ExitStatus status) {
            capturing = false;
            if (status == QProcess::CrashExit || code != 0)
              report("Spectacle could not complete the capture");
            process->deleteLater();
          });
      process->start("spectacle",
                     {"--background", "--nonotify", "--new-instance", "--delay",
                      "0", "--release-capture", mode});
    });
  }

  QString perform(const QString &operation, const QString &path) {
    bool known = false;
    for (auto value : state["items"].toArray())
      if (value.toObject()["path"].toString() == path)
        known = true;
    if (!known)
      return json(
          {{"ok", false}, {"error", "Screenshot is no longer on the line"}});
    if (operation == "dismiss" || operation == "save") {
      auto op = operation.toUtf8(), file = path.toUtf8();
      char *reply = pg_action(op.constData(), file.constData());
      auto result = QJsonDocument::fromJson(reply).object();
      pg_string_free(reply);
      if (!result["ok"].toBool())
        report(result["error"].toString());
      if (operation == "save" && result["ok"].toBool())
        tray.showMessage("Pegline", "Saved to " + result["saved"].toString());
      refresh();
      return json(result);
    }
    bool ok = false;
    if (operation == "copy") {
      for (auto panel : panels)
        if (panel && panel->action("copy", path)) {
          ok = true;
          break;
        }
    } else if (operation == "open")
      ok = QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    else if (operation == "annotate") {
      control("Hide");
      ok = QProcess::startDetached("spectacle", {"--edit-existing", path});
    } else if (operation == "reveal")
      ok = QProcess::startDetached("dolphin", {"--select", path});
    if (!ok)
      report("Could not perform " + operation + " on " + path);
    return json({{"ok", ok}});
  }

  QString control(const QString &method, const QStringList &args = {}) {
    if (method == "Status") {
      QJsonObject result = state;
      QJsonArray surfaces;
      bool visible = false;
      for (auto panel : panels)
        if (panel) {
          visible |= panel->isRevealed();
          surfaces.append(QJsonObject{{"screen", panel->screenName()},
                                      {"width", panel->width()},
                                      {"height", panel->height()},
                                      {"revealed", panel->isRevealed()},
                                      {"flying", panel->isFlying()}});
        }
      result["visible"] = visible;
      result["surfaces"] = surfaces;
      result["platform"] = QApplication::platformName();
      result["capturing"] = capturing;
      return json(result);
    }
    if (method == "CaptureRegion") {
      capture("--region");
      return "ok";
    }
    if (method == "Action" && args.size() == 2)
      return perform(args[0], args[1]);
    if (method == "Quit") {
      QTimer::singleShot(0, qApp, &QApplication::quit);
      return "ok";
    }
    if (method != "Show" && method != "Hide" && method != "Toggle")
      return json({{"ok", false}, {"error", "Unknown control method"}});
    bool visible = false;
    for (auto panel : panels)
      if (panel)
        visible |= panel->isRevealed();
    bool target = method == "Show" || (method == "Toggle" && !visible);
    for (auto panel : panels)
      if (panel)
        panel->setRevealed(target);
    return "ok";
  }

  ~Desktop() override {
    for (auto panel : panels)
      if (panel)
        delete panel;
  }
};

class Controls : public QDBusVirtualObject {
  Desktop &desktop;

public:
  explicit Controls(Desktop &desktop) : desktop(desktop) {}
  QString introspect(const QString &) const override {
    return "<interface name=\"org.choppy.Pegline\">"
           "<method name=\"Show\"><arg type=\"s\" direction=\"out\"/></method>"
           "<method name=\"Hide\"><arg type=\"s\" direction=\"out\"/></method>"
           "<method name=\"Toggle\"><arg type=\"s\" "
           "direction=\"out\"/></method>"
           "<method name=\"CaptureRegion\"><arg type=\"s\" "
           "direction=\"out\"/></method>"
           "<method name=\"Quit\"><arg type=\"s\" direction=\"out\"/></method>"
           "<method name=\"Status\"><arg type=\"s\" "
           "direction=\"out\"/></method>"
           "<method name=\"Action\"><arg type=\"s\" direction=\"in\"/><arg "
           "type=\"s\" direction=\"in\"/><arg type=\"s\" "
           "direction=\"out\"/></method>"
           "</interface>";
  }
  bool handleMessage(const QDBusMessage &message,
                     const QDBusConnection &connection) override {
    if (message.interface() != "org.choppy.Pegline")
      return false;
    QStringList arguments;
    for (const auto &argument : message.arguments())
      arguments.append(argument.toString());
    connection.send(
        message.createReply(desktop.control(message.member(), arguments)));
    return true;
  }
};

extern "C" int pg_run(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setApplicationName("pegline");
  app.setApplicationDisplayName("Pegline");
  app.setDesktopFileName("org.choppy.Pegline");
  app.setQuitOnLastWindowClosed(false);
  app.setWindowIcon(ownIcon());
  QString command;
  for (const auto &argument : app.arguments()) {
    if (argument == "--show")
      command = "Show";
    if (argument == "--hide")
      command = "Hide";
    if (argument == "--toggle")
      command = "Toggle";
    if (argument == "--capture-region")
      command = "CaptureRegion";
    if (argument == "--quit")
      command = "Quit";
    if (argument == "--status")
      command = "Status";
  }
  auto bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    qCritical() << "Pegline requires a graphical session D-Bus";
    return 1;
  }
  if (bus.interface()->isServiceRegistered("org.choppy.Pegline")) {
    if (app.arguments().contains("--daemon"))
      return 0;
    QDBusInterface remote("org.choppy.Pegline", "/Pegline",
                          "org.choppy.Pegline", bus);
    auto reply = remote.call(command.isEmpty() ? "Toggle" : command);
    if (reply.type() == QDBusMessage::ErrorMessage) {
      qCritical() << reply.errorMessage();
      return 1;
    }
    if (command == "Status" && !reply.arguments().isEmpty())
      QTextStream(stdout) << reply.arguments().first().toString() << '\n';
    return 0;
  }
  if (command == "Status" || command == "Hide" || command == "Quit") {
    qWarning() << "Pegline is not running";
    return command == "Quit" ? 0 : 3;
  }
  if (!bus.registerService("org.choppy.Pegline")) {
    qCritical() << "Could not register Pegline session service";
    return 1;
  }
  Desktop desktop;
  Controls controls(desktop);
  if (!bus.registerVirtualObject("/Pegline", &controls)) {
    qCritical() << "Could not register Pegline controls";
    return 1;
  }
  if (command == "CaptureRegion")
    desktop.control("CaptureRegion");
  else if (command == "Show" || command == "Toggle")
    desktop.control("Show");
  return app.exec();
}
