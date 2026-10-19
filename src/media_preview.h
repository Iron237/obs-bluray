#pragma once
#include "common.h"
#include <gst/app/gstappsink.h>
#include <memory>
#include <functional>

namespace sd {
// Latest-frame mailbox only: full-resolution media never crosses GUI IPC.
class FrameMailbox {
public:
    explicit FrameMailbox(QString key,bool writer=false);
    void write(const QByteArray &jpeg);
    QByteArray read(quint64 &sequence);
private:
    QSharedMemory memory_;QMutex mutex_;bool writer_;
};
struct DeckBridge {
    ~DeckBridge();
    QMutex mutex;
    GstSample *video=nullptr;
    QByteArray audio;
    QJsonObject status;
    std::shared_ptr<FrameMailbox> frames;
    // Optional raw delivery to a native media host. Callbacks stop before teardown.
    std::function<void(GstSample *)> videoOutput, audioOutput, videoPrerollOutput;
    void setVideo(GstSample *sample);
    GstSample *getVideo();
    void addAudio(GstSample *sample);
    QByteArray takeAudio(int frames);
    void clear();
};
void attachJpegMailbox(GstElement *pipeline,const char *name,FrameMailbox *mailbox);
}
