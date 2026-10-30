// WDAudioFix.asi v3 - WASAPI diagnostics + format shim for games that go silent on S/PDIF devices.
//
// v3: Watch Dogs reads the endpoint's own device format from its property store.
//     With Dolby Digital Live on, that's the IEC 61937 (Dolby Digital) carrier format,
//     and the game's sound-effects engine never outputs anything. FakeDevice makes the
//     matching device report a plain stereo PCM format and speaker layout instead,
//     which is what VB-Cable reports (and where everything works).
//
// How it works:
//   1. Hooks CoCreateInstance. The first time anything in the process creates an
//      MMDeviceEnumerator, we open a throwaway stream on the default render device
//      and hook the shared implementation of IMMDeviceEnumerator, IMMDevice,
//      IAudioClient(2) and IAudioRenderClient. Every object of those types in the
//      process uses the same code in MMDevAPI/AudioSes, so this covers all of them.
//   2. Logs device selection, stream setup, Start, and - once per second per
//      stream - how much audio the game wrote and how loud it was. That tells us
//      whether the game is sending real sound, silence, or nothing at all.
//   3. Fixes (WDAudioFix.ini):
//      - AutoConvert:    adds AUTOCONVERTPCM to shared-mode Initialize calls.
//      - ForceMixFormat: reports a chosen float mix format (e.g. stereo) to the game.
//      - StripRawMode:   removes RAW/offload stream options, which skip the
//                        driver's effects - including Dolby Digital Live / DTS encoders.
//
// Build: x64 DLL renamed .asi. Load with Ultimate ASI Loader.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmreg.h>
#include <propidl.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include "MinHook.h"

// ---------------------------------------------------------------- config / log

struct Config {
    bool autoConvert   = true;
    bool forceMix      = false;
    UINT forceRate     = 48000;
    UINT forceChannels = 2;
    bool stripRaw      = true;
    bool renderStats   = true;
    std::string fakeDevice = "digital output";   // lowercase substring of the device name; empty = off
    UINT fakeChannels  = 2;
    UINT fakeBits      = 24;
    bool logProps      = true;
} g_cfg;

static HMODULE    g_self = nullptr;
static FILE*      g_log  = nullptr;
static std::mutex g_logMtx;

static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d T%lu] ", t.wHour, t.wMinute, t.wSecond,
            t.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void PathNextToSelf(wchar_t* out, size_t n, const wchar_t* ext) {
    GetModuleFileNameW(g_self, out, (DWORD)n);
    wchar_t* dot = wcsrchr(out, L'.');
    if (dot) *dot = 0;
    wcsncat_s(out, n, ext, _TRUNCATE);
}

static void LoadConfig() {
    wchar_t ini[MAX_PATH];
    PathNextToSelf(ini, MAX_PATH, L".ini");
    g_cfg.autoConvert   = GetPrivateProfileIntW(L"Fix", L"AutoConvert",    1,     ini) != 0;
    g_cfg.forceMix      = GetPrivateProfileIntW(L"Fix", L"ForceMixFormat", 0,     ini) != 0;
    g_cfg.forceRate     = GetPrivateProfileIntW(L"Fix", L"ForceRate",      48000, ini);
    g_cfg.forceChannels = GetPrivateProfileIntW(L"Fix", L"ForceChannels",  2,     ini);
    g_cfg.stripRaw      = GetPrivateProfileIntW(L"Fix", L"StripRawMode",   1,     ini) != 0;
    g_cfg.renderStats   = GetPrivateProfileIntW(L"Log", L"RenderStats",    1,     ini) != 0;
    g_cfg.logProps      = GetPrivateProfileIntW(L"Log", L"DeviceProperties", 1,   ini) != 0;
    g_cfg.fakeChannels  = GetPrivateProfileIntW(L"Fix", L"FakeChannels",   2,     ini);
    g_cfg.fakeBits      = GetPrivateProfileIntW(L"Fix", L"FakeBits",       24,    ini);
    if (g_cfg.forceChannels < 1 || g_cfg.forceChannels > 8) g_cfg.forceChannels = 2;
    if (g_cfg.fakeChannels  < 1 || g_cfg.fakeChannels  > 8) g_cfg.fakeChannels  = 2;
    if (g_cfg.fakeBits != 16 && g_cfg.fakeBits != 24 && g_cfg.fakeBits != 32) g_cfg.fakeBits = 24;

    wchar_t dev[256] = {};
    GetPrivateProfileStringW(L"Fix", L"FakeDevice", L"Digital Output", dev, 256, ini);
    int n = WideCharToMultiByte(CP_UTF8, 0, dev, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 1 ? (size_t)n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, dev, -1, &s[0], n, nullptr, nullptr);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    g_cfg.fakeDevice = s;
}

