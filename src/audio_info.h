#pragma once
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace sd {
// Inputs: index/pid/language/coding plus libbluray format/rate codes.
// Reads at most 4 MiB of a TS/M2TS file, without decoding or external tools.
// Unknown measurements are omitted. Original fields are retained.
QJsonArray enrichAudioTracks(const QString &clipPath, const QJsonArray &audio);
QString audioTrackLabel(const QJsonObject &track);
}
