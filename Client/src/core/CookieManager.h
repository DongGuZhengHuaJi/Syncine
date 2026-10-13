//
// Cookie:给 yt-dlp 带上登录态。
//
// 为什么需要:网链模式下要登录才能看的内容(B 站会员视频、部分直播间),
// 不带 cookie 的话 yt-dlp 直接解析不出来。
//
// 三条写入路径(**都收敛到同一个文件** `~/.config/Syncine/cookies.txt`):
//   · importFromBrowser(key) —— 调 yt-dlp 把某个浏览器里的 cookie 导出来
//   · importFromFile(url)    —— 用户自己选一个 cookies.txt
//   · clear()                —— 删掉
//
// 为什么要"临时文件 + 预创建 0600 + 改名"这一套(每一步都是实测踩出来的):
//   · yt-dlp 写文件用的是 0666 & ~umask(通常 0644)—— 直写正式文件会留一段
//     "别的用户可读"的窗口;我们预先把 .part 建成 0600,它原地重写时不改权限
//   · 目录不存在时它是**崩在 Python 里**、一个字节都不写,所以必须先 mkpath
//   · 写失败时它是 traceback 崩溃 —— 直写正式文件会把**上一份能用的 cookie 毁掉**
//   · 改名是原子的:正在跑的下载要么读到旧文件、要么读到新文件,不会读到半截
//
// 两个"不是 bug"的现象,别去修:
//   · **每次下载 yt-dlp 都会回写这个文件**(它退出时会把内存里的 cookie jar
//     存回去,能顺手刷新站点轮换过的 cookie)→ 所以 mtime 一直在变,界面上显示的
//     "导入时间"是**单独存下来的**;文件被删掉后也可能被下载重新创建出来(那时是
//     0644,watcher 里会重贴 0600)
//   · 同一台机器开两个客户端实例时,两边共用这一个文件(last-writer-wins)
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
    // 形状和 OptionList 的 options 一致,可以直接喂它
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

    // 显示用:"Firefox" / "手动导入";空 = 没导过
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

    // 正在导入(按钮要禁用)
    Q_PROPERTY(bool busy
               READ busy
               NOTIFY stateChanged)

    // 失败原因,**已经翻译成人话**(空的 yt-dlp 原文用户看不懂,映射表在本文件里)
    Q_PROPERTY(QString errorText
               READ errorText
               NOTIFY stateChanged)

    // cookie 文件路径(设置页显示)
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

    // 重新探测浏览器(装了新浏览器不用重启程序)
    Q_INVOKABLE void refreshBrowsers();

    Q_INVOKABLE void importFromBrowser(const QString &key);
    // FileDialog 给的 selectedFile 是 url,不是路径 —— 这里收 QUrl,进来 toLocalFile()
    Q_INVOKABLE void importFromFile(const QUrl &fileUrl);
    Q_INVOKABLE void clear();

    // ---- 给 UrlDownloader 用 ----
    // 就绪时 {"--cookies", 路径},否则空。和 Toolchain::ffmpegLocationArgs() 对称:
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
