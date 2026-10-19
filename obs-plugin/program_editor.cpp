#include "program_editor.h"
#include "program.h"
#include <QtWidgets>
#include <algorithm>
#include <cmath>

namespace {
QString playlistName(int id) { return QString("%1.mpls").arg(id, 5, 10, QChar('0')); }
QPushButton *button(const QString &text, const char *name, QLayout *layout) {
    auto *result = new QPushButton(text);
    result->setObjectName(name);
    layout->addWidget(result);
    return result;
}
QTableWidgetItem *fixedItem(const QString &text) {
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}
} // namespace

QString ProgramEditor::formatTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0)
        return {};
    const qint64 ms = qRound64(seconds * 1000.0);
    return QString("%1:%2:%3.%4")
        .arg(ms / 3600000, 2, 10, QChar('0'))
        .arg((ms / 60000) % 60, 2, 10, QChar('0'))
        .arg((ms / 1000) % 60, 2, 10, QChar('0'))
        .arg(ms % 1000, 3, 10, QChar('0'));
}
bool ProgramEditor::parseTime(const QString &text, double *seconds) {
    const QString value = text.trimmed();
    const QRegularExpression clock("^(\\d+):([0-5]\\d):([0-5]\\d)(?:\\.(\\d{1,3}))?$");
    const auto match = clock.match(value);
    double result = 0;
    bool ok = false;
    if (match.hasMatch()) {
        result =
            match.captured(1).toDouble(&ok) * 3600.0 + match.captured(2).toInt() * 60.0 + match.captured(3).toInt();
        result += match.captured(4).leftJustified(3, QChar('0')).toInt() / 1000.0;
    } else if (QRegularExpression("^\\d+(?:\\.\\d+)?$").match(value).hasMatch())
        result = value.toDouble(&ok);
    if (!ok || !std::isfinite(result) || result < 0)
        return false;
    if (seconds)
        *seconds = result;
    return true;
}

void ProgramEditor::configureAppearance() {
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    // This editor deliberately uses a light surface. Pair every palette role with
    // it instead of inheriting white text from Windows/OBS dark-mode settings.
    QPalette palette;
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool disabled = group == QPalette::Disabled;
        const QColor text(disabled ? "#58677b" : "#202b3c");
        palette.setColor(group, QPalette::Window, QColor("#f4f6fa"));
        palette.setColor(group, QPalette::WindowText, text);
        palette.setColor(group, QPalette::Base, QColor(disabled ? "#edf1f6" : "#ffffff"));
        palette.setColor(group, QPalette::AlternateBase, QColor("#f7f9fc"));
        palette.setColor(group, QPalette::Text, text);
        palette.setColor(group, QPalette::Button, QColor(disabled ? "#edf1f6" : "#ffffff"));
        palette.setColor(group, QPalette::ButtonText, text);
        palette.setColor(group, QPalette::Highlight, QColor("#d7e7fc"));
        palette.setColor(group, QPalette::HighlightedText, QColor("#18283c"));
        palette.setColor(group, QPalette::PlaceholderText, QColor("#58677b"));
        palette.setColor(group, QPalette::ToolTipBase, QColor("#ffffff"));
        palette.setColor(group, QPalette::ToolTipText, QColor("#202b3c"));
        palette.setColor(group, QPalette::Light, QColor("#ffffff"));
        palette.setColor(group, QPalette::Midlight, QColor("#eaf0f7"));
        palette.setColor(group, QPalette::Mid, QColor("#cbd5e3"));
        palette.setColor(group, QPalette::Dark, QColor("#8797ad"));
        palette.setColor(group, QPalette::Shadow, QColor("#58677b"));
        palette.setColor(group, QPalette::Accent, QColor("#2563b8"));
    }
    // Set the helper application's palette as well: stylesheet children, popup
    // lists and transient dialogs otherwise fall back to the system palette.
    QApplication::setPalette(palette);
}

