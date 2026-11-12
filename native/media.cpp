#include "media.h"
#include <utility>

#ifdef Q_OS_LINUX
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QTimer>
#elif defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

Media::Media(QObject *parent) : QObject(parent)
{
#ifdef Q_OS_LINUX
    auto *timer = new QTimer(this);
    timer->setTimerType(Qt::VeryCoarseTimer);
    timer->setInterval(2000);
    connect(timer, &QTimer::timeout, this, &Media::refresh);
    timer->start();
#endif
}

void Media::refresh()
{
#ifdef Q_OS_LINUX
    if (pending_) return;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;
    pending_ = true;
    auto query = QDBusMessage::createMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                              "org.freedesktop.DBus", "ListNames");
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(query, 1500), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        const QDBusPendingReply<QStringList> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) { pending_ = false; return; }
        QStringList players;
        for (const auto &name : reply.value()) {
            if (name.startsWith("org.mpris.MediaPlayer2.")) players.append(name);
        }
        if (players.removeOne(preferred_)) players.prepend(preferred_);
        inspect(players);
    });
#endif
}

#ifdef Q_OS_LINUX
void Media::inspect(QStringList players, Track fallback)
{
    if (players.isEmpty()) {
        player_ = preferred_;
        pending_ = false;
        if (changed) changed(fallback);
        return;
    }
    const QString name = players.takeFirst();
    auto query = QDBusMessage::createMethodCall(name, "/org/mpris/MediaPlayer2",
                                              "org.freedesktop.DBus.Properties", "GetAll");
    query << QStringLiteral("org.mpris.MediaPlayer2.Player");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(query, 900), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, name, players, fallback]() mutable {
        const QDBusPendingReply<QVariantMap> reply = *watcher;
        watcher->deleteLater();
        if (!reply.isError()) {
            const QVariantMap state = reply.value();
            const auto metadata = qdbus_cast<QVariantMap>(state.value("Metadata"));
            Track track{metadata.value("xesam:title").toString(),
                        metadata.value("xesam:artist").toStringList().join(", "),
                        QUrl(metadata.value("mpris:artUrl").toString()),
                        state.value("PlaybackStatus").toString() == "Playing"};
            if (track.playing) {
                preferred_ = player_ = name;
                pending_ = false;
                if (changed) changed(track);
                return;
            }
            if (fallback.title.isEmpty() && !track.title.isEmpty()) {
                preferred_ = name;
                fallback = track;
            }
        }
        inspect(players, fallback);
    });
}
#endif

void Media::send(Command command)
{
#ifdef Q_OS_WIN
    const WORD key = command == Previous ? VK_MEDIA_PREV_TRACK :
                     command == Next ? VK_MEDIA_NEXT_TRACK : VK_MEDIA_PLAY_PAUSE;
    INPUT events[2] = {};
    events[0].type = events[1].type = INPUT_KEYBOARD;
    events[0].ki.wVk = events[1].ki.wVk = key;
    events[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, events, sizeof(INPUT));
#elif defined(Q_OS_LINUX)
    if (player_.isEmpty()) return;
    const QString method = command == Previous ? "Previous" : command == Next ? "Next" : "PlayPause";
    auto call = QDBusMessage::createMethodCall(player_, "/org/mpris/MediaPlayer2",
                                             "org.mpris.MediaPlayer2.Player", method);
    QDBusConnection::sessionBus().asyncCall(call, 1500);
#endif
}
