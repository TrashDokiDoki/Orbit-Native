#include "network_tools.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <iterator>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <QWinEventNotifier>
#elif defined(Q_OS_LINUX)
#include <csignal>
#include <unistd.h>
#include <sys/syscall.h>
#endif

namespace {
QJsonObject error(const QString &message) { return {{"error", message}}; }

#if defined(Q_OS_WIN)
QJsonValue zapretStatus()
{
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return QJsonValue::Undefined;
    const auto service = OpenServiceW(manager, L"zapret", SERVICE_QUERY_STATUS);
    const auto issue = GetLastError();
    CloseServiceHandle(manager);
    if (!service) return issue == ERROR_SERVICE_DOES_NOT_EXIST ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonValue::Undefined);
    SERVICE_STATUS_PROCESS state{};
    DWORD size = 0;
    const bool ok = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&state), sizeof(state), &size);
    CloseServiceHandle(service);
    if (!ok) return QJsonValue::Undefined;
    if (state.dwCurrentState == SERVICE_RUNNING) return "Running";
    if (state.dwCurrentState == SERVICE_STOPPED) return "Stopped";
    return "Pending";
}

bool proxyProcess(const QString &path, bool stop)
{
    const auto processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const auto name = QFileInfo(path).fileName();
    bool found = false;
    for (BOOL more = Process32FirstW(processes, &entry); more; more = Process32NextW(processes, &entry)) {
        if (QString::fromWCharArray(entry.szExeFile).compare(name, Qt::CaseInsensitive)) continue;
        const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | (stop ? PROCESS_TERMINATE : 0), FALSE, entry.th32ProcessID);
        if (!handle) continue;
        wchar_t executable[32768];
        DWORD length = DWORD(std::size(executable));
        if (QueryFullProcessImageNameW(handle, 0, executable, &length) &&
            QDir::fromNativeSeparators(QString::fromWCharArray(executable, int(length))).compare(path, Qt::CaseInsensitive) == 0) {
            found = true;
            if (stop) TerminateProcess(handle, 0);
        }
        CloseHandle(handle);
    }
    CloseHandle(processes);
    return found;
}

void changeZapret(QObject *context, bool enable, const QString &proxy, const NetworkResult &done)
{
    wchar_t system[MAX_PATH];
    const auto size = GetSystemDirectoryW(system, MAX_PATH);
    if (!size || size >= MAX_PATH) { done(error(QStringLiteral("Системный каталог Windows недоступен"))); return; }
    const auto program = (QString::fromWCharArray(system, int(size)) + "\\sc.exe").toStdWString();
    SHELLEXECUTEINFOW run{};
    run.cbSize = sizeof(run);
    run.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    run.lpVerb = L"runas";
    run.lpFile = program.c_str();
    run.lpParameters = enable ? L"start zapret" : L"stop zapret";
    run.nShow = SW_HIDE;
    if (!ShellExecuteExW(&run) || !run.hProcess) {
        done(error(QStringLiteral("Подтвердите запрос Windows для управления службой Zapret")));
        return;
    }
    auto *watch = new QWinEventNotifier(run.hProcess, context);
    QObject::connect(watch, &QWinEventNotifier::activated, context, [context, watch, handle=run.hProcess, proxy, done] {
        watch->setEnabled(false);
        DWORD code = 1;
        GetExitCodeProcess(handle, &code);
        CloseHandle(handle);
        watch->deleteLater();
        if (code != 0) { done(error(QStringLiteral("Windows не переключила службу Zapret"))); return; }
        networkToolStatus(context, proxy, done);
    });
}
#elif defined(Q_OS_LINUX)
using ProcessResult = std::function<void(bool, QByteArray)>;
void systemctl(QObject *context, const QStringList &args, int timeout, ProcessResult done)
{
    const auto program = QStandardPaths::findExecutable("systemctl");
    if (program.isEmpty()) { done(false, {}); return; }
    auto *process = new QProcess(context);
    QObject::connect(process, &QProcess::errorOccurred, context, [process, done](QProcess::ProcessError issue) {
        if (issue != QProcess::FailedToStart) return;
        done(false, {});
        process->deleteLater();
    });
    QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), context,
        [process, done](int code, QProcess::ExitStatus status) {
            done(code == 0 && status == QProcess::NormalExit, process->readAllStandardOutput());
            process->deleteLater();
        });
    QTimer::singleShot(timeout, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
    process->start(program, args);
}

QJsonValue zapretStatus(const QByteArray &text)
{
    if (text.contains("LoadState=not-found")) return QJsonValue::Null;
    if (!text.contains("LoadState=loaded")) return QJsonValue::Undefined;
    if (text.contains("ActiveState=active")) return "Running";
    if (text.contains("ActiveState=inactive") || text.contains("ActiveState=failed")) return "Stopped";
    return "Pending";
}

