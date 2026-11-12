#pragma once
#include "fbink.h"

struct RefreshPolicy {
    static RefreshPolicy forDevice(const FBInkState& state);
    bool adaptiveCleaning = false;
    bool clearScene = false;
    int keyboardFeedbackDelay = 0;

    static constexpr int CleanIdleMs = 750;
    static constexpr int CleanCooldownMs = 30000;
    static constexpr int CleanRetryMs = 2000;
    static constexpr int HotPixelThreshold = 10;
    static constexpr int ScreenEquivalentThreshold = 3;
};
