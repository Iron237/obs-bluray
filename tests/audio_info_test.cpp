#include "audio_info.h"
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

namespace {
QByteArray pes(int assignment = 3, int rate = 4, int depth = 3, int extension = 0) {
    QByteArray bytes = QByteArray::fromHex("000001bd0000800000");
    bytes[8] = char(extension); bytes.append(QByteArray(extension, char(0)));
    bytes.append(char(0)); bytes.append(char(24));
    bytes.append(char((assignment << 4) | rate)); bytes.append(char(depth << 6));
    return bytes;
}
QByteArray packet(int pid, const QByteArray &payload, bool begins = true, int cc = 0, bool m2ts = true) {
    Q_ASSERT(payload.size() <= 182);
    QByteArray bytes(188, char(-1));
    bytes[0] = 0x47; bytes[1] = char((begins ? 0x40 : 0) | ((pid >> 8) & 0x1f));
    bytes[2] = char(pid); bytes[3] = char(0x30 | cc);
    const int adaptation = 183 - int(payload.size());
    bytes[4] = char(adaptation); if (adaptation > 0) bytes[5] = 0;
    bytes.replace(5 + adaptation, payload.size(), payload);
    return m2ts ? QByteArray(4, char(0)) + bytes : bytes;
}
QJsonObject lpcm(int pid = 0x1100) {
    return {{"index", 0}, {"pid", pid}, {"language", "jpn"}, {"coding", 0x80}, {"format", 3}, {"rate", 4}};
}
QJsonArray probe(const QByteArray &data, const QJsonArray &tracks = QJsonArray{lpcm()}) {
    QTemporaryDir directory;
    QFile file(directory.filePath("synthetic.m2ts"));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) return {};
    file.close(); return sd::enrichAudioTracks(file.fileName(), tracks);
}
}

