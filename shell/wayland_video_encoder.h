#pragma once

#include "portal_screencast.h"

#include <QTimer>
#include <functional>

struct _GstElement;

class WaylandVideoEncoder final : public QObject {
public:
    using Finished = std::function<void(QString)>;

    explicit WaylandVideoEncoder(QObject *parent = nullptr);
    ~WaylandVideoEncoder() override;

    QString start(PortalScreenStream stream, const QString &outputPath, const QString &monitor, Finished finished);
    void stop();

private:
    void poll();
    void finish(QString error);
    void release();

    _GstElement *pipeline_ = nullptr;
    QDBusUnixFileDescriptor remote_;
    QTimer pollTimer_;
    QTimer stopTimer_;
    Finished finished_;
};
