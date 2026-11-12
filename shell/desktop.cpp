#include "desktop.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QJsonObject>
#include <QHash>
#include <QNetworkInterface>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <utility>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bluetoothapis.h>
#include <powrprof.h>
#elif defined(Q_OS_LINUX)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#endif

namespace {
QJsonObject error(const QString &message) { return {{"error", message}}; }

QJsonObject connections()
{
    bool haveEthernet = false, haveWifi = false, ethernetUp = false, wifiUp = false;
    for (const auto &network : QNetworkInterface::allInterfaces()) {
        const auto up = network.flags().testFlag(QNetworkInterface::IsUp) &&
                        network.flags().testFlag(QNetworkInterface::IsRunning);
        if (network.type() == QNetworkInterface::Wifi) { haveWifi = true; wifiUp |= up; }
        if (network.type() == QNetworkInterface::Ethernet) { haveEthernet = true; ethernetUp |= up; }
    }
    const bool wifi = haveWifi && !ethernetUp && (wifiUp || !haveEthernet);
    return {{"networkName", wifi ? "Wi-Fi" : "Ethernet"}, {"networkUp", wifi ? wifiUp : ethernetUp}};
}

#if defined(Q_OS_WIN)
const QString themeKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize");

QString bluetooth()
{
    BLUETOOTH_FIND_RADIO_PARAMS params{sizeof(BLUETOOTH_FIND_RADIO_PARAMS)};
    HANDLE radio = nullptr;
    const auto find = BluetoothFindFirstRadio(&params, &radio);
    if (!find) return "missing";
    if (radio) CloseHandle(radio);
    BluetoothFindRadioClose(find);
    return "available";
}

bool shutdownPrivilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token)) return false;
    TOKEN_PRIVILEGES privilege{};
    privilege.PrivilegeCount = 1;
    privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool found = LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &privilege.Privileges[0].Luid);
    SetLastError(ERROR_SUCCESS);
    const bool allowed = found && AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr) &&
                         GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    CloseHandle(token);
    return allowed;
}
#elif defined(Q_OS_LINUX)
bool desktopHas(const QString &name)
{
    return QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP")).contains(name, Qt::CaseInsensitive);
}

void process(QObject *context, const QString &exe, const QStringList &args,
             std::function<void(bool, QByteArray)> done)
{
    auto *child = new QProcess(context);
    QObject::connect(child, &QProcess::errorOccurred, context, [child, done](QProcess::ProcessError issue) {
        if (issue != QProcess::FailedToStart) return;
        done(false, {});
        child->deleteLater();
    });
    QObject::connect(child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), context,
                     [child, done](int code, QProcess::ExitStatus exit) {
        done(code == 0 && exit == QProcess::NormalExit, child->readAllStandardOutput());
        child->deleteLater();
    });
    QTimer::singleShot(6000, child, [child] { if (child->state() != QProcess::NotRunning) child->kill(); });
    child->start(exe, args);
}

QDBusMessage loginManager(const QString &method)
{
    return QDBusMessage::createMethodCall("org.freedesktop.login1", "/org/freedesktop/login1",
                                         "org.freedesktop.login1.Manager", method);
}
void loginCommand(QObject *context, const QDBusMessage &request, const DesktopResult &done)
{
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(request, 30000), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [watcher, done] {
        const QDBusPendingReply<> reply = *watcher;
        watcher->deleteLater();
        done(reply.isError() ? QJsonValue(error(QStringLiteral("Команда отклонена системной службой питания"))) : QJsonValue(true));
    });
}

void readLinuxTheme(QObject *context, std::function<void(bool)> done)
{
    auto query = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                               "org.freedesktop.portal.Settings", "Read");
    query << QStringLiteral("org.freedesktop.appearance") << QStringLiteral("color-scheme");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(query, 1500), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [watcher, done] {
        const QDBusPendingReply<QDBusVariant> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) { done(false); return; }
        auto value = reply.value().variant();
        if (value.metaType() == QMetaType::fromType<QDBusVariant>()) value = value.value<QDBusVariant>().variant();
        done(value.toUInt() == 1);
    });
}
#endif
}

QJsonValue openSystemSettings(const QString &page)
{
    const QHash<QString, QString> windows{{"ethernet","network-ethernet"}, {"wifi","network-wifi"},
        {"bluetooth","bluetooth"}, {"vpn","network-vpn"}, {"sound","sound"},
        {"theme","personalization-colors"}, {"notifications","notifications"}, {"apps","appsfeatures"}};
    if (!windows.contains(page)) return error(QStringLiteral("Неизвестный раздел параметров"));
#if defined(Q_OS_WIN)
    if (QDesktopServices::openUrl(QUrl("ms-settings:" + windows.value(page)))) return true;
#elif defined(Q_OS_LINUX)
    const auto gnome = QStandardPaths::findExecutable("gnome-control-center");
    const auto kde = QStandardPaths::findExecutable("systemsettings");
    const auto xfce = QStandardPaths::findExecutable("xfce4-settings-manager");
    const QHash<QString, QString> gnomePages{{"ethernet","network"}, {"wifi","wifi"}, {"bluetooth","bluetooth"},
        {"vpn","network"}, {"sound","sound"}, {"theme","appearance"}, {"notifications","notifications"}, {"apps","applications"}};
    if (!gnome.isEmpty() && (desktopHas("GNOME") || kde.isEmpty()) &&
        QProcess::startDetached(gnome, {gnomePages.value(page)})) return true;
    if (!kde.isEmpty() && QProcess::startDetached(kde, {})) return true;
    if (!xfce.isEmpty() && QProcess::startDetached(xfce, {})) return true;
#endif
    return error(QStringLiteral("Не удалось открыть параметры системы"));
}

