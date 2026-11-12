#pragma once

#include <QJsonObject>
#include <functional>

class QObject;

using NetworkResult = std::function<void(QJsonObject)>;
bool validProxyFile(const QString &file);
void networkToolStatus(QObject *context, const QString &proxy, NetworkResult done);
void setNetworkTool(QObject *context, const QString &key, bool enable, const QString &proxy, NetworkResult done);
QJsonObject happStatus();
bool toggleHapp();