bool proxyProcess(const QString &path, bool stop)
{
    bool found = false;
    const QDir proc("/proc");
    for (const auto &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool valid = false;
        const auto pid = entry.toInt(&valid);
        if (!valid || pid <= 1) continue;
        const QFileInfo image(proc.filePath(entry + "/exe"));
        if (image.symLinkTarget() != path) continue;
        found = true;
        if (!stop) break;
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
        const auto fd = syscall(SYS_pidfd_open, pid, 0);
        if (fd >= 0) {
            if (QFileInfo(image.filePath()).symLinkTarget() == path) syscall(SYS_pidfd_send_signal, fd, SIGTERM, nullptr, 0);
            close(int(fd));
            continue;
        }
#endif
        if (QFileInfo(image.filePath()).symLinkTarget() == path) kill(pid, SIGTERM);
    }
    return found;
}
#endif
}

bool validProxyFile(const QString &file)
{
    const QFileInfo info(file);
    if (!info.isAbsolute() || !info.isFile() || !info.isExecutable()) return false;
#if defined(Q_OS_WIN)
    static const QRegularExpression name("^TgWsProxy_windows(_arm64)?[.]exe$", QRegularExpression::CaseInsensitiveOption);
#else
    static const QRegularExpression name("^(TgWsProxy_linux_(amd64|arm64)|tg-ws-proxy|TgWsProxy)$");
#endif
    return name.match(info.fileName()).hasMatch();
}

void networkToolStatus(QObject *context, const QString &proxy, NetworkResult done)
{
    const auto path = validProxyFile(proxy) ? QFileInfo(proxy).canonicalFilePath() : QString{};
    auto result = QJsonObject{{"proxyConfigured", !path.isEmpty()}, {"proxyRunning", !path.isEmpty() && proxyProcess(path, false)}};
#if defined(Q_OS_WIN)
    const auto service = zapretStatus();
    if (!service.isUndefined()) result["zapret"] = service;
    done(result);
#elif defined(Q_OS_LINUX)
    systemctl(context, {"show", "-p", "LoadState", "-p", "ActiveState", "zapret.service"}, 1800,
        [result, done](bool, const QByteArray &text) mutable {
            const auto service = zapretStatus(text);
            if (!service.isUndefined()) result["zapret"] = service;
            done(result);
        });
#endif
}

void setNetworkTool(QObject *context, const QString &key, bool enable, const QString &proxy, NetworkResult done)
{
    if (key == "tg") {
        if (!validProxyFile(proxy)) { done(error(QStringLiteral("Сначала выберите файл TG WS Proxy"))); return; }
        const auto path = QFileInfo(proxy).canonicalFilePath();
        if (enable) {
            if (!proxyProcess(path, false) && !QProcess::startDetached(path, {}, QFileInfo(path).absolutePath())) {
                done(error(QStringLiteral("Не удалось запустить TG WS Proxy"))); return;
            }
        } else {
            proxyProcess(path, true);
        }
        QTimer::singleShot(350, context, [context, proxy, done] { networkToolStatus(context, proxy, done); });
        return;
    }
    if (key != "zapret") { done(error(QStringLiteral("Неизвестный инструмент"))); return; }
    networkToolStatus(context, proxy, [context, enable, proxy, done](QJsonObject status) {
        if (!status.contains("zapret") || status.value("zapret").isNull()) {
            done(error(QStringLiteral("Служба Zapret не установлена или недоступна"))); return;
        }
        if (status.value("zapret").toString() == (enable ? "Running" : "Stopped")) { done(status); return; }
#if defined(Q_OS_WIN)
        changeZapret(context, enable, proxy, done);
#elif defined(Q_OS_LINUX)
        systemctl(context, {enable ? "start" : "stop", "zapret.service"}, 60000,
            [context, proxy, done](bool ok, const QByteArray &) {
                if (ok) networkToolStatus(context, proxy, done);
                else done(error(QStringLiteral("systemd не разрешил переключить службу Zapret")));
            });
#endif
    });
}

QJsonObject happStatus()
{
    bool installed = false;
#if defined(Q_OS_WIN)
    QSettings association(QStringLiteral("HKEY_CLASSES_ROOT\\happ\\shell\\open\\command"), QSettings::NativeFormat);
    installed = !association.value(".").toString().isEmpty() || !association.value("Default").toString().isEmpty();
#elif defined(Q_OS_LINUX)
    installed = !QStandardPaths::findExecutable("happ").isEmpty() || !QStandardPaths::findExecutable("Happ").isEmpty();
#endif
    bool tunnel = false;
    for (const auto &network : QNetworkInterface::allInterfaces()) {
        const auto name = network.name() + " " + network.humanReadableName();
        if (network.flags().testFlag(QNetworkInterface::IsUp) &&
            (name.contains("happ-tun", Qt::CaseInsensitive) || name.contains("happ tunnel", Qt::CaseInsensitive))) tunnel = true;
    }
    return {{"installed", installed || tunnel}, {"mode", tunnel ? "tun" : "off"}};
}

bool toggleHapp()
{
    return QDesktopServices::openUrl(QUrl("happ://toggle"));
}
