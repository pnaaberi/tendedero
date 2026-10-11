#pragma once
#include <QImage>
#include <QSize>
#include <QString>

bool isVideoCapture(const QString &path);
QImage capturePreview(const QString &path, QSize bounds);
