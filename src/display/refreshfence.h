#pragma once

#include "framebuffer.h"
#include "refreshwaiter.h"

#include <QObject>
#include <QRect>
#include <QVector>
#include <memory>

enum class UpdateFence : quint8 {
    None,
    Submission,
    Completion
};
enum class FenceStatus : quint8 {
    Ready,
    Waiting,
    Failed
};

// Tracks which submitted regions the driver may still read. Only the blocking
// wait runs on a worker; all bookkeeping belongs to the GUI thread.
class RefreshFence final : public QObject {
    // Qt declares short parameter names in this macro.
    // NOLINTNEXTLINE(readability-identifier-length)
    Q_OBJECT
public:
    struct SourceUpdate {
        uint32_t marker;
        QRect region;
    };

    explicit RefreshFence(Framebuffer& framebuffer);
    ~RefreshFence() override;
    void submitted(uint32_t marker, const QRect& region);
    FenceStatus prepare(UpdateFence fence);
    void start(UpdateFence fence, const QVector<uint32_t>& markers = {});
    bool drain();

    bool waiting() const {
        return bool(m_waiter);
    }

    int error() const {
        return m_error;
    }

    const QVector<SourceUpdate>& sources() const {
        return m_sources;
    }

signals:
    void settled();

private:
    bool satisfied(UpdateFence fence) const;
    bool collect();
    Framebuffer& m_framebuffer;
    std::unique_ptr<RefreshWaiter> m_waiter;
    QVector<uint32_t> m_markers;
    QVector<SourceUpdate> m_sources;
    QVector<uint32_t> m_waitMarkers;
    UpdateFence m_waitFence = UpdateFence::None;
    bool m_legacyPending = false;
    int m_error = 0;
};
