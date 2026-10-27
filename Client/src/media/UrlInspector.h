//
// 添加条目弹窗的探测器：执行一次 yt-dlp 的 `-J`，将结果整理为弹窗所需的显示形式。
//
// 与 UrlDownloader 的分工：本类只探测（不挑直链、不下载、不试拉），
// UrlDownloader 负责播放路径的解析与下载。两者是独立实例（见 main.cpp），
// 弹窗打开时播放路径可同时工作，互不 cancel。
//
// 弹窗的两个视图均由同一份 MediaInfo 生成：
//   简单视图 —— qualities（按分辨率/清晰度归纳的档位）
//   详情视图 —— videoFormats / audioFormats / combinedFormats（每条流一行）
// 用户在详情中手选一条（selectFormat）后，buildSpec() 产出的即为精确 id 的 spec。
//

#ifndef SYNCINE_URLINSPECTOR_H
#define SYNCINE_URLINSPECTOR_H

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantList>

#include "media/FormatModel.h"

class QProcess;
class CookieManager;
class Toolchain;

class UrlInspector : public QObject {
    Q_OBJECT

    // idle / probing / inspected / failed —— 界面按它切显示
    Q_PROPERTY(QString stage
               READ stage
               NOTIFY stateChanged)

    Q_PROPERTY(QString title
               READ title
               NOTIFY inspected)

    Q_PROPERTY(QString uploader
               READ uploader
               NOTIFY inspected)

    Q_PROPERTY(qint64 durationMs
               READ durationMs
               NOTIFY inspected)

    Q_PROPERTY(bool live
               READ live
               NOTIFY inspected)

    // 简单视图的档位，每项 { label, detail, spec, height, hdr }
    // (spec 是**档位**的 spec:普通视频是高度数字，直播是某条流的 format_id)
    Q_PROPERTY(QVariantList qualities
               READ qualities
               NOTIFY inspected)

    // ---- 剧集/合集（yt-dlp 的多集链接）----
    // 非空 = 这个链接是"一季"(顶层没有 formats,每一集自带)。
    // 每项 { id(平台的集编号), title(去重过前缀的显示名), itemId(房间条目 id),
    //        url(这一集的地址), durationMs, index(第几集) }
    // 界面的流程：列出来 → 点某一集 → pickEpisode() 重新解析那一集 → 挑画质
    Q_PROPERTY(QVariantList episodes
               READ episodes
               NOTIFY inspected)
    // 整季的名字（"学生会的一己之见"）,给它显示标题用
    Q_PROPERTY(QString playlistTitle
               READ playlistTitle
               NOTIFY inspected)

    // 当前展示的是**整季列表**还是某一集（m_p_info 那一层）。
    // 需要该标志的原因：进入某一集后 episodes 会被清空并替换为该集的 formats，
    // 界面不能以 episodes 是否为空判断所在层级——否则「返回剧集」的入口
    // 会在进入单集后消失（实测）。界面的整季快照与此标志配合使用。
    Q_PROPERTY(bool playlistView
               READ playlistView
               NOTIFY stateChanged)

    // 正在解析用户点的那一集（界面显示"加载中…"用 —— 此时 stage 会回到 probing）
    Q_PROPERTY(bool pickingEpisode
               READ pickingEpisode
               NOTIFY stateChanged)

    // 详情视图：每项 { id, title, detail, hdr }
    Q_PROPERTY(QVariantList videoFormats
               READ videoFormats
               NOTIFY inspected)
    Q_PROPERTY(QVariantList audioFormats
               READ audioFormats
               NOTIFY inspected)
    Q_PROPERTY(QVariantList combinedFormats
               READ combinedFormats
               NOTIFY inspected)

    // 详情视图里手选中的那条（空 = 没手选）。视频和音频各记一份 ——
    // 选"合一格式"时会同时占掉两个
    Q_PROPERTY(QString pickedVideoId
               READ pickedVideoId
               NOTIFY pickedChanged)
    Q_PROPERTY(QString pickedAudioId
               READ pickedAudioId
               NOTIFY pickedChanged)

    Q_PROPERTY(QString errorText
               READ errorText
               NOTIFY failed)

    // 当前解析/展示的是哪个地址。界面靠它认"这一条就是刚解析出来的那一条"
    // (比如给某一集记下的画质，要知道现在这趟解析属于哪一集)
    Q_PROPERTY(QString probingUrl
               READ probingUrl
               NOTIFY stateChanged)

    // 当前选中的档位（界面用它高亮）。"最高画质"档是 "0",没选过是空。
    //
    // **必须是属性而不是 Q_INVOKABLE 方法**:QML 里 `onPicked: …selectTier(x)`
    // 之后，`selectedValue: urlInspector.tierSpec()` 这种**方法调用不会**因为
    // 信号被重新求值——表现为「点击选项但高亮不变」。属性带 NOTIFY
    // 才会触发绑定重算（实测）。
    Q_PROPERTY(QString tierSpec
               READ tierSpec
               NOTIFY pickedChanged)

    // 当前选择合出来的 spec,空 = 还没得可选（弹窗靠它决定"加入"按钮能不能点）。
    // 和 buildSpec() 的区别：这个**不带 HDR 后缀** —— 后缀由提交时按设置缀上，
    // 这里只回答"选没选够"。同样是属性，理由同上。
    Q_PROPERTY(QString selectionSpec
               READ selectionSpec
               NOTIFY pickedChanged)

public:
    explicit UrlInspector(const Toolchain *toolchain,
                          const CookieManager *cookies = nullptr,
                          QObject *parent = nullptr);
    ~UrlInspector() override;

