#include "gui/onlinecomputerentries.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QtTest>
#include <memory>

class CalibrationHostsTest : public QObject
{
    Q_OBJECT

    static QVariantList entries(std::initializer_list<std::pair<QString, QString>> values)
    {
        QVariantList result;
        for (const auto& value : values) {
            result.append(QVariantMap{{"uuid", value.first}, {"name", value.second}});
        }
        return result;
    }

private slots:
    void confirmedStatusOnly()
    {
        NvComputer online, offline, unknown;
        online.state = NvComputer::CS_ONLINE;
        online.pairState = NvComputer::PS_NOT_PAIRED;
        online.name = "Online without apps";
        online.uuid = "online";
        offline.state = NvComputer::CS_OFFLINE;
        offline.pairState = NvComputer::PS_PAIRED;
        offline.name = "Offline with cached apps";
        offline.uuid = "offline";
        offline.appList.append(NvApp{});
        unknown.state = NvComputer::CS_UNKNOWN;
        unknown.pairState = NvComputer::PS_PAIRED;
        unknown.uuid = "unknown";
        QVector<NvComputer*> computers{&offline, &unknown, &online};

        QCOMPARE(onlineComputerEntries(computers), entries({{"online", "Online without apps"}}));
        QVERIFY(online.appList.isEmpty());
        QCOMPARE(offline.appList.size(), 1);
        QCOMPARE(offline.pairState, NvComputer::PS_PAIRED);

        online.state = NvComputer::CS_OFFLINE;
        QVERIFY(onlineComputerEntries(computers).isEmpty());
        unknown.state = NvComputer::CS_ONLINE;
        unknown.name = "Now confirmed";
        QCOMPARE(onlineComputerEntries(computers), entries({{"unknown", "Now confirmed"}}));
        offline.state = NvComputer::CS_ONLINE;
        QCOMPARE(onlineComputerEntries(computers).size(), 2);
    }

    void refreshAndStaleSelection()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(CALIBRATION_HOST_SELECTION_PATH));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> selection(component.create());
        QVERIFY(selection);
        QCOMPARE(selection->property("currentIndex").toInt(), -1);
        QCOMPARE(selection->property("selectedUuid").toString(), QString());

        QVERIFY(selection->setProperty("hosts", entries({{"a", "Alpha"}, {"b", "Beta"}})));
        QCOMPARE(selection->property("selectedUuid").toString(), QString("a"));
        QVERIFY(QMetaObject::invokeMethod(selection.get(), "select", Q_ARG(QVariant, 1)));
        QCOMPARE(selection->property("selectedUuid").toString(), QString("b"));

        // A discovery refresh/reorder or rename preserves UUID identity.
        QVERIFY(selection->setProperty("hosts", entries({{"b", "Renamed"}, {"a", "Alpha"}})));
        QCOMPARE(selection->property("selectedUuid").toString(), QString("b"));
        QCOMPARE(selection->property("currentIndex").toInt(), 0);
        QVERIFY(selection->setProperty("hosts", entries({{"a", "Alpha"}})));
        QCOMPARE(selection->property("selectedUuid").toString(), QString("a"));
        QCOMPARE(selection->property("currentIndex").toInt(), 0);

        QVERIFY(selection->setProperty("hosts", QVariantList{}));
        QCOMPARE(selection->property("selectedUuid").toString(), QString());
        QCOMPARE(selection->property("currentIndex").toInt(), -1);
        QVERIFY(selection->setProperty("hosts", entries({{"b", "Back online"}})));
        QCOMPARE(selection->property("selectedUuid").toString(), QString("b"));
        QCOMPARE(selection->property("currentIndex").toInt(), 0);
    }

    void testedHostLoss()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(CALIBRATION_HOST_SELECTION_PATH));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> selection(component.create());
        QVERIFY(selection);
        QSignalSpy unavailable(selection.get(), SIGNAL(testedHostUnavailable()));
        QVERIFY(unavailable.isValid());
        QVERIFY(selection->setProperty("hosts", entries({{"a", "Alpha"}, {"b", "Beta"}})));
        QVERIFY(selection->setProperty("testedUuid", "a"));
        QVERIFY(selection->property("testedHostAvailable").toBool());

        // Losing a different host does not cancel/invalidate the active host.
        QVERIFY(selection->setProperty("hosts", entries({{"a", "Alpha"}})));
        QCOMPARE(unavailable.size(), 0);
        QVERIFY(selection->setProperty("hosts", entries({{"b", "Beta"}})));
        QCOMPARE(unavailable.size(), 1);
        QVERIFY(!selection->property("testedHostAvailable").toBool());
        QCOMPARE(selection->property("selectedUuid").toString(), QString("b"));

        // Repeated offline updates do not repeatedly request cancellation.
        QVERIFY(selection->setProperty("hosts", QVariantList{}));
        QCOMPARE(unavailable.size(), 1);
        QVERIFY(selection->setProperty("hosts", entries({{"a", "Back online"}})));
        QVERIFY(selection->property("testedHostAvailable").toBool());
        QCOMPARE(unavailable.size(), 1);
        QVERIFY(selection->setProperty("hosts", QVariantList{}));
        QCOMPARE(unavailable.size(), 2);
    }
};

// QCoreApplication only: no windows, display connection, graphics device,
// discovery manager, sockets, or real calibration objects are constructed.
QTEST_GUILESS_MAIN(CalibrationHostsTest)
#include "tst_calibrationhosts.moc"
