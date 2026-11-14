#pragma once

#include <QObject>

class KdeLayoutListener final : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

public Q_SLOTS:
    void onLayoutChanged(uint) { Q_EMIT changed(); }
    void onLayoutsReconfigured() { Q_EMIT changed(); }

Q_SIGNALS:
    void changed();
};
