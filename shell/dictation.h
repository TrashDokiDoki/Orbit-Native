#pragma once

#include <QString>
#include <memory>

class QDialog;

class Dictation {
public:
    Dictation();
    ~Dictation();
    bool open();
    static bool test(const QString &pcmFile);

private:
    std::unique_ptr<QDialog> dialog_;
};
