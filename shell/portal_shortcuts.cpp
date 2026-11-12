#include "portal_shortcuts.h"
#include "portal_request.h"

#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusMetaType>
#include <QUuid>
#include <utility>

struct ShortcutDefinition {
    QString id;
    QVariantMap options;
};
using Definitions = QList<ShortcutDefinition>;
Q_DECLARE_METATYPE(ShortcutDefinition)
Q_DECLARE_METATYPE(Definitions)

QDBusArgument &operator<<(QDBusArgument &argument, const ShortcutDefinition &value)
{
    argument.beginStructure();
    argument << value.id << value.options;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, ShortcutDefinition &value)
{
    argument.beginStructure();
    argument >> value.id >> value.options;
    argument.endStructure();
    return argument;
}

namespace {
constexpr auto desktop = "org.freedesktop.portal.Desktop";
constexpr auto desktopPath = "/org/freedesktop/portal/desktop";
constexpr auto shortcutsInterface = "org.freedesktop.portal.GlobalShortcuts";

bool sameShortcuts(const QList<PortalShortcut> &a, const QList<PortalShortcut> &b)
{
    if (a.size() != b.size()) return false;
    for (qsizetype i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].trigger != b[i].trigger || a[i].description != b[i].description) return false;
    return true;
}
}

PortalShortcuts::PortalShortcuts(QObject *parent) : QObject(parent), bus_(QDBusConnection::sessionBus())
{
    qDBusRegisterMetaType<ShortcutDefinition>();
    qDBusRegisterMetaType<Definitions>();
    bus_.connect(desktop, desktopPath, shortcutsInterface, "Activated", this,
                 SLOT(activated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
}

PortalShortcuts::~PortalShortcuts()
{
    close(session_);
}

void PortalShortcuts::configure(QList<PortalShortcut> shortcuts, std::function<void(QStringList)> report)
{
    const auto changed = !sameShortcuts(shortcuts, shortcuts_);
    shortcuts_ = std::move(shortcuts);
    report_ = std::move(report);
    if (!changed) { if (reported_) reportMissing(bound_); return; }
    reported_ = false;
    const auto generation = ++generation_;
    close(std::exchange(session_, QString()));
    if (shortcuts_.isEmpty()) { reportMissing({}); return; }
    const auto token = "orbit_keys_" + QUuid::createUuid().toString(QUuid::Id128);
    portalRequest(this, shortcutsInterface, "CreateSession", {}, {{"session_handle_token", token}},
        [this, generation](uint status, const QVariantMap &data) {
            const auto path = data.value("session_handle").toString();
            if (generation != generation_) { close(path); return; }
            if (status || path.isEmpty()) { reportMissing({}); return; }
            session_ = path;
            Definitions definitions;
            for (const auto &shortcut : std::as_const(shortcuts_))
                definitions.append({shortcut.id, {{"description", shortcut.description}, {"preferred_trigger", shortcut.trigger}}});
            portalRequest(this, shortcutsInterface, "BindShortcuts",
                          {QVariant::fromValue(QDBusObjectPath(path)), QVariant::fromValue(definitions), QString()}, {},
                [this, generation](uint result, const QVariantMap &data) {
                    if (generation != generation_) return;
                    QStringList bound;
                    if (!result) {
                        const auto definitions = qdbus_cast<Definitions>(data.value("shortcuts"));
                        for (const auto &definition : definitions) bound.append(definition.id);
                    }
                    reportMissing(bound);
                });
        });
}

void PortalShortcuts::activated(const QDBusObjectPath &session, const QString &id, qulonglong, const QVariantMap &)
{
    if (session.path() != session_) return;
    for (const auto &shortcut : std::as_const(shortcuts_)) {
        if (shortcut.id == id && shortcut.invoke) {
            shortcut.invoke();
            return;
        }
    }
}

void PortalShortcuts::close(const QString &path)
{
    if (!path.isEmpty())
        bus_.asyncCall(QDBusMessage::createMethodCall(desktop, path, "org.freedesktop.portal.Session", "Close"), 2000);
}

void PortalShortcuts::reportMissing(const QStringList &bound)
{
    reported_ = true;
    bound_ = bound;
    QStringList unavailable;
    for (const auto &shortcut : std::as_const(shortcuts_))
        if (!bound.contains(shortcut.id)) unavailable.append(shortcut.original);
    if (report_) report_(unavailable);
}
