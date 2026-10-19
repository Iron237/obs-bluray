#pragma once
#include "catalog.h"
#include "subtitles.h"
#include "media_preview.h"
#include <gst/app/gstappsrc.h>
#include <memory>
#include <atomic>

namespace sd {
class MediaWorker : public QObject {
    Q_OBJECT
public:
    MediaWorker(QString id, QJsonObject config, QObject *parent=nullptr);
    ~MediaWorker();
    void start();
    void stop();
    QJsonObject command(const QJsonObject &request);
    QJsonObject status() const;
    void setDeckBridge(std::shared_ptr<DeckBridge> bridge,GstContext *context){deck_=std::move(bridge);context_=context;}
    QString encoderSpecification() const {return encoderSpec();}
    QString outputSpecification(const QString &output,const QString &mux) const{return outputSpec(output,mux);}
signals:
    void sourcePrepared(double timeline);
    void finished(bool ok);
    void statusChanged(QJsonObject status);
private:
    struct Limit { MediaWorker *owner; GstClockTime first=GST_CLOCK_TIME_NONE; std::atomic<bool> eos{false}; } videoLimit_{this},audioLimit_{this};
    static void onPad(GstElement *,GstPad *,gpointer);
    static void onNeedData(GstAppSrc *,guint,gpointer);
    static GstPadProbeReturn onLimit(GstPad *,GstPadProbeInfo *,gpointer);
    static GstPadProbeReturn onEncoded(GstPad *,GstPadProbeInfo *,gpointer);
    static GstPadProbeReturn onCpuSubtitle(GstPad *,GstPadProbeInfo *,gpointer);
    void poll();
    void sampleOutput();
    void complete(bool ok,const QString &error={});
    void build();
    void selectDecoders();
    QString encoderSpec() const;
    QString outputSpec(const QString &output,const QString &muxName) const;
    void attachProbe(const char *element,GstPadProbeCallback callback,gpointer data);
    void publishStatus();
    QString id_,state_="starting",error_,pipelineText_,diagnostic_;
    QJsonObject config_;
    int currentBitrate_=6000;
    GstElement *pipeline_=nullptr,*encoder_=nullptr,*outputSink_=nullptr;
    GstBus *bus_=nullptr;BLURAY *disc_=nullptr;
    QVector<qint64> clipOffsets_;int currentClip_=0;
    QByteArray codecHeaders_;
    std::unique_ptr<Subtitles> subtitles_;
    std::shared_ptr<DeckBridge> deck_;
    GstContext *context_=nullptr; // Owned by the studio worker, outlives deck threads.
    QTimer timer_;QElapsedTimer elapsed_;QLocalServer server_;
    std::atomic<quint64> frames_{0},bytes_{0};
    std::atomic<qint64> firstEncoded_{-1},lastEncoded_{0};
    std::atomic<qint64> firstOutput_{-1},lastOutput_{-1};
    std::atomic<int> audioSeen_{0},subtitleSeen_{0};
    QMutex trackMutex_;bool videoLinked_=false,audioLinked_=false,subtitleLinked_=false,hasAudio_=true;
    std::atomic<bool> stopped_{false};
    bool finished_=false,discSource_=false,gpu_=false,stopPending_=false,durationEos_=false;
    double timeline_=0,totalDuration_=0,sentPosition_=0;QStringList decoderNames_;mutable QMutex decoderMutex_;
    quint64 lastBytes_=0,lastFrames_=0;double bitrateMeasured_=0,fps_=0;int lastSampleMs_=0;
};
}
