//
// 应用的全局设置：用户偏好的唯一读写入口。
//
// 落盘位置 `~/.config/Syncine/syncine.ini`（AppConfigLocation），使用 QSettings 的
// IniFormat 而非平台原生格式，便于直接打开查看。
//
// 约定：
//   · 默认值均在此给出，ini 中缺失的键使用默认值，故首次运行不写入内容亦可工作。
//   · setter 都带 Q_INVOKABLE：Q_PROPERTY 的 WRITE 访问器不是 QML 可调用的方法，
//     QML 中写 `appSettings.setLogLevel(x)` 会抛 "is not a function" 并中断其后语句
//     （见 syncine-qt-pitfalls）；属性赋值 `appSettings.logLevel = x` 同样可用。
//   · 兜底服务器地址是编译期常量（kFallbackServerUrl），用户只能覆盖、不能修改，
//     部署时修改该常量。
//

#ifndef SYNCINE_APPSETTINGS_H
#define SYNCINE_APPSETTINGS_H

#include <QObject>
#include <QSettings>
#include <QString>

#include <memory>

class AppSettings : public QObject {
    Q_OBJECT

    // 用户自行填写的服务器地址。空 = 使用兜底地址（见 fallbackServerUrl）。
    Q_PROPERTY(QString serverUrl
               READ serverUrl
               WRITE setServerUrl
               NOTIFY serverUrlChanged)

    // ---- cookie 的元信息 ----
    // cookie 文件本身由 CookieManager 管理（固定路径），此处只存其相关元信息：
    // 上次从哪个浏览器导入、来源如何显示、导入时间。
    //
    // 放在此处而非由 CookieManager 自建 QSettings 的原因：
    // 同一个 ini 不能有两个 QSettings 实例——各自持有缓存会互相覆盖。
    Q_PROPERTY(QString lastCookieBrowser
               READ lastCookieBrowser
               WRITE setLastCookieBrowser
               NOTIFY lastCookieBrowserChanged)

    Q_PROPERTY(QString cookieSource
               READ cookieSource
               WRITE setCookieSource
               NOTIFY cookieSourceChanged)

    // ISO 8601 字符串（空 = 从未导入过）
    Q_PROPERTY(QString cookieImportedAt
               READ cookieImportedAt
               WRITE setCookieImportedAt
               NOTIFY cookieImportedAtChanged)

    // 网链模式添加条目时默认选中的画质档（spec 的「主体」，见 FormatModel.h）：
    // "0" = 最高画质，"1080" = 不超过 1080P……
    // 注意其不带 HDR 后缀——那由下面的 hdrAllowed 决定，提交时经
    // UrlInspector::buildSpec 缀上。
    Q_PROPERTY(QString defaultQuality
               READ defaultQuality
               WRITE setDefaultQuality
               NOTIFY defaultQualityChanged)

    // 添加条目时是否允许 HDR/杜比视界。默认关闭。
    // 关闭时下载端强制只选 SDR，并在下载后用 ffprobe 复核；即使条目 spec
    // 允许 HDR，本地关闭也会收紧。不做离线转码。
    Q_PROPERTY(bool hdrAllowed
               READ hdrAllowed
               WRITE setHdrAllowed
               NOTIFY hdrAllowedChanged)

    // 通用链接是否也带 cookie。默认关闭：避免把 B 站登录态发给任意站点。
    Q_PROPERTY(bool genericCookies
               READ genericCookies
               WRITE setGenericCookies
               NOTIFY genericCookiesChanged)

    // ---- 弹幕显示 ----
    // 显示区域：弹幕只占播放器顶部的这个百分比，档位 10/25/50/75/100。
    Q_PROPERTY(int danmakuArea
               READ danmakuArea
               WRITE setDanmakuArea
               NOTIFY danmakuAreaChanged)

    // 不透明度：10~100（百分比）
    Q_PROPERTY(int danmakuOpacity
               READ danmakuOpacity
               WRITE setDanmakuOpacity
               NOTIFY danmakuOpacityChanged)

    // 字号：50~200（百分比）
    Q_PROPERTY(int danmakuFontSize
               READ danmakuFontSize
               WRITE setDanmakuFontSize
               NOTIFY danmakuFontSizeChanged)

    // 速度档位 0~4，对应 0.5x / 0.75x / 1.0x / 1.25x / 1.5x
    Q_PROPERTY(int danmakuSpeed
               READ danmakuSpeed
               WRITE setDanmakuSpeed
               NOTIFY danmakuSpeedChanged)

    // 字号是否跟随播放器高度缩放（关闭后使用固定的 720p 基准字号）
    Q_PROPERTY(bool danmakuScaleWithScreen
               READ danmakuScaleWithScreen
               WRITE setDanmakuScaleWithScreen
               NOTIFY danmakuScaleWithScreenChanged)

    // 音频设备：麦克风（采集）与听筒（播放）。
    //
    // 存的是设备名（ADM 给出的显示名），空串 = 跟随系统默认。
    // 不存索引的原因：声卡设备索引会随插拔漂移，今日为 3 号、明日可能变为 2 号，
    // 存索引会导致静默选错设备。（Linux 上 WebRTC 的 ADM 不提供 GUID——
    // guid 恒为空，因此只能用名字。）
    //
    // 真正将设备落到声卡的是 WebrtcManager：启动时可能尚不可行（WebRTC 的声卡
    // 在进房间时才初始化），此时先记下意图，进房后再套用。
    Q_PROPERTY(QString micDevice
               READ micDevice
               WRITE setMicDevice
               NOTIFY micDeviceChanged)

