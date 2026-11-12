#pragma once

#include <QStringList>

// Values follow the canonical framebuffer rotations, before device-specific mapping.
enum class StartupRotation : qint8 {
    Unchanged = -1,
    Portrait = 0,
    Clockwise = 1,
    UpsideDown = 2,
    CounterClockwise = 3,
};

struct TouchOptions {
    QString device;
    bool debug = false;
    bool transformExplicit = false;
    bool swapXY = false;
    bool invertX = false;
    bool invertY = false;
};

struct PlatformOptions {
    bool debug = false;
    int paletteLevels = 0;
    StartupRotation rotation = StartupRotation::Unchanged;
    TouchOptions touch;

    static bool parse(const QStringList& arguments, PlatformOptions& options, QString& error);
};
