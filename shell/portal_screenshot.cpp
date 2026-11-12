#include "portal_screenshot.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QImageReader>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVariantMap>
#include <utility>

namespace {
constexpr auto desktop = "org.freedesktop.portal.Desktop";
constexpr auto requestInterface = "org.freedesktop.portal.Request";

class Screenshot final : public QObject {
    Q_OBJECT

public:
    Screenshot(QObject *parent, std::function<void(QImage, QString)> result)
        : QObject(parent), result_(std::move(result)), bus_(QDBusConnection::sessionBus())
    {
    }

    void start()
    {
        if (!bus_.isConnected()) {
            finish({}, QStringLiteral("Системный портал снимков экрана недоступен"));
            return;
        }
        const auto token = "orbit_" + QUuid::createUuid().toString(QUuid::Id128);
        const auto sender = bus_.baseService().mid(1).replace('.', '_');
        if (!listen("/org/freedesktop/portal/desktop/request/" + sender + '/' + token)) {
            finish({}, QStringLiteral("Не удалось подключиться к порталу снимков экрана"));
            return;
        }
        auto request = QDBusMessage::createMethodCall(desktop, "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.Screenshot", "Screenshot");
        request << QString() << QVariantMap{{"handle_token", token}, {"interactive", false}};
        auto *reply = new QDBusPendingCallWatcher(bus_.asyncCall(request, 8000), this);
        connect(reply, &QDBusPendingCallWatcher::finished, this, [this, reply] {
            const QDBusPendingReply<QDBusObjectPath> result = *reply;
            reply->deleteLater();
            if (!result_) return;
            if (result.isError()) {
                finish({}, QStringLiteral("Системный портал не смог сделать снимок экрана"));
                return;
            }
            const auto actual = result.value().path();
            if (actual != path_ && !listen(actual))
                finish({}, QStringLiteral("Портал не вернул результат захвата"));
        });
        QTimer::singleShot(120000, this, [this] {
            if (!result_) return;
            auto close = QDBusMessage::createMethodCall(desktop, path_, requestInterface, "Close");
            bus_.asyncCall(close, 2000);
            finish({}, QStringLiteral("Время ожидания снимка истекло"));
        });
    }

private Q_SLOTS:
    void response(uint status, const QVariantMap &data)
    {
        if (!result_) return;
        if (status != 0) {
            finish({}, status == 1 ? QStringLiteral("Снимок отменён") : QStringLiteral("Не удалось сделать снимок"));
            return;
        }
        const auto uri = QUrl(data.value("uri").toString());
        if (!uri.isLocalFile() || !uri.host().isEmpty()) {
            finish({}, QStringLiteral("Портал не вернул файл снимка"));
            return;
        }
        QImageReader image(uri.toLocalFile());
        const auto size = image.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > 40000000) {
            finish({}, QStringLiteral("Размер снимка не поддерживается"));
            return;
        }
        auto screenshot = image.read();
        const auto issue = screenshot.isNull() ? QStringLiteral("Не удалось открыть снимок") : QString();
        finish(std::move(screenshot), issue);
    }

private:
    bool listen(const QString &path)
    {
        if (!path_.isEmpty()) bus_.disconnect(desktop, path_, requestInterface, "Response",
                                              this, SLOT(response(uint,QVariantMap)));
        path_ = path;
        return bus_.connect(desktop, path_, requestInterface, "Response", this, SLOT(response(uint,QVariantMap)));
    }

    void finish(QImage image, const QString &error)
    {
        auto callback = std::move(result_);
        if (callback) callback(std::move(image), error);
        deleteLater();
    }

    std::function<void(QImage, QString)> result_;
    QDBusConnection bus_;
    QString path_;
};
}

void portalScreenshot(QObject *parent, std::function<void(QImage, QString)> result)
{
    (new Screenshot(parent, std::move(result)))->start();
}

#include "portal_screenshot.moc"
