//
// B 站弹幕 XML 解析器（纯逻辑，不依赖网络/房间/播放器）。
//
// 输入是 comment.bilibili.com/<cid>.xml 的整份内容：
//   <i>
//     <d p="time,mode,size,color,timestamp,pool,uidHash,dmid[,weight]">text</d>
//     ...
//   </i>
// time 是秒（5 位小数），mode 1/2/3 滚动、4 底部、5 顶部、6 逆向，
// 7/8/9 是高级/代码/BAS 弹幕，本播放器不支持，直接丢弃。
//
// 原始 XML **不按时间排序**（实测顺序杂乱），所以解析完必须排序；
// 调用方拿到的 items 已按 timeMs 升序。
//

#ifndef SYNCINE_DANMAKUXMLPARSER_H
#define SYNCINE_DANMAKUXMLPARSER_H

#include <QByteArray>
#include <QList>
#include <QString>

struct DanmakuItem {
    qint64 timeMs = 0;
    int mode = 1;          // 1/2/3 滚动，4 底部，5 顶部，6 逆向
    int fontSize = 25;     // XML 里的原始字号，显示时按播放器高度缩放
    quint32 color = 0xFFFFFF;
    QString text;
};

struct DanmakuParseResult {
    QList<DanmakuItem> items;
    int total = 0;          // 遇到的 <d> 总数
    int skipped = 0;        // 被过滤的数量（模式/池/空文本/字段错误）
    bool truncated = false; // 达到 maxItems 上限，后面没再读
    bool ok = false;        // XML 本身是否合法（false 时 items 为空）
    QString error;
};

namespace DanmakuXmlParser {

// maxItems 是防御性上限：正常视频几千条，恶意/异常文件可能极大。
DanmakuParseResult parse(const QByteArray &xml, int maxItems = 20000);

} // namespace DanmakuXmlParser

#endif //SYNCINE_DANMAKUXMLPARSER_H
