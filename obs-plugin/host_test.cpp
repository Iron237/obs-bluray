#include <obs.h>
#include <obs-encoder.h>
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
#include <thread>

// Test-only QPC-clocked producer. A video delivery gap leaves audio and all
// timestamps running; it never stalls OBS's render thread or rewinds the clock.
struct SyntheticSource {
    obs_source_t *source;std::atomic<bool> quit=false;std::thread thread;int gapMs;uint32_t width,height;
    SyntheticSource(obs_source_t *s,obs_data_t *settings):source(s),gapMs(int(obs_data_get_int(settings,"_syncGapMs"))),width(obs_data_get_bool(settings,"_syncFullHD")?1920:640),height(width==1920?1080:360){thread=std::thread([this]{run();});}
    ~SyntheticSource(){quit=true;thread.join();}
    static bool pulse(double t){constexpr double times[]={2,4.7,7.1,10.4,12.3,15.7,18.2,21.6,24,27.9,30.2,33.6,36.1,39.9,42.2,45.7,48.4,51.8,54.1,57.9,60.2,63.6,66.1,69.9,72.2,75.7,78.4,81.8,84.1,87.9,90.2,93.6,96.1,99.9,102.2,105.7,108.4,111.8,114.1,117.9};for(double p:times)if(t>=p&&t<=p+.125)return true;return false;}
    void run(){
        std::vector<uint32_t> pixels(size_t(width)*height,0xff000000);std::vector<float> pcm(480*2);uint64_t video=0,audio=0;const uint64_t anchor=os_gettime_ns()+100000000;bool previous=false;
        while(!quit){const auto now=os_gettime_ns();
            if(now>=anchor+audio*1000000000/48000){for(size_t i=0;i<480;++i){const double t=double(audio+i)/48000;const float value=pulse(t)?float(.25*std::sin(2*3.141592653589793*1000*t)):0;pcm[i*2]=pcm[i*2+1]=value;}obs_source_audio out{};out.data[0]=reinterpret_cast<const uint8_t*>(pcm.data());out.frames=480;out.speakers=SPEAKERS_STEREO;out.format=AUDIO_FORMAT_FLOAT;out.samples_per_sec=48000;out.timestamp=anchor+audio*1000000000/48000;obs_source_output_audio(source,&out);audio+=480;}
            const uint64_t vtime=video*1001000000000ULL/24000;
            if(now>=anchor+vtime){const double t=double(vtime)/1e9;const bool bright=pulse(t);if(bright!=previous){std::fill(pixels.begin(),pixels.end(),bright?0xffffffff:0xff000000);previous=bright;}if(!(t>=8&&t<8+double(gapMs)/1000)){obs_source_frame frame{};frame.data[0]=reinterpret_cast<uint8_t*>(pixels.data());frame.linesize[0]=width*4;frame.width=width;frame.height=height;frame.timestamp=anchor+vtime;frame.format=VIDEO_FORMAT_BGRA;frame.full_range=true;obs_source_output_video(source,&frame);}++video;}
            Sleep(1);
        }
    }
};

