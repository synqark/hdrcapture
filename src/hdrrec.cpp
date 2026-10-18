// hdrrec - minimal HDR10 desktop recorder prototype
//   Desktop Duplication (scRGB FP16, capture D3D11 device) -> shared texture ->
//   D3D11 shader on the encode device (BT.709 linear -> BT.2020 PQ, P010)
//   -> hevc_nvenc / av1_nvenc (D3D11 frames, HDR10 metadata) -> mkv
//
// Usage: hdrrec.exe [options]
//   -o FILE        output file (default: rec_YYYYmmdd_HHMMSS.mkv)
//   -t SEC         record duration in seconds (default 0 = until Enter / Ctrl+C)
//   -monitor N     DXGI output index (default 0)
//   -fps N         frame rate (default 60)
//   -codec NAME    hevc | av1 (default hevc)
//   -cq N          constant quality, VBR with a target quality (default 18; hevc 1..51, av1 1..63)
//   -bitrate MBPS  average bitrate in Mbit/s instead of -cq (VBR, max rate 1.5x unless -maxrate)
//   -preset P      p1..p7 (default p4)
//   -maxrate MBPS  VBV max rate in Mbit/s (default 0 = unlimited)
//   -maxcll N      MaxCLL nits (default 1000)
//   -maxfall N     MaxFALL nits (default 400)
//   -mdl N         mastering display max luminance nits (default 1000)
//   -audio 0|1     record system audio (WASAPI loopback of the default output, default 1)
//   -acodec NAME   aac | pcm (16-bit) (default aac)
//   -abitrate N    AAC bitrate in kbit/s (default 256)
//   -again DB      audio gain in dB (default 0; the result is clipped at full scale)
//   -gpuprio N     GPU priority of the recorder: 0 = unchanged (default), 1 = above normal,
//                  2 = high, 3 = realtime (needs administrator rights)
//   -audio-process NAME|PID  record only this application's audio (and its child processes), e.g.
//                  AceCombat8.exe; falls back to system audio (with a warning) if it is not running
//   -timelog FILE  write per-capture present times / latency / missed frames as CSV
//   -list-monitors print the DXGI outputs (index for -monitor) and exit
//   (the mouse cursor is not recorded; file names may contain any Unicode characters)

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
}

static std::atomic<bool> g_stop{false};

static BOOL WINAPI ctrl_handler(DWORD) { g_stop = true; return TRUE; }

static std::string to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring to_wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static std::string averr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

#define CHECK_AV(expr, msg)                                                   \
    do { int _r = (expr); if (_r < 0) {                                       \
        fprintf(stderr, "ERROR: %s: %s\n", msg, averr(_r).c_str());           \
        return 1; } } while (0)

#define CHECK_HR(expr, msg)                                                   \
    do { HRESULT _hr = (expr); if (FAILED(_hr)) {                             \
        fprintf(stderr, "ERROR: %s (hr=0x%08lX)\n", msg, (unsigned long)_hr); \
        return 1; } } while (0)

// ---------------------------------------------------------------------------
// Shader: scRGB (linear BT.709, 1.0 = 80 nits) -> PQ BT.2020 -> Y'CbCr limited
// range, 10-bit stored in the upper bits of 16-bit P010 planes.
// ---------------------------------------------------------------------------
static const char *kShader = R"HLSL(
Texture2D<float4> src : register(t0);

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

static const float3x3 M709to2020 = {
    0.627404, 0.329283, 0.043313,
    0.069097, 0.919540, 0.011362,
    0.016391, 0.088013, 0.895595 };

float3 pq_oetf(float3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float3 to_pq2020(float3 scrgb) {
    // matrix first (keeps out-of-709 colours encoded as negative scRGB), then clip
    return pq_oetf(max(mul(M709to2020, scrgb * 80.0), 0.0));
}

static const float3 KY = float3(0.2627, 0.6780, 0.0593);

float ps_y(float4 p : SV_Position) : SV_Target {
    float3 e = to_pq2020(src.Load(int3(p.xy, 0)).rgb);
    float  y = dot(e, KY);
    return (64.0 + 876.0 * y) * 64.0 / 65535.0;
}

float2 ps_uv(float4 p : SV_Position) : SV_Target {
    int2 b = int2(p.xy) * 2;
    float3 e = to_pq2020(src.Load(int3(b,              0)).rgb)
             + to_pq2020(src.Load(int3(b + int2(1, 0), 0)).rgb)
             + to_pq2020(src.Load(int3(b + int2(0, 1), 0)).rgb)
             + to_pq2020(src.Load(int3(b + int2(1, 1), 0)).rgb);
    e *= 0.25;
    float y  = dot(e, KY);
    float cb = (e.b - y) / 1.8814;
    float cr = (e.r - y) / 1.4746;
    return (512.0 + 896.0 * float2(cb, cr)) * 64.0 / 65535.0;
}
)HLSL";

struct Converter {
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *psY = nullptr, *psUV = nullptr;
    ID3D11Texture2D *stage = nullptr;       // FP16 copy of the captured frame
    ID3D11ShaderResourceView *srv = nullptr;
    std::map<std::pair<ID3D11Texture2D *, UINT>, std::pair<ID3D11RenderTargetView *, ID3D11RenderTargetView *>> rtvs;
    int w = 0, h = 0;

