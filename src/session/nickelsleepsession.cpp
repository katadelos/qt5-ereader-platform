#include "nickelsleepsession.h"

#include "evdevbuttons.h"
#include "evdevtouch.h"
#include "refreshscheduler.h"

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <qpa/qwindowsysteminterface.h>

namespace {
constexpr int SessionTimeoutMs = 3000;
constexpr int ActivityIntervalMs = 1000;
}

NickelSleepSession::NickelSleepSession(
    RefreshScheduler* display, EvdevTouch* touch, EvdevButtons* buttons, QObject* parent)
    : QObject(parent), m_display(display), m_touch(touch), m_buttons(buttons) {
    m_socket.connectToServer(QStringLiteral("/var/run/k2-nickel.sock"));
    if (!m_socket.waitForConnected(SessionTimeoutMs)) {
        qFatal("ereader: could not connect to the stock sleep session");
    }
    m_socket.write("{\"command\":\"watch_sleep\"}\n");
    m_socket.flush();
    QElapsedTimer deadline;
    deadline.start();
    while (!m_socket.canReadLine() && deadline.elapsed() < SessionTimeoutMs) {
        if (!m_socket.waitForReadyRead(SessionTimeoutMs - static_cast<int>(deadline.elapsed()))) {
            break;
        }
    }
    const auto reply = QJsonDocument::fromJson(m_socket.readLine()).object();
    if (!reply.value(QStringLiteral("active")).toBool()) {
        qFatal("ereader: the stock sleep session was not accepted");
    }
    connect(&m_socket, &QLocalSocket::readyRead, this, &NickelSleepSession::read);
    connect(&m_socket, &QLocalSocket::disconnected, this, [] {
        qFatal("ereader: the stock sleep session ended");
    });
    qGuiApp->installEventFilter(this);
    read();
}

NickelSleepSession::~NickelSleepSession() {
    m_socket.disconnect(this);
}

void NickelSleepSession::read() {
    while (m_socket.canReadLine()) {
        const auto message = QJsonDocument::fromJson(m_socket.readLine()).object();
        const auto event = message.value(QStringLiteral("event")).toString();
        if (event == QStringLiteral("sleep")) {
            m_sleeping = true;
            m_buttons->setSuspended(true);
            if (!m_touch->setSuspended(true) || !m_display->setSuspended(true)) {
                qFatal("ereader: could not release the display for sleep");
            }
            QWindowSystemInterface::handleApplicationStateChanged(Qt::ApplicationSuspended);
            m_socket.write("{\"command\":\"sleeping\"}\n");
            m_socket.flush();
        } else if (event == QStringLiteral("wake")) {
            if (!m_touch->setSuspended(false) || !m_display->setSuspended(false)) {
                qFatal("ereader: could not reclaim the display after sleep");
            }
            m_buttons->setSuspended(false);
            m_sleeping = false;
            m_lastActivity.invalidate();
            QWindowSystemInterface::handleApplicationStateChanged(Qt::ApplicationActive);
        }
    }
}

bool NickelSleepSession::eventFilter(QObject* /*watched*/, QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseMove:
        if (!static_cast<QMouseEvent*>(event)->buttons()) {
            return false;
        }
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::KeyPress:
        break;
    default:
        return false;
    }
    if (m_sleeping) {
        return true;
    }
    if (!event->spontaneous()) {
        return false;
    }
    // Only user input resets the native idle timer. Rendering, network work
    // and an open window do not count as activity.
    if (!m_lastActivity.isValid() || m_lastActivity.elapsed() >= ActivityIntervalMs) {
        m_socket.write("{\"command\":\"activity\"}\n");
        m_socket.flush();
        m_lastActivity.start();
    }
    return false;
}
