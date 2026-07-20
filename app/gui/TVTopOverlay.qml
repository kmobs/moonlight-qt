import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import SystemProperties 1.0

Item {
    id: overlay

    property bool allowAddHost: false
    property bool allowBack: false
    property bool shown: true

    signal addHostRequested()
    signal backRequested()
    signal settingsRequested()
    signal helpRequested()

    implicitHeight: 72

    function showTemporarily() {
        shown = true
        hideTimer.restart()
    }

    function toggle() {
        shown = !shown
        if (shown) {
            hideTimer.restart()
        }
    }

    opacity: shown ? 1.0 : 0.0
    visible: opacity > 0.0
    z: 20

    Behavior on opacity {
        NumberAnimation {
            duration: 160
            easing.type: Easing.OutCubic
        }
    }

    Timer {
        id: hideTimer
        interval: 3500
        onTriggered: {
            if (!container.activeFocus) {
                overlay.shown = false
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: "#A8162438"
        border.width: 1
        border.color: "#5A7FA2"
    }

    RowLayout {
        id: container
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 10

        NavigableToolButton {
            visible: overlay.allowBack
            iconSource: "qrc:/res/arrow_left.svg"
            ToolTip.delay: 800
            ToolTip.timeout: 2500
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Back")
            onClicked: overlay.backRequested()
        }

        NavigableToolButton {
            visible: overlay.allowAddHost
            iconSource: "qrc:/res/ic_add_to_queue_white_48px.svg"
            ToolTip.delay: 800
            ToolTip.timeout: 2500
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Add PC manually")
            onClicked: overlay.addHostRequested()
        }

        Item {
            Layout.fillWidth: true
        }

        NavigableToolButton {
            visible: SystemProperties.hasBrowser
            iconSource: "qrc:/res/question_mark.svg"
            ToolTip.delay: 800
            ToolTip.timeout: 2500
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Help")
            onClicked: overlay.helpRequested()
        }

        NavigableToolButton {
            iconSource: "qrc:/res/settings.svg"
            ToolTip.delay: 800
            ToolTip.timeout: 2500
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Settings")
            onClicked: overlay.settingsRequested()
        }
    }
}