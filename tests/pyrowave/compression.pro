TEMPLATE = app
TARGET = tst_pyrowavecompression
QT -= gui core
CONFIG += console c++17
CONFIG -= app_bundle
include($$PWD/../../pyrowave/compression/compression.pri)
SOURCES += $$PWD/tst_pyrowavecompression.cpp $$PWD/../../app/streaming/video/pyrowave/pyrowaveframing.cpp
HEADERS += $$PWD/../../app/streaming/video/pyrowave/pyrowaveframing.h
