// bms-dlss: zobrazeni snimku Frame Generation v procesu hry ([fg] GamePresent=1, vychozi; BMSD_PRESENT_VERSION 7).
//
// Proc: ovladac (NVIDIA G-SYNC, obecne VRR v okne) zapina promennou frekvenci jen procesu, kteremu patri okno v popredi.
// Present z helperu (jiny proces) jde vzdy na pevnou frekvenci monitoru - overeno spikem tools/spike_vrr (cizi proces:
// 100 % intervalu na mrizce obnovy, vlastni proces: VRR, i do vlastniho dcerineho okna) a okem ve hre
// (docs/vrr-diagnostika-vysledek.md). Helper proto snimky (skutecne i generovane) v case presentu jen zkopiruje do
// sdilenych obrazu (gpImg) a zverejni (gpPub + event); vlakno tohoto souboru v procesu hry obraz zkopiruje do vlastniho
// Vulkan swapchainu na prezentacnim okne proxy a zavola vkQueuePresentKHR.
//
// Vlastni VkDevice vytvorene primo na Vulkan ovladaci (bez loaderu a jeho vrstev, viz GpIcdGipa) na stejnem GPU jako
// DXVK, vlastni fronta: present nebrzdi DXVK a overlaye do nej nekresli. Vlakno vlastni swapchain, povrch a importy; hlavni vlakno (Present hry) ho jen spousti a zastavuje (GpStart / GpStop) a cte selhani
// (g_gpFailed -> zaloha). Synchronizace s helperem: timeline gpSemReady (helper: obraz seq zapsan) a gpSemDone (proxy:
// obraz seq precten - signal vzdy, i kdyz se nezobrazi: minimalizace, okno obsazene, vlakno pozadu).

#define GP_DEV_FUNCS(X)                                                                                           \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateImage) X(vkDestroyImage)               \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateSemaphore) \
    X(vkDestroySemaphore) X(vkImportSemaphoreWin32HandleKHR) X(vkCreateCommandPool) X(vkAllocateCommandBuffers)   \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier)               \
    X(vkCmdBlitImage) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences)                      \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)         \
    X(vkQueuePresentKHR) X(vkCmdClearColorImage) X(vkWaitSemaphores) X(vkSignalSemaphore)
#define GP_INST_FUNCS(X)                                                                                          \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceQueueFamilyProperties)                          \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR)                                \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                     \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR)                     \
    X(vkGetPhysicalDeviceFeatures2)

struct {
#define DECL(n) PFN_##n n = nullptr;
    GP_DEV_FUNCS(DECL)
    GP_INST_FUNCS(DECL)
#undef DECL
} gvk;

bool g_gpDevTried, g_gpDevOk;
VkInstance g_gpInst;      // instance a GPU presenteru (vychozi = DXVK; ladeni bit 3 = vlastni instance)
VkPhysicalDevice g_gpPd;
VkDevice g_gpDev;
VkQueue g_gpQ;
uint32_t g_gpQf = UINT32_MAX;
HANDLE g_gpEv, g_gpTh;
volatile LONG g_gpQuit, g_gpFailed;
char g_gpWhy[160];
HWND g_gpWnd;          // prezentacni okno (nastavi hlavni vlakno pred startem vlakna)
bool g_gpActive;       // tento beh presenteru: zobrazuje proxy (helperu gpEnable = 1)
// [fg] GamePresentDebug (ladeni): bit 0 = misto kopie obrazu helperu jen vymazat (bez pristupu k importovane pameti),
// bit 1 = na gpSemReady ceka CPU (ne GPU), bit 2 = gpSemDone signalizuje CPU po fence (ne GPU), bit 3 = vlastni
// VkInstance, bit 4 = fronta index 0, bit 5 = vzdy FIFO, bit 6 = vlastni instance bez implicitnich vrstev, bit 7 = bez
// importu (jen vymazany swapchain), bit 8 = vlastni VkDevice pres loader (ne primo ovladac), bit 10 = zarizeni DXVK,
// bit 11 = zaznam presentu (bms_dlss_gptrace.csv), bit 12 / 13 = globalni priorita fronty HIGH / REALTIME, bit 14 = casna
// kopie (helper zverejni hned po sve kopii s cilovym casem presentu), bit 15 = bez kontroly okna mezi snimky (stare
// chovani: swapchain se pri minimalizaci uvolni az s dalsim snimkem)
bool g_gpOwnDev;  // vlastni VkDevice (jinak zarizeni a fronta DXVK pod LockSubmissionQueue)
int g_gpDebug;
DWORD g_gpRefreshMs;       // >0: jednou znovu vytvorit swapchain + povrch tolik ms po prvnim (FG zapnuta za behu)
ULONGLONG g_gpRefreshAt;   // 0 = ceka na prvni swapchain, ~0 = hotovo
DWORD g_gpRefreshMode = 4; // [fg] LiveRefreshMode: 1 = novy swapchain, 2 = okno hry WS_EX_LAYERED tam a zpet
                           // (oboji po LiveRefreshMs), 4 = WS_EX_LAYERED hned pri zapnuti (present.cpp)
int g_gpRefreshStage;
LONG g_gpScLost;  // swapchain presenteru vznikl pri g_atLostTop (alttab.inl); jen vlakno presenteru
volatile LONG g_gpRefreshKick;  // hlavni vlakno: obnovu swapchainu presenteru provest hned ([fg] OffWindow)
// Vlakno hry (ScHealComposed): swapchain presenteru i s povrchem znovu - zobrazeni jde pres kompozici DWM, ackoli
// okno hry je v popredi a zabira cely monitor (swapchain vznikl, kdyz okno v popredi nebylo; v102x)
volatile LONG g_gpRecreateReq;

// Ladeni bit 11 (2048): zaznam presentu do bms_dlss_gptrace.csv (typ snimku, casy CPU a GPU v QPC). Zaznamenava od
// spousti (soubor bms_dlss_gptrace.go ve slozce proxy, kontrola kazdych 5 s, soubor smaze) kGpTrMax presentu, pak
// soubor jednou zapise (hra se v testech zavira natvrdo).
struct GpTrRec {
    uint64_t seq;
    uint32_t flags, idx;
    LONGLONG post, wake, acq, sub, p0, p1, g0, g1;
};
const int kGpTrMax = 5000;
GpTrRec* g_gpTr;
int g_gpTrN, g_gpTrSlot[3] = {-1, -1, -1};
bool g_gpTrDone, g_gpTrOn;
uint32_t g_gpCurFlags;   // gpFlags zverejneneho obrazu (cte vlakno spolu s gpPub)
LONGLONG g_gpCurReal;    // gpRealQpc: postQpc skutecneho snimku, jehoz obsah obraz ukazuje (mereni latence)
int g_gpLcSlot[3] = {-1, -1, -1};  // zaznam latchain.inl k pouziti slotu prikazu (konec kopie na GPU)
LONGLONG g_gpCurWake;    // QPC probuzeni vlakna pro tento obraz
LONGLONG g_gpCurPost;    // gpPostQpc / gpQPost tohoto obrazu (pri casnem zverejneni = cilovy cas presentu)
HANDLE g_gpTimer;        // hi-res casovac vlakna presenteru (casna kopie: cekani na cilovy cas)
VkQueryPool g_gpQp;
double g_gpTsNs;         // ns na tik casove znacky GPU
LONGLONG g_gpCalQpc, g_gpCalAt;
uint64_t g_gpCalGpu;
PFN_vkGetCalibratedTimestampsEXT g_gpCal;
PFN_vkCreateQueryPool g_gpCreateQp;
PFN_vkCmdResetQueryPool g_gpCmdResetQp;
PFN_vkCmdWriteTimestamp g_gpCmdTs;
PFN_vkGetQueryPoolResults g_gpGetQp;

