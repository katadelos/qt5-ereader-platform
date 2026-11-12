#pragma once
#include <QtCore/qglobal.h>

// Call on the GUI thread after QGuiApplication has initialized the platform.
// Rectangles use framebuffer pixels in the current orientation.
extern "C" {
Q_DECL_EXPORT void ereader_set_palette_levels(int levels);
Q_DECL_EXPORT void ereader_set_interactive_mode(bool active);
Q_DECL_EXPORT void ereader_set_inverted(bool enabled);
Q_DECL_EXPORT void ereader_set_typing_mode(bool active);
Q_DECL_EXPORT bool ereader_can_hw_invert();
// Clockwise degrees from clockwise landscape: 0, 90, 180 or 270.
// Upright portrait is 270. Invalid values return false.
Q_DECL_EXPORT bool ereader_set_rotation(int degrees);
Q_DECL_EXPORT void ereader_request_quality_refresh(int left, int top, int width, int height);
Q_DECL_EXPORT void ereader_request_full_refresh();
Q_DECL_EXPORT void ereader_request_transition_refresh(bool inverted);
// Wait for submitted updates and finish an in-flight clear. Queued Qt paints are not flushed.
Q_DECL_EXPORT bool ereader_wait_for_refresh();
// Each rectangle is four consecutive integers: x, y, width, height.
// A count of zero clears the regions and permits a null rectangles pointer.
Q_DECL_EXPORT void ereader_set_keyboard_regions(const int* rectangles, int count);
Q_DECL_EXPORT quint64 ereader_keyboard_frame();
// Readiness means the pixels were submitted, not optical panel completion.
Q_DECL_EXPORT bool
ereader_keyboard_region_ready(int left, int top, int width, int height, quint64 frame);
Q_DECL_EXPORT int ereader_keyboard_feedback_delay();
}