    // 看一眼这个链接（拿标题/时长/档位/详情列表）。重复调用会作废上一次。
    // 多集链接（剧集/合集）只列表，不挑流 —— 用户得先点定某一集（pickEpisode）
    Q_INVOKABLE void inspect(const QString &url);

    // 从剧集列表里点某一集：拿那一集的地址重新解析(之后 qualities/详情
    // 就是这一集的)。index 是 episodes 里的下标
    Q_INVOKABLE void pickEpisode(int index);

    // 从"某一集"返回整季列表。**不重新请求** —— 整季的解析结果一直留着
    // (m_playlistJson),这一步只是把它恢复回来，瞬间完成。
    // 地址由界面负责换回整季的地址（它那边有清单）
    //
    // **Q_INVOKABLE 别漏**:漏了 C++ 里编译运行都正常，只有 QML 一调就报
    // "Property 'backToPlaylist' ... is not a function"(这个坑在本类里踩第二次了)
    Q_INVOKABLE void backToPlaylist();

    // 直接灌一份 yt-dlp 的 `-J` 输出（不起进程）。
    // 给离线的逻辑验证用 —— 选择状态机(哪些属性会变、selectionSpec 什么时候
    // 非空)是弹窗能不能提交的关键，值得不启动界面就能验。
    void applyJson(const QJsonObject &root);

    // 详情视图点一行：role 0=视频 1=音频 2=合一（合一同时占掉视频和音频两项）。
    // 再点同一条 = 取消选择
    Q_INVOKABLE void selectFormat(const QString &formatId, int role);
    Q_INVOKABLE void clearPicked();

    // 简单视图点某一档。spec 来自 qualities 里那一项的 spec 字段
    // (普通视频是高度数字，直播是流 id —— 是不是直播由 C++ 自己判断)
    Q_INVOKABLE void selectTier(const QString &spec);

    // 关弹窗/换链接时清状态（在飞的任务会取消）。只发 stateChanged
    Q_INVOKABLE void cancel();

    // 把当前选择拼成条目上要记的 spec:
    //   allowHdr = 设置里的"允许 HDR"开关。档位直接据此缀/不缀 ":h"；
    //   手选流只在它本身是 HDR 且开关允许时才缀（避免 SDR 手选被标成 +HDR）。
    //   直播的 HDR 由清单决定，不缀后缀。
    // 返回空串 = 用户没选过任何东西（弹窗不该提交，靠 canSubmit 挡住）
    Q_INVOKABLE QString buildSpec(bool allowHdr) const;

    // 当前选中的档位（界面用它高亮）。"最高画质"档返回 "0",没选过返回空
    QString tierSpec() const
    {
        return m_tierSpec.isEmpty() && m_tierChosen ? QStringLiteral("0") : m_tierSpec;
    }
    // 当前选择合出来的 spec(不带 HDR 后缀),空 = 还不够提交
    QString selectionSpec() const;

    QString stage() const { return m_stage; }
    QString probingUrl() const { return m_url; }
    QVariantList episodes() const { return m_episodes; }
    QString playlistTitle() const { return m_playlistTitle; }
    bool pickingEpisode() const { return m_pickingEpisode; }
    // 现在展示的是整季列表吗（进了某一集之后就是 false,哪怕 episodes 还没清）
    bool playlistView() const { return m_playlistView; }
    QString title() const { return m_info.title; }
    QString uploader() const { return m_info.uploader; }
    qint64 durationMs() const { return m_info.durationMs; }
    bool live() const { return m_info.live; }
    QVariantList qualities() const { return m_qualities; }
    QVariantList videoFormats() const { return m_videoFormats; }
    QVariantList audioFormats() const { return m_audioFormats; }
    QVariantList combinedFormats() const { return m_combinedFormats; }
    QString pickedVideoId() const { return m_pickedVideoId; }
    QString pickedAudioId() const { return m_pickedAudioId; }
    QString errorText() const { return m_error; }

signals:
    void stateChanged();
    // 解析成功。档位/详情列表都在这一刻才有效
    void inspected();
    void pickedChanged();
    void failed(const QString &message);

private:
    void handleFinished(int exitCode);
    void fail(const QString &message);
    // 清掉上一次的结果（标题、列表、手选）
    void resetResults();

    const Toolchain *m_toolchain = nullptr;
    const CookieManager *m_cookies = nullptr;
    QProcess *m_process = nullptr;

    QString m_stage = QStringLiteral("idle");
    QString m_error;
    QString m_url;
    MediaInfo m_info;

    QVariantList m_qualities;
    QVariantList m_videoFormats;
    QVariantList m_audioFormats;
    QVariantList m_combinedFormats;

    // 剧集列表和整季的名字（非空 = 刚才看的是多集链接）
    QVariantList m_episodes;
    QString m_playlistTitle;
    // 现在展示的是整季列表（见 playlistView 的说明）
    bool m_playlistView = false;
    // 用户点了某一集、正在重新解析它
    bool m_pickingEpisode = false;
    // 整季那份完整结果（点某一集时先存下来，backToPlaylist 直接恢复 —— 不重跑进程）
    QJsonObject m_playlistJson;

    // 详情视图的手选（存的是 format_id;键找不到时 buildSpec 会退回档位 spec）
    QString m_pickedVideoId;
    QString m_pickedAudioId;
    // 简单视图选中的档位 spec(空串 + m_tierChosen = "最高画质"档)
    QString m_tierSpec;
    bool m_tierChosen = false;
    // 直播的档位 spec 就是某条流的 format_id,标记一下免得 buildSpec 找错表
    bool m_tierIsLiveId = false;

    QByteArray m_outputBuffer;
    QStringList m_outputTail;
};

#endif //SYNCINE_URLINSPECTOR_H
