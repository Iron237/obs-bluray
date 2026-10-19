#include "pipeline.h"
#include "program.h"
#include "raw_ipc.h"
#include <gst/video/video.h>
#include <mutex>
#include <cmath>

namespace {
struct Delivery {
    double timeline=0, start=0, end=0, positionOrigin=0;
    bool ranged=false, separate=false, subtitleAnchored=false, hasAudio=true;
    std::atomic<GstClockTime> firstVideo{GST_CLOCK_TIME_NONE};
    std::atomic<double> position{0};
    std::atomic<bool> videoEnd{false},audioEnd{false};
    std::atomic<bool> videoReady{false},pauseWhenReady{false},muted{false};
    std::unique_ptr<sd::Subtitles> subtitles;
    // GStreamer prerolls video before starting either synchronized appsink.
    double contentTime(GstClockTime pts) const {return timeline+(double(pts)-double(firstVideo.load()))/GST_SECOND;}
};
}
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
        QJsonObject metadata;
        const auto programPayload=config.value("programJson");
        if((!programPayload.isUndefined()&&!programPayload.isNull()&&(!programPayload.isString()||!programPayload.toString().trimmed().isEmpty()))||config.value("chapter").toInt()>0){
            metadata=sd::inspectSource(config["input"].toString());
            if(!metadata["ok"].toBool())throw std::runtime_error(metadata["error"].toString().toStdString());
        }
        auto plan=sd::resolveProgram(config,metadata);
        if(!plan.ok())throw std::runtime_error(plan.error.toStdString());
        if(!plan.active){
            if(config.value("playlist").toInt(-1)<0)config.remove("playlist");
            if(config.value("chapter").toInt()>0){
                const auto playlist=config.value("playlist").toInt(metadata["suggestedPlaylist"].toInt());
                for(const auto &t:metadata["titles"].toArray())if(t.toObject()["playlist"].toInt()==playlist){const auto chapters=t.toObject()["chapters"].toArray();const int index=config["chapter"].toInt()-1;if(index>=chapters.size())throw std::runtime_error("章节编号超出范围");config["start"]=chapters[index].toObject()["start"];}
            }
        }
        config["_obsSource"] = true; config["mode"] = "transcode";
        config["backend"] = config.value("decoder").toString("software");
        if(config["backend"] == "auto") config["backend"] = "software";
        config["output"] = "studio://monitor";
        std::unique_ptr<sd::MediaWorker> worker;
        std::shared_ptr<sd::DeckBridge> bridge;
        std::shared_ptr<Delivery> delivery;
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
        auto clear = [&](uint32_t state){if(ipc.lock()){
            auto*h=ipc.data;++h->generation;h->state=state;
            if(h->videoBytes<=sdobs::maxVideo)std::memset(h->subtitle,0,h->videoBytes);
            h->videoBytes=0;h->width=h->height=h->stride=0;
            h->audioRead=h->audioWrite;h->subtitleFrames=0;h->error[0]=0;ipc.unlock();
        }};
        bool advance=false;uint64_t generation=0;
        std::function<void(const QString &,double,bool)> load;
        load = [&](const QString &episodeId,double relativeSeek,bool paused) {
            // NULL waits for all old streaming callbacks before their state is replaced.
            worker.reset();++generation;advance=false;
            if(plan.active){plan=sd::resolveProgram(config,metadata,episodeId);if(!plan.ok())throw std::runtime_error(plan.error.toStdString());}
            auto c=plan.active?plan.config:config;
            const double seconds=plan.active?plan.start+relativeSeek:relativeSeek;
            auto d=std::make_shared<Delivery>();delivery=d;
            d->ranged=plan.active;d->start=seconds;d->positionOrigin=plan.active?plan.start:0;d->end=plan.active?plan.end:0;d->position=plan.active?relativeSeek:seconds;
            d->pauseWhenReady=plan.active&&paused;d->muted=plan.active&&paused;
            d->hasAudio=c.value("audioTrack").toInt(0)>=0;
            if(plan.active)for(const auto&t:metadata["titles"].toArray())if(t.toObject()["playlist"]==c["playlist"])d->hasAudio=d->hasAudio&&!t.toObject()["audio"].toArray().isEmpty();
            d->separate=c.value("separateSubtitles").toBool(true)&&!c["subtitle"].toString().isEmpty();
            if(d->separate){auto sc=c;sc["start"]=seconds;d->subtitles=std::make_unique<sd::Subtitles>(sc);d->subtitles->load();c["subtitle"]="";}
            c["start"]=seconds;c["_paused"]=paused&&!plan.active;
            // Duration probes count each track from its first decoded buffer. Range
            // selection instead uses one MPLS clock and clips PCM at sample edges.
            if(plan.active)c.remove("duration");
            {std::lock_guard guard(clockMutex);clockOffset=-1;}
            clear(sdobs::opening);
            if(ipc.lock()){
                auto*h=ipc.data;h->separateSubtitles=d->separate;h->positionMs=qint64(d->position*1000);h->durationMs=plan.active?qint64(plan.duration*1000):0;
                h->episodeIndex=-1;h->episodeCount=0;h->playlist=c.value("playlist").toInt(-1);
                QString label;
                if(plan.active){const auto es=plan.program["episodes"].toArray();for(qsizetype i=0;i<es.size();++i){const auto e=es[i].toObject();if(e["enabled"].toBool())++h->episodeCount;if(e["id"]==plan.episodeId){h->episodeIndex=int(i);label=e["label"].toString();}}}
                const auto id=plan.active?plan.episodeId.toUtf8():QByteArray();const auto title=label.toUtf8();strncpy_s(h->episodeId,id.constData(),_TRUNCATE);strncpy_s(h->episodeLabel,title.constData(),_TRUNCATE);ipc.unlock();
            }
            bridge=std::make_shared<sd::DeckBridge>();
            auto deliverVideo = [&,d](GstSample *sample,bool preroll) {
                // Keep the first in-range preview fixed while the control thread
                // waits for PAUSED; a decoder can finish another pending frame.
                if(d->muted&&d->videoReady)return;
                auto *buffer=gst_sample_get_buffer(sample);const auto pts=GST_BUFFER_PTS(buffer);
                if(!GST_CLOCK_TIME_IS_VALID(d->firstVideo)&&GST_CLOCK_TIME_IS_VALID(pts))d->firstVideo=pts;
                const double content=GST_CLOCK_TIME_IS_VALID(pts)&&GST_CLOCK_TIME_IS_VALID(d->firstVideo)?d->contentTime(pts):d->timeline;
                if(d->ranged){if(content+1e-8<d->start)return;if(content>=d->end-1e-8){d->videoEnd=true;return;}}
                GstVideoInfo info{}; GstVideoFrame frame{};
                if(!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) || !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) return;
                const uint32_t width=GST_VIDEO_INFO_WIDTH(&info),height=GST_VIDEO_INFO_HEIGHT(&info),stride=width*4;const size_t bytes=size_t(stride)*height;
                if(d->subtitles&&!d->subtitleAnchored){d->subtitles->setTimeline(content);d->subtitleAnchored=true;}
                const auto overlay=d->subtitles&&bytes<=sdobs::maxVideo?d->subtitles->renderTransparent(int(width),int(height),pts):QByteArray();
                // Preroll must never anchor the running shared audio/video clock.
                const auto ts=preroll?sdobs::nowNs():timestamp(sample);
                if(bytes<=sdobs::maxVideo&&ipc.lock()){
                    auto*h=ipc.data;h->width=width;h->height=height;h->stride=stride;h->videoBytes=uint32_t(bytes);h->videoTimestamp=ts;
                    for(uint32_t y=0;y<height;++y)std::memcpy(h->video+size_t(y)*stride,static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame,0))+ptrdiff_t(y)*GST_VIDEO_FRAME_PLANE_STRIDE(&frame,0),stride);
                    if(!overlay.isEmpty())std::memcpy(h->subtitle,overlay.constData(),bytes);else if(h->separateSubtitles)std::memset(h->subtitle,0,bytes);
                    ++h->videoSequence;ipc.unlock();d->position=d->ranged?qMax(0.0,content-d->positionOrigin):content;d->videoReady=true;
                }else if(bytes>sdobs::maxVideo)fail("源分辨率超过 4096×2160，请先缩放素材");
                gst_video_frame_unmap(&frame);
            };
            bridge->videoOutput=[deliverVideo](GstSample*s){deliverVideo(s,false);};bridge->videoPrerollOutput=[deliverVideo](GstSample*s){deliverVideo(s,true);};
            bridge->audioOutput=[&,d](GstSample*sample){
                // A paused range seek may need to decode from an earlier keyframe
                // before its preview is available. That scan must stay silent.
                if(d->muted)return;
                GstMapInfo map{};auto*buffer=gst_sample_get_buffer(sample);if(!gst_buffer_map(buffer,&map,GST_MAP_READ))return;
                size_t first=0,last=map.size/8;
                if(d->ranged){const auto pts=GST_BUFFER_PTS(buffer);if(!GST_CLOCK_TIME_IS_VALID(pts)||!GST_CLOCK_TIME_IS_VALID(d->firstVideo)){gst_buffer_unmap(buffer,&map);return;}
                    const double content=d->contentTime(pts);const auto edge=[&](double t){return size_t(qBound(0.0,std::ceil((t-content)*48000-1e-6),double(last)));};
                    first=edge(d->start);if(content+double(last)/48000>=d->end){last=edge(d->end);d->audioEnd=true;}
                }
                const size_t bytes=last>first?(last-first)*8:0;
                if(bytes&&bytes<=sdobs::audioBytes){const auto ts=timestamp(sample)+uint64_t(first)*GST_SECOND/48000;if(ipc.lock()){
                    auto*h=ipc.data;if(h->audioWrite-h->audioRead>=sdobs::audioSlots){++h->audioRead;++h->audioDropped;}
                    auto&a=h->audio[h->audioWrite++%sdobs::audioSlots];a.timestamp=ts;a.bytes=uint32_t(bytes);std::memcpy(a.data,map.data+first*8,bytes);ipc.unlock();}}
                gst_buffer_unmap(buffer,&map);
            };
            worker=std::make_unique<sd::MediaWorker>(QUuid::createUuid().toString(QUuid::WithoutBraces),c);worker->setDeckBridge(bridge,nullptr);
            QObject::connect(worker.get(),&sd::MediaWorker::sourcePrepared,&app,[d](double timeline){d->timeline=timeline;});
            const auto token=generation;
            QObject::connect(worker.get(),&sd::MediaWorker::statusChanged,&app,[&,d,token](QJsonObject s){if(token!=generation)return;const auto state=s["state"].toString();if(ipc.lock()){
                auto*h=ipc.data;h->positionMs=qint64(d->position*1000);if(!d->ranged)h->durationMs=qint64(s["sourceDuration"].toDouble()*1000);
                h->subtitleFrames=d->subtitles?d->subtitles->renderedFrames.load():quint64(s["subtitleFrames"].toDouble());
                h->state=state=="running"?(d->pauseWhenReady?sdobs::opening:sdobs::playing):state=="paused"?sdobs::paused:state=="completed"?sdobs::ended:state=="failed"?sdobs::failed:state=="stopped"?sdobs::stopped:sdobs::opening;
                const auto e=s["error"].toString().toUtf8();strncpy_s(h->error,e.constData(),_TRUNCATE);ipc.unlock();}});
            QObject::connect(worker.get(),&sd::MediaWorker::finished,&app,[&,token](bool ok){if(token==generation&&ok)advance=true;});
            worker->start();
        };
        HANDLE parent=args.contains("--parent")?OpenProcess(SYNCHRONIZE,FALSE,value("--parent").toUInt()):nullptr;
        QTimer commands;commands.setInterval(10);uint64_t seen=0;
        QObject::connect(&commands,&QTimer::timeout,&app,[&]{try{
            if(parent&&WaitForSingleObject(parent,0)==WAIT_OBJECT_0){app.quit();return;}
            uint32_t cmd=0;int64_t v=0;if(ipc.lock()){if(ipc.data->commandSequence!=seen){seen=ipc.data->commandSequence;cmd=ipc.data->command;v=ipc.data->commandValue;}if(delivery)ipc.data->positionMs=qint64(delivery->position*1000);ipc.unlock();}
            const auto currentId=plan.active?plan.episodeId:QString();const bool paused=worker&&worker->status()["state"]=="paused";
            if(cmd!=sdobs::none)advance=false;
            if(cmd==sdobs::seek&&worker){const double duration=plan.active?plan.duration:worker->status()["sourceDuration"].toDouble();if(v>=0&&(duration<=0||double(v)<duration*1000))load(currentId,double(v)/1000,paused);}
            else if(cmd==sdobs::restart)load(currentId,plan.active?0:config["start"].toDouble(),false);
            else if(cmd==sdobs::stop){worker.reset();++generation;delivery.reset();clear(sdobs::stopped);}
            else if(cmd==sdobs::pause&&worker){if(delivery&&delivery->ranged&&!delivery->videoReady){delivery->pauseWhenReady=true;delivery->muted=true;}else worker->command({{"op","pause"}});}
            else if(cmd==sdobs::resume){if(worker&&worker->status()["state"]!="completed"&&worker->status()["state"]!="failed"){if(delivery){delivery->pauseWhenReady=false;delivery->muted=false;}{std::lock_guard guard(clockMutex);clockOffset=-1;}worker->command({{"op","resume"}});}else load(currentId,plan.active?0:config["start"].toDouble(),false);}
            else if(plan.active&&(cmd==sdobs::next||cmd==sdobs::previous)){const auto id=sd::adjacentProgramEpisode(plan.program,currentId,cmd==sdobs::next?1:-1);if(!id.isEmpty())load(id,0,paused);}
            if(worker&&delivery&&delivery->pauseWhenReady&&delivery->videoReady){delivery->pauseWhenReady=false;worker->command({{"op","pause"}});}
            if(worker&&delivery&&delivery->ranged&&delivery->videoEnd&&(!delivery->hasAudio||delivery->audioEnd))advance=true;
            if(advance){advance=false;const auto id=plan.active?sd::nextProgramEpisode(plan.program,plan.episodeId):QString();
                if(plan.active&&!id.isEmpty())load(id,0,false);
                else if(!plan.active&&config["loop"].toBool())load({},config["start"].toDouble(),false);
                else{worker.reset();++generation;if(delivery&&plan.active)delivery->position=plan.duration;if(ipc.lock()){ipc.data->state=sdobs::ended;ipc.data->audioRead=ipc.data->audioWrite;if(plan.active)ipc.data->positionMs=qint64(plan.duration*1000);ipc.unlock();}}
            }
        }catch(const std::exception&e){worker.reset();delivery.reset();fail(QString::fromUtf8(e.what()));}});
        load(plan.active?plan.episodeId:QString(),plan.active?0:config["start"].toDouble(),false);commands.start();
        int result=app.exec();commands.stop();worker.reset();if(parent)CloseHandle(parent);return result;
    }catch(const std::exception&e){fail(QString::fromUtf8(e.what()));return 1;}
}
