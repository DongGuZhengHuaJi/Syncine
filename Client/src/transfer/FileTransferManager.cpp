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

#include "core/Log.h"
#include "core/RoomTypes.h"
#include "session/MemberModel.h"
#include "session/RoomSession.h"
#include "webrtc/WebrtcManager.h"

namespace {

constexpr int kChunkSize = 64 * 1024;             // 单个二进制块（SCTP 上限 256KB）
constexpr qint64 kSendBudgetPerTick = 1024 * 1024; // 每拍最多发送 1MB
constexpr int kPumpIntervalMs = 10;
constexpr int kNotifyIntervalMs = 200;

// DataChannel 内部的 SCTP 发送缓冲水位。达到高水位就停止塞数据，
// 等排水到低水位再继续。SCTP 自己负责拥塞控制，这里只是避免把
// 几 MB 的数据一次性压进发送队列，导致内存上涨和延迟失控。
constexpr qint64 kSendBufferHighWater = 4 * 1024 * 1024;
constexpr qint64 kSendBufferLowWater = 1024 * 1024;
// 断线后等待 P2P 恢复/续传的最长时间
constexpr qint64 kInterruptedGraceMs = 60 * 1000;

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
}

FileTransferManager::~FileTransferManager() {
    m_pumpTimer.stop();
    m_notifyTimer.stop();
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
    return m_openChannels.contains(peerId)
           || (m_webrtc != nullptr && m_webrtc->isDataChannelOpen(peerId));
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
    record->hashReplayRemaining = 0;
    record->resumeRequested = false;
    record->finalHash.clear();
    record->interruptedFrom = TransferState::Failed;
    record->interruptedAtMs = 0;
    record->speedBps = 0;
    record->lastSampleBytes = 0;
    record->lastSampleMs = 0;
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

void FileTransferManager::onDataChannelOpened(const QString &peerId) {
    LOG_INFO("Transfer") << "与" << peerId << "的数据通道已就绪";
    m_openChannels.insert(peerId);
    m_sendPaused.remove(peerId);
    if (m_pendingControl.contains(peerId) && !m_pumpTimer.isActive())
        m_pumpTimer.start();
    emit connectionChanged(peerId);

    // 通道重建后，把中断的传输重新挂上
    reissueAfterReconnect(peerId);
}

void FileTransferManager::onDataChannelClosed(const QString &peerId) {
    const bool wasOpen = m_openChannels.remove(peerId) > 0;
    m_pendingControl.remove(peerId);
    m_sendPaused.remove(peerId);
    if (!wasOpen)
        return;

    LOG_WARN("Transfer") << "与" << peerId << "的数据通道已关闭";

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
            // 等数据通道恢复后按断点前的状态重发控制消息。
            record->interruptedFrom = record->state;
            record->state = TransferState::Interrupted;
            record->interruptedAtMs = QDateTime::currentMSecsSinceEpoch();
            record->resumeRequested = false;
            record->hashReplayRemaining = 0;
            record->speedBps = 0;
            record->lastSampleMs = 0;
            record->lastSampleBytes = 0;
            model->updateRecord(record);
        }
    }

    // 通知节拍继续跑：既负责速度显示，也负责断线超时兜底
    if (!m_notifyTimer.isActive())
        m_notifyTimer.start();
    emit connectionChanged(peerId);
}

void FileTransferManager::onPeerRemoved(const QString &peerId) {
    m_openChannels.remove(peerId);
    m_pendingControl.remove(peerId);
    m_sendPaused.remove(peerId);

    FileTransferModel *model = modelFor(peerId);
    if (model != nullptr) {
        const QList<TransferRecordPtr> records = model->records();
        for (const TransferRecordPtr &record : records) {
            if (isActiveTransferState(record->state)) {
                failRecord(record, QStringLiteral("对方已离开房间"), false);
            }
        }
    }

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

    m_openChannels.clear();
    m_pendingControl.clear();
    m_sendPaused.clear();
    m_pumpTimer.stop();
    m_notifyTimer.stop();
}

