#include "displaybuffer.h"

#include <QPainter>
#include <cstring>
#include <utility>

namespace {
constexpr int GrayscaleLevels = 256;
constexpr int MaximumGray = GrayscaleLevels - 1;

std::pair<int, int> changedRowSpan(
    const unsigned char* current,
    const unsigned char* previous,
    int pixelCount,
    int bytesPerPixel) {
    int first = 0;
    int last = pixelCount - 1;
    if (bytesPerPixel == 1) {
        // Avoid a variable-size memcmp call for every grayscale pixel.
        while (first < pixelCount && current[first] == previous[first]) {
            ++first;
        }
        while (last > first && current[last] == previous[last]) {
            --last;
        }
    } else {
        while (first < pixelCount && std::memcmp(
                                         current + first * bytesPerPixel,
                                         previous + first * bytesPerPixel,
                                         static_cast<std::size_t>(bytesPerPixel)) == 0) {
            ++first;
        }
        while (last > first && std::memcmp(
                                   current + last * bytesPerPixel,
                                   previous + last * bytesPerPixel,
                                   static_cast<std::size_t>(bytesPerPixel)) == 0) {
            --last;
        }
    }

    return {first, last};
}

}

bool DisplayBuffer::reset(bool inverted) {
    QImage output = m_framebuffer.snapshot(inverted);
    QImage shadow = output.copy();
    if (output.isNull() || shadow.isNull()) {
        return false;
    }
    m_output = output;
    m_shadow = shadow;
    return true;
}

bool DisplayBuffer::resetShadow(bool inverted) {
    QImage shadow = m_framebuffer.snapshot(inverted);
    if (shadow.isNull() || shadow.size() != m_output.size() ||
        shadow.format() != m_output.format()) {
        return false;
    }
    m_shadow = shadow;
    return true;
}

bool DisplayBuffer::configurePalette(int levels) {
    if (levels == 0) {
        m_paletteLevels = 0;
        m_paletteLut.clear();
        return true;
    }
    if (levels < 2 || levels > GrayscaleLevels) {
        return false;
    }

    m_paletteLevels = levels;
    m_paletteLut.resize(GrayscaleLevels);
    for (int value = 0; value < GrayscaleLevels; ++value) {
        const int index = (value * (levels - 1) + 127) / MaximumGray;
        m_paletteLut[value] =
            static_cast<quint8>((index * MaximumGray + (levels - 1) / 2) / (levels - 1));
    }
    return true;
}

void DisplayBuffer::composeOutput(const QRegion& region) {
    const QRegion clippedRegion = region.intersected(geometry());
    if (m_output.format() == QImage::Format_Grayscale8) {
        // Qt still composes glyphs and SVG alpha masks in opaque RGB32.
        // Use QPainter's same qGray conversion without its generic raster
        // pipeline, folding the optional output palette into this pass.
        const quint8* palette = m_paletteLut.isEmpty() ? nullptr : m_paletteLut.constData();
        for (const QRect& rect : clippedRegion) {
            for (int row = rect.top(); row <= rect.bottom(); ++row) {
                const QRgb* source = reinterpret_cast<const QRgb*>(m_composite.constScanLine(row));
                uchar* destination = m_output.scanLine(row);
                for (int column = rect.left(); column <= rect.right(); ++column) {
                    const int gray = qGray(source[column]);
                    destination[column] = (palette != nullptr) ? palette[gray] : gray;
                }
            }
        }
        return;
    }

    QPainter blitter(&m_output);
    blitter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const QRect& rect : clippedRegion) {
        blitter.drawImage(rect, m_composite, rect);
    }
}

QRect DisplayBuffer::changedBoundingRect(const QRect& region) const {
    const QRect clipped = region.intersected(geometry());
    if (clipped.isEmpty() || m_output.isNull() || m_shadow.isNull()) {
        return {};
    }

    const int bytesPerPixel = m_output.depth() / 8;
    const int rowBytes = clipped.width() * bytesPerPixel;
    int left = clipped.right() + 1;
    int right = clipped.left() - 1;
    int top = clipped.bottom() + 1;
    int bottom = clipped.top() - 1;

    for (int row = clipped.top(); row <= clipped.bottom(); ++row) {
        const unsigned char* current = m_output.constScanLine(row) + clipped.left() * bytesPerPixel;
        const unsigned char* previous =
            m_shadow.constScanLine(row) + clipped.left() * bytesPerPixel;
        if (std::memcmp(current, previous, static_cast<std::size_t>(rowBytes)) == 0) {
            continue;
        }

        const auto span = changedRowSpan(current, previous, clipped.width(), bytesPerPixel);

        left = qMin(left, clipped.left() + span.first);
        right = qMax(right, clipped.left() + span.second);
        top = qMin(top, row);
        bottom = row;
    }

    if (right < left || bottom < top) {
        return {};
    }
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

QRegion DisplayBuffer::changedRegion(const QRegion& region) const {
    QRegion changed;
    for (const QRect& rect : region) {
        const QRect bounds = changedBoundingRect(rect);
        if (!bounds.isEmpty()) {
            changed += bounds;
        }
    }
    return changed;
}

void DisplayBuffer::updateFramebufferShadow(const QRegion& region) {
    if (region.isEmpty()) {
        return;
    }

    const int bytesPerPixel = m_output.depth() / 8;
    for (const QRect& rect : region) {
        const QRect clipped = rect.intersected(geometry());
        if (clipped.isEmpty()) {
            continue;
        }
        const std::size_t rowBytes = static_cast<std::size_t>(clipped.width() * bytesPerPixel);
        for (int row = clipped.top(); row <= clipped.bottom(); ++row) {
            unsigned char* destination = m_shadow.scanLine(row) + clipped.left() * bytesPerPixel;
            const unsigned char* source =
                m_output.constScanLine(row) + clipped.left() * bytesPerPixel;
            std::memcpy(destination, source, rowBytes);
        }
    }
}

int DisplayBuffer::copyOutputToFramebuffer(const QRegion& region, bool inverted) {
    return m_framebuffer.write(m_output, region, inverted);
}
