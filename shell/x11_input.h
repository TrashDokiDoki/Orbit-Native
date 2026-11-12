#pragma once

#include <QKeySequence>
#include <QString>
#include <functional>
#include <memory>
#include <utility>

class X11Input {
public:
    X11Input();
    ~X11Input();
    bool add(QKeyCombination combination, std::function<void()> action);
    void clear();
    std::pair<int, QString> layout() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
