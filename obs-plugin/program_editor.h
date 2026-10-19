#pragma once
#include <QDialog>
#include <QJsonObject>

class QTableWidget;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QDoubleSpinBox;
class QLabel;

// A separate process owns this widget; the OBS module never loads Qt Widgets.
class ProgramEditor final : public QDialog {
  public:
    // Initialize the isolated helper process before creating any widgets.
    static void configureAppearance();
    explicit ProgramEditor(const QJsonObject &metadata, const QJsonObject &program = {}, QWidget *parent = nullptr);
    QJsonObject program(QString *error = nullptr);
    bool saveToFile(const QString &path, QString *error = nullptr);
    static bool parseTime(const QString &text, double *seconds);
    static QString formatTime(double seconds);

  private:
    QJsonObject title(int playlist) const;
    void appendEntry(const QJsonObject &entry);
    void addSelectedPlaylist();
    void addChapterCandidates();
    void moveEntry(int delta);
    void refreshStartItems();
    void refreshChapters();
    void storeSubtitle();
    void loadSubtitle(int playlist);
    void updateSubtitleControls();
    QJsonObject metadata_, original_, subtitles_;
    QTableWidget *titles_{}, *entries_{};
    QComboBox *mode_{}, *start_{}, *chapterStart_{}, *chapterEnd_{}, *subtitlePlaylist_{}, *subtitleMode_{},
        *subtitleTrack_{};
    QCheckBox *loop_{};
    QLineEdit *subtitleFile_{}, *fontDirectory_{};
    QDoubleSpinBox *subtitleOffset_{};
    QLabel *status_{};
    int currentSubtitlePlaylist_ = -1;
    bool loading_ = false;
};
