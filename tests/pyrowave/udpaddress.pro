TEMPLATE = app
TARGET = tst_pyrowaveudpaddress
CONFIG += console c++17
CONFIG -= app_bundle
QT += network
QT -= gui
SOURCES += $$PWD/tst_pyrowaveudpaddress.cpp \
           $$PWD/../../app/backend/pyrowaveudpprobe.cpp
HEADERS += $$PWD/../../app/backend/pyrowaveudpprobe.h
INCLUDEPATH += $$PWD/../../app
