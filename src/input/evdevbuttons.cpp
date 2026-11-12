#include "evdevbuttons.h"

#include <QDir>
#include <QGuiApplication>
#include <QSocketNotifier>
#include <QVariant>
#include <QWindow>
#include <cerrno>
#include <fcntl.h>
#include <linux/input.h>
#include <qpa/qwindowsysteminterface.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr int InputBatchSize = 32;
// KOReader's Kobo mapping: top and bottom with the Forma buttons on the right.
constexpr unsigned PageBack = 193;
constexpr unsigned PageForward = 194;
// The numeric event is stable; older Kobo kernel headers omit its name.
constexpr unsigned DroppedReport = 3;
}

EvdevButtons::EvdevButtons(const FBInkState& state, QObject* parent) : QObject(parent) {
    setScreenState(state);
    const QDir inputDirectory(QStringLiteral("/dev/input"));
    for (const QString& name :
         inputDirectory.entryList({QStringLiteral("event*")}, QDir::System, QDir::Name)) {
        const QString path = inputDirectory.filePath(name);
        if (openDevice(path)) {
            qDebug("ereader.buttons: using %s", qPrintable(path));
            m_notifier = new QSocketNotifier(m_fd.get(), QSocketNotifier::Read, this);
            connect(m_notifier, &QSocketNotifier::activated, this, [this](int) { readEvents(); });
            return;
        }
    }
}

EvdevButtons::~EvdevButtons() {
    delete m_notifier;
}

bool EvdevButtons::openDevice(const QString& path) {
    FileDescriptor descriptor(
        open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    const int deviceFd = descriptor.get();
    if (deviceFd < 0) {
        return false;
    }

    constexpr unsigned bitsPerWord = sizeof(unsigned long) * 8;
    unsigned long keys[(KEY_CNT + bitsPerWord - 1) / bitsPerWord] = {};
    const auto hasKey = [&keys](unsigned key) {
        return ((keys[key / bitsPerWord] >> (key % bitsPerWord)) & 1UL) != 0;
    };
    if (ioctl(deviceFd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 || !hasKey(PageBack) ||
        !hasKey(PageForward) || hasKey(KEY_A)) {
        return false;
    }

    // gpio-keys also carries power, cover and rotation events. Nickel's bridge
    // suppresses its key handling while we own the display; do not grab this
    // entire device and deprive its power/orientation services of those events.
    m_fd.reset(descriptor.release());
    return true;
}

void EvdevButtons::setScreenState(const FBInkState& state) {
    m_rotation = state.rotation_map[state.current_rota & 3];
}

void EvdevButtons::readEvents() {
    emit aboutToReadButtons();
    input_event events[InputBatchSize];
    const ssize_t count = read(m_fd.get(), events, sizeof(events));
    if (count <= 0 || count % sizeof(input_event) != 0) {
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        }
        m_notifier->setEnabled(false);
        m_fd.reset();
        qWarning("ereader.buttons: input device disconnected or unreadable");
        return;
    }
    for (ssize_t index = 0; index < count / static_cast<ssize_t>(sizeof(input_event)); ++index) {
        handleEvent(events[index]);
    }
}

void EvdevButtons::setSuspended(bool suspended) {
    if (m_fd.get() < 0) {
        return;
    }
    m_notifier->setEnabled(!suspended);
    input_event events[InputBatchSize];
    while (read(m_fd.get(), events, sizeof(events)) > 0) {
    }
    m_discardUntilReport = false;
}

void EvdevButtons::handleEvent(const input_event& event) {
    if (event.type == EV_SYN && event.code == DroppedReport) {
        m_discardUntilReport = true;
        return;
    }
    if (m_discardUntilReport) {
        if (event.type == EV_SYN && event.code == SYN_REPORT) {
            m_discardUntilReport = false;
        }
        return;
    }
    // One page per physical press. The firmware's 80 ms repeat would queue
    // several e-ink page turns before the user could see or stop them.
    if (event.type != EV_KEY || event.value != 1 ||
        (event.code != PageBack && event.code != PageForward)) {
        return;
    }

    QWindow* window = QGuiApplication::focusWindow();
    if ((window == nullptr) || !window->isVisible()) {
        return;
    }

    // A QML app may rotate its content independently of the framebuffer.
    // Clockwise content rotation means counter-clockwise physical rotation.
    const int contentRotation = window->property("ereaderContentRotation").toInt() / 90;
    const int rotation = (m_rotation - contentRotation + 4) & 3;
    bool forward = event.code == PageForward;
    // Match KOReader: lower/right button advances in every orientation.
    if (rotation == 1 || rotation == 2) {
        forward = !forward;
    }
    const int key = forward ? Qt::Key_PageDown : Qt::Key_PageUp;
    QWindowSystemInterface::handleExtendedKeyEvent(
        window, QEvent::KeyPress, key, Qt::NoModifier, event.code, 0, 0);
    QWindowSystemInterface::handleExtendedKeyEvent(
        window, QEvent::KeyRelease, key, Qt::NoModifier, event.code, 0, 0);
}
