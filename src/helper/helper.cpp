// bms-dlss: 64bitovy pomocny proces.
//
// Spousti ho proxy d3d9.dll ze hry: bms_dlss_helper.exe <pid hry>
// Importuje sdilene obrazy a timeline semafory z Vulkan zarizeni DXVK a kazdy
// snimek zpracuje COLOR_IN -> COLOR_OUT podle rezimu (BMSD_MODE_*):
//   DLAA       - compute shader spocita motion vektory z hloubky a kamer (mv.comp),
//                NVIDIA NGX DLSS v nativnim rozliseni (DLAA) zapise do vlastniho RGBA8 obrazu,
//                blit (prevod RGBA -> BGRA) do COLOR_OUT
//                Vstup DLSS (BmsdShared::inputMode, Alt+Insert): 0 = LDR (barva sceny primo), 1 az 3 = linearni
//                HDR (linearize.comp sRGB -> RGBA16F, DLSS do RGBA16F, alpha.comp HDR_IN zpet do sRGB RGBA8
//                pred RCAS), 1 s pevnou expozici 1.0, 2 s auto expozici DLSS, 3 navic s inverznim tone
//                mappingem (presne invertovatelny jasovy Reinhard, [dlaa] ITMStrength) a auto expozici
//                Hloubka DLSS (BmsdShared::depthMode, Alt+F5): 0 = DEPTH_IN primo, 1 = depthmin.comp R32F minimum
//                okoli 3x3, 2 = R32F konstantni 1.0 (diagnostika); MV vzdy z puvodni hloubky
//                Supersampling (BmsdShared::outScale, [upscale] OutputScale, Alt+F3): S > 1 = vstup W x H (beze
//                zmeny), vystup DLSS round(W*S) x round(H*S) v rezimu podle pomeru (Quality, Balanced, ... - overeno
//                NGX_DLSS_GET_OPTIMAL_SETTINGS), downsample.comp zmensi zpet na W x H (Lanczos-2, u HDR vcetne prevodu
//                do sRGB, alfa z COLOR_IN), pak RCAS. Kdyz S nejde, bezi DLAA (S = 1) a duvod je v logu.
//                Velikosti: g_w x g_h = sdilene obrazy = finalni obraz; g_inW x g_inH = vlastni vstupni obrazy
//                (MV, linearni barva, hloubka R32F; vzdy = g_w x g_h); g_rW x g_rH = render = skutecny vstup DLSS
//                (cast A = g_w x g_h); g_outW x g_outH = vystup DLSS. Obrazy velikosti vystupu vznikaji v CreateDlss.
//                Cast B (od verze 10, BmsdShared::renderWidth/Height): hra kresli scenu mensi (mat_viewportscale)
//                do leveho horniho rohu plnych cilu. Platny vstup je jen sub-rect g_rW x g_rH sdilenych obrazu:
//                mv.comp a linearize.comp pocitaji jen v nem, DLSS dostane InRenderSubrectDimensions a rezim podle
//                pomeru (Quality / Balanced / Performance), vystup je primo g_w x g_h (bez zmenseni; alfa a RCAS
//                jako u DLAA, alfa se cte z COLOR_IN v meritku sub-rectu). Kdyz DLSS pro dany vstup nejde, neni to
//                trvale selhani: helperRenderOk = 0, proxy most nespousti a obraz roztahne engine. Bez DLSS
//                (kopie, selhane vyhodnoceni) helper sub-rect roztahne blitem (LINEAR), ladeni MV NEAREST.
//   ladeni MV  - misto obrazu vizualizace motion vektoru (stejny shader)
//   kopie, zrcadleni - test cesty tam a zpet (krok 3b)
// Kdyz NGX/DLSS nejde pouzit, rezim DLAA jen kopiruje (hra bezi dal) a duvod je v logu
// a v helperMsg. Nastaveni cte z bms_dlss.ini vedle exe (sekce [dlaa]).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <wintrust.h>
#include <winver.h>
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>  // DLSS Frame Generation (src/helper/fg.inl)
// AMD FSR pres ffx API (FidelityFX SDK 2.4, Kits\FidelityFX; jen hlavicky, DLL se nacitaji za behu - LoadFfx)
#include "api/include/ffx_api.h"
#include "api/include/vk/ffx_api_vk.h"
#include "upscalers/include/ffx_upscale.h"
#include "framegeneration/include/ffx_framegeneration.h"  // FSR Frame Generation (src/helper/fg.inl)
#include "afg/afg.h"  // adaptivni FSR FG: vlastni FSR 3.1.4 s libovolnym t (build\x64\afg.lib, src/helper/afg)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

#include "../bmsd_shared.h"
#include "../res/version.h"
#include "mv_comp.h"     // SPIR-V z src/helper/mv.comp (generuje build.ps1 pres glslang)
#include "alpha_comp.h"  // SPIR-V z src/helper/alpha.comp
#include "sharpen_comp.h"  // SPIR-V z src/helper/sharpen.comp (RCAS)
#include "fgsharp_comp.h"  // SPIR-V z src/helper/fgsharp.comp (RCAS generovanych snimku FG)
#include "uialpha_comp.h"  // SPIR-V z src/helper/uialpha.comp (maska UI pro DLSS-G, v25u)
#include "alpha_hdr_comp.h"  // SPIR-V z src/helper/alpha.comp s -DHDR_IN (linearni HDR -> sRGB + alfa)
#include "linearize_comp.h"  // SPIR-V z src/helper/linearize.comp (sRGB -> linearni RGBA16F)
#include "depthmin_comp.h"   // SPIR-V z src/helper/depthmin.comp (hloubka pro DLSS: min 3x3 / konstanta)
#include "reactive_comp.h"   // SPIR-V z src/helper/reactive.comp (maska reaktivity pro FSR / DLSS)
#include "downsample_comp.h"      // SPIR-V z src/helper/downsample.comp (vystup DLSS S > 1 -> W x H)
#include "downsample_hdr_comp.h"  // totez s -DHDR_IN (zdroj linearni RGBA16F, prevod do sRGB pro kazdy tap)
#include "downsample_s_comp.h"      // obe varianty s -DMAX_SRC=26: mensi dlazdice pro S <= 2.18 (vyssi obsazenost)
#include "downsample_hdr_s_comp.h"

// Deklarace z nvsdk_ngx.h (ta hlavicka tahne D3D zavislosti).
extern "C" const wchar_t* NVSDK_CONV GetNGXResultAsString(NVSDK_NGX_Result InNGXResult);

// ---------------------------------------------------------------------------
// Log, chyby
// ---------------------------------------------------------------------------
static FILE* g_log;
static std::mutex g_logMx;  // NGX muze logovat z jinych vlaken

static void LogV(const char* fmt, va_list ap) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

// ---- Zaznam padu helperu (neosetrena vyjimka: v nasem kodu, NGX / DLSS-G, FidelityFX nebo ovladaci) ----
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevUef;
static volatile LONG g_crashBusy;
static void CrashModuleOf(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    if (a > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                          (LPCWSTR)a, &m) && m) {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* b = wcsrchr(path, L'\\');
        snprintf(out, n, "%ls+0x%llX", b ? b + 1 : path, (unsigned long long)(a - (uintptr_t)m));
    } else {
        snprintf(out, n, "%p", (void*)a);
    }
}
static LONG WINAPI CrashUef(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && ep->ContextRecord && !InterlockedExchange(&g_crashBusy, 1)) {
        static char line[1400];
        char where[200];
        CrashModuleOf((uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof where);
        int n = snprintf(line, sizeof line, "PAD: neosetrena vyjimka 0x%08lX na %s, vlakno %lu",
                         (unsigned long)ep->ExceptionRecord->ExceptionCode, where, GetCurrentThreadId());
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 && n > 0)
            n += snprintf(line + n, sizeof line - n, ", %s adresy %p",
                          er->ExceptionInformation[0] == 0 ? "cteni" : er->ExceptionInformation[0] == 8 ? "spusteni"
                                                                                                         : "zapis",
                          (void*)er->ExceptionInformation[1]);
        // adresy kodu na zasobniku (navratove adresy; muze obsahovat i stare hodnoty), nejvys 10
        const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
        const uintptr_t lo = (uintptr_t)tib->StackLimit, hi = (uintptr_t)tib->StackBase;
        uintptr_t sp = (uintptr_t)ep->ContextRecord->Rsp;
        if (n > 0 && n < (int)sizeof line - 32) n += snprintf(line + n, sizeof line - n, " | zasobnik:");
        int k = 0;
        if (sp >= lo && sp < hi)
            for (uintptr_t q = sp & ~(uintptr_t)7; q + 8 <= hi && q < sp + 8192 && k < 10; q += 8) {
                const uintptr_t v = *(const uintptr_t*)q;
                HMODULE m = nullptr;
                if (v < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                       (LPCWSTR)v, &m))
                    continue;
                MEMORY_BASIC_INFORMATION mi = {};
                if (!VirtualQuery((void*)v, &mi, sizeof mi) ||
                    !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                    continue;
                CrashModuleOf(v, where, sizeof where);
                if (n > 0 && n < (int)sizeof line - 220) n += snprintf(line + n, sizeof line - n, " %s", where);
                ++k;
            }
        // zamek logu muze drzet prave padajici vlakno: zkusit, jinak zapsat bez nej
        const bool locked = g_logMx.try_lock();
        if (g_log) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
            fflush(g_log);
        }
        if (locked) g_logMx.unlock();
    }
    return g_prevUef ? g_prevUef(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static BmsdShared* g_sh;

static void SetHelperMsg(const char* msg) {
    if (!g_sh) return;
    strncpy(g_sh->helperMsg, msg, sizeof g_sh->helperMsg - 1);
    g_sh->helperMsg[sizeof g_sh->helperMsg - 1] = 0;
}

// Fatalni chyba: proxy most vypne.
static void Error(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Log("CHYBA: %s", buf);
    if (g_sh) {
        SetHelperMsg(buf);
        MemoryBarrier();
        g_sh->helperError = 1;
    }
}

// DLAA nejde pouzit: rezim DLAA bude jen kopirovat, most bezi dal.
static bool g_dlaaBroken;
// NGX / DLSS na tomto systemu neni (InitNgx selhal: GPU bez DLSS, stary ovladac, chybi DLL). Pro menu proxy
// (BmsdShared::helperDlssBroken = 1 -> volba DLSS sede "jen NVIDIA RTX"; 2 = jine trvale selhani DLSS).
static bool g_ngxUnsupported;
static void DlaaFail(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Log("DLAA NEDOSTUPNE: %s - rezim DLAA bude jen kopirovat", buf);
    g_dlaaBroken = true;
    if (g_sh) {
        SetHelperMsg(buf);
        g_sh->helperDlssBroken = g_ngxUnsupported ? 1 : 2;
        MemoryBarrier();
        g_sh->dlaaStatus = -1;
    }
}

static std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; ++w) s.push_back(*w < 128 ? (char)*w : '?');
    return s;
}

static std::string NgxStr(NVSDK_NGX_Result r) {
    char buf[32];
    snprintf(buf, sizeof buf, "0x%08X ", (unsigned)r);
    return buf + Narrow(GetNGXResultAsString(r));
}

// ---------------------------------------------------------------------------
// Nastaveni z bms_dlss.ini (stejna slozka jako exe)
// ---------------------------------------------------------------------------
static std::wstring g_dir;

// ---------------------------------------------------------------------------
// Verze skutecne nactene knihovny NGX pro menu ("DLSS 310.9.16", "DLSS-G 310.9.16"). NGX muze misto DLL vedle helperu
// nacist novejsi z NVIDIA App / OTA (ProgramData/NVIDIA/NGX/models/<slozka>/versions/.../files/*.bin), proto se
// hleda mezi moduly nactenymi v tomto procesu: jmeno souboru = dllName, nebo cesta obsahuje /models/<modelDir>/.
// Vic nalezu (NGX si DLL vedle helperu nacte kvuli verzi a pouzije novejsi) = nejvyssi verze. Nic nenalezeno = verze
// souboru vedle helperu. "" = nezjisteno. Volat az po vytvoreni feature (knihovna je urcite nactena).
// ---------------------------------------------------------------------------
static bool FileVersion(const std::wstring& path, uint64_t* ver) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return false;
    std::vector<uint8_t> buf(size);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data()) ||
        !VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi || len < sizeof *fi)
        return false;
    *ver = ((uint64_t)fi->dwFileVersionMS << 32) | fi->dwFileVersionLS;
    return true;
}

static std::string VersionStr(uint64_t v) {
    const unsigned a = (unsigned)(v >> 48), b = (unsigned)(v >> 32) & 0xFFFF, c = (unsigned)(v >> 16) & 0xFFFF,
                   d = (unsigned)v & 0xFFFF;
    char buf[48];
    if (d) snprintf(buf, sizeof buf, "%u.%u.%u.%u", a, b, c, d);
    else snprintf(buf, sizeof buf, "%u.%u.%u", a, b, c);
    return buf;
}

static std::string LoadedNgxVersion(const wchar_t* dllName, const wchar_t* modelDir, const char* what) {
    uint64_t best = 0;
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 8 && snap == INVALID_HANDLE_VALUE; ++i) {  // ERROR_BAD_LENGTH = seznam modulu se prave meni
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snap != INVALID_HANDLE_VALUE) {
        std::wstring pat = std::wstring(L"\\models\\") + modelDir + L"\\";
        MODULEENTRY32W me = {sizeof me};
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
            std::wstring low = me.szExePath;
            for (wchar_t& c : low) c = (wchar_t)towlower(c);
            if (_wcsicmp(me.szModule, dllName) != 0 && low.find(pat) == std::wstring::npos) continue;
            uint64_t v = 0;
            const bool have = FileVersion(me.szExePath, &v);
            Log("%s: nacteny modul %s (verze %s)", what, Narrow(me.szExePath).c_str(),
                have ? VersionStr(v).c_str() : "nezjistena");
            if (have && v > best) best = v;
        }
        CloseHandle(snap);
    }
    if (!best && FileVersion(g_dir + L"\\" + dllName, &best))
        Log("%s: modul v procesu nenalezen - verze souboru %s vedle helperu: %s", what, Narrow(dllName).c_str(),
            VersionStr(best).c_str());
    return best ? VersionStr(best) : std::string();
}

static struct {
    float jitterSignX = 1.0f, jitterSignY = 1.0f;  // znamenko jitteru predavaneho DLSS
    float mvScaleX = 1.0f, mvScaleY = 1.0f;        // InMVScale pro DLSS
    float mvDebugRange = 16.0f;                    // ladeni MV: pixely pohybu pro plnou barvu
    float viewmodelDepth = 0.1f;                   // hloubka <= tato = zbran v ruce (MV 0); 0 = vypnuto
    int vmCheck = 0;                               // [dlaa] ViewmodelCheck: mereni MV zbrane (pokryti, fotometrie)
    float cutDistance = 128.0f;                    // strih kamery: posun oka za snimek (jednotky Source)
    float cutAngle = 45.0f;                        // strih kamery: otoceni smeru pohledu za snimek (stupne)
    // Preset ([dlaa] Preset, Ctrl+F7) cte proxy a predava ho ve sdilene pameti (BmsdShared::preset).
    int ngxLog = 0;                                // 0 vypnuto, 1 zapnuto, 2 podrobne
    float itmStrength = 0.95f;                     // InputMode 3: sila inverzniho tone mappingu k (0..0.99)
    // [upscale] (OutputScale cte proxy a predava ho ve sdilene pameti, BmsdShared::outScale)
    // Filtr zmenseni (BMSD_DOWN_*): 0 Lanczos-2, 1 Catmull-Rom, 2 Mitchell (B = C = 1/3), 3 box. Od v8 ho proxy
    // posila kazdy snimek (z ini, za behu Alt+F1); tady jen do logu a jako zaloha.
    uint32_t downsampleFilter = 1;
    bool downsampleLinear = false;                 // filtrovat v linearnim svetle misto hodnot sRGB
    // FSR ([upscale] Upscaler cte proxy a predava ho ve sdilene pameti, BmsdShared::upscaler)
    std::string fsrVersion;                        // [upscale] FsrVersion: cast nazvu verze ("3.1"); "" = vybere ffx API
    bool fsrSignatureCheck = true;                 // [upscale] FsrSignatureCheck: DLL AMD jen s platnym podpisem AMD
    bool fsrDebug = false;                         // [upscale] FsrDebug: FFX_UPSCALE_ENABLE_DEBUG_CHECKING (hlaseni do logu)
    bool fsrLinear = false;                        // [upscale] FsrInput: 0 = LDR sRGB (vychozi), 1 = linearni (pokus)
    bool fsrNonLinear = true;                      // [upscale] FsrNonLinear: LDR vstup FSR oznacit jako sRGB (S4)
    // Maska reaktivity (od verze 14; zapina ji proxy - [upscale] ReactiveMask, helper ji dostava jen pri maskDraws > 0)
    float reactiveScale = 1.0f;                    // [upscale] ReactiveScale: nasobek masky (max(RGB, A) * scale)
    float reactiveMax = 0.9f;                      // [upscale] ReactiveMax: strop hodnoty masky
    bool reactiveFsr = true;                       // [upscale] ReactiveFsr: maska do FSR (reactive)
    bool reactiveTC = false;                       // [upscale] ReactiveTC: tataz maska i jako transparencyAndComposition
    bool reactiveDlss = false;                     // [upscale] ReactiveDlss: maska do DLSS (Bias Current Color Mask)
    int reactiveDump = 0;                          // [upscale] ReactiveDump (ladeni): ulozit tolik trojic vstup / vystup / maska
    int reactiveDumpAfter = 300;                   // ... az po tolika snimcich s maskou od proxy (-1 = po souboru rmdump.go)
    int reactiveDumpEvery = 1;                     // ... kazdy N-ty takovy snimek
    bool autoPresetK = true;                       // [dlaa] AutoPresetK: Preset=0 = K pro vsechny rezimy (SDK by dal M / L)
} g_cfg;

static const char* FilterName(uint32_t f) {
    static const char* names[BMSD_DOWN_COUNT] = {"Lanczos-2", "Catmull-Rom", "Mitchell", "box"};
    return f < BMSD_DOWN_COUNT ? names[f] : "?";
}

static std::wstring IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, def, buf, 64, (g_dir + L"\\bms_dlss.ini").c_str());
    return buf;
}

// Desetinne cislo z [dlaa]: tecka i carka ("0,9"; _wtof by carku tise usekl), mezery nevadi - stejne jako proxy.
// Prazdne = def; neni cislo -> WARN a def; znaky za cislem -> WARN (cislo plati).
static float IniFloat(const wchar_t* key, float def, const wchar_t* sec = L"dlaa") {
    const std::wstring raw = IniStr(sec, key, L"");
    std::wstring t;
    for (wchar_t c : raw)
        if (!iswspace(c)) t.push_back(c == L',' ? L'.' : c);
    if (t.empty()) return def;
    wchar_t* end = nullptr;
    const double v = wcstod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(v)) {
        Log("WARN: [%s] %s=%s neni cislo - pouzivam %g", Narrow(sec).c_str(), Narrow(key).c_str(),
            Narrow(raw.c_str()).c_str(), def);
        return def;
    }
    if (*end)
        Log("WARN: [%s] %s=%s - znaky '%s' za cislem ignoruji (pouzivam %g)", Narrow(sec).c_str(),
            Narrow(key).c_str(), Narrow(raw.c_str()).c_str(), Narrow(end).c_str(), v);
    return (float)v;
}

static void LoadConfig() {
    g_cfg.jitterSignX = IniFloat(L"JitterSignX", 1.0f) < 0 ? -1.0f : 1.0f;
    g_cfg.jitterSignY = IniFloat(L"JitterSignY", 1.0f) < 0 ? -1.0f : 1.0f;
    g_cfg.mvScaleX = IniFloat(L"MVScaleX", 1.0f);
    g_cfg.mvScaleY = IniFloat(L"MVScaleY", 1.0f);
    if (g_cfg.mvScaleX == 0.0f) g_cfg.mvScaleX = 1.0f;  // DLSS: nikdy 0
    if (g_cfg.mvScaleY == 0.0f) g_cfg.mvScaleY = 1.0f;
    g_cfg.mvDebugRange = IniFloat(L"MVDebugRange", 16.0f);
    if (g_cfg.mvDebugRange <= 0.0f) g_cfg.mvDebugRange = 16.0f;
    // Zbran v ruce: hra ji kresli s viewportem MinZ = 0, MaxZ = 0.1 a hybe se s kamerou -> MV = 0
    g_cfg.viewmodelDepth = IniFloat(L"ViewmodelDepth", 0.1f);
    if (g_cfg.viewmodelDepth < 0.0f) g_cfg.viewmodelDepth = 0.0f;
    // Mereni MV zbrane v ruce (v25a): pokryti pixelu zbrane MV objektu a fotometricka chyba predchoziho snimku
    // posunuteho o MV (s MV objektu proti MV = 0). Jen diagnostika, 2 obrazy R32F velikosti vstupu.
    g_cfg.vmCheck = (int)wcstol(IniStr(L"dlaa", L"ViewmodelCheck", L"0").c_str(), nullptr, 0);
    // Strih kamery (teleport, kamera cutsceny, respawn): reset historie DLSS. 0 = kriterium vypnuto.
    // 128 jednotek za snimek je i pri 30 FPS vic nez padani maximalni rychlosti; 45 st za snimek je
    // rychlejsi nez bezne trhnuti mysi.
    g_cfg.cutDistance = IniFloat(L"CutDistance", 128.0f);
    g_cfg.cutAngle = IniFloat(L"CutAngle", 45.0f);
    g_cfg.ngxLog = (int)wcstol(IniStr(L"dlaa", L"NgxLog", L"0").c_str(), nullptr, 0);
    g_cfg.autoPresetK = wcstol(IniStr(L"dlaa", L"AutoPresetK", L"1").c_str(), nullptr, 0) != 0;
    // Inverzni tone mapping (InputMode 3): k < 1, jinak by rozsireni pro bilou slo do nekonecna
    g_cfg.itmStrength = std::min(0.99f, std::max(0.0f, IniFloat(L"ITMStrength", 0.95f)));
    Log("config [dlaa]: JitterSign=%+.0f,%+.0f MVScale=%g,%g ViewmodelDepth=%g CutDistance=%g CutAngle=%g "
        "MVDebugRange=%g NgxLog=%d ITMStrength=%.2f",
        g_cfg.jitterSignX, g_cfg.jitterSignY, g_cfg.mvScaleX, g_cfg.mvScaleY, g_cfg.viewmodelDepth,
        g_cfg.cutDistance, g_cfg.cutAngle, g_cfg.mvDebugRange, g_cfg.ngxLog, g_cfg.itmStrength);
    g_cfg.downsampleFilter = (uint32_t)wcstoul(IniStr(L"upscale", L"DownsampleFilter", L"1").c_str(), nullptr, 0);
    if (g_cfg.downsampleFilter >= BMSD_DOWN_COUNT) {
        Log("WARN: [upscale] DownsampleFilter=%u neni 0 az 3 - pouzivam 1 (Catmull-Rom)", g_cfg.downsampleFilter);
        g_cfg.downsampleFilter = 1;
    }
    g_cfg.downsampleLinear = wcstol(IniStr(L"upscale", L"DownsampleLinear", L"0").c_str(), nullptr, 0) != 0;
    Log("config [upscale]: DownsampleFilter=%u (%s) DownsampleLinear=%d (%s)", g_cfg.downsampleFilter,
        FilterName(g_cfg.downsampleFilter), (int)g_cfg.downsampleLinear,
        g_cfg.downsampleLinear ? "linearni svetlo" : "sRGB");
    g_cfg.fsrVersion = Narrow(IniStr(L"upscale", L"FsrVersion", L"").c_str());
    while (!g_cfg.fsrVersion.empty() && g_cfg.fsrVersion.back() == ' ') g_cfg.fsrVersion.pop_back();
    g_cfg.fsrSignatureCheck = wcstol(IniStr(L"upscale", L"FsrSignatureCheck", L"1").c_str(), nullptr, 0) != 0;
    g_cfg.fsrDebug = wcstol(IniStr(L"upscale", L"FsrDebug", L"0").c_str(), nullptr, 0) != 0;
    g_cfg.fsrLinear = wcstol(IniStr(L"upscale", L"FsrInput", L"0").c_str(), nullptr, 0) == 1;
    g_cfg.fsrNonLinear = wcstol(IniStr(L"upscale", L"FsrNonLinear", L"1").c_str(), nullptr, 0) != 0;
    Log("config [upscale] FSR: FsrVersion=%s FsrSignatureCheck=%d FsrDebug=%d FsrInput=%d (%s) FsrNonLinear=%d",
        g_cfg.fsrVersion.empty() ? "(vybere ffx API)" : g_cfg.fsrVersion.c_str(), (int)g_cfg.fsrSignatureCheck,
        (int)g_cfg.fsrDebug, (int)g_cfg.fsrLinear, g_cfg.fsrLinear ? "linearni" : "LDR sRGB",
        (int)g_cfg.fsrNonLinear);
    g_cfg.reactiveScale = std::max(0.0f, IniFloat(L"ReactiveScale", 1.0f, L"upscale"));
    g_cfg.reactiveMax = std::min(1.0f, std::max(0.0f, IniFloat(L"ReactiveMax", 0.9f, L"upscale")));
    g_cfg.reactiveFsr = wcstol(IniStr(L"upscale", L"ReactiveFsr", L"1").c_str(), nullptr, 0) != 0;
    g_cfg.reactiveTC = wcstol(IniStr(L"upscale", L"ReactiveTC", L"0").c_str(), nullptr, 0) != 0;
    g_cfg.reactiveDlss = wcstol(IniStr(L"upscale", L"ReactiveDlss", L"0").c_str(), nullptr, 0) != 0;
    g_cfg.reactiveDump = std::max(0, std::min(200, (int)wcstol(IniStr(L"upscale", L"ReactiveDump", L"0").c_str(), nullptr, 0)));
    g_cfg.reactiveDumpAfter = std::max(-1, (int)wcstol(IniStr(L"upscale", L"ReactiveDumpAfter", L"300").c_str(), nullptr, 0));
    g_cfg.reactiveDumpEvery = std::max(1, (int)wcstol(IniStr(L"upscale", L"ReactiveDumpEvery", L"1").c_str(), nullptr, 0));
    if (g_cfg.reactiveDump)
        Log("config [upscale] ladeni masky: ReactiveDump=%d ReactiveDumpAfter=%d ReactiveDumpEvery=%d (rmdump_NN_*.bmp)",
            g_cfg.reactiveDump, g_cfg.reactiveDumpAfter, g_cfg.reactiveDumpEvery);
    Log("config [upscale] maska reaktivity: ReactiveScale=%g ReactiveMax=%g ReactiveFsr=%d ReactiveTC=%d "
        "ReactiveDlss=%d (zapina proxy: ReactiveMask)",
        g_cfg.reactiveScale, g_cfg.reactiveMax, (int)g_cfg.reactiveFsr, (int)g_cfg.reactiveTC, (int)g_cfg.reactiveDlss);
}

// Upscaler (BMSD_UPSCALER_*), stejne nazvy jako v ini a menu
static const char* UpscalerName(uint32_t u) {
    static const char* names[BMSD_UPSCALER_COUNT] = {"vypnuto", "DLSS", "FSR", "XeSS", "auto"};
    return u < BMSD_UPSCALER_COUNT ? names[u] : "?";
}

// Vstup DLSS (BMSD_INPUT_*), stejne nazvy jako v logu proxy; u InputMode 3 i sila k
static const char* InputName(uint32_t m) {
    static const char* names[BMSD_INPUT_COUNT] = {"LDR", "linearni HDR", "linearni HDR + auto expozice", nullptr};
    static char itm[64];
    if (m == BMSD_INPUT_HDR_ITM) {
        snprintf(itm, sizeof itm, "linearni HDR + inverzni tone mapping (k=%.2f)", g_cfg.itmStrength);
        return itm;
    }
    return m < BMSD_INPUT_COUNT ? names[m] : "?";
}

// 0 = vychozi SDK (pro DLAA K), jinak pismeno presetu (K = 11, L = 12, M = 13)
static std::string PresetName(uint32_t p) {
    if (p == 0) return "vychozi";
    if (p <= 26) return std::string(1, (char)('A' + p - 1));
    return std::to_string(p);
}

// ---------------------------------------------------------------------------
// Vulkan
// ---------------------------------------------------------------------------
#define VK_INST_FUNCS(X)                                                                                    \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)                         \
    X(vkGetPhysicalDeviceFeatures2) X(vkEnumerateDeviceExtensionProperties)                                  \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceMemoryProperties2)

#define VK_DEV_FUNCS(X)                                                                                     \
    X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateSemaphore) X(vkImportSemaphoreWin32HandleKHR)             \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)         \
    X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkQueueSubmit)     \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkDeviceWaitIdle) X(vkCreateImageView)            \
    X(vkDestroyImageView) X(vkCreateSampler) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout)        \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateComputePipelines) X(vkCreateDescriptorPool)   \
    X(vkDestroyPipeline) X(vkDestroyDescriptorPool) X(vkDestroyPipelineLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets)    \
    X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdClearColorImage) X(vkCreateQueryPool)                    \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCreateBuffer)                \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory)

static struct {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkCreateInstance vkCreateInstance;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
#define DECL(n) PFN_##n n;
    VK_INST_FUNCS(DECL)
    VK_DEV_FUNCS(DECL)
#undef DECL
} vk;

static VkInstance g_inst;
static VkPhysicalDevice g_pd;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qf = UINT32_MAX;
static VkSemaphore g_semP, g_semH;
// Prezentace v hlavnim helperu a Frame Generation (od verze 12, BmsdShared::fgPresenter; src/helper/fg.inl). Bez
// fgPresenter se nic z toho nepouzije (zarizeni, rozsireni i fronty jsou presne jako ve verzi 11).
static bool g_fgPresenter;             // proxy chce prezentaci v tomto helperu
static VkQueue g_presQueue;            // fronta vlakna presentu; = g_queue, kdyz GPU druhou frontu nema
static std::mutex g_queueMx;           // g_queue: odeslani (hlavni vlakno; pri sdilene fronte i vlakno presentu)
static std::mutex g_presQOwnMx;
static std::mutex* g_presQMx = &g_presQOwnMx;  // zamek fronty presentu (sdilena fronta: &g_queueMx)
static void FgOnBridgeReimport();
static void FgSnapshot(VkCommandBuffer cmd, uint64_t B, bool camOk, bool reset);
struct OwnImg;
static void FgMvTarget(uint64_t B, OwnImg** img, VkDescriptorSet* set);
static bool BridgeMessage();           // jedna zprava mostu (definice pred wWinMain)
static HANDLE g_evBridge, g_evDone;    // udalosti mostu (proxy -> helper: zprava, helper -> proxy: prevzato)
static uint64_t g_framesDone;          // zpracovanych snimku mostu
static bool g_bridgeFatal;             // zprava mostu zpracovana z FgOnPresentMsg selhala: helper konci
// vkDeviceWaitIdle potrebuje vsechny fronty v klidu (zadne soubezne odeslani / present z vlakna presentu)
static void DeviceWaitIdleLocked();
static bool g_ptTimingExt;    // VK_EXT_present_timing + VK_KHR_present_id2 zapnuto ([fg] PresentTiming=1, jen mereni)
static bool g_ptCalibExt;     // VK_KHR_calibrated_timestamps zapnuto (prevod casu present_timing na QPC)
static bool g_ptCaps2Ext;     // VK_KHR_get_surface_capabilities2 na instanci (podpora present_timing povrchem)
static bool g_memBudget;      // VK_EXT_memory_budget zapnuto (log VRAM helperu po vytvoreni feature)
static float g_tsPeriod;      // ns na tik casovych razitek; 0 = fronta je neumi (bez mereni)

struct Img {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageAspectFlags aspect = 0;
    uint32_t w = 0, h = 0;  // velikost z importu (proxy dava nepouzite vstupy jen 1x1)
};
static Img g_img[BMSD_IMG_COUNT];
static uint32_t g_w, g_h;
// Sdileny vstup ma plnou velikost (proxy posila OBJMV_IN / MASK_IN 1x1, kdyz je funkce vypnuta; s daty v nich prijde
// vzdy plny - tohle je jen pojistka proti cteni mimo obraz)
static bool FullInput(int i) { return g_img[i].w >= g_w && g_img[i].h >= g_h; }
static int32_t g_gen;

