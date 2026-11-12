#pragma once

#include "displaybuffer.h"
#include "framebuffer.h"
#include "platformarguments.h"
#include "refreshfence.h"
#include "refreshpolicy.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

enum class SubmitStatus : quint8 {
    Submitted,
    Deferred,
    Failed
};

class RefreshScheduler : public QObject {
    // Qt declares short parameter names in this macro.
    // NOLINTNEXTLINE(readability-identifier-length)
    Q_OBJECT
public:
    RefreshScheduler(const PlatformOptions& options, const QImage& composite);
    ~RefreshScheduler() override;
    bool initialize();
    QRegion redraw(const QRegion& touched);

    bool suspended() const {
        return m_lifecycle != Lifecycle::Active;
    }

    void setPaletteLevels(int levels);
    void setInteractiveMode(bool active);
    void setTypingMode(bool active);
    void setKeyboardRegions(const QVector<QRect>& regions);

    quint64 keyboardFrame() const {
        return m_keyboard.frame;
    }

    bool keyboardRegionReady(const QRect& region, quint64 frame) const;

    int keyboardFeedbackDelay() const {
        return m_policy.keyboardFeedbackDelay;
    }

    void setInverted(bool enabled);

    bool canHardwareInvert() const {
        return m_framebuffer.state().can_hw_invert;
    }

    bool setRotation(int degrees);
    void requestQualityRefresh(const QRect& region);
    void requestTransitionRefresh(bool inverted);
    void requestFullRefresh();
    bool waitForRefresh();

    const FBInkState& framebufferState() const {
        return m_framebuffer.state();
    }

    bool synchronizeFramebuffer();
    bool setSuspended(bool suspended);

signals:
    void geometryChanged(const QRect& geometry, const QSizeF& physicalSize);
    void repaintRequested(const QRect& region);
    void framebufferChanged(const FBInkState& state);

private:
    enum class Lifecycle : quint8 {
        Active,
        Suspended,
        Failed
    };
    enum class InteractionPhase : quint8 {
        Idle,
        Active,
        Settling,
        // A framebuffer change ended interaction while its final damage was flushing.
        Draining
    };

    struct RefreshWork {
        // Pending retains intent without a wakeup, including while suspended.
        // The other pending phases name who will wake this work next.
        enum class Phase : quint8 {
            Idle,
            Pending,
            Scheduled,
            AwaitingFence,
            AwaitingComposition
        };

        bool pending() const {
            return phase != Phase::Idle;
        }

        bool scheduled() const {
            return phase == Phase::Scheduled;
        }

        void request();
        void addDamage(const QRegion& region);
        QRegion takeDamage();
        void schedule(int delay);
        void pause();
        void clear();

        QTimer timer;
        QRegion damage;
        Phase phase = Phase::Idle;
    };
    enum class FullRefreshMode : quint8 {
        None,
        Transition,
        Clear
    };
    enum class ScenePhase : quint8 {
        Ready,
        AwaitingComposition,
        AwaitingSubmission
    };
    enum class ClearPhase : quint8 {
        Idle,
        AwaitingWhite
    };
    enum class KeyboardPhase : quint8 {
        Disabled,
        AwaitingComposition,
        AwaitingSubmission,
        Ready
    };

    struct KeyboardFeedback {
        KeyboardPhase phase = KeyboardPhase::Disabled;
        QVector<QRect> regions;
        quint64 frame = 0;
        QElapsedTimer lastTextSubmit;
    };

    struct RefreshRequest {
        QRect region;
        WFM_MODE_INDEX_T waveform = WFM_GC16;
        bool flashing = false;
        bool cleanScene = false;
        bool typingScene = false;
    };

    bool interactive() const {
        return m_interactionPhase == InteractionPhase::Active ||
               m_interactionPhase == InteractionPhase::Settling;
    }