static bool MatchesFakeDevice(const std::string& name) {
    if (g_cfg.fakeDevice.empty()) return false;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return lower.find(g_cfg.fakeDevice) != std::string::npos;
}

// Set while the plugin itself reads device properties, so our own reads aren't hooked/logged.
static thread_local bool g_internal = false;
struct InternalScope {
    bool prev;
    InternalScope() : prev(g_internal) { g_internal = true; }
    ~InternalScope() { g_internal = prev; }
};

// ---------------------------------------------------------------- string helpers

static std::string Utf8(const wchar_t* w) {
    if (!w) return "(null)";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::string IidName(REFIID iid) {
    struct Known { const IID* id; const char* name; };
    static const Known known[] = {
        { &__uuidof(IAudioClient),         "IAudioClient" },
        { &__uuidof(IAudioClient2),        "IAudioClient2" },
        { &__uuidof(IAudioClient3),        "IAudioClient3" },
        { &__uuidof(IAudioRenderClient),   "IAudioRenderClient" },
        { &__uuidof(IAudioClock),          "IAudioClock" },
        { &__uuidof(ISimpleAudioVolume),   "ISimpleAudioVolume" },
        { &__uuidof(IAudioStreamVolume),   "IAudioStreamVolume" },
        { &__uuidof(IChannelAudioVolume),  "IChannelAudioVolume" },
        { &__uuidof(IAudioSessionControl), "IAudioSessionControl" },
        { &__uuidof(IAudioSessionManager), "IAudioSessionManager" },
        { &__uuidof(IAudioEndpointVolume), "IAudioEndpointVolume" },
    };
    for (auto& k : known) if (IsEqualIID(iid, *k.id)) return k.name;
    wchar_t buf[64] = {};
    StringFromGUID2(iid, buf, 64);
    return Utf8(buf);
}

// PKEY_Device_FriendlyName
static const PROPERTYKEY kFriendlyName =
    {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14};

static std::string DeviceName(IMMDevice* dev) {
    if (!dev) return "?";
    InternalScope guard;
    std::string name;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(kFriendlyName, &pv)) && pv.vt == VT_LPWSTR) name = Utf8(pv.pwszVal);
        PropVariantClear(&pv);
        ps->Release();
    }
    if (name.empty()) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) { name = Utf8(id); CoTaskMemFree(id); }
    }
    return name.empty() ? "?" : name;
}

// ---------------------------------------------------------------- format helpers

static const GUID kSubPcm   = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID kSubFloat = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

static void Describe(const WAVEFORMATEX* f, char* buf, size_t n) {
    if (!f) { snprintf(buf, n, "(null)"); return; }
    const char* kind = "tag?";
    unsigned mask = 0, valid = f->wBitsPerSample;
    if (f->wFormatTag == WAVE_FORMAT_PCM)             kind = "PCM";
    else if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) kind = "FLOAT";
    else if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        kind  = IsEqualGUID(x->SubFormat, kSubFloat) ? "EXT/FLOAT"
              : IsEqualGUID(x->SubFormat, kSubPcm)   ? "EXT/PCM" : "EXT/other";
        mask  = x->dwChannelMask;
        valid = x->Samples.wValidBitsPerSample;
    }
    snprintf(buf, n, "%s tag=0x%04X ch=%u rate=%lu bits=%u valid=%u mask=0x%X align=%u",
             kind, f->wFormatTag, f->nChannels, f->nSamplesPerSec,
             f->wBitsPerSample, valid, mask, f->nBlockAlign);
}

static bool IsPlainPcmOrFloat(const WAVEFORMATEX* f) {
    if (!f || f->nChannels < 1 || f->nChannels > 8) return false;
    if (f->wFormatTag == WAVE_FORMAT_PCM || f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        return IsEqualGUID(x->SubFormat, kSubPcm) || IsEqualGUID(x->SubFormat, kSubFloat);
    }
    return false;
}

static DWORD MaskFor(UINT ch) {
    switch (ch) {
    case 1:  return SPEAKER_FRONT_CENTER;
    case 2:  return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 4:  return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 6:  return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                    SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    case 8:  return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                    SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                    SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    default: return 0;
    }
}

struct StreamFmt { WORD ch = 0, bits = 0, align = 0; bool isFloat = false; DWORD rate = 0; };

static StreamFmt FmtFrom(const WAVEFORMATEX* f) {
    StreamFmt s;
    if (!f) return s;
    s.ch = f->nChannels; s.bits = f->wBitsPerSample; s.align = f->nBlockAlign; s.rate = f->nSamplesPerSec;
    s.isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
        s.isFloat = IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat, kSubFloat);
    return s;
}