const int kSlots = 3;
static VkCommandPool g_pool;
static VkCommandBuffer g_cmd[kSlots], g_initCmd;
static VkFence g_fence[kSlots], g_initFence;
static bool g_fenceUsed[kSlots];
static int g_slot;

// Mereni GPU casu (casova razitka, BOTTOM_OF_PIPE = "po vsem predchozim"): na slot az 7 dotazu (kTs*). Vysledky se
// ctou po cekani na fence slotu, prumer jde do logu kazdych ~5 s a pred zmenou feature. Od v24x (profil GPU): krome
// DLSS a zmenseni i predzpracovani (MV, linearizace, hloubka, maska), dozpracovani (doostreni, alfa), snapshot FG a
// zaverecna kopie; zapsane znacky v g_tsMask (useky mezi po sobe jdoucimi zapsanymi znackami).
enum : uint32_t { kTsStart, kTsPre, kTsDlss, kTsDown, kTsPost, kTsSnap, kTsEnd, kTsPerSlot };
static VkQueryPool g_qpool;
static uint32_t g_tsMask[kSlots];  // ktera razitka slot v poslednim cmd bufferu zapsal (bit = kTs*)
static double g_tsDlssMs, g_tsDownMs;
static unsigned g_tsN, g_tsNDown;
// Profil (v24x): soucty useku [0] predzpracovani, [1] DLSS/FSR, [2] zmenseni, [3] dozpracovani, [4] snapshot FG,
// [5] zaverecna kopie, [6] celkem (start -> konec); g_profN = pocet snimku s celkem
static double g_profMs[7], g_profMax;
static unsigned g_profN;
static LARGE_INTEGER g_tsLastLog;
// CPU cas helperu na snimek (do stejneho logu kazdych ~5 s): zpozdeni probuzeni (SetEvent v proxy -> probuzeni,
// z BmsdShared::postQpc) a zpracovani (probuzeni -> vkQueueSubmit hotov). V synchronnim rezimu proxy ceka soucet obou.
static double g_cpuWakeSum, g_cpuWakeMax, g_cpuProcSum, g_cpuProcMax;
static unsigned g_cpuWakeN, g_cpuProcN;

// Compute shader motion vektoru
static bool g_computeOk;
static VkSampler g_sampler;
static VkDescriptorSetLayout g_dsl;
// Set 1 (od v24y): jen vystup MV mv.comp. g_mvSet = g_mvImg; s FG pise mv.comp primo do obrazu MV snapshotu
// (FgSnap::mvSet) a snapshot pak MV nekopiruje (profil v24x: ~0,13 ms ve 4K). g_mvCur = obraz MV tohoto snimku.
static VkDescriptorSetLayout g_dslMv;
static VkDescriptorSet g_mvSet;
static VkPipelineLayout g_pipeLayout;
static VkPipeline g_pipe;       // mv.comp
static VkPipeline g_alphaPipe;  // alpha.comp
static VkPipeline g_sharpPipe;  // sharpen.comp (RCAS + alfa)
static VkPipeline g_linPipe;       // linearize.comp (vstup HDR)
static VkPipeline g_alphaHdrPipe;  // alpha.comp HDR_IN (vystup HDR -> sRGB + alfa)
static bool g_hdrFmtOk;            // GPU umi RGBA16F storage+sampled a R16F sampled (vstup HDR)
static VkPipeline g_depthPipe;     // depthmin.comp (hloubka pro DLSS, DepthMode 1 a 2)
static bool g_depthFmtOk;          // GPU umi R32F storage+sampled a pipeline existuje
static VkPipeline g_reactPipe;     // reactive.comp (maska reaktivity pro FSR / DLSS)
static bool g_reactFmtOk;          // GPU umi R8 storage+sampled a pipeline existuje
// downsample.comp (vystup DLSS velikosti vystupu -> W x H): [0] mala dlazdice (MAX_SRC=26, S <= kDownSmallMax),
// [1] velka (S <= 3). HDR_IN varianty (RGBA16F -> sRGB) jen s g_hdrFmtOk.
static VkPipeline g_downPipe[2];
static VkPipeline g_downHdrPipe[2];
const float kDownSmallMax = (26.0f - 2.0f) / 11.0f;  // nejvetsi pomer pro MAX_SRC=26 (11*S + 2 texelu dlazdice)
static bool g_upOk;                // supersampling jde (pipeline zmenseni existuje); jinak vzdy S = 1
static VkDescriptorPool g_descPool;
static VkDescriptorSet g_descSet;

// Vlastni obrazy helperu (podle rozliseni) a pohledy na sdilene vstupy
struct OwnImg {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
static OwnImg* g_mvCur;  // obraz MV tohoto snimku (g_mvImg nebo MV snapshotu FG, viz g_dslMv)
static OwnImg g_mvImg;   // motion vektory RG32F, velikost vstupu DLSS (budouci MV objektu: dalsi obraz teze velikosti)
static OwnImg g_outImg;  // finalni obraz W x H, RGBA8 storage: vystup DLSS pri S = 1 / zmenseni pri S > 1 / ladeni MV
static OwnImg g_sharpImg;  // vystup doostreni (RCAS), RGBA8 storage, W x H
// Vystup DLSS pri S > 1 a vstupu LDR: RGBA8 velikosti vystupu (binding 8), cte ho downsample.comp. Vznika v CreateDlss
// (EnsureBig), rusi se pri S = 1, vstupu HDR (tam je vystupem g_hdrOutImg), reimportu a konci.
static OwnImg g_bigImg;
static VkImageView g_colorInView, g_depthInView;
static VkImageView g_objInView;  // OBJMV_IN (RGBA32F, MV pohyblivych objektu z proxy), binding 9, layout GENERAL
static bool g_ownOk;
const VkFormat kMvFormat = VK_FORMAT_R32G32_SFLOAT;
const VkFormat kOutFormat = VK_FORMAT_R8G8B8A8_UNORM;
// Vstup DLSS v linearnim HDR (InputMode 1, 2, 3): vytvari se az pri prvnim pouziti (EnsureHdr), rusi se pri
// reimportu, navratu na LDR a konci (DestroyHdr).
static OwnImg g_linImg;     // vstup DLSS: COLOR_IN prevedeny do linearniho RGBA16F (binding 5), velikost vstupu
static OwnImg g_hdrOutImg;  // vystup DLSS v linearnim RGBA16F (binding 6), velikost vystupu DLSS
static OwnImg g_expImg;     // expozice 1x1 R16F = 1.0 (Guide 3.9), trvale v SHADER_READ_ONLY_OPTIMAL
static bool g_hdrImgOk;
const VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
const VkFormat kExpFormat = VK_FORMAT_R16_SFLOAT;  // Guide 3.9: cte se jen prvni kanal, doporuceno R16F
// Hloubka pro DLSS (DepthMode 1, 2): vytvari se v CreateSized (kdyz GPU umi R32F), rusi v DestroySized
// (reimport, zmena rozliseni, konec). Kazdy snimek z UNDEFINED -> GENERAL (zapis) -> SHADER_READ_ONLY (DLSS).
static OwnImg g_depthImg;  // R32F (binding 7)
static bool g_depthImgOk;
// Mereni MV zbrane (ViewmodelCheck): jas minuleho snimku (R32F, stridave 0 / 1; vazby 13 cteni, 14 zapis)
static OwnImg g_vmPrev[2];
static bool g_vmPrevOk, g_vmPrevPending, g_vmPrevValid, g_vmPrevInit;
static uint32_t g_vmParity, g_vmPrevW, g_vmPrevH;
static float g_vmPrevJit[2];
static bool g_depthPending, g_reactPending;  // obraz se vytvori az pri prvnim pouziti (EnsureDepthImg / EnsureReactImg)
// Maska reaktivity (od verze 14): R8 velikosti vstupu (binding 12) z MASK_IN (binding 11, layout GENERAL). Vytvari se
// v CreateSized, rusi v DestroySized; kazdy snimek s maskou UNDEFINED -> GENERAL (reactive.comp) -> SHADER_READ_ONLY.
static OwnImg g_reactImg;
static bool g_reactImgOk;
static VkImageView g_maskInView;
const VkFormat kReactFormat = VK_FORMAT_R8_UNORM;
const VkFormat kDepthFormat = VK_FORMAT_R32_SFLOAT;
// Format sdileneho DEPTH_IN (hloubka sceny tak, jak ji vytvoril DXVK): D24_UNORM_S8_UINT na NVIDIA a Intelu,
// D32_SFLOAT_S8_UINT na AMD. Bere se z BmsdImage::format pri importu; vsechny pohledy, NGX, FSR a snapshot hloubky FG
// pouzivaji tento format (aspekt hloubky, shadery ctou 0 = near, 1 = far v obou).
static VkFormat g_depthFmt = VK_FORMAT_D24_UNORM_S8_UINT;

static bool IsBridgeDepthFormat(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// NGX
static const char kProjectId[] = "6b3f0d2a-9c41-4e8b-a7d5-2f1e8c6b0a93";  // vlastni GUID projektu bms-dlss
static const char kEngineVersion[] = "1.0";
static NVSDK_NGX_FeatureCommonInfo g_ngxCommon;
static const wchar_t* g_ngxPaths[1];
static bool g_ngxInit;                   // NVSDK_NGX_VULKAN_Init uspelo
static NVSDK_NGX_Parameter* g_ngxParams;
static NVSDK_NGX_Parameter* g_ngxCaps;   // parametry z GetCapabilityParameters (callback pro NGX_DLSS_GET_OPTIMAL_SETTINGS)
static NVSDK_NGX_Handle* g_dlss;
// Supersampling: pozadavek proxy (BmsdShared::outScale, naposledy zpracovany - potvrzeni) a skutecny stav feature
static float g_outScaleReq = 1.0f;       // pozadavek tak, jak prisel (potvrzuje se presne tato hodnota)
static float g_outScale = 1.0f;          // S, se kterym feature bezi (1 = DLAA nebo fallback)
static uint32_t g_inW, g_inH;            // vlastni vstupni obrazy (MV, linearni barva, hloubka R32F) = g_w x g_h
static uint32_t g_rW, g_rH;              // render = skutecny vstup DLSS: sub-rect od (0, 0); cast A = g_w x g_h
static uint32_t g_rReqW, g_rReqH;        // pozadavek proxy tak, jak prisel (potvrzuje se presne tato hodnota)
static bool g_subFailPending;            // cast B: vyhodnoceni selhalo, po snimku feature uvolnit (helperRenderOk = 0)
static bool g_subScaleLogged;            // cast B: pozadovane S > 1 se nepouziva (zalogovano jednou)
static bool g_blitLinearOk;              // BGRA8 jde blitovat s LINEAR (roztazeni sub-rectu bez DLSS v casti B)
static uint32_t g_outW, g_outH;          // vystup DLSS; = g_w x g_h pri S = 1 (cast A i B)
static int32_t g_perfQuality = -1;       // NVSDK_NGX_PerfQuality_Value feature, -1 = zadna
static bool g_upFailPending;             // vyhodnoceni pri S > 1 selhalo: po snimku spadnout na S = 1
static uint32_t g_dlssPreset;            // preset, se kterym byla vytvorena (nebo naposledy pozadovana) feature
static uint32_t g_dlssInputMode;         // inputMode od proxy, naposledy zpracovany (potvrzeni pro proxy)
static uint32_t g_hdrMode;               // vstup, se kterym feature skutecne bezi (BMSD_INPUT_*; LDR pri fallbacku)
static bool g_dlssFresh;                 // prvni vyhodnoceni po vytvoreni -> reset
static uint32_t g_lastMode = BMSD_MODE_OFF;
static LARGE_INTEGER g_lastEvalTime, g_qpf;
static unsigned g_dlaaFrames;
static unsigned g_cuts;                  // pocet detekovanych strihu kamery
static float g_loggedSharpness = -1.0f, g_loggedSignX, g_loggedSignY;  // naposledy zalogovane hodnoty
static uint32_t g_loggedDepthMode = UINT32_MAX;  // naposledy zalogovany depthMode od proxy
static int g_loggedMask;  // naposledy zalogovany stav masky reaktivity (0 = zatim nic)
static uint32_t g_lastDepthMode;                 // hloubka DLSS v minulem snimku (zmena -> reset historie)
static uint32_t g_loggedDownFilter = UINT32_MAX; // naposledy zalogovany filtr zmenseni od proxy (Alt+F1)
static uint32_t g_lastPhases, g_lastPhasesAuto;  // fazi jitteru v minulem snimku (zmena -> log a reset historie)
static bool g_fatal;                             // GPU nedobehla / ztracena: helper konci (proxy most vypne)
// Volba upscaleru (od verze 11, BmsdShared::upscaler). Pozadavek se potvrzuje presne tak, jak prisel (g_upReqRaw);
// bez upscalerValid (proxy volbu nezna) plati DLSS. g_upActive = co skutecne bezi (DLSS feature / FSR kontext).
static uint32_t g_upReqRaw, g_upReqValid;        // posledni zpracovany pozadavek proxy (upscaler, upscalerValid)
static uint32_t g_upActive = BMSD_UPSCALER_OFF;  // upscaler, se kterym bezi feature / kontext
static uint32_t g_loggedUpReq = UINT32_MAX;      // naposledy zalogovany pozadavek (log jen pri zmene)
// FSR (AMD ffx API, podepsane DLL z FidelityFX SDK 2.4 vedle helperu). DLL se nacitaji az pri prvnim pozadavku na
// FSR (LoadFfx); pri chybe (chybi DLL, podpis, zadna verze) je FSR trvale nedostupne a helper pouzije DLSS / kopii.
static HMODULE g_ffxDll;
static PfnFfxCreateContext g_ffxCreate;
static PfnFfxDestroyContext g_ffxDestroy;
static PfnFfxQuery g_ffxQuery;
static PfnFfxDispatch g_ffxDispatch;
static PfnFfxConfigure g_ffxConfigure;   // jen frame generation (fg.inl); nullptr = loader ji nema
static bool g_ffxTried;                  // nacteni ffx API pro upscaler uz probehlo (uspech = g_ffxUpOk)
static bool g_ffxUpOk;                   // loader nacten a ffx API nabizi aspon jednu verzi upscaleru
static bool g_fsrBroken;                 // FSR trvale nedostupne (duvod v logu)
static bool g_fsrFailPending;            // vyhodnoceni FSR selhalo (S = 1, cast A): po snimku uvolnit, prejit na DLSS
// Selhani vyhodnoceni FSR neni hned trvale (S5): FSR se vynecha jen do dalsi zmeny od proxy (upscaler / preset /
// vstup / S / render, RecreateDlss); trvale (g_fsrBroken) az po kFsrMaxEvalFails selhanich bez uspesneho snimku mezi.
static const int kFsrMaxEvalFails = 3;
static int g_fsrEvalFails;               // selhani vyhodnoceni FSR za sebou (uspesny snimek FSR nuluje)
static bool g_fsrSuspended;              // FSR docasne vynechano (do dalsi zmeny od proxy)
static bool g_fsrNonLinFlags;            // kontext FSR ma vstup oznaceny jako sRGB (NON_LINEAR_COLORSPACE, S4)
static bool g_dlssFailPending;           // vyhodnoceni DLSS selhalo (S = 1, cast A): po snimku uvolnit, zkusit FSR (D1)
static ffxContext g_fsr;                 // FSR kontext (nullptr = zadny)
static uint64_t g_fsrVersionId;          // ffxOverrideVersion ([upscale] FsrVersion), 0 = vyber ffx API
static uint64_t g_fsr31Id;               // nejnovejsi verze "3.1.x" z nabidky ffx API (zaloha, kdyz vychozi nejde)
static std::string g_fsr31Name;
static std::string g_fsrVersionName;     // verze, kterou ffx API pro kontext vybralo (log, helperUpscalerName)
static std::string g_dlssVersion;        // verze skutecne nactene nvngx_dlss ("310.9.16"; "" = zatim nezjistena)
static bool g_dlssVersionTried;
static float g_fsrLoggedNear = -1.0f, g_fsrLoggedFar, g_fsrLoggedFov;  // parametry kamery pro FSR (log pri zmene)
// MV pohyblivych objektu (od verze 9): pocitadla mv.comp v mapovanem bufferu (binding 10), na slot kObjStatPerSlot
// hodnot uint = 6 poli po 64 kosich (prijato, odmitnuto hloubkou, validace pocet / soucet / maximum v 1/256 px,
// validace odmitnuto hloubkou). Ctou se se zpozdenim, az dobehne fence slotu (CollectObjStats; v prikazech je bariera
// SHADER_WRITE -> HOST_READ); prumery jdou do logu kazdych ~5 s, do sdilene pameti vse krome odmitnutych statickych.
const uint32_t kObjStatBuckets = 64;
// + (v25a) zbran v ruce: pixelu, s MV objektu, soucet |MV| (1/256 px), fotometricka chyba s MV / s MV = 0 (1/1024)
const uint32_t kObjStatPerSlot = 11 * kObjStatBuckets;
static VkBuffer g_objStatBuf;
static VkDeviceMemory g_objStatMem;
static uint32_t* g_objStatPtr;
static bool g_objSlotUsed[kSlots];               // slot ma v pocitadlech data snimku s MV objektu
static uint64_t g_objAcc, g_objRej, g_objValN, g_objValSum, g_objValRej;
static uint32_t g_objValMax, g_objFrames;
static uint64_t g_vmPx, g_vmAcc, g_vmMv, g_vmE1, g_vmE0;
static float g_objTolLogged;
static uint32_t g_loggedObjMode = UINT32_MAX;    // naposledy zalogovany objMvMode od proxy
// Limit cekani na vlastni GPU praci: snimek trva milisekundy. Pri ztracenem zarizeni hry by semafor od proxy nemusel
// prijit nikdy - helper pak skonci s chybou misto nekonecneho cekani (proxy most vypne).
static const uint64_t kGpuTimeoutNs = 5000000000ull;

// Hloubka DLSS (BMSD_DEPTH_*), stejne nazvy jako v logu proxy
static const char* DepthName(uint32_t m) {
    static const char* names[BMSD_DEPTH_COUNT] = {"puvodni", "min 3x3", "konstantni (test)"};
    return m < BMSD_DEPTH_COUNT ? names[m] : "?";
}

// Rezim DLSS (NVSDK_NGX_PerfQuality_Value), stejne nazvy jako v logu proxy
static const char* PerfName(int32_t v) {
    static const char* names[] = {"Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA"};
    return v >= 0 && v < 6 ? names[v] : "?";
}

// Vychozi preset SDK 310 podle rezimu (nvsdk_ngx_defs.h): K pro DLAA/Quality/Balanced, M Performance, L Ultra Perf
static const char* DefaultPresetFor(int32_t v) {
    switch (v) {
    case NVSDK_NGX_PerfQuality_Value_MaxPerf: return "M";
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return "L";
    case NVSDK_NGX_PerfQuality_Value_UltraQuality: return "?";
    default: return "K";
    }
}

// S do logu: 1.0, 1.33, 1.5, 1.72, 2.0, 3.0
static std::string ScaleStr(float s) {
    char b[32];
    snprintf(b, sizeof b, "%.2f", s);
    std::string r = b;
    while (r.size() > 3 && r.back() == '0') r.pop_back();
    return r;
}

// Pozadavek proxy na S: 1 .. 3 (vic DLSS ani nase zmenseni s kMaxTaps neumi), jinak 1
static float SanitizeScale(float s) { return s >= 1.0001f && s <= 3.0001f ? s : 1.0f; }

static void NVSDK_CONV NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string m = message ? message : "";
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    Log("NGX: %s", m.c_str());
}

static NVSDK_NGX_FeatureDiscoveryInfo NgxDiscoveryInfo() {
    NVSDK_NGX_FeatureDiscoveryInfo di = {};
    di.SDKVersion = NVSDK_NGX_Version_API;
    di.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    di.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    di.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    di.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    di.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
    di.ApplicationDataPath = g_dir.c_str();
    di.FeatureInfo = &g_ngxCommon;
    return di;
}

static void SetupNgxCommon() {
    g_ngxPaths[0] = g_dir.c_str();  // nvngx_dlss.dll lezi vedle helperu
    g_ngxCommon = {};
    g_ngxCommon.PathListInfo.Path = g_ngxPaths;
    g_ngxCommon.PathListInfo.Length = 1;
    g_ngxCommon.LoggingInfo.LoggingCallback = NgxLogCallback;
    g_ngxCommon.LoggingInfo.MinimumLoggingLevel = g_cfg.ngxLog >= 2   ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE
                                                  : g_cfg.ngxLog == 1 ? NVSDK_NGX_LOGGING_LEVEL_ON
                                                                      : NVSDK_NGX_LOGGING_LEVEL_OFF;
    g_ngxCommon.LoggingInfo.DisableOtherLoggingSinks = false;
}

static void AddName(std::vector<std::string>& list, const char* name) {
    for (const std::string& s : list)
        if (s == name) return;
    list.push_back(name);
}

// Ze seznamu pozadovanych rozsireni vybere dostupna, chybejici zaloguje.
static std::vector<const char*> FilterExts(const std::vector<std::string>& want,
                                           const std::vector<VkExtensionProperties>& avail, const char* what) {
    std::vector<const char*> out;
    for (const std::string& w : want) {
        bool ok = false;
        for (const VkExtensionProperties& a : avail) ok = ok || w == a.extensionName;
        if (ok)
            out.push_back(w.c_str());
        else
            Log("WARN: %s rozsireni %s neni dostupne", what, w.c_str());
    }
    return out;
}

static bool InitVulkan() {
    HMODULE dll = LoadLibraryW(L"vulkan-1.dll");
    if (!dll) return Error("vulkan-1.dll (64bit) nenalezeno"), false;
    vk.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(dll, "vkGetInstanceProcAddr");
    vk.vkCreateInstance = (PFN_vkCreateInstance)vk.vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
    vk.vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)vk.vkGetInstanceProcAddr(
        nullptr, "vkEnumerateInstanceExtensionProperties");
    if (!vk.vkCreateInstance || !vk.vkEnumerateInstanceExtensionProperties)
        return Error("vulkan-1.dll bez zakladnich funkci"), false;

