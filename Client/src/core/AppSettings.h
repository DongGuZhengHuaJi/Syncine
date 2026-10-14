//
// 应用的全局设置 —— 唯一一处"用户偏好"的读写入口。
//
// 为什么单独成一层:
//   之前这些东西是散的 —— 服务器地址写死在 NetworkManager 里、日志级别只能靠
//   环境变量、画质每次都手选。散着放的后果是"想让用户改一项,得先找到它藏在
//   哪一层"。现在全部收在这里:落盘一份 ini,界面只认这个对象。
//
// 落盘位置:`~/.config/Syncine/syncine.ini`(QStandardPaths::AppConfigLocation)。
// 用 QSettings 的 IniFormat 而不是平台原生的注册表/plist —— 出问题时用户能
// 直接打开这个文件看,我们排查也方便。
//
// 几条约定:
//   · **默认值都在这里给**(空串/0),ini 里没有的键就用默认值,所以第一次
//     运行不写任何东西也能跑。
//   · **setter 都带 Q_INVOKABLE**。Q_PROPERTY 的 WRITE 访问器不是 QML 可调用
//     的方法,QML 里写 `appSettings.setLogLevel(x)` 会抛 "is not a function"
//     并且**中断它后面的语句**(踩过,见 syncine-qt-pitfalls)。
//     QML 里 `appSettings.logLevel = x` 这种属性赋值也照样能用。
//   · **兜底服务器地址不是设置项**,是编译期常量(kFallbackServerUrl)——
//     用户只能"覆盖"它,改不了它。部署的时候改那一行。
//

#ifndef SYNCINE_APPSETTINGS_H
#define SYNCINE_APPSETTINGS_H

#include <QObject>
#include <QSettings>
#include <QString>

#include <memory>

class AppSettings : public QObject {
    Q_OBJECT

    // 用户自己填的服务器地址。**空 = 用兜底地址**(见 fallbackServerUrl)。
    Q_PROPERTY(QString serverUrl
               READ serverUrl
               WRITE setServerUrl
               NOTIFY serverUrlChanged)

    // ---- cookie 的元信息 ----
    // cookie 文件本身由 CookieManager 管理(固定路径),这里只存"关于它的事":
    //   上次从哪个浏览器导的、来源怎么显示、什么时候导的
    //
    // 为什么放这儿而不是让 CookieManager 自己开一个 QSettings:
    // **同一个 ini 不能有两个 QSettings 实例** —— 各自有缓存,会互相覆盖。
    Q_PROPERTY(QString lastCookieBrowser
               READ lastCookieBrowser
               WRITE setLastCookieBrowser
               NOTIFY lastCookieBrowserChanged)

    Q_PROPERTY(QString cookieSource
               READ cookieSource
               WRITE setCookieSource
               NOTIFY cookieSourceChanged)

    // ISO 8601 字符串(空 = 从没导过)
    Q_PROPERTY(QString cookieImportedAt
               READ cookieImportedAt
               WRITE setCookieImportedAt
               NOTIFY cookieImportedAtChanged)

    // 网链模式加条目时默认选中的画质档(spec 的"主体",见 FormatModel.h):
    // "0" = 最高画质,"1080" = 不超过 1080P……
    // 注意它**不带** HDR 后缀 —— 那是下面 hdrAllowed 的事,提交时由
    // UrlInspector::buildSpec 缀上
    Q_PROPERTY(QString defaultQuality
               READ defaultQuality
               WRITE setDefaultQuality
               NOTIFY defaultQualityChanged)

    // 简单模式选档时**允不允许下 HDR/杜比视界**。
    // 默认关:HDR 在 Qt 播放器上要过一遍离线转 SDR(见 HdrConverter),
    Q_PROPERTY(bool hdrAllowed
               READ hdrAllowed
               WRITE setHdrAllowed
               NOTIFY hdrAllowedChanged)

    // 音频设备:麦克风(采集)和听筒(播放)。
    //
    // 存的是**设备名**(ADM 给的显示名),空串 = 跟随系统默认。
    // 为什么不存索引:声卡设备的索引会随插拔漂移,今天选的是 3 号、
    // 明天可能就变成 2 号了,存索引会静默选错设备。
    // (Linux 上 WebRTC 的 ADM 不提供 GUID —— guid 恒为空,所以只能用名字。)
    //
    // 真正把设备落到声卡上的是 WebrtcManager:它可能在启动时还做不到
    // (WebRTC 的声卡是进房间才初始化的),这时只记下意图,进房后再套用。
    Q_PROPERTY(QString micDevice
               READ micDevice
               WRITE setMicDevice
               NOTIFY micDeviceChanged)

    Q_PROPERTY(QString speakerDevice
               READ speakerDevice
               WRITE setSpeakerDevice
               NOTIFY speakerDeviceChanged)

    // 日志级别:trace / debug / info / warn / error。
    // 注意环境变量 SYNCINE_LOG_LEVEL 优先级更高(临时调试用,见 applyLogLevel)
    Q_PROPERTY(QString logLevel
               READ logLevel
               WRITE setLogLevel
               NOTIFY logLevelChanged)

    // 内置的兜底服务器地址(只读,给界面显示"留空则用 xxx"用)
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

    QString micDevice() const { return m_micDevice; }
    Q_INVOKABLE void setMicDevice(const QString &deviceName);

    QString speakerDevice() const { return m_speakerDevice; }
    Q_INVOKABLE void setSpeakerDevice(const QString &deviceName);

    QString logLevel() const { return m_logLevel; }
    Q_INVOKABLE void setLogLevel(const QString &level);

    QString fallbackServerUrl() const;

    // 把 ini 里的级别应用到日志系统。main() 里调一次;
    // 界面改了级别之后也要调(改完立刻生效,不用重启)。
    // **环境变量存在时不动** —— 那是临时调试用的,不该被 ini 挡住。
    void applyLogLevel();

    // ini 文件的完整路径(设置页显示用,也方便排查)。
    // **必须带 Q_INVOKABLE**:QML 里 `appSettings.filePath()` 是"调方法",
    // 不是读属性 —— 只当普通 getter 的话会抛
    // "Property 'filePath' ... is not a function",而且**中断它所在的那条绑定**。
    Q_INVOKABLE QString filePath() const;

    // ---- 下面两个严格说不是"用户偏好",是应用级的路径/动作 ----
    // 放这儿是因为设置页本来就把它们和上面那些放一起显示,而为一个
    // "打开文件夹" 新建一个类不值当。哪天真多到三四个再拆出去。
    Q_INVOKABLE QString logDirectory() const;
    Q_INVOKABLE void openLogDirectory();

signals:
    void serverUrlChanged();
    void lastCookieBrowserChanged();
    void cookieSourceChanged();
    void cookieImportedAtChanged();
    void defaultQualityChanged();
    void hdrAllowedChanged();
    void micDeviceChanged();
    void speakerDeviceChanged();
    void logLevelChanged();

private:
    std::unique_ptr<QSettings> m_settings;

    QString m_serverUrl;
    QString m_lastCookieBrowser;
    QString m_cookieSource;
    QString m_cookieImportedAt;
    // 空 = 最高画质;数字串 = 高度上限(见头部的说明)
    QString m_defaultQuality;
    bool m_hdrAllowed = false;
    // 空串 = 跟随系统默认(见上面两个属性的说明)
    QString m_micDevice;
    QString m_speakerDevice;
    QString m_logLevel = QStringLiteral("info");
};

#endif //SYNCINE_APPSETTINGS_H
