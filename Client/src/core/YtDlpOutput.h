//
// yt-dlp 混合输出的解析工具。
//
// yt-dlp 的 stdout/stderr 被合并后，JSON 前后可能夹着 WARNING/ERROR 行；
// 警告文本本身也可能包含 '{'。两个调用方（UrlInspector / UrlDownloader）
// 之前各写了一份“从第一个 '{' 开始解析”和“挑最后一个 ERROR 行”，
// 这里统一，避免行为分叉。
//

#ifndef SYNCINE_YTDLPOUTPUT_H
#define SYNCINE_YTDLPOUTPUT_H

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QString>
#include <QStringList>

namespace YtDlpOutput {

// 从输出中提取 JSON 对象。依次尝试：
//   1. 从第一个 '{' 到结尾（正常情况）；
//   2. 从后往前找以 '{' 开头的行（警告里带 '{'、或 JSON 前有噪声）。
// 失败返回空对象。
inline QJsonObject parseJsonObject(const QByteArray &output)
{
    QJsonParseError error{};

    const int firstBrace = output.indexOf('{');
    if (firstBrace >= 0) {
        const QJsonDocument document =
            QJsonDocument::fromJson(output.mid(firstBrace), &error);
        if (document.isObject())
            return document.object();
    }

    const QList<QByteArray> lines = output.split('\n');
    for (int i = lines.size() - 1; i >= 0; --i) {
        if (!lines.at(i).trimmed().startsWith('{'))
            continue;

        QByteArray candidate;
        for (int j = i; j < lines.size(); ++j) {
            if (!candidate.isEmpty())
                candidate.append('\n');
            candidate.append(lines.at(j));
        }
        const QJsonDocument document = QJsonDocument::fromJson(candidate, &error);
        if (document.isObject())
            return document.object();
    }
    return {};
}

// 挑最后一行 "ERROR:" 作为失败原因；没有就返回整段输出（最多由调用方截断）。
inline QString lastError(const QStringList &output, const QString &fallback)
{
    for (int i = output.size() - 1; i >= 0; --i) {
        const QString line = output.at(i).trimmed();
        if (line.startsWith(QLatin1String("ERROR:")))
            return line;
    }
    return output.isEmpty() ? fallback : output.join(QLatin1Char('\n'));
}

} // namespace YtDlpOutput

#endif // SYNCINE_YTDLPOUTPUT_H
