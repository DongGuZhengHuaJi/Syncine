//
// 音频输出设备的公共工具：「所有出声的地方共用一个听筒」的粘合层。
//
// 设备列表只有一个来源：WebRTC 的 ADM（名字为 PulseAudio 给出的 sink
// description）。但放声音的对象有三个，分属两套体系：对方语音走 WebRTC 的 ADM
// （认索引）；推来的电影声（MovieAudioPlayer 的 QAudioSink）与本机播放的电影声
// （PlaybackController 的 QAudioOutput）是 Qt Multimedia 对象，拿不到 ADM 索引，
// 只能按名字认设备。
//
// 后两者需自行在 QMediaDevices 中查找：Qt 给出的字符串与 PulseAudio 一致
// （同为 sink description），按名字精确匹配即可对上，见 findAudioOutputByName。
// 匹配不上时退回系统默认——设备被拔出是常态，退回默认优于完全不出声。
//

#ifndef SYNCINE_AUDIODEVICES_H
#define SYNCINE_AUDIODEVICES_H

#include <QAudioDevice>
#include <QMediaDevices>
#include <QString>

#include "core/Log.h"

// 设备名 → 输出设备。空名字 = 跟随系统默认。
inline QAudioDevice findAudioOutputByName(const QString &name) {
    if (name.isEmpty())
        return QMediaDevices::defaultAudioOutput();

    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    for (const QAudioDevice &device : outputs) {
        if (device.description() == name)
            return device;
    }

    LOG_WARN("Audio") << "输出设备「" << name << "」在 Qt 侧不存在，改用系统默认设备";
    return QMediaDevices::defaultAudioOutput();
}

#endif //SYNCINE_AUDIODEVICES_H
