#pragma once
#include "refreshscheduler.h"

#include <QtFbSupport/private/qfbscreen_p.h>

class EreaderScreen : public QFbScreen {
public:
    explicit EreaderScreen(const PlatformOptions& options);
    bool initialize() override;
    QRegion doRedraw() override;

    RefreshScheduler* display() {
        return &m_display;
    }

private:
    RefreshScheduler m_display;
};
