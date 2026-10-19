#include "program.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtTest/QTest>

namespace {
QJsonObject catalog() {
    return {{"suggestedPlaylist", 12}, {"titles", QJsonArray{
        QJsonObject{{"playlist", 7}, {"duration", 120}, {"chapters", QJsonArray{
            QJsonObject{{"index", 0}, {"start", 0}}, QJsonObject{{"index", 1}, {"start", 60}}}},
            {"subtitles", QJsonArray{QJsonObject{{"index", 2}, {"type", "PGS"}}, QJsonObject{{"index", 4}, {"type", "PGS"}}}}},
        QJsonObject{{"playlist", 12}, {"duration", 90}, {"subtitles", QJsonArray{}}},
        QJsonObject{{"playlist", 21}, {"duration", 300}, {"subtitles", QJsonArray{QJsonObject{{"index", 0}, {"type", "PGS"}}}}}
    }}};
}
QJsonObject episode(QString id, int playlist, double start = 0, double end = 0, bool enabled = true) {
    return {{"id", id}, {"playlist", playlist}, {"start", start}, {"end", end}, {"enabled", enabled}};
}
QJsonObject sample() {
    return {{"version", 1}, {"playMode", "sequence"}, {"selectedEpisode", "a"}, {"loop", false},
        {"episodes", QJsonArray{episode("a", 7, 10, 35), episode("skip", 21, 0, 0, false),
                               episode("b", 7, 80), episode("c", 12)}}};
}
QJsonObject subtitle(const QJsonObject &program, int playlist) {
    for (const auto &value : program.value("subtitles").toArray())
        if (value.toObject().value("playlist").toInt() == playlist) return value.toObject();
    return {};
}
}

