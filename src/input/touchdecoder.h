#pragma once

#include "touchtypes.h"

#include <QPoint>

struct input_event;

// Converts one evdev protocol into complete contact frames. Device I/O,
// recovery and coordinate transformation belong to the caller.
class TouchDecoder {
public:
    void configure(const TouchCapabilities& capabilities, const TouchSnapshot& snapshot);
    void reset(const TouchSnapshot& snapshot = {});

    void discardUntilRelease() {
        m_discardUntilRelease = true;
    }

    bool handleEvent(const input_event& event);
    QVector<TouchContact> takeFrame();

private:
    void handleSingleTouchEvent(const input_event& event);
    void handleSlotEvent(const input_event& event);
    void handleSlotAbsoluteEvent(const input_event& event);
    void handleTypeAEvent(const input_event& event);
    void finishTypeAContact();
    void finishTypeAFrame();

    TouchCapabilities m_capabilities;
    QVector<TouchContact> m_contacts;
    bool m_frameDirty = false;
    bool m_discardUntilRelease = false;
    bool m_touchKeyDown = false;

    struct SingleState {
        QPoint position;
        QPoint lastPosition;
    } m_single;

    struct SlotState {
        int current = 0;
    } m_slots;

    struct TypeAState {
        TouchContact pending;
        QVector<TouchContact> contacts;
    } m_typeA;
};
