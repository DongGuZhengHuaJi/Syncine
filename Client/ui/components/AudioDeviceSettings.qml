import QtQuick
import QtQuick.Controls
import SyncineApp

// 「麦克风 / 听筒」两个设备选择行。
//
// 设置页的音频卡片和房间里的设置弹窗**共用这一份** —— 两处要显示的东西完全
// 一样，分成两份迟早会不一致（而且房间那边本来就该是"同一份设置的快捷入口"）。
//
// 用法（宿主负责在合适的时机刷新一次设备列表，这里不自己刷）:
//     AudioDeviceSettings { width: parent.width }
//     // 设置页：onSectionChanged 里 if (section === "audio") signalingChannel.refreshAudioDevices()
//     // 房间里：弹窗 onOpened 里 signalingChannel.refreshAudioDevices()
//
// 不在 Component.onCompleted 中自行刷新的原因：本组件多被置于 Card /
// Flickable 内，父项隐藏时子项的 visible 不会随之变化——依据 visible 判断
// 「当前页是否被切换」并不可靠，交由宿主处理最为稳妥（与 CookieManager 的
// refreshBrowsers 写法一致）。
Column {
    id: root

    width: parent ? parent.width : 0
    spacing: 10

    // 枚举不到设备（声音服务没起来）时给一句人话，而不是两个空列表
    readonly property bool noDevice:
        signalingChannel.micDevices.length === 0
        && signalingChannel.speakerDevices.length === 0

    Label {
        width: parent.width
        visible: root.noDevice
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: Style.danger
        text: "未找到可用音频设备。请确认系统声音服务（PipeWire / PulseAudio）正在运行，"
              + "然后重新打开本页。"
    }

    Label {
        text: "麦克风"
        font.pixelSize: 13
        color: Style.textPrimary
    }

    OptionList {
        // value 是**设备名**,空串 = 跟随系统默认。
        // 用名字而不是索引：索引会随设备插拔漂移（见 WebrtcManager.h）
        options: signalingChannel.micDevices
        selectedValue: signalingChannel.micDevice
        onPicked: (value) => signalingChannel.setMicDevice(value)
    }

    Label {
        text: "听筒 / 扬声器"
        topPadding: 4
        font.pixelSize: 13
        color: Style.textPrimary
    }

    OptionList {
        options: signalingChannel.speakerDevices
        selectedValue: signalingChannel.speakerDevice
        onPicked: (value) => signalingChannel.setSpeakerDevice(value)
    }

    Label {
        width: parent.width
        topPadding: 4
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: Style.textFaint
        text: "修改后立即生效，无需重连。麦克风决定他人听到的声音；"
              + "听筒决定你听到的对方语音，以及共享模式下推来的电影声。"
    }
}
