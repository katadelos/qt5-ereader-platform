#pragma once

#include "fbink.h"
#include "platformarguments.h"
#include "touchdecoder.h"
#include "touchdevice.h"

#include <QObject>
#include <QScopedPointer>

class QSocketNotifier;
class QTouchDevice;

// Adapts complete evdev contact frames to Qt and coordinates input ownership.
class EvdevTouch : public QObject {
    // Qt declares short parameter names in this macro.
    // NOLINTNEXTLINE(readability-identifier-length)
    Q_OBJECT

public:
    EvdevTouch(
        const TouchOptions& options, const FBInkState& screenState, QObject* parent = nullptr);
    ~EvdevTouch() override;
    void setScreenState(const FBInkState& state);
    bool setSuspended(bool suspended);

signals:
    void aboutToReadTouch();
    void touchActiveChanged(bool active);

private:
    enum class Lifecycle : quint8 {
        Stopped,
        Reading,
        Recovering,
        Suspended
    };

    bool reading() const {
        return m_lifecycle == Lifecycle::Reading || m_lifecycle == Lifecycle::Recovering;
    }

    void readEvents();
    bool synchronize();
    void handleEvent(const input_event& event);
    void reportFrame(const QVector<TouchContact>& contacts);
    void cancelTouch();
    void stopInput();
    QPointF normalizedPosition(const TouchContact& contact) const;

    TouchOptions m_options;
    TouchDevice m_device;
    TouchDecoder m_decoder;
    QScopedPointer<QSocketNotifier> m_notifier;
    QScopedPointer<QTouchDevice> m_touchDevice;
    QSize m_screenSize;
    bool m_swapXY = false;
    bool m_mirrorX = false;
    bool m_mirrorY = false;
    bool m_touchActive = false;
    Lifecycle m_lifecycle = Lifecycle::Stopped;
};
