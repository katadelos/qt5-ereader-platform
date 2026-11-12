#include "touchdevice.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <cerrno>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>

namespace {
constexpr int InputBatchSize = 32;
constexpr int DeviceNameSize = 256;
constexpr int MaximumSlots = 32;
constexpr int ZForceSensorWidth = 1200;
constexpr int ZForceSensorHeight = 1600;

bool configureProtocol(
    int descriptor, bool singleTouch, bool hasSlots, bool typeA, TouchCapabilities& capabilities) {
    input_absinfo absInfo = {};
    if (hasSlots) {
        if (ioctl(descriptor, EVIOCGABS(TouchEvent::Slot), &absInfo) < 0 || absInfo.minimum != 0 ||
            absInfo.maximum < 0 || absInfo.maximum >= MaximumSlots) {
            return false;
        }
        capabilities.slotCount = absInfo.maximum + 1;
    } else if (!singleTouch && !typeA) {
        return false;
    }

    if (singleTouch) {
        capabilities.protocol = TouchProtocol::Single;
    } else if (typeA) {
        capabilities.protocol = TouchProtocol::TypeA;
    } else {
        capabilities.protocol = TouchProtocol::TypeB;
    }
    return true;
}

}

QStringList TouchDevice::candidates() {
    const QDir inputDirectory(QStringLiteral("/sys/class/input"));
    QStringList preferred;
    QStringList remaining;
    for (const QString& event :
         inputDirectory.entryList({QStringLiteral("event*")}, QDir::Dirs, QDir::Name)) {
        const QString device = QStringLiteral("/dev/input/") + event;
        QFile nameFile(inputDirectory.filePath(event + QStringLiteral("/device/name")));
        const QString name = nameFile.open(QIODevice::ReadOnly)
                                 ? QString::fromLocal8Bit(nameFile.readAll()).trimmed().toLower()
                                 : QString();
        if (name.contains(QStringLiteral("cypress")) || name.contains(QStringLiteral("cyttsp")) ||
            name.contains(QStringLiteral("truetouch")) ||
            name.contains(QStringLiteral("elan touchscreen")) ||
            name.contains(QStringLiteral("zforce")) || name.contains(QStringLiteral("neonode"))) {
            preferred.append(device);
        } else {
            remaining.append(device);
        }
    }
    preferred.append(remaining);
    return preferred;
}

