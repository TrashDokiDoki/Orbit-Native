#pragma once

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <functional>

struct PortalShortcut {
    QString id;
    QString description;
    QString original;
    QString trigger;
    std::function<void()> invoke;
};

class PortalShortcuts final : public QObject {
    Q_OBJECT
public:
    explicit PortalShortcuts(QObject *parent);
    ~PortalShortcuts() override;
    void configure(QList<PortalShortcut> shortcuts, std::function<void(QStringList)> report);

private Q_SLOTS:
    void activated(const QDBusObjectPath &session, const QString &id, qulonglong time, const QVariantMap &options);

private:
    void close(const QString &path);
    void reportMissing(const QStringList &bound);

    QDBusConnection bus_;
    QList<PortalShortcut> shortcuts_;
    QString session_;
    quint64 generation_ = 0;
    bool reported_ = false;
    QStringList bound_;
    std::function<void(QStringList)> report_;
};
