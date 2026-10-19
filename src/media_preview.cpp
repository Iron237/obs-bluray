#include "media_preview.h"
#include <cstring>
namespace sd {
static constexpr int mailboxSize=1024*1024;
struct FrameHeader {quint64 sequence;quint32 length;quint32 magic;};
FrameMailbox::FrameMailbox(QString key,bool writer):memory_(key),writer_(writer){
    if(writer_){if(!memory_.create(mailboxSize))memory_.attach();if(memory_.isAttached()&&memory_.lock()){std::memset(memory_.data(),0,sizeof(FrameHeader));memory_.unlock();}}
}
void FrameMailbox::write(const QByteArray &jpeg){
    QMutexLocker guard(&mutex_);if(!writer_||!memory_.isAttached()||jpeg.isEmpty()||jpeg.size()>mailboxSize-int(sizeof(FrameHeader)))return;
    if(!memory_.lock())return;auto *h=static_cast<FrameHeader*>(memory_.data());std::memcpy(h+1,jpeg.constData(),size_t(jpeg.size()));h->length=quint32(jpeg.size());h->magic=0x53444652;++h->sequence;memory_.unlock();
}
QByteArray FrameMailbox::read(quint64 &sequence){
    QMutexLocker guard(&mutex_);if(!memory_.isAttached()&&!memory_.attach(QSharedMemory::ReadOnly))return {};
    if(!memory_.lock())return {};const auto*h=static_cast<const FrameHeader*>(memory_.constData());QByteArray result;
    if(h->magic==0x53444652&&h->sequence!=sequence&&h->length>0&&h->length<=quint32(memory_.size()-sizeof(FrameHeader))){result=QByteArray(reinterpret_cast<const char*>(h+1),int(h->length));sequence=h->sequence;}memory_.unlock();return result;
}
DeckBridge::~DeckBridge(){if(video)gst_sample_unref(video);}
void DeckBridge::setVideo(GstSample *sample){if(videoOutput){videoOutput(sample);return;}QMutexLocker guard(&mutex);if(video)gst_sample_unref(video);video=gst_sample_ref(sample);}
GstSample *DeckBridge::getVideo(){QMutexLocker guard(&mutex);return video?gst_sample_ref(video):nullptr;}
void DeckBridge::addAudio(GstSample *sample){if(audioOutput){audioOutput(sample);return;}GstMapInfo map{};auto *buffer=gst_sample_get_buffer(sample);if(!gst_buffer_map(buffer,&map,GST_MAP_READ))return;QMutexLocker guard(&mutex);audio.append(reinterpret_cast<const char*>(map.data),qsizetype(map.size));const int limit=48000*8/3;if(audio.size()>limit)audio.remove(0,(audio.size()-limit+7)/8*8);gst_buffer_unmap(buffer,&map);}
QByteArray DeckBridge::takeAudio(int count){QMutexLocker guard(&mutex);QByteArray result(count*8,char(0));const auto n=qMin(result.size(),audio.size());if(n)std::memcpy(result.data(),audio.constData(),size_t(n));audio.remove(0,n);return result;}
void DeckBridge::clear(){QMutexLocker guard(&mutex);if(video)gst_sample_unref(video);video=nullptr;audio.clear();status={};}
void attachJpegMailbox(GstElement *pipeline,const char *name,FrameMailbox *mailbox){
    auto *sink=gst_bin_get_by_name(GST_BIN(pipeline),name);if(!sink)return;
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample=+[](GstAppSink*s,gpointer p){auto *sample=gst_app_sink_pull_sample(s);if(!sample)return GST_FLOW_EOS;GstMapInfo map{};auto *b=gst_sample_get_buffer(sample);if(gst_buffer_map(b,&map,GST_MAP_READ)){static_cast<FrameMailbox*>(p)->write(QByteArray(reinterpret_cast<const char*>(map.data),qsizetype(map.size)));gst_buffer_unmap(b,&map);}gst_sample_unref(sample);return GST_FLOW_OK;};
    callbacks.new_preroll=+[](GstAppSink*s,gpointer p){auto *sample=gst_app_sink_pull_preroll(s);if(!sample)return GST_FLOW_OK;GstMapInfo map{};auto*b=gst_sample_get_buffer(sample);if(gst_buffer_map(b,&map,GST_MAP_READ)){static_cast<FrameMailbox*>(p)->write(QByteArray(reinterpret_cast<const char*>(map.data),qsizetype(map.size)));gst_buffer_unmap(b,&map);}gst_sample_unref(sample);return GST_FLOW_OK;};
    gst_app_sink_set_callbacks(GST_APP_SINK(sink),&callbacks,mailbox,nullptr);gst_object_unref(sink);
}
}
