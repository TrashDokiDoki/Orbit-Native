#include "media.h"
#include "audio.h"
#include "desktop.h"
#include "network_tools.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDrag>
#include <QBuffer>
#include <QFileIconProvider>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPixmap>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QHostAddress>
#include <QProcess>
#include <QTemporaryFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QRegularExpression>
#include <QStandardPaths>
#include <optional>
#include <QTemporaryDir>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QIcon>
#include <QMenu>
#include <QKeySequence>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QVersionNumber>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <array>
#include <functional>
#include <memory>
#include <map>
#include <vector>
#include <utility>
#include <tuple>
#include "webview/webview.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wrl.h>
#elif defined(Q_OS_LINUX)
#include "x11_input.h"
#include "portal_screenshot.h"
#include "wayland_video_encoder.h"
#include "portal_shortcuts.h"
#include "dictation.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusArgument>
#include <QDBusServiceWatcher>
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <gdk/gdkx.h>
#include <gtk-layer-shell.h>
#include <gio/gdesktopappinfo.h>
#include <webkit2/webkit2.h>
#endif

namespace {
QByteArray json(const QJsonValue &value)
{
    const auto array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}

QByteArray read(const QString &file)
{
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly)) return {};
    return input.readAll();
}

QJsonObject object(const QString &file)
{
    return QJsonDocument::fromJson(read(file)).object();
}

QJsonObject error(const QString &text) { return {{"error", text}}; }

class OrbitShellController final : public QObject {
#ifdef ORBIT_KWIN_LAYOUT_DBUS
    Q_OBJECT
#endif
public:
    OrbitShellController(QString directory, bool testing)
        : directory_(std::move(directory)), testing_(testing), media_(this),
          settings_(object(":/shell/defaults.json")), state_(object(":/shell/emptyState.json"))
    {
        if (!testing_) {
            const auto folder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QDir().mkpath(folder);
            store_ = QDir(folder).filePath("shell.json");
            const auto saved = object(store_);
            const auto settings = saved.value("settings").toObject();
            const auto state = saved.value("state").toObject();
            for (auto i = settings.begin(); i != settings.end(); ++i) settings_[i.key()] = i.value();
            for (auto i = state.begin(); i != state.end(); ++i) state_[i.key()] = i.value();
        }
#if defined(Q_OS_WIN) && defined(__ICoreWebView2_19_INTERFACE_DEFINED__)
        memoryIdle_.setSingleShot(true);
        connect(&memoryIdle_, &QTimer::timeout, this, [this] { setMemoryTarget(true); });
#endif
        collapse_.setSingleShot(true);
        connect(&collapse_, &QTimer::timeout, this, &OrbitShellController::applyBounds);
        alarm_.setSingleShot(true);
        connect(&alarm_, &QTimer::timeout, this, &OrbitShellController::timerDue);
    }

    int run()
    {
#ifdef Q_OS_LINUX
        if (testing_ && qEnvironmentVariableIsSet("ORBIT_TEST_DICTATION_PCM") &&
            !Dictation::test(qEnvironmentVariable("ORBIT_TEST_DICTATION_PCM"))) {
            qWarning("Orbit smoke: offline dictation did not recognize the test audio");
            return 3;
        }
#endif
        gpuEnabled_ = settings_.value("gpuAcceleration").toBool(true);
        if (testing_ && qEnvironmentVariableIsSet("ORBIT_TEST_SOFTWARE_RENDERING")) gpuEnabled_ = false;
#ifdef Q_OS_WIN
        if (!gpuEnabled_) {
            auto arguments = qgetenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS");
            if (!arguments.isEmpty()) arguments += ' ';
            arguments += "--disable-gpu";
            qputenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", arguments);
        }
#endif
#ifdef Q_OS_LINUX
        if (!gtk_init_check(nullptr, nullptr)) return 2;
        mainGtkWindow_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        auto *display = gtk_widget_get_display(mainGtkWindow_);
        wayland_ = GDK_IS_WAYLAND_DISPLAY(display);
        x11_ = GDK_IS_X11_DISPLAY(display);
        if (auto *visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(mainGtkWindow_)))
            gtk_widget_set_visual(mainGtkWindow_, visual);
        gtk_widget_set_app_paintable(mainGtkWindow_, TRUE);
        if (gtk_layer_is_supported()) {
            auto *window = GTK_WINDOW(mainGtkWindow_);
            gtk_layer_init_for_window(window);
            gtk_layer_set_namespace(window, "orbit-native");
            gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_TOP);
            gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
            const bool recordingTest = testing_ && qEnvironmentVariableIsSet("ORBIT_TEST_RECORD_OUTPUT");
            gtk_layer_set_keyboard_mode(window, recordingTest ? GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE :
                                                              GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        }
        main_ = webview_create(false, mainGtkWindow_);
#else
        main_ = webview_create(false, nullptr);
#endif
        if (!main_ || !open(main_, "index.html", true)) {
            if (main_) webview_destroy(main_);
#ifdef Q_OS_LINUX
            gtk_widget_destroy(mainGtkWindow_);
#endif
            return 2;
        }
        if (testing_ && !testSteamDownload()) {
            qWarning("Orbit smoke: Steam progress fixture failed");
            webview_destroy(main_);
#ifdef Q_OS_LINUX
            gtk_widget_destroy(mainGtkWindow_);
#endif
            return 3;
        }
        resize(false);
        visible(main_, true);
#ifdef Q_OS_WIN
        if (testing_ && !testCaptureVisibility()) {
            qWarning("Orbit smoke: Windows capture visibility toggle failed");
            webview_destroy(main_);
            return 3;
        }
#endif
        media_.changed = [this](const Track &track) { updateMedia(track); };
        if (!testing_ || qEnvironmentVariableIsSet("ORBIT_TEST_MEDIA")) media_.refresh();
#ifdef Q_OS_LINUX
        if (testing_ && x11_ && qEnvironmentVariableIsSet("ORBIT_TEST_KEYBOARD_LAYOUT")) {
            x11Input_ = std::make_unique<X11Input>();
            updateKeyboard();
            connect(&keyboardPoll_, &QTimer::timeout, this, &OrbitShellController::updateKeyboard);
            keyboardPoll_.start(500);
        }
#endif
        if (!testing_) {
#ifdef Q_OS_LINUX
            if (wayland_) portalShortcuts_ = std::make_unique<PortalShortcuts>(this);
            else if (x11_) x11Input_ = std::make_unique<X11Input>();
#endif
        }
#ifdef Q_OS_LINUX
        if (wayland_ && (!testing_ || qEnvironmentVariableIsSet("ORBIT_TEST_KEYBOARD_LAYOUT")) &&
            qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':').contains("KDE", Qt::CaseInsensitive)) {
            updateKeyboard();
            auto bus = QDBusConnection::sessionBus();
            bus.connect("org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts",
                        "layoutChanged", this, SLOT(refreshKdeLayout()));
            bus.connect("org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts",
                        "layoutsReconfigured", this, SLOT(refreshKdeLayout()));
            auto *watcher = new QDBusServiceWatcher("org.kde.keyboard", bus,
                QDBusServiceWatcher::WatchForOwnerChange, this);
            connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                    [this] { refreshKdeLayout(); });
            refreshKdeLayout();
        }
#endif
        if (!testing_) {
            connect(&keyboardPoll_, &QTimer::timeout, this, &OrbitShellController::updateKeyboard);
            keyboardPoll_.start(500);
            connect(&steamPoll_, &QTimer::timeout, this, [this] { refreshSteamDownload(); });
            steamPoll_.start(5000);
            refreshSteamDownload();
            updateKeyboard();
            registerHotkeys();
            startTray();
            updateTimer_.setInterval(6 * 60 * 60 * 1000);
            updateTimer_.setTimerType(Qt::VeryCoarseTimer);
            connect(&updateTimer_, &QTimer::timeout, this, &OrbitShellController::notifyUpdate);
            updateTimer_.start();
            QTimer::singleShot(10000, this, &OrbitShellController::notifyUpdate);
            connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &OrbitShellController::captureClipboard);
        }
        if (testing_) QTimer::singleShot(20000, this, &OrbitShellController::quit);
        scheduleTimer();
        webview_run(main_);
        shuttingDown_ = true;
        keyboardPoll_.stop();
        steamPoll_.stop();
        collapse_.stop();
        alarm_.stop();
        updateTimer_.stop();
        media_.changed = {};
#ifdef Q_OS_WIN
#if defined(__ICoreWebView2_19_INTERFACE_DEFINED__)
        memoryIdle_.stop();
#endif
        drainWebViewReplies();
#endif
        if (testing_) std::fprintf(stderr, "Orbit smoke: event loop stopped\n");
        if (tray_) tray_->setContextMenu(nullptr);
        if (editor_) webview_destroy(editor_);
        if (preferences_) webview_destroy(preferences_);
        clearHotkeys();
#ifdef Q_OS_LINUX
        portalShortcuts_.reset();
#endif
        webview_destroy(main_);
#ifdef Q_OS_LINUX
        gtk_widget_destroy(mainGtkWindow_);
#endif
        if (testing_) std::fprintf(stderr, "Orbit smoke: views released\n");
        return testing_ && !passed_ ? 1 : 0;
    }

private:
    void quit()
    {
        if (shuttingDown_) return;
        shuttingDown_ = true;
        webview_terminate(main_);
    }

#ifdef Q_OS_WIN
    void drainWebViewReplies()
    {
        // WebView 0.12 destroys its controller before draining its message queue.
        // Complete dispatched replies while the corresponding controllers are still alive.
        for (const auto window : {main_, editor_, preferences_}) {
            if (window && webview_dispatch(window, [](webview_t, void *context) {
                    --*static_cast<int *>(context);
                }, &shutdownReplies_) == WEBVIEW_ERROR_OK) ++shutdownReplies_;
        }
        const auto deadline = GetTickCount64() + 500;
        while (shutdownReplies_ > 0 && GetTickCount64() < deadline) {
            MSG message;
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message != WM_QUIT) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            } else MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
#endif

    static QString coverData(QIODevice *input)
    {
        QImageReader decoder(input);
        const auto size = decoder.size();
        if (!size.isValid() || size.width() > 4096 || size.height() > 4096) return {};
        decoder.setScaledSize(size.scaled(128, 128, Qt::KeepAspectRatio));
        QByteArray png;
        QBuffer output(&png);
        output.open(QIODevice::WriteOnly);
        if (!decoder.read().save(&output, "PNG")) return {};
        return "data:image/png;base64," + QString::fromLatin1(png.toBase64());
    }

    void updateRestriction(const QString &foregroundName)
    {
        bool restricted = false;
        for (auto name : settings_.value("restrictedApps").toString().split(QRegularExpression("[,\\r\\n]"), Qt::SkipEmptyParts)) {
            name = name.trimmed().toLower();
            if (name.endsWith(".exe")) name.chop(4);
            if (!name.isEmpty() && name == foregroundName) { restricted = true; break; }
        }
        if (restricted != restricted_) {
            restricted_ = restricted;
            updateMedia(currentTrack_);
        }
    }

    void updateAutohide(bool fullscreen, bool cursorOver)
    {
        const bool enabled = settings_.value("autohide").toBool() ||
            (restricted_ && settings_.value("ruleMode").toString() == "hide");
        if (!enabled) {
            if (autoHidden_) {
                autoHidden_ = false;
                applyBounds();
            }
            return;
        }
        if (fullscreen || expanded_ || mini_ || collapse_.isActive() ||
            QDateTime::currentMSecsSinceEpoch() < indicatorUntil_) return;
        if (autoHidden_ == cursorOver) {
            autoHidden_ = !cursorOver;
            applyBounds();
        }
    }

#ifdef Q_OS_WIN
    void updateWindowsVisibility(HWND foreground, DWORD owner)
    {
        if (!foreground) return;
        if (owner != GetCurrentProcessId() && foreground != lastRuleWindow_) {
            lastRuleWindow_ = foreground;
            foregroundName_.clear();
            if (auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner)) {
                std::array<wchar_t, 2048> name{};
                DWORD size = DWORD(name.size());
                if (QueryFullProcessImageNameW(process, 0, name.data(), &size))
                    foregroundName_ = QFileInfo(QString::fromWCharArray(name.data(), size)).completeBaseName().toLower();
                CloseHandle(process);
            }
        }
        updateRestriction(foregroundName_);
        if (capturePending_ || !snapshot_.isEmpty()) return;
        const auto island = static_cast<HWND>(webview_get_window(main_));
        MONITORINFO monitor{sizeof(MONITORINFO)};
        RECT bounds{}, active{};
        if (!GetMonitorInfoW(MonitorFromWindow(island, MONITOR_DEFAULTTOPRIMARY), &monitor) ||
            !GetWindowRect(island, &bounds)) return;
        const auto area = monitor.rcMonitor;
        const bool fullscreen = owner != GetCurrentProcessId() && foreground != GetShellWindow() &&
            GetWindowRect(foreground, &active) && !(GetWindowLongPtrW(foreground, GWL_STYLE) & WS_CAPTION) &&
            active.left <= area.left && active.right >= area.right &&
            active.top <= area.top && active.bottom >= area.bottom;
        const bool hide = settings_.value("hideFullscreen").toBool() && fullscreen && !recording_;
        if (hide != hiddenFullscreen_ || (hide && IsWindowVisible(island))) {
            hiddenFullscreen_ = hide;
            visible(main_, !hide);
        }
        POINT cursor{};
        if (!GetCursorPos(&cursor)) return;
        const bool over = cursor.x >= bounds.left && cursor.x < bounds.right &&
            cursor.y >= area.top && cursor.y < bounds.bottom;
        updateAutohide(hide, over);
    }
#elif defined(Q_OS_LINUX)
    void updateX11Visibility()
    {
        if (!x11Input_) return;
        const auto active = x11Input_->foreground();
        const auto ownPid = static_cast<unsigned long>(QCoreApplication::applicationPid());
        if (active.window && active.pid != ownPid) {
            lastExternalX11_ = active.window;
            if (active.pid != lastForegroundPid_) {
                lastForegroundPid_ = active.pid;
                const QFileInfo image(QString("/proc/%1/exe").arg(active.pid));
                foregroundName_ = QFileInfo(image.symLinkTarget()).completeBaseName().toLower();
            }
        }
        updateRestriction(foregroundName_);
        if (capturePending_ || !snapshot_.isEmpty()) return;
        const bool hide = settings_.value("hideFullscreen").toBool() && active.pid != ownPid && active.fullscreen && !recording_;
        if (hide != hiddenFullscreen_ || (hide && gtk_widget_get_visible(mainGtkWindow_))) {
            hiddenFullscreen_ = hide;
            visible(main_, !hide);
        }
        int x = 0, y = 0, width = 0, height = 0;
        gtk_window_get_position(GTK_WINDOW(mainGtkWindow_), &x, &y);
        gtk_window_get_size(GTK_WINDOW(mainGtkWindow_), &width, &height);
        const auto cursor = QCursor::pos();
        const auto *screen = QGuiApplication::screens().value(settings_.value("monitor").toInt(), QGuiApplication::primaryScreen());
        if (!screen) return;
        const bool over = cursor.x() >= x && cursor.x() < x + width &&
            cursor.y() >= screen->geometry().top() && cursor.y() < y + height;
        updateAutohide(hide, over);
    }
