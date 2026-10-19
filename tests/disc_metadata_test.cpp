#include "disc_metadata.h"
#include <QtCore>
#include <QtTest>

namespace {
QJsonObject catalog() {
    return {
        {"ok", true},
        {"navigation", QJsonObject{{"titleCount", 2}, {"metadataAvailable", false}}},
        {"discTitles", QJsonArray{QJsonObject{{"number", 1}, {"name", ""}, {"objectRef", 7}},
                                  QJsonObject{{"number", 2}, {"name", "Keep navigation"}}}},
        {"titles",
         QJsonArray{QJsonObject{{"playlist", 42},
                                {"duration", 90},
                                {"chapters", QJsonArray{QJsonObject{{"index", 0}, {"start", 0}, {"duration", 30}},
                                                        QJsonObject{{"index", 1}, {"start", 30}, {"duration", 60}}}}},
                    QJsonObject{{"playlist", 1},
                                {"duration", 60},
                                {"chapters", QJsonArray{QJsonObject{{"index", 0}, {"start", 0}, {"duration", 60}}}}}}}};
}
void write(QTemporaryDir &directory, const QString &relative, const QByteArray &bytes) {
    const auto path = directory.filePath(relative);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
QByteArray tn(const QStringList &names) {
    QString xml = "<m:manifest xmlns:m='urn:synthetic'><m:chapters>";
    for (const auto &name : names)
        xml += "<m:name>" + name.toHtmlEscaped() + "</m:name>";
    return (xml + "</m:chapters></m:manifest>").toUtf8();
}
QJsonObject firstChapter(const QJsonObject &value) {
    return value["titles"].toArray()[0].toObject()["chapters"].toArray()[0].toObject();
}
} // namespace
class DiscMetadataTest : public QObject {
    Q_OBJECT
  private slots:
    void absentPreservesCatalogExactly() {
        QTemporaryDir directory;
        QCOMPARE(sd::enrichDiscMetadata(directory.path(), catalog()), catalog());
    }
    void dlTitleNumbersRemainNavigationOnly() {
        QTemporaryDir directory;
        write(directory, "BDMV/META/DL/bdmt_jpn.xml",
              QStringLiteral("<manifest><title><name>作品 &amp; 名前</name></title><tableOfContents><titleName "
                             "titleNumber='1'>本編</titleName><titleName titleNumber='42'>Not an MPLS "
                             "mapping</titleName></tableOfContents></manifest>")
                  .toUtf8());
        const auto result = sd::enrichDiscMetadata(directory.path(), catalog());
        QCOMPARE(result["discName"].toString(), QStringLiteral("作品 & 名前"));
        QCOMPARE(result["discTitles"].toArray()[0].toObject()["name"].toString(), QStringLiteral("本編"));
        QCOMPARE(result["discTitles"].toArray().size(), 2);
        QCOMPARE(result["titles"], catalog()["titles"]);
        QCOMPARE(result["discTitles"].toArray()[0].toObject()["objectRef"].toInt(), 7);
        QCOMPARE(result["discNameSource"].toObject()["language"].toString(), QString("jpn"));
        QVERIFY(result["navigation"].toObject()["xmlMetadataAvailable"].toBool());
    }
    void tnFilenameIdentifiesPlaylistAndNamesKeepTimes() {
        QTemporaryDir directory;
        write(directory, "BDMV/META/TN/tnmt_jpn_00042.xml", tn({QStringLiteral("曲名 A"), QStringLiteral("曲名 B")}));
        const auto result = sd::enrichDiscMetadata(directory.path(), catalog());
        const auto chapter = firstChapter(result);
        QCOMPARE(chapter["name"].toString(), QStringLiteral("曲名 A"));
        QCOMPARE(chapter["start"].toDouble(), 0.0);
        QCOMPARE(chapter["duration"].toDouble(), 30.0);
        QCOMPARE(chapter["nameSource"].toObject()["file"].toString(), QString("BDMV/META/TN/tnmt_jpn_00042.xml"));
        QCOMPARE(result["titles"].toArray()[1], catalog()["titles"].toArray()[1]);
        QVERIFY(!result.contains("discName"));
    }
    void preferredLanguagesAndStableFallback() {
        QTemporaryDir directory;
        for (const QString lang : {"eng", "jpn", "zho"})
            write(directory, "BDMV/META/TN/tnmt_" + lang + "_00042.xml", tn({lang}));
        QCOMPARE(firstChapter(sd::enrichDiscMetadata(directory.path(), catalog()))["name"].toString(), QString("zho"));
        QFile::remove(directory.filePath("BDMV/META/TN/tnmt_zho_00042.xml"));
        QCOMPARE(firstChapter(sd::enrichDiscMetadata(directory.path(), catalog()))["name"].toString(), QString("jpn"));
        QFile::remove(directory.filePath("BDMV/META/TN/tnmt_jpn_00042.xml"));
        QFile::remove(directory.filePath("BDMV/META/TN/tnmt_eng_00042.xml"));
        write(directory, "BDMV/META/TN/tnmt_fra_00042.xml", tn({"fra"}));
        write(directory, "BDMV/META/TN/tnmt_deu_00042.xml", tn({"deu"}));
        QCOMPARE(firstChapter(sd::enrichDiscMetadata(directory.path(), catalog()))["name"].toString(), QString("deu"));
    }
    void malformedPreferredFallsBackWithoutPartialMutation() {
        QTemporaryDir directory;
        write(directory, "BDMV/META/TN/tnmt_zho_00042.xml", "<manifest><chapters><name>bad partial</name>");
        write(directory, "BDMV/META/TN/tnmt_eng_00042.xml", tn({"valid fallback"}));
        QCOMPARE(firstChapter(sd::enrichDiscMetadata(directory.path(), catalog()))["name"].toString(),
                 QString("valid fallback"));
        QFile::remove(directory.filePath("BDMV/META/TN/tnmt_eng_00042.xml"));
        QCOMPARE(sd::enrichDiscMetadata(directory.path(), catalog()), catalog());
    }
    void mismatchedCountsAreMarkedWithoutInventingChapters() {
        QTemporaryDir directory;
        write(directory, "BDMV/META/TN/tnmt_eng_00042.xml", tn({"one"}));
        auto result = sd::enrichDiscMetadata(directory.path(), catalog());
        auto title = result["titles"].toArray()[0].toObject();
        QCOMPARE(title["chapters"].toArray().size(), 2);
        QVERIFY(!title["chapters"].toArray()[1].toObject().contains("name"));
        QVERIFY(!title["chapterNames"].toObject()["countMatches"].toBool());
        QCOMPARE(title["chapterNames"].toObject()["xmlCount"].toInt(), 1);
        write(directory, "BDMV/META/TN/tnmt_eng_00042.xml", tn({"one", "two", "unmatched third"}));
        result = sd::enrichDiscMetadata(directory.path(), catalog());
        title = result["titles"].toArray()[0].toObject();
        QCOMPARE(title["chapters"].toArray().size(), 2);
        QCOMPARE(title["chapterNames"].toObject()["matchedCount"].toInt(), 2);
        QCOMPARE(title["chapterNames"].toObject()["xmlCount"].toInt(), 3);
    }
    void emptyNameRetainsChapterPosition() {
        QTemporaryDir directory;
        write(directory, "BDMV/META/TN/tnmt_eng_00042.xml", tn({"", "second"}));
        const auto result = sd::enrichDiscMetadata(directory.path(), catalog());
        const auto chapters = result["titles"].toArray()[0].toObject()["chapters"].toArray();
        QVERIFY(!chapters[0].toObject().contains("name"));
        QCOMPARE(chapters[1].toObject()["name"].toString(), QString("second"));
    }
    void exactFilenamePatternsOnly() {
        QTemporaryDir directory;
        for (const QString bad : {"tnmt_eng_42.xml", "tnmt_eng00042.xml", "tnmt_eng_00042.xml.bak", "tnmt_en_00042.xml",
                                  "tnmt_eng_00001.xml"})
            write(directory, "BDMV/META/TN/" + bad, tn({"ignored"}));
        // The last valid filename belongs to playlist 1, so only that playlist may change.
        const auto result = sd::enrichDiscMetadata(directory.path(), catalog());
        QCOMPARE(result["titles"].toArray()[0], catalog()["titles"].toArray()[0]);
        write(directory, "BDMV/META/TN/TNMT_JPN_00042.XML", tn({"uppercase"}));
        QCOMPARE(firstChapter(sd::enrichDiscMetadata(directory.path(), catalog()))["name"].toString(),
                 QString("uppercase"));
    }
    void dtdAndEntitiesAreRejected() {
        QTemporaryDir directory;
        write(directory, "secret.txt", "PRIVATE-MARKER");
        const auto path = QUrl::fromLocalFile(directory.filePath("secret.txt")).toString();
        write(directory, "BDMV/META/TN/tnmt_eng_00042.xml",
              ("<!DOCTYPE manifest [<!ENTITY x SYSTEM '" + path +
               "'>]><manifest><chapters><name>&x;</name></chapters></manifest>")
                  .toUtf8());
        QCOMPARE(sd::enrichDiscMetadata(directory.path(), catalog()), catalog());
        write(directory, "BDMV/META/TN/tnmt_eng_00042.xml",
              "<!DOCTYPE manifest [<!ENTITY x 'expanded'>]><manifest><chapters><name>&x;</name></chapters></manifest>");
        QCOMPARE(sd::enrichDiscMetadata(directory.path(), catalog()), catalog());
    }
    void oversizedAndDeepInputsAreIgnored() {
        QTemporaryDir directory;
        write(directory, "BDMV/META/TN/tnmt_eng_00042.xml", QByteArray(1024 * 1024 + 1, ' '));
        QCOMPARE(sd::enrichDiscMetadata(directory.path(), catalog()), catalog());
        write(directory, "BDMV/META/TN/tnmt_eng_00042.xml", tn({QString(4097, QChar('x'))}));
        QCOMPARE(sd::enrichDiscMetadata(directory.path(), catalog()), catalog());
        QByteArray deep;
        for (int i = 0; i < 70; ++i)
            deep += "<a>";
        deep += "<chapters><name>deep</name></chapters>";
        for (int i = 0; i < 70; ++i)
            deep += "</a>";
        write(directory, "BDMV/META/TN/tnmt_eng_00042.xml", deep);
        QCOMPARE(sd::enrichDiscMetadata(directory.path(), catalog()), catalog());
    }
};
QTEST_GUILESS_MAIN(DiscMetadataTest)
#include "disc_metadata_test.moc"
