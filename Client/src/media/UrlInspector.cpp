#include "UrlInspector.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QVariantMap>

#include "core/CookieManager.h"
#include "core/Log.h"
#include "core/Toolchain.h"
#include "media/FormatModel.h"

namespace {

constexpr int kTailLines = 15;

QString errorFromOutput(const QStringList &output, const QString &fallback)
{
    for (int i = output.size() - 1; i >= 0; --i) {
        const QString line = output.at(i).trimmed();
        if (line.startsWith(QLatin1String("ERROR:")))
            return line;
    }
    return output.isEmpty() ? fallback : output.join(QLatin1Char('\n'));
}

QVariantMap toMap(const QualityOption &option)
{
    QVariantMap map;
    map[QStringLiteral("label")] = option.label;
    map[QStringLiteral("detail")] = option.detail;
    map[QStringLiteral("spec")] = option.spec;
    map[QStringLiteral("height")] = option.height;
    map[QStringLiteral("hdr")] = option.hasHdr;
    return map;
}

QVariantMap toMap(const FormatChoice &choice)
{
    QVariantMap map;
    map[QStringLiteral("id")] = choice.formatId;
    map[QStringLiteral("title")] = choice.title;
    map[QStringLiteral("detail")] = choice.detail;
    map[QStringLiteral("hdr")] = choice.hdr;
    return map;
}

} // namespace

UrlInspector::UrlInspector(const Toolchain *toolchain,
                           const CookieManager *cookies,
                           QObject *parent)
    : QObject(parent),
      m_toolchain(toolchain),
      m_cookies(cookies) {
}

UrlInspector::~UrlInspector()
{
    if (m_process != nullptr) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

void UrlInspector::inspect(const QString &url)
{
    if (m_toolchain == nullptr || !m_toolchain->ytDlpReady()) {
        fail(QStringLiteral("找不到 yt-dlp,无法解析链接"));
        return;
    }

    if (m_process != nullptr) {
        disconnect(m_process, nullptr, this, nullptr);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }

    resetResults();
    m_error.clear();
    m_url = url;
    m_stage = QStringLiteral("probing");
    emit stateChanged();
    emit pickedChanged();

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setProgram(m_toolchain->ytDlpPath());

    QStringList args{QStringLiteral("-J"), QStringLiteral("--no-playlist")};
    args << m_toolchain->ffmpegLocationArgs();
    if (m_cookies != nullptr)
        args << m_cookies->cookieArgs();
    args << url;
    m_process->setArguments(args);
    LOG_DEBUG("UrlInspector") << "yt-dlp" << args.join(QLatin1Char(' '));

    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        const QByteArray raw = m_process->readAllStandardOutput();
        m_outputBuffer.append(raw);
        for (const QString &line : QString::fromUtf8(raw).split(QLatin1Char('\n'),
                                                                Qt::SkipEmptyParts)) {
            m_outputTail.append(line);
            while (m_outputTail.size() > kTailLines)
                m_outputTail.removeFirst();
        }
    });

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(QStringLiteral("yt-dlp 启动失败:%1").arg(m_process->errorString()));
    });

    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
                QProcess *finished = m_process;
                m_process = nullptr;
                if (finished != nullptr)
                    finished->deleteLater();

                if (status != QProcess::NormalExit) {
                    fail(QStringLiteral("解析被中断"));
                    return;
                }
                handleFinished(exitCode);
            });

    m_process->start();
}

void UrlInspector::handleFinished(int exitCode)
{
    const QByteArray output = m_outputBuffer;
    const int start = output.indexOf('{');
    const QJsonDocument document = start >= 0
        ? QJsonDocument::fromJson(output.mid(start))
        : QJsonDocument();
    if (exitCode != 0 || !document.isObject()) {
        fail(errorFromOutput(m_outputTail, QStringLiteral("yt-dlp 没能解析这个链接")));
        return;
    }

    applyJson(document.object());
}

