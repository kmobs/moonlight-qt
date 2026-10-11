TEMPLATE = app
TARGET = tst_calibrationhosts
QT = core network qml testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
INCLUDEPATH += $$PWD/../../app $$PWD/../../moonlight-common-c/moonlight-common-c/src
SOURCES += $$PWD/tst_calibrationhosts.cpp $$PWD/../../app/backend/nvaddress.cpp
DEFINES += CALIBRATION_HOST_SELECTION_PATH=\\\"$$PWD/../../app/gui/CalibrationHostSelection.qml\\\"
