pragma Singleton
import QtQuick 2.9
QtObject {
    property bool uiNavMode: true
    function setUiNavMode(value) { uiNavMode = value }
}