#endif

    void refreshSteamDownload(const QStringList &testRoots = {})
    {
        QStringList roots;
#ifdef Q_OS_WIN
        const QSettings user(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"), QSettings::NativeFormat);
        const QSettings machine(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Valve\\Steam"), QSettings::Registry32Format);
        roots = {user.value("SteamPath").toString(), machine.value("InstallPath").toString()};
#else
        const QDir home(QDir::homePath());
        roots = {home.filePath(".steam/steam"), home.filePath(".local/share/Steam"),
                 home.filePath(".var/app/com.valvesoftware.Steam/data/Steam")};
#endif
        QString progress;
        if (!testRoots.isEmpty()) roots = testRoots;
        for (const auto &root : roots) {
            if (root.isEmpty()) continue;
            const QDir directory(root);
            QFile log(directory.filePath("logs/content_log.txt"));
            const auto now = QDateTime::currentDateTime();
            const auto modified = QFileInfo(log).lastModified();
            if (!modified.isValid()) continue;
            const auto age = modified.secsTo(now);
            if (age < 0 || !log.open(QIODevice::ReadOnly)) continue;
            if (!log.seek(qMax(qint64{0}, log.size() - 65536))) continue;
            static const QRegularExpression event(R"(^\[([^\]]+)\] AppID (\d+) (.+)$)");
            static const QRegularExpression sample(R"(^update (?:started|progress)\s*:\s*download (\d+)/(\d+))");
            static const QRegularExpression ended(R"(^(?:finished update|update canceled|update finished|App update changed\s*:\s*None))");
            QString id;
            qint64 done = 0, total = 0;
            bool recentProgress = false;
            for (const auto &line : QString::fromUtf8(log.read(65536)).split('\n')) {
                const auto item = event.match(line.trimmed());
                if (!item.hasMatch()) continue;
                const auto detail = item.captured(3);
                const auto update = sample.match(detail);
                if (update.hasMatch()) {
                    const auto time = QDateTime::fromString(item.captured(1), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                    if (!time.isValid()) continue;
                    id = item.captured(2);
                    done = update.captured(1).toLongLong();
                    total = update.captured(2).toLongLong();
                    recentProgress = time.secsTo(now) >= 0 && time.secsTo(now) <= 120;
                } else if (id == item.captured(2) && ended.match(detail).hasMatch()) {
                    id.clear();
                }
            }
            if (id.isEmpty() || total <= 0 || done >= total) continue;
            QStringList libraries{root};
            QFile folders(directory.filePath("steamapps/libraryfolders.vdf"));
            if (folders.open(QIODevice::ReadOnly)) {
                static const QRegularExpression library(R"vdf("path"\s+"([^"]+)")vdf");
                auto paths = library.globalMatch(QString::fromUtf8(folders.read(1024 * 1024)));
                while (paths.hasNext()) libraries.append(paths.next().captured(1).replace("\\\\", "\\"));
            }
            libraries.removeDuplicates();
            static const QRegularExpression expected(R"acf("BytesToDownload"\s+"(\d+)")acf");
            static const QRegularExpression received(R"acf("BytesDownloaded"\s+"(\d+)")acf");
            for (const auto &library : libraries) {
                QFile manifest(QDir(library).filePath("steamapps/appmanifest_" + id + ".acf"));
                if (!manifest.open(QIODevice::ReadOnly)) continue;
                const auto data = QString::fromUtf8(manifest.read(1024 * 1024));
                if (expected.match(data).captured(1).toLongLong() != total) continue;
                const auto bytes = received.match(data).captured(1).toLongLong();
                if (bytes <= total) {
                    const auto manifestTime = QFileInfo(manifest).lastModified();
                    if (manifestTime.isValid() && bytes > 0 && manifestTime.secsTo(now) >= 0 &&
                        manifestTime.secsTo(now) <= 120) recentProgress = true;
                    done = qMax(done, bytes);
                }
            }
            if (recentProgress && done < total) {
                progress = QStringLiteral("↓ Steam %1%").arg(qMin(99, int(100.0 * done / total)));
                break;
            }
        }
        if (progress == steamDownload_) return;
        steamDownload_ = progress;
        applyBounds();
        if (mainReady_) emitTo(main_, "steam-download", steamDownload_);
    }

    bool testSteamDownload()
    {
        QTemporaryDir fixture;
        const QDir root(fixture.path());
        if (!fixture.isValid() || !root.mkpath("logs") || !root.mkpath("steamapps")) return false;
        const auto timestamp = QDateTime::currentDateTime().toString("[yyyy-MM-dd HH:mm:ss] ").toUtf8();
        QFile log(root.filePath("logs/content_log.txt"));
        QFile manifest(root.filePath("steamapps/appmanifest_123.acf"));
        if (!log.open(QIODevice::WriteOnly) || !manifest.open(QIODevice::WriteOnly)) return false;
        const auto earlier = QDateTime::currentDateTime().addSecs(-180).toString("[yyyy-MM-dd HH:mm:ss] ").toUtf8();
        log.write(earlier + "AppID 123 update started : download 0/100\n");
        manifest.write("\"BytesToDownload\" \"100\"\n\"BytesDownloaded\" \"37\"\n");
        log.close();
        manifest.close();
        QTemporaryDir stale;
        const QDir staleRoot(stale.path());
        if (!stale.isValid() || !staleRoot.mkpath("logs")) return false;
        QFile staleLog(staleRoot.filePath("logs/content_log.txt"));
        if (!staleLog.open(QIODevice::WriteOnly)) return false;
        staleLog.write(earlier + "AppID 456 update progress : download 20/100\n");
        staleLog.close();
        const QStringList roots{stale.path(), fixture.path()};
        refreshSteamDownload(roots);
        if (steamDownload_ != QStringLiteral("↓ Steam 37%") || !log.open(QIODevice::Append)) return false;
        log.write(timestamp + "AppID 123 finished update, 1 mounted depots\n");
        log.close();
        refreshSteamDownload(roots);
        return steamDownload_.isEmpty();
    }

#ifdef ORBIT_KWIN_LAYOUT_DBUS
    Q_SLOT void refreshKdeLayout()
    {
        const auto revision = ++kdeLayoutRevision_;
        const auto bus = QDBusConnection::sessionBus();
        auto query = [](const QString &method) {
            return QDBusMessage::createMethodCall("org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", method);
        };
        auto *active = new QDBusPendingCallWatcher(bus.asyncCall(query("getLayout"), 1000), this);
        connect(active, &QDBusPendingCallWatcher::finished, this, [this, active, bus, query, revision] {
            const auto reply = active->reply();
            active->deleteLater();
            if (revision != kdeLayoutRevision_) return;
            if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
                kdeLayoutLabel_.clear();
                updateKeyboard();
                return;
            }
            const auto index = reply.arguments().first().toUInt();
            auto *layouts = new QDBusPendingCallWatcher(bus.asyncCall(query("getLayoutsList"), 1000), this);
            connect(layouts, &QDBusPendingCallWatcher::finished, this, [this, layouts, index, revision] {
                const auto reply = layouts->reply();
                layouts->deleteLater();
                if (revision != kdeLayoutRevision_) return;
                if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return;
                const auto value = reply.arguments().first();
                if (value.metaType() != QMetaType::fromType<QDBusArgument>()) return;
                const auto list = value.value<QDBusArgument>();
                if (list.currentSignature() != "a(sss)") return;
                list.beginArray();
                QString label;
                for (uint i = 0; !list.atEnd(); ++i) {
                    QString name, display, description;
                    list.beginStructure();
                    list >> name >> display >> description;
                    list.endStructure();
                    if (i != index) continue;
                    label = display.isEmpty() ? name.toUpper() : display;
                }
                list.endArray();
                kdeLayoutLabel_ = label == "US" || label == "GB" ? QStringLiteral("EN") : label;
                updateKeyboard();
            });
        });
    }
#endif

    void updateKeyboard()
    {
        int layout = 0;
        bool caps = false;
        bool inactive = false;
        QString label;
        int battery = -1;
        bool plugged = false;
#ifdef Q_OS_WIN
        const auto foreground = GetForegroundWindow();
        DWORD owner = 0;
        const DWORD thread = GetWindowThreadProcessId(foreground, &owner);
        if (owner && owner != GetCurrentProcessId()) lastExternalWindow_ = foreground;
        updateWindowsVisibility(foreground, owner);
        SYSTEM_POWER_STATUS power{};
        if (GetSystemPowerStatus(&power) && power.BatteryLifePercent <= 100) {
            battery = power.BatteryLifePercent;
            plugged = power.ACLineStatus == 1;
        }
        layout = LOWORD(reinterpret_cast<quintptr>(GetKeyboardLayout(thread)));
        wchar_t iso[16]{};
        if (GetLocaleInfoW(MAKELCID(layout, SORT_DEFAULT), LOCALE_SISO639LANGNAME, iso, 16))
            label = QString::fromWCharArray(iso).toUpper();
        caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
        LASTINPUTINFO input{sizeof(LASTINPUTINFO)};
        if (GetLastInputInfo(&input)) inactive = DWORD(GetTickCount() - input.dwTime) >= 90000;
#elif defined(Q_OS_LINUX)
        auto *display = gdk_display_get_default();
        auto *keymap = display ? gdk_keymap_get_for_display(display) : nullptr;
        if (!keymap) return;
        caps = gdk_keymap_get_caps_lock_state(keymap);
        if (x11Input_) {
            std::tie(layout, label) = x11Input_->layout();
            updateX11Visibility();
        } else if (wayland_) {
            label = kdeLayoutLabel_;
        }
#endif
        if (keyboardKnown_ && caps == caps_ && layout == layout_ && label == layoutLabel_ && inactive == inactive_ &&
            battery == battery_ && plugged == plugged_) return;
        const bool notifyCaps = keyboardKnown_ && caps != caps_;
        const bool notifyLayout = keyboardKnown_ && label != layoutLabel_ && !label.isEmpty();
        keyboardKnown_ = true;
        caps_ = caps;
        layout_ = layout;
        layoutLabel_ = label;
        inactive_ = inactive;
        battery_ = battery;
        plugged_ = plugged;
        if (notifyCaps || notifyLayout) {
            indicatorUntil_ = QDateTime::currentMSecsSinceEpoch() + settings_.value("eventDuration").toInt(1650) + 50;
            if (autoHidden_) { autoHidden_ = false; applyBounds(); }
        }
        updateMedia(currentTrack_);
        if (notifyCaps) emitTo(main_, "caps", caps);
        if (notifyLayout) emitTo(main_, "layout", label);
    }

    void updateMedia(const Track &track)
    {
        if (coverUrl_ != track.cover) {
            coverUrl_ = track.cover;
            fetchedCover_.clear();
            const auto revision = ++coverRequest_;
            if (coverUrl_.isLocalFile()) {
                QFile input(coverUrl_.toLocalFile());
                if (input.open(QIODevice::ReadOnly) && input.size() <= 8 * 1024 * 1024)
                    fetchedCover_ = coverData(&input);
            } else if (coverUrl_.scheme() == "https") {
                QNetworkRequest request(coverUrl_);
                request.setTransferTimeout(5000);
                auto *reply = network_.get(request);
                connect(reply, &QIODevice::readyRead, this, [reply] {
                    if (reply->bytesAvailable() > 8 * 1024 * 1024) reply->abort();
                });
                connect(reply, &QNetworkReply::finished, this, [this, reply, revision] {
                    if (revision == coverRequest_ && reply->error() == QNetworkReply::NoError) {
                        fetchedCover_ = coverData(reply);
                        updateMedia(currentTrack_);
                    }
                    reply->deleteLater();
                });
            }
        }
        currentTrack_ = track;
        const auto cover = track.coverData.isEmpty() ? fetchedCover_ : track.coverData;
        const bool coverChanged = cover != sentCover_;
        if (coverChanged) {
            sentCover_ = cover;
            ++coverRevision_;
        }
        QJsonValue media = QJsonValue::Null;
        if (!track.title.isEmpty()) {
            media = QJsonObject{
                {"title", track.title}, {"artist", track.artist}, {"album", track.album},
                {"playing", track.playing}, {"source", track.source}, {"cover", cover}, {"coverRevision", coverRevision_},
                {"position", track.position}, {"duration", track.duration}, {"updatedAt", track.updatedAt}
            };
        }
        const QJsonObject snapshot{{"media", media}, {"sources", QJsonArray::fromStringList(track.sources)},
                                   {"caps", caps_}, {"layout", layout_}, {"layoutLabel", layoutLabel_},
                                   {"inactive", inactive_}, {"battery", battery_},
                                   {"plugged", plugged_}, {"restricted", restricted_}};
        if (lastSystem_ == snapshot) return;
        lastSystem_ = snapshot;
        if (musicPlaying_ != track.playing) {
            musicPlaying_ = track.playing;
            if (!musicPlaying_) mini_ = false;
            if (!expanded_) applyBounds();
        }
        auto event = snapshot;
        if (!media.isNull() && !coverChanged && !cover.isEmpty()) {
            auto mediaWithoutCover = media.toObject();
            mediaWithoutCover.remove("cover");
            event["media"] = mediaWithoutCover;
        }
        emitTo(main_, "system", event);
    }

    bool open(webview_t window, const QString &page, bool island)
    {
        const auto file = QDir(directory_).filePath(page);
        if (!QFileInfo::exists(file)) return false;
        const auto address = QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath()).toEncoded();
        const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto js = read(":/shell/bridge.js");
        js.replace("__ORBIT_URL__", json(QString::fromUtf8(address)));
        js.replace("__ORBIT_TOKEN__", json(token));
        js.replace("__ORBIT_CONTRACT__", read(":/shell/api.json"));
        auto *channel = new Channel{this, window, token};
        channels_.emplace_back(channel);
        webview_bind(window, "orbitNativeInvoke", [](const char *id, const char *request, void *context) {
            auto *channel = static_cast<Channel *>(context);
            channel->owner->receive(*channel, id, request);
        }, channel);
        webview_init(window, js.constData());
#if defined(Q_OS_WIN) && defined(__ICoreWebView2File_INTERFACE_DEFINED__)
        if (island) setupFileDrop(window, token, QUrl::fromEncoded(address));
#endif
        if (testing_ && (island || page == "capture.html")) {
            webview_bind(window, "orbitShellTest", [](const char *, const char *args, void *data) {
                auto *self = static_cast<OrbitShellController *>(data);
                const auto result = QJsonDocument::fromJson(args).array();
                self->passed_ = result.at(0).toBool();
                if (!self->passed_ && result.size() > 1) qWarning().noquote() << json(result.at(1));
                self->quit();
            }, this);
        }
        if (testing_ && island) {
            settings_["collapseOnLeave"] = false;
            webview_bind(window, "orbitShellTrace", [](const char *id, const char *args, void *context) {
                std::fprintf(stderr, "Orbit smoke: %s\n", args);
                std::fflush(stderr);
                webview_return(static_cast<webview_t>(context), id, 0, "null");
            }, window);
            if (qEnvironmentVariableIsSet("ORBIT_TEST_RECORD_OUTPUT")) {
                webview_init(window, R"JS(
                    document.addEventListener('DOMContentLoaded', () => {
                        const button = document.querySelector('#record-button');
                        const status = document.querySelector('#record-status');
                        const message = document.querySelector('#message');
                        document.querySelector('#record-audio').checked = true;
                        let stopping = false;
                        const poll = setInterval(() => {
                            if (message.textContent.startsWith('Не удалось начать запись') ||
                                status.textContent === 'Источник экрана не передал данные для записи.') {
                                clearInterval(poll);
                                orbitShellTest(false, {error: message.textContent || status.textContent});
                            } else if (!stopping && status.textContent.startsWith('Идёт запись')) {
                                stopping = true;
                                // Sway's damage-driven capture only emits frames when pixels change.
                                const marker = document.createElement('div');
                                marker.style.cssText = 'position:fixed;top:80px;left:30px;width:30px;height:30px;z-index:9999;pointer-events:none;background:red';
                                document.body.append(marker);
                                const animation = setInterval(() => {
                                    marker.style.backgroundColor = marker.style.backgroundColor === 'red' ? 'blue' : 'red';
                                }, 200);
                                setTimeout(() => { clearInterval(animation); marker.remove(); button.click(); }, 3200);
                            } else if (stopping && status.textContent === 'Запись сохранена') {
                                clearInterval(poll);
                                orbitShellTest(true);
                            } else if (stopping && (message.textContent ||
                                /^(Сохранение отменено|Не удалось сохранить запись|Ошибка записи экрана)/.test(status.textContent))) {
                                clearInterval(poll);
                                orbitShellTest(false, {error: message.textContent || status.textContent});
                            }
                        }, 100);
                        const focus = setInterval(() => {
                            if (!state) return;
                            if (document.querySelector('#panel').inert) { expand(true); return; }
                            if (tab !== 'tools') showTab('tools');
                            button.focus();
                            if (document.activeElement !== button) return;
                            clearInterval(focus);
                            orbitShellTrace('record button ready');
                        }, 100);
                    });
                )JS");
            } else {
                const auto testProxy = qEnvironmentVariable("ORBIT_TEST_PROXY");
                if (validProxyFile(testProxy)) {
                    settings_["proxyPath"] = testProxy;
                    webview_init(window, "window.orbitStrictProxyTest = true;");
                }
                if (qEnvironmentVariableIsSet("ORBIT_TEST_MEDIA")) webview_init(window, "window.orbitStrictMediaTest = true;");
                if (qEnvironmentVariableIsSet("ORBIT_TEST_AUDIO")) webview_init(window, "window.orbitStrictAudioTest = true;");
                if (qEnvironmentVariableIsSet("ORBIT_TEST_SOFTWARE_RENDERING"))
                    webview_init(window, "window.orbitTestSoftwareRendering = true;");
                if (qEnvironmentVariableIsSet("ORBIT_TEST_EXPECT_WAYLAND"))
                    webview_init(window, "window.orbitExpectWayland = true;");
#ifdef Q_OS_LINUX
                if (qEnvironmentVariableIsSet("ORBIT_TEST_KEYBOARD_LAYOUT")) {
                    webview_init(window, "window.orbitExpectKeyboardLayout = true;");
                }
                if ((x11_ || wayland_) && qEnvironmentVariableIsSet("ORBIT_TEST_KEYBOARD_LAYOUT")) {
                    webview_bind(window, "orbitTestSetGermanLayout", [](const char *id, const char *, void *context) {
                        const bool started = QProcess::startDetached(
                            qEnvironmentVariableIsSet("ORBIT_TEST_EXPECT_WAYLAND") ? "gdbus" : "setxkbmap",
                            qEnvironmentVariableIsSet("ORBIT_TEST_EXPECT_WAYLAND")
                                ? QStringList{"call", "--session", "--dest", "org.kde.keyboard", "--object-path",
                                              "/Layouts", "--method", "org.kde.KeyboardLayouts.switchToGerman"}
                                : QStringList{"de"});
                        webview_return(static_cast<webview_t>(context), id, 0, started ? "true" : "false");
                    }, window);
                }
#endif
                webview_init(window, R"JS(
                document.addEventListener('DOMContentLoaded', async () => {
                  const initial = await orbit.get();
                  if (window.orbitExpectKeyboardLayout) {
                    let changed;
                    orbit.onLayout(label => { changed = label; });
                    const started = await window.orbitTestSetGermanLayout?.();
                    const displayed = () => document.querySelector('#system-status').textContent.includes('DE');
                    for (let i = 0; started && i < 40 && (changed !== 'DE' || !displayed()); ++i)
                      await new Promise(resolve => setTimeout(resolve, 75));
                    if (changed !== 'DE' || !displayed()) {
                      window.orbitShellTest(false, {stage:'keyboard layout', changed});
                      return;
                    }
                  }
                  if (window.orbitTestSoftwareRendering && initial?.gpuAccelerationActive !== false) {
                    window.orbitShellTest(false, {stage:'software rendering'});
                    return;
                  }
                  if (window.orbitExpectWayland && initial?.waylandRecording !== true) {
                    window.orbitShellTest(false, {stage:'Wayland backend', waylandRecording:initial?.waylandRecording});
                    return;
                  }
                  await orbit.expand(true);
                  const notes = [{id:'qa',name:'Orbit QA',folder:'Local',text:'Save and load'}];
                  await orbit.notesSave(notes);
                  const added = await orbit.reminder('add', {text:'Orbit QA',at:Date.now()+60000,repeat:'once'});
                  const stored = await orbit.get();
                  let alarmDelivered = false;
                  orbit.onAlarm(alarm => { if (alarm.body === 'Orbit alarm fixture') alarmDelivered = true; });
                  await orbit.reminder('add', {text:'Orbit alarm fixture',at:Date.now()+220,repeat:'once'});
                  for (let i=0; i<25 && !alarmDelivered; i++) await new Promise(resolve=>setTimeout(resolve,50));
                  if (alarmDelivered) document.querySelector('#alarm-dismiss')?.click();
                  const eq = await orbit.eqProfile([0,0,2,0,0,0,0,0,0]);
                  const eqSaved = (await orbit.eqProfile()).gains?.[2] === 2 && eq.gains?.[2] === 2;
                  showTab('weather');
                  let sound = await orbit.audio('status');
                  let soundShape = Boolean(sound.endpoints || sound.error);
                  if (window.orbitStrictAudioTest) {
                    for (let i=0; i<20 && !sound.endpoints; i++) {
                      await new Promise(resolve=>setTimeout(resolve,100));
                      sound = await orbit.audio('status');
                    }
                    const second = 'orbit_preview_test_second';
                    if (!sound.endpoints?.orbit_preview_test || !sound.endpoints?.[second]) {
                      window.orbitShellTest(false, {stage:'audio endpoints', sound});
                      return;
                    }
                    const switched = await orbit.audio('select', second);
                    const level = await orbit.audio('level', 40);
                    const mute = await orbit.audio('mute');
                    const restored = await orbit.audio('mute');
                    await orbit.audio('select', 'orbit_preview_test');
                    soundShape = switched.id === second && level.level === 40 &&
                      typeof mute.muted === 'boolean' && restored.muted === !mute.muted;
                  }

                  let mediaTest = true;
                  if (window.orbitStrictMediaTest) {
                    for (let i=0; i<40 && system.media?.title !== 'Orbit test track'; i++)
                      await new Promise(resolve=>setTimeout(resolve,100));
                    const track = system.media;
                    mediaTest = track?.title === 'Orbit test track' && track.artist === 'Orbit artist' &&
                      track.album === 'Orbit album' && track.position === 27 && track.duration === 180 &&
                      track.cover?.startsWith('data:image/png;') && system.sources.includes(track.source);
                    if (mediaTest) {
                      await orbit.native('media', {command:'play'});
                      for (let i=0; i<40 && system.media?.playing; i++)
                        await new Promise(resolve=>setTimeout(resolve,100));
                      mediaTest = system.media?.playing === false;
                    }
                  }
                  let proxyTest = true;
                  if (window.orbitStrictProxyTest) {
                    const before = await orbit.networkToolsStatus();
                    const started = await orbit.networkToolsSet('tg', true);
                    const stopped = await orbit.networkToolsSet('tg', false);
                    proxyTest = before.proxyConfigured && !before.proxyRunning && started.proxyRunning && !stopped.proxyRunning;
                  }
                  const controls = await orbit.controls('status');
                  await orbitShellTrace('resolve dropped paths');
                  const paths = await orbit.resolveDropPaths({files:[], getData:() => new URL('icon.png', location.href).href});
                  await orbitShellTrace('add to shelf');
                  const shelf = await orbit.shelfAdd(paths);
                  await orbitShellTrace('add launcher');
                  const launcher = await orbit.dropLauncher(paths);
                  await orbitShellTrace('load file icon');
                  const dropTest = paths.length === 1 && shelf[0]?.path === paths[0] && launcher.count === 1 &&
                    (await orbit.fileIcon(paths[0]))?.startsWith?.('data:image/png;');
                  const desktopShape = typeof controls.networkUp === 'boolean' && typeof controls.dark === 'boolean';
                  const valid = Boolean(alarmDelivered && eqSaved && soundShape && desktopShape && proxyTest && mediaTest && dropTest && initial.settings && stored.state.notes[0]?.text === notes[0].text &&
                    added[0]?.text === 'Orbit QA' && document.querySelector('#island.expanded') &&
                    document.querySelector('[data-page=weather]') &&
                    getComputedStyle(document.querySelector('#island')).borderTopLeftRadius !== '0px');
                  if (!valid) window.orbitShellTest(false, {
                    alarmDelivered, eqSaved, soundShape, desktopShape, proxyTest, mediaTest, dropTest,
                    media: system.media && {...system.media, cover: Boolean(system.media.cover)},
                    sources: system.sources,
                    loadedSettings: Boolean(initial.settings),
                    savedNotes: stored.state.notes[0]?.text === notes[0].text,
                    savedReminder: added[0]?.text === 'Orbit QA',
                    expanded: Boolean(document.querySelector('#island.expanded')),
                    weather: Boolean(document.querySelector('[data-page=weather]')),
                    islandRadius: getComputedStyle(document.querySelector('#island')).borderTopLeftRadius
                  });
                  else {
                    const recorder = await orbit.recordStream.begin('video/webm');
                    const fragment = recorder === true && await orbit.recordStream.chunk(new Blob([new Uint8Array([0x1a,0x45,0xdf,0xa3])]));
                    const saved = fragment === true && await orbit.recordStream.finish();
                    if (!saved?.saved) { window.orbitShellTest(false, {stage:'record stream', fragment, saved}); return; }
                    await orbitShellTrace('open capture editor');
                    const capture = await orbit.capture('edit');
                    if (capture?.error) window.orbitShellTest(false, {stage:'capture', capture});
                  }
                });
            )JS");
            }
        } else if (testing_ && page == "capture.html") {
            if (qEnvironmentVariableIsSet("ORBIT_TEST_WAYLAND_DESKTOP")) {
                webview_init(window, "window.orbitTestWaylandDesktop = true;");
            }
            webview_init(window, R"JS(
                document.addEventListener('DOMContentLoaded', () => {
                  orbit.onCapture(async () => {
                    const canvas = document.querySelector('canvas');
                    const width = window.orbitTestWaylandDesktop ? 1280 : 96;
                    const height = window.orbitTestWaylandDesktop ? 960 : 64;
                    for (let i=0; i<60 && canvas.width!==width; i++) await new Promise(resolve=>setTimeout(resolve,50));
                    const decoded = canvas.width === width && canvas.height === height;
                    const color = await orbit.color('#7856aa');
                    const copied = await orbit.captureResult({action:'copy',data:canvas.toDataURL('image/png')});
                    window.orbitShellTest(decoded && color === true && copied?.ok === true && typeof window.jsQR === 'function');
                  });
                });
            )JS");
        }
        webview_set_title(window, island ? "Orbit Native" : page == "capture.html" ? "Orbit Native — Capture" : "Orbit Native — Settings");
        if (island) styleWindow();
        else {
            webview_set_size(window, 1020, 790, WEBVIEW_HINT_NONE);
#ifdef Q_OS_WIN
            const auto hwnd = static_cast<HWND>(webview_get_window(window));
            SetWindowSubclass(hwnd, hideSecondary, 1, reinterpret_cast<DWORD_PTR>(this));
            if (page == "settings.html") {
                const UINT dpi = GetDpiForWindow(hwnd);
                SetWindowPos(hwnd, nullptr, 0, 0, MulDiv(1020, dpi, 96), MulDiv(790, dpi, 96),
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
#elif defined(Q_OS_LINUX)
            g_signal_connect(webview_get_window(window), "delete-event", G_CALLBACK(+[](GtkWidget *widget, GdkEvent *, gpointer data) {
                auto *self = static_cast<OrbitShellController *>(data);
                if (self->editor_ && widget == webview_get_window(self->editor_)) self->closeCapture();
                else gtk_widget_hide(widget);
                return TRUE;
            }), this);
#endif
        }
#ifdef Q_OS_LINUX
        auto *browser = WEBKIT_WEB_VIEW(webview_get_native_handle(window, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
        if (browser && island) {
            webkit_settings_set_enable_media_stream(webkit_web_view_get_settings(browser), TRUE);
            if (testing_ && qEnvironmentVariableIsSet("ORBIT_TEST_RECORD_OUTPUT"))
                g_object_set_data(G_OBJECT(browser), "orbit-record-test", GINT_TO_POINTER(1));
            auto *trustedUrl = g_strdup(address.constData());
            g_signal_connect_data(browser, "permission-request", G_CALLBACK(+[](WebKitWebView *view,
                WebKitPermissionRequest *request, gpointer data) -> gboolean {
                if (!WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request) ||
                    !webkit_user_media_permission_is_for_display_device(WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request)) ||
                    g_strcmp0(webkit_web_view_get_uri(view), static_cast<const gchar *>(data)) != 0) return FALSE;
                if (g_object_get_data(G_OBJECT(view), "orbit-record-test")) {
                    webkit_permission_request_allow(request);
                    return TRUE;
                }
                auto *parent = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(view)));
                auto *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                    GTK_BUTTONS_NONE, "%s", "Разрешить Orbit записывать экран?");
                gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Отмена", GTK_RESPONSE_CANCEL,
                    "Продолжить", GTK_RESPONSE_ACCEPT, nullptr);
                g_signal_connect_data(dialog, "response", G_CALLBACK(+[](GtkDialog *dialog, gint response, gpointer context) {
                    auto *permission = WEBKIT_PERMISSION_REQUEST(context);
                    if (response == GTK_RESPONSE_ACCEPT) webkit_permission_request_allow(permission);
                    else webkit_permission_request_deny(permission);
                    gtk_widget_destroy(GTK_WIDGET(dialog));
                }), g_object_ref(request), +[](gpointer request, GClosure *) { g_object_unref(request); }, G_CONNECT_DEFAULT);
                gtk_widget_show(dialog);
                return TRUE;
            }), trustedUrl, +[](gpointer url, GClosure *) { g_free(url); }, G_CONNECT_DEFAULT);
            g_signal_connect(browser, "drag-data-received", G_CALLBACK(+[](GtkWidget *, GdkDragContext *, gint, gint,
                GtkSelectionData *selection, guint, guint, gpointer context) {
                auto *self = static_cast<OrbitShellController *>(context);
                auto **uris = gtk_selection_data_get_uris(selection);
                if (!uris) return;
                self->recentDrop_ = QJsonArray();
                for (auto index = 0; uris[index] && index < 100; ++index)
                    self->recentDrop_.append(QString::fromUtf8(uris[index]));
                self->recentDropTime_ = QDateTime::currentMSecsSinceEpoch();
                g_strfreev(uris);
            }), this);
        }
        if (browser && !gpuEnabled_) webkit_settings_set_hardware_acceleration_policy(
            webkit_web_view_get_settings(browser), WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
#endif
        webview_navigate(window, address.constData());
        return true;
    }

#ifdef Q_OS_WIN
    static LRESULT CALLBACK hideSecondary(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context)
    {
        auto *self = reinterpret_cast<OrbitShellController *>(context);
        if (message == WM_CLOSE) {
            if (self->editor_ && window == webview_get_window(self->editor_)) self->closeCapture();
            else ShowWindow(window, SW_HIDE);
            return 0;
        }
        if (message == WM_GETMINMAXINFO && self->preferences_ && window == webview_get_window(self->preferences_)) {
            const auto result = DefSubclassProc(window, message, wParam, lParam);
            auto *limits = reinterpret_cast<MINMAXINFO *>(lParam);
            const UINT dpi = GetDpiForWindow(window);
            limits->ptMinTrackSize.x = (std::max)(limits->ptMinTrackSize.x, LONG(MulDiv(640, dpi, 96)));
            limits->ptMinTrackSize.y = (std::max)(limits->ptMinTrackSize.y, LONG(MulDiv(460, dpi, 96)));
            return result;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
#endif

#if defined(Q_OS_WIN) && defined(__ICoreWebView2File_INTERFACE_DEFINED__)
    void setupFileDrop(webview_t window, const QString &token, const QUrl &page)
    {
        using Microsoft::WRL::ComPtr;
        using Microsoft::WRL::Callback;
        auto *controller = static_cast<ICoreWebView2Controller *>(
            webview_get_native_handle(window, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
        ComPtr<ICoreWebView2> browser;
        if (!controller || FAILED(controller->get_CoreWebView2(&browser))) return;
        EventRegistrationToken registration{};
        browser->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this, window, marker = token + ":drop", page](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *event) {
                LPWSTR rawMessage = nullptr;
                if (FAILED(event->TryGetWebMessageAsString(&rawMessage))) return S_OK;
                const auto message = QString::fromWCharArray(rawMessage);
                CoTaskMemFree(rawMessage);
                if (message != marker) return S_OK;
                LPWSTR rawSource = nullptr;
                if (FAILED(event->get_Source(&rawSource))) return S_OK;
                const auto source = QUrl(QString::fromWCharArray(rawSource));
                CoTaskMemFree(rawSource);
                if (source != page) return S_OK;
                ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> extended;
                ComPtr<ICoreWebView2ObjectCollectionView> files;
                UINT32 count = 0;
                if (FAILED(event->QueryInterface(IID_PPV_ARGS(&extended))) ||
                    FAILED(extended->get_AdditionalObjects(&files)) || !files ||
                    FAILED(files->get_Count(&count))) return S_OK;
                QJsonArray paths;
                for (UINT32 index = 0; index < count && index < 100; ++index) {
                    ComPtr<IUnknown> item;
                    ComPtr<ICoreWebView2File> file;
                    LPWSTR path = nullptr;
                    if (FAILED(files->GetValueAtIndex(index, &item)) || !item ||
                        FAILED(item.As(&file)) || FAILED(file->get_Path(&path))) continue;
                    paths.append(QString::fromWCharArray(path));
                    CoTaskMemFree(path);
                }
                emitTo(window, "native-drop-paths", paths);
                return S_OK;
            }).Get(), &registration);
    }
#endif

    void clearHotkeys()
    {
#ifdef Q_OS_WIN
        const auto window = static_cast<HWND>(webview_get_window(main_));
        for (const auto &[id, callback] : hotkeys_) UnregisterHotKey(window, id);
        hotkeys_.clear();
#elif defined(Q_OS_LINUX)
        if (x11Input_) x11Input_->clear();
#endif
    }

#ifdef Q_OS_LINUX
    static QString portalTrigger(QKeyCombination combination)
    {
        QStringList parts;
        const auto modifiers = combination.keyboardModifiers();
        if (modifiers.testFlag(Qt::ControlModifier)) parts.append("CTRL");
        if (modifiers.testFlag(Qt::AltModifier)) parts.append("ALT");
        if (modifiers.testFlag(Qt::ShiftModifier)) parts.append("SHIFT");
        if (modifiers.testFlag(Qt::MetaModifier)) parts.append("LOGO");
        const int key = combination.key();
        if (key >= Qt::Key_A && key <= Qt::Key_Z) parts.append(QChar('a' + key - Qt::Key_A));
        else if (key >= Qt::Key_0 && key <= Qt::Key_9) parts.append(QChar(key));
        else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) parts.append(QString("F%1").arg(key - Qt::Key_F1 + 1));
        else if (key == Qt::Key_Space) parts.append("space");
        else return {};
        return parts.join('+');
    }
#endif

    void registerHotkeys()
    {
        clearHotkeys();
        unavailableHotkeys_ = QJsonArray();
#ifdef Q_OS_WIN
        const auto window = static_cast<HWND>(webview_get_window(main_));
        int next = 1;
#elif defined(Q_OS_LINUX)
        QList<PortalShortcut> portalActions;
#endif
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
        const auto add = [&](const QString &id, const QString &description, const QString &shortcut, std::function<void()> action) {
            if (shortcut.isEmpty()) return;
            auto portable = shortcut;
            portable.replace(QRegularExpression("(^|[+])Control[+]", QRegularExpression::CaseInsensitiveOption), "\\1Ctrl+");
            const auto sequence = QKeySequence::fromString(portable, QKeySequence::PortableText);
            if (sequence.count() == 1) {
                const auto combination = sequence[0];
                const auto key = combination.key();
                if (combination.keyboardModifiers() == Qt::NoModifier &&
                    (key < Qt::Key_F1 || key > Qt::Key_F24)) {
                    unavailableHotkeys_.append(shortcut);
                    return;
                }
#ifdef Q_OS_WIN
                Q_UNUSED(id);
                Q_UNUSED(description);
                const auto modifiers = combination.keyboardModifiers();
                const UINT flags = MOD_NOREPEAT |
                    (modifiers.testFlag(Qt::ControlModifier) ? MOD_CONTROL : 0) |
                    (modifiers.testFlag(Qt::AltModifier) ? MOD_ALT : 0) |
                    (modifiers.testFlag(Qt::ShiftModifier) ? MOD_SHIFT : 0) |
                    (modifiers.testFlag(Qt::MetaModifier) ? MOD_WIN : 0);
                UINT vk = 0;
                if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9) ||
                    key == Qt::Key_Space) vk = UINT(key);
                else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) vk = VK_F1 + key - Qt::Key_F1;
                if (vk && RegisterHotKey(window, next, flags, vk)) {
                    hotkeys_.emplace(next++, std::move(action));
                    return;
                }
#elif defined(Q_OS_LINUX)
                if (portalShortcuts_) {
                    const auto trigger = portalTrigger(combination);
                    if (!trigger.isEmpty()) {
                        portalActions.append({id, description, shortcut, trigger, std::move(action)});
                        return;
                    }
                }
                if (x11Input_ && x11Input_->add(combination, std::move(action))) return;
#endif
            }
            unavailableHotkeys_.append(shortcut);
        };
        const auto focus = [this] {
#ifdef Q_OS_WIN
            const auto window = static_cast<HWND>(webview_get_window(main_));
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
#elif defined(Q_OS_LINUX)
            gtk_window_present(GTK_WINDOW(webview_get_window(main_)));
#endif
        };
        const auto tab = [this, focus](const QString &page) {
            resize(true);
            focus();
            emitTo(main_, "tab", page);
        };
        const auto grab = [this](const QString &mode) {
            capture(mode, [this](QJsonValue result) {
                const auto message = result.toObject().value("error").toString();
                if (!message.isEmpty()) emitTo(main_, "error", message);
            });
        };
        add("search", QStringLiteral("Открыть поиск Orbit"), settings_.value("shortcut").toString(), [tab] { tab("search"); });
        add("island", QStringLiteral("Открыть или свернуть Orbit"), "Ctrl+Alt+I", [this, focus] {
            resize(!expanded_);
            if (expanded_) focus();
        });
        add("clipboard", QStringLiteral("Открыть буфер обмена"), "Ctrl+Alt+V", [tab] { tab("clipboard"); });
        add("screenshot", QStringLiteral("Снимок экрана"), "Ctrl+Alt+S", [grab] { grab("edit"); });
        add("ocr", QStringLiteral("Распознать текст с экрана"), "Ctrl+Alt+O", [grab] { grab("ocr"); });
        add("color", QStringLiteral("Взять цвет с экрана"), "Ctrl+Alt+C", [grab] { grab("color"); });
        add("qr", QStringLiteral("Считать QR-код"), "Ctrl+Alt+Q", [grab] { grab("qr"); });
        add("record", QStringLiteral("Запись экрана"), "Ctrl+Alt+R", [this, tab] { tab("tools"); emitTo(main_, "record-toggle", true); });
