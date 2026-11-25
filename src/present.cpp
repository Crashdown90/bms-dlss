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
#include <winternl.h>
#include <d3d9.h>
#include <d3dkmthk.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bmsd_shared.h"
#include "dxvk_interop.h"
#include "interop.h"
#include "present.h"

bool g_lcWantFirst;  // src/latchain.inl, vlakno renderu: ceka se na prvni volani zarizeni po uvolneni snimku

namespace {

// ---- konfigurace ----
struct PCfg {
    bool on = false;
    DWORD startTimeoutMs = 30000;  // helper nema swapchain do tolika ms -> zaloha (bez pauz hry a dlouhe prace helperu)
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
    int adaptLock = -1;            // [fg] AdaptLock: adaptivni rezimy - limiter drzi skutecne snimky na zlomku cile q / p
                                   // (0 / 1; -1 = klic v ini neni: vychozi podle rezimu, FgLockOn)
    int adaptLockDen = 6;          // [fg] AdaptLockDen: nejvyssi jmenovatel p zlomku (2 .. 8)
    int adaptMinFps = 40;          // [fg] AdaptMinFps (v102l): adaptivni rezimy - zamek nejde pod tolik skutecnych FPS
                                   // (nejvys cil / 2); hra pod tim = cil vystupu jen AdaptLowMult x skutecne (0 = vyp)
    int adaptLowMult = 3;          // [fg] AdaptLowMult (v102l): nejvyssi nasobek, kdyz hra jede pod AdaptMinFps (2 .. 6)
    // [fg] Preference (v103e, src/fgpref.inl): kdy adaptivni rezimy generuji (PREF_*); PreferenceFps = prah profilu
    // "vlastni" (30 .. 300), PreferenceHyst = hystereze v % (5 .. 40), PreferenceMinGain = nejmensi zisk v %
    // (0 .. 100, -1 = podle profilu)
    int prefMode = PREF_BAL;
    int prefPct = 75;              // v103f2: prah vlastniho profilu v % cile (20 .. 95)
    int prefHyst = 10;             // v103f2: vlastni profil - vypnout o tolik procentnich bodu vys (3 .. 40)
    int prefMinGain = -1;
    bool prefBp = true;            // [debug] PrefBackpressure (v103f3, vychozi 1 od f4): zpetny tlak ve stavu jen skutecne
    bool prefCap = true;           // [debug] PrefCap (v103f, testy S1): 0 = stav jen skutecne s preferenci nikdy bez stropu
    bool prefBpReg = false;        // [debug] PrefBpReg (v103g): regulator latence pocita cekani zpetneho tlaku jako cekani na GPU
    double prefCapK = 1.0;         // [debug] PrefCapK (v103g): zakladni k stropu jen skutecne (0,85 .. 1; 1 = bez stropu,
                                   // dokud presenter nepreskakuje): hra tesne pod moznosti GPU = prazdna fronta, nizsi latence
    int gpPrio = 0;                // [fg] GamePresentPriority (v103f): globalni priorita fronty presenteru 0 / 1 HIGH / 2 REALTIME
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
    int fgResumeGuard = 8;  // [fg] ResumeGuard: ochrana po navratu (Alt+Tab, pauza snimku hry) nejvys tolik s - strop
                            // skutecnych snimku / stupen zamku z doby pred Alt+Tabem, nic nesrazet; 0 = jako v102j
    bool presAtStart = false;  // [fg] Mode != 0 (nebo ForcePresenter) pri startu: presenter hned od prvniho Present
    bool fgForce = false;      // [fg] ForcePresenter=1: presenter bezi i s FG vypnutou (ladeni)
    DWORD liveRefreshMs = 800; // (+ [fg] LiveRefreshMode: 1 = novy swapchain, 2 = okno hry WS_EX_LAYERED tam a zpet)
    DWORD liveRefreshMode = 4;
// [fg] LiveRefreshMs: obnova swapchainu presenteru po zapnuti FG za behu
    bool offChild = false;    // [fg] OffTarget=1: bez FG DXVK do dcerineho okna proxy misto primo do okna hry (ladeni)
    // [fg] OffWindow=0 (vychozi od v103j): bez FG DXVK primo do okna hry, flip ridi DxQueuePresent (g_dxWantFlip).
    // 1 (vychozi v102v az v103i): bez FG DXVK do vlastniho dcerineho okna proxy; flip na nem po Alt+Tabu ovladac
    // neuvolni, proto je s 1 vychozi DxvkFse=0.
    // Swapchain DXVK s celoobrazovkovym flipem ovladace (Present_HookDxvkVulkan) drzi flip i potom, co do nej DXVK
    // prestane prezentovat, a presenter FG pak jde jen "Hardware: Legacy Copy to front buffer" (~120 misto 156
    // snimku/s). Skryti okna ani jeho zniceni nepomuze (zniceni: dalsi swapchain DXVK uz jen pres kompozici). Proto se
    // pri zapnuti FG okno DXVK (pod oknem presenteru) zmensi na 1x1, DXVK do nej posle 2 snimky (swapchain znovu 1x1,
    // flip uvolnen), okno se skryje a presenter si swapchain jednou vytvori znovu. Pri vypnuti FG ho SyncChild vrati
    // na celou plochu a DXVK si swapchain zase vytvori sam.
    bool offOwn = false;
    // [fg] HealComposed=1 (vychozi, v102x): presenter FG jde pres kompozici DWM, ackoli je okno hry v popredi pres cely
    // monitor -> swapchain presenteru znovu (ScHealComposed). 0 = vypnuto.
    bool healComposed = true;
    // [fg] GamePresent=1 (vychozi): snimky FG zobrazuje proxy v procesu hry (src/gamepresent.inl) - jen tak ovladac
    // zapne VRR (G-SYNC / FreeSync v okne); 0 = jako do v19 (helper prezentuje sam, monitor na pevne frekvenci)
    bool gamePresent = true;
    bool gamePresentEarly = true;  // [fg] GamePresentEarly: helper zverejni obraz hned po kopii s cilovym casem
} cfg;
bool g_gtOn = true;  // [debug] GameThreads: cas vlaken hry (src/gamethreads.inl)
bool g_lcOn = true;  // [debug] LatChain: retezec latence vstup -> Present -> zobrazeni (src/latchain.inl)
bool g_lcCsv;        // [debug] LatChainCsv: zaznamy snimku, pump a slotu do bms_dlss_latchain.csv
static int LcDispBegin(LONGLONG real, bool gen);  // src/latchain.inl (vlakno presenteru FG)
static void LcDispSet(int i, LONGLONG p0, LONGLONG p1, LONGLONG g1);
static void LcSetPost(LONGLONG pq);
// v103f3 (S1, zpetny tlak): fgpref.inl nastavi, kdyz ve stavu jen skutecne s preferenci ma hra pred predanim snimku pockat,
// az presenter v procesu hry prevezme posledni zverejneny obraz (misto zahazovani v presenteru); g_bpMaxMs = nejdel
static volatile bool g_bpWant;
static volatile double g_bpMaxMs = 20.0;
static double g_bpLastMs;  // v103g: cekani zpetneho tlaku v poslednim Present (Present_BpWaitMs)
bool g_feCsvInit;    // [debug] FgEstCsv: snimky kolem prepnuti do bms_dlss_fgest.csv (src/fgest.inl)
bool g_scOn = true;  // [debug] SelfCheck: samokontroly (radky "KONTROLA" v logu), viz SelfCheck
int g_scTest;        // [debug] SelfCheckTest (ladeni, overeni samokontrol): bit 0 (1) = TargetFps bez omezeni stropem
                     // zobrazeni (stav pred v102u), bit 1 (2) = helper nevypina zbytecne generovani (cte helper),
                     // bit 2 (4) = zamek adaptivniho rezimu drzet na nejnizsim stupni
bool g_menuOpenNow;  // menu modu otevrene (Present_FgSetPause, proxy kazdy snimek)
int g_sceneSeen;  // snimky s koncem sceny od startu (Present_NoteScene; jen do startSkip + 1)

// ---- okna (vlastni vlakno se smyckou zprav) ----
const UINT WMP_HIDDEN = WM_APP + 0x51;   // wParam = sirka, lParam = vyska -> HWND
const UINT WMP_CHILD = WM_APP + 0x52;    // wParam = rodic -> HWND
const UINT WMP_DESTROY = WM_APP + 0x53;  // wParam = okno
const UINT WMP_UNSTICK = WM_APP + 0x54;  // wParam = okno hry
const UINT WMP_SHOW = WM_APP + 0x55;     // wParam = dcerine okno proxy, lParam = SW_* (synchronne, jen vlastni okna)
HINSTANCE g_hinst;
HWND g_ctl;  // okno pro zpravy (HWND_MESSAGE) vlakna oken
// alttab.inl: flip DXVK podle popredi okna hry (vlakno oken; udalost zmeny popredi a pro jistotu kazdych ~50 ms)
void AtTick();
void CALLBACK AtWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
extern volatile LONG g_atFrames;
extern volatile LONG g_atLostTop;

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
    if (m == WM_TIMER) {
        AtTick();
        return 0;
    }
    if (m == WMP_SHOW) {
        if ((int)l < 0)  // zmensit na 1x1 (swapchain DXVK na nem pri dalsim Present znovu, bez celoobrazovkoveho flipu)
            SetWindowPos((HWND)w, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        else
            ShowWindow((HWND)w, (int)l);
        return 1;
    }
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
    SetTimer(g_ctl, 1, 50, nullptr);
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, AtWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, AtWinEvent, GetCurrentProcessId(), 0,
                    WINEVENT_OUTOFCONTEXT);
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
int g_offShrink;  // [fg] OffWindow: 1..2 = DXVK jeste tolik snimku do zmenseneho okna, 3 = skryt

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
ULONGLONG g_startChk, g_startPaused;  // limit startu: posledni kontrola a cas, kdy limit stal (pauza hry, prace helperu)
uint64_t g_lastAck;
int g_dxFse;  // [present] DxvkFse
// v103j: bez FG DXVK primo do okna hry s celoobrazovkovym flipem (Alt+Tab ho uvolni), pred zapnutim FG swapchain DXVK na
// okne hry znovu bez flipu (DISALLOWED) a po vypnuti FG zase s flipem. Vymenu udela samo DXVK na svem vlakne: hacek
// vkQueuePresentKHR po skutecnem presentu vrati VK_SUBOPTIMAL_KHR, dokud swapchain nema zadany rezim (DxQueuePresent).
volatile LONG g_dxWantFlip = 1;   // vlakno hry: swapchain DXVK na okne hry ma mit flip
volatile LONG g_dxGameFlip = -1;  // aktualni swapchain DXVK na okne hry: 1 = flip, 0 = DISALLOWED, -1 = zadny / jiny
volatile LONG g_dxGamePres;       // presenty na tomto swapchainu
int g_dxRel;                      // 1 = ceka se na uvolneni flipu pred startem presenteru
LONGLONG g_dxRelQpc;
bool DxSwapOn();
bool g_fbOffWin;  // zaloha s [fg] OffWindow: DXVK do dcerineho okna proxy (jako PS_OFF), bez sondy okna hry
bool g_fbRetry;  // zaloha z pomalosti (helper zije): vypnuti FG v menu -> PS_OFF, dalsi zapnuti = novy pokus
// Automaticky novy pokus po zaloze z pomalosti (helper zije; start, "nereaguje", GPU nedobehla, chyba presenteru):
// odstup 5 / 15 / 45 / 135 s, nejvys kFbAutoMax pokusu; pocitadlo se vynuluje po 120 s bezne prezentace. Pak uz jen
// rucne (vypnuti a zapnuti FG v menu).
ULONGLONG g_fbAutoAt;
unsigned g_fbAutoN;
const unsigned kFbAutoMax = 4;
ULONGLONG g_slotStall;     // kopie pro helper nedobehla v kratkem limitu: od kdy (0 = ne); zaloha az po kSlotStallMs
const ULONGLONG kSlotStallMs = 20000;
ULONGLONG g_hangChk;       // posledni kontrola "nereaguje" (mezera nad 0,5 s = hra nevolala Present, nepocita se)
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
Acc g_stBp;          // v103f3: zpetny tlak - cekani hry na prevzeti obrazu presenterem (jen skutecne)
unsigned g_stBpTo;   // ... z toho vyprselo
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
double g_fgOutCap;                     // nejvyssi okamzity vystup presenteru (FPS): strop VRR, rucni cil nad nim 2 x cil
bool g_fgFree;                         // rucni cil nad stropem VRR: presenter bez stropu, obrazy po poradi
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
// Adaptivni rezimy (rozvrh A v helperu, limiter na cil / RealCap): FSR adaptivni a DLSS-G adaptivni
static bool FgIsAdapt(int m) { return m == BMSD_FG_FSRADAPT || m == BMSD_FG_DLSSGADAPT; }
// Statistiku cte Present_FgInfo (menu, kazdy snimek s otevrenym menu) i radek logu (kazdych ~5 s)
void FgLatchStats() {
    if (!g_sh || g_sh->statSeq == g_fgLastStatSeq) return;
    g_fgLastStatSeq = g_sh->statSeq;
    MemoryBarrier();
    const uint32_t real = g_sh->statFgReal, gen = g_sh->statFgGen, mode = g_sh->fgActiveMode;
    // adaptivni FSR generuje jen cast snimku (nebo nic nad cilem): cisla plati vzdy
    g_fgLastShare = real > 0 && (gen * 10 >= real * 8 || FgIsAdapt((int)mode));
    if (!g_fgLastShare || !mode) return;
    g_fgLastMode = (int)mode;
    g_fgLastReal = g_sh->statFgRealFps;
    g_fgLastOut = g_sh->statFgOutFps;
    g_fgLastEval = g_sh->statFgEvalMs;
}

const char* FgModeName(int m) {
    return m == BMSD_FG_DLSSG ? "DLSS-G" : m == BMSD_FG_FSR2X ? "FSR" : m == BMSD_FG_FSRADAPT ? "FSR adaptivni"
           : m == BMSD_FG_DLSSGADAPT ? "DLSS-G adaptivni" : "vypnuto";
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
bool FgModeImplemented(int m) {
    return m == BMSD_FG_DLSSG || m == BMSD_FG_FSR2X || m == BMSD_FG_FSRADAPT || m == BMSD_FG_DLSSGADAPT;
}

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
    if (!_wcsicmp(s, L"dlssgadaptive") || !_wcsicmp(s, L"dlssadaptive")) return BMSD_FG_DLSSGADAPT;
    const long v = wcstol(s, nullptr, 0);
    return v > 0 && v < BMSD_FG_COUNT ? (int)v : BMSD_FG_OFF;
}

uint32_t g_adLockWord;  // zlomkovy zamek adaptivniho FSR: p | q << 8 zverejnene helperu (0 = bez zamku)
// Zlomkovy zamek zapnuty: [fg] AdaptLock z ini, jinak vychozi podle rezimu (FSR adaptivni 0, DLSS-G adaptivni
// kDgLockDefault - docs/adaptivni-dlssg-etapa2-vysledek.md)
static const bool kDgLockDefault = true;
// Rucni cil nad stropem VRR (v103h): zamek vychozi i pro FSR adaptivni. Bez nej hra vytizi GPU na 100 %, kopie obrazu
// (stovky za sekundu) pak cekaji az 16 ms a vystup se rozpada; zamek drzi GPU do ~92 %.
static bool FgLockOn() { return cfg.adaptLock >= 0 ? cfg.adaptLock != 0 : ((g_fgMode == BMSD_FG_DLSSGADAPT && kDgLockDefault) || (g_fgMode == BMSD_FG_FSRADAPT && g_fgFree)); }

// [fg] Preference z ini: nazev profilu (PREF_*), nebo cislo 30 .. 300 = vlastni s timto prahem (zapise do *fps); -1 = nezname
static int PrefParse(const wchar_t* in, int* fps) {
    wchar_t s[32];
    int n = 0;
    while (*in == L' ' || *in == L'\t') ++in;
    for (; *in && *in != L';' && *in != L'#' && n < 31; ++in) s[n++] = *in;
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t')) --n;
    s[n] = 0;
    if (!_wcsicmp(s, L"always")) return PREF_ALWAYS;
    if (!_wcsicmp(s, L"responsiveness")) return PREF_RESP;
    if (!_wcsicmp(s, L"balanced")) return PREF_BAL;
    if (!_wcsicmp(s, L"smooth") || !_wcsicmp(s, L"smoothness")) return PREF_SMOOTH;
    if (!_wcsicmp(s, L"custom")) return PREF_CUSTOM;
    if (!_wcsicmp(s, L"steady")) return PREF_STEADY;
    if (s[0] >= L'0' && s[0] <= L'9') {
        const long v = wcstol(s, nullptr, 10);
        if (v >= 20 && v <= 95) {  // v103f2: % cile
            *fps = (int)v;
            return PREF_CUSTOM;
        }
    }
    return -1;
}
static const char* PrefName(int p) {
    static const char* const k[] = {"always", "responsiveness", "balanced", "smooth", "custom", "steady"};
    return p >= 0 && p < PREF_COUNT ? k[p] : "?";
}

// DLSS-G adaptivni vyzaduje DLSS-G s nasobkem nad 2x (RTX 50). Kdyz ho helper nenabizi (RTX 40, bez DLSS-G, selhani
// DLSS-G), prejde se na nahradni rezim: FSR adaptivni (stejny cil a rozvrh), jinak DLSS-G, jinak vypnuto. Jednou
// zprava do logu a OSD (Present_TakeFgSubstNote).
volatile LONG g_fgSubstNote;
static void FgAdaptSubstitute() {
    if (g_fgMode != BMSD_FG_DLSSGADAPT || !cfg.inMain || !g_sh || g_state != PS_HELPER || !g_sh->fgAvailValid) return;
    const uint32_t mask = g_sh->fgAvailMask;
    if ((mask >> BMSD_FG_DLSSGADAPT) & 1) return;
    const int to = ((mask >> BMSD_FG_FSRADAPT) & 1) ? BMSD_FG_FSRADAPT : ((mask >> BMSD_FG_DLSSG) & 1) ? BMSD_FG_DLSSG : BMSD_FG_OFF;
    Log("prezentace: FG rezim DLSS-G adaptivni na tomto GPU neni k dispozici (potrebuje DLSS-G s nasobkem nad 2x - RTX 50; "
        "helper hlasi nasobek az %ux) - nahradni rezim: %s",
        g_sh->fgMultMax, FgModeName(to));
    Present_FgSetMode(to);
    InterlockedExchange(&g_fgSubstNote, to + 1);
}

// Cil vystupu po rezerve limiteru: auto = minus [fg] LimiterMargin (presenter ma strop f - f^2/3600, pod nim nesmi nic
// chybet), rucni cil (v103h) presne.
double FgTgtEff() { return cfg.fgTargetFps > 0 ? g_fgTargetD : g_fgTargetD * (1.0 - cfg.fgMarginPct / 100.0); }

// Adaptivni FSR: nastaveni pro helper (cil vystupu po rezerve LimiterMargin, frekvence, hranice, zamek, strop)
static double g_adOutReg;  // v103h: efektivni cil, kdyz na zadany nestaci ani 6x (FgAdaptLock; 0 = zadany cil)
static double g_adOutCap;  // AdaptMinFps / AdaptLowMult: cil vystupu adaptivnich rezimu pri pomale hre (0 = bez omezeni)
void FgPublishAdapt() {
    if (!g_sh || !cfg.inMain) return;
    double tgt = FgTgtEff();
    if (g_adOutReg > 0.0 && g_adOutReg < tgt) tgt = g_adOutReg;
    if (g_adOutCap > 0.0 && g_adOutCap < tgt) tgt = g_adOutCap;
    g_sh->adaptTargetMilli = tgt > 0.0 ? (uint32_t)(tgt * 1000.0) : 0u;
    g_sh->refreshMilli = g_fgHz > 1 ? (uint32_t)g_fgHz * 1000u : 0u;
    g_sh->outCapMilli = (uint32_t)(g_fgOutCap * 1000.0);
    g_sh->gpQueue = g_fgFree ? 1u : 0u;
    g_sh->adaptFillMilli = cfg.adaptFill > 0 ? (uint32_t)cfg.adaptFill * 1000u : 0u;
    g_sh->adaptLock = g_adLockWord;  // zlomkovy zamek (stupen voli limiter, FgAdaptLock)
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
    const double vrrCap = (double)hz - (double)hz * hz / 3600.0;  // strop VRR
    double t = cfg.fgTargetFps > 0 ? (double)cfg.fgTargetFps : vrrCap;
    // Rucni cil (v103h) plati presne, i nad frekvenci monitoru. Presenter pak strop VRR nema (jinak by zahazoval skutecne
    // snimky, v102u) a obrazy zobrazuje po poradi; nejkratsi interval je jen pojistka proti dvojici obrazu. Rucni cil
    // pod stropem VRR a auto: strop VRR jako driv.
    // ([debug] SelfCheckTest bit 0: rucni cil nad stropem, ale presenter se stropem - stav pred v102u, overeni samokontrol)
    const bool over = cfg.fgTargetFps > 0 && t > 0.99 * vrrCap;
    const bool freeOut = over && !(g_scTest & 1);
    const double outCap = freeOut ? 2.0 * t : hzKnown && hz >= 30 ? vrrCap : 0.0;
    if (t < 20.0) t = 20.0;
    const uint32_t ti = (uint32_t)floor(t + 1e-6);
    const bool capCh = fabs(outCap - g_fgOutCap) > 1e-6 || freeOut != g_fgFree;
    g_fgOutCap = outCap;
    g_fgFree = freeOut;
    if (fabs(t - g_fgTargetD) > 1e-6 || capCh) {
        g_fgTargetD = t;
        g_fgTarget = ti;
        if (over)
            Log("prezentace: FG cil vystupu %u FPS ([fg] TargetFps, presne; nad stropem VRR %.1f FPS %d Hz monitoru - obraz "
                "se muze trhat%s), skutecnych snimku hry nejvys %.1f FPS (nasobek %ldx)",
                ti, vrrCap, g_fgHz, freeOut ? "" : "; TEST: presenter se stropem", t / (double)g_fgMult, g_fgMult);
        else if (cfg.fgTargetFps > 0)
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

// v103g: diagnosticky prekryv ([menu] DiagOverlay) - casy obrazu na monitoru (presenter FG, vlakno presenteru) a Present
// hry (vlakno hry). Kazdy kruh ma jednoho zapisovatele; ctenar (vlakno hry) jen kopiruje.
static const unsigned kDgRing = 8192;  // ~2 s i pri vysokem cili vystupu (az ~4000 obrazu/s)
struct DgRing {
    LONGLONG t[kDgRing];
    volatile LONG n;
};
static DgRing g_dgMon, g_dgGame;
static volatile bool g_dgOn;
static void DgPush(DgRing& r, LONGLONG t) {
    const LONG i = r.n;
    r.t[(unsigned)i % kDgRing] = t;
    MemoryBarrier();
    r.n = i + 1;
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

bool FinishSlot(int i, uint64_t ns, VkResult* res = nullptr) {
    if (res) *res = VK_SUCCESS;
    if (g_fenceUsed[i]) {
        const VkResult r = vk.vkWaitForFences(g_dev, 1, &g_fence[i], VK_TRUE, ns);
        if (res) *res = r;
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
    g_fbRetry = false;
    if (g_state != PS_FALLBACK && cfg.inMain && g_fgMode != BMSD_FG_OFF) InterlockedExchange(&g_fbNote, 1);
    StopHelper(1000);
    GpStop("zaloha");  // swapchain proxy pryc pred znicenim prezentacniho okna
    g_gpActive = false;
    g_state = PS_FALLBACK;
    g_offShrink = 0;
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
    // [fg] OffWindow (vychozi): DXVK jako s vypnutou FG do dcerineho okna proxy pres celou klientskou plochu (flip bez
    // kompozice; primo do okna hry jde DXVK jen pres kompozici DWM, v102v). Dcerine okno mohlo zustat 1x1 a skryte
    // (zmenseni pri startu presenteru) - SyncChild ho vrati na celou plochu. Sonda okna hry se nepousti.
    g_fbOffWin = false;
    if (cfg.offChild && g_vkReady && EnsureWndThread()) {
        if (!g_child) g_child = (HWND)SendMessageW(g_ctl, WMP_CHILD, (WPARAM)g_gameWnd, 0);
        if (g_child) {
            g_fbOffWin = true;
            g_probeGaveUp = true;
            SyncChild(g_child);
            ShowWindowAsync(g_child, SW_SHOWNA);
            Log("prezentace: ZALOHA (%s) - DXVK prezentuje do dcerineho okna proxy (jako s vypnutou FG)", reason);
        }
    }
    // Okno hry (zadny swapchain na nem nebyl): vyzkouset - volne -> DXVK primo; obsazene -> dcerine okno a sonda.
    // (DXVK uz do okna hry prezentoval - FG byla za behu vypnuta: swapchain na nem drzi DXVK sam, sonda by selhala.)
    const bool free = !g_fbOffWin && (g_dxvkOnGame || (g_vkReady && ProbeWindow(g_gameWnd)));
    if (g_fbOffWin) {
    } else if (free || !g_vkReady) {
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

// Zaloha z pomalosti (helper zije): jde zkusit znovu - rucne v menu (g_fbRetry) a automaticky s rostoucim odstupem.
void FallbackRetry(const char* reason) {
    Fallback(reason);
    if (!cfg.inMain || !g_proc || WaitForSingleObject(g_proc, 0) != WAIT_TIMEOUT) return;
    g_fbRetry = true;
    InterlockedCompareExchange(&g_fbNote, 2, 1);
    if (g_fbAutoN < kFbAutoMax) {
        ULONGLONG wait = 5000;
        for (unsigned i = 0; i < g_fbAutoN; ++i) wait *= 3;
        g_fbAutoAt = GetTickCount64() + wait;
        Log("prezentace: zaloha z pomalosti (helper zije) - novy pokus o FG automaticky za %llu s (%u/%u); jde i rucne "
            "vypnutim a zapnutim FG v menu", wait / 1000ull, g_fbAutoN + 1, kFbAutoMax);
    } else {
        g_fbAutoAt = 0;
        Log("prezentace: automaticke pokusy o FG vycerpany (%u) - FG jde zkusit znovu vypnutim a zapnutim v menu",
            kFbAutoMax);
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
bool DxSwapOn() { return g_dxFse == 1 && !cfg.offChild; }
bool PresenterWanted() { return !cfg.inMain || cfg.fgForce || g_fgMode != BMSD_FG_OFF; }

// Cil DXVK s vypnutym presenterem: okno hry (nebo s [fg] OffTarget=1 dcerine okno proxy pres jeho klientskou plochu)
HWND OffTarget() {
    if (g_direct) {
        g_dxvkOnGame = true;
        return g_gameWnd;
    }
    if (g_child) SyncChild(g_child);
    InterlockedIncrement(&g_atFrames);
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
    g_offShrink = 0;
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
        // Prezentace helperu skoncila chybou (znicene prezentacni okno, nebo od v103c i vlastni chyba presenteru, napr.
        // GPU nedobehla na pomalem PC): helper novy start prijme (restartSeq); skutecna chyba pak jen znovu selze
        // a pocet automatickych pokusu je omezeny (FallbackRetry)
        Log("prezentace: helper skoncil chybou (%s) - zapinam znovu", g_sh->helperMsg);
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
    g_startChk = g_startPaused = 0;
    if (!g_vkReady && !InitVk(d)) return Fallback("Vulkan DXVK nedostupny"), false;
    if (d != g_vkDev) return Fallback("nove hlavni zarizeni"), false;
    if (cfg.target == 1)
        g_presWnd = g_gameWnd;  // primo okno hry (nejmensi latence; tvrdy konec helperu ho ale "zamrazi")
    else if (EnsureWndThread())
        g_presWnd = (HWND)SendMessageW(g_ctl, WMP_CHILD, (WPARAM)g_gameWnd, 0);
    if (!g_presWnd) return Fallback("prezentacni okno helperu nevzniklo"), false;
    const bool shrink = g_child && g_state == PS_OFF && cfg.offOwn;
    if (shrink)
        g_offShrink = 1;  // okno zatim viditelne (DXVK do nej do prvniho snimku presenteru), pak zmensit (Present_OnPresent)
    else if (g_child)
        ShowWindowAsync(g_child, SW_HIDE);  // dcerine okno DXVK z PS_OFF ([fg] OffTarget=1)
    // Zobrazeni v procesu hry (VRR): vlakno presenteru proxy na prezentacnim okne; helper jen zverejnuje obrazy
    g_gpRefreshMs = shrink ? (cfg.liveRefreshMs ? cfg.liveRefreshMs : 1500)
                    : g_state == PS_OFF && cfg.target != 1 ? cfg.liveRefreshMs : 0;
    g_gpRefreshAt = 0;
    g_gpRefreshStage = 0;
    // OffWindow: swapchain presenteru jednou znovu, az okno DXVK uvolni flip (GpRefreshKick); LiveRefreshMs je pojistka
    g_gpRefreshMode = shrink ? 1u : cfg.liveRefreshMode;
    // bit 4: okno hry z iFlip DWM vyhodit hned (pred prvnim swapchainem presenteru) - jedno bliknuti pri prepnuti.
    // S OffWindow netreba: DXVK do okna hry nikdy neprezentoval.
    if (!shrink && g_state == PS_OFF && cfg.target != 1 && (cfg.liveRefreshMode & 4) && g_ctl && IsWindow(g_gameWnd))
        PostMessageW(g_ctl, WMP_UNSTICK, (WPARAM)g_gameWnd, 0);  // ne Send: vlakno oken saha na okno hry -> deadlock
    InterlockedExchange(&g_gpRefreshKick, 0);
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
    {  // v103f3: zpetny tlak (jen skutecne s preferenci) - predchozi snimek musi byt zverejnen helperem a prevzat presenterem
        static uint64_t s_pubAtPost;
        if (g_bpWant && g_gpActive) {
            const LONGLONG t0 = Qpc();
            const double lim = g_bpMaxMs;
            for (;;) {
                MemoryBarrier();
                const uint64_t pub = (uint64_t)InterlockedCompareExchange64((volatile LONG64*)&g_sh->gpPub, 0, 0) & 0xFFFFFFFFFFFFull;
                if ((pub > s_pubAtPost && g_gpLastSeq >= pub) || Ms(t0, Qpc()) > lim) break;
                Sleep(0);
            }
            const double w = Ms(t0, Qpc());
            g_bpLastMs = w;
            g_stBp.add(w);
            if (w > lim) ++g_stBpTo;
        }
        if (!(g_bpWant && g_gpActive)) g_bpLastMs = 0.0;
        s_pubAtPost = (uint64_t)InterlockedCompareExchange64((volatile LONG64*)&g_sh->gpPub, 0, 0) & 0xFFFFFFFFFFFFull;
    }
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
    // Kopie pro helper ze stejneho slotu jeste bezi: na pomalem PC (strankovani VRAM pri nacitani) GPU smi stat i
    // sekundy. Vlakno hry ceka nejvys 250 ms, pak snimek vynecha a zkusi to pri dalsim; zaloha az po kSlotStallMs bez
    // dokonceni (opakovatelna), hned jen pri ztrate zarizeni.
    VkResult fr = VK_SUCCESS;
    if (!FinishSlot(k, 250000000ull, &fr)) {
        bb->Release();
        const ULONGLONG nowS = GetTickCount64();
        if (fr == VK_TIMEOUT && !g_slotStall) {
            g_slotStall = nowS;
            Log("prezentace: WARN - GPU zatim nedokoncila kopii pro helper (250 ms) - snimky vynechavam, cekam (pomale "
                "GPU / nacitani?)");
        }
        if (fr != VK_TIMEOUT) {
            Fallback("GPU nedokoncila kopii pro helper (chyba zarizeni)");
        } else if (nowS - g_slotStall > kSlotStallMs) {
            g_slotStall = 0;
            FallbackRetry("GPU nedokoncila kopii pro helper ani za 20 s");
        } else {
            ++g_stDropped;
        }
        return false;
    }
    if (g_slotStall) {
        Log("prezentace: kopie pro helper po %.1f s dokoncena - pokracuji", (GetTickCount64() - g_slotStall) / 1000.0);
        g_slotStall = 0;
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
               !(cfg.adaptIdleSkip && FgIsAdapt((int)g_fgMode) && !g_sh->adaptEngaged)) {
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
        const LONGLONG pq = Qpc();
        InterlockedExchange64((volatile LONG64*)&g_sh->postQpc, pq);
        LcSetPost(pq);
    } else {
        g_sh->frameValue = N;
        const LONGLONG pq = Qpc();
        g_sh->postQpc = (uint64_t)pq;
        MemoryBarrier();
        LcSetPost(pq);
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
        FallbackRetry(keep);
        return false;
    }
    if (g_gpActive && g_gpFailed) {
        static char keepGp[220];
        snprintf(keepGp, sizeof keepGp, "zobrazeni v procesu hry: %s", g_gpWhy);
        FallbackRetry(keepGp);
        return false;
    }
    if (g_state == PS_STARTING) {
        // (hlavni helper s minimalizovanym oknem hry: pripojeny a potvrzuje snimky = nastartoval, jen nema kam kreslit)
        // Zobrazeni v procesu hry: navic swapchain proxy (gpState), jinak by se cekalo na obsazene okno do zalohy
        const int32_t gst = g_gpActive ? g_sh->gpState : BMSD_PST_PRESENTING;
        const bool gpOk = gst == BMSD_PST_PRESENTING || gst == BMSD_PST_NOWINDOW;
        if ((st == BMSD_PST_PRESENTING || (cfg.inMain && st == BMSD_PST_NOWINDOW)) && gpOk) {
            g_state = PS_HELPER;
            Log("prezentace: helper prezentuje do okna hry (start %.2f s, z toho limit stal %.2f s, zobrazuje %s)",
                (now - g_startTick) / 1000.0, g_startPaused / 1000.0, g_gpActive ? "proces hry" : "helper");
        } else {
            // Limit stoji, kdyz hra Present nevola nebo vola ridce (nacitani mapy: mezera > 0,2 s - i nejpomalejsi
            // hratelny snimek je kratsi) a kdyz helper hlasi dlouhou praci hlavniho vlakna (vytvoreni generatoru se
            // studenou cache, helperBusyTick obnovovany po dobu prace)
            const ULONGLONG gap = g_startChk ? now - g_startChk : 0;
            if (gap > 200 || (cfg.inMain && Interop_HelperBusy())) g_startPaused += gap;
            g_startChk = now;
            if (now - g_startTick - g_startPaused > cfg.startTimeoutMs) {
                const bool inUse = st == BMSD_PST_INUSE || gst == BMSD_PST_INUSE;
                // Helper zije (jen pomaly start): novy pokus automaticky s odstupem i rucne v menu (FallbackRetry)
                if (inUse) Fallback("okno hry je obsazene (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)");
                else FallbackRetry("helper did not start presenting in time");
                return false;
            }
        }
    }
    // Nereaguje: nejaky snimek ceka na prevzeti a helper uz hangMs nic nepotvrdil (prevezme kazdy snimek do ms,
    // i bez okna). Hodiny bezi jen, kdyz neco ceka.
    if (cfg.testStopSec && g_state == PS_HELPER && now - g_startTick > cfg.testStopSec * 1000ull) {
        Fallback("test: [present] TestStopSec - helper pozadan o cisty konec");
        return false;
    }
    const uint64_t ack = g_sh->helperAck;
    // Doba, kdy hra Present nevolala (nacitani, mezera nad 0,5 s), se do limitu nepocita
    const ULONGLONG hgap = g_hangChk ? now - g_hangChk : 0;
    g_hangChk = now;
    if (hgap > 500) g_ackTick = g_ackTick + hgap < now ? g_ackTick + hgap : now;
    // Helper hlasi dlouhou praci hlavniho vlakna (helperBusyTick, obnovovany): hodiny "nereaguje" stoji
    if (ack != g_lastAck || ack >= g_N || (cfg.inMain && Interop_HelperBusy())) {
        g_lastAck = ack;
        g_ackTick = now;
    } else if (now - g_ackTick > cfg.hangMs) {
        FallbackRetry("helper nepotvrzuje snimky (nereaguje)");
        return false;
    }
    // Presenter bezi 120 s bez zalohy: automaticke pokusy zase od zacatku
    if (g_fbAutoN && g_state == PS_HELPER && now - g_startTick > 120000) {
        Log("prezentace: FG bezi %llu s po automatickem pokusu - pocitadlo pokusu vynulovano", (now - g_startTick) / 1000ull);
        g_fbAutoN = 0;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Samokontroly ([debug] SelfCheck=1, vychozi). Kazdych ~5 s (s radkem "prezentace") se hodnoty, ktere mod uz ma,
// porovnaji s tim, co ma platit. Poruseni = jeden radek "KONTROLA: <co> | <cisla>" v logu; stejna kontrola nejvys
// jednou za 60 s (s poctem potlacenych), na obrazovce nic. Duvod: chyby, ktere pocitadlo FPS neukaze (v102u: strop nad
// frekvenci monitoru - helper zahazoval pres polovinu skutecnych snimku a pohyb sekal; v102v: bez FG slo zobrazeni
// pres kompozici DWM).
// Kontroly nad statistikou helperu se pocitaji jen v klidnem intervalu (hra v popredi, zadny snimek delsi nez 300 ms,
// beze zmeny rezimu / nasobku / swapchainu, mimo ochranu po Alt+Tabu) a hlasi se az pri druhem poruseni po sobe
// (~10 s): okna statistiky helperu a proxy se nekryji a prechody (zapnuti FG, Alt+Tab, nacitani) invarianty kratce
// porusuji. Cena: par porovnani za snimek, dva dotazy jadru grafiky a nekolik dotazu na okno jednou za 5 s.
enum ScId {
    SC_DROP,      // helper prevzal vic skutecnych snimku, nez zobrazil
    SC_HANDOFF,   // proxy nepredala helperu vsechny snimky hry
    SC_GPSKIP,    // presenter v procesu hry preskakuje obrazy
    SC_GENMISS,   // generovane snimky vypadavaji
    SC_ADSLOT,    // adaptivni rezimy: sloty mrizky bez obrazu
    SC_COMPOSED,  // zobrazeni pres kompozici DWM misto flipu
    SC_CAP,       // strop limiteru nad stropem zobrazeni / nasobek
    SC_OUTCAP,    // vystup nad stropem zobrazeni
    SC_OVERLIM,   // hra bezi nad stropem limiteru
    SC_USELESS,   // adaptivni rezim generuje zbytecne
    SC_LOCKLOW,   // zamek dlouho na nizkem stupni
    SC_LOWFPS,    // hra pod 40 FPS, bez generovani zvladala vic
    SC_LOWBASE,   // velky nasobek z nizkeho zakladu
    SC_NOTURN,    // mys ve hre hybe, kamera se neotaci
    SC_INACTIVE,  // okno hry v popredi, ale hra dostala WM_ACTIVATEAPP(FALSE)
    SC_PREFGEN,   // preference: generuje, i kdyz podle profilu nema (fgpref.inl)
    SC_PREFFLAP,  // preference: prepinani generuje <-> jen skutecne > 6 / min mimo udalosti (fgpref.inl)
    SC_ALTTAB,    // hra minimalizovana, ale jeji obraz dal drzi vystup monitoru (alttab.inl)
    SC_COUNT
};
struct ScSt {
    ULONGLONG last;  // GetTickCount64 posledniho hlaseni (0 = zadne)
    unsigned suppressed, streak;
};
ScSt g_sc[SC_COUNT];
// (g_scOn = [debug] SelfCheck a g_scTest = [debug] SelfCheckTest jsou u konfigurace)
// Cisla, ktera limiter a zamek zapisuji kazdy snimek (jen prirazeni / porovnani); interval nuluje SelfCheck
double g_scMaxFrameMs;     // nejdelsi interval mezi Present hry v intervalu
LONGLONG g_scPrevPresQ;
unsigned g_scLimN;         // snimku intervalu, ve kterych limiter bezel
double g_scLimRateMax;     // nejvyssi strop limiteru v intervalu (skutecnych FPS)
double g_scLimOutMax;      // nejvyssi strop limiteru x nasobek v intervalu (FPS vystupu)
// Zamek na nizkem stupni (zapisuje FgAdaptLock): od kdy (QPC, 0 = ne) a posledni hodnoty pro hlaseni
LONGLONG g_scLockLowQ, g_scLockRoomQ;  // stupen <= 1/3; a navic hra i GPU hotove do 60 % intervalu stupne
int g_scLockQ, g_scLockP;
double g_scLockRate, g_scLockBusy, g_scLockGpu;
// Ochrana po Alt+Tabu (FgResumeGuard); plati jen kdyz limiter bezi
bool g_rgBg;               // okno hry neni v popredi
bool g_rgProt;             // ochranna doba po navratu
double g_limRateLast;      // strop limiteru v poslednim snimku (skutecnych FPS, pro log)

// Poruseni v "need" vyhodnocenich po sobe
bool ScStreak(ScId id, bool bad, unsigned need = 2) {
    g_sc[id].streak = bad ? g_sc[id].streak + 1 : 0;
    return g_sc[id].streak >= need;
}
void ScReport(ScId id, const char* fmt, ...) {
    ScSt& s = g_sc[id];
    const ULONGLONG now = GetTickCount64();
    if (s.last && now - s.last < 60000) {
        ++s.suppressed;
        return;
    }
    char b[720];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (s.suppressed) Log("KONTROLA: %s (od minuleho hlaseni potlaceno %u x)", b, s.suppressed);
    else Log("KONTROLA: %s", b);
    s.last = now;
    s.suppressed = 0;
}

// Cesta zobrazeni: citace jadra grafiky (D3DKMTQueryStatistics) - snimky, ktere na vystup monitoru poslal proces hry,
// a vsechny snimky na tomtez vystupu. Flip (PresentMon "Hardware: Legacy Flip"): skoro vsechny jsou od procesu hry.
// Kompozice ("Composed: Copy with GPU GDI"): ke kazdemu snimku hry pribude snimek spravce oken (DWM), "ostatni" jsou
// pak ~ FPS hry, nejvys frekvence monitoru. Mereno 2026-11-23 (RTX 5080, Windows 11 26200, 165 Hz): flip 332 z 332
// snimku za sekundu od hry, kompozice 165 z 330. Funkce jsou v gdi32 a hledaji se za behu; kdyz chybi nebo selzou,
// cesta se nezjistuje. Nerozlisi "Legacy Copy to front buffer" od flipu (oboji posila proces hry sam).
struct ScKmt {
    bool tried, ok;
    unsigned fails;
    NTSTATUS(APIENTRY* open)(D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME*);
    NTSTATUS(APIENTRY* close)(const D3DKMT_CLOSEADAPTER*);
    NTSTATUS(APIENTRY* query)(const D3DKMT_QUERYSTATISTICS*);
    HMONITOR mon;
    LUID luid;
    UINT src;
    int hz;
    bool have;  // predchozi vzorek plati
    ULONG proc, glob;
    LONGLONG q;
} g_scKmt;
bool g_scPathOk;                  // posledni vzorek cesty zobrazeni plati
double g_scProcPs, g_scOtherPs;   // snimku za sekundu na vystupu monitoru: od procesu hry / od ostatnich (DWM)

void ScSamplePath() {
    ScKmt& k = g_scKmt;
    g_scPathOk = false;
    if (!g_scOn && !cfg.healComposed) return;  // cestu potrebuje i ScHealComposed
    if (!k.tried) {
        k.tried = true;
        if (HMODULE g = GetModuleHandleW(L"gdi32.dll")) {
            k.open = (decltype(k.open))(void*)GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName");
            k.close = (decltype(k.close))(void*)GetProcAddress(g, "D3DKMTCloseAdapter");
            k.query = (decltype(k.query))(void*)GetProcAddress(g, "D3DKMTQueryStatistics");
        }
        k.ok = k.open && k.close && k.query;
    }
    if (!k.ok || !g_gameWnd) return;
    auto fail = [&k]() {
        k.have = false;
        k.mon = nullptr;
        if (++k.fails >= 5) k.ok = false;  // system to neumi: dal se neptat
    };
    const HMONITOR mon = MonitorFromWindow(g_gameWnd, MONITOR_DEFAULTTONEAREST);
    if (!mon) return;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) return;
    if (mon != k.mon) {
        k.have = false;
        D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME oa = {};
        wcsncpy(oa.DeviceName, mi.szDevice, 31);
        if (k.open(&oa) < 0) return fail();
        D3DKMT_CLOSEADAPTER ca = {};
        ca.hAdapter = oa.hAdapter;
        k.close(&ca);
        k.luid = oa.AdapterLuid;
        k.src = oa.VidPnSourceId;
        k.mon = mon;
    }
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    k.hz = EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
               ? (int)dm.dmDisplayFrequency : 0;
    static D3DKMT_QUERYSTATISTICS qp, qg;  // ~800 B kazda (jen vlakno hry)
    memset(&qp, 0, sizeof qp);
    qp.Type = D3DKMT_QUERYSTATISTICS_PROCESS_VIDPNSOURCE;
    qp.AdapterLuid = k.luid;
    qp.hProcess = GetCurrentProcess();
    qp.QueryProcessVidPnSource.VidPnSourceId = k.src;
    memset(&qg, 0, sizeof qg);
    qg.Type = D3DKMT_QUERYSTATISTICS_VIDPNSOURCE;
    qg.AdapterLuid = k.luid;
    qg.QueryVidPnSource.VidPnSourceId = k.src;
    if (k.query(&qp) < 0 || k.query(&qg) < 0) return fail();
    k.fails = 0;
    const ULONG pf = qp.QueryResult.ProcessVidPnSourceInformation.Frame;
    const ULONG gf = qg.QueryResult.VidPnSourceInformation.GlobalInformation.Frame;
    const LONGLONG now = Qpc();
    if (k.have) {
        const double dt = Ms(k.q, now) / 1000.0;
        if (dt >= 2.0 && dt <= 30.0) {
            const double p = (double)(ULONG)(pf - k.proc) / dt, a = (double)(ULONG)(gf - k.glob) / dt;
            g_scProcPs = p;
            g_scOtherPs = a > p ? a - p : 0.0;
            g_scPathOk = true;
        }
    }
    k.have = true;
    k.proc = pf;
    k.glob = gf;
    k.q = now;
}
// Kompozice podle posledniho vzorku: "ostatni" aspon 40 % toho, co posila hra (nejvys frekvence monitoru), a aspon 20/s
bool ScPathComposed() {
    const double hz = g_scKmt.hz > 1 ? (double)g_scKmt.hz : 60.0;
    return g_scPathOk && g_scProcPs >= 30.0 && g_scOtherPs >= std::max(20.0, 0.4 * std::min(g_scProcPs, hz));
}
// Pripona radku "prezentace": co rika jadro grafiky o ceste zobrazeni
const char* ScPathText(char* b, size_t n) {
    if (!g_scPathOk || g_scProcPs < 30.0) return "";
    snprintf(b, n, " | vystup monitoru: od hry %.0f/s, od ostatnich %.0f/s (%s)", g_scProcPs, g_scOtherPs,
             ScPathComposed() ? "kompozice DWM" : "bez kompozice");
    return b;
}
bool ScForeground() {
    DWORD pid = 0;
    const HWND fw = GetForegroundWindow();
    if (fw) GetWindowThreadProcessId(fw, &pid);
    return fw && pid == GetCurrentProcessId();
}
// Klientska plocha okna hry zabira cely monitor (jinak je kompozice v poradku: okno na plose)
bool ScWindowCoversMonitor() {
    if (!g_gameWnd || IsIconic(g_gameWnd)) return false;
    const HMONITOR mon = MonitorFromWindow(g_gameWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    RECT rc = {};
    POINT p = {0, 0};
    if (!mon || !GetMonitorInfoW(mon, &mi) || !GetClientRect(g_gameWnd, &rc) || !ClientToScreen(g_gameWnd, &p)) return false;
    return p.x <= mi.rcMonitor.left && p.y <= mi.rcMonitor.top && p.x + rc.right >= mi.rcMonitor.right &&
           p.y + rc.bottom >= mi.rcMonitor.bottom;
}

// Vyhodnoceni jednou za interval statistiky (el = delka intervalu proxy v ms; statFresh = helper od minula zapsal
// novou statistiku). Vola LogStats pred nulovanim citacu intervalu.
// Presenter FG v procesu hry jde pres kompozici DWM, ackoli by mel flipovat (okno hry v popredi pres cely monitor).
// Stava se, kdyz swapchain presenteru vznikl v dobe, kdy okno hry v popredi nebylo (hra spustena na pozadi): ovladac
// ho pak v kompozici nechava ("Composed: Copy with GPU GDI", vkQueuePresent ~6,5 ms, zobrazeno ~100/s, presenter
// preskakuje obrazy a hra s DLSS-G adaptivnim jen ~62 FPS; 2026-11-23 w_dga_1, az novy swapchain po Alt+Tabu byl
// "Hardware: Legacy Flip"). Prvni takovy interval (~5 s, okno hry v popredi uz i v tom minulem) -> swapchain
// presenteru i s povrchem znovu (g_gpRecreateReq), nejvys 3x, pak az po dalsi ztrate popredi. Kontrola SC_COMPOSED
// pak 2 intervaly nehlasi (prechod). Bez ohledu na [debug] SelfCheck. [fg] HealComposed=0 vypne.
unsigned g_scHealSkip;  // SelfCheck: tolik dalsich intervalu bez kontroly cesty zobrazeni (po obnove swapchainu)
void ScHealComposed() {
    static unsigned s_tries;
    static bool s_fore;
    if (!cfg.healComposed) return;
    const bool fore = ScForeground(), wasFore = s_fore;
    s_fore = fore;
    if (!fore) s_tries = 0;
    const bool pending = g_gpRefreshAt && g_gpRefreshAt != ~0ull;  // obnova po zapnuti FG za behu jeste nebyla
    const bool bad = fore && wasFore && cfg.inMain && g_sh && g_state == PS_HELPER && g_gpActive && !pending &&
                     g_scMaxFrameMs < 300.0 && ScWindowCoversMonitor() && ScPathComposed();
    if (!bad || s_tries >= 3) return;
    ++s_tries;
    g_scHealSkip = 2;
    InterlockedExchange(&g_gpRecreateReq, 1);
    Log("prezentace: zobrazeni FG jde pres kompozici DWM (od hry %.0f/s, od ostatnich %.0f/s), okno hry v popredi pres "
        "cely monitor - swapchain presenteru znovu (pokus %u z 3)",
        g_scProcPs, g_scOtherPs, s_tries);
}

static void FgPrefSelfCheck();  // src/fgpref.inl
void SelfCheck(double el, bool statFresh) {
    if (!g_scOn || el <= 0.0) return;
    FgPrefSelfCheck();
    static bool s_fore, s_cover;
    static unsigned s_calm, s_switches;
    static int s_state = -1;
    static LONG s_mode;
    static int32_t s_fgState;
    static uint32_t s_actMode, s_actMult, s_gpSkipped;
    static double s_refFps;       // FPS hry v poslednim klidnem intervalu bez generovani
    static ULONGLONG s_refTick;   // a kdy to bylo (GetTickCount64)
    const ULONGLONG tick = GetTickCount64();
    const double sec = el / 1000.0, fps = g_stPresents / sec;
    const bool fore = ScForeground(), cover = ScWindowCoversMonitor();
    const bool helper = cfg.inMain && g_sh && g_state == PS_HELPER;
    MemoryBarrier();
    const int32_t fgState = helper ? g_sh->fgState : (int32_t)BMSD_FGS_OFF;
    const uint32_t actMode = helper ? g_sh->fgActiveMode : 0u, actMult = helper ? g_sh->fgActiveMult : 0u;
    const bool limOn = g_stPresents > 0 && g_scLimN * 10u >= g_stPresents * 9u;
    const bool same = (int)g_state == s_state && g_fgMode == s_mode && fgState == s_fgState && actMode == s_actMode &&
                      actMult == s_actMult && g_switches == s_switches;
    const bool calm = fore && s_fore && same && g_scMaxFrameMs < 300.0 && !g_fgPause && !(limOn && (g_rgBg || g_rgProt));
    s_calm = calm ? s_calm + 1 : 0;
    const bool adapt = FgIsAdapt((int)g_fgMode);
    const int hz = g_scKmt.hz > 1 ? g_scKmt.hz : g_fgHz;
    double vrrCap = hz > 1 ? (double)hz - (double)hz * hz / 3600.0 : 0.0;  // strop zobrazeni helperu
    // rucni cil nad stropem VRR (v103h) je zamer: hlida se az prekroceni cile
    if (g_fgFree && g_fgTargetD > vrrCap) vrrCap = g_fgTargetD;

    // 2. Cesta zobrazeni: flip, ne kompozice (bez FG kresli DXVK, s FG presenter v procesu hry)
    const bool healSkip = g_scHealSkip > 0;  // swapchain presenteru se prave obnovuje (ScHealComposed)
    if (g_scHealSkip) --g_scHealSkip;
    if (ScStreak(SC_COMPOSED, calm && !healSkip && cover && s_cover && (helper || g_dxFse == 1) && ScPathComposed()))
        ScReport(SC_COMPOSED, "zobrazeni jde pres kompozici DWM, ne flipem | na vystup monitoru od hry %.0f snimku/s, od "
                 "ostatnich (DWM) %.0f/s; monitor %d Hz, hra %.1f FPS, FG %s, zobrazuje %s",
                 g_scProcPs, g_scOtherPs, hz, fps, helper ? FgModeName((int)actMode) : "vypnuta",
                 helper ? (g_gpActive ? "presenter v procesu hry" : "helper")
                        : g_direct ? "DXVK primo do okna hry" : "DXVK do dcerineho okna");

    // 7. Mys a kamera (v102x, chyba uzivatele 2026-11-23 23:16: po Alt+Tabu mys neotacela kamerou, hra ~33 FPS).
    // Proxy pocita raw input mysi, ktery si vlakno hry vyzvedlo (WH_GETMESSAGE), a otaceni kamery hlavniho pohledu
    // (cViewProj mezi snimky). Hlasi se: (a) okno hry v popredi, kurzor skryty (hra ma mys, ne menu hry ani konzole),
    // menu modu zavrene, kamera nalezena v aspon polovine snimku, pohyb mysi >= 500 bodu za interval a kamera
    // otocena o < 0,5 st - dvakrat po sobe; (b) okno hry je v popredi, ale posledni WM_ACTIVATEAPP, ktery okno hry
    // dostalo, rikal "neaktivni" (Source pak ignoruje mys a spi engine_no_focus_sleep) - dvakrat po sobe.
    {
        static double s_turn;
        static unsigned s_camF;
        static uint64_t s_dist;
        static bool s_have;
        double turn = 0.0;
        unsigned camF = 0;
        uint64_t dist = 0;
        bool hooked = false;
        int appAct = -1;
        Proxy_CamInputTotals(&turn, &camF, &dist, &hooked, &appAct);
        const double dTurn = turn - s_turn;
        const unsigned dCam = camF - s_camF;
        const uint64_t dDist = dist - s_dist;
        const bool have = s_have;
        s_turn = turn;
        s_camF = camF;
        s_dist = dist;
        s_have = true;
        CURSORINFO ci = {sizeof ci};
        const bool curHidden = GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING);
        const bool gameFore = g_gameWnd && GetForegroundWindow() == g_gameWnd;
        if (ScStreak(SC_NOTURN, have && hooked && gameFore && s_fore && curHidden && !g_menuOpenNow &&
                                    g_scMaxFrameMs < 300.0 && dCam * 2u >= g_stPresents && g_stPresents >= 50 &&
                                    dDist >= 500 && dTurn < 0.5))
            ScReport(SC_NOTURN, "mys ve hre hybe, ale kamera se neotaci | pohyb mysi %llu bodu, otoceni kamery %.2f st za "
                     "%.1f s, kurzor skryty, hra aktivni (WM_ACTIVATEAPP) %s, hra %.1f FPS, FG %s",
                     (unsigned long long)dDist, dTurn, sec, appAct < 0 ? "?" : appAct ? "ano" : "NE", fps,
                     helper ? FgModeName((int)actMode) : "vypnuta");
        if (ScStreak(SC_INACTIVE, hooked && gameFore && s_fore && appAct == 0))
            ScReport(SC_INACTIVE, "okno hry je v popredi, ale hra se povazuje za neaktivni | posledni WM_ACTIVATEAPP okna hry "
                     "= FALSE, hra %.1f FPS, nejdelsi snimek %.0f ms, pohyb mysi %llu bodu, otoceni %.2f st",
                     fps, g_scMaxFrameMs, (unsigned long long)dDist, dTurn);
    }

    if (helper) {
        // 3. Strop: limiter skutecnych snimku x nasobek nejvys strop zobrazeni (+1 %)
        if (limOn && vrrCap > 0.0 && g_scLimOutMax > vrrCap * 1.01)
            ScReport(SC_CAP, "strop limiteru je nad stropem zobrazeni / cilem | limiter %.1f skutecnych FPS, s nasobkem %.1f FPS "
                     "vystupu > strop zobrazeni / cil %.1f FPS (monitor %d Hz); rezim %s, [fg] TargetFps %d",
                     g_scLimRateMax, g_scLimOutMax, vrrCap, hz, FgModeName((int)g_fgMode), cfg.fgTargetFps);
        // Hra nema bezet rychleji, nez dovoli limiter
        if (ScStreak(SC_OVERLIM, calm && limOn && g_scLimRateMax > 0.0 && fps > g_scLimRateMax * 1.03 + 0.5))
            ScReport(SC_OVERLIM, "hra bezi nad stropem limiteru | hra %.1f FPS, strop limiteru %.1f FPS; limiter spal %u x "
                     "z %u snimku (pozde %u, novy slot %u)",
                     fps, g_scLimRateMax, g_stLimSleep.n, g_stPresents, g_stLimLate, g_stLimResync);
        // proxy -> helper: kazdy snimek hry ma byt predan
        const unsigned notPosted = g_stPresents > g_stPosted ? g_stPresents - g_stPosted : 0u;
        if (ScStreak(SC_HANDOFF, s_calm >= 2 && g_stPresents >= 50 && notPosted > std::max(4.0, 0.02 * g_stPresents)))
            ScReport(SC_HANDOFF, "proxy nepredala helperu vsechny snimky hry | snimku %u, predano %u, vynechano (helper "
                     "pozadu) %u, helper nepripraven %u za %.1f s",
                     g_stPresents, g_stPosted, g_stDropped, g_stNotReady, sec);

        // Statistika helperu (jeji okno ~5 s se s intervalem proxy nekryje: klid uz druhy interval po sobe)
        const bool hw = statFresh && s_calm >= 2 && g_sh->helperState == BMSD_PST_PRESENTING && g_sh->statRecreate == 0;
        const uint32_t taken = g_sh->statFrames, real = g_sh->statFgReal, gen = g_sh->statFgGen;
        const uint32_t slots = g_sh->scAdSlots, lost = g_sh->scAdSlotsLost, sw = g_sh->scAdSwitches;
        const uint32_t dropped = g_sh->scDropped, gskip = g_sh->scGenSkipped;
        // 1. Zahazovani. Bez rozvrhu A (pevny nasobek, FG pozastavena, adaptivni rezim prave negeneruje) se kazdy
        // prevzaty skutecny snimek zobrazi: zobrazene skutecne = prevzate. Rozvrh A (adaptivni rezim generuje) ukazuje
        // skutecny snimek primo jen kdyz padne na slot mrizky, jinak jeho obsah nesou mezisnimky - tam plati: zadna
        // polozka (skutecny snimek se svymi mezisnimky) se nezahodi pred zobrazenim a zadny slot nezustane bez obrazu.
        bool drop = false, slotBad = false;
        if (hw && sw == 0 && taken >= 50) {
            if (slots == 0) drop = (double)taken - (double)real > std::max(4.0, 0.02 * taken);
            else drop = dropped > std::max(4.0, 0.02 * taken);
            slotBad = slots > 0 && lost > std::max(4.0, 0.02 * slots);
        }
        if (ScStreak(SC_DROP, drop))
            ScReport(SC_DROP, "helper zahazuje skutecne snimky | prevzato %u, zobrazeno skutecnych %u + generovanych %u, "
                     "zahozenych polozek %u za ~5 s; rezim %s %ux%s, strop %u FPS, hra %.1f FPS",
                     taken, real, gen, dropped, FgModeName((int)actMode), actMult,
                     slots ? " (rozvrh A)" : "", g_fgTarget, fps);
        if (ScStreak(SC_ADSLOT, slotBad))
            ScReport(SC_ADSLOT, "adaptivni rezim: sloty mrizky zustavaji bez obrazu | slotu %u, bez obrazu %u, zobrazeno "
                     "%u skutecnych + %u generovanych, prevzato %u za ~5 s; rezim %s, cil %.1f FPS",
                     slots, lost, real, gen, taken, FgModeName((int)actMode), g_sh->adaptTargetMilli / 1000.0);
        // Generovane snimky se maji zobrazit vsechny (pevny nasobek i rozvrh A)
        if (ScStreak(SC_GENMISS, hw && gen + gskip >= 50 && gskip > std::max(4.0, 0.05 * (gen + gskip))))
            ScReport(SC_GENMISS, "generovane snimky vypadavaji | zobrazeno generovanych %u, vynechano %u za ~5 s; rezim %s "
                     "%ux, skutecnych %.1f FPS, vystup %.1f FPS",
                     gen, gskip, FgModeName((int)actMode), actMult, g_sh->statFgRealFps, g_sh->statFgOutFps);
        // Presenter v procesu hry ma zobrazit kazdy obraz od helperu
        const uint32_t gsk = g_sh->gpSkipped, dsk = gsk - s_gpSkipped;
        s_gpSkipped = gsk;
        if (ScStreak(SC_GPSKIP, hw && g_gpActive && real + gen >= 50 && dsk > std::max(4.0, 0.02 * (real + gen))))
            ScReport(SC_GPSKIP, "presenter v procesu hry preskakuje obrazy | preskoceno %u, helper predal %u skutecnych + "
                     "%u generovanych za ~5 s",
                     dsk, real, gen);
        // Vystup nema prekrocit strop zobrazeni (helper drzi nejkratsi interval mezi snimky)
        if (ScStreak(SC_OUTCAP, hw && vrrCap > 0.0 && g_sh->statFgOutFps > vrrCap * 1.02 + 0.5))
            ScReport(SC_OUTCAP, "vystup je nad stropem zobrazeni / cilem | zobrazeno %.1f FPS > strop %.1f FPS (monitor %d Hz); "
                     "rezim %s", g_sh->statFgOutFps, vrrCap, hz, FgModeName((int)actMode));
        // 4. Zbytecne generovani: adaptivni rezim generuje, ackoli odhad FPS bez generatoru je uz 10 s nad hranici,
        // od ktere se ma vypnout (1,08 x horni hranice; mimo dobu, kdy je vypnuti po nevydarenem odhadu blokovane)
        if (statFresh && adapt && g_sh->scAdCostOverMs >= 10000u)
            ScReport(SC_USELESS, "adaptivni rezim generuje, i kdyz by hra cil stihla sama | odhad bez generatoru %.1f FPS "
                     "> hranice vypnuti %.1f FPS uz %.0f s; skutecnych %.1f FPS, vystup %.1f FPS, cil %.1f FPS",
                     g_sh->scAdCostEst, g_sh->scAdCostLimit, g_sh->scAdCostOverMs / 1000.0, g_sh->statFgRealFps,
                     g_sh->statFgOutFps, g_sh->adaptTargetMilli / 1000.0);
        // Velky nasobek z nizkeho zakladu ([fg] AdaptMinFps / AdaptLowMult): pod hranici jen AdaptLowMult x skutecne
        if (adapt && cfg.adaptMinFps > 0 && g_fgTargetD > 0.0) {
            const double minEff = std::min((double)cfg.adaptMinFps, FgTgtEff() / 2.0);
            const double rf = g_sh->statFgRealFps, of = g_sh->statFgOutFps;
            if (ScStreak(SC_LOWBASE, hw && sw == 0 && slots > 0 && rf > 5.0 && rf < 0.9 * minEff &&
                                         of > rf * ((double)cfg.adaptLowMult + 0.6)))
                ScReport(SC_LOWBASE, "velky nasobek z nizkeho zakladu | skutecnych %.1f FPS (pod AdaptMinFps %.0f), vystup "
                         "%.1f FPS = %.1fx, povoleno nejvys %dx",
                         rf, minEff, of, of / rf, cfg.adaptLowMult);
        } else {
            g_sc[SC_LOWBASE].streak = 0;
        }
    } else {
        g_sc[SC_OVERLIM].streak = g_sc[SC_HANDOFF].streak = g_sc[SC_DROP].streak = g_sc[SC_ADSLOT].streak = 0;
        g_sc[SC_GENMISS].streak = g_sc[SC_GPSKIP].streak = g_sc[SC_OUTCAP].streak = g_sc[SC_LOWBASE].streak = 0;
    }

    // 5. Zamek a FPS pod moznostmi hry
    const bool generating = helper && fgState == BMSD_FGS_ACTIVE && (!adapt || g_sh->adaptEngaged);
    const bool refOk = s_refTick && tick - s_refTick <= 120000;  // hra bez generovani v poslednich 2 minutach
    {
        // Zamek na stupni <= 1/3 dele nez 10 s (cas bezi jen pri generovani v popredi, bez pauz - FgAdaptLock). Hlasi se
        // jen s dukazem, ze hra muze vic: hra i GPU jsou hotove do 60 % intervalu stupne (zamek mel davno o stupen
        // nahoru), nebo hra bez generovani v poslednich 2 minutach zvladala aspon 1,5x tolik.
        const LONGLONG now = Qpc(), q10 = 10 * g_qpf.QuadPart;
        const bool lowLong = g_scLockLowQ && now - g_scLockLowQ >= q10, room = g_scLockRoomQ && now - g_scLockRoomQ >= q10;
        const bool refSays = refOk && s_refFps >= 1.5 * g_scLockRate;
        if (generating && lowLong && (room || refSays)) {
            char ref[96] = "hra bez generovani zatim nemerena";
            if (refOk)
                snprintf(ref, sizeof ref, "hra bez generovani pred %.0f s %.1f FPS", (double)(tick - s_refTick) / 1000.0,
                         s_refFps);
            ScReport(SC_LOCKLOW, "zamek je dlouho na nizkem stupni | stupen %d/%d uz %.0f s, limiter %.1f skutecnych FPS, "
                     "cas snimku hry %.1f ms a GPU hotovo %.1f ms z intervalu %.1f ms%s; %s",
                     g_scLockQ, g_scLockP, (double)(now - g_scLockLowQ) / (double)g_qpf.QuadPart, g_scLockRate,
                     g_scLockBusy, g_scLockGpu, g_scLockRate > 0.0 ? 1000.0 / g_scLockRate : 0.0,
                     room ? " (hra i GPU maji rezervu)" : "", ref);
        }
    }
    {
        // Hra pod 40 FPS pri generovani, ackoli bez generovani zvladala aspon 1,5x tolik (a aspon 60). Pevny nasobek:
        // nizky zaklad zvoleny nasobkem (strop / nasobek) se nehlasi, jen propad pod 90 % tohoto zakladu.
        double nominal = 0.0;
        if (!adapt && actMult >= 2 && g_fgTargetD > 0.0) nominal = FgTgtEff() / actMult;
        const bool low = generating && calm && fps > 5.0 && fps < 40.0 && refOk && s_refFps >= std::max(60.0, 1.5 * fps) &&
                         (adapt || fps < 0.9 * nominal);
        if (ScStreak(SC_LOWFPS, low))
            ScReport(SC_LOWFPS, "hra bezi pod 40 FPS, ackoli bez generovani zvladala vic | hra %.1f FPS, pred %.0f s bez "
                     "generovani %.1f FPS; rezim %s %ux, strop limiteru %.1f FPS, limiter spal %u x z %u",
                     fps, (double)(tick - s_refTick) / 1000.0, s_refFps, FgModeName((int)actMode), actMult,
                     g_scLimRateMax, g_stLimSleep.n, g_stPresents);
    }
    // Reference: FPS hry v klidnem intervalu, ve kterem se negenerovalo (FG vypnuta, pozastavena, adaptivni rezim jen
    // se skutecnymi snimky - tam je to dolni odhad, hru drzi cil)
    const bool noGen = !helper || fgState != BMSD_FGS_ACTIVE || (adapt && !g_sh->adaptEngaged && !g_adLockWord);
    if (calm && noGen && fps > 5.0) {
        s_refFps = fps;
        s_refTick = tick;
    }

    s_fore = fore;
    s_cover = cover;
    s_state = (int)g_state;
    s_mode = g_fgMode;
    s_fgState = fgState;
    s_actMode = actMode;
    s_actMult = actMult;
    s_switches = g_switches;
    g_scMaxFrameMs = 0.0;
    g_scLimN = 0;
    g_scLimRateMax = g_scLimOutMax = 0.0;
}

static void FgEstLog(double el);  // src/fgest.inl (vlozeny pred FgLimiter)

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
    if (g_scPrevPresQ) {  // samokontroly: nejdelsi snimek intervalu (pauza, nacitani)
        const double dt = Ms(g_scPrevPresQ, now);
        if (dt > g_scMaxFrameMs) g_scMaxFrameMs = dt;
    }
    g_scPrevPresQ = now;
    if (!g_statQpc) g_statQpc = now;
    const double el = Ms(g_statQpc, now);
    if (el < 5000.0) return;
    char win[256];
    DescribeWindow(g_gameWnd, win, sizeof win);
    ScSamplePath();  // samokontroly: cesta zobrazeni (flip / kompozice) za uplynuly interval
    ScHealComposed();
    char pathTxt[128];
    bool statFresh = false;  // helper od minuleho radku zapsal novou statistiku (plati i pro pole statFg*)
    if (g_state == PS_OFF || g_state == PS_IDLE) {  // FG vypnuta: jen kratky radek (jako bez presenteru)
        Log("prezentace %.1f s: FG vypnuta, presenter nebezi -> %s; snimku %u (%.1f FPS)%s", el / 1000.0,
            g_direct ? "DXVK primo do okna hry" : g_child ? "DXVK do dcerineho okna" : "DXVK do skryteho okna",
            g_stPresents, g_stPresents * 1000.0 / el, ScPathText(pathTxt, sizeof pathTxt));
        if (cfg.inMain) FgEstLog(el);
        SelfCheck(el, false);
        g_stPresents = g_stPosted = g_stDropped = g_stNotReady = 0;
        g_stCopy = g_stReuse = g_stSlot = g_stOPresent = g_stBp = Acc();
        g_stBpTo = 0;
        g_stHl = g_stHlMiss = g_stHlSnaps = 0;
        g_stLimSleep = g_stLimErr = Acc();
        g_stLimLate = g_stLimResync = 0;
        g_statQpc = now;
        return;
    }
    if (g_state == PS_FALLBACK) {
        Log("prezentace %.1f s: ZALOHA (%s) -> %s; snimku %u | %s%s", el / 1000.0, g_fbReason,
            g_direct ? "DXVK primo do okna hry" : g_child ? "DXVK do dcerineho okna" : "DXVK do skryteho okna",
            g_stPresents, win, ScPathText(pathTxt, sizeof pathTxt));
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
            "prevzeti obrazu %.3f / %.2f ms (%u x), zpetny tlak %.3f / %.2f ms (%u x, vyprselo %u), o_Present %.3f / %.2f ms | %s | %s%s",
            el / 1000.0, g_state == PS_HELPER ? "helper" : "start", g_sh ? g_sh->helperState : 0, g_stPresents,
            g_stPosted, g_stDropped, g_stNotReady, g_stCopy.avg(), g_stCopy.max, g_stSlot.avg(), g_stSlot.max,
            g_stReuse.avg(), g_stReuse.max, g_stReuse.n, g_stBp.avg(), g_stBp.max, g_stBp.n, g_stBpTo, g_stOPresent.avg(), g_stOPresent.max, hs, win,
            ScPathText(pathTxt, sizeof pathTxt));
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
        FgEstLog(el);
    }
    SelfCheck(el, statFresh);  // pred nulovanim citacu intervalu
    if (cfg.inMain && g_sh) {
        g_stHl = g_stHlMiss = g_stHlSnaps = 0;
        g_stLimSleep = g_stLimErr = Acc();
        g_stLimLate = g_stLimResync = 0;
    }
    g_stPresents = g_stPosted = g_stDropped = g_stNotReady = 0;
    g_stCopy = g_stReuse = g_stSlot = g_stOPresent = g_stBp = Acc();
    g_stBpTo = 0;
    g_statQpc = now;
}

// Navrat po Alt+Tab / pauze snimku hry ([fg] ResumeGuard, v102k; uzivatel: po Alt+Tab se FPS vraci do maxima pomalu).
// Po navratu hra jeste nekolik sekund jede pomaleji (Windows / ovladac ji na pozadi pribrzdily); regulator rezimu 3 to
// bral jako nestihajici GPU, strop srazil a zvedal ho jen +2 %/s, zamek adaptivniho rezimu sel o stupen dolu a nahoru
// az po drzeni. Tady se navrat pozna (okno procesu hry zpet v popredi, nebo limiter dlouho nebezel - hra po Alt+Tab
// nekreslila) a nasleduje ochranna doba: strop / stupen z doby pred Alt+Tabem, nic se nesrazi ani nezvysuje, mereni
// "nestiha" se nepocita, auto nasobek stoji. Konci, az hra 0,5 s v kuse bezi aspon 93 % FPS z doby pred Alt+Tabem
// (nejdriv po 1 s), nejpozdeji po ResumeGuard s. Pak regulator pokracuje normalne - srazi jen skutecny problem.
// Vola ho limiter kazdy skutecny snimek (jen kdyz bezi), pro vsechny rezimy FG; s ResumeGuard=0 jen log (FPS po navratu).
// (g_rgBg = okno hry neni v popredi a g_rgProt = ochranna doba po navratu jsou nahore u samokontrol)
static unsigned g_rgEvent;       // pocitadlo navratu (regulator / zamek si pri zmene obnovi stav z doby pred Alt+Tabem)
static LONGLONG g_rgLastRunQ;    // posledni beh limiteru
static void FgResumeGuard(LONGLONG now) {
    static LONGLONG s_chkQ, s_protQ, s_okSince, s_snapQ, s_secQ;
    static bool s_focus = true;
    static double s_dtEma, s_snap[2], s_ref;
    static unsigned s_secN, s_secFrames, s_sec[12];
    static double s_secCap[12];
    const LONGLONG q1s = g_qpf.QuadPart;
    const bool gap = g_rgLastRunQ && now - g_rgLastRunQ > q1s * 3 / 10;
    if (g_rgLastRunQ && !gap) {
        const double dt = Ms(g_rgLastRunQ, now);
        if (dt > 0.0 && dt < 200.0) s_dtEma = s_dtEma > 0.0 ? s_dtEma + 0.1 * (dt - s_dtEma) : dt;
    }
    g_rgLastRunQ = now;
    const double fps = s_dtEma > 0.0 ? 1000.0 / s_dtEma : 0.0;
    if (!s_chkQ || now - s_chkQ > q1s / 4 || gap) {
        s_chkQ = now;
        DWORD pid = 0;
        const HWND fw = GetForegroundWindow();
        if (fw) GetWindowThreadProcessId(fw, &pid);
        const bool focus = !fw || pid == GetCurrentProcessId();
        // FPS z doby pred Alt+Tabem: snimek stavu 0,5-1 s stary (ten nejnovejsi uz muze byt zpomaleny)
        if (s_focus && (!focus || gap) && !s_protQ) s_ref = std::max(s_snap[0], s_snap[1]);
        if (focus && (!s_focus || gap)) {
            if (cfg.fgResumeGuard > 0)
                Log("prezentace: navrat do hry (%s) - ochrana az %d s: strop z doby pred Alt+Tabem, nic nesrazet (hra "
                    "pred Alt+Tabem %.1f FPS)",
                    s_focus ? "pauza snimku hry" : "okno zpet v popredi", cfg.fgResumeGuard, s_ref);
            else
                Log("prezentace: navrat do hry (%s) - ResumeGuard=0 (hra pred Alt+Tabem %.1f FPS)",
                    s_focus ? "pauza snimku hry" : "okno zpet v popredi", s_ref);
            s_protQ = cfg.fgResumeGuard > 0 ? now : 0;
            s_okSince = 0;
            s_dtEma = 0.0;
            s_secQ = now;
            s_secN = s_secFrames = 0;
            ++g_rgEvent;
        }
        s_focus = focus;
    }
    g_rgBg = !s_focus && cfg.fgResumeGuard > 0;
    if (!s_focus) {
        g_rgProt = false;
        return;
    }
    if (!s_protQ) {  // bezny beh: snimek FPS kazdych 0,5 s
        if (!s_snapQ || now - s_snapQ >= q1s / 2) {
            s_snapQ = now;
            s_snap[1] = s_snap[0];
            s_snap[0] = fps;
        }
    } else {
        const double el = (double)(now - s_protQ) / (double)q1s;
        const bool ok = s_ref <= 0.0 || (fps > 0.0 && fps >= 0.93 * s_ref);
        if (!ok) s_okSince = 0;
        else if (!s_okSince) s_okSince = now;
        const bool back = el >= 1.0 && s_okSince && now - s_okSince >= q1s / 2;
        if (back || el >= (double)cfg.fgResumeGuard) {
            Log("prezentace: navrat do hry - ochrana skoncila po %.1f s (%s; hra %.1f FPS, pred Alt+Tabem %.1f)", el,
                back ? "hra zase bezi jako pred Alt+Tabem" : "cas vyprsel", fps, s_ref);
            s_protQ = 0;
            s_snapQ = now;
            s_snap[0] = s_snap[1] = fps;
        }
    }
    g_rgProt = s_protQ != 0;
    // FPS hry po sekundach 12 s po navratu (jeden radek logu; strop = strop limiteru skutecnych FPS)
    if (s_secQ) {
        ++s_secFrames;
        if (now - s_secQ >= q1s) {
            s_secCap[s_secN] = g_limRateLast;
            s_sec[s_secN++] = s_secFrames;
            s_secFrames = 0;
            s_secQ += q1s;
            if (now - s_secQ >= q1s) s_secQ = now;
            if (s_secN >= 12) {
                char b[400];
                int n = 0;
                for (unsigned i = 0; i < s_secN && n < (int)sizeof b - 24; ++i)
                    n += snprintf(b + n, sizeof b - n, " %u/%.0f", s_sec[i], s_secCap[i]);
                Log("prezentace: navrat do hry - FPS hry / strop limiteru po sekundach:%s", b);
                s_secQ = 0;
            }
        }
    }
}

// Pomala hra v adaptivnich rezimech ([fg] AdaptMinFps / AdaptLowMult, v102l; uzivatel: "generovat pro 26 snimku do
// plna je blbost"). Kdyz hra sama jede pod AdaptMinFps (nejvys cil / 2) skutecnych FPS, cil vystupu pro helper klesne na
// AdaptLowMult x skutecne FPS (vychozi 3x): mene mezisnimku z tak nizkeho zakladu (latence jednoho intervalu a
// artefakty rostou s delkou intervalu), mene prace GPU. Navrat k plnemu cili az nad 115 % hranice (hystereze). Rozhoduje
// se kazdych 0,5 s; na pozadi a v ochranne dobe po Alt+Tabu se stav nemeni. Zamek pod hranici nejde (FgAdaptLock).
// v103h: s vysokym cilem (az 6x) stoji plny cil na GPU vic nez 3x, takze hra tesne nad hranici by po navratu zase klesla
// pod ni a kmitala. Navrat, po kterem hra do 15 s znovu klesne, se proto uci: dalsi navrat az pri skutecnych FPS
// o 15 % nad tim, z ceho se naposledy nezdarilo (nejvys 4 x hranice), a podminka musi platit 2 s (po kazdem nezdaru
// 2x dele, nejvys 30 s). Po 60 s bez nezdaru ve stavu plny cil, nebo po 120 s v omezeni se naucena hranice o krok sniz.
static void FgAdaptLowBase(LONGLONG now, bool adapt) {
    static LONGLONG s_prevQ, s_decQ;
    static double s_dtEma;
    static bool s_low;
    static double s_upFps, s_holdS = 2.0, s_lastUpFps;  // v103h: naucena hranice navratu (0 = 1,15 x minEff), drzeni, FPS pri navratu
    static LONGLONG s_upSince, s_upQ, s_lowQ;
    auto set = [](double cap) {
        if (fabs(cap - g_adOutCap) > 1e-6) {
            g_adOutCap = cap;
            FgPublishAdapt();
        }
    };
    if (!adapt || cfg.adaptMinFps <= 0) {
        s_low = false;
        s_prevQ = 0;
        set(0.0);
        return;
    }
    if (s_prevQ) {
        const double dt = Ms(s_prevQ, now);
        if (dt > 0.0 && dt < 200.0) s_dtEma = s_dtEma > 0.0 ? s_dtEma + 0.1 * (dt - s_dtEma) : dt;
    }
    s_prevQ = now;
    if (s_decQ && now - s_decQ < g_qpf.QuadPart / 2) return;
    s_decQ = now;
    if (g_rgBg || g_rgProt || s_dtEma <= 0.0) return;
    const double fps = 1000.0 / s_dtEma, F = FgTgtEff();
    const double minEff = std::min((double)cfg.adaptMinFps, F / 2.0);
    if (!s_low && fps > 5.0 && fps < minEff) {
        s_low = true;
        if (s_upQ && now - s_upQ < 15 * g_qpf.QuadPart) {  // navrat nevysel (plny cil hru zase stahl pod hranici)
            s_upFps = std::min(4.0 * minEff, 1.15 * std::max(s_lastUpFps, 1.15 * minEff));
            s_holdS = std::min(30.0, 2.0 * s_holdS);
        }
        s_lowQ = now;
        s_upSince = 0;
        Log("prezentace: FG adaptivni - hra %.1f FPS pod AdaptMinFps %.0f: cil vystupu nejvys %dx skutecne (navrat nad %.1f FPS "
            "po %.0f s)", fps, minEff, cfg.adaptLowMult, std::max(1.15 * minEff, s_upFps), s_holdS);
    } else if (s_low) {
        if (s_upFps > 0.0 && now - s_lowQ > 120 * g_qpf.QuadPart) {  // scena se mohla zmenit: hranici po kroku zpet
            s_upFps = s_upFps / 1.15 > 1.15 * minEff ? s_upFps / 1.15 : 0.0;
            s_lowQ = now;
        }
        if (fps > std::max(1.15 * minEff, s_upFps)) {
            if (!s_upSince) s_upSince = now;
        } else {
            s_upSince = 0;
        }
        if (s_upSince && now - s_upSince >= (LONGLONG)(s_holdS * (double)g_qpf.QuadPart)) {
            s_low = false;
            s_upQ = now;
            s_lastUpFps = fps;
            s_upSince = 0;
            Log("prezentace: FG adaptivni - hra %.1f FPS zase nad AdaptMinFps %.0f: plny cil vystupu %.1f", fps, minEff, F);
        }
    } else if (s_upQ && now - s_upQ > 60 * g_qpf.QuadPart) {  // plny cil drzi: naucene po kroku zapomenout
        s_upQ = 0;
        s_holdS = std::max(2.0, 0.5 * s_holdS);
        s_upFps = s_upFps / 1.15 > 1.15 * minEff ? s_upFps / 1.15 : 0.0;
    }
    if (!s_low) {
        set(0.0);
        return;
    }
    const double cap = std::min(F, fps * (double)cfg.adaptLowMult);
    if (g_adOutCap <= 0.0 || fabs(cap - g_adOutCap) > 0.05 * g_adOutCap) set(std::max(20.0, cap));
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
    if (am != cur || g_rgBg || g_rgProt) {  // helper jeste neprepnul; pozadi / ochrana po Alt+Tabu: mereni neplati
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
    // Pomale PC (v103c, audit c. 14): z nizkeho zakladu (pod AdaptMinFps skutecnych) nahoru nejvys na AdaptLowMult -
    // generovat 5-6x z ~25 FPS je chyba, ne vysledek (stejna mez jako u adaptivnich rezimu)
    const bool lowBase = cfg.adaptMinFps > 0 && fps > 0.0 && fps < (double)cfg.adaptMinFps && cur >= cfg.adaptLowMult;
    if (g_limScale < (double)cfg.fgAutoUpBelow / 100.0 && cur < mx && !lowBase) want = 1;
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

// Zlomkovy zamek adaptivniho FSR ([fg] AdaptLock=1). Rozvrh A ma mrizku slotu s periodou P = 1 / cil. Kdyz limiter
// drzi skutecne snimky presne na cil * q / p (p <= AdaptLockDen), pada kazdy q-ty skutecny snimek presne na slot
// (zobrazi se primo, podil 1 / p) a mezisnimky maji pevne casy t = k / p; helper pak vystaci s posunem obsahu o P / q
// kratsim. Tady se voli stupen q / p a vraci perioda limiteru v QPC (0 = bez zamku, limiter jako dosud):
//   - jen kdyz helper generuje (adaptEngaged); jinak volne (strop = cil nebo RealCap),
//   - z volneho: po 0,3 s nejvyssi stupen pod dosazenymi FPS (a pod RealCap),
//   - dolu rychle: ve dvou oknech 0,25 s po sobe hra nestihla slot u > 30 % snimku -> nejvyssi nizsi stupen pod
//     dosazenymi FPS okna (zadny = volne: hra je i pod nejnizsim stupnem),
//   - nahoru pomalu: GPU hotovo (median 5 mereni) i cas snimku hry do 80 % intervalu vyssiho stupne po dobu drzeni
//     (2 s; po kmitnuti - dolu do 10 s po zvyseni - dvojnasobek az 30 s, po 60 s klidu zpet); skok rovnou na nejvyssi
//     takovy stupen,
//   - z nejvyssiho stupne do volneho (hra by cil stihla sama) stejne; kdyz helper do 1,5 s generovat neprestane,
//     vybere se stupen znovu podle dosazenych FPS.
// Perioda se pocita ze stejnych cisel jako mrizka helperu (adaptTargetMilli, strop VRR z refreshMilli), takze faze
// proti mrizce neujizdi; jemne ji dorovnava helper.
// Snimek nestihl slot limiteru? Bez SampleSync: Present po konci slotu (minuly konec + interval). Se SampleSync spi
// pumpa zprav a Present je o snimek dal od mrizky: nestih = pumpa se znackou minuleho snimku prisla po konci slotu.
static bool g_ssLimLate, g_ssLimUsed, g_ssLimPrev;
static bool LimLateChk(LONGLONG now, LONGLONG ivQ) {
    if (g_ssLimPrev) return g_ssLimLate;
    return g_limNext && now >= g_limNext + ivQ;
}

// Predpoved pro jiny stupen zamku (v103d, docs/zamek-kmitani-vysledek.md): mereni z posledni ~1 s generovani na
// soucasnem stupni (snimky z fgest.inl, cas vlaken a enginu GPU z gamethreads.inl). Cas snimku bez spanku limiteru
// a GPU hotovo (latence) vyssi stupen nepredpovi: na 1/2 je GPU hotovo 8,7 ms a cas snimku 3 ms, a presto GPU na 3/5
// nestaci (engine 10,8 ms na skutecny snimek x 93,5 FPS > 100 %). Definice za fgest.inl.
struct FgLockMeas {
    double gr;   // engine GPU na skutecny snimek bez generatoru (hra + helper - cena generatoru, ms)
    double cm;   // cena generatoru na skutecny snimek (prumer 64 vzorku helperu, ms)
    double cpu;  // CPU na snimek: max(hlavni vlakno bez cekani na render a spanku limiteru, vlakno renderu) (ms)
    double dt;   // interval skutecnych snimku (ms)
};
static bool FgLockMeasure(LONGLONG now, uint32_t lockWord, FgLockMeas* o);

// Vytizeni 3D enginu GPU (hra + helper) za posledni ~1 s podle citacu jadra grafiky: 0 .. ~1, -1 = nezname. Pro regulatory
// s rucnim cilem nad stropem VRR (v103h): "GPU hotovo" je latence, vytizeni propustnost. Definice za fgest.inl.
static double FgGpuUtil(LONGLONG now);

static LONGLONG FgAdaptLock(LONGLONG now, double capRate, bool running, LONGLONG sleptQ) {
    static struct {
        int q, p;
    } s_st[32];
    static int s_n, s_den, s_cur = -1;  // s_cur: index stupne, -1 = volne
    static double s_cjTab[32];          // v103d: naucena cena generatoru na skutecny snimek podle stupne (0 = nezname)
    static double s_lbTab[32], s_lbHold[32];  // v103d: dolni mez ceny po nezdarenem zvyseni, plati do s_lbUntil
    static LONGLONG s_lbUntil[32];
    static LONGLONG s_prevQ, s_winQ, s_okSince, s_lastUp, s_lastDown, s_freeSince, s_probeUntil;
    static unsigned s_wn, s_wlate, s_logs, s_lateWins;
    static double s_dtEma, s_busyEma, s_upHold = 2.0, s_d[5], s_lateLast = 1.0;
    static int s_dn, s_dp, s_badIdx = -2;
    static LONGLONG s_badUntil;
    static uint32_t s_lastPub;
    const LONGLONG q1s = g_qpf.QuadPart;
    auto publish = [](uint32_t w) {
        if (g_adLockWord != w) {
            g_adLockWord = w;
            if (g_sh) g_sh->adaptLock = w;
            MemoryBarrier();
        }
    };
    // ResumeGuard (v102k): stupen z doby pred Alt+Tabem (snimek 0,5-1 s stary) se po navratu obnovi a v ochranne dobe
    // se stupen nemeni (driv: pozde chodici prvni snimky -> stupen dolu, nahoru az po drzeni 2 s a vic)
    // v102l: snimek stavu se posuzuje k okamziku ztraty popredi / posledniho snimku pred pauzou, ne k navratu (hra
    // minimalizovana 8 s -> snimek "stary" a stupen se neobnovil)
    static LONGLONG s_rgSnapQ[2], s_rgEngQ, s_rgCallQ, s_rgPendUntil;
    static int s_rgSnap[2] = {-2, -2}, s_rgSave = -2;  // index stupne, -1 = volne, -2 = nic
    static unsigned s_rgSeen;
    static bool s_rgHave, s_rgPend;
    const bool rg = cfg.fgResumeGuard > 0, rgHold = rg && (g_rgBg || g_rgProt);
    if (rg) {
        if (!s_rgHave && (g_rgBg || g_rgEvent != s_rgSeen)) {
            int sv = -2;
            const LONGLONG lossQ = s_rgCallQ ? s_rgCallQ : now;  // posledni snimek pred ztratou / pauzou
            if (s_rgEngQ && lossQ - s_rgEngQ <= q1s)             // zamek tehdy byl zapojeny
                for (int k = 0; k < 2; ++k) {
                    const int v = s_rgSnap[k];
                    if (v == -2 || s_rgEngQ - s_rgSnapQ[k] > q1s * 3 / 2) continue;
                    sv = sv == -2 ? v : (sv == -1 || v == -1) ? -1 : std::min(sv, v);  // vyssi z obou
                }
            s_rgSave = sv;
            s_rgHave = true;
        }
        if (g_rgEvent != s_rgSeen) {
            s_rgSeen = g_rgEvent;
            s_rgPend = s_rgSave != -2;
            s_rgPendUntil = 0;
            s_rgHave = false;
            s_upHold = 2.0;  // drzeni nahoru z doby pred Alt+Tabem (kmitani pri zpomalene hre) neplati
            s_lastUp = s_lastDown = 0;
            s_badIdx = -2;
            char st[40] = "volne";
            if (s_rgSave == -2) snprintf(st, sizeof st, "neznamy (zamek nebyl zapojeny)");
            else if (s_rgSave >= 0 && s_rgSave < s_n) snprintf(st, sizeof st, "%d/%d", s_st[s_rgSave].q, s_st[s_rgSave].p);
            Log("prezentace: FG zamek - navrat do hry, stupen pred Alt+Tabem %s", st);
        }
        if (s_rgPend && !g_rgProt && !g_rgBg) {  // helper po ochranne dobe 2 s negeneruje: stupen uz neplati
            if (!s_rgPendUntil) s_rgPendUntil = now + 2 * q1s;
            else if (now > s_rgPendUntil) s_rgPend = false;
        }
    }
    s_rgCallQ = now;
    const bool engaged = running && FgLockOn() && g_sh && g_sh->adaptEngaged && g_sh->fgState == BMSD_FGS_ACTIVE &&
                         g_sh->adaptTargetMilli >= 20000u;
    if (!engaged) {
        if (s_cur >= 0 && (++s_logs <= 200 || s_logs % 20 == 0))
            Log("prezentace: FG zamek - volne (helper negeneruje), byl stupen %d/%d", s_st[s_cur].q, s_st[s_cur].p);
        s_cur = -1;
        s_prevQ = s_freeSince = s_okSince = 0;
        s_dtEma = s_busyEma = 0.0;
        s_dn = 0;
        g_scLockLowQ = g_scLockRoomQ = 0;
        publish(0u);
        return 0;
    }
    // tabulka stupnu sestupne podle q / p; nejvys 5 mezisnimku na par (6x; AFG i DLSS-G), DLSS-G adaptivni p <= 6
    // (casy DLSS-G k / (n + 1), n <= 5)
    const bool dgTab = g_fgMode == BMSD_FG_DLSSGADAPT;
    const int denKey = cfg.adaptLockDen + (dgTab ? 100 : 0);
    if (s_den != denKey) {
        s_den = denKey;
        s_n = 0;
        s_cur = -1;
        const int maxP = dgTab ? std::min(cfg.adaptLockDen, 6) : cfg.adaptLockDen, maxGen = 5;
        for (int p = 2; p <= maxP; ++p)
            for (int q = 1; q < p; ++q) {
                int a = p, b = q;
                while (b) {
                    const int t = a % b;
                    a = b;
                    b = t;
                }
                if (a != 1 || (q == 1 ? p - 1 : (p + q - 1) / q) > maxGen) continue;
                s_st[s_n].q = q;
                s_st[s_n].p = p;
                ++s_n;
            }
        std::sort(s_st, s_st + s_n, [](const auto& x, const auto& y) { return x.q * y.p > y.q * x.p; });
        for (int i = 0; i < 32; ++i) {  // naucene ceny generatoru podle stupne (v103d) patri stare tabulce
            s_cjTab[i] = s_lbTab[i] = s_lbHold[i] = 0.0;
            s_lbUntil[i] = 0;
        }
        s_rgSnap[0] = s_rgSnap[1] = s_rgSave = -2;
        s_rgPend = false;
    }
    // perioda mrizky helperu (stejny vypocet jako FgAdaptMakePlan)
    const double qms = (double)q1s / 1000.0;
    double Pms = 1000000.0 / (double)g_sh->adaptTargetMilli;
    if (g_sh->outCapMilli) {
        const double minIv = (double)(LONGLONG)((double)q1s * 1000.0 / (double)g_sh->outCapMilli) / qms;
        if (Pms < minIv) Pms = minIv;
    }
    const LONGLONG Pq = (LONGLONG)(Pms * qms);
    const double F = 1000.0 / Pms;
    auto rateOf = [&](int i) { return F * (double)s_st[i].q / (double)s_st[i].p; };
    // AdaptMinFps (v102l): stupne pod spodni hranici skutecnych FPS (nejvys cil / 2) se nepouziji - hra pod ni = volne
    // (helper pak generuje nejvys AdaptLowMult x, FgAdaptLowBase)
    const double minEff = cfg.adaptMinFps > 0 ? std::min((double)cfg.adaptMinFps, F / 2.0) : 0.0;
    int nOk = s_n;
    while (nOk > 0 && rateOf(nOk - 1) < minEff - 0.01) --nOk;
    // rucni cil nad stropem VRR a hra pod AdaptMinFps (cil = AdaptLowMult x skutecne): stupen 1/3 by byl presne na hranici
    // FPS hry a kmital by s "volne" (69 zmen za 3,5 min) -> bez zamku (v103h)
    if (g_fgFree && g_adOutCap > 0.0) nOk = 0;
    // mereni: interval snimku hry, cas snimku bez spanku limiteru, GPU hotovo
    double frameDt = 0.0;
    if (s_prevQ) {
        const double dt = Ms(s_prevQ, now);
        frameDt = dt;
        if (dt > 0.0 && dt < 200.0) {
            s_dtEma = s_dtEma > 0.0 ? s_dtEma + 0.1 * (dt - s_dtEma) : dt;
            const double busy = std::max(0.0, dt - (double)sleptQ / qms);
            s_busyEma = s_busyEma > 0.0 ? s_busyEma + 0.1 * (busy - s_busyEma) : busy;
        }
    }
    s_prevQ = now;
    const uint32_t gd = Interop_FgGpuDone();
    if ((gd >> 16) && (gd >> 16) != s_lastPub) {
        s_lastPub = gd >> 16;
        s_d[s_dp] = (double)(gd & 0xFFFFu) / 100.0;
        s_dp = (s_dp + 1) % 5;
        if (s_dn < 5) ++s_dn;
    }
    double med = 0.0;
    if (s_dn >= 5) {
        double t[5];
        memcpy(t, s_d, sizeof t);
        std::sort(t, t + 5);
        med = t[2];
    }
    const double fps = s_dtEma > 0.0 ? 1000.0 / s_dtEma : 0.0;
    // Cil, na ktery GPU nestaci ani s nejvyssim nasobkem (6x, stupen 1/6; v103h): zadny nizsi stupen uz neni a volna hra by
    // GPU vytizila na 100 % (fronta, vynechane obrazy). Misto mrizky na cil je vystup celym nasobkem m skutecnych FPS:
    // efektivni cil (g_adOutReg) = m x skutecne FPS, ktere GPU zvladne s rezervou (~90 %), a limiter drzi stupen 1/m
    // z nej (kazdy skutecny snimek se zobrazi, mezisnimky v k / m). Skutecne FPS urcuje jen kapacita GPU (nahoru podle
    // predpovedi do 90 %, dolu pri nestihu slotu), ne zadany cil. Nasobek se zkousi od 3x nahoru (nejvys 6x): vyssi, kdyz
    // rytmus obrazu drzi (podil intervalu mimo pasmo) a vystup podle ceny generatoru vzroste; kdyz se rytmus rozpadne,
    // o jeden zpet a dalsi pokus az za 60 s (pak 2x dele, nejvys 480 s). Jen dokud je 1/m aspon AdaptMinFps.
    // Vytizeni GPU, pri kterem rytmus obrazu jeste drzi (stovky kopii za sekundu cekaji na rozpracovane snimky): uci se za
    // behu od 89 %, dolu po nerovnomernem useku (aspon 84 %), po 60 s klidu o 1 bod zpet (nejvys 92 %). Niz ne: rytmus
    // pak kazi nahodne cekani kopii na rozpracovany snimek, ne nedostatek rezervy (h3: skutecne FPS klesly az na 45).
    static double s_regT, s_mHold[7], s_uMax = 0.89;
    static LONGLONG s_uQ;
    static LONGLONG s_regQ, s_mQ, s_mBlock[7];
    static int s_regM = 3, s_mBad, s_mOk;
    static LONG s_mSeq;
    static uint32_t s_regMode;
    double retR = 0.0;
    const double tgtFull = FgTgtEff();
    if (fabs(tgtFull - s_regT) > 1e-6 || s_regMode != (uint32_t)g_fgMode) {
        s_regT = tgtFull;
        s_regMode = (uint32_t)g_fgMode;
        g_adOutReg = 0.0;
        s_regM = std::min(3, cfg.adaptLockDen);
        for (int i = 0; i < 7; ++i) s_mBlock[i] = 0, s_mHold[i] = 60.0;
        FgPublishAdapt();
    }
    int low = nOk - 1;
    bool lowMax = low >= 0 && s_st[low].q == 1 && s_st[low].p >= std::min(6, cfg.adaptLockDen) && g_adOutCap <= 0.0;
    if (g_adOutReg > 0.0) {  // stupen 1/m
        low = -1;
        for (int i = 0; i < nOk; ++i)
            if (s_st[i].q == 1 && s_st[i].p == s_regM) low = i;
        lowMax = low >= 0 && g_adOutCap <= 0.0;
    }
    auto regSet = [&](double t, const char* why) {
        t = std::min(t, tgtFull);
        if (t < (double)s_regM * minEff) return false;
        const double old = g_adOutReg > 0.0 ? g_adOutReg : tgtFull;
        g_adOutReg = t >= 0.999 * tgtFull ? 0.0 : t;
        s_regQ = now;
        FgPublishAdapt();
        if (++s_logs <= 200 || s_logs % 20 == 0)
            Log("prezentace: FG efektivni cil %.1f -> %.1f FPS z %.1f (%dx; %s; hra %.1f FPS, GPU hotovo %.1f ms, drzeni nahoru %.0f s)",
                old, g_adOutReg > 0.0 ? g_adOutReg : tgtFull, tgtFull, s_regM, why, fps, med, s_upHold);
        return true;
    };
    // v103d: predpoved vytizeni GPU (engine) a CPU na jinem stupni. Plati jen po 2 s na stupni (cena generatoru je
    // prumer 64 vzorku, po zmene stupne chvili stara). Cena generatoru na skutecny snimek se uci zvlast pro kazdy stupen
    // (DLSS-G: par + volani, na 2/5 ~3 ms, na 1/2 ~1,65 ms); stupen bez nauceni = umerne poctu mezisnimku na skutecny
    // snimek, nejmene 0,6 x dnesni (cena paru se nezmensi).
    double* const s_cj = s_cjTab;
    static LONGLONG s_chgQ;
    static int s_fromIdx = -2;
    static double s_upGr;  // engine bez generatoru pred poslednim zvysenim (ms, 0 = nezname)
    FgLockMeas lm{};
    const bool lmOk = s_cur >= 0 && s_cur < s_n && s_chgQ && now - s_chgQ >= 2 * q1s && FgLockMeasure(now, g_adLockWord, &lm);
    // posledni uspesne mereni: s citaci GPU se puvodni pravidlo (GPU hotovo, cas snimku) nepouzije, dokud mereni
    // nechybi > 10 s (z_slow v103d1: 1/2 -> 3/5 "velka rezerva" v prvnich 2 s po zmene, CPU na 3/5 102 %)
    static LONGLONG s_lmOkQ;
    if (lmOk) s_lmOkQ = now;
    const bool lmRecent = s_lmOkQ && now - s_lmOkQ <= 10 * q1s;
    auto genOf = [&](int i) { return (double)(s_st[i].p - s_st[i].q) / (double)s_st[i].q; };
    // ucit jen na zdravem stupni (bez nestihu, GPU do 90 %): v menu hry (Esc) cena na 1/2 skoci 1,66 -> 3,2 ms a
    // naucena hodnota pak drzela zamek na 2/5 (z_dga v103d0); dolu rychle, nahoru pomalu
    if (lmOk && s_lateLast < 0.05 && (lm.gr + lm.cm) * rateOf(s_cur) / 1000.0 <= 0.9)
        s_cj[s_cur] = s_cj[s_cur] <= 0.0 ? lm.cm : s_cj[s_cur] + (lm.cm < s_cj[s_cur] ? 0.3 : 0.05) * (lm.cm - s_cj[s_cur]);
    auto costOf = [&](int j) {
        if (j < 0) return 0.0;
        double c = s_cj[j] > 0.0 ? s_cj[j] : lm.cm * std::max(0.6, genOf(j) / genOf(s_cur));
        // dolni mez z nezdareneho zvyseni: na stupni j hra nestihla pri enginu bez generatoru s_upGr, cena tedy byla
        // aspon interval - s_upGr (jinak by GPU stacila). Plati 60 s, po kazdem dalsim nezdaru 2x dele (nejvys 480 s).
        if (s_lbUntil[j] && now < s_lbUntil[j]) c = std::max(c, s_lbTab[j]);
        return c;
    };
    // vytizeni na stupni j (-1 = volne pri 95 % cile, bez generatoru): podil sekundy
    auto predG = [&](int j) { return (lm.gr + costOf(j)) * (j < 0 ? 0.95 * F : rateOf(j)) / 1000.0; };
    auto predC = [&](int j) { return lm.cpu * (j < 0 ? 0.95 * F : rateOf(j)) / 1000.0; };
    auto change = [&](int to, const char* why) {
        if (to == s_cur) return;
        if (++s_logs <= 200 || s_logs % 20 == 0) {
            char a[24] = "volne", b[24] = "volne", pr[96] = "predpoved nezname";
            if (s_cur >= 0) snprintf(a, sizeof a, "%d/%d", s_st[s_cur].q, s_st[s_cur].p);
            if (to >= 0) snprintf(b, sizeof b, "%d/%d", s_st[to].q, s_st[to].p);
            if (lmOk)
                snprintf(pr, sizeof pr, "predpoved GPU %.0f %% CPU %.0f %% (dnes GPU %.0f %%, engine bez gen. %.2f ms, c %.2f -> %.2f ms)",
                         predG(to) * 100.0, predC(to) * 100.0, predG(s_cur) * 100.0, lm.gr, lm.cm, costOf(to));
            Log("prezentace: FG zamek - stupen %s -> %s (%s; skutecnych %.1f FPS z cile %.1f, strop %.1f; hra %.1f FPS, "
                "cas snimku %.1f ms, GPU hotovo %.1f ms, drzeni nahoru %.0f s; %s)",
                a, b, why, to >= 0 ? rateOf(to) : std::min(capRate, F), F, capRate, fps, s_busyEma, med, s_upHold, pr);
        }
        s_chgQ = now;
        s_cur = to;
        s_winQ = now;
        s_wn = s_wlate = 0;
        s_okSince = s_freeSince = 0;
        s_dn = 0;  // GPU hotovo merit znovu na novem stupni
        s_lateLast = 1.0;
        s_lateWins = 0;
    };
    if (rg && !rgHold && !s_rgHave && (!s_rgSnapQ[0] || now - s_rgSnapQ[0] >= q1s / 2)) {
        s_rgSnap[1] = s_rgSnap[0];
        s_rgSnapQ[1] = s_rgSnapQ[0];
        s_rgSnap[0] = s_cur;
        s_rgSnapQ[0] = now;
    }
    s_rgEngQ = now;
    // obnovit i opakovane: helper po navratu nekdy kratce negeneruje (zamek "volne") - v ochranne dobe zase stupen pred
    // Alt+Tabem, ne vyber z pomalych snimku (v102m)
    if (s_rgPend && (g_rgProt || s_cur == -1) && s_cur != s_rgSave && s_rgSave < nOk)
        change(s_rgSave, "navrat do hry - stupen z doby pred Alt+Tabem");
    if (s_cur >= nOk) change(-1, "stupen pod AdaptMinFps");
    if (s_cur >= 0 && rateOf(s_cur) > capRate + 0.01) {  // RealCap snizen za behu
        int pick = -1;
        for (int i = s_cur + 1; i < nOk && pick < 0; ++i)
            if (rateOf(i) <= capRate + 0.01) pick = i;
        change(pick, "strop skutecnych FPS");
    }
    if (g_scTest & 4) {  // [debug] SelfCheckTest: zamek na nejnizsim povolenem stupni (overeni samokontroly)
        if (nOk > 0 && s_cur != nOk - 1) change(nOk - 1, "TEST [debug] SelfCheckTest - nejnizsi stupen");
    } else if (rgHold) {  // pozadi / ochrana po navratu: stupen nemenit, mereni nestihu nepocitat
        s_freeSince = s_okSince = 0;
        s_winQ = now;
        s_wn = s_wlate = 0;
        s_lateWins = 0;
    } else if (s_cur < 0) {
        if (!s_freeSince) s_freeSince = now;
        if (now - s_freeSince >= q1s * 3 / 10 && now >= s_probeUntil && fps > 0.0) {
            int pick = -1;
            // hra sedi na stropu (RealCap): zvladne ho, staci stupen pod stropem; jinak pod dosazenymi FPS
            const double lim = fps >= 0.97 * capRate ? capRate + 0.01 : 0.985 * fps;
            for (int i = 0; i < nOk && pick < 0; ++i)
                if (rateOf(i) <= capRate + 0.01 && rateOf(i) <= lim) pick = i;
            if (g_adOutReg > 0.0) pick = low;  // efektivni cil: vzdy stupen 1/m
            if (pick < 0 && lowMax && fps > 10.0 && fps < rateOf(low) && now - s_freeSince >= q1s)
                regSet((double)s_regM * (FgGpuUtil(now) > 0.9 || FgGpuUtil(now) < 0.0 ? 0.94 : 0.985) * fps, "GPU / hra na zadany cil nestaci ani s nejvyssim nasobkem");
            if (pick >= 0) {
                if (s_lastUp && now - s_lastUp < 10 * q1s) {  // zkouska "volne" nevysla (hra cil skoro stiha): az 30 s
                    s_upHold = std::min(30.0, s_upHold * 2.0);
                    s_lastDown = now;
                    s_lastUp = 0;
                }
                change(pick, "helper generuje");
            }
        }
    } else {
        ++s_wn;
        if (LimLateChk(now, Pq * s_st[s_cur].p / s_st[s_cur].q)) ++s_wlate;
        // v103d: zaskub (snimek nad 2,5 x interval stupne - menu hry, nacitani) v nestihajicim okne = udalost, ne
        // stupen, ktery hra nezvlada (bez trestu pro stupen a bez delsiho drzeni)
        static double s_wMax;
        static bool s_hitchW;
        const bool hitchReg = s_wMax > 100.0;  // pauza hry v okne: rytmus neposuzovat
        if (frameDt > s_wMax) s_wMax = frameDt;
        const double winMs = Ms(s_winQ, now);
        if (winMs >= 250.0 && s_wn >= 4) {
            const double lateF = (double)s_wlate / (double)s_wn, fpsW = (double)s_wn * 1000.0 / winMs;
            const bool hitch = s_wMax > 2.5 * 1000.0 / rateOf(s_cur);
            s_winQ = now;
            s_wn = s_wlate = 0;
            s_wMax = 0.0;
            s_lateLast = lateF;
            // dve okna po sobe (0,5 s): jednorazovy zaskub stupen nemeni (helper ho prekona jako bez zamku)
            if (lateF > 0.3) {
                if (!s_lateWins) s_hitchW = false;
                s_hitchW = s_hitchW || hitch;
                ++s_lateWins;
            } else {
                s_lateWins = 0;
            }
            const bool regDown = s_lateWins >= 2 && s_cur == low && lowMax && !s_hitchW;
            if (s_lateWins >= 2 && regDown && (now - s_regQ < q1s || regSet(std::min(0.97 * F, (double)s_st[low].p * 0.95 * fpsW), "hra nestiha slot"))) {
                s_lateWins = 0;  // (do 1 s po zmene se fronta teprve vyprazdnuje)
                if (s_lastUp && now - s_lastUp < 10 * q1s) {
                    s_upHold = std::min(30.0, s_upHold * 2.0);
                    s_lastUp = 0;
                }
                s_lastDown = now;
            } else if (s_lateWins >= 2) {
                s_lateWins = 0;
                int pick = -1;
                for (int i = s_cur + 1; i < nOk && pick < 0; ++i)
                    if (rateOf(i) <= 0.985 * fpsW) pick = i;
                // v103d: dosazene FPS okna jsou nestihem stazene (3/5 -> 2/5, ackoli 1/2 hra drzi). Hned po zvyseni
                // zpet na stupen, odkud se slo; jinak nejvyssi nizsi stupen, ktery podle predpovedi staci (GPU a CPU do
                // 92 %), kdyz je vyssi nez podle dosazenych FPS.
                const char* why = "hra nestiha slot";
                if (s_lastUp && now - s_lastUp < 10 * q1s && s_fromIdx > s_cur && s_fromIdx < nOk) {
                    pick = s_fromIdx;
                    if (s_upGr > 0.0 && !s_hitchW && s_cur < 32) {
                        s_lbTab[s_cur] = std::max(0.0, 1000.0 / rateOf(s_cur) - s_upGr);
                        s_lbHold[s_cur] = s_lbHold[s_cur] > 0.0 && now < s_lbUntil[s_cur] + 120 * q1s ? std::min(480.0, 2.0 * s_lbHold[s_cur]) : 60.0;
                        s_lbUntil[s_cur] = now + (LONGLONG)(s_lbHold[s_cur] * (double)q1s);
                        Log("prezentace: FG zamek - stupen %d/%d nevysel: cena generatoru aspon %.2f ms (engine bez gen. %.2f ms), "
                            "plati %.0f s", s_st[s_cur].q, s_st[s_cur].p, s_lbTab[s_cur], s_upGr, s_lbHold[s_cur]);
                    }
                    why = "hra nestiha slot, zpet na stupen pred zvysenim";
                } else if (lmOk) {
                    for (int i = s_cur + 1; i < nOk && (pick < 0 || i < pick); ++i)
                        if (rateOf(i) <= capRate + 0.01 && predG(i) <= (g_fgFree ? s_uMax : 0.92) && predC(i) <= 0.92) {
                            if (pick < 0 || i < pick) why = "hra nestiha slot, stupen podle predpovedi";
                            pick = i;
                            break;
                        }
                }
                if (s_hitchW) {  // zaskub: stupen netrestat
                    s_lastUp = 0;
                } else if (s_lastUp && now - s_lastUp < 10 * q1s) {  // kmitnuti: jednou za zvyseni
                    s_upHold = std::min(8.0, s_upHold * 2.0);  // v102l: nejvys 8 s (driv 30 s - zamek uvizl)
                    s_lastUp = 0;
                    s_badIdx = s_cur;  // tento stupen hra po zvyseni nezvladla: 30 s jen s velkou rezervou (v102r)
                    s_badUntil = now + 30 * q1s;
                }
                s_lastDown = now;
                change(pick, why);
                s_lateLast = 1.0;
            }
        }
        if (s_cur >= 0) {
            if (s_lastDown && now - s_lastDown > 60 * q1s && s_upHold > 2.0) s_upHold = 2.0;
            int up = -2;  // -2 = nic, -1 = volne, jinak index stupne
            // Vyssi stupen: GPU hotovo do 85 % a cas snimku hry bez spanku limiteru do 80 % jeho intervalu (v102r; s 80 %
            // zamek u hry omezene GPU uvizl na 3/5, GPU 77 % intervalu 2/3). Stupen, ktery hra po zvyseni nezvladla,
            // 30 s jen pri GPU do 60 % (jinak kmitani 3/4 <-> 4/5 kazdych 8 s). (v102m zkusil GPU do
            // 100 % - hra omezena GPU pak kmitala 2/3 <-> 3/4 / 5/6, 24 zmen za 2 min). Velka rezerva (cas snimku i GPU do 50 %)
            // = nahoru uz po 1 s. Jeden neuspesny vzorek casovac nenuluje (az 0,5 s v kuse). Nahoru vzdy jen o jeden
            // sousedni stupen (v102m: skok 2/3 -> 5/6 podle jednoho vzorku "GPU hotovo" a hned zpet = skubnuti; GPU
            // hotovo je latence, ne propustnost - na vyssim stupni se GPU vytizi vic, nez se z nizsiho da odhadnout).
            // v103d: s merenim enginu GPU a casu vlaken (FgLockMeasure) rozhoduje predpoved pro vyssi stupen: vytizeni
            // GPU do 92 % (stupen, ktery hra po zvyseni nezvladla, 30 s do 85 %) a CPU do 85 %; velka rezerva = obe do
            // 60 %. GPU hotovo a cas snimku bez spanku se pak nepouziji (vyssi stupen nepredpovi: 1/2 -> 3/5 -> 1/2
            // kazdych ~40 s, l_dga). Bez citacu GPU (jiny ovladac / GPU) puvodni pravidlo.
            bool easy = false;
            if (g_adOutReg > 0.0 && s_cur == low) {  // efektivni cil pod zadanym: nejdriv zpet k zadanemu cili, pak stupen
                const double iv = 1000.0 / rateOf(s_cur), g = lmOk ? std::max(predG(s_cur), predC(s_cur)) : med / iv;
                const bool ok = s_lateLast < 0.05 && now - s_regQ >= 2 * q1s &&
                                (lmOk ? predG(s_cur) <= s_uMax - 0.02 && predC(s_cur) <= 0.85 : !lmRecent && s_dn >= 5 && med < 0.85 * iv && s_busyEma < 0.8 * iv);
                if (!ok) {
                    s_okSince = 0;
                } else {
                    if (!s_okSince) s_okSince = now;
                    const double gap = s_uMax - 0.02 - g;  // do kapacity: rovnou podle mereni, posledni kousek po 1 %
                    if (now - s_okSince >= (LONGLONG)((gap > 0.03 ? 1.0 : s_upHold) * (double)q1s)) {
                        s_okSince = 0;
                        s_lastUp = now;
                        regSet(F * (gap > 0.03 ? std::min(1.25, (s_uMax - 0.02) / std::max(0.1, g)) : 1.01), "rezerva GPU");
                    }
                }
            }
            if (g_adOutReg > 0.0 && s_cur == low && !hitchReg) {  // nasobek podle rytmu obrazu
                const LONG sq = g_gpQualSeq;
                if (sq != s_mSeq) {
                    s_mSeq = sq;
                    const double bad1 = g_gpQualPct;
                    if (now - s_regQ < 2 * q1s || now - s_mQ < 2 * q1s || g_gpQualMax > 50.0f) {
                        s_mBad = s_mOk = 0;
                    } else {
                        if (bad1 > 3.0) ++s_mBad, s_mOk = 0;
                        else if (bad1 <= 1.0) ++s_mOk, s_mBad = std::max(0, s_mBad - 1);
                    }
                    const double Rn = rateOf(s_cur);
                    const double predNow = lmOk ? predG(s_cur) : FgGpuUtil(now);
                    auto setM = [&](int m, double r, const char* why) {
                        const int old = s_regM;
                        s_regM = m;
                        if (!regSet((double)m * r, why)) {
                            s_regM = old;
                            return;
                        }
                        s_mQ = now;
                        s_mBad = s_mOk = 0;
                        s_chgQ = now;
                        retR = r;  // Pq tohoto volani jeste plati pro stary efektivni cil
                        for (int i = 0; i < nOk; ++i)
                            if (s_st[i].q == 1 && s_st[i].p == m) s_cur = i;
                    };
                    if (s_mOk >= 8 && now - s_uQ > 60 * q1s && s_uMax < 0.92) s_uMax += 0.01, s_uQ = now;
                    if (s_mBad >= 3 && predNow > 0.84) {  // nejdriv vic rezervy GPU (skutecne FPS o 2 % dolu), nasobek az potom
                        s_uMax = std::max(0.84, std::min(s_uMax, predNow) - 0.02);
                        s_uQ = now;
                        s_mBad = s_mOk = 0;
                        s_lastDown = now;
                        regSet(0.98 * F, "rytmus obrazu nedrzi - vetsi rezerva GPU");
                    } else if (s_mBad >= 3 && s_regM > 2) {
                        s_mBlock[s_regM] = now + (LONGLONG)(s_mHold[s_regM] * (double)q1s);
                        s_mHold[s_regM] = std::min(480.0, 2.0 * s_mHold[s_regM]);
                        setM(s_regM - 1, Rn, "rytmus obrazu nedrzi - nizsi nasobek");
                    } else if (s_mOk >= 8 && s_regM < std::min(6, cfg.adaptLockDen) && now >= s_mBlock[s_regM + 1] &&
                               now - s_regQ >= 4 * q1s) {
                        // kapacita s dalsim mezisnimkem: cena generatoru umerne (posledni mezisnimky ~stejne drahe)
                        const double c1 = lmOk ? lm.cm * (1.0 + 0.8 / (double)(s_regM - 1)) : 0.0;
                        const double r1 = lmOk ? std::min(Rn, (s_uMax - 0.02) * 1000.0 / (lm.gr + c1)) : 0.9 * Rn;
                        if ((s_regM + 1) * r1 > 1.10 * s_regM * Rn && r1 >= minEff)
                            setM(s_regM + 1, r1, "rytmus obrazu drzi - vyssi nasobek");
                        else
                            s_mOk = 0;
                    }
                }
            }
            const bool regHold = g_adOutReg > 0.0;
            const bool bad = s_badIdx == s_cur - 1 && now < s_badUntil;
            if (regHold) {
            } else if (lmOk && s_lateLast < 0.05) {
                const int cand = s_cur == 0 ? (capRate >= 0.95 * F ? -1 : -2) : (rateOf(s_cur - 1) <= capRate + 0.01 ? s_cur - 1 : -2);
                if (cand != -2 && predG(cand) <= (g_fgFree ? s_uMax - (bad ? 0.06 : 0.0) : bad ? 0.85 : 0.92) && predC(cand) <= 0.85) {
                    up = cand;
                    easy = up >= 0 && predG(up) <= 0.6 && predC(up) <= 0.6 && (!s_lastDown || now - s_lastDown > 10 * q1s);
                } else if (cand != -2 && s_dn >= 5 && s_busyEma > 0.0 && med < 0.85 * 1000.0 / (cand < 0 ? 0.95 * F : rateOf(cand)) &&
                           s_busyEma < 0.8 * 1000.0 / (cand < 0 ? 0.95 * F : rateOf(cand))) {
                    static LONGLONG s_vetoLogQ;  // puvodni pravidlo by slo nahoru: zalogovat (nejvys 1x za 30 s)
                    if (!s_vetoLogQ || now - s_vetoLogQ > 30 * q1s) {
                        s_vetoLogQ = now;
                        char b[24] = "volne";
                        if (cand >= 0) snprintf(b, sizeof b, "%d/%d", s_st[cand].q, s_st[cand].p);
                        Log("prezentace: FG zamek - na %s ne: predpoved GPU %.0f %% CPU %.0f %% (dnes GPU %.0f %% CPU %.0f %%, engine bez "
                            "gen. %.2f ms, c %.2f -> %.2f ms, CPU %.2f ms; GPU hotovo %.1f ms, cas snimku %.1f ms)",
                            b, predG(cand) * 100.0, predC(cand) * 100.0, predG(s_cur) * 100.0, predC(s_cur) * 100.0, lm.gr, lm.cm,
                            costOf(cand), lm.cpu, med, s_busyEma);
                    }
                }
            } else if (!lmOk && !lmRecent && (!s_chgQ || now - s_chgQ >= 2 * q1s) && s_dn >= 5 && s_busyEma > 0.0 &&
                       s_lateLast < 0.05) {
                auto fits = [&](double r) {
                    const double iv = 1000.0 / r;
                    return med < (bad ? 0.6 : 0.85) * iv && s_busyEma < 0.8 * iv;
                };
                if (s_cur == 0) {
                    if (capRate >= 0.95 * F && fits(0.95 * F)) up = -1;
                } else if (rateOf(s_cur - 1) <= capRate + 0.01 && fits(rateOf(s_cur - 1))) {
                    up = s_cur - 1;
                }
                if (up != -2) {
                    const double iv = 1000.0 / (up == -1 ? 0.95 * F : rateOf(up));
                    // ne do "volne" (hra cil skoro stiha -> 5/6 <-> volne po 6 s, v102n) a ne brzy po kmitnuti
                    // GPU hotovo do 50 % (v102o s 90 %: hra omezena GPU kmitala 3/4 <-> 4/5 - hotovo na vyssim stupni
                    // skoci z 6 na 10 ms)
                    easy = up >= 0 && med < 0.5 * iv && s_busyEma < 0.5 * iv && (!s_lastDown || now - s_lastDown > 10 * q1s);
                }
            }
            static LONGLONG s_okFail;
            if (regHold) {
            } else if (up != -2) {
                s_okFail = 0;
                if (!s_okSince) s_okSince = now;
                if (now - s_okSince >= (LONGLONG)((easy ? 1.0 : s_upHold) * (double)q1s)) {
                    s_lastUp = now;
                    s_fromIdx = s_cur;  // v103d: pri nestihu hned po zvyseni zpet sem
                    s_upGr = lmOk ? lm.gr : 0.0;
                    if (up == -1) s_probeUntil = now + q1s * 3 / 2;
                    change(up, easy ? "velka rezerva GPU" : "rezerva GPU");
                }
            } else if (s_okSince) {
                if (!s_okFail) s_okFail = now;
                else if (now - s_okFail >= q1s / 2) s_okSince = s_okFail = 0;
            }
        }
    }
    {  // samokontroly: jak dlouho je zamek na stupni <= 1/3 (a jak dlouho pritom hra i GPU maji rezervu)
        static LONGLONG s_scPrevQ, s_scNoRoomQ;
        const bool gapSc = s_scPrevQ && now - s_scPrevQ > q1s * 3 / 10;  // pauza snimku hry: pocitat znovu
        s_scPrevQ = now;
        if (s_cur < 0 || 3 * s_st[s_cur].q > s_st[s_cur].p || rgHold || gapSc) {
            g_scLockLowQ = g_scLockRoomQ = 0;
        } else {
            const double iv = 1000.0 / rateOf(s_cur);
            if (!g_scLockLowQ) g_scLockLowQ = now;
            // rezerva: GPU hotovo i cas snimku hry do 60 % intervalu stupne (o stupen vys se chodi uz pod 71 / 67 %);
            // kratky vypadek (do 1 s, mereni po zmene stupne) cas nenuluje
            bool room = s_dn >= 5 && med < 0.6 * iv && s_busyEma > 0.0 && s_busyEma < 0.6 * iv;
            // v103e: cas snimku bez spanku je jen vlakno renderu - hra omezena hlavnim vlaknem (host_sleep, pomale CPU)
            // tu "rezervu" mela vzdy (t5_bal_hyst: plany poplach na 1/3). S casem vlaken a enginu GPU (FgLockMeasure)
            // musi rezervu mit i max(hlavni vlakno, render) a engine GPU na skutecny snimek.
            FgLockMeas lm;
            if (room && FgLockMeasure(now, g_adLockWord, &lm)) room = lm.cpu < 0.6 * iv && lm.gr + lm.cm < 0.6 * iv;
            if (room) {
                s_scNoRoomQ = 0;
                if (!g_scLockRoomQ) g_scLockRoomQ = now;
            } else if (g_scLockRoomQ) {
                if (!s_scNoRoomQ) s_scNoRoomQ = now;
                else if (now - s_scNoRoomQ > q1s) g_scLockRoomQ = s_scNoRoomQ = 0;
            }
            g_scLockQ = s_st[s_cur].q;
            g_scLockP = s_st[s_cur].p;
            g_scLockRate = rateOf(s_cur);
            g_scLockBusy = s_busyEma;
            g_scLockGpu = med;
        }
    }
    if (s_cur < 0) {
        publish(0u);
        return 0;
    }
    publish((uint32_t)s_st[s_cur].p | ((uint32_t)s_st[s_cur].q << 8));
    if (retR > 0.0) return (LONGLONG)((double)q1s / retR);
    return Pq * s_st[s_cur].p / s_st[s_cur].q;
}

static LONGLONG g_limSleptQ;  // jak dlouho limiter v minulem volani spal (QPC)

#include "gamethreads.inl"  // cas vlaken hry (hlavni vlakno, render, cekani) - jen mereni
#include "latchain.inl"  // Impulse krok A: retezec latence vstup -> Present -> zobrazeni - jen mereni
#include "fgest.inl"  // krok 0 "FG jen tam, kde se vyplati": jen mereni do logu (radek "odhad FG")
#include "fgpref.inl"  // kroky 1-2: [fg] Preference - kdy adaptivni rezim generuje (rozhodnuti pro helper)

// Mereni pro predpoved zamku (deklarace u FgAdaptLock): posledni ~1 s snimku hry, vsechny generuje na stupni lockWord,
// okno hry v popredi, bez pauzy (> 100 ms). Engine GPU hra + helper na snimek (citace jadra grafiky) aspon v 80 %
// snimku, cas vlaken aspon v 80 %. Hlavni vlakno: interval - cekani na render; se SampleSync spi limiter v pumpe
// zprav hlavniho vlakna, proto tam bez spanku limiteru (busy).
static bool FgLockMeasure(LONGLONG now, uint32_t lockWord, FgLockMeas* o) {
    const LONGLONG from = now - g_qpf.QuadPart;
    unsigned n = 0, nT = 0, nG = 0;
    double sDt = 0.0, sGb = 0.0, sM = 0.0, sR = 0.0, cm = 0.0;
    for (unsigned i = g_feN; i-- > 0 && g_feN - i < (unsigned)kFeRing - 2;) {
        const FeFrame& f = g_fe[i % kFeRing];
        if (f.t < from) break;
        if (f.st != FE_GEN || f.lock != lockWord || !f.fore || f.dt <= 0.0f || f.dt > 100.0f) return false;
        ++n;
        sDt += f.dt;
        if (f.gb >= 0.0f) {
            sGb += f.gb;
            ++nG;
        }
        if (f.tok && f.rc <= 2.0f * f.dt && f.mc <= 2.0f * f.dt) {  // CPU cas obcas nesmyslny (zmena vlakna)
            const double m = (g_ssOn ? (double)f.busy : (double)f.dt) - (double)f.wr;
            sM += std::max(m, (double)f.mc);
            sR += f.rc;
            ++nT;
        }
        if (cm <= 0.0 && f.cm > 0.0f) cm = f.cm;  // nejnovejsi
    }
    if (n < 15 || nG * 10 < n * 8 || nT * 10 < n * 8 || cm <= 0.0) return false;
    o->dt = sDt / n;
    o->cm = cm;
    o->gr = std::max(0.0, sGb / nG - cm);
    o->cpu = std::max(sM / nT, sR / nT);
    return true;
}

static double FgGpuUtil(LONGLONG now) {
    const LONGLONG from = now - g_qpf.QuadPart;
    unsigned n = 0, nG = 0;
    double sDt = 0.0, sGb = 0.0;
    for (unsigned i = g_feN; i-- > 0 && g_feN - i < (unsigned)kFeRing - 2;) {
        const FeFrame& f = g_fe[i % kFeRing];
        if (f.t < from) break;
        if (f.dt <= 0.0f || f.dt > 200.0f) continue;
        ++n;
        if (f.gb >= 0.0f) {
            sDt += f.dt;
            sGb += f.gb;
            ++nG;
        }
    }
    return n >= 10 && nG * 10 >= n * 8 && sDt > 0.0 ? sGb / sDt : -1.0;
}

void FgLimiter() {
    MemoryBarrier();
    // SampleSync (latchain.inl): limiter spal v pumpe zprav hlavniho vlakna -> spanek, bit a nestih z hlaseni hacku
    LONGLONG ssSlept = 0;
    bool ssLate = false;
    const bool ssSlp = g_ssOn && SsLimReport(&ssSlept, &ssLate);
    // spanek hlavniho vlakna v pumpe od minuleho volani, vcetne prave probihajiciho: pumpa spi, zatimco render kresli,
    // takze dokonceny spanek patri k snimku jen nekdy (cas snimku bez spanku pro zamek a odhad FG pak skakal mezi
    // M a celym intervalem a zlomkovy zamek kmital 1/2 <-> 2/5, m_dgas v103b5)
    const LONGLONG ssDelta = g_ssOn ? SsSleptDelta() : 0;
    if (ssSlp) g_stLimSleep.add(Ms(0, ssSlept));
    if (ssLate) ++g_stLimLate;
    g_ssLimLate = ssLate;
    g_ssLimPrev = g_ssLimUsed;  // minuly snimek spal v pumpe (LimLateChk v tomto volani)
    g_ssLimUsed = false;
    if (g_ssLimPrev) g_limSleptQ = ssDelta;
    const LONGLONG sleptQ = g_limSleptQ;
    g_limSleptQ = 0;
    FgEstFrame(Qpc(), sleptQ);
    // Adaptivni FSR: hra vzdy nejvys na cil vystupu (nad nim helper nic nedoplnuje a nesmi prestrelit strop VRR), se
    // [fg] RealCap na strop skutecnych FPS - i kdyz helper prave negeneruje (stav ACTIVE i PAUSED).
    FgAdaptSubstitute();
    const bool adapt = FgIsAdapt((int)g_fgMode);
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
    if (ssSlp) g_limBits |= 1;
    if (!active) {
        g_limNext = 0;
        if (g_adLockWord) FgAdaptLock(nowA, 0.0, false, 0);  // zamek pustit (helper zpet na volny rozvrh)
        return;
    }
    FgResumeGuard(nowA);
    FgAdaptLowBase(nowA, adapt);
    double rate;  // skutecnych snimku za sekundu (pred rezervou)
    // (FgAutoMult je volan z regulatoru rezimu 3 nize)
    if (adapt) {
        rate = g_fgTargetD;
        if (cfg.adaptCap > 0 && cfg.adaptCap < rate) rate = (double)cfg.adaptCap;
        // v103e: stav "jen skutecne" s preferenci a GPU na hrane - strop pod moznostmi GPU (fgpref.inl, PfRealCap)
        if (g_pf.capFps > 0.0 && g_pf.capFps < rate && g_sh->fgState == BMSD_FGS_ACTIVE && !g_sh->adaptEngaged)
            rate = g_pf.capFps;
    } else {
        const uint32_t am = g_sh->fgActiveMult;
        rate = g_fgTargetD / (am >= 2 && am <= 6 ? (double)am : 2.0);
    }
    // Rezerva GPU ([fg] LimiterAdapt): hra, ktera strop N / TargetFps nestiha (GPU na hranici, napr. 4K a 3x), chodi
    // nepravidelne, vlakno presentu helperu je pozadu a generovane snimky zahazuje (trhani, "FG se vypina"). Kdyz za
    // ~1 s nestihne slot vic nez 25 % snimku, strop klesne na 93 % skutecne dosazenych FPS; bez nestihu (< 5 %) se
    // kazde ~2 s vraci o 1 % zpet. Jen pevne nasobky (adaptivni FSR si mezery doplnuje sam).
    const LONGLONG now = Qpc();
    // Zlomkovy zamek adaptivniho FSR: perioda limiteru = perioda mrizky helperu * p / q (0 = bez zamku)
    if (!adapt && g_adLockWord) FgAdaptLock(now, 0.0, false, 0);  // jiny rezim FG: zamek pustit
    const LONGLONG lockPeriod = adapt && (FgLockOn() || g_adLockWord) ? FgAdaptLock(now, rate, true, sleptQ) : 0;
    // Ne pro adaptivni FSR (v24q, overeno 4K): hra zastropovana na ~37 FPS, post -> hotovo dal 64-74 ms - GPU vytizi
    // samo generovani (~4 snimky na skutecny, kazdy zvlast), rezerva jen ubere skutecne FPS a skubani zustane.
    if (cfg.fgLimAdapt == 3 && (!adapt || (cfg.fgLimAdaptFsr && !FgLockOn()))) {
        // Rezim 3 (v24u): regulator podle casu GPU (obdoba Reflexu). Helper meri skutecny cas od Present hry do
        // dokonceni sve prace pro snimek na GPU (BmsdShared::fgGpuDone). Delsi nez interval skutecnych snimku = GPU
        // nestiha a fronta pred ni roste: strop hned dolu (median 5 mereni, ne az po 0,5 s jako rezim 2). Trvale pod
        // 85 % intervalu: strop pomalu nahoru (0,5 % za 1 s, po doznani drzeni). Pojistka jako rezim 2: hra slot
        // nestiha (> 25 % snimku za 0,25 s).
        static LONGLONG s_winQ, s_probeAt, s_lastHit, s_lastCut, s_prevQ, s_startQ;
        static unsigned s_n, s_late, s_logs;
        static uint32_t s_am, s_lastPub;
        static double s_d[5], s_dtEma, s_cutMed, s_bound, s_holdS = 4.0;
        static double s_rgSnap[2], s_rgSave = 1.0;  // ResumeGuard: strop pred Alt+Tabem
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
            s_rgSnap[0] = s_rgSnap[1] = 0.0;  // strop pred Alt+Tabem z jineho nasobku neplati
        }
        // Alt+Tab (v25v, uzivatel: po navratu nizke skutecne FPS, rostou po 1): na pozadi hru brzdi Windows / ovladac,
        // regulator to bral jako nestihajici GPU a strop srazil; zpet jen +0,5 %/s. Na pozadi proto strop nesrazit a po
        // navratu do popredi nebo po pauze FG (limiter nebezel > 0,3 s) zacit znovu od plneho stropu.
        // ResumeGuard (v102k): navrat pozna FgResumeGuard; strop se vrati na hodnotu z doby pred Alt+Tabem a v ochranne
        // dobe se nic nesrazi ani nezvysuje (driv 100 %, 1,5 s klid, pak srazeni podle zpomalene hry a +2 %/s zpet).
        static LONGLONG s_lastRunQ, s_fgChkQ, s_focusQ, s_rgSnapQ;
        static bool s_focus = true, s_rgHave, s_rgWasProt;
        static unsigned s_rgSeen;
        const bool rg = cfg.fgResumeGuard > 0;
        if (rg) {
            if (!g_rgBg && !g_rgProt && !s_rgHave && (!s_rgSnapQ || now - s_rgSnapQ >= q1s / 2)) {
                s_rgSnapQ = now;  // strop 0,5-1 s zpet (nejnovejsi uz muze byt srazeny zpomalenou hrou)
                s_rgSnap[1] = s_rgSnap[0];
                s_rgSnap[0] = g_limScale;
            }
            if (!s_rgHave && (g_rgBg || g_rgEvent != s_rgSeen)) {  // ztrata popredi / pauza: zapamatovat strop
                s_rgSave = std::max(s_rgSnap[1], g_limScale);
                s_rgHave = true;
            }
            if (g_rgEvent != s_rgSeen) {
                s_rgSeen = g_rgEvent;
                Log("prezentace: regulator FG - navrat do hry, strop skutecnych %.0f %% -> %.0f %% (pred Alt+Tabem), "
                    "v ochranne dobe nic nesrazet",
                    g_limScale * 100.0, s_rgSave * 100.0);
                g_limScale = g_limScaleLogged = s_rgSave;
                s_rgHave = false;
                s_dn = 0;
                s_startQ = now;
                s_winQ = now;
                s_n = s_late = 0;
            }
            if (s_rgWasProt && !g_rgProt && !g_rgBg) {  // ochrana skoncila: merit znovu
                s_dn = 0;
                s_winQ = now;
                s_n = s_late = 0;
            }
            s_rgWasProt = g_rgProt;
        } else if (!s_fgChkQ || now - s_fgChkQ > q1s / 4 || (s_lastRunQ && now - s_lastRunQ > q1s * 3 / 10)) {
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
            if (!rg && s_lastRunQ && now - s_lastRunQ > q1s * 3 / 10 && g_limScale < 1.0) {
                Log("prezentace: regulator FG - po pauze (%.1f s) strop skutecnych %.0f %% -> 100 %%",
                    (double)(now - s_lastRunQ) / (double)q1s, g_limScale * 100.0);
                g_limScale = g_limScaleLogged = 1.0;
            }
        }
        s_lastRunQ = now;
        // Zaskub (nacitani, HDD: snimek nad 200 ms nebo nad 2x prumer) neni "hra nestiha slot": do podilu pozdnich
        // snimku se nepocita, jinak by kazde ridke nacitani srazilo strop skutecnych az na 40 % (audit c. 13)
        bool hitch = false;
        if (s_prevQ) {
            const double dt = Ms(s_prevQ, now);
            hitch = dt >= 200.0 || (s_dtEma > 0.0 && dt > 2.0 * s_dtEma);
            if (dt > 0.0 && dt < 200.0) s_dtEma = s_dtEma > 0.0 ? s_dtEma + 0.1 * (dt - s_dtEma) : dt;
        }
        s_prevQ = now;
        ++s_n;
        if (LimLateChk(now, (LONGLONG)((double)q1s / (rate * g_limScale))) && !hitch) ++s_late;
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
        // rucni cil nad stropem VRR (v103h): rate je strop, ktery hra nemusi ani zdaleka dat (482 / 2 = 241) -> jako adaptivni
        const double minScale = adapt || g_fgFree ? std::min(0.4, 20.0 / rate) : 0.4;
        const bool focusSettle = s_focusQ && now - s_focusQ < q1s * 3 / 2;
        // pozadi / ochrana po navratu: strop nemenit (rg), jinak jako v25y
        const bool hold = rg ? g_rgBg || g_rgProt : !s_focus || focusSettle;
        // v103h: s rucnim cilem nad stropem VRR drzet GPU do ~85 % (nad tim cekaji kopie stovek obrazu za sekundu a vystup
        // je nerovnomerny); bez citacu GPU jako driv podle casu "GPU hotovo"
        const double util = g_fgFree && !adapt ? FgGpuUtil(now) : -1.0;
        const bool gpuHit = !hold && s_dn >= 5 && (util >= 0.0 ? util > 0.88 : ratio > 1.0) && g_limScale > minScale && !draining && !settling;
        const bool lateHit = !hold && winMs >= 250.0 && lateF > 0.25 && fps > 10.0;
        if (cool && (gpuHit || lateHit)) {
            // Mirne nad intervalem (do 130 %): jen -3 % (cas hotovo kolisa se scenou). Vic, nebo hra uz slot nestiha:
            // podle pomeru / dosazenych FPS (nejvys -15 % naraz).
            double ns = g_limScale * (gpuHit && ratio < 1.3 ? 0.97 : std::max(0.85, std::min(0.97, 0.95 / ratio)));
            if (util >= 0.0) ns = g_limScale * std::max(0.85, std::min(0.98, 0.84 / std::max(0.5, util)));
            if ((lateHit || ratio >= 1.3) && fps > 10.0) ns = std::min(ns, (util > 0.9 ? 0.95 : 0.99) * fps / rate);
            // v102l (audit: 1.0.0 / 1.0.1 strop 13 % = 10 FPS po Alt+F9): dolni mez i pro nestih slotu - strop nad
            // moznostmi hry jen nic neomezi, strop pod nimi hru drzi dole
            ns = std::max(minScale, ns);
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
        } else if (g_limScale < 1.0 && s_dn >= 5 && (util >= 0.0 ? util < 0.80 : ratio < 0.85) && lateF < 0.05 && !(rg && hold)) {
            if (g_limScale < s_bound - 0.025 && cool && now - s_lastCut > q1s / 2) {
                // pod znamou hranici (po vetsim srazeni): rychle zpet na 2 % pod ni; s velkou rezervou GPU (< 60 %
                // intervalu, napr. po jednorazovem zaseknuti) rovnou (v25x, driv +1 % po 0,5 s = 50 -> 95 % za ~25 s)
                const double step = util >= 0.0 ? 0.7 * g_limScale * (0.84 / std::max(0.3, util) - 1.0)
                                    : ratio > 0.0 && ratio < 0.6 ? g_limScale * (0.8 / ratio - 1.0) : 0.01;
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
        if (LimLateChk(now, (LONGLONG)((double)q1s / (rate * g_limScale)))) ++s_late;
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
        if (LimLateChk(now, (LONGLONG)((double)g_qpf.QuadPart / (rate * g_limScale)))) ++s_late;
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
    const LONGLONG period = lockPeriod ? lockPeriod
                                       : (LONGLONG)((double)g_qpf.QuadPart / rate * (cfg.fgTargetFps > 0 ? 1.0 : 1.0 + cfg.fgMarginPct / 100.0));
    g_limRateLast = period > 0 ? (double)g_qpf.QuadPart / (double)period : 0.0;
    {  // samokontroly: limiter bezel, nejvyssi strop intervalu (skutecne FPS a po vynasobeni FPS vystupu)
        ++g_scLimN;
        const uint32_t amSc = adapt ? 1u : g_sh->fgActiveMult;
        const double outSc = g_limRateLast * (double)(amSc >= 2u && amSc <= 6u ? amSc : adapt ? 1u : 2u);
        if (g_limRateLast > g_scLimRateMax) g_scLimRateMax = g_limRateLast;
        if (outSc > g_scLimOutMax) g_scLimOutMax = outSc;
    }
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
    // SampleSync: spat az v pumpe zprav pred odberem vstupu (konec slotu snimku f prospi pumpa se znackou f); nestih
    // a spanek hlasi hacek (zacatek funkce). Faze mrizky zustava tady.
    if (SsReady()) {
        SsSetAbs(g_limNext);
        g_ssLimUsed = true;
        return;
    }
    if (now >= g_limNext) {  // snimek slot nestihl: nespat, fazi drzet
        ++g_stLimLate;
        return;
    }
    LimiterSleepUntil(g_limNext);
    const LONGLONG woke = Qpc();
    g_limSleptQ = woke - now;
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
        Log("config [fg]: WARNING: the previous run ended with an error (%s) - Frame Generation (%s) is off for this "
            "session; it can be turned on again in the menu ([fg] SafeStart=0 disables this safeguard)",
            g_safeStartWhy, FgModeName(fgMode));
        fgMode = BMSD_FG_OFF;
        g_safeStartHit = true;
    }
    g_fgMode = fgMode;
    g_scOn = GetPrivateProfileIntW(L"debug", L"SelfCheck", 1, ini) != 0;
    g_gtOn = GetPrivateProfileIntW(L"debug", L"GameThreads", 1, ini) != 0;
    g_lcOn = GetPrivateProfileIntW(L"debug", L"LatChain", 1, ini) != 0;
    g_lcCsv = GetPrivateProfileIntW(L"debug", L"LatChainCsv", 0, ini) != 0;
    {
        const int ss = (int)GetPrivateProfileIntW(L"latency", L"SampleSync", 0, ini);
        g_ssOn = ss == 1 || ss == 2;
        g_ssReg = ss == 2;
    }
    if (g_ssOn) {
        if (!g_lcOn) g_lcOn = true;  // znacka pumpy zprav je soucast mereni latence vstupu
        if (!g_gtOn) g_gtOn = true;  // hacek pumpy vstupu (PeekMessageA v inputsystem.dll) je v gamethreads.inl
        Log("config [latency]: SampleSync=%d - spanek limiteru FG%s v pumpe zprav hlavniho vlakna (pred odberem vstupu), "
            "zaloha spanek po Present", g_ssReg ? 2 : 1, g_ssReg ? " a regulatoru latence (pokus)" : "");
    }
    g_feCsvInit = GetPrivateProfileIntW(L"debug", L"FgEstCsv", 0, ini) != 0;
    g_scTest = (int)GetPrivateProfileIntW(L"debug", L"SelfCheckTest", 0, ini);
    GetPrivateProfileStringW(L"fg", L"TargetFps", L"0", fgBuf, 64, ini);
    cfg.fgTargetFps = (int)wcstol(fgBuf, nullptr, 0);
    if (cfg.fgTargetFps < 0) cfg.fgTargetFps = 0;
    if (cfg.fgTargetFps > kFgTargetMax) {
        Log("config [fg]: TargetFps=%d je nad mezi %d - plati %d", cfg.fgTargetFps, kFgTargetMax, kFgTargetMax);
        cfg.fgTargetFps = kFgTargetMax;
    }
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
    {  // 0 / 1; "auto" nebo chybejici klic = vychozi podle rezimu (FgLockOn)
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"fg", L"AdaptLock", L"auto", v, 16, ini);
        const wchar_t* q = v;
        while (*q == L' ' || *q == 9) ++q;
        cfg.adaptLock = *q >= L'0' && *q <= L'9' ? (wcstol(q, nullptr, 10) != 0 ? 1 : 0) : -1;
    }
    cfg.adaptLockDen = (int)GetPrivateProfileIntW(L"fg", L"AdaptLockDen", 6, ini);
    cfg.adaptMinFps = (int)GetPrivateProfileIntW(L"fg", L"AdaptMinFps", 40, ini);
    if (cfg.adaptMinFps < 0 || cfg.adaptMinFps > 500) cfg.adaptMinFps = 40;
    cfg.adaptLowMult = (int)GetPrivateProfileIntW(L"fg", L"AdaptLowMult", 3, ini);
    if (cfg.adaptLowMult < 2 || cfg.adaptLowMult > 6) cfg.adaptLowMult = 3;
    if (cfg.adaptLockDen < 2 || cfg.adaptLockDen > 8) cfg.adaptLockDen = 6;
    {  // [fg] Preference: nazev profilu, nebo cislo N (= vlastni, generovat pod N % cile); v103f2 prahy v % cile
        wchar_t v[32] = L"";
        GetPrivateProfileStringW(L"fg", L"Preference", L"balanced", v, 32, ini);
        cfg.prefPct = (int)GetPrivateProfileIntW(L"fg", L"PreferencePct", 75, ini);
        if (cfg.prefPct < 20 || cfg.prefPct > 95) {  // v103f (D3): na mez, ne na vychozi
            const int c = cfg.prefPct < 20 ? 20 : 95;
            Log("config [fg]: PreferencePct=%d mimo 20-95 - plati %d", cfg.prefPct, c);
            cfg.prefPct = c;
        }
        const int p = PrefParse(v, &cfg.prefPct);
        if (p < 0) Log("config [fg]: Preference=%ls neznama - plati balanced", v);
        cfg.prefMode = p < 0 ? PREF_BAL : p;
        cfg.prefHyst = (int)GetPrivateProfileIntW(L"fg", L"PreferenceHyst", 10, ini);
        if (cfg.prefHyst < 3 || cfg.prefHyst > 40) cfg.prefHyst = 10;
        if (cfg.prefHyst * 100 < 8 * cfg.prefPct)  // v103f (D1): chyba odhadu B^ 8-13 % (kap. 10.2 bod 2)
            Log("config [fg]: PreferenceHyst=%d bodu je pod chybou odhadu FPS hry (8-13 %%) - muze prepinat sem a tam", cfg.prefHyst);
        GetPrivateProfileStringW(L"fg", L"PreferenceMinGain", L"", v, 32, ini);
        const wchar_t* q = v;
        while (*q == L' ' || *q == 9) ++q;
        cfg.prefMinGain = *q >= L'0' && *q <= L'9' ? (int)wcstol(q, nullptr, 10) : -1;
        if (cfg.prefMinGain > 100) cfg.prefMinGain = 100;
    }
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
    cfg.offOwn = GetPrivateProfileIntW(L"fg", L"OffWindow", 0, ini) == 1;
    cfg.healComposed = GetPrivateProfileIntW(L"fg", L"HealComposed", 1, ini) == 1;
    if (cfg.offOwn) cfg.offChild = true;
    // FG zapnuta za behu: swapchain presenteru jednou znovu vytvorit tolik ms po prvnim (proti zamrzlemu obrazu,
    // kdyz DWM swapchain po DXVK v okne hry nezobrazi). 0 = vypnuto.
    GetPrivateProfileStringW(L"fg", L"LiveRefreshMs", L"800", fgBuf, 64, ini);
    cfg.liveRefreshMs = (DWORD)wcstoul(fgBuf, nullptr, 0);
    GetPrivateProfileStringW(L"fg", L"LimiterAdapt", L"3", fgBuf, 64, ini);  // vychozi 3 (v24w: overeno 4K 2x-4x)
    cfg.fgLimAdapt = (int)wcstol(fgBuf, nullptr, 0);
    GetPrivateProfileStringW(L"fg", L"LimiterAdaptFsr", L"0", fgBuf, 64, ini);
    cfg.fgLimAdaptFsr = (int)wcstol(fgBuf, nullptr, 0);
    cfg.fgResumeGuard = GetPrivateProfileIntW(L"fg", L"ResumeGuard", 8, ini);
    if (cfg.fgResumeGuard < 0 || cfg.fgResumeGuard > 30) cfg.fgResumeGuard = 8;
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
    cfg.gpPrio = (int)GetPrivateProfileIntW(L"fg", L"GamePresentPriority", 0, ini);
    if (cfg.gpPrio < 0 || cfg.gpPrio > 2) cfg.gpPrio = 0;
    if (cfg.gpPrio) g_gpDebug |= cfg.gpPrio == 2 ? 8192 : 4096;  // gamepresent.inl: VK_*_global_priority pri vytvoreni zarizeni
    cfg.prefCap = GetPrivateProfileIntW(L"debug", L"PrefCap", 1, ini) != 0;
    cfg.prefBp = GetPrivateProfileIntW(L"debug", L"PrefBackpressure", 1, ini) != 0;
    cfg.prefBpReg = GetPrivateProfileIntW(L"debug", L"PrefBpReg", 0, ini) != 0;
    if (cfg.prefBpReg) Log("config [debug]: PrefBpReg=1 - regulator latence pocita cekani zpetneho tlaku (spi pred dalsim snimkem)");
    if (!cfg.prefBp) Log("config [debug]: PrefBackpressure=0 - stav jen skutecne bez zpetneho tlaku (presenter muze preskakovat)");
    if (!cfg.prefCap) Log("config [debug]: PrefCap=0 - stav jen skutecne s preferenci bez stropu (test)");
    {
        wchar_t kb[32];
        GetPrivateProfileStringW(L"debug", L"PrefCapK", L"1.0", kb, 32, ini);
        const double k = wcstod(kb, nullptr);
        cfg.prefCapK = k >= 0.85 && k <= 1.0 ? k : 1.0;
        if (cfg.prefCapK < 0.999)
            Log("config [debug]: PrefCapK=%.2f - stav jen skutecne se stropem %.0f %% moznosti GPU (nizsi latence)", cfg.prefCapK,
                cfg.prefCapK * 100.0);
    }
    const bool s2 = num(L"HelperPresent", 0) != 0;
    cfg.presAtStart = fgMode != BMSD_FG_OFF || fgForce;
    cfg.inMain = cfg.presAtStart || (cfg.live && !s2);
    if (fgIni != BMSD_FG_OFF && !FgModeImplemented(fgIni))  // (ne kvuli bezpecnemu startu - ten ma vlastni radek)
        Log("config [fg]: Mode=%s zatim neni implementovano - Frame Generation vypnuto", FgModeName(fgIni));
    cfg.on = s2 || cfg.inMain;
    if (!cfg.on) return;  // vypnuto: nic, ani log
    cfg.startTimeoutMs = (DWORD)num(L"StartTimeoutMs", cfg.inMain ? 30000 : 10000);
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
    Log("config [debug]: SelfCheck=%d (%s)", (int)g_scOn,
        g_scOn ? "samokontroly zapnute: poruseni se zapise do logu, kazda kontrola nejvys jednou za 60 s"
               : "samokontroly vypnute");
    if (g_scTest)
        Log("config [debug]: SelfCheckTest=%d - TEST, zamerne chyby pro overeni samokontrol:%s%s%s", g_scTest,
            g_scTest & 1 ? " TargetFps bez omezeni stropem zobrazeni;" : "",
            g_scTest & 2 ? " helper nevypina zbytecne generovani;" : "",
            g_scTest & 4 ? " zamek na nejnizsim stupni;" : "");
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
            cfg.fgTargetFps ? " (presne)" : " (auto: f - f^2/3600)",
            (int)cfg.fgPauseInMenu, cfg.fgMarginPct, (int)cfg.live,
            cfg.presAtStart ? "" : cfg.offChild ? (cfg.offOwn ? " (FG vypnuta: DXVK do dcerineho okna proxy, OffWindow=1)"
                                                            : " (FG vypnuta: DXVK do dcerineho okna proxy, OffTarget=1)")
                                                : " (FG vypnuta: DXVK primo do okna hry, FG jde zapnout za behu)",
            (int)cfg.gamePresent, cfg.gamePresent ? "snimky FG zobrazuje proces hry - VRR" : "zobrazuje helper - bez VRR",
            (int)cfg.gamePresentEarly);
        Log("config [fg] adaptivni (FSR / DLSS-G): RealCap=%d%s FillBelow=%d%s (rozvrh A) AdaptLock=%d%s (zlomkovy zamek, "
            "jmenovatel nejvys %d)",
            cfg.adaptCap, cfg.adaptCap ? "" : " (vyp)", cfg.adaptFill, cfg.adaptFill ? "" : " (auto 90 % cile)",
            (int)FgLockOn(), cfg.adaptLock < 0 ? " (vychozi rezimu)" : "", cfg.adaptLockDen);
        Log("config [fg]: LimiterAdapt=%d LimiterAdaptFsr=%d ResumeGuard=%d%s", cfg.fgLimAdapt, cfg.fgLimAdaptFsr,
            cfg.fgResumeGuard, cfg.fgResumeGuard ? " s (po Alt+Tab strop / stupen z doby pred nim, nic nesrazet)" : " (vyp)");
        Log("config [fg] adaptivni: AdaptMinFps=%d%s AdaptLowMult=%d", cfg.adaptMinFps,
            cfg.adaptMinFps ? " (zamek nejde pod, hra pod ni = cil vystupu nejvys AdaptLowMult x skutecne)" : " (vyp)",
            cfg.adaptLowMult);
        Log("config [fg] adaptivni: Preference=%s%s PreferencePct=%d %% cile PreferenceHyst=%d bodu PreferenceMinGain=%d%s", PrefName(cfg.prefMode),
            cfg.prefMode == PREF_ALWAYS ? " (doplnovat do cile jako do v103d)" : " (generovat jen pod prahem FPS hry bez generovani, v % cile)",
            cfg.prefPct, cfg.prefHyst, cfg.prefMinGain, cfg.prefMinGain < 0 ? " (podle profilu)" : " %");
        if (cfg.adaptFill > 0 && cfg.prefMode != PREF_ALWAYS)  // v103f (D6): FillBelow z v103d jinak tise nic nedela
            Log("config [fg]: FillBelow=%d plati jen s Preference=always - ted rozhoduje Preference=%s", cfg.adaptFill,
                PrefName(cfg.prefMode));
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
int Present_TakeFallbackNote() { return (int)InterlockedExchange(&g_fbNote, 0); }
int Present_TakeFgSubstNote() { return (int)InterlockedExchange(&g_fgSubstNote, 0); }
bool Present_FgPresenterActive() { return cfg.inMain && !g_broken && !g_liveBlock && g_state != PS_FALLBACK; }
// Zaloha jen z casoveho limitu startu: FG jde v menu / Alt+F9 vypnout a zapnout = novy pokus (FallbackToOff)
bool Present_FgRetryable() { return cfg.inMain && !g_broken && g_state == PS_FALLBACK && g_fbRetry; }

bool Present_FgModeAvailable(int mode) {
    if (mode == BMSD_FG_OFF) return true;
    if (!FgModeImplemented(mode)) return false;
    if (cfg.inMain && g_sh && g_state != PS_FALLBACK && g_sh->fgAvailValid) return ((g_sh->fgAvailMask >> mode) & 1) != 0;
    return true;  // helper se jeste nevyjadril (nebo presenter nebezi): nabidnout, stav rekne az po zapnuti
}

void Present_FgSetPause(bool pause) {
    g_menuOpenNow = pause;
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
    if (FgIsAdapt(mode)) return 2;
    if (mode == BMSD_FG_FSR2X) {  // FSR FG od AMD jen 2x; 3x-6x vlastni generator AFG (v25n, 6x od v103h), kdyz ho helper nabizi
        if (cfg.inMain && g_sh && g_state != PS_FALLBACK && g_sh->fgAvailValid &&
            !((g_sh->fgAvailMask >> BMSD_FG_FSRADAPT) & 1))
            return 2;
        return 6;
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
        g_fpsc.real = FgIsAdapt((int)g_fgMode) ? (float)(g_fpsc.presents * 1000.0 / el) : (float)(dr * 1000.0 / el);
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

double Present_BpWaitMs() { return cfg.prefBpReg ? g_bpLastMs : 0.0; }

bool Present_FpsGet(float* real, float* total, bool* split) {
    if (!g_fpsc.valid) return false;
    *real = g_fpsc.real;
    *total = g_fpsc.total;
    *split = g_fpsc.split;
    return true;
}

// ---- v103g: diagnosticky prekryv (data). Snimky citacu presenteru po ~250 ms (8 = ~2 s).
static struct {
    LONGLONG q[8];
    uint32_t sk[8], pr[8];
    unsigned n;
    LONGLONG lastTick;
} g_dgSnap;

void Present_DiagTick(bool on) {
    g_dgOn = on;
    if (!on) return;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    const LONGLONG now = Qpc();
    if (g_dgSnap.lastTick && now - g_dgSnap.lastTick > g_qpf.QuadPart * 2) g_dgSnap.n = 0;  // prekryv byl vypnuty
    g_dgSnap.lastTick = now;
    DgPush(g_dgGame, now);
    const unsigned last = g_dgSnap.n ? (g_dgSnap.n - 1) % 8u : 0u;
    if (g_sh && (!g_dgSnap.n || now - g_dgSnap.q[last] >= g_qpf.QuadPart / 4)) {
        const unsigned i = g_dgSnap.n % 8u;
        g_dgSnap.q[i] = now;
        g_dgSnap.sk[i] = g_sh->gpSkipped;
        g_dgSnap.pr[i] = g_sh->gpPresented;
        ++g_dgSnap.n;
    }
}

bool Present_DiagGet(PresentDiag* o) {
    o->n = 0;
    o->gameFps = o->monFps = o->ivMed = o->ivSd = 0.0f;
    o->skipped = o->shown = -1;
    o->presenter = false;
    if (!g_qpf.QuadPart) return false;
    const LONGLONG now = Qpc(), q1 = g_qpf.QuadPart;
    auto rate = [&](const DgRing& r, LONGLONG* newest) {
        const LONG n = r.n;
        unsigned c = 0;
        *newest = 0;
        for (LONG i = n - 1; i >= 0 && i >= n - (LONG)kDgRing + 1; --i) {
            const LONGLONG t = r.t[(unsigned)i % kDgRing];
            if (!*newest) *newest = t;
            if (now - t > q1) break;
            ++c;
        }
        return c;
    };
    LONGLONG gNew = 0, mNew = 0;
    const unsigned gc = rate(g_dgGame, &gNew), mc = rate(g_dgMon, &mNew);
    o->presenter = mNew && now - mNew < q1 / 4 && cfg.inMain && g_state == PS_HELPER;
    o->gameFps = (float)gc;
    o->monFps = (float)(o->presenter ? mc : gc);
    const DgRing& r = o->presenter ? g_dgMon : g_dgGame;
    // intervaly za posledni ~2 s (nejvys 200), nejnovejsi posledni; median a sd za posledni ~1 s
    const LONG n = r.n;
    float tmp[200];
    int k = 0;
    LONGLONG prev = 0;
    for (LONG i = n - 1; i >= 1 && i >= n - (LONG)kDgRing + 2 && k < 200; --i) {
        const LONGLONG t = r.t[(unsigned)i % kDgRing], tp = r.t[(unsigned)(i - 1) % kDgRing];
        if (now - t > 2 * q1 || t <= tp) break;
        tmp[k++] = (float)Ms(tp, t);
        prev = t;
    }
    (void)prev;
    for (int i = 0; i < k; ++i) o->iv[i] = tmp[k - 1 - i];
    o->n = k;
    std::vector<float> w;
    double s = 0.0, s2 = 0.0;
    double acc = 0.0;
    for (int i = k - 1; i >= 0 && acc < 1000.0; --i) {
        w.push_back(o->iv[i]);
        acc += o->iv[i];
        s += o->iv[i];
        s2 += (double)o->iv[i] * o->iv[i];
    }
    o->ivShort = o->ivLong = 0;
    if (!w.empty()) {
        std::nth_element(w.begin(), w.begin() + w.size() / 2, w.end());
        o->ivMed = w[w.size() / 2];
        const double m = s / (double)w.size();
        o->ivSd = (float)sqrt(std::max(0.0, s2 / (double)w.size() - m * m));
        for (float v : w) {
            if (v < 0.75f * o->ivMed) ++o->ivShort;
            else if (v > 1.33f * o->ivMed) ++o->ivLong;
        }
    }
    // preskoceno / zobrazeno presenterem za ~1 s (nejstarsi snimek citacu aspon 0,75 s stary)
    if (o->presenter && g_dgSnap.n >= 2) {
        const unsigned cur = (g_dgSnap.n - 1) % 8u;
        for (unsigned j = 1; j < 8u && j < g_dgSnap.n; ++j) {
            const unsigned i = (g_dgSnap.n - 1 - j) % 8u;
            if (g_dgSnap.q[cur] - g_dgSnap.q[i] >= q1 * 3 / 4 || j == 7u || j == g_dgSnap.n - 1) {
                const double el = (double)(g_dgSnap.q[cur] - g_dgSnap.q[i]) / (double)q1;
                if (el > 0.2 && g_dgSnap.sk[cur] >= g_dgSnap.sk[i] && g_dgSnap.pr[cur] >= g_dgSnap.pr[i]) {
                    o->skipped = (int)lround((g_dgSnap.sk[cur] - g_dgSnap.sk[i]) / el);
                    o->shown = (int)lround((g_dgSnap.pr[cur] - g_dgSnap.pr[i]) / el);
                }
                break;
            }
        }
    }
    return gc > 0;
}

void Present_FgSetTargetFps(int fps) {
    cfg.fgTargetFps = fps > 0 ? std::min(fps, kFgTargetMax) : 0;
    UpdateFgTarget();
}

int Present_FgGetRealCap() { return cfg.adaptCap; }
void Present_FgSetRealCap(int fps) {
    cfg.adaptCap = fps > 0 && fps <= 1000 ? fps : 0;
    FgPublishAdapt();
}
int Present_FgGetPref() { return cfg.prefMode; }
void Present_FgSetPref(int pref) {
    cfg.prefMode = pref >= 0 && pref < PREF_COUNT ? pref : PREF_BAL;  // plati hned (prahy), prepnuti az po t_on / t_off
}
int Present_FgGetPrefPct() { return cfg.prefPct; }
void Present_FgSetPrefPct(int pct) { cfg.prefPct = pct >= 20 && pct <= 95 ? pct : 75; }
void Present_FgPrefInfo(PresentFgPref* o) {
    *o = PresentFgPref();
    const auto& P = g_pf;
    o->pref = cfg.prefMode;
    o->prof = cfg.prefMode == PREF_ALWAYS ? PREF_ALWAYS : P.prof;
    o->why = FgIsAdapt((int)g_fgMode) ? (cfg.prefMode == PREF_ALWAYS ? PFI_ALWAYS : P.why) : PFI_OFF;
    o->gen = cfg.inMain && g_sh && g_state == PS_HELPER && g_sh->fgState == BMSD_FGS_ACTIVE && g_sh->adaptEngaged;
    o->base = (float)P.B;
    o->nOn = (float)P.nOn;
    o->nOff = (float)P.nOff;
    o->gain = P.gainKnown ? (float)(P.gain * 100.0) : 0.0f;
    o->gainMin = P.gainRule ? (float)(P.g * 100.0) : -1.0f;
    o->target = (float)P.F;
    o->rough = o->gen && P.est && !P.cpu && P.kgn < 2;
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
    o->adaptTarget = (float)FgTgtEff();
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
    if (cfg.adaptIdleSkip && FgIsAdapt((int)g_fgMode) && st == BMSD_FGS_ACTIVE && !g_sh->adaptEngaged) return nullptr;
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

// Zaloha z casoveho limitu startu (g_fbRetry) a hrac FG v menu vypnul: zpet do PS_OFF, jako by FG byla vypnuta
// normalne - dalsi zapnuti FG je novy pokus (RestartHelper). Jen kdyz helper zije a okno hry je volne pro DXVK.
static bool FallbackToOff() {
    if (!g_proc || WaitForSingleObject(g_proc, 0) != WAIT_TIMEOUT) {
        g_fbRetry = false;
        return false;
    }
    if (!g_direct && !g_fbOffWin) return false;  // okno hry zatim obsazene (sonda zalohy bezi) - zkusit pri dalsim snimku
    g_fbRetry = false;
    if (g_vkReady && FinishAll()) {
        DestroyImages();
        ReleaseHudlessRT();
    } else {
        g_w = g_h = 0;  // pri dalsim zapnuti nove sdilene obrazy (helper svuj import uvolnil)
    }
    g_state = PS_OFF;
    g_limNext = 0;
    EnterOffTarget();
    Log("prezentace: Frame Generation turned off after a start timeout - turning it on again starts a new attempt");
    return true;
}

// FG zapnuta za behu: presenter startuje, az ma DXVK na okne hry swapchain bez flipu a jednou do nej prezentovalo
// (nejvys 1 s - hra zrovna nekresli na obrazovku; pak jako driv). DXVK zatim kresli dal do okna hry.
static bool DxFlipReleased() {
    if (g_dxGameFlip < 0) return true;  // na okne hry zadny swapchain DXVK
    if (!g_dxRel) {
        g_dxRel = 1;
        g_dxRelQpc = Qpc();
    }
    const double ms = Ms(g_dxRelQpc, Qpc());
    const bool ok = g_dxGameFlip == 0 && g_dxGamePres >= 1;
    if (!ok && ms < 1000.0) return false;
    g_dxRel = 0;
    Log("prezentace: flip DXVK na okne hry %s za %.0f ms, presenter startuje", ok ? "uvolnen" : "NEUVOLNEN", ms);
    return true;
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
    if (g_state == PS_FALLBACK) {
        if (DxSwapOn()) InterlockedExchange(&g_dxWantFlip, g_atLostTop ? 0 : 1);  // presenter nebezi: zase s flipem
        g_dxRel = 0;
        // Automaticky novy pokus po zaloze z pomalosti: pres PS_OFF, dalsi snimek FG zapne (StartPresenter, restart)
        const bool want = PresenterWanted();
        const bool autoDue = g_fbRetry && want && g_fbAutoAt && GetTickCount64() >= g_fbAutoAt;
        if (!g_fbRetry || (want && !autoDue) || !FallbackToOff()) return FallbackTarget();
        if (autoDue) {
            g_fbAutoAt = 0;
            ++g_fbAutoN;
            Log("prezentace: automaticky novy pokus o FG (%u/%u)", g_fbAutoN, kFbAutoMax);
        }
        return OffTarget();
    }
    const bool want = PresenterWanted();
    if (DxSwapOn()) InterlockedExchange(&g_dxWantFlip, want || g_atLostTop ? 0 : 1);
    if (!want) g_dxRel = 0;
    if (g_state == PS_IDLE || g_state == PS_OFF) {
        if (!want) {  // FG vypnuta: DXVK primo do okna hry (prvni snimek bez FG pri startu = rovnou PS_OFF)
            // OffWindow, start bez FG: prvni 2 snimky do skryteho okna zarizeni (swapchain DXVK na nem vznikne hned
            // jako pri startu s FG) a hned i VkDevice presenteru FG. Vytvorene az pri zapnuti FG za behu (DXVK uz
            // flipuje do sveho okna) = presenter po celou dobu FG jen pres kompozici (PresentMon "Composed: Copy with
            // GPU GDI", ~167 zobrazenych/s misto 156 flipu, bez VRR) - bylo tak uz pred v102v.
            static int s_warm;
            if (g_state == PS_IDLE && (cfg.offOwn || DxSwapOn()) && s_warm < 2) {
                if (++s_warm == 2 && cfg.inMain && cfg.gamePresent && !g_gpDevTried && (g_vkReady || InitVk(d))) {
                    g_gpDevTried = true;
                    g_gpDevOk = GpInitDevice();
                    Log("prezentace: zarizeni presenteru FG pripraveno uz pri startu bez FG: %s", g_gpDevOk ? "ano" : "ne");
                }
                return nullptr;
            }
            if (g_state == PS_IDLE) {
                g_state = PS_OFF;
                EnterOffTarget();
                Log("prezentace: FG vypnuta - DXVK prezentuje %s; FG jde zapnout za behu",
                    g_direct ? "primo do okna hry" : "do dcerineho okna proxy");
            }
            return OffTarget();
        }
        if (g_state == PS_OFF && DxSwapOn() && !DxFlipReleased()) return OffTarget();
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
    // [fg] OffWindow: presenter uz zobrazuje (jeho okno je nad oknem DXVK) - okno DXVK zmensit na 1x1 a DXVK do nej
    // posle jeste 2 snimky: swapchain si znovu vytvori 1x1 a ten celoobrazovkovy uvolni (jinak presenter jen "Legacy
    // Copy to front buffer"). Pak skryt; pri dalsim vypnuti FG ho SyncChild vrati na celou plochu.
    if (g_offShrink && g_state == PS_HELPER) {
        if (!g_child || !g_ctl) {
            g_offShrink = 0;
        } else if (g_offShrink <= 2) {
            DWORD_PTR res = 0;
            if (g_offShrink == 1)
                SendMessageTimeoutW(g_ctl, WMP_SHOW, (WPARAM)g_child, (LPARAM)-1, SMTO_NORMAL, 500, &res);
            ++g_offShrink;
            return g_child;
        } else {
            DWORD_PTR res = 0;
            SendMessageTimeoutW(g_ctl, WMP_SHOW, (WPARAM)g_child, SW_HIDE, SMTO_NORMAL, 500, &res);
            g_offShrink = 0;
            InterlockedExchange(&g_gpRefreshKick, 1);  // presenter: swapchain hned znovu (uz s flipem)
            Log("prezentace: dcerine okno DXVK %p zmenseno na 1x1 a skryto (swapchain DXVK bez celoobrazovkoveho flipu)",
                (void*)g_child);
        }
    }
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

// Mereni latence (src/latchain.inl) pro proxy.cpp a menu.inl
bool Lc_OnPumpMsg(MSG* m, bool mouse) { return LcOnPumpMsg(m, mouse); }
void Lc_First() { LcFirst(); }
void Lc_PresentEnter(LONGLONG tSubmit, float camDeg) { LcPresentEnter(tSubmit, camDeg); }
void Lc_GpuFrame(LONGLONG tSubmit, double gpuTsMs, LONGLONG tDone) { LcGpuFrame(tSubmit, gpuTsMs, tDone); }
// SampleSync (latency.inl): regulator latence spi v pumpe zprav misto po Present (false = spat po Present jako driv)
bool Ss_Delay(double ms) {
    if (!g_ssReg || !SsReady()) return false;
    SsSetDelayMs(ms);
    return true;
}

void Present_FrameReleased() {
    GtRelease();
    LcRelease();
}


// ---------------------------------------------------------------------------------------------------------------
// Swapchain DXVK a celoobrazovkovy flip ovladace ([present] DxvkFse). DXVK 3.x zadava vkCreateSwapchainKHR s
// VkSurfaceFullScreenExclusiveInfoEXT; v okne (zarizeni hry je vzdy v okne, viz Present_PrepareCreate) s hodnotou
// DISALLOWED. Ovladac NVIDIA pak obraz okna pres celou obrazovku nikdy neflipuje primo ("Hardware: Legacy Flip"), jde
// pres kompozici DWM (PresentMon "Composed: Copy with GPU GDI"): FPS bez FG drzi u frekvence monitoru, bez tearingu,
// latence navic. Presenter FG (gamepresent.inl) strukturu nema, a proto flipuje. Proxy proto v DXVK vymeni import
// GetProcAddress (jen pro vkGetInstanceProcAddr z vulkan-1.dll) a ve vkCreateSwapchainKHR zmeni DISALLOWED na
// DEFAULT (rozhodne ovladac, jako u presenteru). [present] DxvkFse: 1 = DEFAULT (vychozi od v103j, DXVK primo do okna
// hry: ovladac flip po Alt+Tabu uvolni), 0 = beze zmeny. S dcerinym oknem proxy ([fg] OffWindow=1) by flip po Alt+Tabu
// drzel vystup monitoru (zmereno 12 z 12), proto je tam vychozi 0.
namespace {
#include "alttab.inl"

PFN_vkGetInstanceProcAddr g_dxGipa;
PFN_vkGetDeviceProcAddr g_dxGdpa;
PFN_vkCreateWin32SurfaceKHR g_dxSurf;
struct DxSurfWnd {
    VkSurfaceKHR surf;
    HWND wnd;
};
DxSurfWnd g_dxSw[32];  // povrchy DXVK a jejich okna (kruhove, posledni shoda plati)
volatile LONG g_dxSwN;

VKAPI_ATTR VkResult VKAPI_CALL DxCreateWin32Surface(VkInstance inst, const VkWin32SurfaceCreateInfoKHR* ci,
                                                    const VkAllocationCallbacks* al, VkSurfaceKHR* surf) {
    const VkResult r = g_dxSurf(inst, ci, al, surf);
    if (r == VK_SUCCESS && ci && surf) {
        const LONG i = (InterlockedIncrement(&g_dxSwN) - 1) & 31;
        g_dxSw[i].surf = *surf;
        g_dxSw[i].wnd = ci->hwnd;
    }
    return r;
}

// Okno povrchu je skryte okno zarizeni DXVK (proxy, trida bms_dlss_s2_dxvk): to zustava DISALLOWED - jinak by si
// neviditelny swapchain pres celou obrazovku mohl vzit flip, ktery pak chybi presenteru FG (overeno: FG zapnuta za
// behu po startu bez FG -> presenter jen pres kompozici)
HWND DxSurfaceWnd(VkSurfaceKHR surf) {
    HWND w = nullptr;
    const LONG n = g_dxSwN;
    for (LONG k = 0; k < 32 && k < n; ++k) {
        const DxSurfWnd& e = g_dxSw[(n - 1 - k) & 31];
        if (e.surf == surf) {
            w = e.wnd;
            break;
        }
    }
    return w;
}
bool DxHiddenWnd(HWND w) {
    wchar_t cls[64] = L"";
    return w && GetClassNameW(w, cls, 64) && !wcscmp(cls, L"bms_dlss_s2_dxvk");
}
volatile LONG g_dxScLogged;
VkSwapchainKHR g_dxGameSc;  // posledni swapchain DXVK na okne hry (jen vlakna DXVK)
PFN_vkQueuePresentKHR g_dxPresent;

// Po skutecnem presentu: swapchain na okne hry ma jiny rezim, nez je zadano -> VK_SUBOPTIMAL_KHR, DXVK ho pred dalsim
// snimkem vytvori znovu (obraz uz je odeslany, nic se neztrati). Nejvys 8x po sobe, at se DXVK nezacykli.
VKAPI_ATTR VkResult VKAPI_CALL DxQueuePresent(VkQueue q, const VkPresentInfoKHR* pi) {
    VkResult r = g_dxPresent(q, pi);
    if (!pi || pi->swapchainCount != 1 || !g_dxGameSc || pi->pSwapchains[0] != g_dxGameSc) return r;
    InterlockedIncrement(&g_dxGamePres);
    static int s_n;
    if (g_dxGameFlip == g_dxWantFlip || g_dxGameFlip < 0) s_n = 0;
    else if (r == VK_SUCCESS && s_n < 8) {
        ++s_n;
        r = VK_SUBOPTIMAL_KHR;
        if (pi->pResults) pi->pResults[0] = r;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL DxCreateSwapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                                 const VkAllocationCallbacks* al, VkSwapchainKHR* sc) {
    AtDev* ad = AtDevOf(dev);
    if (!ad || !ad->create || !ci) return VK_ERROR_INITIALIZATION_FAILED;
    VkSurfaceFullScreenExclusiveInfoEXT* fse = nullptr;
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT)
            fse = (VkSurfaceFullScreenExclusiveInfoEXT*)p;  // pamet DXVK (zasobnik volajiciho), po volani se vrati
    const VkFullScreenExclusiveEXT was = fse ? fse->fullScreenExclusive : VK_FULL_SCREEN_EXCLUSIVE_MAX_ENUM_EXT;
    const HWND wnd = DxSurfaceWnd(ci->surface);
    const bool hidden = DxHiddenWnd(wnd);
    const bool game = wnd && wnd == g_gameWnd && DxSwapOn();
    const bool flip = !game || g_dxWantFlip != 0;  // s FG swapchain DXVK na okne hry bez flipu (DISALLOWED)
    const bool change = fse && g_dxFse == 1 && was == VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT && !hidden && flip;
    // Dcerine okno proxy: ovladac by flip po Alt+Tabu neuvolnil -> ridi proxy (alttab.inl); bez funkci rozsireni
    // beze zmeny (DISALLOWED)
    const bool child = change && g_atRelease && AtChildWnd(wnd);
    bool app = child && ad->acquireFse && ad->releaseFse && ad->destroy;
    if (change)
        fse->fullScreenExclusive = app ? VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT
                                   : child ? was : VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;
    VkSwapchainCreateInfoKHR c2 = *ci;
    VkSurfaceFullScreenExclusiveWin32InfoEXT fw = {VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
    if (app) {
        fw.pNext = (void*)c2.pNext;
        fw.hmonitor = MonitorFromWindow(wnd ? wnd : g_gameWnd, MONITOR_DEFAULTTOPRIMARY);
        c2.pNext = &fw;
    }
    if (ci->oldSwapchain) AtForget(ci->oldSwapchain, false);
    VkResult r = ad->create(dev, &c2, al, sc);
    if (change) fse->fullScreenExclusive = was;
    if (app && r != VK_SUCCESS) {  // ovladac rizeny rezim odmitl: jako bez zmeny
        Log("prezentace: swapchain DXVK s APPLICATION_CONTROLLED -> %d, zkousim DISALLOWED", (int)r);
        app = false;
        r = ad->create(dev, ci, al, sc);
    }
    if (app && r == VK_SUCCESS && sc) AtScAdd(ad, *sc, wnd);
    if (game && r == VK_SUCCESS && sc) {
        g_dxGameSc = *sc;
        InterlockedExchange(&g_dxGamePres, 0);
        InterlockedExchange(&g_dxGameFlip, change && !child ? 1 : 0);
    }
    if (InterlockedIncrement(&g_dxScLogged) <= 24)
        Log("prezentace: swapchain DXVK %ux%u%s, VkFormat %d, %u obrazku, present mode %d, usage 0x%X, celoobrazovkovy "
            "rezim %s%s -> %d",
            ci->imageExtent.width, ci->imageExtent.height, hidden ? " (skryte okno zarizeni)" : "", (int)ci->imageFormat,
            ci->minImageCount,
            (int)ci->presentMode, (unsigned)ci->imageUsage,
            !fse ? "bez struktury" : was == VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT ? "DISALLOWED"
                                   : was == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT  ? "ALLOWED"
                                   : was == VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT  ? "DEFAULT"
                                                                                  : "APPLICATION_CONTROLLED",
            app ? " -> APPLICATION_CONTROLLED ([present] AltTabRelease=1)"
                : change && !child ? " -> DEFAULT ([present] DxvkFse=1)" : "", (int)r);
    return r;
}

// Funkce DXVK, ktere proxy zachycuje (vkGetDeviceProcAddr / vkGetInstanceProcAddr); ostatni beze zmeny
PFN_vkVoidFunction DxWrap(VkDevice dev, const char* name, PFN_vkVoidFunction r) {
    AtDev* ad = AtDevGet(dev);
    if (!ad) return r;
    if (!strcmp(name, "vkCreateSwapchainKHR")) {
        ad->create = (PFN_vkCreateSwapchainKHR)r;
        return (PFN_vkVoidFunction)DxCreateSwapchain;
    }
    if (!strcmp(name, "vkDestroySwapchainKHR")) {
        ad->destroy = (PFN_vkDestroySwapchainKHR)r;
        return (PFN_vkVoidFunction)AtDestroySwapchain;
    }
    if (!strcmp(name, "vkQueuePresentKHR")) {
        g_dxPresent = (PFN_vkQueuePresentKHR)r;
        return (PFN_vkVoidFunction)DxQueuePresent;
    }
    if (!strcmp(name, "vkAcquireFullScreenExclusiveModeEXT"))
        ad->acquireFse = (PFN_vkAcquireFullScreenExclusiveModeEXT)r;
    if (!strcmp(name, "vkReleaseFullScreenExclusiveModeEXT"))
        ad->releaseFse = (PFN_vkReleaseFullScreenExclusiveModeEXT)r;
    return r;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DxGdpa(VkDevice dev, const char* name) {
    const PFN_vkVoidFunction r = g_dxGdpa ? g_dxGdpa(dev, name) : nullptr;
    return r && name ? DxWrap(dev, name, r) : r;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DxGipa(VkInstance inst, const char* name) {
    const PFN_vkVoidFunction r = g_dxGipa(inst, name);
    if (!r || !name) return r;
    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        g_dxGdpa = (PFN_vkGetDeviceProcAddr)r;
        return (PFN_vkVoidFunction)DxGdpa;
    }
    if (!strcmp(name, "vkCreateWin32SurfaceKHR")) {
        g_dxSurf = (PFN_vkCreateWin32SurfaceKHR)r;
        return (PFN_vkVoidFunction)DxCreateWin32Surface;
    }
    return r;
}

FARPROC WINAPI DxGetProcAddress(HMODULE m, LPCSTR name) {
    const FARPROC r = GetProcAddress(m, name);
    if (r && ((uintptr_t)name >> 16) && !strcmp(name, "vkGetInstanceProcAddr") &&
        m == GetModuleHandleW(L"vulkan-1.dll")) {
        g_dxGipa = (PFN_vkGetInstanceProcAddr)(void*)r;
        return (FARPROC)(void*)DxGipa;
    }
    if (r && ((uintptr_t)name >> 16) && !strcmp(name, "vkGetDeviceProcAddr") &&
        m == GetModuleHandleW(L"vulkan-1.dll")) {
        g_dxGdpa = (PFN_vkGetDeviceProcAddr)(void*)r;
        return (FARPROC)(void*)DxGdpa;
    }
    return r;
}
}  // namespace

void Present_HookDxvkVulkan(HMODULE dxvk, const wchar_t* ini) {
    // Dcerine okno proxy ([fg] OffWindow=1 / OffTarget=1): flip po Alt+Tabu drzi obraz hry -> vychozi bez flipu
    const bool offChild = GetPrivateProfileIntW(L"fg", L"OffWindow", 0, ini) == 1 ||
                          GetPrivateProfileIntW(L"fg", L"OffTarget", 0, ini) == 1;
    g_dxFse = (int)GetPrivateProfileIntW(L"present", L"DxvkFse", offChild ? 0 : 1, ini);
    g_atRelease = (int)GetPrivateProfileIntW(L"present", L"AltTabRelease", 0, ini);
    if (!dxvk) return;
    if (g_dxFse != 1) {
        Log("prezentace: swapchain DXVK beze zmeny celoobrazovkoveho rezimu ([present] DxvkFse=0)");
        return;
    }
    auto base = (const BYTE*)dxvk;
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;
    for (auto imp = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress); dd.VirtualAddress && imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk || !imp->FirstThunk) continue;
        auto names = (const IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, "GetProcAddress") != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&addrs->u1.Function, sizeof addrs->u1.Function, PAGE_READWRITE, &old)) continue;
            addrs->u1.Function = (decltype(addrs->u1.Function))(uintptr_t)&DxGetProcAddress;
            VirtualProtect(&addrs->u1.Function, sizeof addrs->u1.Function, old, &old);
            ++patched;
        }
    }
    Log("prezentace: swapchain DXVK - zakaz celoobrazovkoveho flipu se rusi ([present] DxvkFse=1)%s",
        patched ? "" : " - CHYBA: import GetProcAddress v DXVK nenalezen, beze zmeny");
}
