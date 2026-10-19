#include "program.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QMap>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <cmath>

namespace sd {
namespace {
struct Title { double duration = 0; QList<int> pgs; };
using Titles = QMap<int, Title>;

bool integer(const QJsonValue &value, int minimum, int maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) &&
           value.toDouble() >= minimum && value.toDouble() <= maximum &&
           std::floor(value.toDouble()) == value.toDouble();
}
bool finiteNumber(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble());
}
QString readTitles(const QJsonObject &metadata, Titles &titles) {
    if (!metadata.value("titles").isArray()) return "metadata.titles must be an array";
    for (const auto &value : metadata.value("titles").toArray()) {
        const auto title = value.toObject();
        if (!value.isObject() || !integer(title.value("playlist"), 0, 99999))
            return "metadata title has an invalid playlist";
        const int playlist = title.value("playlist").toInt();
        if (titles.contains(playlist)) return "metadata has duplicate playlists";
        if (!finiteNumber(title.value("duration")) || title.value("duration").toDouble() <= 0)
            return "metadata title duration must be positive";
        Title entry; entry.duration = title.value("duration").toDouble();
        for (const auto &subtitleValue : title.value("subtitles").toArray()) {
            const auto subtitle = subtitleValue.toObject();
            if (subtitle.value("type").toString().compare("PGS", Qt::CaseInsensitive) == 0 &&
                integer(subtitle.value("index"), 0, 65535)) {
                const int track = subtitle.value("index").toInt();
                if (!entry.pgs.contains(track)) entry.pgs.append(track);
            }
        }
        titles.insert(playlist, entry);
    }
    return titles.isEmpty() ? QString("metadata has no playable MPLS titles") : QString();
}
QJsonObject defaultSubtitle(int playlist, const Title &title) {
    return {{"playlist", playlist}, {"mode", title.pgs.isEmpty() ? "off" : "internal"},
            {"track", title.pgs.isEmpty() ? -1 : title.pgs.first()}, {"file", ""},
            {"offset", 0}, {"fontsDir", ""}};
}
QStringList enabledIds(const QJsonObject &program) {
    QStringList ids;
    for (const auto &value : program.value("episodes").toArray()) {
        const auto episode = value.toObject();
        if (episode.value("enabled").toBool()) ids.append(episode.value("id").toString());
    }
    return ids;
}
}

