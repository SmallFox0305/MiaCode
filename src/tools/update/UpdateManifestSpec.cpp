#include "app/services/update/UpdateManifest.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTextStream>

namespace {

bool expect(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

using miacode::update::ManifestStatus;
using miacode::update::parseManifest;

QJsonObject platformEntry(const QString& file, qint64 bytes)
{
    return QJsonObject{
        {QStringLiteral("file"), file},
        {QStringLiteral("bytes"), bytes},
        {QStringLiteral("sha256"), QStringLiteral("0123456789abcdef")},
        {QStringLiteral("url"), QStringLiteral("https://example.invalid/") + file},
    };
}

QJsonObject validManifest()
{
    return QJsonObject{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("channel"), QStringLiteral("beta")},
        {QStringLiteral("major"), 2},
        {QStringLiteral("version"), QStringLiteral("2.1.0-beta.3")},
        {QStringLiteral("releasedAt"), QStringLiteral("2026-09-20")},
        {QStringLiteral("releasePageUrl"),
         QStringLiteral("https://github.com/fanfaredash/MiaCode/releases/tag/v2.1.0-beta.3")},
        {QStringLiteral("mandatory"), false},
        {QStringLiteral("notes"), QJsonObject{
             {QStringLiteral("zh_CN"), QStringLiteral("中文说明")},
             {QStringLiteral("en_US"), QStringLiteral("English notes")},
         }},
        {QStringLiteral("platforms"), QJsonObject{
             {QStringLiteral("macos-arm64"), platformEntry(QStringLiteral("mac.7z"), 84231168)},
             {QStringLiteral("windows-x64"), platformEntry(QStringLiteral("win.7z"), 81002496)},
         }},
    };
}

QByteArray toPayload(const QJsonObject& root)
{
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);
    bool ok = true;

    const QString mac = QStringLiteral("macos-arm64");

    // ---- 正常路径 ----
    {
        const auto result = parseManifest(toPayload(validManifest()), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::Ok,
                     QStringLiteral("a well-formed manifest parses"), err);
        ok &= expect(result.manifest.versionText == QLatin1String("2.1.0-beta.3")
                         && result.manifest.version.minor == 1,
                     QStringLiteral("version is kept both as text and parsed"), err);
        ok &= expect(result.manifest.notes == QStringLiteral("中文说明"),
                     QStringLiteral("notes follow the requested language"), err);
        ok &= expect(result.manifest.package.bytes == 84231168
                         && result.manifest.package.file == QLatin1String("mac.7z"),
                     QStringLiteral("the platform entry is selected by key"), err);
        ok &= expect(result.manifest.releasePageUrl.contains(QLatin1String("releases/tag/")),
                     QStringLiteral("the release page url survives"), err);
    }

    // ---- 语言回退 ----
    {
        const auto result = parseManifest(toPayload(validManifest()), 2, mac, QStringLiteral("ja_JP"));
        ok &= expect(result.status == ManifestStatus::Ok
                         && result.manifest.notes == QLatin1String("English notes"),
                     QStringLiteral("a missing language falls back to en_US"), err);
    }
    {
        QJsonObject root = validManifest();
        root.insert(QStringLiteral("notes"), QJsonObject{});
        const auto result = parseManifest(toPayload(root), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::Ok && result.manifest.notes.isEmpty(),
                     QStringLiteral("no notes at all is still a valid manifest"), err);
    }

    // ---- NotApplicable：不是错误，是「这份 manifest 不适用于你」 ----
    {
        QJsonObject root = validManifest();
        root.insert(QStringLiteral("schema"), 2);
        const auto result = parseManifest(toPayload(root), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::NotApplicable,
                     QStringLiteral("an unknown schema is ignored, not an error"), err);
    }
    {
        const auto result = parseManifest(toPayload(validManifest()), 1, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::NotApplicable,
                     QStringLiteral("a 1.x build ignores a major-2 manifest"), err);
    }
    {
        const auto result = parseManifest(toPayload(validManifest()), 2,
                                          QStringLiteral("linux-x64"), QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::NotApplicable,
                     QStringLiteral("a platform with no package is not applicable"), err);
    }
    {
        const auto result = parseManifest(toPayload(validManifest()), 2,
                                          QString(), QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::NotApplicable,
                     QStringLiteral("an unresolvable platform key is not applicable"), err);
    }

    // ---- Invalid：结构坏了 ----
    {
        const auto result = parseManifest(QByteArray("not json at all"), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::Invalid,
                     QStringLiteral("non-JSON payload is invalid"), err);
    }
    {
        const auto result = parseManifest(QByteArray("[1,2,3]"), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::Invalid,
                     QStringLiteral("a JSON array is invalid"), err);
    }
    {
        QJsonObject root = validManifest();
        root.insert(QStringLiteral("version"), QStringLiteral("2.x"));
        const auto result = parseManifest(toPayload(root), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::Invalid,
                     QStringLiteral("an unparseable version is invalid"), err);
    }
    {
        QJsonObject root = validManifest();
        root.remove(QStringLiteral("releasePageUrl"));
        const auto result = parseManifest(toPayload(root), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::Invalid,
                     QStringLiteral("a manifest without a release page is invalid"), err);
    }
    {
        QJsonObject root = validManifest();
        root.insert(QStringLiteral("releasePageUrl"), QStringLiteral("javascript:alert(1)"));
        const auto result = parseManifest(toPayload(root), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::Invalid,
                     QStringLiteral("a non-https release page is rejected"), err);
    }
    {
        QJsonObject root = validManifest();
        root.remove(QStringLiteral("platforms"));
        const auto result = parseManifest(toPayload(root), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::Invalid,
                     QStringLiteral("a manifest without a platforms object is invalid"), err);
    }
    {
        QJsonObject root = validManifest();
        root.remove(QStringLiteral("major"));
        const auto result = parseManifest(toPayload(root), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(result.status == ManifestStatus::Invalid,
                     QStringLiteral("a manifest without a major is invalid"), err);
    }

    // ---- 每条失败路径都要带上可写进日志的原因 ----
    {
        const auto result = parseManifest(QByteArray("not json"), 2, mac, QStringLiteral("zh_CN"));
        ok &= expect(!result.reason.isEmpty(),
                     QStringLiteral("a failure carries a loggable reason"), err);
    }

    if (ok) {
        QTextStream(stdout) << "update_manifest_spec ok\n";
        return 0;
    }
    return 1;
}
