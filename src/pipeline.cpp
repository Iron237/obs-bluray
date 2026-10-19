#include "pipeline.h"
#include "transport.h"
#include <gst/video/video-event.h>
#include <stdexcept>
#include <thread>

namespace sd {
static constexpr auto boundedQueue="queue max-size-time=3000000000 max-size-buffers=0 max-size-bytes=0";
static void setEnum(GObject *object,const char *name,const QString &value) { gst_util_set_object_arg(object,name,value.toUtf8()); }
MediaWorker::MediaWorker(QString id,QJsonObject config,QObject *parent):QObject(parent),id_(std::move(id)),config_(std::move(config)) {
    currentBitrate_=config_.value("bitrate").toInt(6000);
    connect(&timer_,&QTimer::timeout,this,&MediaWorker::poll);timer_.setInterval(100);
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_,&QLocalServer::newConnection,this,[this]{while(auto *socket=server_.nextPendingConnection()){
        connect(socket,&QLocalSocket::readyRead,this,[this,socket]{if(!socket->canReadLine())return;const auto line=socket->readLine(1024*1024);
            auto r=command(QJsonDocument::fromJson(line).object());socket->write(QJsonDocument(r).toJson(QJsonDocument::Compact)+'\n');socket->flush();socket->disconnectFromServer();});
        connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
    }});
}
MediaWorker::~MediaWorker(){timer_.stop();if(pipeline_)gst_element_set_state(pipeline_,GST_STATE_NULL);if(encoder_)gst_object_unref(encoder_);if(outputSink_)gst_object_unref(outputSink_);if(bus_)gst_object_unref(bus_);if(pipeline_)gst_object_unref(pipeline_);if(disc_)bd_close(disc_);}
void MediaWorker::selectDecoders() {
    QString wanted=config_.value("decoder").toString("auto");if(wanted=="auto")wanted=config_.value("backend").toString("software");
    if(wanted=="amd")wanted="auto";
    auto *features=gst_registry_get_feature_list(gst_registry_get(),GST_TYPE_ELEMENT_FACTORY);
    for(auto *it=features;it;it=it->next){auto *factory=GST_ELEMENT_FACTORY(it->data);const QString name=GST_OBJECT_NAME(factory);
        const QString klass=QString::fromUtf8(gst_element_factory_get_metadata(factory,GST_ELEMENT_METADATA_KLASS));
        if(!klass.contains("Decoder")||!klass.contains("Video"))continue;
        const QString desc=QString::fromUtf8(gst_element_factory_get_metadata(factory,GST_ELEMENT_METADATA_LONGNAME));
        const bool hw=klass.contains("Hardware")||name.startsWith("nv")||name.startsWith("qsv")||name.startsWith("d3d");
        guint rank=GST_RANK_PRIMARY;
        if(wanted=="software")rank=hw?GST_RANK_NONE:GST_RANK_PRIMARY+100;
        else if(wanted=="intel")rank=name.startsWith("qsv")?GST_RANK_PRIMARY+1000:GST_RANK_NONE;
        else if(wanted=="nvidia")rank=(name.startsWith("d3d11")&&desc.contains("NVIDIA"))?GST_RANK_PRIMARY+1000:GST_RANK_NONE;
        gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory),rank);
    }
    gst_plugin_feature_list_free(features);
}
QString MediaWorker::encoderSpec() const {
    const auto backend=config_.value("backend").toString("software"),codec=config_.value("codec").toString("h264");
    const int bitrate=config_.value("bitrate").toInt(6000),gop=config_.value("gop").toInt(48);
    const QString rate=config_.value("rateControl").toString("cbr");
    QString result=encoderFactory(backend,codec)+" name=encoder bitrate="+QString::number(bitrate);
    if(backend=="nvidia")result+=QString(" rc-mode=%1 preset=p4 tune=low-latency gop-size=%2 bframes=0").arg(rate).arg(gop);
    else if(backend=="intel")result+=QString(" rate-control=%1 gop-size=%2 target-usage=4").arg(rate).arg(gop);
    else if(backend=="amd")result+=QString(" rate-control=%1 gop-size=%2").arg(rate).arg(gop);
    else if(codec=="h264")result+=QString(" speed-preset=veryfast tune=zerolatency key-int-max=%1").arg(gop);
    else result+=QString(" speed-preset=ultrafast tune=zerolatency key-int-max=%1").arg(gop);
    return result;
}
QString MediaWorker::outputSpec(const QString &output,const QString &mux) const {
    const bool realtime=config_.value("realtime").toBool(output.contains("://"));const auto sync=realtime?"true":"false";
    const QUrl url(output);QString result;
    if(url.scheme()=="rtmp"||url.scheme()=="rtmps")result="flvmux name="+mux+" streamable=true ! rtmpsink sync="+sync+" location="+gstQuote(output);
    else if(url.scheme()=="srt")result="mpegtsmux name="+mux+" alignment=7 ! srtsink sync="+sync+" auto-reconnect=false poll-timeout=1000 wait-for-connection=true uri="+gstQuote(output);
    else if(url.scheme()=="udp")result="mpegtsmux name="+mux+" alignment=7 ! udpsink sync="+sync+" host="+gstQuote(url.host())+" port="+QString::number(url.port(1234));
    else {
        const QString ext=QFileInfo(output).suffix().toLower();
        result=(ext=="mp4"?"mp4mux":ext=="ts"?"mpegtsmux":"matroskamux")+QString(" name=")+mux;
        result+=" ! filesink sync="+QString(sync)+" location="+gstQuote(QDir::fromNativeSeparators(output));
    }
    const auto position=result.indexOf(" sync=");if(position>=0)result.insert(position," name=sink_"+mux);
    return result;
}
void MediaWorker::attachProbe(const char *name,GstPadProbeCallback callback,gpointer data) {
    auto *element=gst_bin_get_by_name(GST_BIN(pipeline_),name);if(!element)return;
    auto *pad=gst_element_get_static_pad(element,"src");const auto type=GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER|(callback==onLimit?GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM:0));gst_pad_add_probe(pad,type,callback,data,nullptr);gst_object_unref(pad);gst_object_unref(element);
}
void MediaWorker::build() {
    initGst();selectDecoders();
    const QString input=config_["input"].toString();discSource_=isDisc(input);timeline_=config_.value("start").toDouble();
    const auto sourceInfo=inspectSource(input);
    if(!sourceInfo["ok"].toBool())throw std::runtime_error(sourceInfo["error"].toString().toStdString());
    hasAudio_=config_.value("audioTrack").toInt(0)>=0;
    if(discSource_) {
        disc_=bd_open(discRoot(input).toUtf8(),nullptr);if(!disc_)throw std::runtime_error("无法打开 BDMV");
        bd_get_titles(disc_,TITLES_ALL,0);
        const int playlist=config_.value("playlist").toInt(sourceInfo["suggestedPlaylist"].toInt());
        if(!bd_select_playlist(disc_,unsigned(playlist)))throw std::runtime_error("无法选择播放列表");
        auto *info=bd_get_playlist_info(disc_,unsigned(playlist),0);if(!info)throw std::runtime_error("播放列表解析失败");
        totalDuration_=double(info->duration)/90000.0;hasAudio_=hasAudio_&&info->clip_count&&info->clips[0].audio_stream_count;
        if(config_.value("angle").toInt(0)>0)bd_select_angle(disc_,unsigned(config_["angle"].toInt()));
        if(timeline_>=totalDuration_){bd_free_title_info(info);throw std::runtime_error("起始时间超出节目长度");}
        for(unsigned i=0;i<info->clip_count;++i){const auto&clip=info->clips[i];clipOffsets_.append(qint64(clip.start_time)-qint64(clip.in_time));if(double(clip.start_time)/90000.0<=timeline_)currentClip_=int(i);}
        if(info->clip_count&&info->clips[currentClip_].video_stream_count){const auto&clip=info->clips[currentClip_];
            codecHeaders_=codecHeaders(discRoot(input)+"/BDMV/STREAM/"+QString::fromLatin1(clip.clip_id)+".m2ts",clip.video_streams[0].pid,clip.video_streams[0].coding_type==0x24);}
        // Establish the streaming cursor after collecting the codec headers.
        bd_seek_time(disc_,quint64(timeline_*90000));timeline_=double(bd_tell_time(disc_))/90000.0;
        // libbluray reports non-seamless play-item transitions separately from
        // the transport bytes. Pass those discontinuities to the TS demuxer.
        bd_get_event(disc_,nullptr);BD_EVENT initial{};while(bd_get_event(disc_,&initial)){}
        bd_free_title_info(info);
    }else {
        totalDuration_=sourceInfo["duration"].toDouble();bool found=false;for(const auto &s:sourceInfo["streams"].toArray())if(s.toObject()["type"]=="audio")found=true;
        hasAudio_=hasAudio_&&found;if(totalDuration_>0&&timeline_>=totalDuration_)throw std::runtime_error("起始时间超出节目长度");
    }
    const bool copy=config_.value("mode").toString()=="copy";
    const auto backend=config_.value("backend").toString("software");gpu_=!copy&&backend!="software";
    const int adapter=gpuAdapter(backend);const QString adapterOpt=adapter>=0?" adapter="+QString::number(adapter):QString();
    const bool pgs=config_.value("subtitleTrack").toInt(-1)>=0;
    const bool ass=!config_.value("subtitle").toString().isEmpty();
    if(ass){subtitles_=std::make_unique<Subtitles>(config_);subtitles_->load();subtitles_->setTimeline(timeline_);}
    QString size;const int width=config_.value("width").toInt();if(width)size=QString(",width=%1,height=%2").arg(width).arg(config_["height"].toInt());
    QString video=QString(boundedQueue)+" name=videoIn ! identity name=videoLimit single-segment=true ! ";
    if(copy) {
        QString inputCodec="h264";
        if(discSource_){auto *info=bd_get_playlist_info(disc_,unsigned(config_.value("playlist").toInt(sourceInfo["suggestedPlaylist"].toInt())),0);if(info&&info->clip_count&&info->clips[0].video_stream_count&&info->clips[0].video_streams[0].coding_type==0x24)inputCodec="h265";if(info)bd_free_title_info(info);}
        else for(const auto&s:sourceInfo["streams"].toArray())if(s.toObject()["type"]=="video"&&s.toObject()["caps"].toString().contains("h265"))inputCodec="h265";
        if(inputCodec!=config_.value("codec").toString("h264"))throw std::runtime_error("直通模式的输出编码必须与原视频一致");
        video+=inputCodec+"parse name=encoded config-interval=-1 ! tee name=vtee ";
    }else {
        // PGS uses the established CPU compositor; ASS stays on the GPU for hardware encoding.
        if(pgs)video+="d3d11download ! videoconvert ! video/x-raw,format=I420 ! dvdspu name=pgs ! ";
        if(gpu_) {
            video+="d3d11upload"+adapterOpt+" ! d3d11convert"+adapterOpt+" ! video/x-raw(memory:D3D11Memory),format="+(ass?"BGRA":"NV12")+size+" ! ";
            if(ass)video+="d3d11overlay name=assOverlay"+adapterOpt+" ! d3d11convert"+adapterOpt+" ! video/x-raw(memory:D3D11Memory),format=NV12 ! ";
        }else {
            video+="d3d11download ! videoconvert ! videoscale ! video/x-raw,format="+(ass?QString("BGRA"):QString("I420"))+size+" ! ";
            if(ass)video+="identity name=assCpu ! videoconvert ! video/x-raw,format=I420 ! ";
        }
        if(deck_){video+=gpu_?"d3d11convert"+adapterOpt+" ! video/x-raw(memory:D3D11Memory),format=BGRA ! ":"videoconvert ! video/x-raw,format=BGRA ! ";video+="identity name=encoded ! tee name=vtee ";}
        else video+=encoderSpec()+" ! "+config_.value("codec").toString("h264")+"parse name=encoded config-interval=-1 ! tee name=vtee ";
    }
    QString pipeline=video;
    if(hasAudio_) {
        pipeline+=QString(boundedQueue)+" name=audioIn ! identity name=audioLimit single-segment=true ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2"+(deck_?",format=F32LE,layout=interleaved":"")+" ! ";
        if(config_.value("normalizeAudio").toBool())pipeline+="audiodynamic characteristics=soft-knee mode=compressor threshold=0.5 ratio=0.5 ! ";
        if(deck_)pipeline+="appsink name=deckAudio sync=true max-buffers=4 drop=true ";
        else pipeline+="avenc_aac bitrate="+QString::number(config_.value("audioBitrate").toInt(192)*1000)+" ! aacparse ! tee name=atee ";
    }
    const auto output=config_["output"].toString();
    if(!deck_)for(const auto &path:QStringList{output,config_.value("record").toString()}) {
        if(path.isEmpty()||path.contains("://"))continue;
        QFile reservation(path);
        if(!reservation.open(QIODevice::WriteOnly|QIODevice::NewOnly))throw std::runtime_error("输出文件已存在或不可写；任务不会覆盖已有文件");
    }
    if(deck_){
        pipeline+="vtee. ! queue max-size-buffers=2 max-size-time=0 max-size-bytes=0 ! ";
        if(config_["_obsSource"].toBool()&&gpu_)pipeline+="d3d11download ! ";
        pipeline+="appsink name=deckVideo sync=true max-buffers=2 drop=true ";
        if(!config_["_obsSource"].toBool()){
        pipeline+="vtee. ! queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ! ";
        pipeline+=gpu_?"d3d11convert"+adapterOpt+" ! video/x-raw(memory:D3D11Memory),format=BGRA,width=512,height=288 ! d3d11download ! ":"videoscale ! video/x-raw,width=512,height=288 ! ";
        pipeline+="videoconvert ! videorate drop-only=true ! video/x-raw,format=RGB,framerate=10/1 ! jpegenc quality=70 ! appsink name=deckJpeg sync=false max-buffers=1 drop=true ";
        }
    }else{
        pipeline+=outputSpec(output,"outmux")+" vtee. ! "+boundedQueue+" ! outmux. ";
        if(hasAudio_)pipeline+="atee. ! "+QString(boundedQueue)+" ! outmux. ";
    }
    const auto record=deck_?QString():config_.value("record").toString();if(!record.isEmpty()){
        pipeline+=outputSpec(record,"recmux")+" vtee. ! "+boundedQueue+" ! recmux. ";
        if(hasAudio_)pipeline+="atee. ! "+QString(boundedQueue)+" ! recmux. ";
    }
    GError *error=nullptr;pipelineText_=redact(pipeline);pipeline_=gst_parse_launch(pipeline.toUtf8(),&error);
    if(error){QString msg=QString::fromUtf8(error->message);g_clear_error(&error);throw std::runtime_error(msg.toStdString());}
    if(!pipeline_)throw std::runtime_error("无法创建媒体流水线");
    if(context_)gst_element_set_context(pipeline_,context_);
    if(deck_){
        attachJpegMailbox(pipeline_,"deckJpeg",deck_->frames.get());
        for(const char *name:{"deckVideo","deckAudio"})if(auto *sink=gst_bin_get_by_name(GST_BIN(pipeline_),name)){
            GstAppSinkCallbacks callbacks{};
            callbacks.new_sample=+[](GstAppSink*s,gpointer p){auto *self=static_cast<MediaWorker*>(p);auto *sample=gst_app_sink_pull_sample(s);if(!sample)return GST_FLOW_EOS;if(QString::fromUtf8(GST_OBJECT_NAME(s))=="deckVideo")self->deck_->setVideo(sample);else self->deck_->addAudio(sample);gst_sample_unref(sample);return GST_FLOW_OK;};
            callbacks.new_preroll=+[](GstAppSink*s,gpointer p){auto *sample=gst_app_sink_pull_preroll(s);if(sample){if(QString::fromUtf8(GST_OBJECT_NAME(s))=="deckVideo"){auto &deck=static_cast<MediaWorker*>(p)->deck_;if(deck->videoPrerollOutput)deck->videoPrerollOutput(sample);else deck->setVideo(sample);}gst_sample_unref(sample);}return GST_FLOW_OK;};
            gst_app_sink_set_callbacks(GST_APP_SINK(sink),&callbacks,this,nullptr);gst_object_unref(sink);
        }
    }
    auto *source=gst_element_factory_make(discSource_?"decodebin":"uridecodebin","source");
    if(!source)throw std::runtime_error("缺少输入解析组件");
    gst_bin_add(GST_BIN(pipeline_),source);
    const QString caps=copy?"video/x-h264;video/x-h265;audio/x-raw;subpicture/x-pgs":"video/x-raw(ANY);audio/x-raw;subpicture/x-pgs";
    {
        auto *target=gst_caps_from_string(caps.toUtf8());g_object_set(source,"caps",target,nullptr);gst_caps_unref(target);
        g_signal_connect(source,"pad-added",G_CALLBACK(onPad),this);
        g_signal_connect(source,"no-more-pads",G_CALLBACK(+[](GstElement*,gpointer data){auto*self=static_cast<MediaWorker*>(data);QMetaObject::invokeMethod(self,[self]{if(!self->videoLinked_)self->complete(false,"没有找到可解码的视频轨道");else if(self->hasAudio_&&!self->audioLinked_)self->complete(false,"所选音轨不存在或无法解码");else if(self->config_.value("subtitleTrack").toInt(-1)>=0&&!self->subtitleLinked_)self->complete(false,"所选 PGS 字幕轨不存在");});}),this);
    }
    if(discSource_) {
        auto *appsrc=gst_element_factory_make("appsrc","bluraySource");gst_bin_add(GST_BIN(pipeline_),appsrc);
        auto *tsCaps=gst_caps_from_string("video/mpegts,systemstream=true,packetsize=192");
        g_object_set(appsrc,"caps",tsCaps,"format",GST_FORMAT_BYTES,"stream-type",GST_APP_STREAM_TYPE_STREAM,"max-bytes",guint64(2*1024*1024),"block",TRUE,nullptr);gst_caps_unref(tsCaps);
        GstAppSrcCallbacks callbacks{};callbacks.need_data=onNeedData;gst_app_src_set_callbacks(GST_APP_SRC(appsrc),&callbacks,this,nullptr);
        if(!gst_element_link(appsrc,source))throw std::runtime_error("BDMV 输入连接失败");
    }else g_object_set(source,"uri",QUrl::fromLocalFile(QFileInfo(input).absoluteFilePath()).toEncoded().constData(),nullptr);
    g_signal_connect(pipeline_,"deep-element-added",G_CALLBACK(+[](GstBin*,GstBin*,GstElement *element,gpointer data){
        auto*self=static_cast<MediaWorker*>(data);auto*f=gst_element_get_factory(element);if(!f)return;
        const QString klass=QString::fromUtf8(gst_element_factory_get_metadata(f,GST_ELEMENT_METADATA_KLASS));
        const QString factoryName=GST_OBJECT_NAME(f);
        if(!self->codecHeaders_.isEmpty()&&(factoryName=="h264parse"||factoryName=="h265parse")){
            auto *sink=gst_element_get_static_pad(element,"sink");
            gst_pad_add_probe(sink,GST_PAD_PROBE_TYPE_BUFFER,+[](GstPad*,GstPadProbeInfo*info,gpointer user)->GstPadProbeReturn{
                auto*owner=static_cast<MediaWorker*>(user);auto*original=GST_PAD_PROBE_INFO_BUFFER(info);if(!original)return GST_PAD_PROBE_OK;
                const auto&headers=owner->codecHeaders_;const auto size=gst_buffer_get_size(original);auto*combined=gst_buffer_new_allocate(nullptr,gsize(headers.size())+size,nullptr);
                gst_buffer_fill(combined,0,headers.constData(),gsize(headers.size()));GstMapInfo map{};if(gst_buffer_map(combined,&map,GST_MAP_WRITE)){gst_buffer_extract(original,0,map.data+headers.size(),size);gst_buffer_unmap(combined,&map);}
                gst_buffer_copy_into(combined,original,GST_BUFFER_COPY_METADATA,0,gsize(-1));GST_PAD_PROBE_INFO_DATA(info)=combined;gst_buffer_unref(original);return GST_PAD_PROBE_REMOVE;
            },self,nullptr);gst_object_unref(sink);
        }
        if(klass.contains("Decoder")&&klass.contains("Video")){QMutexLocker lock(&self->decoderMutex_);const QString name=GST_OBJECT_NAME(f);if(!self->decoderNames_.contains(name))self->decoderNames_<<name;}
    }),this);
    attachProbe("videoLimit",onLimit,&videoLimit_);if(hasAudio_)attachProbe("audioLimit",onLimit,&audioLimit_);
    attachProbe("encoded",onEncoded,this);
    if(ass&&gpu_) {
        auto *overlay=gst_bin_get_by_name(GST_BIN(pipeline_),"assOverlay");
        g_signal_connect(overlay,"draw",G_CALLBACK(+[](GstElement*,GstObject*,gpointer target,guint64 pts,guint64,gpointer data)->gboolean{
            auto*self=static_cast<MediaWorker*>(data);try{return self->subtitles_->renderGpu(target,pts);}catch(const std::exception&e){const QString msg=QString::fromUtf8(e.what());QMetaObject::invokeMethod(self,[self,msg]{self->complete(false,msg);});return FALSE;}
        }),this);gst_object_unref(overlay);
    }else if(ass)attachProbe("assCpu",onCpuSubtitle,this);
    encoder_=gst_bin_get_by_name(GST_BIN(pipeline_),"encoder");outputSink_=gst_bin_get_by_name(GST_BIN(pipeline_),"sink_outmux");sentPosition_=timeline_;bus_=gst_element_get_bus(pipeline_);
    if(outputSink_){auto *pad=gst_element_get_static_pad(outputSink_,"sink");gst_pad_add_probe(pad,GST_PAD_PROBE_TYPE_BUFFER,+[](GstPad*,GstPadProbeInfo*info,gpointer data)->GstPadProbeReturn{
        auto*self=static_cast<MediaWorker*>(data);auto*buffer=GST_PAD_PROBE_INFO_BUFFER(info);if(buffer&&GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))){const qint64 pts=qint64(GST_BUFFER_PTS(buffer));qint64 first=-1;self->firstOutput_.compare_exchange_strong(first,pts);qint64 last=self->lastOutput_.load();while(pts>last&&!self->lastOutput_.compare_exchange_weak(last,pts)){}}
        return GST_PAD_PROBE_OK;
    },this,nullptr);gst_object_unref(pad);}
}
void MediaWorker::start() {
    try {build();emit sourcePrepared(timeline_);if(!deck_&&!server_.listen(workerName(id_)))throw std::runtime_error("工作进程控制管道创建失败");elapsed_.start();timer_.start();
        const auto result=gst_element_set_state(pipeline_,GST_STATE_PAUSED);if(result==GST_STATE_CHANGE_FAILURE)throw std::runtime_error("媒体预加载失败");
        state_="preroll";publishStatus();
    }catch(const std::exception&e){complete(false,QString::fromUtf8(e.what()));}
}
void MediaWorker::onNeedData(GstAppSrc *source,guint,gpointer data) {
    auto*self=static_cast<MediaWorker*>(data);if(self->stopped_){gst_app_src_end_of_stream(source);return;}
    const int capacity=192*512;GstBuffer *buffer=gst_buffer_new_allocate(nullptr,capacity,nullptr);GstMapInfo map{};gst_buffer_map(buffer,&map,GST_MAP_WRITE);
    const int count=bd_read(self->disc_,map.data,capacity);
    BD_EVENT event{};bool discontinuity=false;
    while(bd_get_event(self->disc_,&event)){
        if(event.event==BD_EVENT_DISCONTINUITY||event.event==BD_EVENT_PLAYITEM)discontinuity=true;
        if(event.event==BD_EVENT_PLAYITEM&&event.param<unsigned(self->clipOffsets_.size()))self->currentClip_=int(event.param);
    }
    if(count>0)rebaseM2ts(map.data,std::size_t(count),self->clipOffsets_.value(self->currentClip_));
    gst_buffer_unmap(buffer,&map);
    if(count<=0){gst_buffer_unref(buffer);gst_app_src_end_of_stream(source);if(count<0)QMetaObject::invokeMethod(self,[self]{self->complete(false,"BDMV 数据读取失败");});return;}
    if(discontinuity)GST_BUFFER_FLAG_SET(buffer,GST_BUFFER_FLAG_DISCONT);
    gst_buffer_resize(buffer,0,count);gst_app_src_push_buffer(source,buffer);
}
void MediaWorker::onPad(GstElement*,GstPad *pad,gpointer data) {
    auto*self=static_cast<MediaWorker*>(data);QMutexLocker lock(&self->trackMutex_);
    auto *caps=gst_pad_get_current_caps(pad);if(!caps)caps=gst_pad_query_caps(pad,nullptr);if(!caps||gst_caps_is_empty(caps)){if(caps)gst_caps_unref(caps);return;}
    const QString type=gst_structure_get_name(gst_caps_get_structure(caps,0));gst_caps_unref(caps);
    const char *target=nullptr,*targetPad="sink";bool *linked=nullptr;
    if(type.startsWith("video/")&&!self->videoLinked_){target="videoIn";linked=&self->videoLinked_;}
    else if(type.startsWith("audio/")){const int index=self->audioSeen_++;if(self->hasAudio_&&index==self->config_.value("audioTrack").toInt(0)){target="audioIn";linked=&self->audioLinked_;}}
    else if(type=="subpicture/x-pgs"){const int index=self->subtitleSeen_++;if(index==self->config_.value("subtitleTrack").toInt(-1)){target="pgs";targetPad="subpicture";linked=&self->subtitleLinked_;}}
    if(target){auto *element=gst_bin_get_by_name(GST_BIN(self->pipeline_),target);if(element){auto *sink=gst_element_get_static_pad(element,targetPad);if(sink){*linked=gst_pad_link(pad,sink)==GST_PAD_LINK_OK;gst_object_unref(sink);}gst_object_unref(element);}}
    else {
        // Drain unselected streams so neither demuxing nor preroll waits on them.
        auto *sink=gst_element_factory_make("fakesink",nullptr);g_object_set(sink,"sync",FALSE,"async",FALSE,nullptr);gst_bin_add(GST_BIN(self->pipeline_),sink);gst_element_sync_state_with_parent(sink);auto *p=gst_element_get_static_pad(sink,"sink");gst_pad_link(pad,p);gst_object_unref(p);
    }
}
GstPadProbeReturn MediaWorker::onLimit(GstPad *pad,GstPadProbeInfo *info,gpointer data) {
    auto *limit=static_cast<Limit*>(data);auto *self=limit->owner;
    if(GST_PAD_PROBE_INFO_TYPE(info)&GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM){
        auto*event=GST_PAD_PROBE_INFO_EVENT(info);if(GST_EVENT_TYPE(event)==GST_EVENT_EOS&&limit->eos)return GST_PAD_PROBE_DROP;return GST_PAD_PROBE_OK;
    }
    auto *buffer=GST_PAD_PROBE_INFO_BUFFER(info);if(!buffer)return GST_PAD_PROBE_OK;
    if(limit->eos)return GST_PAD_PROBE_DROP;
    auto pts=GST_BUFFER_PTS(buffer);if(!GST_CLOCK_TIME_IS_VALID(pts))return GST_PAD_PROBE_OK;
    const double trim=self->discSource_?0:self->config_.value("start").toDouble();
    if(trim>0){
        const quint64 skip=quint64(trim*GST_SECOND);if(pts<skip)return GST_PAD_PROBE_DROP;
        auto *writable=gst_buffer_make_writable(buffer);GST_PAD_PROBE_INFO_DATA(info)=writable;
        GST_BUFFER_PTS(writable)=pts-skip;if(GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DTS(writable)))GST_BUFFER_DTS(writable)=GST_BUFFER_DTS(writable)>=skip?GST_BUFFER_DTS(writable)-skip:0;
        pts=GST_BUFFER_PTS(writable);
    }
    if(!GST_CLOCK_TIME_IS_VALID(limit->first))limit->first=pts;
    const double duration=self->config_.value("duration").toDouble();
    if(duration>0&&pts>=limit->first+quint64(duration*GST_SECOND)) {gst_pad_push_event(pad,gst_event_new_eos());limit->eos=true;return GST_PAD_PROBE_DROP;}
    return GST_PAD_PROBE_OK;
}
GstPadProbeReturn MediaWorker::onEncoded(GstPad*,GstPadProbeInfo *info,gpointer data) {
    auto*self=static_cast<MediaWorker*>(data);auto*buffer=GST_PAD_PROBE_INFO_BUFFER(info);if(buffer){++self->frames_;self->bytes_+=gst_buffer_get_size(buffer);
        const auto pts=GST_BUFFER_PTS(buffer);if(GST_CLOCK_TIME_IS_VALID(pts)){qint64 expected=-1;self->firstEncoded_.compare_exchange_strong(expected,qint64(pts));self->lastEncoded_=qint64(pts);}}
    return GST_PAD_PROBE_OK;
}
GstPadProbeReturn MediaWorker::onCpuSubtitle(GstPad *pad,GstPadProbeInfo *info,gpointer data) {
    auto*self=static_cast<MediaWorker*>(data);auto *buffer=gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));GST_PAD_PROBE_INFO_DATA(info)=buffer;
    auto *caps=gst_pad_get_current_caps(pad);if(caps){self->subtitles_->renderCpu(buffer,caps);gst_caps_unref(caps);}return GST_PAD_PROBE_OK;
}
void MediaWorker::poll() {
    if(finished_)return;
    sampleOutput();
    while(auto *message=gst_bus_pop(bus_)) {
        if(GST_MESSAGE_TYPE(message)==GST_MESSAGE_ERROR){GError *error=nullptr;gchar*debug=nullptr;gst_message_parse_error(message,&error,&debug);const QString sourceName=QString::fromUtf8(GST_OBJECT_NAME(GST_MESSAGE_SRC(message)));const QString msg=redact(QString::fromUtf8(error->message))+" ["+sourceName+"]";diagnostic_=redact(QString::fromUtf8(debug?debug:"")).right(4096);g_clear_error(&error);g_free(debug);gst_message_unref(message);
            if(sourceName=="bluraySource"&&videoLimit_.eos&&(!hasAudio_||audioLimit_.eos))continue;
            complete(false,msg);return;}
        if(GST_MESSAGE_TYPE(message)==GST_MESSAGE_EOS){gst_message_unref(message);complete(true);return;}
        if(GST_MESSAGE_TYPE(message)==GST_MESSAGE_ASYNC_DONE && state_=="preroll") {
            if(deck_&&config_["_paused"].toBool())state_="paused";
            else {state_="running";gst_element_set_state(pipeline_,GST_STATE_PLAYING);}
        }
        gst_message_unref(message);
    }
    if(!durationEos_&&videoLimit_.eos&&(!hasAudio_||audioLimit_.eos)){
        durationEos_=true;stopped_=true;
        auto *source=gst_bin_get_by_name(GST_BIN(pipeline_),"bluraySource");if(source){gst_app_src_end_of_stream(GST_APP_SRC(source));gst_object_unref(source);}
        // Unselected subtitle tracks are drained into fakesinks. They must not
        // make a four-second excerpt wait until the end of a two-hour disc.
        auto *iterator=gst_bin_iterate_sinks(GST_BIN(pipeline_));GValue value=G_VALUE_INIT;
        while(gst_iterator_next(iterator,&value)==GST_ITERATOR_OK){auto *sink=GST_ELEMENT(g_value_get_object(&value));auto *factory=gst_element_get_factory(sink);
            if(factory&&QString::fromUtf8(GST_OBJECT_NAME(factory))=="fakesink")gst_element_send_event(sink,gst_event_new_eos());g_value_reset(&value);}
        g_value_unset(&value);gst_iterator_free(iterator);
    }
    if(state_!="paused"&&elapsed_.elapsed()>30000&&frames_==0){complete(false,"30 秒内没有产生视频帧，请检查媒体和硬件解码器");return;}
    if(elapsed_.elapsed()-lastSampleMs_>=250){const double seconds=(elapsed_.elapsed()-lastSampleMs_)/1000.0;const auto b=bytes_.load(),f=frames_.load();bitrateMeasured_=(b-lastBytes_)*8/seconds/1000;fps_=(f-lastFrames_)/seconds;lastBytes_=b;lastFrames_=f;lastSampleMs_=int(elapsed_.elapsed());publishStatus();}
}
void MediaWorker::sampleOutput() {
    // Never take a media sink lock on the control thread: a synchronized sink
    // can hold it while waiting for its clock. The streaming probe is lock-free.
    const auto first=firstOutput_.load(),last=lastOutput_.load();if(first>=0&&last>=first)sentPosition_=timeline_+double(last-first)/GST_SECOND;
}
QJsonObject MediaWorker::status() const {
    QMutexLocker lock(&decoderMutex_);QJsonArray names;for(const auto &n:decoderNames_)names.append(n);
    return {{"id",id_},{"state",state_},{"error",error_},{"diagnostic",diagnostic_},{"frames",double(frames_.load())},{"videoBytes",double(bytes_.load())},{"fps",fps_},{"videoKbps",bitrateMeasured_},{"elapsed",elapsed_.isValid()?elapsed_.elapsed()/1000.0:0.0},
        {"position",timeline_+(firstEncoded_>=0?double(lastEncoded_-firstEncoded_)/GST_SECOND:0)},{"sentPosition",sentPosition_},{"sourceDuration",totalDuration_},{"targetBitrate",currentBitrate_},
        {"decoders",names},{"gpuCompositing",gpu_&&bool(subtitles_)},{"subtitleFrames",subtitles_?double(subtitles_->renderedFrames.load()):0.0},{"pipeline",pipelineText_},{"pid",double(QCoreApplication::applicationPid())}};
}
void MediaWorker::stop() {
    if(finished_||stopPending_)return;const bool preroll=state_=="preroll";stopPending_=true;state_="stopping";stopped_=true;
    // EOS needs PLAYING to drain a network sink. Otherwise an early stop
    // suppresses the preroll transition and waits forever in PAUSED.
    if(preroll&&pipeline_)gst_element_set_state(pipeline_,GST_STATE_PLAYING);
    if(pipeline_)gst_element_send_event(pipeline_,gst_event_new_eos());else complete(true);
    QTimer::singleShot(8000,this,[this]{if(!finished_)complete(false,"等待封装结束超时；输出文件可能不完整");});
}
void MediaWorker::complete(bool ok,const QString &error) {
    if(finished_)return;finished_=true;stopped_=true;timer_.stop();error_=redact(error);state_=ok?(stopPending_?"stopped":"completed"):"failed";
    sampleOutput();if(pipeline_)gst_element_set_state(pipeline_,GST_STATE_NULL);
    publishStatus();if(!deck_)printJson({{"event","finished"},{"ok",ok},{"error",error_}});emit finished(ok);
}
void MediaWorker::publishStatus(){const auto s=status();if(deck_){QMutexLocker guard(&deck_->mutex);deck_->status=s;}else printJson({{"event","status"},{"status",s}});emit statusChanged(s);}
QJsonObject MediaWorker::command(const QJsonObject &r) {
    const QString op=r["op"].toString();
    if(op=="status")return {{"ok",true},{"status",status()}};
    if(deck_&&(op=="pause"||op=="resume")){
        if(finished_||!pipeline_)return {{"ok",false},{"error","素材已结束，请重新载入"}};
        const bool pause=op=="pause";gst_element_set_state(pipeline_,pause?GST_STATE_PAUSED:GST_STATE_PLAYING);state_=pause?"paused":"running";
        {QMutexLocker guard(&deck_->mutex);deck_->audio.clear();}publishStatus();return {{"ok",true}};
    }
    if(op=="stop"){stop();return {{"ok",true}};}
    if(op=="bitrate") {
        const int value=r["value"].toInt();if(value<100||value>200000)return {{"ok",false},{"error","码率范围为 100–200000 kbps"}};
        if(!encoder_)return {{"ok",false},{"error","直通模式不能调整视频码率"}};
        auto *property=g_object_class_find_property(G_OBJECT_GET_CLASS(encoder_),"bitrate");
        if(!property || !(property->flags & GST_PARAM_MUTABLE_PLAYING))return {{"ok",false},{"error","当前编码器未声明运行中码率重配置支持，请结束任务后修改"}};
        // Configuration stays immutable while streaming threads read it.
        g_object_set(encoder_,"bitrate",guint(value),nullptr);currentBitrate_=value;
        auto *pad=gst_element_get_static_pad(encoder_,"src");gst_pad_send_event(pad,gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,TRUE,0));gst_object_unref(pad);
        return {{"ok",true},{"value",value}};
    }
    return {{"ok",false},{"error","未知工作进程操作"}};
}
}
