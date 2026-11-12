#pragma once
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <functional>
#ifdef ORBIT_WINDOWS_MEDIA
#include <QThreadPool>
#endif

struct Track {
    QString title;
    QString artist;
    QUrl cover;
    bool playing = false;
    QString album, source, coverData;
    QStringList sources;
    double position = -1;
    double duration = 0;
    qint64 updatedAt = 0;
};

class Media final : public QObject {
public:
    enum Command { Previous, PlayPause, Next };
    explicit Media(QObject *parent = nullptr);
    void refresh();
    void send(Command command);
    void select(const QString &source);
    std::function<void(Track)> changed;

private:
#ifdef ORBIT_WINDOWS_MEDIA
    void windowsRequest(int command);
    QString selection_;
    Track previous_;
    bool pending_ = false;
    int queuedCommand_ = -1;
    QThreadPool worker_;
#endif
#ifdef Q_OS_LINUX
    QString selection_;
    QStringList sources_;
    void inspect(QStringList players, Track fallback = {});
    QString player_;
    QString preferred_;
    bool pending_ = false;
#endif
};
