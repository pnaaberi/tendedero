#pragma once
#include <QImage>
#include <QSize>
#include <QString>

bool isVideoCapture(const QString &path);
bool captureValid(const QString &path);
quint64 capturePreviewRevision();
QImage capturePreview(const QString &path, QSize bounds);
