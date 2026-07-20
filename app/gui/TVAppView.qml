import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2

import AppModel 1.0
import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0

CenteredGridView {
    property int computerIndex
    property AppModel appModel: createModel()
    property bool activated
    property bool showHiddenGames
    property bool showGames
    property string heroSource: currentItem && currentItem.appIconSource ? currentItem.appIconSource : ""
    property bool tvModeView: true

    id: appGrid
    focus: true
    activeFocusOnTab: true
    topMargin: 190
    bottomMargin: 36
    cellWidth: 380
    cellHeight: 440
    objectName: qsTr("Library")

    function toggleTvOverlay() {
        topOverlay.toggle()
    }

    Keys.onPressed: {
        topOverlay.showTemporarily()
    }

    TVTheme {
        id: tvTheme
    }

    function computerLost()
    {
        stackView.pop()
    }

    Component.onCompleted: {
        currentIndex = -1
    }

    StackView.onActivated: {
        appModel.computerLost.connect(computerLost)
        activated = true

        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }

        if (!showGames && !showHiddenGames) {
            var directLaunchAppIndex = model.getDirectLaunchAppIndex();
            if (directLaunchAppIndex >= 0) {
                currentIndex = directLaunchAppIndex
                currentItem.launchOrResumeSelectedApp(false)
                showGames = true
            }
        }
    }

    StackView.onDeactivating: {
        appModel.computerLost.disconnect(computerLost)
        activated = false
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import AppModel 1.0; AppModel {}', parent, '')
        model.initialize(ComputerManager, computerIndex, showHiddenGames)
        return model
    }

    Rectangle {
        anchors.fill: parent
        z: -3
        gradient: Gradient {
            GradientStop { position: 0.0; color: tvTheme.backgroundTop }
            GradientStop { position: 1.0; color: tvTheme.backgroundBottom }
        }
    }

    Image {
        anchors.fill: parent
        z: -2
        source: heroSource
        asynchronous: true
        fillMode: Image.PreserveAspectCrop
        sourceSize.width: width
        sourceSize.height: height
        opacity: 0.2
    }

    Rectangle {
        anchors.fill: parent
        z: -1
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#BC0D1625" }
            GradientStop { position: 0.38; color: "#D10C1524" }
            GradientStop { position: 1.0; color: "#EC0A1223" }
        }
    }

    Column {
        x: 42
        y: 26
        spacing: 6

        Label {
            text: appGrid.currentItem && appGrid.currentItem.appName ? appGrid.currentItem.appName : qsTr("Your Games")
            font.pointSize: 32
            font.bold: true
            width: Math.min(appGrid.width - 84, 900)
            elide: Text.ElideRight
        }

        Label {
            text: qsTr("Browse with your controller and press A to launch")
            color: tvTheme.mutedText
            font.pointSize: 13
        }
    }

    TVTopOverlay {
        id: topOverlay
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        allowAddHost: false
        allowBack: true

        onBackRequested: {
            stackView.pop()
        }

        onSettingsRequested: {
            stackView.push("qrc:/gui/SettingsView.qml")
        }

        onHelpRequested: {
            Qt.openUrlExternally("https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide")
        }
    }

    model: appModel

    delegate: NavigableItemDelegate {
        width: 360
        height: 420
        grid: appGrid

        property alias appContextMenu: appContextMenuLoader.item
        property alias appNameText: appNameTextLoader.item
        property alias appIconSource: appIcon.source
        property string appName: model.name

        opacity: model.hidden ? 0.4 : 1.0
        scale: highlighted ? tvTheme.focusScaleFactor : 1.0

        Behavior on scale {
            NumberAnimation {
                duration: tvTheme.focusScaleDurationMs
                easing.type: Easing.OutCubic
            }
        }

        background: Rectangle {
            radius: 18
            color: "#123049"
            border.width: highlighted ? tvTheme.focusBorderWidth : 2
            border.color: highlighted ? tvTheme.focusColor : tvTheme.cardBorder
        }

        Image {
            property bool isPlaceholder: false

            id: appIcon
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 12
            source: model.boxart
            asynchronous: true
            fillMode: Image.PreserveAspectFit

            onSourceSizeChanged: {
                if (!model.isAppCollectorGame &&
                    ((sourceSize.width === 130 && sourceSize.height === 180) ||
                     (sourceSize.width === 628 && sourceSize.height === 888) ||
                     (sourceSize.width === 200 && sourceSize.height === 266)))
                {
                    isPlaceholder = true
                }
                else
                {
                    isPlaceholder = false
                }

                width = 320
                height = 360
            }

            ToolTip.text: model.name
            ToolTip.delay: 900
            ToolTip.timeout: 4500
            ToolTip.visible: (parent.hovered || parent.highlighted) && (!appNameText || appNameText.truncated)
        }

        Loader {
            active: model.running
            asynchronous: true
            anchors.fill: appIcon

            sourceComponent: Item {
                RoundButton {
                    focusPolicy: Qt.NoFocus
                    anchors.horizontalCenterOffset: appIcon.isPlaceholder ? -50 : 0
                    anchors.verticalCenterOffset: appIcon.isPlaceholder ? -85 : -72
                    anchors.centerIn: parent
                    implicitWidth: 92
                    implicitHeight: 92

                    icon.source: "qrc:/res/play_arrow_FILL1_wght700_GRAD200_opsz48.svg"
                    icon.width: 78
                    icon.height: 78

                    onClicked: {
                        launchOrResumeSelectedApp(true)
                    }

                    ToolTip.text: qsTr("Resume Game")
                    ToolTip.delay: 900
                    ToolTip.timeout: 2500
                    ToolTip.visible: hovered

                    Material.background: "#D0808080"
                }

                RoundButton {
                    focusPolicy: Qt.NoFocus
                    anchors.horizontalCenterOffset: appIcon.isPlaceholder ? 50 : 0
                    anchors.verticalCenterOffset: appIcon.isPlaceholder ? -85 : 72
                    anchors.centerIn: parent
                    implicitWidth: 92
                    implicitHeight: 92

                    icon.source: "qrc:/res/stop_FILL1_wght700_GRAD200_opsz48.svg"
                    icon.width: 78
                    icon.height: 78

                    onClicked: {
                        doQuitGame()
                    }

                    ToolTip.text: qsTr("Quit Game")
                    ToolTip.delay: 900
                    ToolTip.timeout: 2500
                    ToolTip.visible: hovered

                    Material.background: "#D0808080"
                }
            }
        }

        Loader {
            id: appNameTextLoader
            active: appIcon.isPlaceholder

            width: appIcon.width
            height: model.running ? 210 : appIcon.height

            anchors.left: appIcon.left
            anchors.right: appIcon.right
            anchors.bottom: appIcon.bottom

            sourceComponent: Label {
                id: appNameText
                text: model.name
                font.pointSize: 18
                font.bold: true
                leftPadding: 20
                rightPadding: 20
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                elide: Text.ElideRight
            }
        }

        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 10
            text: model.name
            font.pointSize: 16
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            leftPadding: 12
            rightPadding: 12
        }

        function launchOrResumeSelectedApp(quitExistingApp)
        {
            var runningId = appModel.getRunningAppId()
            if (runningId !== 0 && runningId !== model.appid) {
                if (quitExistingApp) {
                    quitAppDialog.appName = appModel.getRunningAppName()
                    quitAppDialog.segueToStream = true
                    quitAppDialog.nextAppName = model.name
                    quitAppDialog.nextAppIndex = index
                    quitAppDialog.open()
                }

                return
            }

            var component = Qt.createComponent("StreamSegue.qml")
            var segue = component.createObject(stackView, {
                                                   "appName": model.name,
                                                   "session": appModel.createSessionForApp(index),
                                                   "isResume": runningId === model.appid
                                               })
            stackView.push(segue)
        }

        onClicked: {
            if (!model.running) {
                launchOrResumeSelectedApp(true)
            }
        }

        onPressAndHold: {
            if (appContextMenu.popup) {
                appContextMenu.popup()
            }
            else {
                appContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onReturnPressed: {
            if (model.running) {
                appContextMenu.open()
            }
        }

        Keys.onEnterPressed: {
            if (model.running) {
                appContextMenu.open()
            }
        }

        Keys.onMenuPressed: {
            appContextMenu.open()
        }

        function doQuitGame() {
            quitAppDialog.appName = appModel.getRunningAppName()
            quitAppDialog.segueToStream = false
            quitAppDialog.open()
        }

        Loader {
            id: appContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: appContextMenu
                initiator: appContextMenuLoader.parent
                NavigableMenuItem {
                    text: model.running ? qsTr("Resume Game") : qsTr("Launch Game")
                    onTriggered: launchOrResumeSelectedApp(true)
                }
                NavigableMenuItem {
                    text: qsTr("Quit Game")
                    onTriggered: doQuitGame()
                    visible: model.running
                }
                NavigableMenuItem {
                    checkable: true
                    checked: model.directLaunch
                    text: qsTr("Direct Launch")
                    onTriggered: appModel.setAppDirectLaunch(model.index, !model.directLaunch)
                    enabled: !model.hidden

                    ToolTip.text: qsTr("Launch this app immediately when the host is selected, bypassing the app selection grid.")
                    ToolTip.delay: 1000
                    ToolTip.timeout: 3000
                    ToolTip.visible: hovered
                }
                NavigableMenuItem {
                    checkable: true
                    checked: model.hidden
                    text: qsTr("Hide Game")
                    onTriggered: appModel.setAppHidden(model.index, !model.hidden)
                    enabled: model.hidden || (!model.running && !model.directLaunch)

                    ToolTip.text: qsTr("Hide this game from the app grid. To access hidden games, open the host context menu and choose %1.").arg(qsTr("View All Apps"))
                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                }
            }
        }
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
        visible: appGrid.count === 0

        Label {
            text: qsTr("This computer doesn't seem to have any applications or some applications are hidden")
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    NavigableMessageDialog {
        id: quitAppDialog
        property string appName: ""
        property bool segueToStream: false
        property string nextAppName: ""
        property int nextAppIndex: 0
        text: qsTr("Are you sure you want to quit %1? Any unsaved progress will be lost.").arg(appName)
        standardButtons: Dialog.Yes | Dialog.No

        function quitApp() {
            var component = Qt.createComponent("QuitSegue.qml")
            var params = {"appName": appName, "quitRunningAppFn": function() { appModel.quitRunningApp() }}
            if (segueToStream) {
                params.nextAppName = nextAppName
                params.nextSession = appModel.createSessionForApp(nextAppIndex)
            }
            else {
                params.nextAppName = null
                params.nextSession = null
            }

            stackView.push(component.createObject(stackView, params))
        }

        onAccepted: quitApp()
    }

    ScrollBar.vertical: ScrollBar {}
}
