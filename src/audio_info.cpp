#include "audio_info.h"
#include "audio_probe.h"
#include <QtCore/QFile>
#include <QtCore/QLocale>
#include <QtCore/QMap>
#include <QtCore/QStringList>

namespace sd {
namespace {
constexpr qint64 ProbeBytes = 4 * 1024 * 1024;
QString codecName(int coding) {
    switch (coding) {
    case 0x03: return "MPEG-1 Audio";
    case 0x04: return "MPEG-2 Audio";
    case 0x80: return "LPCM";
    case 0x81: return "Dolby Digital (AC-3)";
    case 0x82: return "DTS";
    case 0x83: return "Dolby TrueHD";
    case 0x84: return "Dolby Digital Plus (E-AC-3)";
    case 0x85: return "DTS-HD High Resolution";
    case 0x86: return "DTS-HD Master Audio";
    case 0xa1: return "Dolby Digital Plus (secondary)";
    case 0xa2: return "DTS-HD (secondary)";
    default: return {};
    }
}
QString languageName(const QString &input) {
    const auto code = input.trimmed().toLower();
    static const QMap<QString, QString> names{
        {"jpn", "日语"}, {"ja", "日语"}, {"eng", "英语"}, {"en", "英语"},
        {"chi", "中文"}, {"zho", "中文"}, {"zh", "中文"}, {"kor", "韩语"}, {"ko", "韩语"},
        {"fra", "法语"}, {"fre", "法语"}, {"fr", "法语"}, {"deu", "德语"}, {"ger", "德语"}, {"de", "德语"},
        {"spa", "西班牙语"}, {"es", "西班牙语"}, {"ita", "意大利语"}, {"it", "意大利语"},
        {"rus", "俄语"}, {"ru", "俄语"}, {"por", "葡萄牙语"}, {"pt", "葡萄牙语"},
        {"und", "语言未标注"}, {"mul", "多语言"}, {"zxx", "无语言内容"}};
    if (names.contains(code)) return names.value(code);
    if (code.isEmpty()) return "语言未标注";
    const auto language = QLocale::codeToLanguage(code);
    if (language != QLocale::AnyLanguage && language != QLocale::C) {
        const auto native = QLocale(language).nativeLanguageName();
        if (!native.isEmpty()) return native;
    }
    return code;
}
int sampleRate(int code) {
    // libbluray bluray.h bd_audio_rate_e; 12/14 describe core/extension pairs.
    switch (code) { case 1: return 48000; case 4: return 96000; case 5: return 192000; default: return 0; }
}
struct Probe {
    QByteArray prefix;
    int continuity = -1;
    bool started = false;
    QJsonObject fields;
};
void parseLpcm(Probe &probe) {
    const auto &bytes = probe.prefix;
    if (bytes.size() < 9) return;
    auto byte = [&bytes](qsizetype index) { return static_cast<unsigned char>(bytes[index]); };
    if (byte(0) || byte(1) || byte(2) != 1 || byte(3) != 0xbd || (byte(6) & 0xc0) != 0x80) {
        probe.started = false; probe.prefix.clear(); return;
    }
    const int offset = 9 + byte(8);
    if (bytes.size() < offset + 4) return;
    const int assignment = byte(offset + 2) >> 4;
    const int rate = sampleRate(byte(offset + 2) & 15);
    const int depthCode = byte(offset + 3) >> 6;
    const int depth = depthCode == 1 ? 16 : depthCode == 2 ? 20 : depthCode == 3 ? 24 : 0;
    // Blu-ray LPCM channel assignments, also documented in FFmpeg pcm-bluray.c.
    static constexpr int counts[] = {0, 1, 0, 2, 3, 3, 4, 4, 5, 6, 7, 8};
    static const char *layouts[] = {"", "1.0", "", "2.0", "3.0", "2/1", "3/1", "2/2", "5.0", "5.1", "7.0", "7.1"};
    if (assignment >= 12 || !counts[assignment] || !rate || !depth) {
        probe.started = false; probe.prefix.clear(); return;
    }
    const int channels = counts[assignment];
    const qint64 bitrate = qint64(rate) * channels * depth;
    // Blu-ray pads odd channel counts and stores 20-bit samples in 24-bit words.
    const qint64 transportBitrate = qint64(rate) * ((channels + 1) & ~1) * (depth == 16 ? 16 : 24);
    probe.fields = {{"sampleRate", rate}, {"channels", channels}, {"channelLayout", layouts[assignment]},
                    {"bitDepth", depth}, {"bitrate", double(bitrate)}, {"bitrateSource", "lpcm-header-calculated"},
                    {"transportBitrate", double(transportBitrate)}, {"streamId", 0xbd}, {"audioInfoSource", "lpcm-pes-header"}};
}
QMap<int, Probe> probeTracks(const QByteArray &data, const QJsonArray &audio) {
    QMap<int, Probe> probes;
    for (const auto &value : audio) {
        const auto track = value.toObject();
        const int pid = track.value("pid").toInt(-1);
        if (track.value("coding").toInt(-1) == 0x80 && pid >= 0 && pid < 8192) probes.insert(pid, {});
    }
    if (probes.isEmpty() || data.isEmpty()) return probes;
    auto byte = [&data](qsizetype index) { return static_cast<unsigned char>(data[index]); };
    qsizetype start = -1; int stride = 0;
    for (qsizetype offset = 0; offset < qMin<qsizetype>(4096, data.size()) && start < 0; ++offset) {
        if (byte(offset) != 0x47) continue;
        for (int candidate : {192, 188}) {
            if (offset + 188 > data.size()) continue;
            const qsizetype remaining = (data.size() - offset - 188) / candidate;
            if (remaining >= 1 && byte(offset + candidate) != 0x47) continue;
            if (remaining >= 2 && byte(offset + candidate * 2) != 0x47) continue;
            if (remaining == 0 && !(data.size() == 188 || (data.size() == 192 && offset == 4))) continue;
            start = offset; stride = candidate; break;
        }
    }
    if (start < 0) return probes;
    for (qsizetype pos = start; pos + 188 <= data.size(); pos += stride) {
        if (byte(pos) != 0x47) break;
        const int pid = ((byte(pos + 1) & 0x1f) << 8) | byte(pos + 2);
        auto found = probes.find(pid);
        if (found == probes.end() || !found->fields.isEmpty()) continue;
        auto &probe = found.value();
        const int control = (byte(pos + 3) >> 4) & 3;
        if ((byte(pos + 1) & 0x80) || (byte(pos + 3) & 0xc0) || control == 0) {
            probe.started = false; probe.continuity = -1; continue;
        }
        int offset = 4;
        if (control & 2) {
            const int length = byte(pos + 4);
            if (5 + length > 188) { probe.started = false; probe.continuity = -1; continue; }
            if (length && (byte(pos + 5) & 0x80)) { probe.started = false; probe.continuity = -1; }
            offset = 5 + length;
        }
        if (!(control & 1) || offset >= 188) continue;
        const int cc = byte(pos + 3) & 15;
        const bool begins = byte(pos + 1) & 0x40;
        if (probe.continuity == cc && !begins) continue; // Duplicate TS packet.
        if (!begins && probe.continuity >= 0 && cc != ((probe.continuity + 1) & 15)) probe.started = false;
        probe.continuity = cc;
        if (begins) { probe.prefix.clear(); probe.started = true; }
        if (!probe.started) continue;
        const auto count = qMin<qsizetype>(188 - offset, 512 - probe.prefix.size());
        if (count > 0) probe.prefix.append(data.constData() + pos + offset, count);
        parseLpcm(probe);
    }
    return probes;
}
QString decimal(double value, int precision = 3) {
    auto text = QString::number(value, 'f', precision);
    while (text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text;
}
}

QJsonArray enrichAudioTracks(const QString &clipPath, const QJsonArray &audio) {
    if(audio.isEmpty())return audio;
    QByteArray head;
    qint64 fileSize = 0;
    if (!clipPath.isEmpty() && !audio.isEmpty()) {
        QFile file(clipPath);
        if (file.open(QIODevice::ReadOnly)) { fileSize = file.size(); head = file.read(ProbeBytes); }
    }
    const auto probes = probeTracks(head, audio);
    QJsonArray result;
    for (const auto &value : audio) {
        if (!value.isObject()) { result.append(value); continue; }
        auto track = value.toObject();
        const auto codec = codecName(track.value("coding").toInt(-1));
        if (!codec.isEmpty()) track["codec"] = codec;
        track["languageName"] = languageName(track.value("language").toString());
        const int rate = sampleRate(track.value("rate").toInt());
        if (rate) track["sampleRate"] = rate;
        else if (track.value("rate").toInt() == 12) track["sampleRateDescription"] = "48/96 kHz core · 192 kHz extension";
        else if (track.value("rate").toInt() == 14) track["sampleRateDescription"] = "48 kHz core · 96 kHz extension";
        switch (track.value("format").toInt()) {
        case 1: track["channels"] = 1; track["channelLayout"] = "1.0"; break;
        case 3: track["channels"] = 2; track["channelLayout"] = "2.0"; break;
        case 6: track["channelLayout"] = "多声道"; break;
        case 12: track["channelLayout"] = "立体声 core / 多声道 extension"; break;
        default: break;
        }
        const auto fields = track.value("coding").toInt(-1) == 0x80 ?
            probes.value(track.value("pid").toInt(-1)).fields : QJsonObject();
        for (auto field = fields.begin(); field != fields.end(); ++field) track[field.key()] = field.value();
        track["label"] = audioTrackLabel(track);
        result.append(track);
    }
    bool compressed=false;for(const auto &track:audio)if(track.toObject().value("coding").toInt()!=0x80){compressed=true;break;}
    return compressed?enrichAudioProbe(head, fileSize, result):result;
}

QString audioTrackLabel(const QJsonObject &track) {
    QStringList parts;
    if (track.value("index").isDouble()) parts.append(QString("#%1").arg(track.value("index").toInt() + 1));
    parts.append(track.value("languageName").toString(languageName(track.value("language").toString())));
    auto codec = track.value("codec").toString(codecName(track.value("coding").toInt(-1)));
    if (codec.isEmpty()) codec = "编码未知";
    parts.append(codec);
    if (track.value("atmos").toBool()) parts.append("Dolby Atmos");
    auto layout = track.value("channelLayout").toString();
    if (layout.isEmpty() && track.value("channels").toInt() > 0) layout = QString("%1 声道").arg(track.value("channels").toInt());
    if (!layout.isEmpty()) parts.append(layout);
    if (track.value("channels").toInt() == 2) parts.append("双声道");
    if (track.value("sampleRate").toDouble() > 0) parts.append(decimal(track.value("sampleRate").toDouble() / 1000) + " kHz");
    else if (!track.value("sampleRateDescription").toString().isEmpty()) parts.append(track.value("sampleRateDescription").toString());
    if (track.value("bitDepth").toInt() > 0) parts.append(QString("%1 bit").arg(track.value("bitDepth").toInt()));
    if (track.value("bitrate").toDouble() > 0) parts.append(decimal(track.value("bitrate").toDouble() / 1000000) + " Mbps");
    else if (track.value("probedBitrate").toDouble() > 0)
        parts.append("≈" + decimal(track.value("probedBitrate").toDouble() / 1000000) + " Mbps（头部片段）");
    if (track.value("pid").isDouble()) parts.append(QString("PID 0x%1").arg(track.value("pid").toInt(), 4, 16, QLatin1Char('0')));
    if (track.value("streamId").isDouble()) parts.append(QString("PES 0x%1").arg(track.value("streamId").toInt(), 2, 16, QLatin1Char('0')));
    return parts.join(" · ");
}
}