    // Rozsireni instance, ktera chce NGX (dotaz jde i pred NVSDK_NGX_VULKAN_Init)
    NVSDK_NGX_FeatureDiscoveryInfo di = NgxDiscoveryInfo();
    std::vector<std::string> instWant;
    {
        uint32_t n = 0;
        VkExtensionProperties* props = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&di, &n, &props);
        if (NVSDK_NGX_FAILED(r))
            Log("WARN: NGX nevratilo rozsireni instance: %s", NgxStr(r).c_str());
        for (uint32_t i = 0; props && i < n; ++i) AddName(instWant, props[i].extensionName);
        std::string list;
        for (const std::string& s : instWant) list += " " + s;
        Log("NGX chce rozsireni instance:%s", list.empty() ? " (zadna)" : list.c_str());
    }
    NVSDK_NGX_FeatureDiscoveryInfo diFg = di;  // Frame Generation (jen s fgPresenter)
    diFg.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    if (g_fgPresenter) {
        // Prezentace v helperu: surface + swapchain; rozsireni, ktera chce NGX pro DLSS-G
        AddName(instWant, VK_KHR_SURFACE_EXTENSION_NAME);
        AddName(instWant, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        uint32_t m = 0;
        VkExtensionProperties* props = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&diFg, &m, &props);
        if (NVSDK_NGX_FAILED(r)) Log("WARN: NGX nevratilo rozsireni instance pro DLSS-G: %s", NgxStr(r).c_str());
        for (uint32_t i = 0; props && i < m; ++i) AddName(instWant, props[i].extensionName);
    }
    uint32_t n = 0;
    vk.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> instAvail(n);
    vk.vkEnumerateInstanceExtensionProperties(nullptr, &n, instAvail.data());
    instAvail.resize(n);
    if (g_fgPresenter && wcstol(IniStr(L"fg", L"PresentTiming", L"0").c_str(), nullptr, 0) != 0)
        AddName(instWant, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);  // [fg] PresentTiming=1 (jen mereni)
    std::vector<const char*> instExts = FilterExts(instWant, instAvail, "instance");
    for (const char* e : instExts) g_ptCaps2Ext = g_ptCaps2Ext || !strcmp(e, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "bms_dlss_helper", 1, "bms-dlss", 1,
                             VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    ici.enabledExtensionCount = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();
    VkResult r = vk.vkCreateInstance(&ici, nullptr, &g_inst);
    if (r != VK_SUCCESS) return Error("vkCreateInstance selhal %d", r), false;
#define LOADI(n)                                                   \
    vk.n = (PFN_##n)vk.vkGetInstanceProcAddr(g_inst, #n);          \
    if (!vk.n) return Error("chybi funkce %s", #n), false;
    VK_INST_FUNCS(LOADI)
#undef LOADI

    uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(g_inst, &count, nullptr);
    VkPhysicalDevice pds[16];
    count = count > 16 ? 16 : count;
    vk.vkEnumeratePhysicalDevices(g_inst, &count, pds);
    uint32_t apiVersion = 0;
    // Vsechna GPU do logu (hybridni notebooky: DXVK a tedy i helper muze bezet na integrovane grafice)
    static const char* const kType[] = {"jine", "integrovana", "diskretni", "virtualni", "CPU"};
    VkPhysicalDeviceType myType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint32_t myVendor = 0;
    char myName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "";
    bool otherDiscrete = false, otherNvidia = false;
    char otherName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "";
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
        vk.vkGetPhysicalDeviceProperties2(pds[i], &p2);
        const VkPhysicalDeviceProperties& pp = p2.properties;
        const bool match = !g_pd && !memcmp(idp.deviceUUID, g_sh->deviceUUID, 16);
        const unsigned char* u = idp.deviceUUID;
        const unsigned char* l = idp.deviceLUID;
        Log("GPU %u: %s (%s, vendor 0x%04X), UUID %02x%02x%02x%02x..., LUID %s%02x%02x%02x%02x%02x%02x%02x%02x%s", i,
            pp.deviceName, pp.deviceType <= 4 ? kType[pp.deviceType] : "?", pp.vendorID, u[0], u[1], u[2], u[3],
            idp.deviceLUIDValid ? "" : "(neplatne) ", l[7], l[6], l[5], l[4], l[3], l[2], l[1], l[0],
            match ? " <- DXVK" : "");
        if (match) {
            g_pd = pds[i];
            apiVersion = pp.apiVersion;
            myType = pp.deviceType;
            myVendor = pp.vendorID;
            snprintf(myName, sizeof myName, "%s", pp.deviceName);
            Log("GPU: %s (shoda UUID s DXVK), Vulkan %u.%u, ovladac 0x%X", pp.deviceName,
                VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), pp.driverVersion);
        } else if (memcmp(idp.deviceUUID, g_sh->deviceUUID, 16)) {
            if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || pp.vendorID == 0x10DE) {
                if (!otherName[0] || pp.vendorID == 0x10DE) snprintf(otherName, sizeof otherName, "%s", pp.deviceName);
            }
            otherDiscrete = otherDiscrete || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            otherNvidia = otherNvidia || pp.vendorID == 0x10DE;
        }
    }
    if (!g_pd) return Error("GPU se shodnym UUID jako DXVK nenalezena (seznam GPU vyse)"), false;
    // Hra (DXVK) bezi na integrovane grafice nebo na GPU bez DLSS, prestoze je v systemu vykonnejsi / NVIDIA karta:
    // vysvetleni do logu, helperMsg a menu (nastaveni Windows: Grafika -> bms.exe -> Vysoky vykon)
    int gpuHint = 0;
    if (myType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && otherDiscrete) gpuHint = 1;
    else if (myVendor != 0x10DE && otherNvidia) gpuHint = 2;
    {  // test hlasky bez hybridniho notebooku: BMS_DLSS_TEST_GPUHINT=1|2
        wchar_t t[8] = L"";
        if (GetEnvironmentVariableW(L"BMS_DLSS_TEST_GPUHINT", t, 8) && (t[0] == L'1' || t[0] == L'2')) {
            gpuHint = t[0] - L'0';
            if (!otherName[0]) snprintf(otherName, sizeof otherName, "%s", "(test)");
            Log("test: BMS_DLSS_TEST_GPUHINT=%d", gpuHint);
        }
    }
    g_sh->helperGpuHint = gpuHint;
    if (gpuHint) {
        Log("POZOR: hra bezi na %s (%s), v systemu je i %s. DLSS%s jen na vykonne karte: ve Windows Nastaveni -> "
            "System -> Obrazovka -> Grafika nastav bms.exe na Vysoky vykon (pripadne v ovladaci NVIDIA / AMD) a "
            "hru restartuj.",
            myName, gpuHint == 1 ? "integrovana grafika" : "GPU bez DLSS", otherName,
            gpuHint == 1 ? ", DLSS-G a rychle FSR" : " a DLSS-G");
        snprintf(g_sh->helperMsg, sizeof g_sh->helperMsg,
                 "hra bezi na %s (%s) - nastav bms.exe na vykonnou GPU (Windows: Nastaveni -> Grafika)", myName,
                 gpuHint == 1 ? "integrovana grafika" : "bez DLSS");
    }

    uint32_t qfCount = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qfCount, nullptr);
    VkQueueFamilyProperties qfp[32];
    qfCount = qfCount > 32 ? 32 : qfCount;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qfCount, qfp);
    for (uint32_t i = 0; i < qfCount && g_qf == UINT32_MAX; ++i)
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) g_qf = i;  // graphics family umi i compute
    if (g_qf != UINT32_MAX && qfp[g_qf].timestampValidBits) {
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        vk.vkGetPhysicalDeviceProperties2(g_pd, &p2);
        g_tsPeriod = p2.properties.limits.timestampPeriod;
    }

    // Rozsireni zarizeni: nase (sdileni s DXVK) + ta, ktera chce NGX
    // VK_EXT_memory_budget jen pro log VRAM helperu (nepovinne)
    std::vector<std::string> devWant = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
                                        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};
    {
        uint32_t m = 0;
        VkExtensionProperties* props = nullptr;
        NVSDK_NGX_Result nr = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(g_inst, g_pd, &di, &m, &props);
        if (NVSDK_NGX_FAILED(nr))
            Log("WARN: NGX nevratilo rozsireni zarizeni: %s", NgxStr(nr).c_str());
        std::string list;
        for (uint32_t i = 0; props && i < m; ++i) {
            AddName(devWant, props[i].extensionName);
            list += std::string(" ") + props[i].extensionName;
        }
        Log("NGX chce rozsireni zarizeni:%s", list.empty() ? " (zadna)" : list.c_str());
    }
    const bool wantPt = g_fgPresenter && wcstol(IniStr(L"fg", L"PresentTiming", L"0").c_str(), nullptr, 0) != 0;
    if (g_fgPresenter) {
        AddName(devWant, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        uint32_t m = 0;
        VkExtensionProperties* props = nullptr;
        NVSDK_NGX_Result nr = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(g_inst, g_pd, &diFg, &m, &props);
        if (NVSDK_NGX_FAILED(nr)) Log("WARN: NGX nevratilo rozsireni zarizeni pro DLSS-G: %s", NgxStr(nr).c_str());
        std::string list;
        for (uint32_t i = 0; props && i < m; ++i) {
            AddName(devWant, props[i].extensionName);
            list += std::string(" ") + props[i].extensionName;
        }
        Log("NGX chce rozsireni zarizeni pro DLSS-G:%s", list.empty() ? " (zadna)" : list.c_str());
        // FSR Frame Generation: 3 z 18 shaderu deklaruji SPIR-V ComputeDerivativeGroupLinearKHR (spike
        // tools/spike_ffg: NVIDIA to nevynucuje, vzorovy framework AMD rozsireni zapina). Zapnout, kdyz ho GPU ma.
        AddName(devWant, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
        // [fg] PresentTiming=1 (jen mereni skutecneho zobrazeni, vychozi vyp): VK_EXT_present_timing + present_id2
        if (wantPt) {
            AddName(devWant, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
            AddName(devWant, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
            AddName(devWant, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        }
    }
    // [fg] HelperQueuePriority (ladeni, vychozi 0): globalni priorita front helperu 1 = HIGH, 2 = REALTIME (kopie pro
    // zobrazeni v procesu hry nema na GPU cekat za renderem hry)
    const int qPrio = (int)wcstol(IniStr(L"fg", L"HelperQueuePriority", L"0").c_str(), nullptr, 0);
    if (qPrio > 0) AddName(devWant, "VK_KHR_global_priority");
    n = 0;
    vk.vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> devAvail(n);
    vk.vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &n, devAvail.data());
    devAvail.resize(n);
    std::vector<const char*> devExts = FilterExts(devWant, devAvail, "zarizeni");
    for (const char* e : devExts) g_memBudget = g_memBudget || !strcmp(e, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    // Vlastnosti: zapneme vse, co GPU umi (NGX a shader s rg32f storage obrazem), krome robustnosti
    // (ta jen zpomaluje).
    const bool has13 = apiVersion >= VK_API_VERSION_1_3;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            has13 ? &f13 : nullptr};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vk.vkGetPhysicalDeviceFeatures2(g_pd, &f2);
    if (!f12.timelineSemaphore) return Error("GPU neumi timeline semafory"), false;
    // [fg] HelperRobust=1 (ladeni, vychozi 0): robustnost bufferu a obrazu podle GPU nechat zapnutou - hledani pristupu
    // mimo rozsah (TDR "MMU Fault" pri startu s FG, docs/kontrolni-seznam.md)
    if (wcstol(IniStr(L"fg", L"HelperRobust", L"0").c_str(), nullptr, 0) != 0) {
        Log("ladeni: robustnost zarizeni helperu zapnuta (robustBufferAccess %u, robustImageAccess %u)",
            f2.features.robustBufferAccess, f13.robustImageAccess);
    } else {
        f2.features.robustBufferAccess = VK_FALSE;
        f13.robustImageAccess = VK_FALSE;
    }
    // VK_EXT_buffer_device_address se nesmi kombinovat s jadrovym bufferDeviceAddress (Vulkan 1.2):
    // kdyby ho NGX chtelo, zapne se jeho vlastni struktura misto jadrove.
    VkPhysicalDeviceBufferDeviceAddressFeaturesEXT bdaExt = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT};
    // VK_KHR_compute_shader_derivatives (jen s fgPresenter, viz vyse): vlastnosti podle GPU
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR csd = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    for (const char* e : devExts)
        if (!strcmp(e, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) {
            VkPhysicalDeviceFeatures2 q2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &csd};
            vk.vkGetPhysicalDeviceFeatures2(g_pd, &q2);
            csd.pNext = nullptr;
            (has13 ? (VkBaseOutStructure*)&f13 : (VkBaseOutStructure*)&f12)->pNext = (VkBaseOutStructure*)&csd;
            Log("VK_KHR_compute_shader_derivatives: computeDerivativeGroupQuads=%u, computeDerivativeGroupLinear=%u",
                csd.computeDerivativeGroupQuads, csd.computeDerivativeGroupLinear);
        }
    // FSR FG 2x (DLL AMD) ma 3 shadery s ComputeDerivativeGroupLinearKHR. Bez nej se nevypina (ovladace to casto
    // nevynucuji a vypnuti by FSR 2x vzalo i tam, kde funguje) - jen varovani pro diagnostiku; FSR adaptivni ho nepouziva.
    if (g_fgPresenter && !csd.computeDerivativeGroupLinear)
        Log("POZOR: GPU nema computeDerivativeGroupLinear (VK_KHR_compute_shader_derivatives) - FSR 2x muze selhat; "
            "pri potizich pouzij FSR adaptivni nebo FG vypni");
    void* chain = &f2;
    for (const char* e : devExts)
        if (!strcmp(e, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
            bdaExt.bufferDeviceAddress = f12.bufferDeviceAddress;
            f12.bufferDeviceAddress = f12.bufferDeviceAddressCaptureReplay = f12.bufferDeviceAddressMultiDevice =
                VK_FALSE;
            bdaExt.pNext = &f2;
            chain = &bdaExt;
        }
    // present_timing (jen s [fg] PresentTiming=1): obe rozsireni a vlastnosti presentTiming + presentId2
    VkPhysicalDevicePresentTimingFeaturesEXT ptf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
    VkPhysicalDevicePresentId2FeaturesKHR pid2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, &ptf};
    {
        bool hasPt = false, hasId2 = false, hasCal = false;
        for (const char* e : devExts) {
            hasPt = hasPt || !strcmp(e, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
            hasId2 = hasId2 || !strcmp(e, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
            hasCal = hasCal || !strcmp(e, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        }
        if (hasPt || hasId2) {
            VkPhysicalDeviceFeatures2 q2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pid2};
            vk.vkGetPhysicalDeviceFeatures2(g_pd, &q2);
            const bool ok = hasPt && hasId2 && ptf.presentTiming && pid2.presentId2;
            Log("present_timing: rozsireni %d, present_id2 %d, vlastnosti presentTiming %u (absolutni %u, relativni %u), "
                "presentId2 %u, calibrated_timestamps %d -> %s",
                hasPt, hasId2, ptf.presentTiming, ptf.presentAtAbsoluteTime, ptf.presentAtRelativeTime, pid2.presentId2,
                hasCal, ok ? "zapnuto (jen mereni)" : "NE");
            if (ok) {
                ptf.presentAtAbsoluteTime = ptf.presentAtRelativeTime = VK_FALSE;  // jen zpetna vazba casu
                ptf.pNext = chain;
                chain = &pid2;
                g_ptTimingExt = true;
                g_ptCalibExt = hasCal;
            } else {
                // rozsireni bez vlastnosti se nesmi zapnout bez struktury: vyradit ze seznamu
                devExts.erase(std::remove_if(devExts.begin(), devExts.end(),
                                             [](const char* e) {
                                                 return !strcmp(e, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) ||
                                                        !strcmp(e, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
                                             }),
                              devExts.end());
            }
        } else if (wantPt) {
            Log("present_timing: ovladac rozsireni VK_EXT_present_timing / VK_KHR_present_id2 nenabizi - bez mereni");
        }
        if (hasCal && !g_ptTimingExt)
            devExts.erase(std::remove_if(devExts.begin(), devExts.end(),
                                         [](const char* e) { return !strcmp(e, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME); }),
                          devExts.end());
    }
    std::string extList;
    for (const char* e : devExts) extList += std::string(" ") + e;
    Log("rozsireni zarizeni:%s", extList.c_str());

    // Druha fronta teze family pro vlakno presentu (blit + vkQueuePresentKHR, ktery muze blokovat omezovac FPS
    // ovladace): odeslani mostu pak na present neceka. Jen s fgPresenter; [fg] SeparateQueue=0 = sdilena pod zamkem.
    const bool twoQueues = g_fgPresenter && qfp[g_qf].queueCount >= 2 &&
                           wcstol(IniStr(L"fg", L"SeparateQueue", L"1").c_str(), nullptr, 0) != 0;
    const float prios[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, g_qf, twoQueues ? 2u : 1u,
                                   prios};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, chain};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();
    VkDeviceQueueGlobalPriorityCreateInfoKHR gpri = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
    bool hasPrio = false;
    for (const char* e : devExts) hasPrio = hasPrio || !strcmp(e, "VK_KHR_global_priority");
    if (qPrio > 0 && hasPrio) {
        gpri.globalPriority = qPrio >= 2 ? VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR : VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
        qci.pNext = &gpri;
    }
    r = vk.vkCreateDevice(g_pd, &dci, nullptr, &g_dev);
    if (qci.pNext && r != VK_SUCCESS) {
        Log("ladeni: vkCreateDevice s globalni prioritou front %d selhal (%d) - bez ni", qPrio, r);
        qci.pNext = nullptr;
        r = vk.vkCreateDevice(g_pd, &dci, nullptr, &g_dev);
    } else if (qPrio > 0) {
        Log("ladeni: globalni priorita front helperu %s %s", qPrio >= 2 ? "REALTIME" : "HIGH",
            qci.pNext ? "zapnuta" : "NEJDE (rozsireni neni)");
    }
    if (r != VK_SUCCESS) return Error("vkCreateDevice selhal %d", r), false;
#define LOADD(n)                                                   \
    vk.n = (PFN_##n)vk.vkGetDeviceProcAddr(g_dev, #n);             \
    if (!vk.n) return Error("chybi funkce %s", #n), false;
    VK_DEV_FUNCS(LOADD)
#undef LOADD
    vk.vkGetDeviceQueue(g_dev, g_qf, 0, &g_queue);
    g_presQueue = g_queue;
    if (twoQueues) vk.vkGetDeviceQueue(g_dev, g_qf, 1, &g_presQueue);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf};
    if (vk.vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool) != VK_SUCCESS)
        return Error("vkCreateCommandPool selhal"), false;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, kSlots};
    if (vk.vkAllocateCommandBuffers(g_dev, &cai, g_cmd) != VK_SUCCESS) return Error("alokace cmd bufferu"), false;
    cai.commandBufferCount = 1;
    if (vk.vkAllocateCommandBuffers(g_dev, &cai, &g_initCmd) != VK_SUCCESS) return Error("alokace cmd bufferu"), false;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int i = 0; i < kSlots; ++i)
        if (vk.vkCreateFence(g_dev, &fci, nullptr, &g_fence[i]) != VK_SUCCESS) return Error("vkCreateFence"), false;
    if (vk.vkCreateFence(g_dev, &fci, nullptr, &g_initFence) != VK_SUCCESS) return Error("vkCreateFence"), false;
    if (g_tsPeriod > 0.0f) {  // nefatalni: bez nich jen chybi mereni casu DLSS a zmenseni
        VkQueryPoolCreateInfo qpci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP,
                                      kSlots * kTsPerSlot};
        if (vk.vkCreateQueryPool(g_dev, &qpci, nullptr, &g_qpool) != VK_SUCCESS) g_qpool = VK_NULL_HANDLE;
    }
    if (!g_qpool) Log("WARN: casova razitka nejdou - bez mereni casu DLSS a zmenseni");
    Log("Vulkan zarizeni vytvoreno (fronta family %u, %u rozsireni)", g_qf, (uint32_t)devExts.size());
    return true;
}

static uint32_t DeviceLocalType(uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp = {};
    vk.vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
        if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return t;
    return UINT32_MAX;
}

// Pamet viditelna z CPU (pocitadla MV objektu); koherentni = bez vkFlush/vkInvalidate
static uint32_t HostVisibleType(uint32_t bits) {
    const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkPhysicalDeviceMemoryProperties mp = {};
    vk.vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
        if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & need) == need) return t;
    return UINT32_MAX;
}

static bool FormatHas(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties fp = {};
    vk.vkGetPhysicalDeviceFormatProperties(g_pd, f, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

// Compute pipeline pro motion vektory. Pri selhani nejde DLAA ani ladeni MV (most bezi dal).
static bool InitCompute() {
    if (!FormatHas(kMvFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !FormatHas(kOutFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(FormatHas(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
          FormatHas(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) ||
        !FormatHas(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        !FormatHas(VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        Log("CHYBA: GPU nepodporuje potrebne formaty (RG32F/RGBA8 storage, D24S8 nebo D32S8 a RGBA32F sampled)");
        return false;
    }
    g_blitLinearOk = FormatHas(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (vk.vkCreateSampler(g_dev, &sci, nullptr, &g_sampler) != VK_SUCCESS) return false;

    // Vazby 5 a 6 (RGBA16F pro vstup HDR) se zapisuji az v EnsureHdr; shadery mv/alpha/sharpen je
    // nepouzivaji, takze nezapsane (nebo po DestroyHdr neplatne) vazby jim nevadi. Totez vazba 7 (R32F hloubka
    // pro DLSS, zapisuje CreateSized jen pri podpore R32F; cte ji jen depthmin.comp) a vazba 8 (vystup DLSS RGBA8
    // velikosti vystupu pri S > 1, zapisuje EnsureBig; cte ji jen downsample.comp bez HDR_IN).
    // Vazby 9 (OBJMV_IN, zapisuje CreateSized) a 10 (pocitadla MV objektu, zapsana niz) cte jen mv.comp.
    // Vazby 11 (MASK_IN) a 12 (maska R8) zapisuje CreateSized, cte / pise jen reactive.comp.
    // Vazby 13 a 14 (pole 2, jas minuleho snimku pro ViewmodelCheck) zapisuje EnsureVmPrev, pouziva jen mv.comp.
    VkDescriptorSetLayoutBinding b[15] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_sampler},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_sampler},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_sampler},
        {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_sampler},
        {12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dlci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 15, b};
    if (vk.vkCreateDescriptorSetLayout(g_dev, &dlci, nullptr, &g_dsl) != VK_SUCCESS) return false;
    const VkDescriptorSetLayoutBinding bMv = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo dlciMv = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1,
                                                    &bMv};
    if (vk.vkCreateDescriptorSetLayout(g_dev, &dlciMv, nullptr, &g_dslMv) != VK_SUCCESS) return false;
    const VkDescriptorSetLayout dsls[2] = {g_dsl, g_dslMv};

    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 116};  // sizeof(MvPush) (static_assert u MvPush)
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 2, dsls, 1, &pcr};
    if (vk.vkCreatePipelineLayout(g_dev, &plci, nullptr, &g_pipeLayout) != VK_SUCCESS) return false;

    // Vsechny shadery sdileji layout (kazdy pouziva jen cast vazeb a push konstant).
    auto makePipe = [](const uint32_t* code, size_t size, VkPipeline* out) {
        VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, size, code};
        VkShaderModule sm = VK_NULL_HANDLE;
        if (vk.vkCreateShaderModule(g_dev, &smci, nullptr, &sm) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                      sm, "main"};
        cpci.layout = g_pipeLayout;
        VkResult r = vk.vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpci, nullptr, out);
        vk.vkDestroyShaderModule(g_dev, sm, nullptr);
        return r == VK_SUCCESS;
    };
    if (!makePipe(kMvCompSpv, sizeof kMvCompSpv, &g_pipe)) return false;
    if (!makePipe(kAlphaCompSpv, sizeof kAlphaCompSpv, &g_alphaPipe)) return false;
    if (!makePipe(kSharpenCompSpv, sizeof kSharpenCompSpv, &g_sharpPipe)) return false;

    // Vstup DLSS v linearnim HDR (InputMode 1, 2, 3): bez podpory formatu nebo shaderu jen LDR.
    g_hdrFmtOk = FormatHas(kHdrFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                 FormatHas(kExpFormat, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    if (!g_hdrFmtOk) {
        Log("WARN: GPU nepodporuje RGBA16F storage/sampled nebo R16F sampled - vstup DLSS HDR (InputMode 1, 2, 3) "
            "pobezi jako 0 (LDR)");
    } else if (!makePipe(kLinearizeCompSpv, sizeof kLinearizeCompSpv, &g_linPipe) ||
               !makePipe(kAlphaHdrCompSpv, sizeof kAlphaHdrCompSpv, &g_alphaHdrPipe)) {
        g_hdrFmtOk = false;
        Log("WARN: shadery pro vstup HDR nejdou vytvorit - InputMode 1, 2, 3 pobezi jako 0 (LDR)");
    }

    // Hloubka pro DLSS (DepthMode 1, 2): bez podpory R32F storage+sampled nebo shaderu jen puvodni hloubka.
    g_depthFmtOk = FormatHas(kDepthFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    if (!g_depthFmtOk) {
        Log("WARN: GPU nepodporuje R32F storage/sampled - hloubka DLSS (DepthMode 1, 2) pobezi jako 0 (puvodni)");
    } else if (!makePipe(kDepthMinCompSpv, sizeof kDepthMinCompSpv, &g_depthPipe)) {
        g_depthFmtOk = false;
        Log("WARN: shader depthmin.comp nejde vytvorit - DepthMode 1, 2 pobezi jako 0 (puvodni)");
    }

    // Maska reaktivity: bez podpory R8 storage+sampled nebo shaderu bez masky (FSR / DLSS jako ve verzi 13)
    g_reactFmtOk = FormatHas(kReactFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    if (!g_reactFmtOk) {
        Log("WARN: GPU nepodporuje R8 storage/sampled - maska reaktivity nepujde");
    } else if (!makePipe(kReactiveCompSpv, sizeof kReactiveCompSpv, &g_reactPipe)) {
        g_reactFmtOk = false;
        Log("WARN: shader reactive.comp nejde vytvorit - maska reaktivity nepujde");
    }

    // Supersampling (S > 1): bez pipeline zmenseni jen DLAA. Varianta HDR_IN jen kdyz jde vstup HDR (jinak by
    // vstup HDR stejne bezel jako LDR).
    g_upOk = makePipe(kDownsampleSmallCompSpv, sizeof kDownsampleSmallCompSpv, &g_downPipe[0]) &&
             makePipe(kDownsampleCompSpv, sizeof kDownsampleCompSpv, &g_downPipe[1]);
    if (g_upOk && g_hdrFmtOk &&
        (!makePipe(kDownsampleSmallHdrCompSpv, sizeof kDownsampleSmallHdrCompSpv, &g_downHdrPipe[0]) ||
         !makePipe(kDownsampleHdrCompSpv, sizeof kDownsampleHdrCompSpv, &g_downHdrPipe[1])))
        g_upOk = false;
    if (!g_upOk) Log("WARN: shader downsample.comp nejde vytvorit - OutputScale > 1 pobezi jako 1 (DLAA)");

    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 + 2},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 + 4 + 2},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    // 1 hlavni set + set 1 (MV) pro g_mvImg a 3 snapshoty FG
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 5, 3, ps};
    if (vk.vkCreateDescriptorPool(g_dev, &dpci, nullptr, &g_descPool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, g_descPool, 1, &g_dsl};
    if (vk.vkAllocateDescriptorSets(g_dev, &dsai, &g_descSet) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo dsaiMv = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, g_descPool, 1, &g_dslMv};
    if (vk.vkAllocateDescriptorSets(g_dev, &dsaiMv, &g_mvSet) != VK_SUCCESS) return false;

    // Pocitadla MV objektu (mv.comp, vazba 10): maly buffer mapovany do pameti, cast pro kazdy slot
    {
        const VkDeviceSize size = (VkDeviceSize)kSlots * kObjStatPerSlot * sizeof(uint32_t);
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        if (vk.vkCreateBuffer(g_dev, &bci, nullptr, &g_objStatBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements req = {};
        vk.vkGetBufferMemoryRequirements(g_dev, g_objStatBuf, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                                   HostVisibleType(req.memoryTypeBits)};
        void* ptr = nullptr;
        if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(g_dev, &ai, nullptr, &g_objStatMem) != VK_SUCCESS ||
            vk.vkBindBufferMemory(g_dev, g_objStatBuf, g_objStatMem, 0) != VK_SUCCESS ||
            vk.vkMapMemory(g_dev, g_objStatMem, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS || !ptr) {
            Log("CHYBA: buffer pocitadel MV objektu nejde vytvorit");
            return false;
        }
        g_objStatPtr = (uint32_t*)ptr;
        memset(g_objStatPtr, 0, (size_t)size);
        VkDescriptorBufferInfo dbi = {g_objStatBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet bw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_descSet, 10, 0, 1,
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dbi};
        vk.vkUpdateDescriptorSets(g_dev, 1, &bw, 0, nullptr);
    }
    Log("compute shadery pripraveny (motion vektory %u B, alfa %u B, RCAS %u B SPIR-V; vstup HDR RGBA16F: %s; "
        "hloubka DLSS R32F: %s; supersampling: %s)",
        (unsigned)sizeof kMvCompSpv, (unsigned)sizeof kAlphaCompSpv, (unsigned)sizeof kSharpenCompSpv,
        g_hdrFmtOk ? "ano" : "ne", g_depthFmtOk ? "ano" : "ne", g_upOk ? "ano" : "ne");
    return true;
}

// NGX + kontrola dostupnosti DLSS. Pri selhani je DLAA nedostupne (most bezi dal).
static bool InitNgx() {
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, g_dir.c_str(), g_inst, g_pd, g_dev,
        vk.vkGetInstanceProcAddr, vk.vkGetDeviceProcAddr, &g_ngxCommon, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) return DlaaFail("NVSDK_NGX_VULKAN_Init selhal: %s", NgxStr(r).c_str()), false;
    g_ngxInit = true;

    NVSDK_NGX_Parameter* caps = nullptr;
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_FAILED(r) || !caps)
        return DlaaFail("NGX GetCapabilityParameters selhal: %s (stary ovladac?)", NgxStr(r).c_str()), false;
    int needsDriver = 0, available = 0, initResult = 0;
    unsigned drvMajor = 0, drvMinor = 0;
    NVSDK_NGX_Result rNeeds = NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                                                       &needsDriver);
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &drvMajor);
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &drvMinor);
    NVSDK_NGX_Result rAvail = NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Result rInit = NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
                                                      &initResult);
    g_ngxCaps = caps;  // zustava (callback optimalnich rozmeru pro S > 1), uvolni se na konci
    Log("NGX: DLSS available=%d (%s), needsUpdatedDriver=%d (min %u.%u), featureInitResult=0x%X (%s)", available,
        NVSDK_NGX_SUCCEED(rAvail) ? "ok" : "nelze zjistit", needsDriver, drvMajor, drvMinor, (unsigned)initResult,
        NVSDK_NGX_SUCCEED(rInit) ? "ok" : "nelze zjistit");
    if (NVSDK_NGX_SUCCEED(rNeeds) && needsDriver)
        return DlaaFail("DLSS vyzaduje novejsi ovladac (min %u.%u)", drvMajor, drvMinor), false;
    // FeatureInitResult je NVSDK_NGX_Result: chybovy kod = DLSS se nenacetlo nebo je pro aplikaci zamitnuto
    const bool initFailed = NVSDK_NGX_SUCCEED(rInit) && NVSDK_NGX_FAILED((NVSDK_NGX_Result)initResult);
    if (NVSDK_NGX_FAILED(rAvail) || !available || initFailed) {
        if (initFailed)
            return DlaaFail("DLSS neni dostupne: %s", NgxStr((NVSDK_NGX_Result)initResult).c_str()), false;
        return DlaaFail("DLSS neni dostupne (chybi nvngx_dlss.dll vedle helperu?)"), false;
    }

    r = NVSDK_NGX_VULKAN_AllocateParameters(&g_ngxParams);
    if (NVSDK_NGX_FAILED(r) || !g_ngxParams)
        return DlaaFail("NGX AllocateParameters selhal: %s", NgxStr(r).c_str()), false;
    Log("NGX inicializovano, DLSS dostupne");
    return true;
}

static bool ImportSemaphore(uint64_t handle, VkSemaphore* out) {
    VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr,
                                     VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
    if (vk.vkCreateSemaphore(g_dev, &sci, nullptr, out) != VK_SUCCESS) return false;
    VkImportSemaphoreWin32HandleInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    ii.semaphore = *out;
    ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    ii.handle = (HANDLE)(uintptr_t)handle;
    VkResult r = vk.vkImportSemaphoreWin32HandleKHR(g_dev, &ii);
    CloseHandle(ii.handle);  // NT handle: import nepreviazi vlastnictvi
    return r == VK_SUCCESS;
}

// Volat jen kdyz GPU nic nezpracovava (feature nesmi byt v rozpracovanem cmd bufferu).
static void ReleaseDlss() {
    if (!g_dlss) return;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_ReleaseFeature(g_dlss);
    Log("DLSS feature uvolnena (%s)", NgxStr(r).c_str());
    g_dlss = nullptr;
}

// Volat jen kdyz GPU nic nezpracovava (kontext nesmi byt v rozpracovanem cmd bufferu; jeho obrazy a pohledy zanikaji).
static void ReleaseFsr() {
    if (!g_fsr) return;
    const ffxReturnCode_t r = g_ffxDestroy(&g_fsr, nullptr);
    Log("FSR kontext uvolnen (kod %u)", (unsigned)r);
    g_fsr = nullptr;
}

static void DestroyOwn(OwnImg& o) {
    if (o.view) vk.vkDestroyImageView(g_dev, o.view, nullptr);
    if (o.img) vk.vkDestroyImage(g_dev, o.img, nullptr);
    if (o.mem) vk.vkFreeMemory(g_dev, o.mem, nullptr);
    o = OwnImg();
}

// Obrazy pro vstup HDR. Volat jen kdyz GPU nic nezpracovava. Vazby 5 a 6 descriptor setu pak ukazuji
// na zrusene pohledy; pouzivaji je jen pruchody HDR, a ty bez g_hdrImgOk nebezi.
static void DestroyHdr() {
    for (OwnImg* o : {&g_linImg, &g_hdrOutImg, &g_expImg}) DestroyOwn(*o);
    g_hdrImgOk = false;
}

// Vystup DLSS RGBA8 velikosti vystupu (S > 1, LDR). Volat jen kdyz GPU nic nezpracovava; vazba 8 pak ukazuje
// na zruseny pohled, cte ji jen downsample.comp (LDR), a ten bez g_bigImg.img nebezi.
static void DestroyBig() { DestroyOwn(g_bigImg); }

// Obrazy a pohledy zavisle na rozliseni + DLSS feature. Volat jen kdyz GPU nic nezpracovava.
// Poradi: nejdriv feature (muze odkazovat na pameti obrazu jen behem vyhodnoceni, ale uvolnit ji pred nimi),
// pak obrazy velikosti vystupu (HDR, RGBA8), pak obrazy velikosti vstupu a finalniho obrazu, nakonec pohledy
// na sdilene obrazy (ty rusi volajici az po DestroySized).
static void DestroySized() {
    ReleaseDlss();
    ReleaseFsr();
    DestroyHdr();
    DestroyBig();
    for (OwnImg* o : {&g_mvImg, &g_outImg, &g_sharpImg, &g_depthImg, &g_reactImg, &g_vmPrev[0], &g_vmPrev[1]})
        DestroyOwn(*o);
    g_vmPrevOk = g_vmPrevValid = false;  // vazby 13 a 14 ted ukazuji na zrusene pohledy
    g_depthImgOk = false;  // vazba 7 ted ukazuje na zruseny pohled; depthmin.comp bez g_depthImgOk nebezi
    g_reactImgOk = false;  // vazby 11 a 12 taky
    if (g_maskInView) vk.vkDestroyImageView(g_dev, g_maskInView, nullptr);
    g_maskInView = VK_NULL_HANDLE;
    if (g_colorInView) vk.vkDestroyImageView(g_dev, g_colorInView, nullptr);
    if (g_depthInView) vk.vkDestroyImageView(g_dev, g_depthInView, nullptr);
    if (g_objInView) vk.vkDestroyImageView(g_dev, g_objInView, nullptr);
    g_colorInView = g_depthInView = g_objInView = VK_NULL_HANDLE;
    g_ownOk = false;
}

static VkImageView MakeView(VkImage img, VkFormat fmt, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    return vk.vkCreateImageView(g_dev, &vci, nullptr, &v) == VK_SUCCESS ? v : VK_NULL_HANDLE;
}

static bool MakeOwnImage(OwnImg& o, VkFormat fmt, VkImageUsageFlags usage, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.vkCreateImage(g_dev, &ci, nullptr, &o.img) != VK_SUCCESS) return false;
    VkMemoryRequirements req = {};
    vk.vkGetImageMemoryRequirements(g_dev, o.img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                               DeviceLocalType(req.memoryTypeBits)};
    if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(g_dev, &ai, nullptr, &o.mem) != VK_SUCCESS)
        return false;
    if (vk.vkBindImageMemory(g_dev, o.img, o.mem, 0) != VK_SUCCESS) return false;
    o.view = MakeView(o.img, fmt, VK_IMAGE_ASPECT_COLOR_BIT);
    o.w = w;
    o.h = h;
    return o.view != VK_NULL_HANDLE;
}

