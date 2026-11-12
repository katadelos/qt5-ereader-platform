#include "refreshscheduler.h"

#include "platformarguments.h"

#include <QDebug>
#include <QImage>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <linux/fb.h>

namespace {
constexpr int OrientationPollIntervalMs = 250;
constexpr int CompletionDrainIntervalMs = 750;
constexpr int GrayscaleLevels = 256;
constexpr int CompositeDepth = 32;
constexpr int QuarterTurnDegrees = 90;
constexpr int PaletteRefreshIdleMs = 2000;
constexpr int InteractiveFrameIntervalMs = 25;
constexpr int TypingTextIntervalMs = 300;
constexpr int QualityCoalesceMs = 35;
constexpr int TouchReleaseSettleMs = 220;

QRect refreshableRegion(const QRect& region, const QRect& bounds) {
    QRect clipped = region.intersected(bounds);
    if (clipped.isEmpty()) {
        return {};
    }

    // MTK Kobo refreshes can shift or clip unaligned regions. FBInk documents
    // an eight-pixel alignment requirement for avoiding these driver artifacts.
    // Expand outward so antialiased borders are included in the update.
    const int left = (clipped.left() / 8) * 8;
    const int top = (clipped.top() / 8) * 8;
    const int right = qMin(bounds.right() + 1, ((clipped.right() + 8) / 8) * 8);
    const int bottom = qMin(bounds.bottom() + 1, ((clipped.bottom() + 8) / 8) * 8);
    return QRect(left, top, right - left, bottom - top);
}
}

void RefreshScheduler::RefreshWork::request() {
    if (!pending()) {
        phase = Phase::Pending;
    }
}

void RefreshScheduler::RefreshWork::addDamage(const QRegion& region) {
    if (region.isEmpty()) {
        return;
    }
    damage += region;
    request();
}

QRegion RefreshScheduler::RefreshWork::takeDamage() {
    const QRegion pendingDamage = damage;
    clear();
    return pendingDamage;
}

void RefreshScheduler::RefreshWork::schedule(int delay) {
    phase = Phase::Scheduled;
    timer.start(delay);
}

void RefreshScheduler::RefreshWork::pause() {
    timer.stop();
    if (pending()) {
        phase = Phase::Pending;
    }
}

void RefreshScheduler::RefreshWork::clear() {
    timer.stop();
    damage = {};
    phase = Phase::Idle;
}

void RefreshScheduler::connectWork(RefreshWork& work, void (RefreshScheduler::*flush)()) {
    work.timer.setSingleShot(true);
    connect(&work.timer, &QTimer::timeout, this, [this, &work, flush] {
        work.pause();
        (this->*flush)();
    });
}

RefreshScheduler::RefreshScheduler(const PlatformOptions& options, const QImage& composite)
    : m_buffer(m_framebuffer, composite), m_fence(m_framebuffer), m_options(options),
      m_debug(options.debug) {
    m_orientationTimer.setInterval(OrientationPollIntervalMs);
    connect(&m_orientationTimer, &QTimer::timeout, this, &RefreshScheduler::synchronizeFramebuffer);
    connectWork(m_adaptiveClean, &RefreshScheduler::performAdaptiveClean);
    connectWork(m_paletteRefresh, &RefreshScheduler::performPaletteRefresh);
    connectWork(m_partialRefresh, &RefreshScheduler::flushPendingPartial);
    connectWork(m_qualityRefresh, &RefreshScheduler::flushPendingQuality);
    connectWork(m_forcedQualityRefresh, &RefreshScheduler::flushForcedQuality);
    m_interactiveSettleTimer.setSingleShot(true);
    connect(
        &m_interactiveSettleTimer,
        &QTimer::timeout,
        this,
        &RefreshScheduler::performInteractiveSettle);
    m_completionDrainTimer.setSingleShot(true);
    connect(&m_completionDrainTimer, &QTimer::timeout, this, [this] {
        refreshReady(UpdateFence::Completion);
    });
    connect(&m_fence, &RefreshFence::settled, this, &RefreshScheduler::resumePending);
}

RefreshScheduler::~RefreshScheduler() {
    m_lifecycle = Lifecycle::Suspended;
    m_orientationTimer.stop();
    stopRefreshTimers();
    if (m_framebuffer.fd() >= 0) {
        drainRefreshes();
    }
}

bool RefreshScheduler::initialize() {
    if (!m_buffer.configurePalette(m_options.paletteLevels) ||
        !m_framebuffer.initialize(m_options) || !m_buffer.reset(m_inverted)) {
        m_lifecycle = Lifecycle::Failed;
        return false;
    }
    m_geometry = m_framebuffer.geometry();
    m_physicalSize = m_framebuffer.physicalSize();

    configureRefreshPolicy();

    emit geometryChanged(m_geometry, m_physicalSize);
    if (m_framebuffer.state().can_rotate) {
        m_orientationTimer.start();
    }

    if (m_debug) {
        qDebug() << "ereader: FBInk framebuffer" << geometry() << CompositeDepth << "bpp"
                 << "stride" << m_framebuffer.state().scanline_stride << "device"
                 << m_framebuffer.state().device_name << "id" << m_framebuffer.state().device_id
                 << "physical bpp" << m_framebuffer.state().bpp;
    }

    return true;
}

