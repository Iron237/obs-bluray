#include "bluray_audio.h"
#include <gst/audio/gstaudiodecoder.h>
#include <gst/audio/audio.h>
#include <cstring>

// Blu-ray LPCM is interleaved, big endian, with an even number of physical
// channels. DVD's 20/24-bit packing permutation must never be applied to it.
struct SdBlurayAudio { GstAudioDecoder parent; guint formatKey; GstAudioInfo info; };
struct SdBlurayAudioClass { GstAudioDecoderClass parent; };
G_DEFINE_TYPE(SdBlurayAudio,sd_bluray_audio,GST_TYPE_AUDIO_DECODER)
static GstStaticPadTemplate inputTemplate=GST_STATIC_PAD_TEMPLATE("sink",GST_PAD_SINK,GST_PAD_ALWAYS,GST_STATIC_CAPS("audio/x-private-ts-lpcm"));
static GstStaticPadTemplate outputTemplate=GST_STATIC_PAD_TEMPLATE("src",GST_PAD_SRC,GST_PAD_ALWAYS,GST_STATIC_CAPS("audio/x-raw,format=(string){S16BE,S24BE},layout=(string)interleaved,rate=(int){48000,96000,192000},channels=(int)[1,8]"));
static GstFlowReturn decodeAudio(GstAudioDecoder *base,GstBuffer *input){
    if(!input)return GST_FLOW_OK;
    auto *self=reinterpret_cast<SdBlurayAudio*>(base);GstMapInfo map{};if(!gst_buffer_map(input,&map,GST_MAP_READ))return GST_FLOW_ERROR;
    if(map.size<4){gst_buffer_unmap(input,&map);return gst_audio_decoder_finish_frame(base,nullptr,1);}
    const int assignment=map.data[2]>>4,rateCode=map.data[2]&15,depth=map.data[3]>>6;
    static constexpr int counts[]={0,1,0,2,3,3,4,4,5,6,7,8};
    const int channels=assignment<12?counts[assignment]:0;
    const int rate=rateCode==1?48000:rateCode==4?96000:rateCode==5?192000:0;
    const int bytes=depth==1?2:(depth==2||depth==3)?3:0;
    if(!channels||!rate||!bytes){gst_buffer_unmap(input,&map);GST_ELEMENT_ERROR(self,STREAM,DECODE,("Invalid Blu-ray LPCM header"),(nullptr));return GST_FLOW_ERROR;}
    using P=GstAudioChannelPosition;
    constexpr P FL=GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,FR=GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,FC=GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,RC=GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
        SL=GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,SR=GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,RL=GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,RR=GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,LFE=GST_AUDIO_CHANNEL_POSITION_LFE1;
    static const P positions[12][8]={{},{GST_AUDIO_CHANNEL_POSITION_MONO},{},{FL,FR},{FL,FR,FC},{FL,FR,RC},{FL,FR,FC,RC},{FL,FR,SL,SR},{FL,FR,FC,SL,SR},{FL,FR,FC,SL,SR,LFE},{FL,FR,FC,SL,SR,RL,RR},{FL,FR,FC,SL,SR,RL,RR,LFE}};
    P canonical[8];std::memcpy(canonical,positions[assignment],sizeof(canonical));gst_audio_channel_positions_to_valid_order(canonical,channels);
    const auto format=bytes==2?GST_AUDIO_FORMAT_S16BE:GST_AUDIO_FORMAT_S24BE;
    const guint key=(guint(map.data[2])<<8)|map.data[3];
    if(self->formatKey!=key){gst_audio_info_set_format(&self->info,format,rate,channels,canonical);if(!gst_audio_decoder_set_output_format(base,&self->info)){gst_buffer_unmap(input,&map);return GST_FLOW_NOT_NEGOTIATED;}self->formatKey=key;}
    const int physical=(channels+1)&~1;const gsize samples=(map.size-4)/(physical*bytes);
    auto *output=gst_buffer_new_allocate(nullptr,samples*channels*bytes,nullptr);GstMapInfo dest{};gst_buffer_map(output,&dest,GST_MAP_WRITE);
    for(gsize frame=0;frame<samples;++frame)std::memcpy(dest.data+frame*channels*bytes,map.data+4+frame*physical*bytes,channels*bytes);
    gst_buffer_unmap(output,&dest);gst_buffer_unmap(input,&map);
    if(channels>1)gst_audio_buffer_reorder_channels(output,format,channels,positions[assignment],canonical);
    return gst_audio_decoder_finish_frame(base,output,1);
}
static void sd_bluray_audio_class_init(SdBlurayAudioClass *klass){
    auto *element=GST_ELEMENT_CLASS(klass);gst_element_class_set_static_metadata(element,"StreamDesk Blu-ray LPCM decoder","Codec/Decoder/Audio","Blu-ray interleaved LPCM without DVD byte permutation","StreamDesk");
    gst_element_class_add_static_pad_template(element,&inputTemplate);gst_element_class_add_static_pad_template(element,&outputTemplate);
    GST_AUDIO_DECODER_CLASS(klass)->handle_frame=decodeAudio;
}
static void sd_bluray_audio_init(SdBlurayAudio *self){self->formatKey=0;gst_audio_info_init(&self->info);gst_audio_decoder_set_needs_format(GST_AUDIO_DECODER(self),FALSE);}
namespace sd {void registerBlurayAudioDecoder(){gst_element_register(nullptr,"sdbluraylpcmdec",GST_RANK_PRIMARY+2000,sd_bluray_audio_get_type());}}
