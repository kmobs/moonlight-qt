#include "../../app/backend/autoupdatechecker.h"

#include <QNetworkReply>
#include <QtTest>
#include <cstring>

class ReleaseReply : public QNetworkReply
{
public:
    ReleaseReply(const QByteArray& data, NetworkError error = NoError) : m_Data(data)
    {
        open(QIODevice::ReadOnly);
        setFinished(true);
        if (error != NoError) {
            setError(error, QStringLiteral("Update request failed"));
        }
    }

    void abort() override {}
    qint64 bytesAvailable() const override { return m_Data.size() - m_Offset + QNetworkReply::bytesAvailable(); }

protected:
    qint64 readData(char* data, qint64 maximum) override
    {
        const qint64 length = qMin(maximum, qint64(m_Data.size()) - m_Offset);
        if (length == 0) return -1;
        std::memcpy(data, m_Data.constData() + m_Offset, size_t(length));
        m_Offset += length;
        return length;
    }

private:
    QByteArray m_Data;
    qint64 m_Offset = 0;
};

class AutoUpdateCheckerTest : public QObject
{
    Q_OBJECT

    static QJsonObject release(const QString& tag, const QString& asset, bool prerelease = false)
    {
        return {{"tag_name", tag}, {"draft", false}, {"prerelease", prerelease},
                {"assets", QJsonArray{QJsonObject{{"name", asset}, {"state", "uploaded"}, {"size", 100}}}}};
    }

    static QVector<int> version(const QString& string)
    {
        QVector<int> result;
        AutoUpdateChecker::parseVersion(string, result);
        return result;
    }

private slots:
    void versionOrdering()
    {
        const QStringList ordered{"6.1.0", "v6.1.0-vrr9", "6.1.0-vrr10", "6.1.0-vrr17",
                                  "6.1.0-vrr17.1", "6.1.0-vrr17.10", "6.1.0-vrr18", "6.2.0"};
        for (int i = 1; i < ordered.size(); ++i) {
            QCOMPARE(AutoUpdateChecker::compareVersion(version(ordered[i - 1]), version(ordered[i])), -1);
            QCOMPARE(AutoUpdateChecker::compareVersion(version(ordered[i]), version(ordered[i - 1])), 1);
        }
        QCOMPARE(AutoUpdateChecker::compareVersion(version("v6.1.0-vrr18"), version("6.1.0-vrr18.0")), 0);
        for (const QString& invalid : {"6.1.0-vrr-lite", "abcdef", "6.1.bad", "6.1.0-vrr18junk",
                                       "6.1.0-vrr99999999999999999999", "6.1.0\n", ""}) {
            QVERIFY2(version(invalid).isEmpty(), qPrintable(invalid));
        }
    }