// Obrazy velikosti vstupu DLSS (MV, hloubka R32F) a finalniho obrazu (vystup, RCAS). Obrazy velikosti vystupu DLSS
// (S > 1) vytvari az CreateDlss, protoze S muze spadnout na 1.
static bool CreateSized() {
    if (!g_computeOk) return false;
    if (!FormatHas(g_depthFmt, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        return Log("CHYBA: GPU neumi cist hloubku sceny ve formatu %d", (int)g_depthFmt), false;
    g_colorInView = MakeView(g_img[BMSD_IMG_COLOR_IN].img, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    g_depthInView = MakeView(g_img[BMSD_IMG_DEPTH_IN].img, g_depthFmt, VK_IMAGE_ASPECT_DEPTH_BIT);
    g_objInView = MakeView(g_img[BMSD_IMG_OBJMV_IN].img, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!g_colorInView || !g_depthInView || !g_objInView)
        return Log("CHYBA: pohledy na sdilene vstupy nejdou vytvorit"), false;
    if (!MakeOwnImage(g_mvImg, kMvFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, g_inW, g_inH) ||
        !MakeOwnImage(g_outImg, kOutFormat,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, g_w,
                      g_h) ||
        !MakeOwnImage(g_sharpImg, kOutFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, g_w, g_h))
        return Log("CHYBA: vlastni obrazy helperu nejdou vytvorit"), false;

    VkDescriptorImageInfo ii[5] = {{g_sampler, g_depthInView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {g_sampler, g_colorInView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {VK_NULL_HANDLE, g_mvImg.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, g_outImg.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, g_sharpImg.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[5];
    for (uint32_t i = 0; i < 5; ++i) {
        wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_descSet, i, 0, 1,
                 i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[i]};
    }
    vk.vkUpdateDescriptorSets(g_dev, 5, wr, 0, nullptr);
    {
        VkWriteDescriptorSet mw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_mvSet, 0, 0, 1,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[2]};
        vk.vkUpdateDescriptorSets(g_dev, 1, &mw, 0, nullptr);
    }
    // OBJMV_IN zustava po cely snimek v GENERAL (prevzeti od DXVK), mv.comp ho cte jen pri objMvCount > 0
    VkDescriptorImageInfo oi = {g_sampler, g_objInView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet ow = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_descSet, 9, 0, 1,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &oi};
    vk.vkUpdateDescriptorSets(g_dev, 1, &ow, 0, nullptr);
    g_ownOk = true;

    // Hloubka pro DLSS (DepthMode 1, 2) a maska reaktivity: az pri prvnim pouziti (EnsureDepthImg / EnsureReactImg
    // v ProcessFrame) - vychozi nastaveni je nepouziva (1440p: ~18 MB VRAM)
    g_depthPending = g_depthFmtOk;
    g_reactPending = g_reactFmtOk;
    g_vmPrevPending = g_depthFmtOk && g_cfg.vmCheck != 0;
    return true;
}

// Mereni MV zbrane (ViewmodelCheck): 2 obrazy R32F velikosti vstupu (jas minuleho snimku). GPU v klidu.
static void EnsureVmPrev() {
    g_vmPrevPending = false;
    bool ok = true;
    for (OwnImg& o : g_vmPrev)
        ok = ok && MakeOwnImage(o, kDepthFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, g_inW, g_inH);
    if (!ok) {
        for (OwnImg& o : g_vmPrev) DestroyOwn(o);
        Log("WARN: obrazy pro mereni MV zbrane nejdou vytvorit - ViewmodelCheck nepobezi");
        return;
    }
    VkDescriptorImageInfo si[2] = {{g_sampler, g_vmPrev[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                   {g_sampler, g_vmPrev[1].view, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorImageInfo wi[2] = {{VK_NULL_HANDLE, g_vmPrev[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, g_vmPrev[1].view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_descSet, 13, 0, 2,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, si},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_descSet, 14, 0, 2,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wi}};
    vk.vkUpdateDescriptorSets(g_dev, 2, w, 0, nullptr);
    g_vmPrevOk = true;
    g_vmPrevValid = false;
    g_vmPrevInit = true;  // prvni snimek: prechod z UNDEFINED
    Log("mereni MV zbrane (ViewmodelCheck): 2 obrazy R32F %ux%u vytvoreny", g_inW, g_inH);
}

// Hloubka pro DLSS (DepthMode 1, 2), R32F velikosti vstupu; nefatalni - bez ni jen DepthMode 0. Volat jen s GPU
// v klidu (meni descriptor set).
static void EnsureDepthImg() {
    g_depthPending = false;
    if (MakeOwnImage(g_depthImg, kDepthFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, g_inW, g_inH)) {
        VkDescriptorImageInfo di = {VK_NULL_HANDLE, g_depthImg.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet dw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_descSet, 7, 0, 1,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &di};
        vk.vkUpdateDescriptorSets(g_dev, 1, &dw, 0, nullptr);
        g_depthImgOk = true;
        Log("obraz R32F pro hloubku DLSS vytvoren (%ux%u)", g_inW, g_inH);
    } else {
        DestroyOwn(g_depthImg);
        Log("WARN: obraz R32F pro hloubku DLSS nejde vytvorit - DepthMode 1, 2 pobezi jako 0 (puvodni)");
    }
}

// Maska reaktivity (R8 velikosti vstupu + pohled na MASK_IN); nefatalni - bez ni FSR / DLSS bez masky. GPU v klidu.
static void EnsureReactImg() {
    g_reactPending = false;
    g_maskInView = MakeView(g_img[BMSD_IMG_MASK_IN].img, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (g_maskInView && MakeOwnImage(g_reactImg, kReactFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                     g_inW, g_inH)) {
        VkDescriptorImageInfo mi[2] = {{g_sampler, g_maskInView, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, g_reactImg.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet mw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_descSet, 11, 0, 1,
                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mi[0]},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_descSet, 12, 0, 1,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &mi[1]}};
        vk.vkUpdateDescriptorSets(g_dev, 2, mw, 0, nullptr);
        g_reactImgOk = true;
        Log("obraz R8 pro masku reaktivity vytvoren (%ux%u)", g_inW, g_inH);
    } else {
        DestroyOwn(g_reactImg);
        if (g_maskInView) vk.vkDestroyImageView(g_dev, g_maskInView, nullptr);
        g_maskInView = VK_NULL_HANDLE;
        Log("WARN: obraz R8 pro masku reaktivity nejde vytvorit - maska nepujde");
    }
}

// Jednorazove prikazy v g_initCmd (vytvoreni DLSS, priprava obrazu): odeslat a pockat.
static VkResult SubmitInitCmd() {
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_initCmd;
    vk.vkResetFences(g_dev, 1, &g_initFence);
    VkResult vr;
    {
        std::lock_guard<std::mutex> lk(g_queueMx);
        vr = vk.vkQueueSubmit(g_queue, 1, &si, g_initFence);
    }
    if (vr == VK_SUCCESS) vr = vk.vkWaitForFences(g_dev, 1, &g_initFence, VK_TRUE, 10000000000ull);
    return vr;
}

static void BeginInitCmd() {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkResetCommandBuffer(g_initCmd, 0);
    vk.vkBeginCommandBuffer(g_initCmd, &bi);
}

// Obrazy pro vstup DLSS v linearnim HDR (InputMode 1, 2, 3): vstup velikosti vstupu DLSS, vystup outW x outH.
// Pri jine velikosti vystupu (zmena S) se vytvori znovu. Volat jen kdyz GPU nic nezpracovava (meni descriptor set).
// false = HDR nejde, volajici pouzije LDR.
static bool EnsureHdr(uint32_t outW, uint32_t outH) {
    if (g_hdrImgOk && g_hdrOutImg.w == outW && g_hdrOutImg.h == outH && g_linImg.w == g_inW && g_linImg.h == g_inH)
        return true;
    DestroyHdr();
    if (!g_hdrFmtOk) return Log("vstup HDR: GPU nepodporuje RGBA16F storage nebo R16F (viz vyse)"), false;
    // RGBA16F: vstup DLSS musi byt sampled, vystup storage (Guide 3.4); pruchody HDR je ctou/pisou jako storage.
    const VkImageUsageFlags hdrUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!MakeOwnImage(g_linImg, kHdrFormat, hdrUsage, g_inW, g_inH) ||
        !MakeOwnImage(g_hdrOutImg, kHdrFormat, hdrUsage, outW, outH) ||
        !MakeOwnImage(g_expImg, kExpFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, 1)) {
        DestroyHdr();
        return Log("vstup HDR: obrazy RGBA16F nejdou vytvorit"), false;
    }

    // Expozice 1x1 = 1.0: jednou vymazat a nechat trvale ve stavu pro cteni (Guide 3.4: vstupy DLSS
    // v "sampled" stavu; NGX je po vyhodnoceni vraci do puvodniho stavu). Bariera se 2. rozsahem
    // ALL_COMMANDS plati i pro dalsi odeslani do stejne fronty.
    BeginInitCmd();
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = g_expImg.img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdPipelineBarrier(g_initCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                            nullptr, 0, nullptr, 1, &b);
    VkClearColorValue one = {};
    one.float32[0] = 1.0f;
    vk.vkCmdClearColorImage(g_initCmd, g_expImg.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1,
                            &b.subresourceRange);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vk.vkCmdPipelineBarrier(g_initCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                            nullptr, 0, nullptr, 1, &b);
    vk.vkEndCommandBuffer(g_initCmd);
    VkResult vr = SubmitInitCmd();
    if (vr != VK_SUCCESS) {
        DestroyHdr();
        return Log("vstup HDR: priprava obrazu expozice selhala (%d)", vr), false;
    }

    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, g_linImg.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, g_hdrOutImg.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[2];
    for (uint32_t i = 0; i < 2; ++i)
        wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_descSet, 5 + i, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[i]};
    vk.vkUpdateDescriptorSets(g_dev, 2, wr, 0, nullptr);
    g_hdrImgOk = true;
    if (outW == g_inW && outH == g_inH)
        Log("vstup HDR: obrazy RGBA16F %ux%u a expozice 1x1 R16F = 1.0 pripraveny", g_inW, g_inH);
    else
        Log("vstup HDR: obrazy RGBA16F %ux%u (vstup) a %ux%u (vystup DLSS), expozice 1x1 R16F = 1.0 pripraveny",
            g_inW, g_inH, outW, outH);
    return true;
}

// Vystup DLSS RGBA8 outW x outH pro S > 1 se vstupem LDR (vazba 8). Volat jen kdyz GPU nic nezpracovava.
static bool EnsureBig(uint32_t outW, uint32_t outH) {
    if (g_bigImg.img && g_bigImg.w == outW && g_bigImg.h == outH) return true;
    DestroyBig();
    // DLSS do nej pise jako do storage obrazu, downsample.comp ho cte pres imageLoad (layout GENERAL)
    if (!MakeOwnImage(g_bigImg, kOutFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, outW, outH)) {
        DestroyBig();
        return Log("vystup DLSS: obraz RGBA8 %ux%u nejde vytvorit", outW, outH), false;
    }
    VkDescriptorImageInfo ii = {VK_NULL_HANDLE, g_bigImg.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_descSet, 8, 0, 1,
                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii};
    vk.vkUpdateDescriptorSets(g_dev, 1, &wr, 0, nullptr);
    return true;
}

// VRAM procesu helperu (VK_EXT_memory_budget; na Windows usage = tento proces, vcetne alokaci NGX) do logu
static void LogVram(const char* when) {
    if (!g_memBudget) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT mb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &mb};
    vk.vkGetPhysicalDeviceMemoryProperties2(g_pd, &mp);
    double used = 0.0;
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; ++i)
        if (mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) used += (double)mb.heapUsage[i];
    Log("VRAM helperu: %.0f MB (%s)", used / (1024.0 * 1024.0), when);
}

// ---------------------------------------------------------------------------
// Mereni GPU casu (casova razitka)
// ---------------------------------------------------------------------------
// Precte vysledky slotu s (volat az kdyz je jeho fence signalizovana).
static void CollectTimestamps(int s) {
    const uint32_t m = g_tsMask[s];
    g_tsMask[s] = 0;
    if (!g_qpool || !m) return;
    uint64_t t[kTsPerSlot] = {};
    for (uint32_t i = 0; i < kTsPerSlot; ++i)
        if ((m >> i) & 1u)
            if (vk.vkGetQueryPoolResults(g_dev, g_qpool, s * kTsPerSlot + i, 1, sizeof(uint64_t), &t[i],
                                         sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
                return;
    const double toMs = g_tsPeriod * 1e-6;
    auto has = [&](uint32_t i) { return ((m >> i) & 1u) != 0; };
    auto seg = [&](uint32_t a, uint32_t b) { return t[b] >= t[a] ? (double)(t[b] - t[a]) * toMs : 0.0; };
    if (has(kTsPre) && has(kTsDlss)) {
        g_tsDlssMs += seg(kTsPre, kTsDlss);
        ++g_tsN;
    }
    if (has(kTsDlss) && has(kTsDown)) {
        g_tsDownMs += seg(kTsDlss, kTsDown);
        ++g_tsNDown;
    }
    if (has(kTsStart) && has(kTsEnd)) {
        // useky mezi po sobe jdoucimi zapsanymi znackami; chybejici znacka = usek se pricte k dalsimu
        static const int slotOf[kTsPerSlot] = {-1, 0, 1, 2, 3, 4, 5};
        uint32_t prev = kTsStart;
        for (uint32_t i = kTsPre; i < kTsPerSlot; ++i)
            if (has(i)) {
                g_profMs[slotOf[i]] += seg(prev, i);
                prev = i;
            }
        const double tot = seg(kTsStart, kTsEnd);
        g_profMs[6] += tot;
        g_profMax = std::max(g_profMax, tot);
        ++g_profN;
    }
}

// Pocitadla MV objektu slotu s (volat az kdyz je jeho fence signalizovana): secist kose a pridat k intervalu.
static void CollectObjStats(int s) {
    if (!g_objSlotUsed[s] || !g_objStatPtr) return;
    g_objSlotUsed[s] = false;
    const uint32_t* v = g_objStatPtr + (size_t)s * kObjStatPerSlot;
    for (uint32_t i = 0; i < kObjStatBuckets; ++i) {
        g_objAcc += v[i];
        g_objRej += v[kObjStatBuckets + i];
        g_objValN += v[2 * kObjStatBuckets + i];
        g_objValSum += v[3 * kObjStatBuckets + i];
        g_objValMax = std::max(g_objValMax, v[4 * kObjStatBuckets + i]);
        g_objValRej += v[5 * kObjStatBuckets + i];
        g_vmPx += v[6 * kObjStatBuckets + i];
        g_vmAcc += v[7 * kObjStatBuckets + i];
        g_vmMv += v[8 * kObjStatBuckets + i];
        g_vmE1 += v[9 * kObjStatBuckets + i];
        g_vmE0 += v[10 * kObjStatBuckets + i];
    }
    ++g_objFrames;
}

// Statistika MV objektu za interval do logu a do sdilene pameti (proxy ji pripise do radku "vykon"); pak vynulovat.
static void ObjStatsFlush() {
    if (g_objFrames) {
        const double f = (double)g_objFrames;
        const double mean = g_objValN ? (double)g_objValSum / 256.0 / (double)g_objValN : 0.0;
        const double mx = (double)g_objValMax / 256.0;
        char val[320] = "";
        // Odmitnute hloubkou u statickych objektu: zakryti necim neprehranym (alfa-test, lana, prusvitne) a hrany;
        // chyba jitteru nebo rasterizace replaye by podil zvedla (MV hladkeho pole by jinak prumer neukazal)
        const uint64_t valAll = g_objValN + g_objValRej;
        if (g_objValN)
            snprintf(val, sizeof val,
                     "; validace statickych objektu: %.0f px/snimek, |MV objektu - MV kamery| prumer %.4f px, max "
                     "%.2f px -> %s; odmitnuto hloubkou %.0f px/snimek (%.2f %% pixelu statickych objektu)",
                     (double)g_objValN / f, mean, mx, mean < 0.05 ? "OK (< 0.05 px)" : "CHYBA (cil < 0.05 px)",
                     (double)g_objValRej / f, 100.0 * (double)g_objValRej / (double)valAll);
        else if (g_objValRej)
            snprintf(val, sizeof val,
                     "; validace: zadny prijaty pixel statickeho objektu, odmitnuto hloubkou %.0f px/snimek (100 %%)",
                     (double)g_objValRej / f);
        else if (g_sh->objMvMode & BMSD_OBJMV_VALIDATE)
            snprintf(val, sizeof val, "; validace: zadny pixel statickeho objektu");
        Log("MV objektu: snimku %u, prijato %.0f px/snimek, odmitnuto hloubkou %.0f px/snimek (tolerance %g)%s",
            g_objFrames, (double)g_objAcc / f, (double)g_objRej / f, g_objTolLogged, val);
        g_sh->objMvStatFrames = g_objFrames;
        g_sh->objMvStatAccepted = (float)((double)g_objAcc / f);
        g_sh->objMvStatRejected = (float)((double)g_objRej / f);
        g_sh->objMvStatValPixels = (float)((double)g_objValN / f);
        g_sh->objMvStatValMean = (float)mean;
        g_sh->objMvStatValMax = (float)mx;
        MemoryBarrier();
        ++g_sh->objMvStatSeq;
    }
    if (g_objFrames && g_vmPx) {
        const double f = (double)g_objFrames;
        char ph[160] = "";
        if (g_vmPrevOk && g_vmAcc)
            snprintf(ph, sizeof ph, "; fotometricka chyba minuleho snimku: s MV zbrane %.4f, s MV 0 %.4f (pomer %.2f)",
                     (double)g_vmE1 / 1024.0 / (double)g_vmAcc, (double)g_vmE0 / 1024.0 / (double)g_vmAcc,
                     g_vmE0 ? (double)g_vmE1 / (double)g_vmE0 : 0.0);
        Log("MV zbrane: %.0f px/snimek, z toho s MV objektu %.1f %%, |MV| prumer %.3f px%s", (double)g_vmPx / f,
            100.0 * (double)g_vmAcc / (double)g_vmPx, g_vmAcc ? (double)g_vmMv / 256.0 / (double)g_vmAcc : 0.0, ph);
    }
    g_vmPx = g_vmAcc = g_vmMv = g_vmE1 = g_vmE0 = 0;
    g_objAcc = g_objRej = g_objValN = g_objValSum = g_objValRej = 0;
    g_objValMax = g_objFrames = 0;
}

// Prumerne casy do logu (kazdych ~5 s, pri force hned - pred zmenou feature) a vynulovat.
static void TimingFlush(bool force) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_tsLastLog.QuadPart) g_tsLastLog = now;
    if (!force && (now.QuadPart - g_tsLastLog.QuadPart) < 5 * g_qpf.QuadPart) return;
    g_tsLastLog = now;
    if (g_tsN) {
        char down[48] = "-";
        if (g_tsNDown) snprintf(down, sizeof down, "%.2f ms", g_tsDownMs / g_tsNDown);
        if (g_rW != g_w || g_rH != g_h)
            Log("%s: %.2f ms, downsample: %s, S=%s (cast B: vstup %ux%u -> vystup %ux%u, %s, snimku %u)",
                g_upActive == BMSD_UPSCALER_FSR ? "fsr" : "dlss", g_tsDlssMs / g_tsN, down, ScaleStr(g_outScale).c_str(), g_rW, g_rH, g_outW, g_outH,
                PerfName(g_perfQuality), g_tsN);
        else
            Log("%s: %.2f ms, downsample: %s, S=%s (vystup %ux%u, %s, snimku %u)",
                g_upActive == BMSD_UPSCALER_FSR ? "fsr" : "dlss", g_tsDlssMs / g_tsN, down,
                ScaleStr(g_outScale).c_str(), g_outW, g_outH, PerfName(g_perfQuality), g_tsN);
    }
    if (g_profN) {
        const double f = 1.0 / g_profN;
        Log("profil GPU helperu: predzprac %.2f, %s %.2f, zmenseni %.2f, dozprac %.2f, snapshot FG %.2f, kopie %.2f, "
            "celkem %.2f ms (max %.2f, snimku %u)",
            g_profMs[0] * f, g_upActive == BMSD_UPSCALER_FSR ? "fsr" : "dlss", g_profMs[1] * f, g_profMs[2] * f,
            g_profMs[3] * f, g_profMs[4] * f, g_profMs[5] * f, g_profMs[6] * f, g_profMax, g_profN);
    }
    for (double& v : g_profMs) v = 0.0;
    g_profMax = 0.0;
    g_profN = 0;
    g_tsDlssMs = g_tsDownMs = 0.0;
    g_tsN = g_tsNDown = 0;
    if (g_cpuProcN) {
        char wake[64] = "neznamo (starsi proxy)";
        if (g_cpuWakeN)
            snprintf(wake, sizeof wake, "prum %.3f / max %.2f ms", g_cpuWakeSum / g_cpuWakeN, g_cpuWakeMax);
        Log("cpu helperu: probuzeni %s, probuzeni -> odeslani prum %.3f / max %.2f ms (snimku %u)", wake,
            g_cpuProcSum / g_cpuProcN, g_cpuProcMax, g_cpuProcN);
    }
    g_cpuWakeSum = g_cpuWakeMax = g_cpuProcSum = g_cpuProcMax = 0.0;
    g_cpuWakeN = g_cpuProcN = 0;
    ObjStatsFlush();
}

// Casy jednoho snimku (QPC je spolecny pro vsechny procesy, postQpc zapisuje proxy tesne pred SetEvent).
static void CpuTiming(uint64_t posted, LONGLONG wake, LONGLONG submitted) {
    const double toMs = 1000.0 / (double)g_qpf.QuadPart;
    if (posted && (LONGLONG)posted <= wake) {
        const double w = (double)(wake - (LONGLONG)posted) * toMs;
        g_cpuWakeSum += w;
        g_cpuWakeMax = std::max(g_cpuWakeMax, w);
        ++g_cpuWakeN;
    }
    const double p = (double)(submitted - wake) * toMs;
    g_cpuProcSum += p;
    g_cpuProcMax = std::max(g_cpuProcMax, p);
    ++g_cpuProcN;
}

// Po vkDeviceWaitIdle: posbirat vysledky vsech slotu (patri jeste k dosavadni feature) a vypsat je.
static void TimingCollectAll() {
    for (int i = 0; i < kSlots; ++i)
        if (g_fenceUsed[i]) {
            CollectTimestamps(i);
            CollectObjStats(i);
        }
    TimingFlush(true);
}

// Kopie NGX_DLSS_GET_OPTIMAL_SETTINGS z nvsdk_ngx_helpers.h (ta hlavicka tahne D3D zavislosti).
static NVSDK_NGX_Result GetOptimalSettings(NVSDK_NGX_Parameter* p, unsigned outW, unsigned outH,
                                           NVSDK_NGX_PerfQuality_Value pq, unsigned* optW, unsigned* optH,
                                           unsigned* maxW, unsigned* maxH, unsigned* minW, unsigned* minH) {
    void* cb = nullptr;
    NVSDK_NGX_Parameter_GetVoidPointer(p, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &cb);
    if (!cb) return NVSDK_NGX_Result_FAIL_OutOfDate;
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Width, outW);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Height, outH);
    NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_PerfQualityValue, pq);
    NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_RTXValue, 0);  // starsi knihovny to chteji
    NVSDK_NGX_Result r = ((PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)cb)(p);
    if (NVSDK_NGX_FAILED(r)) return r;
    NVSDK_NGX_Parameter_GetUI(p, NVSDK_NGX_Parameter_OutWidth, optW);
    NVSDK_NGX_Parameter_GetUI(p, NVSDK_NGX_Parameter_OutHeight, optH);
    // Starsi knihovny bez dynamickeho rozliseni rozsah nevraci: pak min = max = optimalni
    *maxW = *minW = *optW;
    *maxH = *minH = *optH;
    NVSDK_NGX_Parameter_GetUI(p, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, maxW);
    NVSDK_NGX_Parameter_GetUI(p, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, maxH);
    NVSDK_NGX_Parameter_GetUI(p, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, minW);
    NVSDK_NGX_Parameter_GetUI(p, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, minH);
    return r;
}

// Vyber rezimu DLSS pro vystup outW x outH a vstup (render) g_rW x g_rH, kdyz se lisi: supersampling S > 1 (vstup
// W x H, vetsi vystup) nebo cast B (mensi vstup, vystup W x H). NGX_DLSS_GET_OPTIMAL_SETTINGS vraci
// pro kazdy rezim optimalni vstup a povoleny rozsah [min, max] (Guide 3.2.2, vcetne mezi); vstup mimo rozsah by
// vyhodnoceni odmitlo (FAIL_InvalidParameter). Z rezimu, jejichz rozsah vstup obsahuje, vyhraje ten s optimalnim
// vstupem nejblize nasemu (tedy podle pomeru). Vysledek pro stejne rozmery se pamatuje (log jen jednou).
static bool PickPerfQuality(uint32_t outW, uint32_t outH, int32_t* perf, std::string* why) {
    static uint32_t s_outW, s_outH, s_inW, s_inH;
    static bool s_ok;
    static int32_t s_perf;
    static std::string s_why;
    if (outW == s_outW && outH == s_outH && g_rW == s_inW && g_rH == s_inH) {
        *perf = s_perf;
        *why = s_why;
        return s_ok;
    }
    s_outW = outW;
    s_outH = outH;
    s_inW = g_rW;
    s_inH = g_rH;
    s_ok = false;
    s_perf = -1;
    s_why.clear();
    NVSDK_NGX_Parameter* params = g_ngxCaps ? g_ngxCaps : g_ngxParams;
    static const NVSDK_NGX_PerfQuality_Value kModes[] = {
        NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced,
        NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance,
        NVSDK_NGX_PerfQuality_Value_UltraQuality};
    Log("rezimy DLSS pro vystup %ux%u (vstup %ux%u, NGX_DLSS_GET_OPTIMAL_SETTINGS):", outW, outH, g_rW, g_rH);
    uint64_t best = UINT64_MAX;
    for (NVSDK_NGX_PerfQuality_Value m : kModes) {
        unsigned optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
        NVSDK_NGX_Result r = params ? GetOptimalSettings(params, outW, outH, m, &optW, &optH, &maxW, &maxH, &minW,
                                                         &minH)
                                    : NVSDK_NGX_Result_FAIL_NotInitialized;
        if (NVSDK_NGX_FAILED(r)) {
            Log("  %-17s: dotaz selhal (%s)", PerfName(m), NgxStr(r).c_str());
            continue;
        }
        if (!optW || !optH) {
            Log("  %-17s: nedostupny (0x0)", PerfName(m));
            continue;
        }
        const bool fits = minW <= g_rW && g_rW <= maxW && minH <= g_rH && g_rH <= maxH;
        Log("  %-17s: optimalni %ux%u (%.3f), rozsah %ux%u .. %ux%u -> vstup %s", PerfName(m), optW, optH,
            (double)optW / outW, minW, minH, maxW, maxH, fits ? "ano" : "mimo rozsah");
        if (!fits) continue;
        const uint64_t dist = (uint64_t)std::abs((int64_t)optW - (int64_t)g_rW) +
                              (uint64_t)std::abs((int64_t)optH - (int64_t)g_rH);
        if (dist < best) {
            best = dist;
            s_perf = m;
            s_ok = true;
        }
    }
    if (s_ok)
        Log("  -> rezim %s", PerfName(s_perf));
    else
        s_why = "zadny rezim DLSS nebere vstup " + std::to_string(g_rW) + "x" + std::to_string(g_rH) +
                " pro vystup " + std::to_string(outW) + "x" + std::to_string(outH);
    *perf = s_perf;
    *why = s_why;
    return s_ok;
}

// Hint presetu se cte pri vytvoreni feature a plati jen pro rezim, do jehoz parametru je zapsany (Guide 3.12;
// hint DLAA jen pri vstupu = vystupu). Pri S > 1 se rezim meni podle pomeru, proto preset do vsech.
static const char* const kPresetHints[] = {
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};

// Pozadavek proxy na render (vstup DLSS, BmsdShared::renderWidth/Height): 0 nebo hodnota mimo sdileny obraz = cely
// obraz (cast A). Vola se po importu (g_w x g_h zname) a pri zmene pozadavku.
static void SetRender(uint32_t rw, uint32_t rh) {
    g_rReqW = rw;
    g_rReqH = rh;
    g_rW = rw && rw < g_w ? rw : g_w;
    g_rH = rh && rh < g_h ? rh : g_h;
}

// Cast B: DLSS pro vstup g_rW x g_rH nejde (zadny rezim, vytvoreni feature, vyhodnoceni). Neni to trvale selhani
// (g_dlaaBroken ani dlaaStatus se nemeni): helper hlasi helperRenderOk = 0, proxy v casti B most nespousti a obraz
// roztahne engine. Novy pokus pri dalsi zmene renderu (take presetu nebo vstupu) a pri reimportu.
static void SubFail(const char* fmt, ...) {
    char buf[200], msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    snprintf(msg, sizeof msg, "cast B: DLSS pro vstup %ux%u nebezi: %s", g_rW, g_rH, buf);
    Log("%s - obraz roztahne engine (novy pokus pri zmene renderu)", msg);
    SetHelperMsg(msg);
}

// DLSS feature: vstup (render) g_rW x g_rH, vystup podle tryScale (1 = vystup W x H; S > 1 = vystup
// round(W*S) x round(H*S)). Rezim: vstup = vystup -> DLAA, jinak podle pomeru (PickPerfQuality): S > 1 v casti A,
// mensi render v casti B. Dva nezavisle priznaky (viz docs\advokat-upscaling-B2.md, D2): "up" (vystup != finalni
// obraz) zapina obrazy velikosti vystupu a zmenseni, "modeUp" (vstup != vystup) jen vyber rezimu - cast B pri S = 1
// tak jde vetvi DLAA (alfa / RCAS) bez zmenseni. V casti B se nesupersampluje (S se bere jako 1).
// Vola se pri importu, zmene presetu, vstupu, S nebo renderu (proxy ceka, frameValue = 0) a pri fallbacku po selhanem
// vyhodnoceni; GPU nic nezpracovava. Obrazy velikosti vystupu (g_hdrOutImg, g_bigImg) si prizpusobi samo. Kdyz S > 1
// nejde (zadny rezim, obraz, vytvoreni), bezi S = 1: DLSS trvale nedostupne (g_dlaaBroken) je jen kdyz selze S = 1
// s plnym vstupem. Selhani v casti B trvale neni (SubFail).
static void CreateDlss(float tryScale) {
    g_dlssPreset = g_sh->preset;
    g_dlssInputMode = g_sh->inputMode;
    g_hdrMode = BMSD_INPUT_LDR;
    g_outScale = 1.0f;
    g_outW = g_w;
    g_outH = g_h;
    g_perfQuality = -1;
    if (!g_ngxParams || g_dlaaBroken) return;
    const bool sub = g_rW != g_w || g_rH != g_h;  // cast B
    if (!g_ownOk) {
        if (sub) return SubFail("chybi compute shader nebo vlastni obrazy (viz vyse)");  // netrvale, viz SubFail
        return DlaaFail("chybi compute shader nebo vlastni obrazy (viz vyse)");
    }
    if (g_rW < 32 || g_rH < 32) {
        if (sub) return SubFail("vstup je pro DLSS moc maly");
        return DlaaFail("rozliseni %ux%u je pro DLSS moc male", g_rW, g_rH);
    }

    const uint32_t wantMode = g_dlssInputMode < BMSD_INPUT_COUNT ? g_dlssInputMode : BMSD_INPUT_LDR;
    const float reqScale = SanitizeScale(tryScale);
    float scale = reqScale;
    std::string upWhy;  // proc S > 1 neslo (log a helperMsg)
    if (scale > 1.0f && !g_upOk) {
        upWhy = "chybi shader zmenseni (viz vyse)";
        scale = 1.0f;
    }
    if (sub && scale > 1.0f) {
        // Cast B je kvuli uspore: supersampling by ji snedl (cena DLSS roste s vystupem). Pozadavek zustava
        // potvrzeny a plati zase pri plnem renderu.
        if (!g_subScaleLogged) {
            g_subScaleLogged = true;
            Log("cast B: OutputScale %sx se pri zmensenem renderu nepouziva (vystup DLSS = %ux%u), plati zase pri "
                "plnem renderu",
                ScaleStr(scale).c_str(), g_w, g_h);
        }
        scale = 1.0f;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    NVSDK_NGX_DLSS_Create_Params cp = {};
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    uint32_t mode = wantMode, outW = g_w, outH = g_h;
    int32_t perf = NVSDK_NGX_PerfQuality_Value_DLAA;
    for (;;) {
        const bool up = scale > 1.0f;   // vystup DLSS != finalni obraz: obrazy velikosti vystupu + zmenseni
        const bool modeUp = up || sub;  // vstup != vystup: rezim podle pomeru misto DLAA
        outW = g_w;
        outH = g_h;
        perf = NVSDK_NGX_PerfQuality_Value_DLAA;
        if (up) {
            // vystup vztazeny k finalnimu obrazu (W x H = sdilene obrazy), v casti A = vstup
            outW = (uint32_t)lround((double)g_w * scale);
            outH = (uint32_t)lround((double)g_h * scale);
            if (outW > 16384 || outH > 16384) {
                upWhy = "vystup " + std::to_string(outW) + "x" + std::to_string(outH) + " je moc velky";
                scale = 1.0f;
                continue;
            }
        }
        if (modeUp && !PickPerfQuality(outW, outH, &perf, &upWhy)) {
            if (!up) return SubFail("%s", upWhy.c_str());  // cast B (S = 1): bez DLSS, roztahne engine
            scale = 1.0f;
            continue;
        }
        // Obrazy velikosti vystupu, ktere tento pokus nepouzije, uvolnit pred vytvorenim feature (GPU stoji, feature
        // neni): jinak by po selhani S > 1 (casto nedostatek VRAM) RGBA8 / RGBA16F vystupu S > 1 drzely pamet i pri
        // pokusu o S = 1 a fallback by zbytecne koncil trvalou ztratou DLSS. RGBA8 jen pro S > 1 se vstupem LDR
        // (selze-li HDR, EnsureBig ho vytvori), RGBA16F jen se vstupem HDR (EnsureHdr ho pri jine velikosti vytvori znovu).
        if (!(up && wantMode == BMSD_INPUT_LDR)) DestroyBig();
        if (wantMode == BMSD_INPUT_LDR) DestroyHdr();
        mode = wantMode;
        if (mode != BMSD_INPUT_LDR && !EnsureHdr(outW, outH)) {
            Log("vstup DLSS: %s nejde - pouzivam LDR", InputName(mode));
            mode = BMSD_INPUT_LDR;
        }
        // Preset 0 (vychozi): K i pro Performance / Ultra Performance (SDK by dal M / L - se supersamplem 2x / 3x a
        // v usporne casti B ploty blikaji vic, viz README). [dlaa] AutoPresetK=0 = vychozi SDK.
        const uint32_t presetHint =
            g_dlssPreset ? g_dlssPreset : g_cfg.autoPresetK ? (uint32_t)NVSDK_NGX_DLSS_Hint_Render_Preset_K : 0u;
        for (const char* h : kPresetHints) NVSDK_NGX_Parameter_SetUI(g_ngxParams, h, presetHint);
        NVSDK_NGX_Parameter_SetI(g_ngxParams, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
        cp = {};
        // Vstup = skutecny (a nejvetsi) vstup, i kdyz optimalni vstup rezimu je jiny: lezi v povolenem rozsahu
        // (PickPerfQuality) a presety L/M alokuji podle rozmeru pri vytvoreni (Guide 3.2.2.1, bod 3).
        cp.Feature.InWidth = g_rW;
        cp.Feature.InHeight = g_rH;
        cp.Feature.InTargetWidth = outW;
        cp.Feature.InTargetHeight = outH;
        cp.Feature.InPerfQualityValue = (NVSDK_NGX_PerfQuality_Value)perf;
        bool created = false;
        for (;;) {
            // Vystup S > 1 se vstupem LDR: vlastni RGBA8 velikosti vystupu (u HDR je vystupem g_hdrOutImg)
            if (up && mode == BMSD_INPUT_LDR && !EnsureBig(outW, outH)) {
                r = NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
                break;
            }
            // MV v rozliseni vstupu, bez jitteru v MV, hloubka near = 0 / far = 1 (D3D9, neinvertovana).
            // LDR: barva po tonemapovani v sRGB (Guide 3.1.2). HDR: linearni barva, IsHDR (Guide 3.1.2, 5.3);
            // expozice pevna 1.0 z textury (Guide 3.9) nebo AutoExposure (Guide 3.10; i InputMode 3 - rozsah
            // po inverznim tone mappingu neni 0..1 a L/M pevnou expozici stejne ignoruji).
            cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            if (mode != BMSD_INPUT_LDR) cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
            if (mode == BMSD_INPUT_HDR_AUTO || mode == BMSD_INPUT_HDR_ITM)
                cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            BeginInitCmd();
            r = NGX_VULKAN_CREATE_DLSS_EXT1(g_dev, g_initCmd, 1, 1, &g_dlss, g_ngxParams, &cp);
            vk.vkEndCommandBuffer(g_initCmd);
            if (NVSDK_NGX_SUCCEED(r) && g_dlss) {
                created = true;
                break;
            }
            g_dlss = nullptr;
            if (mode == BMSD_INPUT_LDR) break;
            Log("vytvoreni DLSS feature se vstupem %s selhalo: %s - zkousim LDR", InputName(mode), NgxStr(r).c_str());
            mode = BMSD_INPUT_LDR;
        }
        if (created) break;
        if (!up) {
            DestroyBig();  // DLSS nebezi: obrazy velikosti vystupu nedrzet
            DestroyHdr();
            if (sub)  // cast B: netrvale (zadne DlaaFail), viz SubFail
                return SubFail("vytvoreni DLSS feature (%s) -> %ux%u selhalo: %s", PerfName(perf), outW, outH,
                               NgxStr(r).c_str());
            return DlaaFail("vytvoreni DLSS feature %ux%u selhalo: %s", g_rW, g_rH, NgxStr(r).c_str());
        }
        upWhy = std::string("vytvoreni DLSS feature (") + PerfName(perf) + ") " + std::to_string(g_rW) + "x" +
                std::to_string(g_rH) + " -> " + std::to_string(outW) + "x" + std::to_string(outH) +
                " selhalo: " + NgxStr(r);
        Log("%s", upWhy.c_str());
        scale = 1.0f;
    }
    VkResult vr = SubmitInitCmd();
    QueryPerformanceCounter(&t1);
    if (vr != VK_SUCCESS) {
        ReleaseDlss();
        DestroyBig();
        DestroyHdr();
        if (sub) return SubFail("odeslani prikazu pro vytvoreni DLSS selhalo (%d)", vr);  // netrvale, viz SubFail
        return DlaaFail("odeslani prikazu pro vytvoreni DLSS selhalo (%d)", vr);
    }
    // Obrazy, ktere tato konfigurace nepotrebuje (GPU stoji): RGBA16F pri LDR, RGBA8 vystupu mimo S > 1 s LDR
    if (mode == BMSD_INPUT_LDR) DestroyHdr();
    if (!(scale > 1.0f && mode == BMSD_INPUT_LDR)) DestroyBig();
    g_hdrMode = mode;
    g_outScale = scale;
    g_outW = outW;
    g_outH = outH;
    g_perfQuality = perf;
    g_dlssFresh = true;
    g_dlaaFrames = 0;
    const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpf.QuadPart;
    if (scale > 1.0f)
        Log("DLSS feature (%s) vytvorena: vstup %ux%u -> vystup %ux%u, flags 0x%X, %.0f ms", PerfName(perf), g_rW,
            g_rH, outW, outH, (unsigned)cp.InFeatureCreateFlags, ms);
    else if (sub)
        Log("DLSS feature (%s) vytvorena: vstup %ux%u (sub-rect %ux%u) -> vystup %ux%u, flags 0x%X, %.0f ms",
            PerfName(perf), g_rW, g_rH, g_w, g_h, outW, outH, (unsigned)cp.InFeatureCreateFlags, ms);
    else
        Log("DLSS feature (DLAA) vytvorena: %ux%u, flags 0x%X, %.0f ms", g_rW, g_rH,
            (unsigned)cp.InFeatureCreateFlags, ms);
    Log("vystup DLSS: %ux%u (%sx, %s)", outW, outH, ScaleStr(scale).c_str(), PerfName(perf));
    if (reqScale > 1.0f && scale == 1.0f && !sub) {
        char msg[256];
        snprintf(msg, sizeof msg, "vystup DLSS %sx nejde: %s - bezi 1.0x (DLAA)", ScaleStr(reqScale).c_str(),
                 upWhy.c_str());
        Log("%s", msg);
        SetHelperMsg(msg);  // proxy ho zapise do sveho logu; DLSS bezi dal (dlaaStatus zustava 1)
    }
    if (g_dlssPreset == 0 && g_cfg.autoPresetK)
        Log("preset: vychozi = K ([dlaa] AutoPresetK; SDK by pro rezim %s dal %s)", PerfName(perf), DefaultPresetFor(perf));
    else if (g_dlssPreset == 0)
        Log("preset: vychozi (SDK pro rezim %s: %s)", PerfName(perf), DefaultPresetFor(perf));
    else
        Log("preset: %s", PresetName(g_dlssPreset).c_str());
    if (mode == g_dlssInputMode)
        Log("vstup DLSS: %s", InputName(mode));
    else
        Log("vstup DLSS: %s (pozadovano %s, nejde - viz vyse)", InputName(mode), InputName(g_dlssInputMode));
    // Guide 3.9: pevnou expozici berou jen presety J a K, L (a M) maji vzdy vlastni auto expozici.
    if (mode == BMSD_INPUT_HDR && (g_dlssPreset == (uint32_t)NVSDK_NGX_DLSS_Hint_Render_Preset_L ||
                                   g_dlssPreset == (uint32_t)NVSDK_NGX_DLSS_Hint_Render_Preset_M))
        Log("pozn.: preset %s pevnou expozici ignoruje (Guide 3.9), bezi s auto expozici - jako InputMode 2",
            PresetName(g_dlssPreset).c_str());
    LogVram("po vytvoreni feature");
    if (!g_dlssVersionTried) {  // verze skutecne nactene knihovny (jednou; NGX ji za behu nemeni)
        g_dlssVersionTried = true;
        g_dlssVersion = LoadedNgxVersion(L"nvngx_dlss.dll", L"dlss", "DLSS");
        Log("DLSS verze: %s", g_dlssVersion.empty() ? "nezjistena" : g_dlssVersion.c_str());
    }
    // zprava o drivejsim (netrvalem) selhani v casti B uz neplati
    if (!strncmp(g_sh->helperMsg, "cast B:", 7)) SetHelperMsg("");
    MemoryBarrier();
    g_sh->dlaaStatus = 1;
}

// ---------------------------------------------------------------------------
// x64 Visual C++ runtime pro DLL AMD. Helper i proxy jsou /MT, ale amd_fidelityfx_*_vk.dll (FSR, FSR FG 2x) potrebuji
// msvcp140.dll, vcruntime140.dll a vcruntime140_1.dll (x64; prelozene linkerem 14.44, starsi runtime nez 14.40 muze
// padat v std::mutex). Hra je 32bitova, Steam instaluje jen runtime x86, takze x64 muze na cistem systemu chybet.
// Pri startu helperu (pred Vulkanem) se nactou:
//   1. z System32 (vc_redist x64), kdyz tam jsou vsechny tri a msvcp140 / vcruntime140 maji verzi aspon 14.44,
//   2. jinak pribalene kopie ze slozky vcrt\ vedle helperu (redistribuovatelne soubory z Visual Studia),
//   3. jinak starsi runtime ze System32 (varovani v logu),
//   4. jinak FSR a FSR FG 2x nejsou k dispozici (log, helperMsg, menu); DLSS, DLSS-G a FSR adaptivni bezi dal.
// Moduly zustanou nactene: DLL AMD pak importy vyresi na uz nactene moduly stejneho jmena (poradi hledani DLL ve
// Windows), takze se nepouzije jina kopie. Testovaci promenna prostredi BMS_DLSS_TEST_VCRT: "local" = jen kopie z
// vcrt\, "none" = jako bez runtime.
// ---------------------------------------------------------------------------
static bool g_vcrtMissing;
static std::string g_vcrtWhy;
static void CheckVcRuntime() {
    static const wchar_t* const kDlls[] = {L"vcruntime140.dll", L"vcruntime140_1.dll", L"msvcp140.dll"};
    const uint64_t kMinVer = (14ull << 48) | (44ull << 32);
    wchar_t test[16] = {};
    GetEnvironmentVariableW(L"BMS_DLSS_TEST_VCRT", test, 16);
    const bool tLocal = !wcscmp(test, L"local"), tNone = !wcscmp(test, L"none");
    if (tLocal || tNone) Log("TEST: runtime Visual C++ x64 = %s (BMS_DLSS_TEST_VCRT)", tLocal ? "jen vcrt\\" : "zadny");
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    bool sysAll = !tLocal && !tNone, sysNew = sysAll, locAll = !tNone;
    for (int i = 0; i < 3; ++i) {
        uint64_t v = 0;
        const std::wstring sp = std::wstring(sys) + L"\\" + kDlls[i];
        if (GetFileAttributesW(sp.c_str()) == INVALID_FILE_ATTRIBUTES) sysAll = sysNew = false;
        else if (i != 1 && (!FileVersion(sp, &v) || v < kMinVer)) sysNew = false;
        if (GetFileAttributesW((g_dir + L"\\vcrt\\" + kDlls[i]).c_str()) == INVALID_FILE_ATTRIBUTES) locAll = false;
    }
    const bool useLocal = !sysNew && locAll;
    if (!sysAll && !useLocal) {
        g_vcrtMissing = true;
        g_vcrtWhy = "chybi Microsoft Visual C++ Redistributable x64";
        Log("CHYBA: %s (System32 ani vcrt\\ vedle helperu nema msvcp140.dll, vcruntime140.dll a vcruntime140_1.dll x64) "
            "- FSR a FSR FG 2x nejdou, DLSS, DLSS-G a FSR adaptivni bezi dal. Oprava: nainstalovat Microsoft Visual C++ "
            "Redistributable x64 (vc_redist.x64.exe od Microsoftu) nebo nakopirovat slozku vcrt z balicku modu.",
            g_vcrtWhy.c_str());
        return;
    }
    for (const wchar_t* d : kDlls) {
        const std::wstring path = useLocal ? g_dir + L"\\vcrt\\" + d : std::wstring(sys) + L"\\" + d;
        HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) {
            g_vcrtMissing = true;
            g_vcrtWhy = "chybi Microsoft Visual C++ Redistributable x64";
            Log("CHYBA: runtime Visual C++ x64: %s nejde nacist (%lu) - FSR a FSR FG 2x nejdou", Narrow(path.c_str()).c_str(),
                GetLastError());
            return;
        }
        wchar_t got[MAX_PATH] = {};
        GetModuleFileNameW(h, got, MAX_PATH);
        uint64_t v = 0;
        FileVersion(got, &v);
        Log("runtime Visual C++ x64: %s %s%s", Narrow(got).c_str(), v ? VersionStr(v).c_str() : "?",
            useLocal ? " (pribalena kopie)" : "");
    }
    if (!sysNew && !useLocal)
        Log("WARN: runtime Visual C++ x64 v System32 je starsi nez 14.44 (DLL AMD jsou prelozene 14.44) - pri potizich "
            "s FSR nainstalovat aktualni Microsoft Visual C++ Redistributable x64");
}

// ---------------------------------------------------------------------------
// FSR (AMD FidelityFX SDK 2.4, ffx API, Vulkan). Podepsane DLL vedle helperu: amd_fidelityfx_loader_vk.dll (ffx API)
// a amd_fidelityfx_upscaler_vk.dll (providery FSR 4.x ML jen pro AMD RDNA, FSR 3.1.x analyticky pro ostatni GPU).
// Vstupy jsou stejne jako u DLSS (barva LDR / linearni HDR, hloubka D24 nebo R32F, MV z mv.comp v pixelech renderu,
// minula - aktualni pozice), vystup do stejnych vlastnich obrazu; alfa, RCAS, zmenseni pri S > 1 a blit jsou spolecne.
// ---------------------------------------------------------------------------
static VkDeviceContext g_ffxDev;  // zarizeni pro ffx API (dotazy bez kontextu i vytvoreni kontextu)

// FSR trvale nedostupne (chybi DLL, podpis, zadna verze): log + helperMsg, dal DLSS nebo kopie.
static bool FsrUnavailable(const char* fmt, ...) {
    char buf[200], msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    snprintf(msg, sizeof msg, "FSR nedostupne: %s", buf);
    Log("FSR NEDOSTUPNE: %s", buf);
    SetHelperMsg(msg);
    g_fsrBroken = true;
    return false;
}

// Hlaseni ffx API (fpMessage, i z jinych vlaken - Log je zamceny)
static void FfxMsg(uint32_t type, const wchar_t* message) {
    std::string m = Narrow(message);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    Log("FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "chyba" : "varovani", m.c_str());
}

// Authenticode podpis DLL: platny (WinVerifyTrust; revokace se nekontroluje a nic se nestahuje) a podepsany AMD
// (CN=Advanced Micro Devices). Ochrana proti podvrzene DLL vedle helperu.
static bool VerifyAmdSignature(const std::wstring& path, std::string* info) {
    WINTRUST_FILE_INFO fi = {sizeof fi};
    fi.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = {sizeof wd};
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    const LONG st = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    std::string signer;
    if (st == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        if (sg && sg->csCertChain && sg->pasCertChain && sg->pasCertChain[0].pCert) {
            wchar_t name[256] = {};
            CertGetNameStringW(sg->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
            signer = Narrow(name);
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    char b[160];
    if (st != ERROR_SUCCESS) {
        snprintf(b, sizeof b, "podpis neplatny nebo chybi (WinVerifyTrust 0x%08lX)", (unsigned long)st);
        *info = b;
        return false;
    }
    *info = "podepsano: " + signer;
    return signer.find("Advanced Micro Devices") != std::string::npos;
}

// DLL AMD vedle helperu: existuje a ([upscale] FsrSignatureCheck) ma platny podpis AMD. Vysledek se pamatuje
// (loader overuje upscaler i frame generation). false = *why rika proc.
static bool FfxCheckDll(const wchar_t* dll, std::string* why) {
    static std::vector<std::pair<std::wstring, std::string>> s_done;  // jmeno -> "" v poradku / duvod
    for (const auto& d : s_done)
        if (d.first == dll) {
            *why = d.second;
            return d.second.empty();
        }
    const std::wstring path = g_dir + L"\\" + dll;
    std::string res;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        res = "chybi " + Narrow(dll) + " vedle helperu";
    } else if (g_cfg.fsrSignatureCheck) {
        std::string info;
        if (!VerifyAmdSignature(path, &info)) res = Narrow(dll) + ": " + info;
        else Log("FSR: %s - %s (platny podpis)", Narrow(dll).c_str(), info.c_str());
    }
    s_done.push_back(std::make_pair(std::wstring(dll), res));
    *why = res;
    return res.empty();
}

// Loader ffx API (amd_fidelityfx_loader_vk.dll; jednou za beh helperu, spolecny pro upscaler i frame generation).
// Volat az po FfxCheckDll loaderu. Loader hleda DLL efektu ve slozce exe a ve sve slozce (obe = slozka helperu).
static const wchar_t* const kFfxLoaderDll = L"amd_fidelityfx_loader_vk.dll";
static bool FfxLoadLoader(std::string* why) {
    static bool s_tried;
    static std::string s_err;
    if (s_tried) {
        *why = s_err;
        return g_ffxCreate != nullptr;
    }
    s_tried = true;
    g_ffxDev = {g_dev, g_pd, vk.vkGetDeviceProcAddr};
    if (!g_cfg.fsrSignatureCheck) Log("WARN: kontrola podpisu DLL FSR je vypnuta ([upscale] FsrSignatureCheck=0)");
    if (g_vcrtMissing) {
        *why = s_err = g_vcrtWhy;
        return false;
    }
    const std::wstring loader = g_dir + L"\\" + kFfxLoaderDll;
    char b[160];
    g_ffxDll = LoadLibraryExW(loader.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_ffxDll) {
        snprintf(b, sizeof b, "LoadLibrary %s selhal (%lu)", Narrow(kFfxLoaderDll).c_str(), GetLastError());
        *why = s_err = b;
        return false;
    }
    if (HMODULE rt = GetModuleHandleW(L"msvcp140.dll")) {  // ktery runtime DLL AMD skutecne pouzivaji
        wchar_t rp[MAX_PATH] = {};
        GetModuleFileNameW(rt, rp, MAX_PATH);
        Log("FSR: loader nacten, runtime Visual C++: %s", Narrow(rp).c_str());
    }
    g_ffxCreate = (PfnFfxCreateContext)GetProcAddress(g_ffxDll, "ffxCreateContext");
    g_ffxDestroy = (PfnFfxDestroyContext)GetProcAddress(g_ffxDll, "ffxDestroyContext");
    g_ffxQuery = (PfnFfxQuery)GetProcAddress(g_ffxDll, "ffxQuery");
    g_ffxDispatch = (PfnFfxDispatch)GetProcAddress(g_ffxDll, "ffxDispatch");
    g_ffxConfigure = (PfnFfxConfigure)GetProcAddress(g_ffxDll, "ffxConfigure");  // jen pro frame generation
    if (!g_ffxCreate || !g_ffxDestroy || !g_ffxQuery || !g_ffxDispatch) {
        g_ffxCreate = nullptr;
        snprintf(b, sizeof b, "%s nema funkce ffx API", Narrow(kFfxLoaderDll).c_str());
        *why = s_err = b;
        return false;
    }
    return true;
}

// Nacte ffx API pro upscaler (jednou za beh helperu) a zjisti verze upscaleru, ktere ffx API na tomto GPU nabizi.
static bool LoadFfx() {
    if (g_ffxTried) return g_ffxUpOk;
    g_ffxTried = true;
    static const wchar_t* const kDlls[] = {kFfxLoaderDll, L"amd_fidelityfx_upscaler_vk.dll"};
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    std::string why;
    for (const wchar_t* dll : kDlls)
        if (!FfxCheckDll(dll, &why)) return FsrUnavailable("%s", why.c_str());
    if (!FfxLoadLoader(&why)) return FsrUnavailable("%s", why.c_str());
    // Verze upscaleru pro toto zarizeni (dotaz bez kontextu; nazvy se kopiruji - ffx API je muze pozdeji prepsat)
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &g_ffxDev;
    uint64_t count = 0;
    q.outputCount = &count;
    ffxReturnCode_t r = g_ffxQuery(nullptr, &q.header);
    if (r != FFX_API_RETURN_OK || count == 0)
        return FsrUnavailable("ffx API nenabizi zadnou verzi upscaleru (kod %u, pocet %llu)", (unsigned)r,
                              (unsigned long long)count);
    std::vector<uint64_t> ids((size_t)count);
    std::vector<const char*> names((size_t)count);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    r = g_ffxQuery(nullptr, &q.header);
    QueryPerformanceCounter(&t1);
    std::string list;
    std::vector<std::string> copies;
    for (uint64_t i = 0; i < count && r == FFX_API_RETURN_OK; ++i) {
        copies.push_back(names[i] ? names[i] : "?");
        char b[96];
        snprintf(b, sizeof b, "%s%s (id 0x%llX)", i ? ", " : "", copies.back().c_str(), (unsigned long long)ids[i]);
        list += b;
    }
    Log("FSR: ffx API nacteno (%.0f ms), verze upscaleru pro toto GPU: %s",
        (t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpf.QuadPart, list.c_str());
    // Zaloha pro GPU, kde ffx API nabidne FSR 4 (ML, AMD RDNA), ale kontext nejde vytvorit (helper nezapina rozsireni,
    // ktera FSR 4 muze chtit): prvni (= nejnovejsi) verze 3.1.x
    for (size_t i = 0; i < copies.size() && !g_fsr31Id; ++i)
        if (!copies[i].compare(0, 4, "3.1.") || !copies[i].compare(0, 8, "FSR 3.1.")) {
            g_fsr31Id = ids[i];
            g_fsr31Name = copies[i];
        }
    if (!g_cfg.fsrVersion.empty()) {
        for (size_t i = 0; i < copies.size() && !g_fsrVersionId; ++i)
            if (copies[i].find(g_cfg.fsrVersion) != std::string::npos) {
                g_fsrVersionId = ids[i];
                Log("FSR: [upscale] FsrVersion=%s -> %s", g_cfg.fsrVersion.c_str(), copies[i].c_str());
            }
        if (!g_fsrVersionId)
            Log("WARN: [upscale] FsrVersion=%s mezi verzemi neni - verzi vybere ffx API", g_cfg.fsrVersion.c_str());
    }
    g_ffxUpOk = true;
    return true;
}

// Rezim FSR podle pomeru vystup / vstup jako hodnota NVSDK_NGX_PerfQuality_Value (pro proxy a log, stejne jako DLSS)
static int32_t PerfFromRatio(double r) {
    if (r < 1.05) return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (r < 1.6) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (r < 1.85) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (r < 2.5) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

static const char* FsrModeName(int32_t perf) {
    return perf == NVSDK_NGX_PerfQuality_Value_DLAA ? "Native AA" : PerfName(perf);
}

// FSR kontext: vstup (render) g_rW x g_rH, vystup podle tryScale - stejna pravidla jako CreateDlss (S > 1 jen v casti A,
// v casti B vystup W x H; kdyz S > 1 nejde, bezi 1.0x). FSR nema pevne rezimy: bere libovolny pomer 1.0 az 3.0
// (Native AA = 1.0, Quality 1.5, Balanced 1.7, ...). GPU nic nezpracovava, zadna feature ani kontext neexistuje.
// false = FSR pro tuto konfiguraci nejde (trvale jen kdyz LoadFfx nastavilo g_fsrBroken).
static bool CreateFsr(float tryScale) {
    g_hdrMode = BMSD_INPUT_LDR;
    g_outScale = 1.0f;
    g_outW = g_w;
    g_outH = g_h;
    g_perfQuality = -1;
    if (!LoadFfx()) return false;
    const bool sub = g_rW != g_w || g_rH != g_h;
    if (!g_ownOk) return Log("FSR: chybi compute shader nebo vlastni obrazy (viz vyse)"), false;
    if (g_rW < 32 || g_rH < 32) return Log("FSR: vstup %ux%u je moc maly", g_rW, g_rH), false;
    // Vstup FSR nezavisi na InputMode (Alt+Insert, ladeni DLSS): harness (test fsr hdr) - LDR (sRGB tak, jak je) je
    // nejblize referenci (26,4 dB), linearni vstup 20,1 dB (presvetlene hrany), inverzni tone mapping (InputMode 3)
    // s HDR 14,1 dB (auto expozice FSR rusi kompresi ITM, tenke svetle cary tloustnou). [upscale] FsrInput=1 =
    // linearni vstup (jako InputMode 2) pro pokusy.
    const uint32_t wantMode = g_cfg.fsrLinear ? BMSD_INPUT_HDR_AUTO : BMSD_INPUT_LDR;
    const float reqScale = SanitizeScale(g_rW == g_w && g_rH == g_h ? tryScale : 1.0f);
    float scale = reqScale;
    std::string upWhy;
    if (scale > 1.0f && !g_upOk) {
        upWhy = "chybi shader zmenseni (viz vyse)";
        scale = 1.0f;
    }
    if (sub && SanitizeScale(tryScale) > 1.0f && !g_subScaleLogged) {
        g_subScaleLogged = true;
        Log("cast B: OutputScale %sx se pri zmensenem renderu nepouziva (vystup FSR = %ux%u), plati zase pri plnem "
            "renderu",
            ScaleStr(SanitizeScale(tryScale)).c_str(), g_w, g_h);
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    uint32_t mode = wantMode, outW = g_w, outH = g_h, flags = 0;
    double ratio = 1.0;
    bool used31 = false;  // tento kontext vznikl se zalohou 3.1 (jen pro tento pokus, S5: ne globalne)
    for (;;) {
        const bool up = scale > 1.0f;
        outW = up ? (uint32_t)lround((double)g_w * scale) : g_w;
        outH = up ? (uint32_t)lround((double)g_h * scale) : g_h;
        ratio = std::max((double)outW / g_rW, (double)outH / g_rH);
        if (outW > 16384 || outH > 16384 || ratio > 3.0001) {  // FSR: nejvyse Ultra Performance (3.0x)
            char b[128];
            snprintf(b, sizeof b, "vstup %ux%u -> vystup %ux%u (pomer %.3f) FSR neumi (max 3.0)", g_rW, g_rH, outW,
                     outH, ratio);
            upWhy = b;
            if (up) {
                scale = 1.0f;
                continue;
            }
            return Log("FSR: %s", b), false;
        }
        // Obrazy velikosti vystupu, ktere tento pokus nepouzije, uvolnit pred vytvorenim kontextu (jako u DLSS)
        if (!(up && wantMode == BMSD_INPUT_LDR)) DestroyBig();
        if (wantMode == BMSD_INPUT_LDR) DestroyHdr();
        mode = wantMode;
        if (mode != BMSD_INPUT_LDR && !EnsureHdr(outW, outH)) {
            Log("vstup FSR: %s nejde - pouzivam LDR", InputName(mode));
            mode = BMSD_INPUT_LDR;
        }
        if (up && mode == BMSD_INPUT_LDR && !EnsureBig(outW, outH)) {
            upWhy = "obraz vystupu " + std::to_string(outW) + "x" + std::to_string(outH) + " nejde vytvorit";
            scale = 1.0f;
            continue;
        }
        // Auto expozice vzdy (FSR ji stejne pouziva pro zamky, vystup se expozici zase deli; pevna 1.0 dava v harnessu
        // bajtove stejny obraz). Bez HDR (barva 0..1). Hloubka D3D9: 0 = near, 1 = far (neinvertovana, konecna far).
        flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        // LDR = barva v gamma prostoru (sRGB) tak, jak ji kreslila hra: FSR to musi vedet, jinak pocita expozici,
        // zamky jasu a michani historie, jako by byla linearni (S4; ffx_upscale.h NON_LINEAR_COLORSPACE + v dispatchi
        // FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB). [upscale] FsrNonLinear=0 = bez priznaku (stav pred 2026-11-19).
        if (mode == BMSD_INPUT_LDR && g_cfg.fsrNonLinear) flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        if (g_cfg.fsrDebug) flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
        ffxCreateBackendVKDesc be = {};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.deviceContext = g_ffxDev;
        ffxOverrideVersion ov = {};
        ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ov.versionId = g_fsrVersionId;
        ffxCreateContextDescUpscaleVersion ver = {};
        ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        ver.version = FFX_UPSCALER_VERSION;
        ffxCreateContextDescUpscale cd = {};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.flags = flags;
        cd.maxRenderSize = {g_rW, g_rH};
        cd.maxUpscaleSize = {outW, outH};
        cd.fpMessage = FfxMsg;
        cd.header.pNext = &ver.header;
        ver.header.pNext = &be.header;
        if (g_fsrVersionId) be.header.pNext = &ov.header;
        g_fsr = nullptr;
        ffxReturnCode_t r = g_ffxCreate(&g_fsr, &cd.header, nullptr);
        if ((r != FFX_API_RETURN_OK || !g_fsr) && !g_fsrVersionId && g_fsr31Id) {
            // verze vybrana ffx API nejde (napr. FSR 4 bez rozsireni zarizeni) - zkusit FSR 3.1.x
            Log("FSR: ffxCreateContext s verzi vybranou ffx API selhal (kod %u) - zkousim %s", (unsigned)r,
                g_fsr31Name.c_str());
            g_fsr = nullptr;
            ov.versionId = g_fsr31Id;
            be.header.pNext = &ov.header;
            r = g_ffxCreate(&g_fsr, &cd.header, nullptr);
            if (r == FFX_API_RETURN_OK && g_fsr) {
                used31 = true;  // dalsi kontext zkusi zase nejdriv verzi od ffx API (napr. FSR 4 pri S = 1)
                break;
            }
        }
        if (r == FFX_API_RETURN_OK && g_fsr) break;
        g_fsr = nullptr;
        char b[160];
        snprintf(b, sizeof b, "ffxCreateContext (vstup %ux%u -> vystup %ux%u, flags 0x%X) selhal: kod %u", g_rW, g_rH,
                 outW, outH, flags, (unsigned)r);
        Log("FSR: %s", b);
        if (up) {
            upWhy = b;
            scale = 1.0f;
            continue;
        }
        DestroyBig();
        DestroyHdr();
        return false;
    }
    QueryPerformanceCounter(&t1);
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const ffxReturnCode_t qr = g_ffxQuery(&g_fsr, &pv.header);
    g_fsrVersionName = qr == FFX_API_RETURN_OK && pv.versionName ? pv.versionName : "?";
    if (g_fsrVersionName.compare(0, 3, "FSR")) g_fsrVersionName = "FSR " + g_fsrVersionName;
    // Obrazy, ktere tato konfigurace nepotrebuje (GPU stoji)
    if (mode == BMSD_INPUT_LDR) DestroyHdr();
    if (!(scale > 1.0f && mode == BMSD_INPUT_LDR)) DestroyBig();
    g_hdrMode = mode;
    g_fsrNonLinFlags = (flags & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) != 0;
    g_outScale = scale;
    g_outW = outW;
    g_outH = outH;
    g_perfQuality = PerfFromRatio(ratio);
    g_dlssFresh = true;
    g_dlaaFrames = 0;
    g_fsrLoggedNear = -1.0f;  // parametry kamery znovu do logu
    Log("FSR verze: %s (id 0x%llX, %s)", g_fsrVersionName.c_str(), (unsigned long long)pv.versionId,
        used31 ? "zaloha 3.1 - verze od ffx API nesla" : !g_fsrVersionId ? "vybrala ffx API" : "[upscale] FsrVersion");
    Log("FSR kontext (%s) vytvoren: vstup %ux%u%s -> vystup %ux%u, flags 0x%X, %.0f ms", FsrModeName(g_perfQuality),
        g_rW, g_rH, sub ? " (sub-rect, cast B)" : "", outW, outH, flags,
        (t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpf.QuadPart);
    Log("vystup FSR: %ux%u (%sx, %s, pomer %.3f)", outW, outH, ScaleStr(scale).c_str(), FsrModeName(g_perfQuality),
        ratio);
    if (reqScale > 1.0f && scale == 1.0f) {
        char msg[256];
        snprintf(msg, sizeof msg, "vystup FSR %sx nejde: %s - bezi 1.0x (Native AA)", ScaleStr(reqScale).c_str(),
                 upWhy.c_str());
        Log("%s", msg);
        SetHelperMsg(msg);
    }
    if (mode == wantMode)
        Log("vstup FSR: %s ([upscale] FsrInput=%d; InputMode %u plati jen pro DLSS)",
            mode ? "linearni" : g_fsrNonLinFlags ? "LDR (sRGB, priznak NON_LINEAR_COLOR_SRGB)" : "LDR (sRGB, bez priznaku)",
            (int)g_cfg.fsrLinear, g_dlssInputMode);
    else
        Log("vstup FSR: LDR (sRGB) - linearni vstup nejde (viz vyse)");
    LogVram("po vytvoreni FSR kontextu");
    if (!strncmp(g_sh->helperMsg, "cast B:", 7)) SetHelperMsg("");
    MemoryBarrier();
    g_sh->dlaaStatus = 1;
    return true;
}

// Zadny upscaler nebezi (vypnuto nebo vse selhalo): rezim DLAA jen kopiruje, proxy nevynucuje jitter.
static void NoUpscaler(const char* msg) {
    Log("%s", msg);
    SetHelperMsg(msg);
    MemoryBarrier();
    g_sh->dlaaStatus = -1;
}

// Upscaler podle pozadavku proxy (BmsdShared::upscaler): vytvori DLSS feature nebo FSR kontext. Poradi pri selhani:
// pozadovany -> DLSS -> FSR. Z DLSS se na FSR prechazi jen kdyz je DLSS trvale nedostupne (g_dlaaBroken, napr. GPU
// bez DLSS); netrvale selhani DLSS (cast B, SubFail) zustava jako dosud (proxy nic nedela, roztahuje engine).
// XeSS zatim neni (bere se jako nedostupny). Vola se misto CreateDlss: import, zmena presetu / vstupu / S / renderu /
// upscaleru, fallback po selhanem vyhodnoceni. GPU nic nezpracovava, zadna feature ani kontext neexistuje.
static void CreateUpscaler(float tryScale) {
    g_upReqRaw = g_sh->upscaler;
    g_upReqValid = g_sh->upscalerValid;
    const uint32_t req = !g_upReqValid ? BMSD_UPSCALER_DLSS
                         : g_upReqRaw < BMSD_UPSCALER_COUNT ? g_upReqRaw : BMSD_UPSCALER_DLSS;
    // auto (od verze 13, vychozi): DLSS, kdyz jde, jinak FSR - bez hlasky o nahradnim upscaleru
    const bool autoSel = req == BMSD_UPSCALER_AUTO;
    g_dlssPreset = g_sh->preset;
    g_dlssInputMode = g_sh->inputMode;
    g_hdrMode = BMSD_INPUT_LDR;
    g_outScale = 1.0f;
    g_outW = g_w;
    g_outH = g_h;
    g_perfQuality = -1;
    g_upActive = BMSD_UPSCALER_OFF;
    const uint32_t logKey = g_upReqValid ? g_upReqRaw : 0x100u;
    if (logKey != g_loggedUpReq) {
        g_loggedUpReq = logKey;
        if (!g_upReqValid)
            Log("upscaler: DLSS (proxy volbu upscaleru neposila)");
        else if (g_upReqRaw >= BMSD_UPSCALER_COUNT)
            Log("upscaler: neplatna hodnota %u - pouzivam DLSS", g_upReqRaw);
        else
            Log("upscaler: pozadovano %s", UpscalerName(req));
    }
    if (req == BMSD_UPSCALER_OFF) {
        DestroyBig();
        DestroyHdr();
        return NoUpscaler("upscaler vypnut ([upscale] Upscaler=off) - most jen kopiruje");
    }
    uint32_t order[3];
    int n = 0;
    if (req == BMSD_UPSCALER_XESS)
        Log("upscaler: XeSS zatim neni implementovany - zkousim DLSS, pak FSR");
    else if (!autoSel)
        order[n++] = req;
    for (uint32_t c : {(uint32_t)BMSD_UPSCALER_DLSS, (uint32_t)BMSD_UPSCALER_FSR})
        if (c != req) order[n++] = c;
    for (int i = 0; i < n && g_upActive == BMSD_UPSCALER_OFF; ++i) {
        if (order[i] == BMSD_UPSCALER_DLSS) {
            CreateDlss(tryScale);  // pri trvale nedostupnem DLSS hned konci
            if (g_dlss) {
                g_upActive = BMSD_UPSCALER_DLSS;
                break;
            }
            if (!g_dlaaBroken && g_ngxParams) break;  // netrvale selhani (cast B) - dalsi upscaler nezkouset
        } else if (!g_fsrBroken && !g_fsrSuspended && CreateFsr(tryScale)) {
            g_upActive = BMSD_UPSCALER_FSR;
        }
    }
    if (g_upActive == req) return;
    if (autoSel && g_upActive != BMSD_UPSCALER_OFF) {
        static uint32_t s_loggedAuto = UINT32_MAX;
        if (s_loggedAuto != g_upActive) {
            s_loggedAuto = g_upActive;
            Log("upscaler: auto -> %s%s", g_upActive == BMSD_UPSCALER_FSR ? g_fsrVersionName.c_str() : "DLSS",
                g_upActive == BMSD_UPSCALER_FSR ? " (DLSS na tomto systemu neni k dispozici)" : "");
        }
        if (!strncmp(g_sh->helperMsg, "upscaler ", 9)) SetHelperMsg("");  // hlaska z drivejsi vyslovne volby uz neplati
        return;
    }
    if (g_upActive != BMSD_UPSCALER_OFF) {
        char msg[256];
        snprintf(msg, sizeof msg, "upscaler %s nejde - bezi %s (duvod v bms_dlss_helper.log)", UpscalerName(req),
                 g_upActive == BMSD_UPSCALER_FSR ? g_fsrVersionName.c_str() : "DLSS");
        Log("%s", msg);
        SetHelperMsg(msg);
        return;
    }
    // Nic nebezi. DLSS pouzitelne = netrvale selhani v casti B: dlaaStatus zustava, helperRenderOk = 0 (engine).
    DestroyBig();
    DestroyHdr();
    if (!g_dlaaBroken && g_ngxParams) return;
    char msg[256];
    snprintf(msg, sizeof msg, "zadny upscaler nejde (pozadovano %s; duvody v bms_dlss_helper.log) - most jen kopiruje",
             UpscalerName(req));
    NoUpscaler(msg);
}

// Pred zmenou feature nebo obrazu: dobehnout vlastni praci na GPU s limitem (kGpuTimeoutNs), vkDeviceWaitIdle pak
// uz jen potvrdi. false = GPU nedobehla (napr. semafor od proxy po ztrate zarizeni hry neprijde) -> g_fatal, helper
// skonci a proxy most vypne (misto nekonecneho cekani).
static void DeviceWaitIdleLocked() {
    VkResult r;
    {
        std::lock_guard<std::mutex> a(g_queueMx);
        if (g_presQMx != &g_queueMx) {
            std::lock_guard<std::mutex> b(*g_presQMx);
            r = vk.vkDeviceWaitIdle(g_dev);
        } else {
            r = vk.vkDeviceWaitIdle(g_dev);
        }
    }
    if (r != VK_SUCCESS) Log("WARN: vkDeviceWaitIdle -> VkResult %d%s", (int)r, r == VK_ERROR_DEVICE_LOST ? " (zarizeni helperu ztraceno)" : "");
}

static bool DrainGpu(const char* why) {
    for (int i = 0; i < kSlots; ++i) {
        if (!g_fenceUsed[i]) continue;
        const VkResult r = vk.vkWaitForFences(g_dev, 1, &g_fence[i], VK_TRUE, kGpuTimeoutNs);
        if (r != VK_SUCCESS) {
            g_fatal = true;
            Error("%s: rozpracovany snimek nedobehl na GPU (VkResult %d%s)", why, (int)r,
                  r == VK_TIMEOUT ? ", limit 5 s - ztracene zarizeni hry?" : "");
            return false;
        }
    }
    DeviceWaitIdleLocked();
    return true;
}

// Proxy zmenila preset (Alt+F9), vstup DLSS (Alt+Insert), vystup DLSS (Alt+F3), render (cast B, zmena
// mat_viewportscale) nebo upscaler (menu, od verze 11): pockat na GPU, uvolnit feature / kontext a vytvorit znovu
// (obrazy velikosti vystupu si CreateDlss / CreateFsr prizpusobi, deskriptory prepise). Zmena renderu nic neimportuje
// ani nevytvari vlastni obrazy (vsechny W x H).
static void RecreateDlss() {
    const uint32_t p = g_sh->preset, im = g_sh->inputMode;
    const float os = g_sh->outScale;
    const uint32_t rw = g_sh->renderWidth, rh = g_sh->renderHeight;
    if (g_fsrSuspended) {  // S5: po docasnem selhani FSR zkusit znovu pri kazde zmene od proxy (menu, klavesy)
        g_fsrSuspended = false;
        Log("FSR: po selhani vyhodnoceni (%d/%d) zkousim znovu pri teto zmene", g_fsrEvalFails, kFsrMaxEvalFails);
    }
    if (g_sh->upscaler != g_upReqRaw || g_sh->upscalerValid != g_upReqValid)
        Log("zmena upscaleru: %s -> %s (bezel %s)", g_upReqValid ? UpscalerName(g_upReqRaw) : "DLSS (bez volby)",
            g_sh->upscalerValid ? UpscalerName(g_sh->upscaler) : "DLSS (bez volby)", UpscalerName(g_upActive));
    // Novy render plati az po vypsani casu dosavadni feature (TimingCollectAll pise i dosavadni render)
    auto applyRender = [&]() {
        if (rw == g_rReqW && rh == g_rReqH) return;
        const uint32_t ow = g_rW, oh = g_rH;
        SetRender(rw, rh);
        Log("zmena renderu (vstup DLSS): %ux%u -> %ux%u v obrazech %ux%u%s", ow, oh, g_rW, g_rH, g_w, g_h,
            g_rW != g_w || g_rH != g_h ? " (cast B)" : "");
    };
    if (p != g_dlssPreset) Log("zmena presetu: %s -> %s", PresetName(g_dlssPreset).c_str(), PresetName(p).c_str());
    if (im != g_dlssInputMode) Log("zmena vstupu DLSS: %s -> %s", InputName(g_dlssInputMode), InputName(im));
    if (os != g_outScaleReq)
        Log("zmena vystupu DLSS: pozadovano %sx -> %sx", ScaleStr(SanitizeScale(g_outScaleReq)).c_str(),
            ScaleStr(SanitizeScale(os)).c_str());
    g_outScaleReq = os;
    // Bez vlastnich obrazu (nebo jeste pred importem) - jen zapamatovat; CreateUpscaler jen ohlasi selhani podle casti
    // (A trvale, B SubFail), GPU nic nedela. Jinak (i bez feature po selhani v casti B nebo pri trvale nedostupnem
    // DLSS - muze jit FSR) se zkousi znovu.
    if (!g_dlss && !g_fsr && !g_ownOk) {
        applyRender();
        CreateUpscaler(g_outScaleReq);
        return;
    }
    if (!DrainGpu("zmena feature DLSS")) return;
    TimingCollectAll();  // casy patri jeste k dosavadni feature
    applyRender();
    ReleaseDlss();
    ReleaseFsr();
    CreateUpscaler(g_outScaleReq);
}

// Vyhodnoceni DLSS pri S > 1 selhalo (ProcessFrame, snimek uz odeslan jako kopie): bez supersamplingu dal.
// Pozadavek proxy zustava potvrzeny; znovu se zkusi az pri dalsi zmene S nebo reimportu.
static void FallbackToNative() {
    const float was = g_outScale;
    const uint32_t ow = g_outW, oh = g_outH;
    const char* name = g_upActive == BMSD_UPSCALER_FSR ? "FSR" : "DLSS";
    if (!DrainGpu("prechod na 1.0x")) return;
    TimingCollectAll();
    ReleaseDlss();
    ReleaseFsr();
    DestroyBig();  // vystup S > 1 uvolnit pred vytvorenim feature S = 1 (selhani byva nedostatek VRAM)
    CreateUpscaler(1.0f);
    char msg[256];
    snprintf(msg, sizeof msg, "vyhodnoceni %s s vystupem %ux%u (%sx) selhalo - bezi 1.0x", name, ow, oh,
             ScaleStr(was).c_str());
    Log("%s", msg);
    if (g_dlss || g_fsr) SetHelperMsg(msg);
}

// Cast B: vyhodnoceni DLSS selhalo (ProcessFrame, snimek uz odesel roztazeny blitem): feature uvolnit, dal bez DLSS
// (helperRenderOk = 0 -> proxy most nespousti, roztahuje engine). Neni trvale, viz SubFail.
static void SubEvalFail() {
    if (!DrainGpu("cast B bez DLSS")) return;
    TimingCollectAll();
    ReleaseDlss();
    ReleaseFsr();
    DestroyHdr();  // RGBA16F vstupu HDR nedrzet (CreateDlss je pri dalsim pokusu vytvori znovu)
    g_perfQuality = -1;
    SubFail("vyhodnoceni %s selhalo (viz vyse)", g_upActive == BMSD_UPSCALER_FSR ? "FSR" : "DLSS");
}

// Vyhodnoceni FSR selhalo (S = 1, cast A; snimek uz odesel jako kopie): FSR uvolnit a zkusit dalsi upscaler (DLSS).
// Netrvale (S5): prechodny nedostatek pameti nebo deskriptoru nesmi FSR vypnout do restartu hry (na AMD by pak
// nebezel zadny upscaler). FSR se zkusi znovu pri dalsi zmene od proxy (RecreateDlss); trvale vypnuti az po
// kFsrMaxEvalFails selhanich za sebou.
static void FsrEvalFail() {
    if (!DrainGpu("FSR selhalo")) return;
    TimingCollectAll();
    ReleaseFsr();
    if (++g_fsrEvalFails >= kFsrMaxEvalFails) {
        FsrUnavailable("vyhodnoceni (ffxDispatch) selhalo %dx za sebou - zkousim dalsi upscaler", g_fsrEvalFails);
    } else {
        char msg[200];
        snprintf(msg, sizeof msg,
                 "FSR: vyhodnoceni (ffxDispatch) selhalo (%d/%d) - docasne jiny upscaler, FSR znovu pri dalsi zmene "
                 "nastaveni (menu)",
                 g_fsrEvalFails, kFsrMaxEvalFails);
        Log("%s", msg);
        SetHelperMsg(msg);
        g_fsrSuspended = true;
    }
    CreateUpscaler(g_outScaleReq);
}

// Vyhodnoceni DLSS selhalo (S = 1, cast A; snimek uz odesel jako kopie, DlaaFail uz DLSS trvale vypnulo): feature
// uvolnit (nedrzet VRAM) a zkusit FSR, symetricky s FsrEvalFail (D1). CreateUpscaler pri g_dlaaBroken DLSS preskoci.
static void DlssEvalFail() {
    if (!DrainGpu("DLSS selhalo")) return;
    TimingCollectAll();
    ReleaseDlss();
    CreateUpscaler(g_outScaleReq);
}

static bool ImportResources() {
    if (!DrainGpu("reimport zdroju")) return false;
    {  // test pomaleho importu (proxy na nej nesmi blokovat hru ani most vypnout): BMS_DLSS_TEST_SLOWIMPORT=ms
        static int s_slow = -1;
        if (s_slow < 0) {
            wchar_t t[16] = L"";
            s_slow = GetEnvironmentVariableW(L"BMS_DLSS_TEST_SLOWIMPORT", t, 16) ? _wtoi(t) : 0;
        }
        if (s_slow > 0) {
            Log("test: BMS_DLSS_TEST_SLOWIMPORT - import pozdrzen o %d ms", s_slow);
            Sleep((DWORD)s_slow);
        }
    }
    TimingCollectAll();  // casy dosavadni feature (pred vynulovanim fence)
    // Po vkDeviceWaitIdle jsou pouzite fence signalizovane: vynulovat, jinak by je dalsi
    // vkQueueSubmit dostal signalizovane (VUID-vkQueueSubmit-fence-00063).
    for (int i = 0; i < kSlots; ++i)
        if (g_fenceUsed[i]) {
            vk.vkResetFences(g_dev, 1, &g_fence[i]);
            g_fenceUsed[i] = false;
        }
    if (g_fgPresenter) FgOnBridgeReimport();  // DLSS-G feature a snapshoty patri ke stare velikosti sceny
    DestroySized();
    for (Img& im : g_img) {
        if (im.img) vk.vkDestroyImage(g_dev, im.img, nullptr);
        if (im.mem) vk.vkFreeMemory(g_dev, im.mem, nullptr);
        im = Img();
    }
    if (!g_semP) {
        if (!ImportSemaphore(g_sh->semProxyHandle, &g_semP) || !ImportSemaphore(g_sh->semHelperHandle, &g_semH))
            return Error("import semaforu selhal"), false;
        Log("semafory importovany");
    }
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    for (int i = 0; i < BMSD_IMG_COUNT; ++i) {
        const BmsdImage& bi = g_sh->img[i];
        Img& im = g_img[i];
        if (i == BMSD_IMG_DEPTH_IN && !IsBridgeDepthFormat((VkFormat)bi.format))
            return Error("nepodporovany format hloubky sceny %d", (int)bi.format), false;
        im.aspect = IsBridgeDepthFormat((VkFormat)bi.format)
                        ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                        : VK_IMAGE_ASPECT_COLOR_BIT;
        VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, ht};
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = (VkFormat)bi.format;
        ci.extent = {bi.width, bi.height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = bi.usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vk.vkCreateImage(g_dev, &ci, nullptr, &im.img);
        if (r != VK_SUCCESS) return Error("vkCreateImage %d selhal %d", i, r), false;
        VkMemoryRequirements req = {};
        vk.vkGetImageMemoryRequirements(g_dev, im.img, &req);
        if (req.size > bi.allocSize) {  // konec obrazu by lezel mimo importovanou pamet (MMU fault pri cteni)
            CloseHandle((HANDLE)(uintptr_t)bi.handle);
            return Error("obraz %d %ux%u: helper potrebuje %llu B, proxy sdili jen %llu B", i, bi.width, bi.height,
                         (unsigned long long)req.size, (unsigned long long)bi.allocSize), false;
        }
        uint32_t type = DeviceLocalType(req.memoryTypeBits);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, im.img};
        VkImportMemoryWin32HandleInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded, ht,
                                                (HANDLE)(uintptr_t)bi.handle};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, bi.allocSize, type};
        r = vk.vkAllocateMemory(g_dev, &ai, nullptr, &im.mem);
        CloseHandle((HANDLE)(uintptr_t)bi.handle);
        if (r != VK_SUCCESS) return Error("import pameti obrazu %d selhal %d (req %llu, sdileno %llu)", i, r,
                                          (unsigned long long)req.size, (unsigned long long)bi.allocSize), false;
        r = vk.vkBindImageMemory(g_dev, im.img, im.mem, 0);
        if (r != VK_SUCCESS) return Error("bind obrazu %d selhal %d", i, r), false;
        im.w = bi.width;
        im.h = bi.height;
    }
    g_w = g_sh->width;
    g_h = g_sh->height;
    if ((VkFormat)g_sh->img[BMSD_IMG_DEPTH_IN].format != g_depthFmt || g_sh->generation == 1)
        Log("hloubka sceny: %s (Vulkan format %d)",
            g_sh->img[BMSD_IMG_DEPTH_IN].format == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32S8" : "D24S8",
            (int)g_sh->img[BMSD_IMG_DEPTH_IN].format);
    g_depthFmt = (VkFormat)g_sh->img[BMSD_IMG_DEPTH_IN].format;
    g_inW = g_w;  // vlastni vstupni obrazy (MV, linearni barva, hloubka R32F) maji vzdy plnou velikost
    g_inH = g_h;
    // Render (vstup DLSS): cast A = cely sdileny obraz, cast B = sub-rect. Proxy ho zapisuje pred generation,
    // feature tak vznikne rovnou s nim.
    SetRender(g_sh->renderWidth, g_sh->renderHeight);
    Log("obrazy importovany: %ux%u (generace %d)", g_w, g_h, g_sh->generation);
    if (g_rW != g_w || g_rH != g_h) Log("render (vstup DLSS): %ux%u v obrazech %ux%u (cast B)", g_rW, g_rH, g_w, g_h);

    // Nefatalni: bez nich jen nejde DLAA a ladeni MV. S posila proxy spolu s novymi zdroji (pred generation),
    // feature tak vznikne rovnou s aktualnim S.
    if (g_computeOk && !CreateSized()) DestroySized();
    if (g_sh->outScale != g_outScaleReq)
        Log("zmena vystupu DLSS: pozadovano %sx -> %sx", ScaleStr(SanitizeScale(g_outScaleReq)).c_str(),
            ScaleStr(SanitizeScale(g_sh->outScale)).c_str());
    g_outScaleReq = g_sh->outScale;
    CreateUpscaler(g_outScaleReq);
    return true;
}

static VkImageMemoryBarrier IB(VkImage img, VkImageAspectFlags aspect, VkAccessFlags src, VkAccessFlags dst,
                               VkImageLayout oldL, VkImageLayout newL, uint32_t srcQ = VK_QUEUE_FAMILY_IGNORED,
                               uint32_t dstQ = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = srcQ;
    b.dstQueueFamilyIndex = dstQ;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    return b;
}

static void PB(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, uint32_t n,
               const VkImageMemoryBarrier* b) {
    vk.vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, n, b);
}

// ---------------------------------------------------------------------------
// Reprojekce clip -> clip minuleho snimku: prevCam * inverse(cam), v double.
// Matice jsou po radcich (registry c8..c11): clip_i = dot(radek_i, (x, y, z, 1)).
// ---------------------------------------------------------------------------
static bool Invert4(const double* m, double* inv) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-300) return false;
        if (piv != col)
            for (int c = 0; c < 8; ++c) {
                double t = a[col][c];
                a[col][c] = a[piv][c];
                a[piv][c] = t;
            }
        double d = 1.0 / a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] *= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col || a[r][col] == 0.0) continue;
            double f = a[r][col];
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) inv[r * 4 + c] = a[r][c + 4];
    return true;
}

