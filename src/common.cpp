#include "common.h"
#include "bluray_audio.h"
#include <windows.h>
#include <wincrypt.h>
#include <mutex>
#include <stdexcept>

namespace sd {
QString workerName(const QString &id) { return serverName() + "-" + id; }
void setupRuntime() {
    const QString app = QCoreApplication::applicationDirPath();
    QString gst = qEnvironmentVariable("STREAMDESK_GST");
    if (gst.isEmpty()) gst = app + "/runtime/gstreamer";
    if (!QDir(gst + "/bin").exists()) throw std::runtime_error("Missing private GStreamer runtime; set STREAMDESK_GST or use a complete package");
    qputenv("PATH", (QDir::toNativeSeparators(app) + ";" + QDir::toNativeSeparators(gst + "/bin") + ";" + qEnvironmentVariable("PATH")).toUtf8());
    qputenv("GST_PLUGIN_PATH_1_0", (gst + "/lib/gstreamer-1.0").toUtf8());
    qputenv("GST_PLUGIN_SCANNER_1_0", (gst + "/libexec/gstreamer-1.0/gst-plugin-scanner.exe").toUtf8());
    qputenv("GST_REGISTRY_1_0", (dataRoot() + "/gst-registry.bin").toUtf8());
}
void initGst() { static std::once_flag once; std::call_once(once, [] { gst_init(nullptr, nullptr); registerBlurayAudioDecoder(); }); }
QJsonObject publicConfig(QJsonObject c) {
    if (c.value("output").toString().contains("://")) c["output"] = "[network destination hidden]";
    return c;
}
QString validate(const QJsonObject &c) {
    const QString input = c.value("input").toString(), output = c.value("output").toString();
    const QString codec = c.value("codec").toString("h264"), backend = c.value("backend").toString("software");
    if (input.isEmpty() || !QFileInfo::exists(input)) return "输入文件或 BDMV 目录不存在";
    const bool embedded=c.value("useEmbeddedServer").toBool();
    const bool monitor=c["studio"].toBool()&&output=="studio://monitor";
    if(c["studio"].toBool()&&c.value("studioFps").toInt(30)!=30)return "当前工作室输出帧率固定为 30 fps";
    if (output.isEmpty()&&!embedded) return "请设置输出文件或推流地址";
    const QString mode = c.value("mode").toString("transcode");
    if (mode != "transcode" && mode != "copy") return "无效处理模式";
    if (codec != "h264" && codec != "h265") return "编码格式必须为 H.264 或 H.265";
    if (!QStringList{"nvidia", "intel", "amd", "software"}.contains(backend)) return "无效编码后端";
    if (!QStringList{"auto", "nvidia", "intel", "software"}.contains(c.value("decoder").toString("auto"))) return "无效解码后端";
    if (c.value("bitrate").toInt(6000) < 100 || c.value("bitrate").toInt(6000) > 200000) return "视频码率范围为 100–200000 kbps";
    if (!QStringList{"cbr", "vbr"}.contains(c.value("rateControl").toString("cbr"))) return "无效码率模式";
    if (c.value("audioBitrate").toInt(192) < 32 || c.value("audioBitrate").toInt(192) > 512) return "音频码率范围为 32–512 kbps";
    const int w = c.value("width").toInt(0), h = c.value("height").toInt(0);
    if ((w == 0) != (h == 0) || w < 0 || h < 0 || w > 8192 || h > 8192 || w % 2 || h % 2) return "分辨率须为偶数宽高，或同时为 0（保持原始）";
    if (c.value("start").toDouble(0) < 0 || c.value("duration").toDouble(0) < 0) return "起始时间与时长不能为负数";
    if (c.value("audioTrack").toInt(0) < -1 || c.value("subtitleTrack").toInt(-1) < -1) return "无效轨道编号";
    const QString subtitle = c.value("subtitle").toString();
    if (!subtitle.isEmpty() && !QFileInfo::exists(subtitle)) return "外挂字幕文件不存在";
    if (mode == "copy" && (!subtitle.isEmpty() || c.value("subtitleTrack").toInt(-1) >= 0 || w || c.value("normalizeAudio").toBool()))
        return "直通模式不能烧录字幕、缩放或处理音频";
    QUrl url(output);
    if(monitor){ /* Studio preparation does not open an output connection. */ }
    else if(embedded&&(output.isEmpty()||output.startsWith("embedded://"))){ /* Manager resolves the current local ingress at launch. */ }
    else if (output.contains("://")) {
        if (!QStringList{"rtmp", "rtmps", "srt", "udp"}.contains(url.scheme())) return "支持 RTMP(S)、SRT 和 UDP 输出";
        if (url.host().isEmpty()) return "推流地址缺少主机";
        if ((url.scheme() == "rtmp" || url.scheme() == "rtmps") && codec == "h265") return "当前 RTMP 输出仅开放 H.264；H.265 请使用 SRT/UDP 或文件";
    } else {
        if (!QStringList{"mkv", "mp4", "ts"}.contains(QFileInfo(output).suffix().toLower())) return "文件输出支持 .mkv、.mp4 和 .ts";
        if (!QDir(QFileInfo(output).absolutePath()).exists()) return "输出目录不存在";
        if (QFileInfo::exists(output)) return "输出文件已存在，请更换文件名（不会覆盖）";
        if (QFileInfo(input).absoluteFilePath() == QFileInfo(output).absoluteFilePath()) return "不能覆盖源文件";
    }
    if (c.value("record").toString() == output && !output.isEmpty()) return "录制路径与主输出不能相同";
    const QString record = c.value("record").toString();
    if (!record.isEmpty() && (QFileInfo(record).suffix().toLower() != "mkv" || QFileInfo::exists(record) || !QDir(QFileInfo(record).absolutePath()).exists()))
        return "同步录制须指定不存在的 .mkv 文件及有效目录";
    return {};
}
QString gstQuote(const QString &text) { QString s = text; s.replace("\\", "\\\\"); s.replace("\"", "\\\""); return "\"" + s + "\""; }
QString timeText(double seconds) {
    const qint64 s = qMax<qint64>(0, qint64(seconds));
    return QString("%1:%2:%3").arg(s/3600,2,10,QChar('0')).arg(s/60%60,2,10,QChar('0')).arg(s%60,2,10,QChar('0'));
}
QString encoderFactory(const QString &backend, const QString &codec) {
    const QString suffix = codec == "h265" ? "h265" : "h264";
    if (backend == "nvidia") return "nvd3d11" + suffix + "enc";
    if (backend == "intel") return "qsv" + suffix + "enc";
    if (backend == "amd") return "amf" + suffix + "enc";
    return codec == "h265" ? "x265enc" : "x264enc";
}
}