#ifdef Q_OS_WIN
        add("plain-paste", QStringLiteral("Вставить без форматирования"), "Ctrl+Alt+Shift+V", [this] {
            auto *clipboard = QApplication::clipboard();
            const auto *contents = clipboard->mimeData();
            if (!contents || !contents->hasText()) return;
            const auto plainText = clipboard->text();
            clipboard->setText(plainText);
            const auto foreground = GetForegroundWindow();
            DWORD owner = 0;
            GetWindowThreadProcessId(foreground, &owner);
            const auto target = owner && owner != GetCurrentProcessId() ? foreground : lastExternalWindow_;
            pastePlainTextAfterRelease(target, GetClipboardSequenceNumber(), 40);
        });
#elif defined(Q_OS_LINUX)
        if (x11Input_) add("plain-paste", QStringLiteral("Вставить без форматирования"), "Ctrl+Alt+Shift+V", [this] {
            auto *clipboard = QApplication::clipboard();
            const auto *contents = clipboard->mimeData();
            const auto target = x11Input_->foreground().window;
            if (!contents || !contents->hasText() || !target) return;
            const auto plainText = clipboard->text();
            clipboard->setText(plainText);
            pastePlainTextOnX11(target, plainText, 40);
        });
#endif
        for (const auto &entry : settings_.value("items").toArray()) {
            const auto item = entry.toObject();
            const auto target = item.value("target").toString();
            const auto digest = QCryptographicHash::hash(target.toUtf8(), QCryptographicHash::Sha256).toHex().left(20);
            add("app-" + QString::fromLatin1(digest), item.value("name").toString(),
                item.value("hotkey").toString(), [this, target] {
                const auto message = launch(target).toObject().value("error").toString();
                if (!message.isEmpty()) emitTo(main_, "error", message);
            });
        }
