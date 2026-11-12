#include "portal_request.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QTimer>
#include <QUuid>
#include <utility>

namespace {
constexpr auto desktop = "org.freedesktop.portal.Desktop";
constexpr auto desktopPath = "/org/freedesktop/portal/desktop";
constexpr auto requestInterface = "org.freedesktop.portal.Request";

class Request final : public QObject {
    Q_OBJECT
public:
    Request(QObject *parent, std::function<void(uint, QVariantMap)> result)
        : QObject(parent), result_(std::move(result)), bus_(QDBusConnection::sessionBus())
    {
    }

    void start(const QString &interface, const QString &method, QVariantList arguments, QVariantMap options)
    {
        if (!bus_.isConnected()) { response(2, {}); return; }
        const auto token = "orbit_" + QUuid::createUuid().toString(QUuid::Id128);
        const auto sender = bus_.baseService().mid(1).replace('.', '_');
        if (!listen(QString(desktopPath) + "/request/" + sender + '/' + token)) { response(2, {}); return; }

        options["handle_token"] = token;
        arguments.append(options);
        auto request = QDBusMessage::createMethodCall(desktop, desktopPath, interface, method);
        request.setArguments(arguments);
        auto *reply = new QDBusPendingCallWatcher(bus_.asyncCall(request, 8000), this);
        connect(reply, &QDBusPendingCallWatcher::finished, this, [this, reply] {
            const QDBusPendingReply<QDBusObjectPath> result = *reply;
            reply->deleteLater();
            if (!result_) return;
            if (result.isError()) { response(2, {}); return; }
            const auto actual = result.value().path();
            if (actual != path_ && !listen(actual)) response(2, {});
        });
        QTimer::singleShot(120000, this, [this] {
            if (!result_) return;
            auto close = QDBusMessage::createMethodCall(desktop, path_, requestInterface, "Close");
            bus_.asyncCall(close, 2000);
            response(2, {});
        });
    }

private Q_SLOTS:
    void response(uint status, const QVariantMap &data)
    {
        auto callback = std::move(result_);
        if (callback) callback(status, data);
        deleteLater();
    }

private:
    bool listen(const QString &path)
    {
        if (!path_.isEmpty()) bus_.disconnect(desktop, path_, requestInterface, "Response",
                                              this, SLOT(response(uint,QVariantMap)));
        path_ = path;
        return bus_.connect(desktop, path_, requestInterface, "Response", this, SLOT(response(uint,QVariantMap)));
    }

    std::function<void(uint, QVariantMap)> result_;
    QDBusConnection bus_;
    QString path_;
};
}

void portalRequest(QObject *parent, const QString &interface, const QString &method, QVariantList arguments,
                   QVariantMap options, std::function<void(uint, QVariantMap)> result)
{
    (new Request(parent, std::move(result)))->start(interface, method, std::move(arguments), std::move(options));
}

#include "portal_request.moc"
