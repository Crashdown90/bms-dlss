// bms-dlss, spike S2 (FG varianta 2, docs/fg-prezentace-varianty.md): helper prezentuje do okna hry.
//
// [present] HelperPresent=1 (vychozi 0 = proxy se chova presne jako bez spiku):
//   * CreateDevice(Ex)/Reset(Ex): zarizeni, ktere by kreslilo do okna hry (hlavni, pripadne ANGLE z Qt menu), dostane
//     od proxy kopii parametru: Windowed = TRUE, hDeviceWindow i focus = vlastni skryte okno (velikost backbufferu,
//     vlakno proxy), D3DCREATE_NOWINDOWCHANGES, bez VSync. Hra dostane zpet sve hodnoty. DXVK tak okno hry nikdy
//     nevidi (zadna vyhradni cela obrazovka, zadny Lost pri alt-tab) a jeho swapchain je na skrytem okne.
//   * Present hlavniho zarizeni: kopie celeho backbufferu (finalni snimek i s HUD, menu a vystupem DLSS) na fronte
//     DXVK do sdileneho obrazu (3 obrazy, timeline semafory jako most), signal helperu. Druha instance helperu
//     ("bms_dlss_helper.exe <pid> present", src/helper/present.cpp) ma VkSwapchain na HWND hry, obraz do nej zkopiruje
//     (blit) a prezentuje. DXVK dal prezentuje do skryteho okna (tempo hry, strop FPS ovladace, interni logika DXVK).
//   * Zaloha: helper skonci, spadne, nereaguje nebo okno nedostane -> DXVK prezentuje primo do okna hry
//     (hDestWindowOverride). Po tvrdem ukonceni helperu zustava HWND hry pro Vulkan "in use" (spike S1), a DXVK by
//     na nem selhal (a ovladac pri chybe vraci nenulovy handle swapchainu, jehoz zniceni spadne). Proto se okno hry
//     nejdriv vyzkousi vlastnim swapchainem (sonda) - jen kdyz je volne, jde DXVK primo do nej; jinak do dcerineho
//     okna pres celou klientskou plochu hry (WS_DISABLED = mys jde do okna hry) a sonda se opakuje kazdou sekundu.
//
// Volano jen z vlakna, ktere vola Present/Reset (vlakno hry); vlastni je vlakno oken (skryta a dcerina okna, smycka
// zprav) a strazce (konec helperu -> GPU nesmi zustat cekat na semafor helperu).
//
// Frame Generation (od BMSD_VERSION 12 / BMSD_PRESENT_VERSION 2, docs/fg-dlss-plan.md): [fg] Mode != 0 pri startu hry
// zapne stejnou cestu (i bez HelperPresent=1), ale prezentuje HLAVNI helper (ma MV, hloubku a kameru z mostu DLSS,
// src/helper/fg.inl) - druhy proces se nespousti. Proxy navic:
//   * ke kazdemu snimku zapise bridgeFrame = N snimku mostu, ktery k nemu patri (Interop_TakeBridgeN; 0 = snimek hry
//     nesel mostem), sdilene obrazy maji i SAMPLED (generator cte backbuffer primo z nich),
//   * kdyz helper hlasi fgState = BMSD_FGS_ACTIVE (generuje), omezi po o_Present skutecne snimky hry na TargetFps / N
//     (N = nasobek, se kterym helper generuje: [fg] Multiplier 2 / 3 / 4; helper pak prezentuje N-krat tolik); jinak
//     (FG vypnute, pozastavene, nedostupne, zaloha) limiter nespi,
//   * pri zaloze (Fallback) hlavni helper neukoncuje - jen ho pozada, at prestane prezentovat (stop = 1 ->
//     BMSD_PST_STOPPED), most DLSS v nem bezi dal. Zaloha je i cesta pro kazde selhani FG / presenteru a smrt helperu.
// FG zap / vyp za behu ([fg] LiveSwitch=1, vychozi; docs/fg-zapvyp-vysledek.md): zarizeni hry se presmeruje do
// skryteho okna vzdy (i bez FG pri startu), Reset netreba - DXVK drzi swapchain pro kazde okno zvlast a cil se meni
// pres hDestWindowOverride. FG vypnuta = PS_OFF: helper na zadost (stop) uvolni swapchain, generator, snapshoty
// a obrazy, prezentacni okno a sdilene obrazy pryc, DXVK prezentuje primo do okna hry. FG zapnuta = nove prezentacni
// okno, helper znovu na stejnem kanalu (restartSeq), do prvniho snimku od helperu DXVK dal kresli i do okna hry.
// DLSS-G <-> FSR 2x prepina helper sam (fgMode).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <d3d9.h>
#include <psapi.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "bmsd_shared.h"
#include "dxvk_interop.h"
#include "interop.h"
#include "present.h"