    Q_PROPERTY(QString speakerDevice
               READ speakerDevice
               WRITE setSpeakerDevice
               NOTIFY speakerDeviceChanged)

    // 日志级别：trace / debug / info / warn / error。
    // 环境变量 SYNCINE_LOG_LEVEL 优先级更高（供临时调试，见 applyLogLevel）
    Q_PROPERTY(QString logLevel
               READ logLevel
               WRITE setLogLevel
               NOTIFY logLevelChanged)

    // 内置兜底服务器地址（只读，供界面显示「留空则使用该地址」）
    Q_PROPERTY(QString fallbackServerUrl
               READ fallbackServerUrl
               CONSTANT)

public:
    explicit AppSettings(QObject *parent = nullptr);

    QString serverUrl() const { return m_serverUrl; }
    Q_INVOKABLE void setServerUrl(const QString &serverUrl);

    QString lastCookieBrowser() const { return m_lastCookieBrowser; }
    Q_INVOKABLE void setLastCookieBrowser(const QString &key);

    QString cookieSource() const { return m_cookieSource; }
    Q_INVOKABLE void setCookieSource(const QString &source);

    QString cookieImportedAt() const { return m_cookieImportedAt; }
    Q_INVOKABLE void setCookieImportedAt(const QString &isoTime);

    QString defaultQuality() const { return m_defaultQuality; }
    Q_INVOKABLE void setDefaultQuality(const QString &quality);

    bool hdrAllowed() const { return m_hdrAllowed; }
    Q_INVOKABLE void setHdrAllowed(bool allowed);

    bool genericCookies() const { return m_genericCookies; }
    Q_INVOKABLE void setGenericCookies(bool enabled);

    int danmakuArea() const { return m_danmakuArea; }
    Q_INVOKABLE void setDanmakuArea(int area);

    int danmakuOpacity() const { return m_danmakuOpacity; }
    Q_INVOKABLE void setDanmakuOpacity(int opacity);

    int danmakuFontSize() const { return m_danmakuFontSize; }
    Q_INVOKABLE void setDanmakuFontSize(int percent);

    int danmakuSpeed() const { return m_danmakuSpeed; }
    Q_INVOKABLE void setDanmakuSpeed(int speed);

    bool danmakuScaleWithScreen() const { return m_danmakuScaleWithScreen; }
    Q_INVOKABLE void setDanmakuScaleWithScreen(bool enabled);

    QString micDevice() const { return m_micDevice; }
    Q_INVOKABLE void setMicDevice(const QString &deviceName);

    QString speakerDevice() const { return m_speakerDevice; }
    Q_INVOKABLE void setSpeakerDevice(const QString &deviceName);

    QString logLevel() const { return m_logLevel; }
    Q_INVOKABLE void setLogLevel(const QString &level);

    QString fallbackServerUrl() const;

    // 把 ini 中的级别应用到日志系统。main() 中调用一次；
    // 界面修改级别后也需调用（立即生效，无需重启）。
    // 环境变量存在时不操作——那用于临时调试，不应被 ini 覆盖。
    void applyLogLevel();

    // ini 文件的完整路径（供设置页显示，也便于排查）。
    // 必须带 Q_INVOKABLE：QML 中 `appSettings.filePath()` 是方法调用而非读取属性，
    // 若仅为普通 getter 会抛 "Property 'filePath' ... is not a function"，
    // 并中断其所在的整条绑定。
    Q_INVOKABLE QString filePath() const;

    // ---- 以下两项严格来说不属于用户偏好，而是应用级的路径与动作 ----
    // 放在此处是因为设置页将它们与上述项一并显示，为「打开文件夹」单独建类
    // 并不划算。若日后增至三四个再拆分。
    Q_INVOKABLE QString logDirectory() const;
    Q_INVOKABLE void openLogDirectory();

signals:
    void serverUrlChanged();
    void lastCookieBrowserChanged();
    void cookieSourceChanged();
    void cookieImportedAtChanged();
    void defaultQualityChanged();
    void hdrAllowedChanged();
    void genericCookiesChanged();
    void danmakuAreaChanged();
    void danmakuOpacityChanged();
    void danmakuFontSizeChanged();
    void danmakuSpeedChanged();
    void danmakuScaleWithScreenChanged();
    void micDeviceChanged();
    void speakerDeviceChanged();
    void logLevelChanged();

private:
    std::unique_ptr<QSettings> m_settings;

    QString m_serverUrl;
    QString m_lastCookieBrowser;
    QString m_cookieSource;
    QString m_cookieImportedAt;
    // 空 = 最高画质；数字串 = 高度上限（见文件头说明）
    QString m_defaultQuality;
    bool m_hdrAllowed = false;
    bool m_genericCookies = false;
    int m_danmakuArea = 100;
    int m_danmakuOpacity = 100;
    int m_danmakuFontSize = 100;
    int m_danmakuSpeed = 2; // 1.0x
    bool m_danmakuScaleWithScreen = true;
    // 空串 = 跟随系统默认（见上述两个属性的说明）
    QString m_micDevice;
    QString m_speakerDevice;
    QString m_logLevel = QStringLiteral("info");
};

#endif //SYNCINE_APPSETTINGS_H
