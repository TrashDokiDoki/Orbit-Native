#include "audio.h"

#include <QTimer>
#include <algorithm>
#include <utility>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>
#elif defined(Q_OS_LINUX)
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>
#endif

namespace {
QJsonObject failure(const QString &message) { return {{"error", message}}; }
QJsonObject noDevice() { return {{"endpoints", QJsonObject{}}}; }
QJsonObject status(const QJsonObject &devices, const QString &id, int volume, bool muted)
{
    return {{"endpoints", devices}, {"id", id}, {"level", volume}, {"muted", muted}};
}

int nextLevel(const QString &command, const QJsonValue &value, int current)
{
    if (command == "up") return std::min(100, current + 2);
    if (command == "down") return std::max(0, current - 2);
    return qRound(std::clamp(value.toDouble(), 0.0, 100.0));
}
}

#ifdef Q_OS_WIN
namespace {
using Microsoft::WRL::ComPtr;

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

// The endpoint selector uses the same Windows PolicyConfig ABI as Orbit 0.3.3.
struct __declspec(uuid("F8679F50-850A-41CF-9C72-430F290290C8")) PolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR id, ERole role) = 0;
};
const CLSID policyClass{0x870af99c,0x171d,0x4f9e,{0xaf,0x0d,0xe6,0x3d,0xf4,0x0c,0x2b,0xc9}};
const PROPERTYKEY friendlyName{{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}},14};

QString deviceId(IMMDevice *device)
{
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw))) return {};
    const auto result = QString::fromWCharArray(raw);
    CoTaskMemFree(raw);
    return result;
}
QString deviceName(IMMDevice *device)
{
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return {};
    PROPVARIANT value{};
    if (FAILED(properties->GetValue(friendlyName, &value))) return {};
    const auto result = value.vt == VT_LPWSTR ? QString::fromWCharArray(value.pwszVal) : QString{};
    PropVariantClear(&value);
    return result;
}
}

struct Audio::Backend {
    void request(const QString &command, const QJsonValue &value, const Result &result)
    {
        ComApartment apartment;
        ComPtr<IMMDeviceEnumerator> enumerator;
        const auto unavailable = [] { return failure(QStringLiteral("Системное управление звуком недоступно")); };
        if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) { result(unavailable()); return; }
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(enumerator.GetAddressOf())))) { result(unavailable()); return; }

        ComPtr<IMMDeviceCollection> collection;
        QJsonObject devices;
        UINT count = 0;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> item;
            if (FAILED(collection->Item(i, &item))) continue;
            const auto id = deviceId(item.Get());
            const auto name = deviceName(item.Get());
            if (!id.isEmpty()) devices[id] = name.isEmpty() ? id : name;
        }
        if (command == "select") {
            const auto id = value.toString();
            if (!devices.contains(id)) { result(failure(QStringLiteral("Устройство отключено"))); return; }
            const auto nativeId = id.toStdWString();
            ComPtr<PolicyConfig> policy;
            if (FAILED(CoCreateInstance(policyClass, nullptr, CLSCTX_ALL, IID_PPV_ARGS(policy.GetAddressOf()))) ||
                FAILED(policy->SetDefaultEndpoint(nativeId.c_str(), eConsole)) ||
                FAILED(policy->SetDefaultEndpoint(nativeId.c_str(), eMultimedia))) {
                result(failure(QStringLiteral("Не удалось переключить устройство Windows"))); return;
            }
        }
        ComPtr<IMMDevice> current;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &current))) { result(noDevice()); return; }
        ComPtr<IAudioEndpointVolume> volume;
        if (FAILED(current->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void **>(volume.GetAddressOf())))) { result(unavailable()); return; }
        float level = 0;
        BOOL muted = FALSE;
        if (FAILED(volume->GetMasterVolumeLevelScalar(&level)) || FAILED(volume->GetMute(&muted))) { result(unavailable()); return; }
        if (command == "mute") {
            if (FAILED(volume->SetMute(!muted, nullptr))) { result(unavailable()); return; }
            muted = !muted;
        } else if (command == "level" || command == "up" || command == "down") {
            level = float(nextLevel(command, value, qRound(level * 100))) / 100;
            if (FAILED(volume->SetMasterVolumeLevelScalar(level, nullptr))) { result(unavailable()); return; }
        }
        result(status(devices, deviceId(current.Get()), qRound(level * 100), muted != FALSE));
    }
};
#elif defined(Q_OS_LINUX)
struct Audio::Backend {
    pa_glib_mainloop *loop = nullptr;
    pa_context *context = nullptr;
    QTimer timeout;
    Result done;
    QString command;
    QJsonValue input;
    QString current;
    QString monitor;
    QJsonObject devices;
    pa_cvolume channels{};
    bool muted = false;
    bool currentFound = false;