namespace {

// ---- konfigurace ----
struct PCfg {
    bool on = false;
    DWORD startTimeoutMs = 10000;  // helper nema swapchain do tolika ms -> zaloha
    DWORD hangMs = 2000;           // helper nepotvrdil snimek tolik ms -> nereaguje -> zaloha
    DWORD probeMs = 1000;          // zaloha: interval sondy okna hry
    DWORD probeMaxSec = 300;       // zaloha: sondu po tolika s vzdat (zustava dcerine okno)
    int dxvkWindow = 0;            // 0 = skryte okno, 1 = viditelne okno mimo obrazovku (ladeni)
    bool child = true;             // zaloha pres dcerine okno (0 = jen skryte okno, dokud sonda neuspeje)
    bool measure = true;           // helper meri post -> blit hotov a -> zobrazeno (present_wait)
    DWORD testStopSec = 0;         // test zalohy: po N s helper pozadat o cisty konec (0 = vypnuto)
    int target = 0;                // cil helperu: 0 = dcerine okno proxy pres klientskou plochu hry, 1 = HWND hry
    // Frame Generation ([fg]):
    bool inMain = false;           // prezentuje hlavni helper ([fg] Mode != 0 pri startu hry); jinak S2 (druhy proces)
    int fgAutoMax = 6, fgAutoMin = 2;  // [fg] AutoMaxMultiplier / AutoMinMultiplier pro Multiplier=auto (v25j)
    int fgAutoUpBelow = 75;  // [fg] AutoUpBelow: auto zvysi nasobek az pri stropu skutecnych pod tolik % (v25q 85, v25s 75)
    double fgAutoUpHold = 3.0, fgAutoDownHold = 1.0;  // [fg] AutoUpHold / AutoDownHold (s): drzeni pred zmenou (v25r)
    int fgTargetFps = 0;           // [fg] TargetFps: strop vystupu v FPS, 0 = auto (f - f^2/3600 jako Reflex: 165 -> 157,4)
    int adaptCap = 0;              // [fg] RealCap: adaptivni FSR - strop skutecnych FPS (0 = vyp, jen cil)
    int adaptFill = 0;             // [fg] FillBelow: adaptivni FSR (A) - generovat az pod X skutecnych FPS (0 = auto)
    bool fgPauseInMenu = false;    // [fg] PauseInMenu: otevrene menu modu = FG pozastavit (vychozi 0 od 2026-11-21:
                                   // zmeny FG v menu maji byt videt hned; menu je v UI vrstve HUD-less)
    double fgMarginPct = 1.0;      // [fg] LimiterMargin (%): limiter hry bezi o tolik pomaleji nez TargetFps / N, aby
                                   // vystup helperu (N x) zustal tesne POD omezovacem FPS ovladace nastavenym na stejnou
                                   // hodnotu - jinak present helperu na omezovaci nahodne ceka a generovane snimky
                                   // vypadavaji (mereno: ~5 % pri presne shode)
    bool hudless = true;           // [fg] HudLess: HUD-less vstup generatoru (K5); 0 = jako v13 (HUD v obraze)
    bool adaptIdleSkip = true;     // [fg] AdaptIdleSkip: adaptivni FSR bez generovani -> bez kopie HUD-less
    // [fg] PresentStartSkip (vychozi 60): prvnich N snimku s koncem sceny po startu hry se helperu nepredava (helper
    // drzi posledni snimek nacitani, DXVK kresli do skryteho okna). Start hry rovnou do mapy (+map / +load, napr.
    // bm_c2a5a = Surface Tension) s kopii pro helper uz od prvnich snimku sceny konci chybou GPU (MMU fault v SM,
    // nvlddmkm 13 -> VK_ERROR_DEVICE_LOST zarizeni DXVK, deterministicky ~5. snimek sceny); 10 vynechanych snimku
    // staci, 60 je rezerva. Nacteni pozice / zmena mapy pozdeji za behu chybu nema.
    int startSkip = 60;
    // FG zap / vyp za behu ([fg] LiveSwitch=1, vychozi): zarizeni hry se presmeruje do skryteho okna vzdy (i bez FG pri
    // startu) a cil presentu DXVK se meni za behu pres hDestWindowOverride - bez FG DXVK prezentuje primo do okna hry
    // (stav PS_OFF, jako zaloha), s FG prezentuje helper. Reset zarizeni ani restart hry netreba.
    bool live = true;
    int fgLimAdapt = 3;  // [fg] LimiterAdapt: strop skutecnych snimku pod moznosti GPU; 1 = puvodni pokus (pilovity),
                         // 2 = s pameti hranice (drzi se pod ni, zkousi vys jen zridka); 3 = regulator podle casu GPU
                         // (helper meri post -> hotovo; obdoba Reflexu, v24u)
    int fgLimAdaptFsr = 0;  // [fg] LimiterAdaptFsr (v24z, vychozi 0 - 4K: vystup 156 -> 132-150/s, sd 0,3 -> 1,3-2,6 ms, latence 66 -> 48 ms): regulator 3 i pro adaptivni FSR (strop skutecnych snimku
                            // podle casu GPU; generovany snimek je levnejsi nez skutecny, takze nizsi strop GPU ulevi)
    bool presAtStart = false;  // [fg] Mode != 0 (nebo ForcePresenter) pri startu: presenter hned od prvniho Present
    bool fgForce = false;      // [fg] ForcePresenter=1: presenter bezi i s FG vypnutou (ladeni)
    DWORD liveRefreshMs = 800; // (+ [fg] LiveRefreshMode: 1 = novy swapchain, 2 = okno hry WS_EX_LAYERED tam a zpet)
    DWORD liveRefreshMode = 4;
// [fg] LiveRefreshMs: obnova swapchainu presenteru po zapnuti FG za behu
    bool offChild = false;    // [fg] OffTarget=1: bez FG DXVK do dcerineho okna proxy misto primo do okna hry (ladeni)
    // [fg] GamePresent=1 (vychozi): snimky FG zobrazuje proxy v procesu hry (src/gamepresent.inl) - jen tak ovladac
    // zapne VRR (G-SYNC / FreeSync v okne); 0 = jako do v19 (helper prezentuje sam, monitor na pevne frekvenci)
    bool gamePresent = true;
    bool gamePresentEarly = true;  // [fg] GamePresentEarly: helper zverejni obraz hned po kopii s cilovym casem
} cfg;
int g_sceneSeen;  // snimky s koncem sceny od startu (Present_NoteScene; jen do startSkip + 1)

// ---- okna (vlastni vlakno se smyckou zprav) ----
const UINT WMP_HIDDEN = WM_APP + 0x51;   // wParam = sirka, lParam = vyska -> HWND
const UINT WMP_CHILD = WM_APP + 0x52;    // wParam = rodic -> HWND
const UINT WMP_DESTROY = WM_APP + 0x53;  // wParam = okno
const UINT WMP_UNSTICK = WM_APP + 0x54;  // wParam = okno hry
HINSTANCE g_hinst;
HWND g_ctl;  // okno pro zpravy (HWND_MESSAGE) vlakna oken

LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WMP_HIDDEN) {
        const int cw = (int)w, ch = (int)l;
        int x = 0;
        if (cfg.dxvkWindow == 1) x = GetSystemMetrics(SM_XVIRTUALSCREEN) - cw - 64;  // mimo vsechny monitory
        HWND r = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"bms_dlss_s2_dxvk", L"bms_dlss DXVK", WS_POPUP,
                                 x, 0, cw, ch, nullptr, nullptr, g_hinst, nullptr);
        if (r && cfg.dxvkWindow == 1) ShowWindow(r, SW_SHOWNOACTIVATE);
        return (LRESULT)r;
    }
    if (m == WMP_CHILD) {
        HWND parent = (HWND)w;
        RECT rc = {};
        GetClientRect(parent, &rc);
        return (LRESULT)CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"bms_dlss_s2_child", L"",
                                        WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0, rc.right,
                                        rc.bottom, parent, nullptr, g_hinst, nullptr);
    }
    if (m == WMP_DESTROY) return DestroyWindow((HWND)w);
    if (m == WMP_UNSTICK) {  // pokus: DWM prepnout presmerovani okna (vrstvene okno tam a zpet)
        HWND g = (HWND)w;
        const LONG ex = GetWindowLongW(g, GWL_EXSTYLE);
        SetWindowLongW(g, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(g, 0, 255, LWA_ALPHA);
        SetWindowLongW(g, GWL_EXSTYLE, ex);
        SetWindowPos(g, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Log("prezentace: okno hry %p - WS_EX_LAYERED tam a zpet (pokus uvolnit snimek mrtveho swapchainu)", (void*)g);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

DWORD WINAPI WndThreadProc(void* ev) {
    WNDCLASSW wc = {};
    wc.hInstance = g_hinst;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"bms_dlss_s2_dxvk";
    RegisterClassW(&wc);
    wc.lpfnWndProc = ChildProc;
    wc.lpszClassName = L"bms_dlss_s2_child";
    RegisterClassW(&wc);
    wc.lpfnWndProc = CtlProc;
    wc.lpszClassName = L"bms_dlss_s2_ctl";
    RegisterClassW(&wc);
    g_ctl = CreateWindowExW(0, L"bms_dlss_s2_ctl", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hinst, nullptr);
    SetEvent((HANDLE)ev);
    if (!g_ctl) return 1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

bool EnsureWndThread() {
    if (g_ctl) return true;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&CtlProc, (HMODULE*)&g_hinst);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE th = ev ? CreateThread(nullptr, 0, WndThreadProc, ev, 0, nullptr) : nullptr;
    if (th) {
        WaitForSingleObject(ev, 5000);
        CloseHandle(th);
    }
    if (ev) CloseHandle(ev);
    if (!g_ctl) Log("prezentace: CHYBA - vlakno oken nevzniklo");
    return g_ctl != nullptr;
}

void DescribeWindow(HWND w, char* buf, size_t n) {
    RECT wr = {}, cr = {};
    GetWindowRect(w, &wr);
    GetClientRect(w, &cr);
    snprintf(buf, n, "okno %p: %ld,%ld %ldx%ld, klient %ldx%ld, styl 0x%08lX ex 0x%08lX%s%s", (void*)w, wr.left, wr.top,
             wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom, (unsigned long)GetWindowLongW(w, GWL_STYLE),
             (unsigned long)GetWindowLongW(w, GWL_EXSTYLE), IsWindowVisible(w) ? "" : ", neviditelne",
             IsIconic(w) ? ", minimalizovane" : "");
}

// ---- zarizeni presmerovana na skryte okno ----
struct DevInfo {
    IDirect3DDevice9* dev;
    HWND hidden;    // hDeviceWindow DXVK
    HWND target;    // puvodni okno (okno hry)
    UINT interval;  // PresentationInterval, ktery chtela hra
};
DevInfo g_devs[16];
int g_nDevs;
HWND g_gameWnd;  // okno hry (cil prvniho velkeho zarizeni)
bool g_broken;   // presmerovani nejde (vlakno oken) - helper se nespousti, DXVK kresli kam chce hra

DevInfo* FindDev(IDirect3DDevice9* d) {
    for (int i = 0; i < g_nDevs; ++i)
        if (g_devs[i].dev == d) return &g_devs[i];
    return nullptr;
}

const char* IntervalStr(UINT i) {
    return i == D3DPRESENT_INTERVAL_IMMEDIATE ? "IMMEDIATE (bez VSync)" : i <= 1 ? "ONE (VSync)" : "vic nez 1";
}

void ResizeHidden(HWND hidden, HWND target, D3DPRESENT_PARAMETERS* pp) {
    UINT w = pp->BackBufferWidth, h = pp->BackBufferHeight;
    if (!w || !h) {  // 0 = velikost okna (v okne): DXVK ji vezme ze skryteho okna -> dat mu klient okna hry
        RECT rc = {};
        GetClientRect(target, &rc);
        w = rc.right > 0 ? rc.right : 1;
        h = rc.bottom > 0 ? rc.bottom : 1;
    }
    SetWindowPos(hidden, nullptr, 0, 0, (int)w, (int)h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Redirect(D3DPRESENT_PARAMETERS* pp, HWND hidden) {
    pp->Windowed = TRUE;
    pp->hDeviceWindow = hidden;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

// ---- Vulkan (zarizeni DXVK) ----
#define P_DEV_FUNCS(X)                                                                                            \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)      \
    X(vkBindImageMemory) X(vkGetMemoryWin32HandleKHR) X(vkCreateSemaphore) X(vkGetSemaphoreWin32HandleKHR)     \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)           \
    X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkQueueSubmit) X(vkCreateFence)        \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)
#define P_INST_FUNCS(X)                                                                                           \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateWin32SurfaceKHR)          \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)

struct {
#define DECL(n) PFN_##n n = nullptr;
    P_DEV_FUNCS(DECL)
    P_INST_FUNCS(DECL)
#undef DECL
    PFN_vkGetSemaphoreCounterValue vkGetSemaphoreCounterValue = nullptr;
    PFN_vkSignalSemaphore vkSignalSemaphore = nullptr;
} vk;

ID3D9VkInteropDevice* g_idev;
IDirect3DDevice9* g_vkDev;  // zarizeni hry, ze ktereho je g_idev
VkInstance g_inst;
VkPhysicalDevice g_pd;
VkDevice g_dev;
VkQueue g_queue;
uint32_t g_qf;
uint8_t g_uuid[16];
bool g_vkReady;

const int K = BMSD_PRESENT_IMAGES;
struct PImg {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    bool fresh = true;     // jeste nikdy predan helperu (prvni bariera z UNDEFINED)
    uint64_t posted = 0;   // posledni snimek N v tomto obrazu (0 = zadny v teto generaci)
};
PImg g_img[K];
// HUD-less (K5): sdilene obrazy hudless[] (stejny format a velikost jako g_img) a vlastni RT v D3D9, do ktereho proxy
// v bode pred HUD kopiruje barvu sceny (Present_HudlessTarget). g_hlFrame = snimek, ktery se prave predava, ma platny
// HUD-less v RT (Present_HudlessFrame z OnFrameEnd proxy).
PImg g_hl[K];
bool g_hlImages;                 // g_hl v teto generaci existuji
IDirect3DSurface9* g_hlRT;       // D3DPOOL_DEFAULT: pred Resetem uvolnit
UINT g_hlRTW, g_hlRTH;
D3DFORMAT g_hlRTFmt;
bool g_hlFrame;
bool g_hlRTFailLogged;
ULONGLONG g_hlRTRetryTick;  // po selhani CreateRenderTarget (malo VRAM) dalsi pokus az po tomto case (ne kazdy snimek)
uint32_t g_w, g_h;
VkFormat g_fmt = VK_FORMAT_UNDEFINED;
VkSemaphore g_semReady, g_semDone;
VkCommandPool g_pool;
VkCommandBuffer g_cmd[K];
VkFence g_fence[K];
bool g_fenceUsed[K];
IDirect3DSurface9* g_ref[K];  // backbuffer drzeny, dokud kopie slotu nedobehne
uint64_t g_N;                 // posledni zverejneny snimek
const uint64_t kFenceNs = 2000000000ull;

// ---- helper ----
HANDLE g_map, g_evFrame, g_evDone, g_proc, g_watch;
DWORD g_helperPid;
BmsdPresentShared* g_sh;
int32_t g_gen;
CRITICAL_SECTION g_semCs;  // signal semDone z CPU (strazce i vlakno hry)
volatile LONG64 g_maxDoneWait;  // nejvyssi hodnota semDone, na kterou GPU (fronta DXVK) ceka

// Prezentacni okno helperu: dcerine okno pres celou klientskou plochu hry (vlastni vlakno oken proxy, WS_DISABLED =
// mys a fokus ma okno hry). Swapchain helperu je na nem, ne na HWND hry: po tvrdem konci helperu (zabit) DWM dal
// ukazuje posledni snimek mrtveho swapchainu na danem okne a novy swapchain (DXVK, i do jineho dcerineho okna) neni
// videt (overeno ve hre 2026-11-18) - proto se pri zaloze toto okno znici a DXVK kresli do okna hry, na kterem
// zadny swapchain nikdy nebyl.
HWND g_presWnd;

void SyncChild(HWND child) {
    RECT pr = {}, cr = {};
    GetClientRect(g_gameWnd, &pr);
    GetClientRect(child, &cr);
    if (pr.right != cr.right || pr.bottom != cr.bottom)
        SetWindowPos(child, nullptr, 0, 0, pr.right, pr.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

// ---- stav ----
// PS_OFF = presenter vypnuty volbou (FG vypnuta, [fg] LiveSwitch): DXVK prezentuje primo do okna hry, helper nic
// nedrzi; zapnuti FG -> PS_STARTING. PS_FALLBACK = chyba / konec helperu, FG az po restartu hry.
enum PState { PS_IDLE, PS_STARTING, PS_HELPER, PS_FALLBACK, PS_OFF };
PState g_state = PS_IDLE;
const char* g_liveBlock;   // proc zarizeni hry neni presmerovane a FG za behu nejde (nullptr = jde)
bool g_dxvkOnGame;        // DXVK uz nekdy prezentoval primo do okna hry (drzi na nem swapchain - sonda by hlasila
                           // "obsazene" kvuli nemu samotnemu)
unsigned g_switches;       // pocet prepnuti presenteru za behu (log)
LONGLONG g_switchQpc;      // zacatek posledniho zapnuti presenteru (mereni: zapnuti -> prvni snimek od helperu)
ULONGLONG g_startTick, g_ackTick;
uint64_t g_lastAck;
// zaloha
HWND g_child;
bool g_direct;  // DXVK prezentuje primo do okna hry
ULONGLONG g_fbTick, g_nextProbe;
bool g_probeGaveUp;
VkResult g_lastProbe = VK_SUCCESS;
const char* g_fbReason = "";

// ---- statistika (log kazdych ~5 s) ----
LARGE_INTEGER g_qpf;
LONGLONG g_statQpc;
struct Acc {
    double sum = 0, max = 0;
    unsigned n = 0;
    void add(double v) {
        sum += v;
        if (v > max) max = v;
        ++n;
    }
    double avg() const { return n ? sum / n : 0.0; }
};
unsigned g_stPresents, g_stPosted, g_stDropped, g_stNotReady;
unsigned g_stHl, g_stHlMiss, g_stHlSnaps;  // K5: snimky s HUD-less / FG bez HUD-less / kopii do RT (StretchRect)
Acc g_stCopy, g_stReuse, g_stSlot, g_stOPresent;  // ...; o_Present (DXVK do skryteho okna) na CPU hry
LONGLONG g_oPresentQpc;  // zaznam+flush+odeslani; cekani na prevzeti obrazu; cekani na fence slotu
uint32_t g_helperStatSeq;
void* g_lastCaller;
bool g_vtLogged;

LONGLONG Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double Ms(LONGLONG a, LONGLONG b) { return (double)(b - a) * 1000.0 / (double)g_qpf.QuadPart; }

// ---- Frame Generation (presenter v hlavnim helperu) ----
volatile LONG g_fgMode = BMSD_FG_OFF;  // pozadovany rezim BMSD_FG_* (ini pri startu, pak menu)
volatile LONG g_fgPause;               // 1 = menu modu otevrene a [fg] PauseInMenu=1
volatile LONG g_fgMult = 2;            // pozadovany nasobek 2..6 ([fg] Multiplier pri startu, pak menu); auto: prave zvoleny
volatile LONG g_fgMultAuto = 0;        // 1 = [fg] Multiplier=auto (v25j): nasobek DLSS-G / FSR voli FgAutoMult
// BmsdShared::fgMultiplier: nasobek (bity 0-7) + 0x100 = auto (helper pak FSR i pri 2x generuje vlastnim AFG, v25n)
static uint32_t FgMultShared() { return (uint32_t)g_fgMult | (g_fgMultAuto ? 0x100u : 0u); }
uint32_t g_fgTarget;                   // strop vystupu v FPS (ini nebo auto, dolu), 0 = zatim neznamy
double g_fgTargetD;                    // totez presne (auto f - f^2/3600 neni cele cislo)
int g_fgHz;                            // obnovovaci frekvence monitoru okna hry pri poslednim zjisteni (log)
uint64_t g_curBridgeN;                 // B tohoto snimku hry (Interop_TakeBridgeN v Present_OnPresent)
// Limiter skutecnych snimku (TargetFps / nasobek): jen kdyz helper hlasi BMSD_FGS_ACTIVE
HANDLE g_limTimer;
bool g_limTimerTried;
LONGLONG g_limNext;                    // konec slotu minuleho snimku (QPC); 0 = limiter nebezi
double g_limScale = 1.0, g_limScaleLogged = 1.0;  // rezerva GPU limiteru ([fg] LimiterAdapt), 1 = plny strop
uint64_t g_limBits;                    // poslednich 64 snimku hlavniho zarizeni: 1 = limiter spal
Acc g_stLimSleep, g_stLimErr;          // spanek limiteru, |probuzeni - konec slotu|
unsigned g_stLimLate, g_stLimResync;   // snimek pozde (bez spanku, faze drzena) / moc pozde (novy zacatek slotu)
// Posledni interval statistiky helperu, ve kterem FG generovalo (pro radek "Stav" v menu: otevrene menu FG pozastavi)
uint32_t g_fgLastStatSeq;
int g_fgLastMode;                      // rezim, ke kteremu cisla patri (BMSD_FG_*), 0 = zadna
float g_fgLastReal, g_fgLastOut, g_fgLastEval;
bool g_fgLastShare;                    // posledni interval statistiky byl generovany
// Statistiku cte Present_FgInfo (menu, kazdy snimek s otevrenym menu) i radek logu (kazdych ~5 s)
void FgLatchStats() {
    if (!g_sh || g_sh->statSeq == g_fgLastStatSeq) return;
    g_fgLastStatSeq = g_sh->statSeq;
    MemoryBarrier();
    const uint32_t real = g_sh->statFgReal, gen = g_sh->statFgGen, mode = g_sh->fgActiveMode;
    // adaptivni FSR generuje jen cast snimku (nebo nic nad cilem): cisla plati vzdy
    g_fgLastShare = real > 0 && (gen * 10 >= real * 8 || mode == BMSD_FG_FSRADAPT);
    if (!g_fgLastShare || !mode) return;
    g_fgLastMode = (int)mode;
    g_fgLastReal = g_sh->statFgRealFps;
    g_fgLastOut = g_sh->statFgOutFps;
    g_fgLastEval = g_sh->statFgEvalMs;
}

const char* FgModeName(int m) {
    return m == BMSD_FG_DLSSG ? "DLSS-G" : m == BMSD_FG_FSR2X ? "FSR" : m == BMSD_FG_FSRADAPT ? "FSR adaptivni"
                                                                                                    : "vypnuto";
}
const char* FgStateName(int s) {
    switch (s) {
    case BMSD_FGS_OFF: return "vypnuto";
    case BMSD_FGS_ACTIVE: return "generuje";
    case BMSD_FGS_PAUSED: return "pozastaveno";
    case BMSD_FGS_UNAVAILABLE: return "nedostupne";
    case BMSD_FGS_FAILED: return "selhalo";
    default: return "?";
    }
}
// Rezimy, ktere helper umi (ostatni se pri startu berou jako vypnuto - presenter by bezel zbytecne)
bool FgModeImplemented(int m) { return m == BMSD_FG_DLSSG || m == BMSD_FG_FSR2X || m == BMSD_FG_FSRADAPT; }

int ParseFgMode(const wchar_t* in) {
    wchar_t s[32];
    size_t n = 0;
    while (*in == L' ' || *in == L'\t') ++in;
    for (; *in && *in != L';' && *in != L'#' && n < 31; ++in) s[n++] = *in;
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t')) --n;
    s[n] = 0;
    if (!_wcsicmp(s, L"dlssg") || !_wcsicmp(s, L"dlss-g") || !_wcsicmp(s, L"dlss")) return BMSD_FG_DLSSG;
    if (!_wcsicmp(s, L"fsr2x") || !_wcsicmp(s, L"fsr")) return BMSD_FG_FSR2X;
    if (!_wcsicmp(s, L"fsradaptive")) return BMSD_FG_FSRADAPT;
    const long v = wcstol(s, nullptr, 0);
    return v > 0 && v < BMSD_FG_COUNT ? (int)v : BMSD_FG_OFF;
}

// Adaptivni FSR: nastaveni pro helper (cil vystupu po rezerve LimiterMargin, frekvence, hranice, pacing, strop)
void FgPublishAdapt() {
    if (!g_sh || !cfg.inMain) return;
    const double tgt = g_fgTargetD * (1.0 - cfg.fgMarginPct / 100.0);
    g_sh->adaptTargetMilli = tgt > 0.0 ? (uint32_t)(tgt * 1000.0) : 0u;
    g_sh->refreshMilli = g_fgHz > 1 ? (uint32_t)g_fgHz * 1000u : 0u;
    g_sh->adaptFillMilli = cfg.adaptFill > 0 ? (uint32_t)cfg.adaptFill * 1000u : 0u;
    g_sh->adaptPacing = 0u;  // jen rozvrh A (v24z4)
    g_sh->adaptRealCap = cfg.adaptCap > 0 ? (uint32_t)cfg.adaptCap : 0u;
    MemoryBarrier();
}

// Strop / cil vystupu: [fg] TargetFps, nebo auto z obnovovaci frekvence monitoru, na kterem je okno hry, jako Reflex
// (NVIDIA Ultra Low Latency + V-Sync + VRR): f - f^2 / 3600 (165 Hz -> 157,4; 144 -> 138,2; 240 -> 224) - pod
// horni hranici VRR. Zapisuje se i helperu (log, adaptivni FSR).
void UpdateFgTarget() {
    int hz = 0;
    HMONITOR mon = MonitorFromWindow(g_gameWnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi)) {
        DEVMODEW dm = {};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) hz = (int)dm.dmDisplayFrequency;
    }
    const bool hzKnown = hz > 1;
    if (!hzKnown) hz = 60;  // 0 / 1 = "vychozi" frekvence adapteru
    g_fgHz = hzKnown ? hz : 0;
    double t = cfg.fgTargetFps > 0 ? (double)cfg.fgTargetFps : (double)hz - (double)hz * hz / 3600.0;
    if (t < 20.0) t = 20.0;
    const uint32_t ti = (uint32_t)floor(t + 1e-6);
    if (fabs(t - g_fgTargetD) > 1e-6) {
        g_fgTargetD = t;
        g_fgTarget = ti;
        if (cfg.fgTargetFps > 0)
            Log("prezentace: FG strop vystupu %u FPS ([fg] TargetFps, monitor %d Hz), skutecnych snimku hry %.1f FPS "
                "(nasobek %ldx)",
                ti, g_fgHz, t / (double)g_fgMult, g_fgMult);
        else
            Log("prezentace: FG strop vystupu %.2f FPS (auto: %d Hz - f^2/3600), skutecnych snimku hry %.1f FPS (nasobek "
                "%ldx)",
                t, hz, t / (double)g_fgMult, g_fgMult);
    }
    if (g_sh && cfg.inMain) g_sh->targetFps = ti;
    FgPublishAdapt();
}

// Spanek do casu "target" (QPC): hi-res waitable timer, poslednich ~0.4 ms aktivne (presnost slotu)
void LimiterSleepUntil(LONGLONG target) {
    if (!g_limTimerTried) {
        g_limTimerTried = true;
        g_limTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                            TIMER_ALL_ACCESS);
        if (!g_limTimer)
            Log("prezentace: WARN - hi-res casovac neni (%lu), limiter FG spi pres Sleep (mene presne)", GetLastError());
    }
    const LONGLONG spin = g_qpf.QuadPart * 4 / 10000;  // 0.4 ms
    for (;;) {
        const LONGLONG left = target - Qpc();
        if (left <= 0) return;
        if (left > spin) {
            if (g_limTimer) {
                LARGE_INTEGER due;
                due.QuadPart = -((left - spin) * 10000000 / g_qpf.QuadPart);  // relativne, jednotky 100 ns
                if (due.QuadPart < 0 && SetWaitableTimer(g_limTimer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(g_limTimer, 200);
                    continue;
                }
            }
            if (left > g_qpf.QuadPart * 2 / 1000) {  // zaloha bez casovace: Sleep(1) jen kdyz zbyva > 2 ms
                Sleep(1);
                continue;
            }
        }
        YieldProcessor();
    }
}

const char* VkStr(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    default: return "jina chyba";
    }
}

VkImageMemoryBarrier Barrier(VkImage img, VkAccessFlags src, VkAccessFlags dst, VkImageLayout oldL,
                             VkImageLayout newL, uint32_t srcQ = VK_QUEUE_FAMILY_IGNORED,
                             uint32_t dstQ = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = srcQ;
    b.dstQueueFamilyIndex = dstQ;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

bool InitVk(IDirect3DDevice9* d) {
    if (Interop_VkBlocked()) return Log("prezentace: DXVK hry ma neznamou verzi - interop se nepouziva"), false;
    HRESULT hr = d->QueryInterface(__uuidof(ID3D9VkInteropDevice), (void**)&g_idev);
    if (FAILED(hr) || !g_idev) return Log("prezentace: ID3D9VkInteropDevice nedostupne (0x%08X)", hr), false;
    g_idev->GetVulkanHandles(&g_inst, &g_pd, &g_dev);
    uint32_t qi = 0;
    g_idev->GetSubmissionQueue(&g_queue, &qi, &g_qf);
    HMODULE vkdll = GetModuleHandleW(L"vulkan-1.dll");
    auto gipa = vkdll ? (PFN_vkGetInstanceProcAddr)GetProcAddress(vkdll, "vkGetInstanceProcAddr") : nullptr;
    if (!gipa) return Log("prezentace: vkGetInstanceProcAddr nenalezeno"), false;
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(g_inst, "vkGetDeviceProcAddr");
#define LOADD(n)                                                                     \
    vk.n = (PFN_##n)gdpa(g_dev, #n);                                                 \
    if (!vk.n) return Log("prezentace: chybi funkce %s", #n), false;
#define LOADI(n)                                                                     \
    vk.n = (PFN_##n)gipa(g_inst, #n);                                                \
    if (!vk.n) return Log("prezentace: chybi funkce %s", #n), false;
    P_DEV_FUNCS(LOADD)
    P_INST_FUNCS(LOADI)
#undef LOADD
#undef LOADI
    vk.vkGetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue)gdpa(g_dev, "vkGetSemaphoreCounterValue");
    if (!vk.vkGetSemaphoreCounterValue)
        vk.vkGetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue)gdpa(g_dev, "vkGetSemaphoreCounterValueKHR");
    vk.vkSignalSemaphore = (PFN_vkSignalSemaphore)gdpa(g_dev, "vkSignalSemaphore");
    if (!vk.vkSignalSemaphore) vk.vkSignalSemaphore = (PFN_vkSignalSemaphore)gdpa(g_dev, "vkSignalSemaphoreKHR");
    if (!vk.vkGetSemaphoreCounterValue || !vk.vkSignalSemaphore)
        return Log("prezentace: chybi vkSignalSemaphore / vkGetSemaphoreCounterValue"), false;

    VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
    vk.vkGetPhysicalDeviceProperties2(g_pd, &p2);
    memcpy(g_uuid, idp.deviceUUID, 16);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf};
    if (vk.vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, (uint32_t)K};
    if (vk.vkAllocateCommandBuffers(g_dev, &cai, g_cmd) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int i = 0; i < K; ++i)
        if (vk.vkCreateFence(g_dev, &fci, nullptr, &g_fence[i]) != VK_SUCCESS) return false;
    for (VkSemaphore* s : {&g_semReady, &g_semDone}) {
        VkExportSemaphoreCreateInfo esi = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr,
                                           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
        VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &esi,
                                         VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
        if (vk.vkCreateSemaphore(g_dev, &sci, nullptr, s) != VK_SUCCESS)
            return Log("prezentace: vytvoreni semaforu selhalo"), false;
    }
    g_vkDev = d;
    g_vkReady = true;
    Log("prezentace: Vulkan DXVK pripraven (VkDevice %p, fronta family %u)", (void*)g_dev, g_qf);
    return true;
}

#include "gamepresent.inl"

uint64_t DupToHelper(HANDLE h) {
    HANDLE remote = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, g_proc, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) return 0;
    return (uint64_t)(uintptr_t)remote;
}

// Handle zdvojeny do helperu zavrit (obraz se nakonec nepouzije)
void CloseRemoteHandle(uint64_t h) {
    if (h && g_proc) DuplicateHandle(g_proc, (HANDLE)(uintptr_t)h, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
}

uint64_t ExportSem(VkSemaphore s) {
    VkSemaphoreGetWin32HandleInfoKHR hi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr, s,
                                           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
    HANDLE h = nullptr;
    if (vk.vkGetSemaphoreWin32HandleKHR(g_dev, &hi, &h) != VK_SUCCESS || !h) return 0;
    uint64_t r = DupToHelper(h);
    CloseHandle(h);
    return r;
}

// Fronta DXVK nesmi zustat cekat na semDone, kdyz helper skoncil (signal uz nikdy neprijde). Signal z CPU na
// nejvyssi hodnotu, na kterou GPU muze cekat (vola strazce i vlakno hry).
void ReleaseGpuWaits(const char* who) {
    if (!g_vkReady || !g_semDone) return;
    EnterCriticalSection(&g_semCs);
    uint64_t need = (uint64_t)InterlockedCompareExchange64(&g_maxDoneWait, 0, 0);
    if (g_sh && g_sh->helperAck > need) need = g_sh->helperAck;
    uint64_t cur = 0;
    if (need && vk.vkGetSemaphoreCounterValue(g_dev, g_semDone, &cur) == VK_SUCCESS && cur < need) {
        VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, g_semDone, need};
        const VkResult r = vk.vkSignalSemaphore(g_dev, &si);
        Log("prezentace: %s - semafor helperu %llu -> %llu z CPU (%s)", who, (unsigned long long)cur,
            (unsigned long long)need, VkStr(r));
    }
    LeaveCriticalSection(&g_semCs);
}

DWORD WINAPI WatchProc(void*) {
    WaitForSingleObject(g_proc, INFINITE);
    ReleaseGpuWaits("strazce: helper skoncil");
    return 0;
}

bool StartHelper(IDirect3DDevice9* d) {
    const DWORD pid = GetCurrentProcessId();
    wchar_t name[128];
    swprintf(name, 128, L"Local\\bms_dlss_%lu_present", pid);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(BmsdPresentShared), name);
    if (!g_map) return false;
    g_sh = (BmsdPresentShared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BmsdPresentShared));
    if (!g_sh) return false;
    memset(g_sh, 0, sizeof *g_sh);
    g_sh->magic = BMSD_PRESENT_MAGIC;
    g_sh->version = BMSD_PRESENT_VERSION;
    memcpy(g_sh->deviceUUID, g_uuid, 16);
    g_sh->gameHwnd = (uint64_t)(uintptr_t)g_presWnd;  // cil helperu = prezentacni (dcerine) okno
    g_sh->measureDisplay = cfg.measure ? 1 : 0;
    if (cfg.inMain) {  // vse, co helper cte hned po otevreni mapovani, musi byt zapsano pred jeho spustenim
        g_sh->presenterInMain = 1;
        g_sh->gpEnable = g_gpActive ? (1u | ((cfg.gamePresentEarly || (g_gpDebug & 16384)) ? 2u : 0u)) : 0u;
        g_sh->fgMode = (uint32_t)g_fgMode;
        g_sh->fgMultiplier = FgMultShared();
        g_sh->fgPause = g_fgPause ? 1 : 0;
        UpdateFgTarget();
    }
    swprintf(name, 128, L"Local\\bms_dlss_%lu_pframe", pid);
    g_evFrame = CreateEventW(nullptr, FALSE, FALSE, name);
    swprintf(name, 128, L"Local\\bms_dlss_%lu_pdone", pid);
    g_evDone = CreateEventW(nullptr, FALSE, FALSE, name);
    if (!g_evFrame || !g_evDone) return false;
    if (cfg.inMain) {
        // Prezentuje hlavni helper (most DLSS): spustit ho uz ted (jinak startuje az pri prvnim konci sceny) a drzet
        // vlastni kopii handle procesu (most ho pri vypnuti zavira po svem).
        MemoryBarrier();
        Interop_FgPresenterStarting();  // most jeste nezacal: prvni konce sceny vynechat ([fg] BridgeStartSkip)
        HANDLE hp = Interop_EnsureHelper(d);
        if (!hp) return Log("prezentace: hlavni helper nebezi (most selhal nebo je vypnuty)"), false;
        if (!DuplicateHandle(GetCurrentProcess(), hp, GetCurrentProcess(), &g_proc, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
            !g_proc) {
            g_proc = nullptr;
            return Log("prezentace: handle hlavniho helperu nejde zdvojit (chyba %lu)", GetLastError()), false;
        }
        g_helperPid = GetProcessId(g_proc);
    } else {
        std::wstring exe = std::wstring(ProxyDir()) + L"\\bms_dlss_helper.exe";
        std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring(pid) + L" present";
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, ProxyDir(), &si,
                            &pi))
            return Log("prezentace: nelze spustit %ls (chyba %lu)", exe.c_str(), GetLastError()), false;
        CloseHandle(pi.hThread);
        g_proc = pi.hProcess;
        g_helperPid = pi.dwProcessId;
    }
    g_sh->semReadyHandle = ExportSem(g_semReady);
    g_sh->semDoneHandle = ExportSem(g_semDone);
    if (!g_sh->semReadyHandle || !g_sh->semDoneHandle) return Log("prezentace: export semaforu selhal"), false;
    g_watch = CreateThread(nullptr, 0, WatchProc, nullptr, 0, nullptr);
    Log("prezentace: %s (pid %lu), okno hry %p, prezentacni okno helperu %p",
        cfg.inMain ? "prezentuje hlavni helper" : "helper spusten", g_helperPid, (void*)g_gameWnd, (void*)g_presWnd);
    return true;
}