    int compile(const char *entry, const char *target, ID3DBlob **out) {
        ID3DBlob *err = nullptr;
        HRESULT hr = D3DCompile(kShader, strlen(kShader), "hdrrec", nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
        if (FAILED(hr)) {
            fprintf(stderr, "ERROR: shader %s: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
            if (err) err->Release();
            return 1;
        }
        return 0;
    }

    int init(ID3D11Device *d, ID3D11DeviceContext *c, int width, int height) {
        dev = d; ctx = c; w = width; h = height;
        ID3DBlob *b = nullptr;
        if (compile("vs_main", "vs_5_0", &b)) return 1;
        CHECK_HR(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs), "CreateVertexShader");
        b->Release();
        if (compile("ps_y", "ps_5_0", &b)) return 1;
        CHECK_HR(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psY), "CreatePixelShader Y");
        b->Release();
        if (compile("ps_uv", "ps_5_0", &b)) return 1;
        CHECK_HR(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psUV), "CreatePixelShader UV");
        b->Release();

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CHECK_HR(dev->CreateTexture2D(&td, nullptr, &stage), "CreateTexture2D stage");
        CHECK_HR(dev->CreateShaderResourceView(stage, nullptr, &srv), "CreateShaderResourceView");
        return 0;
    }

    void release() {
        for (auto &kv : rtvs) { kv.second.first->Release(); kv.second.second->Release(); }
        rtvs.clear();
        if (srv) srv->Release();
        if (stage) stage->Release();
        if (psUV) psUV->Release();
        if (psY) psY->Release();
        if (vs) vs->Release();
        srv = nullptr; stage = nullptr; psUV = psY = nullptr; vs = nullptr;
    }

    int get_rtvs(ID3D11Texture2D *tex, UINT slice, ID3D11RenderTargetView **y, ID3D11RenderTargetView **uv) {
        auto key = std::make_pair(tex, slice);
        auto it = rtvs.find(key);
        if (it != rtvs.end()) { *y = it->second.first; *uv = it->second.second; return 0; }
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.MipSlice = 0;
        rd.Texture2DArray.FirstArraySlice = slice;
        rd.Texture2DArray.ArraySize = 1;
        rd.Format = DXGI_FORMAT_R16_UNORM;
        CHECK_HR(dev->CreateRenderTargetView(tex, &rd, y), "CreateRenderTargetView (P010 luma)");
        rd.Format = DXGI_FORMAT_R16G16_UNORM;
        CHECK_HR(dev->CreateRenderTargetView(tex, &rd, uv), "CreateRenderTargetView (P010 chroma)");
        rtvs[key] = {*y, *uv};
        return 0;
    }

    int convert(ID3D11Texture2D *src, UINT srcSlice, ID3D11Texture2D *dst, UINT dstSlice) {
        ID3D11RenderTargetView *rtvY, *rtvUV;
        if (get_rtvs(dst, dstSlice, &rtvY, &rtvUV)) return 1;

        ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, src, srcSlice, nullptr);
        return draw(srv, dst, dstSlice);
    }

    // FP16 scRGB shader resource -> P010 render target (one array slice of dst)
    int draw(ID3D11ShaderResourceView *s, ID3D11Texture2D *dst, UINT dstSlice) {
        ID3D11RenderTargetView *rtvY, *rtvUV;
        if (get_rtvs(dst, dstSlice, &rtvY, &rtvUV)) return 1;

        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->RSSetState(nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->PSSetShaderResources(0, 1, &s);

        D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        ctx->OMSetRenderTargets(1, &rtvY, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShader(psY, nullptr, 0);
        ctx->Draw(3, 0);

        vp.Width = (float)(w / 2); vp.Height = (float)(h / 2);
        ctx->OMSetRenderTargets(1, &rtvUV, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShader(psUV, nullptr, 0);
        ctx->Draw(3, 0);

        ID3D11RenderTargetView *nullrtv = nullptr;
        ID3D11ShaderResourceView *nullsrv = nullptr;
        ctx->OMSetRenderTargets(1, &nullrtv, nullptr);
        ctx->PSSetShaderResources(0, 1, &nullsrv);
        return 0;
    }
};

static AVRational chroma(double v) { return av_make_q((int)(v * 50000 + 0.5), 50000); }


// ---------------------------------------------------------------------------
// Desktop Duplication capture (FP16 / scRGB) with present-time and missed-frame info
// ---------------------------------------------------------------------------
struct DupCapture {
    IDXGIOutput5 *output = nullptr;
    IDXGIOutputDuplication *dupl = nullptr;
    ID3D11Device *dev = nullptr;
    bool holding = false;
    int w = 0, h = 0;

    static void set_dpi_awareness() {
        typedef DPI_AWARENESS_CONTEXT(WINAPI * fn_t)(DPI_AWARENESS_CONTEXT);
        HMODULE u = LoadLibraryW(L"user32.dll");
        fn_t f = u ? (fn_t)(void *)GetProcAddress(u, "SetThreadDpiAwarenessContext") : nullptr;
        if (f) f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    // Find the N-th output across all adapters. Returns the adapter index, -1 on failure.
    static int find_output(int monitor, IDXGIOutput **out, DXGI_OUTPUT_DESC *desc) {
        IDXGIFactory1 *fac = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) return -1;
        int n = 0, found = -1;
        IDXGIAdapter1 *ad = nullptr;
        for (UINT a = 0; found < 0 && fac->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; a++) {
            IDXGIOutput *o = nullptr;
            for (UINT i = 0; ad->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; i++) {
                if (n++ == monitor) { *out = o; o->GetDesc(desc); found = (int)a; break; }
                o->Release();
            }
            ad->Release();
        }
        fac->Release();
        return found;
    }

    // Print all outputs in find_output() order (UTF-8):
    //   monitor <N>: <device> <W>x<H> HDR on|off [primary] "<adapter>"
    static void list_outputs() {
        set_dpi_awareness();                 // physical pixels in DesktopCoordinates
        IDXGIFactory1 *fac = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) return;
        int n = 0;
        IDXGIAdapter1 *ad = nullptr;
        for (UINT a = 0; fac->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; a++) {
            DXGI_ADAPTER_DESC1 adesc = {};
            ad->GetDesc1(&adesc);
            IDXGIOutput *o = nullptr;
            for (UINT i = 0; ad->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; i++, n++) {
                DXGI_OUTPUT_DESC d = {};
                o->GetDesc(&d);
                bool hdr = false;
                IDXGIOutput6 *o6 = nullptr;
                if (SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void **)&o6))) {
                    DXGI_OUTPUT_DESC1 d1 = {};
                    if (SUCCEEDED(o6->GetDesc1(&d1))) hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    o6->Release();
                }
                MONITORINFO mi = {sizeof(mi)};
                bool primary = GetMonitorInfoW(d.Monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY);
                printf("monitor %d: %s %ldx%ld HDR %s%s \"%s\"\n", n, to_utf8(d.DeviceName).c_str(),
                       d.DesktopCoordinates.right - d.DesktopCoordinates.left,
                       d.DesktopCoordinates.bottom - d.DesktopCoordinates.top,
                       hdr ? "on" : "off", primary ? " primary" : "", to_utf8(adesc.Description).c_str());
                o->Release();
            }
            ad->Release();
        }
        fac->Release();
    }

    int open(IDXGIOutput *o, ID3D11Device *d) {
        dev = d;
        if (!output) CHECK_HR(o->QueryInterface(__uuidof(IDXGIOutput5), (void **)&output), "IDXGIOutput5 (Windows 10 1703+ required)");
        set_dpi_awareness();
        DXGI_FORMAT fmts[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
        CHECK_HR(output->DuplicateOutput1(dev, 0, 2, fmts, &dupl), "DuplicateOutput1");
        DXGI_OUTDUPL_DESC dd;
        dupl->GetDesc(&dd);
        w = (int)dd.ModeDesc.Width; h = (int)dd.ModeDesc.Height;
        if (dd.ModeDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
            fprintf(stderr, "warning: desktop mode format is %d, not FP16. Is Windows HDR enabled?\n", (int)dd.ModeDesc.Format);
        return 0;
    }

    void release_frame() { if (holding) { dupl->ReleaseFrame(); holding = false; } }

    // 1 = got a frame with a present, 0 = timeout / pointer-only update, -1 = access lost, -2 = error
    int acquire(UINT timeout_ms, ID3D11Texture2D **tex, DXGI_OUTDUPL_FRAME_INFO *info) {
        release_frame();
        IDXGIResource *res = nullptr;
        HRESULT hr = dupl->AcquireNextFrame(timeout_ms, info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
        if (hr == DXGI_ERROR_ACCESS_LOST) return -1;
        if (FAILED(hr)) { fprintf(stderr, "ERROR: AcquireNextFrame hr=0x%08lX\n", (unsigned long)hr); return -2; }
        holding = true;
        if (info->LastPresentTime.QuadPart == 0 || info->AccumulatedFrames == 0) { res->Release(); return 0; }
        hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)tex);
        res->Release();
        if (FAILED(hr)) return -2;
        D3D11_TEXTURE2D_DESC td;
        (*tex)->GetDesc(&td);
        if (td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || (int)td.Width != w || (int)td.Height != h) {
            static int warned = 0;
            if (!warned++) fprintf(stderr, "\nwarning: skipping desktop frame with format %d %ux%u (HDR off?)\n",
                                   (int)td.Format, td.Width, td.Height);
            (*tex)->Release(); *tex = nullptr;
            return 0;
        }
        return 1;
    }

    void close() {
        release_frame();
        if (dupl) { dupl->Release(); dupl = nullptr; }
    }
};

static std::mutex g_mux;   // video and audio packets are written from different threads

static int write_packets(AVCodecContext *enc, AVFormatContext *fmt, AVStream *st, AVPacket *pkt) {
    for (;;) {
        int r = avcodec_receive_packet(enc, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
        if (r < 0) { fprintf(stderr, "ERROR: receive_packet: %s\n", averr(r).c_str()); return r; }
        av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
        pkt->stream_index = st->index;
        {
            std::lock_guard<std::mutex> lk(g_mux);
            r = av_interleaved_write_frame(fmt, pkt);
        }
        if (r < 0) { fprintf(stderr, "ERROR: write_frame: %s\n", averr(r).c_str()); return r; }
    }
}


// ---------------------------------------------------------------------------
// Audio: WASAPI loopback of the default render device (system audio), or process loopback
// (only the sound of one application and its child processes, like OBS "Application Audio Capture")
// ---------------------------------------------------------------------------
static const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIID_IAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const PROPERTYKEY kPKEY_Device_FriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
static const IID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID kIID_IAgileObject = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
static const IID kIID_IActivateAudioInterfaceCompletionHandler =
    {0x41D949AB, 0x9862, 0x444A, {0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB}};

// Process loopback (Windows 10 2004+ / 11), as in audioclientactivationparams.h (missing from mingw-w64).
// Same layout as AUDIOCLIENT_ACTIVATION_PARAMS { ActivationType = PROCESS_LOOPBACK, { pid, mode } }.
struct ProcessLoopbackActivationParams {
    DWORD activation_type;   // 1 = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
    DWORD target_pid;
    DWORD mode;              // 0 = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
};
static const wchar_t *kVirtualAudioDeviceProcessLoopback = L"VAD\\Process_Loopback";

// completion handler for ActivateAudioInterfaceAsync (must be agile: called on a worker thread)
struct ActivateHandler : IActivateAudioInterfaceCompletionHandler, IAgileObject {
    LONG refs = 1;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    virtual ~ActivateHandler() { CloseHandle(done); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **pp) override {
        if (riid == kIID_IUnknown || riid == kIID_IActivateAudioInterfaceCompletionHandler)
            *pp = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        else if (riid == kIID_IAgileObject)
            *pp = static_cast<IAgileObject *>(this);
        else { *pp = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs);
        if (!r) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *) override {
        SetEvent(done);
        return S_OK;
    }
};

// Find a process by executable name (case-insensitive) or by a numeric PID. With several
// processes of the same name, the topmost one is used (its children are captured as well).
static DWORD find_process(const std::string &name) {
    if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos) return (DWORD)atol(name.c_str());
    const std::wstring want = to_wide(name);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    std::map<DWORD, DWORD> matches;   // pid -> parent pid
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, want.c_str()) == 0) matches[pe.th32ProcessID] = pe.th32ParentProcessID;
    CloseHandle(snap);
    for (auto &m : matches)
        if (!matches.count(m.second)) return m.first;
    return matches.empty() ? 0 : matches.begin()->first;
}

