#include "catalog.h"
#include "program_editor.h"
#include <QtWidgets>
#include <windows.h>

int main(int argc, char **argv) {
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    const auto directory = QFileInfo(QString::fromWCharArray(executable, int(length))).absolutePath();
    QCoreApplication::setLibraryPaths({directory});
    qputenv("QT_PLUGIN_PATH", directory.toUtf8());
    qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", (directory + "/platforms").toUtf8());
    qputenv("QT_QPA_PLATFORM", "windows");
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(QStringLiteral(":/brand/obs-bluray.svg")));
    app.setApplicationName("OBS Blu-ray Program Editor");
    app.setOrganizationName("StreamDesk");
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("独立蓝光节目单编辑器"));
    parser.addHelpOption();
    parser.addOption({"input", "Request JSON file", "file"});
    parser.addOption({"output", "Response JSON file", "file"});
    parser.process(app);
    if (!parser.isSet("input") || !parser.isSet("output"))
        return 2;
    try {
        QFile input(parser.value("input"));
        if (!input.open(QIODevice::ReadOnly))
            return 2;
        QJsonParseError jsonError;
        const auto document = QJsonDocument::fromJson(input.readAll(), &jsonError);
        if (jsonError.error != QJsonParseError::NoError || !document.isObject())
            return 2;
        const auto request = document.object();
        // Explicit synthetic metadata lets GUI tests avoid opening private media.
        QJsonObject metadata;
        if (request.contains("metadata")) {
            metadata = request["metadata"].toObject();
        } else {
            sd::setupRuntime();
            metadata = sd::inspectSource(request["input"].toString());
        }
        if (!metadata["ok"].toBool() || metadata["kind"].toString() != "bdmv") {
            QMessageBox::critical(nullptr, QStringLiteral("无法读取光盘"),
                                  metadata["error"].toString(QStringLiteral("请选择完整 BDMV 光盘目录。")));
            return 2;
        }
        ProgramEditor editor(metadata, request["program"].toObject());
        if (editor.exec() != QDialog::Accepted)
            return 1;
        QString error;
        if (!editor.saveToFile(parser.value("output"), &error)) {
            QMessageBox::critical(nullptr, QStringLiteral("保存失败"), error);
            return 2;
        }
        return 0;
    } catch (const std::exception &) {
        return 2;
    }
}
