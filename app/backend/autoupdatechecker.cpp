#include "autoupdatechecker.h"

#include <QNetworkReply>
#include <QJsonDocument>
#include <QRegularExpression>

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) :
    QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);

    // Never communicate over HTTP
    m_Nam->setStrictTransportSecurityEnabled(true);
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    connect(m_Nam, &QNetworkAccessManager::finished,
            this, &AutoUpdateChecker::handleUpdateCheckRequestFinished);

    const QString currentVersion(VERSION_STR);
    qDebug() << "Current Moonlight version:" << currentVersion;
    if (!parseVersion(currentVersion, m_CurrentVersionQuad)) {
        // Development hashes and vrr-lite builds have no ordered release version.
        qDebug() << "Skipping update checks for an unversioned development build";
    }
}

void AutoUpdateChecker::start()
{
    if (!m_Nam || m_CurrentVersionQuad.isEmpty()) {
        return;
    }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE) // Only run update checker on platforms without auto-update
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    // The list includes published prereleases, which are used for VRR releases.
    // Never consult the upstream manifest or fall back to upstream releases.
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/Nonary/moonlight-qt/releases?per_page=100")));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "Moonlight-Nonary-UpdateChecker");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif
    m_Nam->get(request);
#endif
}

bool AutoUpdateChecker::parseVersion(const QString& string, QVector<int>& version)
{
    // Compare base version, VRR revision, then VRR patch numerically. A plain
    // base version sorts before its VRR releases; vrr18 sorts after vrr17.1.
    static const QRegularExpression pattern(QStringLiteral("\\Av?([0-9]+)\\.([0-9]+)\\.([0-9]+)(?:-vrr([0-9]+)(?:\\.([0-9]+))?)?\\z"));
    const auto match = pattern.match(string);
    version.clear();
    if (!match.hasMatch()) {
        return false;
    }
    for (int i = 1; i <= 5; ++i) {
        bool ok = true;
        const int component = match.captured(i).isEmpty() ? 0 : match.captured(i).toInt(&ok);
        if (!ok) {
            version.clear();
            return false;
        }
        version.append(component);
    }
    return true;
}

QString AutoUpdateChecker::getPlatform()
{
#if defined(STEAM_LINK)
    return QStringLiteral("steamlink");
#elif defined(APP_IMAGE)
    return QStringLiteral("appimage");
#elif defined(Q_OS_DARWIN)
    return QStringLiteral("osx");
#else
    return QSysInfo::productType();
#endif
}

int AutoUpdateChecker::compareVersion(const QVector<int>& version1, const QVector<int>& version2)
{
    for (int i = 0; i < qMax(version1.size(), version2.size()); ++i) {
        const int v1 = version1.value(i);
        const int v2 = version2.value(i);
        if (v1 != v2) {
            return v1 < v2 ? -1 : 1;
        }
    }
    return 0;
}

QJsonObject AutoUpdateChecker::findUpdate(const QJsonArray& releases, const QVector<int>& currentVersion,
                                        const QString& platform, const QString& architecture)
{
    if (currentVersion.isEmpty()) {
        return {};
    }

    QJsonObject latest;
    QVector<int> latestVersion = currentVersion;
    for (const auto& entry : releases) {
        const auto release = entry.toObject();
        if (!release.value("draft").isBool() || release.value("draft").toBool() ||
                !release.value("tag_name").isString() || !release.value("assets").isArray()) {
            continue;
        }

        const QString tag = release.value("tag_name").toString();
        QVector<int> version;
        if (!parseVersion(tag, version) || compareVersion(version, latestVersion) <= 0) {
            continue;
        }

        const QString versionString = tag.startsWith('v') ? tag.mid(1) : tag;
        QString assetName;
        if (platform == "windows") {
            const QString arch = architecture == "x86_64" ? QStringLiteral("x64") :
                                 architecture == "i386" ? QStringLiteral("x86") : architecture;
            assetName = QStringLiteral("MoonlightPortable-%1-%2.zip").arg(arch, versionString);
        }
        else if (platform == "appimage") {
            assetName = QStringLiteral("Moonlight-%1-%2.AppImage").arg(versionString, architecture);
        }
        else if (platform == "osx") {
            assetName = QStringLiteral("Moonlight-%1.dmg").arg(versionString);
        }
        else if (platform == "steamlink") {
            assetName = QStringLiteral("Moonlight-SteamLink-%1.zip").arg(versionString);
        }
        else {
            continue;
        }

        for (const auto& assetEntry : release.value("assets").toArray()) {
            const auto asset = assetEntry.toObject();
            if (asset.value("name").toString() == assetName &&
                    asset.value("state").toString() == "uploaded" && asset.value("size").toDouble() > 0) {
                latest = release;
                latestVersion = version;
                break;
            }
        }
    }
    return latest;
}

void AutoUpdateChecker::handleUpdateCheckRequestFinished(QNetworkReply* reply)
{
    Q_ASSERT(reply->isFinished());

    // Free resources and prevent the bearer plugin from polling in the background.
    m_Nam->deleteLater();
    m_Nam = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "Update checking failed with error:" << reply->error();
        return;
    }

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(reply->readAll(), &error);
    if (!document.isArray()) {
        qWarning() << "GitHub release list malformed:" << error.errorString();
        return;
    }

    const auto update = findUpdate(document.array(), m_CurrentVersionQuad,
                                   getPlatform(), QSysInfo::buildCpuArchitecture());
    if (update.isEmpty()) {
        qDebug() << "No newer compatible Nonary Moonlight release found";
        return;
    }

    const QString tag = update.value("tag_name").toString();
    const QString version = tag.startsWith('v') ? tag.mid(1) : tag;
    qDebug() << "Nonary Moonlight update available:" << version;
    emit onUpdateAvailable(version, QStringLiteral("https://github.com/Nonary/moonlight-qt/releases/tag/") + tag);
}