struct AudioRecorder {
    // WASAPI
    IMMDevice *dev = nullptr;
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capc = nullptr;
    WAVEFORMATEX *wf = nullptr;
    HANDLE ev = nullptr;                 // event-driven capture (process loopback)
    int rate = 0, channels = 0, bits = 0;
    bool is_float = false;
    char name[256] = "?";
    // encoding
    AVCodecContext *enc = nullptr;
    AVStream *st = nullptr;
    AVFormatContext *fmt = nullptr;
    AVAudioFifo *fifo = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    int frame_size = 1024;
    bool pcm = false;
    // sync
    std::atomic<int64_t> vstart100{0};   // first video present time, 100 ns QPC units
    int64_t written = 0, encoded = 0;
    int64_t silence_samples = 0, skipped_samples = 0, discontinuities = 0, packets = 0, no_timestamp = 0;
    std::vector<float> planar;
    float gain = 1.0f;                   // -again (linear); samples are clamped to [-1, 1] when != 1
    std::atomic<bool> stop{false};
    std::atomic<int> err{0};
    std::thread th;

    int init_capture() {
        IMMDeviceEnumerator *en = nullptr;
        CHECK_HR(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void **)&en),
                 "create MMDeviceEnumerator");
        HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        en->Release();
        CHECK_HR(hr, "default audio output device");
        IPropertyStore *ps = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kPKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, sizeof(name), nullptr, nullptr);
            PropVariantClear(&v);
            ps->Release();
        }
        CHECK_HR(dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void **)&client), "activate IAudioClient");
        CHECK_HR(client->GetMixFormat(&wf), "GetMixFormat");
        rate = (int)wf->nSamplesPerSec;
        channels = wf->nChannels;
        bits = wf->wBitsPerSample;
        WORD tag = wf->wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE) tag = (WORD)((WAVEFORMATEXTENSIBLE *)wf)->SubFormat.Data1;
        is_float = tag == WAVE_FORMAT_IEEE_FLOAT;
        if (!((is_float && bits == 32) || (tag == WAVE_FORMAT_PCM && (bits == 16 || bits == 32)))) {
            fprintf(stderr, "ERROR: unsupported audio mix format (tag %d, %d bit)\n", tag, bits);
            return 1;
        }
        CHECK_HR(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf, nullptr),
                 "IAudioClient::Initialize (loopback)");
        CHECK_HR(client->GetService(kIID_IAudioCaptureClient, (void **)&capc), "IAudioCaptureClient");
        return 0;
    }

    // Capture only process `pid` (and its children). The stream is requested as stereo float at the
    // default output device's rate; Windows mixes / converts the process audio to it.
    int init_capture_process(DWORD pid, const std::string &pname) {
        rate = 48000;
        {
            IMMDeviceEnumerator *en = nullptr;
            IMMDevice *d = nullptr;
            IAudioClient *c = nullptr;
            WAVEFORMATEX *mix = nullptr;
            if (SUCCEEDED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void **)&en)) &&
                SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d)) &&
                SUCCEEDED(d->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void **)&c)) &&
                SUCCEEDED(c->GetMixFormat(&mix)))
                rate = (int)mix->nSamplesPerSec;
            if (mix) CoTaskMemFree(mix);
            if (c) c->Release();
            if (d) d->Release();
            if (en) en->Release();
        }

        ProcessLoopbackActivationParams ap = {1, pid, 0};
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(ap);
        pv.blob.pBlobData = (BYTE *)&ap;
        ActivateHandler *h = new ActivateHandler();
        IActivateAudioInterfaceAsyncOperation *op = nullptr;
        IUnknown *unk = nullptr;
        HRESULT ar = E_FAIL;
        HRESULT hr = ActivateAudioInterfaceAsync(kVirtualAudioDeviceProcessLoopback, kIID_IAudioClient, &pv, h, &op);
        if (SUCCEEDED(hr) && WaitForSingleObject(h->done, 5000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (SUCCEEDED(hr)) hr = op->GetActivateResult(&ar, &unk);
        if (SUCCEEDED(hr)) hr = ar;
        if (SUCCEEDED(hr)) hr = unk->QueryInterface(kIID_IAudioClient, (void **)&client);
        if (unk) unk->Release();
        if (op) op->Release();
        h->Release();
        if (FAILED(hr)) {
            fprintf(stderr, "warning: process loopback activation failed (hr=0x%08lX)\n", (unsigned long)hr);
            return 1;
        }

        WAVEFORMATEX *f = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        memset(f, 0, sizeof(*f));
        f->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        f->nChannels = 2;
        f->nSamplesPerSec = (DWORD)rate;
        f->wBitsPerSample = 32;
        f->nBlockAlign = (WORD)(f->nChannels * 4);
        f->nAvgBytesPerSec = f->nSamplesPerSec * f->nBlockAlign;
        wf = f;
        channels = 2;
        bits = 32;
        is_float = true;
        snprintf(name, sizeof(name), "process %s (pid %lu)", pname.c_str(), (unsigned long)pid);

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                2000000, 0, wf, nullptr);
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(ev);
        if (SUCCEEDED(hr)) hr = client->GetService(kIID_IAudioCaptureClient, (void **)&capc);
        if (FAILED(hr)) {
            fprintf(stderr, "warning: process loopback initialization failed (hr=0x%08lX)\n", (unsigned long)hr);
            release_capture();
            return 1;
        }
        return 0;
    }

    void release_capture() {
        if (capc) { capc->Release(); capc = nullptr; }
        if (client) { client->Release(); client = nullptr; }
        if (wf) { CoTaskMemFree(wf); wf = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
        if (ev) { CloseHandle(ev); ev = nullptr; }
        snprintf(name, sizeof(name), "?");
    }

    int init_encoder(AVFormatContext *f, bool use_pcm, int kbps) {
        fmt = f;
        pcm = use_pcm;
        const AVCodec *c = avcodec_find_encoder(pcm ? AV_CODEC_ID_PCM_S16LE : AV_CODEC_ID_AAC);
        if (!c) { fprintf(stderr, "ERROR: audio encoder not found\n"); return 1; }
        enc = avcodec_alloc_context3(c);
        enc->sample_rate = rate;
        enc->sample_fmt = pcm ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&enc->ch_layout, channels);
        enc->time_base = av_make_q(1, rate);
        if (!pcm) enc->bit_rate = (int64_t)kbps * 1000;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        CHECK_AV(avcodec_open2(enc, c, nullptr), "open audio encoder");
        if (enc->frame_size > 0) frame_size = enc->frame_size;
        st = avformat_new_stream(fmt, nullptr);
        CHECK_AV(avcodec_parameters_from_context(st->codecpar, enc), "audio codec parameters");
        st->time_base = enc->time_base;
        fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, channels, rate);
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        return 0;
    }

    // append interleaved samples (nullptr = silence) to the FIFO as planar float
    void append(const BYTE *data, int64_t n) {
        while (n > 0) {
            int chunk = (int)FFMIN(n, (int64_t)rate);       // at most 1 s per step
            planar.assign((size_t)chunk * channels, 0.0f);
            if (data) {
                for (int i = 0; i < chunk; i++)
                    for (int ch = 0; ch < channels; ch++) {
                        float v;
                        size_t k = (size_t)i * channels + ch;
                        if (is_float) v = ((const float *)data)[k];
                        else if (bits == 16) v = ((const int16_t *)data)[k] / 32768.0f;
                        else v = ((const int32_t *)data)[k] / 2147483648.0f;
                        if (gain != 1.0f) {
                            v *= gain;
                            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                        }
                        planar[(size_t)ch * chunk + i] = v;
                    }
                data += (size_t)chunk * channels * (bits / 8);
            }
            void *ptrs[AV_NUM_DATA_POINTERS];
            for (int ch = 0; ch < channels && ch < AV_NUM_DATA_POINTERS; ch++) ptrs[ch] = &planar[(size_t)ch * chunk];
            av_audio_fifo_write(fifo, ptrs, chunk);
            written += chunk;
            n -= chunk;
        }
        drain(false);
    }

    int drain(bool final) {
        while (av_audio_fifo_size(fifo) >= frame_size || (final && av_audio_fifo_size(fifo) > 0)) {
            int n = FFMIN(frame_size, av_audio_fifo_size(fifo));
            av_frame_unref(frame);
            frame->nb_samples = n;
            frame->format = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_copy(&frame->ch_layout, &enc->ch_layout);
            frame->sample_rate = rate;
            if (av_frame_get_buffer(frame, 0) < 0) return -1;
            av_audio_fifo_read(fifo, (void **)frame->extended_data, n);
            AVFrame *tosend = frame;
            AVFrame *s16 = nullptr;
            if (pcm) {
                s16 = av_frame_alloc();
                s16->nb_samples = n;
                s16->format = AV_SAMPLE_FMT_S16;
                av_channel_layout_copy(&s16->ch_layout, &enc->ch_layout);
                s16->sample_rate = rate;
                av_frame_get_buffer(s16, 0);
                int16_t *o = (int16_t *)s16->data[0];
                for (int i = 0; i < n; i++)
                    for (int ch = 0; ch < channels; ch++) {
                        float v = ((float *)frame->extended_data[ch])[i];
                        v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                        o[i * channels + ch] = (int16_t)lrintf(v * 32767.0f);
                    }
                tosend = s16;
            }
            tosend->pts = encoded;
            encoded += n;
            int r = avcodec_send_frame(enc, tosend);
            av_frame_free(&s16);
            if (r < 0 || write_packets(enc, fmt, st, pkt) < 0) { err = 1; return -1; }
        }
        return 0;
    }

    // place a WASAPI packet on the video timeline (QPC based), inserting silence for gaps
    // (loopback delivers nothing while no sound plays) and trimming overlaps / clock drift.
    void place(const BYTE *data, int64_t frames, DWORD flags, UINT64 qpc100) {
        const int64_t vs = vstart100.load();
        if (!vs) return;                                   // video has not started yet
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) data = nullptr;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) discontinuities++;
        int64_t skip = 0;
        if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
            int64_t idx = llround((double)((int64_t)qpc100 - vs) * rate / 1e7);
            int64_t e = idx - written;
            const int64_t tol = rate / 100;               // 10 ms
            if (written == 0 && e > 0) { append(nullptr, e); silence_samples += e; }
            else if (written == 0 && e < 0) skip = FFMIN(-e, frames);
            else if (e > tol) { append(nullptr, e); silence_samples += e; }
            else if (e < -tol) { skip = FFMIN(-e, frames); skipped_samples += skip; }
        }
        if (frames > skip)
            append(data ? data + (size_t)skip * channels * (bits / 8) : nullptr, frames - skip);
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(client->Start())) { fprintf(stderr, "ERROR: audio client start failed\n"); err = 1; return; }
        LARGE_INTEGER qpf;
        QueryPerformanceFrequency(&qpf);
        while (!stop) {
            if (ev) WaitForSingleObject(ev, 10);
            else Sleep(5);
            UINT32 next = 0;
            HRESULT hr = capc->GetNextPacketSize(&next);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                fprintf(stderr, "\nwarning: audio device changed/removed, audio continues as silence\n");
                break;
            }
            while (SUCCEEDED(hr) && next > 0) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devpos = 0, qpc = 0;
                hr = capc->GetBuffer(&data, &frames, &flags, &devpos, &qpc);
                if (FAILED(hr)) break;
                packets++;
                if (qpc == 0) {   // no device timestamp: assume the packet ends now
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    qpc = (UINT64)(av_rescale(now.QuadPart, 10000000, qpf.QuadPart) - (int64_t)frames * 10000000 / rate);
                    no_timestamp++;
                }
                place(data, frames, flags, qpc);
                capc->ReleaseBuffer(frames);
                if (err) break;
                hr = capc->GetNextPacketSize(&next);
            }
            if (err) break;
        }
        client->Stop();
        CoUninitialize();
    }

    void start() { th = std::thread([this] { run(); }); }

    // stop capturing, pad with silence up to the video duration, flush the encoder
    void finish(int64_t video_end_samples) {
        stop = true;
        if (th.joinable()) th.join();
        if (!err && written < video_end_samples) {
            silence_samples += video_end_samples - written;
            append(nullptr, video_end_samples - written);
        }
        drain(true);
        avcodec_send_frame(enc, nullptr);
        write_packets(enc, fmt, st, pkt);
    }

    void release() {
        av_audio_fifo_free(fifo);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&enc);
        release_capture();
    }
};