#endif
#ifdef Q_OS_LINUX
        if (portalShortcuts_) {
            const auto invalid = unavailableHotkeys_;
            portalShortcuts_->configure(std::move(portalActions), [this, invalid](QStringList missing) {
                unavailableHotkeys_ = invalid;
                for (const auto &shortcut : std::as_const(missing)) unavailableHotkeys_.append(shortcut);
                emitTo(main_, "hotkeys", unavailableHotkeys_);
            });
            return;
        }
#endif
        emitTo(main_, "hotkeys", unavailableHotkeys_);
    }

#ifdef Q_OS_WIN
    static LRESULT CALLBACK islandMessages(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR, DWORD_PTR context)
    {
        auto *self = reinterpret_cast<OrbitShellController *>(context);
        if (message == WM_CLOSE) { self->quit(); return 0; }
        if (message == WM_HOTKEY) {
            const auto action = self->hotkeys_.find(int(wParam));
            if (action != self->hotkeys_.end()) {
                action->second();
                return 0;
            }
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
#endif

    void styleWindow()
    {
#ifdef Q_OS_WIN
        auto hwnd = static_cast<HWND>(webview_get_window(main_));
        SetWindowSubclass(hwnd, islandMessages, 1, reinterpret_cast<DWORD_PTR>(this));
        auto *controller = static_cast<ICoreWebView2Controller *>(
            webview_get_native_handle(main_, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
        ICoreWebView2Controller2 *transparent = nullptr;
        if (controller && SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&transparent)))) {
            transparent->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0});
            transparent->Release();
        }
        const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME)) | WS_POPUP);
        const auto extra = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (extra & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        if (!setCaptureVisibility(settings_.value("showInCapture").toBool()))
            qWarning("Orbit: SetWindowDisplayAffinity failed (%lu)", GetLastError());
#elif defined(Q_OS_LINUX)
        auto *window = GTK_WINDOW(webview_get_window(main_));
        gtk_window_set_decorated(window, FALSE);
        gtk_window_set_keep_above(window, TRUE);
        g_signal_connect(window, "delete-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer context) {
            auto *self = static_cast<OrbitShellController *>(context);
            self->quit();
            return TRUE;
        }), this);
        auto *style = gtk_widget_get_style_context(GTK_WIDGET(window));
        auto *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, "window { background-color: transparent; }", -1, nullptr);
        gtk_style_context_add_provider(style, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
        gtk_window_set_skip_taskbar_hint(window, TRUE);
        auto *browser = WEBKIT_WEB_VIEW(webview_get_native_handle(main_, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
        GdkRGBA background{0, 0, 0, 0};
        if (browser) webkit_web_view_set_background_color(browser, &background);
#endif
    }

    bool setCaptureVisibility(bool show)
    {
#ifdef Q_OS_WIN
        const auto window = static_cast<HWND>(webview_get_window(main_));
        return SetWindowDisplayAffinity(window, show ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
#else
        Q_UNUSED(show);
        return true;
#endif
    }

#ifdef Q_OS_WIN
    bool testCaptureVisibility()
    {
        const auto window = static_cast<HWND>(webview_get_window(main_));
        DWORD affinity = WDA_NONE;
        if (!setCaptureVisibility(true) || !GetWindowDisplayAffinity(window, &affinity) || affinity != WDA_NONE)
            return false;
        if (!setCaptureVisibility(false) || !GetWindowDisplayAffinity(window, &affinity))
            return false;
        // Windows 10 before version 2004 falls back to WDA_MONITOR.
        return affinity == WDA_EXCLUDEFROMCAPTURE || affinity == WDA_MONITOR;
    }
#endif

    void shape(const QJsonObject &metrics)
    {
#ifdef Q_OS_WIN
        auto hwnd = static_cast<HWND>(webview_get_window(main_));
        RECT client{};
        if (!GetClientRect(hwnd, &client)) return;
        const auto scale = GetDpiForWindow(hwnd) / 96.0 * lastScale_;
        const auto left = std::clamp(qRound(metrics.value("x").toDouble() * scale), 0, int(client.right));
        const auto top = std::clamp(qRound(metrics.value("y").toDouble() * scale), 0, int(client.bottom));
        const auto width = std::clamp(qRound(metrics.value("width").toDouble() * scale), 0, int(client.right) - left);
        const auto height = std::clamp(qRound(metrics.value("height").toDouble() * scale), 0, int(client.bottom) - top);
        if (width < 16 || height < 16) return;
        const auto diameter = std::clamp(qRound(metrics.value("bottom").toDouble() * scale * 2), 0, (std::min)(width, height));
        auto region = diameter > 0 ? CreateRoundRectRgn(left, top, left + width, top + height, diameter, diameter)
                                   : CreateRectRgn(left, top, left + width, top + height);
        if (!region) return;
        if (diameter > 0 && metrics.value("top").toDouble() == 0) {
            auto squareTop = CreateRectRgn(left, top, left + width, top + diameter / 2);
            if (squareTop) { CombineRgn(region, region, squareTop, RGN_OR); DeleteObject(squareTop); }
        }
        if (!SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
#elif defined(Q_OS_LINUX)
        if (!x11_) return;
        auto *surface = gtk_widget_get_window(mainGtkWindow_);
        if (!surface) return;
        const int left = qRound(metrics.value("x").toDouble() * lastScale_);
        const int top = qRound(metrics.value("y").toDouble() * lastScale_);
        const int width = qRound(metrics.value("width").toDouble() * lastScale_);
        const int height = qRound(metrics.value("height").toDouble() * lastScale_);
        if (width < 16 || height < 16 || width > 3500 || height > 2100) return;
        const int limit = (std::min)(width, height) / 2;
        const int upper = std::clamp(qRound(metrics.value("top").toDouble() * lastScale_), 0, limit);
        const int lower = std::clamp(qRound(metrics.value("bottom").toDouble() * lastScale_), 0, limit);
        auto *region = cairo_region_create();
        for (int y = 0; y < height; ++y) {
            int inset = 0;
            if (y < upper) {
                const double dy = upper - y - .5;
                inset = qRound(upper - std::sqrt(upper * upper - dy * dy));
            } else if (y >= height - lower) {
                const double dy = y - (height - lower) + .5;
                inset = qRound(lower - std::sqrt(lower * lower - dy * dy));
            }
            const cairo_rectangle_int_t row{left + inset, top + y, width - 2 * inset, 1};
            cairo_region_union_rectangle(region, &row);
        }
        if (gdk_screen_is_composited(gtk_widget_get_screen(mainGtkWindow_))) {
            // Keep the painted surface intact when the tab changes its height.
            gdk_window_shape_combine_region(surface, nullptr, 0, 0);
            gdk_window_input_shape_combine_region(surface, region, 0, 0);
        } else {
            gdk_window_input_shape_combine_region(surface, nullptr, 0, 0);
            gdk_window_shape_combine_region(surface, region, 0, 0);
        }
        cairo_region_destroy(region);
#else
        Q_UNUSED(metrics);
#endif
    }

    void resize(bool expanded)
    {
        const bool wasExpanded = expanded_;
        expanded_ = expanded;
        mini_ = false;
        autoHidden_ = false;
        collapse_.stop();
        if (wasExpanded && !expanded) collapse_.start(360);
        else applyBounds();
        emitTo(main_, "expanded", expanded);
    }

    void refreshMemoryTarget()
    {
#if defined(Q_OS_WIN) && defined(__ICoreWebView2_19_INTERFACE_DEFINED__)
        if (testing_) return;
        if (expanded_ || mini_ || musicPlaying_ || recording_) {
            memoryIdle_.stop();
            setMemoryTarget(false);
        } else if (!memoryIdle_.isActive() && !lowMemory_) {
            memoryIdle_.start(15000);
        }
#endif
    }

#if defined(Q_OS_WIN) && defined(__ICoreWebView2_19_INTERFACE_DEFINED__)
    void setMemoryTarget(bool low)
    {
        if (lowMemory_ == low) return;
        using Microsoft::WRL::ComPtr;
        auto *controller = static_cast<ICoreWebView2Controller *>(
            webview_get_native_handle(main_, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
        ComPtr<ICoreWebView2> browser;
        ComPtr<ICoreWebView2_19> memory;
        if (!controller || FAILED(controller->get_CoreWebView2(&browser)) || !browser || FAILED(browser.As(&memory))) return;
        if (SUCCEEDED(memory->put_MemoryUsageTargetLevel(low ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW :
                                                               COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL)))
            lowMemory_ = low;
    }
#endif

    void applyBounds()
    {
        refreshMemoryTarget();
        const auto screens = QGuiApplication::screens();
        const auto index = settings_.value("monitor").toInt();
        const auto *screen = screens.value(index, QGuiApplication::primaryScreen());
        if (!screen) return;
        const auto area = screen->availableGeometry();
        const bool edge = settings_.value("shape").toString() == "edge";
        const int top = edge ? screen->geometry().top() : area.top() + std::clamp(settings_.value("top").toInt(), 0, 200);
        const double opacity = std::clamp(settings_.value("opacity").toDouble(1), .45, 1.0);
        const auto mode = settings_.value("musicMode").toString();
        const int musicWidth = !musicPlaying_ ? 184 : mode == "cover" ? 232 : 366;
        const int compactWidth = qMax(musicWidth, steamDownload_.isEmpty() ? 184 : 250);
        const bool panel = expanded_ || collapse_.isActive();
        const auto scale = (std::min)({
            std::clamp(settings_.value("scale").toDouble(1), .8, 2.0),
            (std::max)(.4, (area.width() - 32) / 856.0),
            (std::max)(.4, (area.bottom() - top - 7) / 514.0)
        });
        const QSize size(qRound((panel ? 856 : compactWidth) * scale),
                         qRound((panel ? 514 : mini_ ? 94 : 38) * scale));
        const bool clickThrough = !panel && !mini_ && settings_.value("clickThrough").toBool();
        if (lastScale_ != scale) {
            lastScale_ = scale;
#ifdef Q_OS_WIN
            auto *controller = static_cast<ICoreWebView2Controller *>(
                webview_get_native_handle(main_, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
            if (controller) controller->put_ZoomFactor(scale);
#elif defined(Q_OS_LINUX)
            auto *browser = WEBKIT_WEB_VIEW(webview_get_native_handle(main_, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
            if (browser) webkit_web_view_set_zoom_level(browser, scale);
#endif
        }
#ifdef Q_OS_WIN
        auto hwnd = static_cast<HWND>(webview_get_window(main_));
        if (lastWindowSize_ != size) SetWindowRgn(hwnd, nullptr, TRUE);
#elif defined(Q_OS_LINUX)
        if (lastWindowSize_ != size && x11_) {
            if (auto *surface = gtk_widget_get_window(mainGtkWindow_))
                gdk_window_shape_combine_region(surface, nullptr, 0, 0);
        }
#endif
        if (lastWindowSize_ != size) {
            lastWindowSize_ = size;
#ifdef Q_OS_WIN
            // Match the original frameless window's physical-pixel rounding.
            const UINT dpi = GetDpiForWindow(hwnd);
            SetWindowPos(hwnd, nullptr, 0, 0, MulDiv(size.width(), dpi, 96), MulDiv(size.height(), dpi, 96),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
            webview_set_size(main_, size.width(), size.height(), WEBVIEW_HINT_FIXED);
#endif
        }
        const auto position = settings_.value("position").toString();
#ifdef Q_OS_WIN
        // Work-area and window rectangles must use the same physical coordinates on mixed-DPI desktops.
        const auto monitor = MonitorFromPoint(POINT{screen->geometry().left() + 1, screen->geometry().top() + 1},
                                              MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{sizeof(MONITORINFO)};
        RECT bounds{};
        if (GetMonitorInfoW(monitor, &info) && GetWindowRect(hwnd, &bounds)) {
            const double dpi = screen->devicePixelRatio();
            const int width = bounds.right - bounds.left;
            const int padding = qRound(16 * dpi);
            int x = (info.rcWork.left + info.rcWork.right - width) / 2;
            if (position == "left") x = info.rcWork.left + padding;
            if (position == "right") x = info.rcWork.right - width - padding;
            const int y = autoHidden_ && !panel ? info.rcMonitor.top - (bounds.bottom - bounds.top) + qRound(9 * dpi) :
                edge ? info.rcMonitor.top : info.rcWork.top + qRound((top - area.top()) * dpi);
            SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        const auto previous = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        auto extra = previous & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT);
        if (opacity < 1 || clickThrough) extra |= WS_EX_LAYERED;
        if (clickThrough) extra |= WS_EX_TRANSPARENT;
        if (extra != previous) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, extra);
        if (extra & WS_EX_LAYERED) SetLayeredWindowAttributes(hwnd, 0, BYTE(qRound(opacity * 255)), LWA_ALPHA);
#elif defined(Q_OS_LINUX)
        int x = area.x() + (area.width() - size.width()) / 2;
        if (position == "left") x = area.left() + 16;
        if (position == "right") x = area.right() + 1 - size.width() - 16;
        auto *window = GTK_WINDOW(webview_get_window(main_));
        if (gtk_layer_is_layer_window(window)) {
            auto *display = gtk_widget_get_display(mainGtkWindow_);
            auto *monitor = gdk_display_get_monitor_at_point(display, area.center().x(), area.center().y());
            if (monitor && gtk_layer_get_monitor(window) != monitor) gtk_layer_set_monitor(window, monitor);
            gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, position == "left");
            gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, position == "right");
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, position == "left" ? 16 : 0);
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_RIGHT, position == "right" ? 16 : 0);
            gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, edge ? 0 : top - area.top());
        } else {
            const int y = autoHidden_ && !panel ? screen->geometry().top() - size.height() + qRound(9 * scale) : top;
            gtk_window_move(window, x, y);
        }
        gtk_widget_set_opacity(GTK_WIDGET(window), opacity);
        if (auto *surface = gtk_widget_get_window(GTK_WIDGET(window))) {
            auto *region = clickThrough ? cairo_region_create() : nullptr;
            gdk_window_input_shape_combine_region(surface, region, 0, 0);
            if (region) cairo_region_destroy(region);
        }
#endif
    }

    void emitTo(webview_t window, const QString &event, const QJsonValue &value)
    {
        if (!window || shuttingDown_) return;
        const auto script = "window.orbitNativeDispatch?.(" + json(event) + "," + json(value) + ");";
        webview_eval(window, script.constData());
    }

    bool setAutostart(bool enabled)
    {
        if (testing_) return true;
#ifdef Q_OS_WIN
        QSettings startup(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                          QSettings::NativeFormat);
        if (enabled) startup.setValue("OrbitNative", '"' + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + '"');
        else startup.remove("OrbitNative");
        startup.sync();
        return startup.status() == QSettings::NoError;
#elif defined(Q_OS_LINUX)
        const QDir directory(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
        const auto filename = directory.filePath("autostart/orbit-native-shell.desktop");
        if (!enabled) return !QFile::exists(filename) || QFile::remove(filename);
        if (!directory.mkpath("autostart")) return false;
        auto executable = QCoreApplication::applicationFilePath();
        const QChar slash(92);
        executable.replace(slash, QString(4, slash)).replace('%', "%%");
        for (const auto character : {QChar(34), QChar(36), QChar(96)})
            executable.replace(character, QString(2, slash) + character);
        const QByteArray desktop = "[Desktop Entry]\nType=Application\nName=Orbit Native\nExec=\"" +
                                   executable.toUtf8() + "\"\nTerminal=false\n";
        QSaveFile file(filename);
        return file.open(QIODevice::WriteOnly) && file.write(desktop) == desktop.size() && file.commit();
#else
        return !enabled;
#endif
    }


    bool persist()
    {
        if (testing_) return true;
        QSaveFile file(store_);
        if (!file.open(QIODevice::WriteOnly)) return false;
        const auto body = backup();
        return file.write(body) == body.size() && file.commit();
    }

    QByteArray backup() const
    {
        return QJsonDocument(QJsonObject{{"settings", settings_}, {"state", state_}}).toJson();
    }

    QJsonValue exportBackup()
    {
        const auto path = QFileDialog::getSaveFileName(nullptr, QStringLiteral("Экспорт Orbit Native"),
                                                       "Orbit-backup.json", "JSON (*.json)");
        if (path.isEmpty()) return false;
        QSaveFile file(path);
        const auto data = backup();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            return error(QStringLiteral("Не удалось сохранить резервную копию"));
        }
        return true;
    }

    QJsonValue equalizer(const QJsonValue &request)
    {
        static const std::array<int, 9> frequencies{60, 120, 250, 500, 1000, 2000, 4000, 8000, 16000};
        const auto path = QDir(QFileInfo(store_).absolutePath()).filePath("Orbit-EQ.txt");
        if (request.isUndefined()) {
            const auto gains = state_.value("eq").toArray();
            return QJsonObject{{"gains", gains.isEmpty() ? QJsonArray{0,0,0,0,0,0,0,0,0} : gains}, {"file", path}};
        }
        const auto gains = request.toArray();
        if (gains.size() != frequencies.size()) return error(QStringLiteral("Нужно указать девять полос эквалайзера"));
        int peak = 0;
        QStringList curve;
        for (qsizetype i = 0; i < gains.size(); ++i) {
            const auto gain = gains.at(i).toInt(100);
            if (gain < -12 || gain > 12) return error(QStringLiteral("Усиление должно быть целым числом от −12 до 12 дБ"));
            peak = (std::max)(peak, gain);
            curve.append(QStringLiteral("%1 %2").arg(frequencies.at(i)).arg(gain));
        }
        if (!testing_) {
            const auto profile = QStringLiteral("Preamp: %1 dB\nGraphicEQ: %2\n").arg(-peak).arg(curve.join("; ")).toUtf8();
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(profile) != profile.size() || !file.commit()) {
                return error(QStringLiteral("Не удалось записать профиль эквалайзера"));
            }
        }
        const auto previous = state_.value("eq");
        state_["eq"] = gains;
        if (!persist()) { state_["eq"] = previous; return error(QStringLiteral("Не удалось сохранить настройки эквалайзера")); }
        return QJsonObject{{"gains", gains}, {"file", path}};
    }

    void scheduleTimer()
    {
        alarm_.stop();
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto timer = state_.value("timer").toObject();
        qint64 next = !timer.isEmpty() && !timer.value("paused").toBool() ? timer.value("end").toInteger() : 0;
        const auto reminders = state_.value("reminders").toArray();
        for (const auto &item : reminders) {
            const auto reminder = item.toObject();
            if (reminder.value("done").toBool()) continue;
            const auto at = reminder.value("at").toInteger();
            if (at > 0 && (next == 0 || at < next)) next = at;
        }
        if (next > 0) alarm_.start(int(std::clamp(next - now, qint64{0}, qint64{30000})));
    }

    void notifyDesktop(const QString &title, const QString &body)
    {
        if (testing_) return;
#ifdef Q_OS_LINUX
        auto notification = QDBusMessage::createMethodCall("org.freedesktop.Notifications",
            "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify");
        notification << QString("Orbit Native") << uint(0) << QString("orbit-native-shell") << title << body
                     << QStringList() << QVariantMap() << int(6000);
        QDBusConnection::sessionBus().asyncCall(notification, 2000);
#else
        if (tray_ && QSystemTrayIcon::supportsMessages()) tray_->showMessage(title, body, QSystemTrayIcon::Information, 6000);
#endif
    }

    void showAlarms()
    {
        if (!mainReady_ || pendingAlarms_.isEmpty()) return;
        resize(true);
        for (const auto &item : std::as_const(pendingAlarms_)) emitTo(main_, "alarm", item);
        pendingAlarms_ = QJsonArray();
    }

    void timerDue()
    {
        const auto now = QDateTime::currentDateTime();
        const auto timer = state_.value("timer").toObject();
        QJsonArray events;
        if (!timer.isEmpty() && !timer.value("paused").toBool() && timer.value("end").toInteger() <= now.toMSecsSinceEpoch()) {
            state_["timer"] = QJsonValue::Null;
            events.append(QJsonObject{{"title", QStringLiteral("Таймер завершён")}, {"kind", "timer"},
                {"body", QStringLiteral("Время вышло.")}, {"total", timer.value("total")}});
        }
        auto reminders = state_.value("reminders").toArray();
        for (qsizetype i = 0; i < reminders.size(); ++i) {
            auto reminder = reminders.at(i).toObject();
            auto at = QDateTime::fromMSecsSinceEpoch(reminder.value("at").toInteger());
            if (reminder.value("done").toBool() || at > now) continue;
            events.append(QJsonObject{{"title", QStringLiteral("Напоминание")}, {"kind", "reminder"},
                {"body", reminder.value("text")}});
            if (reminder.value("repeat").toString() == "daily") {
                do { at = at.addDays(1); } while (at <= now);
                reminder["at"] = at.toMSecsSinceEpoch();
            } else {
                reminder["done"] = true;
            }
            reminders[i] = reminder;
        }
        if (!events.isEmpty()) {
            state_["reminders"] = reminders;
            persist();
            emitTo(main_, "state", state_);
            for (const auto &item : events) {
                const auto alarm = item.toObject();
                notifyDesktop(alarm.value("title").toString(), alarm.value("body").toString());
                pendingAlarms_.append(item);
            }
            showAlarms();
        }
        scheduleTimer();
    }

    QJsonValue reminder(const QString &action, QJsonObject item)
    {
        const auto previous = state_.value("reminders");
        auto entries = previous.toArray();
        if (action == "add") {
            const auto text = item.value("text").toString().trimmed().left(500);
            const auto at = item.value("at").toInteger();
            if (text.isEmpty() || at <= QDateTime::currentMSecsSinceEpoch()) {
                return error(QStringLiteral("Укажите текст и время в будущем"));
            }
            entries.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"text", text},
                {"at", at}, {"repeat", item.value("repeat").toString() == "daily" ? "daily" : "once"}, {"done", false}});
        } else if (action == "remove") {
            for (qsizetype i = entries.size(); i-- > 0;) {
                if (entries.at(i).toObject().value("id") == item.value("id")) entries.removeAt(i);
            }
        } else {
            return error(QStringLiteral("Неизвестное действие напоминания"));
        }
        state_["reminders"] = entries;
        if (!persist()) { state_["reminders"] = previous; return error(QStringLiteral("Не удалось сохранить напоминание")); }
        scheduleTimer();
        return entries;
    }

    void captureClipboard()
    {
        if (!settings_.value("clipboard").toBool()) return;
        const auto *mime = QApplication::clipboard()->mimeData();
        if (!mime) return;
        for (const auto &format : mime->formats()) {
            if (format.contains("password", Qt::CaseInsensitive) || format.contains("sensitive", Qt::CaseInsensitive) ||
                format.contains("ExcludeClipboardContentFromMonitorProcessing", Qt::CaseInsensitive) ||
                format.contains("CanIncludeInClipboardHistory", Qt::CaseInsensitive)) return;
        }
        QJsonObject item{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                         {"at", QDateTime::currentMSecsSinceEpoch()}, {"pinned", false}};
        QString field;
        if (mime->hasImage()) {
            auto image = QApplication::clipboard()->image();
            if (image.isNull() || image.sizeInBytes() > 128 * 1024 * 1024) return;
            if (image.width() > 1200 || image.height() > 1200)
                image = image.scaled(1200, 1200, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QByteArray png;
            QBuffer output(&png);
            if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || png.size() > 2 * 1024 * 1024) return;
            item["type"] = "image";
            field = "data";
            item[field] = "data:image/png;base64," + QString::fromLatin1(png.toBase64());
        } else if (mime->hasText()) {
            const auto value = mime->text();
            if (value.trimmed().isEmpty() || value.size() > 100000) return;
            item["type"] = "text";
            field = "text";
            item[field] = value;
        } else return;
        const auto previous = state_.value("clips");
        auto clips = previous.toArray();
        for (const auto &entry : std::as_const(clips)) {
            if (entry.toObject().value(field) == item.value(field)) return;
        }
        clips.prepend(item);
        int unpinned = 0;
        qsizetype storedBytes = 0;
        for (qsizetype i = 0; i < clips.size();) {
            const auto clip = clips.at(i).toObject();
            const auto bytes = clip.value("data").toString().size() + 3 * clip.value("text").toString().size();
            if (!clip.value("pinned").toBool() && (++unpinned > 60 || storedBytes + bytes > 24 * 1024 * 1024))
                clips.removeAt(i);
            else {
                storedBytes += bytes;
                ++i;
            }
        }
        state_["clips"] = clips;
        if (persist()) emitTo(main_, "clips", clips);
        else state_["clips"] = previous;
    }

#ifdef Q_OS_WIN
    void pastePlainTextAfterRelease(HWND target, DWORD clipboardSequence, int attempts)
    {
        if (GetClipboardSequenceNumber() != clipboardSequence) return;
        if ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) |
             GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState('V')) & 0x8000) {
            if (attempts > 0) QTimer::singleShot(50, this, [this, target, clipboardSequence, attempts] {
                pastePlainTextAfterRelease(target, clipboardSequence, attempts - 1);
            });
            return;
        }
        const auto current = GetForegroundWindow();
        DWORD owner = 0;
        GetWindowThreadProcessId(current, &owner);
        if (current != target && owner != GetCurrentProcessId()) return;
        const auto result = pasteToPreviousWindow(target).toObject();
        if (result.contains("error")) emitTo(main_, "error", result.value("error"));
    }
