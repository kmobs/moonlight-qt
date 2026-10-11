import QtQuick 2.9
import QtQuick.Controls 2.2

Button {
    id: control

    activeFocusOnTab: true
    hoverEnabled: true
    implicitHeight: 44
    padding: 12
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0

    contentItem: Label {
        text: control.text
        font: control.font
        color: control.enabled ? "#ffffff" : "#929292"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 44
        radius: 6
        // Selection and action emphasis persist when controller focus moves away.
        // Focus gets a separate white outline, including on selected controls.
        color: !control.enabled ? "#303030" :
               control.down ? "#17466c" :
               control.checked ? (control.activeFocus || control.hovered ? "#346a96" : "#254f73") :
               control.activeFocus ? "#526171" :
               control.hovered ? "#4b545e" : "#383838"
        border.width: control.enabled && control.activeFocus ? 3 : control.hovered || control.checked || control.highlighted ? 2 : 1
        border.color: !control.enabled ? "#484848" :
                      control.activeFocus ? "#ffffff" :
                      control.hovered ? "#c4d8e8" :
                      control.checked || control.highlighted ? "#75bfff" : "#707070"
    }
}
