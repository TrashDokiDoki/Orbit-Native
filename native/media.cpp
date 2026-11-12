#include "media.h"
#include <utility>
#include <QTimer>
#include <QDateTime>

#ifdef Q_OS_LINUX
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#elif defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
#ifdef Q_OS_WIN
void sendMediaKey(Media::Command command)
{
    const WORD key = command == Media::Previous ? VK_MEDIA_PREV_TRACK :
                     command == Media::Next ? VK_MEDIA_NEXT_TRACK : VK_MEDIA_PLAY_PAUSE;
    INPUT events[2] = {};
    events[0].type = events[1].type = INPUT_KEYBOARD;
    events[0].ki.wVk = events[1].ki.wVk = key;
    events[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, events, sizeof(INPUT));
}
#endif
}

#ifdef ORBIT_WINDOWS_MEDIA
Track readWindowsMedia(const QString &preferred, const Track &previous, int command);

void Media::windowsRequest(int command)
{
    if (pending_) {
        if (command >= 0) queuedCommand_ = command;
        return;
    }
    pending_ = true;
    worker_.start([this, selection = selection_, previous = previous_, command] {
        auto track = readWindowsMedia(selection, previous, command);
        QMetaObject::invokeMethod(this, [this, track = std::move(track), command] {
            pending_ = false;
            if (command >= 0 && track.source.isEmpty()) sendMediaKey(static_cast<Command>(command));
            previous_ = track;
            if (changed) changed(track);
            if (queuedCommand_ >= 0) windowsRequest(std::exchange(queuedCommand_, -1));
        }, Qt::QueuedConnection);
    });
}
#endif

Media::Media(QObject *parent) : QObject(parent)
{
#if defined(Q_OS_LINUX) || defined(ORBIT_WINDOWS_MEDIA)
    auto *timer = new QTimer(this);
    timer->setTimerType(Qt::VeryCoarseTimer);
    timer->setInterval(2000);
    connect(timer, &QTimer::timeout, this, &Media::refresh);
    timer->start();
#endif
}

void Media::refresh()
{
#ifdef ORBIT_WINDOWS_MEDIA
    windowsRequest(-1);
#elif defined(Q_OS_LINUX)
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
        sources_ = players;
        const QString first = selection_.isEmpty() ? preferred_ : selection_;
        if (players.removeOne(first)) players.prepend(first);
        inspect(players);
    });
#endif
}

#ifdef Q_OS_LINUX
void Media::inspect(QStringList players, Track fallback)
{
    if (players.isEmpty()) {
        player_ = fallback.source;
        pending_ = false;
        fallback.sources = sources_;
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
                        qdbus_cast<QStringList>(metadata.value("xesam:artist")).join(", "),
                        QUrl(metadata.value("mpris:artUrl").toString()),
                        state.value("PlaybackStatus").toString() == "Playing"};
            track.source = name;
            track.sources = sources_;
            track.album = metadata.value("xesam:album").toString();
            track.duration = metadata.value("mpris:length").toLongLong() / 1000000.0;
            if (state.contains("Position")) track.position = state.value("Position").toLongLong() / 1000000.0;
            track.updatedAt = QDateTime::currentMSecsSinceEpoch();
            if (track.playing || name == selection_) {
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

void Media::select(const QString &source)
{
#if defined(Q_OS_LINUX) || defined(ORBIT_WINDOWS_MEDIA)
    selection_ = source;
    refresh();
#else
    Q_UNUSED(source);
#endif
}

void Media::send(Command command)
{
#ifdef ORBIT_WINDOWS_MEDIA
    windowsRequest(command);
#elif defined(Q_OS_WIN)
    sendMediaKey(command);
#elif defined(Q_OS_LINUX)
    if (player_.isEmpty()) return;
    const QString method = command == Previous ? "Previous" : command == Next ? "Next" : "PlayPause";
    auto call = QDBusMessage::createMethodCall(player_, "/org/mpris/MediaPlayer2",
                                             "org.mpris.MediaPlayer2.Player", method);
    QDBusConnection::sessionBus().asyncCall(call, 1500);
#endif
}
