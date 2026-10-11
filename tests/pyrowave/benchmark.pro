# Explicit opt-in transport benchmark; deliberately absent from pyrowave.pro.
TEMPLATE = app
TARGET = pyrowave_compression_benchmark
QT -= gui core
CONFIG += console c++20 warn_off
CONFIG -= app_bundle

isEmpty(VIBESHINE_SOURCE_DIR): VIBESHINE_SOURCE_DIR = $$clean_path($$PWD/../../../vibeshine)
!exists($$VIBESHINE_SOURCE_DIR/src/pyrowave_policy.cpp): error("Set VIBESHINE_SOURCE_DIR to a Vibeshine checkout containing src/pyrowave_policy.cpp")

include($$PWD/../../pyrowave/pyrowave.pri)
include($$PWD/../../pyrowave/compression/compression.pri)

INCLUDEPATH += $$VIBESHINE_SOURCE_DIR/src
SOURCES += $$PWD/benchmark.cpp \
           $$VIBESHINE_SOURCE_DIR/src/pyrowave_policy.cpp \
           $$PWD/../../app/streaming/video/pyrowave/pyrowaveframing.cpp
HEADERS += $$VIBESHINE_SOURCE_DIR/src/pyrowave_policy.h \
           $$VIBESHINE_SOURCE_DIR/src/pyrowave_protocol.h

linux: LIBS += -ldl -lpthread
win32: LIBS += -luser32
