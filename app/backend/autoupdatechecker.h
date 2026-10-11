#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QJsonObject>

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool notifyPrereleases READ notifyPrereleases WRITE setNotifyPrereleases NOTIFY notifyPrereleasesChanged)
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();
    bool notifyPrereleases() const;
    void setNotifyPrereleases(bool enabled);
    Q_INVOKABLE void dismissUpdate(const QString& version);


signals:
    void onUpdateAvailable(QString newVersion, QString url);
    void notifyPrereleasesChanged();

private slots:
    void handleUpdateCheckRequestFinished(QNetworkReply* reply);

private:
    friend class AutoUpdateCheckerTest;

    static QVector<int> installedWindowsVersion();

    static bool parseVersion(const QString& string, QVector<int>& version, bool windowsProduct = true);

    static int compareVersion(const QVector<int>& version1, const QVector<int>& version2);

    static QJsonObject findUpdate(const QJsonArray& releases, const QVector<int>& currentVersion,
                                 const QString& platform, const QString& architecture, bool allowPrereleases = true);
    void requestPage();
    void stopCheck();
    void createNetworkManager();
    void finishSelection();
    void announce(const QJsonObject& release);
    static bool validNightlyMetadata(const QJsonObject& release, const QJsonObject& metadata);

    QString getPlatform();

    QVector<int> m_CurrentVersionQuad;
    QNetworkAccessManager* m_Nam;
    QJsonArray m_Releases;
    QJsonObject m_Candidate;
    int m_Page = 1;
    bool m_Checking = false;

};