class ProgramTest : public QObject {
    Q_OBJECT
private slots:
    void legacyUnchanged() {
        for (const auto &payload : {QJsonValue(QJsonValue::Undefined), QJsonValue(QJsonValue::Null), QJsonValue("  ")}) {
            QJsonObject config{{"playlist", 99}, {"chapter", 4}, {"start", 123}, {"subtitle", "legacy.ass"}};
            if (!payload.isUndefined()) config["programJson"] = payload;
            const auto plan = sd::resolveProgram(config, {});
            QVERIFY(plan.ok()); QVERIFY(!plan.active); QCOMPARE(plan.config, config);
        }
    }
    void rangesAndDefaults() {
        auto plan = sd::resolveProgram({{"programJson", sample()}, {"chapter", 6}, {"input", "synthetic-disc"},
                                       {"subtitle", "stale.ass"}, {"subtitleTrack", 99}}, catalog());
        QVERIFY2(plan.ok(), qPrintable(plan.error)); QVERIFY(plan.active);
        QCOMPARE(plan.episodeId, QString("a")); QCOMPARE(plan.nextEpisodeId, QString("b"));
        QCOMPARE(plan.start, 10.0); QCOMPARE(plan.end, 35.0); QCOMPARE(plan.duration, 25.0);
        QCOMPARE(plan.titleDuration, 120.0); QCOMPARE(plan.config.value("input").toString(), QString("synthetic-disc"));
        QVERIFY(!plan.config.contains("chapter")); QCOMPARE(plan.config.value("start").toDouble(), 10.0);
        QCOMPARE(plan.config.value("subtitleTrack").toInt(), 2); QCOMPARE(plan.config.value("subtitle").toString(), QString());
        QCOMPARE(plan.program.value("episodes").toArray().size(), 4); // Chapters do not generate episodes.
        QCOMPARE(subtitle(plan.program, 12).value("mode").toString(), QString("off"));
        const auto tail = sd::resolveProgram({{"programJson", sample()}}, catalog(), "b");
        QVERIFY(tail.ok()); QCOMPARE(tail.start, 80.0); QCOMPARE(tail.end, 120.0); QCOMPARE(tail.duration, 40.0);
        QCOMPARE(tail.program.value("episodes").toArray().at(2).toObject().value("end").toDouble(), 0.0);
        QCOMPARE(tail.program.value("selectedEpisode").toString(), QString("b"));
        QVERIFY(!sd::resolveProgram({{"programJson", sample()}}, catalog(), "skip").ok());
    }
    void subtitlesPerMplsAndAbsoluteAss() {
        auto program = sample();
        program["subtitles"] = QJsonArray{
            QJsonObject{{"playlist", 7}, {"mode", "external"}, {"file", "synthetic.ass"}, {"offset", -1.25}, {"fontsDir", "fonts"}},
            QJsonObject{{"playlist", 12}, {"mode", "off"}},
            QJsonObject{{"playlist", 21}, {"mode", "internal"}, {"track", -1}}
        };
        for (const auto &id : {QString("a"), QString("b")}) {
            const auto plan = sd::resolveProgram({{"programJson", program}}, catalog(), id);
            QVERIFY2(plan.ok(), qPrintable(plan.error));
            QCOMPARE(plan.config.value("subtitle").toString(), QString("synthetic.ass"));
            QCOMPARE(plan.config.value("subtitleTrack").toInt(), -1);
            QCOMPARE(plan.config.value("subtitleOffset").toDouble(), -1.25); // Not offset minus episode start.
            QCOMPARE(plan.config.value("fontsDir").toString(), QString("fonts"));
        }
        const auto other = sd::resolveProgram({{"programJson", program}, {"subtitle", "stale.ass"}, {"subtitleTrack", 8},
                                              {"subtitleOffset", 33}, {"fontsDir", "stale-fonts"}}, catalog(), "c");
        QVERIFY(other.ok()); QCOMPARE(other.config.value("subtitle").toString(), QString());
        QCOMPARE(other.config.value("subtitleTrack").toInt(), -1); QCOMPARE(other.config.value("subtitleOffset").toDouble(), 0.0);
        QCOMPARE(other.config.value("fontsDir").toString(), QString());
        const auto normalized = sd::normalizeProgram(program, catalog());
        QCOMPARE(subtitle(normalized.program, 21).value("track").toInt(), 0); // Unused MPLS settings retained.
        const auto again = sd::normalizeProgram(normalized.program, catalog());
        QVERIFY(again.ok()); QCOMPARE(again.program, normalized.program);
    }
    void navigation() {
        auto program = sd::normalizeProgram(sample(), catalog()).program;
        QCOMPARE(sd::nextProgramEpisode(program, "a"), QString("b"));
        QCOMPARE(sd::nextProgramEpisode(program, "b"), QString("c"));
        QCOMPARE(sd::nextProgramEpisode(program, "c"), QString());
        QCOMPARE(sd::adjacentProgramEpisode(program, "b", -1), QString("a"));
        QCOMPARE(sd::adjacentProgramEpisode(program, "a", -1), QString());
        QCOMPARE(sd::adjacentProgramEpisode(program, "skip", 1), QString());
        QCOMPARE(sd::adjacentProgramEpisode(program, "a", 0), QString());
        program["loop"] = true;
        QCOMPARE(sd::nextProgramEpisode(program, "c"), QString("a"));
        QCOMPARE(sd::adjacentProgramEpisode(program, "a", -1), QString("c"));
        program["playMode"] = "single";
        QCOMPARE(sd::nextProgramEpisode(program, "b"), QString("b"));
        QCOMPARE(sd::adjacentProgramEpisode(program, "b", 1), QString("c"));
        program["loop"] = false;
        QCOMPARE(sd::nextProgramEpisode(program, "b"), QString());
        QCOMPARE(sd::nextProgramEpisode(program, "missing"), QString());
    }
    void jsonStringAndDefault() {
        const auto text = QString::fromUtf8(QJsonDocument(sample()).toJson(QJsonDocument::Compact));
        const auto parsed = sd::normalizeProgram(text, catalog());
        QVERIFY(parsed.ok()); QCOMPARE(parsed.program, sd::normalizeProgram(sample(), catalog()).program);
        const auto created = sd::createDefaultProgram(catalog());
        QVERIFY(created.ok()); QCOMPARE(created.program.value("episodes").toArray().size(), 1);
        const auto first = created.program.value("episodes").toArray().first().toObject();
        QCOMPARE(first.value("playlist").toInt(), 12); QCOMPARE(first.value("start").toDouble(), 0.0);
        QCOMPARE(first.value("end").toDouble(), 0.0); QCOMPARE(created.program.value("playMode").toString(), QString("single"));
        auto noSuggestion = catalog(); noSuggestion.remove("suggestedPlaylist");
        QCOMPARE(sd::createDefaultProgram(noSuggestion).program.value("episodes").toArray().first().toObject().value("playlist").toInt(), 7);
    }
    void invalidPrograms_data() {
        QTest::addColumn<QJsonValue>("payload");
        auto invalid = [](const char *name, const QJsonObject &program) { QTest::newRow(name) << QJsonValue(program); };
        QTest::newRow("malformed-json") << QJsonValue("{broken");
        QTest::newRow("array") << QJsonValue(QJsonArray{});
        auto program = sample(); program["version"] = 2; invalid("version", program);
        program = sample(); program["episodes"] = QJsonArray{}; invalid("empty", program);
        program = sample(); program["episodes"] = QJsonArray{episode("a", 7), episode("a", 12)}; invalid("duplicate-id", program);
        program = sample(); program["episodes"] = QJsonArray{episode("a", 7, 0, 0, false)}; invalid("all-disabled", program);
        program = sample(); program["selectedEpisode"] = "skip"; invalid("disabled-selection", program);
        program = sample(); program["selectedEpisode"] = "missing"; invalid("missing-selection", program);
        program = sample(); program["playMode"] = "shuffle"; invalid("mode", program);
        program = sample(); program["loop"] = "true"; invalid("loop-type", program);
        program = sample(); program["episodes"] = QJsonArray{episode("a", 999)}; invalid("missing-mpls", program);
        program = sample(); program["episodes"] = QJsonArray{episode("a", 7, -1)}; invalid("negative-start", program);
        program = sample(); program["episodes"] = QJsonArray{episode("a", 7, 120)}; invalid("start-at-end", program);
        program = sample(); program["episodes"] = QJsonArray{episode("a", 7, 50, 50)}; invalid("zero-range", program);
        program = sample(); program["episodes"] = QJsonArray{episode("a", 7, 50, 40)}; invalid("reversed-range", program);
        program = sample(); program["episodes"] = QJsonArray{episode("a", 7, 0, 120.01)}; invalid("past-title", program);
        auto badEpisode = episode("a", 7); badEpisode["playlist"] = 7.5;
        program = sample(); program["episodes"] = QJsonArray{badEpisode}; invalid("fractional-mpls", program);
        badEpisode = episode("a", 7); badEpisode["start"] = "10";
        program = sample(); program["episodes"] = QJsonArray{badEpisode}; invalid("string-time", program);
        program = sample(); program["subtitles"] = QJsonArray{QJsonObject{{"playlist", 7}, {"mode", "internal"}, {"track", 1}}}; invalid("missing-pgs", program);
        program = sample(); program["subtitles"] = QJsonArray{QJsonObject{{"playlist", 7}, {"mode", "external"}}}; invalid("missing-ass", program);
        program = sample(); program["subtitles"] = QJsonArray{QJsonObject{{"playlist", 7}, {"mode", "auto"}}}; invalid("bad-sub-mode", program);
        program = sample(); program["subtitles"] = QJsonArray{QJsonObject{{"playlist", 7}}, QJsonObject{{"playlist", 7}}}; invalid("duplicate-sub-settings", program);
        program = sample(); program["subtitles"] = QJsonArray{QJsonObject{{"playlist", 999}}}; invalid("missing-sub-mpls", program);
    }
    void invalidPrograms() {
        QFETCH(QJsonValue, payload);
        const auto result = sd::normalizeProgram(payload, catalog());
        QVERIFY(result.active); QVERIFY(!result.ok()); QVERIFY(!result.error.isEmpty());
        const auto plan = sd::resolveProgram({{"programJson", payload}}, catalog());
        QVERIFY(!plan.ok());
    }
    void invalidCatalogAndAutomaticNoPgs() {
        QVERIFY(!sd::normalizeProgram(sample(), {}).ok());
        auto metadata = catalog(); auto titles = metadata.value("titles").toArray();
        auto title = titles[0].toObject(); title["duration"] = 0; titles[0] = title; metadata["titles"] = titles;
        QVERIFY(!sd::normalizeProgram(sample(), metadata).ok());
        auto program = sample(); program["subtitles"] = QJsonArray{QJsonObject{{"playlist", 12}, {"mode", "internal"}, {"track", -1}}};
        const auto result = sd::normalizeProgram(program, catalog());
        QVERIFY(result.ok()); QCOMPARE(subtitle(result.program, 12).value("mode").toString(), QString("off"));
    }
};
QTEST_APPLESS_MAIN(ProgramTest)
#include "program_test.moc"