QJsonValue startDictation()
{
#if defined(Q_OS_WIN)
    INPUT keys[4]{};
    for (auto &key : keys) key.type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[3].ki.wVk = VK_LWIN;
    keys[1].ki.wVk = keys[2].ki.wVk = 'H';
    keys[2].ki.dwFlags = keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(4, keys, sizeof(INPUT)) == 4) return true;
#endif
    return error(QStringLiteral("Системный голосовой ввод недоступен"));
}

void desktopControl(QObject *context, const QString &command, DesktopResult done)
{
    if (command == "status") {
        auto result = connections();
#if defined(Q_OS_WIN)
        QSettings theme(themeKey, QSettings::NativeFormat);
        result["dark"] = theme.value("AppsUseLightTheme", 1).toInt() == 0;
        result["bluetooth"] = bluetooth();
        done(result);
#elif defined(Q_OS_LINUX)
        const bool found = !QDir("/sys/class/bluetooth").entryList({"hci*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
        result["bluetooth"] = found ? "available" : "missing";
        readLinuxTheme(context, [result, done](bool dark) mutable { result["dark"] = dark; done(result); });
#else
        done(error(QStringLiteral("Управление системой недоступно")));
#endif
        return;
    }
    if (command == "theme") {
#if defined(Q_OS_WIN)
        QSettings theme(themeKey, QSettings::NativeFormat);
        const bool dark = theme.value("AppsUseLightTheme", 1).toInt() != 0;
        theme.setValue("AppsUseLightTheme", dark ? 0 : 1);
        theme.setValue("SystemUsesLightTheme", dark ? 0 : 1);
        theme.sync();
        if (theme.status() != QSettings::NoError) { done(error(QStringLiteral("Не удалось изменить тему Windows"))); return; }
        DWORD_PTR unused = 0;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),
                            SMTO_ABORTIFHUNG, 100, &unused);
        done(QJsonObject{{"dark", dark}});
#elif defined(Q_OS_LINUX)
        const auto gsettings = QStandardPaths::findExecutable("gsettings");
        if (gsettings.isEmpty() || !desktopHas("GNOME")) {
            done(error(QStringLiteral("Измените тему в настройках своего рабочего стола")));
            return;
        }
        process(context, gsettings, {"get", "org.gnome.desktop.interface", "color-scheme"},
            [context, gsettings, done](bool read, const QByteArray &value) {
                if (!read) { done(error(QStringLiteral("Не удалось прочитать тему GNOME"))); return; }
                const bool dark = !value.contains("prefer-dark");
                process(context, gsettings, {"set", "org.gnome.desktop.interface", "color-scheme",
                        dark ? "prefer-dark" : "default"},
                    [done, dark](bool ok, const QByteArray &) {
                        if (ok) done(QJsonObject{{"dark", dark}});
                        else done(error(QStringLiteral("Не удалось изменить тему GNOME")));
                    });
            });
#else
        done(error(QStringLiteral("Изменение темы недоступно")));
#endif
        return;
    }
#if defined(Q_OS_WIN)
    if (command == "lock") { done(LockWorkStation() ? QJsonValue(true) : QJsonValue(error(QStringLiteral("Не удалось заблокировать экран")))); return; }
    if (command == "sleep" || command == "restart" || command == "shutdown") {
        if (!shutdownPrivilege()) { done(error(QStringLiteral("Windows не разрешила команду питания"))); return; }
        const bool ok = command == "sleep" ? SetSuspendState(FALSE, FALSE, FALSE) :
            ExitWindowsEx(command == "restart" ? EWX_REBOOT : EWX_POWEROFF, SHTDN_REASON_MAJOR_APPLICATION);
        done(ok ? QJsonValue(true) : QJsonValue(error(QStringLiteral("Windows отклонила команду питания"))));
        return;
    }
#elif defined(Q_OS_LINUX)
    if (command == "lock") {
        auto query = loginManager("GetSessionByPID");
        query << quint32(QCoreApplication::applicationPid());
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(query, 4000), context);
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [context, watcher, done] {
            const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
            watcher->deleteLater();
            if (reply.isError()) { done(error(QStringLiteral("Не удалось определить текущий сеанс Linux"))); return; }
            const auto lock = QDBusMessage::createMethodCall("org.freedesktop.login1", reply.value().path(),
                                                             "org.freedesktop.login1.Session", "Lock");
            loginCommand(context, lock, done);
        });
        return;
    }
    const QHash<QString, QString> methods{{"sleep","Suspend"}, {"restart","Reboot"}, {"shutdown","PowerOff"}};
    if (methods.contains(command)) {
        auto query = loginManager(methods.value(command));
        query << true;
        loginCommand(context, query, done);
        return;
    }
#endif
    done(error(QStringLiteral("Команда управления системой недоступна")));
}
