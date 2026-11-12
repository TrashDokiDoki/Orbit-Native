#include "webview/webview.h"
#include "media.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#include <utility>
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

class Shell final : public QObject {
public:
    Shell(QString directory, bool testing)
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
        alarm_.setSingleShot(true);
        connect(&alarm_, &QTimer::timeout, this, &Shell::timerDue);
    }

    int run()
    {
        main_ = webview_create(false, nullptr);
        if (!main_) return 2;
        if (!open(main_, "index.html", true)) { webview_destroy(main_); return 2; }
        resize(false);
        media_.changed = [this](const Track &track) {
            const auto media = track.title.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonObject{
                {"title", track.title}, {"artist", track.artist}, {"playing", track.playing}, {"source", "MPRIS"}
            });
            if (lastMedia_ == media) return;
            lastMedia_ = media;
            emitTo(main_, "system", QJsonObject{{"media", media}});
        };
        if (!testing_) media_.refresh();
        if (testing_) QTimer::singleShot(20000, this, [this] { webview_terminate(main_); });
        scheduleTimer();
        webview_run(main_);
        if (preferences_) webview_destroy(preferences_);
        webview_destroy(main_);
        return testing_ && !passed_ ? 1 : 0;
    }

private:
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
        if (testing_ && island) {
            webview_init(window, R"JS(
                document.addEventListener('DOMContentLoaded', async () => {
                  const initial = await orbit.get();
                  await orbit.expand(true);
                  const notes = [{id:'qa',name:'Orbit QA',folder:'Local',text:'Save and load'}];
                  await orbit.notesSave(notes);
                  const stored = await orbit.get();
                  const valid = Boolean(initial.settings && stored.state.notes[0]?.text === notes[0].text &&
                    document.querySelector('#island.expanded') && document.querySelector('[data-page=weather]') &&
                    getComputedStyle(document.querySelector('#island')).borderTopLeftRadius !== '0px');
                  window.orbitShellTest(valid);
                });
            )JS");
            webview_bind(window, "orbitShellTest", [](const char *id, const char *args, void *data) {
                auto *self = static_cast<Shell *>(data);
                self->passed_ = QJsonDocument::fromJson(args).array().at(0).toBool();
                webview_return(self->main_, id, 0, "null");
                webview_terminate(self->main_);
            }, this);
        }
        webview_set_title(window, island ? "Orbit Native" : "Orbit Native — Settings");
        if (island) styleWindow();
        else {
            webview_set_size(window, 1020, 790, WEBVIEW_HINT_NONE);
#ifdef Q_OS_WIN
            SetWindowSubclass(static_cast<HWND>(webview_get_window(window)), hidePreferences, 1, 0);
#elif defined(Q_OS_LINUX)
            g_signal_connect(webview_get_window(window), "delete-event", G_CALLBACK(+[](GtkWidget *widget, GdkEvent *, gpointer) {
                gtk_widget_hide(widget);
                return TRUE;
            }), nullptr);
#endif
        }
        webview_navigate(window, address.constData());
        return true;
    }

#ifdef Q_OS_WIN
    static LRESULT CALLBACK hidePreferences(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_CLOSE) { ShowWindow(window, SW_HIDE); return 0; }
        return DefSubclassProc(window, message, wParam, lParam);
    }
