TEMPLATE = app
TARGET = tst_macdisplaytiming
CONFIG += console c++17
CONFIG -= app_bundle
QT -= gui core
SOURCES += $$PWD/tst_macdisplaytiming.cpp
HEADERS += $$PWD/../../app/streaming/video/ffmpeg-renderers/macdisplaytiming.h
