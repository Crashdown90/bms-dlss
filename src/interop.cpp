// bms-dlss, krok 3b: most mezi DXVK (32bit, ve hre) a 64bitovym helperem.
//
// Kazdy snimek na konci sceny (cast B: misto enginoveho roztazeni sceny, viz nize):
//   A) na fronte DXVK: kopie barvy sceny, hloubky (D24S8, na AMD D32S8 - format bere DXVK) a - kdyz proxy v tomto snimku prehrala nejaky pohyblivy
//      objekt - MV objektu (OBJMV, RGBA32F) do sdilenych obrazu, signal semProxy = N. (Linearni hloubka R32F se od
//      verze 10 nekopiruje, helper ji nikdy necetl; sdileny obraz LINDEPTH_IN zustava kvuli rozlozeni.)
//   helper: ceka semProxy >= N, zpracuje (DLAA / kopie / zrcadleni / ladeni MV) do COLOR_OUT,
//      signal semHelper = N
//   B) na fronte DXVK: ceka semHelper >= N, kopie COLOR_OUT zpet do barvy sceny
// DXVK pak pokracuje post-processingem a HUD nad upravenym obrazem.
//
// Cast B (od verze 10, mat_viewportscale < 1): hra kresli scenu do leveho horniho rohu plnych cilu. Sdilene obrazy
// zustavaji velke jako povrch sceny, kopie A bere jen sub-rect renderu (barva, hloubka, OBJMV), helper z nej udela
// cely obraz W x H a kopie B ho zapise do cele barvy sceny - presne tam, kam by engine roztahl sub-rect (proxy pak
// jeho draw potlaci). Zmena renderu = jen zprava helperu (frameValue = 0, nova DLSS feature), zadny reimport. Kdyz
// DLSS pro dany render nejde (helperRenderOk = 0), most v casti B nedela nic a roztahuje engine.
//
// Synchronizace CPU ([bridge] Pipeline, Alt+ScrollLock):
//   synchronni (puvodni): po A hra na CPU ceka (max 100 ms), az helper snimek N odesle na GPU, teprve pak B. Po
//      timeoutu (snimek bez B) se dalsi snimek zapise az po prevzeti N helperem a jeho A ceka na semHelper >= N.
//   pipeline (vychozi): B se odesle hned po A a na semHelper >= N ceka az GPU (timeline semafor: cekani pred
//      odeslanim signalu je ve Vulkanu povolene, signal ale prijit musi). Na CPU se jen na zacatku dalsiho konce
//      sceny overi, ze helper snimek N prevzal (helper cte jen posledni frameValue - preskoceny snimek by GPU nikdy
//      nesignalizovala). Kdyby helper skoncil nebo nereagoval (kPendingTimeoutMs), strazce (vlastni vlakno, hra muze
//      v te dobe viset uvnitr DXVK) helper ukonci a semHelper = N dosignalizuje z CPU -> fronta DXVK nezustane viset.
// Pri kazdem trvalem vypnuti mostu (Fail, zasah strazce) se helper ukonci a sdilene obrazy uvolni (StopBridge).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <d3d9.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "bmsd_shared.h"
#include "dxvk_interop.h"  // ID3D9VkInteropTexture, ID3D9VkInteropDevice (DXVK v3.1.1)
#include "interop.h"

namespace {

#define VK_DEV_FUNCS(X)                                                                                         \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)      \
    X(vkBindImageMemory) X(vkGetMemoryWin32HandleKHR) X(vkCreateSemaphore) X(vkDestroySemaphore)               \
    X(vkGetSemaphoreWin32HandleKHR) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkQueueSubmit)   \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateQueryPool) X(vkCmdResetQueryPool)                X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)

struct VkFns {
#define DECL(n) PFN_##n n = nullptr;
    VK_DEV_FUNCS(DECL)
#undef DECL
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    // Vulkan 1.2 (timeline z CPU): jen pro nouzovy signal strazce; bez nich jen synchronni rezim
    PFN_vkGetSemaphoreCounterValue vkGetSemaphoreCounterValue = nullptr;
    PFN_vkSignalSemaphore vkSignalSemaphore = nullptr;
    PFN_vkWaitSemaphores vkWaitSemaphores = nullptr;
} vk;

ID3D9VkInteropDevice* g_idev;
VkInstance g_inst;
VkPhysicalDevice g_pd;
VkDevice g_dev;
VkQueue g_queue;
uint32_t g_qf;
bool g_vkReady;
volatile bool g_failed;  // nastavuje i strazce (jine vlakno)
uint8_t g_uuid[16];

struct SharedImg {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    VkImageAspectFlags barrierAspect = 0;
    bool fresh = true;
    uint32_t w = 0, h = 0;  // skutecna velikost (nepouzite vstupy jen 1x1, viz CreateSharedImages)
};
SharedImg g_simg[BMSD_IMG_COUNT];
uint32_t g_resW, g_resH;
// VRAM: OBJMV_IN (16 B/px) a MASK_IN (4 B/px) maji plnou velikost jen kdyz je funkce zapnuta (g_*Want z konce sceny);
// jinak 1x1 (rozlozeni a protokol stejne, helper je bez kopie necte). LINDEPTH_IN je vzdy 1x1 (helper ho necte).
// Zapnuti funkce za behu = nova generace s plnym obrazem; vypnuti obraz nezmensuje (az pri dalsi nove generaci).
bool g_objMvWant, g_maskWant;
int g_gpuHint = -1;  // BmsdShared::helperGpuHint (z helperu po jeho pripraveni; -1 = jeste nevime), plati i po Fail
// Vulkan format hloubky sceny tak, jak ho vytvoril DXVK: D3DFMT_D24S8 je na NVIDIA a Intelu D24_UNORM_S8_UINT, na AMD
// (bez D24S8 v optimal tiling) D32_SFLOAT_S8_UINT. Sdileny DEPTH_IN vznika ve stejnem formatu (vkCmdCopyImage mezi
// ruznymi formaty hloubky neni povolene) a helper ho dostane v BmsdImage::format.
VkFormat g_depthFmt = VK_FORMAT_D24_UNORM_S8_UINT;
bool g_loggedDepthFmt;