// GPU priority of the capture copy / conversion / encode submissions (like OBS). Level 0 leaves
// everything unchanged (the verified default); higher levels raise the per-device GPU thread
// priority and the process GPU scheduling class. Failures are warnings only; realtime needs
// administrator rights and falls back to high.
static void set_gpu_priority(int level, std::initializer_list<ID3D11Device *> devs) {
    static const int kThread[4] = {0, 3, 7, 7};                 // IDXGIDevice::SetGPUThreadPriority, -7..7
    static const int kClass[4] = {2, 3, 4, 5};                  // D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL..REALTIME
    static const char *kClassName[6] = {"idle", "below normal", "normal", "above normal", "high", "realtime"};
    level = level < 0 ? 0 : level > 3 ? 3 : level;
    int dev_ok = 0;
    for (ID3D11Device *d : devs) {
        IDXGIDevice *x = nullptr;
        HRESULT hr = d ? d->QueryInterface(__uuidof(IDXGIDevice), (void **)&x) : E_POINTER;
        if (SUCCEEDED(hr)) { hr = x->SetGPUThreadPriority(kThread[level]); x->Release(); }
        if (SUCCEEDED(hr)) dev_ok++;
        else fprintf(stderr, "warning: gpu priority: SetGPUThreadPriority(%d) failed (hr=0x%08lX)\n", kThread[level], (unsigned long)hr);
    }
    typedef LONG(APIENTRY * set_class_t)(HANDLE, int);
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    set_class_t set_class = gdi ? (set_class_t)(void *)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass") : nullptr;
    int cls = kClass[level];
    LONG st = set_class ? set_class(GetCurrentProcess(), cls) : -1;
    if (st != 0 && cls == 5 && set_class) {
        fprintf(stderr, "warning: gpu priority: realtime class needs administrator rights (status 0x%08lX), using high\n",
                (unsigned long)st);
        cls = 4;
        st = set_class(GetCurrentProcess(), cls);
    }
    if (st != 0)
        fprintf(stderr, "warning: gpu priority: scheduling class %s failed (status 0x%08lX)\n", kClassName[cls], (unsigned long)st);
    printf("gpu priority: level %d, device thread priority %+d (%d/%d devices), scheduling class %s (%s)\n", level,
           kThread[level], dev_ok, (int)devs.size(), kClassName[cls], st == 0 ? "ok" : "failed");
}

