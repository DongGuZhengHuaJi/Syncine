//
// 媒体内容哈希：判断「两端是否为同一部片子」的依据。
//
// 两处使用且必须为同一套算法：播放器加载后上报 video_status；同步模式下为条目
// 指定本机文件时上报 playlist_status（此时文件尚未进入播放器）。
//
// 算法为文件前 512KB + 文件大小的 SHA256：只读开头是为了速度（毫秒级），
// 计入文件大小用于区分开头相同但长度不同的文件（如预告片与正片）。
//

#ifndef SYNCINE_MEDIAHASH_H
#define SYNCINE_MEDIAHASH_H

#include <QString>

namespace MediaHash {

// 本地文件的内容哈希；文件读不了返回空串
QString forFile(const QString &path);

// 非本地源（URL 等）退化为对字符串本身取哈希，使同一链接算作同一部片子
QString forText(const QString &text);

} // namespace MediaHash

#endif //SYNCINE_MEDIAHASH_H