bool FinishSlot(int i, uint64_t ns) {
    if (g_fenceUsed[i]) {
        const VkResult r = vk.vkWaitForFences(g_dev, 1, &g_fence[i], VK_TRUE, ns);
        if (r != VK_SUCCESS) return false;
        vk.vkResetFences(g_dev, 1, &g_fence[i]);
        g_fenceUsed[i] = false;
    }
    if (g_ref[i]) {
        g_ref[i]->Release();
        g_ref[i] = nullptr;
    }
    return true;
}

bool FinishAll() {
    bool ok = true;
    for (int i = 0; i < K; ++i) ok = FinishSlot(i, kFenceNs) && ok;
    return ok;
}

void DestroyImages() {
    for (PImg* arr : {g_img, g_hl})
        for (int i = 0; i < K; ++i) {
            PImg& p = arr[i];
            if (p.img) vk.vkDestroyImage(g_dev, p.img, nullptr);
            if (p.mem) vk.vkFreeMemory(g_dev, p.mem, nullptr);
            p = PImg();
        }
    g_hlImages = false;
    if (g_sh) g_sh->hudlessImages = 0;
    g_w = g_h = 0;
}

// HUD-less RT v D3D9 (vlastni povrch; pred Resetem a v zaloze uvolnit - kopie nad nim musi dobehnout)
void ReleaseHudlessRT() {
    if (g_hlRT) g_hlRT->Release();
    g_hlRT = nullptr;
    g_hlRTW = g_hlRTH = 0;
    g_hlFrame = false;
}

// Jeden sdileny obraz (export pameti OPAQUE_WIN32, handle zdvojeny do procesu helperu)
bool MakeSharedImage(PImg& p, BmsdImage& bi, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                     const VkPhysicalDeviceMemoryProperties& mp) {
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, ht};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
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
    VkImage img = VK_NULL_HANDLE;
    if (vk.vkCreateImage(g_dev, &ci, nullptr, &img) != VK_SUCCESS) return false;
    p.img = img;
    VkMemoryRequirements req = {};
    vk.vkGetImageMemoryRequirements(g_dev, p.img, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; ++t)
        if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = t;
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, p.img};
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded,
                                      (VkExternalMemoryHandleTypeFlags)ht};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, req.size, type};
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (type == UINT32_MAX || vk.vkAllocateMemory(g_dev, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    p.mem = mem;
    if (vk.vkBindImageMemory(g_dev, p.img, p.mem, 0) != VK_SUCCESS) return false;
    VkMemoryGetWin32HandleInfoKHR hi = {VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, p.mem, ht};
    HANDLE handle = nullptr;
    if (vk.vkGetMemoryWin32HandleKHR(g_dev, &hi, &handle) != VK_SUCCESS || !handle) return false;
    bi.handle = DupToHelper(handle);
    CloseHandle(handle);
    bi.allocSize = req.size;
    bi.format = fmt;
    bi.usage = usage;
    bi.width = w;
    bi.height = h;
    return bi.handle != 0;
}

bool CreateImages(uint32_t w, uint32_t h, VkFormat fmt) {
    if (!FinishAll()) return Log("prezentace: kopie nedobehly pred novymi obrazy"), false;
    DestroyImages();
    VkPhysicalDeviceMemoryProperties mp = {};
    vk.vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (cfg.inMain) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;  // FG: generator cte backbuffer primo z obrazu
    bool ok = true;
    for (int i = 0; i < K && ok; ++i) ok = MakeSharedImage(g_img[i], g_sh->img[i], w, h, fmt, usage, mp);
    if (!ok) {
        Log("prezentace: CHYBA - sdilene obrazy %ux%u nejdou vytvorit", w, h);
        DestroyImages();
        // handly uz zdvojene do helperu: generace se nezvysi, helper je neimportuje ani nezavre -> zavrit zde
        for (int i = 0; i < K; ++i) {
            if (g_sh->img[i].handle) CloseRemoteHandle(g_sh->img[i].handle);
            g_sh->img[i] = BmsdImage();
        }
        return false;
    }
    // HUD-less (K5): jen s presenterem FG; kdyz nejdou vytvorit, FG bezi bez HUD-less (jako v13)
    bool hl = cfg.inMain && cfg.hudless;
    for (int i = 0; i < K && hl; ++i) hl = MakeSharedImage(g_hl[i], g_sh->hudless[i], w, h, fmt, usage, mp);
    if (cfg.inMain && cfg.hudless && !hl) {
        Log("prezentace: WARN - obrazy HUD-less %ux%u nejdou vytvorit, FG bez HUD-less", w, h);
        for (int i = 0; i < K; ++i) {
            if (g_hl[i].img) vk.vkDestroyImage(g_dev, g_hl[i].img, nullptr);
            if (g_hl[i].mem) vk.vkFreeMemory(g_dev, g_hl[i].mem, nullptr);
            if (g_sh->hudless[i].handle) CloseRemoteHandle(g_sh->hudless[i].handle);
            g_hl[i] = PImg();
            g_sh->hudless[i] = BmsdImage();
        }
    }
    g_hlImages = hl;
    g_sh->hudlessImages = hl ? 1 : 0;
    g_w = w;
    g_h = h;
    g_fmt = fmt;
    g_sh->width = w;
    g_sh->height = h;
    MemoryBarrier();
    g_sh->generation = ++g_gen;
    Log("prezentace: sdilene obrazy %ux%u (VkFormat %d) x%d%s, generace %d", w, h, (int)fmt, K,
        hl ? " + HUD-less x3" : "", g_gen);
    return true;
}

// Okno hry pro Vulkan volne? Vlastni swapchain na zarizeni DXVK: uspech = hned zrusit. Chyba ovladace: pri
// VK_ERROR_NATIVE_WINDOW_IN_USE_KHR vraci nenulovy handle, ktery se nesmi znicit - zahodit.
bool ProbeWindow(HWND w) {
    VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = w;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkResult r = vk.vkCreateWin32SurfaceKHR(g_inst, &sci, nullptr, &surf);
    if (r != VK_SUCCESS) {
        if (r != g_lastProbe) Log("prezentace: sonda okna hry - surface %s", VkStr(r));
        g_lastProbe = r;
        return false;
    }
    VkSurfaceCapabilitiesKHR caps = {};
    vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, surf, &caps);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) ext = {g_w ? g_w : 64, g_h ? g_h : 64};
    if (!ext.width || !ext.height) {  // minimalizovane: zatim nevime
        vk.vkDestroySurfaceKHR(g_inst, surf, nullptr);
        return false;
    }
    uint32_t nf = 1;
    VkSurfaceFormatKHR sf = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, surf, &nf, &sf);  // prvni format staci (VK_INCOMPLETE nevadi)
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surf;
    ci.minImageCount = caps.minImageCount ? caps.minImageCount : 2;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    // Zarizeni DXVK: vytvoreni / zruseni swapchainu sahne na frontu -> pod zamkem fronty DXVK (bez nej pady
    // v nvoglv32 proti vlaknu DXVK, viz GpQLock v gamepresent.inl)
    if (g_idev) g_idev->LockSubmissionQueue();
    r = vk.vkCreateSwapchainKHR(g_dev, &ci, nullptr, &sc);
    if (r == VK_SUCCESS && sc) vk.vkDestroySwapchainKHR(g_dev, sc, nullptr);
    if (g_idev) g_idev->ReleaseSubmissionQueue();
    if (r != VK_SUCCESS && sc && r != g_lastProbe)
        Log("prezentace: sonda: vkCreateSwapchainKHR %s vratil nenulovy handle (chyba ovladace) - zahazuji", VkStr(r));
    vk.vkDestroySurfaceKHR(g_inst, surf, nullptr);
    if (r != g_lastProbe && r != VK_SUCCESS) Log("prezentace: sonda okna hry: %s", VkStr(r));
    g_lastProbe = r;
    return r == VK_SUCCESS;
}

