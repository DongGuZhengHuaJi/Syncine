#include "FileTransferManager.h"

#include <cstring>
#include <utility>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtEndian>
#include <QtGlobal>

#include "core/Log.h"
#include "core/RoomTypes.h"
#include "session/MemberModel.h"
#include "session/RoomSession.h"
#include "transfer/TransferChannels.h"
#include "webrtc/WebrtcManager.h"

namespace {

constexpr int kChunkSize = 32 * 1024;              // 单个文件块
constexpr qint64 kReplayChunkSize = 1024 * 1024;   // 续传重算前缀哈希时的单次读取量
constexpr qint64 kSendBudgetPerTick = 256 * 1024;  // 每拍最多发送 256KB
constexpr int kPumpIntervalMs = 10;
constexpr int kNotifyIntervalMs = 200;

// 发送窗口：已读出但尚未被接收端 ACK 的字节数上限。
// 接收端每 128KB 或 100ms 回报一次。默认 2MB；可用环境变量
// SYNCINE_TRANSFER_WINDOW_KB 覆盖（128–16384），方便在长 RTT 链路上做对照。
constexpr qint64 kDefaultSendWindowBytes = 2 * 1024 * 1024;
const qint64 kSendWindowBytes = [] {
    bool ok = false;
    const int kb = qEnvironmentVariableIntValue("SYNCINE_TRANSFER_WINDOW_KB", &ok);
    if (ok && kb >= 128 && kb <= 16384)
        return static_cast<qint64>(kb) * 1024;
    return kDefaultSendWindowBytes;
}();
constexpr qint64 kAckEveryBytes = 128 * 1024;
constexpr qint64 kAckIntervalMs = 100;

// 数据通道自己的 SCTP 发送缓冲水位。应用层窗口已经限制了在途量，
// 这里只是第二道保险，避免 SCTP 队列无界增长。
const qint64 kSendBufferHighWater =
    qMax<qint64>(4 * 1024 * 1024, kSendWindowBytes + 1024 * 1024);
const qint64 kSendBufferLowWater =
    qMax<qint64>(1024 * 1024, kSendBufferHighWater / 4);
// 断线后等待 P2P 恢复/续传的最长时间
constexpr qint64 kInterruptedGraceMs = 60 * 1000;

// 数据传输停滞判定：15 秒没有收到 ACK / 数据块，转入 Interrupted。
// 之后每 5 秒发一次 resume；60 秒仍无进展则失败。
constexpr qint64 kDataStallMs = 15 * 1000;
constexpr qint64 kResumeRetryMs = 5 * 1000;

// 控制通道心跳。任意控制消息都算“活着”，因此大文件传输期间的 ACK
// 也能维持心跳；15 秒完全没动静才判定通道无响应。
constexpr int kHealthIntervalMs = 5 * 1000;
constexpr qint64 kHealthTimeoutMs = 15 * 1000;

// 传输中每 10 秒打一条进度；GetStats 诊断每 15 秒一次。
// 两者都只在传输期间运行，避免空闲时刷日志。
constexpr qint64 kLogIntervalMs = 10 * 1000;
constexpr qint64 kStatsIntervalMs = 15 * 1000;

quint32 idOf(const QJsonObject &message)
{
    return static_cast<quint32>(message.value(QStringLiteral("id")).toInt());
}

QJsonObject makeControl(const QString &type, quint32 id)
{
    QJsonObject message;
    message.insert(QStringLiteral("t"), type);
    message.insert(QStringLiteral("id"), static_cast<int>(id));
    return message;
}

} // namespace

FileTransferManager::FileTransferManager(WebrtcManager *webrtc,
                                         RoomSession *session,
                                         QObject *parent)
    : QObject(parent),
      m_webrtc(webrtc),
      m_session(session) {

    if (m_webrtc != nullptr) {
        connect(m_webrtc, &WebrtcManager::dataChannelOpened,
                this, &FileTransferManager::onDataChannelOpened);
        connect(m_webrtc, &WebrtcManager::dataChannelClosed,
                this, &FileTransferManager::onDataChannelClosed);
        connect(m_webrtc, &WebrtcManager::dataChannelMessageReceived,
                this, &FileTransferManager::onDataChannelMessage);
        connect(m_webrtc, &WebrtcManager::peerRemoved,
                this, &FileTransferManager::onPeerRemoved);
    }

    if (m_session != nullptr)
        connect(m_session, &RoomSession::left, this, &FileTransferManager::onRoomLeft);

    m_pumpTimer.setInterval(kPumpIntervalMs);
    m_pumpTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pumpTimer, &QTimer::timeout, this, &FileTransferManager::onPumpTimer);

    m_notifyTimer.setInterval(kNotifyIntervalMs);
    connect(&m_notifyTimer, &QTimer::timeout, this, &FileTransferManager::onNotifyTimer);

    m_healthTimer.setInterval(kHealthIntervalMs);
    connect(&m_healthTimer, &QTimer::timeout, this, &FileTransferManager::onHealthTimer);
}

FileTransferManager::~FileTransferManager() {
    m_pumpTimer.stop();
    m_notifyTimer.stop();
    m_healthTimer.stop();
}

// ============================
// 模型
// ============================

FileTransferModel *FileTransferManager::transfersFor(const QString &peerId) {
    return ensureModel(peerId);
}

FileTransferModel *FileTransferManager::ensureModel(const QString &peerId) {
    const auto it = m_models.constFind(peerId);
    if (it != m_models.constEnd())
        return it.value();

    auto *model = new FileTransferModel(peerId, this);
    m_models.insert(peerId, model);
    return model;
}

FileTransferModel *FileTransferManager::modelFor(const QString &peerId) const {
    const auto it = m_models.constFind(peerId);
    return it != m_models.constEnd() ? it.value() : nullptr;
}

bool FileTransferManager::isConnected(const QString &peerId) const {
    if (m_webrtc == nullptr || m_unresponsive.contains(peerId))
        return false;
    // 两条通道都打开才算连通：控制通道负责 ACK/取消，数据通道负责文件块
    return m_webrtc->isDataChannelOpen(peerId,
                                       QString::fromLatin1(TransferChannel::ControlId))
           && m_webrtc->isDataChannelOpen(
               peerId, QString::fromLatin1(TransferChannel::DataId));
}

TransferRecordPtr FileTransferManager::findOutgoing(const QString &peerId,
                                                    quint32 id) const {
    FileTransferModel *model = modelFor(peerId);
    if (model == nullptr)
        return {};

    for (const TransferRecordPtr &record : model->records()) {
        if (record->direction == TransferDirection::Send && record->id == id)
            return record;
    }
    return {};
}

TransferRecordPtr FileTransferManager::findIncoming(const QString &peerId,
                                                    quint32 id) const {
    FileTransferModel *model = modelFor(peerId);
    if (model == nullptr)
        return {};

    for (const TransferRecordPtr &record : model->records()) {
        if (record->direction == TransferDirection::Receive && record->id == id)
            return record;
    }
    return {};
}

// ============================
// 用户操作
// ============================

void FileTransferManager::sendFile(const QString &peerId, const QUrl &fileUrl) {
    if (!isConnected(peerId)) {
        emit errorOccurred(peerId, QStringLiteral("与对方的数据连接尚未就绪，请稍后再试"));
        return;
    }

    const QString path = fileUrl.toLocalFile();
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || !info.isFile() || !info.isReadable()) {
        emit errorOccurred(peerId, QStringLiteral("无法读取所选文件"));
        return;
    }

    // 同一文件正在传输时不重复发起：重复请求会占满窗口，也会让对方看到
    // 多条同名记录，反而分不清哪条是有效的
    if (FileTransferModel *model = modelFor(peerId)) {
        for (const TransferRecordPtr &existing : model->records()) {
            if (existing->direction == TransferDirection::Send
                && isActiveTransferState(existing->state)
                && existing->sourcePath == path) {
                emit errorOccurred(peerId,
                                   QStringLiteral("该文件正在传输中，请等待完成或先取消"));
                return;
            }
        }
    }

    auto record = std::make_shared<TransferRecord>();
    record->id = m_nextId++;
    record->peerId = peerId;
    record->peerName = nicknameFor(peerId);
    record->direction = TransferDirection::Send;
    record->state = TransferState::WaitingAccept;
    record->fileName = info.fileName();
    record->sourcePath = path;
    record->totalBytes = info.size();
    record->createdAt = QDateTime::currentDateTime();

    ensureModel(peerId)->addRecord(record);

    QJsonObject offer = makeControl(QStringLiteral("offer"), record->id);
    offer.insert(QStringLiteral("name"), record->fileName);
    offer.insert(QStringLiteral("size"), static_cast<double>(record->totalBytes));
    if (!sendControl(peerId, offer)) {
        failRecord(record, QStringLiteral("数据通道不可用"), false);
        return;
    }

    LOG_INFO("Transfer") << "发起文件传输:" << record->fileName
                         << QStringLiteral("%1 字节").arg(record->totalBytes)
                         << "→" << peerId;
}

