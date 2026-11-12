#include "ereaderscreen.h"

EreaderScreen::EreaderScreen(const PlatformOptions& options) : m_display(options, mScreenImage) {
    connect(
        &m_display,
        &RefreshScheduler::geometryChanged,
        this,
        [this](const QRect& geometry, const QSizeF& physicalSize) {
            mPhysicalSize = physicalSize;
            if (mScreenImage.isNull()) {
                mGeometry = geometry;
            } else {
                QFbScreen::setGeometry(geometry);
            }
        });
    connect(&m_display, &RefreshScheduler::repaintRequested, this, &QFbScreen::setDirty);
}

bool EreaderScreen::initialize() {
    mFormat = QImage::Format_RGB32;
    constexpr int CompositeDepth = 32;
    mDepth = CompositeDepth;
    if (!m_display.initialize()) {
        return false;
    }
    initializeCompositor();
    return true;
}

QRegion EreaderScreen::doRedraw() {
    if (m_display.suspended()) {
        QFbScreen::doRedraw();
        return {};
    }
    if (!m_display.synchronizeFramebuffer()) {
        return {};
    }
    return m_display.redraw(QFbScreen::doRedraw());
}