// json 解析结果,填充档位/详情列表,发 inspected 信号
void UrlInspector::applyJson(const QJsonObject &root)
{
    resetResults();
    m_info = MediaFormats::parseMediaInfo(root);

    // ---- 多集链接:只列剧集,不挑流 ----
    if (m_info.isPlaylist()) {
        m_playlistTitle = m_info.playlistTitle;
        // 快照结果，返回剧集列表时直接恢复而不是再次解析
        m_playlistJson = root;
        m_playlistView = true;
        int fallbackIndex = 0;
        for (const Episode &episode : m_info.episodes) {
            ++fallbackIndex;
            QVariantMap map;
            map[QStringLiteral("id")] = episode.id;
            map[QStringLiteral("itemId")] = MediaFormats::Episodes::itemId(episode);
            map[QStringLiteral("title")] = MediaFormats::Episodes::displayTitle(episode, fallbackIndex);
            map[QStringLiteral("url")] = episode.url;
            map[QStringLiteral("durationMs")] = episode.durationMs;
            map[QStringLiteral("index")] = fallbackIndex;
            m_episodes.append(map);
        }

        LOG_INFO("UrlInspector") << "解析出多集链接:" << m_playlistTitle
                                 << QStringLiteral("(%1 集)").arg(m_episodes.size());

        m_pickingEpisode = false;
        m_stage = QStringLiteral("inspected");
        emit stateChanged();
        emit pickedChanged();
        emit inspected();
        return;
    }

    for (const QualityOption &option : MediaFormats::qualityOptions(m_info))
        m_qualities.append(toMap(option));

    for (const FormatChoice &choice : MediaFormats::detailChoices(m_info)) {
        if (choice.role == 0)
            m_videoFormats.append(toMap(choice));
        else if (choice.role == 1)
            m_audioFormats.append(toMap(choice));
        else
            m_combinedFormats.append(toMap(choice));
    }

    LOG_INFO("UrlInspector") << "解析完成:" << m_info.title
                             << QStringLiteral("(档位 %1 个,流 %2 条)")
                                    .arg(m_qualities.size())
                                    .arg(m_videoFormats.size() + m_audioFormats.size()
                                         + m_combinedFormats.size());

    m_playlistView = false;
    m_pickingEpisode = false;
    m_stage = QStringLiteral("inspected");
    emit stateChanged();
    emit pickedChanged();
    emit inspected();
}

void UrlInspector::selectFormat(const QString &formatId, int role)
{
    if (formatId.isEmpty())
        return;

    if (role == 0) {
        // 点视频流:清掉"合一"占位(它同时占着音频位)
        m_pickedVideoId = m_pickedVideoId == formatId ? QString() : formatId;
        if (m_pickedVideoId.isEmpty())
            m_pickedAudioId.clear();
        // 手选的意思是"不要合一",音频留空让下载时补最优的一条
    } else if (role == 1) {
        m_pickedAudioId = m_pickedAudioId == formatId ? QString() : formatId;
    } else {
        // 合一:同时占掉两项(它自己就带声音)
        const bool same = m_pickedVideoId == formatId && m_pickedAudioId == formatId;
        m_pickedVideoId = same ? QString() : formatId;
        m_pickedAudioId = same ? QString() : formatId;
    }
    emit pickedChanged();
}

void UrlInspector::clearPicked()
{
    if (m_pickedVideoId.isEmpty() && m_pickedAudioId.isEmpty())
        return;
    m_pickedVideoId.clear();
    m_pickedAudioId.clear();
    emit pickedChanged();
}

void UrlInspector::pickEpisode(int index)
{
    if (index < 0 || index >= m_episodes.size())
        return;

    const QString url = m_episodes.at(index).toMap().value(QStringLiteral("url")).toString();
    if (url.isEmpty())
        return;

    // 整季那份结果留着:界面点"返回全部剧集"时要直接恢复(见 backToPlaylist)
    if (m_playlistJson.isEmpty() && !m_info.isPlaylist()) {
        // 理论上不会走到(剧集列表就是从整季结果来的);空快照时返回按钮会变成
        // "重新解析"的语义,不至于卡死
        LOG_WARN("UrlInspector") << "没有剧集快照,返回时要重新解析";
    }

    LOG_INFO("UrlInspector") << "选了第" << index + 1 << "集,重新解析:" << url;
    m_playlistView = false;
    m_pickingEpisode = true;
    inspect(url);   // 之后 qualities/详情 就是这一集的;inspected 会再发一次
}

void UrlInspector::backToPlaylist()
{
    if (m_playlistJson.isEmpty()) {
        LOG_WARN("UrlInspector") << "没有整季快照,无法返回";
        return;
    }

    LOG_INFO("UrlInspector") << "返回整季列表:" << m_playlistTitle;
    // 先拷出来:applyJson 内部的 resetResults 会把 m_playlistJson 清掉,
    // 而它走到剧集分支时又会重新填上(这里递进去的就是它自己那份)
    const QJsonObject snapshot = m_playlistJson;
    applyJson(snapshot);   // 整份恢复,不重跑 yt-dlp(applyJson 会发 inspected)
}

