#pragma once

#include <QElapsedTimer>
#include <QLocalSocket>
#include <QObject>

class RefreshScheduler;
class EvdevTouch;
class EvdevButtons;

// Coordinate framebuffer and input ownership with Nickel's stock sleep flow.
class NickelSleepSession : public QObject {
public:
    NickelSleepSession(
        RefreshScheduler* display, EvdevTouch* touch, EvdevButtons* buttons, QObject* parent);
    ~NickelSleepSession() override;
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    void read();
    RefreshScheduler* m_display;
    EvdevTouch* m_touch;
    EvdevButtons* m_buttons;
    QLocalSocket m_socket;
    QElapsedTimer m_lastActivity;
    bool m_sleeping = false;
};
