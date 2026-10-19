#include "audio_probe.h"
#include "audio_info.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLibrary>
#include <QtCore/QRegularExpression>
#include <cmath>
#include <memory>

namespace sd {
namespace {
constexpr qsizetype MaximumHead = 4 * 1024 * 1024;
int mediaPid(const QJsonObject &stream) {
    // MediaInfo MPEG-TS ID is PID, optionally followed by a substream ID.
    // ID_String is its decimal/hex presentation, not a stream index.
    const auto value = stream.value("ID");
    if (value.isDouble()) {
        const double number = value.toDouble();
        return std::isfinite(number) && std::floor(number) == number && number >= 0 && number < 8192 ? int(number) : -1;
    }
    static const QRegularExpression idPattern("^\\s*(0[xX][0-9A-Fa-f]+|[0-9]+)(?:-[0-9]+)?(?:\\s*\\([^)]*\\))?\\s*$");
    const auto match = idPattern.match(value.toString());
    if (!match.hasMatch()) return -1;
    const auto text = match.captured(1);
    bool ok = false;
    const int pid = text.startsWith("0x", Qt::CaseInsensitive) ? text.mid(2).toInt(&ok, 16) : text.toInt(&ok, 10);
    return ok && pid >= 0 && pid < 8192 ? pid : -1;
}
double positiveNumber(const QJsonValue &value) {
    bool ok = value.isDouble();
    const double number = value.isString() ? value.toString().toDouble(&ok) : value.toDouble();
    return ok && std::isfinite(number) && number > 0 ? number : 0;
}
bool compatible(int coding, const QString &format) {
    const auto value = format.toUpper();
    switch (coding) {
    case 0x80: return false; // Preserve the exact LPCM header measurements.
    case 0x81: return value == "AC-3";
    case 0x83: return value.startsWith("MLP FBA") || value.contains("TRUEHD");
    case 0x84: case 0xa1: return value.startsWith("E-AC-3");
    case 0x82: case 0x85: case 0x86: case 0xa2: return value.startsWith("DTS");
    case 0x03: case 0x04: return value.startsWith("MPEG AUDIO");
    default: return false;
    }
}
QString atmosEvidence(const QJsonObject &stream, int coding) {
    if (coding != 0x81 && coding != 0x83 && coding != 0x84 && coding != 0xa1) return {};
    static const QRegularExpression atmos("\\bDolby\\s+Atmos\\b", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression joc("\\b(?:E-AC-3|AC-3)\\s+JOC\\b", QRegularExpression::CaseInsensitiveOption);
    for (const auto &key : {"Format_Commercial_IfAny", "Format_Commercial", "Format_Profile", "Format_AdditionalFeatures"}) {
        const auto field = stream.value(key).toString();
        if (atmos.match(field).hasMatch() || joc.match(field).hasMatch()) return QString::fromLatin1(key) + ": " + field;
    }
    // Format_AdditionalFeatures is a token list in some MediaInfo versions.
    if ((coding == 0x84 || coding == 0xa1 || coding == 0x81) &&
        QRegularExpression("(?:^|[ /,])JOC(?:$|[ /,])", QRegularExpression::CaseInsensitiveOption)
            .match(stream.value("Format_AdditionalFeatures").toString()).hasMatch())
        return "Format_AdditionalFeatures: " + stream.value("Format_AdditionalFeatures").toString();
    return {};
}
QJsonObject readBuffer(const QByteArray &head, qint64 fileSize) {
#ifdef Q_OS_WIN
    if (head.isEmpty() || !QCoreApplication::instance()) return {};
    const auto libraryPath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("MediaInfo.dll");
    if (!QFileInfo::exists(libraryPath)) return {};
    QLibrary library(libraryPath);
    if (!library.load()) return {};
    using New = void *(__stdcall *)();
    using Delete = void (__stdcall *)(void *);
    using Option = const wchar_t *(__stdcall *)(void *, const wchar_t *, const wchar_t *);
    using Init = size_t (__stdcall *)(void *, quint64, quint64);
    using Continue = size_t (__stdcall *)(void *, const unsigned char *, size_t);
    using Finalize = size_t (__stdcall *)(void *);
    using Inform = const wchar_t *(__stdcall *)(void *, size_t);
    const auto create = reinterpret_cast<New>(library.resolve("MediaInfo_New"));
    const auto destroy = reinterpret_cast<Delete>(library.resolve("MediaInfo_Delete"));
    const auto option = reinterpret_cast<Option>(library.resolve("MediaInfo_Option"));
    const auto init = reinterpret_cast<Init>(library.resolve("MediaInfo_Open_Buffer_Init"));
    const auto feed = reinterpret_cast<Continue>(library.resolve("MediaInfo_Open_Buffer_Continue"));
    const auto finalize = reinterpret_cast<Finalize>(library.resolve("MediaInfo_Open_Buffer_Finalize"));
    const auto inform = reinterpret_cast<Inform>(library.resolve("MediaInfo_Inform"));
    if (!create || !destroy || !option || !init || !feed || !finalize || !inform) return {};
    std::unique_ptr<void, Delete> handle(create(), destroy);
    if (!handle) return {};
    option(handle.get(), L"Output", L"JSON");
    option(handle.get(), L"Language", L"raw");
    option(handle.get(), L"Complete", L"1");
    option(handle.get(), L"ParseSpeed", L"0.5");
    option(handle.get(), L"File_IsSeekable", L"0");
    const auto length = qMin(head.size(), MaximumHead);
    const auto size = fileSize > 0 ? quint64(qMax(fileSize, qint64(length))) : quint64(-1);
    init(handle.get(), size, 0);
    for (qsizetype pos = 0; pos < length; pos += 64 * 1024) {
        const auto count = qMin<qsizetype>(64 * 1024, length - pos);
        const auto status = feed(handle.get(), reinterpret_cast<const unsigned char *>(head.constData() + pos), size_t(count));
        if (status & 8) break; // MediaInfo finished; requested seeks are never followed.
    }
    finalize(handle.get());
    const auto text = inform(handle.get(), 0);
    if (!text) return {};
    auto result = QJsonDocument::fromJson(QString::fromWCharArray(text).toUtf8()).object();
    result["probeScope"] = "prefix";
    return result;
#else
    Q_UNUSED(head); Q_UNUSED(fileSize);
    return {};
#endif
}
}

QJsonArray mergeAudioProbe(const QJsonObject &report, const QJsonArray &tracks) {
    QJsonArray result;
    const auto streams = report.value("media").toObject().value("track").toArray();
    for (const auto &value : tracks) {
        if (!value.isObject()) { result.append(value); continue; }
        auto track = value.toObject();
        const int pid = track.value("pid").toInt(-1), coding = track.value("coding").toInt(-1);
        QJsonObject matched; int matches = 0;
        for (const auto &streamValue : streams) {
            const auto stream = streamValue.toObject();
            if (stream.value("@type").toString() == "Audio" && mediaPid(stream) == pid &&
                compatible(coding, stream.value("Format").toString())) { matched = stream; ++matches; }
        }
        if (matches != 1) { result.append(track); continue; } // Ambiguous PID/substream: retain unknowns.
        for (const auto &pair : {qMakePair("SamplingRate", "sampleRate"), qMakePair("Channels", "channels"), qMakePair("BitDepth", "bitDepth")}) {
            const double number = positiveNumber(matched.value(pair.first));
            if (number && std::floor(number) == number) track[pair.second] = number;
        }
        const auto layout = matched.value("ChannelLayout").toString();
        if (!layout.isEmpty()) track["channelLayout"] = layout;
        const auto bitrateMode = matched.value("BitRate_Mode").toString();
        const double bitrate = positiveNumber(matched.value("BitRate"));
        if (!bitrateMode.isEmpty()) track["bitrateMode"] = bitrateMode;
        if (bitrate) {
            if (bitrateMode == "CBR") {
                track["bitrate"] = bitrate;
                track["bitrateSource"] = "mediainfo-cbr-header";
            } else {
                track["probedBitrate"] = bitrate;
                track["bitrateSource"] = "mediainfo-prefix-estimate";
            }
            track["bitrateScope"] = "prefix";
        }
        const double nominal = positiveNumber(matched.value("BitRate_Nominal"));
        if (nominal) track["nominalBitrate"] = nominal;
        const auto evidence = atmosEvidence(matched, coding);
        if (!evidence.isEmpty()) {
            track["atmos"] = true;
            track["atmosEvidence"] = evidence;
        }
        track["audioProbeSource"] = "MediaInfo buffer";
        const auto formatProfile = matched.value("Format_Profile").toString();
        if (!formatProfile.isEmpty()) track["formatProfile"] = formatProfile;
        track["label"] = audioTrackLabel(track);
        result.append(track);
    }
    return result;
}

QJsonArray enrichAudioProbe(const QByteArray &head, qint64 fileSize, const QJsonArray &tracks) {
    if (tracks.isEmpty() || head.isEmpty()) return tracks;
    const auto report = readBuffer(head, fileSize);
    return report.isEmpty() ? tracks : mergeAudioProbe(report, tracks);
}
}