void RefreshScheduler::setPaletteLevels(int levels) {
    if (suspended() || levels == m_buffer.paletteLevels()) {
        return;
    }
    if (levels != 0 && (levels < 2 || levels > GrayscaleLevels)) {
        qWarning() << "ereader: palette must be disabled or contain between 2 and 256 levels";
        return;
    }
    if (m_framebuffer.fd() < 0 || m_buffer.composite().isNull() || !m_framebuffer.isValid()) {
        return;
    }
    m_buffer.configurePalette(levels);

    // mScreenImage retains Qt's full-color composition. Reblit the whole
    // current scene so disabling the palette restores the original soft
    // output immediately, without restarting or asking every window to paint.
    const QRegion screenRegion(geometry());
    m_buffer.composeOutput(screenRegion);

    // Opening Quick Settings itself creates a large update. Old Kobo EPDC
    // queues are unreliable when another full GC16 is submitted immediately
    // behind it, so coalesce repeated mode changes and refresh after idle.
    m_paletteRefresh.schedule(PaletteRefreshIdleMs);

    if (m_debug) {
        qDebug() << "ereader: output palette"
                 << ((m_buffer.paletteLevels() != 0) ? QString::number(m_buffer.paletteLevels())
                                                     : QStringLiteral("soft"));
    }
}

void RefreshScheduler::setInteractiveMode(bool active) {
    if (suspended()) {
        return;
    }
    if (active) {
        m_interactiveSettleTimer.stop();
        // Do not paint a queued image-quality frame immediately before its
        // menu overlay. Carry that damage into the first interactive frame so entering
        // a menu is one visible transition, not two separate transitions.
        m_partialRefresh.addDamage(m_qualityRefresh.takeDamage());
        m_interactionPhase = InteractionPhase::Active;
        return;
    }

    if (interactive()) {
        scheduleInteractiveSettle();
    }
}

void RefreshScheduler::setTypingMode(bool active) {
    if (typing() == active) {
        return;
    }

    // Establish the complete keyboard before individual key feedback. Keep
    // this intent until submission succeeds, even if a previous frame is busy.
    m_keyboard.phase = active ? KeyboardPhase::AwaitingComposition : KeyboardPhase::Disabled;
    m_keyboard.lastTextSubmit.invalidate();
}

void RefreshScheduler::setKeyboardRegions(const QVector<QRect>& regions) {
    m_keyboard.regions = regions;
}

bool RefreshScheduler::keyboardRegionReady(const QRect& region, quint64 frame) const {
    // A QQuickWindow swap is not a panel submission. Require a newer backing
    // store frame, then verify these exact pixels reached the submitted shadow.
    return !suspended() && !frameNeedsComposition() && !sceneRefreshPending() &&
           m_keyboard.frame > frame && !region.intersected(geometry()).isEmpty() &&
           m_buffer.changedBoundingRect(region).isEmpty();
}

void RefreshScheduler::setInverted(bool enabled) {
    if (enabled == m_inverted) {
        return;
    }

    m_inverted = enabled;
    if (suspended()) {
        return;
    }
    // Keep Qt and its shadow in logical colors. Supported panels invert in
    // the EPDC; other panels use the existing framebuffer-boundary transform.

    if (m_framebuffer.fd() < 0 || !m_framebuffer.isValid() || m_buffer.output().isNull()) {
        return;
    }

    // Inversion replaces the entire scene, including queued interaction pixels.
    m_interactiveSettleTimer.stop();
    m_partialRefresh.clear();
    m_qualityRefresh.clear();
    m_interactionPhase = InteractionPhase::Idle;
    m_forcedQualityRefresh.clear();
    m_forcedQualityRefresh.addDamage(QRegion(geometry()));
    if (m_clearPhase == ClearPhase::Idle) {
        m_fullRefreshMode = FullRefreshMode::Transition;
        const SubmitStatus result =
            submitOutputUpdate(geometry(), WFM_GC16, true, UpdateFence::Completion);
        if (result == SubmitStatus::Submitted) {
            m_fullRefreshMode = FullRefreshMode::None;
            m_forcedQualityRefresh.clear();
        } else {
            retry(m_forcedQualityRefresh, TouchReleaseSettleMs, result);
        }
    } else {
        retry(m_forcedQualityRefresh, 0, SubmitStatus::Deferred);
    }

    if (m_debug) {
        qDebug() << "ereader: framebuffer inversion" << enabled << "hardware"
                 << canHardwareInvert();
    }
}

bool RefreshScheduler::setRotation(int degrees) {
    if (degrees < 0 || degrees > 3 * QuarterTurnDegrees || degrees % QuarterTurnDegrees != 0) {
        qWarning() << "ereader: unsupported logical rotation" << degrees;
        return false;
    }
    if (suspended() || !drainRefreshes() || !synchronizeFramebuffer()) {
        return false;
    }
    const int result = m_framebuffer.setRotation(degrees);
    if (result < 0) {
        fail("rotate framebuffer", result);
        return false;
    }
    return synchronizeFramebuffer();
}

void RefreshScheduler::prepareFramebufferChange() {
    m_keyboard.lastTextSubmit.invalidate();
    stopRefreshTimers();
}

bool RefreshScheduler::synchronizeFramebuffer() {
    if (suspended() || !m_framebuffer.isValid()) {
        return false;
    }
    // Driver waits hold the framebuffer lock. Keep composing in RAM until
    // completion; every submission checks geometry again before writing.
    if (m_fence.waiting()) {
        return true;
    }
    bool changed = false;
    FBInkState state = {};
    const int result = m_framebuffer.inspect(state, changed);
    if (result < 0) {
        fail("synchronize framebuffer", result);
        return false;
    }
    if (!changed) {
        return true;
    }
    if (!drainRefreshes()) {
        return false;
    }
    prepareFramebufferChange();
    const int adopted = m_framebuffer.adopt(state);
    if (adopted < 0) {
        fail("adopt framebuffer", adopted);
        return false;
    }
    return adoptFramebuffer();
}

