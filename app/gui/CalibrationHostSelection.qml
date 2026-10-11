import QtQml 2.2

QtObject {
    property var hosts: []
    property string selectedUuid: ""
    property string testedUuid: ""
    readonly property int currentIndex: indexOf(selectedUuid)
    readonly property bool testedHostAvailable: indexOf(testedUuid) >= 0
    signal testedHostUnavailable()

    function indexOf(uuid) {
        if (!uuid) return -1
        for (var i = 0; i < hosts.length; ++i) {
            if (hosts[i].uuid === uuid) return i
        }
        return -1
    }

    function select(index) {
        selectedUuid = index >= 0 && index < hosts.length ? hosts[index].uuid : ""
    }

    onHostsChanged: {
        if (indexOf(selectedUuid) < 0) select(hosts.length > 0 ? 0 : -1)
    }
    onTestedHostAvailableChanged: {
        if (testedUuid && !testedHostAvailable) testedHostUnavailable()
    }
}