// Formaty hloubky, ktere most umi predat (oba s aspektem hloubky i stencilu, helper i FSR je ctou pres aspekt hloubky)
bool BridgeDepthFormat(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

const char* DepthFmtName(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : f == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32S8" : "jiny";
}

VkSemaphore g_semP, g_semH;
uint64_t g_frameValue;

const int kSlots = 3;
VkCommandPool g_pool;
VkCommandBuffer g_cmdA[kSlots], g_cmdB[kSlots];
VkFence g_fenceA[kSlots], g_fenceB[kSlots];
// Profil GPU (v24x): casova razitka kopii mostu na fronte DXVK, na slot 4 - zacatek A, konec A, zacatek B (po cekani
// na helper), konec B. Cte se po FinishSlot slotu; 0 dotazu = bez mereni (fronta je neumi / chyba).
VkQueryPool g_qpool;
double g_tsMs;  // ms na tik
bool g_tsSlotUsed[kSlots];
bool g_fenceAUsed[kSlots], g_fenceBUsed[kSlots];
int g_slot;

HANDLE g_map, g_evFrame, g_evDone, g_helperProc;
BmsdShared* g_sh;
bool g_helperStarted;
int32_t g_generation;
volatile uint32_t g_mode;
unsigned g_timeouts, g_okFrames;
ULONGLONG g_ackStallSince;  // od kdy helper nepotvrzuje (0 = potvrzuje), viz kAckStallMaxMs
ULONGLONG g_ackStallLog;    // posledni radek "helper jeste neprevzal" (log nejvys 1x za s)
bool g_loggedImages;
uint64_t g_lastBridgedFrame;
bool g_haveBridgedFrame;
int32_t g_loggedDlaaStatus;
volatile uint32_t g_preset;  // preset DLSS pozadovany proxy
uint32_t g_presetSent;       // preset naposledy predany helperu
volatile uint32_t g_inputMode;  // vstup DLSS (BMSD_INPUT_*) pozadovany proxy
uint32_t g_inputModeSent;       // vstup DLSS naposledy predany helperu
volatile float g_sharpness;  // doostreni po DLSS 0..1
volatile uint32_t g_depthMode;  // hloubka pro DLSS (BMSD_DEPTH_*), predava se kazdy snimek
volatile int32_t g_liveSignX = 1, g_liveSignY = 1;  // znamenko jitteru z klavesy
volatile float g_outScale = 1.0f;  // vystup DLSS (S) pozadovany proxy
float g_outScaleSent = 1.0f;       // S naposledy predane helperu
volatile uint32_t g_upscalerReq = BMSD_UPSCALER_AUTO;  // upscaler pozadovany proxy (od verze 11; auto od verze 13)
uint32_t g_upscalerSent = BMSD_UPSCALER_AUTO;          // upscaler naposledy predany helperu
// Naposledy zalogovany skutecny vystup DLSS od helperu (zmena -> log "vystup DLSS: ...")
uint32_t g_loggedOutW, g_loggedOutH;
float g_loggedOutScale;
int32_t g_loggedPerf = -2;
// Render (vstup DLSS, cast B = sub-rect povrchu sceny) naposledy predany helperu; 0 = jeste zadny
uint32_t g_renderSentW, g_renderSentH;
uint32_t g_loggedRenderW, g_loggedRenderH;  // LogOutput: render u naposledy zalogovaneho vystupu
uint32_t g_subIdleW, g_subIdleH;            // cast B bez DLSS: render, pro ktery uz je to v logu
bool g_loggedMirrorB;
uint64_t g_lastOkFrame;      // posledni snimek hry, jehoz vysledek mostu se zapsal do sceny
bool g_haveOkFrame;
// Frame Generation (od verze 12): prezentace bezi v hlavnim helperu ([fg] Mode != 0 pri startu hry) - helper se pak
// spousti uz pri prvnim Present (Interop_EnsureHelper) a v BmsdShared::fgPresenter dostane 1. g_bridgeNForPresent =
// frameValue (N) posledniho snimku mostu, jehoz vysledek je ve scene, od minuleho Interop_TakeBridgeN (0 = zadny).
bool g_fgPresenter;
// FG zap za behu: helper ma fgPresenter vzdy (prezentace jde zapnout pozdeji). BridgeStartSkip plati jen kdyz presenter
// spustil helper driv, nez most zpracoval prvni snimek (start hry s FG nebo zapnuti hned po startu).
bool g_fgEarly;
bool g_bridgeBegan;  // most uz zpracoval aspon jeden konec sceny
uint64_t g_bridgeNForPresent;
volatile uint32_t g_downFilter;                     // filtr zmenseni pri S > 1 (BMSD_DOWN_*), kazdy snimek
volatile uint32_t g_jitterPhases, g_jitterPhasesAuto;  // jen pro log helperu a reset historie pri zmene

// Povrchy, ktere pouzivaji prikazy odeslane ve slotu (barva, hloubka, linearni hloubka hry a OBJMV proxy). DXVK o nasich prikazech
// nevi: kdyby hra povrch uvolnila (Source pred Reset uvolnuje vsechny render targety), DXVK by obraz i pamet znicil,
// i kdyz na nej jeste ceka nase kopie ve fronte -> GPU cte/pise uvolnenou pamet -> VK_ERROR_DEVICE_LOST. Proto se
// reference drzi, dokud fence slotu nedobehnou.
IDirect3DSurface9* g_slotRefs[kSlots][5];
IDirect3DDevice9* g_vkD3D;  // zarizeni hry, ze ktereho je g_idev (Vulkan DXVK)
bool g_devLost;             // VK_ERROR_DEVICE_LOST: uz zadne cekani ani odesilani (hra by jinak mohla viset)
unsigned g_skipScenes;      // po Resetu: kolik koncu sceny jeste vynechat (hra a DXVK znovu vytvari zdroje)
const unsigned kSkipAfterReset = 3;
// Limit cekani na GPU: snimek mostu trva milisekundy, TDR Windows je 2 s. Dele = zaseknuta GPU nebo ztracene
// zarizeni; most se pak vypne misto nekonecneho cekani (zamrzla hra).
const uint64_t kFenceTimeoutNs = 2000000000ull;

// ---- Pipeline (viz hlavicka souboru) ----
volatile LONG g_pipeline = 1;  // pozadovany rezim: 1 pipeline, 0 synchronni ([bridge] Pipeline, klavesa)
bool g_pipeOk;                 // vkSignalSemaphore apod. jsou k dispozici a strazce bezi
// Zamek mezi hrou (odeslani B + zapis g_pendingN) a strazcem (ukonceni helperu + signal z CPU): bud strazce vidi
// kazde odeslane B, nebo hra po zasahu strazce uz zadne B neodesle.
SRWLOCK g_pendLock = SRWLOCK_INIT;
uint64_t g_pendingN;        // posledni N, jehoz B uz ceka na GPU a helper ho jeste nepotvrdil (0 = nic)
ULONGLONG g_pendingTick;    // kdy bylo to B odeslano (GetTickCount64)
// Posledni N zverejnene helperu (frameValue; 0 = zadne nebo po zprave bez snimku) a posledni N, jehoz B bylo odeslano
// (oba rezimy). Synchronni rezim po timeoutu zverejni snimek bez B: dalsi zapis do sdilene pameti az po jeho prevzeti
// (AckPending) a A dalsiho snimku pak na GPU ceka na semHelper (poradi helper N -> A(N+1) jinak zajistuje B(N)).
uint64_t g_postedN, g_lastBN;
volatile LONG g_aborted;    // strazce helper ukoncil a semHelper uvolnil z CPU (most je vypnuty)
bool g_helperStopped;       // StopBridge: helper uz ukoncen (most trvale vypnuty)
HANDLE g_watchThread;
// Helper nepotvrdil snimek, na ktery uz GPU ceka: limit, pak ho strazce ukonci. Pokryva i pomalou obnovu DLSS feature
// v helperu (fallback z S > 1, stovky ms). Na pomalem PC (strankovani VRAM pri nacitani, antivir) smi helper stat
// i sekundy: po kPendingWarnMs jen zalogovat, ukoncit az po kPendingTimeoutMs - delsi nez vlastni limit cekani helperu
// na GPU (20 s), aby skutecnou chybu nahlasil helper sam. Mrtvy helper = hned.
const DWORD kPendingWarnMs = 5000;
const DWORD kPendingTimeoutMs = 30000;
// Na zacatku konce sceny hra ceka na potvrzeni predchoziho snimku nejvys tolik (pak snimek vynecha, jako drive).
const DWORD kAckWaitMs = 100;
// Helper hlasi dlouhou praci hlavniho vlakna (helperBusyTick, od v103c obnovovany kazdych 0,5 s po dobu prace):
// tick mladsi nez tento limit = helper pracuje, ceka se bez selhani
const DWORD kBusyMaxMs = 30000;
// Helper nepotvrzuje snimky (most bez pipeline nebo vynechane snimky): most se vypne az po tolika ms bez potvrzeni
// (zivy helper mimo dlouhou praci), ne podle poctu koncu sceny - na pomalem PC jinak stacilo ~2 s.
const ULONGLONG kAckStallMaxMs = 30000;
bool HelperBusy() {
    MemoryBarrier();
    const uint32_t b = g_sh ? g_sh->helperBusyTick : 0;
    return b && GetTickCount() - b < kBusyMaxMs;
}

// ---- Mereni (log kazdych ~5 s, Interop_OnPresent) ----
struct Acc {
    double sum = 0.0, max = 0.0;
    unsigned n = 0;
    void add(double v) {
        sum += v;
        if (v > max) max = v;
        ++n;
    }
    double avg() const { return n ? sum / n : 0.0; }
};
LARGE_INTEGER g_qpf;
Acc g_stFrame;   // Present -> Present
Acc g_stTotal;   // cely Interop_OnSceneEnd pri zapnutem mostu
Acc g_stWait;    // cekani CPU na helper (synchronni: na N; pipeline: na N-1)
Acc g_stFlush;   // FlushRenderingCommands (DXVK dozene CS vlakno a odesle rozpracovany prikazovy buffer)
Acc g_stSubmit;  // odeslani A + B (vcetne zamku fronty DXVK = cekani na jeho odesilaci vlakno)
unsigned g_stSkipped;  // konce sceny, kdy most snimek nezpracoval (timeout, priprava helperu...)
uint32_t g_objStatSeq;  // posledni statistika MV objektu z helperu vypsana v radku "vykon"
bool g_loggedObjMv;
bool g_loggedMask;      // prvni kopie masky reaktivity zalogovana     // prvni kopie OBJMV zalogovana (obraz a layout)
Acc g_stSlot;    // cekani na fence slotu mostu (GPU nedokoncilo kopie snimku pred kSlots snimky)
// Zaznam prikazu kopie A: ovladac v nem muze blokovat, dokud GPU nedokonci predchozi praci nad sdilenymi obrazy
// (falesna hra omezena GPU: vkCmdCopyImage do COLOR_IN ~6.8 ms) - viz docs/navrh-reflex.md.
Acc g_stRec;
double g_fWait, g_fFlush, g_fSubmit, g_fSlot, g_fRec;  // soucty v ramci jednoho konce sceny (ms)
double g_fLock;  // z g_fSubmit: cekani na zamek fronty DXVK (LockSubmissionQueue)
Acc g_stLock;
Acc g_stGpuA, g_stGpuB, g_stGpuAB;  // profil GPU (v24x): kopie A, kopie B, konec A -> zacatek B (helper + fronta)
LONGLONG g_lastPresentQpc, g_statStartQpc;

LONGLONG Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double QpcMs(LONGLONG a, LONGLONG b) {
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    return (double)(b - a) * 1000.0 / (double)g_qpf.QuadPart;
}

// Layout pro kopii: GENERAL kopie povoluje, jinak docasne TRANSFER_SRC/DST_OPTIMAL.
VkImageLayout CopyLayout(VkImageLayout dxvk, VkImageLayout transfer) {
    return dxvk == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : transfer;
}

const char* kModeNames[BMSD_MODE_COUNT] = {"vypnuto", "kopie", "zrcadleni", "DLAA", "ladeni MV"};

// Rezim DLSS (NVSDK_NGX_PerfQuality_Value), stejne nazvy jako v logu helperu
const char* PerfName(int32_t v) {
    static const char* names[] = {"Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA"};
    return v >= 0 && v < 6 ? names[v] : "DLSS nebezi";
}

// S do logu: 1.0, 1.33, 1.5, 1.72, 2.0, 3.0
std::string ScaleStr(float s) {
    char b[32];
    snprintf(b, sizeof b, "%.2f", s);
    std::string r = b;
    while (r.size() > 3 && r.back() == '0') r.pop_back();
    return r;
}

// Skutecny vystup DLSS podle helperu: pri zmene (Alt+F3, reimport, fallback) do logu
void LogOutput() {
    const uint32_t w = g_sh->helperOutWidth, h = g_sh->helperOutHeight;
    const float sc = g_sh->helperOutScale;
    const int32_t perf = g_sh->helperPerfQuality;
    const uint32_t rw = g_renderSentW, rh = g_renderSentH;
    if (w == g_loggedOutW && h == g_loggedOutH && sc == g_loggedOutScale && perf == g_loggedPerf &&
        rw == g_loggedRenderW && rh == g_loggedRenderH)
        return;
    g_loggedOutW = w;
    g_loggedOutH = h;
    g_loggedOutScale = sc;
    g_loggedPerf = perf;
    g_loggedRenderW = rw;
    g_loggedRenderH = rh;
    if (perf < 0) return;  // DLSS v helperu nebezi (duvod uz je v logu)
    if (rw && rh && (rw != g_resW || rh != g_resH))  // cast B: vystup = povrch sceny, rezim podle pomeru
        Log("vystup DLSS: %ux%u (cast B: vstup %ux%u, pomer %.3f, %s)", w, h, rw, rh, (double)rw / (double)g_resW,
            PerfName(perf));
    else if (g_outScaleSent > 1.0001f && sc < 1.0001f)
        Log("vystup DLSS: %ux%u (%sx, %s) - pozadovano %sx nejde: %s", w, h, ScaleStr(sc).c_str(), PerfName(perf),
            ScaleStr(g_outScaleSent).c_str(), g_sh->helperMsg);
    else
        Log("vystup DLSS: %ux%u (%sx, %s)", w, h, ScaleStr(sc).c_str(), PerfName(perf));
}

// ---------------------------------------------------------------------------
bool InitVk(IDirect3DDevice9* d3d) {
    if (Interop_VkBlocked()) return Log("most: DXVK hry ma neznamou verzi - interop se nepouziva"), false;
    HRESULT hr = d3d->QueryInterface(__uuidof(ID3D9VkInteropDevice), (void**)&g_idev);
    if (FAILED(hr) || !g_idev) {
        Log("most: ID3D9VkInteropDevice nedostupne (0x%08X)", hr);
        return false;
    }
    g_idev->GetVulkanHandles(&g_inst, &g_pd, &g_dev);
    uint32_t qi = 0;
    g_idev->GetSubmissionQueue(&g_queue, &qi, &g_qf);

    HMODULE vkdll = GetModuleHandleW(L"vulkan-1.dll");
    auto gipa = vkdll ? (PFN_vkGetInstanceProcAddr)GetProcAddress(vkdll, "vkGetInstanceProcAddr") : nullptr;
    if (!gipa) {
        Log("most: vkGetInstanceProcAddr nenalezeno");
        return false;
    }
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(g_inst, "vkGetDeviceProcAddr");
#define LOAD(n)                                     \
    vk.n = (PFN_##n)gdpa(g_dev, #n);                \
    if (!vk.n) {                                    \
        Log("most: chybi funkce %s", #n);           \
        return false;                               \
    }
    VK_DEV_FUNCS(LOAD)
#undef LOAD
    vk.vkGetPhysicalDeviceProperties2 =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(g_inst, "vkGetPhysicalDeviceProperties2");
    vk.vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(g_inst, "vkGetPhysicalDeviceMemoryProperties");
    vk.vkGetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue)gdpa(g_dev, "vkGetSemaphoreCounterValue");
    vk.vkSignalSemaphore = (PFN_vkSignalSemaphore)gdpa(g_dev, "vkSignalSemaphore");
    vk.vkWaitSemaphores = (PFN_vkWaitSemaphores)gdpa(g_dev, "vkWaitSemaphores");

    VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
    vk.vkGetPhysicalDeviceProperties2(g_pd, &p2);
    memcpy(g_uuid, idp.deviceUUID, 16);
    g_tsMs = p2.properties.limits.timestampPeriod * 1e-6;

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf};
    if (vk.vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, kSlots};
    if (vk.vkAllocateCommandBuffers(g_dev, &cai, g_cmdA) != VK_SUCCESS) return false;
    if (vk.vkAllocateCommandBuffers(g_dev, &cai, g_cmdB) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int i = 0; i < kSlots; ++i) {
        if (vk.vkCreateFence(g_dev, &fci, nullptr, &g_fenceA[i]) != VK_SUCCESS) return false;
        if (vk.vkCreateFence(g_dev, &fci, nullptr, &g_fenceB[i]) != VK_SUCCESS) return false;
    }
    if (g_tsMs > 0.0 && vk.vkCreateQueryPool && vk.vkCmdWriteTimestamp && vk.vkGetQueryPoolResults) {
        VkQueryPoolCreateInfo qpci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP,
                                      kSlots * 4};
        if (vk.vkCreateQueryPool(g_dev, &qpci, nullptr, &g_qpool) != VK_SUCCESS) g_qpool = VK_NULL_HANDLE;
    }
    if (!g_qpool) Log("most: casova razitka GPU nejdou - bez profilu kopii");

    for (VkSemaphore* s : {&g_semP, &g_semH}) {
        VkExportSemaphoreCreateInfo esi = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr,
                                           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
        VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &esi,
                                         VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
        if (vk.vkCreateSemaphore(g_dev, &sci, nullptr, s) != VK_SUCCESS) {
            Log("most: vytvoreni semaforu selhalo");
            return false;
        }
    }
    Log("most: Vulkan pripraven (VkDevice=%p, fronta family %u)", (void*)g_dev, g_qf);
    g_vkD3D = d3d;
    g_vkReady = true;
    return true;
}

uint64_t DupToHelper(HANDLE h) {
    HANDLE remote = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, g_helperProc, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        Log("most: DuplicateHandle selhal (%lu)", GetLastError());
        return 0;
    }
    return (uint64_t)(uintptr_t)remote;
}

uint64_t ExportSemaphore(VkSemaphore s) {
    VkSemaphoreGetWin32HandleInfoKHR hi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr, s,
                                           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
    HANDLE h = nullptr;
    if (vk.vkGetSemaphoreWin32HandleKHR(g_dev, &hi, &h) != VK_SUCCESS || !h) return 0;
    uint64_t r = DupToHelper(h);
    CloseHandle(h);
    return r;
}