static bool BuildReprojection(const float* cam, const float* prevCam, float* out) {
    double C[16], P[16], Ci[16];
    for (int i = 0; i < 16; ++i) {
        C[i] = cam[i];
        P[i] = prevCam[i];
    }
    if (!Invert4(C, Ci)) return false;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += P[r * 4 + k] * Ci[k * 4 + c];
            out[r * 4 + c] = (float)s;
        }
    return true;
}

// Pohyb kamery mezi snimky pro detekci strihu (Programming Guide 3.13): posun oka v jednotkach
// sveta a zmena smeru pohledu ve stupnich. Oko je bod, kde clip x = y = w = 0, tedy
// inverse(cam) * (0, 0, 1, 0); smer pohledu je linearni cast radku w (u perspektivy w = hloubka).
static bool CameraMotion(const float* cam, const float* prevCam, double* dist, double* angleDeg) {
    double C[16], P[16], Ci[16], Pi[16];
    for (int i = 0; i < 16; ++i) {
        C[i] = cam[i];
        P[i] = prevCam[i];
    }
    if (!Invert4(C, Ci) || !Invert4(P, Pi)) return false;
    if (fabs(Ci[14]) < 1e-12 || fabs(Pi[14]) < 1e-12) return false;
    double d2 = 0.0, dot = 0.0, lc = 0.0, lp = 0.0;
    for (int k = 0; k < 3; ++k) {
        double e = Ci[k * 4 + 2] / Ci[14] - Pi[k * 4 + 2] / Pi[14];
        d2 += e * e;
        dot += C[12 + k] * P[12 + k];
        lc += C[12 + k] * C[12 + k];
        lp += P[12 + k] * P[12 + k];
    }
    if (lc <= 0.0 || lp <= 0.0) return false;
    double c = dot / sqrt(lc * lp);
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    *dist = sqrt(d2);
    *angleDeg = acos(c) * 57.29577951308232;
    return true;
}

