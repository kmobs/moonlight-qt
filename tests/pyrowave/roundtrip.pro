TEMPLATE = app
TARGET = tst_pyrowaveroundtrip
QT -= gui core
CONFIG += console c++17 warn_off
CONFIG -= app_bundle

include($$PWD/../../pyrowave/pyrowave.pri)
# Reuse an already-built codec when iterating native GPU tests. The default
# remains a self-contained build of the same vendored codec sources.
!isEmpty(PYROWAVE_STATIC_LIBRARY) {
    SOURCES =
    LIBS += $$PYROWAVE_STATIC_LIBRARY
    PRE_TARGETDEPS += $$PYROWAVE_STATIC_LIBRARY
}
include($$PWD/../../pyrowave/compression/compression.pri)

SOURCES += \
    $$PWD/tst_pyrowaveroundtrip.cpp \
    $$PWD/tst_pyrowavecompressiondecode.cpp \
    $$PWD/tst_pyrowavedequant.cpp \
    $$PWD/../../app/streaming/video/pyrowave/pyrowaveframing.cpp
HEADERS += $$PWD/../../app/streaming/video/pyrowave/pyrowaveframing.h

win32: LIBS += -luser32
macx {
    DEFINES += SDL_MAIN_HANDLED
    INCLUDEPATH += $$PWD/../../libs/mac/include \
                   $$PWD/../../libs/mac/include/SDL2 \
                   $$PWD/../../moonlight-common-c/moonlight-common-c/src
    LIBS += -L$$PWD/../../libs/mac/lib -lSDL2 -lavutil.60 -framework Foundation -framework Metal
    QMAKE_RPATHDIR += $$PWD/../../libs/mac/lib
    SOURCES += $$PWD/../../app/streaming/video/pyrowave/pyrowavedecoder.cpp
    OBJECTIVE_SOURCES += $$PWD/tst_pyrowavemetal.mm \
                         $$PWD/../../app/streaming/video/pyrowave/pyrowavemetal.mm
    HEADERS += $$PWD/../../app/streaming/video/pyrowave/pyrowavedecoder.h
}
linux {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl2 libavutil libavcodec libplacebo
    INCLUDEPATH += $$PWD/../../moonlight-common-c/moonlight-common-c/src
    SOURCES += $$PWD/../../app/streaming/video/pyrowave/pyrowavedecoder.cpp \
               $$PWD/../../app/streaming/video/pyrowave/pyrowaveplacebo.cpp \
               $$PWD/../../app/streaming/video/ffmpeg-renderers/plvk_c.c
    HEADERS += $$PWD/../../app/streaming/video/pyrowave/pyrowavedecoder.h \
               $$PWD/../../app/streaming/video/pyrowave/pyrowaveplacebo.h
}