bool RefreshScheduler::setSuspended(bool suspended) {
    if (m_lifecycle == Lifecycle::Failed) {
        return false;
    }
    if ((m_lifecycle == Lifecycle::Suspended) == suspended) {
        return true;
    }
    if (suspended) {
        m_lifecycle = Lifecycle::Suspended;
        m_orientationTimer.stop();
        prepareFramebufferChange();
        if (!drainRefreshes()) {
            return false;
        }
        m_clearPhase = ClearPhase::Idle;
        m_paletteRefresh.clear();
        m_framebuffer.restorePowerdownDelay();
        m_partialRefresh.clear();
        m_qualityRefresh.clear();
        m_forcedQualityRefresh.clear();
        m_fullRefreshMode = FullRefreshMode::None;
        m_interactionPhase = InteractionPhase::Idle;
    } else {
        m_lifecycle = Lifecycle::Active;
        if (!synchronizeFramebuffer()) {
            return false;
        }
        configureRefreshPolicy();
        if (!m_buffer.resetShadow(m_inverted)) {
            fail("allocate framebuffer shadow", -ENOMEM);
            return false;
        }
        // Nickel has painted the sleep screen and may have rotated the panel.
        // Recompose the complete app, even if its content did not change.
        m_scenePhase = ScenePhase::AwaitingComposition;
        emit repaintRequested(geometry());
        m_orientationTimer.start();
    }
    return true;
}

bool RefreshScheduler::adoptFramebuffer() {
    if (!m_buffer.reset(m_inverted)) {
        fail("allocate display buffers", -ENOMEM);
        return false;
    }
    const QRect newGeometry = m_framebuffer.geometry();
    m_physicalSize = m_framebuffer.physicalSize();
    m_scenePhase = ScenePhase::AwaitingComposition;
    m_geometry = newGeometry;
    emit geometryChanged(newGeometry, m_physicalSize);
    emit framebufferChanged(m_framebuffer.state());
    m_interactionPhase = InteractionPhase::Idle;
    m_interactiveSettleTimer.stop();

    m_partialRefresh.clear();
    m_qualityRefresh.clear();
    m_forcedQualityRefresh.clear();
    m_fullRefreshMode = FullRefreshMode::None;
    m_clearPhase = ClearPhase::Idle;
    m_paletteRefresh.clear();
    m_keyboard.phase = typing() ? KeyboardPhase::AwaitingComposition : KeyboardPhase::Disabled;
    configureRefreshPolicy();
    m_adaptiveClean.clear();
    m_changedPixelsSinceClean = 0;
    m_lastPartialSubmit.invalidate();
    m_cleanCooldown.invalidate();
    m_touchCSceneCleanPending = m_policy.clearScene;

    // The geometry notification resizes maximized windows. Force the new Qt
    // scene through the normal full-screen GC16 page-change path.
    emit repaintRequested(newGeometry);

    if (m_debug) {
        qDebug() << "ereader: framebuffer rotation" << m_framebuffer.state().current_rota
                 << newGeometry;
    }
    return true;
}

void RefreshScheduler::requestQualityRefresh(const QRect& region) {
    if (suspended()) {
        return;
    }
    const QRect clipped = region.intersected(geometry());
    if (clipped.isEmpty()) {
        return;
    }

    // This region describes panel history, not framebuffer damage. It must
    // survive even when the current output and shadow already contain the
    // same pixels after an interaction erased the source content.
    m_forcedQualityRefresh.addDamage(clipped);
    m_forcedQualityRefresh.schedule(0);
}

void RefreshScheduler::requestFullRefresh() {
    if (suspended()) {
        return;
    }
    if (m_framebuffer.fd() < 0 || !m_framebuffer.isValid() || m_buffer.output().isNull()) {
        return;
    }

    // A full refresh is deliberately stronger than a flashing GC16 update.
    // INIT resets the panel to white and clears its transition history; GC16
    // then paints the current Qt frame back at full grayscale fidelity.
    m_fullRefreshMode = FullRefreshMode::Clear;
    m_forcedQualityRefresh.clear();
    m_forcedQualityRefresh.addDamage(QRegion(geometry()));
    m_forcedQualityRefresh.schedule(0);
}

void RefreshScheduler::requestTransitionRefresh(bool inverted) {
    if (m_framebuffer.fd() < 0 || !m_framebuffer.isValid() || m_buffer.output().isNull()) {
        return;
    }

    // Apply the output transform as part of the same operation. In particular,
    // a dark-mode power screen must not first repaint the old scene merely to
    // disable inversion and then repaint the special screen a second time.
    m_inverted = inverted;
    if (suspended()) {
        return;
    }

    // Special screens need one authoritative full-screen transition, not the
    // INIT-to-white plus GC16 sequence used for an explicit panel clear. This
    // also supersedes a clear queued while the previous menu was closing.
    if (m_clearPhase == ClearPhase::Idle) {
        m_fullRefreshMode = FullRefreshMode::Transition;
    }
    m_forcedQualityRefresh.clear();
    m_forcedQualityRefresh.addDamage(QRegion(geometry()));
    m_forcedQualityRefresh.schedule(0);
}

bool RefreshScheduler::drainRefreshes() {
    if (!m_fence.drain()) {
        fail("drain refreshes", m_fence.error());
        return false;
    }
    return m_lifecycle != Lifecycle::Failed;
}

