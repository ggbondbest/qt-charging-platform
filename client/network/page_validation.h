#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>

namespace charging::client::network {
inline bool readPage(const QJsonObject& data, const QString& key, int page, int size, bool* more)
{
    const auto integer = [](const QJsonValue& v, int lo, int hi) {
        return v.isDouble() && std::isfinite(v.toDouble()) && std::floor(v.toDouble()) == v.toDouble()
            && v.toDouble() >= lo && v.toDouble() <= hi;
    };
    if (!integer(data.value("page"), 1, 2147483647) || data.value("page").toInt() != page
        || !integer(data.value("pageSize"), 1, 100) || data.value("pageSize").toInt() != size
        || !integer(data.value("total"), 0, 2147483647) || !data.value(key).isArray()) return false;
    const auto count = data.value(key).toArray().size();
    const qint64 offset = (qint64(page) - 1) * size;
    const qint64 total = data.value("total").toInt();
    if (count > size || (count == 0 && total > offset)
        || (count > 0 && offset + count > total)) return false;
    *more = offset + count < total;
    return true;
}
} // namespace charging::client::network