// ---- jen vlakno presenteru ----
const int kGpCmd = 3;
VkCommandPool g_gpPool;
VkCommandBuffer g_gpCmdB[kGpCmd];
VkFence g_gpFence[kGpCmd];
bool g_gpFenceUsed[kGpCmd];
VkSemaphore g_gpAcq[kGpCmd];
int g_gpCi;
VkSurfaceKHR g_gpSurf;
VkSwapchainKHR g_gpSc;
VkImage g_gpScImg[16];
VkSemaphore g_gpScDone[16];
uint32_t g_gpScCount;
VkExtent2D g_gpScExt;
LONG g_gpScCx, g_gpScCy;
uint32_t g_gpScInterval = 2;
VkPresentModeKHR g_gpScMode;
bool g_gpNeedRecreate, g_gpResyncDone, g_gpInUseLogged, g_gpFmtLogged;
ULONGLONG g_gpInUseRetry, g_gpFirstPresentTick, g_gpLastFrameTick;
struct GpWnd {
    LONG style, ex;
    RECT rc;
    BOOL vis;
} g_gpScWnd;
VkImage g_gpImgI[BMSD_GP_IMAGES];
VkDeviceMemory g_gpMemI[BMSD_GP_IMAGES];
uint32_t g_gpIw, g_gpIh;
VkSemaphore g_gpSemReady, g_gpSemDone;
int32_t g_gpImpGen;
uint64_t g_gpLastSeq;
Acc g_gpStPres, g_gpStWake;
unsigned g_gpStShown, g_gpStSkip, g_gpStNoWnd, g_gpStRecreate;
LONGLONG g_gpStatQpc, g_gpLastPresQpc;
Acc g_gpStIv;
double g_gpStIvSq;  // soucet ctvercu intervalu (sd)
// v103g: intervaly za interval statistiky (p1 / p99 a pocet mimo pasmo 0,75-1,33 x median: oko vidi jednotlive kratke /
// dlouhe intervaly, ktere prumer a sd za 5 s skryji - ukazatel Hz monitoru 60-160 pri 102 FPS)
static std::vector<float> g_gpIvBuf;
// v103h: podil intervalu mimo pasmo za ~1 s (rytmus obrazu pro vyber nasobku pri nedosazitelnem cili, FgAdaptLock)
static std::vector<float> g_gpQualBuf;
static LONGLONG g_gpQualQ;
static volatile LONG g_gpQualSeq;  // roste s kazdym oknem
static volatile float g_gpQualPct; // % intervalu mimo 0,75-1,33 x median
static volatile float g_gpQualMax; // nejdelsi interval (ms)
static void GpQualPush(float iv, LONGLONG now) {
    if (!g_gpQualQ) g_gpQualQ = now;
    if (g_gpQualBuf.size() < 4096) g_gpQualBuf.push_back(iv);
    if (now - g_gpQualQ < g_qpf.QuadPart || g_gpQualBuf.size() < 30) return;
    g_gpQualQ = now;
    std::vector<float>& b = g_gpQualBuf;
    std::nth_element(b.begin(), b.begin() + b.size() / 2, b.end());
    const float m = b[b.size() / 2];
    float mx = 0.0f;
    unsigned bad = 0;
    for (float v : b) {
        if (v < 0.75f * m || v > 1.33f * m) ++bad;
        if (v > mx) mx = v;
    }
    g_gpQualPct = 100.0f * (float)bad / (float)b.size();
    g_gpQualMax = mx;
    b.clear();
    MemoryBarrier();
    ++g_gpQualSeq;
}

void GpSetFailed(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_gpWhy, sizeof g_gpWhy, fmt, ap);
    va_end(ap);
    Log("prezentace: CHYBA zobrazeni v procesu hry: %s", g_gpWhy);
    if (g_sh) {
        strncpy(g_sh->gpMsg, g_gpWhy, sizeof g_sh->gpMsg - 1);
        g_sh->gpState = BMSD_PST_FAILED;
    }
    MemoryBarrier();
    InterlockedExchange(&g_gpFailed, 1);
}

void GpSetState(int32_t s) {
    if (!g_sh || g_sh->gpState == s) return;
    static const char* names[] = {"start", "zobrazuje", "okno minimalizovane / 0x0", "okno obsazene", "zastaveno"};
    Log("prezentace: zobrazeni v procesu hry - stav %s", s >= 0 && s <= 4 ? names[s] : "chyba");
    g_sh->gpState = s;
}

