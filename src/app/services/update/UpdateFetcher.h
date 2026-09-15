#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <functional>

namespace miacode::update {

// 取 manifest 的端口。生产实现是唯一使用 QNetworkAccessManager 的地方；
// spec 注入一个同步的假实现。
class UpdateFetcher
{
public:
    // reason 只在 ok == false 时有意义，写日志用，不展示给用户。
    using Callback = std::function<void(bool ok, QByteArray payload, QString reason)>;

    virtual ~UpdateFetcher() = default;

    virtual void fetch(const QUrl& url, Callback callback) = 0;
};

} // namespace miacode::update