struct MvPush {  // musi odpovidat bloku push_constant v mv.comp (std430)
    float rows[16];
    float size[2];
    float jitter[2];
    float debugScale;
    uint32_t valid;
    float vmDepth;
    uint32_t objMode;   // BMSD_OBJMV_* (0 = OBJMV_IN necist)
    float objTol;
    uint32_t statBase;  // zacatek pocitadel slotu v bufferu (pocet uint)
    float prevJitter[2];  // jitter minuleho snimku (ViewmodelCheck)
    uint32_t vmCheck;     // bit 0 = zapsat jas do vazby 14[bit 2], bit 1 = cist minuly z 13[bit 2 ^ 1]
};
static_assert(sizeof(MvPush) == 116, "MvPush");

struct DownPush {  // musi odpovidat bloku push_constant v downsample.comp (std430)
    float ratio[2];    // velikost vystupu DLSS / finalni velikost po osach
    float k;           // HDR_IN: sila inverzniho tone mappingu
    uint32_t flags;    // bit 0: orezat prekmit (s doostrenim), bit 1: filtrovat v linearnim svetle
    uint32_t kernel;   // [upscale] DownsampleFilter
};
static_assert(sizeof(DownPush) == 20, "DownPush");

// Push konstanty alpha.comp (a = k inverzniho tone mappingu, jen HDR_IN) a sharpen.comp (a = con): std430 float na
// offsetu 0, vec2 na offsetu 8. scale = velikost vstupu (sub-rect COLOR_IN) / velikost vystupu: odkud cist alfu.
struct AlphaPush {
    float a;
    float pad;
    float scale[2];
};
static_assert(sizeof(AlphaPush) == 16, "AlphaPush");

// Parametry kamery pro FSR (prevod hloubky na vzdalenost: disokluze, rychlost, zamky) z cViewProj bez jitteru. Radky
// x, y, z, w (registry c8..c11), perspektiva D3D LH: z_ndc = A + B / w_view (0 = near, 1 = far). Radek w je smer
// pohledu (normalizuje se na delku 1), radek z = A * w + (0, 0, 0, B): near = -B / A, far = B / (1 - A); svisly FOV
// z delky radku y (P11 = 1 / tan(fovY / 2)). false = neni to ocekavana perspektiva (pak FSR dostane zalohu).
static bool CameraNearFarFov(const float* cam, float* nearZ, float* farZ, float* fovY) {
    double lw = 0.0, A = 0.0, ly = 0.0;
    for (int k = 0; k < 3; ++k) {
        lw += (double)cam[12 + k] * cam[12 + k];
        A += (double)cam[8 + k] * cam[12 + k];
        ly += (double)cam[4 + k] * cam[4 + k];
    }
    if (!(lw > 1e-24)) return false;
    A /= lw;
    lw = sqrt(lw);
    ly = sqrt(ly) / lw;
    const double B = ((double)cam[11] - A * cam[15]) / lw;
    if (!(A > 1.0) || !(B < 0.0) || !(ly > 1e-6)) return false;
    const double n = -B / A, f = B / (1.0 - A);
    if (!(n > 0.0) || !(f > n)) return false;
    *nearZ = (float)n;
    *farZ = (float)std::min(f, 1e9);
    *fovY = (float)(2.0 * atan(1.0 / ly));
    return true;
}

// Obraz pro ffx API: popis (format, velikost, pouziti) a stav, ve kterem je pri dispatchi; FSR si z nej udela vlastni
// pohledy a po dispatchi ho vrati do stejneho stavu. D24S8 (DEPTH_IN): format R32_UINT + DEPTH|STENCIL -> pohled
// D24_UNORM_S8_UINT s aspektem hloubky; D32S8 (AMD): R32_FLOAT + DEPTH|STENCIL -> D32_SFLOAT_S8_UINT
// (ffx_vk.cpp, getVkFormatFromSurfaceFormatAndUsage).
static FfxApiResource FfxImage(VkImage img, VkFormat fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t state) {
    FfxApiResource r = {};
    r.resource = (void*)img;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK(fmt);
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = usage;
    r.state = state;
    return r;
}

// Jedno vyhodnoceni FSR do cmd. Vstupy jsou v SHADER_READ_ONLY (COMPUTE_READ), vystup v GENERAL (UNORDERED_ACCESS),
// stejne jako pro NGX. Konvence (overeno v harnessu, test fsr):
//   jitter  = posun obrazu v pixelech renderu (+x doprava, +y dolu) = to, co proxy pridava do matic; FSR ho bere stejne
//             (ffx_fsr3upscaler_upsample.h: nejitterovana pozice vzorku = stred pixelu - Jitter()), znamenka z ini
//             a Alt+F11 plati jako u DLSS,
//   MV      = minula - aktualni pozice v pixelech renderu (mv.comp) -> motionVectorScale = (1, 1) (* [dlaa] MVScale),
//   hloubka = D3D9 0 = near, 1 = far (bez FFX_UPSCALE_ENABLE_DEPTH_INVERTED / _INFINITE), near/far/FOV z kamery,
//             1 jednotka Source = 1 palec (viewSpaceToMetersFactor 0.0254),
//   expozice auto (kontext), preExposure 1, ostreni FSR vypnute (RCAS dela helper sam podle [dlaa] Sharpness).
//   maska reaktivity (od verze 14) = reactive (castice, kour, ohen, jiskry z proxy, src/reactive.inl).
static ffxReturnCode_t DispatchFsr(VkCommandBuffer cmd, VkImage color, VkFormat colorFmt, VkImage depth, bool depthR32,
                                   VkImage out, VkFormat outFmt, float jx, float jy, bool reset, float dtMs,
                                   VkImage reactive) {
    const uint32_t READ = FFX_API_RESOURCE_STATE_COMPUTE_READ;
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = FfxImage(color, colorFmt, g_inW, g_inH, FFX_API_RESOURCE_USAGE_READ_ONLY, READ);
    d.depth = depthR32 ? FfxImage(depth, kDepthFormat, g_inW, g_inH, FFX_API_RESOURCE_USAGE_READ_ONLY, READ)
                       : FfxImage(depth, g_depthFmt, g_inW, g_inH,
                                  FFX_API_RESOURCE_USAGE_DEPTHTARGET | FFX_API_RESOURCE_USAGE_STENCILTARGET, READ);
    d.motionVectors = FfxImage(g_mvCur->img, kMvFormat, g_inW, g_inH, FFX_API_RESOURCE_USAGE_READ_ONLY, READ);
    d.output = FfxImage(out, outFmt, g_outW, g_outH, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // exposure: zadny obraz. reactive (a s [upscale] ReactiveTC i transparencyAndComposition): maska reaktivity
    // z proxy (reactive.comp, R8 velikosti vstupu), VK_NULL_HANDLE = zadny obraz (jako verze 13).
    if (reactive) {
        d.reactive = FfxImage(reactive, kReactFormat, g_inW, g_inH, FFX_API_RESOURCE_USAGE_READ_ONLY, READ);
        if (g_cfg.reactiveTC) d.transparencyAndComposition = d.reactive;
    }
    d.jitterOffset = {jx, jy};
    d.motionVectorScale = {g_cfg.mvScaleX, g_cfg.mvScaleY};
    d.renderSize = {g_rW, g_rH};  // cast B: sub-rect od (0, 0) v obrazech W x H (FSR prepocita UV podle popisu obrazu)
    d.upscaleSize = {g_outW, g_outH};
    d.enableSharpening = false;
    d.sharpness = 0.0f;
    d.frameTimeDelta = dtMs > 0.0f && dtMs < 1000.0f ? dtMs : 1000.0f / 60.0f;
    d.preExposure = 1.0f;
    d.reset = reset;
    float n = 7.0f, f = 28000.0f, fov = 1.02f;  // zaloha: Source/BMS near 7, far 28000 j, svisle ~58 st (90 st 16:9)
    const bool camOk = g_sh->camValid && CameraNearFarFov(g_sh->cam, &n, &f, &fov);
    d.cameraNear = n;
    d.cameraFar = f;
    d.cameraFovAngleVertical = fov;
    d.viewSpaceToMetersFactor = 0.0254f;
    d.flags = g_fsrNonLinFlags ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0;  // S4, stejne jako kontext
    static unsigned s_camLogs;
    if (g_fsrLoggedNear < 0.0f || fabsf(n - g_fsrLoggedNear) > 0.05f * g_fsrLoggedNear ||
        fabsf(f - g_fsrLoggedFar) > 0.05f * g_fsrLoggedFar || fabsf(fov - g_fsrLoggedFov) > 0.05f * g_fsrLoggedFov) {
        g_fsrLoggedNear = n;
        g_fsrLoggedFar = f;
        g_fsrLoggedFov = fov;
        if (++s_camLogs <= 20 || s_camLogs % 100 == 0)
            Log("FSR kamera: near %.2f, far %.0f, FOV svisle %.1f st%s", n, f, fov * 57.29578f,
                camOk ? "" : " (zaloha - kamera neplatna nebo neni perspektiva D3D)");
    }
    return g_ffxDispatch(&g_fsr, &d.header);
}

// Prezentace v hlavnim helperu a Frame Generation (jen s BmsdShared::fgPresenter)
static unsigned WindowDpi(HWND w);  // (definice u wWinMain)
#include "fg.inl"

// ---------------------------------------------------------------------------
// Ladeni masky reaktivity ([upscale] ReactiveDump): v prikazech snimku (pred predanim obrazu proxy) kopie COLOR_IN,
// COLOR_OUT a MASK_IN do bufferu viditelneho z CPU, po fence snimku zapis BMP (rmdump_NN_in / _out / _mask.bmp vedle
// helperu). Snimek s dumpem ceka na GPU (jen ladeni). A/B s maskou a bez: [upscale] ReactiveFsr=0 - proxy masku
// posila dal (stejna oblast efektu), FSR ji nedostane.
// ---------------------------------------------------------------------------
static VkBuffer g_rmdBuf;
static VkDeviceMemory g_rmdMem;
static void* g_rmdPtr;
static VkDeviceSize g_rmdSize;
static int g_rmdLeft = -1, g_rmdIdx, g_rmdSeen;
static PFN_vkCmdCopyImageToBuffer g_rmdCopy;  // vlastni ukazatele (fvk se nacita jen s presenterem FG)
static PFN_vkDestroyBuffer g_rmdDestroyBuf;

static bool RmDumpWanted() {
    if (!g_sh->maskDraws || !FullInput(BMSD_IMG_MASK_IN)) return false;
    if (g_cfg.reactiveDumpAfter < 0) {
        // ReactiveDumpAfter=-1: kazde vytvoreni souboru rmdump.go vedle helperu (test ho vytvori ve spravnou chvili)
        // spusti davku ReactiveDump trojic; helper soubor smaze. Celkem nejvys 400 trojic.
        if (g_rmdLeft <= 0) {
            const std::wstring go = g_dir + L"\\rmdump.go";
            if (GetFileAttributesW(go.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
            DeleteFileW(go.c_str());
            if (g_rmdIdx >= 400) return false;
            g_rmdLeft = g_cfg.reactiveDump;
            g_rmdSeen = 0;
            Log("maska: dump spusten souborem rmdump.go (%d trojic od indexu %d)", g_rmdLeft, g_rmdIdx);
        }
        ++g_rmdSeen;
        return (g_rmdSeen - 1) % g_cfg.reactiveDumpEvery == 0;
    }
    if (g_rmdLeft < 0) g_rmdLeft = g_cfg.reactiveDump;
    if (g_rmdLeft <= 0) return false;
    ++g_rmdSeen;
    return g_rmdSeen > g_cfg.reactiveDumpAfter && (g_rmdSeen - g_cfg.reactiveDumpAfter) % g_cfg.reactiveDumpEvery == 0;
}

static bool RmDumpRecord(VkCommandBuffer cmd) {
    if (!g_rmdCopy) {
        g_rmdCopy = (PFN_vkCmdCopyImageToBuffer)vk.vkGetDeviceProcAddr(g_dev, "vkCmdCopyImageToBuffer");
        g_rmdDestroyBuf = (PFN_vkDestroyBuffer)vk.vkGetDeviceProcAddr(g_dev, "vkDestroyBuffer");
        if (!g_rmdCopy || !g_rmdDestroyBuf) return false;
    }
    const VkDeviceSize one = (VkDeviceSize)g_w * g_h * 4, size = one * 3;
    if (g_rmdSize != size) {
        if (g_rmdBuf) g_rmdDestroyBuf(g_dev, g_rmdBuf, nullptr);
        if (g_rmdMem) vk.vkFreeMemory(g_dev, g_rmdMem, nullptr);
        g_rmdBuf = VK_NULL_HANDLE;
        g_rmdMem = VK_NULL_HANDLE;
        g_rmdPtr = nullptr;
        g_rmdSize = 0;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (vk.vkCreateBuffer(g_dev, &bci, nullptr, &g_rmdBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements req = {};
        vk.vkGetBufferMemoryRequirements(g_dev, g_rmdBuf, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                                   HostVisibleType(req.memoryTypeBits)};
        if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(g_dev, &ai, nullptr, &g_rmdMem) != VK_SUCCESS ||
            vk.vkBindBufferMemory(g_dev, g_rmdBuf, g_rmdMem, 0) != VK_SUCCESS ||
            vk.vkMapMemory(g_dev, g_rmdMem, 0, VK_WHOLE_SIZE, 0, &g_rmdPtr) != VK_SUCCESS)
            return false;
        g_rmdSize = size;
    }
    const VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT};
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                            nullptr, 0, nullptr);
    const int src[3] = {BMSD_IMG_COLOR_IN, BMSD_IMG_COLOR_OUT, BMSD_IMG_MASK_IN};
    for (int k = 0; k < 3; ++k) {
        VkBufferImageCopy r = {};
        r.bufferOffset = one * k;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {g_w, g_h, 1};
        g_rmdCopy(cmd, g_img[src[k]].img, VK_IMAGE_LAYOUT_GENERAL, g_rmdBuf, 1, &r);
    }
    const VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_HOST_READ_BIT};
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0,
                            nullptr);
    return true;
}

