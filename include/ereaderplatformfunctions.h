#ifndef EREADERPLATFORMFUNCTIONS_H
#define EREADERPLATFORMFUNCTIONS_H

#include <QtCore/QByteArray>
#include <QtGui/QGuiApplication>

// GUI-thread API. Rectangles are framebuffer pixels in the current orientation.
class EreaderPlatformFunctions {
public:
    using PaletteAction = void (*)(int);
    using InteractiveAction = void (*)(bool);
    using TypingAction = void (*)(bool);
    using InvertedAction = void (*)(bool);
    using HardwareInversionCapability = bool (*)();
    using RotationAction = bool (*)(int);
    using QualityRefreshAction = void (*)(int, int, int, int);
    using TransitionRefreshAction = void (*)(bool);
    using FullRefreshAction = void (*)();
    using WaitForRefreshAction = bool (*)();

    static QByteArray keyboardFeedbackDelayIdentifier() {
        return QByteArrayLiteral("keyboardFeedbackDelay");
    }

    static int keyboardFeedbackDelay() {
        using Function = int (*)();
        const auto function = reinterpret_cast<Function>(
            QGuiApplication::platformFunction(keyboardFeedbackDelayIdentifier()));
        return (function != nullptr) ? function() : 0;
    }

    static QByteArray keyboardRegionReadyIdentifier() {
        return QByteArrayLiteral("keyboardRegionReady");
    }

    static bool keyboardRegionReady(int left, int top, int width, int height, quint64 frame) {
        using Function = bool (*)(int, int, int, int, quint64);
        const auto function = reinterpret_cast<Function>(
            QGuiApplication::platformFunction(keyboardRegionReadyIdentifier()));
        return (function != nullptr) ? function(left, top, width, height, frame) : false;
    }

    static QByteArray keyboardFrameIdentifier() {
        return QByteArrayLiteral("keyboardFrame");
    }

    static quint64 keyboardFrame() {
        using Function = quint64 (*)();
        const auto function = reinterpret_cast<Function>(
            QGuiApplication::platformFunction(keyboardFrameIdentifier()));
        return (function != nullptr) ? function() : 0;
    }

    static QByteArray setKeyboardRegionsIdentifier() {
        return QByteArrayLiteral("setKeyboardRegions");
    }

    static void setKeyboardRegions(const int* rectangles, int count) {
        using Function = void (*)(const int*, int);
        const auto function = reinterpret_cast<Function>(
            QGuiApplication::platformFunction(setKeyboardRegionsIdentifier()));
        if (function != nullptr) {
            function(rectangles, count);
        }
    }

    static QByteArray setPaletteLevelsIdentifier() {
        return QByteArrayLiteral("setPaletteLevels");
    }

    static void setPaletteLevels(int levels) {
        const auto function = reinterpret_cast<PaletteAction>(
            QGuiApplication::platformFunction(setPaletteLevelsIdentifier()));
        if (function != nullptr) {
            function(levels);
        }
    }

    static QByteArray setInteractiveModeIdentifier() {
        return QByteArrayLiteral("setInteractiveMode");
    }

    static void setInteractiveMode(bool active) {
        const auto function = reinterpret_cast<InteractiveAction>(
            QGuiApplication::platformFunction(setInteractiveModeIdentifier()));
        if (function != nullptr) {
            function(active);
        }
    }

    static QByteArray setTypingModeIdentifier() {
        return QByteArrayLiteral("setTypingMode");
    }

    static void setTypingMode(bool active) {
        const auto function = reinterpret_cast<TypingAction>(
            QGuiApplication::platformFunction(setTypingModeIdentifier()));
        if (function != nullptr) {
            function(active);
        }
    }

    static QByteArray canHardwareInvertIdentifier() {
        return QByteArrayLiteral("canHardwareInvert");
    }

    static bool canHardwareInvert() {
        const auto function = reinterpret_cast<HardwareInversionCapability>(
            QGuiApplication::platformFunction(canHardwareInvertIdentifier()));
        return (function != nullptr) && function();
    }

    static QByteArray setInvertedIdentifier() {
        return QByteArrayLiteral("setInverted");
    }

    static void setInverted(bool enabled) {
        const auto function = reinterpret_cast<InvertedAction>(
            QGuiApplication::platformFunction(setInvertedIdentifier()));
        if (function != nullptr) {
            function(enabled);
        }
    }

    static QByteArray setRotationIdentifier() {
        return QByteArrayLiteral("setRotation");
    }

    // Clockwise degrees from clockwise landscape: 0, 90, 180 or 270.
    // Upright portrait is 270. Invalid values return false.
    static bool setRotation(int degrees) {
        const auto function = reinterpret_cast<RotationAction>(
            QGuiApplication::platformFunction(setRotationIdentifier()));
        return (function != nullptr) ? function(degrees) : false;
    }

    static QByteArray requestQualityRefreshIdentifier() {
        return QByteArrayLiteral("requestQualityRefresh");
    }

    static void requestQualityRefresh(int left, int top, int width, int height) {
        const auto function = reinterpret_cast<QualityRefreshAction>(
            QGuiApplication::platformFunction(requestQualityRefreshIdentifier()));
        if (function != nullptr) {
            function(left, top, width, height);
        }
    }

    static QByteArray requestTransitionRefreshIdentifier() {
        return QByteArrayLiteral("requestTransitionRefresh");
    }

    static void requestTransitionRefresh(bool inverted) {
        const auto function = reinterpret_cast<TransitionRefreshAction>(
            QGuiApplication::platformFunction(requestTransitionRefreshIdentifier()));
        if (function != nullptr) {
            function(inverted);
        }
    }

    static QByteArray requestFullRefreshIdentifier() {
        return QByteArrayLiteral("requestFullRefresh");
    }

    static void requestFullRefresh() {
        const auto function = reinterpret_cast<FullRefreshAction>(
            QGuiApplication::platformFunction(requestFullRefreshIdentifier()));
        if (function != nullptr) {
            function();
        }
    }

    static QByteArray waitForRefreshIdentifier() {
        return QByteArrayLiteral("waitForRefresh");
    }

    // Wait for submitted updates and finish an in-flight clear. Queued Qt paints are not flushed.
    static bool waitForRefresh() {
        const auto function = reinterpret_cast<WaitForRefreshAction>(
            QGuiApplication::platformFunction(waitForRefreshIdentifier()));
        return (function != nullptr) ? function() : false;
    }
};

#endif // EREADERPLATFORMFUNCTIONS_H
