#pragma once

#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QString>
#include <functional>

struct PortalScreenStream {
    QDBusUnixFileDescriptor remote;
    uint node = 0;
};

class PortalScreencast final : public QObject {
    Q_OBJECT

public:
    using Result = std::function<void(PortalScreenStream, QString)>;

    explicit PortalScreencast(QObject *parent = nullptr);
    ~PortalScreencast() override;

    void start(Result result);

Q_SIGNALS:
    void closed();

private Q_SLOTS:
    void sessionClosed();

private:
    void selectSource();
    void openRemote(uint node);
    void finish(PortalScreenStream stream, const QString &error);
    void closeSession();

    QString session_;
    Result result_;
};