// Vulkan ovladac (ICD) primo, bez loaderu: instance pak nema zadne vrstvy - overlaye v procesu hry (Steam, RTSS, Epic)
// do ni nekresli. Overeno 2026-11-20: druhe zarizeni pres loader shazuje vrstva Steam overlaye (nvlddmkm 13 / pad
// v SteamOverlayVulkanLayer.dll), zarizeni DXVK pod zamkem fronty brzdi hru (present ~6 ms pod zamkem).
// Ovladac uz nahral loader pro DXVK: hleda se mezi nahranymi knihovnami podle exportu vk_icdGetInstanceProcAddr
// (NVIDIA nvoglv32.dll, AMD amdvlk32.dll, Intel igvk32.dll ...); spravne GPU se pak vybere podle UUID zarizeni DXVK.
// Vyjednani verze rozhrani se stejnym maximem jako loader (7) nic nemeni.
typedef VkResult(VKAPI_PTR* PFN_GpNegotiateIcd)(uint32_t* pVersion);
HMODULE g_gpIcdMods[8];
int g_gpIcdN;
void GpFindIcds() {
    HMODULE mods[512];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    const DWORD n = need / sizeof(HMODULE) < 512 ? need / sizeof(HMODULE) : 512;
    for (DWORD i = 0; i < n && g_gpIcdN < 8; ++i)
        if (GetProcAddress(mods[i], "vk_icdGetInstanceProcAddr")) g_gpIcdMods[g_gpIcdN++] = mods[i];
}
PFN_vkGetInstanceProcAddr GpIcdGipa(HMODULE icd) {
    auto neg = (PFN_GpNegotiateIcd)GetProcAddress(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
    auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(icd, "vk_icdGetInstanceProcAddr");
    char name[MAX_PATH] = "?";
    GetModuleFileNameA(icd, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    uint32_t ver = 7;
    if (!gipa || (neg && (neg(&ver) != VK_SUCCESS || ver < 3))) {
        Log("prezentace: zobrazeni v procesu hry - ovladac %s nejde pouzit (rozhrani %u)", base ? base + 1 : name, ver);
        return nullptr;
    }
    Log("prezentace: zobrazeni v procesu hry - primo ovladac %s (rozhrani ICD %u, bez vrstev)", base ? base + 1 : name, ver);
    return gipa;
}

// Vlastni VkDevice na GPU hry (instance a fyzicke zarizeni DXVK). Hlavni vlakno, jednou.
bool GpInitDevice() {
    HMODULE vkdll = GetModuleHandleW(L"vulkan-1.dll");
    auto gipa = vkdll ? (PFN_vkGetInstanceProcAddr)GetProcAddress(vkdll, "vkGetInstanceProcAddr") : nullptr;
    g_gpInst = g_inst;
    g_gpPd = g_pd;
    // Vychozi = vlastni zarizeni primo na ovladaci; ladeni: bit 8 (256) = vlastni zarizeni pres loader, bit 10 (1024) =
    // zarizeni a fronta DXVK pod LockSubmissionQueue
    const bool useIcd = !(g_gpDebug & (256 | 1024));
    g_gpOwnDev = !(g_gpDebug & 1024);
    if (!g_gpOwnDev) {  // ladeni: zarizeni a fronta DXVK
        if (!gipa || !g_dev || !g_queue || !g_idev)
            return Log("prezentace: zobrazeni v procesu hry - Vulkan DXVK nedostupny"), false;
#define LOADI(n)                                                                                       \
    gvk.n = (PFN_##n)gipa(g_gpInst, #n);                                                               \
    if (!gvk.n) return Log("prezentace: zobrazeni v procesu hry - chybi funkce %s", #n), false;
        GP_INST_FUNCS(LOADI)
#undef LOADI
        g_gpDev = g_dev;
        g_gpQ = g_queue;
        g_gpQf = g_qf;
#define LOADD(n)                                                                                       \
    gvk.n = (PFN_##n)gvk.vkGetDeviceProcAddr(g_gpDev, #n);                                             \
    if (!gvk.n) return Log("prezentace: zobrazeni v procesu hry - chybi funkce %s", #n), false;
        GP_DEV_FUNCS(LOADD)
#undef LOADD
    } else {
    if (useIcd) {
        GpFindIcds();
        if (!g_gpIcdN) return Log("prezentace: zobrazeni v procesu hry - zadny nahrany Vulkan ovladac (ICD)"), false;
    }
    for (int icdI = 0; icdI < (useIcd ? g_gpIcdN : 1) && g_gpInst == g_inst; ++icdI) {
    if (useIcd) {
        gipa = GpIcdGipa(g_gpIcdMods[icdI]);
        if (!gipa) continue;
    }
    if (gipa && ((g_gpDebug & 8) || useIcd)) {  // vlastni instance (GPU podle UUID zarizeni DXVK)
        auto ci = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
        const char* iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "bms_dlss_gp", 1, "bms-dlss", 1,
                                 VK_API_VERSION_1_2};
        VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 2, iext};
        VkInstance inst = VK_NULL_HANDLE;
        // bit 6: bez implicitnich vrstev (overlaye RTSS / Steam / Epic, Fossilize) - promenne jen po dobu vytvoreni
        static const char* kOff[][2] = {{"VK_LOADER_LAYERS_DISABLE", "~implicit~"}, {"DISABLE_RTSS_LAYER", "1"},
                                        {"DISABLE_VK_LAYER_VALVE_steam_overlay_1", "1"},
                                        {"DISABLE_VK_LAYER_VALVE_steam_fossilize_1", "1"},
                                        {"EOS_OVERLAY_DISABLE_VULKAN_WIN32", "1"}};
        const int nOff = (g_gpDebug & 64) ? 5 : 0;
        char old[5][64];
        bool had[5] = {};
        for (int i = 0; i < nOff; ++i) {
            had[i] = GetEnvironmentVariableA(kOff[i][0], old[i], 64) > 0;
            SetEnvironmentVariableA(kOff[i][0], kOff[i][1]);
        }
        const VkResult cr = ci ? ci(&ici, nullptr, &inst) : VK_ERROR_INITIALIZATION_FAILED;
        for (int i = 0; i < nOff; ++i) SetEnvironmentVariableA(kOff[i][0], had[i] ? old[i] : nullptr);
        if (nOff) Log("prezentace: zobrazeni v procesu hry - ladeni: instance bez implicitnich vrstev");
        if (cr == VK_SUCCESS) {
            auto en = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
            auto pr = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
            VkPhysicalDevice pds[16];
            uint32_t n = 16;
            if (en && pr && en(inst, &n, pds) >= 0)
                for (uint32_t i = 0; i < n; ++i) {
                    VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
                    pr(pds[i], &p2);
                    if (!memcmp(idp.deviceUUID, g_uuid, 16)) {
                        g_gpInst = inst;
                        g_gpPd = pds[i];
                    }
                }
        }
        if (g_gpInst == g_inst && inst) {  // tento ovladac GPU hry nema
            auto di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
            if (di) di(inst, nullptr);
        }
        Log("prezentace: zobrazeni v procesu hry - vlastni VkInstance %s", g_gpInst != g_inst ? "ano (GPU hry)" : "bez GPU hry");
    }
    }  // ovladace
    if (useIcd && g_gpInst == g_inst) return Log("prezentace: zobrazeni v procesu hry - GPU hry v ovladacich nenalezeno"), false;
    if (!gipa || !g_gpInst || !g_gpPd) return Log("prezentace: zobrazeni v procesu hry - Vulkan nedostupny"), false;
#define LOADI(n)                                                                                       \
    gvk.n = (PFN_##n)gipa(g_gpInst, #n);                                                                 \
    if (!gvk.n) return Log("prezentace: zobrazeni v procesu hry - chybi funkce %s", #n), false;
    GP_INST_FUNCS(LOADI)
#undef LOADI
    VkQueueFamilyProperties qfp[32];
    uint32_t nq = 32;
    gvk.vkGetPhysicalDeviceQueueFamilyProperties(g_gpPd, &nq, qfp);
    for (uint32_t i = 0; i < nq && g_gpQf == UINT32_MAX; ++i)
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) g_gpQf = i;
    if (g_gpQf == UINT32_MAX) return Log("prezentace: zobrazeni v procesu hry - zadna graficka fronta"), false;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    gvk.vkGetPhysicalDeviceFeatures2(g_gpPd, &f2);
    if (!f12.timelineSemaphore) return Log("prezentace: zobrazeni v procesu hry - GPU bez timeline semaforu"), false;
    VkPhysicalDeviceVulkan12Features en12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    en12.timelineSemaphore = VK_TRUE;
    const char* dext[5] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                           VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, nullptr, nullptr};
    uint32_t ndext = 3;
    const char* calName = nullptr;
    const char* prioExt = nullptr;
    static VkExtensionProperties ep[512];
    uint32_t ne = 0;
    if ((g_gpDebug & (2048 | 4096 | 8192)) || g_lcOn) {
        auto ede = (PFN_vkEnumerateDeviceExtensionProperties)gipa(g_gpInst, "vkEnumerateDeviceExtensionProperties");
        ne = 512;
        if (!ede || ede(g_gpPd, nullptr, &ne, ep) < 0) ne = 0;
    }
    // Bit 12 (4096) / 13 (8192), nebo [fg] GamePresentPriority 1 / 2 (v103f): globalni priorita fronty presentu
    // HIGH / REALTIME (kopie a present nemaji cekat za praci hry a helperu na GPU)
    VkDeviceQueueGlobalPriorityCreateInfoKHR gpri = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
    if (g_gpDebug & (4096 | 8192)) {
        for (uint32_t i = 0; i < ne && !prioExt; ++i)
            if (!strcmp(ep[i].extensionName, "VK_KHR_global_priority")) prioExt = "VK_KHR_global_priority";
        for (uint32_t i = 0; i < ne && !prioExt; ++i)
            if (!strcmp(ep[i].extensionName, "VK_EXT_global_priority")) prioExt = "VK_EXT_global_priority";
        if (prioExt) dext[ndext++] = prioExt;
        gpri.globalPriority = (g_gpDebug & 8192) ? VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR : VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
        Log("prezentace: zobrazeni v procesu hry - globalni priorita fronty %s (%s)",
            (g_gpDebug & 8192) ? "REALTIME" : "HIGH", prioExt ? prioExt : "rozsireni NENI");
    }
    if ((g_gpDebug & 2048) || g_lcOn) {  // zaznam presentu (ladeni) nebo mereni latence: casove znacky GPU na QPC
        auto gpp = (PFN_vkGetPhysicalDeviceProperties)gipa(g_gpInst, "vkGetPhysicalDeviceProperties");
        if (ne && gpp) {
            for (uint32_t i = 0; i < ne && !calName; ++i)
                if (!strcmp(ep[i].extensionName, "VK_EXT_calibrated_timestamps")) calName = "vkGetCalibratedTimestampsEXT";
            for (uint32_t i = 0; i < ne && !calName; ++i)
                if (!strcmp(ep[i].extensionName, "VK_KHR_calibrated_timestamps")) calName = "vkGetCalibratedTimestampsKHR";
            if (calName) {
                dext[ndext++] = calName[25] == 'E' ? "VK_EXT_calibrated_timestamps" : "VK_KHR_calibrated_timestamps";
                VkPhysicalDeviceProperties pp = {};
                gpp(g_gpPd, &pp);
                g_gpTsNs = pp.limits.timestampPeriod;
            }
        }
        Log("prezentace: zobrazeni v procesu hry - %s, casove znacky GPU %s (perioda %.3f ns)",
            (g_gpDebug & 2048) ? "ladeni: zaznam presentu" : "mereni latence ([debug] LatChain)",
            calName ? calName : "NEJSOU", g_gpTsNs);
    }
    float prio = 1.0f;
    // Druha fronta rodiny (kdyz je), at present nesouperi s praci DXVK na stejne fronte
    const uint32_t qi = qfp[g_gpQf].queueCount > 1 && !(g_gpDebug & 16) ? 1u : 0u;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, g_gpQf, qi + 1, nullptr};
    float prios[2] = {prio, prio};
    qci.pQueuePriorities = prios;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en12};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ndext;
    dci.ppEnabledExtensionNames = dext;
    if (prioExt) qci.pNext = &gpri;
    VkResult r = gvk.vkCreateDevice(g_gpPd, &dci, nullptr, &g_gpDev);
    if (prioExt && r != VK_SUCCESS) {  // priorita nepovolena (NOT_PERMITTED) - bez ni
        Log("prezentace: zobrazeni v procesu hry - ladeni: vkCreateDevice s prioritou %s - zkousim bez", VkStr(r));
        qci.pNext = nullptr;
        r = gvk.vkCreateDevice(g_gpPd, &dci, nullptr, &g_gpDev);
    } else if (prioExt) {
        Log("prezentace: zobrazeni v procesu hry - ladeni: fronta s globalni prioritou vytvorena");
    }
    if (r != VK_SUCCESS) return Log("prezentace: zobrazeni v procesu hry - vkCreateDevice %s", VkStr(r)), false;
#define LOADD(n)                                                                                       \
    gvk.n = (PFN_##n)gvk.vkGetDeviceProcAddr(g_gpDev, #n);                                             \
    if (!gvk.n) return Log("prezentace: zobrazeni v procesu hry - chybi funkce %s", #n), false;
    GP_DEV_FUNCS(LOADD)
#undef LOADD
    gvk.vkGetDeviceQueue(g_gpDev, g_gpQf, qi, &g_gpQ);
    if (calName) {
        g_gpCal = (PFN_vkGetCalibratedTimestampsEXT)gvk.vkGetDeviceProcAddr(g_gpDev, calName);
        g_gpCreateQp = (PFN_vkCreateQueryPool)gvk.vkGetDeviceProcAddr(g_gpDev, "vkCreateQueryPool");
        g_gpCmdResetQp = (PFN_vkCmdResetQueryPool)gvk.vkGetDeviceProcAddr(g_gpDev, "vkCmdResetQueryPool");
        g_gpCmdTs = (PFN_vkCmdWriteTimestamp)gvk.vkGetDeviceProcAddr(g_gpDev, "vkCmdWriteTimestamp");
        g_gpGetQp = (PFN_vkGetQueryPoolResults)gvk.vkGetDeviceProcAddr(g_gpDev, "vkGetQueryPoolResults");
        VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP,
                                     (uint32_t)kGpCmd * 2};
        if (!g_gpCal || !g_gpCreateQp || !g_gpCmdResetQp || !g_gpCmdTs || !g_gpGetQp ||
            g_gpCreateQp(g_gpDev, &qpi, nullptr, &g_gpQp) != VK_SUCCESS || qfp[g_gpQf].timestampValidBits < 64)
            g_gpQp = VK_NULL_HANDLE, Log("prezentace: ladeni: casove znacky GPU nejdou (bity %u)",
                                         qfp[g_gpQf].timestampValidBits);
    }
    if (g_gpDebug & 2048) g_gpTr = (GpTrRec*)calloc(kGpTrMax, sizeof(GpTrRec));
    }  // vlastni zarizeni
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_gpQf};
    if (gvk.vkCreateCommandPool(g_gpDev, &pci, nullptr, &g_gpPool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_gpPool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, (uint32_t)kGpCmd};
    if (gvk.vkAllocateCommandBuffers(g_gpDev, &cai, g_gpCmdB) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo bsi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < kGpCmd; ++i)
        if (gvk.vkCreateFence(g_gpDev, &fci, nullptr, &g_gpFence[i]) != VK_SUCCESS ||
            gvk.vkCreateSemaphore(g_gpDev, &bsi, nullptr, &g_gpAcq[i]) != VK_SUCCESS)
            return false;
    Log("prezentace: zobrazeni v procesu hry - %s VkDevice %p (fronta family %u)", g_gpOwnDev ? "vlastni" : "DXVK",
        (void*)g_gpDev, g_gpQf);
    return true;
}

// Fronta DXVK: odeslani a present jen pod zamkem (vlakno odesilani DXVK ji pouziva taky)
struct GpQLock {
    GpQLock() {
        if (!g_gpOwnDev) g_idev->LockSubmissionQueue();
    }
    ~GpQLock() {
        if (!g_gpOwnDev) g_idev->ReleaseSubmissionQueue();
    }
};

// Cekani na vsechny rozpracovane kopie s jednim spolecnym limitem ns (ne ns na kazdou fence)
bool GpWaitCmds(uint64_t ns) {
    bool ok = true;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    const LONGLONG end = Qpc() + (LONGLONG)((double)ns * 1e-9 * (double)g_qpf.QuadPart);
    for (int i = 0; i < kGpCmd; ++i)
        if (g_gpFenceUsed[i]) {
            const LONGLONG left = end - Qpc();
            const uint64_t lns = left > 0 ? (uint64_t)((double)left * 1e9 / (double)g_qpf.QuadPart) : 0;
            if (gvk.vkWaitForFences(g_gpDev, 1, &g_gpFence[i], VK_TRUE, lns) != VK_SUCCESS) {
                ok = false;
                continue;
            }
            gvk.vkResetFences(g_gpDev, 1, &g_gpFence[i]);
            g_gpFenceUsed[i] = false;
        }
    return ok;
}

void GpDestroySwapchain(const char* why, uint64_t waitNs = 1000000000ull) {
    if (!g_gpSc) return;
    if (waitNs) GpWaitCmds(waitNs);
    {
        GpQLock lk;  // ovladac pri ruseni swapchainu saha na frontu (bez zamku pad v nvoglv32 proti vlaknu DXVK)
        gvk.vkDestroySwapchainKHR(g_gpDev, g_gpSc, nullptr);
    }
    g_gpSc = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < g_gpScCount; ++i)
        if (g_gpScDone[i]) gvk.vkDestroySemaphore(g_gpDev, g_gpScDone[i], nullptr);
    memset(g_gpScDone, 0, sizeof g_gpScDone);
    g_gpScCount = 0;
    Log("prezentace: zobrazeni v procesu hry - swapchain uvolnen (%s)", why);
}

void GpDestroyImports() {
    for (int i = 0; i < BMSD_GP_IMAGES; ++i) {
        if (g_gpImgI[i]) gvk.vkDestroyImage(g_gpDev, g_gpImgI[i], nullptr);
        if (g_gpMemI[i]) gvk.vkFreeMemory(g_gpDev, g_gpMemI[i], nullptr);
        g_gpImgI[i] = VK_NULL_HANDLE;
        g_gpMemI[i] = VK_NULL_HANDLE;
    }
    if (g_gpSemReady) gvk.vkDestroySemaphore(g_gpDev, g_gpSemReady, nullptr);
    if (g_gpSemDone) gvk.vkDestroySemaphore(g_gpDev, g_gpSemDone, nullptr);
    g_gpSemReady = g_gpSemDone = VK_NULL_HANDLE;
    g_gpImpGen = 0;
}

// Import generace gen (obrazy + semafory helperu). Handle jsou v tomto procesu; po importu se zavrou.
// false = nejde (handle zatim nejsou / generace se mezitim zmenila) - neni to chyba.
bool GpImport(int32_t gen) {
    BmsdImage im[BMSD_GP_IMAGES];
    memcpy(im, g_sh->gpImg, sizeof im);
    const uint64_t hR = g_sh->gpSemReadyHandle, hD = g_sh->gpSemDoneHandle;
    MemoryBarrier();
    if (g_sh->gpGeneration != gen || !hR || !hD) return false;
    for (const BmsdImage& b : im)
        if (!b.handle) return false;
    g_sh->gpProxyGeneration = gen;  // handle teto generace si bere proxy (helper je pak nezavira)
    MemoryBarrier();
    GpWaitCmds(1000000000ull);
    GpDestroyImports();
    bool ok = true;
    auto impSem = [&](uint64_t h, VkSemaphore* out) {
        VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE,
                                         0};
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
        if (gvk.vkCreateSemaphore(g_gpDev, &sci, nullptr, out) != VK_SUCCESS) return false;
        VkImportSemaphoreWin32HandleInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        ii.semaphore = *out;
        ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        ii.handle = (HANDLE)(uintptr_t)h;
        return gvk.vkImportSemaphoreWin32HandleKHR(g_gpDev, &ii) == VK_SUCCESS;
    };
    ok = impSem(hR, &g_gpSemReady) && impSem(hD, &g_gpSemDone);
    VkPhysicalDeviceMemoryProperties mp = {};
    gvk.vkGetPhysicalDeviceMemoryProperties(g_gpPd, &mp);
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    for (int i = 0; i < BMSD_GP_IMAGES && ok; ++i) {
        const BmsdImage& b = im[i];
        VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                               (VkExternalMemoryHandleTypeFlags)ht};
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = (VkFormat)b.format;
        ci.extent = {b.width, b.height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = b.usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (gvk.vkCreateImage(g_gpDev, &ci, nullptr, &g_gpImgI[i]) != VK_SUCCESS) {
            ok = false;
            break;
        }
        VkMemoryRequirements req = {};
        gvk.vkGetImageMemoryRequirements(g_gpDev, g_gpImgI[i], &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; ++t)
            if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = t;
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, g_gpImgI[i]};
        VkImportMemoryWin32HandleInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded, ht,
                                                (HANDLE)(uintptr_t)b.handle};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, b.allocSize, type};
        if (type == UINT32_MAX || gvk.vkAllocateMemory(g_gpDev, &ai, nullptr, &g_gpMemI[i]) != VK_SUCCESS ||
            gvk.vkBindImageMemory(g_gpDev, g_gpImgI[i], g_gpMemI[i], 0) != VK_SUCCESS)
            ok = false;
    }
    for (const BmsdImage& b : im) CloseHandle((HANDLE)(uintptr_t)b.handle);
    CloseHandle((HANDLE)(uintptr_t)hR);
    CloseHandle((HANDLE)(uintptr_t)hD);
    if (!ok) {
        GpDestroyImports();
        GpSetFailed("import obrazu helperu (generace %d) selhal", gen);
        return false;
    }
    g_gpImpGen = gen;
    g_gpIw = im[0].width;
    g_gpIh = im[0].height;
    Log("prezentace: zobrazeni v procesu hry - obrazy helperu importovany %ux%u (generace %d)", g_gpIw, g_gpIh, gen);
    return true;
}