bool RefreshScheduler::waitForRefresh() {
    if (!drainRefreshes()) {
        return false;
    }
    // A clear already in flight includes its repaint. Unlike normal rendering,
    // this explicit synchronization API is allowed to block for completion.
    if (m_clearPhase != ClearPhase::Idle) {
        if (!synchronizeFramebuffer()) {
            return false;
        }
        if (m_clearPhase != ClearPhase::Idle) {
            if (submitFullClear() != SubmitStatus::Submitted || !drainRefreshes()) {
                return false;
            }
            m_fullRefreshMode = FullRefreshMode::None;
            m_forcedQualityRefresh.clear();
        }
    }
    resumePending();
    return true;
}

QRegion RefreshScheduler::redraw(const QRegion& touched) {
    if (suspended() || touched.isEmpty()) {
        return touched;
    }

    m_buffer.composeOutput(touched);
    if (frameNeedsComposition()) {
        m_scenePhase = ScenePhase::AwaitingSubmission;
    }
    if (m_keyboard.phase == KeyboardPhase::AwaitingComposition) {
        m_keyboard.phase = KeyboardPhase::AwaitingSubmission;
    }
    ++m_keyboard.frame;
    resumePending();

    const QRegion changed = m_buffer.changedRegion(touched);

    if (changed.isEmpty() && !sceneRefreshPending()) {
        if (m_debug) {
            qDebug() << "ereader: skipping unchanged Qt damage" << touched;
        }
        return touched;
    }

    const QRegion screenRegion(geometry());
    // Qt can invalidate broad bounds around a small control change. Use the
    // pixel comparison so repaint bounds alone cannot force a panel update.
    const QRegion visibleDamage = changed.intersected(screenRegion);
    qint64 damagedArea = 0;
    for (const QRect& rect : visibleDamage) {
        damagedArea += qint64(rect.width()) * rect.height();
    }
    const qint64 screenArea = qint64(geometry().width()) * geometry().height();
    // A 180-degree rotation can leave identical framebuffer bytes, but the
    // panel still needs the complete scene refreshed in its new orientation.
    // Large scroll damage must not expand into stationary toolbars. Only
    // Touch C's scene-clearing policy needs the broad-area heuristic; other
    // panels can refresh the changed region with the same grayscale waveform.
    const bool pageChange =
        (m_scenePhase != ScenePhase::Ready) || initialTypingFrame() ||
        (m_policy.clearScene && !typing() && damagedArea * 100 >= screenArea * 80);
    if (m_policy.clearScene && pageChange) {
        m_touchCSceneCleanPending = true;
    }
    if (visibleDamage == screenRegion || pageChange || m_touchCSceneCleanPending) {
        // Dimmed overlays and page changes need a complete grayscale frame,
        // including when triggered by touch. Touch C also needs a clearing
        // cycle when replacing a scene; smaller control updates remain partial.
        // Keep any touch-release timer so it can end interactive mode.
        m_partialRefresh.clear();
        m_qualityRefresh.clear();
        const SubmitStatus result =
            submitOutputUpdate(geometry(), WFM_GC16, false, UpdateFence::Completion);
        if (result == SubmitStatus::Submitted) {
            m_paletteRefresh.clear();
        } else if (result == SubmitStatus::Deferred) {
            m_qualityRefresh.addDamage(screenRegion);
            retry(m_qualityRefresh, QualityCoalesceMs, result);
        }
    } else if (typing()) {
        m_partialRefresh.addDamage(changed);
        // Feedback must not wait for the touch coalescer or release timer.
        flushPendingPartial();
    } else if (interactive()) {
        queueInteractiveRefresh(changed);
    } else {
        // Ordinary UI construction is not animation. Batch one event-loop
        // burst, then render it with a true grayscale waveform so text and
        // rules arrive at their final grayscale values.
        m_qualityRefresh.addDamage(changed);
        m_qualityRefresh.schedule(QualityCoalesceMs);
    }
    if (m_paletteRefresh.scheduled()) {
        m_paletteRefresh.schedule(PaletteRefreshIdleMs);
    }

    return touched;
}

void RefreshScheduler::queueInteractiveRefresh(const QRegion& changed) {
    // Coalesce touch feedback and drags into the newest grayscale frame.
    m_partialRefresh.addDamage(changed);
    if (!m_lastPartialSubmit.isValid() ||
        m_lastPartialSubmit.elapsed() >= InteractiveFrameIntervalMs) {
        flushPendingPartial();
    } else if (!m_partialRefresh.scheduled()) {
        m_partialRefresh.schedule(
            InteractiveFrameIntervalMs - static_cast<int>(m_lastPartialSubmit.elapsed()));
    }

    if (settling()) {
        scheduleInteractiveSettle();
    }
}

void RefreshScheduler::performPaletteRefresh() {
    if (suspended()) {
        return;
    }
    if (m_framebuffer.fd() < 0 || m_buffer.composite().isNull() || !m_framebuffer.isValid()) {
        return;
    }

    const QRegion screenRegion(geometry());
    m_buffer.composeOutput(screenRegion);

    const SubmitStatus result =
        submitOutputUpdate(geometry(), WFM_GC16, true, UpdateFence::Completion);
    if (result == SubmitStatus::Submitted) {
        m_paletteRefresh.clear();
    } else {
        retry(m_paletteRefresh, RefreshPolicy::CleanRetryMs, result);
    }
}

bool RefreshScheduler::typingSourceBusy(const QRect& update, QVector<uint32_t>& hazards) const {
    bool busy = false;
    for (const RefreshFence::SourceUpdate& source : m_fence.sources()) {
        const QRect overlap = update.intersected(source.region);
        // Aligned rectangles can share unchanged gutters. Only a write
        // changing the old source pixels needs to wait for consumption.
        if (!overlap.isEmpty() && !m_buffer.changedBoundingRect(overlap).isEmpty()) {
            busy = true;
            if (!hazards.contains(source.marker)) {
                hazards.append(source.marker);
            }
        }
    }
    return busy;
}

