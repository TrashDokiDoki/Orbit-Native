#pragma once
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <functional>

struct Track {
    QString title;
    QString artist;
    QUrl cover;
    bool playing = false;
};

class Media final : public QObject {
public:
    enum Command { Previous, PlayPause, Next };
    explicit Media(QObject *parent = nullptr);
    void refresh();
    void send(Command command);
    std::function<void(Track)> changed;

private:
#ifdef Q_OS_LINUX
    void inspect(QStringList players, Track fallback = {});
    QString player_;
    QString preferred_;
    bool pending_ = false;
#endif
};
