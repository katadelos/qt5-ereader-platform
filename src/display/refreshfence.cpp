#include "refreshfence.h"

RefreshFence::RefreshFence(Framebuffer& framebuffer) : m_framebuffer(framebuffer) {}

RefreshFence::~RefreshFence() {
    // The descriptor outlives this object, including after a failed wait.
    if (m_waiter) {
        collect();
    }
}

void RefreshFence::submitted(uint32_t marker, const QRect& region) {
    if (m_framebuffer.state().is_kindle_legacy) {
        m_legacyPending = true;
    }
    if (marker != 0) {
        m_markers.append(marker);
        m_sources.append({marker, region});
    }
}

bool RefreshFence::satisfied(UpdateFence fence) const {
    if (fence == UpdateFence::None) {
        return true;
    }
    if (m_legacyPending) {
        return false;
    }
    if (m_markers.isEmpty()) {
        return true;
    }
    const auto& state = m_framebuffer.state();
    return (fence == UpdateFence::Submission || state.unreliable_wait_for) &&
           state.can_wait_for_submission && m_sources.isEmpty();
}

void RefreshFence::start(UpdateFence fence, const QVector<uint32_t>& markers) {
    if ((m_error != 0) || m_waiter || satisfied(fence)) {
        return;
    }
    const auto& state = m_framebuffer.state();
    const bool legacy = state.is_kindle_legacy;
    const bool completion =
        legacy || ((fence == UpdateFence::Completion || !state.can_wait_for_submission) &&
                   !state.unreliable_wait_for);
    if (!completion && !state.can_wait_for_submission) {
        // This driver offers no usable fence. Preserve FBInk's device policy.
        m_markers.clear();
        m_sources.clear();
        return;
    }
    m_waitFence = completion ? UpdateFence::Completion : UpdateFence::Submission;
    m_waitMarkers = completion ? m_markers : markers;
    if (!completion && m_waitMarkers.isEmpty()) {
        for (const auto& source : m_sources) {
            m_waitMarkers.append(source.marker);
        }
    }
    auto kind = RefreshWaiter::Kind::Submission;
    if (legacy) {
        kind = RefreshWaiter::Kind::LegacyCompletion;
    } else if (completion) {
        kind = RefreshWaiter::Kind::Completion;
    }
    m_waiter.reset(new RefreshWaiter(m_framebuffer.fd(), m_waitMarkers, kind));
    connect(
        m_waiter.get(),
        &QThread::finished,
        this,
        [this] {
            if (m_waiter && m_waiter->isFinished()) {
                collect();
            }
            // A synchronous prepare/drain may have collected this worker first.
            // Its completion must still wake every deferred queue.
            emit settled();
        },
        Qt::QueuedConnection);
    m_waiter->start();
}

bool RefreshFence::collect() {
    m_waiter->wait();
    const int result = m_waiter->result;
    m_waiter.reset();
    if (result < 0) {
        m_error = result;
        return false;
    }
    for (int index = m_sources.size() - 1; index >= 0; --index) {
        if (m_waitMarkers.contains(m_sources[index].marker)) {
            m_sources.removeAt(index);
        }
    }
    if (m_waitFence == UpdateFence::Completion ||
        (m_framebuffer.state().unreliable_wait_for && m_sources.isEmpty())) {
        m_markers.clear();
        m_legacyPending = false;
    }
    m_waitMarkers.clear();
    m_waitFence = UpdateFence::None;
    return true;
}

FenceStatus RefreshFence::prepare(UpdateFence fence) {
    if (m_error != 0) {
        return FenceStatus::Failed;
    }
    if (m_waiter) {
        if (m_waiter->isRunning()) {
            return FenceStatus::Waiting;
        }
        if (!collect()) {
            return FenceStatus::Failed;
        }
    }
    if (satisfied(fence)) {
        return FenceStatus::Ready;
    }
    start(fence);
    return m_waiter ? FenceStatus::Waiting : FenceStatus::Ready;
}

bool RefreshFence::drain() {
    while (m_waiter || !satisfied(UpdateFence::Completion)) {
        if (m_waiter && !collect()) {
            return false;
        }
        if (m_error != 0) {
            return false;
        }
        start(UpdateFence::Completion);
    }
    if (m_error != 0) {
        return false;
    }
    // Include updates submitted by stock firmware before this session.
    if (m_framebuffer.fd() >= 0 && m_framebuffer.state().is_kindle_legacy) {
        m_error = RefreshWaiter::waitForLegacyUpdate(m_framebuffer.fd());
    }
    return m_error == 0;
}