bool StartHelper() {
    DWORD pid = GetCurrentProcessId();
    wchar_t name[128];
    swprintf(name, 128, L"Local\\bms_dlss_%lu", pid);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(BmsdShared), name);
    if (!g_map) return false;
    g_sh = (BmsdShared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BmsdShared));
    if (!g_sh) return false;
    memset(g_sh, 0, sizeof(BmsdShared));
    g_sh->magic = BMSD_MAGIC;
    g_sh->version = BMSD_VERSION;
    memcpy(g_sh->deviceUUID, g_uuid, 16);
    g_presetSent = g_preset;
    g_sh->preset = g_presetSent;
    g_inputModeSent = g_inputMode;
    g_sh->inputMode = g_inputModeSent;
    g_outScaleSent = g_outScale;
    g_sh->outScale = g_outScaleSent;
    g_upscalerSent = g_upscalerReq;
    g_sh->upscaler = g_upscalerSent;
    g_sh->upscalerValid = 1;  // helper bez tohoto priznaku bere DLSS
    g_sh->fgPresenter = g_fgPresenter ? 1 : 0;  // 1 = helper zapne swapchain a hleda prezentacni kanal (FG)
    swprintf(name, 128, L"Local\\bms_dlss_%lu_frame", pid);
    g_evFrame = CreateEventW(nullptr, FALSE, FALSE, name);
    swprintf(name, 128, L"Local\\bms_dlss_%lu_done", pid);
    g_evDone = CreateEventW(nullptr, FALSE, FALSE, name);
    if (!g_evFrame || !g_evDone) return false;

    std::wstring exe = std::wstring(ProxyDir()) + L"\\bms_dlss_helper.exe";
    std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring(pid);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, ProxyDir(), &si,
                        &pi)) {
        Log("most: nelze spustit %ls (chyba %lu)", exe.c_str(), GetLastError());
        return false;
    }
    // Job s KILL_ON_JOB_CLOSE: handle jobu drzi jen proxy, takze helper skonci i kdyz hra spadne nebo skonci a helper
    // zrovna visi ve volani ovladace (jinak by zustal jako zombie s VRAM a zamcenym exe). Selhani (hra uz je v jobu,
    // ktery vnoreni nedovoli) neni chyba - helper si konec hry hlida i sam.
    static HANDLE s_job;
    if (!s_job) {
        s_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {};
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (s_job && !SetInformationJobObject(s_job, JobObjectExtendedLimitInformation, &jl, sizeof jl)) {
            CloseHandle(s_job);
            s_job = nullptr;
        }
    }
    if (!s_job || !AssignProcessToJobObject(s_job, pi.hProcess))
        Log("most: helper bez jobu (chyba %lu) - pri padu hry ho ukonci jen jeho vlastni hlidani",
            s_job ? GetLastError() : 0ul);
    CloseHandle(pi.hThread);
    g_helperProc = pi.hProcess;
    g_sh->semProxyHandle = ExportSemaphore(g_semP);
    g_sh->semHelperHandle = ExportSemaphore(g_semH);
    if (!g_sh->semProxyHandle || !g_sh->semHelperHandle) return false;
    Log("most: helper spusten (pid %lu)", pi.dwProcessId);
    return true;
}

// Ceka na "done" od helperu, dokud neplati podminka (pozdni "done" z minuleho snimku se ignoruje),
// nejdele timeoutMs. Skonceni helperu ukonci cekani hned (hra nezamrzne).
template <class Cond>
bool WaitHelper(DWORD timeoutMs, Cond done) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        MemoryBarrier();
        if (done()) return true;
        const ULONGLONG el = GetTickCount64() - start;
        if (el >= timeoutMs) return false;
        HANDLE hs[2] = {g_evDone, g_helperProc};
        DWORD r = WaitForMultipleObjects(2, hs, FALSE, (DWORD)(timeoutMs - el));
        if (r != WAIT_OBJECT_0) {  // timeout nebo helper skoncil
            MemoryBarrier();
            return done();
        }
    }
}

// Chybovy VkResult do logu; VK_ERROR_DEVICE_LOST si zapamatuje (dal se uz na GPU neceka ani neodesila).
void LogVk(VkResult r, const char* what) {
    if (r == VK_ERROR_DEVICE_LOST) {
        g_devLost = true;
        Log("most: %s -> VK_ERROR_DEVICE_LOST (zarizeni Vulkan DXVK je ztracene)", what);
    } else if (r == VK_TIMEOUT) {
        Log("most: %s - GPU nedobehla do %llu ms", what, (unsigned long long)(kFenceTimeoutNs / 1000000));
    } else {
        Log("most: %s selhalo (VkResult %d)", what, (int)r);
    }
}

unsigned HeldRefs() {
    unsigned n = 0;
    for (auto& slot : g_slotRefs)
        for (IDirect3DSurface9* s : slot) n += s ? 1 : 0;
    return n;
}

void ReleaseSlotRefs(int i) {
    for (IDirect3DSurface9*& s : g_slotRefs[i])
        if (s) {
            s->Release();
            s = nullptr;
        }
}

// Dokonci slot: pocka na jeho prikazy A i B (timeoutNs 0 = jen zkontrolovat), vynuluje fence a uvolni drzene povrchy.
// false = jeste bezi (timeoutNs 0), nedobehlo v limitu nebo chyba (*err = true). Povrchy pak zustanou drzene:
// radsi unik nez GPU nad uvolnenym obrazem.
bool FinishSlot(int i, uint64_t timeoutNs, bool* err = nullptr) {
    VkFence f[2];
    uint32_t n = 0;
    if (g_fenceAUsed[i]) f[n++] = g_fenceA[i];
    if (g_fenceBUsed[i]) f[n++] = g_fenceB[i];
    if (n) {
        if (g_devLost) {
            if (err) *err = true;
            return false;
        }
        const VkResult r = vk.vkWaitForFences(g_dev, n, f, VK_TRUE, timeoutNs);
        if (r == VK_TIMEOUT && timeoutNs == 0) return false;
        if (r != VK_SUCCESS) {
            LogVk(r, "cekani na snimek mostu");
            if (err) *err = true;
            return false;
        }
        vk.vkResetFences(g_dev, n, f);
        g_fenceAUsed[i] = g_fenceBUsed[i] = false;
    }
    ReleaseSlotRefs(i);
    return true;
}

bool FinishAllSlots() {
    bool ok = true;
    for (int i = 0; i < kSlots; ++i) ok = FinishSlot(i, kFenceTimeoutNs) && ok;
    return ok;
}

// Sdilene obrazy proxy (obrazy a pameti) zrusit. Volat jen kdyz zadny slot nebezi (FinishSlot).
void DestroySharedImages() {
    for (SharedImg& s : g_simg) {
        if (s.img) vk.vkDestroyImage(g_dev, s.img, nullptr);
        if (s.mem) vk.vkFreeMemory(g_dev, s.mem, nullptr);
        s = SharedImg();
    }
    g_resW = g_resH = 0;
}

bool HaveSharedImages() {
    for (const SharedImg& s : g_simg)
        if (s.img || s.mem) return true;
    return false;
}

// Pozadavky na helper, ktere trvaji (import zdroju, nova DLSS feature): hra ceka nejdele kReqSyncMs (bezne zmeny
// stihne helper za desitky az stovky ms - chovani jako drive), pak snimky vynechava a kazdy konec sceny jen zkontroluje
// potvrzeni (PollReq). Pri dlouhem cekani (prvni spusteni, pomaly disk, antivirus) hra nezamrzne a most se nevypne:
// dal se ceka na helper, jen se zapise varovani. Skonceni helperu nebo jeho chybu resi zacatek SceneEnd (Fail).
// Behem cekani se do sdilene pameti nic dalsiho nezapisuje (helper pozadavek prave cte).
enum { REQ_NONE, REQ_GEN, REQ_CFG };
int g_req;
ULONGLONG g_reqT0;
int g_reqWarn;
const DWORD kReqSyncMs = 500;

bool CfgAcked() {
    return g_sh->helperPreset == g_presetSent && g_sh->helperInputMode == g_inputModeSent &&
           g_sh->helperOutScaleAck == g_outScaleSent && g_sh->helperRenderAckW == g_renderSentW &&
           g_sh->helperRenderAckH == g_renderSentH && g_sh->helperUpscalerAck == g_upscalerSent;
}

bool ReqDone() { return g_req == REQ_GEN ? g_sh->helperGeneration == g_generation : CfgAcked(); }

// Pozadavek potvrzen: log jako drive (i doba cekani)
void ReqFinished() {
    const unsigned long long ms = GetTickCount64() - g_reqT0;
    if (g_req == REQ_GEN)
        Log("most: sdilene obrazy %ux%u vytvoreny a importovany (generace %d, %llu ms)", g_resW, g_resH, g_generation,
            ms);
    else
        Log("most: preset DLSS %u, vstup %u, vystup %sx, render %ux%u, upscaler %u predan helperu (%llu ms), bezi %s",
            g_presetSent, g_inputModeSent, ScaleStr(g_outScaleSent).c_str(), g_renderSentW, g_renderSentH,
            g_upscalerSent, ms, g_sh->helperUpscalerName[0] ? g_sh->helperUpscalerName : "nic (kopie)");
    g_req = REQ_NONE;
    LogOutput();
}

// Odeslat pozadavek (zapsany ve sdilene pameti) a kratce pockat. true = helper uz potvrdil.
bool StartReq(int kind) {
    g_req = kind;
    g_reqT0 = GetTickCount64();
    g_reqWarn = 0;
    ResetEvent(g_evDone);
    SetEvent(g_evFrame);
    if (!WaitHelper(kReqSyncMs, [] { return ReqDone(); })) {
        Log("most: helper %s jeste nedokoncil (%lu ms) - hra bezi dal, snimky zatim bez zpracovani",
            kind == REQ_GEN ? "import zdroju" : "zmenu DLSS feature", kReqSyncMs);
        return false;
    }
    ReqFinished();
    return true;
}

// Konec sceny pri nevyrizenem pozadavku: bez cekani. true = vyrizeno, snimek se zpracuje.
bool PollReq() {
    MemoryBarrier();
    if (ReqDone()) return ReqFinished(), true;
    const ULONGLONG el = GetTickCount64() - g_reqT0;
    if ((g_reqWarn == 0 && el >= 15000) || (g_reqWarn == 1 && el >= 60000)) {
        ++g_reqWarn;
        Log("most: POZOR - helper %s nedokoncil ani za %llu s (zprava: %s) - cekam dal, mod zustava zapnuty",
            g_req == REQ_GEN ? "import zdroju" : "zmenu DLSS feature", (unsigned long long)(el / 1000),
            g_sh->helperMsg[0] ? g_sh->helperMsg : "-");
    }
    return false;
}

