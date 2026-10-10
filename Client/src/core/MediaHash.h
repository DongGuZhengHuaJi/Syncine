//
// 媒体内容哈希 —— "两个人手里是不是同一部片子"的判据。
//
// 为什么单独抽出来:这个哈希有两个用处,而且**必须用同一套算法**,
// 否则两边判断会打架:
//   1. 播放器加载了某个视频后上报 video_status(PlaybackController::hash)
//   2. 同步模式下给播放列表某一条指定了本机文件时上报 playlist_status
//      —— 这时文件还没进播放器(甚至不是当前条目),不能借道播放器
//
// 算法:文件前 512KB + 文件大小的 SHA256。
// 只读开头是为了快(几毫秒),加文件大小是为了挡住"同一个开头、不同长度"的情况
// (比如预告片和正片)。前缀相同的不同版本(1080p vs 4K)会得到不同哈希 ——
// 这正是我们要的:它们本来就该算"不同步"。
//

#ifndef SYNCINE_MEDIAHASH_H
#define SYNCINE_MEDIAHASH_H

#include <QString>

namespace MediaHash {

// 本地文件的内容哈希;文件读不了返回空串
QString forFile(const QString &path);

// 非本地源(URL 等)退化成对字符串本身取哈希 —— 同一个链接算同一个"片子"
QString forText(const QString &text);

} // namespace MediaHash

#endif //SYNCINE_MEDIAHASH_H
