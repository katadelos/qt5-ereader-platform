#pragma once
#include <QThread>
#include <QVector>
#include <cstdint>

// Only the blocking kernel wait runs here. Framebuffer writes and refresh
// submissions remain on the GUI thread. A typing batch copies all source
// rectangles before submitting any. Source-consumption fences allow the next
// input frame while the panel is still driving an earlier grayscale update.
class RefreshWaiter final : public QThread {
public:
    enum class Kind : quint8 {
        Submission,
        Completion,
        LegacyCompletion
    };

    RefreshWaiter(int descriptor, const QVector<uint32_t>& markers, Kind kind)
        : m_fd(descriptor), m_markers(markers), m_kind(kind) {}

    static int waitForLegacyUpdate(int descriptor);
    int result = 0;

protected:
    void run() override;

private:
    int m_fd;
    QVector<uint32_t> m_markers;
    Kind m_kind;
};
