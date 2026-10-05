TEMPLATE = app
TARGET = tst_controllernavigation
QT += gui qml network testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
macx:!disable-prebuilts {
    INCLUDEPATH += $$PWD/../../libs/mac/include $$PWD/../../libs/mac/include/SDL2
    LIBS += -L$$PWD/../../libs/mac/lib -lSDL2
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl2
}
INCLUDEPATH += $$PWD/../../app
SOURCES += $$PWD/tst_controllernavigation.cpp \
    $$PWD/../../app/gui/sdlgamepadkeynavigation.cpp \
    $$PWD/../../app/settings/streamingpreferences.cpp \
    $$PWD/../../app/streaming/vrrratepolicy.cpp \
    $$PWD/../../app/diagnostics/diagnosticcapture.cpp \
    $$PWD/../../app/diagnostics/diagnosticzip.cpp
HEADERS += $$PWD/../../app/gui/sdlgamepadkeynavigation.h \
    $$PWD/../../app/settings/streamingpreferences.h