GpWnd GpGetWnd() {
    GpWnd s = {};
    s.style = GetWindowLongW(g_gpWnd, GWL_STYLE);
    s.ex = GetWindowLongW(g_gpWnd, GWL_EXSTYLE);
    GetWindowRect(g_gpWnd, &s.rc);
    s.vis = IsWindowVisible(g_gpWnd);
    return s;
}

VkPresentModeKHR GpPickMode(uint32_t interval) {
    VkPresentModeKHR modes[8];
    uint32_t n = 8;
    gvk.vkGetPhysicalDeviceSurfacePresentModesKHR(g_gpPd, g_gpSurf, &n, modes);
    auto has = [&](VkPresentModeKHR m) {
        for (uint32_t i = 0; i < n; ++i)
            if (modes[i] == m) return true;
        return false;
    };
    if (interval == 0 && !(g_gpDebug & 32)) {  // hra bez VSync: jako DXVK (IMMEDIATE, jinak MAILBOX)
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

const char* GpModeStr(VkPresentModeKHR m) {
    return m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
                                                         : m == VK_PRESENT_MODE_FIFO_KHR      ? "FIFO"
                                                                                              : "?";
}

// Swapchain na prezentacnim okne podle jeho velikosti (jako drive v helperu, vcetne pojistek). false = ted nezobrazovat.
bool GpEnsureSwapchain(ULONGLONG frameGap) {
    if (!IsWindow(g_gpWnd)) {
        GpSetState(BMSD_PST_NOWINDOW);
        return false;
    }
    if (!g_gpSurf) {
        VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = g_gpWnd;
        VkResult r = gvk.vkCreateWin32SurfaceKHR(g_gpInst, &sci, nullptr, &g_gpSurf);
        if (r != VK_SUCCESS) return GpSetFailed("vkCreateWin32SurfaceKHR %s", VkStr(r)), false;
        VkBool32 sup = VK_FALSE;
        gvk.vkGetPhysicalDeviceSurfaceSupportKHR(g_gpPd, g_gpQf, g_gpSurf, &sup);
        if (!sup) return GpSetFailed("fronta %u neumi prezentovat do okna", g_gpQf), false;
    }
    RECT rc = {};
    GetClientRect(g_gpWnd, &rc);
    const bool zero = IsIconic(g_gpWnd) || IsIconic(GetAncestor(g_gpWnd, GA_ROOT)) || rc.right <= 0 || rc.bottom <= 0;
    if (zero) {
        GpDestroySwapchain("okno minimalizovane / 0x0");
        GpSetState(BMSD_PST_NOWINDOW);
        return false;
    }
    const uint32_t interval = g_sh->presentInterval;
    if (g_gpSc && (g_gpNeedRecreate || rc.right != g_gpScCx || rc.bottom != g_gpScCy || interval != g_gpScInterval)) {
        GpDestroySwapchain(g_gpNeedRecreate ? "OUT_OF_DATE / SUBOPTIMAL" : "zmena velikosti okna nebo VSync");
    }
    if (g_gpSc) {
        const GpWnd ws = GpGetWnd();
        if (ws.style != g_gpScWnd.style || ws.ex != g_gpScWnd.ex || ws.vis != g_gpScWnd.vis ||
            memcmp(&ws.rc, &g_gpScWnd.rc, sizeof ws.rc)) {
            GpDestroySwapchain("zmena okna");
        }
        // (pojistky helperu "znovu po pauze" a "znovu 1,5 s po startu" byly kvuli presentu z ciziho procesu - tady ne)
        (void)frameGap;
        if (g_gpSc && InterlockedExchange(&g_gpRecreateReq, 0)) {
            GpDestroySwapchain("obnova: zobrazeni pres kompozici DWM misto flipu");
            gvk.vkDestroySurfaceKHR(g_gpInst, g_gpSurf, nullptr);
            g_gpSurf = VK_NULL_HANDLE;
            return false;
        }
        if (g_gpSc && g_gpScLost != g_atLostTop) {  // hra mimo popredi bez minimalizace / navrat (alttab.inl)
            GpDestroySwapchain(g_atLostTop ? "hra neni v popredi a neminimalizovala se" : "hra zase v popredi");
            gvk.vkDestroySurfaceKHR(g_gpInst, g_gpSurf, nullptr);
            g_gpSurf = VK_NULL_HANDLE;
            return false;
        }
        // FG zapnuta za behu: posledni snimek DXVK v okne hry muze zustat v Independent Flip (displej ho dal
        // skenuje), swapchain presenteru jde pres "Composed: Copy with GPU GDI" a neni videt (pomuze Alt+Tab).
        // Jednou: DWM vyhodit z iFlip okna hry (WS_EX_LAYERED tam a zpet), za 150 ms novy swapchain i s povrchem.
        if (g_gpSc && g_gpRefreshAt && g_gpRefreshAt != ~0ull && InterlockedExchange(&g_gpRefreshKick, 0))
            g_gpRefreshAt = GetTickCount64();
        if (g_gpSc && g_gpRefreshAt && g_gpRefreshAt != ~0ull && GetTickCount64() >= g_gpRefreshAt) {
            if (g_gpRefreshStage == 0 && (g_gpRefreshMode & 2) && g_ctl && IsWindow(g_gameWnd)) {
                PostMessageW(g_ctl, WMP_UNSTICK, (WPARAM)g_gameWnd, 0);
                g_gpRefreshStage = 1;
                g_gpRefreshAt = GetTickCount64() + 150;
            } else {
                g_gpRefreshAt = ~0ull;
                if (g_gpRefreshMode & 1) {
                    GpDestroySwapchain("obnova po zapnuti FG za behu");
                    gvk.vkDestroySurfaceKHR(g_gpInst, g_gpSurf, nullptr);
                    g_gpSurf = VK_NULL_HANDLE;
                    return false;
                }
            }
        }
    }
    g_gpNeedRecreate = false;
    if (g_gpSc) return true;
    g_gpScLost = g_atLostTop;
    const ULONGLONG now = GetTickCount64();
    if (now < g_gpInUseRetry) return false;
    VkSurfaceCapabilitiesKHR caps = {};
    VkResult r = gvk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_gpPd, g_gpSurf, &caps);
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        gvk.vkDestroySurfaceKHR(g_gpInst, g_gpSurf, nullptr);
        g_gpSurf = VK_NULL_HANDLE;
        return false;
    }
    if (r != VK_SUCCESS) return GpSetFailed("surface caps %s", VkStr(r)), false;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) ext = {(uint32_t)rc.right, (uint32_t)rc.bottom};
    if (!ext.width || !ext.height) {
        GpSetState(BMSD_PST_NOWINDOW);
        return false;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return GpSetFailed("swapchain bez TRANSFER_DST"), false;
    VkSurfaceFormatKHR fmts[32];
    uint32_t nf = 32;
    gvk.vkGetPhysicalDeviceSurfaceFormatsKHR(g_gpPd, g_gpSurf, &nf, fmts);
    // Hodnoty jsou uz v sRGB: format UNORM v SRGB_NONLINEAR (blit bez prevodu); nikdy HDR prostor, _SRGB az nakonec
    static const VkFormat kPref[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32,
                                     VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    VkSurfaceFormatKHR sf = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (VkFormat want : kPref)
        for (uint32_t i = 0; i < nf && sf.format == VK_FORMAT_UNDEFINED; ++i)
            if (fmts[i].format == want && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) sf = fmts[i];
    for (uint32_t i = 0; i < nf && sf.format == VK_FORMAT_UNDEFINED; ++i)
        if (fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) sf = fmts[i];
    if (nf == 0 && sf.format == VK_FORMAT_UNDEFINED) sf.format = VK_FORMAT_B8G8R8A8_UNORM;
    if (sf.format == VK_FORMAT_UNDEFINED) return GpSetFailed("swapchain nenabizi SDR format"), false;
    if (sf.format != VK_FORMAT_B8G8R8A8_UNORM && !g_gpFmtLogged) {
        g_gpFmtLogged = true;
        Log("prezentace: zobrazeni v procesu hry - swapchain bez B8G8R8A8_UNORM, VkFormat %d", (int)sf.format);
    }
    const VkPresentModeKHR mode = GpPickMode(interval);
    uint32_t minImg = caps.minImageCount + 1 < 3 ? 3 : caps.minImageCount + 1;
    if (caps.maxImageCount && minImg > caps.maxImageCount) minImg = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_gpSurf;
    ci.minImageCount = minImg;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    // COLOR_ATTACHMENT navic: overlaye v procesu hry (RTSS OSD, Steam overlay) haci vkQueuePresentKHR a do obrazu
    // swapchainu kresli jako do render targetu - bez tohoto priznaku chyba GPU (DEVICE_LOST)
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    (caps.supportedUsageFlags & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    {
        GpQLock lk;
        r = gvk.vkCreateSwapchainKHR(g_gpDev, &ci, nullptr, &sc);
    }
    if (r != VK_SUCCESS) {
        // Chyba ovladace: pri chybe muze byt sc nenulovy - nesmi se znicit, jen zahodit
        if (sc) Log("prezentace: zobrazeni v procesu hry - vkCreateSwapchainKHR %s vratil nenulovy handle - zahazuji",
                    VkStr(r));
        if (r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
            if (!g_gpInUseLogged) {
                g_gpInUseLogged = true;
                Log("prezentace: zobrazeni v procesu hry - okno je pro Vulkan obsazene, zkousim kazdych 250 ms");
            }
            g_gpInUseRetry = now + 250;
            GpSetState(BMSD_PST_INUSE);
            return false;
        }
        return GpSetFailed("vkCreateSwapchainKHR %s", VkStr(r)), false;
    }
    g_gpInUseLogged = false;
    g_gpSc = sc;
    g_gpScWnd = GpGetWnd();
    g_gpScCount = 16;
    gvk.vkGetSwapchainImagesKHR(g_gpDev, g_gpSc, &g_gpScCount, g_gpScImg);
    VkSemaphoreCreateInfo bsi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < g_gpScCount; ++i)
        if (gvk.vkCreateSemaphore(g_gpDev, &bsi, nullptr, &g_gpScDone[i]) != VK_SUCCESS)
            return GpSetFailed("semafor swapchainu"), false;
    g_gpScExt = ext;
    g_gpScCx = rc.right;
    g_gpScCy = rc.bottom;
    g_gpScMode = mode;
    g_gpScInterval = interval;
    ++g_gpStRecreate;
    g_sh->gpPresentMode = (uint32_t)mode;
    g_sh->gpSwapW = ext.width;
    g_sh->gpSwapH = ext.height;
    ++g_sh->gpRecreate;
    Log("prezentace: zobrazeni v procesu hry - swapchain %ux%u, %s, VkFormat %d, %u obrazku, okno %p", ext.width,
        ext.height, GpModeStr(mode), (int)sf.format, g_gpScCount, (void*)g_gpWnd);
    if (g_gpRefreshMs && !g_gpRefreshAt) g_gpRefreshAt = GetTickCount64() + g_gpRefreshMs;
    return true;
}

VkImageMemoryBarrier GpIB(VkImage img, VkAccessFlags src, VkAccessFlags dst, VkImageLayout oldL, VkImageLayout newL,
                          uint32_t srcQ = VK_QUEUE_FAMILY_IGNORED, uint32_t dstQ = VK_QUEUE_FAMILY_IGNORED) {
    return Barrier(img, src, dst, oldL, newL, srcQ, dstQ);
}

// Spanek vlakna presenteru do casu target (QPC): vlastni hi-res casovac, posledni ~0.3 ms aktivne
void GpSleepUntil(LONGLONG target) {
    if (!g_gpTimer)
        g_gpTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                           TIMER_ALL_ACCESS);
    const LONGLONG spin = g_qpf.QuadPart * 3 / 10000;
    for (;;) {
        const LONGLONG left = target - Qpc();
        if (left <= 0) return;
        if (left > spin) {
            LARGE_INTEGER due;
            due.QuadPart = -((left - spin) * 10000000 / g_qpf.QuadPart);
            if (g_gpTimer && due.QuadPart < 0 && SetWaitableTimer(g_gpTimer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(g_gpTimer, 100);
                continue;
            }
            if (left > g_qpf.QuadPart * 2 / 1000) {
                Sleep(1);
                continue;
            }
        }
        YieldProcessor();
    }
}

// ---- ladeni bit 11: zaznam presentu ----
LONGLONG GpTrToQpc(uint64_t g) {
    if (!g) return 0;
    const double ns = (double)(int64_t)(g - g_gpCalGpu) * g_gpTsNs;
    return g_gpCalQpc + (LONGLONG)(ns * (double)g_qpf.QuadPart / 1e9);
}
void GpTrCalib() {
    if (!g_gpCal) return;
    const LONGLONG now = Qpc();
    if (g_gpCalAt && Ms(g_gpCalAt, now) < 500.0) return;
    VkCalibratedTimestampInfoEXT ci[2] = {
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT}};
    uint64_t ts[2] = {}, dev = 0;
    if (g_gpCal(g_gpDev, 2, ci, ts, &dev) == VK_SUCCESS) {
        g_gpCalGpu = ts[0];
        g_gpCalQpc = (LONGLONG)ts[1];
        g_gpCalAt = now;
    }
}
void GpTrWrite(const char* why) {
    if (!g_gpTr || g_gpTrDone || !g_gpTrN) return;
    g_gpTrDone = true;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\bms_dlss_gptrace.csv", ProxyDir());
    FILE* f = _wfopen(path, L"w");
    if (!f) return Log("prezentace: ladeni: zaznam presentu nejde zapsat");
    fprintf(f, "# qpf=%lld mode=%s\nseq,flags,idx,post,wake,acq,sub,p0,p1,g0,g1\n", (long long)g_qpf.QuadPart,
            GpModeStr(g_gpScMode));
    for (int i = 0; i < g_gpTrN; ++i) {
        const GpTrRec& r = g_gpTr[i];
        fprintf(f, "%llu,%u,%u,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", (unsigned long long)r.seq, r.flags, r.idx,
                r.post, r.wake, r.acq, r.sub, r.p0, r.p1, r.g0, r.g1);
    }
    fclose(f);
    Log("prezentace: ladeni: zaznam %d presentu zapsan do bms_dlss_gptrace.csv (%s)", g_gpTrN, why);
}