static void RmDumpWrite(VkFence fence, uint64_t N, bool fsr, bool maskUsed) {
    if (vk.vkWaitForFences(g_dev, 1, &fence, VK_TRUE, kGpuTimeoutNs) != VK_SUCCESS || !g_rmdPtr) return;
    static const wchar_t* names[3] = {L"in", L"out", L"mask"};
    const size_t one = (size_t)g_w * g_h * 4;
    bool ok = true;
    for (int k = 0; k < 3; ++k) {
        wchar_t path[MAX_PATH + 64];
        swprintf(path, MAX_PATH + 64, L"%ls\\rmdump_%02d_%ls.bmp", g_dir.c_str(), g_rmdIdx, names[k]);
        FILE* fp = _wfopen(path, L"wb");
        if (!fp) {
            ok = false;
            continue;
        }
        BITMAPFILEHEADER fh = {};
        BITMAPINFOHEADER ih = {};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = (DWORD)(fh.bfOffBits + one);
        ih.biSize = sizeof ih;
        ih.biWidth = (LONG)g_w;
        ih.biHeight = -(LONG)g_h;
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        fwrite(&fh, sizeof fh, 1, fp);
        fwrite(&ih, sizeof ih, 1, fp);
        fwrite((const uint8_t*)g_rmdPtr + one * k, 1, one, fp);
        fclose(fp);
    }
    Log("maska: dump %d (N %llu, render %ux%u, %s, maska %s, %u draw) %s", g_rmdIdx, (unsigned long long)N, g_rW, g_rH,
        fsr ? "FSR" : "DLSS", maskUsed ? "pouzita" : "nepouzita", g_sh->maskDraws, ok ? "ulozen" : "SELHAL");
    ++g_rmdIdx;
    --g_rmdLeft;
}

// ---------------------------------------------------------------------------
// Snimek
// ---------------------------------------------------------------------------
static bool ProcessFrame(uint64_t N, uint32_t mode) {
    int s = g_slot;
    g_slot = (g_slot + 1) % kSlots;
    if (g_fenceUsed[s]) {
        const VkResult wr = vk.vkWaitForFences(g_dev, 1, &g_fence[s], VK_TRUE, kGpuTimeoutNs);
        if (wr != VK_SUCCESS) {
            g_fatal = true;
            return Error("snimek z doby pred %d snimky nedobehl na GPU (VkResult %d%s)", kSlots, (int)wr,
                         wr == VK_TIMEOUT ? ", limit 5 s - ztracene zarizeni hry?" : ""),
                   false;
        }
        vk.vkResetFences(g_dev, 1, &g_fence[s]);
        g_fenceUsed[s] = false;
        CollectTimestamps(s);
        CollectObjStats(s);
    }
    TimingFlush(false);
    const uint32_t EXT = VK_QUEUE_FAMILY_EXTERNAL;
    const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL, SRO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkImageAspectFlags COLOR = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const Img& in = g_img[BMSD_IMG_COLOR_IN];
    const Img& depth = g_img[BMSD_IMG_DEPTH_IN];
    const Img& out = g_img[BMSD_IMG_COLOR_OUT];

    // Cast B (od verze 10): platny vstup je jen sub-rect g_rW x g_rH sdilenych obrazu, vystup je cely g_w x g_h
    const bool sub = g_rW != g_w || g_rH != g_h;
    // Co se skutecne provede (DLAA bez NGX a ladeni MV bez shaderu = kopie, v casti B roztazeni sub-rectu)
    // Upscaler: DLSS feature nebo FSR kontext (od verze 11); vse ostatni (vstupy, alfa, RCAS, zmenseni) je spolecne
    const bool fsrOn = g_fsr != nullptr && !g_fsrBroken;
    const bool dlaa = mode == BMSD_MODE_DLAA && ((g_dlss && !g_dlaaBroken) || fsrOn) && g_ownOk;
    const bool mvDebug = mode == BMSD_MODE_MVDEBUG && g_ownOk;
    // Vstup DLSS v linearnim HDR (feature vytvorena s IsHDR a obrazy RGBA16F existuji)
    const bool hdr = dlaa && g_hdrMode != BMSD_INPUT_LDR && g_hdrImgOk;
    const float itmK = hdr && g_hdrMode == BMSD_INPUT_HDR_ITM ? g_cfg.itmStrength : 0.0f;  // linearize/alpha HDR_IN
    // Supersampling (S > 1): vystup DLSS (g_hdrOutImg u HDR, jinak g_bigImg) je vetsi nez finalni obraz, zmensuje ho
    // downsample.comp (u HDR vcetne prevodu do sRGB) misto alpha.comp. Pri S = 1 presne puvodni cesta DLAA - i v casti
    // B (vystup DLSS = finalni obraz, zadne zmenseni; rezim podle pomeru vybral uz CreateDlss).
    const bool up = dlaa && (g_outW != g_w || g_outH != g_h);
    // Hloubka pro DLSS (Alt+F5): bez obrazu R32F jen puvodni. Zmena za behu = jen jiny zdroj pro DLSS (feature
    // se nemeni, format hloubky se predava pri kazdem vyhodnoceni), historie se ale resetuje.
    // V casti B se DepthMode 1 a 2 nepodporuji (pokus pro L/M; depthmin.comp pocita pres cely obraz): vzdy puvodni.
    const uint32_t depthReq = g_sh->depthMode < BMSD_DEPTH_COUNT ? g_sh->depthMode : BMSD_DEPTH_RAW;
    // Obrazy hloubky / masky az pri prvnim pouziti: GPU nejdriv dobehnout (zmena descriptor setu)
    {
        const bool wantDepth = g_depthPending && dlaa && !sub && depthReq != BMSD_DEPTH_RAW;
        const bool wantReact = g_reactPending && dlaa && g_sh->maskDraws > 0 &&
                               (fsrOn ? g_cfg.reactiveFsr : g_cfg.reactiveDlss) && FullInput(BMSD_IMG_MASK_IN);
        if ((wantDepth || wantReact) && DrainGpu("novy obraz hloubky / masky")) {
            if (wantDepth) EnsureDepthImg();
            if (wantReact) EnsureReactImg();
        }
        if (g_vmPrevPending && dlaa && DrainGpu("obrazy mereni MV zbrane")) EnsureVmPrev();
    }
    const bool depthSubOff = sub && depthReq != BMSD_DEPTH_RAW;
    const uint32_t depthMode = g_depthImgOk && !sub ? depthReq : BMSD_DEPTH_RAW;
    const bool depthPass = dlaa && depthMode != BMSD_DEPTH_RAW;
    const uint32_t depthLogKey = depthReq | (depthSubOff ? 0x100u : 0u);
    if (depthLogKey != g_loggedDepthMode) {
        g_loggedDepthMode = depthLogKey;
        if (depthMode == depthReq)
            Log("hloubka DLSS: %s", DepthName(depthReq));
        else if (depthSubOff)
            Log("hloubka DLSS: %s (pozadovano %s - v casti B se nepodporuje)", DepthName(depthMode),
                DepthName(depthReq));
        else
            Log("hloubka DLSS: %s (pozadovano %s, obraz R32F neni - viz vyse)", DepthName(depthMode),
                DepthName(depthReq));
    }
    // Maska reaktivity (od verze 14): proxy ji v tomto snimku poslala (maskDraws > 0) a upscaler ji ma dostat
    // ([upscale] ReactiveFsr / ReactiveDlss). Jinak presne cesta verze 13 (zadny obraz, zadny pruchod).
    const bool maskFsr = fsrOn && g_cfg.reactiveFsr, maskDlss = !fsrOn && g_cfg.reactiveDlss;
    const bool maskPass = dlaa && g_reactImgOk && g_sh->maskDraws > 0 && (maskFsr || maskDlss) &&
                          FullInput(BMSD_IMG_MASK_IN);
    {
        const int key = !dlaa || !g_sh->maskDraws ? 0 : maskPass ? (fsrOn ? 1 : 2) : (fsrOn ? 3 : 4);
        if (key && key != g_loggedMask) {
            static const char* why[5] = {"", "FSR reactive%s", "DLSS Bias Current Color Mask", "FSR: vypnuto v ini (ReactiveFsr=0)",
                                         "DLSS: vypnuto v ini (ReactiveDlss=0) nebo obraz R8 chybi"};
            char buf[96];
            snprintf(buf, sizeof buf, why[key], g_cfg.reactiveTC ? " + transparencyAndComposition" : "");
            Log("maska reaktivity od proxy: %s (scale %g, max %g)", buf, g_cfg.reactiveScale, g_cfg.reactiveMax);
            g_loggedMask = key;
        }
    }
    const bool depthChanged = dlaa && depthMode != g_lastDepthMode;
    if (dlaa) g_lastDepthMode = depthMode;
    // Filtr zmenseni (Alt+F1) jen jina push konstanta; pocet fazi jitteru (Alt+F2 nebo auto podle S): jina sekvence
    // jitteru -> reset historie DLSS. 0 = proxy pocet neposila.
    const uint32_t downFilter = g_sh->downsampleFilter < BMSD_DOWN_COUNT ? g_sh->downsampleFilter : g_cfg.downsampleFilter;
    if (downFilter != g_loggedDownFilter) {
        g_loggedDownFilter = downFilter;
        Log("zmenseni: %s (%s)%s", FilterName(downFilter), g_cfg.downsampleLinear ? "linearni" : "srgb",
            up ? "" : " - projevi se jen pri vystupu DLSS > 1.0x");
    }
    const uint32_t phases = g_sh->jitterPhases, phasesAuto = g_sh->jitterPhasesAuto;
    const bool phasesChanged = phases && (phases != g_lastPhases || phasesAuto != g_lastPhasesAuto);
    if (phasesChanged) {
        if (phasesAuto)
            Log("jitter: auto 8*S^2 = %u fazi%s", phases, g_lastPhases ? " - reset historie DLSS" : "");
        else
            Log("jitter: %u fazi (rucne)%s", phases, g_lastPhases ? " - reset historie DLSS" : "");
        g_lastPhases = phases;
        g_lastPhasesAuto = phasesAuto;
    }
    if (mode != g_lastMode) {
        Log("rezim %u -> %u (%s%s)", g_lastMode, mode,
            dlaa ? (fsrOn ? (sub || up ? "FSR" : "FSR Native AA") : sub ? "DLSS" : "DLAA")
            : mvDebug ? "ladeni MV" : mode == BMSD_MODE_MIRROR && !sub ? "zrcadleni" : "kopie",
            sub ? (dlaa ? ", cast B" : ", cast B: roztazeni sub-rectu") : "");
    }
    // Reprojekce kamery (jen pro DLAA a ladeni MV); bez platne kamery jsou MV nulove a historie se resetuje.
    MvPush pc = {};
    const bool camOk = (dlaa || mvDebug) && g_sh->camValid && BuildReprojection(g_sh->cam, g_sh->prevCam, pc.rows);
    bool cut = false;
    if (camOk && dlaa) {
        double dist = 0.0, angle = 0.0;
        if (CameraMotion(g_sh->cam, g_sh->prevCam, &dist, &angle) &&
            ((g_cfg.cutDistance > 0.0f && dist > g_cfg.cutDistance) || (g_cfg.cutAngle > 0.0f && angle > g_cfg.cutAngle))) {
            cut = true;
            if (++g_cuts <= 20 || g_cuts % 100 == 0)
                Log("strih kamery #%u: posun %.0f j, otoceni %.0f st -> reset historie DLSS", g_cuts, dist, angle);
        }
    }
    const bool reset =
        !camOk || cut || g_sh->resetHint || g_lastMode != BMSD_MODE_DLAA || g_dlssFresh || depthChanged || phasesChanged;
    // MV pohyblivych objektu (od verze 9): proxy je v tomto snimku prehrala do OBJMV_IN (objMvCount > 0). Bez platne
    // kamery se nepouziji (MV jsou nulove a historie DLSS se stejne resetuje).
    const uint32_t objReq = g_sh->objMvMode & (BMSD_OBJMV_ON | BMSD_OBJMV_VALIDATE | BMSD_OBJMV_VIEWMODEL);
    const float objTol = std::min(1.0f, std::max(0.0f, g_sh->objMvTol));
    if (objReq != g_loggedObjMode || (objReq && objTol != g_objTolLogged)) {
        g_loggedObjMode = objReq;
        g_objTolLogged = objTol;
        if (objReq & BMSD_OBJMV_ON)
            Log("MV objektu: zapnuto (tolerance hloubky %g)%s%s", objTol,
                (objReq & BMSD_OBJMV_VALIDATE) ? ", validace statickych objektu" : "",
                (objReq & BMSD_OBJMV_VIEWMODEL) ? ", i zbran v ruce" : "");
        else
            Log("MV objektu: vypnuto");
    }
    const bool objOn = camOk && (objReq & BMSD_OBJMV_ON) && g_sh->objMvCount > 0 && g_objStatPtr != nullptr &&
                       FullInput(BMSD_IMG_OBJMV_IN);
    // Doostreni a znamenko jitteru posila proxy kazdy snimek (Ctrl+Home, Ctrl+F11 v DLAA)
    const float sharpness = std::min(1.0f, std::max(0.0f, g_sh->sharpness));
    const bool sharpen = dlaa && sharpness > 0.0f;
    const float signX = g_cfg.jitterSignX * (g_sh->liveJitterSignX < 0 ? -1.0f : 1.0f);
    const float signY = g_cfg.jitterSignY * (g_sh->liveJitterSignY < 0 ? -1.0f : 1.0f);
    if (sharpness != g_loggedSharpness) {
        g_loggedSharpness = sharpness;
        Log("ostrost: %.1f%s", sharpness, sharpness > 0.0f ? " (RCAS)" : " (vypnuto)");
    }
    if (signX != g_loggedSignX || signY != g_loggedSignY) {
        g_loggedSignX = signX;
        g_loggedSignY = signY;
        Log("znamenko jitteru: %+.0f %+.0f (ini %+.0f %+.0f, klavesa %+d %+d)", signX, signY, g_cfg.jitterSignX,
            g_cfg.jitterSignY, g_sh->liveJitterSignX < 0 ? -1 : 1, g_sh->liveJitterSignY < 0 ? -1 : 1);
    }
    g_lastMode = mode;

    VkCommandBuffer cmd = g_cmd[s];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkResetCommandBuffer(cmd, 0);
    vk.vkBeginCommandBuffer(cmd, &bi);
    // Casova razitka slotu (pred DLSS, po DLSS, po zmenseni) se nuluji pred kazdym pouzitim
    const bool timing = dlaa && g_qpool;
    if (timing) vk.vkCmdResetQueryPool(cmd, g_qpool, s * kTsPerSlot, kTsPerSlot);
    auto ts = [&](uint32_t i) {
        if (!timing) return;
        vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qpool, s * kTsPerSlot + i);
        g_tsMask[s] |= 1u << i;
    };
    g_tsMask[s] = 0;
    ts(kTsStart);

    // Prevzeti sdilenych obrazu od DXVK (layout GENERAL, viz interop.cpp)
    VkImageMemoryBarrier acq[BMSD_IMG_COUNT];
    for (int i = 0; i < BMSD_IMG_COUNT; ++i) {
        VkAccessFlags dst = i == BMSD_IMG_COLOR_OUT ? VK_ACCESS_TRANSFER_WRITE_BIT
                                                    : VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        acq[i] = IB(g_img[i].img, g_img[i].aspect, 0, dst, GEN, GEN, EXT, g_qf);
    }
    PB(cmd, ALL, ALL, BMSD_IMG_COUNT, acq);

    // Alfa po DLSS se cte z COLOR_IN: cast A stejny pixel (meritko presne 1), cast B v meritku sub-rectu
    const float alphaSx = (float)g_rW / (float)g_w, alphaSy = (float)g_rH / (float)g_h;
    // Bez zpracovani: COLOR_IN -> COLOR_OUT (oba v GENERAL). Cast A: kopie 1:1. Cast B: vstup je jen v sub-rectu,
    // kopie by dala obraz 1:1 v rohu a mimo nej stary obsah -> roztahnout sub-rect na cely vystup jako engine
    // (LINEAR). Jen pro rezim kopie a snimek, ktery uz je ve fronte (selhane vyhodnoceni, DLSS pro tento vstup
    // nebezi); proxy jinak v casti B bez DLSS most nespousti a roztahuje engine.
    auto copyIn = [&]() {
        if (!sub) {
            VkImageCopy c = {};
            c.srcSubresource = {COLOR, 0, 0, 1};
            c.dstSubresource = {COLOR, 0, 0, 1};
            c.extent = {g_w, g_h, 1};
            vk.vkCmdCopyImage(cmd, in.img, GEN, out.img, GEN, 1, &c);
            return;
        }
        // LINEAR blit cte u praveho a dolniho okraje i texel tesne za sub-rectem (stary obsah): okraj sub-rectu
        // nejdriv zopakovat o 1 texel dal (= clamp). Dve kopie po sobe (zdroj radku zahrnuje cil sloupce).
        const VkPipelineStageFlags XFER = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const VkAccessFlags RW = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        VkImageMemoryBarrier eb = IB(in.img, in.aspect, VK_ACCESS_TRANSFER_READ_BIT, RW, GEN, GEN);
        PB(cmd, ALL, XFER, 1, &eb);
        eb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkImageCopy e = {};
        e.srcSubresource = {COLOR, 0, 0, 1};
        e.dstSubresource = {COLOR, 0, 0, 1};
        if (g_rW < g_w) {
            e.srcOffset = {(int32_t)g_rW - 1, 0, 0};
            e.dstOffset = {(int32_t)g_rW, 0, 0};
            e.extent = {1, g_rH, 1};
            vk.vkCmdCopyImage(cmd, in.img, GEN, in.img, GEN, 1, &e);
            PB(cmd, XFER, XFER, 1, &eb);
        }
        if (g_rH < g_h) {
            e.srcOffset = {0, (int32_t)g_rH - 1, 0};
            e.dstOffset = {0, (int32_t)g_rH, 0};
            e.extent = {std::min(g_rW + 1, g_w), 1, 1};
            vk.vkCmdCopyImage(cmd, in.img, GEN, in.img, GEN, 1, &e);
            PB(cmd, XFER, XFER, 1, &eb);
        }
        VkImageBlit blit = {};
        blit.srcSubresource = {COLOR, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t)g_rW, (int32_t)g_rH, 1};
        blit.dstSubresource = {COLOR, 0, 0, 1};
        blit.dstOffsets[1] = {(int32_t)g_w, (int32_t)g_h, 1};
        vk.vkCmdBlitImage(cmd, in.img, GEN, out.img, GEN, 1, &blit,
                          g_blitLinearOk ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    };

    if (dlaa || mvDebug) {
        // Vstupy pro cteni ve shaderech (NGX ocekava "sampled" stav), vlastni obrazy pro zapis.
        // Stary obsah vlastnich obrazu se zahazuje (UNDEFINED); bariera ALL->ALL ceka i na minuly snimek.
        // Vstup HDR: linearni vstup (pise linearize.comp) a vystup DLSS (RGBA16F, pise NGX) taky z UNDEFINED.
        // Hloubka DLSS R32F (pise depthmin.comp) taky; DLSS ji minuly snimek cetl v SHADER_READ_ONLY.
        // S > 1 se vstupem LDR: vystup DLSS RGBA8 velikosti vystupu (pise NGX, cte downsample.comp) taky.
        // Obraz MV tohoto snimku: s FG primo obraz snapshotu B % 3 (FgMvTarget), jinak g_mvImg
        VkDescriptorSet mvSet = g_mvSet;
        g_mvCur = &g_mvImg;
        if (dlaa && g_fgPresenter) FgMvTarget(N, &g_mvCur, &mvSet);
        VkImageMemoryBarrier pre[11] = {
            IB(in.img, in.aspect, 0, VK_ACCESS_SHADER_READ_BIT, GEN, SRO),
            IB(depth.img, depth.aspect, 0, VK_ACCESS_SHADER_READ_BIT, GEN, SRO),
            IB(g_mvCur->img, COLOR, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN),
            IB(g_outImg.img, COLOR, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN)};
        uint32_t nPre = 4;
        if (hdr) {
            pre[nPre++] = IB(g_linImg.img, COLOR, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN);
            pre[nPre++] = IB(g_hdrOutImg.img, COLOR, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, GEN);
        }
        if (depthPass)
            pre[nPre++] = IB(g_depthImg.img, COLOR, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN);
        if (maskPass)
            pre[nPre++] = IB(g_reactImg.img, COLOR, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN);
        if (up && !hdr)
            pre[nPre++] = IB(g_bigImg.img, COLOR, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, GEN);
        const bool vmOn = g_vmPrevOk && dlaa;
        if (vmOn)
            for (OwnImg& o : g_vmPrev)
                pre[nPre++] = IB(o.img, COLOR, VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                 g_vmPrevInit ? VK_IMAGE_LAYOUT_UNDEFINED : GEN, GEN);
        PB(cmd, ALL, ALL, nPre, pre);

        // MV a linearizace jen v renderu (cast A: cely obraz; cast B: sub-rect od (0, 0) - viewport hry zacina
        // v rohu, takze pixel p odpovida NDC 2p / render - 1 a MV vychazi v pixelech renderu). Ladeni MV pise do
        // g_outImg taky jen sub-rect, na cely obraz ho roztahne az zaverecny blit.
        pc.size[0] = (float)g_rW;
        pc.size[1] = (float)g_rH;
        pc.jitter[0] = g_sh->jitterX;  // skutecny posun obrazu (znamenka z ini plati jen pro DLSS)
        pc.jitter[1] = g_sh->jitterY;
        pc.debugScale = mvDebug ? 0.5f / g_cfg.mvDebugRange : 0.0f;
        pc.valid = camOk ? 1u : 0u;
        pc.vmDepth = g_cfg.viewmodelDepth;
        pc.objMode = objOn ? objReq : 0u;
        pc.objTol = objTol;
        pc.statBase = (uint32_t)s * kObjStatPerSlot;
        pc.prevJitter[0] = g_vmPrevJit[0];
        pc.prevJitter[1] = g_vmPrevJit[1];
        pc.vmCheck = 0u;
        if (vmOn) {
            if (g_vmPrevW != g_rW || g_vmPrevH != g_rH) g_vmPrevValid = false;
            pc.vmCheck = 1u | (g_vmPrevValid && objOn ? 2u : 0u) | (g_vmParity ? 4u : 0u);
            g_vmPrevJit[0] = g_sh->jitterX;
            g_vmPrevJit[1] = g_sh->jitterY;
            g_vmPrevW = g_rW;
            g_vmPrevH = g_rH;
            g_vmParity ^= 1u;
            g_vmPrevValid = true;
            g_vmPrevInit = false;
        }
        if (objOn) {
            // pocitadla slotu: minula prace slotu dobehla (cekani na fence vyse) a je sectena
            memset(g_objStatPtr + (size_t)s * kObjStatPerSlot, 0, kObjStatPerSlot * sizeof(uint32_t));
            g_objSlotUsed[s] = true;
        }
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipeLayout, 0, 1, &g_descSet, 0, nullptr);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipeLayout, 1, 1, &mvSet, 0, nullptr);
        vk.vkCmdPushConstants(cmd, g_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vk.vkCmdDispatch(cmd, (g_rW + 7) / 8, (g_rH + 7) / 8, 1);
        if (objOn) {
            // Pocitadla (atomiky mv.comp) -> cteni na CPU po fence slotu: fence sama zapisy zarizeni pro hosta
            // nezviditelni, je potreba bariera do HOST (pamet je HOST_COHERENT, vkInvalidate netreba)
            const VkMemoryBarrier hostRead = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                                              VK_ACCESS_HOST_READ_BIT};
            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                    &hostRead, 0, nullptr, 0, nullptr);
        }
        if (hdr) {
            // COLOR_IN (sRGB) -> linearni RGBA16F pro DLSS. Na MV nezavisi (jiny vystup), bariera mezi nimi neni
            // potreba; descriptor set zustava navazany (stejny layout). Push konstanta k (InputMode 3) prepise
            // zacatek MvPush az po dispatchi mv.comp, ten uz ma hodnoty zaznamenane.
            vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_linPipe);
            // (v casti B jen skupiny pokryvajici sub-rect; DLSS cte jen sub-rect)
            vk.vkCmdPushConstants(cmd, g_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof itmK, &itmK);
            vk.vkCmdDispatch(cmd, (g_rW + 7) / 8, (g_rH + 7) / 8, 1);
        }
        if (depthPass) {
            // DEPTH_IN (uz v SHADER_READ_ONLY z bariery "pre") -> R32F min 3x3 / konstanta 1.0 pro DLSS.
            // Jen cteni sdilene hloubky a zapis do vlastniho obrazu, bez zavislosti na predchozich dispatchich.
            // Push konstanta mode prepise zacatek MvPush az po jejich dispatchich (hodnoty uz zaznamenane).
            vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_depthPipe);
            vk.vkCmdPushConstants(cmd, g_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof depthMode, &depthMode);
            vk.vkCmdDispatch(cmd, (g_inW + 7) / 8, (g_inH + 7) / 8, 1);
        }
        if (maskPass) {
            // MASK_IN (GENERAL, prevzaty od DXVK) -> R8 maska pro FSR / DLSS, jen render. Push konstanty jako u depthmin.
            struct {
                uint32_t w, h;
                float scale, maxValue;
            } rp = {g_rW, g_rH, g_cfg.reactiveScale, g_cfg.reactiveMax};
            vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_reactPipe);
            vk.vkCmdPushConstants(cmd, g_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rp, &rp);
            vk.vkCmdDispatch(cmd, (g_rW + 7) / 8, (g_rH + 7) / 8, 1);
        }

        // Zapisy compute -> cteni v DLSS (vstupy v "sampled" stavu, Guide 3.4)
        VkImageMemoryBarrier mid[5];
        uint32_t nMid = 0;
        mid[nMid++] = IB(g_mvCur->img, COLOR, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, GEN, SRO);
        if (hdr) mid[nMid++] = IB(g_linImg.img, COLOR, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, GEN, SRO);
        if (depthPass)
            mid[nMid++] = IB(g_depthImg.img, COLOR, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, GEN, SRO);
        if (maskPass)
            mid[nMid++] = IB(g_reactImg.img, COLOR, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, GEN, SRO);
        if (mvDebug)
            mid[nMid++] = IB(g_outImg.img, COLOR, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, GEN,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        PB(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, ALL, nMid, mid);

        bool outReady = mvDebug;
        VkImage finalImg = g_outImg.img;  // co jde blitem do COLOR_OUT
        if (dlaa) {
            const VkImageSubresourceRange colorRange = {COLOR, 0, 1, 0, 1};
            const VkImageSubresourceRange depthRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            // Vstup HDR: barva = linearni RGBA16F, vystup = RGBA16F (Guide 3.3: libovolny format, IsHDR pri
            // linearnich barvach, 3.1.2), expozice 1x1 = 1.0 jen v InputMode 1 (v 2 a 3 pocita DLSS sam, 3.10).
            // Vstupy (barva, hloubka, MV) jsou obrazy plne velikosti W x H, platny je sub-rect g_rW x g_rH od (0, 0)
            // (InRenderSubrectDimensions; cast A: cely obraz). Vystup g_outW x g_outH (= g_w x g_h pri S = 1):
            // LDR do g_outImg (S = 1) nebo g_bigImg (S > 1), HDR do g_hdrOutImg.
            NVSDK_NGX_Resource_VK rColor =
                hdr ? NVSDK_NGX_Create_ImageView_Resource_VK(g_linImg.view, g_linImg.img, colorRange, kHdrFormat, g_inW,
                                                             g_inH, false)
                    : NVSDK_NGX_Create_ImageView_Resource_VK(g_colorInView, in.img, colorRange,
                                                             VK_FORMAT_B8G8R8A8_UNORM, g_inW, g_inH, false);
            // Hloubka: DEPTH_IN (D24, aspekt hloubky) nebo R32F z depthmin.comp (stejny rozsah 0 = near, 1 = far)
            NVSDK_NGX_Resource_VK rDepth =
                depthPass ? NVSDK_NGX_Create_ImageView_Resource_VK(g_depthImg.view, g_depthImg.img, colorRange,
                                                                   kDepthFormat, g_inW, g_inH, false)
                          : NVSDK_NGX_Create_ImageView_Resource_VK(g_depthInView, depth.img, depthRange,
                                                                   g_depthFmt, g_inW, g_inH, false);
            NVSDK_NGX_Resource_VK rMv = NVSDK_NGX_Create_ImageView_Resource_VK(g_mvCur->view, g_mvCur->img, colorRange,
                                                                               kMvFormat, g_inW, g_inH, false);
            const OwnImg& dlssOut = hdr ? g_hdrOutImg : up ? g_bigImg : g_outImg;
            NVSDK_NGX_Resource_VK rOut = NVSDK_NGX_Create_ImageView_Resource_VK(
                dlssOut.view, dlssOut.img, colorRange, hdr ? kHdrFormat : kOutFormat, g_outW, g_outH, true);
            NVSDK_NGX_Resource_VK rExp = NVSDK_NGX_Create_ImageView_Resource_VK(g_expImg.view, g_expImg.img,
                                                                                colorRange, kExpFormat, 1, 1, false);
            NVSDK_NGX_Resource_VK rBias = NVSDK_NGX_Create_ImageView_Resource_VK(
                g_reactImg.view, g_reactImg.img, colorRange, kReactFormat, g_inW, g_inH, false);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dtMs = g_lastEvalTime.QuadPart ? (float)((now.QuadPart - g_lastEvalTime.QuadPart) * 1000.0 /
                                                           g_qpf.QuadPart)
                                                 : 0.0f;
            g_lastEvalTime = now;

            NVSDK_NGX_VK_DLSS_Eval_Params ep = {};
            ep.Feature.pInColor = &rColor;
            ep.Feature.pInOutput = &rOut;
            ep.pInDepth = &rDepth;
            ep.pInMotionVectors = &rMv;
            // Jitter = posun obrazu v pixelech vstupu, stejna soustava jako MV (+x doprava, +y dolu);
            // proxy posouva obraz o (+jitterX, +jitterY). Znamenka lze otocit v ini a za behu (Ctrl+F11).
            ep.InJitterOffsetX = signX * g_sh->jitterX;
            ep.InJitterOffsetY = signY * g_sh->jitterY;
            ep.InRenderSubrectDimensions = {g_rW, g_rH};  // cast B: sub-rect (feature je vytvorena presne pro nej)
            ep.InReset = reset ? 1 : 0;
            ep.InMVScaleX = g_cfg.mvScaleX;
            ep.InMVScaleY = g_cfg.mvScaleY;
            ep.InFrameTimeDeltaInMsec = dtMs > 0.0f && dtMs < 1000.0f ? dtMs : 0.0f;
            if (maskPass && maskDlss) ep.pInBiasCurrentColorMask = &rBias;  // jinak nullptr = jako verze 13
            if (hdr) {
                // Guide 3.9: expozice z textury (jen presety J, K; L a M ji ignoruji), 3.9.2: bez pre-expozice.
                if (g_hdrMode == BMSD_INPUT_HDR) ep.pInExposureTexture = &rExp;
                ep.InPreExposure = 1.0f;
                ep.InExposureScale = 1.0f;
            }
            // Mereni: razitka "po vsem predchozim" (BOTTOM_OF_PIPE) pred a po DLSS, po zmenseni
            ts(kTsPre);
            // FSR: stejne vstupy a vystup jako DLSS (DispatchFsr), jitter a MV ve stejne konvenci (posun obrazu,
            // minula - aktualni pozice v pixelech renderu; viz docs\fsr-upscaling.md)
            const char* upName = fsrOn ? "FSR" : "DLSS";
            bool evalOk;
            std::string evalErr;
            if (fsrOn) {
                const ffxReturnCode_t fr =
                    DispatchFsr(cmd, hdr ? g_linImg.img : in.img, hdr ? kHdrFormat : VK_FORMAT_B8G8R8A8_UNORM,
                                depthPass ? g_depthImg.img : depth.img, depthPass, dlssOut.img,
                                hdr ? kHdrFormat : kOutFormat, ep.InJitterOffsetX, ep.InJitterOffsetY, reset, dtMs,
                                maskPass && maskFsr ? g_reactImg.img : VK_NULL_HANDLE);
                evalOk = fr == FFX_API_RETURN_OK;
                if (evalOk) g_fsrEvalFails = 0;  // S5: pocitaji se jen selhani za sebou
                evalErr = "ffxDispatch kod " + std::to_string(fr);
            } else {
                NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, g_dlss, g_ngxParams, &ep);
                evalOk = NVSDK_NGX_SUCCEED(r);
                if (!evalOk) evalErr = NgxStr(r);
            }
            ts(kTsDlss);
            if (!evalOk && up) {
                // S > 1 nesmi DLSS vypnout natrvalo: tento snimek jde jako kopie, po nem FallbackToNative (S = 1)
                Log("vyhodnoceni %s (vystup %ux%u, %s) selhalo: %s - prechazim na 1.0x", upName, g_outW, g_outH,
                    PerfName(g_perfQuality), evalErr.c_str());
                g_upFailPending = true;
            } else if (!evalOk && sub) {
                // Cast B nesmi DLSS vypnout natrvalo: tento snimek jde roztazeny blitem, po nem SubEvalFail
                Log("vyhodnoceni %s (cast B: vstup %ux%u -> %ux%u, %s) selhalo: %s - snimek roztahuji bilinearne",
                    upName, g_rW, g_rH, g_outW, g_outH, PerfName(g_perfQuality), evalErr.c_str());
                g_subFailPending = true;
            } else if (!evalOk && fsrOn) {
                // FSR v casti A pri S = 1: tento snimek jde jako kopie, po nem FsrEvalFail (FSR vypnout, zkusit DLSS)
                Log("vyhodnoceni FSR selhalo: %s - snimek jde jako kopie", evalErr.c_str());
                g_fsrFailPending = true;
            } else if (!evalOk) {
                DlaaFail("vyhodnoceni DLSS selhalo: %s", evalErr.c_str());
                g_dlssFailPending = true;  // po snimku DlssEvalFail: uvolnit feature, zkusit FSR (D1)
            } else if (up) {
                // S > 1: vystup DLSS -> downsample.comp (HDR: prevod do sRGB pro kazdy tap, alfa z COLOR_IN) do
                // g_outImg W x H -> volitelne RCAS (vstup g_outImg, stejne jako pri S = 1) -> blit.
                // NGX meni stav cmd bufferu, proto znovu navazat pipeline i descriptor set.
                VkImageMemoryBarrier db[2];
                uint32_t nDb = 0;
                db[nDb++] = IB(dlssOut.img, COLOR, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT, GEN, GEN);
                if (sharpen)
                    db[nDb++] = IB(g_sharpImg.img, COLOR, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN);
                PB(cmd, ALL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, nDb, db);
                vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipeLayout, 0, 1, &g_descSet, 0,
                                           nullptr);
                // g_outImg je od bariery "pre" v GENERAL pro zapis. Prekmit filtru se orezava jen s doostrenim.
                DownPush dp = {};
                dp.ratio[0] = (float)g_outW / (float)g_w;
                dp.ratio[1] = (float)g_outH / (float)g_h;
                dp.k = itmK;
                dp.flags = (sharpen ? 1u : 0u) | (g_cfg.downsampleLinear ? 2u : 0u);
                dp.kernel = downFilter;
                const int dv = std::max(dp.ratio[0], dp.ratio[1]) <= kDownSmallMax ? 0 : 1;  // dlazdice podle S
                vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hdr ? g_downHdrPipe[dv] : g_downPipe[dv]);
                vk.vkCmdPushConstants(cmd, g_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof dp, &dp);
                vk.vkCmdDispatch(cmd, (g_w + 7) / 8, (g_h + 7) / 8, 1);
                ts(kTsDown);
                if (sharpen) {
                    VkImageMemoryBarrier enc =
                        IB(g_outImg.img, COLOR, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, GEN, GEN);
                    PB(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, &enc);
                    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_sharpPipe);
                    // jako nize (FSR 2); alfa z COLOR_IN na finalni mrizce (S > 1 je jen v casti A: meritko 1)
                    const AlphaPush sp = {exp2f(-(2.0f - 2.0f * sharpness)), 0.0f, {alphaSx, alphaSy}};
                    vk.vkCmdPushConstants(cmd, g_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof sp, &sp);
                    vk.vkCmdDispatch(cmd, (g_w + 7) / 8, (g_h + 7) / 8, 1);
                    finalImg = g_sharpImg.img;
                }
                ts(kTsPost);
                outReady = true;
                g_dlssFresh = false;
                if (++g_dlaaFrames == 1 || g_dlaaFrames % 3000 == 0)
                    Log("%s snimku: %u (vystup %ux%u %sx %s, jitter %.3f,%.3f px, reset %d, kamera %s, vstup %s, "
                        "hloubka %s)",
                        upName, g_dlaaFrames, g_outW, g_outH, ScaleStr(g_outScale).c_str(), PerfName(g_perfQuality),
                        ep.InJitterOffsetX, ep.InJitterOffsetY, ep.InReset, camOk ? "ok" : "neplatna",
                        InputName(hdr ? g_hdrMode : BMSD_INPUT_LDR), DepthName(depthMode));
            } else {
                // Alfa vystupu DLSS je nesmysl (Guide 3.16): vratit alfu ze vstupu sceny. S doostrenim to
                // dela RCAS (do vlastniho obrazu, cte sousedy), bez nej levny pruchod primo ve vystupu.
                // Cast B: vystup W x H, alfa z COLOR_IN v sub-rectu (AlphaPush::scale = render / W; odpovida
                // roztazeni, ktere by udelal engine). V casti A je meritko 1 = stejny pixel.
                // Vstup HDR: nejdriv alpha.comp HDR_IN (linearni RGBA16F -> sRGB RGBA8 + alfa do g_outImg),
                // RCAS pak pracuje v sRGB stejne jako u LDR.
                // NGX meni stav cmd bufferu, proto znovu navazat pipeline i descriptor set.
                VkImageMemoryBarrier ab[2];
                uint32_t nAb = 0;
                ab[nAb++] = IB(hdr ? g_hdrOutImg.img : g_outImg.img, COLOR,
                               VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, GEN, GEN);
                if (sharpen)
                    ab[nAb++] = IB(g_sharpImg.img, COLOR, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN);
                PB(cmd, ALL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, nAb, ab);
                vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipeLayout, 0, 1, &g_descSet, 0,
                                           nullptr);
                if (hdr) {
                    // g_outImg je od bariery "pre" v GENERAL pro zapis, NGX na nej v HDR nesaha.
                    // Push konstanta k: inverze rozsireni z linearize.comp (InputMode 3), jinak 0.
                    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_alphaHdrPipe);
                    const AlphaPush hp = {itmK, 0.0f, {alphaSx, alphaSy}};
                    vk.vkCmdPushConstants(cmd, g_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof hp, &hp);
                    vk.vkCmdDispatch(cmd, (g_w + 7) / 8, (g_h + 7) / 8, 1);
                    if (sharpen) {
                        VkImageMemoryBarrier enc = IB(g_outImg.img, COLOR, VK_ACCESS_SHADER_WRITE_BIT,
                                                      VK_ACCESS_SHADER_READ_BIT, GEN, GEN);
                        PB(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, &enc);
                    }
                }
                if (sharpen || !hdr) {
                    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sharpen ? g_sharpPipe : g_alphaPipe);
                    // RCAS: stejne mapovani jako FSR 2: stops = 2 - 2 * sharpness, con = 2^-stops (alpha.comp
                    // bez HDR_IN prvni hodnotu nepouziva)
                    const AlphaPush ap = {sharpen ? exp2f(-(2.0f - 2.0f * sharpness)) : 0.0f, 0.0f, {alphaSx, alphaSy}};
                    vk.vkCmdPushConstants(cmd, g_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof ap, &ap);
                    if (sharpen) finalImg = g_sharpImg.img;
                    vk.vkCmdDispatch(cmd, (g_w + 7) / 8, (g_h + 7) / 8, 1);
                }
                ts(kTsPost);
                outReady = true;
                g_dlssFresh = false;
                if (++g_dlaaFrames == 1 || g_dlaaFrames % 3000 == 0) {
                    if (sub)
                        Log("%s snimku: %u (cast B: vstup %ux%u -> vystup %ux%u %s, jitter %.3f,%.3f px, reset %d, "
                            "kamera %s, vstup %s, hloubka %s)",
                            upName, g_dlaaFrames, g_rW, g_rH, g_outW, g_outH, PerfName(g_perfQuality), ep.InJitterOffsetX,
                            ep.InJitterOffsetY, ep.InReset, camOk ? "ok" : "neplatna",
                            InputName(hdr ? g_hdrMode : BMSD_INPUT_LDR), DepthName(depthMode));
                    else
                        Log("%s snimku: %u (jitter %.3f,%.3f px, reset %d, kamera %s, vstup %s, hloubka %s)",
                            fsrOn ? "FSR Native AA" : "DLAA", g_dlaaFrames, ep.InJitterOffsetX, ep.InJitterOffsetY, ep.InReset,
                            camOk ? "ok" : "neplatna", InputName(hdr ? g_hdrMode : BMSD_INPUT_LDR),
                            DepthName(depthMode));
                }
            }
        }

        // Vstupy zpet do GENERAL (pro predani DXVK), vystup (DLSS + alfa) ke kopirovani.
        // NGX po vyhodnoceni vraci obrazy do puvodnich stavu (Programming Guide 3.4).
        VkImageMemoryBarrier post[3];
        uint32_t nPost = 0;
        post[nPost++] = IB(in.img, in.aspect, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, SRO, GEN);
        post[nPost++] = IB(depth.img, depth.aspect, VK_ACCESS_SHADER_READ_BIT, 0, SRO, GEN);
        if (dlaa && outReady)
            post[nPost++] = IB(finalImg, COLOR, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, GEN,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        PB(cmd, ALL, VK_PIPELINE_STAGE_TRANSFER_BIT, nPost, post);
        // Frame Generation: MV a hloubka tohoto snimku do kruhu snapshotu (generator je cte az v Present; most je
        // dalsim snimkem prepise). Reset generatoru: neplatna kamera, strih, nebo predchozi snimek hry nesel mostem.
        if (dlaa && g_fgPresenter) {
            FgSnapshot(cmd, N, camOk, !camOk || cut || g_sh->resetHint != 0);
            ts(kTsSnap);
        }

        if (outReady) {
            // RGBA8 -> BGRA8: blit prevadi format (kopie by prohodila kanaly). Vystup DLSS je vzdy W x H. Ladeni MV
            // v casti B: vizualizace je jen v sub-rectu g_outImg -> roztahnout na cely obraz (NEAREST, at jsou
            // videt pixely renderu); v casti A je zdroj = cil.
            const bool stretch = sub && !dlaa;
            VkImageBlit blit = {};
            blit.srcSubresource = {COLOR, 0, 0, 1};
            blit.srcOffsets[1] = {(int32_t)(stretch ? g_rW : g_w), (int32_t)(stretch ? g_rH : g_h), 1};
            blit.dstSubresource = {COLOR, 0, 0, 1};
            blit.dstOffsets[1] = {(int32_t)g_w, (int32_t)g_h, 1};
            vk.vkCmdBlitImage(cmd, finalImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.img, GEN, 1, &blit,
                              VK_FILTER_NEAREST);
        } else {
            copyIn();
        }
    } else if (mode == BMSD_MODE_MIRROR && !sub) {  // zrcadleni se v casti B nepodporuje (jde jako kopie)
        VkImageBlit blit = {};
        blit.srcSubresource = {COLOR, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t)g_w, (int32_t)g_h, 1};
        blit.dstSubresource = {COLOR, 0, 0, 1};
        blit.dstOffsets[0] = {(int32_t)g_w, 0, 0};
        blit.dstOffsets[1] = {0, (int32_t)g_h, 1};
        vk.vkCmdBlitImage(cmd, in.img, GEN, out.img, GEN, 1, &blit, VK_FILTER_NEAREST);
    } else {
        copyIn();
    }

    ts(kTsEnd);
    // Predani sdilenych obrazu zpet DXVK
    const bool rmDump = dlaa && g_cfg.reactiveDump > 0 && RmDumpWanted() && RmDumpRecord(cmd);
    VkImageMemoryBarrier rel[BMSD_IMG_COUNT];
    for (int i = 0; i < BMSD_IMG_COUNT; ++i) {
        VkAccessFlags src = i == BMSD_IMG_COLOR_OUT ? VK_ACCESS_TRANSFER_WRITE_BIT : 0;
        rel[i] = IB(g_img[i].img, g_img[i].aspect, src, 0, GEN, GEN, g_qf, EXT);
    }
    PB(cmd, ALL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, BMSD_IMG_COUNT, rel);
    vk.vkEndCommandBuffer(cmd);

    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = 1;
    ti.pWaitSemaphoreValues = &N;
    ti.signalSemaphoreValueCount = 1;
    ti.pSignalSemaphoreValues = &N;
    VkPipelineStageFlags waitStage = ALL;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ti};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_semP;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_semH;
    VkResult r;
    {
        std::lock_guard<std::mutex> lk(g_queueMx);
        r = vk.vkQueueSubmit(g_queue, 1, &si, g_fence[s]);
    }
    if (r != VK_SUCCESS) return Error("vkQueueSubmit selhal %d", r), false;
    g_fenceUsed[s] = true;
    if (rmDump) RmDumpWrite(g_fence[s], N, fsrOn, maskPass);
    return true;
}