// Helper ukoncit: nejdriv pozadat (sam uvolni swapchain), az pak TerminateProcess.
void StopHelper(DWORD waitMs) {
    if (!g_proc) return;
    if (cfg.inMain) {
        // Hlavni helper se neukoncuje (bezi v nem most DLSS): jen ho pozadat, at prestane prezentovat a uvolni
        // swapchain. Kdyz se jeste nepripojil (helperReady = 0), neni na co cekat - stop uvidi pri pripojeni.
        bool dead = WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0;
        bool stopped = false;
        if (!dead) {
            g_sh->stop = 1;
            MemoryBarrier();
            SetEvent(g_evFrame);
            const ULONGLONG t0 = GetTickCount64();
            while (g_sh->helperReady) {
                MemoryBarrier();
                const int32_t st = g_sh->helperState;
                stopped = !g_sh->swapchainAlive && (st == BMSD_PST_STOPPED || st == BMSD_PST_FAILED);
                if (stopped || GetTickCount64() - t0 >= waitMs) break;
                if (WaitForSingleObject(g_proc, 5) == WAIT_OBJECT_0) {
                    dead = true;
                    break;
                }
            }
        }
        Log("prezentace: hlavni helper %s (stav presenteru %d, swapchain %s, zprava: %s)",
            dead ? "uz nebezi" : stopped ? "prestal prezentovat" : g_sh->helperReady
                ? "na zadost neprestal prezentovat vcas (prezentacni okno se presto znici)" : "se jeste nepripojil",
            g_sh->helperState, g_sh->swapchainAlive ? "NEUVOLNEN" : "uvolnen", g_sh->helperMsg[0] ? g_sh->helperMsg : "-");
        // Zivy helper signaly semDone pro potvrzene snimky odesle sam (dvoji signal stejne hodnoty je chyba); po jeho
        // konci uz zadny neprijde -> uvolnit z CPU (totez dela strazce WatchProc).
        if (dead) ReleaseGpuWaits("helper skoncil");
        return;
    }
    if (WaitForSingleObject(g_proc, 0) != WAIT_OBJECT_0) {
        g_sh->stop = 1;
        MemoryBarrier();
        SetEvent(g_evFrame);
        if (WaitForSingleObject(g_proc, waitMs) != WAIT_OBJECT_0) {
            Log("prezentace: helper na zadost do %lu ms neskoncil - TerminateProcess (okno hry pak muze zustat "
                "obsazene)", waitMs);
            TerminateProcess(g_proc, 5);
            WaitForSingleObject(g_proc, 3000);
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(g_proc, &code);
    Log("prezentace: helper skoncil (kod %lu, swapchain %s, stav %d, zprava: %s)", code,
        g_sh->swapchainAlive ? "NEUVOLNEN (tvrdy konec?)" : "uvolnen", g_sh->helperState,
        g_sh->helperMsg[0] ? g_sh->helperMsg : "-");
    ReleaseGpuWaits("helper skoncil");
}

// Presenter FG presel do zalohy (jednou true): proxy ukaze OSD (Present_TakeFallbackNote)
volatile LONG g_fbNote;
// Bezpecny start ([fg] SafeStart): duvod z logu minuleho spusteni (proxy, Present_SetSafeStart), FG vypnuta
char g_safeStartWhy[96];
bool g_safeStartHit;

void Fallback(const char* reason) {
    const bool helperRan = g_proc != nullptr;
    if (g_state != PS_FALLBACK && cfg.inMain && g_fgMode != BMSD_FG_OFF) InterlockedExchange(&g_fbNote, 1);
    StopHelper(1000);
    GpStop("zaloha");  // swapchain proxy pryc pred znicenim prezentacniho okna
    g_gpActive = false;
    g_state = PS_FALLBACK;
    g_fbReason = reason;
    g_fbTick = GetTickCount64();
    g_direct = false;
    g_probeGaveUp = false;
    // Prezentacni okno helperu znicit (helper uz nebezi): s nim zmizi i swapchain helperu, i kdyz ho helper
    // neuvolnil (zabit) - jinak by DWM v tom miste dal ukazoval jeho posledni snimek.
    if (g_presWnd && g_presWnd == g_gameWnd) {  // Target=1: okno hry nejde znicit
        if (helperRan && g_sh && g_sh->swapchainAlive && EnsureWndThread())
            PostMessageW(g_ctl, WMP_UNSTICK, (WPARAM)g_gameWnd, 0);
        g_presWnd = nullptr;
    } else if (g_presWnd) {
        const BOOL ok = (BOOL)SendMessageW(g_ctl, WMP_DESTROY, (WPARAM)g_presWnd, 0);
        Log("prezentace: prezentacni okno helperu %p %s (swapchain helperu %s)", (void*)g_presWnd,
            ok ? "zniceno" : "NEJDE znicit", helperRan && g_sh && g_sh->swapchainAlive ? "neuvolnen" : "uvolnen");
        g_presWnd = nullptr;
    }
    // Okno hry (zadny swapchain na nem nebyl): vyzkouset - volne -> DXVK primo; obsazene -> dcerine okno a sonda.
    // (DXVK uz do okna hry prezentoval - FG byla za behu vypnuta: swapchain na nem drzi DXVK sam, sonda by selhala.)
    const bool free = g_dxvkOnGame || (g_vkReady && ProbeWindow(g_gameWnd));
    if (free || !g_vkReady) {
        g_direct = true;
        if (g_child) ShowWindowAsync(g_child, SW_HIDE);
        Log("prezentace: ZALOHA (%s) - DXVK prezentuje primo do okna hry%s", reason,
            g_dxvkOnGame ? " (DXVK v nem uz prezentoval)" : g_vkReady ? " (okno volne)" : " (bez Vulkanu nelze overit)");
    } else {
        if (cfg.child && EnsureWndThread()) g_child = (HWND)SendMessageW(g_ctl, WMP_CHILD, (WPARAM)g_gameWnd, 0);
        g_nextProbe = g_fbTick + cfg.probeMs;
        Log("prezentace: ZALOHA (%s) - okno hry je pro Vulkan obsazene (%s)%s; sonda kazdych %lu ms", reason,
            VkStr(g_lastProbe), g_child ? ", DXVK prezentuje do dcerineho okna pres klientskou plochu hry"
                                        : ", DXVK zatim do skryteho okna (obraz nebude videt)",
            cfg.probeMs);
    }
    // Sdilene obrazy uz nejsou potreba (helper skoncil): uvolnit, kdyz kopie dobehly
    if (g_vkReady && FinishAll()) {
        DestroyImages();
        ReleaseHudlessRT();
    }
}

HWND FallbackTarget() {
    if (g_direct) {
        g_dxvkOnGame = true;
        return g_gameWnd;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_child) SyncChild(g_child);  // dcerine okno drzet pres celou klientskou plochu hry
    if (!g_probeGaveUp && now >= g_nextProbe) {
        g_nextProbe = now + cfg.probeMs;
        if (ProbeWindow(g_gameWnd)) {
            g_direct = true;
            g_dxvkOnGame = true;
            Log("prezentace: okno hry je po %.1f s zase volne - DXVK prezentuje primo do nej%s",
                (now - g_fbTick) / 1000.0, g_child ? " (dcerine okno skryto)" : "");
            if (g_child) ShowWindowAsync(g_child, SW_HIDE);
            return g_gameWnd;
        }
        if (now - g_fbTick > cfg.probeMaxSec * 1000ull) {
            g_probeGaveUp = true;
            Log("prezentace: okno hry zustava obsazene %lu s - sondu vzdavam, zustava %s", cfg.probeMaxSec,
                g_child ? "dcerine okno" : "skryte okno");
        }
    }
    return g_child;  // nullptr = skryte okno zarizeni
}

// ---- FG zap / vyp za behu ([fg] LiveSwitch) ----
// Presenter je potreba: S2 vzdy; v hlavnim helperu jen s FG (nebo [fg] ForcePresenter=1)
bool PresenterWanted() { return !cfg.inMain || cfg.fgForce || g_fgMode != BMSD_FG_OFF; }

// Cil DXVK s vypnutym presenterem: okno hry (nebo s [fg] OffTarget=1 dcerine okno proxy pres jeho klientskou plochu)
HWND OffTarget() {
    if (g_direct) {
        g_dxvkOnGame = true;
        return g_gameWnd;
    }
    if (g_child) SyncChild(g_child);
    return g_child;
}

void EnterOffTarget() {
    g_direct = !cfg.offChild;
    if (cfg.offChild) {
        if (!g_child && EnsureWndThread()) g_child = (HWND)SendMessageW(g_ctl, WMP_CHILD, (WPARAM)g_gameWnd, 0);
        if (g_child) ShowWindowAsync(g_child, SW_SHOWNA);
        else g_direct = true;
    }
}

// FG vypnuta: helper prestane prezentovat (uvolni swapchain, generator, snapshoty a obrazy), prezentacni okno pryc,
// sdilene obrazy pryc, DXVK prezentuje primo do okna hry. Helper (most DLSS) bezi dal. false = presli jsme do zalohy.
bool StopPresenter(const char* why) {
    const LONGLONG t0 = Qpc();
    StopHelper(1000);
    if (WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) {
        Fallback("helper skoncil");
        return false;
    }
    GpStop("FG vypnuta");
    g_gpActive = false;
    if (g_presWnd && g_presWnd != g_gameWnd) {
        SendMessageW(g_ctl, WMP_DESTROY, (WPARAM)g_presWnd, 0);
    }
    g_presWnd = nullptr;
    if (g_vkReady && FinishAll()) {
        DestroyImages();
        ReleaseHudlessRT();
    } else {
        // Helper svuj import uz uvolnil: pri dalsim zapnuti nutne nove obrazy (nova generace), i kdyz se velikost
        // nezmeni. Stare se zrusi v CreateImages, az kopie dobehnou.
        g_w = g_h = 0;
        Log("prezentace: WARN - kopie pro helper nedobehly, sdilene obrazy zatim zustavaji (pri zapnuti FG nove)");
    }
    g_state = PS_OFF;
    g_limNext = 0;
    EnterOffTarget();
    ++g_switches;
    Log("prezentace: presenter VYPNUT (%s) za %.1f ms - DXVK prezentuje %s (prepnuti %u)", why, Ms(t0, Qpc()),
        g_direct ? "primo do okna hry" : "do dcerineho okna proxy", g_switches);
    return true;
}

// Helper uz jednou prezentoval a na zadost proxy prestal: novy start na stejnem kanalu (restartSeq, BMSD_PRESENT_VERSION 5)
bool RestartHelper() {
    if (!g_proc || WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) return Log("prezentace: helper nebezi"), false;
    MemoryBarrier();
    const int32_t st = g_sh->helperState;
    if (st == BMSD_PST_FAILED) {
        // Prezentace helperu skoncila jen na znicenem prezentacnim okne (FG vypnuta, helper stop nestihl): helper
        // novy start prijme (restartSeq), jine chyby ne
        if (strcmp(g_sh->helperMsg, "prezentacni okno uz neexistuje") != 0)
            return Log("prezentace: helper hlasi chybu: %s", g_sh->helperMsg), false;
        Log("prezentace: helper skoncil na znicenem prezentacnim okne - zapinam znovu");
    }
    if (g_sh->swapchainAlive) Log("prezentace: WARN - helper jeste drzi swapchain (stav %d) - zapinam presto", st);
    g_sh->helperReady = 0;
    g_sh->helperState = BMSD_PST_START;
    g_sh->helperMsg[0] = 0;
    g_sh->gameHwnd = (uint64_t)(uintptr_t)g_presWnd;
    g_sh->gpEnable = g_gpActive ? (1u | ((cfg.gamePresentEarly || (g_gpDebug & 16384)) ? 2u : 0u)) : 0u;
    g_sh->gpState = BMSD_PST_START;
    g_sh->gpMsg[0] = 0;
    g_sh->fgMode = (uint32_t)g_fgMode;
    g_sh->fgMultiplier = FgMultShared();
    g_sh->fgPause = g_fgPause ? 1 : 0;
    UpdateFgTarget();
    MemoryBarrier();
    g_sh->stop = 0;
    g_sh->restartSeq = g_sh->restartSeq + 1;
    MemoryBarrier();
    SetEvent(g_evFrame);
    return true;
}

// Zapnuti presenteru (start hry s FG, nebo FG zapnuta za behu z PS_OFF). false = zaloha.
bool StartPresenter(IDirect3DDevice9* d) {
    const bool restart = g_state == PS_OFF && g_sh != nullptr;
    g_switchQpc = Qpc();
    g_startTick = GetTickCount64();
    if (!g_vkReady && !InitVk(d)) return Fallback("Vulkan DXVK nedostupny"), false;
    if (d != g_vkDev) return Fallback("nove hlavni zarizeni"), false;
    if (cfg.target == 1)
        g_presWnd = g_gameWnd;  // primo okno hry (nejmensi latence; tvrdy konec helperu ho ale "zamrazi")
    else if (EnsureWndThread())
        g_presWnd = (HWND)SendMessageW(g_ctl, WMP_CHILD, (WPARAM)g_gameWnd, 0);
    if (!g_presWnd) return Fallback("prezentacni okno helperu nevzniklo"), false;
    if (g_child) ShowWindowAsync(g_child, SW_HIDE);  // dcerine okno DXVK z PS_OFF ([fg] OffTarget=1)
    // Zobrazeni v procesu hry (VRR): vlakno presenteru proxy na prezentacnim okne; helper jen zverejnuje obrazy
    g_gpRefreshMs = g_state == PS_OFF && cfg.target != 1 ? cfg.liveRefreshMs : 0;
    g_gpRefreshAt = 0;
    g_gpRefreshStage = 0;
    g_gpRefreshMode = cfg.liveRefreshMode;
    // bit 4: okno hry z iFlip DWM vyhodit hned (pred prvnim swapchainem presenteru) - jedno bliknuti pri prepnuti
    if (g_state == PS_OFF && cfg.target != 1 && (cfg.liveRefreshMode & 4) && g_ctl && IsWindow(g_gameWnd))
        PostMessageW(g_ctl, WMP_UNSTICK, (WPARAM)g_gameWnd, 0);  // ne Send: vlakno oken saha na okno hry -> deadlock
    g_gpActive = cfg.inMain && GpStart(g_presWnd);
    if (restart) {
        if (!RestartHelper()) return Fallback("hlavni helper nejde znovu zapnout"), false;
    } else {
        if (!StartHelper(d)) return Fallback(cfg.inMain ? "hlavni helper nejde spustit / pripojit" : "helper nejde spustit"), false;
    }
    const bool live = g_state == PS_OFF;
    g_state = PS_STARTING;
    g_ackTick = g_startTick;
    g_lastAck = g_sh->helperAck;
    if (live) {
        ++g_switches;
        Log("prezentace: presenter ZAPNUT za behu (FG %s, %s, prepnuti %u) - DXVK do skryteho okna, helper prezentuje do "
            "okna %p", FgModeName(g_fgMode), restart ? "znovu" : "poprve", g_switches, (void*)g_presWnd);
    }
    return true;
}

static bool PresentEarlyFlush() {
    static int s_v = -1;
    if (s_v < 0) {
        const std::wstring ini = std::wstring(ProxyDir()) + L"\\bms_dlss.ini";
        s_v = GetPrivateProfileIntW(L"fg", L"EarlyFlush", 1, ini.c_str()) != 0 ? 1 : 0;
    }
    return s_v != 0;
}

// Snimek pro helper: kopie backbufferu do sdileneho obrazu, signal. false = snimek nepredan.
bool PostFrame(IDirect3DDevice9* d, IDirect3DSwapChain9* sc) {
    IDirect3DSurface9* bb = nullptr;
    HRESULT hr = sc ? sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)
                    : d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) return false;
    D3DSURFACE_DESC desc = {};
    bb->GetDesc(&desc);
    // Handle VkImage obrazu DXVK se cte az po FlushRenderingCommands: CS vlakno DXVK ma pak prikazy snimku zpracovane,
    // takze pripadna vymena uloziste obrazu (premisteni pameti DXVK) uz probehla a nase kopie nepracuji se starym
    // VkImage (podezreni na vzacny MMU fault na fronte DXVK pri startu FG). Pozdejsi flush pred odeslanim zustava.
    // [fg] EarlyFlush=0 = puvodni poradi (jen pro srovnani).
    if (PresentEarlyFlush()) g_idev->FlushRenderingCommands();
    VkImage bbImg = VK_NULL_HANDLE;
    VkImageLayout bbL = VK_IMAGE_LAYOUT_GENERAL;
    VkImageCreateInfo bbInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    {
        ID3D9VkInteropTexture* tex = nullptr;
        if (SUCCEEDED(bb->QueryInterface(__uuidof(ID3D9VkInteropTexture), (void**)&tex)) && tex) {
            if (FAILED(tex->GetVulkanImageInfo(&bbImg, &bbL, &bbInfo))) bbImg = VK_NULL_HANDLE;
            tex->Release();
        }
    }
    if (!bbImg || bbInfo.samples != VK_SAMPLE_COUNT_1_BIT || desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        bb->Release();
        Fallback(!bbImg ? "backbuffer bez Vulkan obrazu (interop)" : "backbuffer s MSAA (spike neumi resolve)");
        return false;
    }
    if (desc.Width != g_w || desc.Height != g_h || bbInfo.format != g_fmt) {
        Log("prezentace: backbuffer %ux%u, D3DFMT %d, VkFormat %d, layout %d", desc.Width, desc.Height,
            (int)desc.Format, (int)bbInfo.format, (int)bbL);
        if (!CreateImages(desc.Width, desc.Height, bbInfo.format)) {
            bb->Release();
            Fallback("sdilene obrazy nejdou vytvorit");
            return false;
        }
    }
    // HUD-less (K5): RT s barvou sceny pred HUD z tohoto snimku (Present_HudlessFrame); jinak snimek bez HUD-less
    VkImage hlImg = VK_NULL_HANDLE;
    VkImageLayout hlL = VK_IMAGE_LAYOUT_GENERAL;
    if (g_hlFrame && g_hlImages && g_hlRT) {
        VkImageCreateInfo hi = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ID3D9VkInteropTexture* tex = nullptr;
        if (SUCCEEDED(g_hlRT->QueryInterface(__uuidof(ID3D9VkInteropTexture), (void**)&tex)) && tex) {
            if (FAILED(tex->GetVulkanImageInfo(&hlImg, &hlL, &hi))) hlImg = VK_NULL_HANDLE;
            tex->Release();
        }
        if (hlImg && (hi.extent.width != g_w || hi.extent.height != g_h || hi.format != g_fmt ||
                      hi.samples != VK_SAMPLE_COUNT_1_BIT)) {
            static bool s_logged;
            if (!s_logged) {
                s_logged = true;
                Log("prezentace: HUD-less RT %ux%u VkFormat %d nesedi s backbufferem %ux%u VkFormat %d - snimek bez "
                    "HUD-less", hi.extent.width, hi.extent.height, (int)hi.format, g_w, g_h, (int)g_fmt);
            }
            hlImg = VK_NULL_HANDLE;
        }
    }
    g_hlFrame = false;
    const LONGLONG q0 = Qpc();
    const uint64_t N = g_N + 1;
    const int k = (int)(N % K);
    PImg& p = g_img[k];
    PImg& ph = g_hl[k];
    // Obraz znovu jen kdyz ho helper uz prevzal (jinak by mohl cist prepsany obsah)
    MemoryBarrier();
    if (p.posted && g_sh->helperAck < p.posted) {
        const ULONGLONG t0 = GetTickCount64();
        while (g_sh->helperAck < p.posted && GetTickCount64() - t0 < 50) {
            WaitForSingleObject(g_evDone, 5);
            MemoryBarrier();
        }
        g_stReuse.add(Ms(q0, Qpc()));
        if (g_sh->helperAck < p.posted) {  // snimek vynechat (hra nesmi stat); dlouhe zdrzeni resi CheckHelper
            ++g_stDropped;
            bb->Release();
            return false;
        }
    }
    const LONGLONG qs = Qpc();
    if (!FinishSlot(k, kFenceNs)) {
        bb->Release();
        Fallback("GPU nedokoncila kopii pro helper v limitu");
        return false;
    }
    const LONGLONG qrec = Qpc();
    g_stSlot.add(Ms(qs, qrec));
    const uint64_t waitDone = p.posted;  // helper snimek p.posted prevzal: jeho cteni (semDone) na GPU dobehne
    if (waitDone) {
        LONG64 cur = InterlockedCompareExchange64(&g_maxDoneWait, 0, 0);
        if ((LONG64)waitDone > cur) InterlockedExchange64(&g_maxDoneWait, (LONG64)waitDone);
    }
    const VkImageLayout srcL = bbL == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL
                                                               : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL;
    const uint32_t EXT = VK_QUEUE_FAMILY_EXTERNAL;
    VkCommandBuffer cmd = g_cmd[k];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkResetCommandBuffer(cmd, 0);
    vk.vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier pre[2] = {
        Barrier(bbImg, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, bbL, srcL),
        p.fresh ? Barrier(p.img, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN)
                : Barrier(p.img, 0, VK_ACCESS_TRANSFER_WRITE_BIT, GEN, GEN, EXT, g_qf)};
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                            nullptr, 2, pre);
    VkImageCopy rc = {};
    rc.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rc.extent = {g_w, g_h, 1};
    vk.vkCmdCopyImage(cmd, bbImg, srcL, p.img, GEN, 1, &rc);
    VkImageMemoryBarrier post[2] = {
        Barrier(bbImg, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, srcL, bbL),
        Barrier(p.img, VK_ACCESS_TRANSFER_WRITE_BIT, 0, GEN, GEN, g_qf, EXT)};
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                            nullptr, 2, post);
    if (hlImg) {  // HUD-less: kopie RT -> hudless[k] (vlastnictvi obrazu jako u img[k]: proxy <-> EXTERNAL)
        const VkImageLayout hsrcL = hlL == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL
                                                                   : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkImageMemoryBarrier hpre[2] = {
            Barrier(hlImg, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, hlL, hsrcL),
            ph.fresh ? Barrier(ph.img, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN)
                     : Barrier(ph.img, 0, VK_ACCESS_TRANSFER_WRITE_BIT, GEN, GEN, EXT, g_qf)};
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                0, nullptr, 2, hpre);
        vk.vkCmdCopyImage(cmd, hlImg, hsrcL, ph.img, GEN, 1, &rc);
        VkImageMemoryBarrier hpost[2] = {
            Barrier(hlImg, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, hsrcL,
                    hlL),
            Barrier(ph.img, VK_ACCESS_TRANSFER_WRITE_BIT, 0, GEN, GEN, g_qf, EXT)};
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                                0, nullptr, 2, hpost);
    }
    vk.vkEndCommandBuffer(cmd);
    g_ref[k] = bb;  // reference z GetBackBuffer: pustit az po fence slotu
    g_idev->FlushRenderingCommands();  // prace hry na snimku je odeslana pred nasi kopii

    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = waitDone ? 1 : 0;
    ti.pWaitSemaphoreValues = &waitDone;
    ti.signalSemaphoreValueCount = 1;
    ti.pSignalSemaphoreValues = &N;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ti};
    si.waitSemaphoreCount = waitDone ? 1 : 0;
    si.pWaitSemaphores = &g_semDone;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_semReady;
    g_idev->LockSubmissionQueue();
    const VkResult r = vk.vkQueueSubmit(g_queue, 1, &si, g_fence[k]);
    g_idev->ReleaseSubmissionQueue();
    if (r != VK_SUCCESS) {
        g_ref[k] = nullptr;
        bb->Release();
        {  // diagnostika: stav semaforu a slotu v okamziku chyby
            uint64_t rd = 0, dn = 0;
            const VkResult r1 = vk.vkGetSemaphoreCounterValue(g_dev, g_semReady, &rd);
            const VkResult r2 = vk.vkGetSemaphoreCounterValue(g_dev, g_semDone, &dn);
            Log("prezentace: vkQueueSubmit %s (snimek N %llu, obraz %d, ceka na semDone %llu; semReady %llu (%s), "
                "semDone %llu (%s), helperAck %llu, B mostu %llu)",
                VkStr(r), (unsigned long long)N, k, (unsigned long long)waitDone, (unsigned long long)rd, VkStr(r1),
                (unsigned long long)dn, VkStr(r2), (unsigned long long)g_sh->helperAck,
                (unsigned long long)g_curBridgeN);
        }
        Fallback("odeslani kopie pro helper selhalo");
        return false;
    }
    g_fenceUsed[k] = true;
    p.fresh = false;
    p.posted = N;
    if (hlImg) {
        ph.fresh = false;
        ++g_stHl;
    } else if (g_hlImages && g_sh->fgState == BMSD_FGS_ACTIVE && !g_fgPause &&
               !(cfg.adaptIdleSkip && g_fgMode == BMSD_FG_FSRADAPT && !g_sh->adaptEngaged)) {
        ++g_stHlMiss;  // FG generuje, ale snimek je bez HUD-less (bod pred HUD nenalezen / RT nejde)
    }
    g_N = N;
    if (cfg.inMain) {
        // FG: bridgeFrame (snimek mostu s MV, hloubkou a kamerou k tomuto snimku; 0 = zadny) musi helper precist
        // spolu se spravnym N. Zverejneni je seqlock nad postQpc: 0 = zapis probiha -> bridgeFrame -> frameValue ->
        // postQpc = cas (vzdy != 0, roste). Ctenar: q1 = postQpc (0 = znovu), N, bridgeFrame, q2 = postQpc, q1 == q2.
        // postQpc se zapisuje atomicky (32bit proces: jinak dva zapisy po 32 bitech).
        InterlockedExchange64((volatile LONG64*)&g_sh->postQpc, 0);
        g_sh->bridgeFrame = g_curBridgeN;
        g_sh->hudlessFrame = hlImg ? N : 0;
        g_sh->frameValue = N;
        InterlockedExchange64((volatile LONG64*)&g_sh->postQpc, Qpc());
    } else {
        g_sh->frameValue = N;
        g_sh->postQpc = (uint64_t)Qpc();
        MemoryBarrier();
    }
    SetEvent(g_evFrame);
    g_stCopy.add(Ms(qrec, Qpc()));
    ++g_stPosted;
    return true;
}