void FileTransferManager::acceptTransfer(const QString &peerId, quint32 id) {
    const TransferRecordPtr record = findIncoming(peerId, id);
    if (record == nullptr)
        return;
    const bool awaitingAccept =
        record->state == TransferState::WaitingAccept
        || (record->state == TransferState::Interrupted
            && record->interruptedFrom == TransferState::WaitingAccept);
    if (!awaitingAccept)
        return;

    const QString directoryPath = downloadDirectory();
    if (!QDir().mkpath(directoryPath)) {
        failRecord(record, QStringLiteral("无法创建接收目录"), true);
        return;
    }

    const QString target = uniquePath(directoryPath, record->fileName);
    auto file = std::make_shared<QFile>(target + QStringLiteral(".part"));
    if (!file->open(QIODevice::WriteOnly)) {
        failRecord(record, QStringLiteral("无法写入接收目录: %1").arg(file->errorString()),
                   true);
        return;
    }

    record->targetPath = target;
    record->partialPath = file->fileName();
    record->file = file;
    record->hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    record->transferred = 0;
    record->peerReceived = 0;
    record->hashReplayRemaining = 0;
    record->resumeRequested = false;
    record->resumeAllowsRewind = false;
    record->finalHash.clear();
    record->interruptedFrom = TransferState::Failed;
    record->interruptedAtMs = 0;
    record->speedBps = 0;
    record->lastSampleBytes = 0;
    record->lastSampleMs = 0;
    record->lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    record->lastAckBytes = 0;
    record->lastAckMs = record->lastProgressMs;
    record->lastResumeMs = 0;
    record->lastLogMs = 0;
    record->state = TransferState::Transferring;

    if (!sendControl(peerId, makeControl(QStringLiteral("accept"), record->id))) {
        failRecord(record, QStringLiteral("数据通道不可用"), false);
        return;
    }

    ensureModel(peerId)->updateRecord(record);
    updatePending(peerId);
    startPumping();

    LOG_INFO("Transfer") << "接受文件:" << record->fileName << "→" << record->targetPath;
}

void FileTransferManager::rejectTransfer(const QString &peerId, quint32 id) {
    const TransferRecordPtr record = findIncoming(peerId, id);
    if (record == nullptr)
        return;
    const bool awaitingAccept =
        record->state == TransferState::WaitingAccept
        || (record->state == TransferState::Interrupted
            && record->interruptedFrom == TransferState::WaitingAccept);
    if (!awaitingAccept)
        return;

    sendControl(peerId, makeControl(QStringLiteral("reject"), record->id));
    finishRecord(record, TransferState::Rejected);
    LOG_INFO("Transfer") << "拒绝接收:" << record->fileName;
}

void FileTransferManager::cancelTransfer(const QString &peerId, quint32 id) {
    TransferRecordPtr record = findOutgoing(peerId, id);
    if (record == nullptr || !isActiveTransferState(record->state))
        record = findIncoming(peerId, id);

    if (record == nullptr || !isActiveTransferState(record->state))
        return;

    sendControl(peerId, makeControl(QStringLiteral("cancel"), record->id));
    finishRecord(record, TransferState::Cancelled);
    LOG_INFO("Transfer") << "取消传输:" << record->fileName;
}