// Jeden obraz seq: kopie do swapchainu a present (nebo jen potvrzeni gpSemDone = seq)
void GpProcess(uint64_t seq) {
    const int k = (int)(seq % BMSD_GP_IMAGES);
    const int c = g_gpCi;
    g_gpCi = (g_gpCi + 1) % kGpCmd;
    if (g_gpFenceUsed[c]) {
        // Pomale PC (strankovani VRAM pri nacitani): GPU smi stat i sekundy - cekat po krocich s logem do 20 s.
        // Skutecne zaseknuti konci pres TDR ztratou zarizeni (fence pak hned vrati chybu).
        VkResult wr = gvk.vkWaitForFences(g_gpDev, 1, &g_gpFence[c], VK_TRUE, 2000000000ull);
        if (wr == VK_TIMEOUT) {
            Log("prezentace: WARN - GPU zatim nedobehla kopii do swapchainu (2 s), cekam dal (nejvys 20 s)");
            for (int i = 0; i < 9 && wr == VK_TIMEOUT; ++i)
                wr = gvk.vkWaitForFences(g_gpDev, 1, &g_gpFence[c], VK_TRUE, 2000000000ull);
            Log("prezentace: kopie do swapchainu %s (VkResult %d)", wr == VK_SUCCESS ? "dobehla" : "NEDOBEHLA", (int)wr);
        }
        if (wr != VK_SUCCESS)
            return GpSetFailed("GPU nedobehla (kopie do swapchainu)");
        gvk.vkResetFences(g_gpDev, 1, &g_gpFence[c]);
        g_gpFenceUsed[c] = false;
    }
    if (g_gpTrSlot[c] >= 0 || g_gpLcSlot[c] >= 0) {  // casove znacky predchoziho pouziti slotu (ladeni, latence)
        uint64_t ts[2] = {};
        GpTrCalib();
        if (g_gpGetQp(g_gpDev, g_gpQp, (uint32_t)c * 2, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            if (g_gpTrSlot[c] >= 0) {
                g_gpTr[g_gpTrSlot[c]].g0 = GpTrToQpc(ts[0]);
                g_gpTr[g_gpTrSlot[c]].g1 = GpTrToQpc(ts[1]);
            }
            LcDispSet(g_gpLcSlot[c], 0, 0, GpTrToQpc(ts[1]));
        }
        g_gpTrSlot[c] = -1;
        g_gpLcSlot[c] = -1;
    }
    GpTrRec* tr = nullptr;
    if (g_gpTr && !g_gpTrDone && g_gpTrOn) {
        if (g_gpTrN >= kGpTrMax) {
            GpTrWrite("plny");
        } else {
            tr = &g_gpTr[g_gpTrN++];
            tr->seq = seq;
            tr->flags = g_gpCurFlags;
            tr->post = g_gpCurPost;
            tr->wake = g_gpCurWake;
        }
    }
    const ULONGLONG nowTick = GetTickCount64();
    const ULONGLONG gap = g_gpLastFrameTick && nowTick - g_gpLastFrameTick > 1000 ? nowTick - g_gpLastFrameTick : 0;
    g_gpLastFrameTick = nowTick;
    bool acquired = false;
    uint32_t idx = 0;
    if (GpEnsureSwapchain(gap)) {
        VkResult r;
        {
            GpQLock lk;
            r = gvk.vkAcquireNextImageKHR(g_gpDev, g_gpSc, 0, g_gpAcq[c], VK_NULL_HANDLE, &idx);
        }
        for (int tries = 0; r == VK_NOT_READY || r == VK_TIMEOUT; ++tries) {  // bez cekani pod zamkem fronty DXVK
            if (tries >= (g_fgFree ? 400 : 100)) break;
            if (g_fgFree) GpSleepUntil(Qpc() + g_qpf.QuadPart / 4000);  // rucni cil nad stropem VRR: kratsi krok (0,25 ms)
            else Sleep(1);
            GpQLock lk;
            r = gvk.vkAcquireNextImageKHR(g_gpDev, g_gpSc, 0, g_gpAcq[c], VK_NULL_HANDLE, &idx);
        }
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            acquired = true;
            if (r == VK_SUBOPTIMAL_KHR) g_gpNeedRecreate = true;
        } else if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            g_gpNeedRecreate = true;
        } else if (r == VK_ERROR_SURFACE_LOST_KHR) {
            GpDestroySwapchain("SURFACE_LOST");
            gvk.vkDestroySurfaceKHR(g_gpInst, g_gpSurf, nullptr);
            g_gpSurf = VK_NULL_HANDLE;
        } else if (r != VK_TIMEOUT && r != VK_NOT_READY) {
            return GpSetFailed("vkAcquireNextImageKHR %s", VkStr(r));
        }
    }
    if (g_gpFailed) return;
    if (!acquired) ++g_gpStNoWnd;
    if (tr) {
        tr->acq = Qpc();
        tr->idx = acquired ? idx : 99u;
    }
    const bool trTs = (tr || g_lcOn) && g_gpQp;
    VkCommandBuffer cmd = g_gpCmdB[c];
    const bool noImp = (g_gpDebug & 128) != 0;
    const bool dbgClear = (g_gpDebug & 1) != 0 || noImp, dbgCpuWait = (g_gpDebug & 2) != 0 || noImp,
               dbgCpuSig = (g_gpDebug & 4) != 0 || noImp;
    if (dbgCpuWait && !noImp) {
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &g_gpSemReady, &seq};
        const VkResult wr = gvk.vkWaitSemaphores(g_gpDev, &wi, 200000000ull);
        if (wr != VK_SUCCESS) return GpSetFailed("ladeni: CPU cekani na gpSemReady %llu: %s", (unsigned long long)seq,
                                                 VkStr(wr));
    }
    if (acquired && dbgClear) {
        GpSetState(BMSD_PST_PRESENTING);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                       VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        gvk.vkResetCommandBuffer(cmd, 0);
        gvk.vkBeginCommandBuffer(cmd, &bi);
        VkImageMemoryBarrier pre = GpIB(g_gpScImg[idx], 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        gvk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &pre);
        VkClearColorValue cc = {};
        cc.float32[0] = (float)(seq % 64) / 64.0f;
        cc.float32[1] = 0.2f;
        cc.float32[3] = 1.0f;
        VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        gvk.vkCmdClearColorImage(cmd, g_gpScImg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
        VkImageMemoryBarrier post = GpIB(g_gpScImg[idx], VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        gvk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &post);
        gvk.vkEndCommandBuffer(cmd);
    } else if (acquired) {
        GpSetState(BMSD_PST_PRESENTING);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                       VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        gvk.vkResetCommandBuffer(cmd, 0);
        gvk.vkBeginCommandBuffer(cmd, &bi);
        if (trTs) g_gpCmdResetQp(cmd, g_gpQp, (uint32_t)c * 2, 2);
        const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL, DST = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        const uint32_t EXT = VK_QUEUE_FAMILY_EXTERNAL;
        VkImageMemoryBarrier pre[2] = {
            GpIB(g_gpImgI[k], 0, VK_ACCESS_TRANSFER_READ_BIT, GEN, GEN, EXT, g_gpQf),  // prevzeti od helperu
            GpIB(g_gpScImg[idx], 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, DST)};
        gvk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 2, pre);
        if (trTs) g_gpCmdTs(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, g_gpQp, (uint32_t)c * 2);
        VkImageBlit b = {};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {(int32_t)g_gpIw, (int32_t)g_gpIh, 1};
        b.dstOffsets[1] = {(int32_t)g_gpScExt.width, (int32_t)g_gpScExt.height, 1};
        const bool same = g_gpIw == g_gpScExt.width && g_gpIh == g_gpScExt.height;
        gvk.vkCmdBlitImage(cmd, g_gpImgI[k], GEN, g_gpScImg[idx], DST, 1, &b, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        VkImageMemoryBarrier post[2] = {
            GpIB(g_gpImgI[k], VK_ACCESS_TRANSFER_READ_BIT, 0, GEN, GEN, g_gpQf, EXT),  // zpet helperu
            GpIB(g_gpScImg[idx], VK_ACCESS_TRANSFER_WRITE_BIT, 0, DST, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)};
        gvk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                                 0, nullptr, 2, post);
        if (trTs) g_gpCmdTs(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_gpQp, (uint32_t)c * 2 + 1);
        gvk.vkEndCommandBuffer(cmd);
    }
    // Odeslat vzdy: ceka na gpSemReady = seq a signalizuje gpSemDone = seq (helper pak obraz znovu pouzije)
    VkSemaphore waitS[2];
    uint64_t waitV[2];
    VkPipelineStageFlags waitStage[2];
    uint32_t nw = 0, ns = 0;
    if (!dbgCpuWait) {
        waitS[nw] = g_gpSemReady;
        waitV[nw] = seq;
        waitStage[nw++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (acquired) {
        waitS[nw] = g_gpAcq[c];
        waitV[nw] = 0;
        waitStage[nw++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    VkSemaphore sigS[2];
    uint64_t sigV[2];
    if (!dbgCpuSig) {
        sigS[ns] = g_gpSemDone;
        sigV[ns++] = seq;
    }
    if (acquired) {
        sigS[ns] = g_gpScDone[idx];
        sigV[ns++] = 0;
    }
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = nw;
    ti.pWaitSemaphoreValues = waitV;
    ti.signalSemaphoreValueCount = ns;
    ti.pSignalSemaphoreValues = sigV;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ti};
    si.waitSemaphoreCount = nw;
    si.pWaitSemaphores = waitS;
    si.pWaitDstStageMask = waitStage;
    si.commandBufferCount = acquired ? 1 : 0;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = ns;
    si.pSignalSemaphores = sigS;
    VkResult r;
    if (tr) tr->sub = Qpc();
    {
        GpQLock lk;
        r = gvk.vkQueueSubmit(g_gpQ, 1, &si, g_gpFence[c]);
    }
    if (r != VK_SUCCESS) return GpSetFailed("vkQueueSubmit %s", VkStr(r));
    g_gpFenceUsed[c] = true;
    if (tr && trTs && acquired && !dbgClear) g_gpTrSlot[c] = (int)(tr - g_gpTr);
    const int lci = acquired ? LcDispBegin(g_gpCurReal, (g_gpCurFlags & 1u) != 0) : -1;
    if (trTs && acquired && !dbgClear) g_gpLcSlot[c] = lci;
    if (dbgCpuSig && !(g_gpDebug & 128)) {  // ladeni: gpSemDone z CPU az po dokonceni prace (fence)
        if (gvk.vkWaitForFences(g_gpDev, 1, &g_gpFence[c], VK_TRUE, 1000000000ull) != VK_SUCCESS)
            return GpSetFailed("ladeni: fence kopie nedobehla");
        VkSemaphoreSignalInfo sgi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, g_gpSemDone, seq};
        const VkResult sr = gvk.vkSignalSemaphore(g_gpDev, &sgi);
        if (sr != VK_SUCCESS) return GpSetFailed("ladeni: CPU signal gpSemDone %s", VkStr(sr));
    }
    g_sh->gpDoneSeq = seq;
    if (!acquired) return;
    // Casne zverejneni (ladeni bit 14, helper gpEnable bit 1): kopie uz bezi, present az v cilovy cas helperu
    if ((g_gpCurFlags & 2u) && g_gpCurPost) {
        const LONGLONG now = Qpc();
        if (g_gpCurPost > now && Ms(now, g_gpCurPost) < 50.0) GpSleepUntil(g_gpCurPost);
    }
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_gpScDone[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_gpSc;
    pi.pImageIndices = &idx;
    LONGLONG p0, p1;
    {
        GpQLock lk;
        p0 = Qpc();
        r = gvk.vkQueuePresentKHR(g_gpQ, &pi);
        p1 = Qpc();
    }
    if (tr) {
        tr->p0 = p0;
        tr->p1 = p1;
    }
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) LcDispSet(lci, p0, p1, 0);
    g_gpStPres.add(Ms(p0, p1));
    if (g_dgOn && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) DgPush(g_dgMon, p0);  // v103g: diagnosticky prekryv
    if (g_gpLastPresQpc && Ms(g_gpLastPresQpc, p0) < 200.0) {
        const double iv = Ms(g_gpLastPresQpc, p0);
        g_gpStIv.add(iv);
        g_gpStIvSq += iv * iv;
        if (g_gpIvBuf.size() < 8192) g_gpIvBuf.push_back((float)iv);
        if (g_fgFree) GpQualPush((float)iv, p0);
    }
    g_gpLastPresQpc = p0;
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        ++g_gpStShown;
        ++g_sh->gpPresented;
        if (!g_gpFirstPresentTick) g_gpFirstPresentTick = GetTickCount64();
        if (r == VK_SUBOPTIMAL_KHR) g_gpNeedRecreate = true;
    } else if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_gpNeedRecreate = true;
    } else if (r == VK_ERROR_SURFACE_LOST_KHR) {
        GpDestroySwapchain("SURFACE_LOST");
        gvk.vkDestroySurfaceKHR(g_gpInst, g_gpSurf, nullptr);
        g_gpSurf = VK_NULL_HANDLE;
    } else {
        GpSetFailed("vkQueuePresentKHR %s", VkStr(r));
    }
}

