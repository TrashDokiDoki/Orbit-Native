#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

void portalRequest(QObject *parent, const QString &interface, const QString &method, QVariantList arguments,
                   QVariantMap options, std::function<void(uint, QVariantMap)> result);
