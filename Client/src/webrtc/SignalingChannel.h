//
// WebRTC 信令 <-> 房间消息 之间的胶水层。
//
// 出站：WebRTC 事件 -> 带路由信息的信令消息
// 入站：信令消息 -> 对应 PeerLink 的状态机
//
// 多对端之后这一层多了两件它该管的事：
//   1. **该和谁建连** —— 谁发起、什么时候发起（syncPeers）
//   2. **路由** —— 每条信令归哪个对端（靠 peerId 认领）
//   3. **断线恢复** —— ICE Disconnected/Failed 时由发起方做 ICE restart，
//      只换网络路径，轨道、DataChannel、DTLS 都保留（见 onIceDisconnected）
//
// 为什么要定"谁发起":网状网里若两端同时 CreateOffer,会撞成 glare
// (双方都在 offer 状态，收到对方的 offer 谁也不知道该回滚谁)。
// 用 clientId 比较来单向决定，是无需额外协商就能打破对称的办法。
//

#ifndef SYNCINE_SIGNALINGCHANNEL_H
#define SYNCINE_SIGNALINGCHANNEL_H

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>

#include "core/Protocol.h"
#include "webrtc/PeerLink.h"

class NetworkManager;
class RoomSession;
class WebrtcManager;

class SignalingChannel : public QObject {
    Q_OBJECT

    // 麦克风开关状态。QML 直接读这个属性，不要自己再存一份。
    Q_PROPERTY(bool audioEnabled
               READ audioEnabled
               NOTIFY audioEnabledChanged)

    // 接收端两条音轨的音量（0.0 ~ 1.0）。QML 的两个滑块绑定这两个属性
    Q_PROPERTY(double movieVolume
               READ movieVolume
               WRITE setMovieVolume
               NOTIFY movieVolumeChanged)

    Q_PROPERTY(double chatVolume
               READ chatVolume
               WRITE setChatVolume
               NOTIFY chatVolumeChanged)

    // 推送画质档位（共享模式下由房主设置）。
    // 用 int 而不是枚举，是为了不让 QML 去认 WebrtcManager 的类型：
    // 0=流畅 1=标准 2=高清 3=原画，顺序和 WebrtcManager::VideoQuality 一致。
    // 界面要显示文字请用 videoQualityName()。
    Q_PROPERTY(int videoQuality
               READ videoQuality
               WRITE setVideoQuality
               NOTIFY videoQualityChanged)

    // ---- 音频设备（麦克风 / 听筒）----
    //
    // 和 audioEnabled、两个音量一样只是转发给 WebrtcManager,本类不存状态。
    // 界面不直接看 WebrtcManager —— 那是 WebRTC 那一层的实现细节。
    //
    // 设备的"值"一律是**设备名**(空串 = 跟随系统默认),不是索引：
    // 索引会随设备插拔漂移，存下来就会选错设备（原因见 WebrtcManager.h）。
    Q_PROPERTY(QVariantList micDevices
               READ micDevices
               NOTIFY micDevicesChanged)

    Q_PROPERTY(QVariantList speakerDevices
               READ speakerDevices
               NOTIFY speakerDevicesChanged)

    Q_PROPERTY(QString micDevice
               READ micDevice
               NOTIFY micDeviceChanged)

    Q_PROPERTY(QString speakerDevice
               READ speakerDevice
               NOTIFY speakerDeviceChanged)

public:
    SignalingChannel(RoomSession *session,
                     NetworkManager *networkManager,
                     WebrtcManager *webrtcManager,
                     QObject *parent = nullptr);
    ~SignalingChannel() override = default;

    // 开关麦克风。QML 绑定这个。
    Q_INVOKABLE void setAudioEnabled(bool enabled);

    bool audioEnabled() const;

    // 接收端音量。和 setAudioEnabled 一样只是转发给 WebrtcManager,
    // 本类不自己存状态。
    double movieVolume() const;
    void setMovieVolume(double volume);

    double chatVolume() const;
    void setChatVolume(double volume);

    // 推送画质。越界值会被夹到合法范围。
    //
    // **必须带 Q_INVOKABLE**:它同时是 Q_PROPERTY 的 WRITE 访问器，但 QML 里
    // `signalingChannel.setVideoQuality(1)` 这种**方法调用**要求它是可调用的
    // 槽/可调用方法 —— 只当属性 setter 的话 QML 会直接抛
    // "Property 'setVideoQuality' ... is not a function",而且点了没反应、
    // 连对话框都不会关（异常中断了后面的语句）。
    // 属性赋值 `signalingChannel.videoQuality = 1` 不写 Q_INVOKABLE 也能用，
    // 但界面上两种写法都可能出现，故二者都支持。
    int videoQuality() const;
    Q_INVOKABLE void setVideoQuality(int quality);

