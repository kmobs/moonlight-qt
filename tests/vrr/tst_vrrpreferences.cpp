#include "settings/streamingpreferences.h"
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

// These tests exercise settings persistence without inspecting the display.
namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

class VrrPreferencesTest : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
private slots:
    void initTestCase()
    {
        QVERIFY(directory.isValid());
        QCoreApplication::setOrganizationName("MoonlightVrrSettingsTest");
        QCoreApplication::setApplicationName("IsolatedPreferences");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory.path());
    }
    void init() { QSettings().clear(); }
    void migration()
    {
        auto* prefs = StreamingPreferences::get();
        prefs->reload();
        QCOMPARE(prefs->vrrBufferPerMille(), 1000);
        for (int mode : {0, 1, 2}) {
            QSettings().setValue("vrrlatencymode", mode);
            prefs->reload();
            const auto expected = VrrTimingOptions::preset(mode);
            QCOMPARE(prefs->vrrBufferPerMille(), expected.bufferPerMille);
            QCOMPARE(prefs->vrrTargetHundredths(), expected.targetHundredths);
            QCOMPARE(prefs->vrrHistorySeconds(), expected.historySeconds);
            QCOMPARE(prefs->vrrToleranceUs(), expected.toleranceUs);
        }
    }
    void customRoundTripAndPresetReset()
    {
        auto* prefs = StreamingPreferences::get();
        prefs->reload();
        prefs->setVrrBufferPerMille(750);
        prefs->setVrrTargetHundredths(9725);
        prefs->setVrrHistorySeconds(30);
        prefs->setVrrToleranceUs(1500);
        prefs->save();
        prefs->applyVrrPreset(0);
        prefs->reload();
        QCOMPARE(prefs->vrrBufferPerMille(), 750);
        QCOMPARE(prefs->vrrTargetHundredths(), 9725);
        QCOMPARE(prefs->vrrHistorySeconds(), 30);
        QCOMPARE(prefs->vrrToleranceUs(), 1500);
        prefs->applyVrrPreset(2);
        prefs->save();
        prefs->reload();
        QCOMPARE(prefs->vrrBufferPerMille(), 500);
        QCOMPARE(prefs->vrrTargetHundredths(), 9900);
        QCOMPARE(prefs->vrrHistorySeconds(), 60);
        QCOMPARE(prefs->vrrToleranceUs(), 500);
    }
    void invalidSavedValues()
    {
        QSettings saved;
        saved.setValue("vrrbufferpermille", -9);
        saved.setValue("vrrtargethundredths", 100000);
        saved.setValue("vrrhistoryseconds", "bad");
        saved.setValue("vrrtoleranceus", 1600);
        auto* prefs = StreamingPreferences::get();
        prefs->reload();
        QCOMPARE(prefs->vrrBufferPerMille(), 250);
        QCOMPARE(prefs->vrrTargetHundredths(), 9999);
        QCOMPARE(prefs->vrrHistorySeconds(), 120);
        QCOMPARE(prefs->vrrToleranceUs(), 1500);
        prefs->setVrrToleranceUs(-1);
        prefs->setVrrTargetHundredths(12);
        prefs->setVrrHistorySeconds(99999);
        prefs->setVrrBufferPerMille(99999);
        QCOMPARE(prefs->vrrToleranceUs(), 250);
        QCOMPARE(prefs->vrrTargetHundredths(), 9000);
        QCOMPARE(prefs->vrrHistorySeconds(), 300);
        QCOMPARE(prefs->vrrBufferPerMille(), 4000);
    }
};
QTEST_GUILESS_MAIN(VrrPreferencesTest)
#include "tst_vrrpreferences.moc"
