#include "clipboard.hpp"
#include "data-control.h"
#include <QApplication>
#include <QSocketNotifier>
#include <cerrno>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>

class NativeClipboard : public QObject {
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_seat *seat = nullptr;
  ext_data_control_manager_v1 *manager = nullptr;
  ext_data_control_device_v1 *device = nullptr;
  ext_data_control_source_v1 *source = nullptr;
  QSocketNotifier *notifier = nullptr;
  QByteArray png, uri;

  static void global(void *data, wl_registry *registry, uint32_t name,
                     const char *interface, uint32_t) {
    auto self = static_cast<NativeClipboard *>(data);
    if (strcmp(interface, "ext_data_control_manager_v1") == 0 && !self->manager)
      self->manager =
          static_cast<ext_data_control_manager_v1 *>(wl_registry_bind(
              registry, name, &ext_data_control_manager_v1_interface, 1));
    if (strcmp(interface, "wl_seat") == 0 && !self->seat) {
      self->seat = static_cast<wl_seat *>(
          wl_registry_bind(registry, name, &wl_seat_interface, 1));
      static const wl_seat_listener listener = {
          [](void *, wl_seat *, uint32_t) {},
          [](void *, wl_seat *, const char *) {}};
      wl_seat_add_listener(self->seat, &listener, self);
    }
  }

  static void send(void *data, ext_data_control_source_v1 *, const char *type,
                   int32_t fd) {
    auto self = static_cast<NativeClipboard *>(data);
    QByteArray bytes = strcmp(type, "image/png") == 0 ? self->png : self->uri;
    // Transfers can exceed pipe capacity. Keep the GUI responsive while the
    // receiving application reads; the worker owns its bytes and descriptor.
    std::thread([bytes, fd] {
      qsizetype offset = 0;
      while (offset < bytes.size()) {
        ssize_t count = ::write(fd, bytes.constData() + offset,
                                size_t(bytes.size() - offset));
        if (count > 0)
          offset += count;
        else if (count < 0 && errno == EINTR)
          continue;
        else
          break;
      }
      ::close(fd);
    }).detach();
  }

public:
  explicit NativeClipboard(QObject *parent) : QObject(parent) {
    display = wl_display_connect(nullptr);
    if (!display)
      return;
    registry = wl_display_get_registry(display);
    static const wl_registry_listener registryListener = {
        global, [](void *, wl_registry *, uint32_t) {}};
    wl_registry_add_listener(registry, &registryListener, this);
    if (wl_display_roundtrip(display) < 0 || !manager || !seat)
      return;
    device = ext_data_control_manager_v1_get_data_device(manager, seat);
    static const ext_data_control_device_v1_listener deviceListener = {
        [](void *, ext_data_control_device_v1 *,
           ext_data_control_offer_v1 *offer) {
          ext_data_control_offer_v1_destroy(offer);
        },
        [](void *, ext_data_control_device_v1 *, ext_data_control_offer_v1 *) {
        },
        [](void *data, ext_data_control_device_v1 *device) {
          auto self = static_cast<NativeClipboard *>(data);
          ext_data_control_device_v1_destroy(device);
          self->device = nullptr;
        },
        [](void *, ext_data_control_device_v1 *, ext_data_control_offer_v1 *) {
        }};
    ext_data_control_device_v1_add_listener(device, &deviceListener, this);
    notifier = new QSocketNotifier(wl_display_get_fd(display),
                                   QSocketNotifier::Read, this);
    QObject::connect(notifier, &QSocketNotifier::activated, this, [this] {
      if (wl_display_dispatch(display) < 0) {
        notifier->setEnabled(false);
        device = nullptr;
      } else
        wl_display_flush(display);
    });
    wl_display_flush(display);
  }

  bool copy(const QByteArray &image, const QByteArray &file) {
    if (!device)
      return false;
    if (source)
      ext_data_control_source_v1_destroy(source);
    png = image;
    uri = file;
    source = ext_data_control_manager_v1_create_data_source(manager);
    static const ext_data_control_source_v1_listener sourceListener = {
        send, [](void *data, ext_data_control_source_v1 *source) {
          auto self = static_cast<NativeClipboard *>(data);
          if (self->source == source)
            self->source = nullptr;
          ext_data_control_source_v1_destroy(source);
        }};
    ext_data_control_source_v1_add_listener(source, &sourceListener, this);
    ext_data_control_source_v1_offer(source, "image/png");
    ext_data_control_source_v1_offer(source, "text/uri-list");
    ext_data_control_device_v1_set_selection(device, source);
    return wl_display_roundtrip(display) >= 0;
  }

  ~NativeClipboard() override {
    if (notifier)
      notifier->setEnabled(false);
    if (source)
      ext_data_control_source_v1_destroy(source);
    if (device)
      ext_data_control_device_v1_destroy(device);
    if (manager)
      ext_data_control_manager_v1_destroy(manager);
    if (seat)
      wl_seat_destroy(seat);
    if (registry)
      wl_registry_destroy(registry);
    if (display)
      wl_display_disconnect(display);
  }
};

bool setNativeClipboard(const QByteArray &png, const QByteArray &uri) {
  if (QApplication::platformName() != "wayland")
    return false;
  static auto clipboard = new NativeClipboard(qApp);
  return clipboard->copy(png, uri);
}