// Stav helperu pred snimkem; false = presli jsme do zalohy.
bool CheckHelper() {
    if (WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) {
        Fallback(g_sh->swapchainAlive ? "helper skoncil bez uvolneni swapchainu (zabit / pad)"
                                      : "helper skoncil");
        return false;
    }
    MemoryBarrier();
    const int32_t st = g_sh->helperState;
    const ULONGLONG now = GetTickCount64();
    if (st == BMSD_PST_FAILED) {
        char why[300];
        snprintf(why, sizeof why, "helper hlasi chybu: %s", g_sh->helperMsg);
        static char keep[300];
        memcpy(keep, why, sizeof keep);
        Fallback(keep);
        return false;
    }
    if (g_gpActive && g_gpFailed) {
        static char keepGp[220];
        snprintf(keepGp, sizeof keepGp, "zobrazeni v procesu hry: %s", g_gpWhy);
        Fallback(keepGp);
        return false;
    }
    if (g_state == PS_STARTING) {
        // (hlavni helper s minimalizovanym oknem hry: pripojeny a potvrzuje snimky = nastartoval, jen nema kam kreslit)
        // Zobrazeni v procesu hry: navic swapchain proxy (gpState), jinak by se cekalo na obsazene okno do zalohy
        const int32_t gst = g_gpActive ? g_sh->gpState : BMSD_PST_PRESENTING;
        const bool gpOk = gst == BMSD_PST_PRESENTING || gst == BMSD_PST_NOWINDOW;
        if ((st == BMSD_PST_PRESENTING || (cfg.inMain && st == BMSD_PST_NOWINDOW)) && gpOk) {
            g_state = PS_HELPER;
            Log("prezentace: helper prezentuje do okna hry (start %.2f s, zobrazuje %s)", (now - g_startTick) / 1000.0,
                g_gpActive ? "proces hry" : "helper");
        } else if (now - g_startTick > cfg.startTimeoutMs) {
            Fallback(st == BMSD_PST_INUSE || gst == BMSD_PST_INUSE ? "okno hry je obsazene (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)"
                                                                     : "helper nezacal prezentovat vcas");
            return false;
        }
    }
    // Nereaguje: nejaky snimek ceka na prevzeti a helper uz hangMs nic nepotvrdil (prevezme kazdy snimek do ms,
    // i bez okna). Hodiny bezi jen, kdyz neco ceka.
    if (cfg.testStopSec && g_state == PS_HELPER && now - g_startTick > cfg.testStopSec * 1000ull) {
        Fallback("test: [present] TestStopSec - helper pozadan o cisty konec");
        return false;
    }
    const uint64_t ack = g_sh->helperAck;
    if (ack != g_lastAck || ack >= g_N) {
        g_lastAck = ack;
        g_ackTick = now;
    } else if (now - g_ackTick > cfg.hangMs) {
        Fallback("helper nepotvrzuje snimky (nereaguje)");
        return false;
    }
    return true;
}

void LogStats(IDirect3DDevice9* d, void* caller, void* ourHook) {
    // Kdo vola Present (Steam overlay haci vtable / kod DXVK?) - pri zmene hned do logu
    if (caller != g_lastCaller) {
        g_lastCaller = caller;
        HMODULE hm = nullptr;
        char mod[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)caller, &hm) && hm)
            GetModuleFileNameA(hm, mod, MAX_PATH);
        const char* base = strrchr(mod, '\\');
        Log("prezentace: Present hlavniho zarizeni vola %s+0x%X", base ? base + 1 : mod,
            (unsigned)((uintptr_t)caller - (uintptr_t)hm));
    }
    void* vt17 = (*(void***)d)[17];
    if (vt17 != ourHook && !g_vtLogged) {
        g_vtLogged = true;
        HMODULE hm = nullptr;
        char mod[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)vt17, &hm) && hm)
            GetModuleFileNameA(hm, mod, MAX_PATH);
        Log("prezentace: vtable[17] (Present) hlavniho zarizeni prepsal nekdo jiny: %s", mod);
    }
    ++g_stPresents;
    const LONGLONG now = Qpc();
    if (!g_statQpc) g_statQpc = now;
    const double el = Ms(g_statQpc, now);
    if (el < 5000.0) return;
    char win[256];
    DescribeWindow(g_gameWnd, win, sizeof win);
    bool statFresh = false;  // helper od minuleho radku zapsal novou statistiku (plati i pro pole statFg*)
    if (g_state == PS_OFF || g_state == PS_IDLE) {  // FG vypnuta: jen kratky radek (jako bez presenteru)
        Log("prezentace %.1f s: FG vypnuta, presenter nebezi -> %s; snimku %u (%.1f FPS)", el / 1000.0,
            g_direct ? "DXVK primo do okna hry" : g_child ? "DXVK do dcerineho okna" : "DXVK do skryteho okna",
            g_stPresents, g_stPresents * 1000.0 / el);
        g_stPresents = g_stPosted = g_stDropped = g_stNotReady = 0;
        g_stCopy = g_stReuse = g_stSlot = g_stOPresent = Acc();
        g_stHl = g_stHlMiss = g_stHlSnaps = 0;
        g_stLimSleep = g_stLimErr = Acc();
        g_stLimLate = g_stLimResync = 0;
        g_statQpc = now;
        return;
    }
    if (g_state == PS_FALLBACK) {
        Log("prezentace %.1f s: ZALOHA (%s) -> %s; snimku %u | %s", el / 1000.0, g_fbReason,
            g_direct ? "DXVK primo do okna hry" : g_child ? "DXVK do dcerineho okna" : "DXVK do skryteho okna",
            g_stPresents, win);
    } else {
        char hs[400] = "helper: zatim bez statistiky";
        if (g_sh && g_sh->statSeq != g_helperStatSeq) {
            g_helperStatSeq = g_sh->statSeq;
            statFresh = true;
            snprintf(hs, sizeof hs,
                     "helper: prevzato %u, zobrazeno %u, swapchain %ux%u mod %u (znovu %u), probuzeni %.3f / %.2f ms, "
                     "post -> blit hotov %.3f / %.2f ms, post -> zobrazeno %.3f / %.2f ms, vkQueuePresent %.3f / %.2f ms, "
                     "interval %.2f / %.2f ms",
                     g_sh->statFrames, g_sh->statPresented, g_sh->swapW, g_sh->swapH, g_sh->presentMode,
                     g_sh->statRecreate, g_sh->statWakeMs, g_sh->statWakeMax, g_sh->statDoneMs, g_sh->statDoneMax,
                     g_sh->statDispMs, g_sh->statDispMax, g_sh->statPresentMs, g_sh->statPresentMax, g_sh->statIntervalMs,
                     g_sh->statIntervalMax);
        }
        Log("prezentace %.1f s (%s, stav helperu %d): snimku %u, predano %u, vynechano (helper pozadu) %u, helper "
            "nepripraven %u, CPU hry: kopie+flush+odeslani %.3f / %.2f ms, fence slotu %.3f / %.2f ms, cekani na "
            "prevzeti obrazu %.3f / %.2f ms (%u x), o_Present %.3f / %.2f ms | %s | %s",
            el / 1000.0, g_state == PS_HELPER ? "helper" : "start", g_sh ? g_sh->helperState : 0, g_stPresents,
            g_stPosted, g_stDropped, g_stNotReady, g_stCopy.avg(), g_stCopy.max, g_stSlot.avg(), g_stSlot.max,
            g_stReuse.avg(), g_stReuse.max, g_stReuse.n, g_stOPresent.avg(), g_stOPresent.max, hs, win);
    }
    if (cfg.inMain && g_sh) {
        FgLatchStats();
        UpdateFgTarget();  // auto: okno hry mohlo prejit na jiny monitor / jinou frekvenci
        const bool fb = g_state == PS_FALLBACK;
        MemoryBarrier();
        const int st = fb ? BMSD_FGS_OFF : g_sh->fgState;
        char hf[320] = "helper: zatim bez statistiky FG";
        if (!fb && statFresh)
            snprintf(hf, sizeof hf,
                     "helper: nasobek %ux, skutecnych %u, generovanych %u, sparovano %u / nesparovano %u, eval %.2f / "
                     "%.2f ms, sd intervalu presentu %.3f ms, %.1f -> %.1f FPS, s HUD-less %u",
                     g_sh->fgActiveMult, g_sh->statFgReal, g_sh->statFgGen, g_sh->statFgPaired, g_sh->statFgUnpaired, g_sh->statFgEvalMs,
                     g_sh->statFgEvalMax, g_sh->statFgIntSd, g_sh->statFgRealFps, g_sh->statFgOutFps,
                     g_sh->statFgHudless);
        char msg[140];
        memcpy(msg, g_sh->fgMsg, sizeof g_sh->fgMsg);
        msg[sizeof g_sh->fgMsg - 1] = 0;
        Log("prezentace FG %.1f s: pozadovano %s, stav %s%s%s%s (bezi %s), strop %u FPS; hra %.1f FPS | limiter: spal "
            "%u x z %u (pozde %u, novy slot %u), spanek %.2f / %.2f ms, |odchylka slotu| %.3f / %.3f ms | HUD-less: "
            "predano %u, FG bez HUD-less %u, kopii RT %u%s | %s",
            el / 1000.0, FgModeName(g_fgMode), fb ? "ZALOHA - " : "", FgStateName(st), msg[0] && !fb ? ": " : "",
            fb ? "" : msg, FgModeName(fb ? 0 : (int)g_sh->fgActiveMode), g_fgTarget, g_stPresents * 1000.0 / el,
            g_stLimSleep.n, g_stPresents, g_stLimLate, g_stLimResync, g_stLimSleep.avg(), g_stLimSleep.max,
            g_stLimErr.avg(), g_stLimErr.max, g_stHl, g_stHlMiss, g_stHlSnaps,
            !cfg.hudless ? " (vypnuto, [fg] HudLess=0)" : g_hlImages ? "" : " (obrazy nejsou)", fb ? "-" : hf);
        g_stHl = g_stHlMiss = g_stHlSnaps = 0;
        g_stLimSleep = g_stLimErr = Acc();
        g_stLimLate = g_stLimResync = 0;
    }
    g_stPresents = g_stPosted = g_stDropped = g_stNotReady = 0;
    g_stCopy = g_stReuse = g_stSlot = g_stOPresent = Acc();
    g_statQpc = now;
}

// Limiter skutecnych snimku pri generovani (plan 1.2): po o_Present spat do konce slotu N / TargetFps (N = nasobek,
// se kterym helper generuje). Faze slotu se drzi (next += perioda), takze prumer je presne TargetFps / N; snimek, ktery
// slot nestihl, se nezdrzuje, a pri zpozdeni o vic nez periodu zacina slot znovu. Jen kdyz helper generuje
// (BMSD_FGS_ACTIVE).
// Automaticky nasobek DLSS-G ([fg] Multiplier=auto, v25j; NVIDIA Dynamic MFG jde jen pres Streamline v DX12, tohle je
// vlastni obdoba). Vola ho regulator limiteru rezimu 3 (LimiterAdapt=3) kazdy skutecny snimek:
//   - nahoru: regulator musel strop skutecnych snimku srazit pod AutoUpBelow (75 %; v25q 85 %, do v25p 93 %; uzivatel: skutecne snimky maji prednost) AutoUpHold (3 s) v kuse -
//     mensi ztratu vystupu snese (uzivatel: kvuli 5 FPS nezvysovat, vyssi nasobek srazi skutecne FPS a prida odezvu),
//   - dolu: strop je plny a GPU je hotove do 85 % intervalu i po prepoctu na nizsi nasobek n (interval se zkrati
//     na n/cur; cena generovanych snimku ubyde, to je rezerva navic) - po dobu AutoDownHold (1 s, po kmitnuti az 30 s);
//     skok rovnou na nejnizsi takove n (v25o; driv po 1 a prah 75 % -> 6x -> 3x trvalo 15 s a 2x se nedosahlo,
//     i kdyz rucne 2x stacilo; regulator snimky ubira az nad 100 %, pridava pod 85 %).
// Po kazde zmene 1,5 s klid (regulator se pri zmene nasobku resetuje a meri znovu). DLSS-G a FSR (v25n).
static void FgAutoMult(double ratio, int am, LONGLONG now, LONGLONG q1s, double fps, double rate) {
    static LONGLONG s_since, s_lastChange;
    static int s_dir, s_lastDir;
    static double s_downHold = 0.0;
    if (s_downHold <= 0.0) s_downHold = cfg.fgAutoDownHold;
    const double kDown = 0.85;  // odhad GPU hotovo / interval pri nizsim nasobku (jako regulator: pridava pod 85 %)
    if (!g_fgMultAuto || (g_fgMode != BMSD_FG_DLSSG && g_fgMode != BMSD_FG_FSR2X)) return;
    const int mx = std::min(cfg.fgAutoMax, Present_FgMultMax((int)g_fgMode));
    const int mn = std::min(cfg.fgAutoMin, mx);
    const int cur = (int)g_fgMult;
    static bool s_only2Logged;
    if (mx <= 2 && g_fgMode == BMSD_FG_DLSSG && Present_FgMultKnown() && !s_only2Logged) {  // RTX 40 (v25m): auto = 2x
        s_only2Logged = true;
        Log("prezentace: FG auto nasobek - DLSS-G na tomto GPU umi jen 2x, auto drzi 2x");
    }
    if (cur > mx || cur < mn) {  // mez se zmenila (GPU ohlasilo strop)
        Present_FgSetMultEff(std::max(mn, std::min(mx, cur)));
        s_lastChange = now;
        return;
    }
    if (am != cur) {  // helper jeste neprepnul
        s_dir = 0;
        return;
    }
    if (s_lastChange && now - s_lastChange < q1s * 3 / 2) return;
    if (s_lastChange && s_lastDir < 0 && now - s_lastChange > 60 * q1s) s_downHold = cfg.fgAutoDownHold;  // dlouho klid: zpet
    // nejnizsi nasobek, na kterem by GPU podle mereni na aktualnim stihalo (0 = zadny nizsi)
    int down = 0;
    if (ratio > 0.0 && g_limScale > 0.995)
        for (int n = mn; n < cur && !down; ++n)
            if (ratio * (double)cur / (double)n < kDown) down = n;
    int want = 0;
    if (g_limScale < (double)cfg.fgAutoUpBelow / 100.0 && cur < mx) want = 1;
    else if (down) want = -1;
    if (want != s_dir) {
        s_dir = want;
        s_since = now;
        return;
    }
    // dolu rychle (nizsi nasobek = vic skutecnych snimku, mensi odezva), nahoru pomalu (v25r, uzivatel)
    if (!want || now - s_since < (LONGLONG)((want > 0 ? cfg.fgAutoUpHold : s_downHold) * (double)q1s)) return;
    // kmitani: nahoru brzy po snizeni -> priste dolu az po delsim drzeni
    if (want > 0 && s_lastDir < 0 && now - s_lastChange < 10 * q1s) s_downHold = std::min(30.0, s_downHold * 2.0);
    const int nm = want > 0 ? cur + 1 : down;
    Log("prezentace: FG auto nasobek %dx -> %dx (strop skutecnych %.0f %% z %.1f FPS, hra %.1f FPS, GPU hotovo %.0f %% "
        "intervalu; drzeni dolu %.0f s)",
        cur, nm, g_limScale * 100.0, rate, fps, ratio > 0.0 ? ratio * 100.0 : -1.0, s_downHold);
    Present_FgSetMultEff(nm);
    s_lastChange = now;
    s_lastDir = want;
    s_dir = 0;
}

