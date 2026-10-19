#include "catalog.h"
#include "audio_info.h"
#include "disc_metadata.h"
#include <gst/pbutils/pbutils.h>
#include <libbluray/bdnav/meta_data.h>
#include <memory>
namespace sd {
QString discRoot(QString path) {
    path = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
    if (QFileInfo(path).isFile() && QFileInfo(path).suffix().compare("mpls",Qt::CaseInsensitive)==0) path = QFileInfo(path).absolutePath()+"/../..";
    if (QFileInfo(path).fileName().compare("BDMV",Qt::CaseInsensitive)==0) path += "/..";
    return QDir::cleanPath(path);
}
bool isDisc(const QString &path) { return QDir(discRoot(path)+"/BDMV/PLAYLIST").exists(); }
QByteArray codecHeaders(const QString &path,int videoPid,bool hevc) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};
    QByteArray elementary,headers;QSet<int> found;
    // CLPI entry points need not repeat SPS/PPS. Prime the parser with codec
    // headers from the same clip, without decoding the preceding programme.
    const auto data=file.read(4*1024*1024);
    for(qsizetype p=0;p+192<=data.size();p+=192){const auto *ts=reinterpret_cast<const unsigned char*>(data.constData()+p+4);
        if(ts[0]!=0x47||(((ts[1]&0x1f)<<8)|ts[2])!=videoPid||!(ts[3]&0x10))continue;
        int offset=4;if(ts[3]&0x20)offset+=1+ts[4];if(offset>=188)continue;
        if((ts[1]&0x40)&&offset+9<=188){const auto *pes=ts+offset;if(!pes[0]&&!pes[1]&&pes[2]==1)offset+=9+pes[8];}
        if(offset<188)elementary.append(reinterpret_cast<const char*>(ts+offset),188-offset);
    }
    qsizetype cursor=0;
    while(true){const auto start=elementary.indexOf(QByteArray("\0\0\1",3),cursor);if(start<0)break;const auto next=elementary.indexOf(QByteArray("\0\0\1",3),start+3);if(next<0)break;
        const auto nal=elementary.mid(start+3,next-start-3);if(nal.isEmpty())break;const int type=hevc?((static_cast<unsigned char>(nal[0])>>1)&63):(nal[0]&31);
        if((hevc?(type==32||type==33||type==34):(type==7||type==8))&&!found.contains(type)) {headers.append(QByteArray("\0\0\0\1",4));headers.append(nal);found.insert(type);}
        cursor=next;if(found.size()==(hevc?3:2))break;
    }
    return headers;
}
QJsonObject titleJson(const BLURAY_TITLE_INFO *t) {
    QJsonArray chapters, clips, audio, subtitles;
    for (unsigned i=0;i<t->chapter_count;++i) chapters.append(QJsonObject{{"index",int(i)},{"start",double(t->chapters[i].start)/90000.0},{"duration",double(t->chapters[i].duration)/90000.0}});
    for (unsigned i=0;i<t->clip_count;++i) {
        const auto &clip=t->clips[i];
        clips.append(QJsonObject{{"id",QString::fromLatin1(clip.clip_id)},{"in",double(clip.in_time)/90000.0},{"out",double(clip.out_time)/90000.0},{"start",double(clip.start_time)/90000.0}});
        if(i)continue;
        for(unsigned a=0;a<clip.audio_stream_count;++a) audio.append(QJsonObject{{"index",int(a)},{"pid",int(clip.audio_streams[a].pid)},{"language",QString::fromLatin1(reinterpret_cast<const char*>(clip.audio_streams[a].lang))},{"coding",int(clip.audio_streams[a].coding_type)},{"format",int(clip.audio_streams[a].format)},{"rate",int(clip.audio_streams[a].rate)}});
        for(unsigned a=0;a<clip.pg_stream_count;++a) subtitles.append(QJsonObject{{"index",int(a)},{"pid",int(clip.pg_streams[a].pid)},{"language",QString::fromLatin1(reinterpret_cast<const char*>(clip.pg_streams[a].lang))},{"type","PGS"}});
    }
    return {{"playlist",int(t->playlist)},{"duration",double(t->duration)/90000.0},{"angles",int(t->angle_count)},{"chapters",chapters},{"clips",clips},{"audio",audio},{"subtitles",subtitles}};
}
QJsonObject inspectSource(const QString &path) {
    initGst();
    if(!QFileInfo::exists(path))return {{"ok",false},{"error","输入路径不存在"}};
    QJsonObject result{{"ok",true},{"path",path}};
    QJsonArray external;
    QDir dir(QFileInfo(path).isDir()?path:QFileInfo(path).absolutePath());
    for(const auto &s:dir.entryList({"*.ass","*.ssa","*.srt"},QDir::Files))external.append(dir.absoluteFilePath(s));
    if(isDisc(path)) {
        const QString root=discRoot(path); result["kind"]="bdmv";result["root"]=root;
        std::unique_ptr<BLURAY,decltype(&bd_close)> bd(bd_open(root.toUtf8(),nullptr),bd_close);
        if(!bd)return {{"ok",false},{"error","libbluray 无法打开此光盘目录"}};
        const auto *meta=bd_get_meta(bd.get());
        QJsonArray discTitles;
        if(const auto *disc=bd_get_disc_info(bd.get())){
            for(uint32_t i=1;i<=disc->num_titles;++i){const auto *title=disc->titles?disc->titles[i]:nullptr;if(!title)continue;
                discTitles.append(QJsonObject{{"number",int(i)},{"name",QString::fromUtf8(title->name?title->name:"")},{"navigation",title->bdj?"bdj":"hdmv"},{"objectRef",double(title->id_ref)},{"interactive",bool(title->interactive)}});
            }
            result["navigation"]=QJsonObject{{"titleCount",int(disc->num_titles)},{"hdmvTitles",int(disc->num_hdmv_titles)},{"bdjTitles",int(disc->num_bdj_titles)},{"topMenu",disc->top_menu!=nullptr},{"metadataAvailable",meta!=nullptr}};
        }
        // Disc-index title numbers identify navigation objects, not MPLS IDs.
        // Keep optional labels separate; inferring an episode mapping is unsafe.
        result["discTitles"]=discTitles;
        if(meta&&meta->di_name)result["discName"]=QString::fromUtf8(meta->di_name);
        QJsonArray titles;QHash<QString,QJsonArray> audioCache;double maxDuration=-1; int main=-1;
        QDir playlists(root+"/BDMV/PLAYLIST");
        for(const auto &p:playlists.entryList({"*.mpls"},QDir::Files)) {
            auto *info=bd_get_playlist_info(bd.get(),QFileInfo(p).completeBaseName().toUInt(),0);
            if(!info)continue; auto t=titleJson(info);
            if(info->clip_count){const auto clip=QString::fromLatin1(info->clips[0].clip_id);const auto key=clip+QString::fromUtf8(QJsonDocument(t["audio"].toArray()).toJson(QJsonDocument::Compact));if(!audioCache.contains(key))audioCache.insert(key,enrichAudioTracks(root+"/BDMV/STREAM/"+clip+".m2ts",t["audio"].toArray()));t["audio"]=audioCache.value(key);}
            titles.append(t);
            if(t["duration"].toDouble()>maxDuration){maxDuration=t["duration"].toDouble();main=int(info->playlist);}
            bd_free_title_info(info);
        }
        result["titles"]=titles;result["suggestedPlaylist"]=main;result["duration"]=maxDuration;
        result=enrichDiscMetadata(root,result);
        QDir subdir(root+"/BDMV");
        for(const auto &s:subdir.entryList({"*.ass","*.ssa","*.srt"},QDir::Files)) {
            const auto absolute=subdir.absoluteFilePath(s);if(!external.contains(absolute))external.append(absolute);
        }
        if(titles.isEmpty())return {{"ok",false},{"error","未发现可解析的 MPLS 播放列表"}};
    } else {
        result["kind"]="file";
        GError *error=nullptr;auto *discoverer=gst_discoverer_new(15*GST_SECOND,&error);
        if(!discoverer){const QString msg=error?error->message:"discoverer failed";g_clear_error(&error);return {{"ok",false},{"error",msg}};}
        auto *info=gst_discoverer_discover_uri(discoverer,QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toEncoded().constData(),&error);
        if(!info || gst_discoverer_info_get_result(info)!=GST_DISCOVERER_OK) {
            result["ok"]=false;result["error"]=error?QString::fromUtf8(error->message):"无法解析媒体（可能缺少解码插件）";
        } else {
            result["duration"]=double(gst_discoverer_info_get_duration(info))/GST_SECOND;
            result["seekable"]=bool(gst_discoverer_info_get_seekable(info));QJsonArray streams;
            GList *list=gst_discoverer_info_get_stream_list(info);int a=0,s=0;
            for(GList *it=list;it;it=it->next) {
                auto *stream=GST_DISCOVERER_STREAM_INFO(it->data);GstCaps *caps=gst_discoverer_stream_info_get_caps(stream);
                gchar *str=caps?gst_caps_to_string(caps):nullptr;
                QJsonObject item{{"caps",str?QString::fromUtf8(str):QString()}};
                if(GST_IS_DISCOVERER_VIDEO_INFO(stream)) {
                    item["type"]="video";auto *v=GST_DISCOVERER_VIDEO_INFO(stream);
                    item["width"]=int(gst_discoverer_video_info_get_width(v));item["height"]=int(gst_discoverer_video_info_get_height(v));
                }else if(GST_IS_DISCOVERER_AUDIO_INFO(stream)){item["type"]="audio";item["index"]=a++;item["language"]=QString::fromUtf8(gst_discoverer_audio_info_get_language(GST_DISCOVERER_AUDIO_INFO(stream)));}
                else if(GST_IS_DISCOVERER_SUBTITLE_INFO(stream)){item["type"]="subtitle";item["index"]=s++;}
                else item["type"]="other";
                streams.append(item);g_free(str);if(caps)gst_caps_unref(caps);
            }
            gst_discoverer_stream_info_list_free(list);result["streams"]=streams;
        }
        if(info)gst_discoverer_info_unref(info);g_clear_error(&error);g_object_unref(discoverer);
    }
    result["externalSubtitles"]=external;return result;
}
}
