#include "framebuffer.h"

#include "platformarguments.h"

#include <QDebug>
#include <QPainter>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <linux/fb.h>
#include <sys/ioctl.h>

namespace {
constexpr int MaximumPhysicalSizeMm = 1000;
constexpr int QuarterTurnDegrees = 90;
constexpr unsigned char InversionMask = 0xff;
constexpr int EpdcPowerdownDelayMs = 5000;
constexpr unsigned long MxcfbSetPowerdownDelay = _IOW('F', 0x30, int32_t);
constexpr unsigned long MxcfbGetPowerdownDelay = _IOR('F', 0x31, int32_t);
}

QImage::Format Framebuffer::imageFormat(const FBInkState& state) {
    // FBInk resolves device quirks, including empty kernel channel bitfields.
    switch (state.pixel_format) {
    case FBINK_PXFMT_Y8:
        return QImage::Format_Grayscale8;
    case FBINK_PXFMT_BGR565:
        return QImage::Format_RGB16;
    case FBINK_PXFMT_BGR24:
        return QImage::Format_BGR888;
    case FBINK_PXFMT_RGB24:
        return QImage::Format_RGB888;
    case FBINK_PXFMT_BGRA:
        return QImage::Format_ARGB32;
    case FBINK_PXFMT_RGBA:
        return QImage::Format_RGBA8888;
    case FBINK_PXFMT_BGR32:
        return QImage::Format_RGB32;
    case FBINK_PXFMT_RGB32:
        return QImage::Format_RGBX8888;
    default:
        return QImage::Format_Invalid;
    }
}

bool Framebuffer::initialize(const PlatformOptions& options) {
    m_config.is_verbose = options.debug;
    m_config.is_quiet = !options.debug;
    m_fd = fbink_open();
    if (m_fd < 0) {
        qWarning() << "ereader: unable to open framebuffer through FBInk";
        return false;
    }
    if (fbink_init(m_fd, &m_config) < 0) {
        qWarning() << "ereader: unable to initialize FBInk";
        return false;
    }

    fb_var_screeninfo varInfo = {};
    fb_fix_screeninfo fixInfo = {};
    fbink_get_fb_info(&varInfo, &fixInfo);
    m_originalBpp = static_cast<int>(varInfo.bits_per_pixel);
    m_originalRotation = static_cast<int>(varInfo.rotate);
    m_originalGrayscale = static_cast<int>(varInfo.grayscale);

    if (options.rotation != StartupRotation::Unchanged) {
        // Native rotation numbers vary by model; FBInk must identify it first.
        const uint32_t rotation =
            fbink_rota_canonical_to_native(static_cast<uint8_t>(options.rotation));
        if (rotation > 3) {
            return false;
        }
        m_rotationRequested = true;
        const int result = fbink_set_fb_info(
            m_fd, rotation, KEEP_CURRENT_BITDEPTH, KEEP_CURRENT_GRAYSCALE, &m_config);
        if (result < 0) {
            qWarning() << "ereader: unable to configure framebuffer rotation" << result;
            return false;
        }
    }

    FBInkState state = {};
    fbink_get_state(&m_config, &state);
    if (map(state) < 0) {
        return false;
    }

    fbink_get_fb_info(&varInfo, &fixInfo);
    m_physicalSize = QSizeF(varInfo.width, varInfo.height);
    if (m_physicalSize.width() <= 0 || m_physicalSize.height() <= 0 ||
        m_physicalSize.width() > MaximumPhysicalSizeMm ||
        m_physicalSize.height() > MaximumPhysicalSizeMm) {
        if (m_state.screen_dpi == 0) {
            qWarning() << "ereader: panel density is unavailable";
            return false;
        }
        const qreal millimetersPerPixel = 25.4 / m_state.screen_dpi;
        m_physicalSize = QSizeF(
            geometry().width() * millimetersPerPixel, geometry().height() * millimetersPerPixel);
    }
    return true;
}