// rw x rh = render tohoto snimku (cast B: sub-rect; jinak = w x h), predava se helperu spolu s novymi obrazy
// 0 = selhalo, 1 = helper obrazy importoval, 2 = import jeste bezi (snimky se do potvrzeni vynechavaji, PollReq)
int CreateSharedImages(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh) {
    if (!FinishAllSlots()) return 0;
    DestroySharedImages();
    // Selhani uprostred: uvolnit, co uz vzniklo, a zavrit handly duplikovane do helperu v tomto volani (generation se
    // nezvysila, helper je nikdy neimportuje ani nezavre) - jinak by VRAM i handly zustaly do konce hry.
    uint64_t dup[BMSD_IMG_COUNT] = {};
    auto fail = [&](int i, const char* what) {
        Log("most: sdilene obrazy %ux%u: %s (obraz %d) selhalo", w, h, what, i);
        for (uint64_t hd : dup)
            if (hd)
                DuplicateHandle(g_helperProc, (HANDLE)(uintptr_t)hd, nullptr, nullptr, 0, FALSE,
                                DUPLICATE_CLOSE_SOURCE);
        memset(g_sh->img, 0, sizeof g_sh->img);
        DestroySharedImages();
        return 0;
    };
    const VkImageUsageFlags xfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT;
    g_simg[BMSD_IMG_COLOR_IN] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_FORMAT_B8G8R8A8_UNORM, xfer,
                                 VK_IMAGE_ASPECT_COLOR_BIT};
    g_simg[BMSD_IMG_DEPTH_IN] = {VK_NULL_HANDLE, VK_NULL_HANDLE, g_depthFmt, xfer,
                                 VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
    g_simg[BMSD_IMG_LINDEPTH_IN] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_FORMAT_R32_SFLOAT,
                                    xfer | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT};
    g_simg[BMSD_IMG_COLOR_OUT] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_FORMAT_B8G8R8A8_UNORM, xfer,
                                  VK_IMAGE_ASPECT_COLOR_BIT};
    // MV pohyblivych objektu (kopie z cile OBJMV proxy; helper ho jen cte v mv.comp)
    g_simg[BMSD_IMG_OBJMV_IN] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_FORMAT_R32G32B32A32_SFLOAT, xfer,
                                 VK_IMAGE_ASPECT_COLOR_BIT};
    // Maska reaktivity (kopie z cile MASKA proxy, A8R8G8B8 = B8G8R8A8; helper ji cte v reactive.comp)
    g_simg[BMSD_IMG_MASK_IN] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_FORMAT_B8G8R8A8_UNORM, xfer,
                                VK_IMAGE_ASPECT_COLOR_BIT};

    VkPhysicalDeviceMemoryProperties mp = {};
    vk.vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    uint64_t vram = 0;
    for (int i = 0; i < BMSD_IMG_COUNT; ++i) {
        SharedImg& s = g_simg[i];
        const bool tiny = i == BMSD_IMG_LINDEPTH_IN || (i == BMSD_IMG_OBJMV_IN && !g_objMvWant) ||
                          (i == BMSD_IMG_MASK_IN && !g_maskWant);
        s.w = tiny ? 1 : w;
        s.h = tiny ? 1 : h;
        VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, ht};
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = s.fmt;
        ci.extent = {s.w, s.h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = s.usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img = VK_NULL_HANDLE;  // pri chybe je vystup vkCreate* nedefinovany - do g_simg jen platny
        if (vk.vkCreateImage(g_dev, &ci, nullptr, &img) != VK_SUCCESS) return fail(i, "vkCreateImage");
        s.img = img;
        VkMemoryRequirements req = {};
        vk.vkGetImageMemoryRequirements(g_dev, s.img, &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; ++t)
            if ((req.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = t;
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, s.img};
        VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded,
                                          (VkExternalMemoryHandleTypeFlags)ht};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, req.size, type};
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (type == UINT32_MAX || vk.vkAllocateMemory(g_dev, &ai, nullptr, &mem) != VK_SUCCESS)
            return fail(i, "alokace pameti");
        s.mem = mem;
        if (vk.vkBindImageMemory(g_dev, s.img, s.mem, 0) != VK_SUCCESS) return fail(i, "vkBindImageMemory");
        VkMemoryGetWin32HandleInfoKHR hi = {VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, s.mem, ht};
        HANDLE handle = nullptr;
        if (vk.vkGetMemoryWin32HandleKHR(g_dev, &hi, &handle) != VK_SUCCESS || !handle)
            return fail(i, "vkGetMemoryWin32HandleKHR");
        BmsdImage& bi = g_sh->img[i];
        bi.handle = dup[i] = DupToHelper(handle);
        CloseHandle(handle);
        bi.allocSize = req.size;
        vram += req.size;
        bi.format = s.fmt;
        bi.usage = s.usage;
        bi.width = s.w;
        bi.height = s.h;
        if (!bi.handle) return fail(i, "predani handlu helperu");
    }
    g_resW = w;
    g_resH = h;
    g_sh->width = w;
    g_sh->height = h;
    g_presetSent = g_preset;
    g_sh->preset = g_presetSent;  // helper vytvori DLSS feature uz s aktualnim presetem a vstupem
    g_inputModeSent = g_inputMode;
    g_sh->inputMode = g_inputModeSent;
    g_outScaleSent = g_outScale;  // i S pred generation: feature vznikne rovnou s nim (ne dvakrat po sobe)
    g_sh->outScale = g_outScaleSent;
    g_upscalerSent = g_upscalerReq;  // i upscaler pred generation (jako preset)
    g_sh->upscaler = g_upscalerSent;
    g_renderSentW = rw;  // totez render (cast B): pri zmene rozliseni s mat_viewportscale < 1 jedna feature
    g_renderSentH = rh;
    g_sh->renderWidth = rw;
    g_sh->renderHeight = rh;
    g_sh->frameValue = 0;
    g_postedN = 0;  // helperDoneValue bude 0 (zprava bez snimku); minuly snimek uz helper prevzal (AckPending)
    g_sh->generation = ++g_generation;
    Log("most: sdilene obrazy %ux%u (generace %d): VRAM %.1f MB (MV objektu %s, maska reaktivity %s)", w, h,
        g_generation, (double)vram / (1024.0 * 1024.0), g_objMvWant ? "plna velikost" : "1x1",
        g_maskWant ? "plna velikost" : "1x1");

    // Helper pri importu vytvari i DLSS feature (nacteni modelu muze pri prvnim spusteni trvat i nekolik sekund):
    // kratce se pocka, pak hra bezi dal bez zpracovani snimku, dokud helper import nepotvrdi (PollReq).
    return StartReq(REQ_GEN) ? 1 : 2;
}

static bool BridgeEarlyFlush() {
    static int s_v = -1;
    if (s_v < 0) {
        const std::wstring ini = std::wstring(ProxyDir()) + L"\\bms_dlss.ini";
        s_v = GetPrivateProfileIntW(L"fg", L"EarlyFlush", 1, ini.c_str()) != 0 ? 1 : 0;
    }
    return s_v != 0;
}

VkImage GetVkImage(IDirect3DSurface9* s, VkImageLayout* layout, VkFormat* fmt = nullptr) {
    if (!s) return VK_NULL_HANDLE;
    ID3D9VkInteropTexture* tex = nullptr;
    HRESULT hr = s->QueryInterface(__uuidof(ID3D9VkInteropTexture), (void**)&tex);
    if (FAILED(hr) || !tex) {
        IDirect3DTexture9* parent = nullptr;
        if (SUCCEEDED(s->GetContainer(__uuidof(IDirect3DTexture9), (void**)&parent)) && parent) {
            hr = parent->QueryInterface(__uuidof(ID3D9VkInteropTexture), (void**)&tex);
            parent->Release();
        }
    }
    if (FAILED(hr) || !tex) return VK_NULL_HANDLE;
    VkImage img = VK_NULL_HANDLE;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (FAILED(tex->GetVulkanImageInfo(&img, layout, fmt ? &ci : nullptr))) img = VK_NULL_HANDLE;
    if (fmt) *fmt = ci.format;
    tex->Release();
    return img;
}

VkImageMemoryBarrier Barrier(VkImage img, VkImageAspectFlags aspect, VkAccessFlags src, VkAccessFlags dst,
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

VkImageCopy CopyRegion(VkImageAspectFlags aspect, uint32_t w, uint32_t h) {
    VkImageCopy c = {};
    c.srcSubresource = {aspect, 0, 0, 1};
    c.dstSubresource = {aspect, 0, 0, 1};
    c.extent = {w, h, 1};
    return c;
}

bool Submit(VkCommandBuffer cmd, VkFence fence, VkSemaphore waitSem, uint64_t waitValue, VkSemaphore signalSem,
            uint64_t signalValue) {
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = waitSem ? 1 : 0;
    ti.pWaitSemaphoreValues = &waitValue;
    ti.signalSemaphoreValueCount = signalSem ? 1 : 0;
    ti.pSignalSemaphoreValues = &signalValue;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ti};
    si.waitSemaphoreCount = waitSem ? 1 : 0;
    si.pWaitSemaphores = &waitSem;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = signalSem ? 1 : 0;
    si.pSignalSemaphores = &signalSem;
    const LONGLONG l0 = Qpc();
    g_idev->LockSubmissionQueue();
    g_fLock += QpcMs(l0, Qpc());  // mereni (vykon-1): cekani na zamek fronty DXVK vs. samotny vkQueueSubmit
    VkResult r = vk.vkQueueSubmit(g_queue, 1, &si, fence);
    g_idev->ReleaseSubmissionQueue();
    if (r != VK_SUCCESS) LogVk(r, "vkQueueSubmit");
    return r == VK_SUCCESS;
}

void StopBridge();

// Most se za behu vypnul chybou: OSD jednou (proxy.cpp OnFrameEnd, Interop_TakeFailNote)
volatile LONG g_failNote;

// Vystupni kod helperu citelne (log)
const char* ExitCodeText(DWORD c) {
    switch (c) {
    case 0: return "normalni konec";
    case 1: return "vlastni chyba helperu (viz bms_dlss_helper.log)";
    case 3: return "ukoncen proxy (nereagoval)";
    case STILL_ACTIVE: return "jeste bezi";
    case 0xC0000005: return "PAD: pristup do pameti (viz PAD v bms_dlss_helper.log)";
    case 0xC0000409: return "PAD: poskozeny zasobnik / FailFast";
    case 0xC00000FD: return "PAD: preteceni zasobniku";
    case 0xC0000374: return "PAD: poskozena halda";
    case 0xE06D7363: return "PAD: neosetrena vyjimka C++";
    default: return c >= 0xC0000000u ? "PAD (kod NTSTATUS)" : "jiny kod";
    }
}

void Fail(const char* why) {
    Log("most: CHYBA - %s, most vypinam", why);
    if (!g_failed) InterlockedExchange(&g_failNote, 1);
    g_failed = true;
    g_mode = BMSD_MODE_OFF;
    StopBridge();  // helper ukoncit (jinak drzi VRAM do konce hry), sdilene obrazy uvolnit
}

// Pipeline: GPU (fronta DXVK) ceka v B na semHelper >= N, ale helper signal neodesle (skoncil / nereaguje).
// Helper ukoncit, pockat (s limitem) na signaly, ktere uz odeslal (hodnota semaforu nesmi klesat), a zbytek
// dosignalizovat z CPU. Most se pak vypne. Vola strazce nebo hra (helper skoncil, pred Resetem).
void AbortPending(const char* why) {
    AcquireSRWLockExclusive(&g_pendLock);
    const uint64_t pend = g_pendingN;
    MemoryBarrier();
    if (!pend || g_aborted || g_sh->helperDoneValue >= pend) {  // nic neceka nebo helper mezitim potvrdil
        ReleaseSRWLockExclusive(&g_pendLock);
        return;
    }
    const bool wasDead = WaitForSingleObject(g_helperProc, 0) == WAIT_OBJECT_0;
    if (!wasDead) {
        TerminateProcess(g_helperProc, 3);
        WaitForSingleObject(g_helperProc, 5000);
    }
    MemoryBarrier();
    const uint64_t acked = g_sh->helperDoneValue;  // posledni signal, ktery helper na GPU odeslal
    uint64_t cur = 0;
    VkResult r = VK_SUCCESS;
    bool waitedOk = true, signaled = false;
    if (!g_devLost) {
        if (acked) {
            const uint64_t target = acked < pend ? acked : pend;
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &g_semH, &target};
            waitedOk = vk.vkWaitSemaphores(g_dev, &wi, kFenceTimeoutNs) == VK_SUCCESS;
        }
        vk.vkGetSemaphoreCounterValue(g_dev, g_semH, &cur);
        if (cur < pend) {
            VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, g_semH, pend};
            r = vk.vkSignalSemaphore(g_dev, &si);
            signaled = true;
        }
    }
    g_pendingN = 0;
    InterlockedExchange(&g_aborted, 1);
    if (!g_failed) InterlockedExchange(&g_failNote, 1);
    g_failed = true;
    g_mode = BMSD_MODE_OFF;
    ReleaseSRWLockExclusive(&g_pendLock);
    Log("most: CHYBA - %s: GPU ceka na snimek %llu, helper potvrdil %llu; helper %s, semafor %llu -> %s%s; most "
        "vypinam",
        why, (unsigned long long)pend, (unsigned long long)acked, wasDead ? "uz nebezel" : "ukoncen",
        (unsigned long long)cur, signaled ? "signal z CPU" : "uz uvolnen (GPU nebo system po konci helperu)",
        g_devLost ? " (zarizeni ztraceno)" : !waitedOk ? " (odeslane signaly helperu nedobehly do 2 s)" :
        r != VK_SUCCESS ? " (vkSignalSemaphore selhal)" : "");
}

// Pred zapisem dalsiho snimku (nebo zpravy bez snimku) do sdilene pameti musi helper predchozi zverejneny snimek
// prevzit a odeslat (helperDoneValue >= N). Pipeline: jinak by ho preskocil a B by na GPU cekalo navzdy. Synchronni
// rezim po timeoutu (snimek bez B): helper ho muze zpracovat pozde a cetl by smes snimku N a N+1 (nebo N+1 dvakrat).
// Normalne uz davno potvrzeno (helper mel cely snimek). true = nic neceka. Helper skoncil -> GPU uvolnit hned (ne az
// strazce).
// Helper nepotvrdil vcas (snimek se vynecha). true = most vypnout: helper zije, nepracuje na dlouhe praci a nepotvrdil
// nic uz kAckStallMaxMs. Doba dlouhe prace helperu (helperBusyTick) se nepocita. Log nejvys 1x za s.
bool AckStalled(const char* what) {
    const ULONGLONG now = GetTickCount64();
    if (!g_ackStallSince || HelperBusy()) g_ackStallSince = now;
    ++g_timeouts;
    if (now - g_ackStallLog >= 1000) {
        g_ackStallLog = now;
        Log("most: %s (%.1f s bez potvrzeni, vynechano %u, done=%llu)%s", what, (now - g_ackStallSince) / 1000.0,
            g_timeouts, (unsigned long long)g_sh->helperDoneValue, HelperBusy() ? " - helper hlasi dlouhou praci" : "");
    }
    return now - g_ackStallSince >= kAckStallMaxMs;
}
void AckResumed() {
    if (g_ackStallSince && g_timeouts)
        Log("most: helper zase potvrzuje (po %.1f s, vynechano %u snimku)", (GetTickCount64() - g_ackStallSince) / 1000.0,
            g_timeouts);
    g_ackStallSince = 0;
    g_timeouts = 0;
}

