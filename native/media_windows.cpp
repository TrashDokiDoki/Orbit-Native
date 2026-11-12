#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Media.Control.h>
#include "media.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <chrono>
#include <vector>
#include <type_traits>

namespace {
using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;
using Session = GlobalSystemMediaTransportControlsSession;
using Manager = GlobalSystemMediaTransportControlsSessionManager;

template <typename T>
T result(const Windows::Foundation::IAsyncOperation<T> &operation)
{
    if (operation.wait_for(std::chrono::milliseconds(1500)) != Windows::Foundation::AsyncStatus::Completed) {
        operation.Cancel();
        if constexpr (std::is_arithmetic_v<T>) return T{};
        else return T{nullptr};
    }
    return operation.GetResults();
}

QString source(const Session &session)
{
    const auto name = session.SourceAppUserModelId();
    return QString::fromWCharArray(name.c_str(), name.size());
}

bool playing(const Session &session)
{
    const auto info = session.GetPlaybackInfo();
    return info && info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
}

QString artwork(const IRandomAccessStreamReference &reference)
{
    if (!reference) return {};
    const auto stream = result(reference.OpenReadAsync());
    if (!stream || !stream.Size() || stream.Size() > 8 * 1024 * 1024) return {};
    DataReader reader(stream.GetInputStreamAt(0));
    const auto count = result(reader.LoadAsync(static_cast<uint32_t>(stream.Size())));
    if (!count) return {};
    std::vector<uint8_t> bytes(count);
    reader.ReadBytes(bytes);
    QByteArray data(reinterpret_cast<const char *>(bytes.data()), count);
    QBuffer input(&data);
    QImageReader decoder(&input);
    const auto size = decoder.size();
    if (!size.isValid() || size.width() > 4096 || size.height() > 4096) return {};
    decoder.setScaledSize(size.scaled(128, 128, Qt::KeepAspectRatio));
    const auto image = decoder.read();
    QByteArray png;
    QBuffer output(&png);
    output.open(QIODevice::WriteOnly);
    if (!image.save(&output, "PNG")) return {};
    return "data:image/png;base64," + QString::fromLatin1(png.toBase64());
}

struct Apartment {
    Apartment() { init_apartment(apartment_type::multi_threaded); }
    ~Apartment() { uninit_apartment(); }
};

struct MediaRuntime {
    Apartment apartment;
    Manager manager{nullptr};
};
}

Track readWindowsMedia(const QString &preferred, const Track &previous, int command)
{
    Track track;
    try {
        thread_local MediaRuntime runtime;
        auto &manager = runtime.manager;
        if (!manager) manager = result(Manager::RequestAsync());
        if (!manager) return track;
        auto selected = manager.GetCurrentSession();
        Session active{nullptr}, preferredSession{nullptr};
        for (const auto &session : manager.GetSessions()) {
            const auto name = source(session);
            if (!track.sources.contains(name)) track.sources.append(name);
            if (!preferredSession && name == preferred) preferredSession = session;
            if (!active && playing(session)) active = session;
        }
        if (preferredSession) selected = preferredSession;
        else if (active && (!selected || !playing(selected))) selected = active;
        if (!selected) return track;
        track.source = source(selected);
        if (command == Media::Previous) result(selected.TrySkipPreviousAsync());
        else if (command == Media::Next) result(selected.TrySkipNextAsync());
        else if (command == Media::PlayPause) result(selected.TryTogglePlayPauseAsync());
        track.playing = playing(selected);
        if (const auto timeline = selected.GetTimelineProperties()) {
            track.position = std::chrono::duration<double>(timeline.Position() - timeline.StartTime()).count();
            track.duration = std::chrono::duration<double>(timeline.EndTime() - timeline.StartTime()).count();
            constexpr qint64 unixEpochFrom1601Ms = 11644473600000LL;
            const auto updated = timeline.LastUpdatedTime().time_since_epoch();
            track.updatedAt = std::chrono::duration_cast<std::chrono::milliseconds>(updated).count() - unixEpochFrom1601Ms;
        }
        if (const auto properties = result(selected.TryGetMediaPropertiesAsync())) {
            track.title = QString::fromWCharArray(properties.Title().c_str());
            track.artist = QString::fromWCharArray(properties.Artist().c_str());
            track.album = QString::fromWCharArray(properties.AlbumTitle().c_str());
            if (track.source == previous.source && track.title == previous.title &&
                track.artist == previous.artist && track.album == previous.album && !previous.coverData.isEmpty())
                track.coverData = previous.coverData;
            else
                track.coverData = artwork(properties.Thumbnail());
        }
    } catch (const winrt::hresult_error &) {
        // A player can close while the session is being read.
    }
    return track;
}