static float Peak(const BYTE* b, UINT32 frames, const StreamFmt& f) {
    size_t n = (size_t)frames * f.ch;
    float pk = 0.f;
    if (f.isFloat && f.bits == 32) {
        auto* p = reinterpret_cast<const float*>(b);
        for (size_t i = 0; i < n; ++i) { float v = fabsf(p[i]); if (v > pk) pk = v; }
    } else if (f.bits == 16) {
        auto* p = reinterpret_cast<const int16_t*>(b);
        for (size_t i = 0; i < n; ++i) { float v = fabsf(p[i] / 32768.f); if (v > pk) pk = v; }
    } else if (f.bits == 32) {
        auto* p = reinterpret_cast<const int32_t*>(b);
        for (size_t i = 0; i < n; ++i) { float v = fabsf(p[i] / 2147483648.f); if (v > pk) pk = v; }
    } else if (f.bits == 24) {
        for (size_t i = 0; i < n; ++i) {
            int32_t v = b[3*i] | (b[3*i+1] << 8) | ((int8_t)b[3*i+2] << 16);
            float a = fabsf(v / 8388608.f); if (a > pk) pk = a;
        }
    }
    return pk;
}

// ---------------------------------------------------------------- tracked state

struct ClientInfo  { std::string device = "?"; StreamFmt fmt; };
struct RenderStats {
    int id = 0; std::string device; StreamFmt fmt;
    BYTE* buf = nullptr;
    uint64_t frames = 0, silent = 0, calls = 0, zeroBufs = 0;
    float peak = 0.f; DWORD start = 0; int lines = 0; int getBufFails = 0;
};

static std::mutex g_stMtx;   // lock order: g_stMtx before g_logMtx
static std::unordered_map<void*, ClientInfo>  g_clients;
static std::unordered_map<void*, RenderStats> g_render;
static int g_nextStream = 1;

static std::string ClientDevice(void* c) {
    std::lock_guard<std::mutex> lk(g_stMtx);
    auto it = g_clients.find(c);
    return it != g_clients.end() ? it->second.device : "?";
}

// ---------------------------------------------------------------- device property store

static const PROPERTYKEY kDeviceFormat      = {{0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}}, 0};
static const PROPERTYKEY kOEMFormat         = {{0xe4870e26,0x3cc5,0x4cd2,{0xba,0x46,0xca,0x0a,0x9a,0x70,0xed,0x04}}, 3};
static const GUID        kEndpointFmtid     =  {0x1da5d803,0xd492,0x4edd,{0x8c,0x23,0xe0,0xc0,0xff,0xee,0x7f,0x0e}};
static const PROPERTYKEY kFormFactor        = {kEndpointFmtid, 0};
static const PROPERTYKEY kPhysicalSpeakers  = {kEndpointFmtid, 3};
static const PROPERTYKEY kFullRangeSpeakers = {kEndpointFmtid, 6};
static const PROPERTYKEY kEventDriven       = {kEndpointFmtid, 7};

static bool KeyEq(const PROPERTYKEY& a, const PROPERTYKEY& b) {
    return IsEqualGUID(a.fmtid, b.fmtid) && a.pid == b.pid;
}

static std::string KeyName(const PROPERTYKEY& k) {
    if (KeyEq(k, kDeviceFormat))      return "AudioEngine_DeviceFormat";
    if (KeyEq(k, kOEMFormat))         return "AudioEngine_OEMFormat";
    if (KeyEq(k, kFormFactor))        return "AudioEndpoint_FormFactor";
    if (KeyEq(k, kPhysicalSpeakers))  return "AudioEndpoint_PhysicalSpeakers";
    if (KeyEq(k, kFullRangeSpeakers)) return "AudioEndpoint_FullRangeSpeakers";
    if (KeyEq(k, kEventDriven))       return "AudioEndpoint_Supports_EventDriven_Mode";
    if (KeyEq(k, kFriendlyName))      return "Device_FriendlyName";
    wchar_t g[64] = {};
    StringFromGUID2(k.fmtid, g, 64);
    char buf[96];
    snprintf(buf, sizeof buf, "%s,%lu", Utf8(g).c_str(), k.pid);
    return buf;
}

