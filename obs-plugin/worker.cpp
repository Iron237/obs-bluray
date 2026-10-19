#include "pipeline.h"
#include "raw_ipc.h"
#include <gst/video/video.h>
#include <mutex>

int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    QCoreApplication app(argc, argv);
    app.setOrganizationName("StreamDesk"); app.setApplicationName("OBSBluray"); sd::setupRuntime();
    const auto args = app.arguments();
    if(args.contains("--inspect")){const auto info=sd::inspectSource(args.value(args.indexOf("--inspect")+1));if(args.contains("--inspect-to"))sd::saveJson(args.value(args.indexOf("--inspect-to")+1),info);else sd::printJson(info);return info["ok"].toBool()?0:1;}
    auto value = [&](const char *key) { return args.value(args.indexOf(key) + 1); };
    sdobs::Mapping ipc;
    if(!args.contains("--map") || !ipc.open(value("--map").toStdWString(), false)) return 2;
    auto fail = [&](const QString &error) { if(ipc.lock()) { ipc.data->state = sdobs::failed; const auto bytes = error.toUtf8(); strncpy_s(ipc.data->error, bytes.constData(), _TRUNCATE); ipc.unlock(); } };
    try {
        const QString configPath = value("--config");
        auto config = sd::readJson(configPath); QFile::remove(configPath);
        if(config.value("playlist").toInt(-1)<0)config.remove("playlist");
        if(config.value("chapter").toInt()>0){
            const auto metadata=sd::inspectSource(config["input"].toString());const auto playlist=config.value("playlist").toInt(metadata["suggestedPlaylist"].toInt());
            for(const auto &t:metadata["titles"].toArray())if(t.toObject()["playlist"].toInt()==playlist){const auto chapters=t.toObject()["chapters"].toArray();const int index=config["chapter"].toInt()-1;if(index>=chapters.size())throw std::runtime_error("章节编号超出范围");config["start"]=chapters[index].toObject()["start"];}
        }
        config["_obsSource"] = true; config["mode"] = "transcode";
        config["backend"] = config.value("decoder").toString("software");
        if(config["backend"] == "auto") config["backend"] = "software";
        config["output"] = "studio://monitor";
        // Each source has a private process: decoder ranks and third-party DLLs
        // never change OBS or another source's runtime.
        std::unique_ptr<sd::MediaWorker> worker;
        std::shared_ptr<sd::DeckBridge> bridge;
        std::unique_ptr<sd::Subtitles> subtitles;
        const bool separate=config.value("separateSubtitles").toBool(true)&&!config["subtitle"].toString().isEmpty();
        const auto subtitleConfig=config;
        if(separate)config["subtitle"]="";
        std::mutex clockMutex; int64_t clockOffset = -1;
        auto timestamp = [&](GstSample *sample) {
            auto *buffer = gst_sample_get_buffer(sample); auto pts = GST_BUFFER_PTS(buffer);
            auto *segment = gst_sample_get_segment(sample);
            auto running = segment && GST_CLOCK_TIME_IS_VALID(pts) ? gst_segment_to_running_time(segment, GST_FORMAT_TIME, pts) : GST_CLOCK_TIME_NONE;
            if(!GST_CLOCK_TIME_IS_VALID(running)) return sdobs::nowNs();
            std::lock_guard guard(clockMutex);
            if(clockOffset == -1) clockOffset = int64_t(sdobs::nowNs()) - int64_t(running);
            return uint64_t(clockOffset + int64_t(running));
        };
        std::function<void(double, bool)> load;
        bool reload = false; double reloadAt = 0;
        load = [&](double seconds, bool paused) {
            worker.reset(); bridge = std::make_shared<sd::DeckBridge>();
            subtitles.reset();if(separate){auto c=subtitleConfig;c["start"]=seconds;subtitles=std::make_unique<sd::Subtitles>(c);subtitles->load();}
            { std::lock_guard guard(clockMutex); clockOffset = -1; }
            if(ipc.lock()) { ipc.data->state = sdobs::opening; ipc.data->separateSubtitles=separate; ipc.data->audioRead = ipc.data->audioWrite; ipc.data->error[0] = 0; ipc.unlock(); }
            auto deliverVideo = [&](GstSample *sample,bool preroll) {
                GstVideoInfo info{}; GstVideoFrame frame{};
                if(!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) || !gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) return;
                const uint32_t width = GST_VIDEO_INFO_WIDTH(&info), height = GST_VIDEO_INFO_HEIGHT(&info), stride = width * 4;
                const size_t bytes = size_t(stride) * height;
                const auto overlay=subtitles&&bytes<=sdobs::maxVideo?subtitles->renderTransparent(int(width),int(height),GST_BUFFER_PTS(gst_sample_get_buffer(sample))):QByteArray();
                // Preroll can include font discovery and decoder warm-up. It is
                // a still image, never the anchor of the running A/V clock.
                const auto ts=preroll?sdobs::nowNs():timestamp(sample);
                if(bytes <= sdobs::maxVideo && ipc.lock()) {
                    auto *h = ipc.data; h->width = width; h->height = height; h->stride = stride; h->videoBytes = uint32_t(bytes); h->videoTimestamp = ts;
                    for(uint32_t y=0; y<height; ++y) std::memcpy(h->video + size_t(y)*stride, static_cast<uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)) + ptrdiff_t(y)*GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), stride);
                    if(!overlay.isEmpty())std::memcpy(h->subtitle,overlay.constData(),bytes);
                    ++h->videoSequence; ipc.unlock();
                } else if(bytes > sdobs::maxVideo) fail("源分辨率超过 4096×2160，请先缩放素材");
                gst_video_frame_unmap(&frame);
            };
            bridge->videoOutput=[deliverVideo](GstSample *sample){deliverVideo(sample,false);};
            bridge->videoPrerollOutput=[deliverVideo](GstSample *sample){deliverVideo(sample,true);};
            bridge->audioOutput = [&](GstSample *sample) {
                GstMapInfo map{}; auto *buffer = gst_sample_get_buffer(sample);
                if(!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
                const auto ts = timestamp(sample);
                if(map.size <= sdobs::audioBytes && ipc.lock()) {
                    auto *h = ipc.data;
                    if(h->audioWrite - h->audioRead >= sdobs::audioSlots) { ++h->audioRead; ++h->audioDropped; }
                    auto &a = h->audio[h->audioWrite++ % sdobs::audioSlots]; a.timestamp = ts; a.bytes = uint32_t(map.size); std::memcpy(a.data, map.data, map.size); ipc.unlock();
                }
                gst_buffer_unmap(buffer, &map);
            };
            auto c = config; c["start"] = seconds; c["_paused"] = paused;
            worker = std::make_unique<sd::MediaWorker>(QUuid::createUuid().toString(QUuid::WithoutBraces), c);
            worker->setDeckBridge(bridge, nullptr);
            QObject::connect(worker.get(),&sd::MediaWorker::sourcePrepared,&app,[&](double timeline){if(subtitles)subtitles->setTimeline(timeline);});
            QObject::connect(worker.get(), &sd::MediaWorker::statusChanged, &app, [&](QJsonObject s) {
                const auto state = s["state"].toString();
                if(ipc.lock()) {
                    auto *h = ipc.data;
                    h->positionMs = qint64(s["position"].toDouble() * 1000); h->durationMs = qint64(s["sourceDuration"].toDouble() * 1000); h->subtitleFrames = quint64(s["subtitleFrames"].toDouble());
                    h->state = state == "running" ? sdobs::playing : state == "paused" ? sdobs::paused : state == "completed" ? sdobs::ended : state == "failed" ? sdobs::failed : state == "stopped" ? sdobs::stopped : sdobs::opening;
                    const auto error = s["error"].toString().toUtf8(); strncpy_s(h->error, error.constData(), _TRUNCATE); ipc.unlock();
                }
            });
            QObject::connect(worker.get(), &sd::MediaWorker::finished, &app, [&](bool ok) { if(ok && config["loop"].toBool()) { reload = true; reloadAt = config["start"].toDouble(); } });
            worker->start();
        };
        HANDLE parent = args.contains("--parent") ? OpenProcess(SYNCHRONIZE, FALSE, value("--parent").toUInt()) : nullptr;
        QTimer commands; commands.setInterval(10); uint64_t seen = 0;
        QObject::connect(&commands, &QTimer::timeout, &app, [&] {
            if(parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) { app.quit(); return; }
            uint32_t cmd = 0; int64_t v = 0;
            if(ipc.lock()) { if(ipc.data->commandSequence != seen) { seen = ipc.data->commandSequence; cmd = ipc.data->command; v = ipc.data->commandValue; } ipc.unlock(); }
            if(cmd == sdobs::seek && worker) { const double duration = worker->status()["sourceDuration"].toDouble(); if(v >= 0 && (duration <= 0 || double(v) < duration*1000)) load(double(v)/1000, worker->status()["state"] == "paused"); }
            else if(cmd == sdobs::restart) load(config["start"].toDouble(), false);
            else if(cmd == sdobs::stop) { worker.reset(); if(ipc.lock()) { ipc.data->state = sdobs::stopped; ipc.data->audioRead = ipc.data->audioWrite; ipc.unlock(); } }
            else if(cmd == sdobs::pause && worker) worker->command({{"op","pause"}});
            else if(cmd == sdobs::resume) { if(worker&&worker->status()["state"]!="completed") { {std::lock_guard guard(clockMutex); clockOffset = -1;} worker->command({{"op","resume"}}); } else load(config["start"].toDouble(), false); }
            if(reload) { reload = false; load(reloadAt, false); }
        });
        load(config["start"].toDouble(), false); commands.start();
        int result = app.exec(); commands.stop(); worker.reset(); if(parent) CloseHandle(parent); return result;
    } catch(const std::exception &e) { fail(QString::fromUtf8(e.what())); return 1; }
}