int Framebuffer::map(const FBInkState& state) {
    const QImage::Format format = imageFormat(state);
    if (format == QImage::Format_Invalid || (state.screen_width == 0U) ||
        (state.screen_height == 0U) || (state.scanline_stride == 0U)) {
        qWarning() << "ereader: unsupported framebuffer layout";
        return -EINVAL;
    }
    std::size_t bufferSize = 0;
    unsigned char* memory = fbink_get_fb_pointer(m_fd, &bufferSize);
    const std::size_t visibleSize = static_cast<std::size_t>(state.scanline_stride) *
                                    static_cast<std::size_t>(state.screen_height);
    if ((memory == nullptr) || bufferSize < visibleSize) {
        qWarning() << "ereader: unable to map framebuffer through FBInk";
        return -ENOMEM;
    }
    QImage mapped(
        memory,
        static_cast<int>(state.screen_width),
        static_cast<int>(state.screen_height),
        static_cast<int>(state.scanline_stride),
        format);
    if (mapped.isNull()) {
        return -EINVAL;
    }

    m_image = mapped;
    m_state = state;
    return 0;
}

int Framebuffer::inspect(FBInkState& state, bool& changed) {
    changed = false;
    if (!isValid()) {
        return -ENODEV;
    }
    const int result = fbink_reinit(m_fd, &m_config);
    if (result < 0) {
        return result;
    }
    fbink_get_state(&m_config, &state);
    if (state.current_rota == m_state.current_rota && state.screen_width == m_state.screen_width &&
        state.screen_height == m_state.screen_height &&
        state.scanline_stride == m_state.scanline_stride &&
        state.pixel_format == m_state.pixel_format) {
        return 0;
    }
    changed = true;
    return 0;
}

int Framebuffer::adopt(const FBInkState& state) {
    const bool wasPortrait = geometry().width() < geometry().height();
    const int mapResult = map(state);
    if (mapResult < 0) {
        // FBInk may already have replaced its mapping. Never expose a stale image.
        m_image = {};
        return mapResult;
    }
    if (wasPortrait != (geometry().width() < geometry().height())) {
        m_physicalSize.transpose();
    }
    return 0;
}

int Framebuffer::setRotation(int degrees) {
    if (!isValid()) {
        return -ENODEV;
    }
    if (degrees < 0 || degrees > 3 * QuarterTurnDegrees || degrees % QuarterTurnDegrees != 0) {
        return -EINVAL;
    }
    // The API measures clockwise from landscape; upright portrait is 270.
    const uint32_t rotation =
        fbink_rota_canonical_to_native((degrees / QuarterTurnDegrees + 1) % 4);
    if (rotation > 3) {
        return -EINVAL;
    }
    if (rotation == m_state.current_rota) {
        return 0;
    }
    m_rotationRequested = true;
    return fbink_set_fb_info(
        m_fd, rotation, KEEP_CURRENT_BITDEPTH, KEEP_CURRENT_GRAYSCALE, &m_config);
}

bool Framebuffer::softwareInverted(bool inverted) const {
    // eink_fb uses inverse luminance. Output and shadow remain in logical colors.
    return m_state.is_kindle_legacy != (inverted && !m_state.can_hw_invert);
}

QImage Framebuffer::snapshot(bool inverted) const {
    QImage image = m_image.copy();
    if (softwareInverted(inverted)) {
        image.invertPixels();
    }
    return image;
}

