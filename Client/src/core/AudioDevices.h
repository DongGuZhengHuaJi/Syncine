//
// 音频输出设备的公共小工具 —— "所有出声的地方共用一个听筒"这条需求的粘合剂。
//
// 【为什么需要它】
//
// 设备列表只有一个来源:WebRTC 的 ADM(麦克风/听筒两栏都是从它枚举出来的,
// 名字是 PulseAudio 给的 sink description)。但真正放声音的对象有三个,
// 分属两套体系:
//
//   对方语音        → WebRTC 的 ADM          ← 认索引,把索引喂给它就行
//   推过来的电影声   → MovieAudioPlayer 的 QAudioSink    ┐ 拿不到索引,
//   本机播放的电影声 → PlaybackController 的 QAudioOutput ┘ 只能按**名字**认设备
//
// 后两个是 Qt Multimedia 的对象,得自己去 QMediaDevices 里找对应的设备。
// Qt 这边给的字符串和 PulseAudio 给的是同一个(都是 sink 的 description),
// 所以按名字精确匹配就能对上 —— 见 findAudioOutputByName。
//
// 【匹配不上怎么办】
//
// 退回系统默认。设备被拔掉是常态(上次用的 USB 耳机这次没插),退回默认
// 总比一个都不出声强,而且用户下次打开设置还会看见它不在了。
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

    LOG_WARN("Audio") << "输出设备\"" << name << "\"在 Qt 这边找不到,先用系统默认";
    return QMediaDevices::defaultAudioOutput();
}

#endif //SYNCINE_AUDIODEVICES_H