SubmitStatus RefreshScheduler::flushTypingUpdates(const QRegion& pending) {
    // A driver wait holds the framebuffer ioctl lock, even on another thread.
    // Collect an existing wait, but do not drain unrelated source regions.
    const UpdateFence fence =
        m_framebuffer.state().can_wait_for_submission ? UpdateFence::None : UpdateFence::Completion;
    if (m_clearPhase != ClearPhase::Idle || !refreshReady(fence) || !synchronizeFramebuffer() ||
        frameNeedsComposition()) {
        return deferredStatus();
    }

    QVector<QRect> updates;
    QVector<uint32_t> hazards;
    QRegion deferred;
    auto appendUpdate = [&](const QRect& update) {
        const bool busy = typingSourceBusy(update, hazards);
        if (busy) {
            deferred += update;
        } else {
            updates.append(update);
        }
        return !busy;
    };
    QRegion remaining = pending.intersected(geometry());
    // Qt may invalidate the bounds spanning several controls. Partition
    // before diffing so a key and a distant field never form one update.
    for (const QRect& key : qAsConst(m_keyboard.regions)) {
        if (remaining.intersects(key) && !m_buffer.changedBoundingRect(key).isEmpty()) {
            appendUpdate(refreshableRegion(key, geometry()));
        }
        remaining -= key;
    }
    // GC16 field updates can collide with each other for a whole waveform.
    // Keep the newest field pixels and pace their submissions independently
    // of keys, rather than locking every subsequent key behind a field wait.
    const bool textReady = !m_keyboard.lastTextSubmit.isValid() ||
                           m_keyboard.lastTextSubmit.elapsed() >= TypingTextIntervalMs;
    bool submittedText = false;
    for (const QRect& rect : remaining) {
        const QRect changed = m_buffer.changedBoundingRect(rect);
        if (changed.isEmpty()) {
            continue;
        }
        if (!textReady) {
            deferred += changed;
        } else if (appendUpdate(refreshableRegion(changed, geometry()))) {
            submittedText = true;
        }
    }
    // Submit disjoint keys before starting a potentially blocking source wait.
    const SubmitStatus submitted = sendTypingBatch(updates);
    if (submitted != SubmitStatus::Submitted) {
        return submitted;
    }
    if (submittedText) {
        m_keyboard.lastTextSubmit.restart();
    }
    if (!hazards.isEmpty()) {
        m_fence.start(UpdateFence::Submission, hazards);
    }
    if (!deferred.isEmpty()) {
        m_partialRefresh.addDamage(deferred);
        const int delay =
            m_keyboard.lastTextSubmit.isValid()
                ? qMax(0, TypingTextIntervalMs - int(m_keyboard.lastTextSubmit.elapsed()))
                : 0;
        retry(m_partialRefresh, delay, SubmitStatus::Deferred);
    }
    return SubmitStatus::Submitted;
}

SubmitStatus RefreshScheduler::sendTypingBatch(const QVector<QRect>& updates) {
    if (updates.isEmpty()) {
        return SubmitStatus::Submitted;
    }
    QRegion source;
    for (const QRect& update : updates) {
        source += update;
    }
    // eink_fb has a global completion fence, not per-region markers. Keep
    // one panel update in flight, including when a key and its field change.
    const QVector<QRect> batch =
        m_framebuffer.state().is_kindle_legacy ? QVector<QRect>{source.boundingRect()} : updates;
    if (m_framebuffer.state().is_kindle_legacy) {
        source = QRegion(source.boundingRect());
    }
    // PxP/MDP may read after SEND_UPDATE returns. Copy once, submit the batch,
    // and protect its source until consumed. eink_fb also waits for optical
    // completion so subsequent frames remain replaceable in userspace.
    const int result = m_buffer.copyOutputToFramebuffer(source, m_inverted);
    if (result < 0) {
        fail("copy typing frame", result);
        return SubmitStatus::Failed;
    }
    m_submittingBatch = true;
    SubmitStatus status = SubmitStatus::Submitted;
    for (const QRect& update : batch) {
        // Preserve glyph antialiasing in both polarities. Every key release
        // drives its paper and edges to their final grayscale values directly.
        status = sendUpdate(update, WFM_GC16, false);
        if (status != SubmitStatus::Submitted) {
            break;
        }
        RefreshRequest request;
        request.region = update;
        commitUpdate(request);
    }
    m_submittingBatch = false;
    if (m_framebuffer.state().is_kindle_legacy || status != SubmitStatus::Submitted) {
        m_fence.start(UpdateFence::Completion);
    }
    // Retire completed markers when idle. This does not repaint or flash.
    if (status != SubmitStatus::Failed) {
        m_completionDrainTimer.start(CompletionDrainIntervalMs);
    }
    m_lastPartialSubmit.restart();
    return status;
}