#endif

    void styleWindow()
    {
#ifdef Q_OS_WIN
        auto hwnd = static_cast<HWND>(webview_get_window(main_));
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

    void resize(bool expanded)
    {
        webview_set_size(main_, expanded ? 856 : 184, expanded ? 514 : 38, WEBVIEW_HINT_FIXED);
#ifdef Q_OS_WIN
        auto hwnd = static_cast<HWND>(webview_get_window(main_));
        MONITORINFO monitor{sizeof(MONITORINFO)};
        RECT bounds{};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor) && GetWindowRect(hwnd, &bounds)) {
            const auto area = monitor.rcWork;
            const auto width = bounds.right - bounds.left;
            SetWindowPos(hwnd, HWND_TOPMOST, (area.left + area.right - width) / 2, area.top + 12, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        }
#elif defined(Q_OS_LINUX)
        if (const auto *screen = QGuiApplication::primaryScreen()) {
            const auto area = screen->availableGeometry();
            gtk_window_move(GTK_WINDOW(webview_get_window(main_)), area.center().x() - (expanded ? 428 : 92), area.top() + 12);
        }
#endif
        emitTo(main_, "expanded", expanded);
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
        const auto body = QJsonDocument(QJsonObject{{"settings", settings_}, {"state", state_}}).toJson();
        return file.write(body) == body.size() && file.commit();
    }

    void scheduleTimer()
    {
        alarm_.stop();
        const auto timer = state_.value("timer").toObject();
        if (timer.isEmpty() || timer.value("paused").toBool()) return;
        const auto delay = timer.value("end").toInteger() - QDateTime::currentMSecsSinceEpoch();
        alarm_.start(int(std::clamp(delay, qint64{0}, qint64{30000})));
    }

    void timerDue()
    {
        const auto timer = state_.value("timer").toObject();
        if (timer.value("end").toInteger() > QDateTime::currentMSecsSinceEpoch()) { scheduleTimer(); return; }
        state_["timer"] = QJsonValue::Null;
        persist();
        emitTo(main_, "state", state_);
        resize(true);
        emitTo(main_, "alarm", QJsonObject{{"title", QStringLiteral("Таймер завершён")}, {"kind","timer"},
              {"body", QStringLiteral("Время вышло.")}, {"total", timer.value("total")}});
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

    struct Channel { Shell *owner; webview_t window; QString token; };

    void receive(const Channel &channel, const QByteArray &id, const QByteArray &requestBody)
    {
        const auto window = channel.window;
        const auto respond = [window, id](const QJsonValue &value) { webview_return(window, id.constData(), 0, json(value).constData()); };
        if (requestBody.size() > 8 * 1024 * 1024) { respond(error("Request too large")); return; }
        const auto args = QJsonDocument::fromJson(requestBody).array();
        if (args.size() != 3 || args.at(0).toString() != channel.token) { respond(error("Untrusted window")); return; }
        const auto command = args.at(1).toString();
        const auto params = args.at(2).toArray();
        const auto first = params.at(0);
        if (command == "get") {
            const auto system = QSysInfo::productType() == "windows" ? QString("Windows") : QString("Linux");
            respond(QJsonObject{{"settings", settings_}, {"state", state_}, {"version", ORBIT_VERSION},
                    {"systemLabel", system}, {"displays", QJsonArray{}}, {"hotkeys", QJsonArray{}}, {"gpuAccelerationActive", false}});
            if (window == main_ && !lastMedia_.isNull()) emitTo(main_, "system", QJsonObject{{"media", lastMedia_}});
        } else if (command == "expand" && window == main_) {
            resize(first.toBool()); respond(true);
        } else if (command == "open-settings" && window == main_) {
            if (!preferences_) {
                preferences_ = webview_create(false, nullptr);
                if (preferences_ && !open(preferences_, "settings.html", false)) { webview_destroy(preferences_); preferences_ = nullptr; }
            } else {
#ifdef Q_OS_WIN
                ShowWindow(static_cast<HWND>(webview_get_window(preferences_)), SW_SHOW);
#elif defined(Q_OS_LINUX)
                gtk_window_present(GTK_WINDOW(webview_get_window(preferences_)));
#endif
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
            emitTo(main_, "settings-changed", settings_);
            respond(QJsonObject{{"settings", settings_}, {"hotkeys", QJsonArray{}}});
        } else if (command == "notes-save" && window == main_ && first.isArray()) {
            const auto previous = state_.value("notes");
            state_["notes"] = first;
            if (!persist()) { state_["notes"] = previous; respond(error(QStringLiteral("Не удалось сохранить заметки"))); }
            else respond(true);
        } else if (command == "weather") {
            weather(first.toString(), respond);
        } else if (command == "timer" && window == main_) {
            respond(timer(first.toObject()));
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
        } else if (command == "native" && first.toString() == "media") {
            const auto action = params.at(1).toObject().value("command").toString();
            if (action != "previous" && action != "next" && action != "play") { respond(error("Unsupported media command")); return; }
            media_.send(action == "previous" ? Media::Previous : action == "next" ? Media::Next : Media::PlayPause);
            respond(true);
        } else if (command == "update-status") {
            respond(QJsonObject{{"current", ORBIT_VERSION}});
        } else if (command == "file-icon") {
            respond(QString());
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
    bool testing_ = false, passed_ = false;
    Media media_;
    QNetworkAccessManager network_;
    QTimer alarm_;
    QJsonObject settings_, state_;
    QJsonValue lastMedia_ = QJsonValue::Null;
    webview_t main_ = nullptr, preferences_ = nullptr;
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
    Shell shell(path, args.contains("--smoke-test"));
    return shell.run();
}
