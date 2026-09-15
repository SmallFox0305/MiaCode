#include "app/services/update/UpdateManifest.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSysInfo>
#include <QUrl>

namespace miacode::update {
namespace {

ManifestParseResult failure(ManifestStatus status, const QString& reason)
{
    ManifestParseResult result;
    result.status = status;
    result.reason = reason;
    return result;
}

PlatformPackage readPackage(const QJsonObject& entry)
{
    PlatformPackage package;
    package.file = entry.value(QStringLiteral("file")).toString();
    package.bytes = static_cast<qint64>(entry.value(QStringLiteral("bytes")).toDouble(0));
    package.sha256 = entry.value(QStringLiteral("sha256")).toString();
    package.url = entry.value(QStringLiteral("url")).toString();
    package.minOsVersion = entry.value(QStringLiteral("minOsVersion")).toString();
    return package;
}

QString selectNotes(const QJsonObject& notes, const QString& languageToken)
{
    if (notes.contains(languageToken)) {
        return notes.value(languageToken).toString();
    }
    return notes.value(QStringLiteral("en_US")).toString();
}

} // namespace

ManifestParseResult parseManifest(const QByteArray& payload,
                                  int selfMajor,
                                  const QString& platformKey,
                                  const QString& languageToken)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(ManifestStatus::Invalid,
                       QStringLiteral("manifest is not a JSON object: %1").arg(parseError.errorString()));
    }
    const QJsonObject root = document.object();

    const QJsonValue schemaValue = root.value(QStringLiteral("schema"));
    if (!schemaValue.isDouble()) {
        return failure(ManifestStatus::Invalid, QStringLiteral("manifest has no numeric schema"));
    }
    if (schemaValue.toInt() > kSupportedManifestSchema) {
        return failure(ManifestStatus::NotApplicable,
                       QStringLiteral("manifest schema %1 is newer than this build understands")
                           .arg(schemaValue.toInt()));
    }

    const QJsonValue majorValue = root.value(QStringLiteral("major"));
    if (!majorValue.isDouble()) {
        return failure(ManifestStatus::Invalid, QStringLiteral("manifest has no numeric major"));
    }
    if (majorValue.toInt() != selfMajor) {
        // 版本族隔离：1.x 与 2.x 是两套独立应用，互不推送。
        return failure(ManifestStatus::NotApplicable,
                       QStringLiteral("manifest targets major %1, this build is major %2")
                           .arg(majorValue.toInt()).arg(selfMajor));
    }

    const QString versionText = root.value(QStringLiteral("version")).toString();
    const auto version = SemanticVersion::parse(versionText);
    if (!version.has_value()) {
        return failure(ManifestStatus::Invalid,
                       QStringLiteral("manifest version is unparseable: '%1'").arg(versionText));
    }

    const QString releasePageUrl = root.value(QStringLiteral("releasePageUrl")).toString();
    const QUrl parsedUrl(releasePageUrl);
    if (releasePageUrl.isEmpty() || !parsedUrl.isValid()
        || parsedUrl.scheme() != QLatin1String("https")) {
        // 这个 URL 会被交给系统浏览器打开，所以 scheme 必须是 https。
        return failure(ManifestStatus::Invalid,
                       QStringLiteral("release page url is not https: '%1'").arg(releasePageUrl));
    }

    const QJsonValue platformsValue = root.value(QStringLiteral("platforms"));
    if (!platformsValue.isObject()) {
        return failure(ManifestStatus::Invalid, QStringLiteral("manifest has no platforms object"));
    }
    if (platformKey.isEmpty()) {
        return failure(ManifestStatus::NotApplicable,
                       QStringLiteral("the running platform has no release key"));
    }
    const QJsonObject platforms = platformsValue.toObject();
    if (!platforms.contains(platformKey)) {
        return failure(ManifestStatus::NotApplicable,
                       QStringLiteral("no package for platform '%1'").arg(platformKey));
    }

    ManifestParseResult result;
    result.status = ManifestStatus::Ok;
    result.manifest.channel = root.value(QStringLiteral("channel")).toString();
    result.manifest.major = majorValue.toInt();
    result.manifest.versionText = versionText;
    result.manifest.version = *version;
    result.manifest.releasedAt = root.value(QStringLiteral("releasedAt")).toString();
    result.manifest.releasePageUrl = releasePageUrl;
    result.manifest.mandatory = root.value(QStringLiteral("mandatory")).toBool(false);
    result.manifest.notes = selectNotes(root.value(QStringLiteral("notes")).toObject(), languageToken);
    result.manifest.package = readPackage(platforms.value(platformKey).toObject());
    return result;
}

QString currentPlatformKey()
{
    // 取值必须与 .github/workflows/package.yml 矩阵里的 platform 字段一致。
    const QString architecture = QSysInfo::currentCpuArchitecture();
#if defined(Q_OS_MACOS)
    // macOS 发布版只有 Apple Silicon 一种包（CMakeLists 强制 arm64）。
    // Rosetta 下跑出来的 x86_64 会走到空串，调用方按「本平台无包」处理。
    if (architecture == QLatin1String("arm64")) {
        return QStringLiteral("macos-arm64");
    }
    return QString();
#elif defined(Q_OS_WIN)
    if (architecture == QLatin1String("arm64")) {
        return QStringLiteral("windows-arm64");
    }
    if (architecture == QLatin1String("x86_64")) {
        return QStringLiteral("windows-x64");
    }
    return QString();
#else
    return QString();
#endif
}

} // namespace miacode::update
