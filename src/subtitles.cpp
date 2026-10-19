#include "subtitles.h"
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdexcept>
#include <cstdarg>
using Microsoft::WRL::ComPtr;
namespace sd {
static void check(HRESULT hr) { if(FAILED(hr))throw std::runtime_error("D3D11 subtitle compositor initialization failed"); }
int gpuAdapter(const QString &backend) {
    ComPtr<IDXGIFactory1> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return -1;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);
        if((backend=="intel"&&d.VendorId==0x8086)||(backend=="nvidia"&&d.VendorId==0x10de)||(backend=="amd"&&d.VendorId==0x1002))return int(i);}
    return -1;
}
Subtitles::Subtitles(const QJsonObject &c):config_(c),timeline_(c.value("start").toDouble()){}
Subtitles::~Subtitles(){if(track_)ass_free_track(track_);if(renderer_)ass_renderer_done(renderer_);if(library_)ass_library_done(library_);}
void Subtitles::load() {
    const QString path=config_["subtitle"].toString();if(path.isEmpty())return;
    library_=ass_library_init();if(!library_)throw std::runtime_error("libass initialization failed");
    ass_set_message_cb(library_,[](int level,const char *format,va_list args,void *data){
        if(level>2)return;char buf[1024]{};vsnprintf(buf,sizeof(buf),format,args);auto *self=static_cast<Subtitles*>(data);
        const auto msg=QString::fromUtf8(buf);if(self->warnings_.size()<15&&!self->warnings_.contains(msg))self->warnings_<<msg;
    },this);
    const QString fonts=config_["fontsDir"].toString();
    if(!fonts.isEmpty()) {QDir dir(fonts);for(const auto &f:dir.entryList({"*.ttf","*.otf","*.ttc"},QDir::Files)){QFile file(dir.absoluteFilePath(f));if(file.open(QIODevice::ReadOnly)){auto data=file.readAll();ass_add_font(library_,f.toUtf8().data(),data.data(),int(data.size()));}}}
    renderer_=ass_renderer_init(library_);if(!renderer_)throw std::runtime_error("libass renderer initialization failed");
    ass_set_fonts(renderer_,nullptr,"Microsoft YaHei",ASS_FONTPROVIDER_AUTODETECT,nullptr,1);
    QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read subtitle file");auto bytes=file.readAll();
    if(QFileInfo(path).suffix().compare("srt",Qt::CaseInsensitive)==0) {
        QString ass="[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Microsoft YaHei,48,&H00FFFFFF,&H00FFFFFF,&H00101010,&H80000000,0,0,0,0,100,100,0,0,1,2,1,2,60,60,50,1\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        QString text=QString::fromUtf8(bytes);text.replace("\r","");
        QRegularExpression re(R"((\d\d):(\d\d):(\d\d)[,.](\d{3})\s*-->\s*(\d\d):(\d\d):(\d\d)[,.](\d{3})[^\n]*\n([\s\S]*?)(?=\n\s*\n|$))");
        auto matches=re.globalMatch(text);while(matches.hasNext()){auto m=matches.next();auto stamp=[&](int n){return QString("%1:%2:%3.%4").arg(m.captured(n).toInt()).arg(m.captured(n+1)).arg(m.captured(n+2)).arg(m.captured(n+3).left(2));};
            QString line=m.captured(9).trimmed();line.replace("\\","\\\\");line.replace("{","\\{");line.replace("}","\\}");line.replace('\n',"\\N");line.remove(QRegularExpression("<[^>]*>"));
            ass+="Dialogue: 0,"+stamp(1)+","+stamp(5)+",Default,,0,0,0,,"+line+"\n";}
        bytes=ass.toUtf8();
    }
    track_=ass_read_memory(library_,bytes.data(),bytes.size(),nullptr);if(!track_)throw std::runtime_error("Invalid ASS/SSA/SRT subtitles");
    if(!track_->n_events)warnings_<<"字幕没有可渲染事件";
}
void Subtitles::setTimeline(double seconds){timeline_=seconds;firstPts_=GST_CLOCK_TIME_NONE;}
void Subtitles::update(int width,int height,GstClockTime pts) {
    changed_=false;if(!track_||!GST_CLOCK_TIME_IS_VALID(pts))return;
    if(!GST_CLOCK_TIME_IS_VALID(firstPts_))firstPts_=pts;
    const double time=timeline_+(double(pts)-double(firstPts_))/GST_SECOND-config_.value("subtitleOffset").toDouble();
    ass_set_frame_size(renderer_,width,height);int change=0;ASS_Image *images=ass_render_frame(renderer_,track_,qint64(time*1000),&change);
    if(!change)return;changed_=true;
    int left=width,top=height,right=0,bottom=0;
    for(auto *i=images;i;i=i->next){if(i->w<=0||i->h<=0)continue;left=qMin(left,i->dst_x);top=qMin(top,i->dst_y);right=qMax(right,i->dst_x+i->w);bottom=qMax(bottom,i->dst_y+i->h);}
    x_=qBound(0,left,width);y_=qBound(0,top,height);w_=qMax(0,qMin(right,width)-x_);h_=qMax(0,qMin(bottom,height)-y_);
    pixels_=QByteArray(w_*h_*4,0);if(!w_||!h_)return;
    for(auto *i=images;i;i=i->next){
        const int r=(i->color>>24)&255,g=(i->color>>16)&255,b=(i->color>>8)&255,alpha=255-(i->color&255);
        for(int iy=0;iy<i->h;++iy){const int y=i->dst_y+iy-y_;if(y<0||y>=h_)continue;
            for(int ix=0;ix<i->w;++ix){const int x=i->dst_x+ix-x_;if(x<0||x>=w_)continue;
                const int a=i->bitmap[iy*i->stride+ix]*alpha/255;if(!a)continue;
                auto *p=reinterpret_cast<unsigned char*>(pixels_.data())+4*(y*w_+x);
                p[0]=static_cast<unsigned char>(b*a/255+p[0]*(255-a)/255);p[1]=static_cast<unsigned char>(g*a/255+p[1]*(255-a)/255);
                p[2]=static_cast<unsigned char>(r*a/255+p[2]*(255-a)/255);p[3]=static_cast<unsigned char>(a+p[3]*(255-a)/255);
            }
        }
    }
}
QByteArray Subtitles::renderTransparent(int width,int height,GstClockTime timestamp) {
    update(width,height,timestamp);QByteArray result(width*height*4,0);
    for(int y=0;y<h_;++y)for(int x=0;x<w_;++x){
        const auto *s=reinterpret_cast<const unsigned char*>(pixels_.constData())+4*(y*w_+x);
        auto *d=reinterpret_cast<unsigned char*>(result.data())+4*((y+y_)*width+x+x_);
        d[3]=s[3];for(int k=0;k<3;++k)d[k]=s[3]?static_cast<unsigned char>(qMin(255,int(s[k])*255/int(s[3]))):0;
    }
    if(w_&&h_)++renderedFrames;return result;
}
void Subtitles::renderCpu(GstBuffer *buffer,GstCaps *caps) {
    GstVideoInfo info;if(!gst_video_info_from_caps(&info,caps))return;
    update(GST_VIDEO_INFO_WIDTH(&info),GST_VIDEO_INFO_HEIGHT(&info),GST_BUFFER_PTS(buffer));if(!w_||!h_)return;
    GstVideoFrame frame;if(!gst_video_frame_map(&frame,&info,buffer,GST_MAP_READWRITE))return;
    auto *base=static_cast<unsigned char*>(GST_VIDEO_FRAME_PLANE_DATA(&frame,0));const int stride=GST_VIDEO_FRAME_PLANE_STRIDE(&frame,0);
    for(int y=0;y<h_;++y)for(int x=0;x<w_;++x){const auto *s=reinterpret_cast<const unsigned char*>(pixels_.constData())+4*(y*w_+x);auto *d=base+(y+y_)*stride+(x+x_)*4;
        for(int k=0;k<3;++k)d[k]=static_cast<unsigned char>(s[k]+d[k]*(255-s[3])/255);}
    gst_video_frame_unmap(&frame);++renderedFrames;
}
void Subtitles::initGpu(ID3D11Device *device) {
    if(device_.Get()==device)return;device_=device;srv_.Reset();texture_.Reset();
    const char *shader=R"(
cbuffer Rect : register(b0) { float4 rect; };
struct V { float4 p : SV_Position; float2 uv : TEXCOORD; };
V vs(uint id : SV_VertexID) { float2 uv=float2(id&1,id>>1); V o; o.uv=uv; o.p=float4(lerp(rect.xy,rect.zw,uv),0,1); return o; }
Texture2D image : register(t0); SamplerState sampleState : register(s0);
float4 ps(V i) : SV_Target { return image.Sample(sampleState,i.uv); }
)";
    ComPtr<ID3DBlob> code,error;check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_4_0",0,0,&code,&error));
    check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs_));code.Reset();
    check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_4_0",0,0,&code,&error));check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps_));
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&sampler,&sampler_));
    D3D11_BLEND_DESC blend{};auto &b=blend.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=D3D11_BLEND_ONE;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    check(device->CreateBlendState(&blend,&blend_));
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;check(device->CreateRasterizerState(&raster,&raster_));
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check(device->CreateBuffer(&cb,nullptr,&constants_));changed_=true;
}
bool Subtitles::renderGpu(void *target,GstClockTime pts) {
    auto *rtv=static_cast<ID3D11RenderTargetView*>(target);ComPtr<ID3D11Resource> resource;rtv->GetResource(&resource);ComPtr<ID3D11Texture2D> video;check(resource.As(&video));
    D3D11_TEXTURE2D_DESC desc{};video->GetDesc(&desc);update(int(desc.Width),int(desc.Height),pts);
    if(!w_||!h_)return false;
    ComPtr<ID3D11Device> device;rtv->GetDevice(&device);initGpu(device.Get());
    if(changed_||!srv_){
        texture_.Reset();srv_.Reset();D3D11_TEXTURE2D_DESC td{};td.Width=UINT(w_);td.Height=UINT(h_);td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels_.constData(),UINT(w_*4),0};check(device->CreateTexture2D(&td,&data,&texture_));check(device->CreateShaderResourceView(texture_.Get(),nullptr,&srv_));
    }
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    // GStreamer reuses this context and caches its own state. Preserve every slot
    // touched by this compositor, otherwise the next conversion can render black.
    ComPtr<ID3D11RenderTargetView> oldTarget;ComPtr<ID3D11DepthStencilView> oldDepthView;
    ComPtr<ID3D11BlendState> oldBlend;FLOAT oldFactor[4]{};UINT oldMask=0;
    ComPtr<ID3D11DepthStencilState> oldDepth;UINT oldStencil=0;
    ComPtr<ID3D11RasterizerState> oldRaster;D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};UINT oldViewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ComPtr<ID3D11InputLayout> oldLayout;D3D11_PRIMITIVE_TOPOLOGY oldTopology{};
    ComPtr<ID3D11VertexShader> oldVs;ComPtr<ID3D11PixelShader> oldPs;ComPtr<ID3D11GeometryShader> oldGs;
    ComPtr<ID3D11Buffer> oldConstants;ComPtr<ID3D11ShaderResourceView> oldSrv;ComPtr<ID3D11SamplerState> oldSampler;
    context->OMGetRenderTargets(1,&oldTarget,&oldDepthView);context->OMGetBlendState(&oldBlend,oldFactor,&oldMask);context->OMGetDepthStencilState(&oldDepth,&oldStencil);
    context->RSGetState(&oldRaster);context->RSGetViewports(&oldViewportCount,oldViewports);context->IAGetInputLayout(&oldLayout);context->IAGetPrimitiveTopology(&oldTopology);
    context->VSGetShader(&oldVs,nullptr,nullptr);context->PSGetShader(&oldPs,nullptr,nullptr);context->GSGetShader(&oldGs,nullptr,nullptr);
    context->VSGetConstantBuffers(0,1,&oldConstants);context->PSGetShaderResources(0,1,&oldSrv);context->PSGetSamplers(0,1,&oldSampler);
    float rect[]={float(2.0*x_/desc.Width-1),float(1-2.0*y_/desc.Height),float(2.0*(x_+w_)/desc.Width-1),float(1-2.0*(y_+h_)/desc.Height)};
    context->UpdateSubresource(constants_.Get(),0,nullptr,rect,0,0);
    D3D11_VIEWPORT vp{0,0,float(desc.Width),float(desc.Height),0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster_.Get());
    context->OMSetRenderTargets(1,&rtv,nullptr);context->OMSetBlendState(blend_.Get(),nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(vs_.Get(),nullptr,0);context->VSSetConstantBuffers(0,1,constants_.GetAddressOf());context->GSSetShader(nullptr,nullptr,0);
    context->PSSetShader(ps_.Get(),nullptr,0);context->PSSetShaderResources(0,1,srv_.GetAddressOf());context->PSSetSamplers(0,1,sampler_.GetAddressOf());context->Draw(4,0);
    context->PSSetShaderResources(0,1,oldSrv.GetAddressOf());context->PSSetSamplers(0,1,oldSampler.GetAddressOf());
    context->VSSetConstantBuffers(0,1,oldConstants.GetAddressOf());context->VSSetShader(oldVs.Get(),nullptr,0);context->PSSetShader(oldPs.Get(),nullptr,0);context->GSSetShader(oldGs.Get(),nullptr,0);
    context->IASetInputLayout(oldLayout.Get());context->IASetPrimitiveTopology(oldTopology);context->RSSetState(oldRaster.Get());context->RSSetViewports(oldViewportCount,oldViewports);
    context->OMSetRenderTargets(1,oldTarget.GetAddressOf(),oldDepthView.Get());context->OMSetBlendState(oldBlend.Get(),oldFactor,oldMask);context->OMSetDepthStencilState(oldDepth.Get(),oldStencil);
    ++renderedFrames;return true;
}
}