void UrlInspector::selectTier(const QString &spec)
{
    // 换档位 = 放弃手选(两种选择是互斥的两套语义)
    m_pickedVideoId.clear();
    m_pickedAudioId.clear();

    // 直播的档位没有"不设限"这回事(每条 variant 都是具体的流):
    // 界面上那个"最高画质"直接落到清晰度最高的一条,高亮才不会扑空
    if (m_info.live && (spec.isEmpty() || spec == QLatin1String("0"))) {
        m_tierSpec = m_qualities.isEmpty()
            ? QString()
            : m_qualities.first().toMap().value(QStringLiteral("spec")).toString();
        m_tierChosen = true;
        m_tierIsLiveId = !m_tierSpec.isEmpty();
        emit pickedChanged();
        return;
    }

    // 普通视频的"最高画质"用 "0" 表示(和 AppSettings 的取值一致),
    // 内部统一成空串 —— 它就是 StreamSpec 里"不设限"的写法
    m_tierSpec = spec == QLatin1String("0") ? QString() : spec;
    m_tierChosen = true;
    m_tierIsLiveId = false;
    // 直播的档位 spec 是 format_id:在 formats 里能找到同名的就是它
    if (m_info.live && !spec.isEmpty()) {
        for (const MediaFormat &format : m_info.formats) {
            if (format.formatId == spec) {
                m_tierIsLiveId = true;
                break;
            }
        }
    }
    emit pickedChanged();
}

QString UrlInspector::selectionSpec() const
{
    // 手选的精确 id 优先(详情视图)
    if (!m_pickedVideoId.isEmpty()) {
        return m_pickedAudioId.isEmpty() || m_pickedAudioId == m_pickedVideoId
            ? m_pickedVideoId
            : QStringLiteral("%1+%2").arg(m_pickedVideoId, m_pickedAudioId);
    }

    // 直播:档位 spec 就是某条流的 format_id,不带 HDR 后缀(直播的 HDR 由清单决定)。
    // 一条档位都没点过时,默认选清晰度最高的那条 —— 和以前"只给最高画质"的行为一致
    if (m_info.live) {
        if (m_tierChosen && m_tierIsLiveId)
            return m_tierSpec;
        if (!m_qualities.isEmpty())
            return m_qualities.first().toMap().value(QStringLiteral("spec")).toString();
        return QString();
    }

    if (m_tierSpec.isEmpty()) {
        // 还没选过任何东西 → 空(弹窗靠它判断"加入"按钮能不能点)
        // "最高画质"档 → 显式的 "0"(它比空串多了"分辨率优先"的排序规则)
        return m_tierChosen ? QStringLiteral("0") : QString();
    }

    return m_tierSpec;
}

QString UrlInspector::buildSpec(bool allowHdr) const
{
    const QString body = selectionSpec();
    if (body.isEmpty())
        return {};

    // HDR 后缀只缀在普通视频的档位上:手选的 id 是用户在详情里看着"HDR"标点出来的,
    // 不该再由开关改写;直播的 HDR 由清单自己决定
    if (m_info.live || !m_pickedVideoId.isEmpty())
        return body;
    return StreamSpec::withHdr(body, allowHdr);
}

void UrlInspector::cancel()
{
    if (m_process != nullptr) {
        LOG_INFO("UrlInspector") << "已取消";
        disconnect(m_process, nullptr, this, nullptr);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }

    resetResults();
    m_error.clear();
    m_url.clear();

    const bool stageChanged = m_stage != QStringLiteral("idle");
    m_stage = QStringLiteral("idle");
    if (stageChanged)
        emit stateChanged();
    else
        emit stateChanged(); // 结果清空了,绑定也要重算
}

void UrlInspector::resetResults()
{
    m_info = MediaInfo();
    m_qualities.clear();
    m_videoFormats.clear();
    m_audioFormats.clear();
    m_combinedFormats.clear();
    m_episodes.clear();
    m_playlistTitle.clear();
    m_playlistView = false;
    m_pickingEpisode = false;
    m_pickedVideoId.clear();
    m_pickedAudioId.clear();
    m_tierSpec.clear();
    m_tierChosen = false;
    m_tierIsLiveId = false;
    m_outputBuffer.clear();
    m_outputTail.clear();
}

void UrlInspector::fail(const QString &message)
{
    m_error = message;
    m_stage = QStringLiteral("failed");
    LOG_WARN("UrlInspector") << "失败:" << message;
    emit stateChanged();
    emit failed(message);
}