// Potvrzeni a skutecny stav pro proxy (po kazde zprave a po fallbacku). Potvrzuje se presne prijaty pozadavek,
// i kdyz nesel (proxy na skutecnou hodnotu necheka, jen ji cte - pocet fazi jitteru, mip bias, log).
// Poradi: nejdriv skutecny stav (vcetne helperRenderOk), bariera, pak vsechna potvrzeni a potvrzeni renderu uplne
// posledni - proxy po shode potvrzeni hned cte stav, nesmi videt potvrzeni presetu se starym stavem.
static void PublishState() {
    const bool on = g_dlss != nullptr || g_fsr != nullptr;
    // bezici upscaler: DLSS feature (ne po trvalem selhani vyhodnoceni) nebo FSR kontext
    const bool running = (g_dlss && !g_dlaaBroken) || (g_fsr && !g_fsrBroken);
    g_sh->helperOutScale = on ? g_outScale : 1.0f;
    g_sh->helperOutWidth = on ? g_outW : g_w;
    g_sh->helperOutHeight = on ? g_outH : g_h;
    g_sh->helperPerfQuality = on ? g_perfQuality : -1;
    g_sh->helperRenderOk = running ? 1 : 0;
    g_sh->helperUpscaler = running ? (int32_t)g_upActive : (int32_t)BMSD_UPSCALER_OFF;
    const std::string dlssName = g_dlssVersion.empty() ? std::string("DLSS") : "DLSS " + g_dlssVersion;
    const char* name = !running ? "" : g_upActive == BMSD_UPSCALER_FSR ? g_fsrVersionName.c_str() : dlssName.c_str();
    strncpy(g_sh->helperUpscalerName, name, sizeof g_sh->helperUpscalerName - 1);
    g_sh->helperUpscalerName[sizeof g_sh->helperUpscalerName - 1] = 0;
    g_sh->helperDlssBroken = g_dlaaBroken ? (g_ngxUnsupported ? 1 : 2) : 0;
    MemoryBarrier();
    g_sh->helperPreset = g_dlssPreset;        // potvrzeni zmeny presetu pro proxy
    g_sh->helperInputMode = g_dlssInputMode;  // potvrzeni zmeny vstupu (i kdyz HDR neslo a bezi LDR)
    g_sh->helperOutScaleAck = g_outScaleReq;  // potvrzeni zmeny S (i kdyz neslo a bezi 1.0)
    g_sh->helperUpscalerAck = g_upReqRaw;     // potvrzeni volby upscaleru (i kdyz nesel a bezi jiny / nic)
    MemoryBarrier();
    g_sh->helperRenderAckW = g_rReqW;
    g_sh->helperRenderAckH = g_rReqH;
}

// Ceka na rozpracovane snimky (s limitem: kdyby hra spadla, semafor od DXVK uz neprijde).
static bool WaitSlots(uint64_t timeoutNs) {
    for (int i = 0; i < kSlots; ++i)
        if (g_fenceUsed[i] && vk.vkWaitForFences(g_dev, 1, &g_fence[i], VK_TRUE, timeoutNs) != VK_SUCCESS)
            return false;
    return true;
}

// Jedna zprava mostu od proxy (udalost ..._frame): reimport zdroju / zmena feature / snimek N. false = fatalni chyba,
// helper konci. Vola hlavni smycka a (s prezentaci v helperu) FgOnPresentMsg pred kazdym snimkem presentu: prace
// mostu pro snimek hry musi byt odeslana DRIV nez prace presentu tehoz (nebo pozdejsiho) snimku. Jinak vznikne
// kruhove cekani na GPU: prace presentu ceka na kopii backbufferu od proxy, ta je ve fronte DXVK az za kopii B mostu,
// B ceka na signal helperu a ten je ve fronte helperu az za praci presentu.
static bool BridgeMessage() {
    LARGE_INTEGER tWake;
    QueryPerformanceCounter(&tWake);
    MemoryBarrier();
    if (g_sh->generation != g_gen) {
        if (!ImportResources()) {
            SetEvent(g_evDone);
            return false;
        }
        g_gen = g_sh->generation;
        PublishState();  // skutecny stav (vystup, render) driv nez potvrzeni importu: proxy ho po nem hned cte
        MemoryBarrier();
        g_sh->helperGeneration = g_gen;
    } else if (g_sh->preset != g_dlssPreset || g_sh->inputMode != g_dlssInputMode ||
               g_sh->outScale != g_outScaleReq || g_sh->renderWidth != g_rReqW ||
               g_sh->renderHeight != g_rReqH || g_sh->upscaler != g_upReqRaw ||
               g_sh->upscalerValid != g_upReqValid) {
        RecreateDlss();
        if (g_fatal) {  // GPU nedobehla: konec (proxy uvidi helperError / konec procesu a most vypne)
            SetEvent(g_evDone);
            return false;
        }
    }
    uint64_t N = g_sh->frameValue;
    if (N) {
        const uint64_t posted = g_sh->postQpc;
        if (!ProcessFrame(N, g_sh->mode)) {
            SetEvent(g_evDone);
            return false;
        }
        LARGE_INTEGER tSub;
        QueryPerformanceCounter(&tSub);
        CpuTiming(posted, tWake.QuadPart, tSub.QuadPart);
        ++g_framesDone;
        if (g_framesDone == 1 || g_framesDone % 3000 == 0)
            Log("snimku zpracovano: %llu", (unsigned long long)g_framesDone);
    }
    PublishState();
    MemoryBarrier();
    g_sh->helperDoneValue = N;
    SetEvent(g_evDone);
    if (g_upFailPending) {
        // Vyhodnoceni s S > 1 selhalo: snimek N uz odesel jako kopie, proxy ma "done" - feature pro S = 1
        // vytvorit hned (dalsi snimek pripadne jednou nestihne 100 ms, to proxy snese).
        g_upFailPending = false;
        FallbackToNative();
        if (g_fatal) return false;
        PublishState();
    }
    if (g_subFailPending) {
        // Cast B: vyhodnoceni selhalo, snimek N uz odesel roztazeny blitem. Feature uvolnit a nahlasit
        // helperRenderOk = 0 (pripadny dalsi snimek, ktery proxy mezitim zaradila, pujde taky blitem).
        g_subFailPending = false;
        SubEvalFail();
        if (g_fatal) return false;
        PublishState();
    }
    if (g_fsrFailPending) {
        // FSR (cast A, S = 1): vyhodnoceni selhalo, snimek N uz odesel jako kopie. FSR vypnout, zkusit DLSS.
        g_fsrFailPending = false;
        FsrEvalFail();
        if (g_fatal) return false;
        PublishState();
    }
    if (g_dlssFailPending) {
        // DLSS (cast A, S = 1): vyhodnoceni selhalo, snimek N uz odesel jako kopie. Feature uvolnit, zkusit FSR.
        g_dlssFailPending = false;
        DlssEvalFail();
        if (g_fatal) return false;
        PublishState();
    }
    return true;
}

// Spike S2 (src/helper/present.cpp): prezentacni instance "<pid> present" ([present] HelperPresent=1 v proxy)
int PresentMain(unsigned long pid, const wchar_t* dir);

// Rozpoznani DPI: helper prezentuje (FG) do okna procesu hry, ktery je od nacteni Qt per-monitor DPI aware. Bez
// toho by Windows souradnice cizich oken pro helper pri skalovani != 100 % virtualizovalo (GetClientRect 1707x960
// misto 2560x1440) a velikost swapchainu by nesedela s oknem. Volat pred vytvorenim oken a Vulkanu.
static const char* g_dpiMode = "?";
static void SetupDpiAwareness() {
    typedef BOOL(WINAPI * PfnCtx)(HANDLE);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PfnCtx setCtx = u32 ? (PfnCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
    if (setCtx && setCtx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) g_dpiMode = "per-monitor v2";
    else if (setCtx && GetLastError() == ERROR_ACCESS_DENIED) g_dpiMode = "uz nastaveno (manifest)";
    else if (setCtx && setCtx((HANDLE)-3 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE */)) g_dpiMode = "per-monitor";
    else if (SetProcessDPIAware()) g_dpiMode = "system";
    else g_dpiMode = "nenastaveno";
}

// DPI okna (Windows 10 1607+; jinak 0 = nezjisteno)
static unsigned WindowDpi(HWND w) {
    typedef UINT(WINAPI * PfnDpi)(HWND);
    static PfnDpi f = (PfnDpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    return f && w ? f(w) : 0u;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int) {
    SetupDpiAwareness();
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* p = wcsrchr(dir, L'\\')) *p = 0;
    if (wcsstr(cmdLine, L" present")) return PresentMain(wcstoul(cmdLine, nullptr, 10), dir);  // vlastni log
    g_dir = dir;
    // Log minuleho spusteni se zachova jako .prev.
    MoveFileExW((g_dir + L"\\bms_dlss_helper.log").c_str(), (g_dir + L"\\bms_dlss_helper.log.prev").c_str(),
                MOVEFILE_REPLACE_EXISTING);
    g_log = _wfopen((g_dir + L"\\bms_dlss_helper.log").c_str(), L"w");
    QueryPerformanceFrequency(&g_qpf);

    DWORD pid = wcstoul(cmdLine, nullptr, 10);
    g_prevUef = SetUnhandledExceptionFilter(CrashUef);
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        Log("bms_dlss_helper start - bms-dlss %s (rozhrani %d, sestaveno %s %s), %04u-%02u-%02u, hra pid %lu, DPI rezim "
            "procesu: %s", BMSDLSS_VER_STR, BMSD_VERSION, __DATE__, __TIME__, st.wYear, st.wMonth, st.wDay, pid, g_dpiMode);
    }
    LoadConfig();
    CheckVcRuntime();
    SetupNgxCommon();
    wchar_t name[128];
    swprintf(name, 128, L"Local\\bms_dlss_%lu", pid);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) return Log("nelze otevrit sdilenou pamet"), 1;
    // Cele mapovani (velikost dava proxy; jina verze muze mit jinou velikost struktury).
    void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!view || !VirtualQuery(view, &mbi, sizeof mbi) || mbi.RegionSize < sizeof(BmsdShared))
        return Log("sdilenou pamet nelze namapovat (nebo je moc mala)"), 1;
    BmsdShared* sh = (BmsdShared*)view;
    if (sh->magic != BMSD_MAGIC) return Log("sdilena pamet ma spatny format"), 1;
    if (sh->version != BMSD_VERSION) {
        char msg[160];
        snprintf(msg, sizeof msg, "ruzne verze: proxy %u, helper %u - nainstaluj d3d9.dll i helper ze stejneho buildu",
                 sh->version, BMSD_VERSION);
        Log("CHYBA: %s", msg);
        if (sh->version >= 3) {  // od verze 3 je helperMsg na pevnem miste hned za hlavickou
            g_sh = sh;
            SetHelperMsg(msg);
        }
        return 1;
    }
    g_sh = sh;
    g_fgPresenter = sh->fgPresenter != 0;  // prezentace v tomto helperu (Frame Generation); pred InitVulkan
    if (g_fgPresenter) Log("proxy chce prezentaci v helperu (Frame Generation): swapchain, druha fronta, kanal presentu");
    swprintf(name, 128, L"Local\\bms_dlss_%lu_frame", pid);
    HANDLE evFrame = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    swprintf(name, 128, L"Local\\bms_dlss_%lu_done", pid);
    HANDLE evDone = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!evFrame || !evDone || !parent) return Error("nelze otevrit eventy nebo proces hry"), 1;

    if (!InitVulkan()) return 1;
    g_computeOk = InitCompute();
    if (!g_computeOk) DlaaFail("compute shader motion vektoru nejde vytvorit");
    if (!g_dlaaBroken && !InitNgx()) g_ngxUnsupported = true;
    g_sh->helperDlssBroken = g_dlaaBroken ? (g_ngxUnsupported ? 1 : 2) : 0;
    g_sh->helperFsrBroken = g_vcrtMissing ? 1 : 0;
    if (g_vcrtMissing) SetHelperMsg(("FSR nedostupne: " + g_vcrtWhy).c_str());
    if (g_fgPresenter) FgInit(pid);
    MemoryBarrier();
    g_sh->helperReady = 1;

    // Vyssi priorita vlakna: helper se jednou za snimek probudi na kratkou praci (zaznam + odeslani), na kterou
    // v synchronnim rezimu ceka hra a v pipeline GPU. Vetsinu casu spi v cekani na udalost.
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) Log("SetThreadPriority selhal (%lu)", GetLastError());
    g_evBridge = evFrame;
    g_evDone = evDone;
    HANDLE waits[3] = {evFrame, parent, nullptr};
    for (;;) {
        // S prezentaci v helperu: treti udalost = novy snimek presentu (kanal vznika az pri prvnim Present hry, do
        // te doby se hleda v kratkem intervalu); periodicke probuzeni i kvuli zadosti proxy o konec a statistice.
        if (g_fgPresenter && !g_ps) FgTryAttach();  // i kdyz most bezi bez prestavky (zadne periodicke probuzeni)
        if (g_fgPresenter && !waits[2] && g_fgEvFrame) waits[2] = g_fgEvFrame;
        DWORD w = WaitForMultipleObjects(waits[2] ? 3 : 2, waits, FALSE, g_fgPresenter ? 100 : 5000);
        if (w == WAIT_OBJECT_0 + 1) {
            Log("hra skoncila, koncim (zpracovano snimku: %llu)", (unsigned long long)g_framesDone);
            break;
        }
        if (g_fgPresenter && w != WAIT_OBJECT_0) {  // snimek presentu nebo periodicke probuzeni
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            FgOnPresentMsg(t.QuadPart);
            if (g_bridgeFatal) return 1;  // zprava mostu zpracovana uvnitr selhala
            continue;
        }
        if (w != WAIT_OBJECT_0) continue;
        if (!BridgeMessage()) return 1;
    }

    if (g_fgPresenter) FgShutdown();  // vlakno presentu ukoncit, swapchain a DLSS-G feature uvolnit
    // Uklid NGX jen kdyz GPU dobehla (jinak by uvolnovani mohlo cekat navzdy).
    if (g_dev && WaitSlots(2000000000ull)) {
        DestroySized();
        if (g_ngxParams) NVSDK_NGX_VULKAN_DestroyParameters(g_ngxParams);
        g_ngxParams = nullptr;
        if (g_ngxCaps) NVSDK_NGX_VULKAN_DestroyParameters(g_ngxCaps);
        g_ngxCaps = nullptr;
        if (g_ngxInit) NVSDK_NGX_VULKAN_Shutdown1(g_dev);
        Log("NGX ukonceno");
    } else if (g_dev) {
        Log("GPU nedobehla do 2 s, koncim bez uklidu");
    }
    return 0;
}