bool AckPending(DWORD timeoutMs) {
    AcquireSRWLockExclusive(&g_pendLock);
    const uint64_t pend = g_pendingN;
    ReleaseSRWLockExclusive(&g_pendLock);
    const uint64_t need = pend > g_postedN ? pend : g_postedN;  // B pipeline je vzdy pro zverejneny snimek
    if (!need || !g_sh) return true;
    const bool ok = WaitHelper(timeoutMs, [need] { return g_sh->helperDoneValue >= need || g_aborted; });
    if (ok && !g_aborted) {
        AcquireSRWLockExclusive(&g_pendLock);
        if (pend && g_pendingN == pend) g_pendingN = 0;
        ReleaseSRWLockExclusive(&g_pendLock);
        return true;
    }
    if (!g_aborted && WaitForSingleObject(g_helperProc, 0) == WAIT_OBJECT_0) AbortPending("helper skoncil");
    return false;
}

// Most je trvale vypnuty (g_failed): helper ukoncit a sdilene obrazy uvolnit, jinak by do konce hry drzely VRAM (DLSS
// feature, importovane i vlastni obrazy helperu - pri 1440p a S = 3 pres 1 GB). Nevyrizene B (pipeline) nejdriv
// vyresit: potvrzeni helperem, jinak AbortPending (helper ukonci a semHelper uvolni z CPU). Pred ukoncenim helperu
// pockat (max 2 s) na odeslane prikazy mostu: B ceka na signaly, ktere helper uz odeslal (ukonceni procesu by
// semHelper uvolnilo i samo - Windows ho nastavi na UINT64_MAX, viz killtest). Sdilene obrazy az kdyz zadny slot
// nebezi, pripadne pri dalsim konci sceny. Vola Fail a konec sceny (zasah strazce). Jen z vlakna hry.
void StopBridge() {
    g_req = REQ_NONE;
    if (!g_helperStopped) {
        g_helperStopped = true;
        if (g_helperProc) {
            if (!AckPending(kAckWaitMs) && !g_aborted) AbortPending("most vypinam, helper nepotvrdil snimek");
            if (g_vkReady && !g_devLost) {
                VkFence f[2 * kSlots];
                uint32_t n = 0;
                for (int i = 0; i < kSlots; ++i) {
                    if (g_fenceAUsed[i]) f[n++] = g_fenceA[i];
                    if (g_fenceBUsed[i]) f[n++] = g_fenceB[i];
                }
                const VkResult r = n ? vk.vkWaitForFences(g_dev, n, f, VK_TRUE, kFenceTimeoutNs) : VK_SUCCESS;
                if (r != VK_SUCCESS) LogVk(r, "cekani na snimky mostu pred ukoncenim helperu");
            }
            const bool alive = WaitForSingleObject(g_helperProc, 0) != WAIT_OBJECT_0;
            if (alive) {
                TerminateProcess(g_helperProc, 4);
                WaitForSingleObject(g_helperProc, 5000);
            }
            Log("most: helper %s (most je vypnuty, jeho VRAM se uvolni)", alive ? "ukoncen" : "uz nebezi");
        }
    }
    if (!g_vkReady || g_devLost || !HaveSharedImages()) return;
    for (int i = 0; i < kSlots; ++i)
        if (!FinishSlot(i, 0)) return;  // prikazy slotu jeste bezi -> pri dalsim konci sceny
    DestroySharedImages();
    Log("most: sdilene obrazy uvolneny");
}

// Strazce pipeline: kazdych 10 ms (nebo hned pri skonceni helperu) zkontroluje snimek, na ktery ceka GPU. Bezi ve
// vlastnim vlakne, protoze hra muze v te dobe viset uvnitr DXVK (napr. Present ceka na GPU) a do konce sceny se
// nedostane. Konci po zasahu nebo kdyz je most trvale vypnuty a nic neceka.
DWORD WINAPI WatchdogProc(void*) {
    bool dead = false;
    ULONGLONG busySeen = 0;  // naposledy videna dlouha prace helperu (limit se pocita az od jejiho konce)
    uint64_t warnedFor = 0;  // snimek, pro ktery uz WARN v logu je
    for (;;) {
        if (dead)
            Sleep(10);
        else
            dead = WaitForSingleObject(g_helperProc, 10) == WAIT_OBJECT_0;
        AcquireSRWLockExclusive(&g_pendLock);
        const uint64_t pend = g_pendingN;
        const ULONGLONG since = g_pendingTick;
        const bool stop = g_aborted || (g_failed && !pend);
        ReleaseSRWLockExclusive(&g_pendLock);
        if (stop) return 0;
        if (!pend) continue;
        MemoryBarrier();
        if (g_sh->helperDoneValue >= pend) continue;
        const ULONGLONG now = GetTickCount64();
        if (!dead && HelperBusy()) {
            busySeen = now;
            continue;
        }
        const ULONGLONG waited = now - (since > busySeen ? since : busySeen);
        if (!dead && waited >= kPendingWarnMs && warnedFor != pend) {
            warnedFor = pend;
            Log("most: WARN - helper nepotvrdil snimek %llu do %lu ms (GPU hry na nej ceka) - cekam dal, nejvys %lu s "
                "(pomale PC?)", (unsigned long long)pend, kPendingWarnMs, kPendingTimeoutMs / 1000);
        }
        if (dead || waited >= kPendingTimeoutMs)
            AbortPending(dead ? "helper skoncil" : "helper nepotvrdil snimek do 30 s");
    }
}

const char* SyncName(bool pipe) { return pipe ? "pipeline" : "synchronni"; }

}  // namespace

void Interop_SetMode(uint32_t mode) {
    g_mode = mode % BMSD_MODE_COUNT;
    if (g_failed && g_mode) {
        Log("most: drive selhal, rezim %s nelze zapnout (restartuj hru)", kModeNames[g_mode]);
        g_mode = BMSD_MODE_OFF;
        return;
    }
    Log("most: rezim %s", kModeNames[g_mode]);
}

uint32_t Interop_GetMode() { return g_mode; }
uint32_t Interop_FgGpuDone() { return g_sh ? g_sh->fgGpuDone : 0u; }

bool Interop_DlaaReady() { return g_sh && !g_failed && g_sh->dlaaStatus > 0 && g_sh->helperRenderOk != 0; }

const char* Interop_WhyIdle() {
    if (g_failed) return "most selhal (viz CHYBA vyse; restartuj hru)";
    if (g_mode == BMSD_MODE_OFF) return "most je vypnuty (Alt+F8 nebo [bridge] StartMode)";
    if (!g_sh || !g_sh->helperReady) return "helper neni pripraven";
    if (g_mode == BMSD_MODE_MIRROR) return "zrcadleni se pri zmensenem renderu nepodporuje";
    if (g_mode == BMSD_MODE_DLAA && g_sh->dlaaStatus < 0) return "DLSS neni dostupne (viz bms_dlss_helper.log)";
    if (g_mode == BMSD_MODE_DLAA && !g_sh->helperRenderOk)
        return "DLSS pro tento render nebezi (viz bms_dlss_helper.log)";
    return nullptr;
}

void Interop_SetPreset(uint32_t preset) { g_preset = preset; }

void Interop_SetInputMode(uint32_t mode) { g_inputMode = mode < BMSD_INPUT_COUNT ? mode : BMSD_INPUT_LDR; }

void Interop_SetSharpness(float s) { g_sharpness = s; }

void Interop_SetDepthMode(uint32_t mode) { g_depthMode = mode < BMSD_DEPTH_COUNT ? mode : BMSD_DEPTH_RAW; }

void Interop_SetJitterSign(int32_t sx, int32_t sy) {
    g_liveSignX = sx;
    g_liveSignY = sy;
}

void Interop_SetOutputScale(float scale) { g_outScale = scale >= 1.0f && scale <= 3.0001f ? scale : 1.0f; }

void Interop_SetUpscaler(uint32_t u) { g_upscalerReq = u < BMSD_UPSCALER_COUNT ? u : BMSD_UPSCALER_AUTO; }

int Interop_ActiveUpscaler(const char** name) {
    if (name) *name = g_sh && !g_failed ? g_sh->helperUpscalerName : "";
    return g_sh && !g_failed ? g_sh->helperUpscaler : 0;
}

static bool g_vkBlocked;
void Interop_BlockVk() { g_vkBlocked = true; }
bool Interop_VkBlocked() { return g_vkBlocked; }
bool Interop_TakeFailNote() { return InterlockedExchange(&g_failNote, 0) != 0; }
bool Interop_HelperBusy() { return HelperBusy(); }
int Interop_DlssBroken() { return g_sh && !g_failed && g_sh->helperReady ? g_sh->helperDlssBroken : 0; }
int Interop_GpuHint() { return g_gpuHint > 0 ? g_gpuHint : 0; }
int Interop_FsrBroken() { return g_sh && !g_failed && g_sh->helperReady ? g_sh->helperFsrBroken : 0; }

float Interop_ActiveOutScale() {
    if (!g_sh || g_failed || g_sh->dlaaStatus <= 0) return 1.0f;
    const float s = g_sh->helperOutScale;
    return s >= 1.0f && s <= 3.0001f ? s : 1.0f;
}

bool Interop_FrameBridged(uint64_t frame) { return g_haveOkFrame && !g_failed && g_lastOkFrame == frame; }

void Interop_SetDownsampleFilter(uint32_t filter) { g_downFilter = filter < BMSD_DOWN_COUNT ? filter : 0; }

void Interop_SetJitterPhases(uint32_t phases, bool autoMode) {
    g_jitterPhases = phases;
    g_jitterPhasesAuto = autoMode ? 1 : 0;
}

void Interop_SetPipeline(bool on) {
    g_pipeline = on ? 1 : 0;
    Log("synchronizace mostu: %s%s", on ? "pipeline (CPU hry na helper neceka, B ceka na GPU)" :
                                          "synchronni (CPU hry ceka na odeslani helperu, puvodni)",
        on && g_vkReady && !g_pipeOk ? " - neni k dispozici, bezi synchronni (viz vyse)" : "");
}

bool Interop_GetPipeline() { return g_pipeline != 0; }