ProgramResult normalizeProgram(const QJsonValue &payload, const QJsonObject &metadata) {
    ProgramResult result;
    if (payload.isUndefined() || payload.isNull() ||
        (payload.isString() && payload.toString().trimmed().isEmpty())) return result;
    result.active = true;
    auto fail = [&result](const QString &message) { result.error = message; return result; };
    QJsonObject source;
    if (payload.isObject()) source = payload.toObject();
    else if (payload.isString()) {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(payload.toString().toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            return fail("programJson must contain a JSON object");
        source = document.object();
    } else return fail("programJson must be an object or JSON string");
    if (!integer(source.value("version"), 1, 1)) return fail("program version must be 1");
    Titles titles;
    const auto titleError = readTitles(metadata, titles);
    if (!titleError.isEmpty()) return fail(titleError);
    if (!source.value("episodes").isArray() || source.value("episodes").toArray().isEmpty())
        return fail("episodes must be a nonempty array");
    QJsonArray episodes;
    QSet<QString> ids;
    QList<int> referencedPlaylists;
    QString firstEnabled;
    for (const auto &value : source.value("episodes").toArray()) {
        if (!value.isObject()) return fail("each episode must be an object");
        const auto episode = value.toObject();
        if (!episode.value("id").isString() || episode.value("id").toString().trimmed().isEmpty())
            return fail("episode id must be a nonempty string");
        const auto id = episode.value("id").toString().trimmed();
        if (ids.contains(id)) return fail("episode ids must be unique");
        ids.insert(id);
        if (episode.contains("label") && !episode.value("label").isString())
            return fail("episode label must be a string");
        if (episode.contains("enabled") && !episode.value("enabled").isBool())
            return fail("episode enabled must be boolean");
        const bool enabled = episode.value("enabled").toBool(true);
        if (enabled && firstEnabled.isEmpty()) firstEnabled = id;
        if (!integer(episode.value("playlist"), 0, 99999) ||
            !titles.contains(episode.value("playlist").toInt())) return fail("episode playlist is unavailable");
        const int playlist = episode.value("playlist").toInt();
        const auto startValue = episode.value("start").isUndefined() ? QJsonValue(0) : episode.value("start");
        const auto endValue = episode.value("end").isUndefined() ? QJsonValue(0) : episode.value("end");
        if (!finiteNumber(startValue) || !finiteNumber(endValue) ||
            startValue.toDouble() < 0 || endValue.toDouble() < 0) return fail("episode times must be finite nonnegative seconds");
        const double start = startValue.toDouble(), end = endValue.toDouble();
        const double resolvedEnd = end == 0 ? titles.value(playlist).duration : end;
        if (start >= resolvedEnd || resolvedEnd > titles.value(playlist).duration)
            return fail("episode range must be within its MPLS duration and have positive length");
        QString label = episode.value("label").toString().trimmed();
        if (label.isEmpty()) label = id;
        episodes.append(QJsonObject{{"id", id}, {"label", label}, {"enabled", enabled},
                                   {"playlist", playlist}, {"start", start}, {"end", end}});
        if (!referencedPlaylists.contains(playlist)) referencedPlaylists.append(playlist);
    }
    if (firstEnabled.isEmpty()) return fail("at least one episode must be enabled");
    if (source.contains("playMode") && !source.value("playMode").isString()) return fail("playMode must be a string");
    const auto mode = source.value("playMode").toString("single");
    if (mode != "single" && mode != "sequence") return fail("playMode must be single or sequence");
    if (source.contains("loop") && !source.value("loop").isBool()) return fail("loop must be boolean");
    if (source.contains("selectedEpisode") && !source.value("selectedEpisode").isString())
        return fail("selectedEpisode must be a string");
    auto selected = source.value("selectedEpisode").toString().trimmed();
    if (selected.isEmpty()) selected = firstEnabled;
    bool selectedEnabled = false;
    for (const auto &value : episodes) {
        const auto episode = value.toObject();
        if (episode.value("id").toString() == selected) selectedEnabled = episode.value("enabled").toBool();
    }
    if (!selectedEnabled) return fail("selectedEpisode must identify an enabled episode");
    if (source.contains("subtitles") && !source.value("subtitles").isArray()) return fail("subtitles must be an array");
    QJsonArray subtitles;
    QSet<int> subtitlePlaylists;
    for (const auto &value : source.value("subtitles").toArray()) {
        if (!value.isObject()) return fail("each subtitle setting must be an object");
        const auto subtitle = value.toObject();
        if (!integer(subtitle.value("playlist"), 0, 99999) || !titles.contains(subtitle.value("playlist").toInt()))
            return fail("subtitle playlist is unavailable");
        const int playlist = subtitle.value("playlist").toInt();
        if (subtitlePlaylists.contains(playlist)) return fail("subtitle settings must be unique per MPLS");
        subtitlePlaylists.insert(playlist);
        const auto subtitleMode = subtitle.value("mode").toString("internal");
        if ((subtitle.contains("mode") && !subtitle.value("mode").isString()) ||
            (subtitleMode != "internal" && subtitleMode != "external" && subtitleMode != "off"))
            return fail("subtitle mode must be internal, external or off");
        if (subtitle.contains("track") && !integer(subtitle.value("track"), -1, 65535)) return fail("subtitle track must be an integer >= -1");
        for (const auto &key : {"file", "fontsDir"})
            if (subtitle.contains(key) && !subtitle.value(key).isString()) return fail("subtitle file/fontsDir must be strings");
        const auto offset = subtitle.value("offset").isUndefined() ? QJsonValue(0) : subtitle.value("offset");
        if (!finiteNumber(offset)) return fail("subtitle offset must be finite seconds");
        auto normalized = defaultSubtitle(playlist, titles.value(playlist));
        normalized["mode"] = subtitleMode;
        if (subtitleMode == "internal") {
            int track = subtitle.value("track").toInt(-1);
            if (track == -1 && !titles.value(playlist).pgs.isEmpty()) track = titles.value(playlist).pgs.first();
            if (track != -1 && !titles.value(playlist).pgs.contains(track)) return fail("internal subtitle track is unavailable");
            normalized["track"] = track;
            normalized["mode"] = track == -1 ? "off" : "internal";
            normalized["offset"] = offset;
        } else if (subtitleMode == "external") {
            const auto file = subtitle.value("file").toString().trimmed();
            if (file.isEmpty()) return fail("external subtitle file must be nonempty");
            normalized["track"] = -1;
            normalized["file"] = file;
            normalized["offset"] = offset;
            normalized["fontsDir"] = subtitle.value("fontsDir").toString();
        } else normalized = QJsonObject{{"playlist", playlist}, {"mode", "off"}, {"track", -1}, {"file", ""}, {"offset", 0}, {"fontsDir", ""}};
        subtitles.append(normalized);
    }
    for (int playlist : referencedPlaylists)
        if (!subtitlePlaylists.contains(playlist)) subtitles.append(defaultSubtitle(playlist, titles.value(playlist)));
    result.program = {{"version", 1}, {"episodes", episodes}, {"subtitles", subtitles},
                      {"playMode", mode}, {"selectedEpisode", selected}, {"loop", source.value("loop").toBool(false)}};
    return result;
}

ProgramResult createDefaultProgram(const QJsonObject &metadata) {
    Titles titles;
    const auto error = readTitles(metadata, titles);
    if (!error.isEmpty()) return {true, {}, error};
    int playlist = metadata.value("suggestedPlaylist").toInt(-1);
    if (!titles.contains(playlist)) playlist = metadata.value("titles").toArray().first().toObject().value("playlist").toInt();
    return normalizeProgram(QJsonObject{{"version", 1}, {"episodes", QJsonArray{QJsonObject{
        {"id", "episode-1"}, {"label", QString("MPLS %1").arg(playlist, 5, 10, QLatin1Char('0'))},
        {"enabled", true}, {"playlist", playlist}, {"start", 0}, {"end", 0}}}}}, metadata);
}

QString adjacentProgramEpisode(const QJsonObject &program, const QString &currentEpisodeId, int direction) {
    if (direction != -1 && direction != 1) return {};
    const auto ids = enabledIds(program);
    const auto current = ids.indexOf(currentEpisodeId);
    if (current < 0) return {};
    auto next = current + direction;
    if (next < 0 || next >= ids.size()) {
        if (!program.value("loop").toBool()) return {};
        next = next < 0 ? ids.size() - 1 : 0;
    }
    return ids.at(next);
}
QString nextProgramEpisode(const QJsonObject &program, const QString &currentEpisodeId) {
    if (program.value("playMode").toString() == "single")
        return program.value("loop").toBool() && enabledIds(program).contains(currentEpisodeId) ? currentEpisodeId : QString();
    return adjacentProgramEpisode(program, currentEpisodeId, 1);
}

ProgramPlan resolveProgram(const QJsonObject &sourceConfig, const QJsonObject &metadata,
                           const QString &requestedEpisodeId) {
    ProgramPlan plan;
    plan.config = sourceConfig;
    auto result = normalizeProgram(sourceConfig.value("programJson"), metadata);
    plan.active = result.active;
    plan.error = result.error;
    if (!result.ok() || !result.active) return plan;
    plan.program = result.program;
    const auto selected = requestedEpisodeId.isEmpty() ? result.program.value("selectedEpisode").toString() : requestedEpisodeId;
    QJsonObject episode;
    for (const auto &value : result.program.value("episodes").toArray()) {
        const auto candidate = value.toObject();
        if (candidate.value("id").toString() == selected && candidate.value("enabled").toBool()) { episode = candidate; break; }
    }
    if (episode.isEmpty()) { plan.error = "requested episode must identify an enabled episode"; return plan; }
    const int playlist = episode.value("playlist").toInt();
    for (const auto &value : metadata.value("titles").toArray()) {
        const auto title = value.toObject();
        if (title.value("playlist").toInt() == playlist) { plan.titleDuration = title.value("duration").toDouble(); break; }
    }
    plan.episodeId = selected;
    plan.program["selectedEpisode"] = selected;
    plan.start = episode.value("start").toDouble();
    plan.end = episode.value("end").toDouble();
    if (plan.end == 0) plan.end = plan.titleDuration;
    plan.duration = plan.end - plan.start;
    plan.nextEpisodeId = nextProgramEpisode(plan.program, selected);
    plan.config.remove("chapter");
    plan.config["playlist"] = playlist;
    plan.config["start"] = plan.start;
    plan.config["duration"] = plan.duration;
    plan.config["programJson"] = plan.program;
    for (const auto &value : plan.program.value("subtitles").toArray()) {
        const auto subtitle = value.toObject();
        if (subtitle.value("playlist").toInt() != playlist) continue;
        plan.config["subtitleTrack"] = subtitle.value("track");
        plan.config["subtitle"] = subtitle.value("file");
        plan.config["subtitleOffset"] = subtitle.value("offset");
        plan.config["fontsDir"] = subtitle.value("fontsDir");
        break;
    }
    return plan;
}
}
