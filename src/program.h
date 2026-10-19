#pragma once
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>

namespace sd {
struct ProgramResult {
    bool active = false;
    QJsonObject program;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

struct ProgramPlan {
    bool active = false;
    QJsonObject program;
    QJsonObject config;
    QString episodeId;
    QString nextEpisodeId;
    double start = 0; // Absolute MPLS time, including for external subtitles.
    double end = 0;
    double duration = 0;
    double titleDuration = 0;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Undefined, null, or an empty string means legacy configuration (inactive).
// Active values are a version-1 object or its JSON string representation.
ProgramResult normalizeProgram(const QJsonValue &payload, const QJsonObject &metadata);
ProgramResult createDefaultProgram(const QJsonObject &metadata);
ProgramPlan resolveProgram(const QJsonObject &sourceConfig, const QJsonObject &metadata,
                           const QString &requestedEpisodeId = {});
// Automatic end-of-episode transition; obeys playMode and loop.
QString nextProgramEpisode(const QJsonObject &program, const QString &currentEpisodeId);
// Manual navigation; ignores playMode, obeys enabled order and loop. Direction is -1 or 1.
QString adjacentProgramEpisode(const QJsonObject &program, const QString &currentEpisodeId,
                              int direction);
}
