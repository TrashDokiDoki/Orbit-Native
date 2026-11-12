#pragma once

#include <QKeySequence>
#include <QString>
#include <functional>
#include <memory>
#include <utility>

struct X11Foreground {
    unsigned long window = 0;
    unsigned long pid = 0;
    bool fullscreen = false;
};

class X11Input {
public:
    X11Input();
    ~X11Input();
    bool add(QKeyCombination combination, std::function<void()> action);
    void clear();
    std::pair<int, QString> layout() const;
    X11Foreground foreground() const;
    bool activate(unsigned long window) const;
    bool paste(unsigned long window) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
