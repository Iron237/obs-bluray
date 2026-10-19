#include <obs-module.h>
#include <objbase.h>
#include <util/platform.h>
#include "raw_ipc.h"
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <fstream>

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void) { return "Blu-ray BDMV playlists and synchronized transparent ASS subtitles"; }
MODULE_EXPORT const char *obs_module_author(void) { return "StreamDesk"; }

namespace {
std::wstring wide(const std::string &s) { if(s.empty())return {}; int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,out.data(),n);out.pop_back();return out; }
std::wstring workerPath() { char *p=obs_module_file("worker/obs-bluray-worker.exe");auto result=wide(p?p:"");bfree(p);return result; }
std::mutex registryMutex;
struct Session;
std::unordered_map<std::string,std::weak_ptr<Session>> sessions;
struct Session {
    sdobs::Mapping ipc; HANDLE process=nullptr,job=nullptr; std::wstring configPath;
    ~Session(){
        if(job)CloseHandle(job);
        if(process){if(WaitForSingleObject(process,1500)==WAIT_TIMEOUT)TerminateProcess(process,0);CloseHandle(process);}
        if(!configPath.empty())DeleteFileW(configPath.c_str());
    }
    bool start(obs_data_t *settings){
        GUID guid;CoCreateGuid(&guid);wchar_t token[40];StringFromGUID2(guid,token,40);
        const std::wstring key=L"Local\\StreamDesk-OBS-"+std::wstring(token);
        if(!ipc.open(key,true))return false;
        wchar_t temp[MAX_PATH];GetTempPathW(MAX_PATH,temp);configPath=std::wstring(temp)+L"StreamDesk-OBS-"+token+L".json";
        HANDLE f=CreateFileW(configPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(f==INVALID_HANDLE_VALUE)return false;
        auto *json=obs_data_get_json(settings);DWORD written=0;const DWORD bytes=DWORD(strlen(json));const bool saved=WriteFile(f,json,bytes,&written,nullptr)&&written==bytes;CloseHandle(f);if(!saved)return false;
        auto exe=workerPath();std::wstring cmd=L"\""+exe+L"\" --map \""+key+L"\" --config \""+configPath+L"\" --parent "+std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
        job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return false;
        if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,std::filesystem::path(exe).parent_path().c_str(),&si,&pi))return false;
        process=pi.hProcess;if(!AssignProcessToJobObject(job,process)){TerminateProcess(process,1);CloseHandle(pi.hThread);return false;}
        ResumeThread(pi.hThread);CloseHandle(pi.hThread);return true;
    }
    void command(sdobs::Command command,int64_t value=0){if(ipc.lock()){ipc.data->command=command;ipc.data->commandValue=value;++ipc.data->commandSequence;ipc.unlock();}}
};
struct Source {
    obs_source_t *source;bool subtitle=false;
    std::shared_ptr<Session> session;
    std::string parent;
    std::atomic<bool> quit=false;std::thread thread;
    std::atomic<int64_t> position=0,duration=0;
    std::atomic<uint32_t> state=sdobs::opening;
    Source(obs_source_t *s,bool sub):source(s),subtitle(sub){}
    ~Source(){stop();if(!subtitle){std::lock_guard guard(registryMutex);sessions.erase(obs_source_get_uuid(source));}}
    void stop(){quit=true;if(thread.joinable())thread.join();session.reset();}
    void update(obs_data_t *settings){
        stop();quit=false;state=sdobs::opening;position=0;duration=0;
        obs_source_output_video(source,nullptr);
        if(subtitle)parent=obs_data_get_string(settings,"parent");
        else{
            const auto *input=obs_data_get_string(settings,"input");if(!input||!*input){state=sdobs::stopped;return;}
            session=std::make_shared<Session>();if(!session->start(settings)){state=sdobs::failed;blog(LOG_ERROR,"[OBS Blu-ray] Cannot start source worker (Windows error %lu)",GetLastError());session.reset();return;}
            std::lock_guard lock(registryMutex);sessions[obs_source_get_uuid(source)]=session;
        }
        thread=std::thread([this]{run();});
    }
    void run(){
        uint64_t sequence=0;std::vector<uint8_t> pixels;std::vector<sdobs::Audio> audio;
        std::string lastError;uint32_t prior=sdobs::opening;
        while(!quit){
            auto s=session;
            if(subtitle){std::lock_guard guard(registryMutex);auto it=sessions.find(parent);s=it==sessions.end()?nullptr:it->second.lock();}
            if(!s){if(prior!=sdobs::stopped){obs_source_output_video(source,nullptr);prior=sdobs::stopped;}state=sdobs::stopped;Sleep(20);continue;}
            uint32_t width=0,height=0,stride=0;uint64_t timestamp=0;bool newFrame=false;std::string error;
            if(s->ipc.lock(10)){
                auto *h=s->ipc.data;state=h->state;position=h->positionMs;duration=h->durationMs;error=h->error;
                if(h->videoSequence!=sequence&&h->videoBytes<=sdobs::maxVideo&&h->width&&h->height&&h->videoBytes==uint64_t(h->stride)*h->height&&h->stride>=h->width*4){
                    sequence=h->videoSequence;width=h->width;height=h->height;stride=h->stride;timestamp=h->videoTimestamp;
                    if(!subtitle||h->separateSubtitles){const auto *p=subtitle?h->subtitle:h->video;pixels.assign(p,p+h->videoBytes);newFrame=true;}
                }
                audio.clear();if(!subtitle){while(h->audioRead<h->audioWrite){const auto &a=h->audio[h->audioRead++%sdobs::audioSlots];if(a.bytes<=sdobs::audioBytes&&a.bytes%8==0)audio.push_back(a);}}
                s->ipc.unlock();
            }
            if(WaitForSingleObject(s->process,0)==WAIT_OBJECT_0&&state!=sdobs::ended&&state!=sdobs::failed){state=sdobs::failed;error="蓝光工作进程已退出";}
            if(error!=lastError&&!error.empty())blog(LOG_ERROR,"[OBS Blu-ray] %s",error.c_str());lastError=error;
            if(newFrame){obs_source_frame frame{};frame.data[0]=pixels.data();frame.linesize[0]=stride;frame.width=width;frame.height=height;frame.timestamp=timestamp;frame.format=VIDEO_FORMAT_BGRA;frame.full_range=true;obs_source_output_video(source,&frame);}
            for(const auto &a:audio){obs_source_audio out{};out.data[0]=a.data;out.frames=a.bytes/8;out.speakers=SPEAKERS_STEREO;out.format=AUDIO_FORMAT_FLOAT;out.samples_per_sec=48000;out.timestamp=a.timestamp;obs_source_output_audio(source,&out);}
            if(state==sdobs::ended&&prior!=sdobs::ended&&!subtitle)obs_source_media_ended(source);
            prior=state.load();Sleep(2);
        }
    }
};
const char *name(void *){return "蓝光原盘 / BDMV (StreamDesk)";}
const char *subtitleName(void *){return "蓝光关联字幕 / ASS (StreamDesk)";}
void defaults(obs_data_t *s){obs_data_set_default_int(s,"playlist",-1);obs_data_set_default_int(s,"audioTrack",0);obs_data_set_default_int(s,"subtitleTrack",-1);obs_data_set_default_string(s,"decoder","software");obs_data_set_default_bool(s,"separateSubtitles",true);}
struct Inventory{obs_data_t *metadata=nullptr;~Inventory(){obs_data_release(metadata);}};
obs_data_t *inspect(const char *input){
    if(!input||!*input)return nullptr;
    GUID guid;CoCreateGuid(&guid);wchar_t token[40],temp[MAX_PATH];StringFromGUID2(guid,token,40);GetTempPathW(MAX_PATH,temp);
    auto path=std::wstring(temp)+L"StreamDesk-inspect-"+token+L".json";auto exe=workerPath();
    std::wstring cmd=L"\""+exe+L"\" --inspect \""+wide(input)+L"\" --inspect-to \""+path+L"\"";
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};obs_data_t *result=nullptr;
    if(CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,std::filesystem::path(exe).parent_path().c_str(),&si,&pi)){
        if(WaitForSingleObject(pi.hProcess,15000)==WAIT_TIMEOUT){TerminateProcess(pi.hProcess,1);WaitForSingleObject(pi.hProcess,2000);}
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        std::ifstream file(path,std::ios::binary);std::string json((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());if(!json.empty())result=obs_data_create_from_json(json.c_str());
    }
    DeleteFileW(path.c_str());return result;
}
bool updateTracks(obs_properties_t *p,obs_property_t *,obs_data_t *settings){
    auto *inventory=static_cast<Inventory*>(obs_properties_get_param(p));if(!inventory||!inventory->metadata)return false;
    auto *audio=obs_properties_get(p,"audioTrack"),*pgs=obs_properties_get(p,"subtitleTrack"),*chapters=obs_properties_get(p,"chapter");
    obs_property_list_clear(audio);obs_property_list_clear(pgs);obs_property_list_clear(chapters);
    obs_property_list_add_int(audio,"关闭音频",-1);obs_property_list_add_int(pgs,"关闭盘内 PGS",-1);obs_property_list_add_int(chapters,"按起始秒数播放",0);
    const auto chosen=obs_data_get_int(settings,"playlist");const auto playlist=chosen<0?obs_data_get_int(inventory->metadata,"suggestedPlaylist"):chosen;
    auto *titles=obs_data_get_array(inventory->metadata,"titles");
    for(size_t i=0;i<obs_data_array_count(titles);++i){auto *title=obs_data_array_item(titles,i);
        if(obs_data_get_int(title,"playlist")==playlist){
            for(auto name:{"audio","subtitles","chapters"}){auto *items=obs_data_get_array(title,name);for(size_t n=0;n<obs_data_array_count(items);++n){auto *item=obs_data_array_item(items,n);std::string label;
                if(strcmp(name,"chapters")==0){const auto seconds=int(obs_data_get_double(item,"start"));label="第 "+std::to_string(n+1)+" 章 · "+std::to_string(seconds/60)+":"+(seconds%60<10?"0":"")+std::to_string(seconds%60);obs_property_list_add_int(chapters,label.c_str(),n+1);}
                else{label=std::to_string(n)+" · "+obs_data_get_string(item,"language");obs_property_list_add_int(strcmp(name,"audio")==0?audio:pgs,label.c_str(),n);}
                obs_data_release(item);}obs_data_array_release(items);}
        }
        obs_data_release(title);
    }
    obs_data_array_release(titles);return true;
}
bool updateInventory(obs_properties_t *p,obs_property_t *,obs_data_t *settings){
    auto *inventory=static_cast<Inventory*>(obs_properties_get_param(p));obs_data_release(inventory->metadata);inventory->metadata=inspect(obs_data_get_string(settings,"input"));
    auto *list=obs_properties_get(p,"playlist");obs_property_list_clear(list);obs_property_list_add_int(list,"自动选择最长节目",-1);
    if(inventory->metadata){auto *titles=obs_data_get_array(inventory->metadata,"titles");for(size_t i=0;i<obs_data_array_count(titles);++i){auto *title=obs_data_array_item(titles,i);auto seconds=int(obs_data_get_double(title,"duration"));const auto id=obs_data_get_int(title,"playlist");char label[128];snprintf(label,sizeof(label),"%05lld.mpls · %02d:%02d:%02d",id,seconds/3600,seconds/60%60,seconds%60);obs_property_list_add_int(list,label,id);obs_data_release(title);}obs_data_array_release(titles);}
    return updateTracks(p,nullptr,settings);
}
obs_properties_t *properties(void *){
    auto *p=obs_properties_create();
    obs_properties_set_flags(p,OBS_PROPERTIES_DEFER_UPDATE);
    obs_properties_set_param(p,new Inventory,+[](void *v){delete static_cast<Inventory*>(v);});
    auto *input=obs_properties_add_path(p,"input","蓝光根目录或 BDMV 目录",OBS_PATH_DIRECTORY,nullptr,nullptr);obs_property_set_modified_callback(input,updateInventory);
    auto *playlist=obs_properties_add_list(p,"playlist","节目 / MPLS 播放列表",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);obs_property_list_add_int(playlist,"自动选择最长节目",-1);obs_property_set_modified_callback(playlist,updateTracks);
    auto *chapter=obs_properties_add_list(p,"chapter","起始章节",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);obs_property_list_add_int(chapter,"按起始秒数播放",0);
    obs_properties_add_float(p,"start","起始时间（秒）",0,86400,0.1);
    obs_properties_add_int(p,"angle","角度（从 0 开始）",0,8,1);
    auto *audio=obs_properties_add_list(p,"audioTrack","音轨",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);obs_property_list_add_int(audio,"默认音轨",0);obs_property_list_add_int(audio,"关闭音频",-1);
    auto *pgs=obs_properties_add_list(p,"subtitleTrack","盘内 PGS（合成到画面）",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);obs_property_list_add_int(pgs,"关闭盘内 PGS",-1);
    auto *d=obs_properties_add_list(p,"decoder","视频解码",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(d,"CPU（兼容优先）","software");obs_property_list_add_string(d,"NVIDIA D3D11","nvidia");obs_property_list_add_string(d,"Intel Quick Sync","intel");
    obs_properties_add_path(p,"subtitle","外挂字幕",OBS_PATH_FILE,"Subtitle (*.ass *.ssa *.srt)",nullptr);
    obs_properties_add_path(p,"fontsDir","字幕字体目录（可选）",OBS_PATH_DIRECTORY,nullptr,nullptr);
    obs_properties_add_float(p,"subtitleOffset","字幕延迟（秒，正值延后）",-3600,3600,0.1);
    obs_properties_add_bool(p,"separateSubtitles","外挂字幕输出为独立透明源（另添加“蓝光关联字幕”）");
    obs_properties_add_bool(p,"loop","节目结束后循环播放");return p;
}
obs_properties_t *subtitleProperties(void *){
    auto *p=obs_properties_create();auto *list=obs_properties_add_list(p,"parent","关联蓝光画面源",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(list,"请选择蓝光画面源","");
    obs_enum_sources(+[](void *param,obs_source_t *s){if(strcmp(obs_source_get_id(s),"streamdesk_bluray")==0)obs_property_list_add_string(static_cast<obs_property_t*>(param),obs_source_get_name(s),obs_source_get_uuid(s));return true;},list);
    return p;
}
void *create(obs_data_t *settings,obs_source_t *s){auto *data=new Source(s,false);data->update(settings);return data;}
void *createSubtitle(obs_data_t *settings,obs_source_t *s){auto *data=new Source(s,true);data->update(settings);return data;}
void destroy(void *p){delete static_cast<Source*>(p);}
void update(void *p,obs_data_t *s){static_cast<Source*>(p)->update(s);}
void playPause(void *p,bool pause){auto *s=static_cast<Source*>(p);if(s->session)s->session->command(pause?sdobs::pause:sdobs::resume);}
void restart(void *p){auto *s=static_cast<Source*>(p);if(s->session)s->session->command(sdobs::restart);}
void stop(void *p){auto *s=static_cast<Source*>(p);if(s->session)s->session->command(sdobs::stop);}
void seek(void *p,int64_t time){auto *s=static_cast<Source*>(p);if(s->session)s->session->command(sdobs::seek,time);}
obs_media_state mediaState(void *p){switch(static_cast<Source*>(p)->state.load()){case sdobs::playing:return OBS_MEDIA_STATE_PLAYING;case sdobs::paused:return OBS_MEDIA_STATE_PAUSED;case sdobs::stopped:return OBS_MEDIA_STATE_STOPPED;case sdobs::ended:return OBS_MEDIA_STATE_ENDED;case sdobs::failed:return OBS_MEDIA_STATE_ERROR;default:return OBS_MEDIA_STATE_OPENING;}}
}
bool obs_module_load(void){
    obs_source_info info{};info.id="streamdesk_bluray";info.type=OBS_SOURCE_TYPE_INPUT;info.output_flags=OBS_SOURCE_ASYNC_VIDEO|OBS_SOURCE_AUDIO|OBS_SOURCE_CONTROLLABLE_MEDIA;
    info.get_name=name;info.create=create;info.destroy=destroy;info.update=update;info.get_defaults=defaults;info.get_properties=properties;info.icon_type=OBS_ICON_TYPE_MEDIA;
    info.media_play_pause=playPause;info.media_restart=restart;info.media_stop=stop;info.media_set_time=seek;info.media_get_time=+[](void *p)->int64_t{return static_cast<Source*>(p)->position.load();};
    info.media_get_duration=+[](void *p)->int64_t{return static_cast<Source*>(p)->duration.load();};info.media_get_state=mediaState;obs_register_source(&info);
    obs_source_info sub{};sub.id="streamdesk_bluray_subtitle";sub.type=OBS_SOURCE_TYPE_INPUT;sub.output_flags=OBS_SOURCE_ASYNC_VIDEO;sub.get_name=subtitleName;sub.create=createSubtitle;sub.destroy=destroy;sub.update=update;sub.get_properties=subtitleProperties;sub.icon_type=OBS_ICON_TYPE_TEXT;obs_register_source(&sub);
    blog(LOG_INFO,"[OBS Blu-ray] 0.1.0 loaded (raw source + synchronized ASS source)");return true;
}
