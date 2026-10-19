#pragma once
#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace sd {
// Optional MediaInfo.dll, loaded exclusively from the executable's directory.
// Buffer only: consumes at most 4 MiB and never opens/seeks a media file itself.
QJsonArray enrichAudioProbe(const QByteArray &head, qint64 fileSize, const QJsonArray &tracks);
// Pure merge for MediaInfo JSON reports. Audio streams must have a matching TS PID;
// stream ordinal, MenuID, titles and filenames are never used to guess a match.
QJsonArray mergeAudioProbe(const QJsonObject &report, const QJsonArray &tracks);
}
