#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <functional>

void portalScreenshot(QObject *parent, std::function<void(QImage, QString)> result);
