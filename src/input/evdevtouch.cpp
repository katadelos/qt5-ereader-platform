#include "evdevtouch.h"

#include <QDebug>
#include <QSocketNotifier>
#include <QTouchDevice>
#include <cstring>
#include <linux/fb.h>
#include <linux/input.h>
#include <qpa/qwindowsysteminterface.h>

namespace {
constexpr int InputBatchSize = 32;
constexpr int TouchAreaSize = 8;
}

EvdevTouch::EvdevTouch(const TouchOptions& options, const FBInkState& screenState, QObject* parent)
    : QObject(parent), m_options(options) {
    setScreenState(screenState);
    const QStringList candidates =
        options.device.isEmpty() ? TouchDevice::candidates() : QStringList{options.device};
    QString device;
    for (const QString& candidate : candidates) {
        if (m_device.open(candidate, m_screenSize, m_swapXY)) {
            device = candidate;
            break;
        }
    }
    if (m_device.fd() < 0) {
        qWarning("ereader.touch: touchscreen input device is not available");
        return;
    }

    TouchSnapshot snapshot;
    if (!m_device.synchronize(snapshot)) {
        m_device.close();
        return;
    }
    const TouchCapabilities& capabilities = m_device.capabilities();
    m_decoder.configure(capabilities, snapshot);
    if (options.debug) {
        qDebug(
            "ereader.touch: using %s protocol=%d slots=%d x=%d..%d y=%d..%d pressure=%d..%d",
            qPrintable(device),
            int(capabilities.protocol),
            capabilities.slotCount,
            capabilities.x.minimum,
            capabilities.x.maximum,
            capabilities.y.minimum,
            capabilities.y.maximum,
            capabilities.pressure.minimum,
            capabilities.pressure.maximum);
    }

    m_touchDevice.reset(new QTouchDevice);
    m_touchDevice->setName(QStringLiteral("E-paper touchscreen"));
    m_touchDevice->setType(QTouchDevice::TouchScreen);
    m_touchDevice->setCapabilities(QTouchDevice::Position | QTouchDevice::Area);
    if (capabilities.pressure.hasRange()) {
        m_touchDevice->setCapabilities(m_touchDevice->capabilities() | QTouchDevice::Pressure);
    }
    QWindowSystemInterface::registerTouchDevice(m_touchDevice.data());

    m_notifier.reset(new QSocketNotifier(m_device.fd(), QSocketNotifier::Read, this));
    m_lifecycle = Lifecycle::Reading;
    connect(m_notifier.data(), &QSocketNotifier::activated, this, [this](int) { readEvents(); });
}

EvdevTouch::~EvdevTouch() {
    if ((m_touchDevice != nullptr) &&
        QWindowSystemInterface::isTouchDeviceRegistered(m_touchDevice.data())) {
        QWindowSystemInterface::unregisterTouchDevice(m_touchDevice.data());
    }
}

void EvdevTouch::setScreenState(const FBInkState& state) {
    bool swap = state.touch_swap_axes;
    bool mirrorX = state.touch_mirror_x;
    bool mirrorY = state.touch_mirror_y;
    if (m_options.transformExplicit) {
        // Explicit flags describe raw axes before swapping. FBInk's device
        // quirks instead describe mirroring after the swap.
        swap = m_options.swapXY;
        const bool invertX = m_options.invertX;
        const bool invertY = m_options.invertY;
        mirrorX = swap ? invertY : invertX;
        mirrorY = swap ? invertX : invertY;
    }

    // Use FBInk's per-model map, including Forma's mirrored native rotations.
    // These flags follow FBInk's finger_trace coordinate translation.
    switch (state.rotation_map[state.current_rota & 3]) {
    case FB_ROTATE_CW:
        swap = !swap;
        mirrorY = !mirrorY;
        break;
    case FB_ROTATE_UD:
        mirrorX = !mirrorX;
        mirrorY = !mirrorY;
        break;
    case FB_ROTATE_CCW:
        swap = !swap;
        mirrorX = !mirrorX;
        break;
    default:
        break;
    }
    const QSize size(static_cast<int>(state.screen_width), static_cast<int>(state.screen_height));
    if (size == m_screenSize && swap == m_swapXY && mirrorX == m_mirrorX && mirrorY == m_mirrorY) {
        return;
    }

    // A drag must not turn into a tap on a different control after rotating.
    // Consume its remaining events until all fingers have been lifted.
    if (m_touchActive) {
        m_decoder.discardUntilRelease();
        cancelTouch();
    }
    m_screenSize = size;
    m_swapXY = swap;
    m_mirrorX = mirrorX;
    m_mirrorY = mirrorY;
    if (m_options.debug) {
        qDebug() << "ereader.touch: framebuffer transform" << size << state.current_rota << "swap"
                 << swap << "mirror" << mirrorX << mirrorY;
    }
}

void EvdevTouch::cancelTouch() {
    if (!m_touchActive) {
        return;
    }
    QWindowSystemInterface::handleTouchCancelEvent(nullptr, m_touchDevice.data());
    m_touchActive = false;
    emit touchActiveChanged(false);
}