void GpStats() {
    const LONGLONG now = Qpc();
    if (!g_gpStatQpc) g_gpStatQpc = now;
    const double el = Ms(g_gpStatQpc, now);
    if (el < 5000.0) return;
    if (g_gpTr && !g_gpTrOn) {  // ladeni: spoustec zaznamu presentu
        wchar_t go[MAX_PATH];
        swprintf(go, MAX_PATH, L"%s\\bms_dlss_gptrace.go", ProxyDir());
        if (GetFileAttributesW(go) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(go);
            g_gpTrOn = true;
            Log("prezentace: ladeni: zaznam presentu spusten");
        }
    }
    g_sh->gpPresMs = (float)g_gpStPres.avg();
    g_sh->gpPresMax = (float)g_gpStPres.max;
    g_sh->gpWakeMs = (float)g_gpStWake.avg();
    g_sh->gpWakeMax = (float)g_gpStWake.max;
    MemoryBarrier();
    ++g_sh->gpStatSeq;
    const double ivVar = g_gpStIv.n ? g_gpStIvSq / g_gpStIv.n - g_gpStIv.avg() * g_gpStIv.avg() : 0.0;
    const double ivSd = ivVar > 0.0 ? sqrt(ivVar) : 0.0;
    double ivMed = 0.0, ivP1 = 0.0, ivP99 = 0.0;
    unsigned ivShort = 0, ivLong = 0;
    if (!g_gpIvBuf.empty()) {
        std::vector<float>& b = g_gpIvBuf;
        std::sort(b.begin(), b.end());
        const size_t n = b.size();
        ivMed = b[n / 2];
        ivP1 = b[std::min(n - 1, n / 100)];
        ivP99 = b[std::min(n - 1, n * 99 / 100)];
        for (float v : b) {
            if (v < 0.75 * ivMed) ++ivShort;
            else if (v > 1.33 * ivMed) ++ivLong;
        }
    }
    g_gpIvBuf.clear();
    if (g_gpStShown || g_gpStSkip || g_gpStNoWnd)
        Log("prezentace: zobrazeni v procesu hry %.1f s: zobrazeno %u (%.1f/s), preskoceno %u, bez okna %u, swapchain "
            "%ux%u %s (novy %u) | probuzeni %.3f / %.2f ms, vkQueuePresent %.3f / %.2f ms, interval %.2f ms (sd %.3f, "
            "max %.2f; median %.2f, p1 / p99 %.2f / %.2f, mimo pasmo 0.75-1.33 x median: kratke %u, dlouhe %u)",
            el / 1000.0, g_gpStShown, g_gpStShown * 1000.0 / el, g_gpStSkip, g_gpStNoWnd, g_gpScExt.width,
            g_gpScExt.height, GpModeStr(g_gpScMode), g_gpStRecreate, g_gpStWake.avg(), g_gpStWake.max,
            g_gpStPres.avg(), g_gpStPres.max, g_gpStIv.avg(), ivSd, g_gpStIv.max, ivMed, ivP1, ivP99, ivShort, ivLong);
    g_gpStPres = g_gpStWake = g_gpStIv = Acc();
    g_gpStIvSq = 0;
    g_gpStShown = g_gpStSkip = g_gpStNoWnd = g_gpStRecreate = 0;
    g_gpStatQpc = now;
}