    void publishedPrereleasesAndNumericSelection()
    {
        auto draft = release("v6.1.0-vrr20", "MoonlightPortable-x64-6.1.0-vrr20.zip");
        draft["draft"] = true;
        const QJsonArray releases{
            release("v6.1.0-vrr17.1", "MoonlightPortable-x64-6.1.0-vrr17.1.zip"),
            draft,
            release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip", true),
            release("v6.1.0-vrr19", "Moonlight-6.1.0-vrr19-x86_64.AppImage"),
            release("v6.1.0-vrr9", "MoonlightPortable-x64-6.1.0-vrr9.zip")};
        QCOMPARE(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr17"), "windows", "x86_64")
                 .value("tag_name").toString(), QString("v6.1.0-vrr18"));
        QVERIFY(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr18"), "windows", "x86_64").isEmpty());
        QVERIFY(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr21"), "windows", "x86_64").isEmpty());
        QVERIFY(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr-lite"), "windows", "x86_64").isEmpty());
    }

    void platformAndArchitecture()
    {
        const QJsonArray releases{
            release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip"),
            release("v6.1.0-vrr19", "MoonlightPortable-arm64-6.1.0-vrr19.zip"),
            release("v6.1.0-vrr20", "Moonlight-6.1.0-vrr20-x86_64.AppImage"),
            release("v6.1.0-vrr21", "Moonlight-6.1.0-vrr21.dmg"),
            release("v6.1.0-vrr22", "Moonlight-SteamLink-6.1.0-vrr22.zip"),
            release("v6.1.0-vrr23", "MoonlightPortable-x86-6.1.0-vrr23.zip")};
        const auto current = version("6.1.0-vrr17.1");
        auto selectedTag = [&](const QString& platform, const QString& arch) {
            return AutoUpdateChecker::findUpdate(releases, current, platform, arch).value("tag_name").toString();
        };
        QCOMPARE(selectedTag("windows", "x86_64"), QString("v6.1.0-vrr18"));
        QCOMPARE(selectedTag("windows", "arm64"), QString("v6.1.0-vrr19"));
        QCOMPARE(selectedTag("windows", "i386"), QString("v6.1.0-vrr23"));
        QCOMPARE(selectedTag("appimage", "x86_64"), QString("v6.1.0-vrr20"));
        QVERIFY(selectedTag("appimage", "arm64").isEmpty());
        QCOMPARE(selectedTag("osx", "arm64"), QString("v6.1.0-vrr21"));
        QCOMPARE(selectedTag("steamlink", "arm"), QString("v6.1.0-vrr22"));
        QVERIFY(selectedTag("linux", "x86_64").isEmpty());
    }

    void malformedAndIncompleteReleases()
    {
        auto missingDraft = release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip");
        missingDraft.remove("draft");
        auto incomplete = release("v6.1.0-vrr19", "MoonlightPortable-x64-6.1.0-vrr19.zip");
        incomplete["assets"] = QJsonArray{QJsonObject{{"name", "MoonlightPortable-x64-6.1.0-vrr19.zip"},
                                                    {"state", "new"}, {"size", 0}}};
        const QJsonArray releases{QJsonValue("invalid"), QJsonObject{}, missingDraft, incomplete,
            release("v6.1.0-vrr20oops", "MoonlightPortable-x64-6.1.0-vrr20oops.zip"),
            // The upstream manifest format must never produce an update.
            QJsonObject{{"platform", "windows"}, {"arch", "x86_64"}, {"version", "99.0.0"},
                        {"browser_url", "https://github.com/moonlight-stream/moonlight-qt/releases"}}};
        QVERIFY(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr17.1"), "windows", "x86_64").isEmpty());
    }

    void notificationUsesOwnRepository()
    {
        AutoUpdateChecker checker;
        QSignalSpy notifications(&checker, &AutoUpdateChecker::onUpdateAvailable);
        auto update = release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip");
        update["html_url"] = "https://github.com/moonlight-stream/moonlight-qt/releases";
        auto* reply = new ReleaseReply(QJsonDocument(QJsonArray{update}).toJson());
        checker.handleUpdateCheckRequestFinished(reply);
#if defined(Q_OS_WIN)
        QCOMPARE(notifications.size(), 1);
        QCOMPARE(notifications[0][0].toString(), QString("6.1.0-vrr18"));
        QCOMPARE(notifications[0][1].toString(), QString("https://github.com/Nonary/moonlight-qt/releases/tag/v6.1.0-vrr18"));
#else
        QCOMPARE(notifications.size(), 0);
#endif
    }

    void failedResponsesStayQuiet()
    {
        for (const QByteArray& data : {QByteArray("bad json"), QByteArray("{\"message\":\"API rate limit exceeded\"}"), QByteArray("[]")}) {
            AutoUpdateChecker checker;
            QSignalSpy notifications(&checker, &AutoUpdateChecker::onUpdateAvailable);
            checker.handleUpdateCheckRequestFinished(new ReleaseReply(data));
            QCOMPARE(notifications.size(), 0);
        }
        AutoUpdateChecker checker;
        QSignalSpy notifications(&checker, &AutoUpdateChecker::onUpdateAvailable);
        checker.handleUpdateCheckRequestFinished(new ReleaseReply("[]", QNetworkReply::ContentAccessDenied));
        QCOMPARE(notifications.size(), 0);
    }
};

QTEST_GUILESS_MAIN(AutoUpdateCheckerTest)
#include "tst_autoupdatechecker.moc"
