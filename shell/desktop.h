#pragma once
#include <QJsonValue>
#include <functional>

class QObject;

using DesktopResult = std::function<void(QJsonValue)>;
void desktopControl(QObject *context, const QString &command, DesktopResult done);
QJsonValue openSystemSettings(const QString &page);
QJsonValue startDictation();