void FgLimiter() {
    MemoryBarrier();
    // Adaptivni FSR: hra vzdy nejvys na cil vystupu (nad nim helper nic nedoplnuje a nesmi prestrelit strop VRR), se
    // [fg] RealCap na strop skutecnych FPS - i kdyz helper prave negeneruje (stav ACTIVE i PAUSED).
    const bool adapt = g_fgMode == BMSD_FG_FSRADAPT;
    // Kratke pozastaveni pevneho nasobku (strih kamery: helper 1 snimek PAUSED) limiter nevypina: jinak novy slot bez
    // spanku = snimek navic, fronta helperu se plni a generovane snimky se dohaneji (v24t). Nejvys 300 ms.
    static LONGLONG s_lastActive;
    const bool want = cfg.inMain && g_state == PS_HELPER && g_sh && g_fgTargetD > 0.0 && !g_fgPause &&
                      g_fgMode != BMSD_FG_OFF;
    const LONGLONG nowA = Qpc();
    const bool brief = !adapt && want && g_sh->fgState == BMSD_FGS_PAUSED && g_limNext && s_lastActive &&
                       nowA - s_lastActive < (LONGLONG)(0.3 * (double)g_qpf.QuadPart);
    const bool active = want && (g_sh->fgState == BMSD_FGS_ACTIVE || (adapt && g_sh->fgState == BMSD_FGS_PAUSED) || brief);
    if (want && g_sh->fgState == BMSD_FGS_ACTIVE) s_lastActive = nowA;
    g_limBits <<= 1;
    if (!active) {
        g_limNext = 0;
        return;
    }
    double rate;  // skutecnych snimku za sekundu (pred rezervou)
    // (FgAutoMult je volan z regulatoru rezimu 3 nize)
    if (adapt) {
        rate = g_fgTargetD;
        if (cfg.adaptCap > 0 && cfg.adaptCap < rate) rate = (double)cfg.adaptCap;
    } else {
        const uint32_t am = g_sh->fgActiveMult;
        rate = g_fgTargetD / (am >= 2 && am <= 6 ? (double)am : 2.0);
    }
    // Rezerva GPU ([fg] LimiterAdapt): hra, ktera strop N / TargetFps nestiha (GPU na hranici, napr. 4K a 3x), chodi
    // nepravidelne, vlakno presentu helperu je pozadu a generovane snimky zahazuje (trhani, "FG se vypina"). Kdyz za
    // ~1 s nestihne slot vic nez 25 % snimku, strop klesne na 93 % skutecne dosazenych FPS; bez nestihu (< 5 %) se
    // kazde ~2 s vraci o 1 % zpet. Jen pevne nasobky (adaptivni FSR si mezery doplnuje sam).
    const LONGLONG now = Qpc();
    // Ne pro adaptivni FSR (v24q, overeno 4K): hra zastropovana na ~37 FPS, post -> hotovo dal 64-74 ms - GPU vytizi
    // samo generovani (~4 snimky na skutecny, kazdy zvlast), rezerva jen ubere skutecne FPS a skubani zustane.
    if (cfg.fgLimAdapt == 3 && (!adapt || cfg.fgLimAdaptFsr)) {
        // Rezim 3 (v24u): regulator podle casu GPU (obdoba Reflexu). Helper meri skutecny cas od Present hry do
        // dokonceni sve prace pro snimek na GPU (BmsdShared::fgGpuDone). Delsi nez interval skutecnych snimku = GPU
        // nestiha a fronta pred ni roste: strop hned dolu (median 5 mereni, ne az po 0,5 s jako rezim 2). Trvale pod
        // 85 % intervalu: strop pomalu nahoru (0,5 % za 1 s, po doznani drzeni). Pojistka jako rezim 2: hra slot
        // nestiha (> 25 % snimku za 0,25 s).
        static LONGLONG s_winQ, s_probeAt, s_lastHit, s_lastCut, s_prevQ, s_startQ;
        static unsigned s_n, s_late, s_logs;
        static uint32_t s_am, s_lastPub;
        static double s_d[5], s_dtEma, s_cutMed, s_bound, s_holdS = 4.0;
        static int s_dn, s_dp;
        const uint32_t amKey = adapt ? 0xADu : g_sh->fgActiveMult;  // adaptivni: nasobek se meni sam, stav nenulovat
        const LONGLONG q1s = g_qpf.QuadPart;
        if (!s_winQ || s_am != amKey) {
            s_am = amKey;
            s_winQ = now;
            s_n = s_late = 0;
            s_probeAt = s_lastHit = s_lastCut = s_prevQ = 0;
            s_dn = s_dp = 0;
            s_dtEma = s_bound = 0.0;
            s_holdS = 4.0;
            g_limScale = g_limScaleLogged = 1.0;
        }
        // Alt+Tab (v25v, uzivatel: po navratu nizke skutecne FPS, rostou po 1): na pozadi hru brzdi Windows / ovladac,
        // regulator to bral jako nestihajici GPU a strop srazil; zpet jen +0,5 %/s. Na pozadi proto strop nesrazit a po
        // navratu do popredi nebo po pauze FG (limiter nebezel > 0,3 s) zacit znovu od plneho stropu.
        static LONGLONG s_lastRunQ, s_fgChkQ, s_focusQ;
        static bool s_focus = true;
        if (!s_fgChkQ || now - s_fgChkQ > q1s / 4 || (s_lastRunQ && now - s_lastRunQ > q1s * 3 / 10)) {
            s_fgChkQ = now;
            DWORD pid = 0;
            const HWND fw = GetForegroundWindow();
            if (fw) GetWindowThreadProcessId(fw, &pid);
            const bool focus = !fw || pid == GetCurrentProcessId();
            // navrat: okno zpet v popredi, nebo regulator dlouho nebezel (hra po Alt+Tab nekreslila, v25y)
            const bool gap = s_lastRunQ && now - s_lastRunQ > q1s * 3 / 10;
            if (focus && (!s_focus || gap)) {
                // v25x: prvni snimky po navratu jeste chodi pozde (hra byla brzdena) -> 1,5 s nic nesrazit a merit znovu
                Log("prezentace: regulator FG - %s, strop skutecnych %.0f %% -> 100 %%, 1,5 s bez srazeni",
                    s_focus ? "pauza snimku hry (Alt+Tab?)" : "hra zpet v popredi", g_limScale * 100.0);
                g_limScale = g_limScaleLogged = 1.0;
                s_dn = 0;
                s_startQ = now;
                s_focusQ = now;
                s_winQ = now;
                s_n = s_late = 0;
            }
            s_focus = focus;
        }
        if (!g_limNext) {  // limiter prave zacina (FG zapnuta, po nacitani): fronta z doby bez stropu se teprve vyprazdni
            s_startQ = now;
            s_dn = 0;
            if (s_lastRunQ && now - s_lastRunQ > q1s * 3 / 10 && g_limScale < 1.0) {
                Log("prezentace: regulator FG - po pauze (%.1f s) strop skutecnych %.0f %% -> 100 %%",
                    (double)(now - s_lastRunQ) / (double)q1s, g_limScale * 100.0);
                g_limScale = g_limScaleLogged = 1.0;
            }
        }
        s_lastRunQ = now;
        if (s_prevQ) {
            const double dt = Ms(s_prevQ, now);
            if (dt > 0.0 && dt < 200.0) s_dtEma = s_dtEma > 0.0 ? s_dtEma + 0.1 * (dt - s_dtEma) : dt;
        }
        s_prevQ = now;
        ++s_n;
        if (g_limNext && now >= g_limNext + (LONGLONG)((double)q1s / (rate * g_limScale))) ++s_late;
        const uint32_t gd = Interop_FgGpuDone();
        if ((gd >> 16) && (gd >> 16) != s_lastPub) {
            s_lastPub = gd >> 16;
            s_d[s_dp] = (double)(gd & 0xFFFFu) / 100.0;
            s_dp = (s_dp + 1) % 5;
            if (s_dn < 5) ++s_dn;
        }
        const double ivMs = 1000.0 / (rate * g_limScale);
        double med = 0.0;
        if (s_dn >= 5) {
            double t[5];
            memcpy(t, s_d, sizeof t);
            std::sort(t, t + 5);
            med = t[2];
        }
        const double ratio = med / ivMs;
        const double fps = s_dtEma > 0.0 ? 1000.0 / s_dtEma : 0.0;
        const double winMs = Ms(s_winQ, now);
        const double lateF = s_n ? (double)s_late / (double)s_n : 0.0;
        const bool cool = !s_lastCut || now - s_lastCut > q1s / 4;
        const char* why = nullptr;
        // GPU (cas hotovo) jen do 40 % stropu (2x ve 4K drzi ~70 %): kdyby cas hotovo byl delsi nez interval i bez fronty (prace helperu
        // soubezne se hrou), nesrazi strop libovolne nizko; dal jen pojistka nestihu.
        // Do 1 s po snizeni se fronta teprve vyprazdnuje: znovu dolu jen kdyz cas hotovo neklesa.
        const bool draining = s_lastCut && now - s_lastCut < q1s && med < s_cutMed;
        const bool settling = s_startQ && now - s_startQ < q1s;  // 1 s po zacatku jen pojistka nestihu
        // adaptivni FSR: rate = cil vystupu (~157), skutecne FPS ve 4K ~45 -> spodni mez 20 FPS misto 40 % rate
        const double minScale = adapt ? std::min(0.4, 20.0 / rate) : 0.4;
        const bool focusSettle = s_focusQ && now - s_focusQ < q1s * 3 / 2;
        const bool gpuHit = s_focus && !focusSettle && s_dn >= 5 && ratio > 1.0 && g_limScale > minScale && !draining && !settling;
        const bool lateHit = s_focus && !focusSettle && winMs >= 250.0 && lateF > 0.25 && fps > 10.0;
        if (cool && (gpuHit || lateHit)) {
            // Mirne nad intervalem (do 130 %): jen -3 % (cas hotovo kolisa se scenou). Vic, nebo hra uz slot nestiha:
            // podle pomeru / dosazenych FPS (nejvys -15 % naraz).
            double ns = g_limScale * (gpuHit && ratio < 1.3 ? 0.97 : std::max(0.85, std::min(0.97, 0.95 / ratio)));
            if ((lateHit || ratio >= 1.3) && fps > 10.0) ns = std::min(ns, 0.99 * fps / rate);
            ns = std::max(adapt ? minScale : 0.1, ns);
            if (ns < g_limScale - 1e-6) {
                // naraz na tutez hranici (do 30 s, +-2 %): zkouset nad ni mene casto
                if (s_lastHit && now - s_lastHit < 30 * q1s && fabs(g_limScale - s_bound) < 0.02)
                    s_holdS = std::min(30.0, s_holdS * 2.0);
                s_bound = g_limScale;
                s_lastHit = now;
                g_limScale = ns;
                why = gpuHit ? "GPU nestiha" : "hra nestiha slot";
            }
            s_lastCut = now;
            s_cutMed = med > 0.0 ? med : 1e9;
            s_probeAt = now + (LONGLONG)(s_holdS * (double)q1s);
            s_dn = 0;  // dalsi rozhodnuti az z mereni po zmene
        } else if (g_limScale < 1.0 && s_dn >= 5 && ratio < 0.85 && lateF < 0.05) {
            if (g_limScale < s_bound - 0.025 && cool && now - s_lastCut > q1s / 2) {
                // pod znamou hranici (po vetsim srazeni): rychle zpet na 2 % pod ni; s velkou rezervou GPU (< 60 %
                // intervalu, napr. po jednorazovem zaseknuti) rovnou (v25x, driv +1 % po 0,5 s = 50 -> 95 % za ~25 s)
                const double step = ratio > 0.0 && ratio < 0.6 ? g_limScale * (0.8 / ratio - 1.0) : 0.01;
                g_limScale = std::min(s_bound - 0.02, g_limScale + std::max(0.01, step));
                s_lastCut = now;
                why = "zpet k hranici";
            } else if (s_probeAt && now >= s_probeAt) {
                // velka rezerva GPU (< 70 %): hra byla jen chvili pomala (po Alt+Tab, nacitani) -> +2 %/s (v25y, driv 0,5 %)
                g_limScale = std::min(1.0, g_limScale + (ratio < 0.7 ? 0.02 : 0.005));
                s_probeAt = now + q1s;
                why = "zkouska vys";
            }
        }
        if (s_lastHit && now - s_lastHit > 60 * q1s && s_holdS > 4.0) {
            s_holdS = std::max(4.0, s_holdS * 0.5);
            s_lastHit = now - 30 * q1s;
        }
        const bool up = why && why[0] == 'z';
        if (why && (!up || fabs(g_limScale - g_limScaleLogged) >= 0.02 ||
                    (g_limScale == 1.0 && g_limScaleLogged != 1.0))) {
            if (up || ++s_logs <= 60 || s_logs % 20 == 0)
                Log("prezentace: FG regulator GPU (%s) - strop skutecnych snimku %.1f FPS (%.1f %% z %.1f; hra %.1f "
                    "FPS, GPU hotovo %.1f ms = %.0f %% intervalu, nestihla %.0f %%, drzeni %.0f s)",
                    why, rate * g_limScale, g_limScale * 100.0, rate, fps, med, ratio * 100.0, lateF * 100.0, s_holdS);
            g_limScaleLogged = g_limScale;
        }
        if (winMs >= 250.0) {
            s_winQ = now;
            s_n = s_late = 0;
        }
        if (!adapt) FgAutoMult(s_dn >= 5 ? ratio : -1.0, (int)g_sh->fgActiveMult, now, q1s, fps, rate);
        rate *= g_limScale;
    } else if (cfg.fgLimAdapt == 2 && !adapt) {
        // Rezim 2 (v24s): hranici GPU si pamatuje. Kdyz hra v okne ~0,5 s nestihne slot u > 15 % snimku (GPU na
        // hranici), strop = 97 % prave dosazenych FPS a drzi se; vys se zkousi az po dobe drzeni (8 s, po kazdem
        // dalsim narazu do 60 s nejdriv 2x delsi), a to jen o 0,5 % za 2 s. Bez narazu 60 s se doba drzeni vraci.
        static LONGLONG s_winQ, s_probeAt, s_lastHit;
        static unsigned s_n, s_late;
        static uint32_t s_am;
        static double s_holdS = 8.0;
        const uint32_t amKey = g_sh->fgActiveMult;
        const LONGLONG q1s = g_qpf.QuadPart;
        if (!s_winQ || s_am != amKey) {
            s_am = amKey;
            s_winQ = now;
            s_n = s_late = 0;
            s_probeAt = s_lastHit = 0;
            s_holdS = 8.0;
            g_limScale = g_limScaleLogged = 1.0;
        }
        ++s_n;
        if (g_limNext && now >= g_limNext + (LONGLONG)((double)q1s / (rate * g_limScale))) ++s_late;
        const double winMs = Ms(s_winQ, now);
        if (winMs >= 500.0) {
            const double fps = (double)s_n * 1000.0 / winMs, lateF = (double)s_late / (double)s_n;
            const char* why = nullptr;
            if (lateF > 0.15 && fps > 10.0) {
                const double ns = std::max(0.1, std::min(g_limScale, 0.97 * fps / rate));
                if (ns < g_limScale - 1e-6 || g_limScale >= 1.0) {
                    // naraz do 60 s po minulem: hranice je blizko, zkouset vys jeste mene casto
                    if (s_lastHit && now - s_lastHit < 60 * q1s) s_holdS = std::min(120.0, s_holdS * 2.0);
                    s_lastHit = now;
                    g_limScale = ns;
                    why = "naraz";
                }
                s_probeAt = now + (LONGLONG)(s_holdS * (double)q1s);
            } else if (g_limScale < 1.0 && lateF < 0.03 && s_probeAt && now >= s_probeAt) {
                g_limScale = std::min(1.0, g_limScale + 0.005);
                s_probeAt = now + 2 * q1s;
                why = "zkouska vys";
            }
            if (s_lastHit && now - s_lastHit > 60 * q1s && s_holdS > 8.0) {
                s_holdS = std::max(8.0, s_holdS * 0.5);
                s_lastHit = now - 30 * q1s;
            }
            if (why && (fabs(g_limScale - g_limScaleLogged) >= 0.02 || why[0] == 'n' ||
                        (g_limScale == 1.0 && g_limScaleLogged != 1.0))) {
                Log("prezentace: FG rezerva GPU (%s) - strop skutecnych snimku %.1f FPS (%.1f %% z %.1f; hra %.1f "
                    "FPS, nestihla %.0f %%, drzeni %.0f s)",
                    why, rate * g_limScale, g_limScale * 100.0, rate, fps, lateF * 100.0, s_holdS);
                g_limScaleLogged = g_limScale;
            }
            s_winQ = now;
            s_n = s_late = 0;
        }
        rate *= g_limScale;
    } else if (cfg.fgLimAdapt == 1 && !adapt) {
        static LONGLONG s_winQ;
        static unsigned s_n, s_late, s_okWins;
        static uint32_t s_am;
        const uint32_t amKey = adapt ? 100u : g_sh->fgActiveMult;
        if (!s_winQ || s_am != amKey) {  // jiny nasobek / rezim: rezerva od nuly
            s_am = amKey;
            s_winQ = now;
            s_n = s_late = s_okWins = 0;
            g_limScale = g_limScaleLogged = 1.0;
        }
        ++s_n;
        if (g_limNext && now >= g_limNext + (LONGLONG)((double)g_qpf.QuadPart / (rate * g_limScale))) ++s_late;
        const double winMs = Ms(s_winQ, now);
        if (winMs >= 1000.0) {
            const double fps = (double)s_n * 1000.0 / winMs, lateF = (double)s_late / (double)s_n;
            if (lateF > 0.25 && fps > 10.0) {
                g_limScale = std::max(0.1, std::min(g_limScale, 0.93 * fps / rate));
                s_okWins = 0;
            } else if (lateF < 0.05 && ++s_okWins >= 2) {
                g_limScale = std::min(1.0, g_limScale + 0.01);
                s_okWins = 0;
            }
            if (fabs(g_limScale - g_limScaleLogged) >= 0.03 || (g_limScale == 1.0 && g_limScaleLogged != 1.0)) {
                Log("prezentace: FG rezerva GPU - strop skutecnych snimku %.1f FPS (%.0f %% z %.1f; hra %.1f FPS, "
                    "nestihla %.0f %%)",
                    rate * g_limScale, g_limScale * 100.0, rate, fps, lateF * 100.0);
                g_limScaleLogged = g_limScale;
            }
            s_winQ = now;
            s_n = s_late = 0;
        }
        rate *= g_limScale;
    }
    const LONGLONG period = (LONGLONG)((double)g_qpf.QuadPart / rate * (1.0 + cfg.fgMarginPct / 100.0));
    if (!g_limNext) {  // prvni snimek po zapnuti: jen zacatek slotu
        g_limNext = now;
        return;
    }
    g_limNext += period;
    if (now > g_limNext + period) {  // hra je o vic nez periodu pozadu (zasek, nacitani): novy zacatek
        g_limNext = now;
        ++g_stLimResync;
        return;
    }
    if (now >= g_limNext) {  // snimek slot nestihl: nespat, fazi drzet
        ++g_stLimLate;
        return;
    }
    LimiterSleepUntil(g_limNext);
    const LONGLONG woke = Qpc();
    g_stLimSleep.add(Ms(now, woke));
    g_stLimErr.add(fabs(Ms(g_limNext, woke)));
    g_limBits |= 1;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
void Present_LoadConfig(const wchar_t* ini) {
    auto num = [ini](const wchar_t* key, int def) {
        wchar_t buf[64];
        GetPrivateProfileStringW(L"present", key, L"", buf, 64, ini);
        return buf[0] ? (int)wcstol(buf, nullptr, 0) : def;
    };
    // Frame Generation: [fg] Mode pri startu rozhoduje, jestli prezentuje hlavni helper (cesta presenteru se voli
    // pri CreateDevice; prepnuti v menu za behu jen meni fgMode). Rezim, ktery helper zatim neumi, = vypnuto.
    wchar_t fgBuf[64];
    GetPrivateProfileStringW(L"fg", L"Mode", L"0", fgBuf, 64, ini);
    const int fgIni = ParseFgMode(fgBuf);
    int fgMode = FgModeImplemented(fgIni) ? fgIni : BMSD_FG_OFF;
    // Bezpecny start: minule spusteni skoncilo ztratou zarizeni GPU nebo padem procesu -> tento start bez FG (jen za
    // behu, ini se nemeni; v menu jde FG zase zapnout). [fg] SafeStart=0 vypne.
    if (g_safeStartWhy[0] && fgMode != BMSD_FG_OFF && GetPrivateProfileIntW(L"fg", L"SafeStart", 1, ini) != 0) {
        Log("config [fg]: minule spusteni skoncilo chybou (%s) - Frame Generation (%s) je pro tento start vypnuta "
            "([fg] SafeStart=0 vypne tuto pojistku)", g_safeStartWhy, FgModeName(fgMode));
        fgMode = BMSD_FG_OFF;
        g_safeStartHit = true;
    }
    g_fgMode = fgMode;
    GetPrivateProfileStringW(L"fg", L"TargetFps", L"0", fgBuf, 64, ini);
    cfg.fgTargetFps = (int)wcstol(fgBuf, nullptr, 0);
    if (cfg.fgTargetFps < 0 || cfg.fgTargetFps > 1000) cfg.fgTargetFps = 0;
    GetPrivateProfileStringW(L"fg", L"PauseInMenu", L"0", fgBuf, 64, ini);
    cfg.fgPauseInMenu = wcstol(fgBuf, nullptr, 0) != 0;
    GetPrivateProfileStringW(L"fg", L"LimiterMargin", L"1.0", fgBuf, 64, ini);
    for (wchar_t* c = fgBuf; *c; ++c)
        if (*c == L',') *c = L'.';
    cfg.fgMarginPct = wcstod(fgBuf, nullptr);
    if (!(cfg.fgMarginPct >= 0.0 && cfg.fgMarginPct <= 20.0)) cfg.fgMarginPct = 1.0;
    GetPrivateProfileStringW(L"fg", L"HudLess", L"1", fgBuf, 64, ini);
    cfg.hudless = wcstol(fgBuf, nullptr, 0) != 0;
    cfg.adaptIdleSkip = GetPrivateProfileIntW(L"fg", L"AdaptIdleSkip", 1, ini) != 0;
    GetPrivateProfileStringW(L"fg", L"Multiplier", L"2", fgBuf, 64, ini);
    {
        const long m = wcstol(fgBuf, nullptr, 0);
        g_fgMultAuto = (fgBuf[0] == L'a' || fgBuf[0] == L'A') ? 1 : 0;
        g_fgMult = m >= 2 && m <= 6 ? m : 2;
        cfg.fgAutoMax = GetPrivateProfileIntW(L"fg", L"AutoMaxMultiplier", 6, ini);
        if (cfg.fgAutoMax < 2 || cfg.fgAutoMax > 6) cfg.fgAutoMax = 6;
        cfg.fgAutoMin = GetPrivateProfileIntW(L"fg", L"AutoMinMultiplier", 2, ini);
        if (cfg.fgAutoMin < 2 || cfg.fgAutoMin > cfg.fgAutoMax) cfg.fgAutoMin = 2;
        cfg.fgAutoUpBelow = GetPrivateProfileIntW(L"fg", L"AutoUpBelow", 75, ini);
        if (cfg.fgAutoUpBelow < 50 || cfg.fgAutoUpBelow > 99) cfg.fgAutoUpBelow = 75;
        {
            wchar_t hb[32];
            GetPrivateProfileStringW(L"fg", L"AutoUpHold", L"3.0", hb, 32, ini);
            cfg.fgAutoUpHold = std::max(0.5, std::min(30.0, _wtof(hb)));
            GetPrivateProfileStringW(L"fg", L"AutoDownHold", L"1.0", hb, 32, ini);
            cfg.fgAutoDownHold = std::max(0.5, std::min(30.0, _wtof(hb)));
        }
        if (g_fgMultAuto) {  // start auto: [fg] AutoStartMultiplier (vychozi = AutoMinMultiplier)
            const int st = GetPrivateProfileIntW(L"fg", L"AutoStartMultiplier", cfg.fgAutoMin, ini);
            g_fgMult = std::max(cfg.fgAutoMin, std::min(cfg.fgAutoMax, st));
        }
    }
    // Adaptivni FSR
    GetPrivateProfileStringW(L"fg", L"RealCap", L"0", fgBuf, 64, ini);
    cfg.adaptCap = (int)wcstol(fgBuf, nullptr, 0);
    if (cfg.adaptCap < 0 || cfg.adaptCap > 1000) cfg.adaptCap = 0;
    GetPrivateProfileStringW(L"fg", L"FillBelow", L"0", fgBuf, 64, ini);
    cfg.adaptFill = (int)wcstol(fgBuf, nullptr, 0);
    if (cfg.adaptFill < 0 || cfg.adaptFill > 1000) cfg.adaptFill = 0;
    // [fg] Pacing (rozvrh B) odstranen ve v24z4 - plati vzdy A
    // Ladeni / testy: ForcePresenter=1 zapne prezentaci hlavnim helperem i s Mode=0 (jen skutecne snimky; FG pak jde
    // v menu zapnout za behu bez restartu)
    GetPrivateProfileStringW(L"fg", L"ForcePresenter", L"0", fgBuf, 64, ini);
    const bool fgForce = wcstol(fgBuf, nullptr, 0) != 0;
    cfg.fgForce = fgForce;
    // FG zap / vyp za behu ([fg] LiveSwitch=1, vychozi): cesta presmerovani i bez FG pri startu (presenter az se
    // zapnutim FG). LiveSwitch=0 = jako drive (bez FG pri startu proxy nic nepresmerovava, FG az po restartu).
    GetPrivateProfileStringW(L"fg", L"LiveSwitch", L"1", fgBuf, 64, ini);
    cfg.live = wcstol(fgBuf, nullptr, 0) != 0;
    GetPrivateProfileStringW(L"fg", L"OffTarget", L"0", fgBuf, 64, ini);
    cfg.offChild = wcstol(fgBuf, nullptr, 0) == 1;
    // FG zapnuta za behu: swapchain presenteru jednou znovu vytvorit tolik ms po prvnim (proti zamrzlemu obrazu,
    // kdyz DWM swapchain po DXVK v okne hry nezobrazi). 0 = vypnuto.
    GetPrivateProfileStringW(L"fg", L"LiveRefreshMs", L"800", fgBuf, 64, ini);
    cfg.liveRefreshMs = (DWORD)wcstoul(fgBuf, nullptr, 0);
    GetPrivateProfileStringW(L"fg", L"LimiterAdapt", L"3", fgBuf, 64, ini);  // vychozi 3 (v24w: overeno 4K 2x-4x)
    cfg.fgLimAdapt = (int)wcstol(fgBuf, nullptr, 0);
    GetPrivateProfileStringW(L"fg", L"LimiterAdaptFsr", L"0", fgBuf, 64, ini);
    cfg.fgLimAdaptFsr = (int)wcstol(fgBuf, nullptr, 0);
    GetPrivateProfileStringW(L"fg", L"LiveRefreshMode", L"4", fgBuf, 64, ini);
    cfg.liveRefreshMode = (DWORD)wcstoul(fgBuf, nullptr, 0);
    GetPrivateProfileStringW(L"fg", L"GamePresent", L"1", fgBuf, 64, ini);
    cfg.gamePresent = wcstol(fgBuf, nullptr, 0) != 0;
    // Casna kopie (vychozi 1): helper obraz zkopiruje a zverejni hned (s cilovym casem presentu), proxy ho hned
    // zkopiruje do swapchainu a v cilovy cas zavola jen present - kopie na GPU tak neceka za praci hry v case
    // presentu (sd zobrazeni FG 2x 1.2 -> 0.2 ms, docs/vrr-diagnostika-vysledek.md). Ladeni bit 14 = zapnout i pri 0.
    GetPrivateProfileStringW(L"fg", L"GamePresentEarly", L"1", fgBuf, 64, ini);
    cfg.gamePresentEarly = wcstol(fgBuf, nullptr, 0) != 0;
    GetPrivateProfileStringW(L"fg", L"GamePresentDebug", L"0", fgBuf, 64, ini);
    g_gpDebug = (int)wcstol(fgBuf, nullptr, 0);
    if (g_gpDebug) Log("config [fg]: GamePresentDebug=%d (ladeni zobrazeni v procesu hry)", g_gpDebug);
    const bool s2 = num(L"HelperPresent", 0) != 0;
    cfg.presAtStart = fgMode != BMSD_FG_OFF || fgForce;
    cfg.inMain = cfg.presAtStart || (cfg.live && !s2);
    if (fgIni != fgMode) Log("config [fg]: Mode=%s zatim neni implementovano - Frame Generation vypnuto", FgModeName(fgIni));
    cfg.on = s2 || cfg.inMain;
    if (!cfg.on) return;  // vypnuto: nic, ani log
    cfg.startTimeoutMs = (DWORD)num(L"StartTimeoutMs", 10000);
    // Hlavni helper obcas stovky ms nepotvrzuje (vytvoreni DLSS / DLSS-G feature, reimport): delsi limit nez u S2.
    // Skutecne zaseknuty helper ukonci po 5 s strazce mostu (interop.cpp) a zaloha nabehne pres konec procesu.
    cfg.hangMs = (DWORD)num(L"HangMs", cfg.inMain ? 6000 : 2000);
    cfg.probeMs = (DWORD)num(L"ProbeMs", 1000);
    cfg.probeMaxSec = (DWORD)num(L"ProbeMaxSec", 300);
    cfg.dxvkWindow = num(L"DxvkWindow", 0);
    cfg.child = num(L"ChildFallback", 1) != 0;
    cfg.measure = num(L"MeasureDisplay", 1) != 0;
    cfg.testStopSec = (DWORD)num(L"TestStopSec", 0);
    {
        wchar_t sk[32];
        GetPrivateProfileStringW(L"fg", L"PresentStartSkip", L"60", sk, 32, ini);
        cfg.startSkip = (int)wcstol(sk, nullptr, 0);
        if (cfg.startSkip < 0) cfg.startSkip = 0;
    }
    cfg.target = num(L"Target", 0) == 1 ? 1 : 0;
    QueryPerformanceFrequency(&g_qpf);
    InitializeCriticalSection(&g_semCs);
    Log("config [present]: %s StartTimeoutMs=%lu HangMs=%lu ProbeMs=%lu ProbeMaxSec=%lu DxvkWindow=%d ChildFallback=%d "
        "MeasureDisplay=%d TestStopSec=%lu Target=%d (%s)",
        cfg.inMain ? "prezentace v hlavnim helperu (Frame Generation: DXVK do skryteho okna, helper prezentuje do "
                     "okna hry)"
                   : "HelperPresent=1 (spike S2: DXVK do skryteho okna, helper prezentuje do okna hry)",
        cfg.startTimeoutMs, cfg.hangMs, cfg.probeMs, cfg.probeMaxSec, cfg.dxvkWindow, (int)cfg.child, (int)cfg.measure,
        cfg.testStopSec, cfg.target, cfg.target ? "HWND hry" : "dcerine okno proxy");
    if (cfg.inMain) {
        Interop_SetFgPresenter(true);  // helper (startuje pozdeji) zapne swapchain a hleda prezentacni kanal
        Log("config [fg]: Mode=%s%s Multiplier=%s%ld (auto %d..%d, nahoru pod %d %% po %.1f s, dolu po %.1f s) TargetFps=%d%s PauseInMenu=%d LimiterMargin=%.2f %% (limiter hry na "
            "TargetFps / nasobek minus rezerva, jen kdyz helper generuje) LiveSwitch=%d%s GamePresent=%d (%s) GamePresentEarly=%d",
            FgModeName(fgMode), fgForce && fgMode == BMSD_FG_OFF ? " (ForcePresenter=1: prezentuje helper i bez FG)" : "",
            g_fgMultAuto ? "auto, start " : "", g_fgMult, cfg.fgAutoMin, cfg.fgAutoMax, cfg.fgAutoUpBelow, cfg.fgAutoUpHold, cfg.fgAutoDownHold, cfg.fgTargetFps,
            cfg.fgTargetFps ? "" : " (auto: f - f^2/3600)",
            (int)cfg.fgPauseInMenu, cfg.fgMarginPct, (int)cfg.live,
            cfg.presAtStart ? "" : cfg.offChild ? " (FG vypnuta: DXVK do dcerineho okna proxy, OffTarget=1)"
                                                : " (FG vypnuta: DXVK primo do okna hry, FG jde zapnout za behu)",
            (int)cfg.gamePresent, cfg.gamePresent ? "snimky FG zobrazuje proces hry - VRR" : "zobrazuje helper - bez VRR",
            (int)cfg.gamePresentEarly);
        Log("config [fg] adaptivni FSR: RealCap=%d%s FillBelow=%d%s (rozvrh A)", cfg.adaptCap,
            cfg.adaptCap ? "" : " (vyp)", cfg.adaptFill, cfg.adaptFill ? "" : " (auto 90 % cile)");
    }
}

bool Present_Enabled() { return cfg.on; }
void Present_NoteScene() {
    if (g_sceneSeen <= cfg.startSkip) ++g_sceneSeen;
}

// ---- Frame Generation: rozhrani pro menu a proxy ----
int Present_FgGetMode() { return (int)g_fgMode; }

void Present_FgSetMode(int mode) {
    if (mode < 0 || mode >= BMSD_FG_COUNT) mode = BMSD_FG_OFF;
    g_fgMode = mode;
    // Presenter bezi: helper prepne za behu (DLSS-G <-> FSR). Vyp <-> zap: presenter zastavi / zapne nejblizsi Present
    // (PresenterWanted). Bez cesty presmerovani (LiveSwitch=0, zarizeni nepresmerovano) plati az po restartu hry.
    if (cfg.inMain && g_sh) {
        g_sh->fgMode = (uint32_t)mode;
        MemoryBarrier();
    }
}

void Present_SetSafeStart(const char* why) { strncpy(g_safeStartWhy, why ? why : "", sizeof g_safeStartWhy - 1); }
bool Present_SafeStartHit() { return g_safeStartHit; }
bool Present_TakeFallbackNote() { return InterlockedExchange(&g_fbNote, 0) != 0; }
bool Present_FgPresenterActive() { return cfg.inMain && !g_broken && !g_liveBlock && g_state != PS_FALLBACK; }

bool Present_FgModeAvailable(int mode) {
    if (mode == BMSD_FG_OFF) return true;
    if (!FgModeImplemented(mode)) return false;
    if (cfg.inMain && g_sh && g_state != PS_FALLBACK && g_sh->fgAvailValid) return ((g_sh->fgAvailMask >> mode) & 1) != 0;
    return true;  // helper se jeste nevyjadril (nebo presenter nebezi): nabidnout, stav rekne az po zapnuti
}

void Present_FgSetPause(bool pause) {
    const LONG v = pause && cfg.fgPauseInMenu ? 1 : 0;
    if (v == g_fgPause) return;
    g_fgPause = v;
    if (cfg.inMain && g_sh) {
        g_sh->fgPause = (uint32_t)v;
        MemoryBarrier();
    }
}

bool Present_FgGetPauseInMenu() { return cfg.fgPauseInMenu; }
void Present_FgSetPauseInMenu(bool on) { cfg.fgPauseInMenu = on; }  // projevi se dalsim Present_FgSetPause

int Present_FgGetTargetFps() { return cfg.fgTargetFps; }

int Present_FgGetMult() { return g_fgMultAuto ? 0 : (int)g_fgMult; }
bool Present_FgMultAuto() { return g_fgMultAuto != 0; }

// Nasobek, ktery helper pouzije (pri auto ho meni FgAutoMult; rezim auto zustava)
void Present_FgSetMultEff(int mult) {
    g_fgMult = mult >= 2 && mult <= 6 ? mult : 2;
    if (cfg.inMain && g_sh) {
        g_sh->fgMultiplier = FgMultShared();
        MemoryBarrier();
    }
}

// 0 = auto (FgAutoMult voli 2..AutoMaxMultiplier podle rezervy GPU), jinak pevny nasobek 2..6
void Present_FgSetMult(int mult) {
    if (mult == 0) {
        g_fgMultAuto = 1;
        if (g_fgMult < cfg.fgAutoMin || g_fgMult > cfg.fgAutoMax) g_fgMult = cfg.fgAutoMin;
    } else {
        g_fgMultAuto = 0;
        g_fgMult = mult >= 2 && mult <= 6 ? mult : 2;
    }
    if (cfg.inMain && g_sh) {  // helper prepne od dalsiho snimku (DLSS-G MultiFrameCount se smi menit mezi pary snimku)
        g_sh->fgMultiplier = FgMultShared();
        MemoryBarrier();
    }
}

int Present_FgMultMax(int mode) {
    if (mode == BMSD_FG_FSRADAPT) return 2;
    if (mode == BMSD_FG_FSR2X) {  // FSR FG od AMD jen 2x; 3x-5x vlastni generator AFG (v25n), kdyz ho helper nabizi
        if (cfg.inMain && g_sh && g_state != PS_FALLBACK && g_sh->fgAvailValid &&
            !((g_sh->fgAvailMask >> BMSD_FG_FSRADAPT) & 1))
            return 2;
        return 5;
    }
    if (mode != BMSD_FG_DLSSG) return 4;
    if (cfg.inMain && g_sh && g_state != PS_FALLBACK && g_sh->fgAvailValid) {
        if (!((g_sh->fgAvailMask >> BMSD_FG_DLSSG) & 1)) return 2;  // bez DLSS-G (v25m): auto i menu jako FSR 2x
        if (g_sh->fgMultMax >= 2) return (int)g_sh->fgMultMax;
    }
    return 4;  // helper se jeste nevyjadril (nebo presenter nebezi): nabidnout, nasobek pak omezi helper
}

bool Present_FgMultKnown() {
    return cfg.inMain && g_sh && g_state != PS_FALLBACK && g_sh->fgAvailValid && g_sh->fgMultMax >= 2;
}

// Ukazatel FPS: zobrazene snimky za posledni ~0.5 s. S presenterem v hlavnim helperu (helper prezentuje) z citacu
// helperu (skutecne + generovane, presne to, co je na obrazovce), jinak z Present hlavniho zarizeni.
static struct {
    LONGLONG t0;
    unsigned presents;
    uint64_t real0, gen0;
    float real, total;
    bool split, valid;
} g_fpsc;

static uint64_t ReadU64(volatile uint64_t* p) {
    return (uint64_t)InterlockedCompareExchange64((volatile LONGLONG*)p, 0, 0);  // 32bitova proxy: cteni bez trhani
}

void Present_FpsTick() {
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    const LONGLONG now = Qpc();
    ++g_fpsc.presents;
    const bool helper = cfg.inMain && g_sh && g_state == PS_HELPER;
    const uint64_t r = helper ? ReadU64(&g_sh->presCountReal) : 0, g = helper ? ReadU64(&g_sh->presCountGen) : 0;
    if (!g_fpsc.t0) {
        g_fpsc.t0 = now;
        g_fpsc.presents = 0;
        g_fpsc.real0 = r;
        g_fpsc.gen0 = g;
        return;
    }
    const double el = Ms(g_fpsc.t0, now);
    if (el < 500.0) return;
    if (el > 2000.0) {  // ukazatel byl vypnuty / hra stala (nacitani): nove okno, stara cisla neplati
        g_fpsc = {};
        return;
    }
    const uint64_t dr = r >= g_fpsc.real0 ? r - g_fpsc.real0 : 0, dg = g >= g_fpsc.gen0 ? g - g_fpsc.gen0 : 0;
    if (helper && (dr || dg)) {
        // adaptivni FSR (rozvrh A) zobrazi jen malo skutecnych snimku (vetsina slotu je generovana): skutecne = snimky
        // hry (Present hlavniho zarizeni), celkem = vse zobrazene helperem
        g_fpsc.real = g_fgMode == BMSD_FG_FSRADAPT ? (float)(g_fpsc.presents * 1000.0 / el) : (float)(dr * 1000.0 / el);
        g_fpsc.total = (float)((dr + dg) * 1000.0 / el);
        g_fpsc.split = dg > 0;
    } else {
        g_fpsc.real = g_fpsc.total = (float)(g_fpsc.presents * 1000.0 / el);
        g_fpsc.split = false;
    }
    g_fpsc.valid = true;
    g_fpsc.t0 = now;
    g_fpsc.presents = 0;
    g_fpsc.real0 = r;
    g_fpsc.gen0 = g;
}

bool Present_FpsGet(float* real, float* total, bool* split) {
    if (!g_fpsc.valid) return false;
    *real = g_fpsc.real;
    *total = g_fpsc.total;
    *split = g_fpsc.split;
    return true;
}

void Present_FgSetTargetFps(int fps) {
    cfg.fgTargetFps = fps > 0 && fps <= 1000 ? fps : 0;
    UpdateFgTarget();
}

int Present_FgGetRealCap() { return cfg.adaptCap; }
void Present_FgSetRealCap(int fps) {
    cfg.adaptCap = fps > 0 && fps <= 1000 ? fps : 0;
    FgPublishAdapt();
}
int Present_FgGetFillBelow() { return cfg.adaptFill; }
void Present_FgSetFillBelow(int fps) {
    cfg.adaptFill = fps > 0 && fps <= 1000 ? fps : 0;
    FgPublishAdapt();
}

bool Present_FgLimiting() {
    uint64_t v = g_limBits;
    unsigned n = 0;
    for (; v; v &= v - 1) ++n;
    return n >= 8;
}

void Present_FgInfo(PresentFgInfo* o) {
    static char msg[sizeof(((BmsdPresentShared*)nullptr)->fgMsg) + 1];
    static char name[sizeof(((BmsdPresentShared*)nullptr)->fgName) + 1];
    *o = PresentFgInfo();
    o->name = "";
    o->requested = (int)g_fgMode;
    o->presenter = cfg.inMain && !g_broken && !g_liveBlock;
    o->fallback = cfg.inMain && !g_liveBlock && (g_state == PS_FALLBACK || g_broken);
    o->fallbackReason = g_broken ? "presmerovani oken selhalo" : g_fbReason;
    o->state = BMSD_FGS_OFF;
    o->msg = "";
    o->targetFps = g_fgTarget;
    o->menuPause = g_fgPause != 0;
    o->refreshHz = g_fgHz;
    o->adaptTarget = (float)(g_fgTargetD * (1.0 - cfg.fgMarginPct / 100.0));
    o->adaptFill = cfg.adaptFill > 0 ? (float)cfg.adaptFill : 0.9f * o->adaptTarget;
    if (!cfg.inMain || !g_sh || o->fallback || g_state == PS_OFF || g_state == PS_IDLE) return;
    MemoryBarrier();
    o->helperReady = g_sh->helperReady != 0;
    o->state = g_sh->fgState;
    o->activeMode = (int)g_sh->fgActiveMode;
    memcpy(msg, g_sh->fgMsg, sizeof g_sh->fgMsg);
    msg[sizeof msg - 1] = 0;
    o->msg = msg;
    o->activeMult = (int)g_sh->fgActiveMult;
    o->adaptEngaged = g_sh->adaptEngaged != 0;
    o->realFps = g_sh->statFgRealFps;
    o->outFps = g_sh->statFgOutFps;
    o->evalMs = g_sh->statFgEvalMs;
    memcpy(name, g_sh->fgName, sizeof g_sh->fgName);
    name[sizeof name - 1] = 0;
    for (char* c = name; *c; ++c)
        if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?';  // jen ASCII (atlas pisma menu)
    o->name = name;
    FgLatchStats();
    o->genShare = g_fgLastShare;
    if (g_fgLastMode == (int)g_fgMode) {
        o->lastRealFps = g_fgLastReal;
        o->lastOutFps = g_fgLastOut;
        o->lastEvalMs = g_fgLastEval;
    }
}

void Present_PrepareCreate(HWND* focus, DWORD* flags, D3DPRESENT_PARAMETERS* pp, void** token) {
    *token = nullptr;
    if (!cfg.on || !pp || g_broken) return;
    HWND target = pp->hDeviceWindow ? pp->hDeviceWindow : *focus;
    char win[256] = "-";
    if (target) DescribeWindow(target, win, sizeof win);
    Log("prezentace: CreateDevice %ux%u, Windowed %d, interval %s, focus %p, flags 0x%lX, %s", pp->BackBufferWidth,
        pp->BackBufferHeight, pp->Windowed, IntervalStr(pp->PresentationInterval), (void*)*focus,
        (unsigned long)*flags, win);
    if (!target) return;
    bool redirect;
    if (!g_gameWnd) {
        redirect = pp->BackBufferWidth * pp->BackBufferHeight >= 320u * 240u ||
                   (!pp->BackBufferWidth && !pp->BackBufferHeight);
        if (redirect) {
            g_gameWnd = GetAncestor(target, GA_ROOT) ? GetAncestor(target, GA_ROOT) : target;
            if (g_gameWnd != target) Log("prezentace: okno hry = korenove okno %p", (void*)g_gameWnd);
        }
    } else {
        redirect = target == g_gameWnd || GetAncestor(target, GA_ROOT) == g_gameWnd;
    }
    if (!redirect) {
        Log("prezentace: zarizeni do okna %p (ne okno hry) - beze zmeny", (void*)target);
        return;
    }
    // FG za behu (LiveSwitch, hra startovala bez FG): presmerovani bez FG nesmi hre nic vzit. VSync (DXVK v presmerovani
    // prezentuje bez nej) a cela obrazovka v jinem rozliseni nez plocha (DXVK by v okne nemenil rezim obrazovky) =
    // zarizeni beze zmeny, FG az po restartu hry s [fg] Mode.
    const bool bigDev = pp->BackBufferWidth * pp->BackBufferHeight >= 320u * 240u ||
                        (!pp->BackBufferWidth && !pp->BackBufferHeight);
    if (cfg.inMain && !cfg.presAtStart && bigDev) {
        const char* block = nullptr;
        if (pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
            block = "hra ma zapnuty VSync";
        } else if (!pp->Windowed) {
            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof mi;
            HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                if (pp->BackBufferWidth != (UINT)(mi.rcMonitor.right - mi.rcMonitor.left) ||
                    pp->BackBufferHeight != (UINT)(mi.rcMonitor.bottom - mi.rcMonitor.top)) {
                    block = "cela obrazovka v jinem rozliseni nez plocha";
                } else if (pp->FullScreen_RefreshRateInHz) {
                    // Vyhradni cela obrazovka s frekvenci jinou nez plocha (plocha 60 Hz, hra 144 Hz): v okne by hra
                    // bezela na frekvenci plochy (a s G-SYNC "jen cela obrazovka" bez VRR) - zarizeni beze zmeny.
                    DEVMODEW dm = {};
                    dm.dmSize = sizeof dm;
                    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1 &&
                        (pp->FullScreen_RefreshRateInHz + 1 < dm.dmDisplayFrequency ||
                         pp->FullScreen_RefreshRateInHz > dm.dmDisplayFrequency + 1)) {
                        static char s_why[96];
                        snprintf(s_why, sizeof s_why, "cela obrazovka s frekvenci %u Hz, plocha %lu Hz",
                                 pp->FullScreen_RefreshRateInHz, (unsigned long)dm.dmDisplayFrequency);
                        block = s_why;
                    }
                }
            }
        }
        if (block) {
            g_liveBlock = block;
            Log("prezentace: FG za behu nejde (%s) - zarizeni beze zmeny; FG az po restartu hry s [fg] Mode", block);
            return;
        }
    }
    if (!EnsureWndThread()) {
        g_broken = true;
        Log("prezentace: CHYBA - bez vlakna oken nejde presmerovat, spike vypnut (DXVK kresli do okna hry)");
        return;
    }
    UINT w = pp->BackBufferWidth, h = pp->BackBufferHeight;
    if (!w || !h) {
        RECT rc = {};
        GetClientRect(target, &rc);
        w = rc.right > 0 ? rc.right : 1;
        h = rc.bottom > 0 ? rc.bottom : 1;
    }
    HWND hidden = (HWND)SendMessageW(g_ctl, WMP_HIDDEN, (WPARAM)w, (LPARAM)h);
    if (!hidden) {
        g_broken = true;
        Log("prezentace: CHYBA - skryte okno nevzniklo (%lu), spike vypnut", GetLastError());
        return;
    }
    DevInfo* di = new DevInfo{nullptr, hidden, target, pp->PresentationInterval};
    Redirect(pp, hidden);
    *focus = hidden;
    *flags |= D3DCREATE_NOWINDOWCHANGES;
    *token = di;
    Log("prezentace: -> DXVK: Windowed 1, okno %p (skryte %ux%u), IMMEDIATE, +NOWINDOWCHANGES", (void*)hidden, w, h);
}