DWORD WINAPI GpThreadProc(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    g_gpLastSeq = 0;
    g_gpNeedRecreate = g_gpResyncDone = g_gpInUseLogged = false;
    g_gpInUseRetry = g_gpFirstPresentTick = g_gpLastFrameTick = 0;
    g_gpStatQpc = g_gpLastPresQpc = 0;
    Log("prezentace: zobrazeni v procesu hry - vlakno bezi (okno %p)", (void*)g_gpWnd);
    while (!g_gpQuit) {
        WaitForSingleObject(g_gpEv, 100);
        if (g_gpQuit) break;
        if (!g_sh) continue;
        MemoryBarrier();
        const int32_t gen = g_sh->gpGeneration;
        if ((g_gpDebug & 128) && gen) {  // ladeni: bez importu - jen vymazany swapchain pro kazdy zverejneny obraz
            const uint64_t pub = (uint64_t)InterlockedCompareExchange64((volatile LONG64*)&g_sh->gpPub, 0, 0);
            const uint64_t sq = pub & 0xFFFFFFFFFFFFull;
            if (sq && sq != g_gpLastSeq) {
                g_gpLastSeq = sq;
                g_gpImpGen = gen;
                GpProcess(sq);
                if (g_gpFailed) break;
            }
            GpStats();
            continue;
        }
        if (gen && gen != g_gpImpGen && !GpImport(gen) && g_gpFailed) break;
        const uint64_t pub = (uint64_t)InterlockedCompareExchange64((volatile LONG64*)&g_sh->gpPub, 0, 0);
        const uint64_t seq = pub & 0xFFFFFFFFFFFFull;
        const uint32_t pg = (uint32_t)(pub >> 48);
        if (seq && seq != g_gpLastSeq && g_gpImpGen && pg == ((uint32_t)g_gpImpGen & 0xFFFFu)) {
            // Rucni cil nad stropem VRR (v103h): obrazy po poradi. Helper slot neprepise, dokud ho proxy neprecetla
            // (gpSemDone), takze cekajicich je nejvys BMSD_GP_IMAGES. Jinak jako driv jen posledni zverejneny.
            uint64_t s0 = seq;
            if ((g_sh->gpFlags & 4u) && g_gpLastSeq && seq > g_gpLastSeq)
                s0 = std::max(g_gpLastSeq + 1, seq >= BMSD_GP_IMAGES ? seq - BMSD_GP_IMAGES + 1 : 1);
            if (g_gpLastSeq && s0 > g_gpLastSeq + 1) {
                g_gpStSkip += (unsigned)(s0 - g_gpLastSeq - 1);
                g_sh->gpSkipped += (uint32_t)(s0 - g_gpLastSeq - 1);
            }
            for (uint64_t s = s0; s <= seq && !g_gpFailed && !g_gpQuit; ++s) {
                g_gpLastSeq = s;
                const int k = (int)(s % BMSD_GP_IMAGES);
                const LONGLONG post = (LONGLONG)g_sh->gpQPost[k];
                const LONGLONG t = Qpc();
                g_gpCurFlags = g_sh->gpQFlags[k];
                g_gpCurReal = (LONGLONG)g_sh->gpQReal[k];
                g_gpCurWake = t;
                g_gpCurPost = post;
                if (post && t > post && Ms(post, t) < 100.0) g_gpStWake.add(Ms(post, t));
                GpProcess(s);
            }
            if (g_gpFailed) break;
        } else if (g_gpSc && !(g_gpDebug & 32768)) {
            // Bez noveho snimku: hra na cele obrazovce po minimalizaci (Alt+Tab) hned prestane kreslit, takze by se
            // swapchain uvolnil az po obnoveni okna. Do te doby by jeho posledni snimek zustal na displeji pres plochu
            // (zamrzly obraz, Alt+Tab "nefunguje", pomuze az Ctrl+Alt+Del) - 2026-11-20 17:39.
            RECT rc = {};
            if (!IsWindow(g_gpWnd)) {
                GpDestroySwapchain("okno zniceno (bez snimku)");
                GpSetState(BMSD_PST_NOWINDOW);
            } else if (IsIconic(GetAncestor(g_gpWnd, GA_ROOT)) || !IsWindowVisible(g_gpWnd) ||
                       !GetClientRect(g_gpWnd, &rc) || rc.right <= 0 || rc.bottom <= 0 ||
                       (g_atLostTop && g_gpScLost == 0)) {
                GpDestroySwapchain("okno minimalizovane / skryte / 0x0 / neni v popredi (bez snimku)");
                GpSetState(BMSD_PST_NOWINDOW);
            }
        }
        GpStats();
    }
    // uvolnit swapchain (okno se pak muze znicit), povrch a importy; jedno spolecne cekani max 1 s (GpStop ceka 3 s)
    if (!GpWaitCmds(1000000000ull)) Log("prezentace: WARN - kopie do swapchainu do 1 s nedobehly (konec vlakna)");
    GpTrWrite("konec vlakna");
    GpDestroySwapchain(g_gpFailed ? "chyba" : "konec", 0);
    if (g_gpSurf) gvk.vkDestroySurfaceKHR(g_gpInst, g_gpSurf, nullptr);
    g_gpSurf = VK_NULL_HANDLE;
    GpDestroyImports();
    if (g_sh && !g_gpFailed) g_sh->gpState = BMSD_PST_STOPPED;
    Log("prezentace: zobrazeni v procesu hry - vlakno skoncilo");
    return 0;
}