void Interop_OnPresent() {
    const LONGLONG now = Qpc();
    if (g_lastPresentQpc) g_stFrame.add(QpcMs(g_lastPresentQpc, now));
    g_lastPresentQpc = now;
    if (!g_statStartQpc) g_statStartQpc = now;
    const double el = QpcMs(g_statStartQpc, now);
    if (el < 5000.0) return;
    if (g_stFrame.n) {
        char bridge[760];
        if (g_stTotal.n || g_stSkipped)
            snprintf(bridge, sizeof bridge,
                     "most %s: snimku %u (vynechano %u), konec sceny prum %.3f / max %.2f ms, z toho cekani na helper "
                     "%.3f / %.2f, flush DXVK %.3f / %.2f, odeslani A+B %.3f / %.2f (z toho zamek fronty %.3f / %.2f), slot GPU "
                     "%.3f / %.2f, zaznam kopii %.3f / %.2f ms; GPU: kopie A %.3f / %.2f, B %.3f / %.2f, A->B (helper) "
                     "%.2f / %.2f ms",
                     SyncName(g_pipeline && g_pipeOk), g_stTotal.n, g_stSkipped, g_stTotal.avg(), g_stTotal.max,
                     g_stWait.avg(), g_stWait.max, g_stFlush.avg(), g_stFlush.max, g_stSubmit.avg(), g_stSubmit.max,
                     g_stLock.avg(), g_stLock.max,
                     g_stSlot.avg(), g_stSlot.max, g_stRec.avg(), g_stRec.max, g_stGpuA.avg(), g_stGpuA.max,
                     g_stGpuB.avg(), g_stGpuB.max, g_stGpuAB.avg(), g_stGpuAB.max);
        else
            snprintf(bridge, sizeof bridge, "most vypnuty");
        // MV pohyblivych objektu: pocty a CPU casy z proxy, statistika prijatych pixelu a validace z helperu
        char obj[640], objHelper[256] = "";
        ProxyObjMvPerf(obj, sizeof obj);
        if (g_sh && !g_failed && g_sh->objMvStatSeq != g_objStatSeq) {
            g_objStatSeq = g_sh->objMvStatSeq;
            int k = snprintf(objHelper, sizeof objHelper, "; helper (%u snimku): prijato %.0f px/snimek, odmitnuto hloubkou %.0f",
                             g_sh->objMvStatFrames, g_sh->objMvStatAccepted, g_sh->objMvStatRejected);
            if (g_sh->objMvStatValPixels > 0.0f && k > 0 && k < (int)sizeof objHelper)
                snprintf(objHelper + k, sizeof objHelper - k, ", validace %.0f px/snimek: prumer %.4f / max %.2f px (%s)",
                         g_sh->objMvStatValPixels, g_sh->objMvStatValMean, g_sh->objMvStatValMax,
                         g_sh->objMvStatValMean < 0.05f ? "OK" : "CHYBA, cil < 0.05");
        }
        char partB[720], lat[512], rm[320];  // partB: cast B vcetne pocitadel LightsB (400 B radek orezavalo)
        ProxyPartBPerf(partB, sizeof partB);  // cast B: prazdne, kdyz se v intervalu neobjevila
        ProxyLatencyPerf(lat, sizeof lat);    // regulator latence: prazdne pri [latency] Mode=0
        ProxyReactivePerf(rm, sizeof rm);     // maska reaktivity: prazdne pri [upscale] ReactiveMask=0
        char cam[200];
        ProxyCamInputPerf(cam, sizeof cam);   // otaceni kamery a vstup mysi za interval
        Log("vykon %.1f s: %.1f FPS, snimek prum %.2f / max %.2f ms | %s | %s%s%s%s%s%s", el / 1000.0,
            g_stFrame.n * 1000.0 / el, g_stFrame.avg(), g_stFrame.max, bridge, obj, objHelper, partB, lat, rm, cam);
    } else {
        char obj[640];
        ProxyObjMvPerf(obj, sizeof obj);  // jen vynulovat pocitadla intervalu
        ProxyPartBPerf(obj, sizeof obj);
        ProxyLatencyPerf(obj, sizeof obj);
        ProxyReactivePerf(obj, sizeof obj);
        ProxyCamInputPerf(obj, sizeof obj);
    }
    g_stFrame = g_stTotal = g_stWait = g_stFlush = g_stSubmit = g_stLock = g_stSlot = g_stRec = g_stGpuA = g_stGpuB = g_stGpuAB = Acc();
    g_stSkipped = 0;
    g_statStartQpc = now;
}

void Interop_BeforeReset() {
    // Historie DLSS po Resetu neplati (resetHint), jitter a mip bias az po snimku, ktery most po Resetu zpracuje,
    // novy obraz sceny do logu. Prvni konce sceny po Resetu se vynechaji (hra i DXVK zdroje teprve vytvari).
    g_haveBridgedFrame = false;
    g_haveOkFrame = false;
    g_bridgeNForPresent = 0;
    g_loggedImages = false;
    g_loggedObjMv = false;
    g_loggedMask = false;
    g_skipScenes = kSkipAfterReset;
    if (!g_vkReady) return;
    // Reset (a hra pred nim) uvolnuje render targety: nase odeslane kopie nad nimi musi dobehnout a reference na
    // povrchy hry je nutne pustit (u zarizeni bez Ex by s nimi Reset selhal). Bez cekani na semafory po Resetu.
    const unsigned held = HeldRefs();
    const ULONGLONG t0 = GetTickCount64();
    // Pipeline: B posledniho snimku muze na GPU cekat na helper. Kdyz ho helper do 2 s neodesle, GPU uvolnit
    // (helper ukoncit, signal z CPU) - jinak by Reset v DXVK cekal na necinnost GPU navzdy.
    if (g_sh && !AckPending(2000) && !g_aborted) AbortPending("helper nepotvrdil snimek pred Reset");
    const bool ok = FinishAllSlots();
    Log("most: pred Reset zarizeni hry - rozpracovane snimky mostu %s (%llu ms), uvolneno povrchu hry %u/%u%s",
        ok ? "dobehly" : "NEDOBEHLY", (unsigned long long)(GetTickCount64() - t0), held - HeldRefs(), held,
        g_devLost ? ", zarizeni Vulkan je ztracene" : "");
    if (!ok && !g_failed) Fail(g_devLost ? "zarizeni Vulkan DXVK ztraceno" : "GPU nedokoncila snimky mostu pred Reset");
}

static bool SceneEnd(const SceneEndInfo& in);

// Barva sceny s MSAA (most snimek vynechal): cas posledniho vyskytu, 0 = posledni snimek prosel kontrolou formatu
static volatile LONG64 g_msaaSeenMs;

bool Interop_MsaaBlocked() {
    const ULONGLONG t = (ULONGLONG)InterlockedCompareExchange64(&g_msaaSeenMs, 0, 0);
    return t && GetTickCount64() - t < 2000;
}

bool Interop_OnSceneEnd(const SceneEndInfo& in) {
    const bool on = g_mode != BMSD_MODE_OFF && !g_failed && in.color;
    const LONGLONG t0 = on ? Qpc() : 0;
    g_fWait = g_fFlush = g_fSubmit = g_fSlot = g_fRec = g_fLock = 0.0;
    const bool queued = SceneEnd(in);
    if (on) {
        g_stTotal.add(QpcMs(t0, Qpc()));
        g_stWait.add(g_fWait);
        g_stFlush.add(g_fFlush);
        g_stSubmit.add(g_fSubmit);
        g_stLock.add(g_fLock);
        g_stSlot.add(g_fSlot);
        g_stRec.add(g_fRec);
        if (!(g_haveOkFrame && g_lastOkFrame == in.frame)) ++g_stSkipped;
    }
    return queued;
}

// Vulkan DXVK a hlavni helper: inicializace a spusteni pri prvnim pouziti (konec sceny, nebo prvni Present pri
// prezentaci v helperu - Interop_EnsureHelper). 0 = selhalo (most je vypnuty, Fail), 1 = helper prave spusten
// (inicializuje se), 2 = uz bezel.
static int EnsureHelperStarted(IDirect3DDevice9* dev) {
    if (!g_vkReady && !InitVk(dev)) return Fail("inicializace Vulkanu"), 0;
    if (g_helperStarted) return 2;
    g_helperStarted = true;
    if (!StartHelper()) return Fail("spusteni helperu"), 0;
    const bool fns = vk.vkGetSemaphoreCounterValue && vk.vkSignalSemaphore && vk.vkWaitSemaphores;
    if (fns) g_watchThread = CreateThread(nullptr, 0, WatchdogProc, nullptr, 0, nullptr);
    g_pipeOk = fns && g_watchThread;
    if (!g_pipeOk)
        Log("most: pipeline nelze (%s) - jen synchronni rezim",
            fns ? "nejde spustit vlakno strazce" : "chybi vkSignalSemaphore/vkWaitSemaphores");
    else
        Log("most: synchronizace %s", SyncName(g_pipeline != 0));
    return 1;
}

void Interop_SetFgPresenter(bool on) { g_fgPresenter = on; }
void Interop_FgPresenterStarting() {
    if (!g_bridgeBegan) g_fgEarly = true;
}

HANDLE Interop_EnsureHelper(IDirect3DDevice9* dev) {
    if (g_failed || g_helperStopped || !dev) return nullptr;
    if (g_vkReady && dev != g_vkD3D) return nullptr;  // most je svazany s jinym zarizenim
    if (!EnsureHelperStarted(dev) || g_failed || g_helperStopped) return nullptr;
    return g_helperProc;
}

uint64_t Interop_TakeBridgeN() {
    const uint64_t n = g_bridgeNForPresent;
    g_bridgeNForPresent = 0;
    return g_failed ? 0 : n;
}