#endif

#ifdef Q_OS_LINUX
    void pastePlainTextOnX11(unsigned long target, const QString &text, int attempts)
    {
        if (x11Input_->foreground().window != target || QApplication::clipboard()->text() != text) return;
        if (x11Input_->pasteShortcutHeld()) {
            if (attempts > 0) QTimer::singleShot(50, this, [this, target, text, attempts] {
                pastePlainTextOnX11(target, text, attempts - 1);
            });
            return;
        }
        if (!x11Input_->paste(target))
            emitTo(main_, "error", QStringLiteral("Текст скопирован. Вставьте его сочетанием Ctrl+V."));
    }
#endif

    QJsonValue pasteToPreviousWindow(
#ifdef Q_OS_WIN
        HWND target = nullptr
#endif
    )
    {
#ifdef Q_OS_WIN
        if (!target) target = lastExternalWindow_;
        if (!target || !IsWindow(target))
            return error(QStringLiteral("Откройте окно для вставки. Запись уже скопирована."));
        resize(false);
        SetForegroundWindow(target);
        if (GetForegroundWindow() != target)
            return error(QStringLiteral("Не удалось переключить окно. Запись уже скопирована."));
        INPUT keys[4]{};
        for (auto &key : keys) key.type = INPUT_KEYBOARD;
        keys[0].ki.wVk = keys[3].ki.wVk = VK_CONTROL;
        keys[1].ki.wVk = keys[2].ki.wVk = 'V';
        keys[2].ki.dwFlags = keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(4, keys, sizeof(INPUT)) == 4) return true;
#elif defined(Q_OS_LINUX)
        if (x11Input_ && lastExternalX11_ && x11Input_->activate(lastExternalX11_)) {
            const auto target = lastExternalX11_;
            resize(false);
            QTimer::singleShot(160, this, [this, target] {
                if (!x11Input_->paste(target)) emitTo(main_, "error", QStringLiteral("Запись скопирована. Вставьте её сочетанием Ctrl+V."));
            });
            return true;
        }
