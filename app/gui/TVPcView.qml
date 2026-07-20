import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerModel 1.0

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0

CenteredGridView {
    property ComputerModel computerModel: createModel()
    property bool tvModeView: true

    id: pcGrid
    focus: true
    activeFocusOnTab: true
    topMargin: 220
    bottomMargin: 110
    cellWidth: 520
    cellHeight: 290
    objectName: qsTr("TV Hosts")

    function toggleTvOverlay() {
        topOverlay.toggle()
    }

    Keys.onPressed: {
        topOverlay.showTemporarily()
    }

    TVTheme {
        id: tvTheme
    }

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        currentIndex = -1
    }

    StackView.onActivated: {
        ComputerManager.computerAddCompleted.connect(addComplete)

        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }
    }

    StackView.onDeactivating: {
        ComputerManager.computerAddCompleted.disconnect(addComplete)
    }

    function pairingComplete(error)
    {
        pairDialog.close()

        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.helpText = ""
            errorDialog.open()
        }
    }

    function addComplete(success, detectedPortBlocking)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")

            if (detectedPortBlocking) {
                errorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking Moonlight. Streaming over the Internet may not work while connected to this network.")
            }
            else {
                errorDialog.helpText = qsTr("Click the Help button for possible solutions.")
            }

            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.pairingCompleted.connect(pairingComplete)
        model.connectionTestCompleted.connect(testConnectionDialog.connectionTestComplete)
        return model
    }

    Rectangle {
        anchors.fill: parent
        z: -2
        gradient: Gradient {
            GradientStop { position: 0.0; color: tvTheme.backgroundTop }
            GradientStop { position: 1.0; color: tvTheme.backgroundBottom }
        }
    }

    Rectangle {
        anchors.fill: parent
        z: -1
        color: tvTheme.heroOverlay
    }

    Rectangle {
        x: 42
        y: 98
        width: parent.width - 84
        height: 96
        radius: 18
        color: "#1D2C42"
        border.width: 1
        border.color: "#4E6A88"

        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: 17
            color: "#223450"
            opacity: 0.95
        }

        Image {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            source: "qrc:/res/desktop_windows-48px.svg"
            sourceSize.width: 60
            sourceSize.height: 60
            opacity: 0.95
        }

        Column {
            anchors.left: parent.left
            anchors.leftMargin: 96
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            Label {
                text: qsTr("Choose a Host")
                font.pointSize: 26
                font.bold: true
                elide: Text.ElideRight
                width: parent.width
            }

            Label {
                text: qsTr("Select your gaming PC to open its library and launch with the controller.")
                color: tvTheme.mutedText
                font.pointSize: 13
                elide: Text.ElideRight
                width: parent.width
            }
        }
    }

    TVTopOverlay {
        id: topOverlay
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        allowAddHost: true
        allowBack: stackView.depth > 1

        onAddHostRequested: {
            addPcDialog.open()
        }

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

    Row {
        anchors.centerIn: parent
        spacing: 12
        visible: pcGrid.count === 0

        BusyIndicator {
            id: searchSpinner
            visible: StreamingPreferences.enableMdns
            running: visible
            width: 52
            height: 52
        }

        Label {
            height: searchSpinner.height
            elide: Label.ElideRight
            text: StreamingPreferences.enableMdns ? qsTr("Searching for compatible hosts on your local network...")
                                              : qsTr("Automatic PC discovery is disabled. Add your PC manually.")
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
            width: Math.min(parent.parent.width * 0.7, 940)
        }
    }

    TVHintBar {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 42
        anchors.rightMargin: 42
        anchors.bottomMargin: 18
        primaryLabel: qsTr("Connect")
        secondaryLabel: qsTr("Back")
        tertiaryLabel: qsTr("Add PC")
        menuLabel: qsTr("Settings")
    }

    model: computerModel

    delegate: NavigableItemDelegate {
        width: 500
        height: 270
        grid: pcGrid

        property alias pcContextMenu: pcContextMenuLoader.item

        scale: highlighted ? tvTheme.focusScaleFactor : 1.0
        Behavior on scale {
            NumberAnimation {
                duration: tvTheme.focusScaleDurationMs
                easing.type: Easing.OutCubic
            }
        }

        background: Rectangle {
            radius: 18
            color: tvTheme.cardColor
            border.width: highlighted ? tvTheme.focusBorderWidth : 2
            border.color: highlighted ? tvTheme.focusColor : tvTheme.cardBorder
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: 8
            radius: 14
            color: "#122338"
            opacity: model.online ? 1.0 : 0.72
        }

        Image {
            id: pcIcon
            anchors.left: parent.left
            anchors.leftMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            source: "qrc:/res/desktop_windows-48px.svg"
            sourceSize.width: 124
            sourceSize.height: 124
        }

        Image {
            id: stateIcon
            anchors.left: pcIcon.right
            anchors.leftMargin: 18
            anchors.verticalCenter: pcIcon.verticalCenter
            visible: !model.statusUnknown && (!model.online || !model.paired)
            source: !model.online ? "qrc:/res/warning_FILL1_wght300_GRAD200_opsz24.svg" : "qrc:/res/baseline-lock-24px.svg"
            sourceSize.width: 48
            sourceSize.height: 48
        }

        BusyIndicator {
            id: statusUnknownSpinner
            anchors.left: pcIcon.right
            anchors.leftMargin: 18
            anchors.verticalCenter: pcIcon.verticalCenter
            width: 48
            height: 48
            visible: model.statusUnknown
            running: visible
        }

        Column {
            anchors.left: stateIcon.visible || statusUnknownSpinner.visible ? stateIcon.right : pcIcon.right
            anchors.leftMargin: 18
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            Label {
                text: model.name
                font.pointSize: 24
                font.bold: true
                elide: Text.ElideRight
                width: parent.width
            }

            Label {
                text: model.online ? (model.paired ? qsTr("Ready to stream") : qsTr("Online - pairing required")) : qsTr("Offline")
                color: tvTheme.mutedText
                font.pointSize: 14
                elide: Text.ElideRight
                width: parent.width
            }
        }

        Loader {
            id: pcContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: pcContextMenu
                initiator: pcContextMenuLoader.parent
                MenuItem {
                    text: qsTr("PC Status: %1").arg(model.online ? qsTr("Online") : qsTr("Offline"))
                    font.bold: true
                    enabled: false
                }
                NavigableMenuItem {
                    text: qsTr("View All Apps")
                    onTriggered: {
                        var component = Qt.createComponent("TVAppView.qml")
                        var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name, "showHiddenGames": true})
                        stackView.push(appView)
                    }
                    visible: model.online && model.paired
                }
                NavigableMenuItem {
                    text: qsTr("Wake PC")
                    onTriggered: computerModel.wakeComputer(index)
                    visible: !model.online && model.wakeable
                }
                NavigableMenuItem {
                    text: qsTr("Test Network")
                    onTriggered: {
                        computerModel.testConnectionForComputer(index)
                        testConnectionDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Rename PC")
                    onTriggered: {
                        renamePcDialog.pcIndex = index
                        renamePcDialog.originalName = model.name
                        renamePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Delete PC")
                    onTriggered: {
                        deletePcDialog.pcIndex = index
                        deletePcDialog.pcName = model.name
                        deletePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("View Details")
                    onTriggered: {
                        showPcDetailsDialog.pcDetails = model.details
                        showPcDetailsDialog.open()
                    }
                }
            }
        }

        onClicked: {
            if (model.online) {
                if (!model.serverSupported) {
                    errorDialog.text = qsTr("The version of GeForce Experience on %1 is not supported by this build of Moonlight. You must update Moonlight to stream from %1.").arg(model.name)
                    errorDialog.helpText = ""
                    errorDialog.open()
                }
                else if (model.paired) {
                    var component = Qt.createComponent("TVAppView.qml")
                    var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name})
                    stackView.push(appView)
                }
                else {
                    var pin = computerModel.generatePinString()
                    computerModel.pairComputer(index, pin)
                    pairDialog.pin = pin
                    pairDialog.open()
                }
            }
            else {
                pcContextMenu.open()
            }
        }

        onPressAndHold: {
            if (pcContextMenu.popup) {
                pcContextMenu.popup()
            }
            else {
                pcContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onMenuPressed: {
            pcContextMenu.open()
        }

        Keys.onDeletePressed: {
            deletePcDialog.pcIndex = index
            deletePcDialog.pcName = model.name
            deletePcDialog.open()
        }
    }

    ErrorMessageDialog {
        id: errorDialog
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide"
    }

    NavigableMessageDialog {
        id: pairDialog
        closePolicy: Popup.CloseOnEscape
        property string pin: "0000"
        text: qsTr("Please enter %1 on your host PC. This dialog will close when pairing is completed.").arg(pin) + "\n\n" +
              qsTr("If your host PC is running Sunshine, navigate to the Sunshine web UI to enter the PIN.")
        standardButtons: Dialog.Cancel
    }

    NavigableMessageDialog {
        id: deletePcDialog
        property int pcIndex: -1
        property string pcName: ""
        text: qsTr("Are you sure you want to remove '%1'?").arg(pcName)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            computerModel.deleteComputer(pcIndex)
        }
    }

    NavigableMessageDialog {
        id: testConnectionDialog
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok

        onAboutToShow: {
            testConnectionDialog.text = qsTr("Moonlight is testing your network connection to determine if any required ports are blocked.") + "\n\n" + qsTr("This may take a few seconds…")
            showSpinner = true
        }

        function connectionTestComplete(result, blockedPorts)
        {
            if (result === -1) {
                text = qsTr("The network test could not be performed because none of Moonlight's connection testing servers were reachable from this PC. Check your Internet connection or try again later.")
                imageSrc = "qrc:/res/baseline-warning-24px.svg"
            }
            else if (result === 0) {
                text = qsTr("This network does not appear to be blocking Moonlight. If you still have trouble connecting, check your PC's firewall settings.") + "\n\n" + qsTr("If you are trying to stream over the Internet, install the Moonlight Internet Hosting Tool on your gaming PC and run the included Internet Streaming Tester to check your gaming PC's Internet connection.")
                imageSrc = "qrc:/res/baseline-check_circle_outline-24px.svg"
            }
            else {
                text = qsTr("Your PC's current network connection seems to be blocking Moonlight. Streaming over the Internet may not work while connected to this network.") + "\n\n" + qsTr("The following network ports were blocked:") + "\n"
                text += blockedPorts
                imageSrc = "qrc:/res/baseline-error_outline-24px.svg"
            }

            showSpinner = false
        }
    }

    NavigableDialog {
        id: renamePcDialog
        property string label: qsTr("Enter the new name for this PC:")
        property string originalName
        property int pcIndex: -1

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                computerModel.renameComputer(pcIndex, editText.text)
            }
        }

        ColumnLayout {
            Label {
                text: renamePcDialog.label
                font.bold: true
            }

            TextField {
                id: editText
                placeholderText: renamePcDialog.originalName
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    renamePcDialog.accept()
                }

                Keys.onEnterPressed: {
                    renamePcDialog.accept()
                }
            }
        }
    }

    NavigableMessageDialog {
        id: showPcDetailsDialog
        property string pcDetails: ""
        text: showPcDetailsDialog.pcDetails
        imageSrc: "qrc:/res/baseline-help_outline-24px.svg"
        standardButtons: Dialog.Ok
    }

    NavigableDialog {
        id: addPcDialog
        property string label: qsTr("Enter the IP address of your host PC:")

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            addPcEditText.forceActiveFocus()
        }

        onClosed: {
            addPcEditText.clear()
        }

        onAccepted: {
            if (addPcEditText.text) {
                ComputerManager.addNewHostManually(addPcEditText.text.trim())
            }
        }

        ColumnLayout {
            Label {
                text: addPcDialog.label
                font.bold: true
            }

            TextField {
                id: addPcEditText
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    addPcDialog.accept()
                }

                Keys.onEnterPressed: {
                    addPcDialog.accept()
                }
            }
        }
    }

    ScrollBar.vertical: ScrollBar {}
}