void RefreshScheduler::flushPendingPartial() {
    if (suspended()) {
        return;
    }
    const QRegion pending = m_partialRefresh.takeDamage().intersected(geometry());
    if (pending.isEmpty()) {
        return;
    }

    if (typing() && !sceneRefreshPending()) {
        const SubmitStatus result = flushTypingUpdates(pending);
        if (result == SubmitStatus::Deferred) {
            m_partialRefresh.addDamage(pending);
            retry(m_partialRefresh, InteractiveFrameIntervalMs, result);
        }
        return;
    }

    if (!refreshReady(UpdateFence::Completion)) {
        m_partialRefresh.addDamage(pending);
        retry(m_partialRefresh, InteractiveFrameIntervalMs, deferredStatus());
        return;
    }

    const QRegion changed = m_buffer.changedRegion(pending);
    if (changed.isEmpty() && !sceneRefreshPending()) {
        return;
    }

    const QRect atomicRegion = sceneRefreshPending() ? geometry() : changed.boundingRect();
    // Keep grayscale source pixels for controls and the initial keyboard scene.
    // Forma queues PxP reads after submission, so finish the previous update
    // before replacing its framebuffer source.
    const SubmitStatus result =
        submitOutputUpdate(atomicRegion, WFM_GC16, false, UpdateFence::Completion);
    if (result != SubmitStatus::Submitted) {
        m_partialRefresh.addDamage(pending);
        retry(m_partialRefresh, InteractiveFrameIntervalMs, result);
        return;
    }

    m_lastPartialSubmit.restart();
}

void RefreshScheduler::flushPendingQuality() {
    if (suspended()) {
        return;
    }
    const QRegion pending = m_qualityRefresh.takeDamage().intersected(geometry());
    if (pending.isEmpty()) {
        return;
    }

    if (interactive() || typing()) {
        m_partialRefresh.addDamage(pending);
        if (!m_partialRefresh.scheduled()) {
            m_partialRefresh.schedule(0);
        }
        return;
    }

    if (!refreshReady(UpdateFence::Completion)) {
        m_qualityRefresh.addDamage(pending);
        retry(m_qualityRefresh, QualityCoalesceMs, deferredStatus());
        return;
    }

    const QRegion changed = m_buffer.changedRegion(pending);
    if (changed.isEmpty() && !sceneRefreshPending()) {
        return;
    }

    const QRect qualityRegion = sceneRefreshPending() ? geometry() : changed.boundingRect();
    const SubmitStatus result =
        submitOutputUpdate(qualityRegion, WFM_GC16, false, UpdateFence::Completion);
    if (result != SubmitStatus::Submitted) {
        m_qualityRefresh.addDamage(pending);
        retry(m_qualityRefresh, QualityCoalesceMs, result);
    }
}

void RefreshScheduler::flushForcedQuality() {
    if (suspended()) {
        return;
    }
    if (m_forcedQualityRefresh.damage.isEmpty()) {
        m_forcedQualityRefresh.clear();
        return;
    }

    // Let touch-up, the final interactive frame, and Qt Quick's optimistic relayout all
    // settle first. Unlike ordinary quality damage, this request deliberately
    // bypasses m_buffer.changedBoundingRect() and always drives the requested pixels.
    if (m_clearPhase == ClearPhase::Idle &&
        (interactive() || settling() || m_partialRefresh.pending() || m_qualityRefresh.pending())) {
        m_forcedQualityRefresh.schedule(TouchReleaseSettleMs);
        return;
    }

    const FullRefreshMode fullRefreshMode = m_fullRefreshMode;
    const bool fullScreen = fullRefreshMode != FullRefreshMode::None;
    const QRect qualityRegion =
        fullScreen ? geometry()
                   : m_forcedQualityRefresh.damage.intersected(geometry()).boundingRect();
    // Direct-manipulation cleanup remains a bounded flashing GC16. Only an
    // explicit panel clear takes the stronger whole-panel INIT path.
    const SubmitStatus result =
        fullRefreshMode == FullRefreshMode::Clear || m_clearPhase != ClearPhase::Idle
            ? submitFullClear()
            : submitOutputUpdate(qualityRegion, WFM_GC16, true, UpdateFence::Completion);
    if (result != SubmitStatus::Submitted) {
        retry(m_forcedQualityRefresh, TouchReleaseSettleMs, result);
        return;
    }

    m_forcedQualityRefresh.clear();
    if (fullScreen) {
        m_fullRefreshMode = FullRefreshMode::None;
        m_paletteRefresh.clear();
    }
}

void RefreshScheduler::scheduleInteractiveSettle() {
    m_interactionPhase = interactive() ? InteractionPhase::Settling : InteractionPhase::Draining;
    m_interactiveSettleTimer.start(TouchReleaseSettleMs);
}

void RefreshScheduler::performInteractiveSettle() {
    // The single-shot timer has fired. Keep interaction active while flushing,
    // unless a framebuffer change has already ended it.
    m_interactionPhase = interactive() ? InteractionPhase::Active : InteractionPhase::Idle;
    if (suspended()) {
        return;
    }
    if (m_partialRefresh.pending()) {
        flushPendingPartial();
    }
    if (m_partialRefresh.pending()) {
        scheduleInteractiveSettle();
        return;
    }

    // Local interactions already use grayscale GC16 with protected source
    // pixels. Ending a touch must not drive unchanged controls elsewhere on
    // the panel through another waveform. Explicit cleanup requests retain
    // their own region and remain separate from ordinary touch feedback.
    m_interactionPhase = InteractionPhase::Idle;
    if (m_forcedQualityRefresh.pending()) {
        m_forcedQualityRefresh.schedule(0);
    }
}

void RefreshScheduler::configureRefreshPolicy() {
    m_policy = RefreshPolicy::forDevice(m_framebuffer.state());
    m_touchCSceneCleanPending = m_policy.clearScene;

    m_framebuffer.configurePowerdownDelay();

    if (!m_policy.adaptiveCleaning) {
        m_pixelDebt.clear();
        return;
    }

    const qsizetype pixelCount = static_cast<qsizetype>(geometry().width()) * geometry().height();
    m_pixelDebt.fill(0, pixelCount);

    if (m_debug) {
        qDebug() << "ereader: Kobo Mini adaptive cleaning enabled; pixels" << pixelCount
                 << "hot threshold" << RefreshPolicy::HotPixelThreshold << "global threshold"
                 << RefreshPolicy::ScreenEquivalentThreshold << "screen equivalents";
    }
}

