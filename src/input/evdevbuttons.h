#ifndef EVDEVBUTTONS_H
#define EVDEVBUTTONS_H

#include "fbink.h"
#include "filedescriptor.h"

#include <QObject>

class QSocketNotifier;
struct input_event;

class EvdevButtons : public QObject {
    // Qt declares short parameter names in this macro.
    // NOLINTNEXTLINE(readability-identifier-length)
    Q_OBJECT

public:
    explicit EvdevButtons(const FBInkState& state, QObject* parent = nullptr);
    ~EvdevButtons() override;
    void setScreenState(const FBInkState& state);
    void setSuspended(bool suspended);

signals:
    void aboutToReadButtons();

private:
    bool openDevice(const QString& path);
    void readEvents();
    void handleEvent(const input_event& event);

    FileDescriptor m_fd;
    int m_rotation = 0;
    bool m_discardUntilReport = false;
    QSocketNotifier* m_notifier = nullptr;
};

#endif // EVDEVBUTTONS_H
