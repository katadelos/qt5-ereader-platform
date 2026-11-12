#include "refreshpolicy.h"

namespace {
constexpr int GrayscaleDepth = 8;
constexpr int LegacyKeyboardFeedbackDelayMs = 120;
}

RefreshPolicy RefreshPolicy::forDevice(const FBInkState& state) {
    RefreshPolicy policy;
    const bool kobo = fbink_target() == FBINK_TARGET_KOBO;
    policy.adaptiveCleaning =
        kobo && state.device_id == DEVICE_KOBO_MINI && state.bpp == GrayscaleDepth;
    policy.clearScene = kobo && state.device_id == DEVICE_KOBO_TOUCH_C;
    policy.keyboardFeedbackDelay = state.is_kindle_legacy ? LegacyKeyboardFeedbackDelayMs : 0;
    return policy;
}
