#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QJsonObject>

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();

signals:
    void onUpdateAvailable(QString newVersion, QString url);

private slots:
    void handleUpdateCheckRequestFinished(QNetworkReply* reply);

private:
    friend class AutoUpdateCheckerTest;

    static bool parseVersion(const QString& string, QVector<int>& version);

    static int compareVersion(const QVector<int>& version1, const QVector<int>& version2);

    static QJsonObject findUpdate(const QJsonArray& releases, const QVector<int>& currentVersion,
                                 const QString& platform, const QString& architecture);

    QString getPlatform();

    QVector<int> m_CurrentVersionQuad;
    QNetworkAccessManager* m_Nam;
};