int main() {
    // start of process, for the "init" time report (how long a hotkey press takes to start recording)
    double init_base_ms = 0;
    {
        FILETIME c, e, k, u, now;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        GetSystemTimePreciseAsFileTime(&now);
        auto ft = [](FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e4; };
        init_base_ms = ft(now) - ft(c);
    }
    auto t_main = std::chrono::steady_clock::now();

    // arguments as UTF-8 (FFmpeg's file protocol takes UTF-8 paths; argv would be the ANSI code page)
    std::vector<std::string> argv;
    {
        int wargc = 0;
        wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        for (int i = 0; i < wargc; i++) argv.push_back(to_utf8(wargv[i]));
        LocalFree(wargv);
    }
    const int argc = (int)argv.size();

    std::string out_path, codec_name = "hevc", preset = "p4";
    double duration = 0;
    int monitor = 0, fps = 60, cq = 18, maxcll = 1000, maxfall = 400, mdl = 1000;
    double maxrate = 0, bitrate = 0;
    std::string timelog;
    int audio_on = 1, abitrate = 256;
    double again_db = 0;
    int gpuprio = 0;
    std::string acodec = "aac", audio_proc;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); }
            return argv[++i].c_str();
        };
        if (a == "-o") out_path = next();
        else if (a == "-t") duration = atof(next());
        else if (a == "-monitor") monitor = atoi(next());
        else if (a == "-fps") fps = atoi(next());
        else if (a == "-codec") codec_name = next();
        else if (a == "-cq") cq = atoi(next());
        else if (a == "-preset") preset = next();
        else if (a == "-maxrate") maxrate = atof(next());
        else if (a == "-bitrate") bitrate = atof(next());
        else if (a == "-maxcll") maxcll = atoi(next());
        else if (a == "-maxfall") maxfall = atoi(next());
        else if (a == "-mdl") mdl = atoi(next());
        else if (a == "-timelog") timelog = next();
        else if (a == "-audio") audio_on = atoi(next());
        else if (a == "-acodec") acodec = next();
        else if (a == "-abitrate") abitrate = atoi(next());
        else if (a == "-audio-process") audio_proc = next();
        else if (a == "-again") again_db = atof(next());
        else if (a == "-gpuprio") gpuprio = atoi(next());
        else if (a == "-list-monitors") { DupCapture::list_outputs(); return 0; }
        else if (a == "-h" || a == "--help") {
            printf("hdrrec [-o out.mkv] [-t sec] [-monitor N] [-fps 60] [-codec hevc|av1] [-cq 18]\n"
                   "       [-bitrate Mbps] [-preset p4] [-maxrate Mbps] [-maxcll 1000] [-maxfall 400] [-mdl 1000]\n"
                   "       [-audio 0|1] [-acodec aac|pcm] [-abitrate 256] [-again dB] [-audio-process NAME|PID]\n"
                   "       [-gpuprio 0..3]\n"
                   "       [-timelog times.csv]\n"
                   "       [-list-monitors]\n");
            return 0;
        } else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (out_path.empty()) {
        time_t t = time(nullptr); struct tm lt; localtime_s(&lt, &t);
        char buf[64]; strftime(buf, sizeof(buf), "rec_%Y%m%d_%H%M%S.mkv", &lt);
        out_path = buf;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
    timeBeginPeriod(1);                      // 1 ms Sleep granularity for the capture poll loop
    av_log_set_level(AV_LOG_WARNING);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // ---------------- capture: Desktop Duplication on the monitor's adapter ----------------
    IDXGIOutput *dxgi_out = nullptr;
    DXGI_OUTPUT_DESC odesc = {};
    int adapter = DupCapture::find_output(monitor, &dxgi_out, &odesc);
    if (adapter < 0) { fprintf(stderr, "ERROR: monitor %d not found\n", monitor); return 1; }
    char adapter_str[16];
    snprintf(adapter_str, sizeof(adapter_str), "%d", adapter);
    AVBufferRef *dev_ref = nullptr;
    CHECK_AV(av_hwdevice_ctx_create(&dev_ref, AV_HWDEVICE_TYPE_D3D11VA, adapter_str, nullptr, 0), "create D3D11 device");
    AVD3D11VADeviceContext *d3d = (AVD3D11VADeviceContext *)((AVHWDeviceContext *)dev_ref->data)->hwctx;

    // Separate D3D11 device for Desktop Duplication: NVENC on the encode device holds the
    // device lock while it waits for the GPU, which used to stall the capture loop.
    IDXGIAdapter *cap_adapter = nullptr;
    CHECK_HR(dxgi_out->GetParent(__uuidof(IDXGIAdapter), (void **)&cap_adapter), "output adapter");
    ID3D11Device *cdev = nullptr;
    ID3D11DeviceContext *cctx = nullptr;
    {
        D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        CHECK_HR(D3D11CreateDevice(cap_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fls, 2, D3D11_SDK_VERSION,
                                   &cdev, nullptr, &cctx), "create capture D3D11 device");
    }
    if (gpuprio > 0) set_gpu_priority(gpuprio, {cdev, d3d->device});

    DupCapture cap;
    if (cap.open(dxgi_out, cdev)) return 1;
    const int W = cap.w, H = cap.h;
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    printf("capture: monitor %d (adapter %d, %ls), %dx%d, FP16 scRGB via Desktop Duplication\n",
           monitor, adapter, odesc.DeviceName, W, H);

    // ---------------- P010 output frame pool on the same D3D11 device ----------------
    {
        UINT sup = 0;
        d3d->device->CheckFormatSupport(DXGI_FORMAT_P010, &sup);
        printf("P010 format support: 0x%08X (render target: %s)\n", sup,
               (sup & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? "yes" : "NO");
    }
    // Some drivers reject planar (P010) texture *arrays* with RENDER_TARGET, so try
    // individually allocated textures first, then arrays.
    struct PoolCfg { int pool; UINT bind; const char *name; };
    const PoolCfg cfgs[] = {
        {0,  D3D11_BIND_RENDER_TARGET,                        "single textures, RT"},
        {0,  D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, "single textures, RT|SRV"},
        {24, D3D11_BIND_RENDER_TARGET,                        "texture array, RT"},
        {24, D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER, "texture array, RT|VIDEO_ENCODER"},
    };
    AVBufferRef *out_frames_ref = nullptr;
    for (const PoolCfg &c : cfgs) {
        AVBufferRef *ref = av_hwframe_ctx_alloc(dev_ref);
        AVHWFramesContext *fc = (AVHWFramesContext *)ref->data;
        fc->format = AV_PIX_FMT_D3D11;
        fc->sw_format = AV_PIX_FMT_P010;
        fc->width = W;
        fc->height = H;
        fc->initial_pool_size = c.pool;
        ((AVD3D11VAFramesContext *)fc->hwctx)->BindFlags = c.bind;
        int r = av_hwframe_ctx_init(ref);
        if (r >= 0) {
            // a dynamic pool only allocates on demand: make sure one texture can really be created
            AVFrame *probe = av_frame_alloc();
            r = av_hwframe_get_buffer(ref, probe, 0);
            av_frame_free(&probe);
        }
        if (r >= 0) { printf("P010 pool: %s\n", c.name); out_frames_ref = ref; break; }
        printf("P010 pool: %s -> failed (%s)\n", c.name, averr(r).c_str());
        av_buffer_unref(&ref);
    }
    if (!out_frames_ref) { fprintf(stderr, "ERROR: could not create a P010 render-target frame pool\n"); return 1; }
    {   // pre-allocate textures so that no allocation happens while recording
        AVFrame *pre[40] = {};
        for (int i = 0; i < 40; i++) { pre[i] = av_frame_alloc(); av_hwframe_get_buffer(out_frames_ref, pre[i], 0); }
        for (int i = 0; i < 40; i++) av_frame_free(&pre[i]);
    }

    Converter conv;
    d3d->lock(d3d->lock_ctx);
    int cr = conv.init(d3d->device, d3d->device_context, W, H);
    d3d->unlock(d3d->lock_ctx);
    if (cr) return 1;

    // Shared FP16 ring between the capture device (A) and the encode device (B).
    // Keyed mutex protocol: key 0 = owned by capture, key 1 = owned by encoder.
    struct RingTex {
        ID3D11Texture2D *texB = nullptr; ID3D11ShaderResourceView *srvB = nullptr; IDXGIKeyedMutex *kmB = nullptr;
        ID3D11Texture2D *texA = nullptr; IDXGIKeyedMutex *kmA = nullptr;
        bool busy = false;
    };
    const int kRing = 6;
    RingTex ring[kRing];
    for (int i = 0; i < kRing; i++) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        d3d->lock(d3d->lock_ctx);
        HRESULT hr = d3d->device->CreateTexture2D(&td, nullptr, &ring[i].texB);
        if (SUCCEEDED(hr)) hr = d3d->device->CreateShaderResourceView(ring[i].texB, nullptr, &ring[i].srvB);
        d3d->unlock(d3d->lock_ctx);
        CHECK_HR(hr, "create shared ring texture");
        CHECK_HR(ring[i].texB->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&ring[i].kmB), "keyed mutex (encode side)");
        IDXGIResource *dr = nullptr;
        HANDLE sh = nullptr;
        CHECK_HR(ring[i].texB->QueryInterface(__uuidof(IDXGIResource), (void **)&dr), "IDXGIResource");
        hr = dr->GetSharedHandle(&sh);
        dr->Release();
        CHECK_HR(hr, "GetSharedHandle");
        CHECK_HR(cdev->OpenSharedResource(sh, __uuidof(ID3D11Texture2D), (void **)&ring[i].texA), "OpenSharedResource");
        CHECK_HR(ring[i].texA->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&ring[i].kmA), "keyed mutex (capture side)");
    }
    printf("capture/encode: separate D3D11 devices, %d shared FP16 buffers\n", kRing);

    // ---------------- encoder ----------------
    const char *enc_name = codec_name == "av1" ? "av1_nvenc" : "hevc_nvenc";
    const AVCodec *codec = avcodec_find_encoder_by_name(enc_name);
    if (!codec) { fprintf(stderr, "ERROR: encoder %s not found\n", enc_name); return 1; }
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    enc->width = W;
    enc->height = H;
    enc->time_base = av_make_q(1, fps);
    enc->framerate = av_make_q(fps, 1);
    enc->sample_aspect_ratio = av_make_q(1, 1);
    enc->pix_fmt = AV_PIX_FMT_D3D11;
    enc->sw_pix_fmt = AV_PIX_FMT_P010;
    enc->hw_frames_ctx = av_buffer_ref(out_frames_ref);
    enc->color_primaries = AVCOL_PRI_BT2020;
    enc->color_trc = AVCOL_TRC_SMPTE2084;
    enc->colorspace = AVCOL_SPC_BT2020_NCL;
    enc->color_range = AVCOL_RANGE_MPEG;
    enc->chroma_sample_location = AVCHROMA_LOC_CENTER;
    enc->gop_size = fps * 2;
    enc->bit_rate = 0;
    if (bitrate > 0 && maxrate <= 0) maxrate = bitrate * 1.5;
    if (bitrate > 0) enc->bit_rate = (int64_t)(bitrate * 1e6);
    if (maxrate > 0) {
        enc->rc_max_rate = (int64_t)(maxrate * 1e6);
        enc->rc_buffer_size = (int)(maxrate * 2e6);
    }
    av_opt_set(enc->priv_data, "preset", preset.c_str(), 0);
    av_opt_set(enc->priv_data, "tune", "hq", 0);
    av_opt_set(enc->priv_data, "rc", "vbr", 0);
    if (bitrate <= 0) av_opt_set_int(enc->priv_data, "cq", cq, 0);   // with -bitrate: plain VBR
    if (codec_name != "av1") av_opt_set(enc->priv_data, "profile", "main10", 0);

    // HDR10 static metadata (SEI / OBU via nvenc, container via coded_side_data)
    AVFrameSideData *sd = av_frame_side_data_new(&enc->decoded_side_data, &enc->nb_decoded_side_data,
                                                 AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
                                                 sizeof(AVMasteringDisplayMetadata), 0);
    AVMasteringDisplayMetadata *mdm = (AVMasteringDisplayMetadata *)sd->data;
    memset(mdm, 0, sizeof(*mdm));
    mdm->display_primaries[0][0] = chroma(0.708); mdm->display_primaries[0][1] = chroma(0.292);
    mdm->display_primaries[1][0] = chroma(0.170); mdm->display_primaries[1][1] = chroma(0.797);
    mdm->display_primaries[2][0] = chroma(0.131); mdm->display_primaries[2][1] = chroma(0.046);
    mdm->white_point[0] = chroma(0.3127); mdm->white_point[1] = chroma(0.3290);
    mdm->max_luminance = av_make_q(mdl, 1);
    mdm->min_luminance = av_make_q(1, 10000);
    mdm->has_primaries = 1;
    mdm->has_luminance = 1;
    sd = av_frame_side_data_new(&enc->decoded_side_data, &enc->nb_decoded_side_data,
                                AV_FRAME_DATA_CONTENT_LIGHT_LEVEL, sizeof(AVContentLightMetadata), 0);
    AVContentLightMetadata *cll = (AVContentLightMetadata *)sd->data;
    cll->MaxCLL = maxcll;
    cll->MaxFALL = maxfall;

    AVFormatContext *fmt = nullptr;
    CHECK_AV(avformat_alloc_output_context2(&fmt, nullptr, nullptr, out_path.c_str()), "alloc output");
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    CHECK_AV(avcodec_open2(enc, codec, nullptr), "open encoder (check NVIDIA driver)");

    AVStream *st = avformat_new_stream(fmt, nullptr);
    CHECK_AV(avcodec_parameters_from_context(st->codecpar, enc), "codec parameters");
    st->time_base = enc->time_base;
    st->avg_frame_rate = enc->framerate;

    AudioRecorder audio;
    if (audio_on) {
        int ar = 1;
        if (!audio_proc.empty()) {
            DWORD pid = find_process(audio_proc);
            if (!pid)
                fprintf(stderr, "warning: audio process \"%s\" is not running, recording system audio instead\n", audio_proc.c_str());
            else if ((ar = audio.init_capture_process(pid, audio_proc)) != 0)
                fprintf(stderr, "warning: audio process \"%s\" could not be captured, recording system audio instead\n", audio_proc.c_str());
        }
        if (ar && audio.init_capture()) return 1;
        if (audio.init_encoder(fmt, acodec == "pcm", abitrate)) return 1;
        audio.gain = (float)pow(10.0, again_db / 20.0);
        printf("audio: \"%s\" %d Hz %dch -> %s, gain %+.1f dB\n", audio.name, audio.rate, audio.channels,
               acodec == "pcm" ? "PCM 16-bit" : (std::to_string(abitrate) + " kbps AAC").c_str(), again_db);
    }
    CHECK_AV(avio_open(&fmt->pb, out_path.c_str(), AVIO_FLAG_WRITE), "open output file");
    CHECK_AV(avformat_write_header(fmt, nullptr), "write header");
    if (audio_on) audio.start();

    char rc_desc[64];
    if (bitrate > 0) snprintf(rc_desc, sizeof(rc_desc), "vbr %.0f Mbps (max %.0f)", bitrate, maxrate);
    else snprintf(rc_desc, sizeof(rc_desc), "cq=%d", cq);
    printf("encoder: %s %s %s, HDR10 BT.2020/PQ, MDL=%d MaxCLL=%d MaxFALL=%d\n",
           enc_name, preset.c_str(), rc_desc, mdl, maxcll, maxfall);
    printf("init: %.0f ms (process start %.0f ms + setup %.0f ms)\n",
           init_base_ms + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_main).count(),
           init_base_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_main).count());
    printf("recording to %s  (press Enter or Ctrl+C to stop%s)\n", out_path.c_str(),
           duration > 0 ? ", or wait for -t" : "");

    // Stop on Enter / any stdin input / stdin EOF (the GUI writes a newline). Raw ReadFile instead of
    // getchar(): a thread blocked inside the CRT holds the stdin lock, and the CRT's exit-time cleanup
    // then waits for it forever (with the MSYS2 GCC 16 runtime hdrrec never exited from a console).
    std::thread([] {
        char buf[64];
        DWORD n = 0;
        ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, sizeof(buf), &n, nullptr);
        g_stop = true;
    }).detach();

    // ---------------- main loop ----------------
    // Constant frame rate placement (phase-locked):
    //   s = present_time * fps - phase, slot = round(s).
    //   present_time is DXGI's LastPresentTime (QPC) of the captured desktop frame, so arrival
    //   jitter of the capture loop does not matter. 'phase' is tracked so that presents land in
    //   the middle of their output slots. A frame that rounds into an already used slot is moved
    //   to the next slot (if it is no more than one slot early); empty slots repeat the previous image.
    //
    // Missed-frame detection:
    //   AccumulatedFrames > 1 means the desktop was updated more than once since the previous
    //   acquire, i.e. the capture loop was too slow and (AccumulatedFrames - 1) frames were lost.
    //   Empty slots are therefore split into "capture misses" and "source repeats" (the game /
    //   compositor did not produce a new frame).
    AVPacket *pkt = av_packet_alloc();
    int64_t first_qpc = 0, last_slot = -1, captured = 0, dups = 0, drops = 0;
    int64_t missed_presents = 0, dup_capture = 0, dup_source = 0, access_lost = 0;
    double lat_sum = 0, lat_max = 0;
    auto t0 = std::chrono::steady_clock::now();
    auto last_report = t0;
    int ret = 0;
    FILE *tl = timelog.empty() ? nullptr : _wfopen(to_wide(timelog).c_str(), L"w");
    if (tl) fprintf(tl, "present_us,acquire_latency_ms,accumulated,slot_float,slot,action,phase,ring,copy_ms,release_ms\n");
    double phase = 0.0;
    const double kFillStart = 6.0;           // empty slots before repeats are fed during a freeze
    const int64_t kFillMargin = 3;           // ...staying this many slots behind the current time
    const double kPhaseMax = 0.5;            // |phase| limit in slots (keeps video within ~8 ms of real time)
    int64_t locked_frames = 0;

    // ---- encoder thread: converts shared FP16 buffers to P010 and encodes ----
    struct Item { int ring; int64_t slot; };          // ring == -1: repeat the previous image
    std::deque<Item> queue;
    std::mutex qm;
    std::condition_variable qcv;
    bool cap_done = false;
    std::atomic<int> enc_err{0};
    size_t queued_frames = 0, max_queued = 0;
    int64_t overflow = 0;

    auto send = [&](AVFrame *f, int64_t slot) -> int {
        AVFrame *c = av_frame_clone(f);
        c->pts = slot;
        int r = avcodec_send_frame(enc, c);
        av_frame_free(&c);
        if (r < 0) { fprintf(stderr, "ERROR: send_frame: %s\n", averr(r).c_str()); return r; }
        return write_packets(enc, fmt, st, pkt);
    };
    auto release_ring = [&](int i) {
        std::lock_guard<std::mutex> lk(qm);
        ring[i].busy = false;
        queued_frames--;
    };
    std::thread enc_thread([&] {
        AVFrame *prev = nullptr;
        for (;;) {
            Item it;
            {
                std::unique_lock<std::mutex> lk(qm);
                qcv.wait(lk, [&] { return !queue.empty() || cap_done; });
                if (queue.empty()) break;
                it = queue.front(); queue.pop_front();
            }
            if (it.ring < 0) {
                if (!enc_err && prev && send(prev, it.slot) < 0) { enc_err = 1; g_stop = true; }
                continue;
            }
            RingTex &rt = ring[it.ring];
            if (enc_err) { rt.kmB->AcquireSync(1, 1000); rt.kmB->ReleaseSync(0); release_ring(it.ring); continue; }

            AVFrame *out = av_frame_alloc();
            int gr = av_hwframe_get_buffer(out_frames_ref, out, 0);
            HRESULT hr = rt.kmB->AcquireSync(1, 1000);
            int e = 1;
            if (gr >= 0 && hr == S_OK) {
                d3d->lock(d3d->lock_ctx);
                e = conv.draw(rt.srvB, (ID3D11Texture2D *)out->data[0], (UINT)(intptr_t)out->data[1]);
                d3d->device_context->Flush();
                d3d->unlock(d3d->lock_ctx);
            }
            if (hr == S_OK) rt.kmB->ReleaseSync(0);
            release_ring(it.ring);
            if (gr < 0 || hr != S_OK || e) {
                fprintf(stderr, "\nERROR: convert failed (get_buffer %d, AcquireSync 0x%08lX)\n", gr, (unsigned long)hr);
                av_frame_free(&out); enc_err = 1; g_stop = true; continue;
            }
            out->color_primaries = AVCOL_PRI_BT2020;
            out->color_trc = AVCOL_TRC_SMPTE2084;
            out->colorspace = AVCOL_SPC_BT2020_NCL;
            out->color_range = AVCOL_RANGE_MPEG;
            out->chroma_location = AVCHROMA_LOC_CENTER;
            if (send(out, it.slot) < 0) { enc_err = 1; g_stop = true; }
            av_frame_free(&prev);
            prev = out;
        }
        av_frame_free(&prev);
    });
    auto push = [&](int ri, int64_t slot) {
        { std::lock_guard<std::mutex> lk(qm); queue.push_back({ri, slot}); }
        qcv.notify_one();
    };
    // reserve a free shared buffer, -1 if the encoder is too far behind.
    // Round robin, not "lowest free index": the encoder frees a buffer on the CPU right after it
    // submits the conversion, so reusing that same buffer for the next capture made the capture
    // device's copy wait on the encode device's GPU work every frame, and Desktop Duplication cannot
    // deliver the next frame until that copy is done (with av1_nvenc p6 every other frame was missed).
    int next_ring = 0;
    auto take_ring = [&]() -> int {
        std::lock_guard<std::mutex> lk(qm);
        for (int k = 0; k < kRing; k++) {
            int i = (next_ring + k) % kRing;
            if (ring[i].busy) continue;
            next_ring = (i + 1) % kRing;
            ring[i].busy = true;
            queued_frames++;
            if (queued_frames > max_queued) max_queued = queued_frames;
            return i;
        }
        overflow++;
        return -1;
    };

    auto qpc_now = []() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (int64_t)q.QuadPart; };
    auto print_status = [&](bool force) {
        auto now = std::chrono::steady_clock::now();
        if (!force && std::chrono::duration<double>(now - last_report).count() < 1.0) return;
        double elapsed = std::chrono::duration<double>(now - t0).count();
        double media = (double)(last_slot + 1) / fps;
        printf("\r time=%.1fs out=%lld unique=%lld dup=%lld(src %lld / miss %lld) drop=%lld missed=%lld lat=%.1f/%.1fms q=%zu ovf=%lld speed=%.3fx   ",
               media, (long long)(last_slot + 1), (long long)(captured - drops), (long long)dups,
               (long long)dup_source, (long long)dup_capture, (long long)drops, (long long)missed_presents,
               captured ? lat_sum / captured : 0.0, lat_max, queued_frames, (long long)overflow,
               elapsed > 0 ? media / elapsed : 0.0);
        fflush(stdout);
        last_report = now;
    };

    while (!g_stop) {
        ID3D11Texture2D *tex = nullptr;
        DXGI_OUTDUPL_FRAME_INFO info = {};
        // the capture device is used by this thread only, so blocking with a short timeout is fine
        int r = cap.acquire(5, &tex, &info);
        int64_t acq_qpc = qpc_now();

        if (r == -1) {                       // mode change, UAC prompt, fullscreen switch...
            access_lost++;
            fprintf(stderr, "\nwarning: desktop duplication access lost, re-opening\n");
            cap.close();
            int tries = 0;
            while (!g_stop && cap.open(dxgi_out, cdev) && ++tries < 50) Sleep(100);
            if (!cap.dupl) { fprintf(stderr, "ERROR: could not re-open desktop duplication\n"); ret = 1; break; }
            if (cap.w != W || cap.h != H) { fprintf(stderr, "ERROR: desktop resolution changed\n"); ret = 1; break; }
            continue;
        }
        if (r == -2) { ret = 1; break; }
        if (r == 0) {                        // no new desktop frame
            if (duration > 0 && first_qpc && (double)(acq_qpc - first_qpc) / qpf.QuadPart >= duration) break;
            // Source frozen (loading screen etc.): hand the repeats to the encoder as time passes instead
            // of queueing the whole gap when the next frame arrives. That burst (~100 repeats after a
            // 2 s freeze) kept NVENC busy and overflowed the shared ring right after the freeze.
            // Starts only after kFillStart empty slots (never at 60 / 30 fps) and stays kFillMargin
            // slots behind "now", so a frame presented now still lands on a new slot.
            if (last_slot >= 0) {
                const double sf_now = (double)(acq_qpc - first_qpc) / qpf.QuadPart * fps - phase;
                if (sf_now - (double)last_slot > kFillStart) {
                    const int64_t fill_to = (int64_t)floor(sf_now) - kFillMargin;
                    for (int64_t k = last_slot + 1; k <= fill_to; k++) {
                        push(-1, k);
                        dups++;
                        dup_source++;
                        last_slot = k;
                        if (tl) fprintf(tl, "%lld,0,0,%.3f,%lld,fill,%.4f,,,\n",
                                        (long long)((acq_qpc - first_qpc) * 1000000LL / qpf.QuadPart), sf_now, (long long)k, phase);
                    }
                }
            }
            print_status(false);
            cap.release_frame();
            continue;
        }

        const int64_t pq = info.LastPresentTime.QuadPart;
        if (!first_qpc) {
            first_qpc = pq;
            audio.vstart100 = av_rescale(first_qpc, 10000000, qpf.QuadPart);
        }
        const double lat_ms = (double)(acq_qpc - pq) * 1000.0 / qpf.QuadPart;
        const long long pres_us = (long long)((pq - first_qpc) * 1000000LL / qpf.QuadPart);
        const int acc = (int)info.AccumulatedFrames;
        captured++;
        lat_sum += lat_ms;
        if (lat_ms > lat_max) lat_max = lat_ms;
        if (acc > 1 && captured > 1) missed_presents += acc - 1;   // the first frame's count is backlog

        double sf = (double)(pq - first_qpc) / qpf.QuadPart * fps - phase;
        int64_t slot = llround(sf);
        double err = sf - (double)slot;                 // [-0.5, 0.5]
        phase += (locked_frames < 30 ? 0.3 : 0.03) * err;
        // The phase may only center presents inside their slots, not absorb a rate difference: with a
        // source slightly below 60 fps (e.g. 59.94) it grew without bound and the video ran ahead of
        // the QPC-placed audio (0.8 s after 30 min). Clamped, the slow drift ends in a single repeat
        // every ~1000 frames, which is the correct 59.94 -> 60 conversion.
        phase = phase > kPhaseMax ? kPhaseMax : (phase < -kPhaseMax ? -kPhaseMax : phase);
        locked_frames++;

        const char *action = "new";
        if (slot <= last_slot) {
            if ((double)(last_slot + 1) - sf <= 1.0) {
                slot = last_slot + 1;                   // arrived early after a hitch: use the next slot
                action = "early";
            } else {
                drops++;
                if (tl) fprintf(tl, "%lld,%.3f,%d,%.3f,%lld,drop,%.4f,,,\n", pres_us, lat_ms, acc, sf, (long long)slot, phase);
                tex->Release();
                continue;
            }
        }
        if (tl) fprintf(tl, "%lld,%.3f,%d,%.3f,%lld,%s,%.4f", pres_us, lat_ms, acc, sf, (long long)slot, action, phase);

        // fill empty slots with the previous image
        if (last_slot >= 0 && slot > last_slot + 1) {
            int64_t gap = slot - last_slot - 1;
            int64_t miss = acc > 1 ? FFMIN(gap, (int64_t)(acc - 1)) : 0;
            dup_capture += miss;
            dup_source += gap - miss;
            for (int64_t k = last_slot + 1; k < slot; k++) { push(-1, k); dups++; }
        }

        int ri = take_ring();
        const int64_t sync_qpc = qpc_now();
        if (ri >= 0 && ring[ri].kmA->AcquireSync(0, 100) == S_OK) {
            cctx->CopyResource(ring[ri].texA, tex);
            ring[ri].kmA->ReleaseSync(1);
            push(ri, slot);
        } else {
            if (ri >= 0) { release_ring(ri); std::lock_guard<std::mutex> lk(qm); overflow++; }
            push(-1, slot);                             // keep CFR: repeat the previous image
        }
        const int64_t rel_qpc = qpc_now();
        tex->Release();
        cap.release_frame();
        if (tl) fprintf(tl, ",%d,%.3f,%.3f\n", ri, (double)(rel_qpc - sync_qpc) * 1000.0 / qpf.QuadPart,
                        (double)(qpc_now() - rel_qpc) * 1000.0 / qpf.QuadPart);
        last_slot = slot;

        print_status(false);
        if (duration > 0 && (double)(last_slot + 1) / fps >= duration) break;
    }

    // pad up to the stop time if the screen was static at the end
    if (last_slot >= 0 && first_qpc && ret == 0) {
        double end_s = (double)(qpc_now() - first_qpc) / qpf.QuadPart;
        if (duration > 0 && end_s > duration) end_s = duration;
        int64_t end_slot = (int64_t)(end_s * fps) - 1;
        for (int64_t k = last_slot + 1; k <= end_slot; k++) { push(-1, k); dups++; dup_source++; last_slot = k; }
    }
    cap.close();
    print_status(true);
    printf("\nstopping, flushing encoder...\n");
    fflush(stdout);
    if (audio_on) audio.finish(av_rescale(last_slot + 1, audio.rate, fps));
    { std::lock_guard<std::mutex> lk(qm); cap_done = true; }
    qcv.notify_one();
    enc_thread.join();
    if (enc_err) ret = 1;
    avcodec_send_frame(enc, nullptr);
    write_packets(enc, fmt, st, pkt);
    av_write_trailer(fmt);
    avio_closep(&fmt->pb);
    if (tl) fclose(tl);

    printf("done: %lld frames written -> %s\n", (long long)(last_slot + 1), out_path.c_str());
    printf("  unique captures      : %lld\n", (long long)(captured - drops));
    printf("  repeated frames      : %lld  (source did not update: %lld, capture missed: %lld)\n",
           (long long)dups, (long long)dup_source, (long long)dup_capture);
    printf("  dropped (same slot)  : %lld\n", (long long)drops);
    printf("  missed desktop frames: %lld  (AccumulatedFrames > 1)\n", (long long)missed_presents);
    printf("  capture latency      : avg %.2f ms, max %.2f ms\n", captured ? lat_sum / captured : 0.0, lat_max);
    printf("  encoder queue        : max %zu of %d buffers, overflow %lld\n", max_queued, kRing, (long long)overflow);
    if (access_lost) printf("  access lost events   : %lld\n", (long long)access_lost);
    if (audio_on) {
        printf("  audio                : %.2f s encoded, %lld packets, silence inserted %.2f s, trimmed %.3f s, discontinuities %lld%s\n",
               (double)audio.encoded / audio.rate, (long long)audio.packets,
               (double)audio.silence_samples / audio.rate, (double)audio.skipped_samples / audio.rate,
               (long long)audio.discontinuities, audio.err ? "  (ERROR)" : "");
        if (audio.no_timestamp)
            printf("  audio no timestamp   : %lld packets (placed by arrival time)\n", (long long)audio.no_timestamp);
        printf("  video duration       : %.2f s\n", (double)(last_slot + 1) / fps);
    }
    fflush(stdout);
    fflush(stderr);

    // (cleanup progress goes to stderr, unbuffered, to locate any teardown problem)
    fprintf(stderr, "[cleanup] encoder\n");
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
    if (audio_on) audio.release();
    fprintf(stderr, "[cleanup] muxer\n");
    avformat_free_context(fmt);
    fprintf(stderr, "[cleanup] converter\n");
    conv.release();
    fprintf(stderr, "[cleanup] frame pool\n");
    av_buffer_unref(&out_frames_ref);
    for (int i = 0; i < kRing; i++) {
        ring[i].kmA->Release(); ring[i].texA->Release();
        ring[i].kmB->Release(); ring[i].srvB->Release(); ring[i].texB->Release();
    }
    cctx->Release(); cdev->Release(); cap_adapter->Release();
    fprintf(stderr, "[cleanup] device\n");
    av_buffer_unref(&dev_ref);
    if (cap.output) cap.output->Release();
    dxgi_out->Release();
    timeEndPeriod(1);
    fprintf(stderr, "[cleanup] done, exit code %d\n", ret);
    fflush(stderr);
    return ret;
}
