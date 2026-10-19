#include "program_editor.h"
#include <QtTest>
#include <QtWidgets>
#include <cmath>

namespace {
QJsonObject metadata() {
    return {
        {"ok", true},
        {"kind", "bdmv"},
        {"suggestedPlaylist", 1},
        {"titles",
         QJsonArray{
             QJsonObject{{"playlist", 1},
                         {"duration", 150.0},
                         {"chapters", QJsonArray{QJsonObject{{"start", 0.0}}, QJsonObject{{"start", 30.0}},
                                                 QJsonObject{{"start", 90.0}}}},
                         {"subtitles",
                          QJsonArray{QJsonObject{{"index", 0}, {"pid", 4608}, {"language", "jpn"}, {"type", "PGS"}},
                                     QJsonObject{{"index", 1}, {"pid", 4609}, {"language", "eng"}, {"type", "PGS"}}}}},
             QJsonObject{{"playlist", 2},
                         {"duration", 80.0},
                         {"chapters", QJsonArray{QJsonObject{{"start", 0.0}}}},
                         {"subtitles", QJsonArray{}}}}}};
}
QJsonObject subtitleFor(const QJsonObject &program, int playlist) {
    for (const auto &value : program["subtitles"].toArray())
        if (value.toObject()["playlist"].toInt() == playlist)
            return value.toObject();
    return {};
}
void click(ProgramEditor &editor, const char *name) {
    auto *widget = editor.findChild<QPushButton *>(name);
    QVERIFY(widget);
    widget->click();
}
QTableWidget *entries(ProgramEditor &editor) { return editor.findChild<QTableWidget *>("programEntries"); }
double luminance(const QColor &color) {
    auto linear = [](double value) { return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}
double contrast(const QColor &first, const QColor &second) {
    const auto a = luminance(first), b = luminance(second);
    return (qMax(a, b) + 0.05) / (qMin(a, b) + 0.05);
}
} // namespace

class ProgramEditorTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        ProgramEditor::configureAppearance();
        // Offscreen Qt has no Windows font discovery; load installed fonts for visual QA.
        const QDir fonts(qEnvironmentVariable("WINDIR") + "/Fonts");
        for (const QString &name : {QString("segoeui.ttf"), QString("msyh.ttc")}) {
            const auto path = fonts.filePath(name);
            if (QFileInfo::exists(path))
                QFontDatabase::addApplicationFont(path);
        }
        QApplication::setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
    }
    void timeFormats() {
        double seconds = -1;
        QVERIFY(ProgramEditor::parseTime("01:02:03.045", &seconds));
        QCOMPARE(seconds, 3723.045);
        QVERIFY(ProgramEditor::parseTime(" 90.125 ", &seconds));
        QCOMPARE(seconds, 90.125);
        QVERIFY(ProgramEditor::parseTime("25:00:00.1", &seconds));
        QCOMPARE(seconds, 90000.1);
        for (const QString bad : {"", "-1", "nan", "inf", "01:60:00", "00:00:60", "1:2:3", "1e9", "0:00:00.1234"})
            QVERIFY2(!ProgramEditor::parseTime(bad, &seconds), qPrintable(bad));
        QCOMPARE(ProgramEditor::formatTime(3723.045), QString("01:02:03.045"));
    }
    void embeddedOriginalIcon() {
        QVERIFY(QFile::exists(":/brand/obs-bluray.svg"));
        const QIcon icon(QStringLiteral(":/brand/obs-bluray.svg"));
        QVERIFY(!icon.isNull());
        QVERIFY(!icon.pixmap(24, 24).isNull());
        ProgramEditor editor(metadata());
        QVERIFY(!editor.windowIcon().isNull());
    }
    void readableUnderSystemPalette_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void readableUnderSystemPalette() {
        QFETCH(bool, dark);
        const auto original = QApplication::palette();
        const auto restore = qScopeGuard([&] { QApplication::setPalette(original); });
        auto system = original;
        for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            for (auto role : {QPalette::Window, QPalette::Base, QPalette::Button})
                system.setColor(group, role, dark ? QColor("#202020") : QColor("#ffffff"));
            for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
                system.setColor(group, role, dark ? QColor("#ffffff") : QColor("#202020"));
        }
        QApplication::setPalette(system);
        // Exercise the same initialization used by the standalone executable.
        ProgramEditor::configureAppearance();
        auto meta = metadata();
        meta["navigation"] = QJsonObject{{"titleCount", 3}, {"hdmvTitles", 3}, {"bdjTitles", 0}};
        ProgramEditor editor(meta);
        editor.show();
        QApplication::processEvents();
        auto readable = [](QWidget *widget, QPalette::ColorRole foreground, QPalette::ColorRole background,
                           QPalette::ColorGroup group, double minimum) {
            const auto palette = widget->palette();
            const auto ink = palette.color(group, foreground), paper = palette.color(group, background);
            const auto message = QString("%1/%2 contrast %3 (%4 on %5)")
                                     .arg(widget->metaObject()->className(), widget->objectName())
                                     .arg(contrast(ink, paper)).arg(ink.name(), paper.name());
            QVERIFY2(contrast(ink, paper) >= minimum, qPrintable(message));
        };
        for (auto *label : editor.findChildren<QLabel *>())
            readable(label, QPalette::WindowText, QPalette::Window, QPalette::Active, 4.5);
        for (auto *button : editor.findChildren<QPushButton *>())
            readable(button, QPalette::ButtonText, QPalette::Button,
                     button->isEnabled() ? QPalette::Active : QPalette::Disabled, 4.5);
        for (auto *table : editor.findChildren<QTableWidget *>()) {
            readable(table, QPalette::Text, QPalette::Base, QPalette::Active, 4.5);
            readable(table, QPalette::Text, QPalette::AlternateBase, QPalette::Inactive, 4.5);
            readable(table, QPalette::HighlightedText, QPalette::Highlight, QPalette::Active, 4.5);
            readable(table->horizontalHeader(), QPalette::ButtonText, QPalette::Button, QPalette::Active, 4.5);
        }
        for (auto *combo : editor.findChildren<QComboBox *>()) {
            readable(combo, QPalette::Text, QPalette::Base,
                     combo->isEnabled() ? QPalette::Active : QPalette::Disabled, 4.5);
            readable(combo->view(), QPalette::Text, QPalette::Base, QPalette::Active, 4.5);
        }
        for (auto *input : editor.findChildren<QLineEdit *>())
            readable(input, QPalette::Text, QPalette::Base,
                     input->isEnabled() ? QPalette::Active : QPalette::Disabled, 4.5);
        auto *subMode = editor.findChild<QComboBox *>("subtitleMode");
        subMode->setCurrentIndex(subMode->findData("external"));
        auto *input = editor.findChild<QLineEdit *>("subtitleFile");
        input->setText(QStringLiteral("synthetic.ass"));
        input->setFocus();
        readable(input, QPalette::Text, QPalette::Base, QPalette::Active, 4.5);
        readable(input, QPalette::PlaceholderText, QPalette::Base, QPalette::Active, 4.5);
        entries(editor)->editItem(entries(editor)->item(0, 1));
        QApplication::processEvents();
        for (auto *cellEditor : entries(editor)->findChildren<QLineEdit *>())
            readable(cellEditor, QPalette::Text, QPalette::Base, QPalette::Active, 4.5);
        const auto screenshot = qEnvironmentVariable("PROGRAM_EDITOR_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(editor.grab().save(QFileInfo(screenshot).dir().filePath(
                dark ? "program-editor-dark.png" : "program-editor-light.png")));
    }
    void defaultDoesNotGuessEpisodes() {
        ProgramEditor editor(metadata());
        QString error;
        const auto p = editor.program(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(p["episodes"].toArray().size(), 1);
        const auto ep = p["episodes"].toArray().first().toObject();
        QCOMPARE(ep["playlist"].toInt(), 1);
        QCOMPARE(ep["start"].toDouble(), 0.0);
        QCOMPARE(ep["end"].toDouble(), 0.0);
        QVERIFY(!ep["label"].toString().contains(QStringLiteral("第")));
        QVERIFY(!ep["id"].toString().isEmpty());
    }
    void duplicateReorderAndSelectedIdentity() {
        ProgramEditor editor(metadata());
        auto *table = entries(editor);
        table->selectRow(0);
        click(editor, "duplicateEntry");
        QCOMPARE(table->rowCount(), 2);
        QVERIFY(table->item(0, 0)->data(Qt::UserRole) != table->item(1, 0)->data(Qt::UserRole));
        table->item(1, 1)->setText(QStringLiteral("自选片段"));
        table->item(1, 3)->setText("30");
        table->item(1, 4)->setText("00:01:30.000");
        auto *start = editor.findChild<QComboBox *>("startEntry");
        start->setCurrentIndex(1);
        const QString chosen = start->currentData().toString();
        click(editor, "moveEntryUp");
        QString error;
        auto p = editor.program(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(p["selectedEpisode"].toString(), chosen);
        QCOMPARE(p["episodes"].toArray().first().toObject()["label"].toString(), QStringLiteral("自选片段"));
        table->selectRow(1);
        click(editor, "removeEntry");
        QCOMPARE(table->rowCount(), 1);
    }
    void chapterRangeAndWholePlaylist() {
        ProgramEditor editor(metadata());
        editor.findChild<QComboBox *>("chapterStart")->setCurrentIndex(1);
        editor.findChild<QComboBox *>("chapterEnd")->setCurrentIndex(2);
        click(editor, "applyChapters");
        auto *catalog = editor.findChild<QTableWidget *>("playlistCatalog");
        catalog->selectRow(1);
        click(editor, "addPlaylist");
        QString error;
        const auto p = editor.program(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto eps = p["episodes"].toArray();
        QCOMPARE(eps.size(), 2);
        QCOMPARE(eps[0].toObject()["start"].toDouble(), 30.0);
        QCOMPARE(eps[0].toObject()["end"].toDouble(), 90.0);
        QCOMPARE(eps[1].toObject()["playlist"].toInt(), 2);
        QCOMPARE(eps[1].toObject()["end"].toDouble(), 0.0);
    }
    void subtitlePerPlaylistSurvivesSwitchAndSegment() {
        ProgramEditor editor(metadata());
        auto *subMode = editor.findChild<QComboBox *>("subtitleMode");
        auto *subPlaylist = editor.findChild<QComboBox *>("subtitlePlaylist");
        subMode->setCurrentIndex(subMode->findData("external"));
        editor.findChild<QLineEdit *>("subtitleFile")->setText("synthetic.ass");
        editor.findChild<QLineEdit *>("subtitleFonts")->setText("fonts");
        editor.findChild<QDoubleSpinBox *>("subtitleOffset")->setValue(1.25);
        subPlaylist->setCurrentIndex(1);
        subMode->setCurrentIndex(subMode->findData("off"));
        subPlaylist->setCurrentIndex(0);
        QCOMPARE(editor.findChild<QLineEdit *>("subtitleFile")->text(), QString("synthetic.ass"));
        entries(editor)->item(0, 3)->setText("30");
        QString error;
        const auto p = editor.program(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto sub = subtitleFor(p, 1);
        QCOMPARE(sub["file"].toString(), QString("synthetic.ass"));
        QCOMPARE(sub["offset"].toDouble(), 1.25);
        QCOMPARE(sub["fontsDir"].toString(), QString("fonts"));
        QCOMPARE(subtitleFor(p, 2)["mode"].toString(), QString("off"));
    }
    void chapterBatchCandidatesKeepExactBoundaries() {
        auto meta = metadata();
        auto titles = meta["titles"].toArray();
        auto first = titles[0].toObject();
        // Duplicate, out-of-range, and unsorted chapter starts are not zero-length episodes.
        first["chapters"] =
            QJsonArray{QJsonObject{{"start", 90.0}}, QJsonObject{{"start", 0.0}}, QJsonObject{{"start", 30.0}},
                       QJsonObject{{"start", 30.0}}, QJsonObject{{"start", 150.0}}};
        titles[0] = first;
        meta["titles"] = titles;
        ProgramEditor editor(meta);
        click(editor, "splitPlaylistChapters");
        QString error;
        const auto p = editor.program(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto episodes = p["episodes"].toArray();
        QCOMPARE(episodes.size(), 4); // Existing whole MPLS plus three candidates.
        QCOMPARE(episodes[1].toObject()["start"].toDouble(), 0.0);
        QCOMPARE(episodes[1].toObject()["end"].toDouble(), 30.0);
        QCOMPARE(episodes[2].toObject()["start"].toDouble(), 30.0);
        QCOMPARE(episodes[2].toObject()["end"].toDouble(), 90.0);
        QCOMPARE(episodes[3].toObject()["start"].toDouble(), 90.0);
        QCOMPARE(episodes[3].toObject()["end"].toDouble(), 0.0);
        QCOMPARE(episodes[3].toObject()["label"].toString(), QStringLiteral("00001.mpls · 章03"));
    }
    void navigationTitlesAreNotPlaylistMappings() {
        auto meta = metadata();
        meta["navigation"] = QJsonObject{{"titleCount", 3}, {"hdmvTitles", 1}, {"bdjTitles", 2}};
        meta["discTitles"] = QJsonArray{QJsonObject{{"number", 99}, {"name", QStringLiteral("菜单目录")}}};
        ProgramEditor editor(meta);
        const auto *navigation = editor.findChild<QLabel *>("navigationInfo");
        QVERIFY(navigation);
        QVERIFY(navigation->text().contains(QStringLiteral("3 个标题")));
        QVERIFY(navigation->toolTip().contains(QStringLiteral("菜单目录")));
        QCOMPARE(editor.findChild<QTableWidget *>("playlistCatalog")->rowCount(), 2);
        QString error;
        const auto p = editor.program(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(p["episodes"].toArray().first().toObject()["playlist"].toInt(), 1);
    }
    void realChapterMetadataNamesRemainEditable() {
        auto meta = metadata();
        auto titles = meta["titles"].toArray();
        auto first = titles[0].toObject();
        auto chapters = first["chapters"].toArray();
        auto chapter = chapters[0].toObject();
        chapter["name"] = QStringLiteral("盘内曲名 A");
        chapters[0] = chapter;
        first["chapters"] = chapters;
        titles[0] = first;
        meta["titles"] = titles;
        ProgramEditor editor(meta);
        QVERIFY(editor.findChild<QComboBox *>("chapterStart")->itemText(0).contains(QStringLiteral("盘内曲名 A")));
        click(editor, "splitPlaylistChapters");
        QCOMPARE(entries(editor)->item(1, 1)->text(), QStringLiteral("盘内曲名 A"));
        entries(editor)->item(1, 1)->setText(QStringLiteral("用户命名"));
        QString error;
        const auto p = editor.program(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(p["episodes"].toArray()[1].toObject()["label"].toString(), QStringLiteral("用户命名"));
    }
    void invalidCannotOverwriteResponse() {
        QTemporaryDir directory;
        const QString path = directory.filePath("response.json");
        QFile original(path);
        QVERIFY(original.open(QIODevice::WriteOnly));
        original.write("preserve-me");
        original.close();
        ProgramEditor editor(metadata());
        entries(editor)->item(0, 3)->setText("151");
        QString error;
        QVERIFY(!editor.saveToFile(path, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("preserve-me"));
        original.close();
        entries(editor)->item(0, 3)->setText("not-a-time");
        QVERIFY(!editor.saveToFile(path, &error));
        QVERIFY(error.contains(QStringLiteral("时间格式")));
        entries(editor)->item(0, 3)->setText("0");
        entries(editor)->item(0, 0)->setCheckState(Qt::Unchecked);
        QVERIFY(!editor.saveToFile(path, &error));
        QVERIFY(!error.isEmpty());
    }
    void atomicSaveRoundtripAndCancel() {
        QTemporaryDir directory;
        ProgramEditor editor(metadata());
        QString error;
        const QString path = directory.filePath("response.json");
        QVERIFY2(editor.saveToFile(path, &error), qPrintable(error));
        QFile output(path);
        QVERIFY(output.open(QIODevice::ReadOnly));
        const auto p = QJsonDocument::fromJson(output.readAll()).object();
        QVERIFY(!p.contains("input"));
        QVERIFY(!p.contains("program"));
        ProgramEditor restored(metadata(), p);
        QCOMPARE(restored.program(&error), p);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        click(restored, "cancelProgram");
        QCOMPARE(restored.result(), int(QDialog::Rejected));
    }
    void syntheticScreenshot() {
        const QString path = qEnvironmentVariable("PROGRAM_EDITOR_SCREENSHOT");
        if (path.isEmpty())
            QSKIP("Optional screenshot destination not configured");
        ProgramEditor editor(metadata());
        auto *table = entries(editor);
        table->selectRow(0);
        click(editor, "duplicateEntry");
        table->item(0, 1)->setText(QStringLiteral("00001.mpls · 片段 A"));
        table->item(0, 4)->setText("00:00:30.000");
        table->item(1, 1)->setText(QStringLiteral("00001.mpls · 片段 B"));
        table->item(1, 3)->setText("00:00:30.000");
        auto *catalog = editor.findChild<QTableWidget *>("playlistCatalog");
        catalog->selectRow(1);
        click(editor, "addPlaylist");
        table->selectRow(1);
        editor.show();
        QTest::qWait(150);
        QVERIFY(editor.grab().save(path));
    }
};
QTEST_MAIN(ProgramEditorTest)
#include "program_editor_test.moc"