void FileTransferManager::onDataChannelMessage(const QString &peerId, bool binary,
                                               const QByteArray &data) {
    if (binary) {
        if (data.size() <= 4)
            return;

        const quint32 id = qFromBigEndian<quint32>(data.constData());
        const TransferRecordPtr record = findIncoming(peerId, id);
        if (record == nullptr || record->state != TransferState::Transferring)
            return;

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

        if (!m_notifyTimer.isActive())
            m_notifyTimer.start();
        return;
    }

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
    } else if (type == QLatin1String("resume")) {
        handleResume(peerId, message);
    } else if (type == QLatin1String("resume_ack")) {
        handleResumeAck(peerId, message);
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

    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (m_webrtc->sendDataChannelMessage(peerId, payload, false))
        return true;

    // 发送队列满（大文件正在传）时先存着，由发送节拍重试。
    // 取消、校验这类消息丢了会让对端一直等，不能当作失败丢弃。
    if (!m_webrtc->isDataChannelOpen(peerId)) {
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
        // 已经有一条同 id 的接收记录。若它停在“已接受、但 accept 消息
        // 在断线中丢了”的状态，重发 accept，让发送方从 0 开始。
        const TransferRecordPtr existing = findIncoming(peerId, id);
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
    record->hashReplayRemaining = 0;
    record->resumeRequested = false;
    record->finalHash.clear();
    record->interruptedFrom = TransferState::Failed;
    record->interruptedAtMs = 0;
    record->speedBps = 0;
    record->lastSampleBytes = 0;
    record->lastSampleMs = 0;
    record->state = TransferState::Transferring;

    ensureModel(peerId)->updateRecord(record);
    startPumping();

    LOG_INFO("Transfer") << "对方已接收，开始发送:" << record->fileName;
}

void FileTransferManager::handleReject(const QString &peerId, const QJsonObject &message) {
    const TransferRecordPtr record = findOutgoing(peerId, idOf(message));
    if (record == nullptr || record->state != TransferState::WaitingAccept)
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

void FileTransferManager::handleResume(const QString &peerId, const QJsonObject &message) {
    const TransferRecordPtr record = findIncoming(peerId, idOf(message));
    if (record == nullptr)
        return;
    if (record->state != TransferState::Interrupted
        || record->interruptedFrom != TransferState::Transferring) {
        return;
    }

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
    record->file->seek(size);

    record->state = TransferState::Transferring;
    record->interruptedFrom = TransferState::Failed;
    record->interruptedAtMs = 0;
    record->resumeRequested = false;
    record->speedBps = 0;
    record->lastSampleMs = 0;
    record->lastSampleBytes = 0;
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

    if (record->file == nullptr) {
        auto file = std::make_shared<QFile>(record->sourcePath);
        if (!file->open(QIODevice::ReadOnly)) {
            failRecord(record,
                       QStringLiteral("无法读取源文件: %1").arg(file->errorString()),
                       true);
            return;
        }
        record->file = file;
    }
    if (record->file->size() != record->totalBytes) {
        failRecord(record, QStringLiteral("源文件在传输期间发生变化，请重新发送"), true);
        return;
    }
    if (!record->file->seek(0)) {
        failRecord(record, QStringLiteral("无法定位源文件"), true);
        return;
    }

    // 从 0 开始重算前缀哈希；hashReplayRemaining 由发送节拍逐块消化
    record->hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    record->transferred = received;
    record->hashReplayRemaining = received;
    record->resumeRequested = false;
    record->finalHash.clear();
    record->interruptedFrom = TransferState::Failed;
    record->interruptedAtMs = 0;
    record->state = TransferState::Transferring;
    record->speedBps = 0;
    record->lastSampleMs = 0;
    record->lastSampleBytes = 0;
    ensureModel(peerId)->updateRecord(record);
    startPumping();

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
    bool active = false;

    // 数据通道事件里拿不到 peers 列表，控制消息队列按 model 的 key 遍历
    for (auto it = m_models.cbegin(); it != m_models.cend(); ++it) {
        const QString &peerId = it.key();

        // 先补发积压的控制消息；仍发不出去就留到下一拍。
        // 不依赖 m_openChannels：即使 open 信号还没处理，Send 自己会判断
        auto pending = m_pendingControl.find(peerId);
        if (pending != m_pendingControl.end()) {
            QVector<QByteArray> &messages = pending.value();
            while (!messages.isEmpty()) {
                if (!m_webrtc->sendDataChannelMessage(peerId, messages.first(), false))
                    break;
                messages.removeFirst();
            }
            if (messages.isEmpty()) {
                m_pendingControl.erase(pending);
            } else {
                active = true;
            }
        }

        if (!m_openChannels.contains(peerId))
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

        const qint64 buffered =
            static_cast<qint64>(m_webrtc->dataChannelBufferedAmount(peerId));
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
    // SHA-256 状态和接收端的 .part 文件对齐。按块做，避免卡住界面。
    if (record->hashReplayRemaining > 0) {
        const qint64 want = qMin<qint64>(kChunkSize,
                                         qMin(record->hashReplayRemaining, *budget));
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
        *budget -= prefix.size();

        if (record->hashReplayRemaining == 0) {
            record->file->seek(record->transferred);
            if (record->transferred >= record->totalBytes)
                finishSending(record);
        }
        return true;
    }

    const qint64 remaining = record->totalBytes - record->transferred;
    if (remaining <= 0) {
        finishSending(record);
        return false;
    }

    const qint64 want = qMin<qint64>(kChunkSize, qMin(remaining, *budget));
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

    if (!m_webrtc->sendDataChannelMessage(record->peerId, frame, true)) {
        // 发送队列满时下一拍重试；文件位置要退回，否则这块数据会丢
        record->file->seek(record->transferred);
        if (!m_webrtc->isDataChannelOpen(record->peerId))
            failRecord(record, QStringLiteral("数据通道已断开"), false);
        return false;
    }

    record->hash->addData(chunk);
    record->transferred += chunk.size();
    *budget -= chunk.size();

    if (record->transferred >= record->totalBytes)
        finishSending(record);
    return true;
}

void FileTransferManager::finishSending(const TransferRecordPtr &record) {
    if (record->file != nullptr) {
        record->file->close();
        record->file.reset();
    }

    record->state = TransferState::Verifying;

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
    record->finalHash.clear();

    ensureModel(record->peerId)->updateRecord(record);
    updatePending(record->peerId);

    LOG_WARN("Transfer") << "传输失败:" << record->fileName << message;
}

void FileTransferManager::onNotifyTimer() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool active = false;

    for (FileTransferModel *model : std::as_const(m_models)) {
        const QList<TransferRecordPtr> records = model->records();
        for (const TransferRecordPtr &record : records) {
            if (record->state == TransferState::Interrupted) {
                active = true;
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

            model->updateRecord(record);
        }
    }

    if (!active)
        m_notifyTimer.stop();
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

void FileTransferManager::reissueAfterReconnect(const QString &peerId) {
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

        // 超时从这次重发重新计时；对端没回应的话由通知节拍兜底
        record->interruptedAtMs = now;

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
            if (record->resumeRequested)
                break;
            if (sendControl(peerId, makeControl(QStringLiteral("resume"), record->id))) {
                record->resumeRequested = true;
                LOG_INFO("Transfer") << "请求断点续传:" << record->fileName;
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
