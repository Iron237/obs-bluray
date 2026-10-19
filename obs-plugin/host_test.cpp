#include <obs.h>
#include <util/platform.h>
#include <windows.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <string>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <filesystem>

struct Capture {
    std::atomic<uint64_t> video=0,audio=0,transparent=0,nonzeroAlpha=0;
    std::mutex mutex;std::vector<uint8_t> pcm;std::vector<uint8_t> firstVideo;
};
static obs_source_frame *video(void *data,obs_source_frame *frame){
    auto *c=static_cast<Capture*>(data);++c->video;bool transparent=false,nonzero=false;
    for(uint32_t y=0;y<frame->height;y+=4)for(uint32_t x=0;x<frame->width;x+=4){auto a=frame->data[0][size_t(y)*frame->linesize[0]+x*4+3];transparent|=a==0;nonzero|=a>0;}
    if(transparent)++c->transparent;if(nonzero)++c->nonzeroAlpha;
    {std::lock_guard guard(c->mutex);if(c->firstVideo.empty())c->firstVideo.assign(frame->data[0],frame->data[0]+size_t(frame->linesize[0])*frame->height);}
    return frame;
}
static obs_audio_data *audio(void *data,obs_audio_data *audio){
    auto *c=static_cast<Capture*>(data);c->audio+=audio->frames;
    std::lock_guard guard(c->mutex);
    // libobs filter audio is planar float at the host output sample rate.
    if(audio->data[0]&&audio->data[1])for(uint32_t i=0;i<audio->frames;++i)for(int ch=0;ch<2;++ch){auto *p=audio->data[ch]+size_t(i)*4;c->pcm.insert(c->pcm.end(),p,p+4);}
    return audio;
}
static void mixedAudio(void *data,size_t,audio_data *audio){auto *c=static_cast<Capture*>(data);std::lock_guard guard(c->mutex);c->pcm.insert(c->pcm.end(),audio->data[0],audio->data[0]+size_t(audio->frames)*8);}
static void require(bool condition,const char *message){if(!condition)throw std::runtime_error(message);}
static std::string utf8(const wchar_t *s){int n=WideCharToMultiByte(CP_UTF8,0,s,-1,nullptr,0,nullptr,nullptr);std::string out(n,0);WideCharToMultiByte(CP_UTF8,0,s,-1,out.data(),n,nullptr,nullptr);out.pop_back();return out;}
int wmain(int argc,wchar_t **argv){
    if(argc<5)return 2;
    const std::string module=utf8(argv[1]),dataPath=utf8(argv[2]),input=utf8(argv[3]),artifact=utf8(argv[4]);
    try{
        require(obs_startup("zh-CN",nullptr,nullptr),"obs_startup");
        wchar_t obsRoot[32768]{};require(GetEnvironmentVariableW(L"OBS_ROOT",obsRoot,32768)>0,"Set OBS_ROOT to your OBS installation");
        const auto root=std::filesystem::path(obsRoot);const auto obsData=utf8((root/L"data/libobs").c_str())+"/";const auto graphics=utf8((root/L"bin/64bit/libobs-d3d11.dll").c_str());obs_add_data_path(obsData.c_str());
        obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;require(obs_reset_audio(&ai),"obs_reset_audio");
        obs_video_info vi{};vi.graphics_module=graphics.c_str();vi.fps_num=30;vi.fps_den=1;vi.base_width=640;vi.base_height=360;vi.output_width=640;vi.output_height=360;vi.output_format=VIDEO_FORMAT_NV12;vi.colorspace=VIDEO_CS_709;vi.range=VIDEO_RANGE_PARTIAL;vi.scale_type=OBS_SCALE_BILINEAR;require(obs_reset_video(&vi)==OBS_VIDEO_SUCCESS,"obs_reset_video");
        obs_module_t *loaded=nullptr;require(obs_open_module(&loaded,module.c_str(),dataPath.c_str())==MODULE_SUCCESS,"obs_open_module");require(obs_init_module(loaded),"obs_init_module");
        obs_source_info filter{};filter.id="sd_test_capture";filter.type=OBS_SOURCE_TYPE_FILTER;filter.output_flags=OBS_SOURCE_ASYNC_VIDEO|OBS_SOURCE_AUDIO;filter.get_name=+[](void*){return "Capture";};filter.create=+[](obs_data_t *s,obs_source_t*)->void*{return reinterpret_cast<void*>(obs_data_get_int(s,"capture"));};filter.destroy=+[](void*){};filter.filter_video=video;filter.filter_audio=audio;obs_register_source(&filter);
        filter.id="sd_test_capture_video";filter.output_flags=OBS_SOURCE_ASYNC_VIDEO;filter.filter_audio=nullptr;obs_register_source(&filter);
        auto addCapture=[](obs_source_t *source,Capture *capture){auto *d=obs_data_create();obs_data_set_int(d,"capture",reinterpret_cast<int64_t>(capture));const bool hasAudio=obs_source_get_output_flags(source)&OBS_SOURCE_AUDIO;auto *f=obs_source_create_private(hasAudio?"sd_test_capture":"sd_test_capture_video","capture",d);obs_data_release(d);obs_source_filter_add(source,f);obs_source_release(f);};
        auto *settings=obs_data_create();obs_data_set_string(settings,"input",input.c_str());obs_data_set_int(settings,"playlist",-1);obs_data_set_int(settings,"audioTrack",0);obs_data_set_int(settings,"subtitleTrack",-1);obs_data_set_string(settings,"decoder",argc>6?utf8(argv[6]).c_str():"software");obs_data_set_bool(settings,"separateSubtitles",true);
        if(argc>5)obs_data_set_string(settings,"subtitle",utf8(argv[5]).c_str());
        if(argc>7)obs_data_set_double(settings,"start",std::stod(utf8(argv[7])));
        if(argc>8){auto *overrides=obs_data_create_from_json(utf8(argv[8]).c_str());require(overrides,"settings JSON");obs_data_apply(settings,overrides);obs_data_release(overrides);}
        // A real OBS session continuously consumes its mix. Subscribe before
        // creating sources and keep it alive through playback controls; late
        // subscription otherwise tests a stale, unconsumed audio graph.
        Capture mixCapture;audio_convert_info conversion{};conversion.samples_per_sec=48000;conversion.format=AUDIO_FORMAT_FLOAT;conversion.speakers=SPEAKERS_STEREO;
        require(audio_output_connect(obs_get_audio(),0,&conversion,mixedAudio,&mixCapture),"mixed audio subscription");
        auto *source=obs_source_create("streamdesk_bluray","Test Blu-ray",settings,nullptr);require(source,"obs_source_create");
        Capture mainCapture,subCapture;addCapture(source,&mainCapture);
        auto *subSettings=obs_data_create();obs_data_set_string(subSettings,"parent",obs_source_get_uuid(source));auto *sub=obs_source_create("streamdesk_bluray_subtitle","Test subtitles",subSettings,nullptr);obs_data_release(subSettings);require(sub,"subtitle source");addCapture(sub,&subCapture);
        obs_source_inc_showing(source);obs_source_inc_active(source);obs_set_output_source(0,source);
        const auto attributes=GetFileAttributesW(argv[3]);if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)){auto *p=obs_source_properties(source);obs_properties_apply_settings(p,settings);require(obs_property_list_item_count(obs_properties_get(p,"playlist"))>1,"No MPLS inventory");require(obs_property_list_item_count(obs_properties_get(p,"audioTrack"))>1,"No audio inventory");require(obs_property_list_item_count(obs_properties_get(p,"chapter"))>1,"No chapter inventory");obs_properties_destroy(p);}
        auto wait=[&](auto predicate,int milliseconds,const char *error){for(int i=0;i<milliseconds/20;++i){if(predicate())return;Sleep(20);}throw std::runtime_error(error);};
        if(obs_data_get_bool(settings,"_expectError")){
            wait([&]{return obs_source_media_get_state(source)==OBS_MEDIA_STATE_ERROR;},10000,"Invalid source did not report error");
            audio_output_disconnect(obs_get_audio(),0,mixedAudio,&mixCapture);obs_set_output_source(0,nullptr);obs_source_dec_active(source);obs_source_dec_showing(source);obs_source_release(sub);obs_source_release(source);obs_data_release(settings);obs_shutdown();return 0;
        }
        wait([&]{return mainCapture.video>20&&mainCapture.audio>48000;},30000,"No video/audio reached libobs");
        if(argc>5&&wcslen(argv[5]))wait([&]{return subCapture.nonzeroAlpha>0&&subCapture.transparent>0;},15000,"ASS transparency/source sync failed");
        Sleep(2500);
        {std::lock_guard guard(mixCapture.mutex);FILE *file=nullptr;fopen_s(&file,(artifact+".mix.f32").c_str(),"wb");require(file,"mixed audio artifact");fwrite(mixCapture.pcm.data(),1,mixCapture.pcm.size(),file);fclose(file);}
        {std::lock_guard guard(mainCapture.mutex);FILE *file=nullptr;fopen_s(&file,(artifact+".f32").c_str(),"wb");require(file,"audio artifact");fwrite(mainCapture.pcm.data(),1,mainCapture.pcm.size(),file);fclose(file);}
        obs_source_media_play_pause(source,true);wait([&]{return obs_source_media_get_state(source)==OBS_MEDIA_STATE_PAUSED;},5000,"Pause failed");auto time=obs_source_media_get_time(source);Sleep(500);require(std::abs(obs_source_media_get_time(source)-time)<200,"Pause position drift");
        obs_source_media_set_time(source,12000);wait([&]{auto ms=obs_source_media_get_time(source);return ms>7000&&ms<15000;},10000,"Seek failed");
        obs_source_media_play_pause(source,false);wait([&]{return obs_source_media_get_state(source)==OBS_MEDIA_STATE_PLAYING;},7000,"Resume failed");
        obs_source_media_stop(source);wait([&]{return obs_source_media_get_state(source)==OBS_MEDIA_STATE_STOPPED;},7000,"Stop failed");obs_source_media_restart(source);wait([&]{return obs_source_media_get_state(source)==OBS_MEDIA_STATE_PLAYING;},10000,"Restart failed");
        FILE *result=nullptr;fopen_s(&result,(artifact+".json").c_str(),"wb");require(result,"result artifact");fprintf(result,"{\"ok\":true,\"videoFrames\":%llu,\"audioFrames\":%llu,\"subtitleFrames\":%llu,\"transparentFrames\":%llu}\n",mainCapture.video.load(),mainCapture.audio.load(),subCapture.nonzeroAlpha.load(),subCapture.transparent.load());fclose(result);
        audio_output_disconnect(obs_get_audio(),0,mixedAudio,&mixCapture);obs_set_output_source(0,nullptr);obs_source_dec_active(source);obs_source_dec_showing(source);obs_source_release(sub);obs_source_release(source);obs_data_release(settings);obs_shutdown();return 0;
    }catch(const std::exception &e){fprintf(stderr,"TEST FAILED: %s\n",e.what());return 1;}
}
