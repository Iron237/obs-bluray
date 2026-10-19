#include "common.h"
#include <windows.h>
#include <wincrypt.h>
#include <stdexcept>
namespace sd {
QString dataRoot() {
    QString path = qEnvironmentVariable("STREAMDESK_DATA");
    if (path.isEmpty()) path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/studio-v3";
    QDir().mkpath(path);
    return path;
}
QString serverName() {
    return "StreamDesk-" + QString::fromLatin1(QCryptographicHash::hash(dataRoot().toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}
QJsonObject readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
    QJsonParseError err; auto doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) throw std::runtime_error("Invalid JSON object");
    return doc.object();
}
void saveJson(const QString &path, const QJsonObject &value, bool protect) {
    QByteArray bytes = QJsonDocument(value).toJson();
    if (protect) {
        DATA_BLOB input{DWORD(bytes.size()), reinterpret_cast<BYTE *>(bytes.data())}, output{};
        if (!CryptProtectData(&input, L"StreamDesk", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
            throw std::runtime_error("Windows DPAPI encryption failed");
        bytes = QByteArray(reinterpret_cast<char *>(output.pbData), int(output.cbData)); LocalFree(output.pbData);
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save configuration atomically");
}
QJsonObject loadProtected(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read encrypted configuration");
    auto bytes = file.readAll();
    DATA_BLOB input{DWORD(bytes.size()), reinterpret_cast<BYTE *>(bytes.data())}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Windows DPAPI decryption failed");
    bytes = QByteArray(reinterpret_cast<char *>(output.pbData), int(output.cbData)); LocalFree(output.pbData);
    QJsonParseError error; auto doc = QJsonDocument::fromJson(bytes, &error);
    if (!doc.isObject() || error.error != QJsonParseError::NoError) throw std::runtime_error("Corrupt configuration");
    return doc.object();
}
QString redact(QString text) {
    static QRegularExpression url(R"((?:rtmps?|rtsp|srt|udp|tcp|https?)://[^\s\"'<>]+)", QRegularExpression::CaseInsensitiveOption);
    text.replace(url, "[network destination hidden]");
    return text;
}
void printJson(const QJsonObject &object) {
    QFile out; out.open(stdout, QIODevice::WriteOnly); out.write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n'); out.flush();
}
}