#endif
        return error(QStringLiteral("Запись скопирована. Вставьте её сочетанием Ctrl+V."));
    }

    QJsonValue clipAction(const QString &id, const QString &action)
    {
        const auto previous = state_.value("clips");
        auto entries = previous.toArray();
        if (action == "clear") {
            for (qsizetype i = entries.size(); i-- > 0;) {
                if (!entries.at(i).toObject().value("pinned").toBool()) entries.removeAt(i);
            }
        } else {
            for (qsizetype i = 0; i < entries.size(); ++i) {
                auto entry = entries.at(i).toObject();
                if (entry.value("id").toString() != id) continue;
                if (action == "copy" || action == "paste") {
                    auto *clipboard = QApplication::clipboard();
                    if (entry.value("type").toString() == "image") {
                        const auto data = entry.value("data").toString();
                        const QString prefix = "data:image/png;base64,";
                        QImage image;
                        if (!data.startsWith(prefix) ||
                            !image.loadFromData(QByteArray::fromBase64(data.mid(prefix.size()).toLatin1()), "PNG"))
                            return error(QStringLiteral("Не удалось восстановить изображение"));
                        clipboard->setImage(image);
                    } else clipboard->setText(entry.value("text").toString());
                    return action == "paste" ? pasteToPreviousWindow() : QJsonValue(true);
                }
                if (action == "pin") {
                    entry["pinned"] = !entry.value("pinned").toBool();
                    entries[i] = entry;
                } else if (action == "delete") entries.removeAt(i);
                else return error(QStringLiteral("Неизвестное действие с буфером обмена"));
                break;
            }
        }
        state_["clips"] = entries;
        if (!persist()) { state_["clips"] = previous; return error(QStringLiteral("Не удалось сохранить историю")); }
        emitTo(main_, "clips", entries);
        return entries;
    }

    QStringList localPaths(const QJsonArray &paths)
    {
        QStringList files;
        for (qsizetype index = 0; index < paths.size() && index < 100; ++index) {
            const QFileInfo info(paths.at(index).toString());
            if (info.isAbsolute() && info.exists()) files.append(info.absoluteFilePath());
        }
        files.removeDuplicates();
        return files;
    }

    QJsonValue addLaunchers(const QJsonArray &paths)
    {
        const auto files = localPaths(paths);
        if (files.isEmpty()) return error(QStringLiteral("Лаунчер принимает локальные файлы и папки"));
        const auto previous = settings_.value("items");
        auto items = previous.toArray();
        QJsonArray additions;
        for (const auto &path : files) {
            bool known = false;
            for (const auto &entry : std::as_const(items)) {
                const auto target = entry.toObject().value("target").toString();
#ifdef Q_OS_WIN
                known |= target.compare(path, Qt::CaseInsensitive) == 0;
#else
                known |= target == path;
#endif
            }
            if (known) continue;
            const QFileInfo file(path);
            const auto name = file.isDir() ? file.fileName() : file.completeBaseName();
            const QJsonObject item{{"name", name.left(40)}, {"target", path}, {"symbol", QStringLiteral("↗")}, {"group", ""}};
            additions.append(item);
            items.append(item);
        }
        if (items.size() > 24) return error(QStringLiteral("В лаунчере может быть до 24 ярлыков"));
        settings_["items"] = items;
        if (!persist()) {
            settings_["items"] = previous;
            return error(QStringLiteral("Не удалось сохранить ярлыки"));
        }
        if (preferences_ && !additions.isEmpty()) emitTo(preferences_, "launcher-added", additions);
        return QJsonObject{{"settings", settings_}, {"count", additions.size()}};
    }

    QJsonValue shelfAdd(const QJsonValue &paths)
    {
        const auto files = paths.isArray() ? localPaths(paths.toArray()) :
            QFileDialog::getOpenFileNames(nullptr, QStringLiteral("Добавить на полку"));
        const auto previous = state_.value("shelf");
        auto entries = previous.toArray();
        for (const auto &file : files) {
            bool exists = false;
            for (const auto &entry : std::as_const(entries)) exists |= entry.toObject().value("path").toString() == file;
            if (!exists) entries.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"name", QFileInfo(file).fileName()}, {"path", file}});
        }
        state_["shelf"] = entries;
        if (!persist()) { state_["shelf"] = previous; return error(QStringLiteral("Не удалось сохранить полку")); }
        return entries;
    }

    QJsonValue shelfAction(const QString &id, const QString &action)
    {
        const auto previous = state_.value("shelf");
        auto entries = previous.toArray();
        for (qsizetype i = 0; i < entries.size(); ++i) {
            const auto entry = entries.at(i).toObject();
            if (entry.value("id").toString() != id) continue;
            const QFileInfo path(entry.value("path").toString());
            if (action == "open") return launch(path.absoluteFilePath());
            if (action == "reveal") return launch(path.absolutePath());
            if (action == "remove") {
                entries.removeAt(i);
                state_["shelf"] = entries;
                if (!persist()) { state_["shelf"] = previous; return error(QStringLiteral("Не удалось сохранить полку")); }
                return entries;
            }
        }
        return error(QStringLiteral("Файл не найден на полке"));
    }

    bool dragShelfItem(const QString &id)
    {
        for (const auto &entry : state_.value("shelf").toArray()) {
            const auto file = entry.toObject();
            if (file.value("id").toString() != id) continue;
            const QFileInfo info(file.value("path").toString());
            if (!info.isAbsolute() || !info.exists()) return false;
            auto *drag = new QDrag(this);
            auto *data = new QMimeData;
            data->setUrls({QUrl::fromLocalFile(info.absoluteFilePath())});
            drag->setMimeData(data);
            drag->setPixmap(QFileIconProvider().icon(info).pixmap(40, 40));
            QTimer::singleShot(0, drag, [drag] {
                drag->exec(Qt::CopyAction);
                drag->deleteLater();
            });
            return true;
        }
        return false;
    }

    QString fileIcon(const QString &path)
    {
        bool known = false;
        const auto shelf = state_.value("shelf").toArray();
        for (const auto &entry : shelf) known |= entry.toObject().value("path").toString() == path;
        for (const auto &entry : settings_.value("items").toArray())
            known |= entry.toObject().value("target").toString() == path;
        if (!known || !QFileInfo(path).isAbsolute() || !QFileInfo::exists(path)) return {};
#ifdef Q_OS_WIN
        SHFILEINFOW file{};
        const auto nativePath = QDir::toNativeSeparators(path).toStdWString();
        if (!SHGetFileInfoW(nativePath.c_str(), 0, &file, sizeof(file), SHGFI_ICON) || !file.hIcon) return {};
        const auto icon = QImage::fromHICON(file.hIcon);
        DestroyIcon(file.hIcon);
#else
        QPixmap icon;
        if (path.endsWith(".desktop")) {
            QSettings desktop(path, QSettings::IniFormat);
            const auto name = desktop.value("Desktop Entry/Icon").toString();
            if (!name.isEmpty()) {
                const auto source = QFileInfo(name).isAbsolute() ? QIcon(name) : QIcon::fromTheme(name);
                icon = source.pixmap(48, 48);
            }
        }
        if (icon.isNull()) icon = QFileIconProvider().icon(QFileInfo(path)).pixmap(48, 48);
#endif
        QBuffer png;
        if (!png.open(QIODevice::WriteOnly) || !icon.save(&png, "PNG")) return {};
        return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.data().toBase64());
    }

    QJsonValue beginRecording(const QString &mime)
    {
        if (recordFile_) return error(QStringLiteral("Запись уже открыта"));
        if (mime.startsWith("video/mp4")) recordExtension_ = "mp4";
        else if (mime.startsWith("video/webm")) recordExtension_ = "webm";
        else return error(QStringLiteral("Формат записи не поддерживается"));
        auto temporary = std::make_unique<QTemporaryFile>();
        if (!temporary->open()) return error(QStringLiteral("Не удалось создать временный файл записи"));
        recordFile_ = std::move(temporary);
        return true;
    }

    QJsonValue writeRecording(const QString &encoded)
    {
        if (!recordFile_ || encoded.size() > 768 * 1024)
            return error(QStringLiteral("Не удалось получить фрагмент записи"));
        const auto result = QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!result || result.decoded.isEmpty() || result.decoded.size() > 512 * 1024)
            return error(QStringLiteral("Получен повреждённый фрагмент записи"));
        if (recordFile_->write(result.decoded) != result.decoded.size())
            return error(QStringLiteral("Не удалось записать фрагмент на диск"));
        return true;
    }

    QJsonValue saveRecording(bool externallyWritten = false)
    {
        auto input = std::move(recordFile_);
        if (!input) return error(QStringLiteral("Запись экрана пуста"));
        QFile externalInput(input->fileName());
        if (externallyWritten && !externalInput.open(QIODevice::ReadOnly))
            return error(QStringLiteral("Не удалось открыть записанный файл"));
        QFile &source = externallyWritten ? externalInput : *input;
        if ((!externallyWritten && !input->flush()) || !source.size() || !source.seek(0))
            return error(QStringLiteral("Запись экрана пуста"));
        const QDir videos(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
        const auto filename = "Orbit-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + '.' + recordExtension_;
        const auto liveTestPath = testing_ ? qEnvironmentVariable("ORBIT_TEST_RECORD_OUTPUT") : QString();
        std::optional<QTemporaryDir> testOutput;
        if (testing_ && liveTestPath.isEmpty()) {
            testOutput.emplace();
            if (!testOutput->isValid()) return error(QStringLiteral("Не удалось создать тестовую папку"));
        }
        const auto path = !liveTestPath.isEmpty() ? liveTestPath : testOutput ? QDir(testOutput->path()).filePath(filename) :
            QFileDialog::getSaveFileName(nullptr, QStringLiteral("Сохранить запись Orbit"),
                                                      videos.filePath(filename), recordExtension_.toUpper() + " (*." + recordExtension_ + ')');
        if (path.isEmpty()) return QJsonObject{{"saved", false}};
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly)) return error(QStringLiteral("Не удалось открыть файл для записи"));
        while (!source.atEnd()) {
            const auto chunk = source.read(1024 * 1024);
            if (chunk.isEmpty() || output.write(chunk) != chunk.size())
                return error(QStringLiteral("Не удалось сохранить запись"));
        }
        if (!output.commit()) return error(QStringLiteral("Не удалось сохранить запись"));
        if (testOutput) {
            QFile saved(path);
            if (!saved.open(QIODevice::ReadOnly) || saved.readAll() != QByteArray::fromHex("1a45dfa3"))
                return error(QStringLiteral("Тестовая запись не совпадает с исходными данными"));
        }
        return QJsonObject{{"saved", true}};
    }

    QJsonValue timer(const QJsonObject &command)
    {
        const auto action = command.value("action").toString();
        auto value = state_.value("timer").toObject();
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (action == "start") {
            const auto minutes = command.value("minutes").toDouble();
            if (minutes <= 0 || minutes > 1440) return error(QStringLiteral("Таймер: от 0 до 1440 минут"));
            const auto duration = qint64(minutes * 60000);
            value = {{"end", now + duration}, {"total", duration}, {"paused", false}};
        } else if (action == "stop") {
            value = {};
        } else if (!value.isEmpty() && action == "pause") {
            if (value.value("paused").toBool()) {
                value["end"] = now + value.take("remaining").toInteger();
                value["paused"] = false;
            } else {
                value["remaining"] = qMax(qint64{0}, value.value("end").toInteger() - now);
                value["paused"] = true;
            }
        } else if (!value.isEmpty() && action == "reset") {
            value["end"] = now + value.value("total").toInteger();
            value["paused"] = false;
            value.remove("remaining");
        }
        state_["timer"] = value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
        if (!persist()) return error(QStringLiteral("Не удалось сохранить таймер"));
        scheduleTimer();
        emitTo(main_, "state", state_);
        return state_.value("timer");
    }

    using Respond = std::function<void(QJsonValue)>;

#ifdef Q_OS_LINUX
    void startWaylandRecording(bool captureAudio, const Respond &respond)
    {
        if (recordFile_ || waylandPortal_) {
            respond(error(QStringLiteral("Запись уже открыта")));
            return;
        }
        waylandPortal_ = new PortalScreencast(this);
        connect(waylandPortal_, &PortalScreencast::closed, this, [this] { waylandEncoder_.stop(); });
        if (captureAudio) {
            recordingAudio_.request("monitor", {}, [this, respond](const QJsonObject &result) {
                const auto monitor = result.value("monitor").toString();
                if (monitor.isEmpty()) {
                    std::exchange(waylandPortal_, nullptr)->deleteLater();
                    respond(error(result.value("error").toString(QStringLiteral("Источник системного звука недоступен"))));
                    return;
                }
                openWaylandRecording(monitor, respond);
            });
        } else {
            openWaylandRecording({}, respond);
        }
    }

    void openWaylandRecording(const QString &monitor, const Respond &respond)
    {
        waylandPortal_->start([this, monitor, respond](PortalScreenStream stream, QString issue) {
            if (issue.isEmpty()) {
                const auto result = beginRecording("video/webm");
                if (result.isObject()) issue = result.toObject().value("error").toString();
            }
            if (issue.isEmpty()) {
                issue = waylandEncoder_.start(std::move(stream), recordFile_->fileName(), monitor, [this](QString failure) {
                    auto *portal = std::exchange(waylandPortal_, nullptr);
                    portal->deleteLater();
                    recording_ = false;
                    refreshMemoryTarget();
                    if (!failure.isEmpty()) recordFile_.reset();
                    const auto result = failure.isEmpty() ? saveRecording(true) : QJsonValue(error(failure));
                    emitTo(main_, "wayland-recording-finished", result);
                });
            }
            if (!issue.isEmpty()) {
                recordFile_.reset();
                if (auto *portal = std::exchange(waylandPortal_, nullptr)) portal->deleteLater();
                respond(error(issue));
                return;
            }
            recording_ = true;
            refreshMemoryTarget();
            respond(true);
        });
    }
#endif
    void visible(webview_t window, bool show)
    {
        if (!window) return;
#ifdef Q_OS_WIN
        ShowWindow(static_cast<HWND>(webview_get_window(window)), show ? SW_SHOWNA : SW_HIDE);
#elif defined(Q_OS_LINUX)
        auto *widget = GTK_WIDGET(webview_get_window(window));
        if (show) gtk_widget_show(widget);
        else gtk_widget_hide(widget);
#endif
    }

    void closeCapture()
    {
        visible(editor_, false);
        visible(main_, true);
        snapshot_ = {};
        capturePending_ = false;
    }

    void capture(const QString &mode, const Respond &done)
    {
        if (mode != "edit" && mode != "full" && mode != "ocr" && mode != "qr" && mode != "color") {
            done(error(QStringLiteral("Неизвестный режим захвата")));
            return;
        }
        if (capturePending_ || !snapshot_.isEmpty()) { done(error(QStringLiteral("Редактор уже открыт"))); return; }
        capturePending_ = true;
        if (!settings_.value("showInCapture").toBool()) visible(main_, false);
        QTimer::singleShot(testing_ ? 0 : 220, this, [this, mode, done] {
#ifdef Q_OS_LINUX
            if ((!testing_ && wayland_) ||
                (testing_ && qEnvironmentVariableIsSet("ORBIT_TEST_PORTAL"))) {
                portalScreenshot(this, [this, mode, done](QImage image, const QString &issue) {
                    if (!issue.isEmpty()) {
                        closeCapture();
                        done(error(issue));
                        return;
                    }
                    openScreenshot(image, mode, done);
                });
                return;
            }
#endif
            QImage screenImage;
            const auto screens = QGuiApplication::screens();
            auto *screen = screens.value(settings_.value("monitor").toInt(), QGuiApplication::primaryScreen());
            if (testing_) {
                screenImage = QImage(96, 64, QImage::Format_RGB32);
                screenImage.fill(QColor("#7856aa"));
            } else if (screen) {
                screenImage = screen->grabWindow(0).toImage();
            }
            openScreenshot(screenImage, mode, done);
        });
    }

    void openScreenshot(const QImage &screenImage, const QString &mode, const Respond &done)
    {
        if (screenImage.isNull()) {
            closeCapture();
            done(error(QStringLiteral("Не удалось сделать снимок монитора")));
            return;
        }
        if (mode == "full") {
            if (!testing_) QApplication::clipboard()->setImage(screenImage);
            closeCapture();
            done(true);
            return;
        }
        QBuffer png;
        if (!png.open(QIODevice::WriteOnly) || !screenImage.save(&png, "PNG") || png.size() > 32 * 1024 * 1024) {
            closeCapture();
            done(error(QStringLiteral("Снимок слишком большой или недоступен")));
            return;
        }
        snapshot_ = QJsonObject{{"image", QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.data().toBase64())},
            {"mode", mode}};
        if (!editor_) {
            if (testing_) std::fprintf(stderr, "Orbit smoke: creating editor window\n");
            editor_ = webview_create(false, nullptr);
            if (testing_) std::fprintf(stderr, "Orbit smoke: initializing editor page\n");
            if (editor_ && !open(editor_, "capture.html", false)) { webview_destroy(editor_); editor_ = nullptr; }
        } else {
            emitTo(editor_, "capture", snapshot_);
        }
        if (!editor_) {
            closeCapture();
            done(error(QStringLiteral("Не удалось открыть редактор")));
            return;
        }
        visible(main_, false);
