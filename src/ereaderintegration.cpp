#include "ereaderintegration.h"

#include "ereaderscreen.h"
#include "evdevbuttons.h"
#include "evdevtouch.h"
#include "nickelsleepsession.h"

#include <QtEventDispatcherSupport/private/qgenericunixeventdispatcher_p.h>
#include <QtFbSupport/private/qfbbackingstore_p.h>
#include <QtFbSupport/private/qfbwindow_p.h>
#include <QtFontDatabaseSupport/private/qgenericunixfontdatabase_p.h>
#include <QtServiceSupport/private/qgenericunixservices_p.h>
#include <qpa/qplatforminputcontext.h>
#include <qpa/qplatforminputcontextfactory_p.h>
#include <qpa/qwindowsysteminterface.h>

EreaderIntegration::EreaderIntegration(const PlatformOptions& options)
    : m_options(options), m_primaryScreen(new EreaderScreen(options)),
      m_fontDb(new QGenericUnixFontDatabase), m_services(new QGenericUnixServices) {}

EreaderIntegration::~EreaderIntegration() {
    if (m_screenRegistered) {
        // Qt deletes both QScreen and QPlatformScreen in handleScreenRemoved.
        // Relinquish ownership so QScopedPointer does not free the screen twice.
        QWindowSystemInterface::handleScreenRemoved(m_primaryScreen.take());
    }
}

void EreaderIntegration::initialize() {
    if (m_primaryScreen->initialize()) {
        QWindowSystemInterface::handleScreenAdded(m_primaryScreen.data());
        m_screenRegistered = true;
        auto* display = m_primaryScreen->display();
        auto* touch = new EvdevTouch(m_options.touch, display->framebufferState(), this);
        QObject::connect(
            display, &RefreshScheduler::framebufferChanged, touch, &EvdevTouch::setScreenState);
        QObject::connect(
            touch,
            &EvdevTouch::aboutToReadTouch,
            display,
            &RefreshScheduler::synchronizeFramebuffer);
        QObject::connect(
            touch, &EvdevTouch::touchActiveChanged, display, &RefreshScheduler::setInteractiveMode);
        auto* buttons = new EvdevButtons(display->framebufferState(), this);
        QObject::connect(
            display, &RefreshScheduler::framebufferChanged, buttons, &EvdevButtons::setScreenState);
        QObject::connect(
            buttons,
            &EvdevButtons::aboutToReadButtons,
            display,
            &RefreshScheduler::synchronizeFramebuffer);
        if ((qgetenv("K2_STOCK_DISPLAY") == "nickel") && qgetenv("K2_ALLOW_SLEEP") == "1") {
            new NickelSleepSession(display, touch, buttons, this);
        }
    } else {
        qWarning("ereader: failed to initialize framebuffer screen");
    }

    m_inputContext.reset(QPlatformInputContextFactory::create());
}

bool EreaderIntegration::hasCapability(QPlatformIntegration::Capability cap) const {
    switch (cap) {
    case ThreadedPixmaps:
    case WindowManagement:
        return false;
    default:
        return QPlatformIntegration::hasCapability(cap);
    }
}

QPlatformWindow* EreaderIntegration::createPlatformWindow(QWindow* window) const {
    return new QFbWindow(window);
}

QPlatformBackingStore* EreaderIntegration::createPlatformBackingStore(QWindow* window) const {
    return new QFbBackingStore(window);
}

QAbstractEventDispatcher* EreaderIntegration::createEventDispatcher() const {
    return createUnixEventDispatcher();
}

QPlatformFontDatabase* EreaderIntegration::fontDatabase() const {
    return m_fontDb.data();
}

QPlatformServices* EreaderIntegration::services() const {
    return m_services.data();
}

QPlatformNativeInterface* EreaderIntegration::nativeInterface() const {
    return const_cast<EreaderIntegration*>(this);
}

QList<QPlatformScreen*> EreaderIntegration::screens() const {
    return {m_primaryScreen.data()};
}