// Vlakno presenteru, ktere na GpStop neskoncilo vcas: dokud bezi, novy start se odmita (sdili globaly g_gp*)
HANDLE g_gpZombie;

// Hlavni vlakno: zastavit vlakno presenteru (uvolni swapchain na prezentacnim okne). Pred znicenim okna.
void GpStop(const char* why) {
    if (!g_gpTh) return;
    InterlockedExchange(&g_gpQuit, 1);
    SetEvent(g_gpEv);
    // Vlakno konci nejpozdeji za ~3 s (kopie 2 s + uklid 1 s); pri zaseknute GPU pockat dele, nez se okno znici
    DWORD r = WaitForSingleObject(g_gpTh, 3000);
    if (r != WAIT_OBJECT_0) {
        Log("prezentace: WARN - vlakno zobrazeni v procesu hry do 3 s neskoncilo (%s) - cekam dal", why);
        r = WaitForSingleObject(g_gpTh, 5000);
    }
    if (r == WAIT_OBJECT_0) {
        CloseHandle(g_gpTh);
    } else {
        Log("prezentace: CHYBA - vlakno zobrazeni v procesu hry neskoncilo ani za 8 s (%s) - dalsi start odmitnut",
            why);
        if (g_gpZombie) CloseHandle(g_gpZombie);
        g_gpZombie = g_gpTh;
    }
    g_gpTh = nullptr;
}

// Hlavni vlakno: zapnout zobrazeni v procesu hry na okne wnd. false = zobrazuje helper (vlastni swapchain, bez VRR).
bool GpStart(HWND wnd) {
    if (!cfg.gamePresent) return false;
    if (!g_gpDevTried) {
        g_gpDevTried = true;
        g_gpDevOk = GpInitDevice();
        if (!g_gpDevOk) Log("prezentace: zobrazeni v procesu hry nejde - zobrazuje helper (bez VRR)");
    }
    if (!g_gpDevOk) return false;
    if (!g_gpEv) {
        wchar_t name[128];
        swprintf(name, 128, L"Local\\bms_dlss_%lu_gpframe", GetCurrentProcessId());
        g_gpEv = CreateEventW(nullptr, FALSE, FALSE, name);
        if (!g_gpEv) return Log("prezentace: zobrazeni v procesu hry - event nejde vytvorit"), false;
    }
    GpStop("novy start");
    if (g_gpZombie) {
        if (WaitForSingleObject(g_gpZombie, 0) != WAIT_OBJECT_0)
            return Log("prezentace: zobrazeni v procesu hry - predchozi vlakno jeste bezi, zobrazuje helper"), false;
        CloseHandle(g_gpZombie);
        g_gpZombie = nullptr;
    }
    g_gpWnd = wnd;
    InterlockedExchange(&g_gpQuit, 0);
    InterlockedExchange(&g_gpFailed, 0);
    g_gpWhy[0] = 0;
    g_gpTh = CreateThread(nullptr, 0, GpThreadProc, nullptr, 0, nullptr);
    if (!g_gpTh) return Log("prezentace: zobrazeni v procesu hry - vlakno nejde spustit"), false;
    return true;
}