#ifdef Q_OS_WIN
        auto hwnd = static_cast<HWND>(webview_get_window(editor_));
        auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME)) | WS_POPUP);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(webview_get_window(main_)), MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            const auto area = monitor.rcMonitor;
            SetWindowPos(hwnd, HWND_TOPMOST, area.left, area.top, area.right-area.left, area.bottom-area.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
#elif defined(Q_OS_LINUX)
        gtk_window_fullscreen(GTK_WINDOW(webview_get_window(editor_)));
#endif
        visible(editor_, true);
        if (testing_) std::fprintf(stderr, "Orbit smoke: editor shown\n");
        capturePending_ = false;
        done(true);
    }

    static QImage decodeImage(const QString &source)
    {
        const QString prefix = QStringLiteral("data:image/png;base64,");
        if (!source.startsWith(prefix) || source.size() > 46 * 1024 * 1024) return {};
        auto bytes = QByteArray::fromBase64(source.mid(prefix.size()).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::ReadOnly)) return {};
        QImageReader reader(&buffer, "PNG");
        const auto size = reader.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > 40000000) return {};
        return reader.read();
    }

    void recognize(const QImage &image, const Respond &done)
    {
        auto executable = QStandardPaths::findExecutable("tesseract");
        QStringList languages{"-l", "rus+eng"};
#ifdef Q_OS_WIN
        const QDir bundled(QDir(QCoreApplication::applicationDirPath()).filePath("orbit-ocr"));
        const auto installed = bundled.filePath("tesseract.exe");
        if (QFileInfo::exists(installed)) {
            executable = installed;
            languages << "--tessdata-dir" << bundled.filePath("tessdata");
        } else languages.clear();
#endif
        if (executable.isEmpty()) {
            done(error(QStringLiteral("Для OCR установите Tesseract и добавьте его в PATH.")));
            return;
        }
        auto *process = new QProcess(this);
        auto *file = new QTemporaryFile(QDir::tempPath() + "/orbit-ocr-XXXXXX.png", process);
        if (!file->open() || !image.save(file, "PNG")) {
            process->deleteLater();
            done(error(QStringLiteral("Не удалось сохранить временное изображение")));
            return;
        }
        const auto path = file->fileName();
        file->close();
        connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError issue) {
            if (issue != QProcess::FailedToStart) return;
            done(error(QStringLiteral("Не удалось запустить Tesseract")));
            process->deleteLater();
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [process, done](int code, QProcess::ExitStatus exit) {
            if (code || exit != QProcess::NormalExit) {
                done(error(QStringLiteral("OCR не выполнен. Проверьте языковые данные Tesseract.")));
            } else {
                const auto text = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
                QApplication::clipboard()->setText(text);
                done(QJsonObject{{"text", text}});
            }
            process->deleteLater();
        });
        QTimer::singleShot(20000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
#ifdef Q_OS_WIN
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
        });
