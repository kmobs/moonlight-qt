TEMPLATE = app
TARGET = tst_autoupdatechecker
CONFIG += console c++17 testcase
# Actual PE identity is read by the Windows updater, independently of VERSION_STR.
VERSION = 6.2.171
CONFIG -= app_bundle
QT += network testlib
QT -= gui
DEFINES += VERSION_STR=\\\"6.1.0-vrr17.1\\\"
SOURCES += $$PWD/tst_autoupdatechecker.cpp \
           $$PWD/../../app/backend/autoupdatechecker.cpp
HEADERS += $$PWD/../../app/backend/autoupdatechecker.h