void RefreshScheduler::recordPartialChanges(const QRegion& region) {
    if (!m_policy.adaptiveCleaning || region.isEmpty() || m_pixelDebt.isEmpty()) {
        return;
    }

    const int screenWidth = geometry().width();
    const quint64 screenPixels = static_cast<quint64>(screenWidth) * geometry().height();
    bool hotPixel = false;

    for (const QRect& rect : region) {
        const QRect clipped = rect.intersected(geometry());
        for (int row = clipped.top(); row <= clipped.bottom(); ++row) {
            const unsigned char* current = m_buffer.output().constScanLine(row) + clipped.left();
            const unsigned char* previous = m_buffer.shadow().constScanLine(row) + clipped.left();
            qsizetype debtIndex = static_cast<qsizetype>(row) * screenWidth + clipped.left();

            for (int column = 0; column < clipped.width(); ++column, ++debtIndex) {
                if (current[column] == previous[column]) {
                    continue;
                }

                ++m_changedPixelsSinceClean;
                quint8& debt = m_pixelDebt[debtIndex];
                if (debt < RefreshPolicy::HotPixelThreshold) {
                    ++debt;
                }
                hotPixel = hotPixel || debt >= RefreshPolicy::HotPixelThreshold;
            }
        }
    }

    if (hotPixel ||
        m_changedPixelsSinceClean >= screenPixels * RefreshPolicy::ScreenEquivalentThreshold) {
        m_adaptiveClean.request();
    }

    if (m_adaptiveClean.pending()) {
        scheduleAdaptiveClean();
    }
}

bool RefreshScheduler::refreshReady(UpdateFence fence) {
    if (suspended()) {
        return false;
    }
    const FenceStatus result = m_fence.prepare(fence);
    if (result == FenceStatus::Failed) {
        fail("wait for refresh", m_fence.error());
    }
    return result == FenceStatus::Ready;
}

RefreshScheduler::RefreshRequest
RefreshScheduler::planUpdate(const QRect& region, WFM_MODE_INDEX_T waveform, bool flashing) const {
    RefreshRequest request;
    request.cleanScene = m_touchCSceneCleanPending;
    request.typingScene = initialTypingFrame();
    request.region = refreshableRegion(sceneRefreshPending() ? geometry() : region, geometry());
    request.waveform = waveform;
    if (request.cleanScene || request.typingScene || region == geometry()) {
        request.waveform = WFM_GC16;
    }
    request.flashing = flashing || request.cleanScene;
    return request;
}

void RefreshScheduler::commitUpdate(const RefreshRequest& request) {
    recordPartialChanges(QRegion(request.region));
    m_buffer.updateFramebufferShadow(QRegion(request.region));
    if (request.typingScene) {
        m_keyboard.phase = KeyboardPhase::Ready;
    }
    if (request.cleanScene) {
        m_touchCSceneCleanPending = false;
    }
    if (request.region == geometry()) {
        m_scenePhase = ScenePhase::Ready;
        resetCleaningDebt();
    }
}

SubmitStatus RefreshScheduler::submitOutputUpdate(
    const QRect& region, WFM_MODE_INDEX_T waveform, bool flashing, UpdateFence fence) {
    if (m_lifecycle == Lifecycle::Failed) {
        return SubmitStatus::Failed;
    }
    if (m_keyboard.phase == KeyboardPhase::AwaitingComposition ||
        m_clearPhase != ClearPhase::Idle || !refreshReady(fence)) {
        return deferredStatus();
    }
    if (!synchronizeFramebuffer() || frameNeedsComposition()) {
        return deferredStatus();
    }
    const RefreshRequest request = planUpdate(region, waveform, flashing);
    if (request.region.isEmpty()) {
        return SubmitStatus::Submitted;
    }
    const int result = m_buffer.copyOutputToFramebuffer(QRegion(request.region), m_inverted);
    if (result < 0) {
        return fail("copy framebuffer pixels", result);
    }
    const SubmitStatus submitted = sendUpdate(request.region, request.waveform, request.flashing);
    if (submitted != SubmitStatus::Submitted) {
        return submitted;
    }
    commitUpdate(request);
    return SubmitStatus::Submitted;
}

SubmitStatus RefreshScheduler::submitFullClear() {
    if (m_lifecycle == Lifecycle::Failed) {
        return SubmitStatus::Failed;
    }
    if (!refreshReady(UpdateFence::Completion)) {
        return deferredStatus();
    }
    if (!synchronizeFramebuffer() || frameNeedsComposition() ||
        m_keyboard.phase == KeyboardPhase::AwaitingComposition) {
        return deferredStatus();
    }
    if (m_clearPhase == ClearPhase::Idle) {
        // Preserve output in RAM while INIT drives the panel to white.
        m_framebuffer.fillWhite();
        const SubmitStatus submitted = sendUpdate(geometry(), WFM_INIT, true);
        if (submitted != SubmitStatus::Submitted) {
            return submitted;
        }
        m_clearPhase = ClearPhase::AwaitingWhite;
        return SubmitStatus::Deferred;
    }
    const RefreshRequest request = planUpdate(geometry(), WFM_GC16, true);
    const int result = m_buffer.copyOutputToFramebuffer(QRegion(request.region), m_inverted);
    if (result < 0) {
        return fail("restore pixels after clear", result);
    }
    const SubmitStatus submitted = sendUpdate(request.region, request.waveform, request.flashing);
    if (submitted != SubmitStatus::Submitted) {
        return submitted;
    }
    commitUpdate(request);
    m_clearPhase = ClearPhase::Idle;
    return SubmitStatus::Submitted;
}

