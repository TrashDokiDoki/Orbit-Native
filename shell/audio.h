#pragma once

#include <QJsonObject>
#include <functional>
#include <memory>

class Audio {
public:
    using Result = std::function<void(QJsonObject)>;

    Audio();
    ~Audio();
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    void request(const QString &command, const QJsonValue &value, Result result);

private:
    struct Backend;
    std::unique_ptr<Backend> backend_;
};