void Present_AfterCreate(void* token, HRESULT hr, IDirect3DDevice9* dev) {
    DevInfo* di = (DevInfo*)token;
    if (!di) return;
    if (FAILED(hr) || !dev) {
        Log("prezentace: CreateDevice 0x%08X - skryte okno rusim", (unsigned)hr);
        SendMessageW(g_ctl, WMP_DESTROY, (WPARAM)di->hidden, 0);
    } else {
        // Zaznam podle adresy zarizeni: stejna adresa = stare zarizeni uz neexistuje (jeho zaznam prepsat). Plne pole:
        // prepsat nejstarsi zaznam jineho nez hlavniho zarizeni - okno noveho zarizeni se nerusi (DXVK ho uz pouziva).
        di->dev = dev;
        DevInfo* slot = FindDev(dev);
        if (!slot && g_nDevs < 16) slot = &g_devs[g_nDevs++];
        if (!slot) {
            static int s_next;
            for (int k = 0; k < 16 && !slot; ++k, s_next = (s_next + 1) % 16)
                if (g_devs[s_next].dev != g_vkDev) slot = &g_devs[s_next];
            if (!slot) slot = &g_devs[15];
            Log("prezentace: tabulka zarizeni plna - prepisuji zaznam zarizeni %p", (void*)slot->dev);
        }
        *slot = *di;
        Log("prezentace: zarizeni %p presmerovano (skryte okno %p, okno hry %p)", (void*)dev, (void*)di->hidden,
            (void*)di->target);
    }
    delete di;
}

