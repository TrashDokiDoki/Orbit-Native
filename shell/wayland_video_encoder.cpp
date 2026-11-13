#include "wayland_video_encoder.h"

#include <QFile>
#include <gst/gst.h>
#include <utility>

WaylandVideoEncoder::WaylandVideoEncoder(QObject *parent) : QObject(parent)
{
    pollTimer_.setInterval(100);
    stopTimer_.setSingleShot(true);
    stopTimer_.setInterval(8000);
    connect(&pollTimer_, &QTimer::timeout, this, &WaylandVideoEncoder::poll);
    connect(&stopTimer_, &QTimer::timeout, this, [this] {
        finish(QStringLiteral("Не удалось завершить кодирование видео"));
    });
}

WaylandVideoEncoder::~WaylandVideoEncoder()
{
    release();
}

QString WaylandVideoEncoder::start(PortalScreenStream stream, const QString &outputPath, const QString &monitor, Finished finished)
{
    if (pipeline_) return QStringLiteral("Запись уже запущена");
    if (!stream.remote.isValid() || !stream.node) return QStringLiteral("Портал не вернул видеопоток");

    GError *failure = nullptr;
    if (!gst_init_check(nullptr, nullptr, &failure)) {
        g_clear_error(&failure);
        return QStringLiteral("Не удалось запустить GStreamer");
    }
    QByteArray description =
        "pipewiresrc name=source keepalive-time=200 ! queue leaky=downstream max-size-buffers=3 max-size-bytes=0 max-size-time=0 "
                "! videoconvert ! video/x-raw,format=I420 ! vp8enc deadline=1 ! mux.video_0 "
        "webmmux name=mux ! filesink name=output";
    if (!monitor.isEmpty())
        description += " pulsesrc name=audio ! queue ! audioconvert ! audioresample ! opusenc ! mux.audio_0";
    pipeline_ = gst_parse_launch(description.constData(), &failure);
    if (!pipeline_ || failure) {
        g_clear_error(&failure);
        release();
        return QStringLiteral("Компоненты кодирования WebM недоступны");
    }
    remote_ = std::move(stream.remote);
    auto *source = gst_bin_get_by_name(GST_BIN(pipeline_), "source");
    auto *output = gst_bin_get_by_name(GST_BIN(pipeline_), "output");
    const auto target = QByteArray::number(stream.node);
    const auto path = QFile::encodeName(outputPath);
    g_object_set(source, "fd", remote_.fileDescriptor(), "path", target.constData(), nullptr);
    g_object_set(output, "location", path.constData(), nullptr);
    gst_object_unref(source);
    gst_object_unref(output);
    if (auto *audio = gst_bin_get_by_name(GST_BIN(pipeline_), "audio")) {
        const auto device = monitor.toUtf8();
        g_object_set(audio, "device", device.constData(), nullptr);
        gst_object_unref(audio);
    }

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        release();
        return QStringLiteral("Не удалось начать запись видеопотока");
    }
    finished_ = std::move(finished);
    pollTimer_.start();
    return {};
}

void WaylandVideoEncoder::stop()
{
    if (!pipeline_ || stopTimer_.isActive()) return;
    stopTimer_.start();
    if (!gst_element_send_event(pipeline_, gst_event_new_eos()))
        finish(QStringLiteral("Не удалось остановить запись видеопотока"));
}

void WaylandVideoEncoder::poll()
{
    auto *bus = gst_element_get_bus(pipeline_);
    auto *message = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    gst_object_unref(bus);
    if (!message) return;

    QString error;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *failure = nullptr;
        gst_message_parse_error(message, &failure, nullptr);
        error = QStringLiteral("Ошибка записи видеопотока");
        if (failure) error += QStringLiteral(". ") + QString::fromUtf8(failure->message);
        g_clear_error(&failure);
    }
    gst_message_unref(message);
    finish(std::move(error));
}

void WaylandVideoEncoder::finish(QString error)
{
    auto finished = std::move(finished_);
    release();
    if (finished) finished(std::move(error));
}

void WaylandVideoEncoder::release()
{
    pollTimer_.stop();
    stopTimer_.stop();
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    remote_ = {};
}
