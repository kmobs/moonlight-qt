import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

Item {
    id: hintBar

    property string primaryLabel: ""
    property string secondaryLabel: ""
    property string tertiaryLabel: ""
    property string menuLabel: ""

    implicitHeight: 68

    Rectangle {
        anchors.fill: parent
        radius: 18
        color: "#AA0D1524"
        border.width: 1
        border.color: "#496780"
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 18
        anchors.rightMargin: 18
        spacing: 18

        Item {
            Layout.fillWidth: true
        }

        HintChip {
            keyText: "A"
            keyColor: "#33C24D"
            labelText: hintBar.primaryLabel
        }

        HintChip {
            keyText: "B"
            keyColor: "#E05252"
            labelText: hintBar.secondaryLabel
        }

        HintChip {
            keyText: "Y"
            keyColor: "#E2B93B"
            labelText: hintBar.tertiaryLabel
        }

        HintChip {
            keyText: "MENU"
            keyColor: "#5B7CFF"
            labelText: hintBar.menuLabel
        }
    }

    component HintChip: Item {
        property string keyText: ""
        property string keyColor: "#ffffff"
        property string labelText: ""

        implicitHeight: 42
        implicitWidth: Math.max(keyCircle.width + labelTextItem.width + 18, 72)

        RowLayout {
            anchors.fill: parent
            spacing: 8

            Rectangle {
                id: keyCircle
                width: keyText.length > 3 ? 42 : 34
                height: width
                radius: width / 2
                color: hintBar.keyColor
                border.width: 1
                border.color: "#FFFFFF"

                Text {
                    anchors.centerIn: parent
                    text: keyText
                    color: "#08111C"
                    font.bold: true
                    font.pointSize: keyText.length > 3 ? 9 : 12
                }
            }

            Text {
                id: labelTextItem
                Layout.alignment: Qt.AlignVCenter
                text: labelText
                color: "#F2F6FF"
                font.pointSize: 12
                elide: Text.ElideRight
            }
        }
    }
}