bool Present_PrepareReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (!cfg.on || !pp) return false;
    DevInfo* di = FindDev(dev);
    if (!di) return false;
    Log("prezentace: Reset %ux%u, Windowed %d, interval %s", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed,
        IntervalStr(pp->PresentationInterval));
    // Zarizeni zustava presmerovane (okno bez VSync DXVK); zmena VSync nebo vyhradni cele obrazovky za behu se tim
    // neprojevi - jen upozornit (pri startu hry s temito volbami se zarizeni nepresmeruje, viz Present_PrepareCreate).
    bool otherRes = false;
    if (!pp->Windowed) {
        MONITORINFO mi = {sizeof mi};
        HMONITOR mon = MonitorFromWindow(di->target, MONITOR_DEFAULTTONEAREST);
        otherRes = mon && GetMonitorInfoW(mon, &mi) &&
                   (pp->BackBufferWidth != (UINT)(mi.rcMonitor.right - mi.rcMonitor.left) ||
                    pp->BackBufferHeight != (UINT)(mi.rcMonitor.bottom - mi.rcMonitor.top));
    }
    if ((pp->PresentationInterval != di->interval && pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) ||
        otherRes)
        Log("prezentace: POZOR - hra pri Resetu chce %s; plati az po restartu hry (zarizeni zustava v okne bez VSync, "
            "s FG ridi VSync helper)",
            otherRes ? "celou obrazovku v jinem rozliseni nez plocha" : "VSync");
    di->interval = pp->PresentationInterval;
    ResizeHidden(di->hidden, di->target, pp);
    Redirect(pp, di->hidden);
    return true;
}

void Present_BeforeReset(IDirect3DDevice9* dev) {
    if (!cfg.on || dev != g_vkDev || !g_vkReady) return;
    // Reset rusi backbuffer: nase kopie nad nim musi dobehnout a reference pustit
    const bool done = FinishAll();
    if (!done) Log("prezentace: WARN - kopie pro helper pred Resetem nedobehly");
    g_hlRTRetryTick = 0;
    // HUD-less RT (D3DPOOL_DEFAULT): jinak Reset selze; po Resetu vznikne znovu pri dalsim snimku. Kdyz kopie nad nim
    // jeste bezi a zarizeni je Ex (Reset zdroje DEFAULT nevadi), radsi ho nechat (uvolni se pri zmene velikosti).
    if (!done && g_hlRT) {
        IDirect3DDevice9Ex* ex = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&ex)) && ex) {
            ex->Release();
            Log("prezentace: HUD-less RT se pred Resetem neuvolnuje (kopie nad nim nedobehla, zarizeni Ex)");
            return;
        }
    }
    ReleaseHudlessRT();
}

IDirect3DSurface9* Present_HudlessTarget(IDirect3DDevice9* d) {
    // Jen kdyz helper generuje nebo je kratce pozastaveny (reset, zaseknuti - dalsi snimek uz generuje) a most bezi
    // (minuly snimek mel B; prvni konce sceny po startu / nahrani mapy most vynechava, viz [fg] BridgeStartSkip);
    // jinak by kopie stala cas zbytecne. Menu modu (g_fgPause) a vypnute FG ne.
    MemoryBarrier();
    const int32_t st = g_sh ? g_sh->fgState : BMSD_FGS_OFF;
    if (!cfg.inMain || !cfg.hudless || g_state != PS_HELPER || !g_sh || !g_hlImages || d != g_vkDev || g_fgPause ||
        g_fgMode == BMSD_FG_OFF || (st != BMSD_FGS_ACTIVE && !(st == BMSD_FGS_PAUSED && g_curBridgeN)) || !g_w)
        return nullptr;
    // Adaptivni FSR prave jen se skutecnymi snimky (hra stiha cil): HUD-less nikdo nepouzije ([fg] AdaptIdleSkip)
    if (cfg.adaptIdleSkip && g_fgMode == BMSD_FG_FSRADAPT && st == BMSD_FGS_ACTIVE && !g_sh->adaptEngaged) return nullptr;
    if (g_hlRT && (g_hlRTW != g_w || g_hlRTH != g_h)) {
        if (!FinishAll()) return nullptr;  // kopie nad starym RT jeste bezi
        ReleaseHudlessRT();
    }
    if (!g_hlRT) {
        if (g_hlRTRetryTick && GetTickCount64() < g_hlRTRetryTick) return nullptr;
        IDirect3DSurface9* bb = nullptr;
        D3DSURFACE_DESC desc = {};
        if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return nullptr;
        bb->GetDesc(&desc);
        bb->Release();
        if (desc.Width != g_w || desc.Height != g_h) return nullptr;  // sdilene obrazy jeste nemaji novou velikost
        HRESULT hr = d->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_hlRT,
                                           nullptr);
        if (FAILED(hr) || !g_hlRT) {
            g_hlRT = nullptr;
            if (!g_hlRTFailLogged) Log("prezentace: HUD-less RT %ux%u D3DFMT %d nejde vytvorit (0x%08X)", desc.Width,
                                       desc.Height, (int)desc.Format, (unsigned)hr);
            g_hlRTFailLogged = true;
            g_hlRTRetryTick = GetTickCount64() + 2000;
            return nullptr;
        }
        g_hlRTRetryTick = 0;
        g_hlRTW = desc.Width;
        g_hlRTH = desc.Height;
        g_hlRTFmt = desc.Format;
        Log("prezentace: HUD-less RT %ux%u D3DFMT %d vytvoren", desc.Width, desc.Height, (int)desc.Format);
    }
    return g_hlRT;
}

void Present_HudlessFrame(bool valid, unsigned snaps) {
    g_hlFrame = valid && g_hlRT != nullptr;
    g_stHlSnaps += snaps;
}

void Present_AfterReset(IDirect3DDevice9* dev, HRESULT hr) {
    if (!cfg.on || !FindDev(dev)) return;
    char win[256] = "-";
    if (g_gameWnd) DescribeWindow(g_gameWnd, win, sizeof win);
    Log("prezentace: Reset -> 0x%08X; okno hry %s", (unsigned)hr, win);
}

HWND Present_OnPresent(IDirect3DDevice9* d, IDirect3DSwapChain9* sc, HWND h, bool isMain, void* caller,
                       void* ourHook) {
    if (!cfg.on) return h;
    DevInfo* di = FindDev(d);
    if (!di) return h;  // zarizeni nepresmerovano (jine okno, nebo spike nefunguje)
    if (!isMain) return h == di->target ? nullptr : h;  // vedlejsi (ANGLE): jen do sveho skryteho okna
    // FG: snimek mostu, ktery patri k tomuto snimku hry - vyzvednout pri kazdem Present (i kdyz se snimek nepreda),
    // jinak by stare cislo dostal dalsi snimek bez konce sceny (menu hry, nacitani)
    if (cfg.inMain) g_curBridgeN = Interop_TakeBridgeN();
    LogStats(d, caller, ourHook);
    if (g_state == PS_FALLBACK) return FallbackTarget();
    const bool want = PresenterWanted();
    if (g_state == PS_IDLE || g_state == PS_OFF) {
        if (!want) {  // FG vypnuta: DXVK primo do okna hry (prvni snimek bez FG pri startu = rovnou PS_OFF)
            if (g_state == PS_IDLE) {
                g_state = PS_OFF;
                EnterOffTarget();
                Log("prezentace: FG vypnuta - DXVK prezentuje %s; FG jde zapnout za behu",
                    g_direct ? "primo do okna hry" : "do dcerineho okna proxy");
            }
            return OffTarget();
        }
        if (!StartPresenter(d)) return FallbackTarget();
    } else if (!want) {  // FG vypnuta za behu: presenter pryc, DXVK primo do okna hry
        if (!StopPresenter("FG vypnuta")) return FallbackTarget();
        return OffTarget();
    }
    if (d != g_vkDev) {  // jine hlavni zarizeni nez to, pro ktere je Vulkan (spike: neresi)
        Fallback("nove hlavni zarizeni");
        return FallbackTarget();
    }
    if (!CheckHelper()) return FallbackTarget();
    SyncChild(g_presWnd);
    g_sh->presentInterval = di->interval == D3DPRESENT_INTERVAL_IMMEDIATE ? 0 : 1;
    if (!g_sh->helperReady) {
        ++g_stNotReady;
        if (g_switches) {  // FG zapnuta za behu: helper probudit (most ho jinak budi jen svymi zpravami)
            SetEvent(g_evFrame);
            return OffTarget();  // a do prvniho snimku od helperu dal kreslit do okna hry (zadny zamrzly obraz)
        }
        return nullptr;
    }
    if (g_sceneSeen >= 1 && g_sceneSeen <= cfg.startSkip) {  // [fg] PresentStartSkip (viz cfg)
        static bool s_logged;
        if (!s_logged) {
            s_logged = true;
            Log("prezentace: prvnich %d snimku sceny po startu se helperu nepredava ([fg] PresentStartSkip)",
                cfg.startSkip);
        }
        return nullptr;
    }
    PostFrame(d, sc);
    if (g_state == PS_FALLBACK) return FallbackTarget();
    g_oPresentQpc = Qpc();
    // FG zapnuta za behu: dokud helper neprezentuje, DXVK dal i do okna hry (prezentacni okno helperu je nad nim)
    if (g_state == PS_STARTING && g_switches) return OffTarget();
    return nullptr;  // DXVK do skryteho okna (hDeviceWindow)
}

void Present_AfterPresent(IDirect3DDevice9*, bool isMain) {
    if (!cfg.on || !isMain) return;
    if (g_oPresentQpc) {
        g_stOPresent.add(Ms(g_oPresentQpc, Qpc()));
        g_oPresentQpc = 0;
    }
    if (cfg.inMain) FgLimiter();  // strop skutecnych snimku pri generovani (spi az po Present = vstup co nejpozdeji)
}
