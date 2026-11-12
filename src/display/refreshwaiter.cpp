#include "refreshwaiter.h"

#include "fbink.h"

#include <cerrno>
#include <sys/ioctl.h>

namespace {
// eink_fb's FBIO_WAITFORVSYNC has no payload, unlike the generic fbdev ioctl.
constexpr unsigned long EinkFbWaitForVsync = _IO('F', 0x20);
}

void RefreshWaiter::run() {
    if (m_kind == Kind::LegacyCompletion) {
        result = waitForLegacyUpdate(m_fd);
        return;
    }
    for (uint32_t marker : m_markers) {
        const int waitResult = m_kind == Kind::Completion ? fbink_wait_for_complete(m_fd, marker)
                                                          : fbink_wait_for_submission(m_fd, marker);
        if (waitResult < 0) {
            result = waitResult;
        }
    }
}

int RefreshWaiter::waitForLegacyUpdate(int descriptor) {
    int result;
    do {
        result = ioctl(descriptor, EinkFbWaitForVsync, 0);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? -errno : 0;
}
