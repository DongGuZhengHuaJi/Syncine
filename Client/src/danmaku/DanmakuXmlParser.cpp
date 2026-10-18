#include "DanmakuXmlParser.h"

#include <QStringList>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMinFontSize = 12;
constexpr int kMaxFontSize = 64;
constexpr int kMaxTextLength = 500;

// p 字段不够时跳过；B 站目前固定 8 段（新版可能多出 weight 等，忽略即可）。
// pool 位于第 6 段（下标 5）：1 是字幕池，0 是普通弹幕。
bool parseCoordinate(const QString &text, DanmakuItem *item, bool *isSubtitle)
{
    const QStringList fields = text.split(QLatin1Char(','));
    if (fields.size() < 4)
        return false;

    bool ok = false;
    const double seconds = fields[0].toDouble(&ok);
    if (!ok || !std::isfinite(seconds) || seconds < 0)
        return false;
    item->timeMs = static_cast<qint64>(std::llround(seconds * 1000.0));

    item->mode = fields[1].toInt(&ok);
    if (!ok)
        return false;

    const int size = fields[2].toInt(&ok);
    item->fontSize = ok ? std::clamp(size, kMinFontSize, kMaxFontSize) : 25;

    const uint color = fields[3].toUInt(&ok);
    item->color = ok ? (color & 0xFFFFFFu) : 0xFFFFFFu;

    *isSubtitle = false;
    if (fields.size() >= 6) {
        const int pool = fields[5].toInt(&ok);
        *isSubtitle = ok && pool != 0;
    }
    return true;
}

} // namespace

namespace DanmakuXmlParser {

DanmakuParseResult parse(const QByteArray &xml, int maxItems)
{
    DanmakuParseResult result;

    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement() || reader.name() != QLatin1String("d"))
            continue;

        ++result.total;

        DanmakuItem item;
        bool isSubtitle = false;
        // p 只是一个普通属性；不直接拆它的返回值，交给 parseCoordinate。
        const QString coordinate =
            reader.attributes().value(QLatin1String("p")).toString();

        // readElementText() 会把解析器推进到 </d>，即使后面要丢弃也要先读。
        const QString text = reader.readElementText().trimmed();

        if (text.isEmpty()
            || !parseCoordinate(coordinate, &item, &isSubtitle)
            || isSubtitle
            || item.mode < 1 || item.mode > 6) {
            ++result.skipped;
            continue;
        }

        item.text = text.left(kMaxTextLength);
        result.items.append(item);

        if (maxItems > 0 && result.items.size() >= maxItems) {
            result.truncated = true;
            break;
        }
    }

    if (reader.hasError()) {
        result.ok = false;
        result.error = reader.errorString();
        result.items.clear();
        return result;
    }

    std::stable_sort(result.items.begin(), result.items.end(),
                     [](const DanmakuItem &a, const DanmakuItem &b) {
                         return a.timeMs < b.timeMs;
                     });
    result.ok = true;
    return result;
}

} // namespace DanmakuXmlParser
