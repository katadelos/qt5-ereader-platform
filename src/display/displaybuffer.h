#pragma once
#include "framebuffer.h"

#include <QRegion>
#include <QVector>

// Owns converted output and the last submitted logical pixels. Qt owns composite.
class DisplayBuffer {
public:
    DisplayBuffer(Framebuffer& framebuffer, const QImage& composite)
        : m_composite(composite), m_framebuffer(framebuffer) {}

    bool reset(bool inverted);
    bool resetShadow(bool inverted);
    bool configurePalette(int levels);
    void composeOutput(const QRegion& region);
    QRect changedBoundingRect(const QRect& region) const;
    QRegion changedRegion(const QRegion& region) const;
    void updateFramebufferShadow(const QRegion& region);
    int copyOutputToFramebuffer(const QRegion& region, bool inverted);

    const QImage& composite() const {
        return m_composite;
    }

    const QImage& output() const {
        return m_output;
    }

    const QImage& shadow() const {
        return m_shadow;
    }

    int paletteLevels() const {
        return m_paletteLevels;
    }

private:
    QRect geometry() const {
        return m_output.rect();
    }

    const QImage& m_composite;
    Framebuffer& m_framebuffer;
    QImage m_output;
    QImage m_shadow;
    int m_paletteLevels = 0;
    QVector<quint8> m_paletteLut;
};
