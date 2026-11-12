#include "media.h"
#include "audio.h"
#include "desktop.h"
#include "network_tools.h"

#include <QApplication>
#include <QClipboard>
#include <QBuffer>
#include <QFileIconProvider>
#include <QMimeData>
#include <QPixmap>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QHostAddress>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QIcon>
#include <QMenu>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QVersionNumber>
#include <algorithm>
#include <cmath>
#include <array>
#include <functional>
#include <memory>
#include <vector>
#include <utility>
#include "webview/webview.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <commctrl.h>
#elif defined(Q_OS_LINUX)
#include <gtk/gtk.h>
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
        collapse_.setSingleShot(true);
        connect(&collapse_, &QTimer::timeout, this, &OrbitShellController::applyBounds);
        alarm_.setSingleShot(true);
        connect(&alarm_, &QTimer::timeout, this, &OrbitShellController::timerDue);
    }

    int run()
    {
        main_ = webview_create(false, nullptr);
        if (!main_) return 2;
        if (!open(main_, "index.html", true)) { webview_destroy(main_); return 2; }
        resize(false);
        media_.changed = [this](const Track &track) { updateMedia(track); };
        if (!testing_ || qEnvironmentVariableIsSet("ORBIT_TEST_MEDIA")) media_.refresh();
        if (!testing_) {
            connect(&keyboardPoll_, &QTimer::timeout, this, &OrbitShellController::updateKeyboard);
            keyboardPoll_.start(500);
            updateKeyboard();
            startTray();
            updateTimer_.setInterval(6 * 60 * 60 * 1000);
            updateTimer_.setTimerType(Qt::VeryCoarseTimer);
            connect(&updateTimer_, &QTimer::timeout, this, &OrbitShellController::notifyUpdate);
            updateTimer_.start();
            QTimer::singleShot(10000, this, &OrbitShellController::notifyUpdate);
            connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &OrbitShellController::captureClipboard);
        }
        if (testing_) QTimer::singleShot(20000, this, [this] { webview_terminate(main_); });
        scheduleTimer();
        webview_run(main_);
        if (tray_) tray_->setContextMenu(nullptr);
        if (editor_) webview_destroy(editor_);
        if (preferences_) webview_destroy(preferences_);
        webview_destroy(main_);
        return testing_ && !passed_ ? 1 : 0;
    }