    bool settling() const {
        return m_interactionPhase == InteractionPhase::Settling ||
               m_interactionPhase == InteractionPhase::Draining;
    }

    bool typing() const {
        return m_keyboard.phase != KeyboardPhase::Disabled;
    }

    bool initialTypingFrame() const {
        return m_keyboard.phase == KeyboardPhase::AwaitingComposition ||
               m_keyboard.phase == KeyboardPhase::AwaitingSubmission;
    }

    bool frameNeedsComposition() const {
        return m_scenePhase == ScenePhase::AwaitingComposition;
    }

    bool sceneRefreshPending() const {
        return m_scenePhase != ScenePhase::Ready || m_touchCSceneCleanPending ||
               initialTypingFrame();
    }

    SubmitStatus deferredStatus() const {
        return m_lifecycle == Lifecycle::Failed ? SubmitStatus::Failed : SubmitStatus::Deferred;
    }

    SubmitStatus fail(const char* operation, int error);
    void stopRefreshTimers();
    void resumePending();
    void connectWork(RefreshWork& work, void (RefreshScheduler::*flush)());
    void retry(RefreshWork& work, int delay, SubmitStatus result);
    bool drainRefreshes();
    void prepareFramebufferChange();
    bool adoptFramebuffer();
    void configureRefreshPolicy();
    void queueInteractiveRefresh(const QRegion& changed);
    bool typingSourceBusy(const QRect& update, QVector<uint32_t>& hazards) const;
    void performPaletteRefresh();
    void flushPendingPartial();
    SubmitStatus flushTypingUpdates(const QRegion& pending);
    SubmitStatus sendTypingBatch(const QVector<QRect>& updates);
    void flushPendingQuality();
    void flushForcedQuality();
    void scheduleInteractiveSettle();
    void performInteractiveSettle();
    void recordPartialChanges(const QRegion& region);
    bool refreshReady(UpdateFence fence);
    RefreshRequest planUpdate(const QRect& region, WFM_MODE_INDEX_T waveform, bool flashing) const;
    void commitUpdate(const RefreshRequest& request);
    SubmitStatus submitOutputUpdate(
        const QRect& region, WFM_MODE_INDEX_T waveform, bool flashing, UpdateFence fence);
    SubmitStatus submitFullClear();
    void scheduleAdaptiveClean();
    void performAdaptiveClean();
    void resetCleaningDebt();
    SubmitStatus sendUpdate(const QRect& region, WFM_MODE_INDEX_T waveform, bool flashing);

    QRect geometry() const {
        return m_geometry;
    }

    Framebuffer m_framebuffer;
    DisplayBuffer m_buffer;
    RefreshFence m_fence;
    QRect m_geometry;
    QSizeF m_physicalSize;
    PlatformOptions m_options;
    bool m_debug;
    Lifecycle m_lifecycle = Lifecycle::Active;
    QTimer m_orientationTimer;
    RefreshWork m_paletteRefresh;
    RefreshWork m_partialRefresh;
    RefreshWork m_qualityRefresh;
    RefreshWork m_forcedQualityRefresh;
    QTimer m_interactiveSettleTimer;
    QTimer m_completionDrainTimer;
    QElapsedTimer m_lastPartialSubmit;
    KeyboardFeedback m_keyboard;
    bool m_submittingBatch = false;
    InteractionPhase m_interactionPhase = InteractionPhase::Idle;
    ScenePhase m_scenePhase = ScenePhase::Ready;
    FullRefreshMode m_fullRefreshMode = FullRefreshMode::None;
    ClearPhase m_clearPhase = ClearPhase::Idle;
    bool m_inverted = false;
    RefreshPolicy m_policy;
    bool m_touchCSceneCleanPending = false;
    quint64 m_changedPixelsSinceClean = 0;
    QVector<quint8> m_pixelDebt;
    RefreshWork m_adaptiveClean;
    QElapsedTimer m_cleanCooldown;
};
