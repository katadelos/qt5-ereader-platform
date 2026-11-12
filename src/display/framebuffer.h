#pragma once
#include "fbink.h"

#include <QImage>
#include <QRegion>

struct PlatformOptions;

// Owns the FBInk descriptor, mapped image and device configuration. The caller
// must drain outstanding source/panel fences before changing or closing it.
class Framebuffer {
public:
    Framebuffer() = default;
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    ~Framebuffer();

    bool initialize(const PlatformOptions& options);
    int inspect(FBInkState& state, bool& changed);
    int adopt(const FBInkState& state);
    int setRotation(int degrees);
    int submitUpdate(const QRect& region, WFM_MODE_INDEX_T waveform, bool flashing, bool inverted);
    int write(const QImage& image, const QRegion& region, bool inverted);
    QImage snapshot(bool inverted) const;
    void fillWhite();
    void configurePowerdownDelay();
    void restorePowerdownDelay();

    bool isValid() const {
        return m_fd >= 0 && !m_image.isNull();
    }

    int fd() const {
        return m_fd;
    }

    const FBInkState& state() const {
        return m_state;
    }

    QRect geometry() const {
        return m_image.rect();
    }

    QSizeF physicalSize() const {
        return m_physicalSize;
    }

    uint32_t lastMarker() const {
        return m_lastMarker;
    }

private:
    static QImage::Format imageFormat(const FBInkState& state);
    bool softwareInverted(bool inverted) const;
    int map(const FBInkState& state);

    int m_fd = -1;
    QImage m_image;
    QSizeF m_physicalSize;
    FBInkConfig m_config = {};
    FBInkState m_state = {};
    uint32_t m_lastMarker = 0;
    int m_originalBpp = 0;
    int m_originalRotation = 0;
    int m_originalGrayscale = 0;
    bool m_rotationRequested = false;
    int m_originalPowerdownDelay = 0;
    bool m_hasOriginalPowerdownDelay = false;
};
