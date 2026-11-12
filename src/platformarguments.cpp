#include "platformarguments.h"

namespace {
constexpr int GrayscaleLevels = 256;

bool parsePalette(const QString& value, int& result, QString& error) {
    if (value.isEmpty()) {
        return true;
    }
    bool valid = false;
    result = value.toInt(&valid);
    if (!valid || (result != 0 && (result < 2 || result > GrayscaleLevels))) {
        error = QStringLiteral("palette must be disabled (0) or contain 2 to 256 levels");
        return false;
    }
    return true;
}

bool parseRotation(const QString& value, StartupRotation& result, QString& error) {
    const QString rotation = value.toLower();
    if (rotation.isEmpty()) {
        return true;
    }
    if (rotation == QStringLiteral("portrait")) {
        result = StartupRotation::Portrait;
    } else if (rotation == QStringLiteral("cw") || rotation == QStringLiteral("landscape")) {
        result = StartupRotation::Clockwise;
    } else if (rotation == QStringLiteral("ud")) {
        result = StartupRotation::UpsideDown;
    } else if (rotation == QStringLiteral("ccw")) {
        result = StartupRotation::CounterClockwise;
    } else {
        error = QStringLiteral("unsupported rotation: ") + value;
        return false;
    }
    return true;
}

}

bool PlatformOptions::parse(
    const QStringList& arguments, PlatformOptions& options, QString& error) {
    PlatformOptions parsed;
    bool paletteSpecified = false;
    bool rotationSpecified = false;
    bool deviceSpecified = false;
    error.clear();

    for (const QString& argument : arguments) {
        const int separator = argument.indexOf(QLatin1Char('='));
        const bool hasValue = separator >= 0;
        const QString name = hasValue ? argument.left(separator) : argument;
        const QString value = hasValue ? argument.mid(separator + 1) : QString();
        const bool enabled = !hasValue || value == QStringLiteral("1");

        if (name == QStringLiteral("debug")) {
            parsed.debug |= enabled;
        } else if (name == QStringLiteral("touch_debug")) {
            parsed.touch.debug |= enabled;
        } else if (name == QStringLiteral("touchscreen_transform_explicit")) {
            parsed.touch.transformExplicit |= enabled;
        } else if (name == QStringLiteral("touchscreen_swap_xy")) {
            parsed.touch.swapXY |= enabled;
        } else if (name == QStringLiteral("touchscreen_invert_x")) {
            parsed.touch.invertX |= enabled;
        } else if (name == QStringLiteral("touchscreen_invert_y")) {
            parsed.touch.invertY |= enabled;
        } else if (name == QStringLiteral("touchscreen_device") && hasValue && !deviceSpecified) {
            parsed.touch.device = value;
            deviceSpecified = true;
        } else if (name == QStringLiteral("palette") && hasValue && !paletteSpecified) {
            paletteSpecified = true;
            if (!parsePalette(value, parsed.paletteLevels, error)) {
                return false;
            }
        } else if (name == QStringLiteral("rotation") && hasValue && !rotationSpecified) {
            rotationSpecified = true;
            if (!parseRotation(value, parsed.rotation, error)) {
                return false;
            }
        }
    }

    parsed.touch.debug |= parsed.debug;
    options = parsed;
    return true;
}