ProgramEditor::ProgramEditor(const QJsonObject &metadata, const QJsonObject &program, QWidget *parent)
    : QDialog(parent), metadata_(metadata), original_(program) {
    setWindowTitle(QStringLiteral("蓝光节目单 · OBS Blu-ray"));
    setWindowIcon(QIcon(QStringLiteral(":/brand/obs-bluray.svg")));
    resize(1220, 870);
    setMinimumSize(980, 720);
    setStyleSheet(QStringLiteral(
        "QWidget{color:#202b3c} QDialog{background:#f4f6fa} QGroupBox{font-weight:600;border:1px solid "
        "#d9e0eb;border-radius:8px;margin-top:12px;padding:12px} "
        "QGroupBox::title{subcontrol-origin:margin;left:12px;padding:0 5px} "
        "QTableWidget{background:white;alternate-background-color:#f7f9fc;gridline-color:#e4e9f1;selection-background-"
        "color:#d7e7fc;selection-color:#18283c;border:1px solid #d9e0eb;border-radius:4px} "
        "QHeaderView::section{background:#eaf0f7;padding:7px;border:0;border-bottom:1px solid #d9e0eb} "
        "QPushButton{padding:7px 12px;background:white;border:1px solid #cbd5e3;border-radius:5px} "
        "QPushButton:hover{background:#e8f1ff} QPushButton:pressed{background:#d7e7fc} "
        "QPushButton:disabled,QLineEdit:disabled,QComboBox:disabled,QDoubleSpinBox:disabled{"
        "color:#58677b;background:#edf1f6} "
        "QLineEdit,QComboBox,QDoubleSpinBox{background:white;padding:5px;border:1px solid #cbd5e3;border-radius:4px} "
        "QLineEdit:focus,QComboBox:focus,QDoubleSpinBox:focus,QPushButton:focus{border:1px solid #2563b8} "
        "QComboBox QAbstractItemView{color:#202b3c;background:white;selection-background-color:#d7e7fc;"
        "selection-color:#18283c;border:1px solid #8797ad} "
        "QToolTip{color:#202b3c;background:white;border:1px solid #8797ad;padding:4px} "
        "QLabel#hint{color:#52647c} "
        "QPushButton#saveProgram{background:#2563b8;color:white;border-color:#2563b8;font-weight:600}"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(10);
    auto *heading = new QLabel(QStringLiteral("编排蓝光节目单"));
    auto headingFont = heading->font();
    headingFont.setPointSize(17);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);
    auto *hint = new QLabel(
        QStringLiteral("选择 MPLS 整段或自定义范围；章节仅用于定位，不自动判断剧集。保存后由 OBS 应用配置。"));
    hint->setObjectName("hint");
    hint->setWordWrap(true);
    root->addWidget(hint);
    const auto navigation = metadata["navigation"].toObject();
    if (!navigation.isEmpty()) {
        auto *navigationInfo =
            new QLabel(QStringLiteral("盘内导航：%1 个标题（HDMV %2 / BD-J %3）。菜单名称不一定对应 MPLS。")
                           .arg(navigation["titleCount"].toInt())
                           .arg(navigation["hdmvTitles"].toInt())
                           .arg(navigation["bdjTitles"].toInt()));
        navigationInfo->setObjectName("navigationInfo");
        navigationInfo->setWordWrap(true);
        QStringList names;
        for (const auto &value : metadata["discTitles"].toArray()) {
            const auto object = value.toObject();
            const QString name =
                value.isString() ? value.toString() : object["name"].toString(object["title"].toString());
            if (!name.isEmpty())
                names.append(name);
        }
        navigationInfo->setToolTip(names.isEmpty()
                                       ? QStringLiteral("盘内导航标题与 MPLS 播放列表分别展示，不推断二者映射。")
                                       : names.join('\n'));
        root->addWidget(navigationInfo);
    }
    auto *split = new QSplitter;
    root->addWidget(split, 3);
    auto *catalogBox = new QGroupBox(QStringLiteral("光盘播放列表"));
    auto *catalogLayout = new QVBoxLayout(catalogBox);
    split->addWidget(catalogBox);
    titles_ = new QTableWidget(0, 4);
    titles_->setObjectName("playlistCatalog");
    titles_->setHorizontalHeaderLabels(
        {QStringLiteral("MPLS 文件"), QStringLiteral("时长"), QStringLiteral("章节"), QStringLiteral("PGS")});
    titles_->setSelectionBehavior(QAbstractItemView::SelectRows);
    titles_->setSelectionMode(QAbstractItemView::SingleSelection);
    titles_->setAlternatingRowColors(true);
    titles_->verticalHeader()->hide();
    titles_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    titles_->horizontalHeader()->setStretchLastSection(true);
    catalogLayout->addWidget(titles_);
    auto *add = button(QStringLiteral("添加整个 MPLS →"), "addPlaylist", catalogLayout);
    auto *splitChapters = button(QStringLiteral("按当前 MPLS 章节拆分 →"), "splitPlaylistChapters", catalogLayout);
    splitChapters->setToolTip(QStringLiteral("按章节起点添加候选条目，保留现有节目单；章节不一定代表一集或一首歌。"));
    auto *programBox = new QGroupBox(QStringLiteral("节目顺序与时间范围"));
    auto *programLayout = new QVBoxLayout(programBox);
    split->addWidget(programBox);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 5);
    entries_ = new QTableWidget(0, 5);
    entries_->setObjectName("programEntries");
    entries_->setHorizontalHeaderLabels({QStringLiteral("启用"), QStringLiteral("名称"), QStringLiteral("MPLS"),
                                         QStringLiteral("起点"), QStringLiteral("终点")});
    entries_->setSelectionBehavior(QAbstractItemView::SelectRows);
    entries_->setSelectionMode(QAbstractItemView::SingleSelection);
    entries_->setAlternatingRowColors(true);
    entries_->verticalHeader()->hide();
    entries_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    entries_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    programLayout->addWidget(entries_);
    auto *actions = new QHBoxLayout;
    programLayout->addLayout(actions);
    auto *duplicate = button(QStringLiteral("复制条目"), "duplicateEntry", actions);
    auto *up = button(QStringLiteral("上移"), "moveEntryUp", actions);
    auto *down = button(QStringLiteral("下移"), "moveEntryDown", actions);
    auto *remove = button(QStringLiteral("删除"), "removeEntry", actions);
    actions->addStretch();
    auto *chapterRow = new QHBoxLayout;
    programLayout->addLayout(chapterRow);
    chapterRow->addWidget(new QLabel(QStringLiteral("按章选范围")));
    chapterStart_ = new QComboBox;
    chapterStart_->setObjectName("chapterStart");
    chapterEnd_ = new QComboBox;
    chapterEnd_->setObjectName("chapterEnd");
    chapterRow->addWidget(chapterStart_, 1);
    chapterRow->addWidget(new QLabel(QStringLiteral("至")));
    chapterRow->addWidget(chapterEnd_, 1);
    auto *useChapters = button(QStringLiteral("应用到选中条目"), "applyChapters", chapterRow);
    auto *rangeHint =
        new QLabel(QStringLiteral("双击名称、起点或终点编辑。时间支持 HH:mm:ss.zzz 或秒；终点不包含在播放范围内。"));
    rangeHint->setObjectName("hint");
    rangeHint->setWordWrap(true);
    programLayout->addWidget(rangeHint);
    auto *playRow = new QHBoxLayout;
    programLayout->addLayout(playRow);
    playRow->addWidget(new QLabel(QStringLiteral("播放方式")));
    mode_ = new QComboBox;
    mode_->setObjectName("playMode");
    mode_->addItem(QStringLiteral("顺序播放"), "sequence");
    mode_->addItem(QStringLiteral("仅播放起播集"), "single");
    playRow->addWidget(mode_);
    playRow->addWidget(new QLabel(QStringLiteral("起播集")));
    start_ = new QComboBox;
    start_->setObjectName("startEntry");
    playRow->addWidget(start_, 1);
    loop_ = new QCheckBox(QStringLiteral("循环"));
    loop_->setObjectName("loopProgram");
    playRow->addWidget(loop_);
    auto *subBox = new QGroupBox(QStringLiteral("每个 MPLS 的字幕设置"));
    auto *subLayout = new QGridLayout(subBox);
    root->addWidget(subBox);
    subtitlePlaylist_ = new QComboBox;
    subtitlePlaylist_->setObjectName("subtitlePlaylist");
    subtitleMode_ = new QComboBox;
    subtitleMode_->setObjectName("subtitleMode");
    subtitleMode_->addItem(QStringLiteral("盘内 PGS（自动）"), "auto");
    subtitleMode_->addItem(QStringLiteral("盘内 PGS（指定轨道）"), "pgs");
    subtitleMode_->addItem(QStringLiteral("外挂 ASS / SSA / SRT"), "external");
    subtitleMode_->addItem(QStringLiteral("关闭字幕"), "off");
    subtitleTrack_ = new QComboBox;
    subtitleTrack_->setObjectName("subtitleTrack");
    subLayout->addWidget(new QLabel(QStringLiteral("MPLS")), 0, 0);
    subLayout->addWidget(subtitlePlaylist_, 0, 1);
    subLayout->addWidget(new QLabel(QStringLiteral("模式")), 0, 2);
    subLayout->addWidget(subtitleMode_, 0, 3);
    subLayout->addWidget(subtitleTrack_, 0, 4, 1, 2);
    subtitleFile_ = new QLineEdit;
    subtitleFile_->setObjectName("subtitleFile");
    subtitleFile_->setPlaceholderText(QStringLiteral("选择外挂字幕，按原 MPLS 的时间轴对齐"));
    auto *browseFile = new QPushButton(QStringLiteral("选择字幕…"));
    browseFile->setObjectName("browseSubtitle");
    subLayout->addWidget(new QLabel(QStringLiteral("外挂字幕")), 1, 0);
    subLayout->addWidget(subtitleFile_, 1, 1, 1, 4);
    subLayout->addWidget(browseFile, 1, 5);
    subtitleOffset_ = new QDoubleSpinBox;
    subtitleOffset_->setObjectName("subtitleOffset");
    subtitleOffset_->setRange(-86400, 86400);
    subtitleOffset_->setDecimals(3);
    subtitleOffset_->setSuffix(QStringLiteral(" 秒"));
    fontDirectory_ = new QLineEdit;
    fontDirectory_->setObjectName("subtitleFonts");
    auto *browseFonts = new QPushButton(QStringLiteral("选择字体目录…"));
    browseFonts->setObjectName("browseFonts");
    subLayout->addWidget(new QLabel(QStringLiteral("字幕偏移")), 2, 0);
    subLayout->addWidget(subtitleOffset_, 2, 1);
    subLayout->addWidget(new QLabel(QStringLiteral("字体目录")), 2, 2);
    subLayout->addWidget(fontDirectory_, 2, 3, 1, 2);
    subLayout->addWidget(browseFonts, 2, 5);
    auto *subHint = new QLabel(QStringLiteral(
        "同一 MPLS 的所有片段共用这一套字幕。片段起点改变时，外挂字幕时间轴不会归零；正偏移使字幕延后。"));
    subHint->setObjectName("hint");
    subHint->setWordWrap(true);
    subLayout->addWidget(subHint, 3, 0, 1, 6);
    status_ = new QLabel;
    status_->setObjectName("editorStatus");
    status_->setWordWrap(true);
    root->addWidget(status_);
    auto *footer = new QHBoxLayout;
    root->addLayout(footer);
    footer->addStretch();
    auto *cancel = button(QStringLiteral("取消"), "cancelProgram", footer);
    auto *save = button(QStringLiteral("保存节目单"), "saveProgram", footer);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, [this] {
        QString error;
        this->program(&error);
        if (!error.isEmpty()) {
            status_->setText(error);
            status_->setStyleSheet("color:#b42318");
            return;
        }
        accept();
    });
    connect(add, &QPushButton::clicked, this, [this] { addSelectedPlaylist(); });
    connect(splitChapters, &QPushButton::clicked, this, [this] { addChapterCandidates(); });
    connect(titles_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { addSelectedPlaylist(); });
    connect(duplicate, &QPushButton::clicked, this, [this] {
        const int row = entries_->currentRow();
        if (row < 0)
            return;
        QJsonObject item{{"enabled", entries_->item(row, 0)->checkState() == Qt::Checked},
                         {"label", entries_->item(row, 1)->text()},
                         {"playlist", entries_->item(row, 2)->data(Qt::UserRole).toInt()}};
        appendEntry(item);
        const int last = entries_->rowCount() - 1;
        entries_->item(last, 3)->setText(entries_->item(row, 3)->text());
        entries_->item(last, 4)->setText(entries_->item(row, 4)->text());
    });
    connect(up, &QPushButton::clicked, this, [this] { moveEntry(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveEntry(1); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = entries_->currentRow();
        if (row >= 0) {
            entries_->removeRow(row);
            refreshStartItems();
            refreshChapters();
        }
    });
    connect(entries_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (!loading_ && (item->column() == 0 || item->column() == 1))
            refreshStartItems();
    });
    connect(entries_, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) { refreshChapters(); });
    connect(useChapters, &QPushButton::clicked, this, [this] {
        const int row = entries_->currentRow();
        if (row < 0 || chapterStart_->currentIndex() < 0 || chapterEnd_->currentIndex() < 0)
            return;
        entries_->item(row, 3)->setText(formatTime(chapterStart_->currentData().toDouble()));
        entries_->item(row, 4)->setText(formatTime(chapterEnd_->currentData().toDouble()));
    });
    connect(subtitlePlaylist_, &QComboBox::currentIndexChanged, this, [this] {
        if (!loading_) {
            storeSubtitle();
            loadSubtitle(subtitlePlaylist_->currentData().toInt());
        }
    });
    connect(subtitleMode_, &QComboBox::currentIndexChanged, this, [this] { updateSubtitleControls(); });
    connect(browseFile, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getOpenFileName(this, QStringLiteral("选择外挂字幕"), subtitleFile_->text(),
                                                       QStringLiteral("字幕文件 (*.ass *.ssa *.srt)"));
        if (!file.isEmpty())
            subtitleFile_->setText(file);
    });
    connect(browseFonts, &QPushButton::clicked, this, [this] {
        const auto path =
            QFileDialog::getExistingDirectory(this, QStringLiteral("选择字幕字体目录"), fontDirectory_->text());
        if (!path.isEmpty())
            fontDirectory_->setText(path);
    });
    loading_ = true;
    for (const auto &value : metadata_["titles"].toArray()) {
        const auto t = value.toObject();
        const int id = t["playlist"].toInt();
        const int row = titles_->rowCount();
        titles_->insertRow(row);
        titles_->setItem(row, 0, fixedItem(playlistName(id)));
        titles_->item(row, 0)->setData(Qt::UserRole, id);
        titles_->setItem(row, 1, fixedItem(formatTime(t["duration"].toDouble())));
        titles_->setItem(row, 2, fixedItem(QString::number(t["chapters"].toArray().size())));
        titles_->setItem(row, 3, fixedItem(QString::number(t["subtitles"].toArray().size())));
        subtitlePlaylist_->addItem(playlistName(id), id);
    }
    const auto initial = program.isEmpty() ? sd::createDefaultProgram(metadata).program : program;
    for (const auto &value : initial["subtitles"].toArray()) {
        const auto sub = value.toObject();
        subtitles_[QString::number(sub["playlist"].toInt())] = sub;
    }
    for (const auto &value : initial["episodes"].toArray())
        appendEntry(value.toObject());
    mode_->setCurrentIndex(qMax(0, mode_->findData(initial["playMode"].toString("sequence"))));
    loop_->setChecked(initial["loop"].toBool());
    loading_ = false;
    refreshStartItems();
    start_->setCurrentIndex(qMax(0, start_->findData(initial["selectedEpisode"].toString())));
    if (titles_->rowCount())
        titles_->selectRow(0);
    if (entries_->rowCount())
        entries_->selectRow(0);
    if (subtitlePlaylist_->count())
        loadSubtitle(subtitlePlaylist_->currentData().toInt());
    split->setSizes({340, 800});
}

