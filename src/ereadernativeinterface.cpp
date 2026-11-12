#include "ereaderintegration.h"
#include "ereaderplatform.h"
#include "ereaderplatformfunctions.h"
#include "ereaderscreen.h"

#include <QtGui/private/qguiapplication_p.h>

RefreshScheduler& EreaderIntegration::display() {
    auto* integration =
        static_cast<EreaderIntegration*>(QGuiApplicationPrivate::platformIntegration());
    return *integration->m_primaryScreen->display();
}

QFunctionPointer EreaderIntegration::platformFunction(const QByteArray& function) const {
    if (function == EreaderPlatformFunctions::setPaletteLevelsIdentifier()) {
        return QFunctionPointer(setPaletteLevelsStatic);
    }
    if (function == EreaderPlatformFunctions::setInteractiveModeIdentifier()) {
        return QFunctionPointer(setInteractiveModeStatic);
    }
    if (function == EreaderPlatformFunctions::setTypingModeIdentifier()) {
        return QFunctionPointer(setTypingModeStatic);
    }
    if (function == EreaderPlatformFunctions::setInvertedIdentifier()) {
        return QFunctionPointer(setInvertedStatic);
    }
    if (function == EreaderPlatformFunctions::canHardwareInvertIdentifier()) {
        return QFunctionPointer(canHardwareInvertStatic);
    }
    if (function == EreaderPlatformFunctions::setRotationIdentifier()) {
        return QFunctionPointer(setRotationStatic);
    }
    if (function == EreaderPlatformFunctions::requestQualityRefreshIdentifier()) {
        return QFunctionPointer(requestQualityRefreshStatic);
    }
    if (function == EreaderPlatformFunctions::requestTransitionRefreshIdentifier()) {
        return QFunctionPointer(requestTransitionRefreshStatic);
    }
    if (function == EreaderPlatformFunctions::requestFullRefreshIdentifier()) {
        return QFunctionPointer(requestFullRefreshStatic);
    }
    if (function == EreaderPlatformFunctions::waitForRefreshIdentifier()) {
        return QFunctionPointer(waitForRefreshStatic);
    }

    if (function == EreaderPlatformFunctions::setKeyboardRegionsIdentifier()) {
        return QFunctionPointer(setKeyboardRegionsStatic);
    }
    if (function == EreaderPlatformFunctions::keyboardFrameIdentifier()) {
        return QFunctionPointer(keyboardFrameStatic);
    }
    if (function == EreaderPlatformFunctions::keyboardRegionReadyIdentifier()) {
        return QFunctionPointer(keyboardRegionReadyStatic);
    }
    if (function == EreaderPlatformFunctions::keyboardFeedbackDelayIdentifier()) {
        return QFunctionPointer(keyboardFeedbackDelayStatic);
    }

    return nullptr;
}

void EreaderIntegration::setPaletteLevelsStatic(int levels) {
    display().setPaletteLevels(levels);
}

void EreaderIntegration::setInteractiveModeStatic(bool active) {
    display().setInteractiveMode(active);
}

void EreaderIntegration::setInvertedStatic(bool enabled) {
    display().setInverted(enabled);
}

void EreaderIntegration::setTypingModeStatic(bool active) {
    display().setTypingMode(active);
}

bool EreaderIntegration::canHardwareInvertStatic() {
    return display().canHardwareInvert();
}

bool EreaderIntegration::setRotationStatic(int degrees) {
    return display().setRotation(degrees);
}

void EreaderIntegration::requestQualityRefreshStatic(int left, int top, int width, int height) {
    display().requestQualityRefresh(QRect(left, top, width, height));
}

void EreaderIntegration::requestFullRefreshStatic() {
    display().requestFullRefresh();
}

void EreaderIntegration::requestTransitionRefreshStatic(bool inverted) {
    display().requestTransitionRefresh(inverted);
}

bool EreaderIntegration::waitForRefreshStatic() {
    return display().waitForRefresh();
}

extern "C" Q_DECL_EXPORT void ereader_set_palette_levels(int levels) {
    EreaderPlatformFunctions::setPaletteLevels(levels);
}

extern "C" Q_DECL_EXPORT void ereader_set_interactive_mode(bool active) {
    EreaderPlatformFunctions::setInteractiveMode(active);
}

extern "C" Q_DECL_EXPORT void ereader_set_inverted(bool enabled) {
    EreaderPlatformFunctions::setInverted(enabled);
}

extern "C" Q_DECL_EXPORT void ereader_set_typing_mode(bool active) {
    EreaderPlatformFunctions::setTypingMode(active);
}

extern "C" Q_DECL_EXPORT bool ereader_can_hw_invert() {
    return EreaderPlatformFunctions::canHardwareInvert();
}

extern "C" Q_DECL_EXPORT bool ereader_set_rotation(int degrees) {
    return EreaderPlatformFunctions::setRotation(degrees);
}

extern "C" Q_DECL_EXPORT void
ereader_request_quality_refresh(int left, int top, int width, int height) {
    EreaderPlatformFunctions::requestQualityRefresh(left, top, width, height);
}

extern "C" Q_DECL_EXPORT void ereader_request_full_refresh() {
    EreaderPlatformFunctions::requestFullRefresh();
}

extern "C" Q_DECL_EXPORT void ereader_request_transition_refresh(bool inverted) {
    EreaderPlatformFunctions::requestTransitionRefresh(inverted);
}

extern "C" Q_DECL_EXPORT bool ereader_wait_for_refresh() {
    return EreaderPlatformFunctions::waitForRefresh();
}

void EreaderIntegration::setKeyboardRegionsStatic(const int* rectangles, int count) {
    if (count < 0 || (count > 0 && (rectangles == nullptr))) {
        qWarning("ereader: invalid keyboard region array");
        return;
    }
    QVector<QRect> regions;
    regions.reserve(count);
    for (int i = 0; i < count; ++i) {
        const int* rectangle = rectangles + qsizetype(i) * 4;
        regions.append(QRect(rectangle[0], rectangle[1], rectangle[2], rectangle[3]));
    }
    display().setKeyboardRegions(regions);
}

quint64 EreaderIntegration::keyboardFrameStatic() {
    return display().keyboardFrame();
}

bool EreaderIntegration::keyboardRegionReadyStatic(
    int left, int top, int width, int height, quint64 frame) {
    return display().keyboardRegionReady(QRect(left, top, width, height), frame);
}

int EreaderIntegration::keyboardFeedbackDelayStatic() {
    return display().keyboardFeedbackDelay();
}

extern "C" Q_DECL_EXPORT void ereader_set_keyboard_regions(const int* rectangles, int count) {
    EreaderPlatformFunctions::setKeyboardRegions(rectangles, count);
}

extern "C" Q_DECL_EXPORT quint64 ereader_keyboard_frame() {
    return EreaderPlatformFunctions::keyboardFrame();
}

extern "C" Q_DECL_EXPORT bool
ereader_keyboard_region_ready(int left, int top, int width, int height, quint64 frame) {
    return EreaderPlatformFunctions::keyboardRegionReady(left, top, width, height, frame);
}

extern "C" Q_DECL_EXPORT int ereader_keyboard_feedback_delay() {
    return EreaderPlatformFunctions::keyboardFeedbackDelay();
}
