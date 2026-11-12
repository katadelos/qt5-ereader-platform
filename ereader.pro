TARGET = qereader

TEMPLATE = lib
CONFIG += plugin

DEFINES += QT_NO_FOREACH

QMAKE_CXXFLAGS += -Wno-missing-field-initializers

QT += core-private gui-private network \
    eventdispatcher_support-private fb_support-private \
    fontdatabase_support-private service_support-private

INCLUDEPATH += $$PWD/include $$PWD/src $$PWD/src/display $$PWD/src/input $$PWD/src/session

isEmpty(FBINK_STAGING_DIR): FBINK_STAGING_DIR = /usr
INCLUDEPATH += $$FBINK_STAGING_DIR/include
LIBS += -L$$FBINK_STAGING_DIR/lib -lfbink

SOURCES = src/display/displaybuffer.cpp \
    src/display/ereaderscreen.cpp \
    src/display/framebuffer.cpp \
    src/display/refreshpolicy.cpp \
    src/display/refreshfence.cpp \
    src/display/refreshscheduler.cpp \
    src/display/refreshwaiter.cpp \
    src/ereaderintegration.cpp \
    src/ereadernativeinterface.cpp \
    src/input/evdevbuttons.cpp \
    src/input/evdevtouch.cpp \
    src/input/touchdevice.cpp \
    src/input/touchdecoder.cpp \
    src/main.cpp \
    src/platformarguments.cpp \
    src/session/nickelsleepsession.cpp

HEADERS = src/display/displaybuffer.h \
    src/display/ereaderscreen.h \
    src/display/framebuffer.h \
    src/display/refreshpolicy.h \
    src/display/refreshfence.h \
    src/display/refreshscheduler.h \
    src/display/refreshwaiter.h \
    src/ereaderintegration.h \
    src/input/evdevbuttons.h \
    src/input/evdevtouch.h \
    src/input/touchdevice.h \
    src/input/touchdecoder.h \
    src/input/touchtypes.h \
    src/input/filedescriptor.h \
    src/platformarguments.h \
    src/session/nickelsleepsession.h \
    include/ereaderplatform.h \
    include/ereaderplatformfunctions.h

DISTFILES += src/ereader.json .clang-tidy .clang-format

target.path = $$[QT_INSTALL_PLUGINS]/platforms
INSTALLS += target

include(checks.pri)