void FileTransferManager::revealTransfer(const QString &peerId, quint32 id) {
    TransferRecordPtr record = findIncoming(peerId, id);
    if (record == nullptr)
        record = findOutgoing(peerId, id);
    if (record == nullptr)
        return;

    const QString path = record->targetPath.isEmpty() ? record->sourcePath
                                                      : record->targetPath;
    if (path.isEmpty())
        return;

    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

// ============================
// 数据通道事件
// ============================

void FileTransferManager::onDataChannelOpened(const QString &peerId,
                                              const QString &channel) {
    const bool wasReady = m_controlOpen.contains(peerId) && m_dataOpen.contains(peerId);

    if (channel == QLatin1String(TransferChannel::ControlId)) {
        m_controlOpen.insert(peerId);
    } else if (channel == QLatin1String(TransferChannel::DataId)) {
        m_dataOpen.insert(peerId);
    } else {
        return;
    }

    LOG_INFO("Transfer") << "与" << peerId << "的数据通道已打开:" << channel;

    const bool ready = m_controlOpen.contains(peerId) && m_dataOpen.contains(peerId);
    if (!ready || wasReady)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_unresponsive.remove(peerId);
    m_lastPongMs.insert(peerId, now);
    m_sendPaused.remove(peerId);
    if (!m_healthTimer.isActive())
        m_healthTimer.start();

    // 立刻探一次活；顺便让双方都建立“最近有控制消息”的时间戳
    sendControl(peerId, makeControl(QStringLiteral("ping"), 0));

    emit connectionChanged(peerId);

    // 通道重建后，把中断的传输重新挂上。新通道是新的 SCTP 流，
    // 旧数据不会再来，允许按接收端偏移回退。
    reissueAfterReconnect(peerId, /*allowRewind=*/true);

    if (m_pendingControl.contains(peerId) && !m_pumpTimer.isActive())
        m_pumpTimer.start();
}

void FileTransferManager::onDataChannelClosed(const QString &peerId,
                                              const QString &channel) {
    const bool wasReady = m_controlOpen.contains(peerId) && m_dataOpen.contains(peerId);

    if (channel == QLatin1String(TransferChannel::ControlId)) {
        m_controlOpen.remove(peerId);
    } else if (channel == QLatin1String(TransferChannel::DataId)) {
        m_dataOpen.remove(peerId);
    } else {
        return;
    }

    if (!wasReady)
        return;

    handleChannelDown(peerId, QStringLiteral("数据通道已关闭"));

    bool anyReady = false;
    for (const QString &id : std::as_const(m_controlOpen)) {
        if (m_dataOpen.contains(id)) {
            anyReady = true;
            break;
        }
    }
    if (!anyReady)
        m_healthTimer.stop();
}

void FileTransferManager::onPeerRemoved(const QString &peerId) {
    m_controlOpen.remove(peerId);
    m_dataOpen.remove(peerId);
    m_unresponsive.remove(peerId);
    m_lastPongMs.remove(peerId);
    m_pendingControl.remove(peerId);
    m_sendPaused.remove(peerId);
    m_statsPeers.remove(peerId);
    if (m_statsPeers.isEmpty())
        m_lastStatsMs = 0;

    FileTransferModel *model = modelFor(peerId);
    if (model != nullptr) {
        const QList<TransferRecordPtr> records = model->records();
        for (const TransferRecordPtr &record : records) {
            if (isActiveTransferState(record->state)) {
                failRecord(record, QStringLiteral("对方已离开房间"), false);
            }
        }
    }

    bool anyReady = false;
    for (const QString &id : std::as_const(m_controlOpen)) {
        if (m_dataOpen.contains(id)) {
            anyReady = true;
            break;
        }
    }
    if (!anyReady)
        m_healthTimer.stop();

    emit connectionChanged(peerId);
}

void FileTransferManager::onRoomLeft() {
    for (FileTransferModel *model : std::as_const(m_models)) {
        const QList<TransferRecordPtr> records = model->records();
        for (const TransferRecordPtr &record : records) {
            if (isActiveTransferState(record->state)) {
                failRecord(record, QStringLiteral("已离开房间"), false);
            }
        }
        model->clear();
    }

    m_controlOpen.clear();
    m_dataOpen.clear();
    m_unresponsive.clear();
    m_lastPongMs.clear();
    m_pendingControl.clear();
    m_sendPaused.clear();
    m_statsPeers.clear();
    m_lastStatsMs = 0;
    m_pumpTimer.stop();
    m_notifyTimer.stop();
    m_healthTimer.stop();
}

void FileTransferManager::onDataChannelMessage(const QString &peerId,
                                               const QString &channel,
                                               bool binary, const QByteArray &data) {
    if (binary && channel != QLatin1String(TransferChannel::DataId)) {
        LOG_WARN("Transfer") << "在控制通道上收到二进制消息，已忽略:" << peerId;
        return;
    }
    if (!binary && channel != QLatin1String(TransferChannel::ControlId)) {
        LOG_WARN("Transfer") << "在数据通道上收到控制消息，已忽略:" << peerId;
        return;
    }

    if (binary) {
        if (data.size() <= 4)
            return;

        const quint32 id = qFromBigEndian<quint32>(data.constData());
        const TransferRecordPtr record = findIncoming(peerId, id);
        if (record == nullptr)
            return;

        // 数据停滞/通道抖动后被标记为 Interrupted，但文件仍在、
        // 哈希状态也还在——收到后续数据块可以直接接着写。
        const bool recovering =
            record->state == TransferState::Interrupted
            && record->interruptedFrom == TransferState::Transferring;
        if (record->state != TransferState::Transferring && !recovering)
            return;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (recovering) {
            if (record->file == nullptr || record->hash == nullptr
                || record->file->size() != record->transferred) {
                failRecord(record, QStringLiteral("断点状态不一致，无法继续接收"), true);
                return;
            }
            record->file->seek(record->transferred);
            record->state = TransferState::Transferring;
            record->interruptedFrom = TransferState::Failed;
            record->interruptedAtMs = 0;
            record->resumeRequested = false;
            record->resumeAllowsRewind = false;
            record->lastResumeMs = 0;
            record->lastProgressMs = now;
            record->speedBps = 0;
            record->lastSampleMs = now;
            record->lastSampleBytes = record->transferred;
            ensureModel(peerId)->updateRecord(record);
            LOG_INFO("Transfer") << "接收端恢复接收:" << record->fileName
                                 << "offset=" << record->transferred;
        }

        const QByteArray chunk = data.mid(4);
        if (record->transferred + chunk.size() > record->totalBytes) {
            failRecord(record, QStringLiteral("接收到的数据超过声明的文件大小"), true);
            return;
        }

        const qint64 written = record->file != nullptr ? record->file->write(chunk) : -1;
        if (written != chunk.size()) {
            failRecord(record,
                       QStringLiteral("写入文件失败: %1")
                           .arg(record->file != nullptr ? record->file->errorString()
                                                        : QStringLiteral("文件未打开")),
                       true);
            return;
        }

        record->hash->addData(chunk);
        record->transferred += written;
        record->lastProgressMs = now;

        // 合并 ACK：每 128KB 或 100ms 回报一次；最后一块必须回报，
        // 否则发送端会一直等不到 complete 的时机
        if (record->transferred >= record->totalBytes
            || record->transferred - record->lastAckBytes >= kAckEveryBytes
            || now - record->lastAckMs >= kAckIntervalMs) {
            sendAck(record);
        }

        if (!m_notifyTimer.isActive())
            m_notifyTimer.start();
        return;
    }

    // 任意控制消息都说明控制通道还活着
    noteChannelAlive(peerId);

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (!document.isObject()) {
        LOG_WARN("Transfer") << "收到无法解析的控制消息:" << error.errorString();
        return;
    }

    const QJsonObject message = document.object();
    const QString type = message.value(QStringLiteral("t")).toString();
    if (type == QLatin1String("offer")) {
        handleOffer(peerId, message);
    } else if (type == QLatin1String("accept")) {
        handleAccept(peerId, message);
    } else if (type == QLatin1String("reject")) {
        handleReject(peerId, message);
    } else if (type == QLatin1String("cancel")) {
        handleCancel(peerId, message);
    } else if (type == QLatin1String("complete")) {
        handleComplete(peerId, message);
    } else if (type == QLatin1String("result")) {
        handleResult(peerId, message);
    } else if (type == QLatin1String("ack")) {
        handleAck(peerId, message);
    } else if (type == QLatin1String("resume")) {
        handleResume(peerId, message);
    } else if (type == QLatin1String("resume_ack")) {
        handleResumeAck(peerId, message);
    } else if (type == QLatin1String("ping")) {
        handlePing(peerId, message);
    } else if (type == QLatin1String("pong")) {
        handlePong(peerId, message);
    } else {
        LOG_DEBUG("Transfer") << "忽略未知控制消息:" << type;
    }
}

// ============================
// 协议
// ============================

bool FileTransferManager::sendControl(const QString &peerId,
                                      const QJsonObject &message) {
    if (m_webrtc == nullptr)
        return false;

    const QString channel = QString::fromLatin1(TransferChannel::ControlId);
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (m_webrtc->sendDataChannelMessage(peerId, channel, payload, false))
        return true;

    // 控制通道极少被占满（它不和文件块共用队列）；这里仍保留队列，
    // 以应对 SCTP 短暂阻塞。控制消息不能丢，取消/校验都靠它。
    if (!m_webrtc->isDataChannelOpen(peerId, channel)) {
        LOG_DEBUG("Transfer") << "控制消息发送失败(通道已关闭):"
                              << message.value(QStringLiteral("t")).toString()
                              << "→" << peerId;
        return false;
    }

    m_pendingControl[peerId].append(payload);
    if (!m_pumpTimer.isActive())
        m_pumpTimer.start();
    return true;
}

void FileTransferManager::handleOffer(const QString &peerId, const QJsonObject &message) {
    const quint32 id = idOf(message);
    const QString name = sanitizeFileName(message.value(QStringLiteral("name")).toString());
    const qint64 size = static_cast<qint64>(message.value(QStringLiteral("size")).toDouble(-1));

    if (id == 0 || name.isEmpty() || size < 0) {
        LOG_WARN("Transfer") << "忽略非法的文件请求:" << name << size;
        return;
    }
    if (findIncoming(peerId, id) != nullptr) {
        // 已经有一条同 id 的接收记录。
        const TransferRecordPtr existing = findIncoming(peerId, id);

        // 终态记录重发 offer：告诉发送端这条已经结束，别让它一直等
        if (existing->state == TransferState::Rejected) {
            sendControl(peerId, makeControl(QStringLiteral("reject"), id));
            return;
        }
        if (existing->state == TransferState::Cancelled
            || existing->state == TransferState::Failed
            || existing->state == TransferState::Completed) {
            sendControl(peerId, makeControl(QStringLiteral("cancel"), id));
            return;
        }

        // 停在“已接受、但 accept 消息在断线中丢了”的状态：
        // 重发 accept，让发送方从 0 开始。
        if (existing->state == TransferState::Interrupted
            && existing->interruptedFrom == TransferState::Transferring
            && existing->transferred == 0) {
            if (existing->file == nullptr) {
                failRecord(existing, QStringLiteral("断点文件丢失"), true);
                return;
            }
            existing->file->resize(0);
            existing->file->seek(0);
            existing->hash = std::make_shared<QCryptographicHash>(
                QCryptographicHash::Sha256);
            existing->state = TransferState::Transferring;
            existing->interruptedFrom = TransferState::Failed;
            existing->interruptedAtMs = 0;
            existing->resumeRequested = false;
            existing->resumeAllowsRewind = false;
            existing->lastProgressMs = QDateTime::currentMSecsSinceEpoch();
            existing->lastAckBytes = 0;
            existing->lastAckMs = existing->lastProgressMs;
            existing->lastResumeMs = 0;
            ensureModel(peerId)->updateRecord(existing);
            sendControl(peerId, makeControl(QStringLiteral("accept"), existing->id));
            LOG_INFO("Transfer") << "重发 accept（上次在断线中丢失）:"
                                 << existing->fileName;
        }
        return;
    }

    auto record = std::make_shared<TransferRecord>();
    record->id = id;
    record->peerId = peerId;
    record->peerName = nicknameFor(peerId);
    record->direction = TransferDirection::Receive;
    record->state = TransferState::WaitingAccept;
    record->fileName = name;
    record->totalBytes = size;
    record->createdAt = QDateTime::currentDateTime();

    ensureModel(peerId)->addRecord(record);
    updatePending(peerId);

    LOG_INFO("Transfer") << "收到文件请求:" << name
                         << QStringLiteral("%1 字节").arg(size) << "←" << peerId;
    emit incomingOffer(peerId, record->peerName, record->fileName);
}

void FileTransferManager::handleAccept(const QString &peerId, const QJsonObject &message) {
    const TransferRecordPtr record = findOutgoing(peerId, idOf(message));
    if (record == nullptr)
        return;

    const bool acceptable =
        record->state == TransferState::WaitingAccept
        || (record->state == TransferState::Interrupted
            && record->interruptedFrom == TransferState::WaitingAccept);
    if (!acceptable)
        return;

    auto file = std::make_shared<QFile>(record->sourcePath);
    if (!file->open(QIODevice::ReadOnly)) {
        failRecord(record, QStringLiteral("无法读取源文件: %1").arg(file->errorString()),
                   true);
        return;
    }
    if (file->size() != record->totalBytes) {
        file->close();
        failRecord(record, QStringLiteral("文件在等待期间发生变化，请重新发送"), true);
        return;
    }

    record->file = file;
    record->hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    record->transferred = 0;
    record->peerReceived = 0;
    record->hashReplayRemaining = 0;
    record->resumeRequested = false;
    record->resumeAllowsRewind = false;
    record->finalHash.clear();
    record->interruptedFrom = TransferState::Failed;
    record->interruptedAtMs = 0;
    record->speedBps = 0;
    record->lastSampleBytes = 0;
    record->lastSampleMs = 0;
    record->lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    record->lastAckBytes = 0;
    record->lastAckMs = record->lastProgressMs;
    record->lastResumeMs = 0;
    record->lastLogMs = 0;
    record->state = TransferState::Transferring;

    ensureModel(peerId)->updateRecord(record);
    startPumping();

    LOG_INFO("Transfer") << "对方已接收，开始发送:" << record->fileName;
}

void FileTransferManager::handleReject(const QString &peerId, const QJsonObject &message) {
    const TransferRecordPtr record = findOutgoing(peerId, idOf(message));
    if (record == nullptr)
        return;

    const bool awaitingAccept =
        record->state == TransferState::WaitingAccept
        || (record->state == TransferState::Interrupted
            && record->interruptedFrom == TransferState::WaitingAccept);
    if (!awaitingAccept)
        return;

    finishRecord(record, TransferState::Rejected);
    LOG_INFO("Transfer") << "对方拒绝接收:" << record->fileName;
}

void FileTransferManager::handleCancel(const QString &peerId, const QJsonObject &message) {
    TransferRecordPtr record = findOutgoing(peerId, idOf(message));
    if (record == nullptr)
        record = findIncoming(peerId, idOf(message));
    if (record == nullptr)
        return;

    if (!isActiveTransferState(record->state))
        return;

    finishRecord(record, TransferState::Cancelled);
    LOG_INFO("Transfer") << "对方取消了传输:" << record->fileName;
}

void FileTransferManager::handleComplete(const QString &peerId, const QJsonObject &message) {
    const TransferRecordPtr record = findIncoming(peerId, idOf(message));
    if (record == nullptr)
        return;

    // 对端重发了 complete：上次的 result 在断线中丢了，重发一次
    if (record->state == TransferState::Completed) {
        QJsonObject result = makeControl(QStringLiteral("result"), record->id);
        result.insert(QStringLiteral("ok"), true);
        sendControl(peerId, result);
        return;
    }
    if (record->state == TransferState::Failed) {
        QJsonObject result = makeControl(QStringLiteral("result"), record->id);
        result.insert(QStringLiteral("ok"), false);
        sendControl(peerId, result);
        return;
    }

    if (record->state == TransferState::Interrupted
        && record->interruptedFrom != TransferState::Transferring) {
        return;
    }
    if (record->state != TransferState::Transferring
        && record->state != TransferState::Interrupted) {
        return;
    }

    verifyIncoming(record, message.value(QStringLiteral("sha256")).toString());
}

void FileTransferManager::handleResult(const QString &peerId, const QJsonObject &message) {
    const TransferRecordPtr record = findOutgoing(peerId, idOf(message));
    if (record == nullptr)
        return;

    // 正常是 Verifying；断线时可能是 Interrupted(Verifying)
    const bool acceptable =
        record->state == TransferState::Verifying
        || (record->state == TransferState::Interrupted
            && record->interruptedFrom == TransferState::Verifying);
    if (!acceptable)
        return;

    if (message.value(QStringLiteral("ok")).toBool()) {
        record->verified = true;
        finishRecord(record, TransferState::Completed);
        LOG_INFO("Transfer") << "对方校验通过，传输完成:" << record->fileName;
        return;
    }

    failRecord(record, QStringLiteral("对方校验失败，文件可能已损坏"), false);
}

void FileTransferManager::handleAck(const QString &peerId, const QJsonObject &message) {
    const TransferRecordPtr record = findOutgoing(peerId, idOf(message));
    if (record == nullptr)
        return;

    const bool transferring = record->state == TransferState::Transferring;
    const bool interrupted =
        record->state == TransferState::Interrupted
        && record->interruptedFrom == TransferState::Transferring;
    if (!transferring && !interrupted)
        return;

    const qint64 received = static_cast<qint64>(
        message.value(QStringLiteral("received")).toDouble(-1));
    if (received < 0 || received > record->totalBytes) {
        failRecord(record, QStringLiteral("对方回报的接收进度非法"), true);
        return;
    }
    if (received < record->peerReceived)
        return; // 旧的 ACK，忽略

    const qint64 confirmed = qMin(received, record->transferred);
    record->lastProgressMs = QDateTime::currentMSecsSinceEpoch();

    if (interrupted) {
        // 停滞期间收到的 ACK 说明接收端还活着：按它报告的偏移对齐。
        // 旧通道上不允许回退重发（SCTP 可能还在重传之前的块）。
        resumeFromReceived(record, confirmed, /*allowRewind=*/false);
        return;
    }

    record->peerReceived = qMax(record->peerReceived, confirmed);

    // 所有数据都已被接收端确认，才发 complete。控制通道是无序的，
    // 过早发 complete 会在最后几个数据块之前到达接收端。
    if (record->transferred >= record->totalBytes
        && record->peerReceived >= record->totalBytes) {
        finishSending(record);
        return;
    }

    if (!m_pumpTimer.isActive())
        m_pumpTimer.start();
}

void FileTransferManager::handlePing(const QString &peerId, const QJsonObject &) {
    sendControl(peerId, makeControl(QStringLiteral("pong"), 0));
}

void FileTransferManager::handlePong(const QString &, const QJsonObject &) {
    // 时间戳已在 noteChannelAlive 中更新，这里无需额外处理
}

void FileTransferManager::handleResume(const QString &peerId, const QJsonObject &message) {
    const TransferRecordPtr record = findIncoming(peerId, idOf(message));
    if (record == nullptr)
        return;

    // 允许在 Transferring 状态下收到 resume：数据停滞时发送端会主动
    // 请求重新对齐，接收端回报当前真实偏移即可。
    const bool resumable =
        record->state == TransferState::Transferring
        || (record->state == TransferState::Interrupted
            && record->interruptedFrom == TransferState::Transferring);
    if (!resumable)
        return;

    if (record->file == nullptr) {
        if (record->partialPath.isEmpty()) {
            failRecord(record, QStringLiteral("断点文件不存在"), true);
            return;
        }
        auto file = std::make_shared<QFile>(record->partialPath);
        if (!file->open(QIODevice::Append)) {
            failRecord(record,
                       QStringLiteral("无法打开断点文件: %1").arg(file->errorString()),
                       true);
            return;
        }
        record->file = file;
    }

    const qint64 size = record->file->size();
    if (size != record->transferred || record->hash == nullptr) {
        // 文件大小和已经哈希的字节数对不上，无法安全续传
        failRecord(record, QStringLiteral("断点状态不一致，无法续传"), true);
        return;
    }
    if (!record->file->seek(size)) {
        failRecord(record, QStringLiteral("无法定位断点文件"), true);
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    record->state = TransferState::Transferring;
    record->interruptedFrom = TransferState::Failed;
    record->interruptedAtMs = 0;
    record->resumeRequested = false;
    record->resumeAllowsRewind = false;
    record->lastResumeMs = 0;
    record->speedBps = 0;
    record->lastSampleMs = now;
    record->lastSampleBytes = record->transferred;
    record->lastProgressMs = now;
    record->lastAckBytes = record->transferred;
    record->lastAckMs = now;
    ensureModel(peerId)->updateRecord(record);

    QJsonObject ack = makeControl(QStringLiteral("resume_ack"), record->id);
    ack.insert(QStringLiteral("received"), static_cast<double>(record->transferred));
    sendControl(peerId, ack);

    if (!m_notifyTimer.isActive())
        m_notifyTimer.start();

    LOG_INFO("Transfer") << "接收端恢复续传:" << record->fileName
                         << "offset=" << record->transferred;
}

void FileTransferManager::handleResumeAck(const QString &peerId,
                                          const QJsonObject &message) {
    const TransferRecordPtr record = findOutgoing(peerId, idOf(message));
    if (record == nullptr)
        return;
    if (record->state != TransferState::Interrupted
        || record->interruptedFrom != TransferState::Transferring) {
        return;
    }

    const qint64 received = static_cast<qint64>(
        message.value(QStringLiteral("received")).toDouble(-1));
    if (received < 0 || received > record->totalBytes) {
        failRecord(record, QStringLiteral("对方返回的续传偏移非法"), true);
        return;
    }

    if (!resumeFromReceived(record, received, record->resumeAllowsRewind))
        return;

    LOG_INFO("Transfer") << "发送端恢复续传:" << record->fileName
                         << "offset=" << received;
}

// ============================
// 传输推进
// ============================

void FileTransferManager::startPumping() {
    if (!m_pumpTimer.isActive())
        m_pumpTimer.start();
    if (!m_notifyTimer.isActive())
        m_notifyTimer.start();
}

void FileTransferManager::onPumpTimer() {
    if (m_webrtc == nullptr) {
        m_pumpTimer.stop();
        return;
    }

    bool active = false;

    // 数据通道事件里拿不到 peers 列表，控制消息队列按 model 的 key 遍历
    for (auto it = m_models.cbegin(); it != m_models.cend(); ++it) {
        const QString &peerId = it.key();

        // 先补发积压的控制消息；仍发不出去就留到下一拍。
        // 不依赖 m_controlOpen：即使 open 信号还没处理，Send 自己会判断
        auto pending = m_pendingControl.find(peerId);
        if (pending != m_pendingControl.end()) {
            const QString controlChannel =
                QString::fromLatin1(TransferChannel::ControlId);
            QVector<QByteArray> &messages = pending.value();
            while (!messages.isEmpty()) {
                if (!m_webrtc->sendDataChannelMessage(peerId, controlChannel,
                                                       messages.first(), false))
                    break;
                messages.removeFirst();
            }
            if (messages.isEmpty()) {
                m_pendingControl.erase(pending);
            } else {
                active = true;
            }
        }

        if (!isConnected(peerId))
            continue;

        FileTransferModel *model = it.value();

        bool hasSending = false;
        for (const TransferRecordPtr &record : model->records()) {
            if (record->direction == TransferDirection::Send
                && record->state == TransferState::Transferring) {
                hasSending = true;
                break;
            }
        }
        if (!hasSending)
            continue;

        // 只要还有发送任务，节拍就必须继续跑；即使当前因为水位被暂停。
        active = true;

        const qint64 buffered = static_cast<qint64>(
            m_webrtc->dataChannelBufferedAmount(
                peerId, QString::fromLatin1(TransferChannel::DataId)));
        bool &paused = m_sendPaused[peerId];
        if (paused) {
            if (buffered <= kSendBufferLowWater)
                paused = false;
        } else if (buffered >= kSendBufferHighWater) {
            paused = true;
            LOG_TRACE("Transfer") << "发送缓冲达到高水位，暂停:" << peerId
                                  << buffered << "字节";
        }
        if (paused)
            continue;

        // 只发送到高水位为止；实际速率仍由 SCTP 拥塞控制决定。
        qint64 budget = qMin(kSendBudgetPerTick, kSendBufferHighWater - buffered);
        if (budget <= 0)
            continue;

        bool progressed = true;

        // 同一拍内轮流推进多条传输，避免第一条把预算吃光
        while (budget > 0 && progressed) {
            progressed = false;
            for (const TransferRecordPtr &record : model->records()) {
                if (record->direction != TransferDirection::Send
                    || record->state != TransferState::Transferring) {
                    continue;
                }

                if (sendNextChunk(record, &budget))
                    progressed = true;
                if (budget <= 0)
                    break;
            }
        }
    }

    if (!active)
        m_pumpTimer.stop();
}

bool FileTransferManager::sendNextChunk(const TransferRecordPtr &record, qint64 *budget) {
    if (record->file == nullptr || record->hash == nullptr)
        return false;

    // 续传时先把 [0, transferred) 的前缀重新哈希一遍，让发送端的
    // SHA-256 状态和接收端的 .part 文件对齐。每拍只处理一块就返回，
    // 让出事件循环；重放本身也算进展，避免被停滞看门狗误判。
    if (record->hashReplayRemaining > 0) {
        const qint64 want = qMin<qint64>(kReplayChunkSize,
                                         record->hashReplayRemaining);
        if (want <= 0)
            return false;

        const QByteArray prefix = record->file->read(want);
        if (prefix.isEmpty()) {
            failRecord(record,
                       QStringLiteral("续传时读取源文件失败: %1")
                           .arg(record->file->errorString()),
                       true);
            return false;
        }

        record->hash->addData(prefix);
        record->hashReplayRemaining -= prefix.size();
        record->lastProgressMs = QDateTime::currentMSecsSinceEpoch();
        *budget -= qMin<qint64>(*budget, prefix.size());

        if (record->hashReplayRemaining == 0) {
            record->file->seek(record->transferred);
            // 全部数据已在对端（例如停滞发生在最后一块之后），
            // 补齐哈希后直接进入校验
            if (record->transferred >= record->totalBytes
                && record->peerReceived >= record->totalBytes)
                finishSending(record);
        }
        // 返回 false 只结束本拍，不结束发送节拍（state 仍是 Transferring）
        return false;
    }

    const qint64 remaining = record->totalBytes - record->transferred;
    if (remaining <= 0) {
        // 数据已全部发出，但要等接收端 ACK 到最后一块才能发 complete
        if (record->peerReceived >= record->totalBytes)
            finishSending(record);
        return false;
    }

    // 应用层发送窗口：未被 ACK 的字节不超过 kSendWindowBytes
    const qint64 inFlight = record->transferred - record->peerReceived;
    if (inFlight >= kSendWindowBytes)
        return false;
    const qint64 windowLeft = kSendWindowBytes - inFlight;

    const qint64 want = qMin<qint64>(
        kChunkSize, qMin(remaining, qMin(*budget, windowLeft)));
    if (want <= 0)
        return false;

    const QByteArray chunk = record->file->read(want);
    if (chunk.isEmpty()) {
        failRecord(record, QStringLiteral("读取源文件失败: %1").arg(record->file->errorString()),
                   true);
        return false;
    }

    QByteArray frame;
    frame.resize(4 + chunk.size());
    qToBigEndian<quint32>(record->id, frame.data());
    std::memcpy(frame.data() + 4, chunk.constData(), static_cast<size_t>(chunk.size()));

    if (!m_webrtc->sendDataChannelMessage(
            record->peerId, QString::fromLatin1(TransferChannel::DataId), frame, true)) {
        // 发送队列满时下一拍重试；文件位置要退回，否则这块数据会丢
        record->file->seek(record->transferred);
        if (!isConnected(record->peerId))
            failRecord(record, QStringLiteral("数据通道已断开"), false);
        return false;
    }

    record->hash->addData(chunk);
    record->transferred += chunk.size();
    *budget -= chunk.size();

    // 最后一块不在这里收尾：控制通道是无序的，complete 可能先于数据块
    // 到达。等接收端 ACK 确认所有字节后再由 handleAck 触发 finishSending。
    return true;
}

void FileTransferManager::finishSending(const TransferRecordPtr &record) {
    if (record->direction != TransferDirection::Send
        || record->state != TransferState::Transferring) {
        return;
    }
    // 前缀哈希还没重放完、或还有数据没被接收端确认时不能发 complete：
    // 控制通道是无序的，complete 可能先于数据块到达接收端。
    if (record->hash == nullptr || record->hashReplayRemaining > 0)
        return;
    if (record->transferred < record->totalBytes
        || record->peerReceived < record->totalBytes)
        return;

    if (record->file != nullptr) {
        record->file->close();
        record->file.reset();
    }

    record->state = TransferState::Verifying;
    record->lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    record->hashReplayRemaining = 0;

    record->finalHash = QString::fromLatin1(record->hash->result().toHex());
    QJsonObject complete = makeControl(QStringLiteral("complete"), record->id);
    complete.insert(QStringLiteral("sha256"), record->finalHash);
    record->hash.reset();

    if (!sendControl(record->peerId, complete)) {
        failRecord(record, QStringLiteral("发送校验信息失败"), false);
        return;
    }

    ensureModel(record->peerId)->updateRecord(record);
    LOG_INFO("Transfer") << "数据发送完毕，等待对方校验:" << record->fileName;
}

void FileTransferManager::verifyIncoming(const TransferRecordPtr &record,
                                         const QString &sha256) {
    record->state = TransferState::Verifying;
    record->lastProgressMs = QDateTime::currentMSecsSinceEpoch();

    if (record->file != nullptr) {
        record->file->close();
        record->file.reset();
    }

    const QByteArray expected = QByteArray::fromHex(sha256.toLatin1());
    const QByteArray actual = record->hash != nullptr ? record->hash->result() : QByteArray();
    record->hash.reset();

    const bool sizeOk = record->transferred == record->totalBytes;
    const bool hashOk = expected.size() == 32 && expected == actual;

    QJsonObject result = makeControl(QStringLiteral("result"), record->id);
    if (!sizeOk || !hashOk) {
        QFile::remove(record->partialPath);
        record->partialPath.clear();

        result.insert(QStringLiteral("ok"), false);
        sendControl(record->peerId, result);

        record->state = TransferState::Failed;
        record->error = sizeOk ? QStringLiteral("文件校验失败，传输中可能已损坏")
                               : QStringLiteral("接收到的字节数与声明不符");
        record->finishedAt = QDateTime::currentDateTime();
        ensureModel(record->peerId)->updateRecord(record);
        updatePending(record->peerId);
        LOG_WARN("Transfer") << "接收校验失败:" << record->fileName << record->error;
        return;
    }

    // 接收期间目标位置可能被别的程序占用，换一个不冲突的名字再落盘
    QString target = record->targetPath;
    if (QFileInfo::exists(target)) {
        target = uniquePath(QFileInfo(target).absolutePath(),
                            QFileInfo(target).fileName());
    }

    if (!QFile::rename(record->partialPath, target)) {
        QFile::remove(record->partialPath);
        record->partialPath.clear();

        result.insert(QStringLiteral("ok"), false);
        sendControl(record->peerId, result);

        record->state = TransferState::Failed;
        record->error = QStringLiteral("保存文件失败");
        record->finishedAt = QDateTime::currentDateTime();
        ensureModel(record->peerId)->updateRecord(record);
        updatePending(record->peerId);
        return;
    }

    record->targetPath = target;
    record->partialPath.clear();
    record->verified = true;
    record->speedBps = 0;
    record->state = TransferState::Completed;
    record->finishedAt = QDateTime::currentDateTime();

    result.insert(QStringLiteral("ok"), true);
    sendControl(record->peerId, result);

    ensureModel(record->peerId)->updateRecord(record);
    updatePending(record->peerId);

    LOG_INFO("Transfer") << "接收完成并校验通过:" << record->fileName << "→" << target;
}

void FileTransferManager::finishRecord(const TransferRecordPtr &record,
                                       TransferState state) {
    if (record->file != nullptr) {
        record->file->close();
        record->file.reset();
    }
    record->hash.reset();

    // 接收方取消/拒绝时清掉半截文件
    if (record->direction == TransferDirection::Receive
        && !record->partialPath.isEmpty()) {
        QFile::remove(record->partialPath);
        record->partialPath.clear();
    }

    record->speedBps = 0;
    record->state = state;
    record->finishedAt = QDateTime::currentDateTime();
    record->interruptedFrom = TransferState::Failed;
    record->interruptedAtMs = 0;
    record->hashReplayRemaining = 0;
    record->resumeRequested = false;
    record->resumeAllowsRewind = false;
    record->lastResumeMs = 0;
    record->lastProgressMs = 0;
    record->finalHash.clear();

    ensureModel(record->peerId)->updateRecord(record);
    updatePending(record->peerId);
}

void FileTransferManager::failRecord(const TransferRecordPtr &record,
                                     const QString &message, bool notifyPeer) {
    if (notifyPeer && record->state != TransferState::Completed
        && record->state != TransferState::Failed
        && record->state != TransferState::Cancelled
        && record->state != TransferState::Rejected) {
        sendControl(record->peerId, makeControl(QStringLiteral("cancel"), record->id));
    }

    if (record->file != nullptr) {
        record->file->close();
        record->file.reset();
    }
    record->hash.reset();

    if (record->direction == TransferDirection::Receive
        && !record->partialPath.isEmpty()) {
        QFile::remove(record->partialPath);
        record->partialPath.clear();
    }

    record->speedBps = 0;
    record->state = TransferState::Failed;
    record->error = message;
    record->finishedAt = QDateTime::currentDateTime();
    record->interruptedFrom = TransferState::Failed;
    record->interruptedAtMs = 0;
    record->hashReplayRemaining = 0;
    record->resumeRequested = false;
    record->resumeAllowsRewind = false;
    record->lastResumeMs = 0;
    record->lastProgressMs = 0;
    record->finalHash.clear();

    ensureModel(record->peerId)->updateRecord(record);
    updatePending(record->peerId);

    LOG_WARN("Transfer") << "传输失败:" << record->fileName << message;
}

void FileTransferManager::onNotifyTimer() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool active = false;
    // 本拍仍有传输（Transferring / Interrupted）的对端，用于低频 GetStats。
    QSet<QString> activePeers;

    for (FileTransferModel *model : std::as_const(m_models)) {
        const QList<TransferRecordPtr> records = model->records();
        for (const TransferRecordPtr &record : records) {
            if (record->state == TransferState::Interrupted) {
                active = true;
                activePeers.insert(record->peerId);

                // 发送端每 5 秒主动请求一次续传；接收端等发送端的 resume
                if (record->direction == TransferDirection::Send
                    && record->interruptedFrom == TransferState::Transferring
                    && peerStillInRoom(record->peerId)
                    && isConnected(record->peerId)
                    && now - record->lastResumeMs >= kResumeRetryMs) {
                    attemptResume(record);
                }

                // 通道一直没恢复（或 resume 一直没得到回应）：放弃并清理
                if (record->interruptedAtMs > 0
                    && now - record->interruptedAtMs >= kInterruptedGraceMs) {
                    failRecord(record,
                               QStringLiteral("连接中断时间过长，传输已取消"), false);
                }
                continue;
            }

            if (record->state != TransferState::Transferring)
                continue;

            active = true;
            activePeers.insert(record->peerId);

            // 兜底：任何路径漏了初始化时，从本拍开始计时，避免永远不触发停滞
            if (record->lastProgressMs == 0)
                record->lastProgressMs = now;

            // 15 秒没有任何 ACK / 数据块：转入 Interrupted 并尝试续传
            if (now - record->lastProgressMs >= kDataStallMs) {
                if (record->direction == TransferDirection::Send) {
                    const quint64 buffered =
                        m_webrtc != nullptr
                            ? m_webrtc->dataChannelBufferedAmount(
                                  record->peerId,
                                  QString::fromLatin1(TransferChannel::DataId))
                            : 0;
                    LOG_WARN("Transfer") << "传输停滞:" << record->fileName
                                         << "已传" << record->transferred
                                         << "已确认" << record->peerReceived
                                         << "速度" << record->speedBps
                                         << "缓冲" << buffered;
                } else {
                    LOG_WARN("Transfer") << "传输停滞:" << record->fileName
                                         << "已写" << record->transferred
                                         << "速度" << record->speedBps;
                }
                markInterrupted(record);
                if (record->direction == TransferDirection::Send)
                    attemptResume(record);
                continue;
            }

            if (record->lastSampleMs == 0) {
                record->lastSampleMs = now;
                record->lastSampleBytes = record->transferred;
            } else {
                const qint64 elapsed = now - record->lastSampleMs;
                if (elapsed >= 400) {
                    record->speedBps =
                        (record->transferred - record->lastSampleBytes) * 1000 / elapsed;
                    record->lastSampleBytes = record->transferred;
                    record->lastSampleMs = now;
                }
            }

            // 每 10 秒一条进度日志；发送端同时打印 ACK 和发送缓冲。
            // 接收端的 peerReceived/buffered 没有意义，只打印已写字节和速度。
            if (now - record->lastLogMs >= kLogIntervalMs) {
                record->lastLogMs = now;
                if (record->direction == TransferDirection::Send) {
                    const quint64 buffered =
                        m_webrtc != nullptr
                            ? m_webrtc->dataChannelBufferedAmount(
                                  record->peerId,
                                  QString::fromLatin1(TransferChannel::DataId))
                            : 0;
                    LOG_DEBUG("Transfer") << "进度" << record->id << record->fileName
                                          << record->transferred << "/" << record->totalBytes
                                          << "ACK" << record->peerReceived
                                          << "速度" << record->speedBps
                                          << "缓冲" << buffered;
                } else {
                    LOG_DEBUG("Transfer") << "进度" << record->id << record->fileName
                                          << record->transferred << "/" << record->totalBytes
                                          << "速度" << record->speedBps;
                }
            }

            model->updateRecord(record);
        }
    }

    // 只有传输期间才采 GetStats；开始新传输时立即采一次，之后每 15 秒一次。
    // 回调在 WebRTC 线程执行，只写一行日志，不阻塞发送/接收。
    if (activePeers != m_statsPeers) {
        m_statsPeers = activePeers;
        m_lastStatsMs = 0;
    }
    if (!activePeers.isEmpty()
        && (m_lastStatsMs == 0 || now - m_lastStatsMs >= kStatsIntervalMs)) {
        m_lastStatsMs = now;
        if (m_webrtc != nullptr) {
            for (const QString &peerId : std::as_const(activePeers))
                m_webrtc->requestDataChannelStats(peerId);
        }
    }

    if (!active)
        m_notifyTimer.stop();
}

// ============================
// 可靠性与恢复
// ============================

void FileTransferManager::sendAck(const TransferRecordPtr &record) {
    QJsonObject ack = makeControl(QStringLiteral("ack"), record->id);
    ack.insert(QStringLiteral("received"), static_cast<double>(record->transferred));
    if (!sendControl(record->peerId, ack))
        return;

    record->lastAckBytes = record->transferred;
    record->lastAckMs = QDateTime::currentMSecsSinceEpoch();
}

void FileTransferManager::markInterrupted(const TransferRecordPtr &record) {
    if (!isActiveTransferState(record->state)
        || record->state == TransferState::Interrupted) {
        return;
    }

    record->interruptedFrom = record->state;
    record->state = TransferState::Interrupted;
    record->interruptedAtMs = QDateTime::currentMSecsSinceEpoch();
    record->resumeRequested = false;
    record->lastResumeMs = 0;
    // **不要清 hashReplayRemaining**：发送端可能正在重算前缀哈希，
    // resume 时靠它判断哈希是否已与 transferred 对齐
    record->speedBps = 0;
    record->lastSampleMs = 0;
    record->lastSampleBytes = 0;

    ensureModel(record->peerId)->updateRecord(record);
    LOG_WARN("Transfer") << "传输中断，等待恢复:" << record->fileName;
}

bool FileTransferManager::resumeFromReceived(const TransferRecordPtr &record,
                                              qint64 received, bool allowRewind) {
    if (record->direction != TransferDirection::Send)
        return false;

    if (record->file == nullptr) {
        auto file = std::make_shared<QFile>(record->sourcePath);
        if (!file->open(QIODevice::ReadOnly)) {
            failRecord(record,
                       QStringLiteral("无法读取源文件: %1").arg(file->errorString()),
                       true);
            return false;
        }
        record->file = file;
    }
    if (record->file->size() != record->totalBytes) {
        failRecord(record, QStringLiteral("源文件在传输期间发生变化，请重新发送"), true);
        return false;
    }
    if (received < 0 || received > record->totalBytes
        || received > record->transferred) {
        failRecord(record, QStringLiteral("对方返回的续传偏移非法"), true);
        return false;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 哈希状态正好覆盖 transferred 时直接接着发；否则从 0 重算前缀。
    // 重放由发送节拍逐块完成，避免一次性读完整份大文件卡住界面。
    const bool hashAligned = record->hash != nullptr
                             && record->hashReplayRemaining == 0
                             && received == record->transferred;
    if (hashAligned) {
        if (!record->file->seek(received)) {
            failRecord(record, QStringLiteral("无法定位源文件"), true);
            return false;
        }
        record->peerReceived = received;
    } else if (!allowRewind) {
        // 通道没有重建：SCTP 可能还在重传之前的块，此时回退重发会
        // 与那些块重复，直接把接收端文件写坏。只允许从 transferred
        // 继续发，让窗口等待 ACK 追上。
        if (record->hash == nullptr) {
            LOG_DEBUG("Transfer") << "通道未重建且哈希缺失，无法继续:"
                                   << record->fileName;
            return false;
        }
        if (received != record->transferred) {
            if (record->hashReplayRemaining > 0) {
                // 接收端尚未收全已发送字节，而前缀哈希也只覆盖了一部分：
                // 回退会重复，继续重放又对不上接收端的偏移，只能等通道重建。
                LOG_DEBUG("Transfer") << "通道未重建，暂不能重放哈希:"
                                       << record->fileName;
                return false;
            }
            if (!record->file->seek(record->transferred)) {
                failRecord(record, QStringLiteral("无法定位源文件"), true);
                return false;
            }
        }
        // received == transferred 时保持文件当前位置：前缀重放若尚未
        // 完成就从原位置继续，重放结束后会自动 seek 到 transferred。
        record->peerReceived = received;
    } else {
        // 新通道：旧数据不会再来，可以安全回退并重算前缀哈希
        record->hash =
            std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
        record->transferred = received;
        record->peerReceived = received;
        record->hashReplayRemaining = received;
        if (!record->file->seek(0)) {
            failRecord(record, QStringLiteral("无法定位源文件"), true);
            return false;
        }
    }

    record->resumeRequested = false;
    record->resumeAllowsRewind = false;
    record->lastResumeMs = 0;
    record->finalHash.clear();
    record->interruptedFrom = TransferState::Failed;
    record->interruptedAtMs = 0;
    record->state = TransferState::Transferring;
    record->speedBps = 0;
    record->lastSampleMs = now;
    record->lastSampleBytes = record->transferred;
    record->lastProgressMs = now;
    record->lastAckBytes = received;
    record->lastAckMs = now;
    record->lastLogMs = 0;
    ensureModel(record->peerId)->updateRecord(record);

    if (record->hashReplayRemaining == 0
        && record->transferred >= record->totalBytes
        && record->peerReceived >= record->totalBytes) {
        finishSending(record);
    } else {
        startPumping();
    }
    return true;
}

void FileTransferManager::attemptResume(const TransferRecordPtr &record) {
    if (record->direction != TransferDirection::Send
        || record->state != TransferState::Interrupted
        || record->interruptedFrom != TransferState::Transferring) {
        return;
    }
    if (!isConnected(record->peerId))
        return;

    record->lastResumeMs = QDateTime::currentMSecsSinceEpoch();
    record->resumeAllowsRewind = false;
    if (!sendControl(record->peerId, makeControl(QStringLiteral("resume"), record->id))) {
        LOG_DEBUG("Transfer") << "续传请求发送失败，稍后重试:" << record->fileName;
        return;
    }

    record->resumeRequested = true;
    LOG_INFO("Transfer") << "请求续传:" << record->fileName
                         << "offset=" << record->transferred;
}

void FileTransferManager::handleChannelDown(const QString &peerId,
                                            const QString &reason) {
    LOG_WARN("Transfer") << "与" << peerId << "的文件传输中断:" << reason;

    FileTransferModel *model = modelFor(peerId);
    if (model != nullptr) {
        const QList<TransferRecordPtr> records = model->records();
        for (const TransferRecordPtr &record : records) {
            if (!isActiveTransferState(record->state))
                continue;

            // 对端真的离开房间：不能续传，直接失败并清理
            if (!peerStillInRoom(peerId)) {
                failRecord(record, QStringLiteral("对方已离开房间"), false);
                continue;
            }

            // 只是连接断了：保留 .part / QFile / hash / 已传字节数，
            // 等通道恢复后按断点前的状态重发控制消息。
            markInterrupted(record);
        }
    }

    m_sendPaused.remove(peerId);
    m_pendingControl.remove(peerId);
    m_lastPongMs.remove(peerId);

    // 通知节拍继续跑：既负责速度显示，也负责断线超时兜底
    if (!m_notifyTimer.isActive())
        m_notifyTimer.start();

    emit connectionChanged(peerId);
}

void FileTransferManager::noteChannelAlive(const QString &peerId) {
    m_lastPongMs.insert(peerId, QDateTime::currentMSecsSinceEpoch());

    if (m_unresponsive.remove(peerId)) {
        LOG_INFO("Transfer") << "与" << peerId << "的控制通道恢复响应";
        emit connectionChanged(peerId);
        reissueAfterReconnect(peerId, /*allowRewind=*/false);
    }
}

void FileTransferManager::onHealthTimer() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool anyReady = false;

    // 迭代副本：handleChannelDown 会发 connectionChanged，QML 处理期间
    // 理论上可能触碰对端状态（例如关闭传输窗口）。
    const QSet<QString> peers = m_controlOpen;
    for (const QString &peerId : peers) {
        if (!m_dataOpen.contains(peerId))
            continue;
        anyReady = true;

        if (m_unresponsive.contains(peerId)) {
            // 继续探测：通道恢复后，第一个 pong/控制消息会把状态带回来
            sendControl(peerId, makeControl(QStringLiteral("ping"), 0));
            continue;
        }

        const qint64 last = m_lastPongMs.value(peerId, 0);
        if (last > 0 && now - last >= kHealthTimeoutMs) {
            LOG_WARN("Transfer") << "与" << peerId << "的控制通道"
                                 << (kHealthTimeoutMs / 1000) << "秒无响应";
            m_unresponsive.insert(peerId);
            handleChannelDown(peerId, QStringLiteral("数据通道无响应"));
            continue;
        }

        sendControl(peerId, makeControl(QStringLiteral("ping"), 0));
    }

    if (!anyReady)
        m_healthTimer.stop();
}

// ============================
// 辅助
// ============================

void FileTransferManager::updatePending(const QString &peerId) {
    if (m_session == nullptr || m_session->members() == nullptr)
        return;

    FileTransferModel *model = modelFor(peerId);
    const int pending = model != nullptr ? model->pendingIncoming() : 0;
    m_session->members()->setPendingFiles(peerId, pending);
}

QString FileTransferManager::nicknameFor(const QString &peerId) const {
    if (m_session != nullptr && m_session->members() != nullptr) {
        for (const Member &member : m_session->members()->members()) {
            if (member.clientId == peerId)
                return member.nickname;
        }
    }
    return peerId.left(8);
}

bool FileTransferManager::peerStillInRoom(const QString &peerId) const {
    if (m_session == nullptr || m_session->members() == nullptr)
        return false;
    return m_session->members()->contains(peerId);
}

void FileTransferManager::reissueAfterReconnect(const QString &peerId,
                                                bool allowRewind) {
    FileTransferModel *model = modelFor(peerId);
    if (model == nullptr)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<TransferRecordPtr> records = model->records();
    for (const TransferRecordPtr &record : records) {
        if (record->state != TransferState::Interrupted)
            continue;
        // 接收端只等发送端的 offer/resume/complete
        if (record->direction != TransferDirection::Send)
            continue;

        // 超时从这次重发重新计时；对端没回应的话由通知节拍兜底。
        // 之前的 resume 可能在断线时丢了，这里统一允许重发。
        record->interruptedAtMs = now;
        record->resumeRequested = false;
        record->resumeAllowsRewind = false;
        record->lastResumeMs = 0;

        switch (record->interruptedFrom) {
        case TransferState::WaitingAccept: {
            QJsonObject offer = makeControl(QStringLiteral("offer"), record->id);
            offer.insert(QStringLiteral("name"), record->fileName);
            offer.insert(QStringLiteral("size"),
                         static_cast<double>(record->totalBytes));
            sendControl(peerId, offer);
            break;
        }
        case TransferState::Transferring: {
            record->resumeAllowsRewind = allowRewind;
            if (sendControl(peerId, makeControl(QStringLiteral("resume"), record->id))) {
                record->resumeRequested = true;
                record->lastResumeMs = now;
                LOG_INFO("Transfer") << "请求断点续传:" << record->fileName
                                     << (allowRewind ? "(通道已重建)"
                                                     : "(通道未重建，仅继续发送)");
            }
            break;
        }
        case TransferState::Verifying: {
            if (record->finalHash.isEmpty())
                break;
            QJsonObject complete = makeControl(QStringLiteral("complete"), record->id);
            complete.insert(QStringLiteral("sha256"), record->finalHash);
            sendControl(peerId, complete);
            break;
        }
        default:
            break;
        }
    }
}

QString FileTransferManager::downloadDirectory() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QDir::homePath();
    return base + QStringLiteral("/Syncine");
}

QString FileTransferManager::sanitizeFileName(const QString &name) {
    // 对方给的名字不可信：只取最后一段，去掉路径分隔符与控制字符
    QString result = QFileInfo(name).fileName();
    result.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1F]")),
                   QStringLiteral("_"));
    result = result.trimmed();

    if (result.isEmpty() || result == QLatin1String(".")
        || result == QLatin1String("..")) {
        result = QStringLiteral("received-file");
    }
    if (result.size() > 180)
        result = result.left(180);
    return result;
}

QString FileTransferManager::uniquePath(const QString &directory, const QString &name) {
    const QFileInfo info(name);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();

    QString candidate = QDir(directory).filePath(name);
    int index = 1;
    while (QFileInfo::exists(candidate) || QFileInfo::exists(candidate + QStringLiteral(".part"))) {
        candidate = QDir(directory).filePath(
            base + QStringLiteral(" (%1)").arg(index++)
            + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix));
    }
    return candidate;
}
