#include "MediaHash.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>

#include "core/Log.h"

namespace {
constexpr qint64 kPrefixBytes = 512 * 1024;
}

QString MediaHash::forFile(const QString &path) {
    if (path.isEmpty())
        return {};

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // 调用方会把空哈希当作「未匹配」，此处记录原因以便定位
        LOG_WARN("Hash") << "文件不可读，按未匹配处理:" << path << file.errorString();
        return {};
    }

    QByteArray data = file.read(kPrefixBytes);
    // 文件大小一并参与：开头相同但长度不同的两个文件不是同一部片子
    data.append(QByteArray::number(file.size()));

    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString MediaHash::forText(const QString &text) {
    if (text.isEmpty())
        return {};

    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}
