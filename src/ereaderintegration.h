#ifndef EREADERINTEGRATION_H
#define EREADERINTEGRATION_H

#include "platformarguments.h"

#include <QScopedPointer>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformnativeinterface.h>

class EreaderScreen;
class RefreshScheduler;

class EreaderIntegration : public QPlatformIntegration, public QPlatformNativeInterface {
public:
    explicit EreaderIntegration(const PlatformOptions& options);
    ~EreaderIntegration() override;

    void initialize() override;
    bool hasCapability(QPlatformIntegration::Capability cap) const override;

    QPlatformWindow* createPlatformWindow(QWindow* window) const override;
    QPlatformBackingStore* createPlatformBackingStore(QWindow* window) const override;
    QAbstractEventDispatcher* createEventDispatcher() const override;

    QPlatformFontDatabase* fontDatabase() const override;
    QPlatformServices* services() const override;

    QPlatformInputContext* inputContext() const override {
        return m_inputContext.data();
    }

    QPlatformNativeInterface* nativeInterface() const override;
    QList<QPlatformScreen*> screens() const;
    QFunctionPointer platformFunction(const QByteArray& function) const override;

private:
    static RefreshScheduler& display();
    static void setKeyboardRegionsStatic(const int* rectangles, int count);
    static quint64 keyboardFrameStatic();
    static bool keyboardRegionReadyStatic(int left, int top, int width, int height, quint64 frame);
    static int keyboardFeedbackDelayStatic();

    static void setPaletteLevelsStatic(int levels);
    static void setInteractiveModeStatic(bool active);
    static void setTypingModeStatic(bool active);
    static void setInvertedStatic(bool enabled);
    static bool canHardwareInvertStatic();
    static bool setRotationStatic(int degrees);
    static void requestQualityRefreshStatic(int left, int top, int width, int height);
    static void requestTransitionRefreshStatic(bool inverted);
    static void requestFullRefreshStatic();
    static bool waitForRefreshStatic();

    PlatformOptions m_options;
    QScopedPointer<EreaderScreen> m_primaryScreen;
    bool m_screenRegistered = false;
    QScopedPointer<QPlatformInputContext> m_inputContext;
    QScopedPointer<QPlatformFontDatabase> m_fontDb;
    QScopedPointer<QPlatformServices> m_services;
};

#endif // EREADERINTEGRATION_H
