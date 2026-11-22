// bms-dlss, krok 2: proxy d3d9.dll pro Black Mesa (volba "Hrat DXVK").
//
// Lezi v bin\thirdparty\dxvk-windows-x86 misto puvodniho d3d9.dll (DXVK),
// ktery je prejmenovany na d3d9_dxvk.dll. Vsechna volani predava DXVK a navic:
//   * Ctrl+F10  - zachyti [capture] Frames po sobe jdoucich snimku (vychozi 3), kazdy do bms_dlss_capture_NNN.txt,
//                 a bytecode kazdeho VS ze zachytu do bms_dlss_vs_<ptr>.bin (tools\disasm_vs.ps1 -> .asm).
//                 [capture] AutoAfterSec > 0: zachyt se spusti sam jednou tolik sekund po prvnim Present.
//   * Ctrl+F11  - zapne/vypne subpixelovy jitter kamery (Halton 2,3); v rezimu DLAA misto toho
//                 cykli znamenko jitteru predavaneho DLSS (ladeni).
//   * Ctrl+F8   - most k 64bitovemu helperu: vypnuto <-> DLAA (s [bridge] DebugCycle=1 i ladici
//                 rezimy kopie, zrcadleni a ladeni MV). DLAA a ladeni MV si jitter vynuti (Scale 1).
//   * Ctrl+F9   - preset DLSS: vychozi -> K -> L -> M -> vychozi.
//   * Ctrl+Home - doostreni po DLSS: 0 -> 0.2 -> ... -> 1.0 -> 0.
//   * Ctrl+Del  - mip LOD bias pro scenu v rezimu DLAA zapnuto/vypnuto ([dlaa] MipBias).
//   * Alt+Ins   - vstup DLSS: LDR -> linearni HDR -> linearni HDR + auto expozice -> linearni HDR + inverzni
//                 tone mapping ([dlaa] InputMode, ITMStrength).
//   * Alt+F5    - hloubka pro DLSS: puvodni -> min 3x3 -> konstantni 1.0 (test) ([dlaa] DepthMode).
//   * Alt+F3    - vystup DLSS (supersampling misto DLDSR): 1.0 -> 4/3 -> 1.5 -> 1.724 -> 2.0 -> 1.0
//                 ([upscale] OutputScale; rezimy Quality, Quality, Balanced, Performance).
//                 Helper DLSS vytvori vystup W*S x H*S a zmensi ho zpet; pocet fazi jitteru (Phases=0) a volitelne
//                 mip bias ([upscale] MipBiasMode=1) se ridi S potvrzenym helperem.
//   * Alt+F2    - pocet fazi jitteru: auto 8*S^2 -> 8 -> 12 -> 16 -> 32 -> auto ([jitter] Phases, reset historie DLSS).
//   * Alt+F1    - filtr zmenseni pri S > 1: Lanczos-2 -> Catmull-Rom -> Mitchell -> box ([upscale] DownsampleFilter).
//   * Alt+Backspace - motion vektory pohyblivych objektu zapnuto/vypnuto ([mv] ObjectMV; src\objmv.inl).
//   * Alt+F12   - adaptivni obdoba Reflexu: adaptivni -> jen mereni -> vypnuto ([latency] Mode; src\latency.inl).
//   * Alt+N     - menu nastaveni primo ve hre ([keys] MenuKey, [menu]; src\menu.inl, docs\navrh-menu.md). Hacek
//                 klavesnice za otevreneho menu stisky hre polyka a polyka i klavesy modu vyse (hra je nedostane).
//   (modifikator je [keys] Modifier, vychozi Alt)
// Cast B (od BMSD_VERSION 10): skutecny upscaling. S mat_viewportscale < 1 hra kresli scenu do leveho horniho rohu
// plnych cilu a nakonec ji jednim draw roztahne pres cely obraz (dev/upscale). Proxy render pozna z viewportu draw
// kamery, jitter prepocita podle viewportu kazdeho draw, most spusti az na draw roztazeni (vstup = sub-rect sceny po
// post-processingu, vystup DLSS W x H do cele sceny) a ten draw potlaci. Kdyz most v casti B bezet nemuze, nedela
// proxy nic a roztahuje engine. V casti B Alt+Del cykli mip bias log2(r) - 1 -> [dlaa] MipBias -> log2(r) -> vypnuto,
// Alt+F2 "auto" znamena 8 * (vystup / vstup)^2 fazi; Alt+F3, Alt+F5 a zrcadleni se v ni nepouzivaji.
// Reset/ResetEx zarizeni: most pred nim dobehne a pusti povrchy hry, po nem nekolik snimku vynecha (BeforeDeviceReset).
//
// Jitter se pridava az tesne pred kazdym draw callem:
//   * jen u shaderu, ktere podle sve tabulky konstant (CTAB) pouzivaji
//     cViewProj, cModelViewProj, camViewProj nebo cProj,
//   * jen kdyz render target 0 ma velikost backbufferu (hlavni pohled, ne stiny
//     ani odrazy),
//   * jen u perspektivnich matic (HUD je ortograficky).
// Proxy si drzi "stinovou" kopii registru s puvodnimi hodnotami od hry a do GPU
// nahrava jitterovanou kopii, takze nevadi, ze hra prepisuje matice po castech.
//
// Preklad: build.ps1 (MSVC, x86).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d9.h>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <vector>

#include <intrin.h>  // _ReturnAddress (spike S2: kdo vola Present)

#include "bmsd_shared.h"
#include "interop.h"
#include "present.h"  // spike S2 ([present] HelperPresent, vychozi vypnuto)
#include "res/version.h"

static const char kMarker[] = "BMS_DLSS_PROXY_MARKER_v1";

// ---------------------------------------------------------------------------
// Predavani exportu do skutecneho DXVK
// ---------------------------------------------------------------------------
static const char* kNames[17] = {
    "D3DPERF_BeginEvent", "D3DPERF_EndEvent", "D3DPERF_GetStatus",
    "D3DPERF_QueryRepeatFrame", "D3DPERF_SetMarker", "D3DPERF_SetOptions",
    "D3DPERF_SetRegion", "DebugSetLevel", "DebugSetMute",
    "Direct3D9EnableMaximizedWindowedModeShim", "Direct3DCreate9",
    "Direct3DCreate9Ex", "Direct3DCreate9On12", "Direct3DCreate9On12Ex",
    "Direct3DShaderValidatorCreate9", "PSGPError", "PSGPSampleTexture"};

extern "C" FARPROC g_fn[17] = {};

#define STUB(i, name) \
    extern "C" __declspec(naked) void Stub_##name() { __asm { jmp dword ptr [g_fn + i * 4] } }
STUB(0, D3DPERF_BeginEvent)
STUB(1, D3DPERF_EndEvent)
STUB(2, D3DPERF_GetStatus)
STUB(3, D3DPERF_QueryRepeatFrame)
STUB(4, D3DPERF_SetMarker)
STUB(5, D3DPERF_SetOptions)
STUB(6, D3DPERF_SetRegion)
STUB(7, DebugSetLevel)
STUB(8, DebugSetMute)
STUB(9, Direct3D9EnableMaximizedWindowedModeShim)
STUB(12, Direct3DCreate9On12)
STUB(13, Direct3DCreate9On12Ex)
STUB(14, Direct3DShaderValidatorCreate9)
STUB(15, PSGPError)
STUB(16, PSGPSampleTexture)

// ---------------------------------------------------------------------------
// Log, konfigurace
// ---------------------------------------------------------------------------
static HMODULE g_real;
static wchar_t g_dir[MAX_PATH];
static std::mutex g_logMx;
static FILE* g_log;
// DLL_PROCESS_DETACH pri konci procesu (pod zamkem zavadece, ostatni vlakna uz jsou ukoncena - nektere mohlo drzet
// zamek logu): log jen kdyz jde zamek ziskat do ~100 ms, ukladani ini z menu bez opakovani (src/menu.inl).
static bool g_procExiting;

void Log(const char* fmt, ...) {
    std::unique_lock<std::mutex> lk(g_logMx, std::defer_lock);
    if (g_procExiting) {
        for (int i = 0; i < 50 && !lk.try_lock(); ++i) Sleep(2);
        if (!lk.owns_lock()) return;
    } else {
        lk.lock();
    }
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

struct Config {
    int captureKey = VK_F10;
    int jitterKey = VK_F11;
    int bridgeKey = VK_F8;
    int presetKey = VK_F9;  // F7 je ve hre "load quick"
    int sharpenKey = VK_HOME;  // Home ve hre nic nedela (F7/End/Ins/PgUp/PgDn ano)
    int mipBiasKey = VK_DELETE;  // Delete ve hre nic nedela
    int inputModeKey = VK_INSERT;  // vstup DLSS (pozor: Ins hra pouziva, s modifikatorem ho dostane taky)
    int depthModeKey = VK_F5;      // hloubka pro DLSS (F5 ve hre neni prirazena)
    int outScaleKey = VK_F3;       // vystup DLSS / supersampling (Alt+F3)
    int phasesKey = VK_F2;         // pocet fazi jitteru (Alt+F2): auto -> 8 -> 12 -> 16 -> 32 -> auto
    int downsampleKey = VK_F1;     // filtr zmenseni pri S > 1 (Alt+F1): Lanczos-2 -> Catmull-Rom -> Mitchell -> box
    int alphaJitKey = 0x75;        // Alt+F6: casove promenlivy prah alpha testu zap/vyp (pokus, ploty s L/M)
    float alphaJit = 0.0f;         // [dlaa] AlphaJitter: rozkmit prahu ALPHAREF (podil 0..1 z 255), 0 = vypnuto
    int pipelineKey = VK_SCROLL;  // synchronizace mostu pipeline <-> synchronni (Alt+ScrollLock, ve hre nic)
    bool pipeline = true;          // [bridge] Pipeline
    bool requireCtrl = true;
    int modifier = VK_MENU;  // [keys] Modifier: alt (vychozi, ve hre +walk), ctrl (+duck), rctrl, shift, none
    // [keys] DebugKeys (vychozi 0): bez nej plati jen Alt+N (menu) a Alt+F8 (mod zap/vyp); ostatni klavesy modu
    // (Alt+F1..F12, Home, Del, Ins, Backspace, ScrollLock) jsou ladici - nastaveni je v menu. 1 = puvodni klavesy.
    bool debugKeys = false;
    // [keys] TestKeys (vychozi 0, menu Pokrocile -> Zkratky pro testovani, uklada se): Alt+F9 rezim FG dokola,
    // Alt+F10 upscaler DLSS <-> FSR. Za behu meni menu (g_testKeysOn), pri DebugKeys=1 maji prednost pred F9/F10.
    bool testKeys = false;
    // Delka Haltonovy sekvence; 0 = auto 8 * S^2 (8 .. 72, Guide 3.7.1.1). Vychozi 8: na plotech v BMS auto
    // (14/18/24/32 fazi pri S > 1) zhorsovalo preset K, s pevnymi 8 ne (test 2026-11-18).
    int phases = 8;
    unsigned downsampleFilter = 1;  // [upscale] DownsampleFilter (BMSD_DOWN_*), helper ho dostava kazdy snimek
    bool downsampleLinear = false;  // [upscale] DownsampleLinear (cte helper sam, tady jen do logu)
    float scale = 1.0f;      // 1.0 = +-0.5 px; vic jen pro vizualni test
    bool startEnabled = false;
    bool debugCycle = false; // Ctrl+F8 prochazi i ladici rezimy mostu
    unsigned preset = 0;     // preset DLSS po spusteni (0 = vychozi, 11..13 = K..M)
    float sharpness = 0.0f;  // doostreni po DLSS po spusteni (0..1, 0 = vypnuto)
    float mipBias = -0.5f;   // mip LOD bias pro scenu v rezimu DLAA (0 = vypnuto)
    float mipBiasNormal = 0.0f;  // [dlaa] MipBiasNormal: pricist k biasu u sampleru normalovych map (v25d)
    float mipBiasAlpha = 0.0f;   // [dlaa] MipBiasAlphaTest: pricist k biasu u drawu s alfa testem (v25d)
    unsigned inputMode = 1;  // vstup DLSS po spusteni (BMSD_INPUT_*; vychozi 1 = linearni HDR, v menu neni)
    unsigned inputModeL = 0; // [dlaa] InputModeL: vstup pro preset L (vychozi 0 = LDR, u plotu klidnejsi)
    bool camHold = true;     // [dlaa] CamHold: pri skoku kamery dat prednost kandidatovi navazujicimu na minuly snimek
    float itmStrength = 0.95f;  // InputMode 3: sila inverzniho tone mappingu (pouziva helper, tady jen do logu)
    unsigned depthMode = 0;     // hloubka pro DLSS po spusteni (BMSD_DEPTH_*)
    float outScale = 1.0f;      // [upscale] OutputScale: vystup DLSS po spusteni (1 = DLAA)
    int mipBiasMode = 0;        // [upscale] MipBiasMode: 0 = [dlaa] MipBias vuci finalnimu obrazu, 1 = log2(In/Out) - 1
    int capFrames = 3;          // [capture] Frames: po sobe jdoucich snimku na jeden zachyt (1..10)
    int capAutoSec = 0;         // [capture] AutoAfterSec: automaticky zachyt N s po prvnim Present (0 = vypnuto)
    // [mv]: motion vektory pohyblivych objektu (src\objmv.inl)
    bool objMv = true;          // ObjectMV: zapnuto po spusteni
    int objMvKey = VK_BACK;     // [keys] ObjectMVKey: Alt+Backspace (Backspace ve hre neni prirazen) zapina/vypina za behu
    float objMvTol = 0.02f;     // DepthTol: tolerance hloubky v helperu, relativne k (1 - d), tj. ke vzdalenosti
    float objMvTeleport = 64.0f;  // TeleportDist: nejvetsi posun objektu za snimek (jednotky Source), jinak bez paru
    bool objMvValidate = false; // Validate: prehravat i staticke objekty a merit rozdil proti MV kamery (diagnostika)
    // Viewmodel (2026-11-21, experiment, vychozi 0): MV i pro zbran v ruce (viewport Z 0..0.1, vlastni FOV). Bez nej
    // ma zbran MV = 0, takze jeji houpani / dychani DLSS nevidi (neklid jemnych povrchu zbrane).
    bool objMvViewmodel = true;  // [mv] Viewmodel (vychozi 1 od v25b: overeno ViewmodelCheck, chyba 5-9x mensi)
    // [upscale] LightsB (vychozi 1 od 2026-11-18, overeno ve hre): v casti B povolit pruchod dynamickych svetel enginu (zaplata client.dll, LightsPatch*)
    bool lightsB = true;
    // [upscale] LightsBDetect: v casti B logovat shadery s konstantami podle renderu a draw pres cely povrch (DetectPre)
    bool lightsBDetect = false;
    // [upscale] LightsBSunFix: v casti B zare slunce (grfinal_ps30) pres upravenou kopii shaderu (O1); 0 = puvodni bod 4
    bool lightsBSunFix = true;
    // [upscale] Upscaler=auto|dlss|fsr|xess|off (UPS_*, vychozi auto = DLSS, kdyz jde, jinak FSR): cim most dopocitava
    // obraz; off = most po startu vypnuty (prebije StartMode). XeSS se pripravuje (UpscalerAvailable).
    int upscaler = 4;
    // [upscale] SkipGameFxaa: FXAA hry (soucast engine_post_ps30, bezi az po mostu) neutralizovat, kdyz snimek prosel
    // DLSS / FSR (FxaaPre); upscaler vyp / most vypnuty = FXAA hry beze zmeny
    bool skipGameFxaa = true;
    // [upscale] StripMsaa: s aktivnim modem (upscaler != vyp pri vytvoreni prostredku) MSAA hry odebrat (backbuffer,
    // RT a hloubka bez vzorku) - s MSAA most kazdy snimek vynecha
    bool stripMsaa = true;
    // [upscale] ReactiveMask: maska reaktivity z pruhlednych a aditivnich draw sceny (src/reactive.inl) pro DLSS / FSR;
    // ReactiveScale a ReactiveMax cte helper
    bool reactiveMask = false;
    bool reactiveGlass = false;  // [upscale] ReactiveGlass: do masky i pruhledne draw bez castic (sklo)
} g_cfg;

// Vystup DLSS (supersampling, Alt+F3). Povolene hodnoty S; Alt+F3 cykli kScaleCycle (3.0 = Ultra Performance jen
// z ini). 1.724 = 1 / 0.58 = pomer Balanced (jako DLDSR + DLSS Balanced v jinych hrach).
static const float kScaleAllowed[] = {1.0f, 4.0f / 3.0f, 1.5f, 1.0f / 0.58f, 2.0f, 3.0f};
static const float kScaleCycle[] = {1.0f, 4.0f / 3.0f, 1.5f, 1.0f / 0.58f, 2.0f};
static float g_outScale = 1.0f;  // pozadovane S (skutecne potvrzuje helper, Interop_ActiveOutScale)
static int g_phases = 8;         // skutecny pocet fazi jitteru (pevny z ini nebo auto podle S)
static int g_phasesSetting = 8;  // [jitter] Phases nebo Alt+F2: 0 = auto 8*S^2, jinak pevny pocet
static bool g_phasesAuto, g_phasesLogged;

// Filtr zmenseni pri S > 1 (Alt+F1), BMSD_DOWN_*
static unsigned g_downFilter;
static const char* DownFilterName(unsigned f) {
    static const char* names[BMSD_DOWN_COUNT] = {"Lanczos-2", "Catmull-Rom", "Mitchell", "box"};
    return f < BMSD_DOWN_COUNT ? names[f] : "?";
}

// S do logu: 1.0, 1.33, 1.5, 1.72, 2.0, 3.0
static std::string ScaleStr(float s) {
    char b[32];
    snprintf(b, sizeof b, "%.2f", s);
    std::string r = b;
    while (r.size() > 3 && r.back() == '0') r.pop_back();
    return r;
}

// [upscale] OutputScale: "1.5", "1,5", "4/3"; prichyti se na nejblizsi povolenou hodnotu.
static float ParseScale(const std::wstring& raw) {
    std::wstring t;
    for (wchar_t c : raw)
        if (!iswspace(c)) t.push_back(c == L',' ? L'.' : c);
    double v = 1.0;
    const size_t slash = t.find(L'/');
    if (slash != std::wstring::npos) {
        const double a = _wtof(t.substr(0, slash).c_str()), b = _wtof(t.substr(slash + 1).c_str());
        v = b > 0.0 ? a / b : 0.0;
    } else if (!t.empty()) {
        v = _wtof(t.c_str());
    }
    float best = 1.0f;
    double bestD = 1e9;
    for (float a : kScaleAllowed)
        if (fabs(v - a) < bestD) {
            bestD = fabs(v - a);
            best = a;
        }
    if (bestD > 0.02)
        Log("WARN: [upscale] OutputScale=%ls neni 1, 4/3, 1.5, 1.724, 2 ani 3 - pouzivam %s", raw.c_str(),
            ScaleStr(best).c_str());
    return best;
}

// Hloubka pro DLSS (Alt+F5): 0 puvodni -> 1 min 3x3 -> 2 konstantni 1.0 (test) -> 0
static unsigned g_depthMode;
static const char* DepthModeName(unsigned m) {
    static const char* names[BMSD_DEPTH_COUNT] = {"puvodni", "min 3x3", "konstantni (test)"};
    return m < BMSD_DEPTH_COUNT ? names[m] : "?";
}

// Vstup DLSS ([dlaa] InputMode, vychozi 1; ladici Alt+Ins): 0 LDR -> 1 linearni HDR -> 2 linearni HDR + auto expozice
// -> 3 linearni HDR + inverzni tone mapping -> 0
static unsigned g_inputMode;
static unsigned g_inputModeL;  // vstup pro preset L (12)
static const char* InputModeName(unsigned m) {
    static const char* names[BMSD_INPUT_COUNT] = {"LDR", "linearni HDR", "linearni HDR + auto expozice", nullptr};
    static char itm[64];
    if (m == BMSD_INPUT_HDR_ITM) {
        snprintf(itm, sizeof itm, "linearni HDR + inverzni tone mapping (k=%.2f)", g_cfg.itmStrength);
        return itm;
    }
    return m < BMSD_INPUT_COUNT ? names[m] : "?";
}

// Doostreni (RCAS v helperu): Ctrl+Home cykli 0 -> 0.2 -> ... -> 1.0 -> 0
static float g_sharpness;
// Znamenko jitteru pro DLSS z klavesy (Ctrl+F11 v rezimu DLAA): (+,+) -> (-,+) -> (+,-) -> (-,-)
static int g_jitterSignIdx;

// Preset DLSS: 0 = vychozi SDK (pro DLAA K), jinak NVSDK_NGX_DLSS_Hint_Render_Preset (K = 11, L = 12, M = 13).
// Preset J (10) zamerne vynechan - uz se nepouziva.
static const unsigned kPresets[] = {0, 11, 12, 13};
static const int kPresetCount = (int)(sizeof kPresets / sizeof kPresets[0]);
static unsigned g_preset;
static unsigned EffInputMode() { return g_preset == 12 ? g_inputModeL : g_inputMode; }  // vstup DLSS pro zvoleny preset

static const char* PresetName(unsigned p) {
    static const char* names[] = {"vychozi", "K", "L", "M"};
    for (int i = 0; i < kPresetCount; ++i)
        if (kPresets[i] == p) return names[i];
    return "?";
}

// "K", "L", "M" nebo cislo; 0 / prazdne = vychozi. Jine hodnoty (zrusene a vyhrazene presety) -> vychozi.
static unsigned ParsePreset(const std::wstring& s) {
    unsigned p = 0;
    if (s.size() == 1 && iswalpha(s[0]))
        p = (unsigned)(towupper(s[0]) - L'A' + 1);
    else if (!s.empty())
        p = (unsigned)wcstoul(s.c_str(), nullptr, 0);
    for (unsigned k : kPresets)
        if (k == p) return p;
    Log("WARN: [dlaa] Preset=%ls neni K, L ani M - pouzivam vychozi", s.c_str());
    return 0;
}

static std::wstring IniPath() { return std::wstring(g_dir) + L"\\bms_dlss.ini"; }

static std::wstring IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[256];
    GetPrivateProfileStringW(sec, key, def, buf, 256, IniPath().c_str());
    return buf;
}

// Desetinne cislo z ini: tecka i carka ("-0,5"; _wtof by carku tise usekl na -0), mezery nevadi - stejne jako
// ParseScale a helper. Prazdne = def; neni cislo -> WARN a def; znaky za cislem -> WARN (cislo plati).
static float IniFloat(const wchar_t* sec, const wchar_t* key, float def) {
    const std::wstring raw = IniStr(sec, key, L"");
    std::wstring t;
    for (wchar_t c : raw)
        if (!iswspace(c)) t.push_back(c == L',' ? L'.' : c);
    if (t.empty()) return def;
    wchar_t* end = nullptr;
    const double v = wcstod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(v)) {
        Log("WARN: [%ls] %ls=%ls neni cislo - pouzivam %g", sec, key, raw.c_str(), def);
        return def;
    }
    if (*end) Log("WARN: [%ls] %ls=%ls - znaky '%ls' za cislem ignoruji (pouzivam %g)", sec, key, raw.c_str(), end, v);
    return (float)v;
}

static void LatLoadConfig();  // src\latency.inl
static void MenuLoadConfig();  // src\menu.inl

static void LoadConfig() {
    g_cfg.captureKey = (int)wcstol(IniStr(L"keys", L"CaptureKey", L"0x79").c_str(), nullptr, 0);
    g_cfg.jitterKey = (int)wcstol(IniStr(L"keys", L"JitterKey", L"0x7A").c_str(), nullptr, 0);
    g_cfg.bridgeKey = (int)wcstol(IniStr(L"keys", L"BridgeKey", L"0x77").c_str(), nullptr, 0);
    g_cfg.requireCtrl = wcstol(IniStr(L"keys", L"RequireCtrl", L"1").c_str(), nullptr, 0) != 0;
    {
        std::wstring m = IniStr(L"keys", L"Modifier", L"alt");
        for (auto& c : m) c = (wchar_t)towlower(c);
        if (m == L"ctrl") g_cfg.modifier = VK_CONTROL;
        else if (m == L"rctrl") g_cfg.modifier = VK_RCONTROL;
        else if (m == L"shift") g_cfg.modifier = VK_SHIFT;
        else if (m == L"none" || !g_cfg.requireCtrl) g_cfg.modifier = 0;
        else g_cfg.modifier = VK_MENU;
        Log("klavesy: modifikator %ls", g_cfg.modifier == 0 ? L"zadny" : m.c_str());
    }
    g_cfg.phases = std::min(256, std::max(0, (int)wcstol(IniStr(L"jitter", L"Phases", L"8").c_str(), nullptr, 0)));
    g_cfg.phasesKey = (int)wcstol(IniStr(L"keys", L"PhasesKey", L"0x71").c_str(), nullptr, 0);
    g_cfg.downsampleKey = (int)wcstol(IniStr(L"keys", L"DownsampleKey", L"0x70").c_str(), nullptr, 0);
    g_cfg.pipelineKey = (int)wcstol(IniStr(L"keys", L"PipelineKey", L"0x91").c_str(), nullptr, 0);
    g_cfg.pipeline = wcstol(IniStr(L"bridge", L"Pipeline", L"1").c_str(), nullptr, 0) != 0;
    // stejne cteni a omezeni jako v helperu
    g_cfg.downsampleFilter = (unsigned)wcstoul(IniStr(L"upscale", L"DownsampleFilter", L"1").c_str(), nullptr, 0);
    if (g_cfg.downsampleFilter >= BMSD_DOWN_COUNT) g_cfg.downsampleFilter = BMSD_DOWN_CATMULLROM;
    g_cfg.downsampleLinear = wcstol(IniStr(L"upscale", L"DownsampleLinear", L"0").c_str(), nullptr, 0) != 0;
    g_cfg.scale = IniFloat(L"jitter", L"Scale", 1.0f);
    g_cfg.startEnabled = wcstol(IniStr(L"jitter", L"StartEnabled", L"0").c_str(), nullptr, 0) != 0;
    g_cfg.presetKey = (int)wcstol(IniStr(L"keys", L"PresetKey", L"0x78").c_str(), nullptr, 0);
    g_cfg.debugCycle = wcstol(IniStr(L"bridge", L"DebugCycle", L"0").c_str(), nullptr, 0) != 0;
    g_cfg.preset = ParsePreset(IniStr(L"dlaa", L"Preset", L"0"));
    g_cfg.sharpenKey = (int)wcstol(IniStr(L"keys", L"SharpenKey", L"0x24").c_str(), nullptr, 0);
    g_cfg.sharpness = std::min(1.0f, std::max(0.0f, IniFloat(L"dlaa", L"Sharpness", 0.0f)));
    g_cfg.mipBiasKey = (int)wcstol(IniStr(L"keys", L"MipBiasKey", L"0x2E").c_str(), nullptr, 0);
    g_cfg.mipBias = std::min(0.0f, std::max(-4.0f, IniFloat(L"dlaa", L"MipBias", -0.5f)));
    g_cfg.mipBiasNormal = std::min(2.0f, std::max(-2.0f, IniFloat(L"dlaa", L"MipBiasNormal", 0.0f)));
    g_cfg.mipBiasAlpha = std::min(2.0f, std::max(-2.0f, IniFloat(L"dlaa", L"MipBiasAlphaTest", 0.0f)));
    Log("config [dlaa]: MipBiasNormal=%+.2f MipBiasAlphaTest=%+.2f (pricita se k mip biasu sceny)", g_cfg.mipBiasNormal,
        g_cfg.mipBiasAlpha);
    g_cfg.inputModeKey = (int)wcstol(IniStr(L"keys", L"InputModeKey", L"0x2D").c_str(), nullptr, 0);
    // vychozi 1 = linearni HDR (od verze 14; v menu uz neni, klic zustava pro pokrocile)
    g_cfg.inputMode = (unsigned)wcstoul(IniStr(L"dlaa", L"InputMode", L"1").c_str(), nullptr, 0);
    if (g_cfg.inputMode >= BMSD_INPUT_COUNT) {
        Log("WARN: [dlaa] InputMode=%u neni 0 az 3 - pouzivam 1 (linearni HDR)", g_cfg.inputMode);
        g_cfg.inputMode = BMSD_INPUT_HDR;
    }
    g_cfg.inputModeL = (unsigned)wcstoul(IniStr(L"dlaa", L"InputModeL", L"0").c_str(), nullptr, 0) % BMSD_INPUT_COUNT;
    g_cfg.camHold = wcstol(IniStr(L"dlaa", L"CamHold", L"1").c_str(), nullptr, 0) != 0;
    // stejne omezeni jako v helperu (ten hodnotu cte z ini sam)
    g_cfg.itmStrength = std::min(0.99f, std::max(0.0f, IniFloat(L"dlaa", L"ITMStrength", 0.95f)));
    g_cfg.depthModeKey = (int)wcstol(IniStr(L"keys", L"DepthModeKey", L"0x74").c_str(), nullptr, 0);
    g_cfg.depthMode = (unsigned)wcstoul(IniStr(L"dlaa", L"DepthMode", L"0").c_str(), nullptr, 0);
    if (g_cfg.depthMode >= BMSD_DEPTH_COUNT) {
        Log("WARN: [dlaa] DepthMode=%u neni 0 az 2 - pouzivam 0 (puvodni)", g_cfg.depthMode);
        g_cfg.depthMode = BMSD_DEPTH_RAW;
    }
    g_cfg.outScaleKey = (int)wcstol(IniStr(L"keys", L"OutputScaleKey", L"0x72").c_str(), nullptr, 0);
    g_cfg.outScale = ParseScale(IniStr(L"upscale", L"OutputScale", L"1.0"));
    g_cfg.mipBiasMode = (int)wcstol(IniStr(L"upscale", L"MipBiasMode", L"0").c_str(), nullptr, 0);
    if (g_cfg.mipBiasMode != 0 && g_cfg.mipBiasMode != 1) {
        Log("WARN: [upscale] MipBiasMode=%d neni 0 ani 1 - pouzivam 0", g_cfg.mipBiasMode);
        g_cfg.mipBiasMode = 0;
    }
    g_cfg.capFrames = std::min(10, std::max(1, (int)wcstol(IniStr(L"capture", L"Frames", L"3").c_str(), nullptr, 0)));
    g_cfg.capAutoSec = std::max(0, (int)wcstol(IniStr(L"capture", L"AutoAfterSec", L"0").c_str(), nullptr, 0));
    if (g_cfg.capAutoSec)
        Log("zachyt: %d snimku, automaticky %d s po prvnim Present", g_cfg.capFrames, g_cfg.capAutoSec);
    else
        Log("zachyt: %d snimku, automaticky vypnuto", g_cfg.capFrames);
    // [mv]: motion vektory pohyblivych objektu
    g_cfg.objMv = wcstol(IniStr(L"mv", L"ObjectMV", L"1").c_str(), nullptr, 0) != 0;
    g_cfg.objMvKey = (int)wcstol(IniStr(L"keys", L"ObjectMVKey", L"0x08").c_str(), nullptr, 0);
    g_cfg.objMvTol = std::min(1.0f, std::max(0.0f, IniFloat(L"mv", L"DepthTol", 0.02f)));
    g_cfg.objMvTeleport = std::max(0.0f, IniFloat(L"mv", L"TeleportDist", 64.0f));
    g_cfg.objMvValidate = wcstol(IniStr(L"mv", L"Validate", L"0").c_str(), nullptr, 0) != 0;
    g_cfg.objMvViewmodel = wcstol(IniStr(L"mv", L"Viewmodel", L"1").c_str(), nullptr, 0) != 0;
    Log("config [mv]: ObjectMV=%d DepthTol=%g TeleportDist=%g Validate=%d Viewmodel=%d objectMVKey=0x%X",
        (int)g_cfg.objMv, g_cfg.objMvTol, g_cfg.objMvTeleport, (int)g_cfg.objMvValidate, (int)g_cfg.objMvViewmodel,
        g_cfg.objMvKey);
    g_cfg.lightsB = wcstol(IniStr(L"upscale", L"LightsB", L"1").c_str(), nullptr, 0) != 0;
    g_cfg.lightsBDetect = wcstol(IniStr(L"upscale", L"LightsBDetect", L"0").c_str(), nullptr, 0) != 0;
    if (g_cfg.lightsBDetect) Log("config [upscale]: LightsBDetect=1 (v casti B hledat shadery s konstantami podle renderu)");
    g_cfg.lightsBSunFix = wcstol(IniStr(L"upscale", L"LightsBSunFix", L"1").c_str(), nullptr, 0) != 0;
    if (!g_cfg.lightsBSunFix) Log("config [upscale]: LightsBSunFix=0 (zare slunce v casti B bez upravene kopie shaderu)");
    Log("config [upscale]: LightsB=%d (opravy svetel, stinu slunce, castic a refrakce v casti B, zaplata client.dll)", g_cfg.lightsB ? 1 : 0);
    {
        std::wstring u = IniStr(L"upscale", L"Upscaler", L"auto");
        for (auto& c : u) c = (wchar_t)towlower(c);
        g_cfg.upscaler = u == L"off" || u == L"0" ? 0 : u == L"dlss" ? 1 : u == L"fsr" ? 2 : u == L"xess" ? 3 : 4;
        if (u != L"auto" && u != L"dlss" && u != L"off" && u != L"0" && u != L"fsr" && u != L"xess")
            Log("WARN: [upscale] Upscaler=%ls neni auto, dlss, fsr, xess ani off - pouzivam auto", u.c_str());
    }
    g_cfg.skipGameFxaa = wcstol(IniStr(L"upscale", L"SkipGameFxaa", L"1").c_str(), nullptr, 0) != 0;
    g_cfg.stripMsaa = wcstol(IniStr(L"upscale", L"StripMsaa", L"1").c_str(), nullptr, 0) != 0;
    Log("config [upscale]: SkipGameFxaa=%d (FXAA hry po DLSS / FSR vynechat) StripMsaa=%d (MSAA hry odebrat)",
        (int)g_cfg.skipGameFxaa, (int)g_cfg.stripMsaa);
    g_cfg.reactiveMask = wcstol(IniStr(L"upscale", L"ReactiveMask", L"0").c_str(), nullptr, 0) != 0;
    g_cfg.reactiveGlass = wcstol(IniStr(L"upscale", L"ReactiveGlass", L"0").c_str(), nullptr, 0) != 0;
    Log("config [upscale]: ReactiveMask=%d ReactiveGlass=%d (maska reaktivity z pruhlednych a aditivnich draw pro "
        "DLSS / FSR)", (int)g_cfg.reactiveMask, (int)g_cfg.reactiveGlass);
    // [keys] DebugKeys=0 (vychozi): ladici klavesy vypnute (kod 0 = zadna klavesa; KeyDown(0) je vzdy false, hacek je
    // nepolyka). Alt+F8 (bridgeKey) a Alt+N (MenuKey) plati vzdy. Uzivatel 2026-11-19: vse ostatni je v menu.
    g_cfg.alphaJitKey = (int)wcstol(IniStr(L"keys", L"AlphaJitterKey", L"0x75").c_str(), nullptr, 0);
    g_cfg.alphaJit = std::min(1.0f, std::max(0.0f, IniFloat(L"dlaa", L"AlphaJitter", 0.0f)));
    g_cfg.debugKeys = wcstol(IniStr(L"keys", L"DebugKeys", L"0").c_str(), nullptr, 0) != 0;
    g_cfg.testKeys = wcstol(IniStr(L"keys", L"TestKeys", L"0").c_str(), nullptr, 0) != 0;
    if (!g_cfg.debugKeys) {
        g_cfg.captureKey = g_cfg.jitterKey = g_cfg.presetKey = g_cfg.sharpenKey = g_cfg.mipBiasKey = 0;
        g_cfg.inputModeKey = g_cfg.depthModeKey = g_cfg.outScaleKey = g_cfg.phasesKey = g_cfg.downsampleKey = 0;
        g_cfg.pipelineKey = g_cfg.objMvKey = g_cfg.alphaJitKey = 0;
    }
    Log("klavesy: Alt+N menu, Alt+F8 mod zap/vyp%s%s",
        g_cfg.debugKeys ? ", ladici klavesy zapnute ([keys] DebugKeys=1)" : "",
        g_cfg.testKeys ? ", zkratky pro testovani Alt+F9 / Alt+F10 ([keys] TestKeys=1)" : "");
    LatLoadConfig();  // [latency] (src\latency.inl; Alt+F12 jen s DebugKeys=1)
    MenuLoadConfig();  // [menu], [keys] MenuKey (src\menu.inl; az po klavesach vyse - sestavi z nich bitmapu)
    Log("config:captureKey=0x%X jitterKey=0x%X bridgeKey=0x%X presetKey=0x%X sharpenKey=0x%X mipBiasKey=0x%X "
        "inputModeKey=0x%X depthModeKey=0x%X outScaleKey=0x%X phasesKey=0x%X downsampleKey=0x%X pipelineKey=0x%X "
        "requireCtrl=%d phases=%d%s scale=%.3f startEnabled=%d debugCycle=%d pipeline=%d preset=%s sharpness=%.2f "
        "mipBias=%.2f inputMode=%u (%s) depthMode=%u (%s) outputScale=%s mipBiasMode=%d downsample=%s (%s)",
        g_cfg.captureKey, g_cfg.jitterKey, g_cfg.bridgeKey, g_cfg.presetKey, g_cfg.sharpenKey, g_cfg.mipBiasKey,
        g_cfg.inputModeKey, g_cfg.depthModeKey, g_cfg.outScaleKey, g_cfg.phasesKey, g_cfg.downsampleKey,
        g_cfg.pipelineKey, g_cfg.requireCtrl, g_cfg.phases, g_cfg.phases ? "" : " (auto 8*S^2)", g_cfg.scale,
        g_cfg.startEnabled, g_cfg.debugCycle, (int)g_cfg.pipeline, PresetName(g_cfg.preset), g_cfg.sharpness,
        g_cfg.mipBias, g_cfg.inputMode, InputModeName(g_cfg.inputMode), g_cfg.depthMode,
        DepthModeName(g_cfg.depthMode), ScaleStr(g_cfg.outScale).c_str(), g_cfg.mipBiasMode,
        DownFilterName(g_cfg.downsampleFilter), g_cfg.downsampleLinear ? "linearni" : "srgb");
}

// ---------------------------------------------------------------------------
// Vtable hooky
// ---------------------------------------------------------------------------
template <class T>
static void HookVtbl(void* obj, int idx, T hook, T& orig, const char* what) {
    void** vt = *(void***)obj;
    if (vt[idx] == (void*)hook) return;
    if (orig && vt[idx] != (void*)orig) {
        Log("WARN: %s ma jinou vtable nez predtim, preskakuji", what);
        return;
    }
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    orig = (T)vt[idx];
    vt[idx] = (void*)hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    Log("hook %s (vtable[%d])", what, idx);
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                      D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDeviceEx)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                        D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*,
                                                        IDirect3DDevice9Ex**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                                 const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_PresentEx)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND,
                                                   const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SCPresent)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                   const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetVSConstF)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetVS)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateVS)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePS)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawIdxPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT,
                                                     UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawPrimUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawIdxPrimUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                       const void*, D3DFORMAT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetRT)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetDS)(IDirect3DDevice9*, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float,
                                               DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_StretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                                     IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetSamplerState)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetSamplerState)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetRenderState)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetVDecl)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetFVF)(IDirect3DDevice9*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetStreamSource)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetStreamSourceFreq)(IDirect3DDevice9*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetIndices)(IDirect3DDevice9*, IDirect3DIndexBuffer9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateQuery)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_QueryIssue)(IDirect3DQuery9*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResetEx)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
// MSAA hry ([upscale] StripMsaa): RT a hloubka (vtable[28], [29]; Ex [129], [131])
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateRT)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD,
                                                 BOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDS)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD,
                                                 BOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateRTEx)(IDirect3DDevice9Ex*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE,
                                                   DWORD, BOOL, IDirect3DSurface9**, HANDLE*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDSEx)(IDirect3DDevice9Ex*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE,
                                                   DWORD, BOOL, IDirect3DSurface9**, HANDLE*, DWORD);
static PFN_CreateRT o_CreateRT;
static PFN_CreateDS o_CreateDS;
static PFN_CreateRTEx o_CreateRTEx;
static PFN_CreateDSEx o_CreateDSEx;
// Lock vertex a index bufferu (vtable[11] obou rozhrani): MV objektu hlidaji prepis dat mezi zaznamem a davkou
typedef HRESULT(STDMETHODCALLTYPE* PFN_VBLock)(IDirect3DVertexBuffer9*, UINT, UINT, void**, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_IBLock)(IDirect3DIndexBuffer9*, UINT, UINT, void**, DWORD);

static PFN_CreateDevice o_CreateDevice;
static PFN_CreateDeviceEx o_CreateDeviceEx;
static PFN_Present o_Present;
static PFN_PresentEx o_PresentEx;
static PFN_SCPresent o_SCPresent;
static PFN_SetVSConstF o_SetVSConstF;
static PFN_SetVS o_SetVS;
static PFN_CreateVS o_CreateVS;
static PFN_CreatePS o_CreatePS;  // jen s [upscale] LightsB (LightsFixPre)
static PFN_DrawPrim o_DrawPrim;
static PFN_DrawIdxPrim o_DrawIdxPrim;
static PFN_DrawPrimUP o_DrawPrimUP;
static PFN_DrawIdxPrimUP o_DrawIdxPrimUP;
static PFN_SetRT o_SetRT;
static PFN_SetDS o_SetDS;
static PFN_Clear o_Clear;
static PFN_SetViewport o_SetViewport;
static PFN_StretchRect o_StretchRect;
static PFN_SetTexture o_SetTexture;
static PFN_SetSamplerState o_SetSamplerState;
static PFN_GetSamplerState o_GetSamplerState;
static PFN_SetRenderState o_SetRenderState;
static PFN_SetVDecl o_SetVDecl;
static PFN_SetFVF o_SetFVF;
static PFN_SetStreamSource o_SetStreamSource;
static PFN_SetStreamSourceFreq o_SetStreamSourceFreq;
static PFN_SetIndices o_SetIndices;
static PFN_CreateQuery o_CreateQuery;
static PFN_QueryIssue o_QueryIssue;
static PFN_Reset o_Reset;
static PFN_ResetEx o_ResetEx;
static PFN_VBLock o_VBLock;
static PFN_IBLock o_IBLock;

// ---------------------------------------------------------------------------
// Informace o vertex shaderech z CTAB
// ---------------------------------------------------------------------------
enum ClipKind : uint8_t { CK_VIEWPROJ, CK_MODELVIEWPROJ, CK_CAMVIEWPROJ, CK_PROJ };
static const char* kClipKindNames[] = {"cViewProj", "cModelViewProj", "camViewProj", "cProj"};

// Cesta draw callu pro MV pohyblivych objektu (src\objmv.inl) podle CTAB herniho VS, viz docs\mv-objekty-krok0.md:
//   brush   - cModel c58..60 + cModelViewProj c4..7 (oPos = pos * c4; svet a brush entity),
//   rigid   - cModel c58..60 + cViewProj c8..11 (oPos = pos * cModel * c8; props),
//   skinned - cModel c58..216 + cViewProj (3 kosti na vrchol, paleta 53 kosti).
// Vse ostatni (okluzni krabicky bez cModel, sprity s cProj, post-process...) se nezaznamenava.
enum : uint8_t { OM_NONE, OM_BRUSH, OM_RIGID, OM_SKINNED, OM_PATHS };

struct ShaderInfo {
    std::string text;      // citelny popis pro zachyty
    bool hasCtab = false;  // shader ma tabulku konstant (post-process shadery ano, nektere efekty ne)
    uint8_t nClip = 0;     // pocet clip-matic v shaderu
    uint8_t clipReg[4];    // jejich pocatecni registry
    uint8_t clipKind[4];
    uint8_t omPath = OM_NONE;  // MV objektu: cesta podle CTAB
    int16_t vpScaledReg = -1;  // VS: registr ViewportTransformScaled (spritecard; O2 mekke castice), -1 = nema
    std::vector<uint8_t> cuse; // VS + LightsBDetect: pouziti konstant c0..c223 (ShaderConstUse), prazdne = nezname
};

static ShaderInfo ParseShader(const DWORD* code) {
    ShaderInfo si;
    if (!code) { si.text = "(null)"; return si; }
    char buf[256];
    DWORD ver = code[0];
    snprintf(buf, sizeof buf, "%s_%u_%u", (ver >> 16) == 0xFFFF ? "ps" : "vs", (ver >> 8) & 0xFF, ver & 0xFF);
    si.text = buf;
    const DWORD* p = code + 1;
    for (int guard = 0; guard < 64; ++guard) {
        DWORD tok = *p;
        if ((tok & 0xFFFF) != 0xFFFE) break;  // uz to neni komentar
        DWORD len = (tok >> 16) & 0x7FFF;
        if (len >= 8 && p[1] == MAKEFOURCC('C', 'T', 'A', 'B')) {
            const uint8_t* base = (const uint8_t*)(p + 2);
            size_t size = (size_t)(len - 1) * 4;
            DWORD nConst = *(const DWORD*)(base + 12);
            DWORD infoOff = *(const DWORD*)(base + 16);
            static const char* sets[] = {"b", "i", "c", "s"};
            int modelCnt = 0;  // pocet registru cModel od c58 (0 = shader ho nema)
            for (DWORD i = 0; i < nConst && infoOff + (i + 1) * 20 <= size; ++i) {
                const uint8_t* ci = base + infoOff + i * 20;
                DWORD nameOff = *(const DWORD*)ci;
                WORD set = *(const WORD*)(ci + 4), idx = *(const WORD*)(ci + 6), cnt = *(const WORD*)(ci + 8);
                if (nameOff >= size) continue;
                const char* name = (const char*)(base + nameOff);
                int nlen = (int)std::min<size_t>(strnlen(name, size - nameOff), 80);
                if (cnt > 1)
                    snprintf(buf, sizeof buf, " %.*s=%s%u..%u", nlen, name, set < 4 ? sets[set] : "?", idx,
                             idx + cnt - 1);
                else
                    snprintf(buf, sizeof buf, " %.*s=%s%u", nlen, name, set < 4 ? sets[set] : "?", idx);
                si.text += buf;
                si.hasCtab = true;
                if (set == 2 && idx == 58 && nlen == 6 && !strncmp(name, "cModel", 6)) modelCnt = cnt;
                if (set == 2 && cnt >= 4 && idx <= 252 && si.nClip < 4) {
                    for (uint8_t k = 0; k < 4; ++k) {
                        if ((int)strlen(kClipKindNames[k]) == nlen && !strncmp(name, kClipKindNames[k], nlen)) {
                            si.clipReg[si.nClip] = (uint8_t)idx;
                            si.clipKind[si.nClip] = k;
                            ++si.nClip;
                        }
                    }
                }
            }
            // MV objektu: jen jednoznacne kombinace (cModel od c58 a prave jedna z matic c4 / c8, bez cProj)
            bool vp = false, mvp = false, other = false;
            for (uint8_t i = 0; i < si.nClip; ++i) {
                if (si.clipKind[i] == CK_VIEWPROJ && si.clipReg[i] == 8) vp = true;
                else if (si.clipKind[i] == CK_MODELVIEWPROJ && si.clipReg[i] == 4) mvp = true;
                else if (si.clipKind[i] != CK_CAMVIEWPROJ) other = true;  // cProj (sprity) nebo jine registry
            }
            if (modelCnt >= 3 && !other && vp != mvp) {
                if (mvp)
                    si.omPath = modelCnt == 3 ? OM_BRUSH : OM_NONE;
                else
                    si.omPath = modelCnt == 3 ? OM_RIGID : OM_SKINNED;
            }
            return si;
        }
        p += 1 + len;
    }
    si.text += " (bez CTAB)";
    return si;
}

// Rozbor bytecode ps/vs 2.0+ (O3 detektoru casti B, O1 kontrola volneho registru): ktere slozky konstant c0..c223
// shader cte. use[reg]: bity 0-3 = ctene slozky x/y/z/w, bit 4 = registr si shader definuje sam (def -> hodnota od hry
// se nepouzije). Ctene slozky = selektory swizzle pro pouzite pruhy: po slozkach (mov, add, mad, mul, cmp ...) podle
// masky cile, dp3 xyz, dp4 xyzw, dp2add xy (src2 x), ostatni (texld, skalarni rcp/pow/..., if, sincos) vsechny 4 pruhy
// (u replikovaneho swizzle je to jedna slozka). Matice m4x4 apod. a relativni adresovani c[a0 + n] -> konzervativne
// vse. false = nelze (sm1 bez delek instrukci, konec mimo meze). sizeDw = delka vcetne koncoveho tokenu.
static const UINT kConstUseRegs = 224;
static bool ShaderConstUse(const DWORD* code, UINT maxDw, uint8_t* use, UINT* sizeDw) {
    if (!code || maxDw < 2 || ((code[0] >> 8) & 0xFF) < 2) return false;
    memset(use, 0, kConstUseRegs);
    UINT i = 1;
    while (i < maxDw && code[i] != 0x0000FFFF) {
        const DWORD tok = code[i];
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        if (i + len >= maxDw) return false;
        const DWORD* q = code + i + 1;
        const DWORD* e = q + len;
        auto regType = [](DWORD t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); };
        if (op == 81) {  // def c#
            if (len >= 1 && regType(q[0]) == 2 && (q[0] & 0x7FF) < kConstUseRegs) use[q[0] & 0x7FF] |= 0x10;
        } else if (op != 31 && op != 47 && op != 48) {  // ne dcl, defb, defi
            const bool hasDst = !(op == 0 || (op >= 25 && op <= 30) || op == 38 || op == 39 || (op >= 40 && op <= 45) ||
                                  op == 65 || op == 96);
            DWORD dmask = 0xF;
            if (hasDst && q < e) {
                dmask = (q[0] >> 16) & 0xF;
                q += (q[0] & (1u << 13)) ? 2 : 1;
                if (tok & (1u << 28)) ++q;  // predikat za cilem
            }
            for (int src = 0; q < e; ++src) {
                const DWORD t = *q;
                const bool rel = (t & (1u << 13)) != 0;
                const DWORD reg = t & 0x7FF;
                if (regType(t) == 2 && reg < kConstUseRegs) {
                    DWORD lanes = 0xF;
                    if (op == 8 || op == 36 || op == 33) lanes = 0x7;           // dp3, nrm, crs
                    else if (op == 90) lanes = src < 2 ? 0x3 : 0x1;              // dp2add
                    else if ((op >= 1 && op <= 5) || op == 10 || op == 11 || op == 12 || op == 13 || op == 18 ||
                             op == 19 || op == 34 || op == 35 || op == 80 || op == 88 || op == 91 || op == 92)
                        lanes = dmask;                                           // po slozkach
                    UINT nRegs = 1;
                    if (op >= 20 && op <= 24 && src == 1) {                      // m4x4 m4x3 m3x4 m3x3 m3x2
                        nRegs = (op == 20 || op == 22) ? 4 : (op == 24 ? 2 : 3);
                        lanes = 0xF;
                    }
                    if (rel) nRegs = kConstUseRegs - reg;                        // c[a0 + n]: konzervativne
                    uint8_t rd = 0;
                    for (int l = 0; l < 4; ++l)
                        if (lanes & (1u << l)) rd |= (uint8_t)(1u << ((t >> (16 + 2 * l)) & 3));
                    if (nRegs > 1) rd = 0xF;
                    for (UINT r = reg; r < reg + nRegs && r < kConstUseRegs; ++r) use[r] |= rd;
                }
                q += rel ? 2 : 1;
            }
        }
        i += 1 + len;
    }
    if (i >= maxDw) return false;
    if (sizeDw) *sizeDw = i + 1;
    return true;
}

static std::mutex g_vsInfoMx;
static std::unordered_map<void*, ShaderInfo> g_vsInfo;

// ---------------------------------------------------------------------------
// Stav hlavniho zarizeni
// ---------------------------------------------------------------------------
static IDirect3DDevice9* g_main;    // zarizeni s nejvetsim backbufferem
static UINT g_mainArea;
static UINT g_bbW, g_bbH;           // velikost backbufferu hlavniho zarizeni
static UINT g_rt0W, g_rt0H;         // velikost aktualniho render targetu 0
static IDirect3DSurface9* g_rt0;    // aktualni render target 0
static D3DFORMAT g_rt0Fmt;
static IDirect3DSurface9* g_ds;     // aktualni depth-stencil

// Konec hlavni sceny (misto pro DLSS): barva sceny RGBA8, jeji depth-stencil,
// linearni hloubka R32F z predpruchodu.
static IDirect3DSurface9* g_sceneRT;
static IDirect3DSurface9* g_sceneDS;
static IDirect3DSurface9* g_linDepth;
static UINT g_sceneW, g_sceneH;  // velikost povrchu g_sceneRT (g_rt0W x g_rt0H pri jeho zaznamu)
static bool g_sceneEndDone;
static bool g_frameUpscaled;  // tento snimek: most odeslal vysledek DLSS / FSR do sceny (SceneBridge; FxaaPre)

// Okluzni dotazy hry (D3DQUERYTYPE_OCCLUSION) hlavniho zarizeni, zacate (Issue BEGIN) a jeste neukoncene.
// Engine meri histogram jasu pro auto expozici okluznimi dotazy: kolem draw luminance_compare (bez zapisu barvy,
// CTAB, do barvy sceny) - a prave ten spousti konec sceny. Draw cally konce sceny (prehrani MV objektu do OBJMV)
// by se do dotazu zapocitaly, kazdy kos histogramu by dostal pixely navic a expozice by tmavla (pohled do slunce,
// Surface Tension s animovanymi NPC). Takovy konec sceny se proto odlozi za Issue(END) dotazu.
static IDirect3DQuery9* g_occlOpen[16];
static int g_nOcclOpen;
static bool g_sceneEndPending;            // konec sceny odlozen (spusti ho Issue(END), jinak dalsi volani)
static std::string g_sceneEndPendingWhy;

// ---- Cast B: skutecny upscaling pres mat_viewportscale (docs\navrh-upscaling-B2.md, zuzeno podle
// docs\advokat-upscaling-B2.md) ----
// Render = viewport perspektivnich draw kamery sceny (DetectRender na konci sceny). Mensi nez povrch sceny a v levem
// hornim rohu = cast B: na konci sceny (CTAB) se jen vybere kamera a prehraji MV objektu (ScenePrep), most (SceneBridge)
// bezi az na draw roztazeni sceny, ktery engine kresli po post-processingu:
//   StretchRect scena -> _rt_FullFrameFB1, pak jeden draw do sceny pres cely viewport, bez perspektivni matice,
//   s texturou 0 = kontejner cile te kopie (ukazatel VS se mezi behy lisi, proto se pozna podle vlastnosti).
// Kdyz most snimek zaradil (vystup DLSS W x H prijde kopii B do cele sceny), draw se potlaci - i bezprostredne
// nasledujici draw se stejnym podpisem. Jinak ho nakresli engine a dalsi snimek je bez jitteru.
static UINT g_renderW, g_renderH;  // render posledniho snimku s koncem sceny (0 = zatim zadny)
static UINT g_renderSurfW, g_renderSurfH;  // povrch sceny tehoz snimku
static bool g_pathB;               // posledni konec sceny: render < povrch sceny (cast B)
static bool g_pathBad;             // posledni konec sceny: viewport kamery nejde pouzit -> snimek bez mostu
static bool g_renderKnown;
static bool g_bHandled;            // tento snimek: draw roztazeni uz byl vyrizen (most spusten)
static bool g_bFollow;             // roztazeni potlaceno: potlacit i hned nasledujici draw se stejnym podpisem
static void *g_bFollowVS, *g_bFollowPS;  // ... a se stejnym VS, PS a poctem primitiv jako potlacene roztazeni
static unsigned g_bFollowPrims;
static bool g_bBetween;            // tento snimek: mezi kopii sceny a roztazenim byl draw do sceny (zapocitano)
static bool g_bFoundLogged, g_bNoUpscaleLogged, g_bScaleLogged, g_bDepthLogged;
static unsigned g_sceneObjDraws;   // draw prehrane davkou MV objektu v tomto snimku (ScenePrep -> SceneBridge)
// Posledni StretchRect z barvy sceny v tomto snimku (i po konci sceny; g_sceneCopy nize je jen do konce sceny).
// Ukazatele jen jako identita v ramci snimku (nuluje se kazdy snimek a pred Resetem).
static struct {
    IDirect3DSurface9* dst;
    void* tex;            // kontejner dst (IDirect3DTexture9), nullptr = dst neni uroven textury
    RECT sr, dr;
    bool hasSr, hasDr, valid;
    unsigned drawsSince;  // draw do barvy sceny od teto kopie
} g_lastSceneSR;
static IDirect3DBaseTexture9* g_t0;  // textura stage 0 podle SetTexture (porad, mimo davku MV objektu)
// ---- K5: HUD-less vstup Frame Generation (docs/fg-hudless-vysledek.md) ----
// Bod "konec post-processingu, pred HUD": po konci sceny je POST draw = draw do barvy sceny, ktery vzorkuje cil
// posledni kopie sceny (StretchRect scena -> _rt_FullFrameFB...; bloom, engine_post, v casti B roztazeni sceny - i
// potlacene). HUD (vgui: titulky, zamerovac, zpravy) kopii sceny necte. Po POST draw se ceka; prvni dalsi zapis do
// sceny, ktery POST neni (draw, Clear barvy, StretchRect do sceny), nebo Present = HUD-less: StretchRect sceny do RT
// presenteru (Present_HudlessTarget). Dalsi POST draw (dalsi pruchod postu, napr. hledi kuse) bod posune a kopie se
// zopakuje. Snimek bez POST draw (mat_postprocess_enable 0, menu hry, nacitani) = bez HUD-less (jako v13).
static IDirect3DBaseTexture9* g_tex4[4];  // textury stage 0..3 podle SetTexture (POST draw cte kopii sceny v t0 / t1)
static struct {
    bool pending;       // byl POST draw a od nej jeste zadny zapis HUD (kopie do RT ceka)
    bool valid;         // RT obsahuje barvu sceny po poslednim POST draw tohoto snimku
    unsigned snaps;     // kopii do RT v tomto snimku
    unsigned postAfter; // POST draw po kopii (bod se posunul)
    bool loggedSame;    // jednou do logu: scena == backbuffer?
} g_hl;
// Viewporty perspektivnich draw hlavniho pohledu pred koncem sceny v tomto snimku (vyber renderu bez kamery
// a pocitadlo draw s jinym viewportem nez render - ukaze, jestli ma jitter podle viewportu draw smysl)
struct VpCount {
    UINT x, y, w, h;
    unsigned n;
};
static VpCount g_vpHist[4];
static int g_nVpHist, g_vpHistLast = -1;
static unsigned g_vpHistOver;
// Pocitadla casti B za interval radku "vykon" (ProxyPartBPerf)
static struct {
    unsigned sceneFrames;   // snimky s koncem sceny (cast A i B)
    unsigned frames;        // z toho cast B
    unsigned suppressed;    // roztazeni potlaceno (most snimek zaradil)
    unsigned extra;         // dalsi potlacene draw se stejnym podpisem
    unsigned notQueued;     // roztazeni nalezeno, most snimek nezaradil -> kreslil engine
    unsigned noUpscale;     // snimek casti B bez draw roztazeni (mat_viewportupscale 0?)
    unsigned between;       // snimky s draw do sceny mezi kopii a roztazenim (roztazeni pak kresli engine)
    unsigned badVp;         // snimky s nepouzitelnym viewportem kamery (bez mostu)
    unsigned otherVpDraws;  // perspektivni draw hlavniho pohledu s jinym viewportem nez render
    unsigned scopeFixed;    // draw masky hledi kuse s opravenym uvOffsets (ScopeFixPre)
    unsigned lightFrames;   // snimky casti B s pruchodem svetel (light buffer MRT, LightsNoteRT)
    unsigned lightPsFixed;  // draw svetel s opravenym viewSize + matCamProjToWorld (LightsFixPre)
    unsigned camParFixed;   // draw s opravenym g_CamParams (cteni light bufferu, LightsFixPre)
    unsigned csmFixed;      // draw stinove masky slunce s opravenou matici (LightsFixPre)
    unsigned csmSkipped;    // draw stinove masky pro jiny pohled (odraz/refrakce vody) - beze zmeny (O4)
    unsigned sunFixed;      // draw paprsku slunce s opravenym _SunPosUV (LightsFixPre)
    unsigned softFixed;     // draw mekkych castic s roztazenou kopii hloubky (SoftDepthPre)
    unsigned softSkipped;   // draw mekkych castic s VS ViewportTransformScaled (spritecard) - bez nahrady hloubky (O2)
    unsigned sunTwin;       // draw zare slunce pres upravenou kopii grfinal (O1, c31 = povrch/render)
    unsigned refractFixed;  // kopie sceny do mensi textury (refrakce) jen ze sub-rectu (H_StretchRect)
} g_stB;
static bool g_lightsPatched;  // cast B: zaplata kontroly light bufferu v client.dll je zapnuta (LightsPatchUpdate)
static bool g_lightsRtSeen;   // v tomto snimku nastaven MRT pruchodu svetel (LightsNoteRT)
// Posledni kopie barvy sceny (StretchRect, napr. do _rt_FullFrameFB) v tomto snimku. Post-process
// shader na konci sceny cte tuto kopii, takze po zapisu vysledku mostu je potreba ji zopakovat.
static struct {
    IDirect3DSurface9* dst;
    RECT srcRect, dstRect;
    bool hasSrc, hasDst;
    D3DTEXTUREFILTERTYPE filter;
    bool valid;
    unsigned drawsAfter;  // draw callu do barvy sceny po teto kopii
} g_sceneCopy;
static void* g_lastTriggerVS;
static unsigned g_statSceneEnd;
static const ShaderInfo* g_curInfo; // CTAB aktualniho vertex shaderu
static void* g_curVS;
static float g_vsConst[256 * 4];    // puvodni hodnoty od hry
static uint8_t g_blkState[256];     // 0 = GPU==hra, 1 = GPU==jitter tohoto snimku, 2 = nevime
static uint64_t g_frame;

// Stin stavu zarizeni pro zaznam MV objektu (src\objmv.inl). Ukazatele nejsou drzene referenci, znamenaji jen
// "prave navazano" (navazane objekty drzi zarizeni); pri zaznamu draw callu se na VB a IB bere AddRef.
static IDirect3DVertexDeclaration9* g_curDecl;  // nullptr = zadna deklarace nebo FVF
static IDirect3DVertexBuffer9* g_curVB0;        // stream 0
static UINT g_curVB0Off, g_curVB0Stride;
static UINT g_curFreq0 = 1;                     // SetStreamSourceFreq(0): 1 = bez instancovani
static IDirect3DIndexBuffer9* g_curIB;
static DWORD g_rsZEnable = TRUE, g_rsZWrite = TRUE, g_rsColorWrite = 0xF, g_rsAlphaBlend = FALSE;
static DWORD g_rsCull = D3DCULL_CCW;
static D3DVIEWPORT9 g_vp;                       // aktualni viewport (SetViewport, SetRenderTarget(0), Reset)
static bool g_vpKnown;

// Vychozi stav zarizeni (nove zarizeni, Reset): bez deklarace, streamu a indexu, vychozi render staty.
static void ResetDrawState() {
    g_curDecl = nullptr;
    g_curVB0 = nullptr;
    g_curVB0Off = g_curVB0Stride = 0;
    g_curFreq0 = 1;
    g_curIB = nullptr;
    g_rsZEnable = g_rsZWrite = TRUE;
    g_rsColorWrite = 0xF;
    g_rsAlphaBlend = FALSE;
    g_rsCull = D3DCULL_CCW;
    g_vpKnown = false;
    g_nOcclOpen = 0;
    g_sceneEndPending = false;
    g_curVS = nullptr;  // Reset vraci vertex shader na NULL (bez SetVertexShader by draw patril staremu shaderu)
    g_curInfo = nullptr;
}

// Jitter
static std::atomic<bool> g_jitterOn{false};  // uzivatelsky prepinac (Ctrl+F11, StartEnabled)
static bool g_jitterActive;              // skutecny stav pro tento snimek (DLAA a ladeni MV ho vynuti)
static bool g_jitterForced;
static float g_jitterScale = 1.0f;       // skutecne meritko (pri vynuceni vzdy 1)
static float g_jitterPxX, g_jitterPxY;  // posun v pixelech renderu (+x doprava, +y dolu)
static float g_jx, g_jy;                // totez v clip prostoru pro viewport g_jitVpW x g_jitVpH
static UINT g_jitVpW, g_jitVpH;         // viewport, pro ktery g_jx / g_jy plati (PreDraw ho meni podle draw)

// Kamera hlavniho pohledu (puvodni, bez jitteru) - pro motion vektory
static float g_cam[16], g_prevCam[16];
static bool g_camSet, g_prevCamValid;

// Kandidati na kameru hlavniho pohledu: ruzne puvodni (bez jitteru) cViewProj perspektivnich drawu
// do plneho RGBA8 cile pred koncem sceny. Source kresli nejdriv 3D skybox (vlastni kamera se stejnou
// orientaci a FOV, ale jinou pozici), pak svet a nakonec zbran v ruce (jine FOV).
struct CamCand {
    float m[16];
    IDirect3DSurface9* rt;
    unsigned draws;
    unsigned firstSeq, lastSeq;  // poradi prvniho a posledniho drawu v ramci snimku
    UINT vpX, vpY, vpW, vpH;     // viewport prvniho drawu (cast B: sub-rect renderu)
    bool vpMixed;                // dalsi draw teze kamery mel jiny viewport
};
static const int kMaxCamCand = 8;
static CamCand g_camCand[kMaxCamCand];
static int g_nCamCand, g_lastCand = -1;
static unsigned g_drawSeq;           // poradi drawu v ramci snimku (od 1)
static unsigned g_zClearSeq;         // g_drawSeq v okamziku posledniho mazani hloubky
static bool g_zClearValid;
static IDirect3DSurface9* g_zClearDS;
static float g_vpMaxZ = 1.0f;        // MaxZ aktualniho viewportu (zbran v ruce 0.1)
static int g_selIdx = -1, g_selCands;
static unsigned g_selDraws;
static bool g_loggedTwin;

// Statistiky (za 300 snimku)
static unsigned g_statDraws, g_statJitterUploads, g_statRestores, g_statMainDraws, g_statNoCam, g_statTwin;

static float Halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

// DLAA a ladeni MV potrebuji skutecny jitter (Scale 1) misto nastaveni Ctrl+F11. DLAA jen kdyz ho
// helper opravdu provadi (pri selhani NGX by jitter jen tresl obrazem). Jitter se pridava jen kdyz
// minuly snimek prosel mostem cely (konec sceny nalezen, helper stihl): kdyz most vypadne (menu,
// zadny konec sceny, timeout), dalsi snimek je bez jitteru a obraz se netrese. Vola se na konci snimku.
static unsigned g_statJitterSkip;
static void UpdateJitterState() {
    uint32_t m = Interop_GetMode();
    bool forced = m == BMSD_MODE_MVDEBUG || (m == BMSD_MODE_DLAA && Interop_DlaaReady());
    if (forced != g_jitterForced) {
        g_jitterForced = forced;
        if (forced)
            Log("jitter: rizen rezimem mostu (scale 1, %d fazi, jen po snimcich, ktere most zpracoval)", g_phases);
        else
            Log("jitter: zpet na uzivatelske nastaveni (%s, scale %.2f)", g_jitterOn ? "zapnuto" : "vypnuto",
                g_cfg.scale);
    }
    if (forced) {
        g_jitterActive = Interop_FrameBridged(g_frame - 1);  // g_frame uz je cislo dalsiho snimku
        if (!g_jitterActive) ++g_statJitterSkip;
    } else {
        g_jitterActive = g_jitterOn;
    }
    g_jitterScale = forced ? 1.0f : g_cfg.scale;
}

// Jitter je v pixelech renderu; do clip prostoru se prevadi podle viewportu draw callu: posun o d NDC je ve
// viewportu sirky w posun o d * w / 2 pixelu (x doprava = +x, y dolu = -y). V casti A je viewport = backbuffer,
// v casti B sub-rect - s delenim velikosti backbufferu by skutecny posun byl jen render / W nasobek hlaseneho.
static void JitterForViewport(UINT w, UINT h, float* jx, float* jy) {
    *jx = w ? 2.0f * g_jitterPxX / (float)w : 0.0f;
    *jy = h ? -2.0f * g_jitterPxY / (float)h : 0.0f;
}

// Pokus (ploty s presety L/M): prah alpha testu (ALPHAREF) se kazdy snimek posune podle sekvence (Halton 5).
// DLSS snimky v historii zprumeruje -> hrana pletiva dostane pokryti pod pixel misto skoku 0/1.
// [dlaa] AlphaJitter = rozkmit (podil z 255), Alt+F6 (DebugKeys=1) zap/vyp. Jen s jitterem (DLSS/FSR bezi).
static float g_alphaJit;      // aktualni rozkmit (0 = vypnuto)
static DWORD g_rsAlphaRef = 0; // posledni ALPHAREF od hry
static int g_alphaOff;        // posun prahu tohoto snimku
static DWORD AlphaRefOut(DWORD base) {
    if (g_alphaJit <= 0 || !g_jitterActive) return base;
    const int v = (int)base + g_alphaOff;
    return (DWORD)(v < 1 ? 1 : v > 255 ? 255 : v);
}
static void UpdateJitter() {
    int i = (int)(g_frame % (uint64_t)g_phases) + 1;  // Halton od 1
    g_alphaOff = g_alphaJit > 0 ? (int)lroundf((Halton(i, 5) - 0.5f) * g_alphaJit * 255.0f) : 0;
    if (g_main && g_rsAlphaRef && o_SetRenderState) o_SetRenderState(g_main, D3DRS_ALPHAREF, AlphaRefOut(g_rsAlphaRef));
    g_jitterPxX = (Halton(i, 2) - 0.5f) * g_jitterScale;
    g_jitterPxY = (Halton(i, 3) - 0.5f) * g_jitterScale;
    // vychozi viewport snimku = cely backbuffer; PreDraw prepocita, kdyz draw kresli do jineho
    g_jitVpW = g_bbW;
    g_jitVpH = g_bbH;
    JitterForViewport(g_bbW, g_bbH, &g_jx, &g_jy);
}

// Jitter do clip-matice po radcich (registry): posun obrazu v clip prostoru o (jx, jy) * w. Jediny kod pro draw
// cally hry (PreDraw) i pro davku MV objektu, aby rasterizace prehranych objektu sedela na hru.
static void JitterMatrixBy(float* m, float jx, float jy) {
    for (int k = 0; k < 4; ++k) {
        m[0 + k] += jx * m[12 + k];  // x' = x + jx*w
        m[4 + k] += jy * m[12 + k];  // y' = y + jy*w
    }
}
static void JitterMatrix(float* m) { JitterMatrixBy(m, g_jx, g_jy); }

// ---------------------------------------------------------------------------
// Zachyceni snimku
// ---------------------------------------------------------------------------
enum { CAP_IDLE, CAP_ARMED, CAP_ACTIVE };
static std::atomic<int> g_capState{CAP_IDLE};
static std::mutex g_capMx;
static std::string g_cap;
static std::unordered_set<void*> g_capShaders;
// Vypsane jednou za soubor zachytu (popis deklarace, VB, IB); VS dump jednou za serii (g_capMx)
static std::unordered_set<void*> g_capDecls, g_capVBs, g_capIBs, g_capVSDumped;
static int g_capIndex;
static unsigned g_capDraws;
static int g_capLeft, g_capSeriesPos;    // zbyvajici snimky serie, poradi snimku v serii (od 1)
static ULONGLONG g_firstPresentMs;       // prvni Present hlavniho zarizeni (pro [capture] AutoAfterSec)
static bool g_autoCapDone;
static bool g_capKeyWas, g_jitterKeyWas, g_bridgeKeyWas, g_presetKeyWas, g_sharpenKeyWas, g_mipBiasKeyWas,
    g_inputModeKeyWas, g_depthModeKeyWas, g_outScaleKeyWas, g_phasesKeyWas, g_downKeyWas, g_pipelineKeyWas,
    g_objMvKeyWas;

// ---------------------------------------------------------------------------
// Mip LOD bias pro DLAA (NVIDIA: log2(render / display) - 1 = -1 pri DLAA)
// ---------------------------------------------------------------------------
// Jen v rezimu DLAA a jen pro perspektivni drawy hlavniho pohledu pred koncem sceny (stejne
// podminky jako jitter), takze HUD, post-process, menu a DLAA vypnute zustanou beze zmeny.
// Proxy si drzi hodnotu D3DSAMP_MIPMAPLODBIAS od hry (pixel samplery 0..15) a hodnotu, ktera je
// prave v zarizeni; SetSamplerState vola jen pri zmene (prechod do/z oblasti s biasem, zmena od hry).
static DWORD g_mipShadow[16];   // hodnota od hry (float jako DWORD)
static DWORD g_mipApplied[16];  // hodnota v zarizeni
static bool g_mipBiasNow;       // v zarizeni je ted hodnota s biasem
static bool g_mipDirty;         // hra menila bias, pri dalsim drawu zkontrolovat
static bool g_mipBiasEnabled;   // [dlaa] MipBias != 0, Ctrl+Delete
static float g_mipBiasValue = -0.5f;  // skutecna hodnota (pri MipBiasMode 1 a S > 1 podle S)
static float g_mipBiasBase = -0.5f;   // [dlaa] MipBias (0 -> -0.5): bias vuci finalnimu obrazu, plati pro kazde S
static bool g_mipBiasOn;        // pro tento snimek (DLAA bezi a most minuly snimek zpracoval)
static unsigned g_statMipCalls;
static unsigned g_statMipNorm, g_statMipAlpha;  // drawy s normalovou mapou / alfa testem pod biasem
static uint16_t g_mipNormNow;  // v zarizeni: maska sampleru s pridavkem MipBiasNormal
static bool g_mipAlphaNow;     // v zarizeni: pridavek MipBiasAlphaTest
static bool g_mipBaseNow;      // v zarizeni: zakladni bias sceny (g_mipBiasValue)
static bool g_mipSceneOn;      // DLAA / FSR bezi s jitterem: pridavky plati i s vypnutym zakladnim biasem (MipBias=0)

// Mip bias podle druhu textury (v25d). Normalove mapy: jemny detail normal dela "jiskreni" odlesku, ktere se meni
// kazdy snimek a DLSS / FSR ho neuklidni - mensi zaporny bias (MipBiasNormal > 0) ho omezi, barevne textury zustanou
// ostre. Sampler pozna podle jmena v CTAB pixel shaderu (H_CreatePS): obsahuje "bump" nebo "normal" (ne "normaliz").
// Alfa test (ploty, mrize, listi): D3DRS_ALPHATESTENABLE v dobe drawu, pridavek MipBiasAlphaTest pro vsechny samplery.
static std::mutex g_mipPsMx;
static std::unordered_map<void*, uint16_t> g_mipPsNorm;  // PS -> maska sampleru normalovych map (jen nenulove)
static std::set<std::string> g_mipSampNames;              // uz zalogovana jmena sampleru (diagnostika)
static bool IsNormalMapName(std::string n) {
    for (char& c : n) c = (char)tolower((unsigned char)c);
    if (n.find("gbuffer") != std::string::npos) return false;  // GBufferNormal = render target bez mip urovni
    return n.find("bump") != std::string::npos ||
           (n.find("normal") != std::string::npos && n.find("normaliz") == std::string::npos);
}
static ShaderInfo ParseShader(const DWORD* code);
static void MipNotePs(void* ps, const DWORD* code) {
    if (!ps || !code || (code[0] >> 16) != 0xFFFF) return;
    const std::string t = ParseShader(code).text + " ";
    uint16_t m = 0;
    std::string fresh;
    {
        std::lock_guard<std::mutex> lk(g_mipPsMx);
        for (size_t pos = t.find(' '); pos != std::string::npos;) {
            const size_t e = t.find(' ', pos + 1);
            if (e == std::string::npos) break;
            const std::string tok = t.substr(pos + 1, e - pos - 1);
            pos = e;
            const size_t q = tok.find("=s");
            if (q == std::string::npos || q == 0) continue;
            const int lo = atoi(tok.c_str() + q + 2);
            const size_t dd = tok.find("..", q);
            const int hi = dd != std::string::npos ? atoi(tok.c_str() + dd + 2) : lo;
            if (lo < 0 || hi > 15 || hi < lo) continue;
            const std::string name = tok.substr(0, q);
            const bool nm = IsNormalMapName(name);
            if (nm)
                for (int r = lo; r <= hi; ++r) m |= (uint16_t)(1u << r);
            if (g_mipSampNames.size() < 400 && g_mipSampNames.insert(name).second) fresh += " " + name + (nm ? "(N)" : "");
        }
        if (m) g_mipPsNorm[ps] = m;
        else g_mipPsNorm.erase(ps);
    }
    if (!fresh.empty()) Log("mip bias: nove samplery PS (N = normalova mapa):%s", fresh.c_str());
}
// Druh drawu pro mip bias: maska normalovych map aktualniho PS a alfa test (jen kdyz je pridavek nenulovy)
static void MipDrawKind(IDirect3DDevice9* d, uint16_t* nmask, bool* alpha) {
    *nmask = 0;
    *alpha = false;
    if (g_cfg.mipBiasNormal != 0.0f) {
        IDirect3DPixelShader9* ps = nullptr;
        if (SUCCEEDED(d->GetPixelShader(&ps)) && ps) {
            {
                std::lock_guard<std::mutex> lk(g_mipPsMx);
                auto it = g_mipPsNorm.find(ps);
                if (it != g_mipPsNorm.end()) *nmask = it->second;
            }
            ps->Release();
        }
    }
    if (g_cfg.mipBiasAlpha != 0.0f) {
        DWORD v = 0;
        if (SUCCEEDED(d->GetRenderState(D3DRS_ALPHATESTENABLE, &v))) *alpha = v != 0;
    }
    if (*nmask) ++g_statMipNorm;
    if (*alpha) ++g_statMipAlpha;
}
// Cast B (Alt+Del cykli): 0 = log2(r) + [dlaa] MipBias (r = render / vystup; vychozi -1 -> -1.58 pri 0.667, vuci
// finalnimu obrazu stejna hustota texelu jako -1 v DLAA), 1 = [dlaa] MipBias jako v DLAA (-1), 2 = log2(r)
// (hustota jako nativni obraz bez biasu, -0.58), 3 = vypnuto. Vstup je ridsi, takze ostrejsi bias pletivo vic
// aliasuje - co je klidnejsi, ukaze test na plotu (docs\advokat-upscaling-B2.md, drobne 6).
static int g_mipBIdx;
static float MipBiasB(int idx, float r) {
    const float lr = log2f(r);
    if (idx == 0) return std::min(lr, std::max(lr - 1.0f, lr + g_mipBiasBase));  // pasmo z Guide 3.5
    if (idx == 1) return g_mipBiasBase;
    return idx == 2 ? lr : 0.0f;
}
static float RenderRatio() {
    return g_pathB && g_renderW && g_renderSurfW ? (float)g_renderW / (float)g_renderSurfW : 1.0f;
}
static bool MipBiasWanted() { return g_pathB ? g_mipBIdx != 3 : g_mipBiasEnabled; }

static void ApplyMipBias(IDirect3DDevice9* d, bool want, uint16_t nmask = 0, bool alpha = false, bool base = true) {
    if (!want) {
        nmask = 0;
        alpha = false;
        base = false;
    }
    if ((want == g_mipBiasNow && nmask == g_mipNormNow && alpha == g_mipAlphaNow && base == g_mipBaseNow &&
         !g_mipDirty) ||
        !o_SetSamplerState)
        return;
    for (DWORD s = 0; s < 16; ++s) {
        DWORD v = g_mipShadow[s];
        if (want) {
            float f;
            memcpy(&f, &v, 4);
            f += (base ? g_mipBiasValue : 0.0f) + (alpha ? g_cfg.mipBiasAlpha : 0.0f) + (((nmask >> s) & 1u) ? g_cfg.mipBiasNormal : 0.0f);
            memcpy(&v, &f, 4);
        }
        if (v != g_mipApplied[s]) {
            o_SetSamplerState(d, s, D3DSAMP_MIPMAPLODBIAS, v);
            g_mipApplied[s] = v;
            ++g_statMipCalls;
        }
    }
    g_mipBiasNow = want;
    g_mipNormNow = nmask;
    g_mipAlphaNow = alpha;
    g_mipBaseNow = base;
    g_mipDirty = false;
}

// Po Reset ma zarizeni vychozi stav (bias 0).
static void ResetMipState() {
    memset(g_mipShadow, 0, sizeof g_mipShadow);
    memset(g_mipApplied, 0, sizeof g_mipApplied);
    g_mipBiasNow = g_mipDirty = false;
}

// Pocet fazi jitteru a mip bias podle S, se kterym DLSS skutecne bezi (potvrzuje helper; mimo DLAA a pri
// fallbacku 1). Jitter zustava v pixelech vstupu. Vola se na konci snimku pred UpdateJitterState.
static void UpdateScaleState() {
    const bool dlaa = Interop_GetMode() == BMSD_MODE_DLAA && Interop_DlaaReady();
    const float s = dlaa ? Interop_ActiveOutScale() : 1.0f;
    // Cast B: vystup DLSS = povrch sceny (S se nepouziva), pomer vystup / vstup dava render
    const bool partB = g_pathB && dlaa;
    const float r = partB ? RenderRatio() : 1.0f;  // render / vystup
    const float ratio = partB ? 1.0f / r : s;      // vystup / vstup DLSS
    // [jitter] Phases=0 (nebo Alt+F2 auto): Guide 3.7.1.1 doporucuje 8 * (Out/In)^2 fazi (4/3 -> 14, 1.5 -> 18,
    // 2 -> 32, 3 -> 72; cast B: 0.667 -> 18, 0.58 -> 24, 0.5 -> 32). Jediny klic pro obe casti, vychozi pevnych 8
    // (testy na plotu, viz Config::phases). Helper pri zmene poctu resetuje historii DLSS (jiz se neopakuje stejna
    // sekvence).
    const bool autoPh = g_phasesSetting <= 0;
    const int phases = autoPh ? std::min(72, std::max(8, (int)lroundf(8.0f * ratio * ratio))) : g_phasesSetting;
    if (phases != g_phases || autoPh != g_phasesAuto || !g_phasesLogged) {
        g_phases = phases;
        g_phasesAuto = autoPh;
        g_phasesLogged = true;
        if (autoPh && partB)
            Log("jitter: auto 8*(vystup/vstup)^2 = %d fazi (cast B, render %.3f)", phases, r);
        else if (autoPh)
            Log("jitter: auto 8*S^2 = %d fazi (S=%s)", phases, ScaleStr(s).c_str());
        else
            Log("jitter: %d fazi (rucne)", phases);
    }
    Interop_SetJitterPhases((uint32_t)phases, autoPh);
    if (g_pathB) {
        // Cast B: bias podle pomeru renderu (hodnota z minuleho snimku s koncem sceny), viz g_mipBIdx. Vypnuto =
        // hodnota se nemeni (ApplyMipBias ji nepouzije).
        if (g_mipBIdx == 3) return;
        const float rr = RenderRatio();
        const float mb = MipBiasB(g_mipBIdx, rr);
        if (mb != g_mipBiasValue) {
            static const char* how[3] = {"log2(render/vystup) + [dlaa] MipBias", "[dlaa] MipBias jako v DLAA",
                                         "log2(render/vystup)"};
            g_mipBiasValue = mb;
            g_mipDirty = true;
            Log("mip bias: %.2f (cast B, render %.3f: %s)", mb, rr, how[g_mipBIdx]);
        }
        return;
    }
    // Mip bias: vychozi [dlaa] MipBias vuci finalnimu obrazu pro kazde S (-1 lezi pro S <= 2 v pasmu
    // [log2(In/Out) - 1, log2(In/Out)] z Guide 3.5 a nezhorsuje moire); MipBiasMode 1 = log2(In/Out) - 1 (A/B).
    const bool byScale = g_cfg.mipBiasMode == 1 && s > 1.0001f;
    const float mb = byScale ? -log2f(s) - 1.0f : g_mipBiasBase;
    if (mb != g_mipBiasValue) {
        g_mipBiasValue = mb;
        g_mipDirty = true;  // ApplyMipBias pri dalsim drawu prepise hodnoty v zarizeni
        if (g_mipBiasEnabled)
            Log("mip bias: %.2f (%s, S=%s)", mb, byScale ? "log2(In/Out) - 1" : "[dlaa] MipBias", ScaleStr(s).c_str());
    }
}

const wchar_t* ProxyDir() { return g_dir; }

static void Cap(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    std::lock_guard<std::mutex> lk(g_capMx);
    g_cap.append(buf, std::min<size_t>((size_t)n, sizeof buf - 1));
    g_cap.push_back('\n');
}

static void CapVec(const char* prefix, UINT reg, const float* v) {
    Cap("%s c%-3u % .6f % .6f % .6f % .6f", prefix, reg, v[0], v[1], v[2], v[3]);
}

static void CapMatrix(const char* name, const float* m) {
    for (int r = 0; r < 4; ++r)
        Cap("# %s[%d] % .6f % .6f % .6f % .6f", name, r, m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
}

// Vlozi p do mnoziny zachytu; true = jeste tam nebyl (vypsat popis).
static bool CapFirst(std::unordered_set<void*>& set, void* p) {
    std::lock_guard<std::mutex> lk(g_capMx);
    return set.insert(p).second;
}

static const char* DeclTypeName(unsigned t) {
    static const char* n[] = {"FLOAT1",   "FLOAT2",   "FLOAT3",    "FLOAT4",    "D3DCOLOR", "UBYTE4",
                              "SHORT2",   "SHORT4",   "UBYTE4N",   "SHORT2N",   "SHORT4N",  "USHORT2N",
                              "USHORT4N", "UDEC3",    "DEC3N",     "FLOAT16_2", "FLOAT16_4", "UNUSED"};
    return t < sizeof n / sizeof n[0] ? n[t] : "?";
}
static const char* DeclUsageName(unsigned u) {
    static const char* n[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL",    "PSIZE", "TEXCOORD", "TANGENT",
                              "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",     "FOG",   "DEPTH",    "SAMPLE"};
    return u < sizeof n / sizeof n[0] ? n[u] : "?";
}
static const char* PoolName(unsigned p) {
    static const char* n[] = {"DEFAULT", "MANAGED", "SYSTEMMEM", "SCRATCH"};
    return p < 4 ? n[p] : "?";
}
// usage bufferu: "DYNAMIC|WRITEONLY" (ostatni bity jen v hex)
static std::string UsageStr(DWORD u) {
    std::string s;
    if (u & D3DUSAGE_DYNAMIC) s += "DYNAMIC";
    if (u & D3DUSAGE_WRITEONLY) s += s.empty() ? "WRITEONLY" : "|WRITEONLY";
    if (u & D3DUSAGE_SOFTWAREPROCESSING) s += s.empty() ? "SOFTWAREPROCESSING" : "|SOFTWAREPROCESSING";
    return s.empty() ? "-" : s;
}

// VD <decl>; pri prvnim vyskytu v souboru i prvky: VDE <decl> <i> stream= off= type= method= usage= idx=
static void CapDecl(IDirect3DVertexDeclaration9* decl) {
    Cap("VD %p", (void*)decl);
    if (!decl || !CapFirst(g_capDecls, decl)) return;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = 0;
    if (FAILED(decl->GetDeclaration(nullptr, &n)) || n == 0 || n > MAXD3DDECLLENGTH + 1 ||
        FAILED(decl->GetDeclaration(el, &n))) {
        Cap("# VD %p: GetDeclaration selhalo", (void*)decl);
        return;
    }
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
        Cap("VDE %p %u stream=%u off=%u type=%s method=%u usage=%s idx=%u", (void*)decl, i, el[i].Stream,
            el[i].Offset, DeclTypeName(el[i].Type), el[i].Method, DeclUsageName(el[i].Usage), el[i].UsageIndex);
}

// SS <stream> vb= off= stride=; pri prvnim vyskytu VB v souboru i VBD <vb> usage= pool= size= fvf=
static void CapStream(UINT stream, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) {
    Cap("SS %u vb=%p off=%u stride=%u", stream, (void*)vb, off, stride);
    if (!vb || !CapFirst(g_capVBs, vb)) return;
    D3DVERTEXBUFFER_DESC desc = {};
    if (FAILED(vb->GetDesc(&desc))) return;
    Cap("VBD %p usage=0x%X(%s) pool=%s size=%u fvf=0x%X", (void*)vb, desc.Usage, UsageStr(desc.Usage).c_str(),
        PoolName(desc.Pool), desc.Size, desc.FVF);
}

// IB <ib>; pri prvnim vyskytu v souboru i IBD <ib> fmt= usage= pool= size=
static void CapIndices(IDirect3DIndexBuffer9* ib) {
    Cap("IB %p", (void*)ib);
    if (!ib || !CapFirst(g_capIBs, ib)) return;
    D3DINDEXBUFFER_DESC desc = {};
    if (FAILED(ib->GetDesc(&desc))) return;
    Cap("IBD %p fmt=%s usage=0x%X(%s) pool=%s size=%u", (void*)ib,
        desc.Format == D3DFMT_INDEX16 ? "INDEX16" : desc.Format == D3DFMT_INDEX32 ? "INDEX32" : "?", desc.Usage,
        UsageStr(desc.Usage).c_str(), PoolName(desc.Pool), desc.Size);
}

// RS <stav> 0x<hodnota> (u DEPTHBIAS a SLOPESCALEDEPTHBIAS i float); jen vybrane stavy, ostatni nic
static const D3DRENDERSTATETYPE kCapRS[] = {
    D3DRS_ZENABLE,           D3DRS_ZWRITEENABLE,       D3DRS_ZFUNC,           D3DRS_ALPHABLENDENABLE,
    D3DRS_ALPHATESTENABLE,   D3DRS_COLORWRITEENABLE,   D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
    D3DRS_COLORWRITEENABLE3, D3DRS_DEPTHBIAS,          D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_CULLMODE,
    D3DRS_SRGBWRITEENABLE,   D3DRS_STENCILENABLE,      D3DRS_SRCBLEND,        D3DRS_DESTBLEND,
    D3DRS_BLENDOP,           D3DRS_SEPARATEALPHABLENDENABLE};
static const char* RSName(D3DRENDERSTATETYPE st) {
    switch (st) {
    case D3DRS_ZENABLE: return "ZENABLE";
    case D3DRS_ZWRITEENABLE: return "ZWRITEENABLE";
    case D3DRS_ZFUNC: return "ZFUNC";
    case D3DRS_ALPHABLENDENABLE: return "ALPHABLENDENABLE";
    case D3DRS_ALPHATESTENABLE: return "ALPHATESTENABLE";
    case D3DRS_COLORWRITEENABLE: return "COLORWRITEENABLE";
    case D3DRS_COLORWRITEENABLE1: return "COLORWRITEENABLE1";
    case D3DRS_COLORWRITEENABLE2: return "COLORWRITEENABLE2";
    case D3DRS_COLORWRITEENABLE3: return "COLORWRITEENABLE3";
    case D3DRS_DEPTHBIAS: return "DEPTHBIAS";
    case D3DRS_SLOPESCALEDEPTHBIAS: return "SLOPESCALEDEPTHBIAS";
    case D3DRS_CULLMODE: return "CULLMODE";
    case D3DRS_SRGBWRITEENABLE: return "SRGBWRITEENABLE";
    case D3DRS_STENCILENABLE: return "STENCILENABLE";
    case D3DRS_SRCBLEND: return "SRCBLEND";
    case D3DRS_DESTBLEND: return "DESTBLEND";
    case D3DRS_BLENDOP: return "BLENDOP";
    case D3DRS_SEPARATEALPHABLENDENABLE: return "SEPARATEALPHABLENDENABLE";
    default: return nullptr;
    }
}
static void CapRS(D3DRENDERSTATETYPE st, DWORD v) {
    const char* name = RSName(st);
    if (!name) return;
    if (st == D3DRS_DEPTHBIAS || st == D3DRS_SLOPESCALEDEPTHBIAS) {
        float f;
        memcpy(&f, &v, 4);
        Cap("RS %s 0x%X (%g)", name, v, f);
    } else {
        Cap("RS %s 0x%X", name, v);
    }
}

// Bytecode VS (GetFunction) do bms_dlss_vs_<ptr>.bin vedle zachytu, jednou za serii.
static void DumpVS(IDirect3DVertexShader9* s) {
    if (!s || !CapFirst(g_capVSDumped, s)) return;
    UINT size = 0;
    if (FAILED(s->GetFunction(nullptr, &size)) || !size) {
        Cap("# VS %p: GetFunction selhalo", (void*)s);
        return;
    }
    std::vector<uint8_t> code(size);
    if (FAILED(s->GetFunction(code.data(), &size))) {
        Cap("# VS %p: GetFunction selhalo", (void*)s);
        return;
    }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\bms_dlss_vs_%p.bin", g_dir, (void*)s);
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        fwrite(code.data(), 1, size, f);
        fclose(f);
        Cap("# VS %p: bytecode %u B -> bms_dlss_vs_%p.bin", (void*)s, size, (void*)s);
    } else {
        Cap("# VS %p: CHYBA zapisu bytecode", (void*)s);
    }
}

// Draw do barvy sceny po konci sceny (post-process, roztazeni v casti B, HUD): co presne cte. Textury vsech 16
// stage, pixel shader (CTAB jednou za soubor, bytecode jednou za serii do bms_dlss_ps_<ptr>.bin) a nenulove PS
// konstanty c0..c31 (zrno, vinetace a nahodne posuny jsou v nich; porovnat mezi snimky serie). Jen v zachytu.
static void CapPostDraw(IDirect3DDevice9* d) {
    for (DWORD stage = 0; stage < 16; ++stage) {
        IDirect3DBaseTexture9* t = nullptr;
        if (FAILED(d->GetTexture(stage, &t)) || !t) continue;
        IDirect3DSurface9* lvl0 = nullptr;
        D3DSURFACE_DESC desc = {};
        if (t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(((IDirect3DTexture9*)t)->GetSurfaceLevel(0, &lvl0)) && lvl0)
            lvl0->GetDesc(&desc);
        Cap("#   t%u %p (povrch %p %ux%u fmt=%u, typ %d)", stage, (void*)t, (void*)lvl0, desc.Width, desc.Height,
            (unsigned)desc.Format, (int)t->GetType());
        if (lvl0) lvl0->Release();
        t->Release();
    }
    IDirect3DPixelShader9* ps = nullptr;
    if (SUCCEEDED(d->GetPixelShader(&ps)) && ps) {
        const bool first = CapFirst(g_capShaders, ps);
        const bool dump = CapFirst(g_capVSDumped, ps);
        std::vector<uint8_t> code;
        UINT size = 0;
        if ((first || dump) && SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8) {
            code.resize((size + 3) & ~3u);
            if (FAILED(ps->GetFunction(code.data(), &size))) code.clear();
        }
        if (first && !code.empty())
            Cap("#   ps %p %s", (void*)ps, ParseShader((const DWORD*)code.data()).text.c_str());
        else
            Cap("#   ps %p", (void*)ps);
        if (dump && !code.empty()) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%s\\bms_dlss_ps_%p.bin", g_dir, (void*)ps);
            if (FILE* f = _wfopen(path, L"wb")) {
                fwrite(code.data(), 1, size, f);
                fclose(f);
                Cap("#   ps %p: bytecode %u B -> bms_dlss_ps_%p.bin", (void*)ps, size, (void*)ps);
            }
        }
        ps->Release();
    } else {
        Cap("#   ps (zadny)");
    }
    float c[32 * 4] = {};
    if (SUCCEEDED(d->GetPixelShaderConstantF(0, c, 32)))
        for (UINT r = 0; r < 32; ++r) {
            const float* v = &c[r * 4];
            if (v[0] || v[1] || v[2] || v[3]) Cap("#   pc%-2u % .6f % .6f % .6f % .6f", r, v[0], v[1], v[2], v[3]);
        }
}

// Stav pixel shaderu pred kazdym draw zachytu (i uvnitr sceny: G-buffer, stinova maska, svetla, dopredny pruchod):
//   P <PS> [CTAB]   novy PS (CTAB jednou za soubor, bytecode jednou za serii do bms_dlss_ps_<PS>.bin)
//   PC c<reg> ...   zmenene PS konstanty c0..c223 proti minulemu draw (prvni draw souboru: vsechny nenulove)
//   PB <16 bitu>    zmenene bool konstanty b0..b15
// Get* pred draw (proxy SetPixelShader ani SetPixelShaderConstantF nehookuje). Jen v zachytu.
static const UINT kCapPsRegs = 224;
static void* g_capLastPS;
static float g_capPc[kCapPsRegs * 4];
static BOOL g_capPb[16];
static void CapPsReset() {
    g_capLastPS = (void*)1;  // prvni draw souboru vzdy vypise PS
    memset(g_capPc, 0, sizeof g_capPc);
    memset(g_capPb, 0, sizeof g_capPb);
}
static void CapPsState(IDirect3DDevice9* d) {
    IDirect3DPixelShader9* ps = nullptr;
    if (SUCCEEDED(d->GetPixelShader(&ps)) && (void*)ps != g_capLastPS) {
        g_capLastPS = ps;
        if (!ps) {
            Cap("P 00000000");
        } else {
            const bool first = CapFirst(g_capShaders, ps);
            const bool dump = CapFirst(g_capVSDumped, ps);
            std::vector<uint8_t> code;
            UINT size = 0;
            if ((first || dump) && SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size <= 256 * 1024) {
                code.resize((size + 3) & ~3u);
                if (FAILED(ps->GetFunction(code.data(), &size))) code.clear();
            }
            if (first && !code.empty())
                Cap("P %p %s", (void*)ps, ParseShader((const DWORD*)code.data()).text.c_str());
            else
                Cap("P %p", (void*)ps);
            if (dump && !code.empty()) {
                wchar_t path[MAX_PATH];
                swprintf(path, MAX_PATH, L"%s\\bms_dlss_ps_%p.bin", g_dir, (void*)ps);
                if (FILE* f = _wfopen(path, L"wb")) {
                    fwrite(code.data(), 1, size, f);
                    fclose(f);
                    Cap("# PS %p: bytecode %u B -> bms_dlss_ps_%p.bin", (void*)ps, size, (void*)ps);
                }
            }
        }
    }
    if (ps) ps->Release();
    float c[kCapPsRegs * 4];
    if (SUCCEEDED(d->GetPixelShaderConstantF(0, c, kCapPsRegs)))
        for (UINT r = 0; r < kCapPsRegs; ++r) {
            const float* v = &c[r * 4];
            if (!memcmp(v, &g_capPc[r * 4], 16)) continue;
            memcpy(&g_capPc[r * 4], v, 16);
            Cap("PC c%-3u % .6f % .6f % .6f % .6f", r, v[0], v[1], v[2], v[3]);
        }
    BOOL b[16] = {};
    if (SUCCEEDED(d->GetPixelShaderConstantB(0, b, 16)) && memcmp(b, g_capPb, sizeof b)) {
        memcpy(g_capPb, b, sizeof b);
        char s[17];
        for (int i = 0; i < 16; ++i) s[i] = b[i] ? '1' : '0';
        s[16] = 0;
        Cap("PB %s", s);
    }
}

static UINT PrimVerts(D3DPRIMITIVETYPE t, UINT n) {
    if (t == D3DPT_TRIANGLESTRIP || t == D3DPT_TRIANGLEFAN) return n + 2;
    return t == D3DPT_TRIANGLELIST && n <= 2 ? n * 3 : 0;
}

// Vrcholy maleho draw (celoobrazovkovy obdelnik: UV a pozice v obdelniku rozhoduji, jakou cast textur cte).
// Stream 0 (Lock READONLY, jen v zachytu) nebo ukazatel UP. Radek: QV <i> <float...> (nejvys 16 floatu).
static void CapQuadVerts(IDirect3DDevice9* d, const void* up, UINT upStride, UINT first, UINT count) {
    if (count == 0 || count > 6) return;
    const uint8_t* p = (const uint8_t*)up;
    UINT stride = upStride;
    IDirect3DVertexBuffer9* vb = nullptr;
    void* locked = nullptr;
    if (!p) {
        UINT off = 0;
        if (FAILED(d->GetStreamSource(0, &vb, &off, &stride)) || !vb || stride < 8 || stride > 256) {
            if (vb) vb->Release();
            return;
        }
        D3DVERTEXBUFFER_DESC desc = {};
        const UINT start = off + first * stride, bytes = count * stride;
        if (FAILED(vb->GetDesc(&desc)) || start + bytes > desc.Size ||
            FAILED(vb->Lock(start, bytes, &locked, D3DLOCK_READONLY)) || !locked) {
            Cap("# QV: vb %p nelze cist", (void*)vb);
            vb->Release();
            return;
        }
        p = (const uint8_t*)locked;
    } else if (stride < 8 || stride > 256) {
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        float f[16];
        const UINT nf = std::min(16u, stride / 4);
        memcpy(f, p + i * stride, nf * 4);
        char line[400];
        int n = snprintf(line, sizeof line, "QV %u", i);
        for (UINT k = 0; k < nf && n > 0 && n < (int)sizeof line - 16; ++k) n += snprintf(line + n, sizeof line - n, " %g", f[k]);
        Cap("%s", line);
    }
    if (vb) {
        vb->Unlock();
        vb->Release();
    }
}

// Behavior flags zarizeni (GetCreationParameters)
static std::string CreateFlagsStr(DWORD f) {
    static const struct { DWORD bit; const char* name; } kFlags[] = {
        {0x2, "FPU_PRESERVE"}, {0x4, "MULTITHREADED"}, {0x10, "PUREDEVICE"}, {0x20, "SOFTWARE_VERTEXPROCESSING"},
        {0x40, "HARDWARE_VERTEXPROCESSING"}, {0x80, "MIXED_VERTEXPROCESSING"}, {0x100, "DISABLE_DRIVER_MANAGEMENT"},
        {0x200, "ADAPTERGROUP_DEVICE"}, {0x400, "DISABLE_DRIVER_MANAGEMENT_EX"}, {0x800, "NOWINDOWCHANGES"},
        {0x2000, "DISABLE_PSGP_THREADING"}, {0x4000, "ENABLE_PRESENTSTATS"}, {0x8000, "DISABLE_PRINTSCREEN"},
        {0x10000000, "SCREENSAVER"}};
    std::string s;
    for (const auto& k : kFlags)
        if (f & k.bit) {
            if (!s.empty()) s += ' ';
            s += k.name;
        }
    return s;
}

// Stav zarizeni na zacatku snimku pres Get* (nastaveny hrou pred zachytem). Stejny format radku jako hooky.
static void CapInitialState(IDirect3DDevice9* d) {
    Cap("# ---- pocatecni stav zarizeni (Get*, ne volani hry) ----");
    if (g_curVS) {
        IDirect3DVertexShader9* vs = (IDirect3DVertexShader9*)g_curVS;
        const ShaderInfo* info = g_curInfo;
        if (CapFirst(g_capShaders, vs)) Cap("V %p %s", (void*)vs, info ? info->text.c_str() : "(neznamy)");
        DumpVS(vs);
    }
    IDirect3DVertexDeclaration9* decl = nullptr;
    HRESULT hr = d->GetVertexDeclaration(&decl);
    if (SUCCEEDED(hr)) {
        CapDecl(decl);
        if (decl) decl->Release();
    } else {
        Cap("# GetVertexDeclaration selhalo (0x%08X)", (unsigned)hr);
    }
    DWORD fvf = 0;
    if (SUCCEEDED(d->GetFVF(&fvf)) && fvf) Cap("FVF 0x%X", fvf);
    for (UINT s = 0; s < 16; ++s) {
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0, freq = 1;
        if (SUCCEEDED(d->GetStreamSource(s, &vb, &off, &stride)) && vb) {
            CapStream(s, vb, off, stride);
            vb->Release();
        }
        if (SUCCEEDED(d->GetStreamSourceFreq(s, &freq)) && freq != 1) Cap("SSF %u 0x%08X", s, freq);
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    if (SUCCEEDED(d->GetIndices(&ib))) {
        CapIndices(ib);
        if (ib) ib->Release();
    }
    for (D3DRENDERSTATETYPE st : kCapRS) {
        DWORD v = 0;
        hr = d->GetRenderState(st, &v);
        if (SUCCEEDED(hr)) CapRS(st, v);
        else Cap("# GetRenderState %s selhalo (0x%08X)", RSName(st), (unsigned)hr);
    }
    for (DWORD stage = D3DVERTEXTEXTURESAMPLER0; stage <= D3DVERTEXTEXTURESAMPLER3; ++stage) {
        IDirect3DBaseTexture9* t = nullptr;
        if (SUCCEEDED(d->GetTexture(stage, &t)) && t) {
            Cap("T%u %p", stage, (void*)t);
            t->Release();
        }
    }
}

// Alt+F10 nebo AutoAfterSec: serie [capture] Frames po sobe jdoucich snimku, zacne s dalsim snimkem.
static void ArmCapture(const char* why) {
    g_capLeft = g_cfg.capFrames;
    g_capSeriesPos = 0;
    {
        std::lock_guard<std::mutex> lk(g_capMx);
        g_capVSDumped.clear();
    }
    g_capState = CAP_ARMED;
    MessageBeep(MB_OK);
    Log("capture %d: pripraveno (%s, %d snimku)", g_capIndex, why, g_capLeft);
}

static void StartCapture() {
    {
        std::lock_guard<std::mutex> lk(g_capMx);
        g_cap.clear();
        g_capShaders.clear();
        g_capDecls.clear();
        g_capVBs.clear();
        g_capIBs.clear();
    }
    CapPsReset();
    g_capDraws = 0;
    ++g_capSeriesPos;
    Cap("# bms-dlss capture, frame %llu, backbuffer %ux%u, jitter=%d (%.4f, %.4f) px, most=%u, serie %d/%d",
        (unsigned long long)g_frame, g_bbW, g_bbH, (int)g_jitterActive, g_jitterPxX, g_jitterPxY,
        Interop_GetMode(), g_capSeriesPos, g_cfg.capFrames);
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (g_main && SUCCEEDED(g_main->GetCreationParameters(&cp)))
        Cap("# zarizeni: adapter=%u type=%u flags=0x%X (%s)", cp.AdapterOrdinal, (unsigned)cp.DeviceType,
            cp.BehaviorFlags, CreateFlagsStr(cp.BehaviorFlags).c_str());
    Cap("# Legenda: C=zapis VS konstant (puvodni hodnoty), V=novy vertex shader + CTAB,");
    Cap("#          RT/DS=render target/depth, CLR=clear, VP=viewport, S=stav registru na zacatku snimku,");
    Cap("#          SR=StretchRect (src -> dst, sr/dr = recty nebo '-' pro cely povrch), T=SetTexture (stage,");
    Cap("#            textura, povrch urovne 0; stage 0..15 a vertex samplery 257..260 = D3DVERTEXTEXTURESAMPLER0..3)");
    Cap("#   U draw do barvy sceny po konci sceny (post-process, roztazeni, HUD) nasleduji radky '#   t<stage>',");
    Cap("#     '#   ps <PS> [CTAB]' a '#   pc<reg>' = textury vsech 16 stage, pixel shader a nenulove PS konstanty");
    Cap("#     c0..c31 pres Get* (bytecode PS: bms_dlss_ps_<PS>.bin). '# potlaceno' = draw proxy nepustila dal");
    Cap("#     (cast B: roztazeni sceny nahradil vystup DLSS).");
    Cap("#   D=draw, vzdy 'prims=<PrimCount> vs=<VS> t=<D3DPRIMITIVETYPE>' a dale podle volani:");
    Cap("#     D    start=<StartVertex>                                          (DrawPrimitive)");
    Cap("#     DI   base=<BaseVertexIndex> min=<MinVertexIndex> nv=<NumVertices> start=<StartIndex>");
    Cap("#     DUP  stride=<VertexStreamZeroStride>                              (DrawPrimitiveUP)");
    Cap("#     DIUP min=<MinVertexIndex> nv=<NumVertices> ifmt=<D3DFORMAT indexu> stride=<stride>");
    Cap("#   VD <decl>                                   SetVertexDeclaration");
    Cap("#   VDE <decl> <i> stream= off= type= method= usage= idx=   prvek deklarace (jednou na deklaraci a soubor)");
    Cap("#   FVF 0x<fvf>                                 SetFVF");
    Cap("#   SS <stream> vb= off= stride=                SetStreamSource");
    Cap("#   VBD <vb> usage=0x<hex>(DYNAMIC|WRITEONLY|-) pool= size= fvf=   popis VB (jednou na VB a soubor)");
    Cap("#   SSF <stream> 0x<setting>                    SetStreamSourceFreq");
    Cap("#   IB <ib>                                     SetIndices");
    Cap("#   IBD <ib> fmt=INDEX16|INDEX32 usage= pool= size=   popis IB (jednou na IB a soubor)");
    Cap("#   RS <stav> 0x<hodnota> [(float)]             SetRenderState, jen ZENABLE, ZWRITEENABLE, ZFUNC,");
    Cap("#     ALPHABLENDENABLE, ALPHATESTENABLE, COLORWRITEENABLE..3, DEPTHBIAS, SLOPESCALEDEPTHBIAS, CULLMODE,");
    Cap("#     SRGBWRITEENABLE, STENCILENABLE, SRCBLEND, DESTBLEND, BLENDOP, SEPARATEALPHABLENDENABLE");
    Cap("#   Pred 'zacatek snimku' je pocatecni stav (V, VD, FVF, SS, SSF, IB, RS, T25x) ziskany pres Get*.");
    Cap("#   Bytecode kazdeho VS ze serie: bms_dlss_vs_<VS>.bin (tools\\disasm_vs.ps1 -> .asm)");
    Cap("#   Pred kazdym D (Get*): P <PS> [CTAB] = jiny pixel shader (bytecode bms_dlss_ps_<PS>.bin), PC c<reg> =");
    Cap("#     zmenena PS konstanta c0..c223 proti minulemu draw (prvni draw: vsechny nenulove), PB = bool b0..b15;");
    Cap("#     QV <i> <floaty> = vrcholy maleho draw (<= 6 vrcholu, stream 0 nebo UP).");
    if (g_prevCamValid) CapMatrix("camPrev", g_prevCam);  // kamera minuleho snimku
    for (UINT r = 0; r < 256; ++r) {
        const float* v = &g_vsConst[r * 4];
        if (v[0] || v[1] || v[2] || v[3]) CapVec("S", r, v);
    }
    if (g_main) CapInitialState(g_main);
    Cap("# ---- zacatek snimku ----");
    g_capState = CAP_ACTIVE;
    Log("capture %d: zacinam (frame %llu, snimek %d/%d)", g_capIndex, (unsigned long long)g_frame, g_capSeriesPos,
        g_cfg.capFrames);
}

// Konec snimku zachytu: soubor se zapise hned (pameti 32bitove hry neubyva); dalsi snimek serie zacne v tomtez
// OnFrameEnd (CAP_ARMED -> StartCapture), takze snimky jdou po sobe.
static void FinishCapture() {
    g_capState = CAP_IDLE;
    if (g_camSet) CapMatrix("camThis", g_cam);
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_capMx);
        text.swap(g_cap);
    }
    text += "# ---- konec snimku, draw callu: " + std::to_string(g_capDraws) + " ----\n";
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\bms_dlss_capture_%03d.txt", g_dir, g_capIndex);
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
        Log("capture %d: ulozeno %zu B, %u draw callu", g_capIndex, text.size(), g_capDraws);
    } else {
        Log("capture %d: CHYBA zapisu souboru", g_capIndex);
    }
    ++g_capIndex;
    if (--g_capLeft > 0) {
        g_capState = CAP_ARMED;
        return;
    }
    MessageBeep(MB_ICONASTERISK);
}

// Kratke stisky klaves modu mezi dvema snimky (nacitani, nizke FPS): hacek klavesnice (menu.inl, MkDecide) klavesu
// spolkne a nastavi jeji bit (i modifikatoru); OnFrameEnd si je na zacatku snimku vezme (KeyTapsBeginFrame) a KeyDown
// je v tomto snimku hlasi jako stisknute, i kdyz uz jsou pustene.
static std::atomic<uint32_t> g_keyTapped[8];
static uint32_t g_keyTapFrame[8];
static void KeyTapsBeginFrame() {
    for (int i = 0; i < 8; ++i) g_keyTapFrame[i] = g_keyTapped[i].exchange(0, std::memory_order_relaxed);
}
static void KeyTap(int vk) {
    if (vk > 0 && vk < 256) g_keyTapped[vk >> 5].fetch_or(1u << (vk & 31), std::memory_order_relaxed);
}
static std::atomic<int> g_reportDone{0};  // menu: hlaseni chyby ulozeno (OSD_REPORT / _FAIL), vlakno kopirovani
// vk 0 = klavesa vypnuta ([keys] DebugKeys=0): nikdy stisknuta
static bool KeyDown(int vk) {
    return vk > 0 && vk < 256 && ((GetAsyncKeyState(vk) & 0x8000) != 0 || ((g_keyTapFrame[vk >> 5] >> (vk & 31)) & 1u));
}
// Zkratky pro testovani ([keys] TestKeys, menu Pokrocile): Alt+F9 rezim FG, Alt+F10 upscaler. Zapisuje vlakno Present
// (MenuLoadConfig, menu), cte i hacek klavesnice (vlakno zprav). Zapnute maji prednost pred ladicimi klavesami se
// stejnou klavesou (DebugKeys=1: F9 preset, F10 zachyt) - DbgKey je pak vypne.
static std::atomic<bool> g_testKeysOn{false};
static const int kTestKeyFg = VK_F9, kTestKeyUpscaler = VK_F10;
static inline bool TestKeyOwns(int vk) {
    return g_testKeysOn.load(std::memory_order_relaxed) && (vk == kTestKeyFg || vk == kTestKeyUpscaler);
}
static inline int DbgKey(int vk) { return TestKeyOwns(vk) ? 0 : vk; }
// Hrana klavesy (stisk od minuleho snimku); "was" si pamatuje stav pro dalsi snimek.
static inline bool KeyEdge(bool now, bool& was) {
    const bool edge = now && !was;
    was = now;
    return edge;
}

// Akce nastaveni maji jednu implementaci Set*(hodnota, Src) pro klavesu i menu (src\menu.inl, docs\navrh-menu.md
// kap. 3): klavesa vola Set*(dalsi hodnota, Src::KEY), menu Set*(vybrana hodnota, Src::MENU). Z menu se nepipa
// a log ma priponu "(menu)". Vse bezi na vlakne Present hlavniho zarizeni (OnFrameEnd).
enum class Src { KEY, MENU };
static bool g_keyBeep = true;  // [menu] Beep: pipnuti u Alt klaves (MenuLoadConfig)
static inline void SrcBeep(Src src, UINT type) {
    if (src == Src::KEY && g_keyBeep) MessageBeep(type);
}
// OSD (src\menu.inl, docs\navrh-menu.md kap. 9): po zmene Alt klavesou kratky radek "polozka: hodnota" vlevo nahore.
// Akce Set* volaji SrcNotify; text a hodnotu vezme OSD z polozky menu se stejnym OSD_* (MenuItem::osd).
enum OsdWhat {
    OSD_UPSCALER, OSD_OUTSCALE, OSD_PRESET, OSD_SHARP, OSD_MIPBIAS, OSD_MIPBIAS_B, OSD_INPUT, OSD_DEPTH, OSD_PHASES,
    OSD_DOWN, OSD_PIPELINE, OSD_JSIGN, OSD_JITTER, OSD_LATENCY, OSD_OBJMV, OSD_LIGHTSB,
    OSD_WARN_B,     // varovani z PartBFrameEnd (DLSS nedopocitava zmenseny render)
    OSD_SAVE_FAIL,  // ulozeni nastaveni z menu selhalo
    OSD_WARN_MSAA,  // most vynechava snimky kvuli MSAA hry (odebrani MSAA se nepodarilo / StripMsaa=0)
    OSD_FG,          // zkratka pro testovani Alt+F9: rezim Frame Generation
    OSD_FG_RESTART,  // Alt+F9, ale hra startovala bez FG (presenter nebezi): vyzaduje restart
    OSD_HINT,        // po startu hry: "Nastaveni modu: Alt+N" ([menu] StartHint)
    OSD_FAIL_FG,     // Frame Generation presla za behu do zalohy (helper skoncil, chyba presenteru)
    OSD_FAIL_BRIDGE, // most (DLSS / FSR) se za behu vypnul chybou
    OSD_SAFESTART,   // minule spusteni skoncilo chybou GPU / padem: FG pro tento start vypnuta ([fg] SafeStart)
    OSD_DXVK,        // DXVK hry chybi nebo ma neznamou verzi
    OSD_RENDER,      // zmena renderu za behu (mat_viewportscale, F4 usporny rezim): text v g_osdRender
    OSD_REPORT,      // menu: hlaseni chyby ulozeno
    OSD_REPORT_FAIL, // menu: hlaseni chyby nejde ulozit
    OSD_COUNT
};
static void OsdNotify(int what);  // src\menu.inl
static wchar_t g_osdRender[96];   // text OSD_RENDER
static inline void SrcNotify(Src src, int what) {
    if (src == Src::KEY) OsdNotify(what);
}
static inline const char* SrcTag(Src src) { return src == Src::MENU ? " (menu)" : ""; }

// ---------------------------------------------------------------------------
// Vyber kamery hlavniho pohledu (pro motion vektory)
// ---------------------------------------------------------------------------
static bool SameMatrix(const CamCand& c, const float* m) { return c.rt == g_rt0 && !memcmp(c.m, m, sizeof c.m); }

// Perspektivni draw do plneho RGBA8 cile s cViewProj = m (puvodni hodnoty od hry).
static void NoteCamera(const float* m) {
    int idx = -1;
    if (g_lastCand >= 0 && g_lastCand < g_nCamCand && SameMatrix(g_camCand[g_lastCand], m)) idx = g_lastCand;
    for (int i = 0; i < g_nCamCand && idx < 0; ++i)
        if (SameMatrix(g_camCand[i], m)) idx = i;
    if (idx < 0) {
        if (g_nCamCand >= kMaxCamCand) return;
        idx = g_nCamCand++;
        CamCand& c = g_camCand[idx];
        memcpy(c.m, m, sizeof c.m);
        c.rt = g_rt0;
        c.draws = 0;
        c.firstSeq = g_drawSeq;
        c.vpX = g_vpKnown ? g_vp.X : 0;
        c.vpY = g_vpKnown ? g_vp.Y : 0;
        c.vpW = g_vpKnown ? g_vp.Width : g_rt0W;
        c.vpH = g_vpKnown ? g_vp.Height : g_rt0H;
        c.vpMixed = false;
    } else if (g_vpKnown) {
        const CamCand& o = g_camCand[idx];
        if (o.vpX != g_vp.X || o.vpY != g_vp.Y || o.vpW != g_vp.Width || o.vpH != g_vp.Height)
            g_camCand[idx].vpMixed = true;
    }
    CamCand& c = g_camCand[idx];
    ++c.draws;
    c.lastSeq = g_drawSeq;
    g_lastCand = idx;
}

static bool Near(float a, float b, float rel, float abs_) { return fabsf(a - b) <= rel * (fabsf(a) + fabsf(b)) + abs_; }

// Stejna orientace a FOV, jina pozice: radky x, y, w (radek z zavisi i na near/far) maji stejnou
// linearni cast a jine posunuti. Tak vypada dvojice kamera 3D skyboxu / kamera sveta.
static bool IsTwin(const float* a, const float* b) {
    static const int rows[3] = {0, 1, 3};
    bool samePos = true;
    for (int r : rows) {
        for (int k = 0; k < 3; ++k)
            if (!Near(a[r * 4 + k], b[r * 4 + k], 1e-4f, 1e-6f)) return false;
        if (!Near(a[r * 4 + 3], b[r * 4 + 3], 1e-4f, 1e-3f)) samePos = false;
    }
    return !samePos;
}

// Pohyb kamery mezi snimky (stejne jako helper CameraMotion): posun oka (jednotky Source) a otoceni smeru pohledu.
static bool CamInvert4(const double* m, double* inv) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-12) return false;
        if (piv != col)
            for (int c = 0; c < 8; ++c) std::swap(a[piv][c], a[col][c]);
        const double d = a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col || a[r][col] == 0.0) continue;
            const double f = a[r][col];
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) inv[r * 4 + c] = a[r][c + 4];
    return true;
}
static bool CamMotion(const float* cam, const float* prev, double* dist, double* angle) {
    double C[16], P[16], Ci[16], Pi[16];
    for (int i = 0; i < 16; ++i) {
        C[i] = cam[i];
        P[i] = prev[i];
    }
    if (!CamInvert4(C, Ci) || !CamInvert4(P, Pi) || fabs(Ci[14]) < 1e-12 || fabs(Pi[14]) < 1e-12) return false;
    double d2 = 0.0, dot = 0.0, lc = 0.0, lp = 0.0;
    for (int k = 0; k < 3; ++k) {
        const double e = Ci[k * 4 + 2] / Ci[14] - Pi[k * 4 + 2] / Pi[14];
        d2 += e * e;
        dot += C[12 + k] * P[12 + k];
        lc += C[12 + k] * C[12 + k];
        lp += P[12 + k] * P[12 + k];
    }
    if (lc <= 0.0 || lp <= 0.0) return false;
    double c = dot / sqrt(lc * lp);
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    *dist = sqrt(d2);
    *angle = acos(c) * 57.29577951308232;
    return true;
}
static bool CamIsCut(const float* cam, const float* prev) {  // prahy jako vychozi [dlaa] CutDistance / CutAngle helperu
    double d = 0.0, a = 0.0;
    return CamMotion(cam, prev, &d, &a) && (d > 128.0 || a > 45.0);
}
static unsigned g_camHoldN;  // kolikrat byl drzen navazujici kandidat (statistika)

// Kamera = kandidat s nejvice drawy do barvy sceny. Kdyz ma "dvojce" (3D skybox x svet), vyhrava ta,
// ktera kreslila po poslednim mazani hloubky sceny (jeji hloubka v depth bufferu zustala); kdyz to
// nejde rozhodnout, ta kreslena pozdeji (Source kresli 3D skybox pred svetem).
// Ladeni (v24v): vsichni kandidati a minula kamera do logu (rozpoznani cizi kamery).
static void CamDebugLog(const char* tag, unsigned n, int bad, int held) {
    for (int i = 0; i < g_nCamCand; ++i) {
        const CamCand& c = g_camCand[i];
        const float* m = c.m;
        Log("kamera ladeni %s #%u: kandidat %d%s%s drawu %u poradi %u-%u vp %u,%u %ux%u%s | m %.5g %.5g %.5g %.5g / "
            "%.5g %.5g %.5g %.5g / %.5g %.5g %.5g %.5g / %.5g %.5g %.5g %.5g",
            tag, n, i, c.rt == g_sceneRT ? " scena" : "", i == bad ? " (cizi)" : i == held ? " (drzena)" : "", c.draws,
            c.firstSeq, c.lastSeq, c.vpX, c.vpY, c.vpW, c.vpH, c.vpMixed ? " smiseny" : "", m[0], m[1], m[2], m[3], m[4],
            m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
    const float* m = g_prevCam;
    Log("kamera ladeni %s #%u: minula | m %.5g %.5g %.5g %.5g / %.5g %.5g %.5g %.5g / %.5g %.5g %.5g %.5g / %.5g %.5g "
        "%.5g %.5g | smazani hloubky po drawu %u (%s), drawu celkem %u",
        tag, n, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15],
        g_zClearSeq, g_zClearValid ? "platne" : "nezname", g_drawSeq);
}

// Draw kreslene uz v prostoru kamery (v24v): cViewProj je cista projekce - pohled = jednotkova matice (oko v pocatku,
// osy sveta). Hra tak kresli cast sceny (az stovky drawu, mezi drawy sveta); skutecna kamera hrace takovou matici mit
// nemuze (presne nulove posunuti i otoceni). Driv vyhravala vyber, kdyz mela vic drawu nez svet -> falesny strih
// ("skok 3693 j / 89 st" = vzdalenost hrace od pocatku). Pri vyberu se nepocita vubec.
static bool IsViewSpaceProj(const float* m) {
    static const int zero[] = {1, 2, 3, 4, 6, 7, 8, 9, 12, 13, 15};
    for (int i : zero)
        if (fabsf(m[i]) > 1e-6f) return false;
    return m[0] != 0.0f && m[5] != 0.0f && m[14] != 0.0f;
}
static unsigned g_camViewSpaceN;

static void SelectCamera() {
    g_camSet = false;
    int best = -1;
    for (int i = 0; i < g_nCamCand; ++i) {
        if (g_camCand[i].rt != g_sceneRT) continue;
        if (IsViewSpaceProj(g_camCand[i].m)) {
            if (++g_camViewSpaceN == 1)
                Log("kamera: kandidat #%d (%u drawu) kresli v prostoru kamery (matice = jen projekce %.4g %.4g %.4g %.4g) - "
                    "z vyberu vyrazen", i, g_camCand[i].draws, g_camCand[i].m[0], g_camCand[i].m[5], g_camCand[i].m[10],
                    g_camCand[i].m[11]);
            continue;
        }
        if (best < 0 || g_camCand[i].draws > g_camCand[best].draws) best = i;
    }
    bool twin = false;
    if (best >= 0) {
        const bool clearKnown = g_zClearValid && g_zClearDS && g_zClearDS == g_sceneDS;
        int after = -1, nAfter = 0, latest = best;
        for (int i = 0; i < g_nCamCand; ++i) {
            const CamCand& c = g_camCand[i];
            if (c.rt != g_sceneRT || (i != best && !IsTwin(c.m, g_camCand[best].m))) continue;
            if (i != best) twin = true;
            if (clearKnown && c.lastSeq > g_zClearSeq) {
                after = i;
                ++nAfter;
            }
            if (c.firstSeq > g_camCand[latest].firstSeq) latest = i;
        }
        if (twin) {
            int pick = nAfter == 1 ? after : latest;
            if (!g_loggedTwin) {
                g_loggedTwin = true;
                Log("kamera: dvojice skybox/svet, vybran kandidat #%d (%u drawu, rozhodl %s), nejvic drawu ma #%d (%u)",
                    pick, g_camCand[pick].draws, nAfter == 1 ? "smazani hloubky" : "poradi", best,
                    g_camCand[best].draws);
            }
            best = pick;
            ++g_statTwin;
        }
        // [dlaa] CamHold (v24t): vybrany kandidat by byl strih kamery (skok o tisice jednotek a ~90 st na par snimku,
        // pak zpet - pri chuzi ve hre vic nez 4x za s), ale jiny kandidat do barvy sceny na minuly snimek navazuje ->
        // ten (jiny pohled hry mel v techto snimcich vic drawu). Bez navazujiciho kandidata zustava skutecny strih.
        if (g_cfg.camHold && g_prevCamValid && CamIsCut(g_camCand[best].m, g_prevCam)) {
            int cont = -1;
            for (int i = 0; i < g_nCamCand; ++i)
                if (i != best && g_camCand[i].rt == g_sceneRT && !IsViewSpaceProj(g_camCand[i].m) &&
                    !CamIsCut(g_camCand[i].m, g_prevCam) &&
                    (cont < 0 || g_camCand[i].draws > g_camCand[cont].draws))
                    cont = i;
            if (cont >= 0) {
                if (++g_camHoldN <= 20 || g_camHoldN % 200 == 0) {
                    double d = 0.0, a = 0.0;
                    CamMotion(g_camCand[best].m, g_prevCam, &d, &a);
                    Log("kamera: drzen navazujici kandidat #%d (%u drawu) misto #%d (%u drawu, skok %.0f j / %.0f st, "
                        "viewport %u,%u %ux%u), kandidatu %d [#%u]",
                        cont, g_camCand[cont].draws, best, g_camCand[best].draws, d, a, g_camCand[best].vpX,
                        g_camCand[best].vpY, g_camCand[best].vpW, g_camCand[best].vpH, g_nCamCand, g_camHoldN);
                }
                if (g_camHoldN <= 3) CamDebugLog("drzeni", g_camHoldN, best, cont);
                best = cont;
            } else {
                static unsigned s_cutLogs;
                if (++s_cutLogs <= 20) {
                    double d = 0.0, a = 0.0;
                    CamMotion(g_camCand[best].m, g_prevCam, &d, &a);
                    Log("kamera: strih bez navazujiciho kandidata (#%d, %u drawu, skok %.0f j / %.0f st), kandidatu %d",
                        best, g_camCand[best].draws, d, a, g_nCamCand);
                }
            }
        }
        memcpy(g_cam, g_camCand[best].m, sizeof g_cam);
        g_camSet = true;
    }
    g_selIdx = best;
    g_selCands = g_nCamCand;
    g_selDraws = best >= 0 ? g_camCand[best].draws : 0;

    if (g_capState == CAP_ACTIVE) {
        Cap("# kamera: kandidatu %d, vybran #%d%s, smazani hloubky po drawu %u (%s)", g_nCamCand, best,
            twin ? " (dvojice skybox/svet)" : "", g_zClearSeq, g_zClearValid ? "platne" : "nezname");
        for (int i = 0; i < g_nCamCand; ++i) {
            const CamCand& c = g_camCand[i];
            Cap("# kandidat #%d: rt=%p%s drawu=%u prvni=%u posledni=%u viewport=%u,%u %ux%u%s", i, (void*)c.rt,
                c.rt == g_sceneRT ? " (scena)" : "", c.draws, c.firstSeq, c.lastSeq, c.vpX, c.vpY, c.vpW, c.vpH,
                c.vpMixed ? " (smiseny)" : "");
            char name[32];
            snprintf(name, sizeof name, "kandidat%d", i);
            CapMatrix(name, c.m);
        }
    }
}

// ---------------------------------------------------------------------------
// Motion vektory pohyblivych objektu: zaznam draw callu v G-bufferu a davkovy replay na konci sceny
// ---------------------------------------------------------------------------
#include "objmv.inl"
#include "reactive.inl"  // maska reaktivity (pruhledne a aditivni draw -> DLSS / FSR)

// ---------------------------------------------------------------------------
// Adaptivni obdoba Reflexu: kdyz je hra omezena GPU, uspi vlakno v Present tak, aby fronta GPU mela <= 1 snimek
// ---------------------------------------------------------------------------
#include "latency.inl"

// Cast B, konec snimku: snimek bez draw roztazeni a jednorazova pojistka (docs\advokat-upscaling-B2.md, D4).
// mat_viewportscale se uklada do config.cfg hry: kdyz zustane < 1 a most scenu nezpracovava (vypnuty, selhal, helper
// nebo DLSS nebezi), obraz roztahuje engine bilinearne a hrac nevi proc -> jednou WARN do logu a pipnuti. Az po 10 s
// souvisleho stavu (start helperu a nacteni DLSS trva par sekund), jen jednou za spusteni.
static void PartBFrameEnd() {
    static LONGLONG s_idleSince;
    static bool s_warned;
    if (!g_sceneEndDone) return;  // menu, nahravani: nic se nemeni
    if (!g_pathB) {
        s_idleSince = 0;
        return;
    }
    if (!g_bHandled && !g_bBetween && !g_pathBad) {  // nepouzitelny viewport je zvlast (badVp)
        ++g_stB.noUpscale;
        if (!g_bNoUpscaleLogged) {
            g_bNoUpscaleLogged = true;
            Log("WARN: cast B: snimek se zmensenym renderem %ux%u bez draw roztazeni sceny (mat_viewportupscale 0, nebo "
                "jiny tvar snimku) - most ho nezpracoval; dalsi vyskyty jen v radku vykon",
                g_renderW, g_renderH);
        }
    }
    if (Interop_FrameBridged(g_frame)) {  // g_frame je jeste cislo tohoto snimku
        s_idleSince = 0;
        return;
    }
    const LONGLONG now = (LONGLONG)GetTickCount64();
    if (!s_idleSince) s_idleSince = now;
    if (s_warned || now - s_idleSince < 10000) return;
    s_warned = true;
    const char* why = Interop_WhyIdle();
    Log("WARN: hra kresli scenu zmensenou (mat_viewportscale < 1: render %ux%u v %ux%u), ale DLSS ji nedopocitava - %s. "
        "Obraz roztahuje engine bilinearne (rozmazany). Zapni DLSS (Alt+F8), nebo vrat v konzoli mat_viewportscale 1.",
        g_renderW, g_renderH, g_renderSurfW, g_renderSurfH,
        why ? why : "most snimky nezpracovava (viz radek vykon)");
    MessageBeep(MB_ICONEXCLAMATION);
    OsdNotify(OSD_WARN_B);
}

// Cast radku "vykon" (interop.cpp, kazdych ~5 s): cast B za uplynuly interval; pak vynuluje. Prazdne, kdyz se
// v intervalu cast B neobjevila a nic neobvykleho se nestalo.
void ProxyPartBPerf(char* buf, size_t n) {
    if (!n) return;
    buf[0] = 0;
    const auto st = g_stB;
    g_stB = {};
    if (!st.frames && !st.badVp && !st.otherVpDraws) return;
    const double f = st.sceneFrames ? (double)st.sceneFrames : 1.0;
    if (st.frames)
        snprintf(buf, n,
                 " | cast B (render %ux%u v %ux%u): snimku %u, roztazeni potlaceno %u (+ dalsi draw %u), kreslil engine "
                 "%u, bez roztazeni %u, draw mezi kopii a roztazenim %u; jiny viewport nez render %.1f draw/snimek, "
                 "nepouzitelny viewport %u snimku, hledi opraveno %u, svetla enginu %u snimku%s (opraveno: svetla %u "
                 "draw, cteni light bufferu %u draw, stinova maska slunce %u draw (+ %u odraz/refrakce beze zmeny), paprsky slunce %u draw (zare %u), mekke "
                 "castice %u draw (+ %u spritecard beze zmeny), kopie pro refrakci %u)",
                 g_renderW, g_renderH, g_renderSurfW, g_renderSurfH, st.frames, st.suppressed, st.extra, st.notQueued,
                 st.noUpscale, st.between, st.otherVpDraws / f, st.badVp, st.scopeFixed, st.lightFrames,
                 g_lightsPatched ? " (zaplata LightsB)" : "", st.lightPsFixed, st.camParFixed, st.csmFixed, st.csmSkipped,
                 st.sunFixed, st.sunTwin, st.softFixed, st.softSkipped, st.refractFixed);
    else
        snprintf(buf, n, " | viewporty: jiny nez render %.1f draw/snimek, nepouzitelny viewport kamery %u snimku",
                 st.otherVpDraws / f, st.badVp);
}

static void LightsPatchUpdate();
static void FirePendingSceneEnd(IDirect3DDevice9* d, const char* where);
static void HlFrameEnd(IDirect3DDevice9* d);  // K5: HUD-less (definice u PreDraw)

// ---------------------------------------------------------------------------
// Akce nastaveni (klavesy v OnFrameEnd i menu, viz enum Src). Volat jen z vlakna Present hlavniho zarizeni, pred
// UpdateScaleState / UpdateJitterState (zmena pak plati od dalsiho snimku). LatSetMode je v latency.inl, OmSetEnabled
// v objmv.inl.
// ---------------------------------------------------------------------------
static const char* DlaaNote() { return Interop_GetMode() == BMSD_MODE_DLAA ? "" : " - projevi se v rezimu DLAA"; }

// Alt+F11 v DLAA, ktere helper opravdu provadi: znamenko jitteru predavaneho DLSS (+,+) -> (-,+) -> (+,-) -> (-,-);
// helper ho nasobi s [dlaa] JitterSignX/Y.
static const int kJitterSigns[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
static void SetJitterSign(int idx, Src src) {
    g_jitterSignIdx = ((idx % 4) + 4) % 4;
    Interop_SetJitterSign(kJitterSigns[g_jitterSignIdx][0], kJitterSigns[g_jitterSignIdx][1]);
    SrcBeep(src, MB_OK);
    SrcNotify(src, OSD_JSIGN);
    Log("znamenko jitteru: %+d %+d (%s; nasobi se s ini JitterSignX/Y)", kJitterSigns[g_jitterSignIdx][0],
        kJitterSigns[g_jitterSignIdx][1], src == Src::KEY ? "klavesa" : "menu");
}

// Alt+F11 mimo DLAA: uzivatelsky jitter zapnuto/vypnuto.
static void SetJitterOn(bool on, Src src) {
    g_jitterOn = on;
    SrcBeep(src, on ? MB_OK : MB_ICONHAND);
    SrcNotify(src, OSD_JITTER);
    Log("jitter: %s (scale %.2f, %d fazi)%s%s", on ? "ZAPNUTO" : "vypnuto", g_cfg.scale, g_phases,
        g_jitterForced ? " - ted ho ale vynucuje rezim mostu" : "", SrcTag(src));
}

// Alt+F8: rezim mostu (BMSD_MODE_*). Zmenu loguje Interop_SetMode.
static uint32_t NextBridgeMode() {
    const uint32_t m = Interop_GetMode();
    if (!g_cfg.debugCycle) return m == BMSD_MODE_OFF ? BMSD_MODE_DLAA : BMSD_MODE_OFF;  // porovnani A/B
    static const uint32_t kOrder[] = {BMSD_MODE_OFF, BMSD_MODE_DLAA, BMSD_MODE_COPY, BMSD_MODE_MIRROR,
                                      BMSD_MODE_MVDEBUG};
    const int n = (int)(sizeof kOrder / sizeof kOrder[0]);
    int i = 0;
    while (i < n && kOrder[i] != m) ++i;
    return kOrder[(i + 1) % n];
}
static void SetBridgeMode(uint32_t mode, Src src) {
    Interop_SetMode(mode);
    SrcBeep(src, Interop_GetMode() ? MB_OK : MB_ICONHAND);
    SrcNotify(src, OSD_UPSCALER);
}

// Alt+F9: preset DLSS vychozi -> K -> L -> M -> vychozi (helper znovu vytvori DLSS feature)
static unsigned NextPreset() {
    int i = 0;
    while (i < kPresetCount && kPresets[i] != g_preset) ++i;
    return kPresets[(i + 1) % kPresetCount];
}
static void SetPreset(unsigned preset, Src src) {
    g_preset = preset;
    Interop_SetPreset(g_preset);
    Interop_SetInputMode(EffInputMode());  // L ma vlastni vstup
    SrcBeep(src, MB_OK);
    SrcNotify(src, OSD_PRESET);
    Log("preset DLSS: %s (%u)%s%s", PresetName(g_preset), g_preset, SrcTag(src), DlaaNote());
}

// Alt+Home: doostreni po DLSS 0 -> 0.2 -> 0.4 -> 0.6 -> 0.8 -> 1.0 -> 0 (menu: libovolna hodnota 0..1)
static float NextSharpness() {
    const int step = (int)(g_sharpness * 5.0f + 0.5f);  // nejblizsi krok 0..5
    return step >= 5 ? 0.0f : (float)(step + 1) / 5.0f;
}
static void SetSharpness(float s, Src src) {
    g_sharpness = std::min(1.0f, std::max(0.0f, s));
    Interop_SetSharpness(g_sharpness);
    SrcBeep(src, g_sharpness > 0.0f ? MB_OK : MB_ICONHAND);
    SrcNotify(src, OSD_SHARP);
    Log("ostrost: %.1f%s%s", g_sharpness, SrcTag(src), DlaaNote());
}

// Alt+Del v casti A: mip bias pro DLAA. v = 0 vypne (hodnota [dlaa] MipBias zustava pro pristi zapnuti), jinak
// zapne s v (klavesa predava g_mipBiasBase = jen zapnuto/vypnuto; jinou hodnotu nastavuje menu, skutecnou
// g_mipBiasValue pak prepocita a zaloguje UpdateScaleState).
static void SetMipBias(float v, Src src) {
    const bool on = v != 0.0f;
    const bool newBase = on && v != g_mipBiasBase;
    if (newBase) g_mipBiasBase = std::min(0.0f, std::max(-4.0f, v));
    g_mipBiasEnabled = on;
    SrcBeep(src, on ? MB_OK : MB_ICONHAND);
    SrcNotify(src, OSD_MIPBIAS);
    if (on && newBase)
        Log("mip bias: %.2f%s%s", g_mipBiasBase, SrcTag(src), DlaaNote());
    else if (on)
        Log("mip bias: %.1f%s%s", g_mipBiasValue, SrcTag(src), DlaaNote());
    else
        Log("mip bias: vypnuto%s", SrcTag(src));
}

// Alt+Del v casti B: log2(r) + MipBias -> MipBias jako v DLAA -> log2(r) -> vypnuto (stejne hodnoty se preskakuji,
// napr. pri r = 0.5 je MipBias -1 = log2(r)). Hodnotu zapise do logu UpdateScaleState.
static int NextMipBiasB() {
    const float r = RenderRatio();
    const int old = g_mipBIdx;
    int idx = old;
    for (int k = 0; k < 4; ++k) {
        idx = (idx + 1) % 4;
        if (idx == 3 || old == 3 || fabsf(MipBiasB(idx, r) - MipBiasB(old, r)) > 0.01f) break;
    }
    return idx;
}
static void SetMipBiasB(int idx, Src src) {
    g_mipBIdx = std::min(3, std::max(0, idx));
    SrcBeep(src, g_mipBIdx != 3 ? MB_OK : MB_ICONHAND);
    SrcNotify(src, OSD_MIPBIAS_B);
    if (g_mipBIdx == 3)
        Log("mip bias: vypnuto (cast B)%s", SrcTag(src));
    else
        g_mipBiasValue = 1.0f;  // vynutit zapis nove hodnoty do logu (i kdyz se cislo nezmenilo)
}

// Alt+Ins: vstup DLSS LDR -> linearni HDR -> linearni HDR + auto expozice -> + inverzni tone mapping -> LDR
// (helper znovu vytvori DLSS feature, stejne jako pri zmene presetu)
// Preset L ma vlastni vstup ([dlaa] InputModeL, vychozi LDR): u plotu s L nejklidnejsi LDR, u K a M naopak HDR
// (uzivatel 2026-11-21). Alt+Ins / menu meni vstup prave zvoleneho presetu.
static void SetInputMode(unsigned mode, Src src) {
    (g_preset == 12 ? g_inputModeL : g_inputMode) = mode % BMSD_INPUT_COUNT;
    const unsigned eff = EffInputMode();
    Interop_SetInputMode(eff);
    SrcBeep(src, MB_OK);
    SrcNotify(src, OSD_INPUT);
    Log("vstup DLSS: %s (%u)%s%s%s", InputModeName(eff), eff, g_preset == 12 ? " [preset L]" : "", SrcTag(src),
        DlaaNote());
}

// Alt+F5: hloubka pro DLSS puvodni -> min 3x3 -> konstantni 1.0 (test) -> puvodni
// (predava se kazdy snimek, helper jen resetuje historii DLSS)
static void SetDepthMode(unsigned mode, Src src) {
    g_depthMode = mode % BMSD_DEPTH_COUNT;
    Interop_SetDepthMode(g_depthMode);
    SrcBeep(src, g_depthMode ? MB_OK : MB_ICONHAND);
    SrcNotify(src, OSD_DEPTH);
    Log("hloubka DLSS: %s (%u)%s%s", DepthModeName(g_depthMode), g_depthMode, SrcTag(src),
        g_pathB ? " - pri zmensenem renderu (cast B) se nepodporuje, projevi se s mat_viewportscale 1" : DlaaNote());
}

// Alt+F3: vystup DLSS (supersampling) 1.0 -> 4/3 -> 1.5 -> 1.724 -> 2.0 -> 1.0 (helper znovu vytvori DLSS feature
// a obrazy velikosti vystupu; skutecny vystup s rezimem zapise most do logu, az ho helper potvrdi). 3.0 jen z ini.
static float NextOutScale() {
    for (float c : kScaleCycle)
        if (c > g_outScale + 0.01f) return c;
    return kScaleCycle[0];
}
static void SyncOutScale();  // za Upscaler nize: FSR dostane vzdy 1.0
static void SetOutScale(float s, Src src) {
    g_outScale = s;
    SyncOutScale();
    SrcBeep(src, g_outScale > 1.0f ? MB_OK : MB_ICONHAND);
    SrcNotify(src, OSD_OUTSCALE);
    Log("vystup DLSS: pozadovano %sx (%ldx%ld)%s%s", ScaleStr(g_outScale).c_str(), lround((double)g_bbW * g_outScale),
        lround((double)g_bbH * g_outScale), SrcTag(src),
        g_pathB ? " - pri zmensenem renderu (cast B) se nepouziva, projevi se s mat_viewportscale 1" : DlaaNote());
}

// Alt+F2: pocet fazi jitteru auto (8*S^2) -> 8 -> 12 -> 16 -> 32 -> auto; cyklus pokracuje od hodnoty z ini
// (jina hodnota z ini -> nejblizsi vyssi v cyklu). Log v UpdateScaleState, helper pri zmene resetuje historii DLSS.
static int NextPhases() {
    static const int kCycle[] = {0, 8, 12, 16, 32};
    const int n = (int)(sizeof kCycle / sizeof kCycle[0]);
    int i = 0;
    while (i < n && kCycle[i] != g_phasesSetting) ++i;
    if (i < n) return kCycle[(i + 1) % n];
    for (int c : kCycle)
        if (c > g_phasesSetting) return c;
    return 0;
}
static void SetPhases(int n, Src src) {
    g_phasesSetting = std::min(256, std::max(0, n));
    SrcBeep(src, MB_OK);
    SrcNotify(src, OSD_PHASES);
}

// Alt+F1: filtr zmenseni pri S > 1 Lanczos-2 -> Catmull-Rom -> Mitchell -> box -> Lanczos-2 (helper ho dostava
// kazdy snimek jako push konstantu, DLSS feature se nemeni)
static void SetDownFilter(unsigned f, Src src) {
    g_downFilter = f % BMSD_DOWN_COUNT;
    Interop_SetDownsampleFilter(g_downFilter);
    SrcBeep(src, MB_OK);
    SrcNotify(src, OSD_DOWN);
    Log("zmenseni: %s (%s)%s%s", DownFilterName(g_downFilter), g_cfg.downsampleLinear ? "linearni" : "srgb",
        SrcTag(src), Interop_ActiveOutScale() > 1.0001f ? "" : " - projevi se jen pri vystupu DLSS > 1.0x (Alt+F3)");
}

// Alt+ScrollLock: synchronizace mostu pipeline <-> synchronni (porovnani A/B, radky "vykon" v logu)
static void SetPipeline(bool on, Src src) {
    Interop_SetPipeline(on);
    SrcBeep(src, Interop_GetPipeline() ? MB_OK : MB_ICONHAND);
    SrcNotify(src, OSD_PIPELINE);
}

// [upscale] LightsB za behu (jen menu, klavesu nema): zaplatu client.dll zapina a vypina LightsPatchUpdate sam.
// Rozpoznani shaderu svetel ale potrebuje hacek CreatePixelShader, ktery se instaluje jen pri startu s LightsB=1
// (nebo LightsBDetect=1, HookDevice). Bez nej LightsPatchUpdate zaplatu nezapne (oprava S2 revize menu/FSR: jinak
// cervene smeti mimo sub-rect); hodnota se z menu ulozi do ini a plati po restartu hry (menu to u polozky pise).
static void SetLightsB(bool on, Src src) {
    g_cfg.lightsB = on;
    Log("LightsB: %s%s%s", on ? "zapnuto" : "vypnuto", SrcTag(src),
        on && !o_CreatePS ? " - plati az po restartu hry (hacek CreatePixelShader se instaluje pri startu)" : "");
}

// Upscaler (menu, [upscale] Upscaler): 0 = vypnuto (most vypnuty), 1 = DLSS, 2 = FSR, 3 = XeSS, 4 = auto (vychozi od
// verze 13: DLSS, kdyz jde, jinak FSR - bez hlasky). Helper umi DLSS a FSR (BmsdShared::upscaler od verze 11):
// SetUpscaler predava volbu pres Interop_SetUpscaler, DllMain po startu. XeSS je v menu sede "pripravuje se", DLSS
// take, kdyz ho helper hlasi jako trvale nedostupne (Interop_DlssBroken; karty bez NGX - "jen NVIDIA RTX"). Pri
// selhani vyslovne volby zkousi helper druhy upscaler (FSR <-> DLSS), duvod je v helperMsg.
enum Upscaler { UPS_OFF = 0, UPS_DLSS = 1, UPS_FSR = 2, UPS_XESS = 3, UPS_AUTO = 4, UPS_COUNT };
static_assert(UPS_DLSS == BMSD_UPSCALER_DLSS && UPS_FSR == BMSD_UPSCALER_FSR && UPS_XESS == BMSD_UPSCALER_XESS &&
                  UPS_AUTO == BMSD_UPSCALER_AUTO && UPS_COUNT == BMSD_UPSCALER_COUNT,
              "Upscaler = BMSD_UPSCALER_*");
static int g_upscaler = UPS_AUTO;  // vybrany upscaler pro zapnuty most (vypnuto = rezim mostu BMSD_MODE_OFF)
static bool UpscalerAvailable(int u) {
    return u == UPS_OFF || u == UPS_AUTO || u == UPS_FSR || (u == UPS_DLSS && !Interop_DlssBroken());
}
static const char* UpscalerName(int u) {
    return u == UPS_DLSS ? "DLSS" : u == UPS_FSR ? "FSR" : u == UPS_XESS ? "XeSS" : u == UPS_AUTO ? "auto" : "vypnuto";
}
// Supersampling jen s DLSS (uzivatel 2026-11-21: FSR pri 2x skladá obraz z poloviny rozliseni a vypada hur nez
// FSR Native AA). Pri FSR (zvolenem, nebo auto na karte bez DLSS) dostane helper 1.0; g_outScale (volba pro DLSS,
// ini) zustava. Vola se pri zmene vystupu i upscaleru a jednou za snimek (UpdateScaleState: DlssBroken z helperu).
static bool FsrSelected() {
    return g_upscaler == UPS_FSR || (g_upscaler == UPS_AUTO && Interop_DlssBroken());
}
static void SyncOutScale() {
    static float s_sent = -1.0f;
    const float eff = FsrSelected() ? 1.0f : g_outScale;
    if (eff == s_sent) return;
    if (s_sent >= 0.0f && eff != g_outScale)
        Log("vystup: FSR - supersampling %sx se nepouziva (jen DLSS), bezi 1.0x", ScaleStr(g_outScale).c_str());
    s_sent = eff;
    Interop_SetOutputScale(eff);
}

// Aktualni stav: vypnuty most = UPS_OFF, DLAA = vybrany upscaler, ladici rezim mostu (kopie, zrcadleni...) = -1.
static int GetUpscaler() {
    const uint32_t m = Interop_GetMode();
    return m == BMSD_MODE_OFF ? UPS_OFF : m == BMSD_MODE_DLAA ? g_upscaler : -1;
}
static void SetUpscaler(int u, Src src) {
    if (u == UPS_OFF) {
        SetBridgeMode(BMSD_MODE_OFF, src);
        return;
    }
    if (u <= UPS_OFF || u >= UPS_COUNT || !UpscalerAvailable(u)) {
        Log("upscaler: %s %s - zustava %s%s", UpscalerName(u),
            u == UPS_DLSS ? "na tomto systemu neni k dispozici" : "se pripravuje", UpscalerName(g_upscaler), SrcTag(src));
        return;
    }
    const bool changed = u != g_upscaler;
    g_upscaler = u;
    Interop_SetUpscaler((uint32_t)u);  // helper: DLSS feature / FSR kontext znovu (pri selhani FSR bezi DLSS)
    SyncOutScale();
    if (changed) Log("upscaler: %s%s", UpscalerName(u), SrcTag(src));
    if (Interop_GetMode() != BMSD_MODE_DLAA) SetBridgeMode(BMSD_MODE_DLAA, src);
}

// ---------------------------------------------------------------------------
// Menu nastaveni ve hre: hacek klavesnice, atlas pisma, prekryv pred Present (pouziva akce Set* vyse)
// ---------------------------------------------------------------------------
#include "menu.inl"

// MSAA hry: most vynechava snimky (barva sceny s MSAA) - odebrani v CreateDevice / Reset se nepovedlo nebo
// [upscale] StripMsaa=0. Jednou za beh: radek do logu a OSD na 6 s; stavovy radek menu hlasi stav trvale.
static void MsaaFrameEnd() {
    static bool s_warned;
    if (s_warned || !Interop_MsaaBlocked()) return;
    s_warned = true;
    Log("WARN: MSAA ve hre je zapnute a most snimky vynechava (mod ho %s) - vypni MSAA v nastaveni videa hry",
        g_cfg.stripMsaa ? "neodebral" : "neodebira: [upscale] StripMsaa=0");
    OsdNotify(OSD_WARN_MSAA);
}

static bool g_dxvkWarn;  // OSD: DXVK chybi nebo ma neznamou verzi (DllMain)
// Chyby za behu: jednou OSD (6 s), aby hrac vedel, proc se zmenil obraz / plynulost a ze se to do restartu nevrati
static void FailNotesFrameEnd() {
    if (Present_TakeFallbackNote()) OsdNotify(OSD_FAIL_FG);
    if (Interop_TakeFailNote()) OsdNotify(OSD_FAIL_BRIDGE);
    if (const int r = g_reportDone.exchange(0)) OsdNotify(r);
    // Upozorneni pri startu: az po nacteni okna hry (~300 snimku), DXVK pred bezpecnym startem
    static unsigned frames;
    if (frames < 1000 && ++frames == 300) {
        if (g_dxvkWarn) OsdNotify(OSD_DXVK);
        else if (Present_SafeStartHit()) OsdNotify(OSD_SAFESTART);
    }
}

static void OnFrameEnd() {
    if (g_sceneEndPending) FirePendingSceneEnd(g_main, "Present");
    FailNotesFrameEnd();
    if (g_main && Present_Enabled()) HlFrameEnd(g_main);  // K5: pred menu modu (to se kresli az do backbufferu)
    g_nOcclOpen = 0;  // dotaz otevreny pres Present se dal nesleduje (jen se pak konec sceny neodlozi)
    Interop_OnPresent();  // cas snimku (Present -> Present), radek "vykon" do logu
    PartBFrameEnd();
    RmFrameEnd();
    LightsPatchUpdate();  // cast B: svetla enginu ([upscale] LightsB, jinak jen pocitadlo)
    if (g_main) ApplyMipBias(g_main, false);  // hodnoty hry zpet (Present = mimo scenu)
    if (!g_sceneEndDone) SelectCamera();  // konec sceny nenastal: kamera aspon jako "predchozi"
    // kamera tohoto snimku -> "predchozi" pro dalsi snimek
    if (g_camSet) {
        memcpy(g_prevCam, g_cam, sizeof g_cam);
        g_prevCamValid = true;
    } else {
        g_prevCamValid = false;
        ++g_statNoCam;
    }

    if (g_capState == CAP_ACTIVE) FinishCapture();
    g_camSet = false;
    g_nCamCand = 0;
    g_lastCand = -1;
    g_drawSeq = 0;
    g_zClearSeq = 0;
    g_zClearValid = false;
    g_zClearDS = nullptr;
    if (g_sceneEndDone) ++g_statSceneEnd;
    g_sceneRT = g_sceneDS = g_linDepth = nullptr;
    g_sceneEndDone = false;
    g_frameUpscaled = false;
    MsaaFrameEnd();
    g_sceneCopy.valid = false;
    // cast B: stav snimku (ukazatele kopie sceny plati jen v ramci snimku)
    g_lastSceneSR.valid = false;
    g_lastSceneSR.dst = nullptr;
    g_lastSceneSR.tex = nullptr;
    g_bHandled = g_bFollow = g_bBetween = false;
    g_sceneObjDraws = 0;
    g_nVpHist = 0;
    g_vpHistLast = -1;
    g_vpHistOver = 0;
    ++g_frame;

    // velikost backbufferu (meni se pri zmene rozliseni)
    IDirect3DSwapChain9* sc = nullptr;
    if (g_main && SUCCEEDED(g_main->GetSwapChain(0, &sc)) && sc) {
        D3DPRESENT_PARAMETERS pp = {};
        if (SUCCEEDED(sc->GetPresentParameters(&pp)) && (pp.BackBufferWidth != g_bbW || pp.BackBufferHeight != g_bbH)) {
            g_bbW = pp.BackBufferWidth;
            g_bbH = pp.BackBufferHeight;
            Log("backbuffer: %ux%u", g_bbW, g_bbH);
        }
        sc->Release();
    }

    // matice v GPU s jitterem minuleho snimku uz neplati
    for (int r = 0; r < 256; ++r)
        if (g_blkState[r] == 1) g_blkState[r] = 2;
    const bool startCap = g_capState == CAP_ARMED;

    // klavesy jen kdyz je v popredi okno hry (ne pri psani jinde)
    DWORD fgPid = 0, fgTid = 0;
    if (HWND fg = GetForegroundWindow()) fgTid = GetWindowThreadProcessId(fg, &fgPid);
    const bool foreground = fgPid == GetCurrentProcessId();
    g_lat.foreground = foreground;  // regulator latence bezi jen v popredi
    KeyTapsBeginFrame();
    // Presne jeden modifikator: Alt+F9 nesmi reagovat na Ctrl+Alt+F9 (ladici klavesy DLSS knihovny).
    bool ctrl;
    {
        const bool c = KeyDown(VK_CONTROL), a = KeyDown(VK_MENU), sh = KeyDown(VK_SHIFT);
        switch (g_cfg.modifier) {
        case 0: ctrl = true; break;
        case VK_CONTROL: ctrl = c && !a && !sh; break;
        case VK_RCONTROL: ctrl = KeyDown(VK_RCONTROL) && !a && !sh; break;
        case VK_SHIFT: ctrl = sh && !c && !a; break;
        default: ctrl = a && !c && !sh; break;
        }
        ctrl = foreground && ctrl;
    }
    bool cap = ctrl && KeyDown(DbgKey(g_cfg.captureKey));
    if (cap && !g_capKeyWas && g_capState == CAP_IDLE) ArmCapture("klavesa");
    g_capKeyWas = cap;
    // [capture] AutoAfterSec: jednou, tolik sekund po prvnim Present (testy bez klaves, nezavisle na popredi)
    if (g_cfg.capAutoSec > 0 && !g_autoCapDone) {
        const ULONGLONG now = GetTickCount64();
        if (!g_firstPresentMs) {
            g_firstPresentMs = now;
        } else if (now - g_firstPresentMs >= (ULONGLONG)g_cfg.capAutoSec * 1000 && g_capState == CAP_IDLE) {
            g_autoCapDone = true;
            ArmCapture("automaticky, AutoAfterSec");
        }
    }

    // Klavesy modu: hrana stisku -> akce Set* (spolecne s menu, viz vyse). Hacek klavesnice (menu.inl) tytez stisky
    // jen polyka, aby je nedostala hra; stav se dal cte pres GetAsyncKeyState. Bez [keys] DebugKeys=1 plati jen
    // Alt+F8 (ostatni kody jsou 0 = KeyDown vzdy false, hacek je nepolyka a hra je dostane).
    // Alt+F11: mimo DLAA zapina/vypina jitter. V DLAA, ktere helper opravdu provadi (jitter je vynuceny), misto toho
    // cykli znamenko jitteru predavaneho DLSS. DLAA bez NGX nebo pred pripravou helperu: jitter se ridi uzivatelem
    // (UpdateJitterState) -> klavesa ho prepina.
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.jitterKey)), g_jitterKeyWas)) {
        if (Interop_GetMode() == BMSD_MODE_DLAA && Interop_DlaaReady())
            SetJitterSign(g_jitterSignIdx + 1, Src::KEY);
        else
            SetJitterOn(!g_jitterOn, Src::KEY);
    }
    if (KeyEdge(ctrl && KeyDown(g_cfg.bridgeKey), g_bridgeKeyWas)) SetBridgeMode(NextBridgeMode(), Src::KEY);
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.presetKey)), g_presetKeyWas)) SetPreset(NextPreset(), Src::KEY);
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.sharpenKey)), g_sharpenKeyWas)) SetSharpness(NextSharpness(), Src::KEY);
    // Alt+Del: mip bias zapnuto/vypnuto (porovnani A/B); v casti B cyklus ctyr hodnot
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.mipBiasKey)), g_mipBiasKeyWas)) {
        if (g_pathB)
            SetMipBiasB(NextMipBiasB(), Src::KEY);
        else
            SetMipBias(g_mipBiasEnabled ? 0.0f : g_mipBiasBase, Src::KEY);
    }
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.inputModeKey)), g_inputModeKeyWas)) SetInputMode(EffInputMode() + 1, Src::KEY);
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.depthModeKey)), g_depthModeKeyWas)) SetDepthMode(g_depthMode + 1, Src::KEY);
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.outScaleKey)), g_outScaleKeyWas)) SetOutScale(NextOutScale(), Src::KEY);
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.phasesKey)), g_phasesKeyWas)) SetPhases(NextPhases(), Src::KEY);
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.downsampleKey)), g_downKeyWas)) SetDownFilter(g_downFilter + 1, Src::KEY);
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.pipelineKey)), g_pipelineKeyWas)) SetPipeline(!Interop_GetPipeline(), Src::KEY);
    static bool s_alphaJitKeyWas;
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_cfg.alphaJitKey)), s_alphaJitKeyWas)) {
        // cyklus rozkmitu 0 -> 0.1 -> 0.25 -> 0.5 -> 0 (pri 0.5 uzivatel: zem a ostnaty drat blikaji vic)
        g_alphaJit = g_alphaJit <= 0 ? 0.1f : g_alphaJit < 0.2f ? 0.25f : g_alphaJit < 0.4f ? 0.5f : 0.0f;
        MessageBeep(g_alphaJit > 0 ? MB_OK : MB_ICONHAND);
        Log("prah alpha testu v case: %s (rozkmit %.2f)%s", g_alphaJit > 0 ? "ZAPNUTO" : "vypnuto", g_alphaJit,
            SrcTag(Src::KEY));
    }

    // Zkratky pro testovani (Alt+F9 FG, Alt+F10 upscaler), jen kdyz je zapnute v menu / [keys] TestKeys=1
    MenuTestKeysFrame(ctrl);
    MenuStartHintFrame();  // po startu "Nastaveni modu: Alt+N"

    // Alt+F12: regulator latence adaptivni -> jen mereni -> vypnuto
    LatKeys(ctrl);

    // MV pohyblivych objektu: uklid zaznamu snimku, Alt+Backspace zapnuto/vypnuto, stav pro dalsi snimek
    OmFrameEnd(ctrl);

    // Menu: hacky klavesnice, fronta stisku z hacku -> stav menu a akce Set* (pred UpdateScaleState jako klavesy)
    MenuFrameInput(foreground, ctrl, fgTid);
    // Frame Generation: s otevrenym menu modu se negeneruje ([fg] PauseInMenu; menu se kresli do backbufferu)
    if (Present_Enabled()) Present_FgSetPause(g_menu.open);

    // jitter pro dalsi snimek (rezim mostu se mohl zmenit klavesou nebo chybou); pocet fazi a mip bias podle S
    SyncOutScale();  // auto na karte bez DLSS: helper hlasi DlssBroken az za behu -> FSR bez supersamplingu
    UpdateScaleState();
    UpdateJitterState();
    UpdateJitter();
    // mip bias jen kdyz DLAA opravdu bezi (stejne jako vynuceny jitter)
    g_mipSceneOn = Interop_GetMode() == BMSD_MODE_DLAA && g_jitterForced && g_jitterActive;
    g_mipBiasOn = MipBiasWanted() && g_mipSceneOn;
    // Menu: prekryv do backbufferu (finalni obraz po celem modu; do barvy sceny = vstupu DLSS se nekresli nikdy).
    // Pred Present i pred kopii backbufferu pro helper (Present_OnPresent) a pred dotazem regulatoru latence.
    MenuFrameDraw(g_main);
    if (startCap) StartCapture();

    if (g_frame % 300 == 0) {
        if (g_frame == 300 || g_frame % 3000 == 0)
            Log("stat frame %llu: draw/snimek %.0f (hlavni pohled s matici %.0f), jitter uploadu %.1f, obnov %.1f, "
                "snimku bez kamery %u/300, s nalezenym koncem sceny %u/300, jitter=%d (vynechan %u/300); "
                "kamera: kandidatu %d, vybran #%d (%u drawu), dvojice skybox/svet %u/300; "
                "mip bias %s, SetSamplerState %.1f/snimek (drawu s normalovou mapou %.1f, s alfa testem %.1f)",
                (unsigned long long)g_frame, g_statDraws / 300.0, g_statMainDraws / 300.0,
                g_statJitterUploads / 300.0, g_statRestores / 300.0, g_statNoCam, g_statSceneEnd,
                (int)g_jitterActive, g_statJitterSkip, g_selCands, g_selIdx, g_selDraws, g_statTwin,
                g_mipBiasOn ? "zapnut" : "vypnut", g_statMipCalls / 300.0, g_statMipNorm / 300.0, g_statMipAlpha / 300.0);
        if ((g_cfg.mipBiasNormal != 0.0f || g_cfg.mipBiasAlpha != 0.0f) && g_frame % 1500 == 0)
            Log("mip bias pridavky (posl. 300 snimku): drawu s normalovou mapou %.1f, s alfa testem %.1f, "
                "SetSamplerState %.1f /snimek", g_statMipNorm / 300.0, g_statMipAlpha / 300.0, g_statMipCalls / 300.0);
        g_statMipNorm = g_statMipAlpha = 0;
        g_statDraws = g_statJitterUploads = g_statRestores = g_statMainDraws = g_statNoCam = g_statSceneEnd = 0;
        g_statTwin = g_statJitterSkip = g_statMipCalls = 0;
    }
}

// Render tohoto snimku (po SelectCamera): viewport draw vybrane kamery v barve sceny; bez kamery nejcastejsi viewport
// perspektivnich draw hlavniho pohledu. Cely povrch sceny = cast A (jako driv), mensi obdelnik v levem hornim rohu =
// cast B (mat_viewportscale < 1). Posunuty, smiseny nebo moc maly viewport cestu ani render nemeni: v casti B snimek
// bez mostu (g_pathBad), v casti A most s celym povrchem (jako pred verzi 10).
// Porovnava se s velikosti povrchu sceny, ne s g_bbW (ta se aktualizuje az v OnFrameEnd).
static void DetectRender() {
    UINT x = 0, y = 0, w = g_sceneW, h = g_sceneH;
    bool mixed = false;
    if (g_selIdx >= 0 && g_selIdx < g_nCamCand) {
        const CamCand& c = g_camCand[g_selIdx];
        x = c.vpX;
        y = c.vpY;
        w = c.vpW;
        h = c.vpH;
        mixed = c.vpMixed;
    } else if (g_nVpHist) {
        int b = 0;
        for (int i = 1; i < g_nVpHist; ++i)
            if (g_vpHist[i].n > g_vpHist[b].n) b = i;
        x = g_vpHist[b].x;
        y = g_vpHist[b].y;
        w = g_vpHist[b].w;
        h = g_vpHist[b].h;
    }
    // Cely povrch (prvni draw kamery) = cast A i kdyby nektery pozdejsi draw mel jiny viewport - presne jako pred
    // verzi 10, ktera se na viewport nedivala. Cast B jen kdyz vsechny draw kamery kreslily do tehoz sub-rectu.
    const bool full = !x && !y && w == g_sceneW && h == g_sceneH;
    const bool sub = !full && !mixed && !x && !y && w <= g_sceneW && h <= g_sceneH && w >= 32 && h >= 32;
    const bool bad = !full && !sub;
    // Draw hlavniho pohledu s jinym viewportem nez render (jitter se pro ne pocita zvlast)
    unsigned other = g_vpHistOver;
    for (int i = 0; i < g_nVpHist; ++i)
        if (g_vpHist[i].x != x || g_vpHist[i].y != y || g_vpHist[i].w != w || g_vpHist[i].h != h) other += g_vpHist[i].n;
    g_stB.otherVpDraws += other;
    if (bad) {
        // Jednotlivy nepouzitelny snimek nemeni cestu ani render (zadne prepinani A <-> B, pipani, mazani historie
        // MV objektu a zmena mip biasu kazdy druhy snimek). Cast B: tento snimek bez mostu (roztahne engine). Cast A
        // (i zatim neznama): jako pred verzi 10 - most s celym povrchem.
        ++g_stB.badVp;
        static bool s_logged[2];
        if (!s_logged[g_pathB]) {
            s_logged[g_pathB] = true;
            Log("WARN: viewport kamery sceny %u,%u %ux%u%s v povrchu %ux%u neni cely povrch ani jeho levy horni roh - "
                "%s (dalsi jen v radku vykon)",
                x, y, w, h, mixed ? " (behem snimku se menil)" : "", g_sceneW, g_sceneH,
                g_pathB ? "v casti B takovy snimek jde bez mostu" : "v casti A most bezi s celym povrchem jako driv");
        }
        if (g_pathB) {
            g_pathBad = true;
            return;
        }
        w = g_sceneW;
        h = g_sceneH;
    }
    const bool sizeChanged = w != g_renderW || h != g_renderH || g_sceneW != g_renderSurfW || g_sceneH != g_renderSurfH;
    if (sizeChanged || sub != g_pathB || !g_renderKnown) {
        // jiny render: MV objektu z minuleho snimku jsou v jinych pixelech (helper navic vytvori novou DLSS feature)
        if (g_renderKnown && sizeChanged) OmClearHistory();
        Log("render: %ux%u v povrchu sceny %ux%u (%.3f) -> %s", w, h, g_sceneW, g_sceneH,
            g_sceneW ? (double)w / (double)g_sceneW : 1.0,
            sub ? "cast B (DLSS dopocita cely obraz misto roztazeni enginu)" : "cast A (DLAA / supersampling)");
        if (g_renderKnown && (sub != g_pathB || (sub && sizeChanged))) {  // zmena mat_viewportscale za behu (F4)
            if (g_keyBeep && sub != g_pathB) MessageBeep(sub ? MB_OK : MB_ICONHAND);
            if (sub)
                swprintf(g_osdRender, 96, L"%ls: %ux%u (%u %%)", Tr(S_OSD_RENDER), w, h,
                         g_sceneW ? (unsigned)(100.0 * w / g_sceneW + 0.5) : 100u);
            else
                swprintf(g_osdRender, 96, L"%ls: %ls", Tr(S_OSD_RENDER), Tr(S_OSD_RENDER_NATIVE));
            OsdNotify(OSD_RENDER);
        }
        g_renderW = w;
        g_renderH = h;
        g_renderSurfW = g_sceneW;
        g_renderSurfH = g_sceneH;
        g_pathB = sub;
        g_renderKnown = true;
    }
    g_pathBad = false;
}

// Konec sceny, 1. cast: kamera, render a davka MV objektu. Od teto chvile uz draw nedostavaji jitter ani mip bias.
static void ScenePrep(IDirect3DDevice9* d, const char* why) {
    g_sceneEndDone = true;
    std::string w = why ? why : "";
    static std::string s_lastWhy;
    if (w != s_lastWhy) {
        s_lastWhy = w;
        Log("konec sceny: %s", w.c_str());
    }
    SelectCamera();  // kamera sceny je ted kompletni (svet i zbran uz jsou nakreslene)
    DetectRender();
    if (Present_Enabled()) Present_NoteScene();
    ++g_stB.sceneFrames;
    // MV pohyblivych objektu: zaznamy z G-bufferu sparovat s minulym snimkem a prehrat do OBJMV (pred kopii A mostu).
    // Stav zarizeni po davce je stejny jako pred ni.
    g_sceneObjDraws = OmBatch(d);
}

// Konec sceny, 2. cast: most (kopie A, helper, kopie B). Cast A: hned po ScenePrep. Cast B: az na draw roztazeni
// (PreDraw), se sub-rectem renderu. true = vysledek mostu bude v cele barve sceny.
static bool SceneBridge(IDirect3DDevice9* d) {
    const unsigned objDraws = g_sceneObjDraws;
    uint32_t maskDraws = 0;
    IDirect3DSurface9* mask = RmForBridge(&maskDraws);
    SceneEndInfo info = {d,
                         g_sceneRT,
                         g_sceneDS,
                         g_pathB ? g_renderW : 0,
                         g_pathB ? g_renderH : 0,
                         g_cam,
                         g_prevCam,
                         g_camSet && g_prevCamValid,
                         g_jitterActive ? g_jitterPxX : 0.0f,
                         g_jitterActive ? g_jitterPxY : 0.0f,
                         g_frame,
                         objDraws ? g_omRT : nullptr,
                         objDraws,
                         OmMode(),
                         g_cfg.objMvTol,
                         mask,
                         maskDraws,
                         g_omEnabled,
                         g_rmOn};
    LatSceneEndEnter();  // regulator latence: most muze cekat na GPU (dokonceni predchoziho snimku)
    const bool queued = Interop_OnSceneEnd(info);
    LatSceneEndLeave();
    g_frameUpscaled = queued && Interop_GetMode() == BMSD_MODE_DLAA;  // DLSS / FSR (ne ladici rezimy mostu)
    return queued;
}

static void TriggerSceneEnd(IDirect3DDevice9* d, const char* why) {
    ScenePrep(d, why);
    if (g_pathBad) return;  // viewport kamery nejde pouzit: snimek bez mostu (dalsi pak bez jitteru)
    if (g_pathB) {
        // Cast B: post-process jeste bezi v nizkem rozliseni nad sub-rectem; most az misto roztazeni (PreDraw).
        // Nic se nezapisuje a kopie sceny se neopakuje.
        ++g_stB.frames;
        if (g_outScale > 1.0001f && !g_bScaleLogged) {
            g_bScaleLogged = true;
            Log("cast B: [upscale] OutputScale=%s se pri zmensenem renderu nepouziva (vystup DLSS = povrch sceny %ux%u, "
                "bez supersamplingu); plati zase s mat_viewportscale 1",
                ScaleStr(g_outScale).c_str(), g_sceneW, g_sceneH);
        }
        if (g_depthMode != BMSD_DEPTH_RAW && !g_bDepthLogged) {
            g_bDepthLogged = true;
            Log("cast B: [dlaa] DepthMode=%u se pri zmensenem renderu nepodporuje, helper pouziva puvodni hloubku",
                g_depthMode);
        }
        return;
    }
    SceneBridge(d);

    // Zopakovat posledni kopii sceny, aby post-process cetl vysledek mostu, ne puvodni obraz.
    if (Interop_GetMode() != 0 && g_sceneCopy.valid) {
        static int s_lastState = -1;
        int state = g_sceneCopy.drawsAfter == 0 ? 1 : 0;
        if (state != s_lastState) {
            s_lastState = state;
            if (state)
                Log("most: opakuji kopii sceny (StretchRect -> %p) po zapisu vysledku", (void*)g_sceneCopy.dst);
            else
                Log("most: kopie sceny probehla %u drawu pred koncem sceny - neopakuji", g_sceneCopy.drawsAfter);
        }
        if (state)
            o_StretchRect(d, g_sceneRT, g_sceneCopy.hasSrc ? &g_sceneCopy.srcRect : nullptr, g_sceneCopy.dst,
                          g_sceneCopy.hasDst ? &g_sceneCopy.dstRect : nullptr, g_sceneCopy.filter);
    }
}

// Odlozeny konec sceny (draw bez zapisu barvy uvnitr okluzniho dotazu hry): normalne az po Issue(END) dotazu,
// pojistkou pri dalsim draw, SetRenderTarget, StretchRect, Clear nebo Present (pak jako drive).
// K5: kopie barvy sceny do RT HUD-less (bod pred HUD). Bez RT (FG negeneruje, menu, zaloha) snimek bez HUD-less.
static void HlSnap(IDirect3DDevice9* d) {
    g_hl.pending = false;
    g_hl.valid = false;
    if (!g_sceneRT || g_sceneW != g_bbW || g_sceneH != g_bbH) return;
    IDirect3DSurface9* rt = Present_HudlessTarget(d);
    if (!rt) return;
    if (!g_hl.loggedSame) {
        g_hl.loggedSame = true;
        IDirect3DSurface9* bb = nullptr;
        const bool same = SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb == g_sceneRT;
        if (bb) bb->Release();
        Log("HUD-less: prvni kopie sceny pred HUD (scena %ux%u %s backbuffer, render %ux%u, cast %s)", g_sceneW,
            g_sceneH, same ? "=" : "!=", g_renderW, g_renderH, g_pathB ? "B" : "A/S");
    }
    g_hl.valid = SUCCEEDED(o_StretchRect(d, g_sceneRT, nullptr, rt, nullptr, D3DTEXF_NONE));
    ++g_hl.snaps;
}

// POST draw: vzorkuje cil posledni kopie sceny (textura stage 0..3 = kontejner cile StretchRect ze sceny)
static bool HlIsPost() {
    if (!g_lastSceneSR.valid || !g_lastSceneSR.tex) return false;
    for (IDirect3DBaseTexture9* t : g_tex4)
        if (t && (void*)t == g_lastSceneSR.tex) return true;
    return false;
}

// Zapis do barvy sceny po konci sceny: post = POST draw (bod pred HUD se posouva za nej); jinak draw HUD, Clear barvy
// nebo StretchRect do sceny = HUD zacina -> nejdriv kopie do RT HUD-less.
static void HlOnSceneWrite(IDirect3DDevice9* d, bool post) {
    if (post) {
        g_hl.pending = true;
        g_hl.valid = false;
    } else if (g_hl.pending) {
        HlSnap(d);
    }
}

// Present (pred menu modu a pred kopii backbufferu pro helper): HUD-less tohoto snimku predat presenteru
static void HlFrameEnd(IDirect3DDevice9* d) {
    if (g_hl.pending && g_sceneEndDone) HlSnap(d);
    Present_HudlessFrame(g_hl.valid && g_sceneEndDone, g_hl.snaps);
    g_hl.pending = g_hl.valid = false;
    g_hl.snaps = 0;
}

static void FirePendingSceneEnd(IDirect3DDevice9* d, const char* where) {
    g_sceneEndPending = false;
    if (g_sceneEndDone || !d) return;
    static const char* s_lastWhere = nullptr;
    if (s_lastWhere != where) {  // jen pri zmene (where = literal)
        s_lastWhere = where;
        Log("konec sceny: odlozen za okluzni dotaz hry (histogram expozice), spusten v %s", where);
    }
    TriggerSceneEnd(d, g_sceneEndPendingWhy.c_str());
}

// Cast B: podpis draw, kterym engine roztahuje scenu (dev/upscale): kresli do barvy sceny pres cely povrch, bez
// perspektivni matice, a textura 0 je kontejner cile posledni kopie sceny (StretchRect scena -> _rt_FullFrameFB1)
// v tomto snimku. Post-process ma viewport sub-rectu, HUD jinou texturu.
// Kopie musi byt presne sub-rect renderu (0,0 - render), jinak roztazeni cte neco jineho nez vstup DLSS.
static bool IsUpscaleDraw(bool perspClip) {
    const RECT& sr = g_lastSceneSR.sr;
    return !perspClip && g_sceneRT && g_rt0 == g_sceneRT && g_vpKnown && !g_vp.X && !g_vp.Y && g_vp.Width == g_rt0W &&
           g_vp.Height == g_rt0H && g_t0 && g_lastSceneSR.valid && g_lastSceneSR.tex && (void*)g_t0 == g_lastSceneSR.tex &&
           g_lastSceneSR.hasSr && !sr.left && !sr.top && sr.right == (LONG)g_renderW && sr.bottom == (LONG)g_renderH;
}

// Pixel shader draw (jen identita; jen pro draw s podpisem roztazeni, tj. par za snimek)
static void* CurPS(IDirect3DDevice9* d) {
    IDirect3DPixelShader9* ps = nullptr;
    if (FAILED(d->GetPixelShader(&ps)) || !ps) return nullptr;
    ps->Release();
    return ps;
}

// Cast B: chyba hry u hledi kuse (docs: PROGRESS.md "Hledi kuse v casti B"). Maska hledi (PS se samplery texScene,
// texStencil, texDistort a konstantou uvOffsets) kresli pres cely povrch sceny a pocita
//   uvMasky = saturate((v0 - uvOffsets.xy) / uvOffsets.zw),
// kde v0 = UV celeho povrchu (stejne UV cte i texScene), ale engine plni uvOffsets z viewportu renderu
// (373/1707, 0, 960/1707, 1 misto 0.21875, 0, 0.5625, 1). Ostry kruh je pak ve stredu povrchu a 1,5x vetsi.
// Oprava: uvOffsets.xy a .zw vynasobit renderem / povrchem (v0 / r = UV sub-rectu), po draw vratit hodnotu hry.
// Jen v B, jen pro draw do sceny s viewportem pres cely povrch a jen pro presne tento bytecode (hash + CTAB).
static const UINT kScopePsSize = 668;
static const DWORD kScopePsHash = 0x243D5382u;  // FNV-1a 32 bytecode (zachyt capture_scopeA/B, A i B shodne)
static const char kScopePsCtab[] = "ps_3_0 texDistort=s2 texScene=s0 texStencil=s1 uvOffsets=c10";
static const UINT kScopePsReg = 10;
static bool g_scopeRestore;      // po draw vratit kScopePsReg na g_scopeOrig
static float g_scopeOrig[4];
static uint64_t g_scopeCacheFrame = ~0ull;  // cache vysledku podle ukazatele PS jen v ramci snimku
static struct { void* ps; bool match; } g_scopeCache[16];
static int g_scopeCacheN;

static DWORD Fnv1a(const uint8_t* p, size_t n) {
    DWORD h = 0x811C9DC5u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

static bool IsScopeMaskPS(IDirect3DPixelShader9* ps) {
    if (g_scopeCacheFrame != g_frame) {
        g_scopeCacheFrame = g_frame;
        g_scopeCacheN = 0;
    }
    for (int i = 0; i < g_scopeCacheN; ++i)
        if (g_scopeCache[i].ps == ps) return g_scopeCache[i].match;
    bool match = false;
    UINT size = 0;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size <= 64 * 1024) {
        std::vector<uint8_t> code((size + 3) & ~3u);
        if (SUCCEEDED(ps->GetFunction(code.data(), &size))) {
            const std::string ctab = ParseShader((const DWORD*)code.data()).text;
            if (ctab == kScopePsCtab) {
                const DWORD h = Fnv1a(code.data(), size);
                match = size == kScopePsSize && h == kScopePsHash;
                static bool s_warned;
                if (!match && !s_warned) {
                    s_warned = true;
                    Log("WARN: cast B: shader masky hledi s jinym bytecode (%u B, hash %08lX) - uvOffsets neopravuji",
                        size, (unsigned long)h);
                }
            }
        }
    }
    if (g_scopeCacheN < (int)(sizeof g_scopeCache / sizeof g_scopeCache[0])) g_scopeCache[g_scopeCacheN++] = {ps, match};
    return match;
}

// Pred draw (PreDraw, po zachytu a mimo potlacene draw): opravit uvOffsets masky hledi. Cast A = nic.
static void ScopeFixPre(IDirect3DDevice9* d) {
    if (!g_pathB || g_pathBad || !g_sceneEndDone || !g_sceneRT || g_rt0 != g_sceneRT || !g_vpKnown || g_vp.X ||
        g_vp.Y || g_vp.Width != g_rt0W || g_vp.Height != g_rt0H || g_rt0W != g_renderSurfW ||
        g_rt0H != g_renderSurfH || !g_renderW || !g_renderH || g_renderW > g_renderSurfW ||
        g_renderH > g_renderSurfH || (g_renderW == g_renderSurfW && g_renderH == g_renderSurfH))
        return;
    IDirect3DPixelShader9* ps = nullptr;
    if (FAILED(d->GetPixelShader(&ps)) || !ps) return;
    const bool match = IsScopeMaskPS(ps);
    ps->Release();
    if (!match || FAILED(d->GetPixelShaderConstantF(kScopePsReg, g_scopeOrig, 1))) return;
    const float rx = (float)g_renderW / (float)g_renderSurfW, ry = (float)g_renderH / (float)g_renderSurfH;
    const float* o = g_scopeOrig;
    const float c[4] = {o[0] * rx, o[1] * ry, o[2] * rx, o[3] * ry};
    if (FAILED(d->SetPixelShaderConstantF(kScopePsReg, c, 1))) return;
    g_scopeRestore = true;
    ++g_stB.scopeFixed;
    static bool s_logged;
    if (!s_logged) {
        s_logged = true;
        Log("cast B: maska hledi kuse - uvOffsets c%u (%.6f %.6f %.6f %.6f) -> (%.6f %.6f %.6f %.6f) podle renderu "
            "%ux%u v %ux%u (chyba hry: maska v UV celeho povrchu)",
            kScopePsReg, o[0], o[1], o[2], o[3], c[0], c[1], c[2], c[3], g_renderW, g_renderH, g_renderSurfW,
            g_renderSurfH);
    }
    if (g_capState == CAP_ACTIVE)
        Cap("# hledi: uvOffsets c%u -> % .6f % .6f % .6f % .6f (po draw zpet)", kScopePsReg, c[0], c[1], c[2], c[3]);
}

// Po draw: hodnota hry zpet (dalsi draw hry s timto PS pocitaji s puvodni konstantou)
static inline void ScopeFixPost(IDirect3DDevice9* d) {
    if (!g_scopeRestore) return;
    g_scopeRestore = false;
    d->SetPixelShaderConstantF(kScopePsReg, g_scopeOrig, 1);
}

// ---------------------------------------------------------------------------
// FXAA hry ([upscale] SkipGameFxaa, vychozi 1). FXAA neni samostatny pruchod: je to kombinace (dynamicky combo)
// tonemap shaderu engine_post_ps30 (bloom, korekce barev, vineta ... v jednom draw), ktery bezi az po mostu a cte
// _rt_FullFrameFB = vysledek DLSS / FSR. Rozbor shaderu (hl2_misc_dir.vpk, 320 kombinaci, 80 s FXAA): FXAA varianta
// a jeji sourozenec bez FXAA se lisi jen castmi FXAA 3.11 Quality; cast FXAA zacina
//   rangeMax * edgeThreshold (cN.z), max(cN.w = edgeThresholdMin, ...), if_lt range, ... -> stred = texldl FB.
// Draw proto nejde vynechat (zmizel by i bloom a tonemap). Misto toho se pro draw zvysi edgeThresholdMin
// (g_fxaaQualitySubpixEdge.w) na 1e30: podminka predcasneho konce plati vzdy, vystup = vzorek stredu FB (LOD 0) +
// bloom, tj. stejny vzorec jako varianta bez FXAA. Po draw hodnota hry zpet. Jen kdyz tento snimek prosel DLSS / FSR
// (most odeslal vysledek do sceny); upscaler vyp, most vypnuty nebo ladici rezim = FXAA hry beze zmeny.
// Rozpoznani: pri CreatePixelShader podle CTAB (g_fxaaQualityRcpFrame + g_fxaaQualitySubpixEdge) a kontroly, ze shader
// ma instrukci max se zdrojem cN.wwww (jinak WARN a shader se nemeni).
static std::mutex g_fxaaMx;
static std::unordered_map<void*, uint8_t> g_fxaaPs;  // PS s FXAA -> registr g_fxaaQualitySubpixEdge
static std::atomic<int> g_fxaaPsN{0};
static bool g_fxaaRestore;  // po draw vratit g_fxaaReg na g_fxaaOrig
static UINT g_fxaaReg;
static float g_fxaaOrig[4];
static unsigned g_fxaaDraws;  // statistika: neutralizovane draw (radek vykon v logu jen pri zmene)

// Registr cN g_fxaaQualitySubpixEdge, kdyz shader ma FXAA s ocekavanym tvarem; -1 = neni FXAA, -2 = jiny tvar
static int FxaaEdgeReg(const DWORD* code) {
    const std::string t = ParseShader(code).text;
    if (t.compare(0, 6, "ps_3_0") != 0) return -1;
    const size_t a = t.find(" g_fxaaQualitySubpixEdge=c");
    if (a == std::string::npos || t.find(" g_fxaaQualityRcpFrame=c") == std::string::npos) return -1;
    const int reg = atoi(t.c_str() + a + 26);
    if (reg < 0 || reg > 223) return -2;
    // max dst, src0, src1 (D3DSIO_MAX = 11): jeden ze zdroju = c[reg].wwww
    const DWORD* p = code + 1;
    for (int guard = 0; guard < 100000; ++guard) {
        const DWORD tok = *p;
        if (tok == 0x0000FFFF) break;
        const DWORD op = tok & 0xFFFF;
        if (op == 0xFFFE) {
            p += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        if (op == 11 && len == 3) {
            for (int s = 2; s <= 3; ++s) {
                const DWORD r = p[s];
                const DWORD type = ((r >> 28) & 7) | ((r >> 8) & 0x18);
                if (type == D3DSPR_CONST && (r & 0x7FF) == (DWORD)reg && ((r >> 16) & 0xFF) == 0xFF) return reg;
            }
        }
        p += 1 + len;
    }
    return -2;
}

static void FxaaNotePs(IDirect3DPixelShader9* ps, const DWORD* code) {
    const int reg = FxaaEdgeReg(code);
    std::lock_guard<std::mutex> lk(g_fxaaMx);
    if (reg >= 0) {
        g_fxaaPs[ps] = (uint8_t)reg;
    } else {
        g_fxaaPs.erase(ps);  // ukazatel po uvolnenem shaderu
        static bool s_warned;
        if (reg == -2 && !s_warned) {
            s_warned = true;
            Log("WARN: FXAA hry: shader s g_fxaaQualitySubpixEdge jineho tvaru (jina verze hry?) - FXAA tohoto shaderu "
                "se nemeni");
        }
    }
    g_fxaaPsN.store((int)g_fxaaPs.size(), std::memory_order_relaxed);
}

// Pred draw (PreDraw, po zachytu a mimo potlacene draw)
static void FxaaPre(IDirect3DDevice9* d) {
    if (!g_frameUpscaled || !g_sceneEndDone || !g_cfg.skipGameFxaa || !g_fxaaPsN.load(std::memory_order_relaxed))
        return;
    IDirect3DPixelShader9* ps = nullptr;
    if (FAILED(d->GetPixelShader(&ps)) || !ps) return;
    int reg = -1;
    {
        std::lock_guard<std::mutex> lk(g_fxaaMx);
        auto it = g_fxaaPs.find(ps);
        if (it != g_fxaaPs.end()) reg = it->second;
    }
    ps->Release();
    if (reg < 0 || FAILED(d->GetPixelShaderConstantF((UINT)reg, g_fxaaOrig, 1))) return;
    const float c[4] = {g_fxaaOrig[0], g_fxaaOrig[1], g_fxaaOrig[2], 1e30f};
    if (FAILED(d->SetPixelShaderConstantF((UINT)reg, c, 1))) return;
    g_fxaaReg = (UINT)reg;
    g_fxaaRestore = true;
    ++g_fxaaDraws;
    static bool s_logged;
    if (!s_logged) {
        s_logged = true;
        Log("FXAA hry: po DLSS / FSR vynechano (engine_post s FXAA, c%u.w edgeThresholdMin %.4f -> 1e30, hodnoty "
            "hry (%.4f %.4f %.4f) zustavaji; [upscale] SkipGameFxaa=0 vypne)",
            g_fxaaReg, g_fxaaOrig[3], g_fxaaOrig[0], g_fxaaOrig[1], g_fxaaOrig[2]);
    }
    if (g_capState == CAP_ACTIVE) Cap("# FXAA hry: c%u.w -> 1e30 (po draw zpet)", g_fxaaReg);
}

static inline void FxaaPost(IDirect3DDevice9* d) {
    if (!g_fxaaRestore) return;
    g_fxaaRestore = false;
    d->SetPixelShaderConstantF(g_fxaaReg, g_fxaaOrig, 1);
}

// Cast B: dynamicka svetla enginu ("NewLights": svetla tramvaje, projektovana svetla se stiny) chybi. Neni to
// konstanta shaderu: CNewRenderer::RenderLightBuffer (client.dll) cely pruchod svetel preskoci, kdyz se velikost
// pohledu (CViewSetup width/height = render po mat_viewportscale) nerovna velikosti light bufferu (cela obrazovka):
// "ERROR!! - Main View NewLights will only work with FullScreen RT". Zachyty capture_lights4k: v A RT0 RGBA16F +
// RT1 G16R16F + RT2 RGBA16F pres celou obrazovku (VS 8EF3F660, 0-4 draw), v B se ten pruchod ani nenastavi.
// Pokus ([upscale] LightsB=1, vychozi vypnuto): v kontrole porovnat nezmensenou velikost (m_nUnscaledWidth +0x14,
// m_nUnscaledHeight +0x20) misto zmensene (+0x10, +0x18) - 2 bajty displacementu, zapis jednoho bajtu je atomicky.
// V A jsou obe velikosti stejne (zaplata se ale v A stejne vraci zpet). Pruchod pak kresli do sub-rectu (viewport z
// CViewSetup). Neovereno: zda shadery svetel a dopredny pruchod pocitaji UV light bufferu / G-bufferu spravne (viz
// PROGRESS.md "Svetla v casti B").
static const uint8_t kLightsSig[] = {0x8B, 0x8F, 0x44, 0x04, 0x00, 0x00, 0x8B, 0x01, 0xFF, 0x50, 0x0C, 0x39,
                                     0x43, 0x10, 0x75, 0x59, 0x8B, 0x8F, 0x44, 0x04, 0x00, 0x00, 0x8B, 0x01,
                                     0xFF, 0x50, 0x10, 0x39, 0x43, 0x18, 0x75, 0x49};
static const size_t kLightsOffW = 13, kLightsOffH = 29;  // displacement [ebx+10h] a [ebx+18h] v kLightsSig
static uint8_t* g_lightsAt;  // zacatek kLightsSig v client.dll (nullptr = nenalezeno / jeste nehledano)
static int g_lightsState;    // 0 = nehledano, 1 = nalezeno, -1 = nelze (bez dalsich pokusu)

static uint8_t* LightsFind() {
    HMODULE m = GetModuleHandleW(L"client.dll");
    if (!m) return nullptr;
    const auto* dos = (const IMAGE_DOS_HEADER*)m;
    const auto* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)m + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* found = nullptr;
    int hits = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = (uint8_t*)m + sec->VirtualAddress;
        const size_t n = sec->Misc.VirtualSize;
        for (size_t k = 0; k + sizeof kLightsSig <= n; ++k)
            if (p[k] == kLightsSig[0] && !memcmp(p + k, kLightsSig, sizeof kLightsSig)) {
                found = p + k;
                ++hits;
            }
    }
    if (hits != 1) {
        Log("WARN: svetla v casti B: kontrola velikosti light bufferu v client.dll nenalezena (%d shod) - zaplata "
            "se nepouzije",
            hits);
        g_lightsState = -1;
        return nullptr;
    }
    Log("svetla v casti B: kontrola velikosti light bufferu nalezena v client.dll+0x%lX",
        (unsigned long)(found - (uint8_t*)m));
    g_lightsState = 1;
    return found;
}

static bool LightsWrite(bool on) {
    uint8_t* w = g_lightsAt + kLightsOffW;
    uint8_t* h = g_lightsAt + kLightsOffH;
    const uint8_t vw = on ? 0x14 : 0x10, vh = on ? 0x20 : 0x18;
    DWORD old = 0;
    if (!VirtualProtect(g_lightsAt, sizeof kLightsSig, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(volatile uint8_t*)w = vw;
    *(volatile uint8_t*)h = vh;
    DWORD tmp = 0;
    VirtualProtect(g_lightsAt, sizeof kLightsSig, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), g_lightsAt, sizeof kLightsSig);
    return true;
}

// Konec snimku: zaplata jen v casti B a jen s [upscale] LightsB=1; jinak (a pri odchodu z B) puvodni bajty.
static void LightsPatchUpdate() {
    if (g_lightsRtSeen) {
        if (g_pathB) ++g_stB.lightFrames;
        g_lightsRtSeen = false;
    }
    // bez hacku CreatePixelShader (start s LightsB=0, zapnuto az z menu) zaplatu nezapinat - LightsFixPre by nemel
    // g_psInfo a svetla by kreslila mimo sub-rect; plati az po restartu (S2)
    const bool want = g_cfg.lightsB && g_pathB && o_CreatePS != nullptr;
    if (want == g_lightsPatched) return;
    if (want && g_lightsState == 0) g_lightsAt = LightsFind();
    if (!g_lightsAt) return;
    if (!LightsWrite(want)) {
        Log("WARN: svetla v casti B: VirtualProtect selhalo (%lu) - zaplata %s", GetLastError(),
            want ? "nepouzita" : "ZUSTAVA");
        if (want) {
            g_lightsState = -1;
            g_lightsAt = nullptr;
        }
        return;
    }
    g_lightsPatched = want;
    Log("svetla v casti B: zaplata client.dll %s (kontrola light bufferu %s)", want ? "zapnuta" : "vypnuta",
        want ? "podle nezmensene velikosti pohledu" : "puvodni");
}

// Pruchod svetel (MRT: RT1 G16R16F pres cely povrch sceny) v tomto snimku - jen pro radek vykon v casti B
static inline void LightsNoteRT(DWORD i, const D3DSURFACE_DESC& desc) {
    if (i == 1 && g_pathB && desc.Format == D3DFMT_G16R16F && desc.Width == g_renderSurfW &&
        desc.Height == g_renderSurfH)
        g_lightsRtSeen = true;
}

// Se zaplatou kresli pruchod svetel do sub-rectu, ale shadery pocitaji UV z vPos a velikosti POHLEDU (render), zatimco
// G-buffer i light buffer jsou textury velikosti povrchu se sub-rectem vlevo nahore (zachyt capture_lightsB_test, A vs
// BL; v A pohled = povrch, takze to v A sedi):
//  1) shadery svetel (CTAB viewSize=c50, matCamProjToWorld=c52..55, matCamProjToWorld_viewmodel=c56..59, texDepth=s0;
//     3 varianty: bodove/spot se stinem i bez): uv = (vPos + 0.5) * c50.xy -> texld G-bufferu, a z tehoz uv
//     ndc = (2u - 1, 1 - 2v), pozice = mat * (ndc, 1 - hloubka, 1). Engine plni c50 = 1/render (B: 1/1707, 1/960).
//     Oprava: c50.xy = 1/povrch (texld spravne); ndc ze shaderu je pak X' = 2u_s - 1, skutecne ndc = a X' + (a - 1),
//     Y = b Y' + (1 - b), a = povrch/render po osach -> slozit do radku matic: (m0 a, m1 b, m2, m3 + m0 (a-1) + m1 (1-b)).
//  2) dopredne shadery ctouci light buffer (CTAB g_CamParams=cN, kazdy jen "mad uv, vPos, cN, cN.wz.."): engine plni
//     (1/rW, 1/rH, 0.5/rW, 0.5/rH) -> v B cte light buffer 1,5x mimo (cerveny smeti mimo sub-rect). Oprava: tytez
//     hodnoty z povrchu (presne to, co by engine dal v A).
// Jen v B se zaplatou LightsB, jen draw s viewportem = render (0,0) do cile velikosti povrchu, jen kdyz konstanta
// hry presne odpovida renderu (jinak nic: vodni odrazy 1024x1024, stinovy atlas). Po draw hodnoty hry zpet.
// Dalsi (zachyt capture_lightsB_tram, A vs B v jedouci tramvaji):
//  3) stinova maska slunce (CSM; CTAB g_matCSMShadowBuffer=c94..97, texS0=s0; 2 varianty) kresli celoobrazovkovy
//     obdelnik pres CELY povrch (VP 2560x1440) s uv v0 = 0..1 povrchu: hloubku cte spravne (pixel = pixel), ale
//     ndc = (2u - 1, 1 - 2v) bere z uv povrchu -> v sub-rectu spatna poloha ve svete. Dopredne shadery ctou masku
//     (s15) stejne jako light buffer pres vPos * g_CamParams (bod 2), takze po oprave 2 by bez opravy 3 cetly masku
//     spocitanou pro jine misto. Oprava: tytez slozeni matice c94..97 jako u svetel (u_s = u povrchu).
//  4) paprsky slunce (CTAB _SunPosUV=c10; rozmazani k slunci a slozeni do sceny pres cely povrch, uv 0..1 povrchu):
//     poloha slunce je v UV pohledu -> v povrchu UV * render/povrch; polomery kotouce c10.z a c12.w (_sunDiskColorInner)
//     taky. Zare (grfinal_ps30): vzdalenost od slunce d = |(c10.xy - v0) * (c10.w, 1)| pocita v UV povrchu, tj.
//     render/povrch krat mensi nez v UV pohledu -> zare max(0, 1 - d^c11.w)^c9.w je v B 1,5x sirsi a s opravenou
//     polohou slunce sviti i do rohu se sluncem mimo zaber (docs/svetla-B-projeti.md N1). Konstantou nejde ->
//     O1 (LightsBSunFix=1): pri CreatePixelShader se ke znamym variantam (otisk FNV-1a) vytvori dvojce s jednou
//     vlozenou instrukci "mul rN.yz, rN, c31.xxyw" hned za "add rN.yz, c10.xxyw, -v0.xxyw" (c31 je ve vsech
//     variantach volny) a v B se pro draw podstrci s c31 = (povrchW/renderW, povrchH/renderH): vzdalenost je pak
//     v UV pohledu jako v A. Polomery kotouce c10.z / c12.w se pro dvojce neskaluji (uz meri v UV pohledu).
enum : uint8_t { LPS_NONE, LPS_LIGHT, LPS_CAMPARAMS, LPS_CSM, LPS_SUN, LPS_SUNDISK };
struct LightPsInfo {
    uint8_t kind;
    uint8_t reg;     // LPS_CAMPARAMS: registr g_CamParams
    uint8_t depthS;  // mekke castice (g_DepthFeatheringConstants): sampler DepthSampler, 0xFF = nema (bod 5)
    uint8_t v = 0, m = 0, mv = 0xFF;  // LPS_LIGHT: viewSize, matCamProjToWorld (4), _viewmodel (4, 0xFF = nema)
    bool grfinal = false;             // LPS_SUN/LPS_SUNDISK: skladani zare do sceny (CTAB _sunRaysColor=c11)
    IDirect3DPixelShader9* twin = nullptr;  // O1: upravena kopie grfinal (vlastni reference, SunTwinCreate)
};
static std::mutex g_psInfoMx;
static std::unordered_map<void*, LightPsInfo> g_psInfo;  // jen s [upscale] LightsB (H_CreatePS)
static const UINT kLightFixMaxRegs = 10;
static UINT g_lightFixReg, g_lightFixCount;  // po draw vratit g_lightFixOrig do c<reg>..c<reg+count-1>
static float g_lightFixOrig[kLightFixMaxRegs * 4];

static LightPsInfo ClassifyLightPs(const DWORD* code) {
    LightPsInfo li = {LPS_NONE, 0, 0xFF};
    if (!code || (code[0] >> 16) != 0xFFFF) return li;
    const std::string t = ParseShader(code).text + " ";
    const size_t ds = t.find(" DepthSampler=s");
    if (ds != std::string::npos && t.find(" g_DepthFeatheringConstants=c") != std::string::npos) {
        const char* s = t.c_str() + ds + 15;
        char* e = nullptr;
        const long n = strtol(s, &e, 10);
        if (e != s && *e == ' ' && n >= 0 && n < 16) li.depthS = (uint8_t)n;
    }
    // Svetla G-bufferu (gblight c50/c52/c56, gbxog c50/c52 bez _viewmodel, gbcsm c32/c33 - slunce): viewSize = 1/render,
    // uv = vPos (+0.5) * viewSize, ndc = (2u - 1, 1 - 2v) -> matCamProjToWorld; vse v souvislem rozsahu <= 10 registru.
    {
        auto regOf = [&](const char* key, int cnt) -> int {
            const size_t q = t.find(key);
            if (q == std::string::npos) return -1;
            const char* s = t.c_str() + q + strlen(key);
            char* e = nullptr;
            const long r = strtol(s, &e, 10);
            if (e == s || r < 0 || r > 220) return -1;
            if (cnt == 1) return *e == ' ' ? (int)r : -1;
            if (e[0] != '.' || e[1] != '.') return -1;
            const long r2 = strtol(e + 2, &e, 10);
            return (r2 - r + 1 == cnt && *e == ' ') ? (int)r : -1;
        };
        const int v = regOf(" viewSize=c", 1), m = regOf(" matCamProjToWorld=c", 4);
        const int mv = regOf(" matCamProjToWorld_viewmodel=c", 4);
        if (v >= 0 && m >= 0 && t.find(" texDepth=s0 ") != std::string::npos) {
            const int lo = std::min(v, std::min(m, mv >= 0 ? mv : m));
            const int hi = std::max(v, std::max(m + 3, mv >= 0 ? mv + 3 : m + 3));
            if (hi - lo + 1 <= (int)kLightFixMaxRegs) {
                li.kind = LPS_LIGHT;
                li.v = (uint8_t)v;
                li.m = (uint8_t)m;
                li.mv = mv >= 0 ? (uint8_t)mv : 0xFF;
                return li;
            }
        }
    }
    if (t.find(" g_matCSMShadowBuffer=c94..97 ") != std::string::npos && t.find(" texS0=s0 ") != std::string::npos) {
        li.kind = LPS_CSM;
        return li;
    }
    if (t.find(" _SunPosUV=c10 ") != std::string::npos) {
        li.kind = t.find(" _sunDiskColorInner=c12 ") != std::string::npos ? LPS_SUNDISK : LPS_SUN;
        li.grfinal = t.find(" _sunRaysColor=c11 ") != std::string::npos;
        return li;
    }
    const size_t p = t.find(" g_CamParams=c");
    if (p != std::string::npos) {
        const char* s = t.c_str() + p + 14;
        char* e = nullptr;
        const long r = strtol(s, &e, 10);
        if (e != s && *e == ' ' && r >= 0 && r < 224) {
            li.kind = LPS_CAMPARAMS;
            li.reg = (uint8_t)r;
        }
    }
    return li;
}

static void DetRecordPs(IDirect3DPixelShader9* ps, const DWORD* code);  // detektor casti B (nize)

// O1: zname varianty grfinal_ps30 (hl2_misc_dir.vpk, Black Mesa 2026-11; otisk FNV-1a jako DetRecordPs). Jina verze
// hry = jiny otisk -> zadna kopie, WARN a puvodni chovani (bod 4 se skalovanim polomeru).
static const DWORD kSunTwinHashes[] = {0xA43F9965u, 0x3F62F704u, 0x49BD7FC4u, 0x9F9C725Du, 0xA29262A9u};
static const UINT kSunTwinReg = 31;

static IDirect3DPixelShader9* SunTwinCreate(IDirect3DDevice9* d, const DWORD* code, IDirect3DPixelShader9* ps) {
    UINT fnSize = 0, n = 0;
    uint8_t use[kConstUseRegs];
    if (FAILED(ps->GetFunction(nullptr, &fnSize)) || fnSize < 8 || fnSize > 256 * 1024 ||
        (code[0] & 0xFFFFFF00u) != 0xFFFF0300u || !ShaderConstUse(code, fnSize / 4, use, &n)) {
        Log("WARN: cast B + LightsB: zare slunce - shader grfinal nejde rozebrat, bez upravene kopie");
        return nullptr;
    }
    const DWORD h = Fnv1a((const uint8_t*)code, (size_t)n * 4);
    bool known = false;
    for (DWORD k : kSunTwinHashes) known |= k == h;
    if (!known) {
        Log("WARN: cast B + LightsB: zare slunce - neznamy otisk grfinal %08lX (%u B, jina verze hry?) - zare v B bez "
            "opravy (puvodni chovani)",
            (unsigned long)h, n * 4);
        return nullptr;
    }
    // vzor: add rN.yz, c10.xxyw, -v0.xxyw (instrukce, cil temp N maska yz, c10 swizzle xxyw, -v0 swizzle xxyw)
    UINT at = 0, hits = 0;
    for (UINT i = 1; i < n - 1;) {
        const DWORD tok = code[i];
        if ((tok & 0xFFFF) == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        if (tok == 0x03000002u && i + 3 < n && (code[i + 1] & 0xFFFFF800u) == 0x80060000u &&
            (code[i + 1] & 0x7FF) < 32 && code[i + 2] == 0xA0D0000Au && code[i + 3] == 0x91D00000u) {
            at = i;
            ++hits;
        }
        i += 1 + ((tok >> 24) & 0xF);
    }
    if (hits != 1 || (use[kSunTwinReg] & 0x1F)) {
        Log("WARN: cast B + LightsB: zare slunce - grfinal %08lX: vzor %u x, c%u %s - bez upravene kopie",
            (unsigned long)h, hits, kSunTwinReg, (use[kSunTwinReg] & 0x1F) ? "obsazen" : "volny");
        return nullptr;
    }
    const DWORD reg = code[at + 1] & 0x7FF;
    std::vector<DWORD> tw(code, code + at + 4);
    const DWORD ins[4] = {0x03000005u, code[at + 1], 0x80E40000u | reg, 0xA0D00000u | kSunTwinReg};  // mul rN.yz, rN, c31.xxyw
    tw.insert(tw.end(), ins, ins + 4);
    tw.insert(tw.end(), code + at + 4, code + n);
    IDirect3DPixelShader9* twin = nullptr;
    const HRESULT hr = o_CreatePS(d, tw.data(), &twin);
    if (FAILED(hr) || !twin) {
        Log("WARN: cast B + LightsB: zare slunce - kopii grfinal %08lX nejde vytvorit (0x%08lX)", (unsigned long)h,
            (unsigned long)hr);
        return nullptr;
    }
    Log("cast B + LightsB: zare slunce - grfinal %08lX: upravena kopie %p (mul r%lu.yz, r%lu, c%u.xxyw za DWORD %u)",
        (unsigned long)h, (void*)twin, (unsigned long)reg, (unsigned long)reg, kSunTwinReg, at);
    return twin;
}

static HRESULT STDMETHODCALLTYPE H_CreatePS(IDirect3DDevice9* d, const DWORD* code, IDirect3DPixelShader9** out) {
    const HRESULT hr = o_CreatePS(d, code, out);
    if (SUCCEEDED(hr) && out && *out && g_cfg.skipGameFxaa) FxaaNotePs(*out, code);
    if (SUCCEEDED(hr) && out && *out) MipNotePs(*out, code);
    if (SUCCEEDED(hr) && out && *out && (g_cfg.lightsB || g_cfg.lightsBDetect)) {
        LightPsInfo li = ClassifyLightPs(code);
        if (li.grfinal && g_cfg.lightsBSunFix) li.twin = SunTwinCreate(d, code, *out);
        if (g_cfg.lightsBDetect) DetRecordPs(*out, code);
        IDirect3DPixelShader9* oldTwin = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_psInfoMx);
            auto it = g_psInfo.find(*out);  // ukazatel po uvolnenem shaderu
            if (it != g_psInfo.end()) {
                oldTwin = it->second.twin;
                g_psInfo.erase(it);
            }
            if (li.kind != LPS_NONE || li.depthS != 0xFF) g_psInfo[*out] = li;
        }
        if (oldTwin) oldTwin->Release();
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Detektor pro cast B ([upscale] LightsBDetect=1, vychozi 0; nezavisle na LightsB): hleda dalsi shadery, ktere v B
// pocitaji s velikosti renderu. Pri CreatePixelShader: vPos ano/ne, pojmenovane c-registry z CTAB, hash. Pri draw v B
// (prvnich kDetChecks draw kazdeho shaderu): hodnoty pojmenovanych konstant PS i VS porovna s 1/rW, 1/rH, rW, rH,
// 0.5/rW, 0.5/rH, rW/sW, rH/sH, sW/rW, sH/rH; shoda = radek "detektor B:" do logu (jednou na shader a registr).
// Navic jednou na PS: draw pres cely povrch do cile velikosti povrchu pred koncem sceny (uv 0..1 povrchu, ale obsah
// v sub-rectu) a PS s vPos kreslene do sub-rectu. Shadery, ktere LightsB uz opravuje, vynechava (jejich registry).
struct DetPs {
    bool vpos = false;
    DWORD hash = 0;
    UINT size = 0;
    std::string text;
    std::vector<std::pair<uint16_t, uint16_t>> cregs;  // (prvni registr, pocet)
    std::vector<uint8_t> cuse;                         // O3: ctene slozky (ShaderConstUse), prazdne = nezname
    unsigned checks = 0;
    bool fullLogged = false, vposLogged = false;
};
static std::unordered_map<void*, DetPs> g_detPs;     // pod g_psInfoMx
static std::unordered_map<void*, unsigned> g_detVs;  // pocet kontrol VS (jen vlakno renderu)
static std::unordered_set<uint64_t> g_detLogged;     // (shader << 16 | registr | 0x8000 pro VS)
static unsigned g_detLines;
static const unsigned kDetChecks = 6, kDetMaxLines = 400;

static void DetParseRegs(const std::string& text, std::vector<std::pair<uint16_t, uint16_t>>& out) {
    // text z ParseShader: " jmeno=c5" nebo " jmeno=c52..55"
    size_t p = 0;
    while ((p = text.find("=c", p)) != std::string::npos) {
        p += 2;
        char* e = nullptr;
        const long a = strtol(text.c_str() + p, &e, 10);
        long b = a;
        if (e && e[0] == '.' && e[1] == '.') b = strtol(e + 2, &e, 10);
        if (a >= 0 && b >= a && b < 224) out.push_back({(uint16_t)a, (uint16_t)std::min<long>(b - a + 1, 8)});
    }
}

// Pad 2 (2026-11-18, dump Steamu crash_bms.exe_20261118200625_1.dmp): shaderapidx9 pri startu vytvari na zasobniku
// testovaci ps_1_1 (def c0, 0,1,0,1; mov r0, c0). ps_1_x nema v tokenu instrukce delku (bity 24-27 jsou 0, v parametrech
// modifikatory, v def float hodnoty), puvodni pruchod tokeny se pres ne dostal za konec shaderu a cetl zasobnik az do
// access violation (zachytil ho tier0 CatchAndWriteMiniDump -> TerminateProcess, bez WER). Detektor proto bere jen
// ps_2_0+ (vPos je stejne jen v ps_3_0; tam delka instrukci plati, overeno na vsech ~5,7 mil. PS kombinacich z VPK hry)
// a pruchod navic omezuje velikosti bytecode z GetFunction. Hash z delky nalezene pruchodem (jako drive).
static void DetRecordPs(IDirect3DPixelShader9* ps, const DWORD* code) {
    if (!ps || !code || (code[0] >> 16) != 0xFFFF || ((code[0] >> 8) & 0xFF) < 2) return;
    UINT fnSize = 0;
    if (FAILED(ps->GetFunction(nullptr, &fnSize)) || fnSize < 8 || fnSize > 256 * 1024) return;
    const UINT n = fnSize / 4;
    DetPs di;
    // vPos = dcl s registrem typu MISCTYPE (17) cislo 0
    UINT i = 1;
    while (i < n && code[i] != 0x0000FFFF) {
        const DWORD tok = code[i];
        if ((tok & 0xFFFF) == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF;
        if ((tok & 0xFFFF) == 0x1F && len >= 2 && i + 2 < n) {
            const DWORD reg = code[i + 2];
            const DWORD type = ((reg >> 28) & 7) | ((reg >> 8) & 0x18);
            if (type == 17 && (reg & 0x7FF) == 0) di.vpos = true;
        }
        i += 1 + len;
    }
    if (i >= n) return;  // konec shaderu nenalezen v mezich: nezaznamenavat
    di.text = ParseShader(code).text;
    di.size = (i + 1) * 4;
    di.cuse.resize(kConstUseRegs);
    if (!ShaderConstUse(code, n, di.cuse.data(), nullptr)) di.cuse.clear();
    di.hash = Fnv1a((const uint8_t*)code, di.size);
    DetParseRegs(di.text, di.cregs);
    std::lock_guard<std::mutex> lk(g_psInfoMx);
    g_detPs[ps] = std::move(di);
}

// Shoda hodnoty s velikosti renderu. O3: "silne" = velikost pixelu (1/r, 0.5/r, 2/r) - nahodou se skoro nevyskytuji;
// "slabe" = rozmer v pixelech a pomery render/povrch (960, 1,5, 0,667 maji i matice a materialy: cModel, cLightScale,
// g_DiffuseModulation ...) - ty se hlasi jen v paru osa X + osa Y v ruznych slozkach registru (DetCheckReg).
struct DetHit {
    const char* name;
    uint8_t axis;  // 0 = X (sirka), 1 = Y (vyska)
    bool strong;
};
static bool DetMatch(float v, float rW, float rH, float sW, float sH, DetHit hits[2], int& nHits) {
    nHits = 0;
    if (v == 0.0f) return false;
    const struct { float x; const char* n; uint8_t axis; bool strong; } k[] = {
        {1.0f / rW, "1/renderW", 0, true},          {1.0f / rH, "1/renderH", 1, true},
        {rW, "renderW", 0, false},                  {rH, "renderH", 1, false},
        {0.5f / rW, "0.5/renderW", 0, true},        {0.5f / rH, "0.5/renderH", 1, true},
        {rW / sW, "renderW/povrchW", 0, false},     {rH / sH, "renderH/povrchH", 1, false},
        {sW / rW, "povrchW/renderW", 0, false},     {sH / rH, "povrchH/renderH", 1, false},
        {2.0f / rW, "2/renderW", 0, true},          {2.0f / rH, "2/renderH", 1, true}};
    for (const auto& e : k)
        if (fabsf(v - e.x) <= 1e-4f * fabsf(e.x) && nHits < 2) hits[nHits++] = {e.n, e.axis, e.strong};
    return nHits > 0;
}

// Jeden registr: slozky, ktere shader cte (use: bity xyzw; 0xFF = nezname -> vsechny; bit 4 = def v shaderu -> nic).
// Vrati true a popis "slozka N = jmeno[, slozka M = jmeno]", kdyz je nalez silny nebo par slabych X+Y.
static bool DetCheckReg(const float* v, uint8_t use, float rW, float rH, float sW, float sH, char* desc, size_t descN) {
    if (use != 0xFF && (use & 0x10)) return false;
    int strongC = -1, wC = -1, hC = -1;
    const char *strongN = nullptr, *wN = nullptr, *hN = nullptr;
    for (int c = 0; c < 4; ++c) {
        if (use != 0xFF && !(use & (1u << c))) continue;
        DetHit h[2];
        int nh = 0;
        if (!DetMatch(v[c], rW, rH, sW, sH, h, nh)) continue;
        for (int j = 0; j < nh; ++j) {
            if (h[j].strong) {
                if (strongC < 0) strongC = c, strongN = h[j].name;
            } else if (h[j].axis == 0) {
                if (wC < 0) wC = c, wN = h[j].name;
            } else if (hC < 0 || hC == wC) {
                hC = c, hN = h[j].name;
            }
        }
    }
    if (strongC >= 0) {
        snprintf(desc, descN, "slozka %d = %s", strongC, strongN);
        return true;
    }
    if (wC >= 0 && hC >= 0 && wC != hC) {
        snprintf(desc, descN, "slozka %d = %s, slozka %d = %s", wC, wN, hC, hN);
        return true;
    }
    return false;
}

static void DetLog(const char* fmt, ...) {
    if (g_detLines >= kDetMaxLines) return;
    if (++g_detLines == kDetMaxLines) {
        Log("detektor B: dosazen limit %u radku, dalsi nalezy se nelogujou", kDetMaxLines);
        return;
    }
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Log("detektor B: %s", buf);
}

static void DetectPre(IDirect3DDevice9* d) {
    if (!g_cfg.lightsBDetect || !g_pathB || g_pathBad || !g_renderW || !g_renderH || g_renderW >= g_renderSurfW)
        return;
    const float rW = (float)g_renderW, rH = (float)g_renderH, sW = (float)g_renderSurfW, sH = (float)g_renderSurfH;
    const UINT vpW = g_vpKnown ? g_vp.Width : g_rt0W, vpH = g_vpKnown ? g_vp.Height : g_rt0H;
    char where[160];
    snprintf(where, sizeof where, "RT %ux%u fmt %u, VP %ux%u, %s konce sceny", g_rt0W, g_rt0H, (unsigned)g_rt0Fmt, vpW,
             vpH, g_sceneEndDone ? "po" : "pred");
    // PS
    IDirect3DPixelShader9* ps = nullptr;
    if (SUCCEEDED(d->GetPixelShader(&ps)) && ps) {
        DetPs di;
        bool have = false, check = false;
        LightPsInfo li = {LPS_NONE, 0, 0xFF};
        {
            std::lock_guard<std::mutex> lk(g_psInfoMx);
            auto it = g_detPs.find(ps);
            if (it != g_detPs.end()) {
                have = true;
                check = it->second.checks < kDetChecks;
                if (check) ++it->second.checks;
                di = it->second;  // kopie (male), at se neloguje pod zamkem
            }
            auto jt = g_psInfo.find(ps);
            if (jt != g_psInfo.end()) li = jt->second;
        }
        const bool fixedWhole = li.kind == LPS_LIGHT || li.kind == LPS_CSM || li.kind == LPS_SUN ||
                                li.kind == LPS_SUNDISK;
        if (have && check && !fixedWhole) {
            const bool full = !g_sceneEndDone && g_rt0W == g_renderSurfW && g_rt0H == g_renderSurfH && vpW == g_renderSurfW &&
                              vpH == g_renderSurfH;
            if (full && !di.fullLogged) {
                DetLog("PS %p (hash %08lX, %u B%s) kresli pres CELY povrch pred koncem sceny (%s) - uv 0..1 povrchu, obsah "
                       "jen v sub-rectu %ux%u: %s",
                       (void*)ps, (unsigned long)di.hash, di.size, di.vpos ? ", vPos" : "", where, g_renderW, g_renderH,
                       di.text.c_str());
                std::lock_guard<std::mutex> lk(g_psInfoMx);
                g_detPs[ps].fullLogged = true;
            }
            const bool sub = vpW == g_renderW && vpH == g_renderH;
            if (di.vpos && sub && !di.vposLogged && li.kind != LPS_CAMPARAMS) {
                DetLog("PS %p (hash %08lX, %u B) pouziva vPos a kresli do sub-rectu (%s): %s", (void*)ps,
                       (unsigned long)di.hash, di.size, where, di.text.c_str());
                std::lock_guard<std::mutex> lk(g_psInfoMx);
                g_detPs[ps].vposLogged = true;
            }
            for (const auto& rc : di.cregs) {
                float v[8 * 4];
                if (FAILED(d->GetPixelShaderConstantF(rc.first, v, rc.second))) continue;
                for (UINT r = 0; r < rc.second; ++r) {
                    const UINT reg = rc.first + r;
                    if (li.kind == LPS_CAMPARAMS && reg == li.reg) continue;  // opravuje LightsB
                    const uint8_t use = reg < di.cuse.size() ? di.cuse[reg] : 0xFF;
                    char m[96];
                    if (!DetCheckReg(&v[r * 4], use, rW, rH, sW, sH, m, sizeof m)) continue;
                    const uint64_t key = ((uint64_t)(uintptr_t)ps << 16) | reg;
                    if (!g_detLogged.insert(key).second) continue;
                    DetLog("PS %p (hash %08lX, %u B%s) c%u = (%g %g %g %g), %s, cte %s%s%s%s (%s): %s", (void*)ps,
                           (unsigned long)di.hash, di.size, di.vpos ? ", vPos" : "", reg, v[r * 4], v[r * 4 + 1],
                           v[r * 4 + 2], v[r * 4 + 3], m, (use & 1) ? "x" : "", (use & 2) ? "y" : "",
                           (use & 4) ? "z" : "", (use & 8) ? "w" : "", where, di.text.c_str());
                }
            }
        }
        ps->Release();
    }
    // VS: pojmenovane konstanty ze stinu g_vsConst (puvodni hodnoty hry), mimo clip-matice
    const ShaderInfo* si = g_curInfo;
    if (si && g_curVS) {
        unsigned& n = g_detVs[g_curVS];
        if (n < kDetChecks) {
            ++n;
            std::vector<std::pair<uint16_t, uint16_t>> regs;
            DetParseRegs(si->text, regs);
            for (const auto& rc : regs)
                for (UINT r = 0; r < rc.second; ++r) {
                    const UINT reg = rc.first + r;
                    bool clip = false;
                    for (uint8_t k = 0; k < si->nClip; ++k)
                        if (reg >= si->clipReg[k] && reg < si->clipReg[k] + 4u) clip = true;
                    if (clip || reg >= 256) continue;
                    // ViewportTransformScaled (spritecard): engine ho v B plni spravne (viewport/RT), mekke castice
                    // s nim LightsB nemeni (O2) -> neni nalez
                    if ((int)reg == si->vpScaledReg) continue;
                    const float* v = &g_vsConst[reg * 4];
                    const uint8_t use = reg < si->cuse.size() ? si->cuse[reg] : 0xFF;
                    char m[96];
                    if (!DetCheckReg(v, use, rW, rH, sW, sH, m, sizeof m)) continue;
                    const uint64_t key = ((uint64_t)(uintptr_t)g_curVS << 16) | 0x8000 | reg;
                    if (!g_detLogged.insert(key).second) continue;
                    DetLog("VS %p c%u = (%g %g %g %g), %s (%s): %s", g_curVS, reg, v[0], v[1], v[2], v[3], m, where,
                           si->text.c_str());
                }
        }
    }
}

static inline bool NearRel(float v, float want) { return fabsf(v - want) <= 1e-4f * fabsf(want); }

// Radky matice (dp4 s (ndc.x, ndc.y, z, 1)) slozit s prevodem ndc z uv povrchu na ndc pohledu (viz bod 1)
static void FoldNdcRows(float* rows, int n, float a, float b) {
    for (int row = 0; row < n; ++row) {
        float* m = &rows[row * 4];
        m[3] += m[0] * (a - 1.0f) + m[1] * (1.0f - b);
        m[0] *= a;
        m[1] *= b;
    }
}

// 5) mekke castice (prach/opar v kanonu; CTAB DepthSampler=sN + g_DepthFeatheringConstants): uv pro hloubku sceny
//    pocita PS z clip pozice (xy/w * 0.5 + 0.5, konstanty jsou literaly) = uv POHLEDU, ale textura hloubky (R32F
//    G-buffer) ma velikost povrchu se sub-rectem -> v B cte hloubku jinde (zaprasene, tmave vlnky). Konstantou nejde.
//    Oprava: jednou za snimek StretchRect sub-rectu hloubky na vlastni texturu velikosti povrchu (POINT) a pro tyto
//    draw ji dat do sampleru N, po draw puvodni textura zpet.
static IDirect3DTexture9* g_softTex;          // nase kopie hloubky (D3DPOOL_DEFAULT, uvolnit pred Resetem)
static IDirect3DSurface9* g_softSurf;
static IDirect3DSurface9* g_softSrc;          // zdroj posledni kopie (jen identita)
static uint64_t g_softFrame = ~0ull;          // snimek posledni kopie
static D3DFORMAT g_softFmt;
static UINT g_softW, g_softH;
static DWORD g_softStage = 0xFFFFFFFF;        // po draw vratit g_softOrig do tohoto sampleru
static IDirect3DBaseTexture9* g_softOrig;     // drzena reference (uvolni SoftDepthPost)
static bool g_softBroken;                     // vytvoreni / kopie selhaly -> uz nezkouset

static void SoftDepthRelease() {
    if (g_softSurf) g_softSurf->Release();
    if (g_softTex) g_softTex->Release();
    g_softSurf = nullptr;
    g_softTex = nullptr;
    g_softSrc = nullptr;
    g_softFrame = ~0ull;
}

static inline void SoftDepthPost(IDirect3DDevice9* d);

static void SoftDepthPre(IDirect3DDevice9* d, DWORD stage) {
    if (g_softStage != 0xFFFFFFFF) SoftDepthPost(d);  // pojistka: predchozi Pre bez Post (nemelo by nastat)
    if (g_softBroken || !o_SetTexture || !o_StretchRect) return;
    IDirect3DBaseTexture9* bt = nullptr;
    if (FAILED(d->GetTexture(stage, &bt)) || !bt) return;
    if (bt->GetType() != D3DRTYPE_TEXTURE || bt == (IDirect3DBaseTexture9*)g_softTex) {
        bt->Release();
        return;
    }
    IDirect3DSurface9* src = nullptr;
    D3DSURFACE_DESC sd = {};
    if (FAILED(((IDirect3DTexture9*)bt)->GetSurfaceLevel(0, &src)) || !src || FAILED(src->GetDesc(&sd)) ||
        sd.Width != g_renderSurfW || sd.Height != g_renderSurfH || !(sd.Usage & D3DUSAGE_RENDERTARGET)) {
        if (src) src->Release();
        bt->Release();
        return;
    }
    if (g_softTex && (g_softFmt != sd.Format || g_softW != sd.Width || g_softH != sd.Height)) SoftDepthRelease();
    if (!g_softTex) {
        if (FAILED(d->CreateTexture(sd.Width, sd.Height, 1, D3DUSAGE_RENDERTARGET, sd.Format, D3DPOOL_DEFAULT,
                                    &g_softTex, nullptr)) ||
            FAILED(g_softTex->GetSurfaceLevel(0, &g_softSurf))) {
            Log("WARN: cast B + LightsB: textura pro hloubku mekkych castic nejde vytvorit (%ux%u fmt %u) - bez opravy",
                sd.Width, sd.Height, (unsigned)sd.Format);
            SoftDepthRelease();
            g_softBroken = true;
            src->Release();
            bt->Release();
            return;
        }
        g_softFmt = sd.Format;
        g_softW = sd.Width;
        g_softH = sd.Height;
    }
    if (g_softFrame != g_frame || g_softSrc != src) {
        const RECT r = {0, 0, (LONG)g_renderW, (LONG)g_renderH};
        if (FAILED(o_StretchRect(d, src, &r, g_softSurf, nullptr, D3DTEXF_POINT))) {
            Log("WARN: cast B + LightsB: kopie hloubky pro mekke castice selhala (fmt %u) - bez opravy",
                (unsigned)sd.Format);
            SoftDepthRelease();
            g_softBroken = true;
            src->Release();
            bt->Release();
            return;
        }
        g_softFrame = g_frame;
        g_softSrc = src;
        static bool s_logged;
        if (!s_logged) {
            s_logged = true;
            Log("cast B + LightsB: mekke castice - hloubka %ux%u (sub-rect %ux%u) roztazena do vlastni textury, "
                "sampler s%lu",
                sd.Width, sd.Height, g_renderW, g_renderH, stage);
        }
    }
    src->Release();
    if (FAILED(o_SetTexture(d, stage, g_softTex))) {
        bt->Release();
        return;
    }
    g_softStage = stage;
    g_softOrig = bt;  // reference drzime do SoftDepthPost
    ++g_stB.softFixed;
}

static inline void SoftDepthPost(IDirect3DDevice9* d) {
    if (g_softStage == 0xFFFFFFFF) return;
    const DWORD stage = g_softStage;
    IDirect3DBaseTexture9* orig = g_softOrig;
    g_softStage = 0xFFFFFFFF;
    g_softOrig = nullptr;
    if (!orig) return;  // pojistka (Pre/Post jsou parove pres DrawFixPost; null by byl jen pri rozbitem parovani)
    o_SetTexture(d, stage, orig);
    orig->Release();
}

static IDirect3DPixelShader9* g_sunTwinPs;  // O1: puvodni PS hry po dobu draw s kopii (reference; LightsFixPost)
static float g_sunTwinC31[4];               // O1: hodnota c31 hry pred draw

static void LightsFixPre(IDirect3DDevice9* d) {
    if (!g_lightsPatched || !g_pathB || g_pathBad || g_sceneEndDone || !g_vpKnown || g_vp.X || g_vp.Y ||
        !g_renderW || !g_renderH || g_rt0W != g_renderSurfW || g_rt0H != g_renderSurfH ||
        g_renderW >= g_renderSurfW || g_renderH > g_renderSurfH)
        return;
    const bool vpRender = g_vp.Width == g_renderW && g_vp.Height == g_renderH;
    const bool vpFull = g_vp.Width == g_renderSurfW && g_vp.Height == g_renderSurfH;
    if (!vpRender && !vpFull) return;
    IDirect3DPixelShader9* ps = nullptr;
    if (FAILED(d->GetPixelShader(&ps)) || !ps) return;
    LightPsInfo li = {LPS_NONE, 0, 0xFF};
    {
        std::lock_guard<std::mutex> lk(g_psInfoMx);
        auto it = g_psInfo.find(ps);
        if (it != g_psInfo.end()) li = it->second;
        if (li.twin) li.twin->AddRef();  // drzime do LightsFixPost (zaznam se muze mezitim prepsat)
    }
    struct Rel {  // ps (a nepouzitou kopii) pustit pri kazdem navratu, pokud si je nevzal g_sunTwin*
        IDirect3DPixelShader9*& a;
        IDirect3DPixelShader9*& b;
        ~Rel() {
            if (a) a->Release();
            if (b) b->Release();
        }
    } rel{ps, li.twin};
    if (li.depthS != 0xFF && vpRender) {
        // O2: spritecard (VS s ViewportTransformScaled) uv hloubky uz v B skaluje sam (c14 = viewport/RT) -> cte
        // spravne puvodni hloubku; roztazena kopie by ji posunula podruhe (docs/svetla-B-projeti.md N2)
        const ShaderInfo* vsi = g_curInfo;
        if (vsi && vsi->vpScaledReg >= 0) {
            ++g_stB.softSkipped;
            static bool s_logged;
            if (!s_logged) {
                s_logged = true;
                Log("cast B + LightsB: mekke castice s VS ViewportTransformScaled c%d (spritecard) - hloubka beze zmeny "
                    "(uv uz skaluje engine)",
                    (int)vsi->vpScaledReg);
            }
        } else {
            SoftDepthPre(d, li.depthS);
        }
    }
    if (li.kind == LPS_NONE) return;
    const float rW = (float)g_renderW, rH = (float)g_renderH, sW = (float)g_renderSurfW, sH = (float)g_renderSurfH;
    const float a = sW / rW, b = sH / rH;
    if (li.kind == LPS_CSM || li.kind == LPS_SUN || li.kind == LPS_SUNDISK) {
        if (!vpFull) return;  // tyto pruchody kresli pres cely povrch s uv 0..1 povrchu
        float* o = g_lightFixOrig;
        if (li.kind == LPS_CSM) {
            // Voda: odraz a refrakce maji vlastni pohled 1024x1024 (G-buffer 1024x1024, bez sub-rectu), masku ale
            // engine pocita do teze textury velikosti povrchu pres cely povrch -> skladat jen pro pruchod hlavniho
            // pohledu, tj. hloubka v s0 velikosti povrchu (jinak by stiny v odrazu/refrakci byly posunute:
            // bm_c2a5a, dno reky pod vodou ve stinu, docs/svetla-B-opravy.md O4)
            bool mainDepth = false;
            IDirect3DBaseTexture9* t0 = nullptr;
            if (SUCCEEDED(d->GetTexture(0, &t0)) && t0) {
                D3DSURFACE_DESC sd = {};
                if (t0->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(((IDirect3DTexture9*)t0)->GetLevelDesc(0, &sd)) &&
                    sd.Width == g_renderSurfW && sd.Height == g_renderSurfH)
                    mainDepth = true;
                t0->Release();
            }
            if (!mainDepth) {
                ++g_stB.csmSkipped;
                static bool s_skipLogged;
                if (!s_skipLogged) {
                    s_skipLogged = true;
                    Log("cast B + LightsB: stinova maska slunce pro jiny pohled (odraz/refrakce vody, hloubka mimo "
                        "velikost povrchu) - beze zmeny");
                }
                return;
            }
            if (FAILED(d->GetPixelShaderConstantF(94, o, 4))) return;
            float c[4 * 4];
            memcpy(c, o, sizeof c);
            FoldNdcRows(c, 4, a, b);
            if (FAILED(d->SetPixelShaderConstantF(94, c, 4))) return;
            g_lightFixReg = 94;
            g_lightFixCount = 4;
            ++g_stB.csmFixed;
            static bool s_logged;
            if (!s_logged) {
                s_logged = true;
                Log("cast B + LightsB: stinova maska slunce - g_matCSMShadowBuffer c94..97 slozeno s ndc render/povrch "
                    "(%.4f, %.4f)",
                    a, b);
            }
            return;
        }
        // Paprsky slunce: c10 = (_SunPosUV.xy, polomer kotouce, pomer stran); kotouc: c12.w vnitrni polomer
        const UINT n = li.kind == LPS_SUNDISK ? 3 : 1;
        if (FAILED(d->GetPixelShaderConstantF(10, o, n))) return;
        float c[3 * 4];
        memcpy(c, o, n * 16);
        const float rx = rW / sW, ry = rH / sH;
        c[0] *= rx;
        c[1] *= ry;
        // O1: zare pres upravenou kopii (vzdalenost v UV pohledu) - polomery kotouce pak bez skalovani
        bool twin = false;
        if (li.twin && g_cfg.lightsBSunFix && g_sunTwinPs == nullptr &&
            SUCCEEDED(d->GetPixelShaderConstantF(kSunTwinReg, g_sunTwinC31, 1))) {
            const float k[4] = {a, b, 0.0f, 0.0f};
            if (SUCCEEDED(d->SetPixelShaderConstantF(kSunTwinReg, k, 1))) {
                if (SUCCEEDED(d->SetPixelShader(li.twin))) {
                    twin = true;
                    g_sunTwinPs = ps;  // puvodni PS hry (reference) vrati LightsFixPost
                    ps = nullptr;
                    ++g_stB.sunTwin;
                } else {
                    d->SetPixelShaderConstantF(kSunTwinReg, g_sunTwinC31, 1);
                }
            }
        }
        if (li.kind == LPS_SUNDISK && !twin) {
            c[2] *= ry;  // vzdalenost v UV s osou x prenasobenou pomerem stran -> meritko jako y
            c[11] *= ry;
        }
        if (FAILED(d->SetPixelShaderConstantF(10, c, n))) return;
        g_lightFixReg = 10;
        g_lightFixCount = n;
        ++g_stB.sunFixed;
        if (twin) {
            static bool s_twinLogged;
            if (!s_twinLogged) {
                s_twinLogged = true;
                Log("cast B + LightsB: zare slunce - upravena kopie grfinal, c%u = (%.4f %.4f) (vzdalenost v UV pohledu)",
                    kSunTwinReg, a, b);
            }
        }
        static bool s_logged;
        if (!s_logged) {
            s_logged = true;
            Log("cast B + LightsB: paprsky slunce - _SunPosUV (%.4f %.4f) -> (%.4f %.4f)", o[0], o[1], c[0], c[1]);
        }
        return;
    }
    if (li.kind == LPS_CAMPARAMS) {
        if (!vpRender) return;
        float* o = g_lightFixOrig;
        if (FAILED(d->GetPixelShaderConstantF(li.reg, o, 1))) return;
        if (!NearRel(o[0], 1.0f / rW) || !NearRel(o[1], 1.0f / rH) || !NearRel(o[2], 0.5f / rW) ||
            !NearRel(o[3], 0.5f / rH))
            return;
        const float c[4] = {1.0f / sW, 1.0f / sH, 0.5f / sW, 0.5f / sH};
        if (FAILED(d->SetPixelShaderConstantF(li.reg, c, 1))) return;
        g_lightFixReg = li.reg;
        g_lightFixCount = 1;
        ++g_stB.camParFixed;
        static bool s_logged;
        if (!s_logged) {
            s_logged = true;
            Log("cast B + LightsB: g_CamParams c%u (%.6f %.6f %.6f %.6f) -> (%.6f %.6f %.6f %.6f) - cteni light bufferu "
                "v sub-rectu",
                li.reg, o[0], o[1], o[2], o[3], c[0], c[1], c[2], c[3]);
        }
        return;
    }
    // LPS_LIGHT: viewSize, matCamProjToWorld a pripadne _viewmodel v jednom souvislem rozsahu registru
    const UINT lo = std::min<UINT>(li.v, std::min<UINT>(li.m, li.mv != 0xFF ? li.mv : li.m));
    const UINT hi = std::max<UINT>(li.v, std::max<UINT>(li.m + 3u, li.mv != 0xFF ? li.mv + 3u : li.m + 3u));
    const UINT cnt = hi - lo + 1;
    float* o = g_lightFixOrig;
    if (cnt > kLightFixMaxRegs || FAILED(d->GetPixelShaderConstantF(lo, o, cnt))) return;
    const float* vs = &o[(li.v - lo) * 4];
    if (!NearRel(vs[0], 1.0f / rW) || !NearRel(vs[1], 1.0f / rH)) return;
    float c[kLightFixMaxRegs * 4];
    memcpy(c, o, cnt * 16);
    c[(li.v - lo) * 4] = 1.0f / sW;
    c[(li.v - lo) * 4 + 1] = 1.0f / sH;
    FoldNdcRows(&c[(li.m - lo) * 4], 4, a, b);
    if (li.mv != 0xFF) FoldNdcRows(&c[(li.mv - lo) * 4], 4, a, b);
    if (FAILED(d->SetPixelShaderConstantF(lo, c, cnt))) return;
    g_lightFixReg = lo;
    g_lightFixCount = cnt;
    ++g_stB.lightPsFixed;
    static bool s_logged;
    if (!s_logged) {
        s_logged = true;
        Log("cast B + LightsB: svetla - viewSize c%u (%.6f %.6f) -> (%.6f %.6f), matCamProjToWorld c%u(+_viewmodel) "
            "slozeno s ndc render/povrch (%.4f, %.4f)",
            li.v, vs[0], vs[1], 1.0f / sW, 1.0f / sH, li.m, a, b);
    }
    if (g_capState == CAP_ACTIVE)
        Cap("# svetla: viewSize c%u -> % .6f % .6f, c%u -> % .6f % .6f % .6f % .6f (po draw zpet)", li.v, 1.0f / sW, 1.0f / sH,
            li.m, c[(li.m - lo) * 4], c[(li.m - lo) * 4 + 1], c[(li.m - lo) * 4 + 2], c[(li.m - lo) * 4 + 3]);
}

static inline void LightsFixPost(IDirect3DDevice9* d) {
    SoftDepthPost(d);
    if (g_sunTwinPs) {  // O1: puvodni grfinal a c31 hry zpet
        d->SetPixelShader(g_sunTwinPs);
        d->SetPixelShaderConstantF(kSunTwinReg, g_sunTwinC31, 1);
        g_sunTwinPs->Release();
        g_sunTwinPs = nullptr;
    }
    if (!g_lightFixCount) return;
    d->SetPixelShaderConstantF(g_lightFixReg, g_lightFixOrig, g_lightFixCount);
    g_lightFixCount = 0;
}

// Po draw, jen kdyz PreDraw vratil post = true (hlavni zarizeni, stejne vlakno jako Pre). Drive se volalo po kazdem
// draw: draw vedlejsiho zarizeni (ANGLE, jine vlakno) pak soubezne s vlaknem renderu vracel texturu / konstanty
// hlavniho zarizeni do sveho zarizeni a uvolnoval g_softOrig -> pad 0xC0000005 v SoftDepthPost (ukazatel null).
static inline void DrawFixPost(IDirect3DDevice9* d) {
    ScopeFixPost(d);
    FxaaPost(d);
    LightsFixPost(d);
}

// Viewport perspektivniho draw hlavniho pohledu do statistiky snimku (DetectRender)
static void NoteViewport() {
    const UINT x = g_vpKnown ? g_vp.X : 0, y = g_vpKnown ? g_vp.Y : 0;
    const UINT w = g_vpKnown ? g_vp.Width : g_rt0W, h = g_vpKnown ? g_vp.Height : g_rt0H;
    int i = g_vpHistLast;
    if (i < 0 || i >= g_nVpHist || g_vpHist[i].x != x || g_vpHist[i].y != y || g_vpHist[i].w != w || g_vpHist[i].h != h) {
        for (i = 0; i < g_nVpHist; ++i)
            if (g_vpHist[i].x == x && g_vpHist[i].y == y && g_vpHist[i].w == w && g_vpHist[i].h == h) break;
        if (i == g_nVpHist) {
            if (g_nVpHist >= (int)(sizeof g_vpHist / sizeof g_vpHist[0])) {
                ++g_vpHistOver;
                return;
            }
            g_vpHist[g_nVpHist++] = {x, y, w, h, 0};
        }
        g_vpHistLast = i;
    }
    ++g_vpHist[i].n;
}

// Tesne pred draw callem: nahrat jitterovane nebo puvodni clip-matice.
// args = argumenty draw callu pro zachyt (mimo zachyt prazdne).
// true = draw nepustit dal (cast B: roztazeni sceny, ktere nahradil vystup DLSS).
// post = true: probehly opravy pred draw (ScopeFixPre, LightsFixPre), po draw zavolat DrawFixPost na stejnem vlakne.
// Jen hlavni zarizeni mimo davku MV objektu. Pozor: DXVK ma jednu vtable pro vsechna zarizeni - draw vedlejsiho
// zarizeni (ANGLE/libGLESv2 menu Qt, 1x1, vlastni vlakno) jdou taky pres nase hooky a soubezne s vlaknem renderu.
static bool PreDraw(IDirect3DDevice9* d, const char* kind, unsigned prims, const char* args, bool& post) {
    post = false;
    if (d != g_main || OmGuarded()) return false;
    if (g_sceneEndPending) FirePendingSceneEnd(d, "dalsim draw");
    ++g_statDraws;
    ++g_drawSeq;
    if (g_capState == CAP_ACTIVE) {
        ++g_capDraws;
        CapPsState(d);
        Cap("D%s prims=%u vs=%p %s", kind, prims, g_curVS, args);
    }
    const ShaderInfo* si = g_curInfo;
    bool mainView = g_rt0W == g_bbW && g_rt0H == g_bbH && g_bbW != 0;

    // Ma shader perspektivni clip-matici? (scena ano; post-process, svetla pres celou obrazovku a HUD ne)
    bool perspClip = false;
    if (si)
        for (uint8_t i = 0; i < si->nClip && !perspClip; ++i) {
            const float* s = &g_vsConst[si->clipReg[i] * 4];
            perspClip = s[12] * s[12] + s[13] * s[13] + s[14] * s[14] > 1e-10f;
        }
    g_rmPersp = mainView && perspClip;  // maska reaktivity (RmReplay po PreDraw)
    if (mainView && perspClip) {
        if (g_rt0Fmt == D3DFMT_A8R8G8B8) {
            // Po konci sceny (ScenePrep) uz ne: cast B z nich jeste spousti most na draw roztazeni
            if (!g_sceneEndDone) {
                g_sceneRT = g_rt0;
                g_sceneDS = g_ds;
                g_sceneW = g_rt0W;
                g_sceneH = g_rt0H;
            }
        } else if (g_rt0Fmt == D3DFMT_R32F) {
            g_linDepth = g_rt0;
        }
        if (!g_sceneEndDone) NoteViewport();
    }
    // Konec sceny = prvni draw post-process shaderu do barvy sceny: shader MA tabulku konstant,
    // ale zadnou perspektivni matici (Texel_Sizes, vsTapOffs...). Shadery bez CTAB jsou
    // celoobrazovkove efekty uprostred sceny (po nich se jeste kresli zbran v ruce).
    // Draw bez zapisu barvy uvnitr okluzniho dotazu hry (histogram expozice): konec sceny az po Issue(END), aby se
    // draw cally konce sceny nezapocitaly do dotazu a draw cetl kopii sceny jako bez modu. Nic nekresli, takze ho
    // zapis vysledku mostu neprepise; jako "draw po kopii sceny" se nepocita (kopie se po mostu zopakuje).
    if (!g_sceneEndDone && !g_sceneEndPending && g_sceneRT && g_rt0 == g_sceneRT && !perspClip && si &&
        si->hasCtab) {
        if (g_nOcclOpen > 0 && !(g_rsColorWrite & 0xF)) {
            g_sceneEndPending = true;
            g_sceneEndPendingWhy = si->text;
            if (g_capState == CAP_ACTIVE) Cap("# konec sceny odlozen: draw uvnitr okluzniho dotazu hry");
        } else {
            TriggerSceneEnd(d, si->text.c_str());
        }
    }
    if (!g_sceneEndDone && !g_sceneEndPending && g_sceneCopy.valid && g_rt0 == g_sceneRT) ++g_sceneCopy.drawsAfter;
    // Zachyt: co ctou draw do sceny po konci sceny (i ten, ktery konec sceny spustil)
    if (g_capState == CAP_ACTIVE && g_sceneEndDone && g_sceneRT && g_rt0 == g_sceneRT) CapPostDraw(d);
    // K5: bod pred HUD (POST draw vcetne potlaceneho roztazeni casti B nize / prvni draw HUD)
    if (g_sceneEndDone && g_sceneRT && g_rt0 == g_sceneRT && Present_Enabled()) HlOnSceneWrite(d, HlIsPost());

    // Cast B: roztazeni sceny. Bez post-processingu (mat_postprocess_enable 0) konec sceny spustil az tento draw
    // (pravidlo CTAB vyse), takze ScenePrep uz probehl.
    if (g_sceneRT && g_rt0 == g_sceneRT && (g_bFollow || (g_pathB && g_sceneEndDone && !g_pathBad))) {
        const bool sig = IsUpscaleDraw(perspClip);
        if (g_bFollow) {
            // engine roztahuje vic draw cally: dalsi by prepsal vystup DLSS
            if (sig && g_curVS == g_bFollowVS && prims == g_bFollowPrims && CurPS(d) == g_bFollowPS) {
                ++g_stB.extra;
                if (g_capState == CAP_ACTIVE) Cap("# potlaceno (cast B: dalsi draw roztazeni sceny)");
                return true;
            }
            g_bFollow = false;
        } else if (sig && !g_bHandled) {
            if (g_lastSceneSR.drawsSince == 0) {
                // Barva sceny v sub-rectu je ted totozna s tim, co by roztazeni cetlo z kopie: vstup DLSS jde primo
                // ze sceny, vystup W x H prijde kopii B do cele sceny - tam, kam by kreslil tento draw.
                g_bHandled = true;
                if (!g_bFoundLogged) {
                    g_bFoundLogged = true;
                    Log("cast B: roztazeni sceny nalezeno (draw%s prims=%u, VS %p, textura 0 %p = kontejner cile kopie "
                        "sceny %p, kopie src %ld,%ld-%ld,%ld%s dst %ld,%ld-%ld,%ld%s)",
                        kind, prims, g_curVS, (void*)g_t0, (void*)g_lastSceneSR.dst, g_lastSceneSR.sr.left,
                        g_lastSceneSR.sr.top, g_lastSceneSR.sr.right, g_lastSceneSR.sr.bottom,
                        g_lastSceneSR.hasSr ? "" : " (cely)", g_lastSceneSR.dr.left, g_lastSceneSR.dr.top,
                        g_lastSceneSR.dr.right, g_lastSceneSR.dr.bottom, g_lastSceneSR.hasDr ? "" : " (cely)");
                }
                if (SceneBridge(d)) {
                    ++g_stB.suppressed;
                    g_bFollow = true;
                    g_bFollowVS = g_curVS;
                    g_bFollowPS = CurPS(d);
                    g_bFollowPrims = prims;
                    if (g_capState == CAP_ACTIVE)
                        Cap("# potlaceno (cast B: roztazeni sceny nahradil vystup DLSS, render %ux%u -> %ux%u)",
                            g_renderW, g_renderH, g_sceneW, g_sceneH);
                    return true;
                }
                ++g_stB.notQueued;  // most snimek nezaradil: roztahne engine (dalsi snimek bez jitteru)
                if (g_capState == CAP_ACTIVE) Cap("# cast B: most snimek nezaradil, roztazeni kresli engine");
            } else if (!g_bBetween) {
                // Od kopie sceny uz se do sceny kreslilo: kopie (z ni cte roztazeni) a scena se lisi -> nechat enginu
                g_bBetween = true;
                ++g_stB.between;
                if (g_capState == CAP_ACTIVE)
                    Cap("# cast B: %u draw do sceny mezi kopii a roztazenim, roztazeni kresli engine",
                        g_lastSceneSR.drawsSince);
            }
        }
    }
    // Cast B: chyba hry v masce hledi kuse (uvOffsets podle renderu, kresli se pres cely povrch)
    post = true;  // od ted se draw uz nepotlaci: Pre opravy maji parovy DrawFixPost
    ScopeFixPre(d);
    FxaaPre(d);        // FXAA hry po DLSS / FSR neutralizovat ([upscale] SkipGameFxaa)
    DetectPre(d);      // cast B + [upscale] LightsBDetect: hledani dalsich shaderu s konstantami podle renderu
    LightsFixPre(d);  // cast B + LightsB: UV svetel a cteni light bufferu podle povrchu
    if (g_lastSceneSR.valid && g_rt0 == g_sceneRT) ++g_lastSceneSR.drawsSince;

    // Mip bias pro DLAA: jen perspektivni drawy hlavniho pohledu pred koncem sceny (jako jitter);
    // vsechny ostatni drawy (post-process, HUD, mensi cile) dostanou zpet hodnoty hry.
    {
        const bool scene = g_mipSceneOn && mainView && perspClip && !g_sceneEndDone;
        uint16_t mipN = 0;
        bool mipA = false;
        if (scene) MipDrawKind(d, &mipN, &mipA);
        ApplyMipBias(d, scene && (g_mipBiasOn || mipN || mipA), mipN, mipA, g_mipBiasOn);
    }

    if (!si || !si->nClip) return false;
    if (mainView) ++g_statMainDraws;
    // Kandidati na kameru: draw do barvy sceny pred koncem sceny, mimo zbran v ruce (viewport MaxZ 0.1)
    const bool sceneColor = mainView && g_rt0Fmt == D3DFMT_A8R8G8B8 && !g_sceneEndDone && g_vpMaxZ >= 0.5f;
    for (uint8_t i = 0; i < si->nClip; ++i) {
        UINT r = si->clipReg[i];
        const float* s = &g_vsConst[r * 4];
        // perspektiva: radek W zavisi na pozici (u orto je xyz == 0)
        bool persp = s[12] * s[12] + s[13] * s[13] + s[14] * s[14] > 1e-10f;
        // s = puvodni hodnoty od hry (stinova kopie), tedy kamera bez jitteru
        if (sceneColor && persp && si->clipKind[i] == CK_VIEWPROJ) NoteCamera(s);
        // Jitter jen do konce sceny: perspektivni drawy po mostu (za bloomem) uz DLAA nevyhladi.
        if (g_jitterActive && mainView && persp && !g_sceneEndDone) {
            // Posun v clip prostoru: podle sub-rectu jen u draw, ktery kresli presne do renderu casti B (viewport od
            // 0,0 velikosti renderu - kamera sceny, zbran v ruce i linearni hloubka); vsechno ostatni (a cela cast A)
            // podle celeho povrchu jako pred verzi 10. Pri zmene jitterovane matice v zarizeni neplati -> nahrat znovu.
            const bool rr = g_pathB && g_vpKnown && !g_vp.X && !g_vp.Y && g_vp.Width == g_renderW &&
                            g_vp.Height == g_renderH;
            const UINT vw = rr ? g_renderW : g_bbW, vh = rr ? g_renderH : g_bbH;
            if (vw != g_jitVpW || vh != g_jitVpH) {
                g_jitVpW = vw;
                g_jitVpH = vh;
                JitterForViewport(vw, vh, &g_jx, &g_jy);
                for (int b = 0; b < 256; ++b)
                    if (g_blkState[b] == 1) g_blkState[b] = 2;
            }
            if (g_blkState[r] != 1) {
                float m[16];
                memcpy(m, s, sizeof m);
                JitterMatrix(m);
                o_SetVSConstF(d, r, m, 4);
                g_blkState[r] = 1;
                ++g_statJitterUploads;
            }
        } else if (g_blkState[r] != 0) {
            o_SetVSConstF(d, r, s, 4);
            g_blkState[r] = 0;
            ++g_statRestores;
        }
    }
    return false;
}

static thread_local int t_inPresent;

// ---------------------------------------------------------------------------
// Hooky zarizeni
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND h,
                                           const RGNDATA* r) {
    const bool mine = t_inPresent++ == 0 && d == g_main;
    if (mine) {
        OnFrameEnd();
        LatBeforePresent(d);
    }
    if (t_inPresent == 1 && Present_Enabled())  // spike S2: snimek helperu, cil prezentace DXVK
        h = Present_OnPresent(d, nullptr, h, mine, _ReturnAddress(), (void*)&H_Present);
    HRESULT hr = o_Present(d, a, b, h, r);
    if (t_inPresent == 1 && Present_Enabled()) Present_AfterPresent(d, mine);
    if (mine) LatAfterPresent(d, hr);
    --t_inPresent;
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_PresentEx(IDirect3DDevice9Ex* d, const RECT* a, const RECT* b, HWND h,
                                             const RGNDATA* r, DWORD f) {
    const bool mine = t_inPresent++ == 0 && (IDirect3DDevice9*)d == g_main;
    if (mine) {
        OnFrameEnd();
        LatBeforePresent(g_main);
    }
    if (t_inPresent == 1 && Present_Enabled())
        h = Present_OnPresent((IDirect3DDevice9*)d, nullptr, h, mine, _ReturnAddress(), (void*)&H_Present);
    HRESULT hr = o_PresentEx(d, a, b, h, r, f);
    if (t_inPresent == 1 && Present_Enabled()) Present_AfterPresent((IDirect3DDevice9*)d, mine);
    if (mine) LatAfterPresent(g_main, hr);
    --t_inPresent;
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SCPresent(IDirect3DSwapChain9* s, const RECT* a, const RECT* b, HWND h,
                                             const RGNDATA* r, DWORD f) {
    bool mine = false;
    if (t_inPresent++ == 0) {
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(s->GetDevice(&dev)) && dev) {
            if (dev == g_main) {
                mine = true;
                OnFrameEnd();
                LatBeforePresent(g_main);
            }
            if (Present_Enabled())
                h = Present_OnPresent(dev, s, h, mine, _ReturnAddress(), (void*)&H_Present);
            dev->Release();
        }
    }
    HRESULT hr = o_SCPresent(s, a, b, h, r, f);
    if (mine && Present_Enabled()) Present_AfterPresent(g_main, true);
    if (mine) LatAfterPresent(g_main, hr);
    --t_inPresent;
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetVSConstF(IDirect3DDevice9* d, UINT start, const float* data, UINT count) {
    if (d == g_main && !OmGuarded() && data && start < 256) {
        UINT n = std::min(count, 256u - start);
        memcpy(&g_vsConst[start * 4], data, n * 16);
        // Bloky, ktere hra (castecne) prepsala: GPU ted obsahuje puvodni nebo smichane hodnoty
        UINT lo = start >= 3 ? start - 3 : 0;
        for (UINT r = lo; r < start + n && r < 256; ++r) {
            if (!g_blkState[r]) continue;
            bool full = r >= start && r + 4 <= start + n;
            g_blkState[r] = full ? 0 : 2;
        }
        if (g_capState == CAP_ACTIVE) {
            Cap("C start=c%u count=%u", start, count);
            for (UINT i = 0; i < n; ++i) CapVec(" ", start + i, data + i * 4);
        }
    }
    return o_SetVSConstF(d, start, data, count);
}

static HRESULT STDMETHODCALLTYPE H_CreateVS(IDirect3DDevice9* d, const DWORD* code, IDirect3DVertexShader9** out) {
    HRESULT hr = o_CreateVS(d, code, out);
    if (SUCCEEDED(hr) && out && *out) {
        ShaderInfo info = ParseShader(code);
        const size_t vt = info.text.find(" ViewportTransformScaled=c");
        if (vt != std::string::npos) {
            const char* p = info.text.c_str() + vt + 26;
            char* e = nullptr;
            const long r = strtol(p, &e, 10);
            if (e != p && r >= 0 && r < 256) info.vpScaledReg = (int16_t)r;
        }
        if (g_cfg.lightsBDetect) {  // O3: ctene slozky konstant VS
            UINT fnSize = 0;
            if (SUCCEEDED((*out)->GetFunction(nullptr, &fnSize)) && fnSize >= 8 && fnSize <= 256 * 1024) {
                info.cuse.resize(kConstUseRegs);
                if (!ShaderConstUse(code, fnSize / 4, info.cuse.data(), nullptr)) info.cuse.clear();
            }
        }
        std::lock_guard<std::mutex> lk(g_vsInfoMx);
        g_vsInfo[*out] = std::move(info);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetVS(IDirect3DDevice9* d, IDirect3DVertexShader9* s) {
    if (d == g_main && !OmGuarded()) {
        g_curVS = s;
        const ShaderInfo* info = nullptr;
        if (s) {
            std::lock_guard<std::mutex> lk(g_vsInfoMx);
            auto it = g_vsInfo.find(s);
            if (it != g_vsInfo.end()) info = &it->second;  // unordered_map: ukazatele na prvky jsou stabilni
        }
        g_curInfo = info;
        if (g_capState == CAP_ACTIVE && s) {
            bool isNew;
            {
                std::lock_guard<std::mutex> lk(g_capMx);
                isNew = g_capShaders.insert(s).second;
            }
            if (isNew) {
                Cap("V %p %s", (void*)s, info ? info->text.c_str() : "(neznamy)");
                DumpVS(s);  // jednou za serii (bms_dlss_vs_<ptr>.bin)
            }
        }
    }
    return o_SetVS(d, s);
}

static HRESULT STDMETHODCALLTYPE H_DrawPrim(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT n) {
    char a[64];
    a[0] = 0;
    if (g_capState == CAP_ACTIVE) {
        snprintf(a, sizeof a, "t=%d start=%u", (int)t, s);
        if (d == g_main && !OmGuarded()) CapQuadVerts(d, nullptr, 0, s, PrimVerts(t, n));
    }
    bool post;
    if (PreDraw(d, "", n, a, post)) return D3D_OK;
    RmReplay(d, [&] { o_DrawPrim(d, t, s, n); });
    const HRESULT hr = o_DrawPrim(d, t, s, n);
    if (post) DrawFixPost(d);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_DrawIdxPrim(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv,
                                               UINT si, UINT n) {
    char a[96];
    a[0] = 0;
    if (g_capState == CAP_ACTIVE) {
        snprintf(a, sizeof a, "t=%d base=%d min=%u nv=%u start=%u", (int)t, b, mi, nv, si);
        if (d == g_main && !OmGuarded() && nv <= 4 && b + (INT)mi >= 0) CapQuadVerts(d, nullptr, 0, (UINT)(b + (INT)mi), nv);
    }
    bool post;
    if (PreDraw(d, "I", n, a, post)) return D3D_OK;
    if (d == g_main) OmRecord(d, t, b, mi, nv, si, n);  // MV objektu: zaznam draw callu v G-bufferu
    RmReplay(d, [&] { o_DrawIdxPrim(d, t, b, mi, nv, si, n); });
    const HRESULT hr = o_DrawIdxPrim(d, t, b, mi, nv, si, n);
    if (post) DrawFixPost(d);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_DrawPrimUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v,
                                              UINT st) {
    char a[64];
    a[0] = 0;
    if (g_capState == CAP_ACTIVE) {
        snprintf(a, sizeof a, "t=%d stride=%u", (int)t, st);
        if (d == g_main && !OmGuarded() && v) CapQuadVerts(d, v, st, 0, PrimVerts(t, n));
    }
    bool post;
    if (PreDraw(d, "UP", n, a, post)) return D3D_OK;
    RmReplay(d, [&] { o_DrawPrimUP(d, t, n, v, st); });
    const HRESULT hr = o_DrawPrimUP(d, t, n, v, st);
    if (post) DrawFixPost(d);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_DrawIdxPrimUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT n,
                                                 const void* id, D3DFORMAT f, const void* v, UINT st) {
    char a[96];
    a[0] = 0;
    if (g_capState == CAP_ACTIVE)
        snprintf(a, sizeof a, "t=%d min=%u nv=%u ifmt=%u stride=%u", (int)t, mi, nv, (unsigned)f, st);
    bool post;
    if (PreDraw(d, "IUP", n, a, post)) return D3D_OK;
    RmReplay(d, [&] { o_DrawIdxPrimUP(d, t, mi, nv, n, id, f, v, st); });
    const HRESULT hr = o_DrawIdxPrimUP(d, t, mi, nv, n, id, f, v, st);
    if (post) DrawFixPost(d);
    return hr;
}

// Stin stavu pro zaznam MV objektu (deklarace, stream 0, indexy, vybrane render staty) a zachyt. Behem davky MV
// objektu (OmGuarded) jen propousteji volani dal.
static HRESULT STDMETHODCALLTYPE H_SetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v) {
    if (d == g_main && !OmGuarded()) {
        switch (st) {
        case D3DRS_ZENABLE: g_rsZEnable = v; break;
        case D3DRS_ZWRITEENABLE: g_rsZWrite = v; break;
        case D3DRS_COLORWRITEENABLE: g_rsColorWrite = v; break;
        case D3DRS_ALPHABLENDENABLE: g_rsAlphaBlend = v; break;
        case D3DRS_CULLMODE: g_rsCull = v; break;
        case D3DRS_ALPHAREF:
            if (!g_rsAlphaRef) Log("hra nastavuje ALPHAREF (prvni hodnota %lu) - prah alpha testu v case jde pouzit", v);
            g_rsAlphaRef = v ? v : 1;
            if (g_capState == CAP_ACTIVE) CapRS(st, v);
            return o_SetRenderState(d, st, AlphaRefOut(v));
        default: break;
        }
        if (g_capState == CAP_ACTIVE) CapRS(st, v);
    }
    return o_SetRenderState(d, st, v);
}
static HRESULT STDMETHODCALLTYPE H_SetVDecl(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* decl) {
    if (d == g_main && !OmGuarded()) {
        g_curDecl = decl;
        if (g_capState == CAP_ACTIVE) CapDecl(decl);
    }
    return o_SetVDecl(d, decl);
}
static HRESULT STDMETHODCALLTYPE H_SetFVF(IDirect3DDevice9* d, DWORD fvf) {
    if (d == g_main && !OmGuarded()) {
        if (fvf) g_curDecl = nullptr;  // FVF nahrazuje deklaraci (SetFVF(0) ji nemeni)
        if (g_capState == CAP_ACTIVE) Cap("FVF 0x%X", fvf);
    }
    return o_SetFVF(d, fvf);
}
static HRESULT STDMETHODCALLTYPE H_SetStreamSource(IDirect3DDevice9* d, UINT s, IDirect3DVertexBuffer9* vb, UINT off,
                                                   UINT stride) {
    if (d == g_main && !OmGuarded()) {
        if (s == 0) {
            g_curVB0 = vb;
            g_curVB0Off = off;
            g_curVB0Stride = stride;
        }
        OmHookVBLock(vb);  // jednou: Lock vsech VB (spolecna vtable)
        if (g_capState == CAP_ACTIVE) CapStream(s, vb, off, stride);
    }
    return o_SetStreamSource(d, s, vb, off, stride);
}
static HRESULT STDMETHODCALLTYPE H_SetStreamSourceFreq(IDirect3DDevice9* d, UINT s, UINT setting) {
    if (d == g_main && !OmGuarded()) {
        if (s == 0) g_curFreq0 = setting;
        if (g_capState == CAP_ACTIVE) Cap("SSF %u 0x%08X", s, setting);
    }
    return o_SetStreamSourceFreq(d, s, setting);
}
static HRESULT STDMETHODCALLTYPE H_SetIndices(IDirect3DDevice9* d, IDirect3DIndexBuffer9* ib) {
    if (d == g_main && !OmGuarded()) {
        g_curIB = ib;
        OmHookIBLock(ib);  // jednou: Lock vsech IB
        if (g_capState == CAP_ACTIVE) CapIndices(ib);
    }
    return o_SetIndices(d, ib);
}

// Okluzni dotazy hry: sledovani BEGIN/END kvuli odlozenemu konci sceny (g_occlOpen). Vtable dotazu ma DXVK jednu pro
// vsechny typy, hook proto propousti ostatni typy (i event/timestamp dotazy regulatoru latence) beze zmeny.
static HRESULT STDMETHODCALLTYPE H_QueryIssue(IDirect3DQuery9* q, DWORD fl) {
    if (!(fl & (D3DISSUE_BEGIN | D3DISSUE_END)) || !g_main || OmGuarded() || q->GetType() != D3DQUERYTYPE_OCCLUSION)
        return o_QueryIssue(q, fl);
    IDirect3DDevice9* dev = nullptr;
    if (FAILED(q->GetDevice(&dev)) || !dev) return o_QueryIssue(q, fl);
    dev->Release();
    if (dev != g_main) return o_QueryIssue(q, fl);
    const HRESULT hr = o_QueryIssue(q, fl);
    int i = 0;
    while (i < g_nOcclOpen && g_occlOpen[i] != q) ++i;
    if (fl & D3DISSUE_BEGIN) {  // BEGIN na zacatem dotazu ho jen restartuje
        if (i == g_nOcclOpen && g_nOcclOpen < (int)(sizeof g_occlOpen / sizeof g_occlOpen[0]))
            g_occlOpen[g_nOcclOpen++] = q;
    } else if (i < g_nOcclOpen) {
        g_occlOpen[i] = g_occlOpen[--g_nOcclOpen];
        // Dotaz je uzavreny (prikaz END je v DXVK pred vsim, co most ted zaradi): odlozeny konec sceny
        if (g_nOcclOpen == 0 && g_sceneEndPending) FirePendingSceneEnd(g_main, "Issue(END)");
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateQuery(IDirect3DDevice9* d, D3DQUERYTYPE t, IDirect3DQuery9** out) {
    const HRESULT hr = o_CreateQuery(d, t, out);
    if (SUCCEEDED(hr) && out && *out && t == D3DQUERYTYPE_OCCLUSION && d == g_main)
        HookVtbl(*out, 6, (PFN_QueryIssue)H_QueryIssue, o_QueryIssue, "Query::Issue");
    return hr;
}

static void CapSurface(const char* what, DWORD idx, IDirect3DSurface9* s, const D3DSURFACE_DESC& desc) {
    Cap("%s%u %p %ux%u fmt=%u ms=%u", what, idx, (void*)s, desc.Width, desc.Height, desc.Format,
        desc.MultiSampleType);
}

static HRESULT STDMETHODCALLTYPE H_SetRT(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    if (OmGuarded()) return o_SetRT(d, i, s);  // davka MV objektu: bez zmeny stinu
    if (g_lat.res && !g_lat.broken && d == g_main) LatPollCheap();  // presnejsi cas dokonceni snimku (odhad latence)
    if (d == g_main && g_sceneEndPending) FirePendingSceneEnd(d, "SetRenderTarget");
    if (d == g_main && i == 0) {
        D3DSURFACE_DESC desc = {};
        if (s) s->GetDesc(&desc);
        // Pojistka: hra opousti barvu sceny a prepina na zacatek bloomu (1/2 nebo 1/4 backbufferu),
        // aniz by predtim kreslila post-process shader do sceny. Jine mensi textury (odraz vody
        // 1024x1024 hned po 3D skyboxu, monitory...) konec sceny NEJSOU.
        bool bloomStart = (desc.Width * 2 == g_bbW && desc.Height * 2 == g_bbH) ||
                          (desc.Width * 4 == g_bbW && desc.Height * 4 == g_bbH);
        if (!g_sceneEndDone && g_sceneRT && g_rt0 == g_sceneRT && s != g_sceneRT && bloomStart)
            TriggerSceneEnd(d, "prepnuti render targetu ze sceny na mensi texturu");
        g_rt0W = desc.Width;
        g_rt0H = desc.Height;
        g_rt0 = s;
        g_rt0Fmt = desc.Format;
        g_vpMaxZ = 1.0f;  // D3D9: SetRenderTarget(0) nastavi viewport na cely cil, Z 0..1
        g_vp.X = g_vp.Y = 0;
        g_vp.Width = desc.Width;
        g_vp.Height = desc.Height;
        g_vp.MinZ = 0.0f;
        g_vp.MaxZ = 1.0f;
        g_vpKnown = s != nullptr;
        if (g_capState == CAP_ACTIVE) CapSurface("RT", i, s, desc);
    } else if (d == g_main && (g_capState == CAP_ACTIVE || (g_pathB && i == 1 && s))) {
        D3DSURFACE_DESC desc = {};
        if (s) s->GetDesc(&desc);
        LightsNoteRT(i, desc);
        if (g_capState == CAP_ACTIVE) CapSurface("RT", i, s, desc);
    }
    return o_SetRT(d, i, s);
}
static HRESULT STDMETHODCALLTYPE H_SetDS(IDirect3DDevice9* d, IDirect3DSurface9* s) {
    if (OmGuarded()) return o_SetDS(d, s);
    if (d == g_main) g_ds = s;
    if (d == g_main && g_capState == CAP_ACTIVE) {
        D3DSURFACE_DESC desc = {};
        if (s) s->GetDesc(&desc);
        CapSurface("DS", 0, s, desc);
    }
    return o_SetDS(d, s);
}
static HRESULT STDMETHODCALLTYPE H_Clear(IDirect3DDevice9* d, DWORD n, const D3DRECT* r, DWORD fl, D3DCOLOR c,
                                         float z, DWORD st) {
    if (OmGuarded()) return o_Clear(d, n, r, fl, c, z, st);
    if (d == g_main && g_sceneEndPending) FirePendingSceneEnd(d, "Clear");
    if (d == g_main && g_capState == CAP_ACTIVE) Cap("CLR flags=0x%X color=0x%08X z=%.3f", fl, c, z);
    if (d == g_main && (fl & D3DCLEAR_TARGET) && g_sceneEndDone && g_sceneRT && g_rt0 == g_sceneRT &&
        Present_Enabled())
        HlOnSceneWrite(d, false);  // K5: Clear barvy sceny po postu = HUD-less nejdriv
    if (d == g_main && (fl & D3DCLEAR_ZBUFFER) && !g_sceneEndDone) {  // pro vyber kamery (skybox x svet)
        g_zClearSeq = g_drawSeq;
        g_zClearValid = true;
        g_zClearDS = g_ds;
        RmOnZClear(d);  // maska reaktivity: znovu od nuly (3D skybox, svet)
    }
    return o_Clear(d, n, r, fl, c, z, st);
}
static HRESULT STDMETHODCALLTYPE H_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* vp) {
    if (OmGuarded()) return o_SetViewport(d, vp);
    if (d == g_main && g_capState == CAP_ACTIVE && vp)
        Cap("VP %u %u %ux%u z=%.2f..%.2f", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
    if (d == g_main && vp) {
        g_vpMaxZ = vp->MaxZ;
        g_vp = *vp;
        g_vpKnown = true;
    }
    return o_SetViewport(d, vp);
}

static HRESULT STDMETHODCALLTYPE H_StretchRect(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT* sr,
                                               IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f) {
    if (d == g_main) {
        if (g_sceneEndPending && !OmGuarded()) FirePendingSceneEnd(d, "StretchRect");
        // Cast B + LightsB (bod 6): kopie cele sceny do mensi textury (refrakce _rt_RefractTexture 1024x1024) - v B je
        // platny jen sub-rect, shadery refrakce ctou uv pohledu -> kopirovat jen sub-rect (jako v A cely obraz).
        RECT subr;
        if (g_lightsPatched && g_pathB && !g_pathBad && !g_sceneEndDone && src && src == g_sceneRT && !sr && dst &&
            !OmGuarded() && g_renderW && g_renderH && g_renderW < g_renderSurfW) {
            D3DSURFACE_DESC dd = {};
            if (SUCCEEDED(dst->GetDesc(&dd)) && (dd.Width != g_renderSurfW || dd.Height != g_renderSurfH)) {
                subr = {0, 0, (LONG)g_renderW, (LONG)g_renderH};
                sr = &subr;
                ++g_stB.refractFixed;
                static bool s_logged;
                if (!s_logged) {
                    s_logged = true;
                    Log("cast B + LightsB: kopie sceny do %ux%u (refrakce) jen ze sub-rectu %ux%u", dd.Width, dd.Height,
                        g_renderW, g_renderH);
                }
            }
        }
        if (g_capState == CAP_ACTIVE) {
            D3DSURFACE_DESC sd = {}, dd = {};
            if (src) src->GetDesc(&sd);
            if (dst) dst->GetDesc(&dd);
            char rs[64] = "-", rd[64] = "-";
            if (sr) snprintf(rs, sizeof rs, "%ld,%ld-%ld,%ld", sr->left, sr->top, sr->right, sr->bottom);
            if (dr) snprintf(rd, sizeof rd, "%ld,%ld-%ld,%ld", dr->left, dr->top, dr->right, dr->bottom);
            Cap("SR %p %ux%u -> %p %ux%u fmt=%u sr=%s dr=%s filtr=%d%s", (void*)src, sd.Width, sd.Height, (void*)dst,
                dd.Width, dd.Height, dd.Format, rs, rd, (int)f, src == g_sceneRT ? " (ze sceny)" : "");
        }
        // K5: StretchRect DO sceny po postu (prekryti obrazu) = HUD-less nejdriv
        if (dst && dst == g_sceneRT && g_sceneEndDone && !OmGuarded() && Present_Enabled()) HlOnSceneWrite(d, false);
        // Cast B: posledni kopie sceny ve snimku (i po konci sceny) - roztazeni pak cte jeji cil jako texturu 0
        if (src && src == g_sceneRT && dst) {
            g_lastSceneSR.dst = dst;
            g_lastSceneSR.tex = nullptr;
            IDirect3DTexture9* cont = nullptr;
            if (SUCCEEDED(dst->GetContainer(__uuidof(IDirect3DTexture9), (void**)&cont)) && cont) {
                g_lastSceneSR.tex = cont;  // jen identita (porovnava se s g_t0 v tomtez snimku)
                cont->Release();
            }
            g_lastSceneSR.hasSr = sr != nullptr;
            g_lastSceneSR.hasDr = dr != nullptr;
            g_lastSceneSR.sr = sr ? *sr : RECT{};
            g_lastSceneSR.dr = dr ? *dr : RECT{};
            g_lastSceneSR.valid = true;
            g_lastSceneSR.drawsSince = 0;
            g_bFollow = false;  // nova kopie: pripadny dalsi draw uz neni "hned nasledujici" roztazeni
        }
        if (src && src == g_sceneRT && !g_sceneEndDone && dst) {
            g_sceneCopy.dst = dst;
            g_sceneCopy.hasSrc = sr != nullptr;
            g_sceneCopy.hasDst = dr != nullptr;
            if (sr) g_sceneCopy.srcRect = *sr;
            if (dr) g_sceneCopy.dstRect = *dr;
            g_sceneCopy.filter = f;
            g_sceneCopy.valid = true;
            g_sceneCopy.drawsAfter = 0;
        }
    }
    return o_StretchRect(d, src, sr, dst, dr, f);
}
static HRESULT STDMETHODCALLTYPE H_SetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t) {
    // Textura 0 se sleduje porad (cast B: podpis draw roztazeni), mimo davku MV objektu
    if (d == g_main && stage == 0 && !OmGuarded()) g_t0 = t;
    if (d == g_main && stage < 4 && !OmGuarded()) g_tex4[stage] = t;
    if (d == g_main && g_capState == CAP_ACTIVE && !OmGuarded() &&
        (stage < 16 || (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3))) {
        IDirect3DSurface9* lvl0 = nullptr;
        if (t && t->GetType() == D3DRTYPE_TEXTURE)
            ((IDirect3DTexture9*)t)->GetSurfaceLevel(0, &lvl0);
        Cap("T%u %p (povrch %p)", stage, (void*)t, (void*)lvl0);
        if (lvl0) lvl0->Release();
    }
    return o_SetTexture(d, stage, t);
}

// MIPMAPLODBIAS pixel sampleru 0..15: zapamatovat hodnotu hry (mip bias pro DLAA ji pak upravuje).
static HRESULT STDMETHODCALLTYPE H_SetSamplerState(IDirect3DDevice9* d, DWORD s, D3DSAMPLERSTATETYPE type, DWORD v) {
    HRESULT hr = o_SetSamplerState(d, s, type, v);
    if (d == g_main && type == D3DSAMP_MIPMAPLODBIAS && s < 16 && !OmGuarded()) {
        g_mipShadow[s] = v;
        g_mipApplied[s] = v;  // zarizeni ted ma hodnotu hry
        g_mipDirty = true;    // pri dalsim drawu pripadne znovu pridat bias
    }
    return hr;
}

// Hra vzdy vidi svou hodnotu, i kdyz je v zarizeni prave hodnota s biasem.
static HRESULT STDMETHODCALLTYPE H_GetSamplerState(IDirect3DDevice9* d, DWORD s, D3DSAMPLERSTATETYPE type,
                                                   DWORD* v) {
    if (d == g_main && type == D3DSAMP_MIPMAPLODBIAS && s < 16 && v) {
        *v = g_mipShadow[s];
        return D3D_OK;
    }
    return o_GetSamplerState(d, s, type, v);
}

// Reset/ResetEx hlavniho zarizeni (okno <-> cela obrazovka, zmena rozliseni, pripadne alt-tab z vyhradni cele
// obrazovky). Hra pred nim uvolnuje vsechny render targety a po nem je vytvari znovu, DXVK znovu vytvori backbuffer
// a auto depth-stencil a nastavi je jako RT0/DS interne - BEZ nasich hooku SetRenderTarget/SetDepthStencilSurface.
static thread_local int t_inReset;

static void BeforeDeviceReset() {
    Log("Reset zarizeni: zacatek (backbuffer %ux%u)", g_bbW, g_bbH);
    // Most: dobehnout odeslane kopie nad povrchy hry, pustit reference, zapomenout snimek (reset historie DLSS)
    Interop_BeforeReset();
    // MV objektu: cil OBJMV, depth, textura palet, shadery, deklarace i reference zaznamu jsou vazane na zarizeni
    // (az po mostu - kopie OBJMV z posledniho snimku uz dobehla). Po Resetu se vytvori znovu; selhani se zkusi znovu.
    OmReleaseDevice("Reset zarizeni");
    g_omBroken = g_omSkinOff = g_omFailLogged = false;
    SoftDepthRelease();  // cast B + LightsB: nase kopie hloubky (D3DPOOL_DEFAULT)
    RmBeforeReset();     // maska reaktivity (D3DPOOL_DEFAULT)
    g_softBroken = false;
    MenuBeforeReset();  // textura atlasu pisma, kdyz je v D3DPOOL_DEFAULT
    LatRelease("Reset zarizeni");  // regulator latence: dotazy znovu az po Resetu, zapnuti az po 120 snimcich
    // Ukazatele na povrchy hry po Resetu neplati (povrchy jsou uvolnene)
    g_rt0 = g_ds = g_sceneRT = g_sceneDS = g_linDepth = nullptr;
    g_sceneCopy.valid = false;
    g_sceneCopy.dst = nullptr;
    // cast B: kopie sceny, textura 0 a stav snimku (po Resetu maji povrchy i textury nove ukazatele)
    g_lastSceneSR.valid = false;
    g_lastSceneSR.dst = nullptr;
    g_lastSceneSR.tex = nullptr;
    g_t0 = nullptr;
    memset(g_tex4, 0, sizeof g_tex4);
    g_hl.pending = g_hl.valid = false;
    g_bHandled = g_bFollow = g_bBetween = false;
    g_sceneObjDraws = 0;
    g_nVpHist = 0;
    g_vpHistLast = -1;
    g_zClearDS = nullptr;
    g_zClearValid = false;
    g_nCamCand = 0;
    g_lastCand = -1;
    g_camSet = false;
    g_prevCamValid = false;  // MV prvniho snimku po Resetu nulove (historie se stejne resetuje)
    // Zachyt: buffery a deklarace po Resetu mohou dostat stare adresy -> popisy (VDE/VBD/IBD) vypsat znovu
    if (g_capState == CAP_ACTIVE) Cap("# Reset zarizeni (stav zarizeni po nem vychozi, bez volani Set*)");
    std::lock_guard<std::mutex> lk(g_capMx);
    g_capDecls.clear();
    g_capVBs.clear();
    g_capIBs.clear();
}

static void AfterDeviceReset(IDirect3DDevice9* d, HRESULT hr, const char* what) {
    ResetMipState();  // vychozi stav sampleru (bias 0)
    ResetDrawState();  // bez deklarace, streamu a indexu, vychozi render staty
    // RT0 a DS, ktere nastavil Reset (backbuffer a auto depth-stencil), a nova velikost backbufferu
    g_rt0 = g_ds = nullptr;
    g_rt0W = g_rt0H = 0;
    g_rt0Fmt = D3DFMT_UNKNOWN;
    g_vpMaxZ = 1.0f;
    if (SUCCEEDED(hr)) {
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(d->GetRenderTarget(0, &s)) && s) {
            D3DSURFACE_DESC desc = {};
            s->GetDesc(&desc);
            g_rt0 = s;
            g_rt0W = desc.Width;
            g_rt0H = desc.Height;
            g_rt0Fmt = desc.Format;
            g_vp.X = g_vp.Y = 0;  // viewport po Resetu = cely backbuffer
            g_vp.Width = desc.Width;
            g_vp.Height = desc.Height;
            g_vp.MinZ = 0.0f;
            g_vp.MaxZ = 1.0f;
            g_vpKnown = true;
            s->Release();
        }
        s = nullptr;
        if (SUCCEEDED(d->GetDepthStencilSurface(&s)) && s) {
            g_ds = s;
            s->Release();
        }
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(d->GetSwapChain(0, &sc)) && sc) {
            D3DPRESENT_PARAMETERS pp = {};
            if (SUCCEEDED(sc->GetPresentParameters(&pp)) && pp.BackBufferWidth && pp.BackBufferHeight) {
                g_bbW = pp.BackBufferWidth;
                g_bbH = pp.BackBufferHeight;
            }
            sc->Release();
        }
    }
    // Prvni snimek po Resetu bez jitteru a mip biasu (most ho jeste nezpracoval, viz Interop_BeforeReset)
    for (int r = 0; r < 256; ++r)
        if (g_blkState[r] == 1) g_blkState[r] = 2;
    UpdateJitterState();
    UpdateJitter();
    g_mipSceneOn = Interop_GetMode() == BMSD_MODE_DLAA && g_jitterForced && g_jitterActive;
    g_mipBiasOn = MipBiasWanted() && g_mipSceneOn;
    Log("%s zarizeni -> 0x%08X: backbuffer %ux%u, RT0 %ux%u; most znovu po %s, historie DLSS se resetuje", what,
        (unsigned)hr, g_bbW, g_bbH, g_rt0W, g_rt0H, Interop_GetMode() ? "nekolika snimcich" : "zapnuti");
}

// Spike S2 ([present] HelperPresent=1): DXVK dostane upravenou kopii parametru (okno, Windowed), hra zpet sve - jen
// s tim, co D3D9 do parametru doplnuje (velikost a format backbufferu, pocet). Jen nejvnejsi volani (DXVK muze
// volat Reset/ResetEx vnitrne pres vtable).
static thread_local int t_s2Nest;

// ---------------------------------------------------------------------------
// MSAA hry ([upscale] StripMsaa, vychozi 1): s MSAA ve hre ma barva sceny vzorky a most kazdy snimek vynecha. Kdyz je
// mod aktivni (most neni vypnuty) v okamziku vytvareni prostredku, proxy v CreateDevice / Reset (backbuffer a
// automaticka hloubka) a v CreateRenderTarget / CreateDepthStencilSurface (i Ex) hlavniho zarizeni vynuti
// D3DMULTISAMPLE_NONE a kvalitu 0 - barva i hloubka konzistentne, takze RT a DS k sobe sedi. StretchRect "resolve"
// z MSAA je pak obycejna kopie. Alpha-to-coverage (ATOC, listi / mrize) bez vzorku DXVK kresli jako alpha test.
// Po vypnuti upscaleru v menu zustava hra bez MSAA az do Resetu / restartu (zmena nastaveni videa).
static bool MsaaStripActive() { return g_cfg.stripMsaa && Interop_GetMode() != BMSD_MODE_OFF; }

static void MsaaLog(const char* what, D3DMULTISAMPLE_TYPE ms, DWORD q, UINT w, UINT h) {
    static int s_logs;
    if (s_logs >= 6) return;
    if (++s_logs == 1)
        Log("MSAA hry vypnuto modem ([upscale] StripMsaa=1): %s %ux%u %ux, kvalita %lu -> bez MSAA", what, w, h,
            (unsigned)ms, (unsigned long)q);
    else
        Log("MSAA hry vypnuto modem: %s %ux%u %ux -> bez MSAA%s", what, w, h, (unsigned)ms,
            s_logs == 6 ? " (dalsi uz bez logu)" : "");
}

// Kopie parametru presentu bez MSAA (jen hlavni okno: backbuffer od 320x240, nebo 0 = velikost okna)
static bool MsaaStripPP(const D3DPRESENT_PARAMETERS* pp, D3DPRESENT_PARAMETERS& out, const char* what) {
    if (!pp || pp->MultiSampleType == D3DMULTISAMPLE_NONE || !MsaaStripActive()) return false;
    const bool main = pp->BackBufferWidth * pp->BackBufferHeight >= 320u * 240u ||
                      (!pp->BackBufferWidth && !pp->BackBufferHeight);
    if (!main) return false;
    out = *pp;
    MsaaLog(what, pp->MultiSampleType, pp->MultiSampleQuality, pp->BackBufferWidth, pp->BackBufferHeight);
    out.MultiSampleType = D3DMULTISAMPLE_NONE;
    out.MultiSampleQuality = 0;
    return true;
}

// Velikost a format backbufferu, ktere DXVK doplnil, zpet do struktury hry (MSAA tam zustava, jak ho hra chtela)
static void MsaaWriteBack(D3DPRESENT_PARAMETERS* game, const D3DPRESENT_PARAMETERS& used) {
    game->BackBufferWidth = used.BackBufferWidth;
    game->BackBufferHeight = used.BackBufferHeight;
    game->BackBufferFormat = used.BackBufferFormat;
    game->BackBufferCount = used.BackBufferCount;
}

static inline bool MsaaStripSurf(IDirect3DDevice9* d, D3DMULTISAMPLE_TYPE& ms, DWORD& q, const char* what, UINT w,
                                 UINT h) {
    if (ms == D3DMULTISAMPLE_NONE || d != g_main || !MsaaStripActive()) return false;
    MsaaLog(what, ms, q, w, h);
    ms = D3DMULTISAMPLE_NONE;
    q = 0;
    return true;
}

static HRESULT STDMETHODCALLTYPE H_CreateRT(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                            DWORD q, BOOL lock, IDirect3DSurface9** out, HANDLE* sh) {
    MsaaStripSurf(d, ms, q, "CreateRenderTarget", w, h);
    return o_CreateRT(d, w, h, f, ms, q, lock, out, sh);
}

static HRESULT STDMETHODCALLTYPE H_CreateDS(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                            DWORD q, BOOL discard, IDirect3DSurface9** out, HANDLE* sh) {
    MsaaStripSurf(d, ms, q, "CreateDepthStencilSurface", w, h);
    return o_CreateDS(d, w, h, f, ms, q, discard, out, sh);
}

static HRESULT STDMETHODCALLTYPE H_CreateRTEx(IDirect3DDevice9Ex* d, UINT w, UINT h, D3DFORMAT f,
                                              D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lock, IDirect3DSurface9** out,
                                              HANDLE* sh, DWORD usage) {
    MsaaStripSurf((IDirect3DDevice9*)d, ms, q, "CreateRenderTargetEx", w, h);
    return o_CreateRTEx(d, w, h, f, ms, q, lock, out, sh, usage);
}

static HRESULT STDMETHODCALLTYPE H_CreateDSEx(IDirect3DDevice9Ex* d, UINT w, UINT h, D3DFORMAT f,
                                              D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL discard, IDirect3DSurface9** out,
                                              HANDLE* sh, DWORD usage) {
    MsaaStripSurf((IDirect3DDevice9*)d, ms, q, "CreateDepthStencilSurfaceEx", w, h);
    return o_CreateDSEx(d, w, h, f, ms, q, discard, out, sh, usage);
}

static void S2WriteBack(D3DPRESENT_PARAMETERS* game, const D3DPRESENT_PARAMETERS& dx) {
    game->BackBufferWidth = dx.BackBufferWidth;
    game->BackBufferHeight = dx.BackBufferHeight;
    game->BackBufferFormat = dx.BackBufferFormat;
    game->BackBufferCount = dx.BackBufferCount;
}

static HRESULT S2Reset(IDirect3DDevice9* d9, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m,
                       HRESULT (*call)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*)) {
    ++t_s2Nest;
    Present_BeforeReset(d9);
    D3DPRESENT_PARAMETERS dx = *pp;
    const bool redirected = Present_PrepareReset(d9, &dx);
    const HRESULT hr = call(d9, &dx, redirected ? nullptr : m);  // v okne bez rezimu obrazovky
    S2WriteBack(pp, dx);
    Present_AfterReset(d9, hr);
    --t_s2Nest;
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    const bool mine = d == g_main && t_inReset++ == 0;
    if (mine) BeforeDeviceReset();
    D3DPRESENT_PARAMETERS* const gamePP = pp;
    D3DPRESENT_PARAMETERS msx;
    const bool msaaStripped = d == g_main && !t_s2Nest && MsaaStripPP(pp, msx, "Reset (backbuffer)");
    if (msaaStripped) pp = &msx;
    HRESULT hr;
    if (Present_Enabled() && pp && !t_s2Nest)
        hr = S2Reset(d, pp, nullptr, [](IDirect3DDevice9* dd, D3DPRESENT_PARAMETERS* p, D3DDISPLAYMODEEX*) {
            return o_Reset(dd, p);
        });
    else
        hr = o_Reset(d, pp);
    if (msaaStripped) MsaaWriteBack(gamePP, msx);
    if (mine) AfterDeviceReset(d, hr, "Reset");
    if (d == g_main) --t_inReset;
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_ResetEx(IDirect3DDevice9Ex* d, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m) {
    IDirect3DDevice9* d9 = (IDirect3DDevice9*)d;
    const bool mine = d9 == g_main && t_inReset++ == 0;
    if (mine) BeforeDeviceReset();
    D3DPRESENT_PARAMETERS* const gamePP = pp;
    D3DPRESENT_PARAMETERS msx;
    const bool msaaStripped = d9 == g_main && !t_s2Nest && MsaaStripPP(pp, msx, "ResetEx (backbuffer)");
    if (msaaStripped) pp = &msx;
    HRESULT hr;
    if (Present_Enabled() && pp && !t_s2Nest)
        hr = S2Reset(d9, pp, m, [](IDirect3DDevice9* dd, D3DPRESENT_PARAMETERS* p, D3DDISPLAYMODEEX* mm) {
            return o_ResetEx((IDirect3DDevice9Ex*)dd, p, mm);
        });
    else
        hr = o_ResetEx(d, pp, m);
    if (msaaStripped) MsaaWriteBack(gamePP, msx);
    if (mine) AfterDeviceReset(d9, hr, "ResetEx");
    if (d9 == g_main) --t_inReset;
    return hr;
}

// gameWnd = okno hry z parametru CreateDevice (pred presmerovanim spiku S2): pro hacek klavesnice menu.
static void HookDevice(IDirect3DDevice9* d, const D3DPRESENT_PARAMETERS* pp, HWND gameWnd) {
    UINT w = pp ? pp->BackBufferWidth : 0, h = pp ? pp->BackBufferHeight : 0;
    if (!w || !h) {  // 0 = velikost okna
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            D3DSURFACE_DESC desc = {};
            bb->GetDesc(&desc);
            w = desc.Width;
            h = desc.Height;
            bb->Release();
        }
    }
    if (w * h > g_mainArea) {
        OmReleaseDevice("nove hlavni zarizeni");  // objekty MV objektu patri staremu zarizeni
        RmBeforeReset();  // maska reaktivity: cil a identita hloubky patri staremu zarizeni
        LatRelease("nove hlavni zarizeni");
        MenuOnNewMainDevice();  // textura atlasu pisma patri staremu zarizeni
        MenuSetGameWindow(gameWnd);
        g_omBroken = g_omSkinOff = g_omFailLogged = false;
        g_main = d;
        g_mainArea = w * h;
        g_bbW = g_rt0W = w;
        g_bbH = g_rt0H = h;
        memset(g_blkState, 0, sizeof g_blkState);
        ResetMipState();  // nove zarizeni ma vychozi stav sampleru
        ResetDrawState();
        {
            std::lock_guard<std::mutex> lk(g_capMx);  // popisy objektu stareho zarizeni v zachytu neplati
            g_capDecls.clear();
            g_capVBs.clear();
            g_capIBs.clear();
            g_capVSDumped.clear();
        }
        Log("hlavni zarizeni: %p (%ux%u)", (void*)d, w, h);
    } else {
        Log("vedlejsi zarizeni: %p (%ux%u) - ignoruji", (void*)d, w, h);
    }

    HookVtbl(d, 17, (PFN_Present)H_Present, o_Present, "Device::Present");
    HookVtbl(d, 37, (PFN_SetRT)H_SetRT, o_SetRT, "Device::SetRenderTarget");
    HookVtbl(d, 39, (PFN_SetDS)H_SetDS, o_SetDS, "Device::SetDepthStencilSurface");
    HookVtbl(d, 43, (PFN_Clear)H_Clear, o_Clear, "Device::Clear");
    HookVtbl(d, 47, (PFN_SetViewport)H_SetViewport, o_SetViewport, "Device::SetViewport");
    HookVtbl(d, 34, (PFN_StretchRect)H_StretchRect, o_StretchRect, "Device::StretchRect");
    HookVtbl(d, 65, (PFN_SetTexture)H_SetTexture, o_SetTexture, "Device::SetTexture");
    HookVtbl(d, 68, (PFN_GetSamplerState)H_GetSamplerState, o_GetSamplerState, "Device::GetSamplerState");
    HookVtbl(d, 69, (PFN_SetSamplerState)H_SetSamplerState, o_SetSamplerState, "Device::SetSamplerState");
    HookVtbl(d, 16, (PFN_Reset)H_Reset, o_Reset, "Device::Reset");
    HookVtbl(d, 81, (PFN_DrawPrim)H_DrawPrim, o_DrawPrim, "Device::DrawPrimitive");
    HookVtbl(d, 82, (PFN_DrawIdxPrim)H_DrawIdxPrim, o_DrawIdxPrim, "Device::DrawIndexedPrimitive");
    HookVtbl(d, 83, (PFN_DrawPrimUP)H_DrawPrimUP, o_DrawPrimUP, "Device::DrawPrimitiveUP");
    HookVtbl(d, 84, (PFN_DrawIdxPrimUP)H_DrawIdxPrimUP, o_DrawIdxPrimUP, "Device::DrawIndexedPrimitiveUP");
    HookVtbl(d, 91, (PFN_CreateVS)H_CreateVS, o_CreateVS, "Device::CreateVertexShader");
    // cast B + LightsB / detektor: rozpoznani shaderu svetel a cteni light bufferu (LightsFixPre); FXAA hry (FxaaNotePs)
    // + mip bias normalovych map (MipNotePs)
    if (g_cfg.lightsB || g_cfg.lightsBDetect || g_cfg.skipGameFxaa || g_cfg.mipBiasNormal != 0.0f)
        HookVtbl(d, 106, (PFN_CreatePS)H_CreatePS, o_CreatePS, "Device::CreatePixelShader");
    HookVtbl(d, 92, (PFN_SetVS)H_SetVS, o_SetVS, "Device::SetVertexShader");
    HookVtbl(d, 94, (PFN_SetVSConstF)H_SetVSConstF, o_SetVSConstF, "Device::SetVertexShaderConstantF");
    // stin stavu pro MV objektu (deklarace, stream 0, indexy, render staty) a zachyt
    HookVtbl(d, 57, (PFN_SetRenderState)H_SetRenderState, o_SetRenderState, "Device::SetRenderState");
    HookVtbl(d, 87, (PFN_SetVDecl)H_SetVDecl, o_SetVDecl, "Device::SetVertexDeclaration");
    HookVtbl(d, 89, (PFN_SetFVF)H_SetFVF, o_SetFVF, "Device::SetFVF");
    HookVtbl(d, 100, (PFN_SetStreamSource)H_SetStreamSource, o_SetStreamSource, "Device::SetStreamSource");
    HookVtbl(d, 102, (PFN_SetStreamSourceFreq)H_SetStreamSourceFreq, o_SetStreamSourceFreq,
             "Device::SetStreamSourceFreq");
    HookVtbl(d, 104, (PFN_SetIndices)H_SetIndices, o_SetIndices, "Device::SetIndices");
    // okluzni dotazy hry (histogram expozice): konec sceny se nesmi zapocitat do dotazu (H_QueryIssue)
    HookVtbl(d, 118, (PFN_CreateQuery)H_CreateQuery, o_CreateQuery, "Device::CreateQuery");
    if (g_cfg.stripMsaa) {  // MSAA hry ([upscale] StripMsaa): RT a hloubka bez vzorku
        HookVtbl(d, 28, (PFN_CreateRT)H_CreateRT, o_CreateRT, "Device::CreateRenderTarget");
        HookVtbl(d, 29, (PFN_CreateDS)H_CreateDS, o_CreateDS, "Device::CreateDepthStencilSurface");
    }

    IDirect3DDevice9Ex* ex = nullptr;
    if (SUCCEEDED(d->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&ex)) && ex) {
        HookVtbl(ex, 121, (PFN_PresentEx)H_PresentEx, o_PresentEx, "DeviceEx::PresentEx");
        HookVtbl(ex, 132, (PFN_ResetEx)H_ResetEx, o_ResetEx, "DeviceEx::ResetEx");
        if (g_cfg.stripMsaa) {
            HookVtbl(ex, 129, (PFN_CreateRTEx)H_CreateRTEx, o_CreateRTEx, "DeviceEx::CreateRenderTargetEx");
            HookVtbl(ex, 131, (PFN_CreateDSEx)H_CreateDSEx, o_CreateDSEx, "DeviceEx::CreateDepthStencilSurfaceEx");
        }
        ex->Release();
    }
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(d->GetSwapChain(0, &sc)) && sc) {
        HookVtbl(sc, 3, (PFN_SCPresent)H_SCPresent, o_SCPresent, "SwapChain::Present");
        sc->Release();
    }
}

static HRESULT STDMETHODCALLTYPE H_CreateDevice(IDirect3D9* self, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                                D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT hr;
    const HWND gameWnd = pp && pp->hDeviceWindow ? pp->hDeviceWindow : w;  // (spike S2 nize okna meni)
    D3DPRESENT_PARAMETERS* const gamePP = pp;
    D3DPRESENT_PARAMETERS msx;
    const bool msaaStripped = !t_s2Nest && MsaaStripPP(pp, msx, "CreateDevice (backbuffer)");
    if (msaaStripped) pp = &msx;
    if (Present_Enabled() && pp && !t_s2Nest) {  // spike S2: DXVK do skryteho okna
        ++t_s2Nest;
        D3DPRESENT_PARAMETERS dx = *pp;
        void* tok = nullptr;
        Present_PrepareCreate(&w, &f, &dx, &tok);
        hr = o_CreateDevice(self, a, t, w, f, &dx, out);
        Present_AfterCreate(tok, hr, SUCCEEDED(hr) && out ? *out : nullptr);
        S2WriteBack(pp, dx);
        --t_s2Nest;
    } else {
        hr = o_CreateDevice(self, a, t, w, f, pp, out);
    }
    if (msaaStripped) MsaaWriteBack(gamePP, msx);
    pp = gamePP;
    Log("CreateDevice -> 0x%08X (%ux%u)", hr, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
    if (SUCCEEDED(hr) && out && *out) HookDevice(*out, pp, gameWnd);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateDeviceEx(IDirect3D9Ex* self, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                                  D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m,
                                                  IDirect3DDevice9Ex** out) {
    HRESULT hr;
    const HWND gameWnd = pp && pp->hDeviceWindow ? pp->hDeviceWindow : w;
    D3DPRESENT_PARAMETERS* const gamePP = pp;
    D3DPRESENT_PARAMETERS msx;
    const bool msaaStripped = !t_s2Nest && MsaaStripPP(pp, msx, "CreateDeviceEx (backbuffer)");
    if (msaaStripped) pp = &msx;
    if (Present_Enabled() && pp && !t_s2Nest) {  // spike S2: DXVK do skryteho okna
        ++t_s2Nest;
        D3DPRESENT_PARAMETERS dx = *pp;
        void* tok = nullptr;
        Present_PrepareCreate(&w, &f, &dx, &tok);
        hr = o_CreateDeviceEx(self, a, t, w, f, &dx, tok ? nullptr : m, out);
        Present_AfterCreate(tok, hr, SUCCEEDED(hr) && out ? (IDirect3DDevice9*)*out : nullptr);
        S2WriteBack(pp, dx);
        --t_s2Nest;
    } else {
        hr = o_CreateDeviceEx(self, a, t, w, f, pp, m, out);
    }
    if (msaaStripped) MsaaWriteBack(gamePP, msx);
    pp = gamePP;
    Log("CreateDeviceEx -> 0x%08X (%ux%u)", hr, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
    if (SUCCEEDED(hr) && out && *out) HookDevice(*out, pp, gameWnd);
    return hr;
}

extern "C" IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdk) {
    auto fn = (IDirect3D9*(WINAPI*)(UINT))g_fn[10];
    IDirect3D9* d3d = fn ? fn(sdk) : nullptr;
    Log("Direct3DCreate9(%u) -> %p", sdk, (void*)d3d);
    if (d3d) HookVtbl(d3d, 16, (PFN_CreateDevice)H_CreateDevice, o_CreateDevice, "D3D9::CreateDevice");
    return d3d;
}

extern "C" HRESULT WINAPI Hook_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    auto fn = (HRESULT(WINAPI*)(UINT, IDirect3D9Ex**))g_fn[11];
    HRESULT hr = fn ? fn(sdk, out) : E_FAIL;
    Log("Direct3DCreate9Ex(%u) -> 0x%08X", sdk, hr);
    if (SUCCEEDED(hr) && out && *out) {
        HookVtbl(*out, 16, (PFN_CreateDevice)H_CreateDevice, o_CreateDevice, "D3D9Ex::CreateDevice");
        HookVtbl(*out, 20, (PFN_CreateDeviceEx)H_CreateDeviceEx, o_CreateDeviceEx, "D3D9Ex::CreateDeviceEx");
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Zaznam padu (i pro hlaseni chyb od uzivatelu): vektorovy handler vyjimek zapise do bms_dlss.log kod, adresu (RVA
// v d3d9.dll nebo modul+offset), registry a navratove adresy naseho modulu ze zasobniku (RVA -> map soubor buildu).
// Jen fatalni kody (access violation, illegal instruction, deleni nulou, stack overflow ...) a jen kdyz instrukce lezi
// v nasem modulu, nebo je nase navratova adresa na zasobniku do kCrashScanBytes. First-chance vyjimky hry a ovladace
// bez nasi adresy se ignoruji. Nikdy nevraci EXCEPTION_CONTINUE_EXECUTION (jen zaloguje a hleda se dal). Nejvys
// kCrashMaxLogs zaznamu s chybou v proxy a kCrashMaxLogs zaznamu mimo proxy (zvlast, at osetrene first-chance
// vyjimky ovladace nevycerpaji limit pro skutecny pad v proxy). Navic retezeny SetUnhandledExceptionFilter: radek o neosetrene vyjimce (kdekoli),
// pokud ho hra / Steam neprepsaly vlastnim filtrem.
static uintptr_t g_selfBase, g_selfEnd;  // obraz d3d9.dll (SizeOfImage)
static PVOID g_veh;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevUef;
static volatile LONG g_crashLogsIn, g_crashLogsOut, g_crashBusy;
static const LONG kCrashMaxLogs = 3;
static const UINT kCrashScanBytes = 8192, kCrashMaxRets = 12;
static char g_crashBuf[4096];  // staticky: pri stack overflow neni zasobnik

static inline bool InSelf(uintptr_t a) { return a >= g_selfBase && a < g_selfEnd; }

static bool CrashFatalCode(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case 0xC0000374:  // poskozeni haldy
        return true;
    }
    return false;
}

// Navratova adresa do naseho modulu: hned pred ni instrukce call (E8 rel32 nebo FF /2 s ruznou delkou ModRM).
static bool CrashLooksLikeRet(uintptr_t a) {
    if (!InSelf(a) || a - g_selfBase < 0x1000) return false;  // hlavicka PE: adresa v kodu je az za ni
    const uint8_t* p = (const uint8_t*)a;
    if (p[-5] == 0xE8) return true;
    for (int len = 2; len <= 7; ++len)
        if (p[-len] == 0xFF && (p[-len + 1] & 0x38) == 0x10) return true;
    return false;
}

// "modul+0xOFFSET" pro adresu mimo nas modul (jen jmeno souboru)
static void CrashModuleOf(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)a, &m) && m && GetModuleFileNameW(m, path, MAX_PATH)) {
        const wchar_t* b = wcsrchr(path, L'\\');
        snprintf(out, n, "%ls+0x%lX", b ? b + 1 : path, (unsigned long)(a - (uintptr_t)m));
    } else {
        snprintf(out, n, "%p (bez modulu)", (void*)a);
    }
}

static void CrashWrite(const char* text) {
    bool locked = false;
    for (int i = 0; i < 50 && !(locked = g_logMx.try_lock()); ++i) Sleep(2);  // vlakno muze drzet zamek samo
    if (g_log) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text);
        fflush(g_log);
    }
    if (locked) g_logMx.unlock();
}

static void CrashRecord(EXCEPTION_POINTERS* ep, const char* kind) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    const uintptr_t ip = (uintptr_t)er->ExceptionAddress;
    // navratove adresy naseho modulu na zasobniku (jen v mezich zasobniku vlakna)
    uintptr_t rets[kCrashMaxRets];
    UINT nRets = 0;
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    uintptr_t sp = c->Esp, top = (uintptr_t)tib->StackBase;
    if (sp < (uintptr_t)tib->StackLimit || sp >= top) sp = top;  // Esp mimo zasobnik vlakna: neskenovat
    const uintptr_t end = std::min<uintptr_t>(top, sp + kCrashScanBytes);
    for (uintptr_t q = sp & ~3u; q + 4 <= end && nRets < kCrashMaxRets; q += 4) {
        const uintptr_t v = *(const uintptr_t*)q;
        if (CrashLooksLikeRet(v) && (!nRets || rets[nRets - 1] != v)) rets[nRets++] = v;
    }
    const bool selfIp = InSelf(ip);
    if (!selfIp && !nRets) return;  // vyjimka hry / ovladace bez nasi ucasti
    if (InterlockedIncrement(selfIp ? &g_crashLogsIn : &g_crashLogsOut) > kCrashMaxLogs) return;
    char where[160], extra[96] = "";
    if (selfIp) snprintf(where, sizeof where, "d3d9.dll (proxy) RVA 0x%lX", (unsigned long)(ip - g_selfBase));
    else CrashModuleOf(ip, where, sizeof where);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        snprintf(extra, sizeof extra, ", %s adresy %p", er->ExceptionInformation[0] == 0 ? "cteni"
                 : er->ExceptionInformation[0] == 8 ? "spusteni" : "zapis", (void*)er->ExceptionInformation[1]);
    int n = snprintf(g_crashBuf, sizeof g_crashBuf,
                     "PAD (%s): vyjimka 0x%08lX%s na %s, vlakno %lu, snimek %llu%s | eax=%08lX ebx=%08lX ecx=%08lX "
                     "edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX efl=%08lX | proxy base %p, navratove "
                     "adresy proxy (RVA, od vrcholu zasobniku):",
                     kind, (unsigned long)er->ExceptionCode, extra, where, GetCurrentThreadId(),
                     (unsigned long long)g_frame,
                     selfIp ? "" : " (mimo proxy, proxy na zasobniku - muze jit i o osetrenou first-chance vyjimku)",
                     c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags,
                     (void*)g_selfBase);
    for (UINT i = 0; i < nRets && n > 0 && n < (int)sizeof g_crashBuf - 16; ++i)
        n += snprintf(g_crashBuf + n, sizeof g_crashBuf - n, " 0x%lX", (unsigned long)(rets[i] - g_selfBase));
    if (!nRets && n > 0 && n < (int)sizeof g_crashBuf - 16) snprintf(g_crashBuf + n, sizeof g_crashBuf - n, " zadne");
    CrashWrite(g_crashBuf);
}

static LONG CALLBACK CrashVeh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord || !CrashFatalCode(ep->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    if ((g_crashLogsIn >= kCrashMaxLogs && g_crashLogsOut >= kCrashMaxLogs) || InterlockedExchange(&g_crashBusy, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    CrashRecord(ep, "first-chance");
    InterlockedExchange(&g_crashBusy, 0);
    return EXCEPTION_CONTINUE_SEARCH;  // nikdy nepokracovat v miste chyby: jen zaznam, osetreni at resi dalsi handler
}

static LONG WINAPI CrashUef(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && !InterlockedExchange(&g_crashBusy, 1)) {
        const uintptr_t ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        char where[160];
        if (InSelf(ip)) snprintf(where, sizeof where, "d3d9.dll (proxy) RVA 0x%lX", (unsigned long)(ip - g_selfBase));
        else CrashModuleOf(ip, where, sizeof where);
        snprintf(g_crashBuf, sizeof g_crashBuf, "PAD: neosetrena vyjimka 0x%08lX na %s, vlakno %lu - proces konci",
                 (unsigned long)ep->ExceptionRecord->ExceptionCode, where, GetCurrentThreadId());
        CrashWrite(g_crashBuf);
        InterlockedExchange(&g_crashBusy, 0);
    }
    return g_prevUef ? g_prevUef(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static void CrashLogInstall(HMODULE self) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)self;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)self + dos->e_lfanew);
    g_selfBase = (uintptr_t)self;
    g_selfEnd = g_selfBase + nt->OptionalHeader.SizeOfImage;
    g_veh = AddVectoredExceptionHandler(1, CrashVeh);
    g_prevUef = SetUnhandledExceptionFilter(CrashUef);
    Log("zaznam padu: zapnut (proxy %p-%p, %s)", (void*)g_selfBase, (void*)g_selfEnd,
        g_veh ? "vektorovy handler + filtr neosetrenych vyjimek" : "jen filtr neosetrenych vyjimek");
}

static void CrashLogRemove() {
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;
    // filtr neosetrenych vyjimek vratit jen kdyz je porad nas (jinak by se prepsal cizi)
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(g_prevUef);
    if (cur != CrashUef) SetUnhandledExceptionFilter(cur);
}

// ---- Start: log minuleho spusteni, hlavicka logu, DXVK ----
// Log minuleho spusteni: skoncilo chybou GPU nebo padem procesu? (pro bezpecny start a uschovu logu)
static bool PrevLogFailure(const std::wstring& path, char* why, size_t n) {
    why[0] = 0;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string t;
    char buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0 && t.size() < (16u << 20)) t.append(buf, r);
    fclose(f);
    if (t.find("DEVICE_LOST") != std::string::npos) snprintf(why, n, "ztrata zarizeni GPU (DEVICE_LOST)");
    else if (t.find("PAD: neosetrena") != std::string::npos) snprintf(why, n, "pad procesu hry");
    return why[0] != 0;
}

// Log s chybou uschovat jako bms_dlss.crash-<datum_cas>.log (nejvys 3 nejnovejsi), dalsi spusteni ho neprepise
static void KeepCrashLog(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    SYSTEMTIME st = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        FILETIME lt;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
    }
    wchar_t name[64];
    swprintf(name, 64, L"\\bms_dlss.crash-%04u%02u%02u-%02u%02u%02u.log", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
    CopyFileW(path.c_str(), (std::wstring(g_dir) + name).c_str(), FALSE);
    std::vector<std::wstring> all;  // jmena s datem se radi chronologicky
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW((std::wstring(g_dir) + L"\\bms_dlss.crash-*.log").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do all.push_back(fd.cFileName);
        while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i + 3 < all.size(); ++i) DeleteFileW((std::wstring(g_dir) + L"\\" + all[i]).c_str());
}

// Verze DXVK z d3d9_dxvk.dll (retezec "vX.Y.Z" v datech knihovny); "" = nenalezena
// Build hry ze Steamu (steamappsppmanifest_362890.acf, 5 slozek nad proxy): "buildid"; "" = nezjisteno.
// Jen informace do logu: mod je overeny na buildech kKnownBuilds, po jine aktualizaci hry projit capture / MV.
static std::string GameBuildId() {
    const std::wstring p = std::wstring(g_dir) + L"\\..\\..\\..\\..\\..\\appmanifest_362890.acf";
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return "";
    char buf[8192];
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char* k = strstr(buf, "\"buildid\"");
    if (!k) return "";
    k += 9;
    while (*k && *k != '"') ++k;
    if (*k != '"') return "";
    const char* e = strchr(++k, '"');
    return e && e - k < 20 ? std::string(k, e) : std::string();
}

static std::string DxvkVersion(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return "";
    std::string t;
    char buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0 && t.size() < (64u << 20)) t.append(buf, r);
    fclose(f);
    auto dig = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t i = 1; i + 6 < t.size(); ++i) {
        if (t[i] != 'v' || t[i - 1] != 0 || !dig(t[i + 1])) continue;
        size_t j = i + 1, dots = 0;
        while (j < t.size() && (dig(t[j]) || t[j] == '.') && j - i < 16) dots += t[j++] == '.';
        if (dots == 2 && j < t.size() && t[j] == 0 && t[j - 1] != '.') return t.substr(i, j - i);
    }
    return "";
}

// Hlavicka logu: verze modu, datum, Windows, napajeni (pro hlaseni chyb)
static void LogHeader() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD build = 0, major = 0, minor = 0;
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) {
        OSVERSIONINFOW vi = {sizeof vi};
        if (fn(&vi) == 0) {
            major = vi.dwMajorVersion;
            minor = vi.dwMinorVersion;
            build = vi.dwBuildNumber;
        }
    }
    SYSTEM_POWER_STATUS ps = {};
    const bool pwr = GetSystemPowerStatus(&ps) != 0;
    Log("bms-dlss %s (rozhrani %d, sestaveno %s %s), spusteno %04u-%02u-%02u, Windows %lu.%lu build %lu, napajeni %s",
        BMSDLSS_VER_STR, BMSD_VERSION, __DATE__, __TIME__, st.wYear, st.wMonth, st.wDay, major, minor, build,
        !pwr ? "?" : ps.ACLineStatus == 1 ? "sit" : ps.ACLineStatus == 0 ? "BATERIE" : "?");
}

// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        GetModuleFileNameW(h, g_dir, MAX_PATH);
        if (wchar_t* s = wcsrchr(g_dir, L'\\')) *s = 0;
        std::wstring logPath = std::wstring(g_dir) + L"\\bms_dlss.log";
        // Log minuleho spusteni se zachova jako .prev (napr. kdyz hra zamrzla a byla ukoncena); kdyz skoncil chybou
        // GPU nebo padem, navic jako bms_dlss.crash-<cas>.log (nejvys 3) a tento start pobezi bez FG ([fg] SafeStart)
        char prevWhy[96];
        const bool prevFail = PrevLogFailure(logPath, prevWhy, sizeof prevWhy);
        if (prevFail) KeepCrashLog(logPath);
        MoveFileExW(logPath.c_str(), (logPath + L".prev").c_str(), MOVEFILE_REPLACE_EXISTING);
        g_log = _wfopen(logPath.c_str(), L"w");
        Log("%s nacten, slozka: %ls", kMarker, g_dir);
        LogHeader();
        if (prevFail) Log("minule spusteni skoncilo chybou: %s (log uschovan jako bms_dlss.crash-*.log)", prevWhy);
        Present_SetSafeStart(prevFail ? prevWhy : "");
        CrashLogInstall(h);
        LoadConfig();
        Present_LoadConfig(IniPath().c_str());  // S2 a Frame Generation; pri HelperPresent=0 a [fg] Mode=0 nic (ani log)
        g_jitterOn = g_cfg.startEnabled;
        g_preset = g_cfg.preset;
        Interop_SetPreset(g_preset);
        g_inputMode = g_cfg.inputMode;
        g_inputModeL = g_cfg.inputModeL;
        g_alphaJit = g_cfg.alphaJit;
        Interop_SetInputMode(EffInputMode());
        g_depthMode = g_cfg.depthMode;
        Interop_SetDepthMode(g_depthMode);
        g_sharpness = g_cfg.sharpness;
        Interop_SetSharpness(g_sharpness);
        g_mipBiasEnabled = g_cfg.mipBias != 0.0f;  // 0 = vypnuto; Alt+Delete pak zapne -0.5
        g_mipBiasBase = g_mipBiasValue = g_mipBiasEnabled ? g_cfg.mipBias : -0.5f;
        g_mipBIdx = g_mipBiasEnabled ? 0 : 3;  // cast B: 0 = log2(render/vystup) + MipBias; MipBias=0 -> vypnuto
        g_outScale = g_cfg.outScale;  // helperu ho preda SyncOutScale po volbe upscaleru nize
        g_downFilter = g_cfg.downsampleFilter;
        Interop_SetDownsampleFilter(g_downFilter);
        g_phasesSetting = g_cfg.phases;  // Alt+F2 cykli od hodnoty z ini
        g_omEnabled = g_cfg.objMv;       // MV pohyblivych objektu (Alt+Backspace)
        g_omValidate = g_cfg.objMvValidate;
        Interop_SetPipeline(g_cfg.pipeline);
        UpdateScaleState();  // pocet fazi z ini (auto zatim pro S = 1)
        // StartMode: 0 = vypnuto, 1 = kopie, 2 = zrcadleni, 3 = DLAA, 4 = ladeni MV
        // [upscale] Upscaler=off: most po startu vypnuty i pri StartMode=3 (menu "vypnuto" se ale neuklada)
        // (helper jeste nebezi: DLSS tu plati za dostupne; kdyz na teto karte nejde, helper sam prejde na FSR)
        if (g_cfg.upscaler != UPS_OFF && !UpscalerAvailable(g_cfg.upscaler))
            Log("upscaler: [upscale] Upscaler=%s se pripravuje - zatim auto", UpscalerName(g_cfg.upscaler));
        else if (g_cfg.upscaler != UPS_OFF)
            g_upscaler = g_cfg.upscaler;
        Interop_SetUpscaler((uint32_t)g_upscaler);  // pred startem helperu: feature vznikne rovnou s nim
        SyncOutScale();                             // FSR: bez supersamplingu (az po volbe upscaleru)
        if (g_cfg.upscaler == UPS_OFF)
            Log("upscaler: [upscale] Upscaler=off - most po startu vypnuty (StartMode se nepouzije)");
        else if (UINT m = (UINT)wcstol(IniStr(L"bridge", L"StartMode", L"0").c_str(), nullptr, 0))
            Interop_SetMode(m);
        UpdateJitterState();

        std::wstring real = std::wstring(g_dir) + L"\\d3d9_dxvk.dll";
        {  // Rozhrani interop (dxvk_interop.h) je podle DXVK 3.x: jina hlavni verze -> interop nepouzivat (mod vypnuty)
            const std::string dv = DxvkVersion(real);
            const bool known = dv.size() > 2 && dv[1] == '3' && dv[2] == '.';
            const bool allow = GetPrivateProfileIntW(L"bridge", L"AllowUnknownDxvk", 0, IniPath().c_str()) != 0;
            Log("DXVK: %s%s", dv.empty() ? "verze nezjistena" : dv.c_str(),
                known ? "" : allow ? " - neznama verze, [bridge] AllowUnknownDxvk=1: pouzivam presto"
                                   : " - neznama verze: most a Frame Generation vypnuty ([bridge] AllowUnknownDxvk=1 "
                                     "zkusi presto)");
            if (!known && !allow && GetFileAttributesW(real.c_str()) != INVALID_FILE_ATTRIBUTES) {
                Interop_BlockVk();
                g_dxvkWarn = true;
            }
        }
        {  // build hry: jen log (overeno: 26713286 = 17. 11. 2026, 26768221 = overeno 22. 11. 2026, v24z)
            static const char* kKnownBuilds[] = {"26713286", "26768221"};
            const std::string b = GameBuildId();
            bool kb = false;
            for (const char* x : kKnownBuilds) kb = kb || b == x;
            Log("hra: Steam build %s%s", b.empty() ? "nezjisten" : b.c_str(),
                b.empty() || kb ? "" : " - WARN: na tomto buildu mod neovereny (po aktualizaci hry zkontrolovat MV a kameru)");
        }
        g_real = LoadLibraryW(real.c_str());
        if (!g_real) {
            g_dxvkWarn = true;
            Log("CHYBA: nelze nacist %ls (err %lu), zkousim systemovy d3d9.dll", real.c_str(), GetLastError());
            wchar_t sys[MAX_PATH];
            GetSystemDirectoryW(sys, MAX_PATH);
            g_real = LoadLibraryW((std::wstring(sys) + L"\\d3d9.dll").c_str());
        }
        for (int i = 0; i < 17; ++i) {
            g_fn[i] = g_real ? GetProcAddress(g_real, kNames[i]) : nullptr;
            if (!g_fn[i]) Log("WARN: export %s nenalezen", kNames[i]);
        }
        Log("realna knihovna: %p", (void*)g_real);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_procExiting = reserved != nullptr;
        Log("konec, snimku: %llu", (unsigned long long)g_frame);
        MenuShutdown(reserved != nullptr);  // hacky klavesnice ukazuji do teto DLL (reserved != 0: konec procesu)
        CrashLogRemove();
        std::unique_lock<std::mutex> lk(g_logMx, std::defer_lock);
        for (int i = 0; i < 50 && !lk.try_lock(); ++i) Sleep(2);
        if (lk.owns_lock() && g_log) fclose(g_log);  // (zamek drzi ukoncene vlakno: soubor zavre system)
        g_log = nullptr;
    }
    return TRUE;
}
