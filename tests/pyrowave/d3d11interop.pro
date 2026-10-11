TEMPLATE = app
TARGET = tst_pyrowaved3d11interop
QT -= gui core
CONFIG += console c++17
CONFIG -= app_bundle
INCLUDEPATH += $$PWD/../../pyrowave/pyrowave/Granite/third_party/khronos/vulkan-headers/include
SOURCES += $$PWD/tst_pyrowaved3d11interop.cpp
