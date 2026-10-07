#pragma once
#include <QByteArray>

// Returns false when the compositor has no ext-data-control protocol.
// Qt remains responsible for clipboard operations on other platforms.
bool setNativeClipboard(const QByteArray &png, const QByteArray &uri);
