#pragma once
#include "common.h"
#include <ass/ass.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <gst/video/video.h>
#include <atomic>
namespace sd {
class Subtitles {
public:
    explicit Subtitles(const QJsonObject &config);
    ~Subtitles();
    void load();
    void setTimeline(double seconds);
    void renderCpu(GstBuffer *buffer, GstCaps *caps);
    QByteArray renderTransparent(int width,int height,GstClockTime timestamp);
    bool renderGpu(void *renderTarget, GstClockTime timestamp);
    QStringList warnings() const { return warnings_; }
    std::atomic<quint64> renderedFrames{0};
private:
    void update(int width,int height,GstClockTime timestamp);
    void initGpu(ID3D11Device *device);
    QJsonObject config_;QStringList warnings_;
    ASS_Library *library_=nullptr;ASS_Renderer *renderer_=nullptr;ASS_Track *track_=nullptr;
    QByteArray pixels_;int x_=0,y_=0,w_=0,h_=0;bool changed_=false;
    GstClockTime firstPts_=GST_CLOCK_TIME_NONE;double timeline_=0;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
};
int gpuAdapter(const QString &backend);
}