static std::string DescribeProp(const PROPVARIANT* pv) {
    char buf[224];
    switch (pv->vt) {
    case VT_EMPTY: return "(empty)";
    case VT_UI4:   snprintf(buf, sizeof buf, "UI4 0x%lX", pv->ulVal); return buf;
    case VT_BOOL:  return pv->boolVal ? "TRUE" : "FALSE";
    case VT_LPWSTR: return "'" + Utf8(pv->pwszVal) + "'";
    case VT_BLOB:
        if (pv->blob.cbSize >= sizeof(WAVEFORMATEX) && pv->blob.pBlobData) {
            Describe(reinterpret_cast<const WAVEFORMATEX*>(pv->blob.pBlobData), buf, sizeof buf);
            return std::string("format ") + buf;
        }
        snprintf(buf, sizeof buf, "BLOB %lu bytes", pv->blob.cbSize); return buf;
    default: snprintf(buf, sizeof buf, "vt=%u", pv->vt); return buf;
    }
}

struct StoreInfo { std::string device; bool fake = false; };
static std::unordered_map<void*, StoreInfo> g_stores;   // guarded by g_stMtx
static std::atomic<int> g_propLogs{0};

// Replace a WAVEFORMATEX blob with plain PCM in the configured channels/bits.
static void FakeFormatBlob(PROPVARIANT* pv) {
    DWORD rate = 48000;
    if (pv->vt == VT_BLOB && pv->blob.cbSize >= sizeof(WAVEFORMATEX) && pv->blob.pBlobData) {
        DWORD r = reinterpret_cast<const WAVEFORMATEX*>(pv->blob.pBlobData)->nSamplesPerSec;
        if (r >= 8000 && r <= 384000) rate = r;
    }
    auto* x = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!x) return;
    ZeroMemory(x, sizeof *x);
    x->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    x->Format.nChannels       = (WORD)g_cfg.fakeChannels;
    x->Format.nSamplesPerSec  = rate;
    x->Format.wBitsPerSample  = (WORD)g_cfg.fakeBits;
    x->Format.nBlockAlign     = (WORD)(g_cfg.fakeChannels * g_cfg.fakeBits / 8);
    x->Format.nAvgBytesPerSec = rate * x->Format.nBlockAlign;
    x->Format.cbSize          = 22;
    x->Samples.wValidBitsPerSample = (WORD)g_cfg.fakeBits;
    x->dwChannelMask          = MaskFor(g_cfg.fakeChannels);
    x->SubFormat              = kSubPcm;
    PropVariantClear(pv);
    pv->vt             = VT_BLOB;
    pv->blob.cbSize    = sizeof(WAVEFORMATEXTENSIBLE);
    pv->blob.pBlobData = reinterpret_cast<BYTE*>(x);
}

// ---------------------------------------------------------------- hook types / originals

using CoCreateInstance_t    = HRESULT (WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using GetDefaultEndpoint_t  = HRESULT (STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, ERole, IMMDevice**);
using Activate_t            = HRESULT (STDMETHODCALLTYPE*)(IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**);
using OpenPropertyStore_t   = HRESULT (STDMETHODCALLTYPE*)(IMMDevice*, DWORD, IPropertyStore**);
using GetValue_t            = HRESULT (STDMETHODCALLTYPE*)(IPropertyStore*, REFPROPERTYKEY, PROPVARIANT*);
using Initialize_t          = HRESULT (STDMETHODCALLTYPE*)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD,
                                                           REFERENCE_TIME, REFERENCE_TIME,
                                                           const WAVEFORMATEX*, LPCGUID);
using IsFormatSupported_t   = HRESULT (STDMETHODCALLTYPE*)(IAudioClient*, AUDCLNT_SHAREMODE,
                                                           const WAVEFORMATEX*, WAVEFORMATEX**);
using GetMixFormat_t        = HRESULT (STDMETHODCALLTYPE*)(IAudioClient*, WAVEFORMATEX**);
using Start_t               = HRESULT (STDMETHODCALLTYPE*)(IAudioClient*);
using GetService_t          = HRESULT (STDMETHODCALLTYPE*)(IAudioClient*, REFIID, void**);
using SetClientProperties_t = HRESULT (STDMETHODCALLTYPE*)(IAudioClient2*, const AudioClientProperties*);
using GetBuffer_t           = HRESULT (STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, BYTE**);
using ReleaseBuffer_t       = HRESULT (STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, DWORD);

static CoCreateInstance_t    oCoCreateInstance    = nullptr;
static GetDefaultEndpoint_t  oGetDefaultEndpoint  = nullptr;
static Activate_t            oActivate            = nullptr;
static OpenPropertyStore_t   oOpenPropertyStore   = nullptr;
static GetValue_t            oGetValue            = nullptr;
static Initialize_t          oInitialize          = nullptr;
static IsFormatSupported_t   oIsFormatSupported   = nullptr;
static GetMixFormat_t        oGetMixFormat        = nullptr;
static Start_t               oStart               = nullptr;
static GetService_t          oGetService          = nullptr;
static SetClientProperties_t oSetClientProperties = nullptr;
static GetBuffer_t           oGetBuffer           = nullptr;
static ReleaseBuffer_t       oReleaseBuffer       = nullptr;

