#include "app/services/update/UpdateService.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
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

using miacode::update::UpdateEnvironment;
using miacode::update::UpdateFetcher;
using miacode::update::UpdateService;
using miacode::update::UpdateStateStore;

// 同步返回预置响应，并记录被请求的 URL。
class FakeFetcher final : public UpdateFetcher
{
public:
    void fetch(const QUrl& url, Callback callback) override
    {
        requestedUrls.append(url.toString());
        ++fetchCount;
        callback(nextOk, nextPayload, nextReason);
    }

    QStringList requestedUrls;
    int fetchCount = 0;
    bool nextOk = true;
    QByteArray nextPayload;
    QString nextReason;
};

class MemoryStore final : public UpdateStateStore
{
public:
    bool checkEnabled() const override { return enabled; }
    void setCheckEnabled(bool value) override { enabled = value; }
    QString channelToken() const override { return channel; }
    void setChannelToken(const QString& token) override { channel = token; }
    QDateTime lastCheckAt() const override { return checkedAt; }
    QString lastOutcome() const override { return outcome; }
    void recordCheck(const QDateTime& atUtc, const QString& value) override
    {
        checkedAt = atUtc;
        outcome = value;
        ++recordCount;
    }
    QString skippedVersion() const override { return skipped; }
    void setSkippedVersion(const QString& version) override { skipped = version; }
    QString knownVersion() const override { return known; }
    void setKnownVersion(const QString& version) override { known = version; }

    bool enabled = true;
    QString channel;
    QDateTime checkedAt;
    QString outcome;
    QString skipped;
    QString known;
    int recordCount = 0;
};

