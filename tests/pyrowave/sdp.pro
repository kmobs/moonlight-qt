TEMPLATE = app
TARGET = tst_pyrowavesdp
CONFIG += console c11
CONFIG -= app_bundle
QT -= gui core

COMMON = $$clean_path($$PWD/../../moonlight-common-c/moonlight-common-c)
INCLUDEPATH += $$COMMON/src $$COMMON/enet/include

SOURCES += tst_pyrowavesdp.c \
           $$COMMON/src/SdpGenerator.c

win32: LIBS += -lws2_32