int Framebuffer::write(const QImage& image, const QRegion& region, bool inverted) {
    if (!isValid()) {
        return -ENODEV;
    }
    if (image.size() != m_image.size() || image.format() != m_image.format()) {
        return -EINVAL;
    }
    const QRegion clipped = region.intersected(geometry());
    const bool invert = softwareInverted(inverted);
    if (m_image.format() == QImage::Format_Grayscale8) {
        // Combine native eink_fb luminance inversion with the source copy.
        for (const QRect& rect : clipped) {
            for (int row = rect.top(); row <= rect.bottom(); ++row) {
                const uchar* source = image.constScanLine(row) + rect.left();
                uchar* destination = m_image.scanLine(row) + rect.left();
                if (invert) {
                    for (int column = 0; column < rect.width(); ++column) {
                        destination[column] = source[column] ^ InversionMask;
                    }
                } else {
                    std::memcpy(destination, source, static_cast<std::size_t>(rect.width()));
                }
            }
        }
        return 0;
    }

    QPainter painter(&m_image);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const QRect& rect : clipped) {
        painter.drawImage(rect, image, rect);
    }
    painter.end();
    if (!invert) {
        return 0;
    }

    // Rectangles are in native coordinates; bypass FBInk's additional rotation.
    for (const QRect& rect : clipped) {
        const FBInkRect native = {
            static_cast<unsigned short>(rect.left()),
            static_cast<unsigned short>(rect.top()),
            static_cast<unsigned short>(rect.width()),
            static_cast<unsigned short>(rect.height())};
        const int result = fbink_invert_rect(m_fd, &native, true);
        if (result < 0) {
            return result;
        }
    }
    return 0;
}

void Framebuffer::fillWhite() {
    m_image.fill(m_state.is_kindle_legacy ? Qt::black : Qt::white);
}

int Framebuffer::submitUpdate(
    const QRect& region, WFM_MODE_INDEX_T waveform, bool flashing, bool inverted) {
    if (!isValid()) {
        return -ENODEV;
    }
    const QRect clipped = region.intersected(geometry());
    if (clipped.isEmpty()) {
        return -EINVAL;
    }
    m_config.wfm_mode = waveform;
    m_config.is_flashing = flashing;
    m_config.is_nightmode = inverted && m_state.can_hw_invert;
    const int result = fbink_refresh(
        m_fd,
        static_cast<uint32_t>(clipped.top()),
        static_cast<uint32_t>(clipped.left()),
        static_cast<uint32_t>(clipped.width()),
        static_cast<uint32_t>(clipped.height()),
        &m_config);
    if (result >= 0) {
        m_lastMarker = fbink_get_last_marker();
    }
    return result;
}

void Framebuffer::restorePowerdownDelay() {
    if (!m_hasOriginalPowerdownDelay) {
        return;
    }
    int32_t delay = m_originalPowerdownDelay;
    if (ioctl(m_fd, MxcfbSetPowerdownDelay, &delay) < 0 && m_config.is_verbose) {
        qWarning() << "ereader: unable to restore EPDC power-down delay";
    }
    m_hasOriginalPowerdownDelay = false;
}

void Framebuffer::configurePowerdownDelay() {
    // Preserve HWTCON power policy. eink_fb has no MXCFB power-delay interface.
    if (m_state.is_mtk || m_state.is_kindle_legacy || m_hasOriginalPowerdownDelay) {
        return;
    }
    int32_t delay = 0;
    if (ioctl(m_fd, MxcfbGetPowerdownDelay, &delay) < 0) {
        if (m_config.is_verbose) {
            qWarning() << "ereader: unable to read EPDC power-down delay";
        }
        return;
    }
    m_originalPowerdownDelay = delay;
    m_hasOriginalPowerdownDelay = true;
    // Keep EPDC rails alive across interaction and its quality cleanup.
    delay = EpdcPowerdownDelayMs;
    if (ioctl(m_fd, MxcfbSetPowerdownDelay, &delay) < 0 && m_config.is_verbose) {
        qWarning() << "ereader: unable to set EPDC power-down delay";
    }
}

Framebuffer::~Framebuffer() {
    if (m_fd < 0) {
        return;
    }
    restorePowerdownDelay();
    if (m_rotationRequested && m_originalBpp > 0) {
        const int result = fbink_set_fb_info(
            m_fd,
            static_cast<uint32_t>(m_originalRotation),
            static_cast<uint8_t>(m_originalBpp),
            static_cast<uint8_t>(m_originalGrayscale),
            &m_config);
        if (result < 0 && m_config.is_verbose) {
            qWarning() << "ereader: unable to restore framebuffer state" << result;
        }
    }
    m_image = {};
    fbink_close(m_fd);
}
