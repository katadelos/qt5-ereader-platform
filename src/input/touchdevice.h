#pragma once

#include "filedescriptor.h"
#include "touchtypes.h"

#include <QSize>
#include <QStringList>

struct input_event;

// Owns the input descriptor and queries kernel state. No Qt event delivery.
class TouchDevice {
public:
    static QStringList candidates();
    bool open(const QString& device, const QSize& screenSize, bool swapXY);
    bool synchronize(TouchSnapshot& snapshot);
    bool setGrabbed(bool grabbed) const;
    // Returns event count, zero when drained, or a negative errno on failure.
    int readEvents(input_event* events, int capacity) const;
    bool discardEvents() const;

    void close() {
        m_fd.reset();
    }

    int fd() const {
        return m_fd.get();
    }

    const TouchCapabilities& capabilities() const {
        return m_capabilities;
    }

private:
    bool synchronizeSingleTouch(TouchSnapshot& snapshot);
    bool synchronizeSlots(TouchSnapshot& snapshot);
    bool synchronizeObservedSlots(TouchSnapshot& snapshot, int currentSlot);
    bool readTouchKey(bool& pressed) const;

    FileDescriptor m_fd;
    TouchCapabilities m_capabilities;
    bool m_slotStateReadable = true;
};
