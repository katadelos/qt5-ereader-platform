#pragma once

#include <QVector>
#include <Qt>

enum class TouchProtocol : quint8 {
    Single,
    TypeA,
    TypeB
};

struct TouchAxis {
    int minimum = 0;
    int maximum = 0;

    bool hasRange() const {
        return maximum > minimum;
    }
};

struct TouchCapabilities {
    TouchProtocol protocol = TouchProtocol::Single;
    int slotCount = 1;
    TouchAxis x;
    TouchAxis y;
    TouchAxis pressure;
    bool hasTouchKey = false;
    bool releaseAtLastPosition = false;
};

struct TouchContact {
    int trackingId = -1;
    int x = 0;
    int y = 0;
    int pressure = 0;
    Qt::TouchPointState state = Qt::TouchPointReleased;
    // Includes contacts awaiting delivery of their release event.
    bool active = false;
    bool xKnown = false;
    bool yKnown = false;
};

struct TouchSnapshot {
    QVector<TouchContact> contacts;
    int currentSlot = 0;
    bool touchKeyDown = false;
    bool discardUntilRelease = false;
};

namespace TouchEvent {
// Stable Linux ABI values, also usable with the device SDK's older headers.
constexpr unsigned Slot = 0x2f;
constexpr unsigned Pressure = 0x3a;
constexpr unsigned Dropped = 3;
}