    // 重新枚举音频设备（打开设置页 / 房间里的设置弹窗时调一次）。
    // 没进过房间也能用 —— 内部会先把 WebRTC 的声卡拉起来。
    Q_INVOKABLE void refreshAudioDevices();

    // 选设备。空串 = 跟随系统默认。选中**立刻生效**,落盘由 main.cpp 接线。
    Q_INVOKABLE void setMicDevice(const QString &name);
    Q_INVOKABLE void setSpeakerDevice(const QString &name);

    QVariantList micDevices() const;
    QVariantList speakerDevices() const;
    QString micDevice() const;
    QString speakerDevice() const;

signals:
    void errorOccurred(const QString &message);
    // 与某个对端的连接真正打通（ICE 完成）,供界面显示状态
    void peerConnected(const QString &peerId);
    void peerDisconnected(const QString &peerId);
    // 麦克风实际开关状态。QML 用它驱动按钮显示，不要自己记状态
    void audioEnabledChanged(bool enabled);

    void movieVolumeChanged(double volume);
    void chatVolumeChanged(double volume);
    void videoQualityChanged(int quality);

    void micDevicesChanged();
    void speakerDevicesChanged();
    void micDeviceChanged(const QString &name);
    void speakerDeviceChanged(const QString &name);

private slots:
    void onMessage(const QString &text);

    void onOfferCreated(const QString &peerId, const QString &link, const QString &sdp);
    void onAnswerCreated(const QString &peerId, const QString &link, const QString &sdp);
    void onIceCandidateCreated(const QString &peerId,
                               const QString &link,
                               const QString &sdp,
                               const QString &sdpMid,
                               int sdpMLineIndex);

    void onPeerConnected(const QString &peerId, const QString &link);
    void onPeerClosed(const QString &peerId, const QString &link);
    void onPeerError(const QString &peerId, const QString &link, const QString &message);

    void onIceDisconnected(const QString &peerId, const QString &link);
    void onIceFailed(const QString &peerId, const QString &link);

    void onRoomEntered();
    void onRoomLeft();
    void onMembersChanged();
    // 断线重连的成员恢复在线：重新对齐连接并重排断线期间失败的 ICE restart
    void onMemberOnline(const QString &clientId);
    // 本端 clientId 变化（首次进房/重连/换房）：旧 id 的 PeerConnection 必须作废
    void onClientIdChanged();

private:
    // 房间成员表变化后，对齐连接：该建的建、该删的删
    void syncPeers();
    void teardownPeer(const QString &peerId);
    // 保证 peerId 有 PeerLink。已存在返回 true;新建失败返回 false。
    bool ensurePeer(const QString &peerId);
    // 本端是否该主动向 peerId 发起协商（半开连接的处理见 .cpp）
    bool shouldInitiate(const QString &peerId) const;
    bool send(const QString &text);

    // ---- ICE 恢复 ----
    // Disconnected 后先等宽限期，Failed 立即尝试；最多重启若干次。
    // 只有 shouldInitiate() 一侧真正发 offer，避免双方同时 restart 撞成 glare。
    void scheduleIceRestart(const QString &peerId, const QString &link,
                            int delayMs, bool countAttempt);
    void clearIceRecovery(const QString &peerId, const QString &link);
    // 信令恢复后，把断线期间没能发出去的 ICE restart 重新排一次
    void rearmIceRecovery();

    RoomSession *m_session = nullptr;
    NetworkManager *m_networkManager = nullptr;
    WebrtcManager *m_webrtcManager = nullptr;

    // 已经发出过 offer 的**连接**(不是对端)。用来区分"等待连上"和"该重试"——
    // 没有它就无法判断一条半开的连接是死了还是还在握手中。
    //
    // 键是 "peerId|link":一个对端有两条连接，各自独立协商、各自可能半开，
    // 所以必须分开记，否则第二条连接会被当成"已经发过了"而永远不发起。
    QSet<QString> m_offeredLinks;

    // 每条 link 已经尝试过几次 ICE restart / 因为协商忙而推迟了几次。
    QHash<QString, int> m_iceRestartAttempts;
    QHash<QString, int> m_iceBusyRetries;
    // 宽限期计时器的代次：每次重新计时/连上都换一个值，旧计时器醒来时
    // 发现代次不匹配就直接退出，不需要真的销毁 QTimer。
    QHash<QString, quint64> m_iceGraceGeneration;
    quint64 m_iceGenerationCounter = 0;
    // 已经报告过 ICE Failed、还没恢复的 link（键是 "peerId|link"）。
    // 信令断线期间 restart offer 发不出去，等 resume 后用这个集合重排。
    QSet<QString> m_iceFailedLinks;

    static QString offerKey(const QString &peerId, const QString &link);
};

#endif //SYNCINE_SIGNALINGCHANNEL_H