#endif
        process->start(executable, QStringList{path, "stdout"} + languages);
    }

    void captureResult(const QJsonObject &request, const Respond &done)
    {
        const auto action = request.value("action").toString();
        if (action == "close") { closeCapture(); done(true); return; }
        const auto image = decodeImage(request.value("data").toString());
        if (image.isNull()) { done(error(QStringLiteral("Пустое или повреждённое изображение"))); return; }
        if (action == "ocr") {
            recognize(image, done);
        } else if (action == "copy") {
            if (!testing_) QApplication::clipboard()->setImage(image);
            closeCapture();
            done(QJsonObject{{"ok", true}});
        } else if (action == "save") {
            const auto path = QFileDialog::getSaveFileName(nullptr, QStringLiteral("Сохранить PNG"), "Orbit.png", "PNG (*.png)");
            if (path.isEmpty()) { done(QJsonObject{{"ok", false}}); return; }
            if (!image.save(path, "PNG")) { done(error(QStringLiteral("Не удалось сохранить PNG"))); return; }
            closeCapture();
            done(QJsonObject{{"ok", true}});
        } else {
            done(error(QStringLiteral("Неизвестное действие редактора")));
        }
    }

    QJsonValue pickColor(const QString &color)
    {
        static const QRegularExpression valid(QStringLiteral("^#[0-9a-fA-F]{6}$"));
        if (!valid.match(color).hasMatch()) return error(QStringLiteral("Неверный цвет"));
        if (!testing_) QApplication::clipboard()->setText(color);
        auto colors = state_.value("colors").toArray();
        for (qsizetype i = colors.size(); i-- > 0;) {
            if (colors.at(i).toString() == color) colors.removeAt(i);
        }
        colors.prepend(color);
        while (colors.size() > 20) colors.removeLast();
        state_["colors"] = colors;
        return persist() ? QJsonValue(true) : QJsonValue(error(QStringLiteral("Не удалось сохранить цвет")));
    }

    void startTray()
    {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
        tray_ = new QSystemTrayIcon(QIcon(QDir(directory_).filePath("icon.png")), this);
        trayMenu_ = std::make_unique<QMenu>();
        trayMenu_->addAction(QStringLiteral("Открыть Orbit"), this, [this] { visible(main_, true); resize(true); });
        trayMenu_->addAction(QStringLiteral("Выход"), this, &OrbitShellController::quit);
        tray_->setContextMenu(trayMenu_.get());
        tray_->setToolTip(QStringLiteral("Orbit Native"));
        tray_->show();
    }

    void notifyUpdate()
    {
        checkUpdate([this](const QJsonValue &result) {
            const auto status = result.toObject();
            const auto version = status.value("latest").toString();
            if (!tray_ || !status.value("available").toBool() || notifiedUpdate_ == version) return;
            notifiedUpdate_ = version;
            tray_->showMessage(QStringLiteral("Orbit Native"), QStringLiteral("Доступна версия %1").arg(version));
        });
    }

    void checkUpdate(const Respond &done)
    {
        if (testing_) {
            done(QJsonObject{{"current", ORBIT_VERSION}, {"latest", QJsonValue::Null}, {"available", false}});
            return;
        }
        QNetworkRequest request(QUrl("https://api.github.com/repos/TrashDokiDoki/Orbit-Native/releases/latest"));
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setRawHeader("User-Agent", "Orbit-Native");
        request.setTransferTimeout(10000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        auto *reply = network_.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
            const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto release = QJsonDocument::fromJson(reply->readAll()).object();
            reply->deleteLater();
            const auto tag = release.value("tag_name").toString();
            static const QRegularExpression version(QStringLiteral("^v[0-9]+[.][0-9]+[.][0-9]+$"));
            if (code != 404 && (code != 200 || !version.match(tag).hasMatch() ||
                release.value("draft").toBool() || release.value("prerelease").toBool())) {
                done(error(QStringLiteral("Не удалось проверить обновления GitHub")));
                return;
            }
            const auto latest = code == 404 ? QJsonValue(QJsonValue::Null) : QJsonValue(tag.mid(1));
            const auto newer = code == 200 && QVersionNumber::fromString(tag.mid(1)) > QVersionNumber::fromString(ORBIT_VERSION);
            update_ = QJsonObject{{"current", ORBIT_VERSION}, {"latest", latest}, {"available", newer}};
            emitTo(main_, "update-result", update_);
            done(update_);
        });
    }

    void request(const QUrl &url, Respond done)
    {
        QNetworkRequest request(url);
        request.setTransferTimeout(10000);
        auto *reply = network_.get(request);
        connect(reply, &QNetworkReply::finished, this, [reply, done] {
            auto data = reply->error() == QNetworkReply::NoError ? QJsonDocument::fromJson(reply->readAll()).object()
                                                                  : error(QStringLiteral("Не удалось получить прогноз"));
            reply->deleteLater();
            done(data);
        });
    }

    void lyrics(const QJsonObject &track, const Respond &done)
    {
        const auto title = track.value("title").toString().trimmed().left(250);
        const auto artist = track.value("artist").toString().trimmed().left(250);
        if (title.isEmpty() || artist.isEmpty()) {
            done(error(QStringLiteral("В медиасессии нет названия или исполнителя")));
            return;
        }
        QUrl url("https://lrclib.net/api/search");
        QUrlQuery query;
        query.addQueryItem("track_name", title);
        query.addQueryItem("artist_name", artist);
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setTransferTimeout(8000);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Orbit/%1").arg(ORBIT_VERSION));
        auto *reply = network_.get(request);
        connect(reply, &QIODevice::readyRead, this, [reply] {
            if (reply->bytesAvailable() > 8 * 1024 * 1024) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [reply, done, title, artist, track] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                done(error(QStringLiteral("Не удалось подключиться к LRCLIB")));
                return;
            }
            const auto normalized = [](const QString &s) {
                return s.normalized(QString::NormalizationForm_KC).trimmed().toLower();
            };
            const auto album = normalized(track.value("album").toString());
            const auto duration = track.value("duration").toDouble();
            const auto entries = QJsonDocument::fromJson(reply->readAll()).array();
            QJsonObject best;
            int bestScore = -1;
            for (const auto &entry : entries) {
                const auto candidate = entry.toObject();
                if (normalized(candidate.value("trackName").toString()) != normalized(title) ||
                    normalized(candidate.value("artistName").toString()) != normalized(artist)) continue;
                const bool close = std::abs(candidate.value("duration").toDouble() - duration) <= 3;
                const bool sameAlbum = !album.isEmpty() && normalized(candidate.value("albumName").toString()) == album;
                const bool synced = !candidate.value("syncedLyrics").toString().isEmpty();
                const int score = (close ? 4 : 0) + (sameAlbum ? 2 : 0) + (synced ? 1 : 0);
                if (score > bestScore) {
                    best = candidate;
                    bestScore = score;
                }
            }
            const auto synced = best.value("syncedLyrics").toString();
            auto text = best.value("plainLyrics").toString();
            if (text.isEmpty())
                text = QString(synced).remove(QRegularExpression(R"(\[\d+:\d+(?:[.:]\d+)?\])"));
            text = text.trimmed().left(25000);
            if (best.value("instrumental").toBool()) text = QStringLiteral("Инструментальный трек");
            if (text.isEmpty()) text = QStringLiteral("В LRCLIB нет текста для этого трека");
            QJsonObject result{{"text", text}};
            if (!best.value("instrumental").toBool() && !synced.isEmpty() &&
                (duration <= 0 || std::abs(best.value("duration").toDouble() - duration) <= 3))
                result["synced"] = synced.left(40000);
            done(result);
        });
    }

    void weather(const QString &city, const QJsonObject &location, const Respond &done)
    {
        if (testing_) { done(error("Network disabled in smoke test")); return; }
        const double latitude = location.value("latitude").toDouble(181);
        const double longitude = location.value("longitude").toDouble(181);
        if (!location.value("name").toString().isEmpty() &&
            std::abs(latitude) <= 90 && std::abs(longitude) <= 180) {
            weatherForecast(location, done);
            return;
        }
        const auto name = city.section(',', 0, 0).simplified();
        if (name.compare(QStringLiteral("Красная Горбатка"), Qt::CaseInsensitive) == 0 ||
            name.compare(QStringLiteral("Красная Горбака"), Qt::CaseInsensitive) == 0) {
            weatherForecast({{"name", QStringLiteral("Красная Горбатка")},
                {"admin1", QStringLiteral("Владимирская область")}, {"country", QStringLiteral("Россия")},
                {"latitude", 55.87701}, {"longitude", 41.75508}}, done);
            return;
        }
        QUrl geo("https://geocoding-api.open-meteo.com/v1/search");
        QUrlQuery params;
        params.setQueryItems({{"name", city.left(80)}, {"count", "1"}, {"language", "ru"}});
        geo.setQuery(params);
        request(geo, [this, done](QJsonValue value) {
            const auto result = value.toObject();
            if (result.contains("error")) { done(result); return; }
            const auto places = result.value("results").toArray();
            if (places.isEmpty()) { done(error(QStringLiteral("Город не найден"))); return; }
            weatherForecast(places.first().toObject(), done);
        });
    }

    void weatherForecast(const QJsonObject &place, const Respond &done)
    {
        QUrl forecast("https://api.open-meteo.com/v1/forecast");
        QUrlQuery query;
        query.setQueryItems({{"latitude", QString::number(place.value("latitude").toDouble())},
            {"longitude", QString::number(place.value("longitude").toDouble())}, {"timezone", "auto"}, {"forecast_days", "7"},
            {"wind_speed_unit", "ms"},
            {"current", "temperature_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m,relative_humidity_2m,surface_pressure,is_day,precipitation"},
            {"hourly", "temperature_2m,weather_code,is_day,precipitation_probability,precipitation,dew_point_2m,surface_pressure,uv_index"},
            {"daily", "temperature_2m_max,temperature_2m_min,weather_code,sunrise,sunset,precipitation_sum,uv_index_max"}});
        forecast.setQuery(query);
        request(forecast, [place, done](QJsonValue data) {
            done(data.toObject().contains("error") ? data : QJsonValue(QJsonObject{{"location", place}, {"forecast", data}}));
        });
    }

    QJsonValue launch(const QString &target)
    {
        const auto url = QUrl(target);
        if (url.scheme() == "https" || url.scheme() == "http" || url.scheme() == "spotify" || url.scheme() == "steam") {
            if (QDesktopServices::openUrl(url)) return true;
        } else {
            const QFileInfo file(target);
            if (!file.isAbsolute() || !file.exists()) return error(QStringLiteral("Ярлык не найден"));
#ifdef Q_OS_LINUX
            if (file.isFile() && file.suffix() == "desktop") {
                auto *desktop = g_desktop_app_info_new_from_filename(QFile::encodeName(file.absoluteFilePath()).constData());
                if (!desktop) return error(QStringLiteral("Не удалось прочитать ярлык приложения"));
                auto *display = gdk_display_get_default();
                auto *context = display ? gdk_display_get_app_launch_context(display) : nullptr;
                const bool launched = g_app_info_launch(G_APP_INFO(desktop), nullptr,
                    context ? G_APP_LAUNCH_CONTEXT(context) : nullptr, nullptr);
                if (context) g_object_unref(context);
                g_object_unref(desktop);
                if (launched) return true;
                return error(QStringLiteral("Не удалось запустить приложение из ярлыка"));
            }
            if (file.isFile() && file.isExecutable()) {
                const auto type = QMimeDatabase().mimeTypeForFile(file);
                if (type.inherits("application/x-executable") || type.inherits("application/x-pie-executable") ||
                    file.suffix().compare("AppImage", Qt::CaseInsensitive) == 0)
                    return QProcess::startDetached(file.absoluteFilePath(), {}, file.absolutePath())
                        ? QJsonValue(true) : QJsonValue(error(QStringLiteral("Не удалось запустить приложение")));
            }
#endif
            if (QDesktopServices::openUrl(QUrl::fromLocalFile(file.absoluteFilePath()))) return true;
        }
        return error(QStringLiteral("Не удалось открыть ярлык"));
    }

    void showSettingsSection()
    {
        if (!preferences_ || !preferencesReady_) return;
        emitTo(preferences_, "settings-section", QJsonObject{
            {"section", settingsSection_}, {"add", std::exchange(settingsAdd_, false)}
        });
    }

    struct Channel { OrbitShellController *owner; webview_t window; QString token; };

    void receive(const Channel &channel, const QByteArray &id, const QByteArray &requestBody)
    {
        if (shuttingDown_) return;
        const auto window = channel.window;
        const auto respond = [this, window, id](const QJsonValue &value) {
            if (!shuttingDown_ && (window == main_ || window == preferences_ || window == editor_))
                webview_return(window, id.constData(), 0, json(value).constData());
        };
        if (requestBody.size() > (window == editor_ ? 48 : 8) * 1024 * 1024) { respond(error("Request too large")); return; }
        const auto args = QJsonDocument::fromJson(requestBody).array();
        if (args.size() != 3 || args.at(0).toString() != channel.token) { respond(error("Untrusted window")); return; }
        const auto command = args.at(1).toString();
        const auto params = args.at(2).toArray();
        const auto first = params.at(0);
        if (command == "get") {
            const auto system = QSysInfo::productType() == "windows" ? QString("Windows") : QString("Linux");
            QJsonArray displays;
            const auto screens = QGuiApplication::screens();
            for (qsizetype i = 0; i < screens.size(); ++i) {
                const auto name = screens.at(i)->name();
                displays.append(QJsonObject{{"index", int(i)}, {"label", name.isEmpty() ? QStringLiteral("Монитор %1").arg(i+1) : name}});
            }
            respond(QJsonObject{{"settings", settings_}, {"state", state_}, {"version", ORBIT_VERSION},
                    {"systemLabel", system}, {"platform", system == "Windows" ? "win32" : "linux"},
                    {"waylandRecording", wayland_},
                    {"gpuAccelerationActive", gpuEnabled_}, {"displays", displays}, {"hotkeys", unavailableHotkeys_},
                    {"steamDownload", steamDownload_}});
            if (window == preferences_) {
                preferencesReady_ = true;
                showSettingsSection();
            }
            if (window == main_) {
                mainReady_ = true;
                if (!lastSystem_.isEmpty()) emitTo(main_, "system", lastSystem_);
                showAlarms();
            }
        } else if (command == "expand" && window == main_) {
            resize(first.toBool()); respond(true);
        } else if (command == "compact-mini" && window == main_) {
            mini_ = first.toBool() && !expanded_ && musicPlaying_;
            if (!expanded_) applyBounds();
            respond(mini_);
        } else if (command == "island-shape" && window == main_) {
            shape(first.toObject()); respond(true);
        } else if (command == "open-settings" && window == main_) {
            settingsSection_ = first.toString(QStringLiteral("general"));
            settingsAdd_ = params.at(1).toBool();
            if (!preferences_) {
                preferences_ = webview_create(false, nullptr);
                if (preferences_ && !open(preferences_, "settings.html", false)) { webview_destroy(preferences_); preferences_ = nullptr; }
            } else {
#ifdef Q_OS_WIN
                ShowWindow(static_cast<HWND>(webview_get_window(preferences_)), SW_SHOW);
#elif defined(Q_OS_LINUX)
                gtk_window_present(GTK_WINDOW(webview_get_window(preferences_)));
#endif
                showSettingsSection();
            }
            respond(preferences_ ? QJsonValue(true) : QJsonValue(error(QStringLiteral("Не удалось открыть настройки"))));
        } else if (command == "save" && window == preferences_) {
            const auto previous = settings_;
            const auto incoming = first.toObject();
            if (!incoming.value("items").isArray() || incoming.value("items").toArray().size() > 24) {
                respond(error(QStringLiteral("Максимум 24 приложения"))); return;
            }
            for (auto i = settings_.begin(); i != settings_.end(); ++i) {
                if (incoming.contains(i.key()) && incoming.value(i.key()).type() == i.value().type()) i.value() = incoming.value(i.key());
            }
            const bool autostartChanged = previous.value("autostart") != settings_.value("autostart");
            const bool captureVisibilityChanged = previous.value("showInCapture") != settings_.value("showInCapture");
            if (captureVisibilityChanged && !setCaptureVisibility(settings_.value("showInCapture").toBool())) {
                settings_ = previous;
                respond(error(QStringLiteral("Не удалось изменить видимость при захвате экрана")));
                return;
            }
            if (autostartChanged && !setAutostart(settings_.value("autostart").toBool())) {
                if (captureVisibilityChanged) setCaptureVisibility(previous.value("showInCapture").toBool());
                settings_ = previous;
                respond(error(QStringLiteral("Не удалось изменить автозапуск")));
                return;
            }
            if (!persist()) {
                if (autostartChanged) setAutostart(previous.value("autostart").toBool());
                if (captureVisibilityChanged) setCaptureVisibility(previous.value("showInCapture").toBool());
                settings_ = previous;
                respond(error(QStringLiteral("Не удалось сохранить настройки")));
                return;
            }
            if (settings_.value("musicMode").toString() != "mini") mini_ = false;
            applyBounds();
            if (!testing_) registerHotkeys();
            emitTo(main_, "settings-changed", settings_);
            respond(QJsonObject{{"settings", settings_}, {"hotkeys", unavailableHotkeys_}});
        } else if (command == "notes-save" && window == main_ && first.isArray()) {
            const auto previous = state_.value("notes");
            state_["notes"] = first;
            if (!persist()) { state_["notes"] = previous; respond(error(QStringLiteral("Не удалось сохранить заметки"))); }
            else respond(true);
        } else if (command == "weather") {
            weather(first.toString(), params.at(1).toObject(), respond);
#ifdef Q_OS_LINUX
        } else if (command == "wayland-record-start" && window == main_ &&
                   wayland_) {
            startWaylandRecording(first.toBool(), respond);
        } else if (command == "wayland-record-stop" && window == main_ && recording_ && waylandPortal_) {
            QTimer::singleShot(0, this, [this] { waylandEncoder_.stop(); });
            respond(true);
#endif
        } else if (command == "record-begin" && window == main_) {
            respond(beginRecording(first.toString()));
        } else if (command == "record-chunk" && window == main_) {
            respond(writeRecording(first.toString()));
        } else if (command == "record-finish" && window == main_) {
            QTimer::singleShot(0, this, [this, respond] { respond(saveRecording()); });
        } else if (command == "record-discard" && window == main_ && !recording_) {
            recordFile_.reset();
            respond(true);
        } else if (command == "record-state" && window == main_) {
            recording_ = first.toBool();
            refreshMemoryTarget();
            emitTo(main_, "recording", recording_);
            respond(true);
        } else if (command == "capture" && window == main_) {
            capture(first.toString(), respond);
        } else if (command == "capture-ready" && window == editor_) {
            if (testing_) std::fprintf(stderr, "Orbit smoke: editor requested screenshot\n");
            respond(snapshot_);
        } else if (command == "capture-result" && window == editor_) {
            captureResult(first.toObject(), respond);
        } else if (command == "color" && window == editor_) {
            respond(pickColor(first.toString()));
        } else if (command == "timer" && window == main_) {
            respond(timer(first.toObject()));
        } else if (command == "reminder" && window == main_) {
            respond(reminder(first.toString(), params.at(1).toObject()));
        } else if (command == "clips") {
            respond(state_.value("clips"));
        } else if (command == "clip-action" && window == main_) {
            respond(clipAction(first.toString(), params.at(1).toString()));
        } else if (command == "shelf-add" && window == main_) {
            respond(shelfAdd(first));
        } else if (command == "drop-launcher" && window == main_) {
            respond(addLaunchers(first.toArray()));
        } else if (command == "drop-paths" && window == main_) {
            QJsonArray paths;
            auto uris = first.toArray();
#ifdef Q_OS_LINUX
            if (uris.isEmpty() && QDateTime::currentMSecsSinceEpoch() - recentDropTime_ < 1000) uris = recentDrop_;
            recentDrop_ = QJsonArray();
            recentDropTime_ = 0;
#endif
            for (const auto &uri : uris) {
                const auto url = QUrl(uri.toString());
                if (url.isLocalFile() && url.host().isEmpty()) paths.append(url.toLocalFile());
            }
            respond(QJsonArray::fromStringList(localPaths(paths)));
        } else if (command == "shelf-action" && window == main_) {
            respond(shelfAction(first.toString(), params.at(1).toString()));
        } else if (command == "copy" && first.isString()) {
            QApplication::clipboard()->setText(first.toString()); respond(true);
        } else if (command == "pick") {
            respond(QFileDialog::getOpenFileName(nullptr, QStringLiteral("Выбрать файл")));
        } else if (command == "launch") {
            const auto item = settings_.value("items").toArray().at(first.toInt(-1)).toObject();
            auto result = launch(item.value("target").toString());
            if (result.isObject() && item.contains("fallback")) result = launch(item.value("fallback").toString());
            respond(result);
        } else if (command == "web") {
            const auto url = QUrl(first.toString());
            respond(url.scheme() == "http" || url.scheme() == "https" ? launch(first.toString()) : QJsonValue(error("Unsupported URL")));
        } else if (command == "media-source" && window == main_) {
            media_.select(first.toString());
            respond(QJsonValue::Null);
        } else if (command == "lyrics" && window == main_) {
            lyrics(first.toObject(), respond);
        } else if (command == "network-tools-status" && window == main_) {
            networkToolStatus(this, settings_.value("proxyPath").toString(), respond);
        } else if (command == "network-tools-set" && window == main_ && params.at(1).isBool()) {
            if (networkChanging_) { respond(error(QStringLiteral("Дождитесь завершения переключения"))); return; }
            networkChanging_ = true;
            setNetworkTool(this, first.toString(), params.at(1).toBool(), settings_.value("proxyPath").toString(),
                [this, respond](QJsonObject status) { networkChanging_ = false; respond(status); });
        } else if (command == "network-tools-pick-proxy" && window == main_) {
            if (networkChanging_) { respond(error(QStringLiteral("Дождитесь завершения переключения"))); return; }
            const auto path = QFileDialog::getOpenFileName(nullptr, QStringLiteral("Выберите TG WS Proxy"));
            if (!path.isEmpty()) {
                if (!validProxyFile(path)) { respond(error(QStringLiteral("Выберите исполняемый файл TG WS Proxy для этой ОС"))); return; }
                const auto previous = settings_.value("proxyPath");
                settings_["proxyPath"] = path;
                if (!persist()) { settings_["proxyPath"] = previous; respond(error(QStringLiteral("Не удалось сохранить путь"))); return; }
            }
            networkToolStatus(this, settings_.value("proxyPath").toString(), respond);
        } else if (command == "network-tools-link" && window == main_) {
            if (first.toString() == "zapret") {
#ifdef Q_OS_LINUX
                respond(launch(QStringLiteral("https://github.com/bol-van/zapret")));
#else
                respond(launch(QStringLiteral("https://github.com/Flowseal/zapret-discord-youtube")));
#endif
            }
            else if (first.toString() == "tg") respond(launch(QStringLiteral("https://github.com/Flowseal/tg-ws-proxy/releases/latest")));
            else respond(error(QStringLiteral("Неизвестный инструмент")));
        } else if (command == "happ" && window == main_) {
            if (first.toString() == "status") respond(happStatus());
            else if (first.toString() == "toggle") respond(toggleHapp() ? QJsonValue(true) : QJsonValue(error(QStringLiteral("Не удалось открыть Happ"))));
            else if (first.toString() == "ip" && !testing_) {
                request(QUrl("https://ipinfo.io/json"), [respond](QJsonValue value) {
                    const auto info = value.toObject();
                    const auto ip = info.value("ip").toString();
                    const auto country = info.value("country").toString();
                    static const QRegularExpression countryCode(QStringLiteral("^[A-Z]{2}$"));
                    if (QHostAddress(ip).isNull() || !countryCode.match(country).hasMatch()) {
                        respond(error(QStringLiteral("Не удалось определить IP и страну")));
                    } else {
                        respond(QJsonObject{{"ip", ip}, {"country", country}});
                    }
                });
            } else respond(error(QStringLiteral("Неизвестная команда Happ")));
        } else if (command == "controls" && window == main_) {
            desktopControl(this, first.toString(), respond);
        } else if (command == "system-link" && window == main_) {
            respond(openSystemSettings(first.toString()));
        } else if (command == "native" && first.toString() == "dictation" && window == main_) {
            resize(false);
#ifdef Q_OS_LINUX
            respond(dictation_.open() ? QJsonValue(true) :
                QJsonValue(error(QStringLiteral("Установите сборку Orbit Native с офлайн-диктовкой."))));
#else
            respond(startDictation());
#endif
        } else if (command == "audio" && window == main_) {
            audio_.request(first.toString(), params.at(1), respond);
        } else if (command == "eq-profile" && window == main_) {
            respond(equalizer(first));
        } else if (command == "native" && first.toString() == "volume" && window == main_) {
            const auto action = params.at(1).toObject().value("command").toString();
            if (action != "up" && action != "down" && action != "mute") { respond(error("Unsupported volume command")); return; }
            audio_.request(action, {}, respond);
        } else if (command == "native" && first.toString() == "media") {
            const auto action = params.at(1).toObject().value("command").toString();
            if (action != "previous" && action != "next" && action != "play") { respond(error("Unsupported media command")); return; }
            media_.send(action == "previous" ? Media::Previous : action == "next" ? Media::Next : Media::PlayPause);
            respond(true);
        } else if (command == "update-status") {
            respond(update_.isEmpty() ? QJsonObject{{"current", ORBIT_VERSION}} : update_);
        } else if (command == "update-check") {
            checkUpdate(respond);
        } else if (command == "update-open" && update_.value("available").toBool()) {
            respond(launch(QStringLiteral("https://github.com/TrashDokiDoki/Orbit-Native/releases/tag/v") + update_.value("latest").toString()));
        } else if (command == "data-export" && window == preferences_) {
            respond(exportBackup());
        } else if (command == "data-folder" && window == preferences_) {
            respond(launch(QFileInfo(store_).absolutePath()));
        } else if (command == "drag-file" && window == main_) {
            respond(dragShelfItem(first.toString()));
        } else if (command == "file-icon") {
            respond(fileIcon(first.toString()));
        } else if (command == "controls-view") {
            respond(true);
        } else if (command == "quit") {
            respond(true);
            quit();
        } else {
            respond(error(QStringLiteral("Эта функция ещё переносится в Orbit Native.")));
        }
    }

    QString directory_, store_;
    QString settingsSection_ = QStringLiteral("general");
    bool settingsAdd_ = false;
    bool preferencesReady_ = false;
    bool testing_ = false, passed_ = false;
    Media media_;
    Audio audio_;
    QNetworkAccessManager network_;
    QTimer alarm_, updateTimer_;
    QSystemTrayIcon *tray_ = nullptr;
    std::unique_ptr<QMenu> trayMenu_;
    QString notifiedUpdate_;
    QJsonObject settings_, state_, update_;
    QJsonObject lastSystem_;
    bool mainReady_ = false;
    bool shuttingDown_ = false;
    QJsonArray pendingAlarms_;
#ifdef Q_OS_LINUX
    QJsonArray recentDrop_;
    qint64 recentDropTime_ = 0;
#endif
    Track currentTrack_;
    QJsonArray unavailableHotkeys_;
#ifdef Q_OS_WIN
    std::map<int, std::function<void()>> hotkeys_;
    HWND lastExternalWindow_ = nullptr;
    HWND lastRuleWindow_ = nullptr;
    int shutdownReplies_ = 0;
#elif defined(Q_OS_LINUX)
    GtkWidget *mainGtkWindow_ = nullptr;
    unsigned long lastExternalX11_ = 0;
    unsigned long lastForegroundPid_ = 0;
    std::unique_ptr<X11Input> x11Input_;
    std::unique_ptr<PortalShortcuts> portalShortcuts_;
    Dictation dictation_;
#endif
    QTimer collapse_;
    QTimer keyboardPoll_;
    QTimer steamPoll_;
    QString steamDownload_;
#if defined(Q_OS_WIN) && defined(__ICoreWebView2_19_INTERFACE_DEFINED__)
    QTimer memoryIdle_;
    bool lowMemory_ = false;
#endif
    int layout_ = 0;
    QString layoutLabel_;
#ifdef Q_OS_LINUX
    QString kdeLayoutLabel_;
    quint64 kdeLayoutRevision_ = 0;
#endif
    bool caps_ = false;
    bool inactive_ = false;
    bool keyboardKnown_ = false;
    QString foregroundName_;
    bool hiddenFullscreen_ = false;
    bool autoHidden_ = false;
    bool restricted_ = false;
    int battery_ = -1;
    bool plugged_ = false;
    qint64 indicatorUntil_ = 0;
    QSize lastWindowSize_;
    double lastScale_ = 1;
    bool expanded_ = false;
    bool mini_ = false;
    bool musicPlaying_ = false;
    bool gpuEnabled_ = true;
    bool wayland_ = false;
#ifdef Q_OS_LINUX
    bool x11_ = false;
#endif
    bool recording_ = false;
    QString recordExtension_;
    std::unique_ptr<QTemporaryFile> recordFile_;
#ifdef Q_OS_LINUX
    PortalScreencast *waylandPortal_ = nullptr;
    WaylandVideoEncoder waylandEncoder_;
    Audio recordingAudio_;
#endif
    QUrl coverUrl_;
    QString fetchedCover_;
    quint64 coverRequest_ = 0;
    qint64 coverRevision_ = 0;
    QString sentCover_;
    webview_t main_ = nullptr, preferences_ = nullptr, editor_ = nullptr;
    QJsonObject snapshot_;
    bool capturePending_ = false;
    bool networkChanging_ = false;
    std::vector<std::unique_ptr<Channel>> channels_;
};
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("OrbitNative");
    app.setApplicationName("Orbit Native Shell");
    const auto args = app.arguments();
    auto path = QDir(app.applicationDirPath()).filePath("orbit-shell-ui");
    const auto ui = args.indexOf("--ui");
    if (ui >= 0 && ui + 1 < args.size()) path = args.at(ui + 1);
    OrbitShellController shell(path, args.contains("--smoke-test"));
    return shell.run();
}

#ifdef ORBIT_KWIN_LAYOUT_DBUS
#include "main.moc"
#endif