// true = kopie B je odeslana (vysledek mostu bude v cele barve sceny)
static bool SceneEnd(const SceneEndInfo& in) {
    // Dobehle sloty uvolni drzene povrchy hry i kdyz je most vypnuty (bez cekani).
    if (g_vkReady && !g_devLost)
        for (int i = 0; i < kSlots; ++i) FinishSlot(i, 0);
    // Most vypnul strazce (AbortPending) nebo sdilene obrazy jeste cekaly na sloty: dokoncit uklid (jednou, levne)
    if (g_failed) return StopBridge(), false;
    if (g_mode == BMSD_MODE_OFF || !in.color) return false;
    if (g_vkReady && in.dev != g_vkD3D)
        return Fail("hra vytvorila nove zarizeni D3D9 (most je svazany s puvodnim)"), false;
    if (g_skipScenes) {
        if (--g_skipScenes == 0) Log("most: po Resetu zarizeni znovu zpracovava snimky");
        return false;
    }
    if (EnsureHelperStarted(in.dev) != 2) return false;  // selhani (Fail), nebo se helper teprve inicializuje
    if (WaitForSingleObject(g_helperProc, 0) == WAIT_OBJECT_0) {
        DWORD code = STILL_ACTIVE;
        GetExitCodeProcess(g_helperProc, &code);
        Log("most: helper skoncil (kod 0x%08lX = %s), zprava: %s", (unsigned long)code, ExitCodeText(code),
            g_sh->helperMsg);
        AbortPending("helper skoncil");  // pipeline: B posledniho snimku na GPU uvolnit hned (jinak strazce)
        return Fail("helper neocekavane skoncil"), false;
    }
    if (g_sh->helperError) {
        Log("most: helper hlasi chybu: %s", g_sh->helperMsg);
        return Fail("chyba v helperu"), false;
    }
    if (!g_sh->helperReady) return false;
    if (g_gpuHint < 0) {
        g_gpuHint = g_sh->helperGpuHint;
        if (g_gpuHint)
            Log("most: POZOR - %s (DLSS%s nejde; Windows Nastaveni -> System -> Obrazovka -> Grafika -> bms.exe -> "
                "Vysoky vykon, pak restart hry)",
                g_gpuHint == 1 ? "hra bezi na integrovane grafice, v systemu je i vykonnejsi karta"
                               : "hra bezi na GPU bez DLSS, v systemu je i karta NVIDIA",
                g_gpuHint == 1 ? " / DLSS-G" : "");
    }
    // Prezentace v helperu (FG): helper bezi uz od prvniho Present, takze by most zacal hned prvnim koncem sceny po
    // nacteni mapy. Prvnich par koncu sceny se vynecha ([fg] BridgeStartSkip, vychozi 120): bez prezentace v helperu
    // most zacina az po startu helperu (~1,5 s po prvnim konci sceny), stejne zpozdeni se tu zachovava.
    if (g_fgPresenter && g_fgEarly) {
        static int s_skip = -1;
        if (s_skip < 0) {
            const std::wstring ini = std::wstring(ProxyDir()) + L"\\bms_dlss.ini";
            s_skip = (int)GetPrivateProfileIntW(L"fg", L"BridgeStartSkip", 120, ini.c_str());
            if (s_skip < 0) s_skip = 0;
            if (s_skip) Log("most: prezentace v helperu - prvnich %d koncu sceny se vynecha ([fg] BridgeStartSkip)", s_skip);
        }
        if (s_skip > 0) {
            if (--s_skip == 0) Log("most: zacina zpracovavat snimky");
            return false;
        }
    }
    g_bridgeBegan = true;
    // Pipeline: B minuleho snimku muze na GPU cekat na helper; dalsi zapis do sdilene pameti az po jeho potvrzeni.
    {
        const LONGLONG w0 = Qpc();
        // Helper uz minule nestihl: hra na nej dal neceka 100 ms na kazdem konci sceny (jinak by jela na <= 10 FPS),
        // jen se podiva
        const bool acked = AckPending(g_ackStallSince ? 1 : kAckWaitMs);
        g_fWait += QpcMs(w0, Qpc());
        if (!acked) {
            if (g_aborted || g_failed) return false;  // helper skoncil: GPU uz uvolnena, most vypnuty (log v AbortPending)
            // Helper je pomaly (napr. obnova DLSS feature): snimek vynechat jako drive. Strazce hlida limit.
            if (AckStalled("helper jeste neprevzal minuly snimek")) Fail("helper opakovane neodpovida");
            return false;
        }
        AckResumed();
    }
    // Import zdroju nebo nova DLSS feature v helperu jeste bezi: snimek vynechat (bez cekani)
    if (g_req != REQ_NONE && !PollReq()) return false;

    D3DSURFACE_DESC cd = {};
    if (FAILED(in.color->GetDesc(&cd)) || cd.Format != D3DFMT_A8R8G8B8 || cd.MultiSampleType != D3DMULTISAMPLE_NONE ||
        !cd.Width || !cd.Height) {
        // vkCmdCopyImage potrebuje stejny pocet vzorku a kompatibilni format jako sdileny BGRA8 obraz
        if (cd.MultiSampleType != D3DMULTISAMPLE_NONE) InterlockedExchange64(&g_msaaSeenMs, (LONG64)GetTickCount64());
        static bool s_logged;
        if (!s_logged) {
            s_logged = true;
            Log("most: barva sceny %ux%u fmt=%u ms=%u neni RGBA8 bez MSAA - snimek vynechan", cd.Width, cd.Height,
                (unsigned)cd.Format, (unsigned)cd.MultiSampleType);
        }
        return false;
    }
    InterlockedExchange64(&g_msaaSeenMs, 0);
    // Render tohoto snimku: cast A = cely povrch sceny, cast B = sub-rect od (0, 0)
    const uint32_t rw = in.renderW && in.renderW < cd.Width ? in.renderW : cd.Width;
    const uint32_t rh = in.renderH && in.renderH < cd.Height ? in.renderH : cd.Height;
    const bool sub = rw != cd.Width || rh != cd.Height;
    // Hloubka sceny: skutecny Vulkan format od DXVK (D24S8 hry je na AMD D32S8). Zjistuje se pred vytvorenim
    // sdilenych obrazu, aby DEPTH_IN vznikl rovnou ve stejnem formatu; pri zmene formatu se obrazy vytvori znovu.
    // Handle VkImage obrazu DXVK se cte az po FlushRenderingCommands: CS vlakno DXVK ma pak prikazy snimku zpracovane,
    // takze pripadna vymena uloziste obrazu (premisteni pameti DXVK) uz probehla a nase kopie nepracuji se starym
    // VkImage (podezreni na vzacny MMU fault na fronte DXVK pri startu FG). Pozdejsi flush pred odeslanim zustava.
    // [fg] EarlyFlush=0 = puvodni poradi (jen pro srovnani).
    if (BridgeEarlyFlush()) g_idev->FlushRenderingCommands();
    VkImageLayout dsL = VK_IMAGE_LAYOUT_GENERAL;
    VkImage dsImg = VK_NULL_HANDLE;
    D3DSURFACE_DESC dd = {};
    if (in.ds && SUCCEEDED(in.ds->GetDesc(&dd)) && dd.Width == cd.Width && dd.Height == cd.Height &&
        (dd.Format == D3DFMT_D24S8 || dd.Format == D3DFMT_D24X8 || dd.Format == D3DFMT_D24FS8) &&
        dd.MultiSampleType == D3DMULTISAMPLE_NONE) {
        VkFormat dsFmt = VK_FORMAT_UNDEFINED;
        dsImg = GetVkImage(in.ds, &dsL, &dsFmt);
        if (dsImg && !BridgeDepthFormat(dsFmt)) {
            static bool s_loggedFmt;
            if (!s_loggedFmt) {
                s_loggedFmt = true;
                Log("most: hloubka sceny ma Vulkan format %d (D3D %u) - most ji neumi predat, snimky bez hloubky",
                    (int)dsFmt, (unsigned)dd.Format);
            }
            dsImg = VK_NULL_HANDLE;
        }
        if (dsImg && dsFmt != g_depthFmt) {
            if (HaveSharedImages())
                Log("most: hloubka sceny je ted %s (Vulkan %d), dosud %s - sdilene obrazy se vytvori znovu",
                    DepthFmtName(dsFmt), (int)dsFmt, DepthFmtName(g_depthFmt));
            g_depthFmt = dsFmt;
            g_resW = g_resH = 0;  // vynuti CreateSharedImages (novy DEPTH_IN, helper importuje novou generaci)
        }
        if (dsImg && !g_loggedDepthFmt) {
            g_loggedDepthFmt = true;
            Log("most: hloubka sceny %s (D3D format %u, Vulkan %d)", DepthFmtName(dsFmt), (unsigned)dd.Format,
                (int)dsFmt);
        }
    }
    // MV objektu nebo maska reaktivity zapnuta za behu, ale sdilene obrazy pro ne jsou jen 1x1: nova generace
    if (HaveSharedImages() && ((in.objMvOn && !g_objMvWant) || (in.maskOn && !g_maskWant))) {
        Log("most: %s zapnuta - sdilene obrazy se vytvori znovu v plne velikosti",
            in.objMvOn && !g_objMvWant ? "MV objektu" : "maska reaktivity");
        g_resW = g_resH = 0;
    }
    if (cd.Width != g_resW || cd.Height != g_resH) {
        g_objMvWant = in.objMvOn;
        g_maskWant = in.maskOn;
        const int r = CreateSharedImages(cd.Width, cd.Height, rw, rh);
        if (!r) return Fail("vytvoreni sdilenych obrazu"), false;
        if (r == 2) return false;  // helper import jeste nepotvrdil
    }
    // Cast B: S se nepouziva (helper ho bere jako 1) - zmena S (Alt+F3) by jen zbytecne cekala na GPU a vytvarela
    // feature znovu. Pozadavek zustava v g_outScale a odejde s prvni zpravou v casti A.
    const float wantScale = sub ? g_outScaleSent : (float)g_outScale;
    if (g_presetSent != g_preset || g_inputModeSent != g_inputMode || g_outScaleSent != wantScale ||
        g_renderSentW != rw || g_renderSentH != rh || g_upscalerSent != g_upscalerReq) {
        // Zmena presetu, vstupu, vystupu (S) nebo renderu (cast B, zmena mat_viewportscale) DLSS: helper znovu
        // vytvori DLSS feature (frameValue = 0 -> bez zpracovani snimku; sdilene obrazy se nemeni). Hra na to jednou
        // pocka (stejne jako pri importu zdroju). Helper potvrzuje prijeti pozadavku (i kdyz S nebo render nejde -
        // skutecny stav je zvlast v helperOutScale a helperRenderOk), takze se neceka na shodu, ktera by pri
        // fallbacku nenastala.
        g_presetSent = g_preset;
        g_sh->preset = g_presetSent;
        g_inputModeSent = g_inputMode;
        g_sh->inputMode = g_inputModeSent;
        g_outScaleSent = wantScale;
        g_sh->outScale = g_outScaleSent;
        g_upscalerSent = g_upscalerReq;
        g_sh->upscaler = g_upscalerSent;
        g_renderSentW = rw;
        g_renderSentH = rh;
        g_sh->renderWidth = rw;
        g_sh->renderHeight = rh;
        g_sh->frameValue = 0;
        g_postedN = 0;  // helperDoneValue bude 0 (zprava bez snimku); minuly snimek uz helper prevzal (AckPending)
        // Bez potvrzeni do kReqSyncMs: snimky se vynechavaji, dokud helper zmenu nepotvrdi (PollReq), most zustava.
        if (!StartReq(REQ_CFG)) return false;
    }
    const uint32_t w = g_resW, h = g_resH;
    if (sub) {
        // Cast B: bez bezici DLSS pro tento render most nedela nic (zadne kopie ani flush) a sub-rect roztahne engine
        // sam - obraz je stejny, jako kdyby ho bilinearne roztahl helper, jen levneji. Neni to selhani mostu: helper
        // to zkusi znovu pri dalsi zmene renderu. Zrcadleni se v casti B nepodporuje vubec.
        if (g_mode == BMSD_MODE_MIRROR) {
            if (!g_loggedMirrorB) {
                g_loggedMirrorB = true;
                Log("most: cast B (render %ux%u v %ux%u): rezim zrcadleni se nepodporuje - obraz roztahuje engine", rw,
                    rh, w, h);
            }
            return false;
        }
        if (g_mode == BMSD_MODE_DLAA && !(g_sh->dlaaStatus > 0 && g_sh->helperRenderOk)) {
            if (g_subIdleW != rw || g_subIdleH != rh) {
                g_subIdleW = rw;
                g_subIdleH = rh;
                Log("most: cast B (render %ux%u v %ux%u): DLSS pro tento vstup nebezi (%s) - most nic nedela, obraz "
                    "roztahuje engine",
                    rw, rh, w, h, g_sh->helperMsg[0] ? g_sh->helperMsg : "viz bms_dlss_helper.log");
            }
            return false;
        }
        g_subIdleW = g_subIdleH = 0;
    }

    // Vulkan obrazy DXVK (pred zamcenim fronty - jde o volani D3D9 objektu)
    VkImageLayout colL = VK_IMAGE_LAYOUT_GENERAL;
    VkImage colImg = GetVkImage(in.color, &colL);
    if (g_simg[BMSD_IMG_DEPTH_IN].fmt != g_depthFmt) dsImg = VK_NULL_HANDLE;  // kopie jen mezi stejnymi formaty
    // MV pohyblivych objektu: cil OBJMV proxy, jen kdyz se do nej v tomto snimku kreslilo (jinak 59 MB kopie zbytecne)
    VkImageLayout objL = VK_IMAGE_LAYOUT_GENERAL;
    VkImage objImg = VK_NULL_HANDLE;
    if (in.objMv && in.objMvCount && g_simg[BMSD_IMG_OBJMV_IN].w == w && g_simg[BMSD_IMG_OBJMV_IN].h == h &&
        SUCCEEDED(in.objMv->GetDesc(&dd)) && dd.Width == w && dd.Height == h &&
        dd.Format == D3DFMT_A32B32G32R32F && dd.MultiSampleType == D3DMULTISAMPLE_NONE)
        objImg = GetVkImage(in.objMv, &objL);
    // Maska reaktivity: cil MASKA proxy, jen kdyz v nem v tomto snimku neco je
    VkImageLayout maskL = VK_IMAGE_LAYOUT_GENERAL;
    VkImage maskImg = VK_NULL_HANDLE;
    if (in.mask && in.maskDraws && g_simg[BMSD_IMG_MASK_IN].w == w && g_simg[BMSD_IMG_MASK_IN].h == h &&
        SUCCEEDED(in.mask->GetDesc(&dd)) && dd.Width == w && dd.Height == h &&
        dd.Format == D3DFMT_A8R8G8B8 && dd.MultiSampleType == D3DMULTISAMPLE_NONE)
        maskImg = GetVkImage(in.mask, &maskL);
    if (!colImg) return false;
    if (maskImg && !g_loggedMask) {
        g_loggedMask = true;
        Log("most: maska reaktivity: MASKA=0x%llX (layout %d), prvni kopie (%u draw)", (unsigned long long)maskImg,
            maskL, in.maskDraws);
    }
    if (objImg && !g_loggedObjMv) {
        g_loggedObjMv = true;
        Log("most: MV objektu: OBJMV=0x%llX (layout %d), prvni kopie (%u draw)", (unsigned long long)objImg, objL,
            in.objMvCount);
    }
    if (!g_loggedImages) {
        g_loggedImages = true;
        Log("most: obrazy sceny: barva=0x%llX (layout %d), depth=0x%llX (layout %d)", (unsigned long long)colImg, colL,
            (unsigned long long)dsImg, dsL);
    }
    // vkCmdCopyImage smi jen TRANSFER_SRC/DST_OPTIMAL nebo GENERAL. Obrazy DXVK jsou ve svem vychozim
    // layoutu (napr. COLOR/DEPTH_STENCIL_ATTACHMENT_OPTIMAL): kolem kopie je prevedeme a pak presne vratime.
    const VkImageLayout colSrcL = CopyLayout(colL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImageLayout colDstL = CopyLayout(colL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageLayout dsSrcL = CopyLayout(dsL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImageLayout objSrcL = CopyLayout(objL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImageLayout maskSrcL = CopyLayout(maskL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    int s = g_slot;
    g_slot = (g_slot + 1) % kSlots;
    // Slot z doby pred tremi snimky: normalne uz davno hotovy. Kdyz GPU nedobehne v limitu (zaseknuta nebo ztracena),
    // most se vypne misto nekonecneho cekani.
    const LONGLONG qs = Qpc();
    const bool slotOk = FinishSlot(s, kFenceTimeoutNs);
    g_fSlot += QpcMs(qs, Qpc());
    if (!slotOk) return Fail(g_devLost ? "zarizeni Vulkan DXVK ztraceno" : "GPU nedokoncila snimek mostu v limitu"), false;
    if (g_qpool && g_tsSlotUsed[s]) {
        g_tsSlotUsed[s] = false;
        uint64_t t[4] = {};
        if (vk.vkGetQueryPoolResults(g_dev, g_qpool, s * 4, 4, sizeof t, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) ==
                VK_SUCCESS &&
            t[1] >= t[0] && t[2] >= t[1] && t[3] >= t[2]) {
            g_stGpuA.add((double)(t[1] - t[0]) * g_tsMs);
            g_stGpuAB.add((double)(t[2] - t[1]) * g_tsMs);
            g_stGpuB.add((double)(t[3] - t[2]) * g_tsMs);
        }
    }
    const LONGLONG qrec = Qpc();
    const uint64_t N = ++g_frameValue;
    const uint32_t EXT = VK_QUEUE_FAMILY_EXTERNAL;
    const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL;
    const VkImageAspectFlags COLOR = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageAspectFlags DS = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    // ---- A: scena -> sdilene vstupy ----
    VkCommandBuffer cmd = g_cmdA[s];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkResetCommandBuffer(cmd, 0);
    vk.vkBeginCommandBuffer(cmd, &bi);
    if (g_qpool) {
        vk.vkCmdResetQueryPool(cmd, g_qpool, s * 4, 4);
        vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qpool, s * 4 + 0);
    }
    {
        // Sdilene vstupy se helperu predavaji vsechny kazdy snimek (i OBJMV_IN bez kopie a LINDEPTH_IN, do ktereho se
        // uz nekopiruje: helper obrazy prebira a vraci v jedne smycce, obsah bez kopie necte - objMvCount = 0).
        VkImageMemoryBarrier pre[10];
        uint32_t n = 0;
        pre[n++] = Barrier(colImg, COLOR, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, colL, colSrcL);
        if (dsImg)
            pre[n++] = Barrier(dsImg, DS, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, dsL, dsSrcL);
        if (objImg)
            pre[n++] = Barrier(objImg, COLOR, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, objL, objSrcL);
        if (maskImg)
            pre[n++] = Barrier(maskImg, COLOR, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, maskL, maskSrcL);
        for (int i : {BMSD_IMG_COLOR_IN, BMSD_IMG_DEPTH_IN, BMSD_IMG_LINDEPTH_IN, BMSD_IMG_OBJMV_IN, BMSD_IMG_MASK_IN}) {
            SharedImg& si = g_simg[i];
            pre[n++] = si.fresh ? Barrier(si.img, si.barrierAspect, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                          VK_IMAGE_LAYOUT_UNDEFINED, GEN)
                                : Barrier(si.img, si.barrierAspect, 0, VK_ACCESS_TRANSFER_WRITE_BIT, GEN, GEN, EXT,
                                          g_qf);
        }
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                nullptr, 0, nullptr, n, pre);

        // Jen render (cast A: cely obraz; cast B: sub-rect od (0, 0), mimo nej je v povrchu hry stary obsah)
        VkImageCopy rc = CopyRegion(COLOR, rw, rh);
        vk.vkCmdCopyImage(cmd, colImg, colSrcL, g_simg[BMSD_IMG_COLOR_IN].img, GEN, 1, &rc);
        if (dsImg) {
            VkImageCopy rd = CopyRegion(VK_IMAGE_ASPECT_DEPTH_BIT, rw, rh);
            vk.vkCmdCopyImage(cmd, dsImg, dsSrcL, g_simg[BMSD_IMG_DEPTH_IN].img, GEN, 1, &rd);
        }
        if (objImg) vk.vkCmdCopyImage(cmd, objImg, objSrcL, g_simg[BMSD_IMG_OBJMV_IN].img, GEN, 1, &rc);
        if (maskImg) vk.vkCmdCopyImage(cmd, maskImg, maskSrcL, g_simg[BMSD_IMG_MASK_IN].img, GEN, 1, &rc);

        VkImageMemoryBarrier post[11];
        n = 0;
        // obrazy DXVK zpet do jejich layoutu
        const VkAccessFlags dxvkAcc = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        post[n++] = Barrier(colImg, COLOR, VK_ACCESS_TRANSFER_READ_BIT, dxvkAcc, colSrcL, colL);
        if (dsImg) post[n++] = Barrier(dsImg, DS, VK_ACCESS_TRANSFER_READ_BIT, dxvkAcc, dsSrcL, dsL);
        if (objImg) post[n++] = Barrier(objImg, COLOR, VK_ACCESS_TRANSFER_READ_BIT, dxvkAcc, objSrcL, objL);
        if (maskImg) post[n++] = Barrier(maskImg, COLOR, VK_ACCESS_TRANSFER_READ_BIT, dxvkAcc, maskSrcL, maskL);
        for (int i : {BMSD_IMG_COLOR_IN, BMSD_IMG_DEPTH_IN, BMSD_IMG_LINDEPTH_IN, BMSD_IMG_OBJMV_IN, BMSD_IMG_MASK_IN}) {
            SharedImg& si = g_simg[i];
            post[n++] = Barrier(si.img, si.barrierAspect, VK_ACCESS_TRANSFER_WRITE_BIT, 0, GEN, GEN, g_qf, EXT);
            si.fresh = false;
        }
        SharedImg& out = g_simg[BMSD_IMG_COLOR_OUT];
        if (out.fresh) {  // jednorazova inicializace a predani helperu
            post[n++] = Barrier(out.img, COLOR, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, GEN, g_qf, EXT);
            out.fresh = false;
        }
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                nullptr, 0, nullptr, n, post);
    }
    if (g_qpool) vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qpool, s * 4 + 1);
    vk.vkEndCommandBuffer(cmd);

    // Povrchy hry (a OBJMV proxy) drzet, dokud prikazy slotu nedobehnou (FinishSlot): DXVK nase prikazy nevidi.
    {
        IDirect3DSurface9* refs[5] = {in.color, dsImg ? in.ds : nullptr, nullptr, objImg ? in.objMv : nullptr,
                                      maskImg ? in.mask : nullptr};
        for (int k = 0; k < 5; ++k) {
            if (refs[k]) refs[k]->AddRef();
            g_slotRefs[s][k] = refs[k];
        }
    }
    LONGLONG q0 = Qpc();
    g_fRec += QpcMs(qrec, q0);
    g_idev->FlushRenderingCommands();
    LONGLONG q1 = Qpc();
    g_fFlush += QpcMs(q0, q1);
    // Minuly zverejneny snimek bez B (synchronni timeout): helper ho uz odeslal (AckPending), jeho prace na GPU ale
    // muze jeste cist vstupy, ktere A prepise -> A ceka na semHelper (signal uz je ve fronte helperu, bez strazce).
    // Jinak to poradi zajistuje B minuleho snimku (ve fronte DXVK pred A).
    const uint64_t waitH = g_postedN > g_lastBN ? g_postedN : 0;
    const bool subA = Submit(cmd, g_fenceA[s], waitH ? g_semH : VK_NULL_HANDLE, waitH, g_semP, N);
    g_fSubmit += QpcMs(q1, Qpc());
    if (!subA) {
        ReleaseSlotRefs(s);  // neodeslano -> GPU obrazy nepouziva
        return Fail(g_devLost ? "zarizeni Vulkan DXVK ztraceno (odeslani A)" : "odeslani A"), false;
    }
    g_fenceAUsed[s] = true;
    // Rezim tohoto snimku (klavesa muze prepnout jen mezi snimky; nevyrizene B resi AckPending v obou rezimech)
    const bool pipe = g_pipeline && g_pipeOk;

    // ---- helper ----
    memcpy(g_sh->cam, in.cam, sizeof g_sh->cam);
    memcpy(g_sh->prevCam, in.prevCam, sizeof g_sh->prevCam);
    g_sh->camValid = in.camValid ? 1 : 0;
    g_sh->jitterX = in.jitterX;
    g_sh->jitterY = in.jitterY;
    g_sh->resetHint = (!g_haveBridgedFrame || in.frame != g_lastBridgedFrame + 1) ? 1 : 0;
    g_lastBridgedFrame = in.frame;
    g_haveBridgedFrame = true;
    g_sh->sharpness = g_sharpness;
    g_sh->depthMode = g_depthMode;
    g_sh->liveJitterSignX = g_liveSignX;
    g_sh->liveJitterSignY = g_liveSignY;
    g_sh->downsampleFilter = g_downFilter;
    g_sh->jitterPhases = g_jitterPhases;
    g_sh->jitterPhasesAuto = g_jitterPhasesAuto;
    // MV pohyblivych objektu: objMvCount > 0 jen kdyz se OBJMV v tomto snimku opravdu zkopiroval
    g_sh->objMvMode = in.objMvMode;
    g_sh->objMvTol = in.objMvTol;
    g_sh->objMvCount = objImg ? in.objMvCount : 0;
    g_sh->maskDraws = maskImg ? in.maskDraws : 0;  // maska reaktivity: jen kdyz se v tomto snimku zkopirovala
    g_sh->mode = g_mode;
    g_sh->frameValue = N;
    g_postedN = N;
    ResetEvent(g_evDone);
    g_sh->postQpc = (uint64_t)Qpc();
    MemoryBarrier();
    SetEvent(g_evFrame);
    if (!pipe) {
        // Synchronni rezim: B az po odeslani prace helperu (signal semHelper = N uz je ve fronte GPU)
        const LONGLONG w0 = Qpc();
        const bool done = WaitHelper(g_ackStallSince ? 10 : 100, [N] { return g_sh->helperDoneValue >= N; });
        g_fWait += QpcMs(w0, Qpc());
        if (!done) {
            if (AckStalled("helper neodpovedel vcas")) Fail("helper opakovane neodpovida");
            return false;
        }
        AckResumed();
    }
    // Pipeline: stav helperu (vystup, DLAA) je z minuleho potvrzeni - jen pro log, o snimek pozdeji nevadi.
    LogOutput();  // fallback po selhanem vyhodnoceni pri S > 1 prijde bez handshake
    if (g_sh->dlaaStatus != g_loggedDlaaStatus) {
        g_loggedDlaaStatus = g_sh->dlaaStatus;
        if (g_loggedDlaaStatus > 0)
            Log("most: DLAA v helperu pripraveno");
        else if (g_loggedDlaaStatus < 0)
            Log("most: DLAA v helperu nedostupne (%s) - rezim DLAA jen kopiruje, jitter se nevynucuje",
                g_sh->helperMsg);
    }

    // ---- B: vystup helperu -> scena ----
    cmd = g_cmdB[s];
    vk.vkResetCommandBuffer(cmd, 0);
    vk.vkBeginCommandBuffer(cmd, &bi);
    if (g_qpool) vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qpool, s * 4 + 2);
    {
        VkImage outImg = g_simg[BMSD_IMG_COLOR_OUT].img;
        VkImageMemoryBarrier pre[2] = {
            Barrier(outImg, COLOR, 0, VK_ACCESS_TRANSFER_READ_BIT, GEN, GEN, EXT, g_qf),
            Barrier(colImg, COLOR, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    colL, colDstL)};
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                nullptr, 0, nullptr, 2, pre);
        // Vystup helperu je vzdy cely obraz W x H: v casti B prepise celou barvu sceny (misto enginoveho roztazeni)
        VkImageCopy rc = CopyRegion(COLOR, w, h);
        vk.vkCmdCopyImage(cmd, outImg, GEN, colImg, colDstL, 1, &rc);
        VkImageMemoryBarrier post[2] = {
            Barrier(outImg, COLOR, 0, 0, GEN, GEN, g_qf, EXT),
            Barrier(colImg, COLOR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    colDstL, colL)};
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                nullptr, 0, nullptr, 2, post);
    }
    if (g_qpool) {
        vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qpool, s * 4 + 3);
        g_tsSlotUsed[s] = true;
    }
    vk.vkEndCommandBuffer(cmd);
    bool subB;
    q0 = Qpc();
    if (pipe) {
        // B ceka na GPU na signal, ktery helper mozna jeste neodeslal. Pod zamkem se strazcem: bud B vidi (a pri
        // selhani helperu ho uvolni), nebo uz zasahl a B se neodesle.
        AcquireSRWLockExclusive(&g_pendLock);
        if (g_aborted || g_failed) {
            ReleaseSRWLockExclusive(&g_pendLock);
            return false;
        }
        subB = Submit(cmd, g_fenceB[s], g_semH, N, VK_NULL_HANDLE, 0);
        if (subB) {
            g_pendingN = N;
            g_pendingTick = GetTickCount64();
        }
        ReleaseSRWLockExclusive(&g_pendLock);
    } else {
        subB = Submit(cmd, g_fenceB[s], g_semH, N, VK_NULL_HANDLE, 0);
    }
    g_fSubmit += QpcMs(q0, Qpc());
    if (!subB) return Fail(g_devLost ? "zarizeni Vulkan DXVK ztraceno (odeslani B)" : "odeslani B"), false;
    g_fenceBUsed[s] = true;
    g_lastBN = N;
    g_timeouts = 0;
    g_lastOkFrame = in.frame;  // vysledek mostu je ve scene
    g_haveOkFrame = true;
    g_bridgeNForPresent = N;   // FG: Present tohoto snimku hry patri ke snimku mostu N (Interop_TakeBridgeN)

    if (++g_okFrames == 1 || g_okFrames % 3000 == 0)
        Log("most: snimku tam a zpet: %u (rezim %s)", g_okFrames, kModeNames[g_mode]);
    return true;
}
