TEMPLATE = app
TARGET = tst_pyrowavemetalcalibration
QT += core gui qml
CONFIG += console c++17
CONFIG -= app_bundle
DEFINES += SDL_MAIN_HANDLED
!macx:error("The headless Metal calibration test requires macOS")

include($$PWD/../../pyrowave/pyrowave.pri)
!isEmpty(PYROWAVE_STATIC_LIBRARY) {
    SOURCES =
    LIBS += $$PYROWAVE_STATIC_LIBRARY
    PRE_TARGETDEPS += $$PYROWAVE_STATIC_LIBRARY
}
include($$PWD/../../pyrowave/compression/compression.pri)

SOURCES += $$PWD/../../app/streaming/video/pyrowave/pyrowavedecoder.cpp \
           $$PWD/../../app/streaming/video/pyrowave/pyrowaveframing.cpp
OBJECTIVE_SOURCES += $$PWD/tst_pyrowavemetalcalibration.mm \
                     $$PWD/../../app/streaming/video/pyrowave/pyrowavemetal.mm \
                     $$PWD/../../app/streaming/video/pyrowave/pyrowavemetalcalibrator.mm
INCLUDEPATH += $$PWD/../../app \
               $$PWD/../../libs/mac/include \
               $$PWD/../../libs/mac/include/SDL2 \
               $$PWD/../../moonlight-common-c/moonlight-common-c/src
LIBS += -L$$PWD/../../libs/mac/lib -lSDL2 -lavutil.60 -framework Foundation -framework Metal
QMAKE_RPATHDIR += $$PWD/../../libs/mac/lib

HEADERS += $$PWD/../../app/streaming/video/pyrowave/pyrowavemetalcalibrator.h