    Backend()
    {
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &timeout, [this] {
            disconnect();
            complete(failure(QStringLiteral("Аудиосервер не ответил")));
        });
    }
    ~Backend()
    {
        timeout.stop();
        done = {};
        disconnect();
        if (loop) pa_glib_mainloop_free(loop);
    }

    void disconnect()
    {
        if (!context) return;
        pa_context_set_state_callback(context, nullptr, nullptr);
        pa_context_disconnect(context);
        pa_context_unref(context);
        context = nullptr;
    }

    void complete(QJsonObject response)
    {
        timeout.stop();
        if (!done) return;
        auto reply = std::exchange(done, {});
        reply(std::move(response));
    }

    void operation(pa_operation *op)
    {
        if (op) pa_operation_unref(op);
        else complete(failure(QStringLiteral("Операция аудиосервера не выполнена")));
    }

    void request(const QString &name, const QJsonValue &value, Result result)
    {
        if (done) { result(failure(QStringLiteral("Дождитесь ответа аудиосервера"))); return; }
        done = std::move(result);
        command = name;
        input = value;
        timeout.start(4000);
        if (context && pa_context_get_state(context) == PA_CONTEXT_READY) { readServer(); return; }
        disconnect();
        if (!loop) loop = pa_glib_mainloop_new(nullptr);
        if (loop) context = pa_context_new(pa_glib_mainloop_get_api(loop), "Orbit Native");
        if (!context) { complete(failure(QStringLiteral("Аудиосервер недоступен"))); return; }
        pa_context_set_state_callback(context, [](pa_context *ctx, void *user) {
            auto *self = static_cast<Backend *>(user);
            const auto state = pa_context_get_state(ctx);
            if (state == PA_CONTEXT_READY && self->done) self->readServer();
            else if (!PA_CONTEXT_IS_GOOD(state)) self->complete(failure(QStringLiteral("PulseAudio или PipeWire-Pulse недоступен")));
        }, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
            complete(failure(QStringLiteral("PulseAudio или PipeWire-Pulse недоступен")));
        }
    }

    void readServer()
    {
        devices = {};
        current.clear();
        monitor.clear();
        currentFound = false;
        operation(pa_context_get_server_info(context, [](pa_context *, const pa_server_info *info, void *user) {
            auto *self = static_cast<Backend *>(user);
            if (!self->done) return;
            if (!info) { self->complete(failure(QStringLiteral("Не удалось прочитать аудиосервер"))); return; }
            self->current = QString::fromUtf8(info->default_sink_name);
            self->readDevices();
        }, this));
    }

    void readDevices()
    {
        operation(pa_context_get_sink_info_list(context, [](pa_context *, const pa_sink_info *info, int end, void *user) {
            auto *self = static_cast<Backend *>(user);
            if (!self->done) return;
            if (end < 0) { self->complete(failure(QStringLiteral("Не удалось получить устройства звука"))); return; }
            if (end > 0) { self->apply(); return; }
            if (!info || !info->name) return;
            const auto id = QString::fromUtf8(info->name);
            self->devices[id] = info->description ? QString::fromUtf8(info->description) : id;
            if (id == self->current) {
                self->monitor = QString::fromUtf8(info->monitor_source_name);
                self->channels = info->volume;
                self->muted = info->mute != 0;
                self->currentFound = true;
            }
        }, this));
    }

    void apply()
    {
        if (command == "monitor") {
            complete(monitor.isEmpty() ? failure(QStringLiteral("Источник системного звука недоступен"))
                                       : QJsonObject{{"monitor", monitor}});
            return;
        }
        if (command == "status") {
            const auto level = currentFound ? qRound(double(pa_cvolume_max(&channels)) * 100 / PA_VOLUME_NORM) : 0;
            complete(devices.isEmpty() ? noDevice() : status(devices, currentFound ? current : QString{}, level, muted));
            return;
        }
        const auto changed = [](pa_context *, int ok, void *user) {
            auto *self = static_cast<Backend *>(user);
            if (!self->done) return;
            if (!ok) { self->complete(failure(QStringLiteral("Аудиосервер отклонил изменение"))); return; }
            self->command = "status";
            self->readServer();
        };
        if (command == "select") {
            const auto id = input.toString();
            if (!devices.contains(id)) { complete(failure(QStringLiteral("Устройство отключено"))); return; }
            operation(pa_context_set_default_sink(context, id.toUtf8().constData(), changed, this));
        } else if (!currentFound) {
            complete(failure(QStringLiteral("Устройство воспроизведения недоступно")));
        } else if (command == "mute") {
            operation(pa_context_set_sink_mute_by_name(context, current.toUtf8().constData(), !muted, changed, this));
        } else {
            const auto level = nextLevel(command, input, qRound(double(pa_cvolume_max(&channels)) * 100 / PA_VOLUME_NORM));
            pa_cvolume_scale(&channels, pa_volume_t(PA_VOLUME_NORM * level / 100));
            operation(pa_context_set_sink_volume_by_name(context, current.toUtf8().constData(), &channels, changed, this));
        }
    }
};
#else
struct Audio::Backend {
    void request(const QString &, const QJsonValue &, const Result &result) { result(noDevice()); }
};
#endif

Audio::Audio() : backend_(std::make_unique<Backend>()) {}
Audio::~Audio() = default;

void Audio::request(const QString &command, const QJsonValue &value, Result result)
{
    if (command == "status" || command == "mute" || command == "up" || command == "down" ||
        (command == "level" && value.isDouble()) ||
#ifdef Q_OS_LINUX
        command == "monitor" ||
#endif
        (command == "select" && value.isString() && value.toString().size() < 256)) {
        backend_->request(command, value, std::move(result));
    } else {
        result(failure(QStringLiteral("Неизвестная команда управления звуком")));
    }
}