void EvdevTouch::stopInput() {
    m_notifier->setEnabled(false);
    cancelTouch();
    m_decoder.reset();
    m_device.close();
    m_lifecycle = Lifecycle::Stopped;
}

bool EvdevTouch::synchronize() {
    TouchSnapshot snapshot;
    if (!m_device.synchronize(snapshot)) {
        return false;
    }
    m_decoder.reset(snapshot);
    return true;
}

void EvdevTouch::readEvents() {
    emit aboutToReadTouch();
    input_event events[InputBatchSize];
    while (reading()) {
        const int count = m_device.readEvents(events, InputBatchSize);
        if (count == 0) {
            return;
        }
        if (count < 0) {
            qWarning("ereader.touch: input read failed: %s", std::strerror(-count));
            stopInput();
            return;
        }
        for (int index = 0; index < count && reading(); ++index) {
            handleEvent(events[index]);
        }
    }
}

bool EvdevTouch::setSuspended(bool suspended) {
    if (m_lifecycle == Lifecycle::Stopped || (m_lifecycle == Lifecycle::Suspended) == suspended) {
        return true;
    }
    if (!m_device.setGrabbed(!suspended)) {
        return false;
    }
    m_notifier->setEnabled(false);
    cancelTouch();
    m_decoder.reset();
    m_lifecycle = suspended ? Lifecycle::Suspended : Lifecycle::Reading;
    // Drop queued input and the PIN/unlock gesture before rejoining the app.
    if (!m_device.discardEvents() || (!suspended && !synchronize())) {
        stopInput();
        return false;
    }
    m_notifier->setEnabled(!suspended);
    return true;
}

void EvdevTouch::handleEvent(const input_event& event) {
    if (event.type == EV_SYN && event.code == TouchEvent::Dropped) {
        cancelTouch();
        m_decoder.reset();
        m_lifecycle = Lifecycle::Recovering;
        return;
    }
    if (m_lifecycle == Lifecycle::Recovering) {
        // All protocols discard the incomplete frame, including Type A.
        if (event.type == EV_SYN && event.code == SYN_REPORT) {
            m_lifecycle = Lifecycle::Reading;
            if (!synchronize()) {
                qWarning("ereader.touch: could not recover input after dropped events");
                stopInput();
            }
        }
        return;
    }
    if (m_decoder.handleEvent(event)) {
        reportFrame(m_decoder.takeFrame());
    }
}

QPointF EvdevTouch::normalizedPosition(const TouchContact& contact) const {
    const TouchCapabilities& capabilities = m_device.capabilities();
    qreal normalizedX = qBound<qreal>(
        0,
        (contact.x - capabilities.x.minimum) /
            qreal(capabilities.x.maximum - capabilities.x.minimum),
        1);
    qreal normalizedY = qBound<qreal>(
        0,
        (contact.y - capabilities.y.minimum) /
            qreal(capabilities.y.maximum - capabilities.y.minimum),
        1);
    if (m_swapXY) {
        qSwap(normalizedX, normalizedY);
    }
    if (m_mirrorX) {
        normalizedX = 1 - normalizedX;
    }
    if (m_mirrorY) {
        normalizedY = 1 - normalizedY;
    }
    return QPointF(normalizedX, normalizedY);
}

void EvdevTouch::reportFrame(const QVector<TouchContact>& contacts) {
    if (contacts.isEmpty()) {
        return;
    }
    const TouchAxis& pressure = m_device.capabilities().pressure;
    QList<QWindowSystemInterface::TouchPoint> points;
    bool touchActive = false;
    for (const TouchContact& contact : contacts) {
        const QPointF normal = normalizedPosition(contact);
        QWindowSystemInterface::TouchPoint point;
        point.id = contact.trackingId;
        point.state = contact.state;
        point.normalPosition = normal;
        point.area = QRectF(0, 0, TouchAreaSize, TouchAreaSize);
        point.area.moveCenter(QPointF(
            normal.x() * (m_screenSize.width() - 1), normal.y() * (m_screenSize.height() - 1)));
        if (contact.state == Qt::TouchPointReleased) {
            point.pressure = 0;
        } else if (pressure.hasRange()) {
            point.pressure =
                (contact.pressure - pressure.minimum) / qreal(pressure.maximum - pressure.minimum);
        } else {
            point.pressure = 1;
        }
        points.append(point);
        touchActive |= contact.state != Qt::TouchPointReleased;
    }
    if (touchActive != m_touchActive) {
        m_touchActive = touchActive;
        emit touchActiveChanged(touchActive);
    }
    if (m_options.debug) {
        qDebug("ereader.touch: frame points=%d", points.size());
        for (const QWindowSystemInterface::TouchPoint& point : points) {
            qDebug(
                "ereader.touch: point id=%d state=0x%x normal=%.3f,%.3f",
                point.id,
                int(point.state),
                point.normalPosition.x(),
                point.normalPosition.y());
        }
    }
    QWindowSystemInterface::handleTouchEvent(nullptr, m_touchDevice.data(), points);
}