QByteArray manifestPayload(const QString& version)
{
    const QJsonObject root{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("channel"), QStringLiteral("beta")},
        {QStringLiteral("major"), 2},
        {QStringLiteral("version"), version},
        {QStringLiteral("releasedAt"), QStringLiteral("2026-09-20")},
        {QStringLiteral("releasePageUrl"),
         QStringLiteral("https://github.com/fanfaredash/MiaCode/releases/tag/v") + version},
        {QStringLiteral("notes"), QJsonObject{{QStringLiteral("en_US"), QStringLiteral("notes")}}},
        {QStringLiteral("platforms"), QJsonObject{
             {QStringLiteral("macos-arm64"), QJsonObject{
                  {QStringLiteral("file"), QStringLiteral("mac.7z")},
                  {QStringLiteral("bytes"), 84231168},
                  {QStringLiteral("sha256"), QStringLiteral("abc")},
                  {QStringLiteral("url"), QStringLiteral("https://example.invalid/mac.7z")},
              }},
         }},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// 当前构建固定当作 2.0.0-alpha / major 2 / macos-arm64 / zh_CN，
// 这样断言不随真实构建版本漂移。
UpdateEnvironment testEnvironment()
{
    UpdateEnvironment environment;
    environment.versionText = QStringLiteral("2.0.0-alpha");
    environment.major = 2;
    environment.platformKey = QStringLiteral("macos-arm64");
    environment.languageToken = QStringLiteral("zh_CN");
    return environment;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);
    bool ok = true;

    // ---- 通道默认值：prerelease 构建默认 beta ----
    {
        FakeFetcher fetcher;
        MemoryStore store;
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.1.0"));
        UpdateService service(fetcher, store, testEnvironment());
        service.checkNow(true);
        ok &= expect(fetcher.requestedUrls.size() == 1
                         && fetcher.requestedUrls.first().endsWith(QLatin1String("/2-beta.json")),
                     QStringLiteral("a prerelease build defaults to the beta channel"), err);
    }
    {
        FakeFetcher fetcher;
        MemoryStore store;
        UpdateEnvironment environment = testEnvironment();
        environment.versionText = QStringLiteral("2.0.0");
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.1.0"));
        UpdateService service(fetcher, store, environment);
        service.checkNow(true);
        ok &= expect(fetcher.requestedUrls.first().endsWith(QLatin1String("/2-stable.json")),
                     QStringLiteral("a release build defaults to the stable channel"), err);
    }
    {
        FakeFetcher fetcher;
        MemoryStore store;
        store.channel = QStringLiteral("stable");
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.1.0"));
        UpdateService service(fetcher, store, testEnvironment());
        service.checkNow(true);
        ok &= expect(fetcher.requestedUrls.first().endsWith(QLatin1String("/2-stable.json")),
                     QStringLiteral("an explicit channel choice overrides the build default"), err);
    }

    // ---- 找到更新 ----
    {
        FakeFetcher fetcher;
        MemoryStore store;
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.1.0"));
        UpdateService service(fetcher, store, testEnvironment());
        QSignalSpy reported(&service, &UpdateService::manualCheckFinished);
        service.checkNow(true);
        ok &= expect(service.updateAvailable() && service.availableVersion() == QLatin1String("2.1.0"),
                     QStringLiteral("a higher manifest version becomes the available update"), err);
        ok &= expect(store.known == QLatin1String("2.1.0"),
                     QStringLiteral("the found version is remembered for the next launch"), err);
        ok &= expect(store.outcome == QLatin1String("ok") && store.recordCount == 1,
                     QStringLiteral("a successful check is recorded once"), err);
        ok &= expect(reported.count() == 1
                         && reported.first().at(0).toString() == QLatin1String("available"),
                     QStringLiteral("a manual check reports that an update is available"), err);
    }

    // ---- 已是最新 ----
    {
        FakeFetcher fetcher;
        MemoryStore store;
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.0.0-alpha"));
        UpdateService service(fetcher, store, testEnvironment());
        QSignalSpy reported(&service, &UpdateService::manualCheckFinished);
        service.checkNow(true);
        ok &= expect(!service.updateAvailable(),
                     QStringLiteral("the same version is not an update"), err);
        ok &= expect(reported.count() == 1
                         && reported.first().at(0).toString() == QLatin1String("up-to-date"),
                     QStringLiteral("a manual check says so when already current"), err);
    }
    {
        FakeFetcher fetcher;
        MemoryStore store;
        fetcher.nextPayload = manifestPayload(QStringLiteral("1.9.9"));
        UpdateService service(fetcher, store, testEnvironment());
        service.checkNow(true);
        ok &= expect(!service.updateAvailable(),
                     QStringLiteral("an older manifest version never downgrades the user"), err);
    }

    // ---- 本平台无包 ----
    {
        FakeFetcher fetcher;
        MemoryStore store;
        UpdateEnvironment environment = testEnvironment();
        environment.platformKey = QString();
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.1.0"));
        UpdateService service(fetcher, store, environment);
        QSignalSpy reported(&service, &UpdateService::manualCheckFinished);
        service.checkNow(true);
        ok &= expect(!service.updateAvailable()
                         && reported.count() == 1
                         && reported.first().at(0).toString() == QLatin1String("no-package"),
                     QStringLiteral("no package for this platform is reported as such, not as latest"), err);
    }

    // ---- 失败路径 ----
    {
        FakeFetcher fetcher;
        MemoryStore store;
        fetcher.nextOk = false;
        fetcher.nextReason = QStringLiteral("host unreachable");
        UpdateService service(fetcher, store, testEnvironment());
        QSignalSpy reported(&service, &UpdateService::manualCheckFinished);
        service.checkNow(true);
        ok &= expect(reported.count() == 1
                         && reported.first().at(0).toString() == QLatin1String("failed"),
                     QStringLiteral("a manual check surfaces a network failure"), err);
        ok &= expect(store.outcome == QLatin1String("error"),
                     QStringLiteral("a failed check is recorded as an error"), err);
    }
    {
        FakeFetcher fetcher;
        MemoryStore store;
        fetcher.nextPayload = QByteArray("not json");
        UpdateService service(fetcher, store, testEnvironment());
        QSignalSpy reported(&service, &UpdateService::manualCheckFinished);
        service.checkNow(true);
        ok &= expect(reported.first().at(0).toString() == QLatin1String("failed"),
                     QStringLiteral("a malformed manifest is a failure, not silence"), err);
    }
    {
        // major 不匹配属于「不适用」，不是失败。
        FakeFetcher fetcher;
        MemoryStore store;
        UpdateEnvironment environment = testEnvironment();
        environment.major = 1;
        environment.versionText = QStringLiteral("1.1.0");
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.1.0"));
        UpdateService service(fetcher, store, environment);
        QSignalSpy reported(&service, &UpdateService::manualCheckFinished);
        service.checkNow(true);
        ok &= expect(!service.updateAvailable()
                         && reported.first().at(0).toString() == QLatin1String("up-to-date"),
                     QStringLiteral("a 1.x build is never pushed a 2.x manifest"), err);
    }

    // ---- 节流 ----
    {
        FakeFetcher fetcher;
        MemoryStore store;
        store.checkedAt = QDateTime::currentDateTimeUtc().addSecs(-3600);
        store.outcome = QStringLiteral("ok");
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.1.0"));
        UpdateService service(fetcher, store, testEnvironment());
        service.checkNow(false);
        ok &= expect(fetcher.fetchCount == 0,
                     QStringLiteral("an automatic check inside the 24h success window is skipped"), err);
        service.checkNow(true);
        ok &= expect(fetcher.fetchCount == 1,
                     QStringLiteral("a manual check ignores the throttle"), err);
    }
    {
        FakeFetcher fetcher;
        MemoryStore store;
        store.checkedAt = QDateTime::currentDateTimeUtc().addSecs(-5 * 3600);
        store.outcome = QStringLiteral("error");
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.1.0"));
        UpdateService service(fetcher, store, testEnvironment());
        service.checkNow(false);
        ok &= expect(fetcher.fetchCount == 1,
                     QStringLiteral("after a failure the window is 4h, so 5h later retries"), err);
    }
    {
        FakeFetcher fetcher;
        MemoryStore store;
        store.enabled = false;
        UpdateService service(fetcher, store, testEnvironment());
        service.checkNow(false);
        ok &= expect(fetcher.fetchCount == 0,
                     QStringLiteral("a disabled automatic check never reaches the network"), err);
        service.checkNow(true);
        ok &= expect(fetcher.fetchCount == 1,
                     QStringLiteral("the manual button still works while automatic checks are off"), err);
    }

    // ---- 跳过版本 ----
    {
        FakeFetcher fetcher;
        MemoryStore store;
        store.skipped = QStringLiteral("2.1.0");
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.1.0"));
        UpdateService service(fetcher, store, testEnvironment());
        service.checkNow(false);
        ok &= expect(!service.updateAvailable(),
                     QStringLiteral("an automatic check honours a skipped version"), err);
        service.checkNow(true);
        ok &= expect(service.updateAvailable(),
                     QStringLiteral("a manual check reports even a skipped version"), err);
    }
    {
        FakeFetcher fetcher;
        MemoryStore store;
        store.skipped = QStringLiteral("2.1.0");
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.2.0"));
        UpdateService service(fetcher, store, testEnvironment());
        service.checkNow(false);
        ok &= expect(service.updateAvailable(),
                     QStringLiteral("skipping one version does not mute the next one"), err);
    }
    {
        FakeFetcher fetcher;
        MemoryStore store;
        fetcher.nextPayload = manifestPayload(QStringLiteral("2.1.0"));
        UpdateService service(fetcher, store, testEnvironment());
        service.checkNow(true);
        service.skipAvailableVersion();
        ok &= expect(store.skipped == QLatin1String("2.1.0") && !service.updateAvailable(),
                     QStringLiteral("skipping clears the indicator and remembers the version"), err);
    }

    // ---- 启动时清理陈旧状态 ----
    {
        FakeFetcher fetcher;
        MemoryStore store;
        // 上次记下 2.0.0-alpha，而现在跑的就是它：标记必须自己消失。
        store.known = QStringLiteral("2.0.0-alpha");
        store.skipped = QStringLiteral("1.5.0");
        UpdateService service(fetcher, store, testEnvironment());
        ok &= expect(!service.updateAvailable(),
                     QStringLiteral("a known version the build has reached no longer shows as available"), err);
        ok &= expect(store.known.isEmpty(),
                     QStringLiteral("a reached known version is cleared from storage"), err);
        ok &= expect(store.skipped.isEmpty(),
                     QStringLiteral("a skipped version below the running build is cleared"), err);
    }
    {
        FakeFetcher fetcher;
        MemoryStore store;
        store.known = QStringLiteral("2.1.0");
        UpdateService service(fetcher, store, testEnvironment());
        ok &= expect(service.updateAvailable() && service.availableVersion() == QLatin1String("2.1.0"),
                     QStringLiteral("a known higher version shows immediately without the network"), err);
        ok &= expect(fetcher.fetchCount == 0,
                     QStringLiteral("restoring the known version does not touch the network"), err);
    }

    // ---- 并发保护 ----
    {
        // 假 fetcher 是同步的，所以这里用「正在进行」的语义来验证：
        // 一次 fetch 未回调完成前不得发起第二次。用一个不回调的 fetcher。
        class NeverCallsBack final : public UpdateFetcher
        {
        public:
            void fetch(const QUrl&, Callback) override { ++fetchCount; }
            int fetchCount = 0;
        };
        NeverCallsBack fetcher;
        MemoryStore store;
        UpdateService service(fetcher, store, testEnvironment());
        service.checkNow(true);
        service.checkNow(true);
        ok &= expect(fetcher.fetchCount == 1,
                     QStringLiteral("a second check while one is in flight is dropped"), err);
    }

    if (ok) {
        QTextStream(stdout) << "update_service_spec ok\n";
        return 0;
    }
    return 1;
}
