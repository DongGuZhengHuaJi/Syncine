//
// Cookie：为 yt-dlp 提供登录态（网链模式下访问需登录的内容，如 B 站会员视频）。
//
// 三条写入路径均收敛到同一文件 `~/.config/Syncine/cookies.txt`：
// importFromBrowser(key) 导出指定浏览器的 cookie；importFromFile(url) 由用户选择
// cookies.txt；clear() 删除。
//
// 采用「临时文件 + 预创建 0600 + 改名」的原因（均为实测结论）：
// yt-dlp 写文件使用 0666 & ~umask（通常 0644），直写正式文件会留下一段其他用户可读
// 的窗口；目录不存在时它会在 Python 中崩溃且不写入任何字节，故须先 mkpath；
// 写失败时它以 traceback 崩溃，直写正式文件会毁掉上一份可用的 cookie；
// 改名是原子操作，并发的下载只会读到旧文件或新文件，不会读到半截。
//
// 两个并非缺陷的现象，不应修改：yt-dlp 每次下载都会回写该文件（退出时存回内存中的
// cookie jar），故 mtime 持续变化，界面显示的「导入时间」是单独存储的；文件被删除后
// 可能在下载中被重建（权限 0644，watcher 会重设为 0600）。同一机器上多个客户端实例
// 共用该文件（last-writer-wins）。
//

#ifndef SYNCINE_COOKIEMANAGER_H
#define SYNCINE_COOKIEMANAGER_H

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <memory>

class AppSettings;
class Toolchain;

class CookieManager : public QObject {
    Q_OBJECT

    // 探测到的浏览器。每项 {label, value},value 是喂给 yt-dlp 的 key。
    // 形状和 OptionList 的 options 一致，可以直接喂它
    Q_PROPERTY(QVariantList browsers
               READ browsers
               NOTIFY browsersChanged)

    // 文件存在、格式对、至少有一条 cookie
    Q_PROPERTY(bool ready
               READ ready
               NOTIFY stateChanged)

    Q_PROPERTY(int cookieCount
               READ cookieCount
               NOTIFY stateChanged)

    // 显示用："Firefox" / "手动导入";空 = 没导过
    Q_PROPERTY(QString source
               READ source
               NOTIFY stateChanged)

    // ISO 8601(空 = 从没导过)。QML 里 `new Date(str)` 能直接解析
    Q_PROPERTY(QString importedAt
               READ importedAt
               NOTIFY stateChanged)

    // 导进来超过 30 天了 —— 该提醒用户重新导一次
    Q_PROPERTY(bool stale
               READ stale
               NOTIFY stateChanged)

    // 正在导入（按钮要禁用）
    Q_PROPERTY(bool busy
               READ busy
               NOTIFY stateChanged)

    // 失败原因，**已经翻译成人话**(空的 yt-dlp 原文用户看不懂，映射表在本文件里)
    Q_PROPERTY(QString errorText
               READ errorText
               NOTIFY stateChanged)

    // cookie 文件路径（设置页显示）
    Q_PROPERTY(QString path
               READ path
               NOTIFY stateChanged)

public:
    CookieManager(const Toolchain *toolchain, AppSettings *settings,
                  QObject *parent = nullptr);
    ~CookieManager() override;

    QVariantList browsers() const { return m_browsers; }
    bool ready() const { return m_ready; }
    int cookieCount() const { return m_cookieCount; }
    QString source() const;
    QString importedAt() const;
    bool stale() const;
    bool busy() const { return m_process != nullptr; }
    QString errorText() const { return m_errorText; }
    QString path() const;

    // 重新探测浏览器（装了新浏览器不用重启程序）
    Q_INVOKABLE void refreshBrowsers();

    Q_INVOKABLE void importFromBrowser(const QString &key);
    // FileDialog 给的 selectedFile 是 url,不是路径 —— 这里收 QUrl,进来 toLocalFile()
    Q_INVOKABLE void importFromFile(const QUrl &fileUrl);
    Q_INVOKABLE void clear();

    // ---- 给 UrlDownloader 用 ----
    // 就绪时 {"--cookies", 路径},否则空。和 Toolchain::ffmpegLocationArgs() 对称：
    // "什么算就绪"只有一处判断
    QStringList cookieArgs() const;

signals:
    void browsersChanged();
    void stateChanged();

private:
    void handleBrowserFinished(int exitCode);
    void fail(const QString &text);
    void refreshState();
    // 校验 .part 并把改名成正式文件
    bool finalizePartial(QString *error);
    // 把一段内容写进 .part(预创建 0600)
    bool writePartial(const QByteArray &content, QString *error);

    const Toolchain *m_toolchain = nullptr;
    AppSettings *m_settings = nullptr;

    QProcess *m_process = nullptr;      // 非空 = 正在导入
    QTimer m_watchdog;                  // keyring 弹窗/挂死时的出口
    QString m_importingKey;             // 导出中:哪个浏览器
    QString m_output;                   // 导出中:累计输出(结束时统一解析)
    bool m_timedOut = false;            // 超时已经报过错,结束回调别再覆盖一遍

    QFileSystemWatcher m_watcher;       // 盯 cookie 文件(下载会回写它)

    QVariantList m_browsers;
    bool m_ready = false;
    int m_cookieCount = 0;
    QString m_errorText;
};

#endif //SYNCINE_COOKIEMANAGER_H