void RefreshScheduler::scheduleAdaptiveClean() {
    if (!m_policy.adaptiveCleaning || !m_adaptiveClean.pending()) {
        return;
    }

    int delay = RefreshPolicy::CleanIdleMs;
    if (m_cleanCooldown.isValid()) {
        const qint64 cooldownRemaining = RefreshPolicy::CleanCooldownMs - m_cleanCooldown.elapsed();
        if (cooldownRemaining > delay) {
            delay = static_cast<int>(cooldownRemaining);
        }
    }
    m_adaptiveClean.schedule(delay);
}

void RefreshScheduler::performAdaptiveClean() {
    if (suspended()) {
        return;
    }
    if (!m_policy.adaptiveCleaning || !m_adaptiveClean.pending()) {
        return;
    }

    if (m_cleanCooldown.isValid() && m_cleanCooldown.elapsed() < RefreshPolicy::CleanCooldownMs) {
        scheduleAdaptiveClean();
        return;
    }

    // On devices with unreliable completion markers, submitOutputUpdate falls
    // back to the submission fence before replacing the framebuffer contents.
    const SubmitStatus result =
        submitOutputUpdate(geometry(), WFM_GC16, true, UpdateFence::Completion);
    if (result != SubmitStatus::Submitted) {
        retry(m_adaptiveClean, RefreshPolicy::CleanRetryMs, result);
        return;
    }
    if (m_debug) {
        qDebug() << "ereader: completed adaptive Kobo Mini GC16 clean";
    }
}

void RefreshScheduler::resetCleaningDebt() {
    if (!m_policy.adaptiveCleaning) {
        return;
    }

    m_adaptiveClean.clear();
    m_changedPixelsSinceClean = 0;
    m_pixelDebt.fill(0);
    m_cleanCooldown.restart();
}

SubmitStatus
RefreshScheduler::sendUpdate(const QRect& region, WFM_MODE_INDEX_T waveform, bool flashing) {
    const QRect clipped = refreshableRegion(region, geometry());
    if (m_framebuffer.fd() < 0 || clipped.isEmpty()) {
        return fail("submit invalid refresh", -EINVAL);
    }
    // Touch C partial waveforms use the driver's histogram selection.
    if (m_policy.clearScene && !flashing && waveform == WFM_GC16) {
        waveform = WFM_AUTO;
    }
    const int result = m_framebuffer.submitUpdate(clipped, waveform, flashing, m_inverted);
    if (result == -EAGAIN || result == -EBUSY) {
        return SubmitStatus::Deferred;
    }
    if (result < 0) {
        return fail("submit refresh", result);
    }
    m_fence.submitted(m_framebuffer.lastMarker(), clipped);
    if (!m_submittingBatch) {
        m_fence.start(UpdateFence::Completion);
    }
    return SubmitStatus::Submitted;
}

void RefreshScheduler::stopRefreshTimers() {
    m_paletteRefresh.pause();
    m_partialRefresh.pause();
    m_qualityRefresh.pause();
    m_forcedQualityRefresh.pause();
    m_interactiveSettleTimer.stop();
    m_interactionPhase = interactive() ? InteractionPhase::Active : InteractionPhase::Idle;
    m_completionDrainTimer.stop();
    m_adaptiveClean.pause();
}

SubmitStatus RefreshScheduler::fail(const char* operation, int error) {
    if (m_lifecycle != Lifecycle::Failed) {
        qWarning() << "ereader:" << operation << "failed:" << error << std::strerror(-error);
    }
    m_lifecycle = Lifecycle::Failed;
    m_orientationTimer.stop();
    stopRefreshTimers();
    return SubmitStatus::Failed;
}

void RefreshScheduler::retry(RefreshWork& work, int delay, SubmitStatus result) {
    // Framebuffer adoption can cancel the request during the submission attempt.
    if (result != SubmitStatus::Deferred || suspended() || !work.pending()) {
        return;
    }
    if (m_fence.waiting()) {
        if (!work.scheduled()) {
            work.phase = RefreshWork::Phase::AwaitingFence;
        }
    } else if (frameNeedsComposition() || m_keyboard.phase == KeyboardPhase::AwaitingComposition) {
        if (!work.scheduled()) {
            work.phase = RefreshWork::Phase::AwaitingComposition;
        }
    } else {
        work.schedule(delay);
    }
}

void RefreshScheduler::resumePending() {
    if (m_fence.error() != 0) {
        fail("wait for refresh", m_fence.error());
        return;
    }
    if (suspended()) {
        return;
    }
    if (m_clearPhase != ClearPhase::Idle) {
        m_forcedQualityRefresh.schedule(0);
        return;
    }
    if (m_partialRefresh.pending() && !m_partialRefresh.scheduled()) {
        m_partialRefresh.schedule(0);
    }
    if (m_qualityRefresh.pending() && !m_qualityRefresh.scheduled()) {
        m_qualityRefresh.schedule(0);
    }
    if (m_forcedQualityRefresh.pending() && !m_forcedQualityRefresh.scheduled()) {
        m_forcedQualityRefresh.schedule(0);
    }
    if (m_paletteRefresh.pending() && !m_paletteRefresh.scheduled()) {
        m_paletteRefresh.schedule(0);
    }
    if (m_adaptiveClean.pending() && !m_adaptiveClean.scheduled()) {
        scheduleAdaptiveClean();
    }
}
