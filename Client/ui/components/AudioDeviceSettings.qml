import QtQuick
import QtQuick.Controls
import SyncineApp

// 「麦克风 / 听筒」两个设备选择行。
//
// 设置页的音频卡片和房间里的设置弹窗**共用这一份** —— 两处要显示的东西完全
// 一样,分成两份迟早会不一致(而且房间那边本来就该是"同一份设置的快捷入口")。
//
// 用法(宿主负责在合适的时机刷新一次设备列表,这里不自己刷):
//     AudioDeviceSettings { width: parent.width }
//     // 设置页:onSectionChanged 里 if (section === "audio") signalingChannel.refreshAudioDevices()
//     // 房间里:弹窗 onOpened 里 signalingChannel.refreshAudioDevices()
//
// 为什么不在这里 Component.onCompleted 自己刷:本组件多半被塞在 Card /
// Flickable 里,父项被隐藏时子项的 visible 并不会跟着变 —— 想靠 visible 判断
// "这一页是不是被切到了"是不可靠的,交给宿主最干脆(和 CookieManager 那边的
// refreshBrowsers 一个写法)。
Column {
    id: root

    width: parent ? parent.width : 0
    spacing: 10

    // 枚举不到设备(声音服务没起来)时给一句人话,而不是两个空列表
    readonly property bool noDevice:
        signalingChannel.micDevices.length === 0
        && signalingChannel.speakerDevices.length === 0

    Label {
        width: parent.width
        visible: root.noDevice
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: Style.danger
        text: "没找到可用的音频设备。检查系统的声音服务(PipeWire / PulseAudio)是否在运行,"
              + "然后回到这一页重新打开一次。"
    }

    Label {
        text: "麦克风"
        font.pixelSize: 13
        color: Style.textPrimary
    }

    OptionList {
        // value 是**设备名**,空串 = 跟随系统默认。
        // 用名字而不是索引:索引会随设备插拔漂移(见 WebrtcManager.h)
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
        text: "改完立刻生效,不用重连。麦克风决定别人听到的你;"
              + "听筒决定你听到的对方 —— 包括他的语音,和共享模式下推过来的电影声。"
    }
}