bool TouchDevice::open(const QString& device, const QSize& screenSize, bool swapXY) {
    if (device.isEmpty()) {
        return false;
    }

    FileDescriptor descriptor(
        ::open(device.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    const int deviceFd = descriptor.get();
    TouchCapabilities capabilities;
    if (deviceFd < 0) {
        return false;
    }

    // Older Touch/Glo panels report ABS_X/Y and BTN_TOUCH. Select their
    // actual event protocol from capabilities, not from successful axis
    // ioctls: Kobo kernels also return zeroed limits for unsupported axes.
    constexpr unsigned bitsPerWord = sizeof(unsigned long) * 8;
    unsigned long absBits[(ABS_CNT + bitsPerWord - 1) / bitsPerWord] = {};
    if (ioctl(deviceFd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) < 0) {
        return false;
    }
    const auto supportsAxis = [&absBits](unsigned axis) {
        return ((absBits[axis / bitsPerWord] >> (axis % bitsPerWord)) & 1UL) != 0;
    };
    unsigned long keyBits[(KEY_CNT + bitsPerWord - 1) / bitsPerWord] = {};
    capabilities.hasTouchKey =
        ioctl(deviceFd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) >= 0 &&
        (((keyBits[BTN_TOUCH / bitsPerWord] >> (BTN_TOUCH % bitsPerWord)) & 1UL) != 0U);
    const bool singleTouch = !supportsAxis(ABS_MT_POSITION_X) || !supportsAxis(ABS_MT_POSITION_Y);
    if (singleTouch &&
        (!supportsAxis(ABS_X) || !supportsAxis(ABS_Y) || !capabilities.hasTouchKey)) {
        return false;
    }

    input_absinfo xInfo = {};
    input_absinfo yInfo = {};
    // Older Linux headers do not parenthesize EVIOCGABS's argument. Pass a
    // single identifier so their `0x40 + abs` cannot capture the conditional.
    const unsigned xAxis = singleTouch ? ABS_X : ABS_MT_POSITION_X;
    const unsigned yAxis = singleTouch ? ABS_Y : ABS_MT_POSITION_Y;
    const unsigned pressureAxis = singleTouch ? ABS_PRESSURE : TouchEvent::Pressure;
    if (ioctl(deviceFd, EVIOCGABS(xAxis), &xInfo) < 0 ||
        ioctl(deviceFd, EVIOCGABS(yAxis), &yInfo) < 0 || xInfo.maximum <= xInfo.minimum ||
        yInfo.maximum <= yInfo.minimum) {
        return false;
    }
    capabilities.x.minimum = xInfo.minimum;
    capabilities.x.maximum = xInfo.maximum;
    capabilities.y.minimum = yInfo.minimum;
    capabilities.y.maximum = yInfo.maximum;

    char name[DeviceNameSize] = {};
    ioctl(deviceFd, EVIOCGNAME(sizeof(name)), name);
    const QByteArray inputName(name);
    capabilities.releaseAtLastPosition = singleTouch && inputName == "zForce-ir-touch";
    // Netronix's driver advertises sensor limits but sends rotated panel
    // pixels in customer-UI mode. Match its known broken range, then use
    // the framebuffer geometry in raw-axis order for every panel size.
    if (inputName == "zForce-ir-touch" && capabilities.x.minimum == 0 &&
        capabilities.x.maximum == ZForceSensorWidth && capabilities.y.minimum == 0 &&
        capabilities.y.maximum == ZForceSensorHeight) {
        capabilities.x.maximum = (swapXY ? screenSize.height() : screenSize.width()) - 1;
        capabilities.y.maximum = (swapXY ? screenSize.width() : screenSize.height()) - 1;
    }

    input_absinfo absInfo = {};
    if (ioctl(deviceFd, EVIOCGABS(pressureAxis), &absInfo) == 0) {
        capabilities.pressure.minimum = absInfo.minimum;
        capabilities.pressure.maximum = absInfo.maximum;
    }
    // EVIOCGABS also succeeds for unsupported axes on Kobo kernels, returning
    // zeroed limits. Use the capability bitmap to distinguish real slots from
    // type A contact snapshots (cyttsp5 repeats tracking IDs every frame).
    const bool hasSlots = supportsAxis(TouchEvent::Slot);
    const bool typeA = !singleTouch && !hasSlots && supportsAxis(ABS_MT_TRACKING_ID);
    if (!configureProtocol(deviceFd, singleTouch, hasSlots, typeA, capabilities)) {
        return false;
    }

    // Nickel keeps its wireless event loop alive while k2 owns the display.
    // Keep our taps out of its input queue; closing our fd releases the grab.
    if (ioctl(deviceFd, EVIOCGRAB, 1) < 0) {
        qWarning("ereader.touch: could not acquire exclusive touchscreen input");
        return false;
    }

    m_fd.reset(descriptor.release());
    m_capabilities = capabilities;
    m_slotStateReadable = true;
    return true;
}

bool TouchDevice::setGrabbed(bool grabbed) const {
    return ioctl(fd(), EVIOCGRAB, grabbed ? 1 : 0) == 0;
}

int TouchDevice::readEvents(input_event* events, int capacity) const {
    ssize_t bytes;
    do {
        bytes = ::read(fd(), events, capacity * sizeof(input_event));
    } while (bytes < 0 && errno == EINTR);
    if (bytes < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
    }
    if (bytes == 0) {
        return -ENODEV;
    }
    if (bytes % sizeof(input_event) != 0) {
        return -EIO;
    }
    return static_cast<int>(bytes / sizeof(input_event));
}

bool TouchDevice::discardEvents() const {
    input_event events[InputBatchSize];
    int count;
    do {
        count = readEvents(events, InputBatchSize);
    } while (count > 0);
    return count == 0;
}

bool TouchDevice::readTouchKey(bool& pressed) const {
    constexpr unsigned bitsPerWord = sizeof(unsigned long) * 8;
    unsigned long keys[(KEY_CNT + bitsPerWord - 1) / bitsPerWord] = {};
    if (ioctl(fd(), EVIOCGKEY(sizeof(keys)), keys) < 0) {
        return false;
    }
    pressed = (((keys[BTN_TOUCH / bitsPerWord] >> (BTN_TOUCH % bitsPerWord)) & 1UL) != 0U);
    return true;
}

bool TouchDevice::synchronize(TouchSnapshot& snapshot) {
    snapshot = {};
    snapshot.contacts.resize(
        m_capabilities.protocol == TouchProtocol::TypeA ? 0 : m_capabilities.slotCount);
    switch (m_capabilities.protocol) {
    case TouchProtocol::Single:
        return synchronizeSingleTouch(snapshot);
    case TouchProtocol::TypeB:
        return synchronizeSlots(snapshot);
    case TouchProtocol::TypeA:
        // Type A supplies complete frames rather than persistent slots.
        // When BTN_TOUCH is available, suppress a gesture held at activation.
        if (m_capabilities.hasTouchKey) {
            if (!readTouchKey(snapshot.touchKeyDown)) {
                return false;
            }
            snapshot.discardUntilRelease = snapshot.touchKeyDown;
        }
        return true;
    }
    return false;
}

bool TouchDevice::synchronizeSingleTouch(TouchSnapshot& snapshot) {
    input_absinfo xInfo = {};
    input_absinfo yInfo = {};
    bool pressed = false;
    if (ioctl(fd(), EVIOCGABS(ABS_X), &xInfo) < 0 || ioctl(fd(), EVIOCGABS(ABS_Y), &yInfo) < 0 ||
        !readTouchKey(pressed)) {
        qWarning("ereader.touch: could not synchronize absolute touchscreen state");
        return false;
    }
    TouchContact& contact = snapshot.contacts[0];
    contact = TouchContact();
    contact.x = xInfo.value;
    contact.y = yInfo.value;
    input_absinfo pressure = {};
    if (m_capabilities.pressure.hasRange() &&
        ioctl(fd(), EVIOCGABS(ABS_PRESSURE), &pressure) == 0) {
        contact.pressure = pressure.value;
    }
    contact.active = pressed;
    contact.trackingId = pressed ? 0 : -1;
    contact.state = pressed ? Qt::TouchPointStationary : Qt::TouchPointReleased;
    // A gesture already in progress belongs to the preceding screen/session.
    snapshot.discardUntilRelease = pressed;
    return true;
}

bool TouchDevice::synchronizeSlots(TouchSnapshot& snapshot) {
    input_absinfo slot = {};
    if (ioctl(fd(), EVIOCGABS(TouchEvent::Slot), &slot) < 0 || slot.value < 0 ||
        slot.value >= m_capabilities.slotCount) {
        qWarning("ereader.touch: could not synchronize the current touchscreen slot");
        return false;
    }
    if (!m_slotStateReadable) {
        return synchronizeObservedSlots(snapshot, slot.value);
    }

    QVector<TouchContact> contacts(m_capabilities.slotCount);
    QVector<int> values(m_capabilities.slotCount + 1);
    int readError = 0;
    const auto readAxis = [this, &values, &readError](unsigned axis) {
        values[0] = static_cast<int>(axis);
        // EVIOCGMTSLOTS is missing from older build headers. Its stable ABI
        // is an axis code followed by one int for each slot, starting at 0.
        const unsigned long request = _IOC(_IOC_READ, 'E', 0x0a, values.size() * sizeof(int));
        if (ioctl(fd(), request, values.data()) < 0) {
            readError = errno;
            return false;
        }
        return true;
    };
    if (!readAxis(ABS_MT_POSITION_X)) {
        if (readError == EINVAL || readError == ENOTTY) {
            // Some slot-capable kernels expose no slot-state ioctl at all.
            // Their EVIOCGABS(MT_*) values are not the persistent slot values.
            m_slotStateReadable = false;
            qWarning("ereader.touch: kernel cannot query slot coordinates; waiting for observed "
                     "X/Y after activation or lost input");
            return synchronizeObservedSlots(snapshot, slot.value);
        }
        qWarning("ereader.touch: could not read touchscreen slot coordinates (%d)", readError);
        return false;
    }
    for (int index = 0; index < m_capabilities.slotCount; ++index) {
        contacts[index].x = values[index + 1];
        contacts[index].xKnown = true;
    }
    if (!readAxis(ABS_MT_POSITION_Y)) {
        qWarning("ereader.touch: could not read touchscreen slot coordinates (%d)", readError);
        return false;
    }
    for (int index = 0; index < m_capabilities.slotCount; ++index) {
        contacts[index].y = values[index + 1];
        contacts[index].yKnown = true;
    }
    if (m_capabilities.pressure.hasRange()) {
        if (!readAxis(TouchEvent::Pressure)) {
            qWarning("ereader.touch: could not read touchscreen slot pressure (%d)", readError);
            return false;
        }
        for (int index = 0; index < m_capabilities.slotCount; ++index) {
            contacts[index].pressure = values[index + 1];
        }
    }
    if (!readAxis(ABS_MT_TRACKING_ID)) {
        qWarning("ereader.touch: could not read touchscreen slot tracking IDs (%d)", readError);
        return false;
    }
    bool pressed = false;
    for (int index = 0; index < m_capabilities.slotCount; ++index) {
        TouchContact& contact = contacts[index];
        contact.trackingId = values[index + 1];
        contact.active = contact.trackingId >= 0;
        contact.state = contact.active ? Qt::TouchPointStationary : Qt::TouchPointReleased;
        pressed |= contact.active;
    }
    snapshot.contacts = contacts;
    snapshot.currentSlot = slot.value;
    snapshot.touchKeyDown = false;
    // Contacts held before activation or during lost input belong to the
    // previous gesture. Track their releases without sending taps to Qt.
    snapshot.discardUntilRelease = pressed;
    return true;
}

bool TouchDevice::synchronizeObservedSlots(TouchSnapshot& snapshot, int currentSlot) {
    if (!m_capabilities.hasTouchKey || !readTouchKey(snapshot.touchKeyDown)) {
        qWarning(
            "ereader.touch: touchscreen exposes neither slot state nor readable contact state");
        return false;
    }
    for (TouchContact& contact : snapshot.contacts) {
        contact = TouchContact();
    }
    snapshot.currentSlot = currentSlot;
    snapshot.discardUntilRelease = snapshot.touchKeyDown;
    // Unknown coordinates remain unknown until real axis events arrive.
    // In particular, a repeated tap must never be synthesized at (0, 0).
    return true;
}
