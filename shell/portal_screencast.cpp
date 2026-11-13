#include "portal_screencast.h"
#include "portal_request.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUuid>
#include <utility>

namespace {
constexpr auto desktop = "org.freedesktop.portal.Desktop";
constexpr auto desktopPath = "/org/freedesktop/portal/desktop";
constexpr auto screencast = "org.freedesktop.portal.ScreenCast";
constexpr auto sessionInterface = "org.freedesktop.portal.Session";

QString portalError(uint status)
{
    return status == 1 ? QStringLiteral("Выбор экрана отменён")
                       : QStringLiteral("Системный портал не предоставил доступ к записи экрана");
}
}

PortalScreencast::PortalScreencast(QObject *parent) : QObject(parent) {}

PortalScreencast::~PortalScreencast()
{
    closeSession();
}

void PortalScreencast::start(Result result)
{
    if (result_ || !session_.isEmpty()) {
        result({}, QStringLiteral("Сеанс записи уже открыт"));
        return;
    }
    result_ = std::move(result);
    const auto token = "orbit_" + QUuid::createUuid().toString(QUuid::Id128);
    portalRequest(this, screencast, "CreateSession", {}, {{"session_handle_token", token}},
        [this](uint status, const QVariantMap &data) {
            if (status != 0) return finish({}, portalError(status));
            const auto handle = data.value("session_handle");
            session_ = handle.metaType() == QMetaType::fromType<QDBusObjectPath>()
                ? handle.value<QDBusObjectPath>().path() : handle.toString();
            if (!session_.startsWith("/org/freedesktop/portal/desktop/session/")) {
                session_.clear();
                return finish({}, portalError(2));
            }
            if (!QDBusConnection::sessionBus().connect(desktop, session_, sessionInterface, "Closed",
                                                       this, SLOT(sessionClosed())))
                return finish({}, portalError(2));
            selectSource();
        });
}

void PortalScreencast::selectSource()
{
    const QVariantList sessionArgs{QVariant::fromValue(QDBusObjectPath(session_))};
    portalRequest(this, screencast, "SelectSources", sessionArgs, {{"types", uint(1)}, {"multiple", false}},
        [this, sessionArgs](uint status, const QVariantMap &) {
            if (!result_) return;
            if (status != 0) return finish({}, portalError(status));
            auto arguments = sessionArgs;
            arguments.append(QString());
            portalRequest(this, screencast, "Start", arguments, {},
                [this](uint status, const QVariantMap &data) {
                    if (!result_) return;
                    if (status != 0) return finish({}, portalError(status));
                    const auto streams = data.value("streams");
                    if (streams.metaType() != QMetaType::fromType<QDBusArgument>())
                        return finish({}, portalError(2));
                    auto stream = streams.value<QDBusArgument>();
                    if (stream.currentSignature() != "a(ua{sv})") return finish({}, portalError(2));
                    stream.beginArray();
                    uint node = 0;
                    if (!stream.atEnd()) {
                        QVariantMap properties;
                        stream.beginStructure();
                        stream >> node >> properties;
                        stream.endStructure();
                    }
                    stream.endArray();
                    if (!node) return finish({}, portalError(2));
                    openRemote(node);
                });
        });
}

void PortalScreencast::openRemote(uint node)
{
    auto request = QDBusMessage::createMethodCall(desktop, desktopPath, screencast, "OpenPipeWireRemote");
    request << QVariant::fromValue(QDBusObjectPath(session_)) << QVariantMap();
    auto *watch = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(request, 8000), this);
    connect(watch, &QDBusPendingCallWatcher::finished, this, [this, watch, node] {
        const QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watch;
        watch->deleteLater();
        if (!result_) return;
        if (reply.isError() || !reply.value().isValid()) return finish({}, portalError(2));
        finish({reply.value(), node}, {});
    });
}

void PortalScreencast::finish(PortalScreenStream stream, const QString &error)
{
    auto result = std::move(result_);
    if (!error.isEmpty()) closeSession();
    if (result) result(std::move(stream), error);
}

void PortalScreencast::sessionClosed()
{
    session_.clear();
    if (result_) finish({}, portalError(2));
    else Q_EMIT closed();
}

void PortalScreencast::closeSession()
{
    if (session_.isEmpty()) return;
    auto bus = QDBusConnection::sessionBus();
    bus.disconnect(desktop, session_, sessionInterface, "Closed", this, SLOT(sessionClosed()));
    bus.asyncCall(QDBusMessage::createMethodCall(desktop, session_, sessionInterface, "Close"), 2000);
    session_.clear();
}
