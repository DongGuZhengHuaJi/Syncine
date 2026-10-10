#include "MediaHash.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>

namespace {
constexpr qint64 kPrefixBytes = 512 * 1024;
}

QString MediaHash::forFile(const QString &path) {
    if (path.isEmpty())
        return {};

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QByteArray data = file.read(kPrefixBytes);
    // 文件大小一起参与:开头相同但长度不同的两个文件不是同一部片子
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