QJsonObject ProgramEditor::title(int playlist) const {
    for (const auto &value : metadata_["titles"].toArray())
        if (value.toObject()["playlist"].toInt() == playlist)
            return value.toObject();
    return {};
}
void ProgramEditor::appendEntry(const QJsonObject &entry) {
    const bool previous = loading_;
    loading_ = true;
    const int id = entry["playlist"].toInt();
    const auto t = title(id);
    const int row = entries_->rowCount();
    entries_->insertRow(row);
    auto *enabled = fixedItem("");
    enabled->setFlags(enabled->flags() | Qt::ItemIsUserCheckable);
    enabled->setCheckState(entry["enabled"].toBool(true) ? Qt::Checked : Qt::Unchecked);
    enabled->setData(Qt::UserRole, entry["id"].toString(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    entries_->setItem(row, 0, enabled);
    entries_->setItem(row, 1, new QTableWidgetItem(entry["label"].toString(playlistName(id))));
    entries_->setItem(row, 2, fixedItem(playlistName(id)));
    entries_->item(row, 2)->setData(Qt::UserRole, id);
    entries_->setItem(row, 3, new QTableWidgetItem(formatTime(entry["start"].toDouble())));
    const double finish = entry["end"].toDouble();
    auto *endItem = new QTableWidgetItem(formatTime(finish == 0 ? t["duration"].toDouble() : finish));
    if (finish == 0) {
        endItem->setData(Qt::UserRole, endItem->text());
        endItem->setToolTip(QStringLiteral("跟随 MPLS 片尾；修改后使用固定终点"));
    }
    entries_->setItem(row, 4, endItem);
    loading_ = previous;
    if (!loading_)
        refreshStartItems();
    entries_->selectRow(row);
}
void ProgramEditor::addSelectedPlaylist() {
    const int row = titles_->currentRow();
    if (row < 0)
        return;
    const int id = titles_->item(row, 0)->data(Qt::UserRole).toInt();
    appendEntry({{"playlist", id}, {"label", playlistName(id)}, {"enabled", true}, {"start", 0}, {"end", 0}});
}
void ProgramEditor::addChapterCandidates() {
    const int row = titles_->currentRow();
    if (row < 0)
        return;
    const int playlist = titles_->item(row, 0)->data(Qt::UserRole).toInt();
    const auto info = title(playlist);
    const double duration = info["duration"].toDouble();
    QList<double> boundaries;
    QMap<double, QString> names;
    for (const auto &chapter : info["chapters"].toArray()) {
        const double seconds = chapter.toObject()["start"].toDouble(-1);
        if (std::isfinite(seconds) && seconds >= 0 && seconds < duration && !boundaries.contains(seconds)) {
            boundaries.append(seconds);
            names.insert(seconds, chapter.toObject()["name"].toString().trimmed());
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    for (qsizetype i = 0; i < boundaries.size(); ++i) {
        appendEntry(
            {{"playlist", playlist},
             {"enabled", true},
             {"label", names.value(boundaries[i]).isEmpty()
                           ? QStringLiteral("%1 · 章%2").arg(playlistName(playlist)).arg(i + 1, 2, 10, QChar('0'))
                           : names.value(boundaries[i])},
             {"start", boundaries[i]},
             {"end", i + 1 < boundaries.size() ? boundaries[i + 1] : 0}});
    }
    status_->setStyleSheet("color:#47617e");
    status_->setText(
        boundaries.isEmpty()
            ? QStringLiteral("当前 MPLS 没有可用的章节起点。")
            : QStringLiteral("已添加 %1 个章节候选，现有条目已保留。请自行确认范围与名称；章节不等于剧集或歌曲。")
                  .arg(boundaries.size()));
}
void ProgramEditor::moveEntry(int delta) {
    const int row = entries_->currentRow(), other = row + delta;
    if (row < 0 || other < 0 || other >= entries_->rowCount())
        return;
    loading_ = true;
    for (int c = 0; c < entries_->columnCount(); ++c) {
        auto *a = entries_->takeItem(row, c), *b = entries_->takeItem(other, c);
        entries_->setItem(row, c, b);
        entries_->setItem(other, c, a);
    }
    loading_ = false;
    refreshStartItems();
    entries_->selectRow(other);
    refreshChapters();
}
void ProgramEditor::refreshStartItems() {
    const QString selected = start_->currentData().toString();
    QSignalBlocker guard(start_);
    start_->clear();
    for (int i = 0; i < entries_->rowCount(); ++i)
        if (entries_->item(i, 0) && entries_->item(i, 0)->checkState() == Qt::Checked)
            start_->addItem(QStringLiteral("%1 · %2").arg(i + 1).arg(entries_->item(i, 1)->text()),
                            entries_->item(i, 0)->data(Qt::UserRole));
    const int index = start_->findData(selected);
    start_->setCurrentIndex(index < 0 ? 0 : index);
}
void ProgramEditor::refreshChapters() {
    chapterStart_->clear();
    chapterEnd_->clear();
    const int row = entries_->currentRow();
    if (row < 0 || !entries_->item(row, 2))
        return;
    const auto t = title(entries_->item(row, 2)->data(Qt::UserRole).toInt());
    int n = 0;
    for (const auto &v : t["chapters"].toArray()) {
        const double seconds = v.toObject()["start"].toDouble();
        const auto name = v.toObject()["name"].toString().trimmed();
        const QString label = QStringLiteral("章 %1 · %2%3")
                                  .arg(++n)
                                  .arg(name.isEmpty() ? QString() : name + " · ")
                                  .arg(formatTime(seconds));
        chapterStart_->addItem(label, seconds);
        chapterEnd_->addItem(label, seconds);
    }
    chapterEnd_->addItem(QStringLiteral("MPLS 结束 · %1").arg(formatTime(t["duration"].toDouble())),
                         t["duration"].toDouble());
    chapterEnd_->setCurrentIndex(chapterEnd_->count() - 1);
}
void ProgramEditor::storeSubtitle() {
    if (currentSubtitlePlaylist_ < 0)
        return;
    subtitles_[QString::number(currentSubtitlePlaylist_)] = QJsonObject{
        {"mode", subtitleMode_->currentData().toString() == "auto" || subtitleMode_->currentData().toString() == "pgs"
                     ? "internal"
                     : subtitleMode_->currentData().toString()},
        {"playlist", currentSubtitlePlaylist_},
        {"track", subtitleMode_->currentData().toString() == "auto" || subtitleTrack_->currentIndex() < 0
                      ? -1
                      : subtitleTrack_->currentData().toInt()},
        {"file", subtitleFile_->text().trimmed()},
        {"offset", subtitleOffset_->value()},
        {"fontsDir", fontDirectory_->text().trimmed()}};
}
void ProgramEditor::loadSubtitle(int playlist) {
    loading_ = true;
    currentSubtitlePlaylist_ = playlist;
    const auto setting = subtitles_[QString::number(playlist)].toObject();
    const QString storedMode = setting["mode"].toString("internal");
    const QString displayedMode =
        storedMode == "internal" ? (setting["track"].toInt(-1) < 0 ? "auto" : "pgs") : storedMode;
    subtitleMode_->setCurrentIndex(qMax(0, subtitleMode_->findData(displayedMode)));
    subtitleTrack_->clear();
    for (const auto &value : title(playlist)["subtitles"].toArray()) {
        const auto track = value.toObject();
        const int index = track["index"].toInt();
        subtitleTrack_->addItem(QStringLiteral("PGS %1 · %2 · PID %3")
                                    .arg(index + 1)
                                    .arg(track["language"].toString(QStringLiteral("未知")))
                                    .arg(track["pid"].toInt()),
                                index);
    }
    subtitleTrack_->setCurrentIndex(qMax(0, subtitleTrack_->findData(setting["track"].toInt())));
    subtitleFile_->setText(setting["file"].toString());
    subtitleOffset_->setValue(setting["offset"].toDouble());
    fontDirectory_->setText(setting["fontsDir"].toString());
    loading_ = false;
    updateSubtitleControls();
}
void ProgramEditor::updateSubtitleControls() {
    const auto mode = subtitleMode_->currentData().toString();
    subtitleTrack_->setEnabled(mode == "pgs");
    for (QWidget *widget :
         QList<QWidget *>{static_cast<QWidget *>(subtitleFile_), static_cast<QWidget *>(fontDirectory_),
                          static_cast<QWidget *>(subtitleOffset_), findChild<QPushButton *>("browseSubtitle"),
                          findChild<QPushButton *>("browseFonts")})
        if (widget)
            widget->setEnabled(mode == "external");
}
QJsonObject ProgramEditor::program(QString *error) {
    if (error)
        error->clear();
    storeSubtitle();
    QJsonArray entries;
    for (int row = 0; row < entries_->rowCount(); ++row) {
        double begin = 0, end = 0;
        if (!parseTime(entries_->item(row, 3)->text(), &begin) || !parseTime(entries_->item(row, 4)->text(), &end)) {
            if (error)
                *error = QStringLiteral("第 %1 项时间格式无效，请输入 HH:mm:ss.zzz 或非负秒数。").arg(row + 1);
            return {};
        }
        const QString name = entries_->item(row, 1)->text().trimmed();
        const int playlist = entries_->item(row, 2)->data(Qt::UserRole).toInt();
        const auto discTitle = title(playlist);
        const bool followsEnd = entries_->item(row, 4)->data(Qt::UserRole).toString() == entries_->item(row, 4)->text();
        if (name.isEmpty()) {
            if (error)
                *error = QStringLiteral("第 %1 项名称不能为空。").arg(row + 1);
            return {};
        }
        if (discTitle.isEmpty() || begin >= end || end > discTitle["duration"].toDouble() + 0.0005) {
            if (error)
                *error = QStringLiteral("第 %1 项范围无效：起点必须早于终点，且范围不能超出 MPLS 时长。").arg(row + 1);
            return {};
        }
        entries.append(QJsonObject{{"enabled", entries_->item(row, 0)->checkState() == Qt::Checked},
                                   {"id", entries_->item(row, 0)->data(Qt::UserRole).toString()},
                                   {"label", entries_->item(row, 1)->text().trimmed()},
                                   {"playlist", entries_->item(row, 2)->data(Qt::UserRole).toInt()},
                                   {"start", begin},
                                   {"end", followsEnd ? 0 : end}});
    }
    if (start_->count() == 0) {
        if (error)
            *error = QStringLiteral("节目单至少需要一个启用的条目。");
        return {};
    }
    for (auto it = subtitles_.constBegin(); it != subtitles_.constEnd(); ++it) {
        const auto sub = it.value().toObject();
        if (sub["mode"].toString() == "external" && sub["file"].toString().isEmpty()) {
            if (error)
                *error = QStringLiteral("%1 选择了外挂字幕，但尚未选择字幕文件。")
                             .arg(playlistName(sub["playlist"].toInt()));
            return {};
        }
    }
    auto result = original_;
    result["version"] = 1;
    result["episodes"] = entries;
    result["playMode"] = mode_->currentData().toString();
    result["selectedEpisode"] = start_->currentData().toString();
    result["loop"] = loop_->isChecked();
    QJsonArray subtitleSettings;
    for (auto it = subtitles_.constBegin(); it != subtitles_.constEnd(); ++it)
        subtitleSettings.append(it.value());
    result["subtitles"] = subtitleSettings;
    // The shared worker model is authoritative for range and track validation.
    const auto normalized = sd::normalizeProgram(result, metadata_);
    if (!normalized.ok()) {
        if (error)
            *error = QStringLiteral("节目单校验未通过：%1").arg(normalized.error);
        return {};
    }
    return normalized.program;
}
bool ProgramEditor::saveToFile(const QString &path, QString *error) {
    QString validation;
    const auto result = program(&validation);
    if (!validation.isEmpty()) {
        if (error)
            *error = validation;
        return false;
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法创建响应文件：%1").arg(output.errorString());
        return false;
    }
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    if (output.write(bytes) != bytes.size() || !output.commit()) {
        if (error)
            *error = QStringLiteral("无法原子保存响应文件：%1").arg(output.errorString());
        return false;
    }
    return true;
}