struct TimedFrame { uint64_t timestamp; double luma; };
struct AudioBlock { uint64_t timestamp, firstFrame; uint32_t frames; };
struct PacketCapture { std::mutex mutex; FILE *file=nullptr; };
static void encodedPacket(obs_output_t*,encoder_packet *packet,encoder_packet_time *time,void *data){auto *c=static_cast<PacketCapture*>(data);std::lock_guard guard(c->mutex);fprintf(c->file,"%d,%lld,%lld,%d,%d,%lld,%lld,%llu,%d\n",int(packet->type),packet->pts,packet->dts,packet->timebase_num,packet->timebase_den,packet->dts_usec,packet->sys_dts_usec,time?time->cts:0,packet->keyframe?1:0);}
struct Capture {
    std::atomic<uint64_t> video=0,audio=0,transparent=0,nonzeroAlpha=0;
    std::mutex mutex;std::vector<uint8_t> pcm;std::vector<uint8_t> firstVideo;
    std::vector<TimedFrame> timing;std::vector<AudioBlock> blocks;
};
static double centerLuma(const uint8_t *pixels,uint32_t stride,uint32_t width,uint32_t height){
    if(width<8||height<8)return 0;
    double sum=0;for(uint32_t y=height/2-4;y<height/2+4;++y)for(uint32_t x=width/2-4;x<width/2+4;++x){auto *p=pixels+size_t(y)*stride+x*4;sum+=(p[0]+p[1]+p[2])/3.;}return sum/64;
}
static obs_source_frame *video(void *data,obs_source_frame *frame){
    auto *c=static_cast<Capture*>(data);++c->video;bool transparent=false,nonzero=false;
    for(uint32_t y=0;y<frame->height;y+=4)for(uint32_t x=0;x<frame->width;x+=4){auto a=frame->data[0][size_t(y)*frame->linesize[0]+x*4+3];transparent|=a==0;nonzero|=a>0;}
    if(transparent)++c->transparent;if(nonzero)++c->nonzeroAlpha;
    {std::lock_guard guard(c->mutex);if(c->firstVideo.empty())c->firstVideo.assign(frame->data[0],frame->data[0]+size_t(frame->linesize[0])*frame->height);c->timing.push_back({frame->timestamp,centerLuma(frame->data[0],frame->linesize[0],frame->width,frame->height)});}
    return frame;
}
static obs_audio_data *audio(void *data,obs_audio_data *audio){
    auto *c=static_cast<Capture*>(data);c->audio+=audio->frames;
    std::lock_guard guard(c->mutex);
    // libobs filter audio is planar float at the host output sample rate.
    if(audio->data[0]&&audio->data[1])for(uint32_t i=0;i<audio->frames;++i)for(int ch=0;ch<2;++ch){auto *p=audio->data[ch]+size_t(i)*4;c->pcm.insert(c->pcm.end(),p,p+4);}
    return audio;
}
static void mixedAudio(void *data,size_t,audio_data *audio){auto *c=static_cast<Capture*>(data);std::lock_guard guard(c->mutex);c->blocks.push_back({audio->timestamp,c->pcm.size()/8,audio->frames});c->pcm.insert(c->pcm.end(),audio->data[0],audio->data[0]+size_t(audio->frames)*8);}
static void renderedVideo(void *data,video_data *frame){auto *c=static_cast<Capture*>(data);std::lock_guard guard(c->mutex);c->timing.push_back({frame->timestamp,centerLuma(frame->data[0],frame->linesize[0],640,360)});}
static void require(bool condition,const char *message){if(!condition)throw std::runtime_error(message);}
static std::string utf8(const wchar_t *s){int n=WideCharToMultiByte(CP_UTF8,0,s,-1,nullptr,0,nullptr,nullptr);std::string out(n,0);WideCharToMultiByte(CP_UTF8,0,s,-1,out.data(),n,nullptr,nullptr);out.pop_back();return out;}
int wmain(int argc,wchar_t **argv){
    if(argc<5)return 2;
    const std::string module=utf8(argv[1]),dataPath=utf8(argv[2]),input=utf8(argv[3]),artifact=utf8(argv[4]);
    try{
        require(obs_startup("zh-CN",nullptr,nullptr),"obs_startup");
        auto *overrides=argc>8?obs_data_create_from_json(utf8(argv[8]).c_str()):nullptr;require(argc<=8||overrides,"settings JSON");
        const int syncSeconds=overrides?int(obs_data_get_int(overrides,"_syncTestSeconds")):0;
        const bool syncFullHD=overrides&&obs_data_get_bool(overrides,"_syncFullHD");
        wchar_t obsRoot[32768]{};require(GetEnvironmentVariableW(L"OBS_ROOT",obsRoot,32768)>0,"Set OBS_ROOT to your OBS installation");
        const auto root=std::filesystem::path(obsRoot);require(AddDllDirectory((root/L"bin/64bit").c_str())!=nullptr,"OBS dependency search path");const auto obsData=utf8((root/L"data/libobs").c_str())+"/";const auto graphics=utf8((root/L"bin/64bit/libobs-d3d11.dll").c_str());obs_add_data_path(obsData.c_str());
        obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;require(obs_reset_audio(&ai),"obs_reset_audio");
        obs_video_info vi{};vi.graphics_module=graphics.c_str();vi.fps_num=syncSeconds?60:30;vi.fps_den=1;vi.base_width=syncFullHD?1920:640;vi.base_height=syncFullHD?1080:360;vi.output_width=vi.base_width;vi.output_height=vi.base_height;vi.output_format=VIDEO_FORMAT_NV12;vi.colorspace=VIDEO_CS_709;vi.range=VIDEO_RANGE_PARTIAL;vi.scale_type=OBS_SCALE_BILINEAR;require(obs_reset_video(&vi)==OBS_VIDEO_SUCCESS,"obs_reset_video");
        obs_module_t *loaded=nullptr;require(obs_open_module(&loaded,module.c_str(),dataPath.c_str())==MODULE_SUCCESS,"obs_open_module");require(obs_init_module(loaded),"obs_init_module");
        obs_source_info filter{};filter.id="sd_test_capture";filter.type=OBS_SOURCE_TYPE_FILTER;filter.output_flags=OBS_SOURCE_ASYNC_VIDEO|OBS_SOURCE_AUDIO;filter.get_name=+[](void*){return "Capture";};filter.create=+[](obs_data_t *s,obs_source_t*)->void*{return reinterpret_cast<void*>(obs_data_get_int(s,"capture"));};filter.destroy=+[](void*){};filter.filter_video=video;filter.filter_audio=audio;obs_register_source(&filter);
        filter.id="sd_test_capture_video";filter.output_flags=OBS_SOURCE_ASYNC_VIDEO;filter.filter_audio=nullptr;obs_register_source(&filter);
        obs_source_info syntheticInfo{};syntheticInfo.id="sd_sync_synthetic";syntheticInfo.type=OBS_SOURCE_TYPE_INPUT;syntheticInfo.output_flags=OBS_SOURCE_ASYNC_VIDEO|OBS_SOURCE_AUDIO;syntheticInfo.get_name=+[](void*){return "QPC sync fixture";};syntheticInfo.create=+[](obs_data_t *s,obs_source_t *source)->void*{return new SyntheticSource(source,s);};syntheticInfo.destroy=+[](void *p){delete static_cast<SyntheticSource*>(p);};obs_register_source(&syntheticInfo);
        auto addCapture=[](obs_source_t *source,Capture *capture){auto *d=obs_data_create();obs_data_set_int(d,"capture",reinterpret_cast<int64_t>(capture));const bool hasAudio=obs_source_get_output_flags(source)&OBS_SOURCE_AUDIO;auto *f=obs_source_create_private(hasAudio?"sd_test_capture":"sd_test_capture_video","capture",d);obs_data_release(d);obs_source_filter_add(source,f);obs_source_release(f);};
        auto *settings=obs_data_create();obs_data_set_string(settings,"input",input.c_str());obs_data_set_int(settings,"playlist",-1);obs_data_set_int(settings,"audioTrack",0);obs_data_set_int(settings,"subtitleTrack",-1);obs_data_set_string(settings,"decoder",argc>6?utf8(argv[6]).c_str():"software");obs_data_set_bool(settings,"separateSubtitles",true);
        if(argc>5)obs_data_set_string(settings,"subtitle",utf8(argv[5]).c_str());
        if(argc>7)obs_data_set_double(settings,"start",std::stod(utf8(argv[7])));
        if(overrides){obs_data_apply(settings,overrides);obs_data_release(overrides);}
        // A real OBS session continuously consumes its mix. Subscribe before
        // creating sources and keep it alive through playback controls; late
        // subscription otherwise tests a stale, unconsumed audio graph.
        Capture mixCapture;audio_convert_info conversion{};conversion.samples_per_sec=48000;conversion.format=AUDIO_FORMAT_FLOAT;conversion.speakers=SPEAKERS_STEREO;
        require(audio_output_connect(obs_get_audio(),0,&conversion,mixedAudio,&mixCapture),"mixed audio subscription");
        Capture renderedCapture;video_scale_info videoConversion{};videoConversion.format=VIDEO_FORMAT_BGRA;videoConversion.width=640;videoConversion.height=360;videoConversion.range=VIDEO_RANGE_FULL;videoConversion.colorspace=VIDEO_CS_709;
        if(syncSeconds)obs_add_raw_video_callback(&videoConversion,renderedVideo,&renderedCapture);
        const bool synthetic=obs_data_get_bool(settings,"_syncSynthetic");
        auto *source=obs_source_create(synthetic?"sd_sync_synthetic":"streamdesk_bluray","Test Blu-ray",settings,nullptr);require(source,"obs_source_create");
        if(obs_data_get_bool(settings,"_syncUnbuffered"))obs_source_set_async_unbuffered(source,true);
        Capture mainCapture,subCapture;addCapture(source,&mainCapture);
        obs_source_t *sub=nullptr;if(!synthetic){auto *subSettings=obs_data_create();obs_data_set_string(subSettings,"parent",obs_source_get_uuid(source));sub=obs_source_create("streamdesk_bluray_subtitle","Test subtitles",subSettings,nullptr);obs_data_release(subSettings);require(sub,"subtitle source");addCapture(sub,&subCapture);}
        if(!synthetic&&!obs_data_get_bool(settings,"_allowBufferedLegacy")){require(obs_source_async_unbuffered(source),"Blu-ray source must not accumulate an OBS async backlog");require(obs_source_async_unbuffered(sub),"ASS source must use the same unbuffered presentation policy");}
        obs_source_inc_showing(source);obs_source_inc_active(source);obs_set_output_source(0,source);
        obs_scene_t *scene=nullptr;obs_source_t *second=nullptr;
        if(syncSeconds){
            scene=obs_scene_create("Sync composition");require(scene,"sync scene");obs_scene_add(scene,source);if(sub)obs_scene_add(scene,sub);
            if(obs_data_get_bool(settings,"_syncSecondSource")){second=obs_source_create("streamdesk_bluray","Second async source",settings,nullptr);require(second,"second source");obs_source_set_muted(second,true);auto *item=obs_scene_add(scene,second);vec2 scale{.25f,.25f};obs_sceneitem_set_scale(item,&scale);}
            obs_set_output_source(0,obs_scene_get_source(scene));
        }
        if(obs_data_get_bool(settings,"_programPropertiesTest")){
            auto *draft=obs_data_create_from_json(obs_data_get_json(settings));const std::string initialProgram=obs_data_get_string(draft,"programJson");auto *properties=obs_source_properties(source);obs_properties_apply_settings(properties,draft);require(initialProgram==obs_data_get_string(draft,"programJson"),"Initial property callbacks corrupted the saved program");
            require(obs_property_list_item_count(obs_properties_get(properties,"programEpisode"))==3,"Enabled episode choices not loaded");
            require(!obs_property_visible(obs_properties_get(properties,"playlist")),"Legacy playlist must be hidden in program mode");
            require(!obs_property_visible(obs_properties_get(properties,"subtitle")),"Legacy subtitle must be hidden in program mode");
            require(obs_property_visible(obs_properties_get(properties,"programMode")),"Program controls missing");
            auto *audioChoices=obs_properties_get(properties,"audioTrack");require(obs_property_list_item_count(audioChoices)>1,"Program audio choices missing");require(std::string(obs_property_list_item_name(audioChoices,1)).find("LPCM")!=std::string::npos,"Detailed LPCM track label missing");require(std::string(obs_property_description(obs_properties_get(properties,"audioDetails"))).find("LPCM")!=std::string::npos,"Selected audio information not presented");
            auto *actual=obs_source_get_settings(source);const std::string before=obs_data_get_string(actual,"programJson");obs_data_release(actual);
            obs_data_set_string(draft,"programMode","single");obs_property_modified(obs_properties_get(properties,"programMode"),draft);
            obs_data_set_string(draft,"programEpisode","b");obs_property_modified(obs_properties_get(properties,"programEpisode"),draft);
            auto *program=obs_data_create_from_json(obs_data_get_string(draft,"programJson"));require(program,"Draft program did not parse");require(std::string(obs_data_get_string(program,"playMode"))=="single","Mode change not saved to draft");require(std::string(obs_data_get_string(program,"selectedEpisode"))=="b","Episode selection not saved to draft");obs_data_release(program);
            actual=obs_source_get_settings(source);require(before==obs_data_get_string(actual,"programJson"),"Editing a draft changed the playing source");obs_data_release(actual);
            require(obs_property_button_clicked(obs_properties_get(properties,"programDisable"),nullptr),"Disable button callback failed");
            require(!*obs_data_get_string(draft,"programJson"),"Disable did not clear draft");require(obs_property_visible(obs_properties_get(properties,"playlist")),"Legacy controls did not return");require(!obs_property_visible(obs_properties_get(properties,"programMode")),"Program controls did not hide");
            obs_properties_destroy(properties);obs_data_release(draft);
            FILE *report=nullptr;fopen_s(&report,(artifact+".properties.json").c_str(),"wb");require(report,"Property test report");fputs("{\"ok\":true,\"enabledEpisodes\":3,\"draftIsolation\":true,\"legacyVisibility\":true}",report);fclose(report);
            audio_output_disconnect(obs_get_audio(),0,mixedAudio,&mixCapture);obs_set_output_source(0,nullptr);obs_source_dec_active(source);obs_source_dec_showing(source);obs_source_release(sub);obs_source_release(source);obs_data_release(settings);obs_wait_for_destroy_queue();obs_shutdown();return 0;
        }
        const auto attributes=GetFileAttributesW(argv[3]);if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)){auto *p=obs_source_properties(source);obs_properties_apply_settings(p,settings);require(obs_property_list_item_count(obs_properties_get(p,"playlist"))>1,"No MPLS inventory");require(obs_property_list_item_count(obs_properties_get(p,"audioTrack"))>1,"No audio inventory");require(obs_property_list_item_count(obs_properties_get(p,"chapter"))>1,"No chapter inventory");obs_properties_destroy(p);}
        auto wait=[&](auto predicate,int milliseconds,const char *error){for(int i=0;i<milliseconds/20;++i){if(predicate())return;Sleep(20);}throw std::runtime_error(error);};
        if(obs_data_get_bool(settings,"_expectError")){
            wait([&]{return obs_source_media_get_state(source)==OBS_MEDIA_STATE_ERROR;},10000,"Invalid source did not report error");
            audio_output_disconnect(obs_get_audio(),0,mixedAudio,&mixCapture);obs_set_output_source(0,nullptr);obs_source_dec_active(source);obs_source_dec_showing(source);obs_source_release(sub);obs_source_release(source);obs_data_release(settings);obs_shutdown();return 0;
        }
        wait([&]{return mainCapture.video>20&&mainCapture.audio>48000;},30000,"No video/audio reached libobs");
        if(argc>5&&wcslen(argv[5]))wait([&]{return subCapture.nonzeroAlpha>0&&subCapture.transparent>0;},15000,"ASS transparency/source sync failed");
        if(syncSeconds){
            require(syncSeconds>=10&&syncSeconds<=120,"sync duration must be 10..120 seconds");
            obs_output_t *encoded=nullptr;obs_encoder_t *videoEncoder=nullptr,*audioEncoder=nullptr;obs_service_t *service=nullptr;
            PacketCapture packetCapture;
            const char *encodeUrl=obs_data_get_string(settings,"_syncEncodeUrl");
            if(*encodeUrl){
                auto load=[&](const wchar_t *name){const auto binary=utf8((root/L"obs-plugins/64bit"/(std::wstring(name)+L".dll")).c_str());const auto data=utf8((root/L"data/obs-plugins"/name).c_str());obs_module_t *m=nullptr;require(obs_open_module(&m,binary.c_str(),data.c_str())==MODULE_SUCCESS,"encoder module load");require(obs_init_module(m),"encoder module init");};
                load(L"obs-nvenc");load(L"obs-ffmpeg");load(L"rtmp-services");obs_post_load_modules();
                auto *v=obs_data_create();obs_data_set_string(v,"rate_control","VBR");obs_data_set_int(v,"bitrate",6000);obs_data_set_int(v,"max_bitrate",7000);videoEncoder=obs_video_encoder_create("obs_nvenc_hevc_tex","Sync NVENC HEVC",v,nullptr);obs_data_release(v);require(videoEncoder,"NVENC encoder create");obs_encoder_set_video(videoEncoder,obs_get_video());
                auto *a=obs_data_create();obs_data_set_int(a,"bitrate",320);audioEncoder=obs_audio_encoder_create("ffmpeg_aac","Sync AAC",a,0,nullptr);obs_data_release(a);require(audioEncoder,"AAC encoder create");obs_encoder_set_audio(audioEncoder,obs_get_audio());
                auto *s=obs_data_create();obs_data_set_string(s,"server",encodeUrl);service=obs_service_create("rtmp_custom","Sync loopback SRT",s,nullptr);obs_data_release(s);require(service,"loopback service create");
                encoded=obs_output_create("ffmpeg_mpegts_muxer","Sync encoded output",nullptr,nullptr);require(encoded,"MPEGTS output create");obs_output_set_video_encoder(encoded,videoEncoder);obs_output_set_audio_encoder(encoded,audioEncoder,0);obs_output_set_service(encoded,service);fopen_s(&packetCapture.file,(artifact+".packets.csv").c_str(),"wb");require(packetCapture.file,"encoded packet timing artifact");fprintf(packetCapture.file,"type,pts,dts,tb_num,tb_den,dts_usec,sys_dts_usec,cts_ns,keyframe\n");obs_output_add_packet_callback(encoded,encodedPacket,&packetCapture);require(obs_output_start(encoded),"encoded MPEGTS output start");
            }
            Sleep(DWORD(syncSeconds)*1000);
            if(encoded){obs_output_stop(encoded);for(int i=0;i<250&&obs_output_active(encoded);++i)Sleep(20);require(!obs_output_active(encoded),"encoded output stop");obs_output_remove_packet_callback(encoded,encodedPacket,&packetCapture);fclose(packetCapture.file);obs_output_release(encoded);obs_encoder_release(videoEncoder);obs_encoder_release(audioEncoder);obs_service_release(service);}
            audio_output_disconnect(obs_get_audio(),0,mixedAudio,&mixCapture);obs_remove_raw_video_callback(renderedVideo,&renderedCapture);
            auto saveTiming=[&](const std::string &suffix,Capture &capture){std::lock_guard guard(capture.mutex);FILE *file=nullptr;fopen_s(&file,(artifact+suffix).c_str(),"wb");require(file,"timing artifact");fprintf(file,"timestamp_ns,luma\n");for(const auto &f:capture.timing)fprintf(file,"%llu,%.3f\n",f.timestamp,f.luma);fclose(file);};
            saveTiming(".rendered.csv",renderedCapture);saveTiming(".source.csv",mainCapture);
            {std::lock_guard guard(mixCapture.mutex);FILE *file=nullptr;fopen_s(&file,(artifact+".mix.f32").c_str(),"wb");require(file,"sync PCM artifact");fwrite(mixCapture.pcm.data(),1,mixCapture.pcm.size(),file);fclose(file);fopen_s(&file,(artifact+".audio.csv").c_str(),"wb");require(file,"sync audio timing");fprintf(file,"timestamp_ns,first_frame,frames\n");for(const auto &b:mixCapture.blocks)fprintf(file,"%llu,%llu,%u\n",b.timestamp,b.firstFrame,b.frames);fclose(file);}
            obs_set_output_source(0,nullptr);obs_source_remove(obs_scene_get_source(scene));obs_scene_release(scene);if(second)obs_source_release(second);obs_source_dec_active(source);obs_source_dec_showing(source);obs_source_release(sub);obs_source_release(source);obs_data_release(settings);obs_wait_for_destroy_queue();obs_shutdown();return 0;
        }
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