private:
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

    void updateKeyboard()
    {
        int layout = 0;
        bool caps = false;
        bool inactive = false;
        QString label;
#ifdef Q_OS_WIN
        const DWORD thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
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
#endif
        if (keyboardKnown_ && caps == caps_ && layout == layout_ && inactive == inactive_) return;
        const bool notifyCaps = keyboardKnown_ && caps != caps_;
        const bool notifyLayout = keyboardKnown_ && layout != layout_ && !label.isEmpty();
        keyboardKnown_ = true;
        caps_ = caps;
        layout_ = layout;
        inactive_ = inactive;
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
                                   {"caps", caps_}, {"layout", layout_}, {"inactive", inactive_}, {"battery", -1}};
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
        if (testing_ && (island || page == "capture.html")) {
            webview_bind(window, "orbitShellTest", [](const char *, const char *args, void *data) {
                auto *self = static_cast<OrbitShellController *>(data);
                self->passed_ = QJsonDocument::fromJson(args).array().at(0).toBool();
                webview_terminate(self->main_);
            }, this);
        }
        if (testing_ && island) {
            const auto testProxy = qEnvironmentVariable("ORBIT_TEST_PROXY");
            if (validProxyFile(testProxy)) {
                settings_["proxyPath"] = testProxy;
                webview_init(window, "window.orbitStrictProxyTest = true;");
            }
            if (qEnvironmentVariableIsSet("ORBIT_TEST_MEDIA")) webview_init(window, "window.orbitStrictMediaTest = true;");
            if (qEnvironmentVariableIsSet("ORBIT_TEST_AUDIO")) webview_init(window, "window.orbitStrictAudioTest = true;");
            webview_init(window, R"JS(
                document.addEventListener('DOMContentLoaded', async () => {
                  const initial = await orbit.get();
                  await orbit.expand(true);
                  const notes = [{id:'qa',name:'Orbit QA',folder:'Local',text:'Save and load'}];
                  await orbit.notesSave(notes);
                  const added = await orbit.reminder('add', {text:'Orbit QA',at:Date.now()+60000,repeat:'once'});
                  const stored = await orbit.get();
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
                      window.orbitShellTest(false);
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
                      await orbit.native('media', {command:'playpause'});
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
                  const desktopShape = typeof controls.networkUp === 'boolean' && typeof controls.dark === 'boolean';
                  const valid = Boolean(eqSaved && soundShape && desktopShape && proxyTest && mediaTest && initial.settings && stored.state.notes[0]?.text === notes[0].text &&
                    added[0]?.text === 'Orbit QA' && document.querySelector('#island.expanded') &&
                    document.querySelector('[data-page=weather]') &&
                    getComputedStyle(document.querySelector('#island')).borderTopLeftRadius !== '0px');
                  if (!valid || (await orbit.capture('edit'))?.error) window.orbitShellTest(false);
                });
            )JS");
        } else if (testing_ && page == "capture.html") {
            webview_init(window, R"JS(
                document.addEventListener('DOMContentLoaded', () => {
                  orbit.onCapture(async () => {
                    const canvas = document.querySelector('canvas');
                    for (let i=0; i<30 && canvas.width!==96; i++) await new Promise(resolve=>setTimeout(resolve,50));
                    const decoded = canvas.width === 96 && canvas.height === 64;
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
            SetWindowSubclass(static_cast<HWND>(webview_get_window(window)), hideSecondary, 1, reinterpret_cast<DWORD_PTR>(this));
#elif defined(Q_OS_LINUX)
            g_signal_connect(webview_get_window(window), "delete-event", G_CALLBACK(+[](GtkWidget *widget, GdkEvent *, gpointer data) {
                auto *self = static_cast<OrbitShellController *>(data);
                if (self->editor_ && widget == webview_get_window(self->editor_)) self->closeCapture();
                else gtk_widget_hide(widget);
                return TRUE;
            }), this);
#endif
        }
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
        return DefSubclassProc(window, message, wParam, lParam);
    }
#endif

    void styleWindow()
    {
#ifdef Q_OS_WIN
        auto hwnd = static_cast<HWND>(webview_get_window(main_));
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
#elif defined(Q_OS_LINUX)
        auto *window = GTK_WINDOW(webview_get_window(main_));
        gtk_window_set_decorated(window, FALSE);
        gtk_window_set_keep_above(window, TRUE);
        gtk_window_set_skip_taskbar_hint(window, TRUE);
        auto *browser = WEBKIT_WEB_VIEW(webview_get_native_handle(main_, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
        GdkRGBA background{0, 0, 0, 1};
        if (browser) webkit_web_view_set_background_color(browser, &background);
#endif
    }

    void shape(const QJsonObject &metrics)
    {
#ifdef Q_OS_WIN
        auto hwnd = static_cast<HWND>(webview_get_window(main_));
        RECT client{};
        if (!GetClientRect(hwnd, &client)) return;
        const auto scale = GetDpiForWindow(hwnd) / 96.0;
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
#else
        Q_UNUSED(metrics);
#endif
    }

    void resize(bool expanded)
    {
        const bool wasExpanded = expanded_;
        expanded_ = expanded;
        mini_ = false;
        collapse_.stop();
        if (wasExpanded && !expanded) collapse_.start(360);
        else applyBounds();
        emitTo(main_, "expanded", expanded);
    }

    void applyBounds()
    {
        const auto screens = QGuiApplication::screens();
        const auto index = settings_.value("monitor").toInt();
        const auto *screen = screens.value(index, QGuiApplication::primaryScreen());
        if (!screen) return;
        const auto area = screen->availableGeometry();
        const bool edge = settings_.value("shape").toString() == "edge";
        const int top = edge ? screen->geometry().top() : area.top() + std::clamp(settings_.value("top").toInt(), 0, 200);
        const double opacity = std::clamp(settings_.value("opacity").toDouble(1), .45, 1.0);
        const auto mode = settings_.value("musicMode").toString();
        const int compactWidth = !musicPlaying_ ? 184 : mode == "cover" ? 232 : 366;
        const bool panel = expanded_ || collapse_.isActive();
        const QSize size(panel ? 856 : compactWidth, panel ? 514 : mini_ ? 94 : 38);
#ifdef Q_OS_WIN
        auto hwnd = static_cast<HWND>(webview_get_window(main_));
        if (lastWindowSize_ != size) SetWindowRgn(hwnd, nullptr, TRUE);
#endif
        if (lastWindowSize_ != size) {
            lastWindowSize_ = size;
            webview_set_size(main_, size.width(), size.height(), WEBVIEW_HINT_FIXED);
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
            const int y = edge ? info.rcMonitor.top : info.rcWork.top + qRound((top - area.top()) * dpi);
            SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (opacity < 1) {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
            SetLayeredWindowAttributes(hwnd, 0, BYTE(qRound(opacity * 255)), LWA_ALPHA);
        } else if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        }
#elif defined(Q_OS_LINUX)
        int x = area.x() + (area.width() - size.width()) / 2;
        if (position == "left") x = area.left() + 16;
        if (position == "right") x = area.right() + 1 - size.width() - 16;
        auto *window = GTK_WINDOW(webview_get_window(main_));
        gtk_window_move(window, x, top);
        gtk_widget_set_opacity(GTK_WIDGET(window), opacity);
#endif
    }

    void emitTo(webview_t window, const QString &event, const QJsonValue &value)
    {
        if (!window) return;
        const auto script = "window.orbitNativeDispatch?.(" + json(event) + "," + json(value) + ");";
        webview_eval(window, script.constData());
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
            resize(true);
            for (const auto &item : events) emitTo(main_, "alarm", item);
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
        if (!mime || !mime->hasText()) return;
        for (const auto &format : mime->formats()) {
            if (format.contains("password", Qt::CaseInsensitive) || format.contains("sensitive", Qt::CaseInsensitive) ||
                format.contains("ExcludeClipboardContentFromMonitorProcessing", Qt::CaseInsensitive) ||
                format.contains("CanIncludeInClipboardHistory", Qt::CaseInsensitive)) return;
        }
        const auto value = mime->text();
        if (value.trimmed().isEmpty() || value.size() > 16000) return;
        const auto previous = state_.value("clips");
        auto clips = previous.toArray();
        for (const auto &entry : std::as_const(clips)) {
            if (entry.toObject().value("text").toString() == value) return;
        }
        clips.prepend(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"type", "text"},
            {"text", value}, {"at", QDateTime::currentMSecsSinceEpoch()}, {"pinned", false}});
        int unpinned = 0;
        for (qsizetype i = 0; i < clips.size();) {
            if (!clips.at(i).toObject().value("pinned").toBool() && ++unpinned > 60) clips.removeAt(i);
            else ++i;
        }
        state_["clips"] = clips;
        if (persist()) emitTo(main_, "clips", clips);
        else state_["clips"] = previous;
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
                if (action == "copy") {
                    QApplication::clipboard()->setText(entry.value("text").toString());
                    return true;
                }
                if (action == "pin") {
                    entry["pinned"] = !entry.value("pinned").toBool();
                    entries[i] = entry;
                } else if (action == "delete") entries.removeAt(i);
                else return error(QStringLiteral("Автовставка ещё переносится. Используйте кнопку «Копировать»."));
                break;
            }
        }
        state_["clips"] = entries;
        if (!persist()) { state_["clips"] = previous; return error(QStringLiteral("Не удалось сохранить историю")); }
        emitTo(main_, "clips", entries);
        return entries;
    }

    QJsonValue shelfAdd()
    {
        const auto files = QFileDialog::getOpenFileNames(nullptr, QStringLiteral("Добавить на полку"));
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

    QString fileIcon(const QString &path)
    {
        bool known = false;
        const auto shelf = state_.value("shelf").toArray();
        for (const auto &entry : shelf) known |= entry.toObject().value("path").toString() == path;
        if (!known || !QFileInfo::exists(path)) return {};
        const auto icon = QFileIconProvider().icon(QFileInfo(path)).pixmap(48, 48);
        QBuffer png;
        if (!png.open(QIODevice::WriteOnly) || !icon.save(&png, "PNG")) return {};
        return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.data().toBase64());
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
        if (!testing_ && QGuiApplication::platformName().startsWith("wayland")) {
            done(error(QStringLiteral("Под Wayland нужен портал захвата. Его адаптер ещё переносится.")));
            return;
        }
        capturePending_ = true;
        visible(main_, false);
        QTimer::singleShot(testing_ ? 0 : 220, this, [this, mode, done] {
            QImage screenImage;
            auto screens = QGuiApplication::screens();
            const auto index = settings_.value("monitor").toInt();
            auto *screen = index >= 0 && index < screens.size() ? screens.at(index) : QGuiApplication::primaryScreen();
            if (testing_) {
                screenImage = QImage(96, 64, QImage::Format_RGB32);
                screenImage.fill(QColor("#7856aa"));
            } else if (screen) {
                screenImage = screen->grabWindow(0).toImage();
            }
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
                editor_ = webview_create(false, nullptr);
                if (editor_ && !open(editor_, "capture.html", false)) { webview_destroy(editor_); editor_ = nullptr; }
            } else {
                emitTo(editor_, "capture", snapshot_);
            }
            if (!editor_) {
                closeCapture();
                done(error(QStringLiteral("Не удалось открыть редактор")));
                return;
            }
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
            capturePending_ = false;
            done(true);
        });
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
        const auto executable = QStandardPaths::findExecutable("tesseract");
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
#ifdef Q_OS_LINUX
        process->start(executable, {path, "stdout", "-l", "rus+eng"});
#else
        process->start(executable, {path, "stdout"});
#endif
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
        trayMenu_->addAction(QStringLiteral("Выход"), this, [this] { webview_terminate(main_); });
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

    void weather(const QString &city, const Respond &done)
    {
        if (testing_) { done(error("Network disabled in smoke test")); return; }
        QUrl geo("https://geocoding-api.open-meteo.com/v1/search");
        QUrlQuery params;
        params.setQueryItems({{"name", city.left(80)}, {"count", "1"}, {"language", "ru"}});
        geo.setQuery(params);
        request(geo, [this, done](QJsonValue value) {
            const auto result = value.toObject();
            if (result.contains("error")) { done(result); return; }
            const auto places = result.value("results").toArray();
            if (places.isEmpty()) { done(error(QStringLiteral("Город не найден"))); return; }
            const auto place = places.first().toObject();
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
        });
    }

    QJsonValue launch(const QString &target)
    {
        const auto url = QUrl(target);
        if (url.scheme() == "https" || url.scheme() == "http" || url.scheme() == "spotify" || url.scheme() == "steam") {
            if (QDesktopServices::openUrl(url)) return true;
        } else if (QFileInfo(target).isAbsolute() && QFileInfo(target).exists() &&
                   QDesktopServices::openUrl(QUrl::fromLocalFile(target))) return true;
        return error(QStringLiteral("Не удалось открыть ярлык"));
    }

    struct Channel { OrbitShellController *owner; webview_t window; QString token; };

    void receive(const Channel &channel, const QByteArray &id, const QByteArray &requestBody)
    {
        const auto window = channel.window;
        const auto respond = [window, id](const QJsonValue &value) { webview_return(window, id.constData(), 0, json(value).constData()); };
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
                    {"systemLabel", system}, {"displays", displays}, {"hotkeys", QJsonArray{}}});
            if (window == preferences_) emitTo(preferences_, "settings-section", QJsonObject{{"section", settingsSection_}});
            if (window == main_ && !lastSystem_.isEmpty()) emitTo(main_, "system", lastSystem_);
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
            if (!preferences_) {
                preferences_ = webview_create(false, nullptr);
                if (preferences_ && !open(preferences_, "settings.html", false)) { webview_destroy(preferences_); preferences_ = nullptr; }
            } else {
#ifdef Q_OS_WIN
                ShowWindow(static_cast<HWND>(webview_get_window(preferences_)), SW_SHOW);
#elif defined(Q_OS_LINUX)
                gtk_window_present(GTK_WINDOW(webview_get_window(preferences_)));
#endif
                emitTo(preferences_, "settings-section", QJsonObject{{"section", settingsSection_}});
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
            if (!persist()) { settings_ = previous; respond(error(QStringLiteral("Не удалось сохранить настройки"))); return; }
            if (settings_.value("musicMode").toString() != "mini") mini_ = false;
            applyBounds();
            emitTo(main_, "settings-changed", settings_);
            respond(QJsonObject{{"settings", settings_}, {"hotkeys", QJsonArray{}}});
        } else if (command == "notes-save" && window == main_ && first.isArray()) {
            const auto previous = state_.value("notes");
            state_["notes"] = first;
            if (!persist()) { state_["notes"] = previous; respond(error(QStringLiteral("Не удалось сохранить заметки"))); }
            else respond(true);
        } else if (command == "weather") {
            weather(first.toString(), respond);
        } else if (command == "capture" && window == main_) {
            capture(first.toString(), respond);
        } else if (command == "capture-ready" && window == editor_) {
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
            respond(shelfAdd());
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
            if (first.toString() == "zapret") respond(launch(QStringLiteral("https://github.com/Flowseal/zapret-discord-youtube")));
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
            respond(startDictation());
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
        } else if (command == "file-icon") {
            respond(fileIcon(first.toString()));
        } else if (command == "controls-view") {
            respond(true);
        } else if (command == "quit") {
            respond(true);
            webview_terminate(main_);
        } else {
            respond(error(QStringLiteral("Эта функция ещё переносится в Orbit Native.")));
        }
    }

    QString directory_, store_;
    QString settingsSection_ = QStringLiteral("general");
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
    Track currentTrack_;
    QTimer collapse_;
    QTimer keyboardPoll_;
    int layout_ = 0;
    bool caps_ = false;
    bool inactive_ = false;
    bool keyboardKnown_ = false;
    QSize lastWindowSize_;
    bool expanded_ = false;
    bool mini_ = false;
    bool musicPlaying_ = false;
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