static std::mutex        g_hookMtx;
static std::atomic<bool> g_hooksDone{false};
static thread_local bool g_inProbe = false;

static const char* FlowName(EDataFlow f) { return f == eRender ? "render" : f == eCapture ? "capture" : "all"; }
static const char* RoleName(ERole r)     { return r == eConsole ? "console" : r == eMultimedia ? "multimedia" : "communications"; }
static const char* ModeName(AUDCLNT_SHAREMODE m) { return m == AUDCLNT_SHAREMODE_SHARED ? "shared" : "exclusive"; }

// ---------------------------------------------------------------- device hooks

static HRESULT STDMETHODCALLTYPE hkGetDefaultEndpoint(IMMDeviceEnumerator* self, EDataFlow flow,
                                                      ERole role, IMMDevice** pp) {
    HRESULT hr = oGetDefaultEndpoint(self, flow, role, pp);
    std::string name = SUCCEEDED(hr) && pp && *pp ? DeviceName(*pp) : "";
    Log("GetDefaultAudioEndpoint(%s, %s) -> 0x%08lX '%s'", FlowName(flow), RoleName(role), hr, name.c_str());
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkActivate(IMMDevice* self, REFIID iid, DWORD ctx,
                                            PROPVARIANT* params, void** pp) {
    HRESULT hr = oActivate(self, iid, ctx, params, pp);
    std::string name = DeviceName(self);
    void* obj = SUCCEEDED(hr) && pp ? *pp : nullptr;
    Log("Activate(%s) on '%s' -> 0x%08lX obj=%p", IidName(iid).c_str(), name.c_str(), hr, obj);
    if (obj && (IsEqualIID(iid, __uuidof(IAudioClient)) || IsEqualIID(iid, __uuidof(IAudioClient2)) ||
                IsEqualIID(iid, __uuidof(IAudioClient3)))) {
        std::lock_guard<std::mutex> lk(g_stMtx);
        g_clients[obj] = ClientInfo{ name, {} };
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkOpenPropertyStore(IMMDevice* self, DWORD access, IPropertyStore** pp) {
    HRESULT hr = oOpenPropertyStore(self, access, pp);
    if (g_internal || FAILED(hr) || !pp || !*pp) return hr;
    std::string name = DeviceName(self);
    StoreInfo si{ name, MatchesFakeDevice(name) };
    std::lock_guard<std::mutex> lk(g_stMtx);
    g_stores[*pp] = si;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkGetValue(IPropertyStore* self, REFPROPERTYKEY key, PROPVARIANT* pv) {
    HRESULT hr = oGetValue(self, key, pv);
    if (g_internal || !pv) return hr;

    StoreInfo si;
    {
        std::lock_guard<std::mutex> lk(g_stMtx);
        auto it = g_stores.find(self);
        if (it == g_stores.end()) return hr;   // not an audio endpoint store we know of
        si = it->second;
    }

    std::string before = SUCCEEDED(hr) ? DescribeProp(pv) : "";
    bool faked = false;
    if (SUCCEEDED(hr) && si.fake) {
        if (KeyEq(key, kDeviceFormat) || KeyEq(key, kOEMFormat)) {
            FakeFormatBlob(pv);
            faked = true;
        } else if ((KeyEq(key, kPhysicalSpeakers) || KeyEq(key, kFullRangeSpeakers)) &&
                   (pv->vt == VT_UI4 || pv->vt == VT_EMPTY)) {
            pv->vt    = VT_UI4;
            pv->ulVal = MaskFor(g_cfg.fakeChannels);
            faked = true;
        }
    }

    if (g_cfg.logProps && g_propLogs++ < 400) {
        if (faked)
            Log("GetValue '%s' %s = %s  -> faked: %s", si.device.c_str(), KeyName(key).c_str(),
                before.c_str(), DescribeProp(pv).c_str());
        else
            Log("GetValue '%s' %s -> 0x%08lX %s", si.device.c_str(), KeyName(key).c_str(), hr, before.c_str());
    }
    return hr;
}

// ---------------------------------------------------------------- IAudioClient hooks

static HRESULT STDMETHODCALLTYPE hkGetMixFormat(IAudioClient* self, WAVEFORMATEX** pp) {
    HRESULT hr = oGetMixFormat(self, pp);
    char d[192]; Describe(SUCCEEDED(hr) && pp ? *pp : nullptr, d, sizeof d);
    Log("GetMixFormat client=%p '%s' -> 0x%08lX  %s", self, ClientDevice(self).c_str(), hr, d);

    if (SUCCEEDED(hr) && pp && g_cfg.forceMix) {
        auto* x = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
        if (x) {
            ZeroMemory(x, sizeof *x);
            x->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
            x->Format.nChannels       = (WORD)g_cfg.forceChannels;
            x->Format.nSamplesPerSec  = g_cfg.forceRate;
            x->Format.wBitsPerSample  = 32;
            x->Format.nBlockAlign     = (WORD)(x->Format.nChannels * 4);
            x->Format.nAvgBytesPerSec = x->Format.nSamplesPerSec * x->Format.nBlockAlign;
            x->Format.cbSize          = 22;
            x->Samples.wValidBitsPerSample = 32;
            x->dwChannelMask          = MaskFor(g_cfg.forceChannels);
            x->SubFormat              = kSubFloat;
            CoTaskMemFree(*pp);
            *pp = &x->Format;
            Describe(*pp, d, sizeof d);
            Log("  -> reporting forced mix format: %s", d);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkIsFormatSupported(IAudioClient* self, AUDCLNT_SHAREMODE mode,
                                                     const WAVEFORMATEX* fmt, WAVEFORMATEX** closest) {
    HRESULT hr = oIsFormatSupported(self, mode, fmt, closest);
    char d[192]; Describe(fmt, d, sizeof d);
    Log("IsFormatSupported(%s) client=%p %s -> 0x%08lX", ModeName(mode), self, d, hr);

    if (hr != S_OK && mode == AUDCLNT_SHAREMODE_SHARED && g_cfg.autoConvert && IsPlainPcmOrFloat(fmt)) {
        if (closest && *closest) { CoTaskMemFree(*closest); *closest = nullptr; }
        Log("  -> overriding to S_OK (autoconvert will handle it)");
        hr = S_OK;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkInitialize(IAudioClient* self, AUDCLNT_SHAREMODE mode, DWORD flags,
                                              REFERENCE_TIME bufDur, REFERENCE_TIME period,
                                              const WAVEFORMATEX* fmt, LPCGUID session) {
    DWORD newFlags = flags;
    if (mode == AUDCLNT_SHAREMODE_SHARED && g_cfg.autoConvert)
        newFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    HRESULT hr = oInitialize(self, mode, newFlags, bufDur, period, fmt, session);

    std::string dev;
    {
        std::lock_guard<std::mutex> lk(g_stMtx);
        ClientInfo& ci = g_clients[self];
        if (SUCCEEDED(hr)) ci.fmt = FmtFrom(fmt);
        dev = ci.device;
    }
    char d[192]; Describe(fmt, d, sizeof d);
    Log("Initialize(%s) client=%p '%s' flags 0x%08lX->0x%08lX buf=%lld period=%lld %s -> 0x%08lX",
        ModeName(mode), self, dev.c_str(), flags, newFlags, (long long)bufDur, (long long)period, d, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkStart(IAudioClient* self) {
    HRESULT hr = oStart(self);
    Log("Start client=%p '%s' -> 0x%08lX", self, ClientDevice(self).c_str(), hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkGetService(IAudioClient* self, REFIID riid, void** pp) {
    HRESULT hr = oGetService(self, riid, pp);
    void* obj = SUCCEEDED(hr) && pp ? *pp : nullptr;
    std::string dev = "?";
    int id = 0;
    {
        std::lock_guard<std::mutex> lk(g_stMtx);
        auto it = g_clients.find(self);
        if (it != g_clients.end()) dev = it->second.device;
        if (obj && IsEqualIID(riid, __uuidof(IAudioRenderClient))) {
            RenderStats rs;
            rs.id     = id = g_nextStream++;
            rs.device = dev;
            if (it != g_clients.end()) rs.fmt = it->second.fmt;
            g_render[obj] = rs;
        }
    }
    if (id) Log("GetService(IAudioRenderClient) client=%p '%s' -> 0x%08lX  => stream #%d", self, dev.c_str(), hr, id);
    else    Log("GetService(%s) client=%p '%s' -> 0x%08lX", IidName(riid).c_str(), self, dev.c_str(), hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkSetClientProperties(IAudioClient2* self, const AudioClientProperties* props) {
    if (!props) return oSetClientProperties(self, props);

    AudioClientProperties p{};
    UINT32 sz = props->cbSize < sizeof p ? props->cbSize : (UINT32)sizeof p;
    memcpy(&p, props, sz);
    p.cbSize = sz;
    bool hasOptions = sz >= offsetof(AudioClientProperties, Options) + sizeof(p.Options);

    Log("SetClientProperties client=%p '%s' cbSize=%u offload=%d category=%d options=0x%X",
        self, ClientDevice(self).c_str(), props->cbSize, p.bIsOffload, (int)p.eCategory,
        hasOptions ? (unsigned)p.Options : 0u);

    if (g_cfg.stripRaw) {
        bool changed = false;
        if (p.bIsOffload) { p.bIsOffload = FALSE; changed = true; }
        if (hasOptions && (p.Options & AUDCLNT_STREAMOPTIONS_RAW)) {
            p.Options = (AUDCLNT_STREAMOPTIONS)(p.Options & ~AUDCLNT_STREAMOPTIONS_RAW);
            changed = true;
        }
        if (changed) {
            HRESULT hr = oSetClientProperties(self, &p);
            Log("  -> stripped RAW/offload so the stream goes through driver effects (DDL/DTS): 0x%08lX", hr);
            return hr;
        }
    }
    return oSetClientProperties(self, props);
}

// ---------------------------------------------------------------- IAudioRenderClient hooks

static HRESULT STDMETHODCALLTYPE hkGetBuffer(IAudioRenderClient* self, UINT32 frames, BYTE** ppData) {
    HRESULT hr = oGetBuffer(self, frames, ppData);
    if (g_cfg.renderStats) {
        std::lock_guard<std::mutex> lk(g_stMtx);
        auto it = g_render.find(self);
        if (it != g_render.end()) {
            RenderStats& s = it->second;
            if (SUCCEEDED(hr) && ppData) s.buf = *ppData;
            else if (s.getBufFails++ < 5) Log("stream #%d GetBuffer(%u) failed: 0x%08lX", s.id, frames, hr);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkReleaseBuffer(IAudioRenderClient* self, UINT32 frames, DWORD flags) {
    if (g_cfg.renderStats) {
        std::lock_guard<std::mutex> lk(g_stMtx);
        auto it = g_render.find(self);
        if (it != g_render.end()) {
            RenderStats& s = it->second;
            DWORD now = GetTickCount();
            if (!s.start) s.start = now;
            s.calls++;
            s.frames += frames;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                s.silent += frames;
            } else if (s.buf && frames && s.fmt.ch) {
                float pk = Peak(s.buf, frames, s.fmt);
                if (pk == 0.f) s.zeroBufs++;
                if (pk > s.peak) s.peak = pk;
            }
            s.buf = nullptr;

            DWORD elapsed = now - s.start;
            if (elapsed >= 1000) {
                if (s.lines < 60) {
                    s.lines++;
                    char pk[32];
                    if (s.peak > 0.f) snprintf(pk, sizeof pk, "%.1f dBFS", 20.f * log10f(s.peak));
                    else              snprintf(pk, sizeof pk, "-inf (digital silence)");
                    Log("stream #%d '%s' %uch: %llu frames in %lu ms (%llu writes), silent-flag frames %llu, "
                        "all-zero writes %llu, peak %s",
                        s.id, s.device.c_str(), s.fmt.ch, (unsigned long long)s.frames, elapsed,
                        (unsigned long long)s.calls, (unsigned long long)s.silent,
                        (unsigned long long)s.zeroBufs, pk);
                }
                s.start = now; s.frames = s.silent = s.calls = s.zeroBufs = 0; s.peak = 0.f;
            }
        }
    }
    return oReleaseBuffer(self, frames, flags);
}

// ---------------------------------------------------------------- installation

template <class T>
static void Hook(void* target, T detour, T* original, const char* name) {
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    if (s != MH_OK) Log("  hook %s at %p failed: %s", name, target, MH_StatusToString(s));
}

static void InstallAudioHooks() {
    std::lock_guard<std::mutex> lk(g_hookMtx);   // other threads wait here until hooks are live
    if (g_hooksDone) return;
    g_inProbe = true;

    IMMDeviceEnumerator* en  = nullptr;
    IMMDevice*           dev = nullptr;
    IAudioClient*        ac  = nullptr;
    IAudioClient2*       ac2 = nullptr;
    IAudioRenderClient*  rc  = nullptr;
    IPropertyStore*      ps  = nullptr;
    WAVEFORMATEX*        mix = nullptr;

    HRESULT hr = oCoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) {
        Log("Default render device: '%s'", DeviceName(dev).c_str());
        if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps))) ps = nullptr;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac));
    }
    if (SUCCEEDED(hr)) {
        // Open a short-lived probe stream so we can reach IAudioRenderClient's code.
        HRESULT h2 = ac->GetMixFormat(&mix);
        if (SUCCEEDED(h2)) {
            char d[192]; Describe(mix, d, sizeof d);
            Log("Default device mix format: %s", d);
            h2 = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, mix, nullptr);
        }
        if (SUCCEEDED(h2)) h2 = ac->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rc));
        if (FAILED(h2)) Log("Probe stream failed (0x%08lX); per-stream audio stats unavailable", h2);
        ac->QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void**>(&ac2));
    }

    if (SUCCEEDED(hr)) {
        void** vEn  = *reinterpret_cast<void***>(en);
        void** vDev = *reinterpret_cast<void***>(dev);
        void** vAc  = *reinterpret_cast<void***>(ac);
        Hook(vEn[4],  &hkGetDefaultEndpoint, &oGetDefaultEndpoint, "GetDefaultAudioEndpoint");
        Hook(vDev[3], &hkActivate,           &oActivate,           "IMMDevice::Activate");
        Hook(vDev[4], &hkOpenPropertyStore,  &oOpenPropertyStore,  "IMMDevice::OpenPropertyStore");
        if (ps) {
            void** vPs = *reinterpret_cast<void***>(ps);
            Hook(vPs[5], &hkGetValue, &oGetValue, "IPropertyStore::GetValue");
        }
        Hook(vAc[3],  &hkInitialize,         &oInitialize,         "Initialize");
        Hook(vAc[7],  &hkIsFormatSupported,  &oIsFormatSupported,  "IsFormatSupported");
        Hook(vAc[8],  &hkGetMixFormat,       &oGetMixFormat,       "GetMixFormat");
        Hook(vAc[10], &hkStart,              &oStart,              "Start");
        Hook(vAc[14], &hkGetService,         &oGetService,         "GetService");
        if (ac2) {
            void** vAc2 = *reinterpret_cast<void***>(ac2);
            Hook(vAc2[16], &hkSetClientProperties, &oSetClientProperties, "SetClientProperties");
        }
        if (rc) {
            void** vRc = *reinterpret_cast<void***>(rc);
            Hook(vRc[3], &hkGetBuffer,     &oGetBuffer,     "GetBuffer");
            Hook(vRc[4], &hkReleaseBuffer, &oReleaseBuffer, "ReleaseBuffer");
        }
        MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
        Log("Audio hooks enabled: %s", MH_StatusToString(s));
        g_hooksDone = true;
    } else {
        Log("Could not open default render device (hr=0x%08lX); will retry on next enumerator", hr);
    }

    if (rc)  rc->Release();
    if (ps)  ps->Release();
    if (ac2) ac2->Release();
    if (ac)  ac->Release();
    if (dev) dev->Release();
    if (en)  en->Release();
    if (mix) CoTaskMemFree(mix);
    g_inProbe = false;
}

static HRESULT WINAPI hkCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* out) {
    HRESULT hr = oCoCreateInstance(clsid, outer, ctx, iid, out);
    if (!g_inProbe && !g_hooksDone && SUCCEEDED(hr) &&
        IsEqualCLSID(clsid, __uuidof(MMDeviceEnumerator))) {
        Log("MMDeviceEnumerator created by game; installing audio hooks");
        InstallAudioHooks();
    }
    return hr;
}

// ---------------------------------------------------------------- entry

static void Startup() {
    wchar_t logPath[MAX_PATH];
    PathNextToSelf(logPath, MAX_PATH, L".log");
    _wfopen_s(&g_log, logPath, L"w");
    LoadConfig();
    Log("WDAudioFix v3 loaded. AutoConvert=%d ForceMixFormat=%d (%u ch @ %u Hz) StripRawMode=%d RenderStats=%d",
        g_cfg.autoConvert, g_cfg.forceMix, g_cfg.forceChannels, g_cfg.forceRate,
        g_cfg.stripRaw, g_cfg.renderStats);
    Log("FakeDevice='%s' -> %u ch %u-bit PCM", g_cfg.fakeDevice.c_str(), g_cfg.fakeChannels, g_cfg.fakeBits);

    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return; }

    // combase is where CoCreateInstance actually lives on Win8+; ole32 forwards to it.
    const wchar_t* mods[] = { L"combase.dll", L"ole32.dll" };
    for (auto m : mods) {
        if (!GetModuleHandleW(m)) LoadLibraryW(m);
        void* target = nullptr;
        if (MH_CreateHookApiEx(m, "CoCreateInstance", reinterpret_cast<void*>(&hkCoCreateInstance),
                               reinterpret_cast<void**>(&oCoCreateInstance), &target) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            Log("Hooked CoCreateInstance in %ls", m);
            return;
        }
    }
    Log("Failed to hook CoCreateInstance");
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        Startup();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
