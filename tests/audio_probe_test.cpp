#include "audio_probe.h"
#include "audio_info.h"
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtTest/QTest>

namespace {
QJsonObject track(int pid = 0x1100, int coding = 0x83) {
    return {{"index", 3}, {"pid", pid}, {"coding", coding}, {"language", "jpn"}, {"codec", "Dolby TrueHD"}};
}
QJsonObject stream(QString format = "MLP FBA", QJsonValue id = "4352") {
    return {{"@type", "Audio"}, {"ID", id}, {"Format", format}, {"SamplingRate", "48000"},
            {"Channels", "8"}, {"BitDepth", "24"}, {"ChannelLayout", "L R C LFE Ls Rs Lrs Rrs"}};
}
QJsonObject report(QJsonArray streams) { return {{"media", QJsonObject{{"track", streams}}}}; }
}
class AudioProbeTest : public QObject {
    Q_OBJECT
private slots:
    void explicitAtmosOnly_data() {
        QTest::addColumn<QString>("field"); QTest::addColumn<QString>("value"); QTest::addColumn<int>("coding");
        QTest::addColumn<QString>("format"); QTest::addColumn<bool>("atmos");
        QTest::newRow("truehd-commercial") << "Format_Commercial_IfAny" << "Dolby TrueHD with Dolby Atmos" << 0x83 << "MLP FBA" << true;
        QTest::newRow("eac3-commercial") << "Format_Commercial" << "Dolby Digital Plus with Dolby Atmos" << 0x84 << "E-AC-3" << true;
        QTest::newRow("eac3-joc-profile") << "Format_Profile" << "E-AC-3 JOC" << 0x84 << "E-AC-3" << true;
        QTest::newRow("eac3-joc-token") << "Format_AdditionalFeatures" << "JOC" << 0x84 << "E-AC-3" << true;
        QTest::newRow("truehd-is-not-atmos") << "Format_Commercial_IfAny" << "Dolby TrueHD" << 0x83 << "MLP FBA" << false;
        QTest::newRow("16ch-not-enough") << "Format_Profile" << "MLP FBA 16-ch / MLP FBA" << 0x83 << "MLP FBA" << false;
        QTest::newRow("title-is-not-evidence") << "Title" << "Dolby Atmos" << 0x83 << "MLP FBA" << false;
        QTest::newRow("non-dolby-not-atmos") << "Format_Commercial" << "Dolby Atmos" << 0x86 << "DTS" << false;
    }
    void explicitAtmosOnly() {
        QFETCH(QString, field); QFETCH(QString, value); QFETCH(int, coding); QFETCH(QString, format); QFETCH(bool, atmos);
        auto info = stream(format); info[field] = value;
        const auto result = sd::mergeAudioProbe(report({info}), {track(0x1100, coding)}).first().toObject();
        QCOMPARE(result.value("atmos").toBool(), atmos);
        if (atmos) { QVERIFY(!result.value("atmosEvidence").toString().isEmpty()); QVERIFY(result.value("label").toString().contains("Dolby Atmos")); }
        else QVERIFY(!result.contains("atmos")); // A short probe cannot prove absence.
    }
    void pidMatchingNotOrdinal() {
        auto first = stream("AC-3", "4353"); first["Channels"] = "6"; first["BitRate"] = "640000"; first["BitRate_Mode"] = "CBR";
        auto second = stream("MLP FBA", "4352"); second["Format_Commercial"] = "Dolby TrueHD with Dolby Atmos";
        const auto result = sd::mergeAudioProbe(report({first, second}), {track(0x1100, 0x83), track(0x1101, 0x81)});
        QVERIFY(result[0].toObject().value("atmos").toBool());
        QCOMPARE(result[1].toObject().value("channels").toInt(), 6);
        QCOMPARE(result[1].toObject().value("bitrate").toInt(), 640000);
        QVERIFY(!result[1].toObject().contains("atmos"));
        QCOMPARE(result[0].toObject().value("index").toInt(), 3);
    }
    void idFormsAndAmbiguity() {
        for (const auto &id : {QJsonValue("0x1100"), QJsonValue("4352 (0x1100)"), QJsonValue("4352-189"), QJsonValue(4352)}) {
            const auto result = sd::mergeAudioProbe(report({stream("MLP FBA", id)}), {track()}).first().toObject();
            QCOMPARE(result.value("channels").toInt(), 8);
        }
        for (const auto &id : {QJsonValue("0"), QJsonValue("1 / 4352"), QJsonValue("4352.5"), QJsonValue(4352.5), QJsonValue(8192), QJsonValue()})
            QCOMPARE(sd::mergeAudioProbe(report({stream("MLP FBA", id)}), {track()}).first().toObject(), track());
        QCOMPARE(sd::mergeAudioProbe(report({stream(), stream()}), {track()}).first().toObject(), track());
        auto withoutId = stream(); withoutId.remove("ID"); withoutId["MenuID"] = "4352";
        QCOMPARE(sd::mergeAudioProbe(report({withoutId}), {track()}).first().toObject(), track());
    }
    void coreAndLosslessSubstreams() {
        const auto result = sd::mergeAudioProbe(report({stream("AC-3", "4352-189"), stream("MLP FBA", "4352-190")}), {track()});
        QCOMPARE(result.first().toObject().value("channels").toInt(), 8);
        const QJsonObject exact{{"pid", 4352}, {"coding", 0x80}, {"bitrate", 4608000}, {"bitDepth", 24}};
        QCOMPARE(sd::mergeAudioProbe(report({stream("PCM")}), {exact}).first().toObject(), exact);
    }
    void bitrateScopeAndInvalidNumbers() {
        auto info = stream(); info["BitRate"] = "4587432"; info["BitRate_Mode"] = "VBR";
        auto result = sd::mergeAudioProbe(report({info}), {track()}).first().toObject();
        QVERIFY(!result.contains("bitrate")); QCOMPARE(result.value("probedBitrate").toInt(), 4587432);
        QCOMPARE(result.value("bitrateScope").toString(), QString("prefix"));
        QVERIFY(result.value("label").toString().contains("头部片段"));
        info["BitRate"] = "-100"; info["SamplingRate"] = "NaN"; info["Channels"] = "8 / 6"; info["BitDepth"] = "Inf";
        result = sd::mergeAudioProbe(report({info}), {track()}).first().toObject();
        QVERIFY(!result.contains("sampleRate")); QVERIFY(!result.contains("channels"));
        QVERIFY(!result.contains("bitDepth")); QVERIFY(!result.contains("probedBitrate"));
    }
    void optionalDllFallback() {
        const QJsonArray original{track()};
        QCOMPARE(sd::enrichAudioProbe({}, 0, original), original);
        QCOMPARE(sd::enrichAudioProbe("not media", 9, original), original);
        QCOMPARE(sd::mergeAudioProbe({}, original), original);
    }
    void realBufferProbeWhenFixtureProvided() {
        const auto directory = qEnvironmentVariable("OBS_BLURAY_AUDIO_PROBE_FIXTURES");
        if (directory.isEmpty()) QSKIP("Optional synthetic AC-3/E-AC-3 fixture directory not supplied");
        struct Fixture { const char *name; int pid; int coding; int channels; int bitrate; };
        for (const auto &fixture : {Fixture{"synthetic-ac3.ts", 4352, 0x81, 6, 640000}, Fixture{"synthetic-eac3.ts", 4353, 0x84, 2, 384000}}) {
            QFile file(directory + '/' + fixture.name); QVERIFY(file.open(QIODevice::ReadOnly));
            const auto result = sd::enrichAudioProbe(file.read(4 * 1024 * 1024), file.size(), {track(fixture.pid, fixture.coding)}).first().toObject();
            QCOMPARE(result.value("bitrate").toInt(), fixture.bitrate);
            QCOMPARE(result.value("channels").toInt(), fixture.channels);
            QCOMPARE(result.value("sampleRate").toInt(), 48000);
            QVERIFY(!result.contains("atmos"));
            QCOMPARE(result.value("bitrateSource").toString(), QString("mediainfo-cbr-header"));
            const auto catalogTrack = sd::enrichAudioTracks(file.fileName(), {QJsonObject{
                {"index", 0}, {"pid", fixture.pid}, {"coding", fixture.coding}, {"format", 6}, {"rate", 1}}}).first().toObject();
            QCOMPARE(catalogTrack.value("bitrate").toInt(), fixture.bitrate);
            QCOMPARE(catalogTrack.value("channels").toInt(), fixture.channels);
        }
    }
};
QTEST_GUILESS_MAIN(AudioProbeTest)
#include "audio_probe_test.moc"