class AudioInfoTest : public QObject {
    Q_OBJECT
private slots:
    void lpcmHeader_data() {
        QTest::addColumn<int>("assignment"); QTest::addColumn<int>("rateCode"); QTest::addColumn<int>("depthCode");
        QTest::addColumn<int>("channels"); QTest::addColumn<int>("rate"); QTest::addColumn<int>("depth");
        QTest::addColumn<qint64>("bitrate"); QTest::addColumn<qint64>("transport");
        QTest::newRow("stereo96k24") << 3 << 4 << 3 << 2 << 96000 << 24 << qint64(4608000) << qint64(4608000);
        QTest::newRow("stereo48k24") << 3 << 1 << 3 << 2 << 48000 << 24 << qint64(2304000) << qint64(2304000);
        QTest::newRow("surround192k16") << 9 << 5 << 1 << 6 << 192000 << 16 << qint64(18432000) << qint64(18432000);
        QTest::newRow("mono48k16-padded") << 1 << 1 << 1 << 1 << 48000 << 16 << qint64(768000) << qint64(1536000);
        QTest::newRow("five20bit-padded") << 8 << 1 << 2 << 5 << 48000 << 20 << qint64(4800000) << qint64(6912000);
    }
    void lpcmHeader() {
        QFETCH(int, assignment); QFETCH(int, rateCode); QFETCH(int, depthCode); QFETCH(int, channels);
        QFETCH(int, rate); QFETCH(int, depth); QFETCH(qint64, bitrate); QFETCH(qint64, transport);
        const auto tracks = probe(packet(0x1100, pes(assignment, rateCode, depthCode)));
        QCOMPARE(tracks.size(), 1); const auto track = tracks.first().toObject();
        QCOMPARE(track.value("sampleRate").toInt(), rate); QCOMPARE(track.value("channels").toInt(), channels);
        QCOMPARE(track.value("bitDepth").toInt(), depth); QCOMPARE(track.value("bitrate").toInteger(), bitrate);
        QCOMPARE(track.value("transportBitrate").toInteger(), transport); QCOMPARE(track.value("streamId").toInt(), 0xbd);
        QCOMPARE(track.value("codec").toString(), QString("LPCM")); QCOMPARE(track.value("languageName").toString(), QString("日语"));
        QCOMPARE(track.value("index").toInt(), 0); QCOMPARE(track.value("pid").toInt(), 0x1100);
    }
    void tsAndFragmentedPes() {
        const auto bytes = pes(11, 4, 3, 180); // Header spans multiple TS packets.
        const auto data = packet(0x1100, bytes.left(100), true, 5, false) +
                          packet(0x1101, pes(3, 1, 1), true, 0, false) +
                          packet(0x1100, bytes.mid(100), false, 6, false);
        const auto track = probe(data).first().toObject();
        QCOMPARE(track.value("channels").toInt(), 8); QCOMPARE(track.value("channelLayout").toString(), QString("7.1"));
        QCOMPARE(track.value("bitDepth").toInt(), 24);
    }
    void damagedPacketsDoNotInventMeasurements() {
        auto bytes = pes();
        for (const auto &data : {packet(0x1100, bytes.left(5), true, 0) + packet(0x1100, bytes.mid(5), false, 2),
                                packet(0x1101, bytes), packet(0x1100, pes(2)), packet(0x1100, pes(3, 2)),
                                packet(0x1100, pes(3, 4, 0)), QByteArray("truncated")}) {
            const auto tracks = probe(data); QCOMPARE(tracks.size(), 1);
            const auto track = tracks.first().toObject();
            QVERIFY(!track.contains("bitDepth")); QVERIFY(!track.contains("bitrate"));
            QCOMPARE(track.value("sampleRate").toInt(), 96000); // Retain CLPI fallback.
        }
        auto scrambled = packet(0x1100, pes()); scrambled[7] = char(-80);
        QVERIFY(!probe(scrambled).first().toObject().contains("bitDepth"));
        auto transportError = packet(0x1100, pes()); transportError[5] = char(static_cast<unsigned char>(transportError[5]) | 0x80);
        QVERIFY(!probe(transportError).first().toObject().contains("bitDepth"));
    }
    void recoveryAfterBrokenPes() {
        const auto bytes = pes();
        const auto data = packet(0x1100, bytes.left(5), true, 0) + packet(0x1100, bytes.mid(5), false, 2) +
                          packet(0x1100, pes(3, 1, 1), true, 3);
        const auto track = probe(data).first().toObject();
        QCOMPARE(track.value("sampleRate").toInt(), 48000); QCOMPARE(track.value("bitDepth").toInt(), 16);
    }
    void compressedMetadataDoesNotGuessChannelsBitrateOrAtmos() {
        QJsonArray audio;
        for (int coding : {0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0xa1, 0xa2})
            audio.append(QJsonObject{{"index", audio.size()}, {"coding", coding}, {"format", 6}, {"rate", 1}, {"language", "eng"}});
        const auto tracks = sd::enrichAudioTracks({}, audio);
        QCOMPARE(tracks.size(), 8);
        for (const auto &value : tracks) {
            const auto track = value.toObject();
            QVERIFY(!track.value("codec").toString().isEmpty()); QVERIFY(!track.contains("channels"));
            QVERIFY(!track.contains("bitDepth")); QVERIFY(!track.contains("bitrate")); QVERIFY(!track.contains("atmos"));
            QVERIFY(!track.value("label").toString().contains("Atmos"));
            QCOMPARE(track.value("channelLayout").toString(), QString("多声道"));
            QCOMPARE(track.value("sampleRate").toInt(), 48000);
        }
        QCOMPARE(tracks[2].toObject().value("codec").toString(), QString("Dolby TrueHD"));
        QCOMPARE(tracks[5].toObject().value("codec").toString(), QString("DTS-HD Master Audio"));
    }
    void comboRatesRemainQualified() {
        for (int code : {12, 14}) {
            const auto tracks = sd::enrichAudioTracks({}, QJsonArray{QJsonObject{{"coding", 0x86}, {"rate", code}, {"format", 12}}});
            const auto track = tracks.first().toObject();
            QVERIFY(!track.contains("sampleRate")); QVERIFY(!track.contains("channels"));
            QVERIFY(track.value("sampleRateDescription").toString().contains("core"));
            QVERIFY(!track.value("label").toString().contains("Atmos"));
        }
    }
    void labelAndMissingFile() {
        const auto track = sd::enrichAudioTracks("missing-synthetic.m2ts", QJsonArray{lpcm()}).first().toObject();
        QVERIFY(!track.contains("bitrate")); QVERIFY(!track.contains("bitDepth"));
        const auto measured = probe(packet(0x1100, pes())).first().toObject();
        const auto label = sd::audioTrackLabel(measured);
        QVERIFY(label.contains("日语")); QVERIFY(label.contains("2.0")); QVERIFY(label.contains("96 kHz"));
        QVERIFY(label.contains("24 bit")); QVERIFY(label.contains("4.608 Mbps")); QVERIFY(label.contains("PID 0x1100"));
        QVERIFY(label.contains("双声道")); QVERIFY(label.contains("PES 0xbd"));
        QVERIFY(sd::audioTrackLabel({{"language", "qzz"}}).contains("qzz"));
        QVERIFY(sd::audioTrackLabel({}).contains("编码未知"));
    }
    void probeReadIsBounded() {
        const auto nullPacket = packet(0x1fff, QByteArray(8, char(0)), false, 0);
        QByteArray data;
        while (data.size() < 4 * 1024 * 1024) data.append(nullPacket);
        data.append(packet(0x1100, pes()));
        QVERIFY(!probe(data).first().toObject().contains("bitDepth"));
    }
};
QTEST_APPLESS_MAIN(AudioInfoTest)
#include "audio_info_test.moc"
