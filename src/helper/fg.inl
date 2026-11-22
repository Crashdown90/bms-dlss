// bms-dlss: prezentace v hlavnim helperu a Frame Generation (od BMSD_VERSION 12, BmsdShared::fgPresenter = 1).
// Vklada se do helper.cpp (pouziva jeho Vulkan zarizeni, NGX, obrazy mostu a pomocne funkce).
//
// Proxy ([fg] Mode != 0 pri startu hry, src/present.cpp) presmeruje DXVK do skryteho okna a kazdy finalni snimek
// (backbuffer v Present, i s HUD a menu) zkopiruje do sdileneho obrazu (BmsdPresentShared, kanal ..._pframe). Tady:
//   * hlavni vlakno (FgOnPresentMsg): snimek N prevezme - bud ho zkopiruje do vlastniho obrazu "real", nebo nad nim
//     vyhodnoti generator (DLSS-G nebo FSR FG: vstup backbuffer + snapshot MV a hloubky snimku mostu B, ktery
//     k nemu patri; vystupy "interp" a "real"). Sdileny obraz hned vraci proxy (semDone = N, helperAck). NGX i ffx
//     API se volaji jen odsud (nejsou thread-safe).
//   * vlakno presentu (FgPresentThread): ma swapchain na prezentacnim okne proxy; pocka na GPU, pak prezentuje
//     generovany snimek a za polovinu intervalu skutecnych snimku skutecny (bez FG jen skutecny hned). Vlastni fronta
//     (kdyz ji GPU ma; jinak sdilena pod zamkem), vlastni command pool.
//   * snapshot (FgSnapshot, vola ProcessFrame mostu): MV (g_mvImg) a hloubka (DEPTH_IN) snimku B se kopiruji do kruhu
//     3 vlastnich obrazu - most je dalsim snimkem prepise, generator je potrebuje az v Present.
// Generatory jsou dva nad stejnymi vstupy (existuje vzdy nejvys jeden, podle [fg] Mode / menu): DLSS-G pres prime NGX
// (FgRecordDlssg) a AMD FSR Frame Generation 2x pres ffx API bez swapchainu (FgCreateFsr / FgRecordFsr, podepsane DLL
// amd_fidelityfx_loader_vk.dll + amd_fidelityfx_framegeneration_vk.dll vedle helperu). Spolecne: FgEnsureFeature
// (vytvoreni pro velikosti backbufferu a mostu, prepnuti rezimu), FgReleaseFeature, pacing, snapshot, limiter, zaloha.
// Selhani cehokoli (create, evaluate, swapchain, okno) = jen skutecne snimky, pripadne stav BMSD_PST_FAILED -> proxy
// prejde na zalohu (DXVK primo do okna hry). Proces helperu kvuli prezentaci nikdy nekonci (bezi v nem most DLSS).

#include <condition_variable>
#include <thread>
// (g_fgPresenter, g_presQueue, g_queueMx, g_presQMx a DeviceWaitIdleLocked jsou v helper.cpp u Vulkanu)

// ---- konfigurace ([fg] v bms_dlss.ini vedle helperu; Mode, TargetFps a PauseInMenu cte proxy) ----
static struct FgCfg {
    int matrixTranspose = 1;  // matice kamery predat transponovane (guide: row-major, "post-multiplication")
    int dumpFrames = 0;       // ulozit tolik dvojic interp / real (BMP vedle logu) kvuli overeni
    int dumpAfter = 300;      // ... az po tolika vygenerovanych snimcich
    float sharpenGen = 0.0f;  // [fg] SharpenGen 0..1: RCAS generovanych snimku (0 = vypnuto), v25i
    int dlssgUir = 0;
    int dlssgUiAlpha = 0;     // [fg] DlssgUiAlpha: maska UI z rozdilu snimek - HUD-less + UIR (pokus v25u)
    float uiAlphaThr = 3.0f;  // [fg] UiAlphaThreshold: prah rozdilu v 1/255
    int uiAlphaWeapon = 1;    // [fg] UiAlphaWeapon: zbran v ruce (hloubka <= ViewmodelDepth) take do masky UI (v25w)         // [fg] DlssgUir: UserInterfaceRecompositionEnabled pri vytvoreni DLSS-G (pokus v25t: preset B?)
    int dlssgLite = -1;       // [fg] DlssgLiteBlending: -1 = nenastavovat (vychozi NGX), 0/1 = LiteCandidateBlendingEnabled (v25p)
    int dumpTrigger = 0;      // [fg] DumpTrigger=1: dump zacne az po vytvoreni souboru fgdump_go vedle helperu
    int dynRes = 1;           // DynamicResolutionScaling pri vytvoreni feature (cast B: sub-rect MV a hloubky)
    int separateQueue = 1;    // vlastni fronta pro present (kdyz ji GPU ma)
    int menuDetect = 0;       // menuDetectionEnabled (spike K0 overil 0; snimky menu hry se stejne negeneruji)
    std::string fsrVersion;   // [fg] FsrVersion: cast nazvu verze FSR FG z nabidky ffx API ("3.1"); "" = vybere ffx API
    int log = 0;              // 1 = podrobny log kazdeho snimku (ladeni)
    int hudlessUse = 1;       // [fg] HudLessUse (ladeni K5): 0 = obrazy HUD-less od proxy jen dumpovat, generatoru
                              // je nepredavat (A/B mereni se stejnou maskou HUD jako v13)
    int adaptSnapPct = 5;     // [fg] AdaptSnapPct: adaptivni FSR rozvrh A - slot s t >= 1 - x % ukaze skutecny snimek
    double adaptSnapMs = 0.0; // [fg] AdaptiveSnapMs (pokus, 0 = vyp): rozvrh A - slot s obsahovym casem do x ms od
                              // skutecneho snimku (pred nim i za nim) ukaze skutecny snimek misto mezisnimku
                              // (nahrazuje AdaptSnapPct)
    int vblankGrid = 0;       // [fg] VblankGrid (pokus, 0 = vyp; jen s PresentTiming=1): rozvrh A na monitoru bez VRR -
                              // mrizka s periodou obnovy (namerenou) a fazi zamcenou na cas zobrazeni (vblank)
    double vblankLeadMs = 3.0;  // [fg] VblankLeadMs: volani presentu tolik ms pred vblankem, pro ktery je slot
} g_fgCfg;

// ---- Vulkan funkce navic (swapchain, cekani na semafor, cteni obrazu) ----
#define FG_INST_FUNCS(X)                                                                                            \
    X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR)                      \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)                           \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define FG_DEV_FUNCS(X)                                                                                             \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)           \
    X(vkQueuePresentKHR) X(vkQueueWaitIdle) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkCmdCopyImageToBuffer)    \
    X(vkDestroyBuffer) X(vkUnmapMemory) X(vkGetSemaphoreCounterValue) X(vkGetFenceStatus)
static struct {
#define DECL(n) PFN_##n n;
    FG_INST_FUNCS(DECL)
    FG_DEV_FUNCS(DECL)
#undef DECL
} fvk;

// ---- stav kanalu ----
static BmsdPresentShared* g_ps;       // nullptr = mapovani zatim neni (proxy ho vytvari pri prvnim Present)
static DWORD g_fgPid;
static HANDLE g_fgEvFrame, g_fgEvDone;
static bool g_fgVkOk;                 // funkce swapchainu nacteny, pooly a vlakno existuji
static bool g_fgDead;                 // presenter skoncil (stop od proxy, chyba, jina verze): zpravy se ignoruji
static bool g_fgStopByProxy;          // konec na zadost proxy (FG vypnuta / zaloha): jde znovu zapnout (restartSeq)
static uint32_t g_fgRestartSeq;       // restartSeq od proxy, se kterym prezentace naposledy zacala
static volatile bool g_fgWndGone;     // prezentace skoncila, protoze proxy znicila prezentacni okno (jde znovu zapnout)
static bool g_fgFreed;
static ULONGLONG g_fgNextAttach;
static int32_t g_fgGen;               // generace sdilenych obrazu presentu
static bool g_fgImgOk;
static const int kFgK = BMSD_PRESENT_IMAGES;
static Img g_fgPres[kFgK];            // sdilene obrazy (backbuffer hry), import
static VkImageView g_fgPresView[kFgK];
// HUD-less (K5, BMSD_PRESENT_VERSION 3): barva sceny pred HUD od proxy, import vedle g_fgPres. g_fgHlOk = obrazy v teto
// generaci existuji -> generator dostava HUD-less vzdy (snimek bez platneho HUD-less = backbuffer, tj. "bez UI").
static Img g_fgHl[kFgK];
static VkImageView g_fgHlView[kFgK];
static bool g_fgHlOk;
static bool FgHlGen() { return g_fgHlOk && g_fgCfg.hudlessUse; }  // generator dostava HUD-less
static VkFormat g_fgFmt = VK_FORMAT_UNDEFINED;
static uint32_t g_fgBbW, g_fgBbH;
static VkSemaphore g_fgSemReady, g_fgSemDone;  // timeline od proxy
static VkSemaphore g_fgSemEval;                // vlastni timeline: prace hlavniho vlakna pro polozku seq je na GPU hotova
static uint64_t g_fgSeq;
static uint64_t g_fgLastN;

// Vystupy: kruh obrazu "interp" + "real" (format backbufferu, trvale v GENERAL). Stav pod g_fgMx.
// Nasobek FG ([fg] Multiplier, menu): 2x = 1 generovany snimek na skutecny, 3x = 2, 4x = 3 (jen DLSS-G na GPU, ktere
// to umi - NGX MultiFrameCountMax). Generovane snimky jednoho skutecneho lezi v interp[0..n-1] (v case 1/N, 2/N, ...).
static const int kFgMaxInterp = 5;  // DLSS-G 6x = 5 (od v25j; RTX 50, MultiFrameCountMax 5)
static const int kAfgMaxInterp = 4; // adaptivni FSR az 4 generovane na skutecny (FgAfgGrowInterp)
static const int kFgOut = 4;
enum { FGO_FREE, FGO_QUEUED, FGO_BUSY };
struct FgOut {
    OwnImg interp[kFgMaxInterp], real;
    int state = FGO_FREE;
};
static FgOut g_fgOut[kFgOut];
static int g_fgNextOut;
static bool g_fgOutStorage;           // obrazy maji STORAGE (generator do nich muze psat)
static int g_fgInterpN = 1;           // kolik obrazu interp ma kazdy slot (podle pozadovaneho nasobku, FgGrowInterp)
static int32_t g_fgGrowFailGen = -1;  // generace, ve ktere doplneni obrazu interp selhalo (dal se nezkousi)
static int g_fgMultMax = 2;           // nejvyssi nasobek DLSS-G na tomto GPU (MultiFrameCountMax + 1, 2..6)
static int g_fgLastMult = 0;          // nasobek posledniho vyhodnoceni (zmena = jen log)
static int g_fgActMult = 2;           // nasobek, se kterym se prave generuje (pro proxy: limiter TargetFps / nasobek)

// Snapshot vstupu generatoru (kruh podle B % 3)
struct FgSnap {
    OwnImg mv, depth;
    VkDescriptorSet mvSet = VK_NULL_HANDLE;  // set 1 mv.comp s obrazem mv (od v24y; z g_descPool, nerusi se)
    uint64_t B = 0;
    bool valid = false, reset = false, camOk = false;
    float cam[16], prevCam[16];
    uint32_t rW = 0, rH = 0;
};
static const int kFgSnap = 3;
static FgSnap g_fgSnap[kFgSnap];
static bool g_fgSnapOk;               // obrazy snapshotu existuji (velikost g_w x g_h)
static bool g_fgSnapWanted;           // FG je pozadovane a feature existuje: ProcessFrame ma snapshot delat
static bool g_fgHagsOff;              // HAGS je ve Windows vypnute (DLSS-G ho vyzaduje): rada v duvodu chyby

// Prikazy hlavniho vlakna pro present (slot = snimek)
static VkCommandBuffer g_fgCmd[kSlots];
static VkFence g_fgFence[kSlots];
static bool g_fgFenceUsed[kSlots];
static int g_fgSlot;
static VkQueryPool g_fgQpool;
static bool g_fgTsUsed[kSlots];

// Generator (DLSS-G)
static NVSDK_NGX_Parameter* g_fgParams;
static NVSDK_NGX_Handle* g_dlssg;
static bool g_fgAvailKnown, g_fgAvail;  // FrameGeneration.Available z NGX
static std::string g_fgAvailWhy;
// Selhani po rezimech (BMSD_FG_*): create / evaluate selhalo -> v tom rezimu do konce jen skutecne snimky; druhy
// generator jde dal zkusit
static bool g_fgFailedM[BMSD_FG_COUNT];
static std::string g_fgFailWhyM[BMSD_FG_COUNT];
static uint32_t g_fgCurMode = BMSD_FG_DLSSG;  // rezim, ktery se prave resi (FgFail zapisuje jeho selhani)
static uint32_t g_fgKeyBbW, g_fgKeyBbH, g_fgKeyW, g_fgKeyH;  // velikosti, pro ktere feature / kontext vznikl
// Generator (AMD FSR Frame Generation 2x, ffx API bez swapchainu; kod podle tools/spike_ffg/ffg_eval.cpp)
static const wchar_t* const kFfgDll = L"amd_fidelityfx_framegeneration_vk.dll";
static ffxContext g_ffg;                // kontext FSR FG (nullptr = zadny)
static bool g_ffgAvail;                 // DLL jsou, podpis plati a ffx API nabizi verzi FG pro toto GPU
static std::string g_ffgAvailWhy;
static std::string g_ffgVersion;        // verze provideru ("3.1.6"; menu: "FSR FG 3.1.6")
static uint64_t g_ffgForceId;           // ffxOverrideVersion z [fg] FsrVersion, 0 = verzi vybere ffx API
static uint64_t g_ffg31Id;              // nejnovejsi verze "3.1.x" z nabidky (zaloha, kdyz vychozi verze nejde vytvorit:
static std::string g_ffg31Name;         // na Radeon RX 9000 muze ffx API nabidnout ML FG 4.x - neovereno)
static int g_ffgLastSlot = -1;          // slot prikazu posledniho snimku s FSR FG (pred dalsim zaznamem musi dobehnout)
static bool g_fgFresh;                  // prvni vyhodnoceni po vytvoreni / pauze -> reset
static uint64_t g_fgLastEvalB;
static uint64_t g_fgFrameId;
static unsigned g_fgGenTotal;           // vygenerovanych snimku celkem (pro DumpAfter)
static int g_fgDumpLeft, g_fgDumpIdx;
static bool g_fgDumpArmed;
static unsigned g_fgDumpPoll;
// DumpTrigger (v25h): dump az po vytvoreni souboru fgdump_go vedle helperu (testy: pohled nastaveny, kamera se toci)
static bool FgDumpArmed() {
    if (!g_fgCfg.dumpTrigger || g_fgDumpArmed) return true;
    if (++g_fgDumpPoll % 16u) return false;
    const std::wstring f = g_dir + L"\\fgdump_go";
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    DeleteFileW(f.c_str());
    g_fgDumpArmed = true;
    Log("FG: dump spusten souborem fgdump_go");
    return true;
}
static int g_fgPrevOutSlot = -1;        // vystupni slot minuleho snimku (jeho "real" = predchozi skutecny snimek, dump)
static uint64_t g_fgPrevOutN;
static int32_t g_fgLoggedState = 99;
static std::string g_fgLoggedMsg;
static std::string g_fgDlssgVersion;    // verze skutecne nactene nvngx_dlssg ("310.9.16"; "" = zatim nezjistena)
static bool g_fgDlssgVersionTried;

// Vlakno presentu
struct FgItem {
    int slot;
    int nInterp;        // generovanych snimku pred skutecnym (0 = jen skutecny; nasobek = nInterp + 1)
    uint64_t seq, N;
    LONGLONG postQpc;
    double intervalMs;  // odhad intervalu skutecnych snimku (EMA)
    // Adaptivni FSR, rozvrh A (pevny rytmus): kazdy snimek ma svuj cas zobrazeni (QPC) z mrizky cile; skutecny snimek
    // se zobrazi jen kdyz na nej padl slot mrizky (showReal). Ostatni rezimy (2x, MFG, rozvrh B) rozvrh podle nasobku.
    bool fixedTimes;
    bool showReal;
    LONGLONG disp[kFgMaxInterp], dispReal;
    LONGLONG content[kFgMaxInterp + 1];  // obsahovy cas snimku (QPC; posledni = skutecny) - jen statistika latence
};
static std::mutex g_fgMx;
static std::condition_variable g_fgCv;
static bool g_ptCatchup;  // vlakno presentu: za aktualni polozkou ceka dalsi ([fg] Catchup)
static int g_ptCatchupMode = -1;  // [fg] Catchup: 0 = vyp, 1 = podle poctu polozek ve fronte, 2 = jen polozky hotove na GPU
static FgItem g_fgQ[3];  // fronta pro vlakno presentu (2x / MFG: nejvys 2, adaptivni rozvrh A: 3)
static int g_fgQn;
static bool g_fgQuit, g_fgQuiesce, g_fgParked, g_fgStopReq;
static std::thread g_fgThread;
static HANDLE g_ptTimer;
// stav vlakna presentu (jen z nej)
static HWND g_ptWnd;
static VkCommandPool g_ptPool;
static VkCommandBuffer g_ptCmd[2];
static VkFence g_ptFence[2];
static bool g_ptFenceUsed[2];
static VkSemaphore g_ptAcq[2];
static int g_ptCmdIdx;
static VkSurfaceKHR g_ptSurf;
static VkSwapchainKHR g_ptSc;
static VkImage g_ptScImg[16];
static VkSemaphore g_ptScDone[16];
static uint32_t g_ptScCount;
static VkExtent2D g_ptScExt;
// Klientska plocha okna (GetClientRect) pri vytvoreni swapchainu. Zmena velikosti se posuzuje podle ni, ne podle
// g_ptScExt (= currentExtent ovladace): kdyby se obe velikosti lisily (DPI virtualizace, zaokrouhleni ovladace),
// swapchain by se jinak vytvarel znovu kazdy snimek.
static SIZE g_ptScClient;
static bool g_ptFmtLogged;  // nahradni format swapchainu uz zalogovan
static VkPresentModeKHR g_ptScMode;
static uint32_t g_ptScInterval = 2;
static bool g_ptNeedRecreate, g_ptResyncDone, g_ptInUseLogged;
static volatile bool g_ptFailed, g_ptStopped;
struct FgWndState {
    LONG style, ex;
    RECT rc;
    BOOL vis;
};
static FgWndState g_ptScWnd;
static ULONGLONG g_ptFirstPresentTick, g_ptLastFrameTick, g_ptFrameGap, g_ptInUseRetry, g_ptInUseSince;
static LONGLONG g_ptLastPresQpc;     // konec posledniho vkQueuePresentKHR (statistika intervalu)
static LONGLONG g_ptLastCallQpc;     // zacatek posledniho vkQueuePresentKHR (rozvrh: volani presentu trva ~0.5 ms a
                                     // rozvrh podle jeho konce by kazdy snimek o tuto dobu zpozdil)
static LONGLONG g_ptNextInterpQpc;  // cil pristiho generovaneho snimku (vyrovnani kolisani), 0 = zadny
static const ULONGLONG kFgResyncMs = 1500;

// Statistika (vlakno presentu zapisuje pod g_fgStatMx, hlavni vlakno publikuje kazdych ~5 s)
struct FgAcc {
    double sum = 0, sum2 = 0, max = 0, min = 1e300;
    unsigned n = 0;
    void add(double v) {
        sum += v;
        sum2 += v * v;
        if (v > max) max = v;
        if (v < min) min = v;
        ++n;
    }
    double avg() const { return n ? sum / n : 0.0; }
    double sd() const {
        if (n < 2) return 0.0;
        const double m = sum / n, var = sum2 / n - m * m;
        return var > 0.0 ? sqrt(var) : 0.0;
    }
};
static std::mutex g_fgStatMx;
static struct {
    unsigned real = 0, gen = 0, recreate = 0, late = 0, dropped = 0;
    unsigned behind = 0, genFail = 0;  // generovany snimek vynechan (vlakno pozadu) / nesel zobrazit
    unsigned catchup = 0;              // polozky ukazane se zkracenym krokem ([fg] Catchup)
    FgAcc interval, pres, done;
    FgAcc gpuDone;       // post -> skutecne hotovo na GPU (vlakno FgGpuWatchThread, v24u)
    FgAcc ivCall;        // interval mezi zacatky volani vkQueuePresentKHR (to, co ridi rozvrh)
    FgAcc lat;           // zacatek volani presentu - obsahovy cas snimku (latence zobrazeni od Present hry)
    unsigned minIvWait = 0;  // present cekal na nejkratsi interval (strop VRR)
    unsigned ivShort = 0;    // interval zacatku volani kratsi nez nejkratsi povoleny - 0.05 ms (nemelo by nastat)
    unsigned adLate = 0, adDrop = 0;  // rozvrh A: slot zobrazen pozde (> P/4) / zahozen (> 2 P pozde)
    unsigned gpBusy = 0;  // zobrazeni v procesu hry: obraz jeste u proxy (snimek vynechan)
    // [fg] PresentTiming=1: skutecne zobrazeni (VK_EXT_present_timing, IMAGE_FIRST_PIXEL_OUT)
    FgAcc dIv;           // interval mezi zobrazenimi po sobe jdoucich presentu (ms)
    FgAcc dLat;          // zacatek volani presentu -> prvni pixel ven (ms)
    FgAcc dCont;         // prvni pixel ven - obsahovy cas snimku (ms; konstantni cast = latence, rozptyl = chyba)
    FgAcc dJud;          // (interval zobrazeni - interval obsahu) po sobe jdoucich snimku (ms) = chyba casu obsahu
    FgAcc dJudAbs;       // |totez| (prumer a max)
    unsigned dN = 0, dDbl = 0, dLong = 0, dLate = 0;  // zobrazeno s casem / double tap (< 0.5 P) / > 1.5 P / pozde
    unsigned dGrid = 0, dGridN = 0;  // intervaly na mrizce obnovy (+-0.12 ms od nasobku periody) / posuzovane
    unsigned dNoTime = 0, dSkipReq = 0;  // vysledek bez casu / present bez dotazu (fronta vysledku plna)
} g_ptSt;
static struct {
    unsigned frames = 0, paired = 0, unpaired = 0, evals = 0;
    unsigned hudless = 0;  // vyhodnoceni s platnym HUD-less od proxy (K5)
    FgAcc wake, eval;
} g_fgSt;
static LONGLONG g_fgStatQpc, g_fgLastPostQpc;
static double g_fgIntervalMs = 12.66;

// ---- Adaptivni FSR (BMSD_FG_FSRADAPT): generator AFG (src/helper/afg) + rozvrh A / B ----
// Nejkratsi interval mezi presenty (strop VRR / limiter ovladace: 1000 / (f - f^2/3600) ms), plati pro vsechny rezimy;
// nastavuje hlavni vlakno z frekvence od proxy, 0 = bez omezeni.
static volatile LONGLONG g_ptMinIvQ;
static bool g_afgAvail = true;          // AFG je prelozene v helperu; selhani vytvoreni -> g_fgFailedM
static bool g_afgOk;                    // kontext AFG existuje
static int g_afgLastSlot = -1;          // slot prikazu posledniho paru AFG (musi dobehnout pred dalsim zaznamem)
static uint64_t g_afgFrameId;           // frameId paru AFG (+1 za kazdy AfgRecordPair)
static bool g_afgPrevPaired;            // minuly skutecny snimek prosel AfgRecordPair (dalsi par navazuje)
static volatile double g_adDoneMs = 3.0;  // vlakno presentu: EMA postQpc -> snimek hotov na GPU (ms)
static volatile double g_adLateMs;        // vlakno presentu: nejvetsi zpozdeni slotu A od posledniho cteni (ms)
static struct {
    bool active;                        // A: hystereze - generuje (jinak jen skutecne snimky)
    LONGLONG belowSince, aboveSince;    // A: od kdy jsou skutecne FPS pod / nad hranici (QPC)
    double ivEma = 0.0;                 // EMA intervalu skutecnych snimku (ms)
    double ivHist[8] = {};              // poslednich 8 intervalu (odhad L)
    int ivPos = 0;
    LONGLONG nextDisp;                  // A: cas zobrazeni pristiho slotu mrizky (QPC), 0 = mrizka nebezi
    double L = 0.0;                     // A: posun obsahu za casem zobrazeni (ms) - aktualni
    double lateBoost = 0.0;             // A: pridavek k L po pozdnich slotech (pomalu klesa)
    bool engaged;                       // generuje (rozvrh A aktivni) - pro proxy a menu
    unsigned switches = 0, slots = 0, slotsReal = 0, slotsDrop = 0;  // statistika okna
    FgAcc Lacc;
    FgAcc snapErr;                      // A + AdaptiveSnapMs: |obsah slotu - skutecny snimek| u prichycenych (ms)
    FgAcc vbErr;                        // A + VblankGrid: |chyba faze mrizky proti vblanku| (ms)
    FgAcc contErr;                      // A: odchylka obsahu zobrazeneho snimku od idealu (ms; mezisnimek 0, skutecny |c - rN|)
    int lastPacing = -1;
    // A: uzitecnost generovani - okno ~1 s: zobrazenych snimku (presCount*) na skutecny snimek
    LONGLONG winStart = 0, activeSince = 0, blockUntil = 0;
    uint64_t winShown0 = 0, winGen0 = 0;
    unsigned winReal = 0, lowGain = 0, uselessOff = 0;
    double lastGain = 0.0, backoffS = 5.0;
} g_ad;
// Regrese casu generatoru adaptivniho FSR na snimek: cas = par + n * mezisnimek (v24z6, jen mereni do logu)
static double g_afgRegN, g_afgRegSx, g_afgRegSy, g_afgRegSxx, g_afgRegSxy;
static int g_fgTsGen[kSlots] = {-1, -1, -1};

static LONGLONG FgQpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
static double FgMs(LONGLONG a, LONGLONG b) { return (double)(b - a) * 1000.0 / (double)g_qpf.QuadPart; }

static const char* FgVkStr(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    default: return "jina chyba";
    }
}
static const char* FgModeStr(VkPresentModeKHR m) {
    switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "?";
    }
}

// Trvala chyba presenteru: proxy prejde na zalohu (DXVK primo do okna hry). Proces helperu bezi dal.
static void FgPresFatal(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Log("prezentace: CHYBA: %s", buf);
    if (g_ps) {
        strncpy(g_ps->helperMsg, buf, sizeof g_ps->helperMsg - 1);
        g_ps->helperMsg[sizeof g_ps->helperMsg - 1] = 0;
        MemoryBarrier();
        g_ps->helperState = BMSD_PST_FAILED;
    }
    g_ptFailed = true;
}

static void FgSetPresState(int32_t s) {
    if (g_ps->helperState != s && g_ps->helperState != BMSD_PST_FAILED) {
        static const char* names[] = {"start", "prezentuje", "okno minimalizovane / 0x0", "okno obsazene", "zastaveno"};
        Log("prezentace: stav: %s", s >= 0 && s <= 4 ? names[s] : "chyba");
        g_ps->helperState = s;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Vlakno presentu: swapchain
// ---------------------------------------------------------------------------------------------------------------
// [fg] PresentTiming=1 (jen mereni, vychozi vyp): VK_EXT_present_timing - pro kazdy present cas, kdy prvni pixel
// opustil GPU (IMAGE_FIRST_PIXEL_OUT; FIRST_PIXEL_VISIBLE ovladac nehlasi). Rozsireni zapina InitVulkan v helper.cpp
// (g_ptTimingExt). Vysledky se ctou bez blokovani po kazdem presentu, casy se prevedou na QPC a porovnaji s casem
// volani presentu a obsahovym casem snimku. Jen vlakno presentu.
static struct {
    PFN_vkSetSwapchainPresentTimingQueueSizeEXT setQueueSize;
    PFN_vkGetSwapchainTimingPropertiesEXT timingProps;
    PFN_vkGetSwapchainTimeDomainPropertiesEXT domainProps;
    PFN_vkGetPastPresentationTimingEXT past;
    PFN_vkGetCalibratedTimestampsKHR calib;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR surfCaps2;
} ptx;
static bool g_ptmFuncs;             // funkce nacteny (FgInit)
static bool g_ptmBroken;            // swapchain s present timing nesel vytvorit / chyba -> do konce bez mereni
static bool g_ptmOn;                // aktualni swapchain ma present timing
static VkPresentStageFlagsEXT g_ptmStages;
static uint64_t g_ptmDomainId;
static VkTimeDomainKHR g_ptmDomain;
static int g_ptmUnit;               // 0 = zatim nezjisteno, 1 = tiky QPC, 2 = ns QPC, 3 = mistni domena (ns + posun)
static LONGLONG g_ptmLocalOff;      // mistni domena: QPC = ns * qpf / 1e9 + posun
static ULONGLONG g_ptmCalibTick;
static uint64_t g_ptmNextId, g_ptmDoneId;  // posledni pridelene id / posledni prectene
static const uint32_t kPtmQueue = 64;
static uint64_t g_ptmPropsCtr = ~0ull, g_ptmDomCtr = ~0ull;
static double g_ptmRefreshMs;       // perioda obnovy pro posouzeni mrizky (swapchain / DWM / proxy)
static uint64_t g_ptmRefDur, g_ptmRefIv;  // posledni refreshDuration / refreshInterval ovladace (ns)
struct PtmRec {
    uint64_t id;
    LONGLONG call, content;
    bool gen;
};
static PtmRec g_ptmRec[256];
static uint64_t g_ptmLastId;        // posledni zobrazeny (pro interval)
static LONGLONG g_ptmLastF, g_ptmLastC;
static double g_ptmIvEma, g_ptmLatEma;
static unsigned g_ptmLoggedFirst;
static unsigned g_ptmPropsLogs, g_ptmPropsChanges;  // vypis vlastnosti swapchainu (ovladac je meni casto)
static bool g_ptmLastVrr;
static double g_ptmREst;            // perioda obnovy odhadnuta z intervalu zobrazeni (ms; 0 = zatim ne)
// VblankGrid: posledni cas zobrazeni (QPC) a perioda obnovy pro rozvrh A v hlavnim vlakne (0 = nezname)
static volatile LONGLONG g_vbRefQ;
static volatile double g_vbPeriodMs;
static volatile LONGLONG g_ptCapOverrideQ;
// [fg] PresentTimingDump=N (ladeni): N zaznamu (id, volani, zobrazeni, obsah, generovany) od 15. s do CSV vedle logu
struct PtmDumpRec {
    uint64_t id;
    LONGLONG call, F, content;
    int gen;
};
static std::vector<PtmDumpRec> g_ptmDump;
static size_t g_ptmDumpN;
static ULONGLONG g_ptmDumpFrom;
static volatile bool g_ptmDumpFull, g_ptmDumpDone;  // VblankGrid aktivni: nejkratsi interval presentu = perioda obnovy

static void FgPtmReset() {
    g_ptmOn = false;
    g_vbRefQ = 0;
    g_ptmNextId = g_ptmDoneId = g_ptmLastId = 0;
    g_ptmLastF = g_ptmLastC = 0;
    g_ptmUnit = 0;
    g_ptmPropsCtr = g_ptmDomCtr = ~0ull;
}

static void FgPtmReadProps() {
    VkSwapchainTimingPropertiesEXT tp = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
    uint64_t ctr = 0;
    const VkResult r = ptx.timingProps(g_dev, g_ptSc, &tp, &ctr);
    if (r != VK_SUCCESS && r != VK_NOT_READY) return;
    g_ptmPropsCtr = ctr;
    g_ptmRefDur = tp.refreshDuration;
    g_ptmRefIv = tp.refreshInterval;
    if (tp.refreshDuration && tp.refreshDuration != UINT64_MAX && !g_ptmREst) g_ptmRefreshMs = tp.refreshDuration / 1e6;
    ++g_ptmPropsChanges;
    const bool vrr = tp.refreshInterval == UINT64_MAX;
    if (g_ptmPropsLogs >= 4 && vrr == g_ptmLastVrr) return;  // ovladac meni hodnoty skoro kazdy snimek: jen zmena VRR
    ++g_ptmPropsLogs;
    g_ptmLastVrr = vrr;
    Log("prezentace: present_timing vlastnosti (%s, citac %llu): refreshDuration %llu ns, refreshInterval %llu ns%s",
        FgVkStr(r), (unsigned long long)ctr, (unsigned long long)tp.refreshDuration,
        (unsigned long long)tp.refreshInterval,
        tp.refreshInterval == UINT64_MAX ? " (ovladac hlasi VRR)" : tp.refreshInterval ? " (pevna frekvence?)" : "");
}

// Kalibrace mistni casove domeny swapchainu na QPC (jen kdyz ovladac QPC domenu nenabizi)
static void FgPtmCalib() {
    if (!ptx.calib || g_ptmUnit != 3) return;
    VkSwapchainCalibratedTimestampInfoEXT sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT};
    sci.swapchain = g_ptSc;
    sci.presentStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
    sci.timeDomainId = g_ptmDomainId;
    VkCalibratedTimestampInfoKHR ci[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, &sci, g_ptmDomain},
                                          {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr,
                                           VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR}};
    uint64_t ts[2] = {}, dev = 0;
    if (ptx.calib(g_dev, 2, ci, ts, &dev) != VK_SUCCESS) return;
    g_ptmLocalOff = (LONGLONG)ts[1] - (LONGLONG)((double)ts[0] * (double)g_qpf.QuadPart / 1e9);
    g_ptmCalibTick = GetTickCount64();
}

static void FgPtmReadDomains() {
    uint64_t ctr = 0;
    VkSwapchainTimeDomainPropertiesEXT dp = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT};
    if (ptx.domainProps(g_dev, g_ptSc, &dp, &ctr) != VK_SUCCESS && dp.timeDomainCount == 0) return;
    VkTimeDomainKHR doms[16] = {};
    uint64_t ids[16] = {};
    dp.timeDomainCount = std::min(dp.timeDomainCount, 16u);
    dp.pTimeDomains = doms;
    dp.pTimeDomainIds = ids;
    const VkResult r = ptx.domainProps(g_dev, g_ptSc, &dp, &ctr);
    g_ptmDomCtr = ctr;
    std::string list;
    int pick = -1, local = -1;
    for (uint32_t i = 0; i < dp.timeDomainCount; ++i) {
        char b[48];
        snprintf(b, sizeof b, " %d/%llu", (int)doms[i], (unsigned long long)ids[i]);
        list += b;
        if (doms[i] == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR && pick < 0) pick = (int)i;
        if ((doms[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT || doms[i] == VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT) &&
            local < 0)
            local = (int)i;
    }
    if (pick < 0 && local >= 0 && ptx.calib) pick = local;
    if (pick >= 0) {
        g_ptmDomain = doms[pick];
        g_ptmDomainId = ids[pick];
        g_ptmUnit = g_ptmDomain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR ? 0 : 3;
        if (g_ptmUnit == 3) FgPtmCalib();
    }
    Log("prezentace: present_timing casove domeny (%s, citac %llu, domena/id):%s -> %s", FgVkStr(r),
        (unsigned long long)ctr, list.c_str(),
        pick < 0 ? "zadna pouzitelna, bez mereni"
                 : g_ptmDomain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR ? "QPC" : "mistni + kalibrace na QPC");
    if (pick < 0) g_ptmOn = false;
}

// Po vytvoreni swapchainu s VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT
static void FgPtmSetup() {
    FgPtmReset();
    if (!ptx.setQueueSize) return;
    const VkResult r = ptx.setQueueSize(g_dev, g_ptSc, kPtmQueue);
    if (r != VK_SUCCESS) {
        Log("prezentace: vkSetSwapchainPresentTimingQueueSizeEXT %s - bez mereni zobrazeni", FgVkStr(r));
        return;
    }
    g_ptmOn = true;
    if (!g_ptmRefreshMs && g_ps && g_ps->refreshMilli) g_ptmRefreshMs = 1e6 / (double)g_ps->refreshMilli;
    FgPtmReadProps();
    FgPtmReadDomains();
    if (g_ptmOn)
        Log("prezentace: mereni zobrazeni zapnuto (dotazy 0x%X, fronta %u, perioda obnovy %.4f ms)", g_ptmStages,
            kPtmQueue, g_ptmRefreshMs);
}

static LONGLONG FgPtmToQpc(uint64_t t) {
    if (g_ptmUnit == 1) return (LONGLONG)t;
    if (g_ptmUnit == 2) return (LONGLONG)((double)t * (double)g_qpf.QuadPart / 1e9);
    return (LONGLONG)((double)t * (double)g_qpf.QuadPart / 1e9) + g_ptmLocalOff;
}

static void FgPtmOne(const VkPastPresentationTimingEXT& pt) {
    PtmRec& rec = g_ptmRec[pt.presentId & 255];
    if (rec.id != pt.presentId) return;
    uint64_t fpo = 0, qend = 0;
    for (uint32_t i = 0; i < pt.presentStageCount; ++i) {
        if (pt.pPresentStages[i].stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) fpo = pt.pPresentStages[i].time;
        if (pt.pPresentStages[i].stage == VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT) qend = pt.pPresentStages[i].time;
    }
    if (!fpo) {
        std::lock_guard<std::mutex> lk(g_fgStatMx);
        ++g_ptSt.dNoTime;
        return;
    }
    if (g_ptmUnit == 0) {  // QPC domena: tiky nebo ns? (cas zobrazeni lezi par ms po volani presentu)
        const double ticks = FgMs(rec.call, (LONGLONG)fpo);
        const double ns = FgMs(rec.call, (LONGLONG)((double)fpo * (double)g_qpf.QuadPart / 1e9));
        g_ptmUnit = (ticks > -5.0 && ticks < 500.0) ? 1 : (ns > -5.0 && ns < 500.0) ? 2 : 0;
        Log("prezentace: present_timing cas QPC = %s (volani -> prvni pixel: tiky %.3f ms, ns %.3f ms)",
            g_ptmUnit == 1 ? "tiky QPC" : g_ptmUnit == 2 ? "nanosekundy" : "NEZNAMY - bez mereni", ticks, ns);
        if (!g_ptmUnit) {
            g_ptmOn = false;
            return;
        }
    }
    const LONGLONG F = FgPtmToQpc(fpo);
    const double lat = FgMs(rec.call, F);
    if (g_ptmLoggedFirst < 3) {
        ++g_ptmLoggedFirst;
        Log("prezentace: present_timing id %llu: volani -> konec fronty %.3f ms, -> prvni pixel %.3f ms",
            (unsigned long long)pt.presentId, qend ? FgMs(rec.call, FgPtmToQpc(qend)) : -1.0, lat);
    }
    std::lock_guard<std::mutex> lk(g_fgStatMx);
    ++g_ptSt.dN;
    if (lat > -1.0 && lat < 200.0) {
        g_ptSt.dLat.add(lat);
        if (g_ptmLatEma > 0.0 && g_ptmIvEma > 0.0 && lat > g_ptmLatEma + 0.5 * g_ptmIvEma) ++g_ptSt.dLate;
        g_ptmLatEma = g_ptmLatEma > 0.0 ? g_ptmLatEma + 0.02 * (lat - g_ptmLatEma) : lat;
    }
    if (rec.content && F > rec.content && FgMs(rec.content, F) < 200.0) g_ptSt.dCont.add(FgMs(rec.content, F));
    if (g_ptmLastId && pt.presentId == g_ptmLastId + 1 && F > g_ptmLastF) {
        const double iv = FgMs(g_ptmLastF, F);
        if (iv < 100.0) {
            g_ptSt.dIv.add(iv);
            if (g_ptmIvEma > 0.0) {
                if (iv < 0.5 * g_ptmIvEma) ++g_ptSt.dDbl;
                if (iv > 1.5 * g_ptmIvEma) ++g_ptSt.dLong;
            }
            g_ptmIvEma = g_ptmIvEma > 0.0 ? g_ptmIvEma + 0.02 * (iv - g_ptmIvEma) : iv;
            // perioda obnovy z intervalu, ktere jsou nasobkem periody (pevna frekvence); pri VRR se nemeni
            {
                const double R0 = g_ptmREst > 0.0 ? g_ptmREst : g_ptmRefreshMs;
                const double k0 = R0 > 1.0 ? floor(iv / R0 + 0.5) : 0.0;
                if (k0 >= 1.0 && k0 <= 4.0 && fabs(iv - k0 * R0) < 0.25) {
                    g_ptmREst = g_ptmREst > 0.0 ? g_ptmREst + 0.01 * (iv / k0 - g_ptmREst) : iv / k0;
                    g_vbPeriodMs = g_ptmREst;
                }
            }
            const double R = g_ptmREst > 0.0 ? g_ptmREst : g_ptmRefreshMs;
            if (R > 1.0) {
                const double k = floor(iv / R + 0.5);
                if (k >= 1.0) {
                    ++g_ptSt.dGridN;
                    if (fabs(iv - k * R) < 0.12) ++g_ptSt.dGrid;
                }
            }
            if (rec.content && g_ptmLastC && rec.content > g_ptmLastC) {
                const double j = iv - FgMs(g_ptmLastC, rec.content);
                if (fabs(j) < 50.0) {
                    g_ptSt.dJud.add(j);
                    g_ptSt.dJudAbs.add(fabs(j));
                }
            }
        }
    }
    g_ptmLastId = pt.presentId;
    g_ptmLastF = F;
    g_vbRefQ = F;
    if (g_ptmDumpN && !g_ptmDumpFull && GetTickCount64() >= g_ptmDumpFrom) {
        g_ptmDump.push_back({pt.presentId, rec.call, F, rec.content, rec.gen ? 1 : 0});
        if (g_ptmDump.size() >= g_ptmDumpN) g_ptmDumpFull = true;
    }
    g_ptmLastC = rec.content;
}

// Hotove vysledky bez blokovani (po kazdem presentu)
static void FgPtmPoll() {
    if (!g_ptmOn || !g_ptSc) return;
    if (g_ptmUnit == 3 && GetTickCount64() - g_ptmCalibTick > 2000) FgPtmCalib();
    for (int round = 0; round < 4; ++round) {
        VkPresentStageTimeEXT st[16][4] = {};
        VkPastPresentationTimingEXT tm[16];
        for (int i = 0; i < 16; ++i) {
            tm[i] = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT};
            tm[i].presentStageCount = 4;
            tm[i].pPresentStages = st[i];
        }
        VkPastPresentationTimingInfoEXT inf = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT};
        inf.swapchain = g_ptSc;
        VkPastPresentationTimingPropertiesEXT pp = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT};
        pp.presentationTimingCount = 16;
        pp.pPresentationTimings = tm;
        const VkResult r = ptx.past(g_dev, &inf, &pp);
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
            static unsigned s_err;
            if (++s_err <= 5) Log("prezentace: vkGetPastPresentationTimingEXT %s", FgVkStr(r));
            return;
        }
        if (pp.timingPropertiesCounter != g_ptmPropsCtr && g_ptmPropsCtr != ~0ull) FgPtmReadProps();
        if (pp.timeDomainsCounter != g_ptmDomCtr && g_ptmDomCtr != ~0ull) FgPtmReadDomains();
        for (uint32_t i = 0; i < pp.presentationTimingCount && i < 16; ++i) {
            if (tm[i].presentId > g_ptmDoneId) g_ptmDoneId = tm[i].presentId;
            if (tm[i].reportComplete) FgPtmOne(tm[i]);
        }
        if (r != VK_INCOMPLETE || !g_ptmOn) return;
    }
}

static bool FgPtWaitCmds(uint64_t ns) {
    for (int i = 0; i < 2; ++i)
        if (g_ptFenceUsed[i]) {
            if (vk.vkWaitForFences(g_dev, 1, &g_ptFence[i], VK_TRUE, ns) != VK_SUCCESS) return false;
            vk.vkResetFences(g_dev, 1, &g_ptFence[i]);
            g_ptFenceUsed[i] = false;
        }
    return true;
}

static void FgPtDestroySwapchain(const char* why) {
    if (!g_ptSc) return;
    if (FgPtWaitCmds(2000000000ull)) {
        std::lock_guard<std::mutex> lk(*g_presQMx);
        fvk.vkQueueWaitIdle(g_presQueue);
    }
    fvk.vkDestroySwapchainKHR(g_dev, g_ptSc, nullptr);
    g_ptSc = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < g_ptScCount; ++i)
        if (g_ptScDone[i]) fvk.vkDestroySemaphore(g_dev, g_ptScDone[i], nullptr);
    memset(g_ptScDone, 0, sizeof g_ptScDone);
    g_ptScCount = 0;
    FgPtmReset();
    MemoryBarrier();
    g_ps->swapchainAlive = 0;
    Log("prezentace: swapchain uvolnen (%s)", why);
}

static void FgPtDestroySurface() {
    if (g_ptSurf) fvk.vkDestroySurfaceKHR(g_inst, g_ptSurf, nullptr);
    g_ptSurf = VK_NULL_HANDLE;
}

static FgWndState FgGetWndState() {
    FgWndState s = {};
    s.style = GetWindowLongW(g_ptWnd, GWL_STYLE);
    s.ex = GetWindowLongW(g_ptWnd, GWL_EXSTYLE);
    GetWindowRect(g_ptWnd, &s.rc);
    s.vis = IsWindowVisible(g_ptWnd);
    return s;
}

static VkPresentModeKHR FgPickMode(uint32_t interval) {
    VkPresentModeKHR modes[8];
    uint32_t n = 8;
    fvk.vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, g_ptSurf, &n, modes);
    auto has = [&](VkPresentModeKHR m) {
        for (uint32_t i = 0; i < n; ++i)
            if (modes[i] == m) return true;
        return false;
    };
    if (interval == 0) {  // hra bez VSync: jako DXVK (IMMEDIATE, jinak MAILBOX)
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

// Swapchain na prezentacnim okne podle jeho aktualni velikosti. false = ted neprezentovat (okno 0x0, obsazene, chyba).
// Prevzato ze spiku S2 (src/helper/present.cpp) vcetne pojistek (zmena okna, pauza ve snimcich, 1.5 s po startu).
static bool FgPtEnsureSwapchain() {
    if (!IsWindow(g_ptWnd)) {  // hra konci, nebo proxy presla na zalohu a okno znicila: tise prestat prezentovat
        Log("prezentace: prezentacni okno %p uz neexistuje - konec prezentace", (void*)g_ptWnd);
        if (g_ps) {
            strncpy(g_ps->helperMsg, "prezentacni okno uz neexistuje", sizeof g_ps->helperMsg - 1);
            g_fgWndGone = true;
            MemoryBarrier();
            g_ps->helperState = BMSD_PST_FAILED;
        }
        g_ptFailed = true;
        return false;
    }
    if (!g_ptSurf) {
        VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = g_ptWnd;
        VkResult r = fvk.vkCreateWin32SurfaceKHR(g_inst, &sci, nullptr, &g_ptSurf);
        if (r != VK_SUCCESS) return FgPresFatal("vkCreateWin32SurfaceKHR %s", FgVkStr(r)), false;
        VkBool32 sup = VK_FALSE;
        fvk.vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, g_ptSurf, &sup);
        if (!sup) return FgPresFatal("fronta %u neumi prezentovat do okna hry", g_qf), false;
    }
    RECT rc = {};
    GetClientRect(g_ptWnd, &rc);
    // Cil je dcerine okno pres klientskou plochu hry: minimalizace je videt jen na korenovem okne
    const bool zero = IsIconic(g_ptWnd) || IsIconic(GetAncestor(g_ptWnd, GA_ROOT)) || rc.right <= 0 || rc.bottom <= 0;
    if (zero) {
        FgPtDestroySwapchain("okno minimalizovane / 0x0");
        FgSetPresState(BMSD_PST_NOWINDOW);
        return false;
    }
    const uint32_t interval = g_ps->presentInterval;
    if (g_ptSc && (g_ptNeedRecreate || rc.right != g_ptScClient.cx || rc.bottom != g_ptScClient.cy ||
                   interval != g_ptScInterval)) {
        FgPtDestroySwapchain(g_ptNeedRecreate ? "OUT_OF_DATE / SUBOPTIMAL" : "zmena velikosti okna nebo VSync");
        ++g_ptSt.recreate;
    }
    if (g_ptSc) {
        const FgWndState ws = FgGetWndState();
        if (ws.style != g_ptScWnd.style || ws.ex != g_ptScWnd.ex || ws.vis != g_ptScWnd.vis ||
            memcmp(&ws.rc, &g_ptScWnd.rc, sizeof ws.rc)) {
            Log("prezentace: okno se zmenilo (styl %08lX -> %08lX, %ld,%ld-%ld,%ld -> %ld,%ld-%ld,%ld, viditelne %d -> "
                "%d) - swapchain znovu",
                g_ptScWnd.style, ws.style, g_ptScWnd.rc.left, g_ptScWnd.rc.top, g_ptScWnd.rc.right,
                g_ptScWnd.rc.bottom, ws.rc.left, ws.rc.top, ws.rc.right, ws.rc.bottom, g_ptScWnd.vis, ws.vis);
            FgPtDestroySwapchain("zmena okna");
            ++g_ptSt.recreate;
        } else if (g_ptFrameGap) {  // hra dele nekreslila (nacitani mapy): swapchain znovu, kdyby byl "mrtvy"
            Log("prezentace: snimky %.1f s nechodily (nacitani?) - swapchain znovu", g_ptFrameGap / 1000.0);
            FgPtDestroySwapchain("po pauze ve snimcich");
            ++g_ptSt.recreate;
        } else if (!g_ptResyncDone && g_ptFirstPresentTick && GetTickCount64() - g_ptFirstPresentTick > kFgResyncMs) {
            g_ptResyncDone = true;
            Log("prezentace: pojistka - swapchain znovu %llu ms po prvnim presentu", kFgResyncMs);
            FgPtDestroySwapchain("pojistka po startu");
            ++g_ptSt.recreate;
        }
    }
    g_ptNeedRecreate = false;
    g_ptFrameGap = 0;
    if (g_ptSc) return true;
    const ULONGLONG now = GetTickCount64();
    if (now < g_ptInUseRetry) return false;

    VkSurfaceCapabilitiesKHR caps = {};
    VkResult r = fvk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_ptSurf, &caps);
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        FgPtDestroySurface();
        return false;
    }
    if (r != VK_SUCCESS) return FgPresFatal("surface caps %s", FgVkStr(r)), false;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) ext = {(uint32_t)rc.right, (uint32_t)rc.bottom};
    if (!ext.width || !ext.height) {
        FgSetPresState(BMSD_PST_NOWINDOW);
        return false;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return FgPresFatal("swapchain neumi TRANSFER_DST"), false;
    VkSurfaceFormatKHR fmts[32];
    uint32_t nf = 32;
    fvk.vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_ptSurf, &nf, fmts);
    // Hodnoty backbufferu jsou uz v sRGB: format UNORM (blit bez prevodu) v barevnem prostoru SRGB_NONLINEAR. Poradi:
    // BGRA8, RGBA8 / ABGR8 (blit prohodi kanaly), 10bit UNORM. Nikdy HDR10 / scRGB prostor (vybledle nebo prepalene
    // barvy); formaty _SRGB jen jako posledni moznost (blit by sRGB kodoval podruhe).
    static const VkFormat kPref[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                     VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    VkSurfaceFormatKHR sf = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (VkFormat want : kPref)
        for (uint32_t i = 0; i < nf && sf.format == VK_FORMAT_UNDEFINED; ++i)
            if (fmts[i].format == want && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) sf = fmts[i];
    for (uint32_t i = 0; i < nf && sf.format == VK_FORMAT_UNDEFINED; ++i)
        if (fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) sf = fmts[i];
    if (nf == 0 && sf.format == VK_FORMAT_UNDEFINED) sf.format = VK_FORMAT_B8G8R8A8_UNORM;  // (jako drive)
    if (sf.format == VK_FORMAT_UNDEFINED)
        return FgPresFatal("swapchain nenabizi SDR format (SRGB_NONLINEAR), FG nejde"), false;
    if (sf.format != VK_FORMAT_B8G8R8A8_UNORM && !g_ptFmtLogged) {
        g_ptFmtLogged = true;
        Log("prezentace: swapchain bez B8G8R8A8_UNORM - pouzit VkFormat %d (SRGB_NONLINEAR)", (int)sf.format);
    }
    const VkPresentModeKHR mode = FgPickMode(interval);
    uint32_t minImg = caps.minImageCount + 1 < 3 ? 3 : caps.minImageCount + 1;
    if (caps.maxImageCount && minImg > caps.maxImageCount) minImg = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_ptSurf;
    ci.minImageCount = minImg;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    // [fg] PresentTiming=1: swapchain se zpetnou vazbou casu zobrazeni (jen kdyz ji povrch podporuje)
    if (g_ptmFuncs && !g_ptmBroken) {
        g_ptmStages = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
        if (ptx.surfCaps2) {
            VkPresentTimingSurfaceCapabilitiesEXT ptc = {VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
            VkSurfaceCapabilities2KHR c2 = {VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, &ptc};
            VkPhysicalDeviceSurfaceInfo2KHR si2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, nullptr, g_ptSurf};
            const VkResult cr = ptx.surfCaps2(g_pd, &si2, &c2);
            Log("prezentace: povrch present_timing (%s): podporovano %u, absolutni %u, relativni %u, dotazy 0x%X",
                FgVkStr(cr), ptc.presentTimingSupported, ptc.presentAtAbsoluteTimeSupported,
                ptc.presentAtRelativeTimeSupported, ptc.presentStageQueries);
            if (cr == VK_SUCCESS) g_ptmStages &= ptc.presentStageQueries;
            if (cr == VK_SUCCESS && (!ptc.presentTimingSupported ||
                                     !(g_ptmStages & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT))) {
                Log("prezentace: povrch neumi cas prvniho pixelu - bez mereni zobrazeni");
                g_ptmBroken = true;
            }
        }
        if (!g_ptmBroken) ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
    }
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    r = fvk.vkCreateSwapchainKHR(g_dev, &ci, nullptr, &sc);
    if (r != VK_SUCCESS && (ci.flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) &&
        r != VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
        if (sc) Log("prezentace: vkCreateSwapchainKHR %s vratil nenulovy handle (chyba ovladace) - zahazuji", FgVkStr(r));
        Log("prezentace: swapchain s present timing %s - znovu bez mereni zobrazeni", FgVkStr(r));
        g_ptmBroken = true;
        ci.flags = 0;
        sc = VK_NULL_HANDLE;
        r = fvk.vkCreateSwapchainKHR(g_dev, &ci, nullptr, &sc);
    }
    if (r != VK_SUCCESS) {
        // Chyba ovladace: pri chybe muze byt sc nenulovy - nesmi se znicit (access violation), jen zahodit.
        if (sc) Log("prezentace: vkCreateSwapchainKHR %s vratil nenulovy handle (chyba ovladace) - zahazuji", FgVkStr(r));
        if (r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
            if (!g_ptInUseLogged) {
                g_ptInUseLogged = true;
                g_ptInUseSince = now;
                Log("prezentace: okno je pro Vulkan obsazene - zkousim kazdych 250 ms");
            }
            g_ptInUseRetry = now + 250;
            FgSetPresState(BMSD_PST_INUSE);
            return false;
        }
        return FgPresFatal("vkCreateSwapchainKHR %s", FgVkStr(r)), false;
    }
    if (g_ptInUseLogged) Log("prezentace: okno volne po %.1f s", (now - g_ptInUseSince) / 1000.0);
    g_ptInUseLogged = false;
    g_ptSc = sc;
    g_ptScWnd = FgGetWndState();
    MemoryBarrier();
    g_ps->swapchainAlive = 1;
    g_ptScCount = 16;
    fvk.vkGetSwapchainImagesKHR(g_dev, g_ptSc, &g_ptScCount, g_ptScImg);
    VkSemaphoreCreateInfo bsi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < g_ptScCount; ++i)
        if (vk.vkCreateSemaphore(g_dev, &bsi, nullptr, &g_ptScDone[i]) != VK_SUCCESS)
            return FgPresFatal("semafor swapchainu"), false;
    g_ptScExt = ext;
    g_ptScClient = {rc.right, rc.bottom};
    g_ptScMode = mode;
    g_ptScInterval = interval;
    g_ps->presentMode = (uint32_t)mode;
    g_ps->swapW = ext.width;
    g_ps->swapH = ext.height;
    Log("prezentace: swapchain %ux%u, %s, VkFormat %d, %u obrazku (min %u), okno %p (klientska plocha %ldx%ld, DPI %u)",
        ext.width, ext.height, FgModeStr(mode), (int)sf.format, g_ptScCount, caps.minImageCount, (void*)g_ptWnd,
        rc.right, rc.bottom, WindowDpi(g_ptWnd));
    if (ext.width != (uint32_t)rc.right || ext.height != (uint32_t)rc.bottom)
        Log("prezentace: POZOR swapchain %ux%u != klientska plocha %ldx%ld (DPI okna %u)", ext.width, ext.height,
            rc.right, rc.bottom, WindowDpi(g_ptWnd));
    if (ci.flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) FgPtmSetup();
    return true;
}

// ---- Zobrazeni v procesu hry ([fg] GamePresent=1, BmsdPresentShared::gpEnable, BMSD_PRESENT_VERSION 7) ----
// Ovladac zapina VRR jen procesu s oknem v popredi (docs/vrr-diagnostika-vysledek.md): helper proto do swapchainu
// neprezentuje, ale v case presentu obraz zkopiruje do sdileneho obrazu a proxy ho v procesu hry zobrazi.
static bool g_gpOn;                    // tento beh presentu: zobrazuje proxy (gpEnable pri startu / znovuzapnuti)
static bool g_gpEarly;                 // gpEnable bit 1: zverejnit hned po kopii, gpPostQpc = cilovy cas presentu
static LONGLONG g_gpWantQ;             // casne zverejneni: cilovy cas dalsiho presentu podle rozvrhu (0 = hned / strop)
static HANDLE g_gpGame;                // proces hry (PROCESS_DUP_HANDLE)
static HANDLE g_gpEv;                  // Local\bms_dlss_<pid>_gpframe
static OwnImg g_gpImg[BMSD_GP_IMAGES];
static bool g_gpFresh[BMSD_GP_IMAGES];  // obraz jeste nebyl u proxy (prvni bariera z UNDEFINED, bez prevodu vlastnictvi)
static uint64_t g_gpSlotSeq[BMSD_GP_IMAGES];
static VkSemaphore g_gpSemReady, g_gpSemDone;
static int32_t g_gpGen;
static uint64_t g_gpSeq;
static bool g_gpFailLogged;
static PFN_vkGetMemoryWin32HandleKHR g_gpGetMemH;
static PFN_vkGetSemaphoreWin32HandleKHR g_gpGetSemH;
static const VkFormat kGpFmt = VK_FORMAT_B8G8R8A8_UNORM;
static void FgPtSleepUntil(LONGLONG target, bool coarse = false);

static uint64_t GpDup(HANDLE h) {
    HANDLE remote = nullptr;
    const BOOL ok = DuplicateHandle(GetCurrentProcess(), h, g_gpGame, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(h);
    return ok ? (uint64_t)(uintptr_t)remote : 0;
}
static void GpCloseRemote(uint64_t h) {
    if (h && g_gpGame)
        DuplicateHandle(g_gpGame, (HANDLE)(uintptr_t)h, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
}

// Uvolnit sadu obrazu a semaforu. Proxy drzi vlastni import (pamet zustava platna, dokud ho neuvolni); handle
// generace, kterou proxy jeste neimportovala, se zavrou v procesu hry. Prace helperu nad obrazy musi byt hotova.
static void GpDestroy() {
    if (g_ps && g_gpGen && g_ps->gpProxyGeneration != g_gpGen) {
        for (int i = 0; i < BMSD_GP_IMAGES; ++i) GpCloseRemote(g_ps->gpImg[i].handle);
        GpCloseRemote(g_ps->gpSemReadyHandle);
        GpCloseRemote(g_ps->gpSemDoneHandle);
    }
    if (g_ps) {
        for (int i = 0; i < BMSD_GP_IMAGES; ++i) g_ps->gpImg[i] = BmsdImage();
        g_ps->gpSemReadyHandle = g_ps->gpSemDoneHandle = 0;
    }
    for (int i = 0; i < BMSD_GP_IMAGES; ++i) {
        if (g_gpImg[i].img) vk.vkDestroyImage(g_dev, g_gpImg[i].img, nullptr);
        if (g_gpImg[i].mem) vk.vkFreeMemory(g_dev, g_gpImg[i].mem, nullptr);
        g_gpImg[i] = OwnImg();
        g_gpSlotSeq[i] = 0;
    }
    if (g_gpSemReady) fvk.vkDestroySemaphore(g_dev, g_gpSemReady, nullptr);
    if (g_gpSemDone) fvk.vkDestroySemaphore(g_dev, g_gpSemDone, nullptr);
    g_gpSemReady = g_gpSemDone = VK_NULL_HANDLE;
}

static bool GpFail(const char* fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    FgPresFatal("zobrazeni v procesu hry: %s", buf);
    return false;
}

// Sada obrazu w x h (BGRA8) a semaforu pro proxy: export, zdvojeni handle do procesu hry, ++gpGeneration.
static bool GpCreate(uint32_t w, uint32_t h) {
    if (!g_gpGame) {
        g_gpGame = OpenProcess(PROCESS_DUP_HANDLE, FALSE, g_fgPid);
        if (!g_gpGame) return GpFail("proces hry nejde otevrit (chyba %lu)", GetLastError());
    }
    if (!g_gpEv) {
        wchar_t name[128];
        swprintf(name, 128, L"Local\\bms_dlss_%lu_gpframe", g_fgPid);
        g_gpEv = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
        if (!g_gpEv) return GpFail("event proxy nejde otevrit (chyba %lu)", GetLastError());
    }
    if (!g_gpGetMemH)
        g_gpGetMemH = (PFN_vkGetMemoryWin32HandleKHR)vk.vkGetDeviceProcAddr(g_dev, "vkGetMemoryWin32HandleKHR");
    if (!g_gpGetSemH)
        g_gpGetSemH = (PFN_vkGetSemaphoreWin32HandleKHR)vk.vkGetDeviceProcAddr(g_dev, "vkGetSemaphoreWin32HandleKHR");
    if (!g_gpGetMemH || !g_gpGetSemH) return GpFail("chybi export handle (VK_KHR_external_*_win32)");
    if (!FgPtWaitCmds(2000000000ull)) return GpFail("GPU nedobehla pred novymi obrazy");
    GpDestroy();
    for (VkSemaphore* sp : {&g_gpSemReady, &g_gpSemDone}) {
        VkExportSemaphoreCreateInfo esi = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr,
                                           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
        VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &esi, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
        if (vk.vkCreateSemaphore(g_dev, &sci, nullptr, sp) != VK_SUCCESS) return GpFail("timeline semafor");
    }
    auto expSem = [](VkSemaphore sm) -> uint64_t {
        VkSemaphoreGetWin32HandleInfoKHR hi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr, sm,
                                               VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
        HANDLE hd = nullptr;
        if (g_gpGetSemH(g_dev, &hi, &hd) != VK_SUCCESS || !hd) return 0;
        return GpDup(hd);
    };
    const uint64_t hr = expSem(g_gpSemReady), hdn = expSem(g_gpSemDone);
    g_ps->gpSemReadyHandle = hr;
    g_ps->gpSemDoneHandle = hdn;
    if (!hr || !hdn) return GpFail("export semaforu");
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int i = 0; i < BMSD_GP_IMAGES; ++i) {
        OwnImg& o = g_gpImg[i];
        VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                               (VkExternalMemoryHandleTypeFlags)ht};
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kGpFmt;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk.vkCreateImage(g_dev, &ci, nullptr, &o.img) != VK_SUCCESS) return GpFail("vkCreateImage %ux%u", w, h);
        VkMemoryRequirements req = {};
        vk.vkGetImageMemoryRequirements(g_dev, o.img, &req);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, o.img};
        VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded,
                                          (VkExternalMemoryHandleTypeFlags)ht};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, req.size,
                                   DeviceLocalType(req.memoryTypeBits)};
        if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(g_dev, &ai, nullptr, &o.mem) != VK_SUCCESS)
            return GpFail("pamet obrazu %ux%u", w, h);
        if (vk.vkBindImageMemory(g_dev, o.img, o.mem, 0) != VK_SUCCESS) return GpFail("bind obrazu");
        VkMemoryGetWin32HandleInfoKHR hi = {VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, o.mem, ht};
        HANDLE hd = nullptr;
        if (g_gpGetMemH(g_dev, &hi, &hd) != VK_SUCCESS || !hd) return GpFail("export pameti obrazu");
        BmsdImage& bi = g_ps->gpImg[i];
        bi.handle = GpDup(hd);
        bi.allocSize = req.size;
        bi.format = (int32_t)kGpFmt;
        bi.usage = usage;
        bi.width = w;
        bi.height = h;
        if (!bi.handle) return GpFail("zdvojeni handle do procesu hry (chyba %lu)", GetLastError());
        o.w = w;
        o.h = h;
        g_gpFresh[i] = true;
        g_gpSlotSeq[i] = 0;
    }
    MemoryBarrier();
    g_ps->gpGeneration = ++g_gpGen;
    MemoryBarrier();
    SetEvent(g_gpEv);  // proxy importuje hned (ne az s prvnim snimkem)
    Log("prezentace: zobrazeni v procesu hry - obrazy %ux%u x%d, generace %d", w, h, BMSD_GP_IMAGES, g_gpGen);
    return true;
}

// Obraz (GENERAL) zverejnit proxy k zobrazeni (misto vlastniho swapchainu). Vraci true, kdyz byl predan.
static bool GpPresentImage(VkImage src, uint32_t sw, uint32_t sh_, bool generated, LONGLONG content) {
    if (g_ptFailed) return false;
    if (!IsWindow(g_ptWnd)) {  // hra konci, nebo proxy presla na zalohu a okno znicila
        Log("prezentace: prezentacni okno %p uz neexistuje - konec prezentace", (void*)g_ptWnd);
        if (g_ps) {
            strncpy(g_ps->helperMsg, "prezentacni okno uz neexistuje", sizeof g_ps->helperMsg - 1);
            g_fgWndGone = true;
            MemoryBarrier();
            g_ps->helperState = BMSD_PST_FAILED;
        }
        g_ptFailed = true;
        return false;
    }
    RECT rc = {};
    GetClientRect(g_ptWnd, &rc);
    if (IsIconic(g_ptWnd) || IsIconic(GetAncestor(g_ptWnd, GA_ROOT)) || rc.right <= 0 || rc.bottom <= 0) {
        FgSetPresState(BMSD_PST_NOWINDOW);
        return false;
    }
    // Velikost obrazu = skutecny snimek (generovane maji stejnou; jina velikost se jen prepocita blitem)
    if (!g_gpImg[0].img || (!generated && (g_gpImg[0].w != sw || g_gpImg[0].h != sh_))) {
        if (!GpCreate(sw, sh_)) return false;
    }
    const uint64_t seq = g_gpSeq + 1;
    const int k = (int)(seq % BMSD_GP_IMAGES);
    if (g_gpSlotSeq[k]) {  // proxy musi obraz z minuleho kola precist
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &g_gpSemDone, &g_gpSlotSeq[k]};
        const VkResult wr = fvk.vkWaitSemaphores(g_dev, &wi, 60000000ull);
        if (wr != VK_SUCCESS) {
            std::lock_guard<std::mutex> lk(g_fgStatMx);
            ++g_ptSt.gpBusy;
            if (!g_gpFailLogged) {
                g_gpFailLogged = true;
                Log("prezentace: proxy obraz %llu do 60 ms neprecetla (%s) - snimek vynechan (dal jen pocitano)",
                    (unsigned long long)g_gpSlotSeq[k], FgVkStr(wr));
            }
            return false;
        }
    }
    const int c = g_ptCmdIdx;
    g_ptCmdIdx ^= 1;
    if (g_ptFenceUsed[c]) {
        if (vk.vkWaitForFences(g_dev, 1, &g_ptFence[c], VK_TRUE, 2000000000ull) != VK_SUCCESS)
            return FgPresFatal("GPU nedobehla (kopie pro zobrazeni)"), false;
        vk.vkResetFences(g_dev, 1, &g_ptFence[c]);
        g_ptFenceUsed[c] = false;
    }
    VkCommandBuffer cmd = g_ptCmd[c];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkResetCommandBuffer(cmd, 0);
    vk.vkBeginCommandBuffer(cmd, &bi);
    const VkImageAspectFlags COLOR = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL, DST = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const uint32_t EXT = VK_QUEUE_FAMILY_EXTERNAL;
    VkImage dst = g_gpImg[k].img;
    VkImageMemoryBarrier pre[2] = {
        IB(src, COLOR, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, GEN, GEN),
        g_gpFresh[k] ? IB(dst, COLOR, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, DST)
                     : IB(dst, COLOR, 0, VK_ACCESS_TRANSFER_WRITE_BIT, GEN, DST, EXT, g_qf)};  // prevzeti od proxy
    PB(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 2, pre);
    VkImageBlit b = {};
    b.srcSubresource = {COLOR, 0, 0, 1};
    b.dstSubresource = {COLOR, 0, 0, 1};
    b.srcOffsets[1] = {(int32_t)sw, (int32_t)sh_, 1};
    b.dstOffsets[1] = {(int32_t)g_gpImg[k].w, (int32_t)g_gpImg[k].h, 1};
    const bool same = sw == g_gpImg[k].w && sh_ == g_gpImg[k].h;
    vk.vkCmdBlitImage(cmd, src, GEN, dst, DST, 1, &b, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    VkImageMemoryBarrier post = IB(dst, COLOR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, DST, GEN, g_qf, EXT);  // predat proxy
    PB(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1, &post);
    vk.vkEndCommandBuffer(cmd);
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.signalSemaphoreValueCount = 1;
    ti.pSignalSemaphoreValues = &seq;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ti};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_gpSemReady;
    {
        std::lock_guard<std::mutex> lk(*g_presQMx);
        const VkResult r = vk.vkQueueSubmit(g_presQueue, 1, &si, g_ptFence[c]);
        if (r != VK_SUCCESS) return FgPresFatal("vkQueueSubmit (kopie pro zobrazeni) %s", FgVkStr(r)), false;
    }
    g_ptFenceUsed[c] = true;
    g_gpFresh[k] = false;
    g_gpSlotSeq[k] = seq;
    g_gpSeq = seq;
    // Strop VRR: dva snimky nikdy blize nez nejkratsi interval (jako u vlastniho swapchainu); kopie bezi na GPU uz ted.
    // Casne zverejneni (g_gpEarly): proxy dostane obraz hned a s nim cilovy cas presentu - kopii do swapchainu udela
    // hned (ma na ni celou dobu cekani) a v cilovy cas zavola jen present.
    bool minIvWaited = false;
    const LONGLONG minIv = g_ptCapOverrideQ ? g_ptCapOverrideQ : g_ptMinIvQ;
    LONGLONG target = 0;
    if (minIv > 0 && g_ptLastCallQpc && FgQpc() < g_ptLastCallQpc + minIv) target = g_ptLastCallQpc + minIv;
    if (g_gpWantQ > target && FgQpc() < g_gpWantQ) target = g_gpWantQ;  // rozvrh (FgPtWaitFor) pri casnem zverejneni
    g_gpWantQ = 0;
    auto publish = [&](LONGLONG t, uint32_t fl) {
        g_ps->gpFlags = fl;
        g_ps->gpPostQpc = (uint64_t)t;
        InterlockedExchange64((volatile LONG64*)&g_ps->gpPub,
                              (LONG64)((((uint64_t)(uint32_t)g_gpGen & 0xFFFFull) << 48) | (seq & 0xFFFFFFFFFFFFull)));
        SetEvent(g_gpEv);
    };
    if (g_gpEarly) publish(target ? target : FgQpc(), (generated ? 1u : 0u) | 2u);
    if (target) {
        FgPtSleepUntil(target, g_gpEarly);  // casne zverejneni: presny cas hlida proxy, tady jen hrube (setri CPU)
        minIvWaited = true;
    }
    const LONGLONG p0 = FgQpc();
    if (!g_gpEarly) publish(p0, generated ? 1u : 0u);
    const LONGLONG p1 = FgQpc();
    FgSetPresState(BMSD_PST_PRESENTING);
    {
        std::lock_guard<std::mutex> lk(g_fgStatMx);
        g_ptSt.pres.add(FgMs(p0, p1));
        if (g_ptLastPresQpc) {
            const double iv = FgMs(g_ptLastPresQpc, p1);
            if (iv < 200.0) g_ptSt.interval.add(iv);
        }
        if (g_ptLastCallQpc) {
            const double ivc = FgMs(g_ptLastCallQpc, p0);
            if (ivc < 200.0) g_ptSt.ivCall.add(ivc);
            if (minIv > 0 && ivc < FgMs(0, minIv) - 0.05) ++g_ptSt.ivShort;
        }
        if (minIvWaited) ++g_ptSt.minIvWait;
        if (content && p0 > content && FgMs(content, p0) < 200.0) g_ptSt.lat.add(FgMs(content, p0));
        if (generated) ++g_ptSt.gen;
        else ++g_ptSt.real;
    }
    g_ptLastPresQpc = p1;
    g_ptLastCallQpc = g_gpEarly && target > p0 ? target : p0;  // rozvrh od ciloveho casu (hrube cekani konci driv)
    if (!g_ptFirstPresentTick) g_ptFirstPresentTick = GetTickCount64();
    static uint64_t s_real, s_gen;
    if (generated) g_ps->presCountGen = ++s_gen;
    else g_ps->presCountReal = ++s_real;
    return true;
}

// Jeden obraz (interp nebo real, GENERAL) do swapchainu a present. Vraci true, kdyz byl snimek predan k zobrazeni.
static void FgPtSleepUntil(LONGLONG target, bool coarse);
static bool FgPtPresentImage(VkImage src, uint32_t sw, uint32_t sh_, bool generated, LONGLONG content = 0) {
    if (g_gpOn) return GpPresentImage(src, sw, sh_, generated, content);
    if (g_ptFailed || !FgPtEnsureSwapchain()) return false;
    const int c = g_ptCmdIdx;
    g_ptCmdIdx ^= 1;
    if (g_ptFenceUsed[c]) {
        if (vk.vkWaitForFences(g_dev, 1, &g_ptFence[c], VK_TRUE, 2000000000ull) != VK_SUCCESS)
            return FgPresFatal("GPU nedobehla (blit presentu)"), false;
        vk.vkResetFences(g_dev, 1, &g_ptFence[c]);
        g_ptFenceUsed[c] = false;
    }
    uint32_t idx = 0;
    VkResult r = fvk.vkAcquireNextImageKHR(g_dev, g_ptSc, 100000000ull, g_ptAcq[c], VK_NULL_HANDLE, &idx);
    if (r == VK_SUBOPTIMAL_KHR) {
        g_ptNeedRecreate = true;
    } else if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_ptNeedRecreate = true;
        return false;
    } else if (r == VK_ERROR_SURFACE_LOST_KHR) {
        FgPtDestroySwapchain("SURFACE_LOST");
        FgPtDestroySurface();
        return false;
    } else if (r == VK_TIMEOUT || r == VK_NOT_READY) {
        return false;
    } else if (r != VK_SUCCESS) {
        return FgPresFatal("vkAcquireNextImageKHR %s", FgVkStr(r)), false;
    }
    FgSetPresState(BMSD_PST_PRESENTING);
    VkCommandBuffer cmd = g_ptCmd[c];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkResetCommandBuffer(cmd, 0);
    vk.vkBeginCommandBuffer(cmd, &bi);
    const VkImageAspectFlags COLOR = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL;
    VkImageMemoryBarrier pre[2] = {
        IB(src, COLOR, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, GEN, GEN),
        IB(g_ptScImg[idx], COLOR, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
    PB(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 2, pre);
    VkImageBlit b = {};
    b.srcSubresource = {COLOR, 0, 0, 1};
    b.dstSubresource = {COLOR, 0, 0, 1};
    b.srcOffsets[1] = {(int32_t)sw, (int32_t)sh_, 1};
    b.dstOffsets[1] = {(int32_t)g_ptScExt.width, (int32_t)g_ptScExt.height, 1};
    const bool same = sw == g_ptScExt.width && sh_ == g_ptScExt.height;
    vk.vkCmdBlitImage(cmd, src, GEN, g_ptScImg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                      same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    VkImageMemoryBarrier post = IB(g_ptScImg[idx], COLOR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    PB(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1, &post);
    vk.vkEndCommandBuffer(cmd);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_ptAcq[c];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_ptScDone[idx];
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_ptScDone[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_ptSc;
    pi.pImageIndices = &idx;
    // [fg] PresentTiming=1: id presentu + dotaz na cas prvniho pixelu (dokud fronta vysledku neni skoro plna)
    VkPresentId2KHR ptmPid = {VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR};
    VkPresentTimingInfoEXT ptmTi = {VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT};
    VkPresentTimingsInfoEXT ptmTis = {VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT};
    uint64_t ptmId = 0;
    if (g_ptmOn) {
        ptmId = ++g_ptmNextId;
        ptmPid.swapchainCount = 1;
        ptmPid.pPresentIds = &ptmId;
        if (ptmId - g_ptmDoneId < kPtmQueue - 8) {
            ptmTi.timeDomainId = g_ptmDomainId;
            ptmTi.presentStageQueries = g_ptmStages;
            ptmTis.pNext = &ptmPid;
            ptmTis.swapchainCount = 1;
            ptmTis.pTimingInfos = &ptmTi;
            pi.pNext = &ptmTis;
            g_ptmRec[ptmId & 255] = {ptmId, 0, content, generated};
        } else {
            pi.pNext = &ptmPid;
            g_ptmRec[ptmId & 255].id = 0;
            std::lock_guard<std::mutex> lk(g_fgStatMx);
            ++g_ptSt.dSkipReq;
        }
    }
    // Strop VRR: dva snimky nikdy blize nez nejkratsi interval (1000 / (f - f^2/3600) ms) - ani generovane, ani po
    // pozdnim snimku. Blit je kratky (~0.1 ms), ceka se tesne pred odeslanim.
    bool minIvWaited = false;
    const LONGLONG minIv = g_ptCapOverrideQ ? g_ptCapOverrideQ : g_ptMinIvQ;
    if (minIv > 0 && g_ptLastCallQpc && FgQpc() < g_ptLastCallQpc + minIv) {
        FgPtSleepUntil(g_ptLastCallQpc + minIv);
        minIvWaited = true;
    }
    LONGLONG p0, p1;
    {
        std::lock_guard<std::mutex> lk(*g_presQMx);
        r = vk.vkQueueSubmit(g_presQueue, 1, &si, g_ptFence[c]);
        if (r != VK_SUCCESS) return FgPresFatal("vkQueueSubmit (blit presentu) %s", FgVkStr(r)), false;
        g_ptFenceUsed[c] = true;
        p0 = FgQpc();
        r = fvk.vkQueuePresentKHR(g_presQueue, &pi);
        p1 = FgQpc();
    }
    {
        std::lock_guard<std::mutex> lk(g_fgStatMx);
        g_ptSt.pres.add(FgMs(p0, p1));
        if (g_ptLastPresQpc) {
            const double iv = FgMs(g_ptLastPresQpc, p1);
            if (iv < 200.0) g_ptSt.interval.add(iv);
        }
        if (g_ptLastCallQpc) {
            const double ivc = FgMs(g_ptLastCallQpc, p0);
            if (ivc < 200.0) g_ptSt.ivCall.add(ivc);
            if (minIv > 0 && ivc < FgMs(0, minIv) - 0.05) ++g_ptSt.ivShort;
        }
        if (minIvWaited) ++g_ptSt.minIvWait;
        if (content && p0 > content && FgMs(content, p0) < 200.0) g_ptSt.lat.add(FgMs(content, p0));
    }
    g_ptLastPresQpc = p1;
    g_ptLastCallQpc = p0;
    if (ptmId) {
        if (g_ptmRec[ptmId & 255].id == ptmId) g_ptmRec[ptmId & 255].call = p0;
        FgPtmPoll();
    }
    if (r == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT) {  // jen mereni: vypnout a swapchain znovu bez nej
        Log("prezentace: fronta vysledku present_timing plna - mereni zobrazeni vypnuto");
        g_ptmBroken = true;
        g_ptmOn = false;
        g_ptNeedRecreate = true;
        return false;
    }
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        if (!g_ptFirstPresentTick) g_ptFirstPresentTick = GetTickCount64();
        if (r == VK_SUBOPTIMAL_KHR) g_ptNeedRecreate = true;
        // citace pro ukazatel FPS v proxy (jen toto vlakno zapisuje; 64bitovy zapis je na x64 atomicky)
        static uint64_t s_real, s_gen;
        if (g_ps) {
            if (generated) g_ps->presCountGen = ++s_gen;
            else g_ps->presCountReal = ++s_real;
        }
        std::lock_guard<std::mutex> lk(g_fgStatMx);
        if (generated) ++g_ptSt.gen;
        else ++g_ptSt.real;
        return true;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_ptNeedRecreate = true;
    } else if (r == VK_ERROR_SURFACE_LOST_KHR) {
        FgPtDestroySwapchain("SURFACE_LOST");
        FgPtDestroySurface();
    } else {
        FgPresFatal("vkQueuePresentKHR %s", FgVkStr(r));
    }
    return false;
}

// Presne cekani do casu QPC (waitable timer + poslednich ~0.5 ms aktivne; spike tools/spike_timer: sd intervalu
// 0.01-0.03 ms na 60-360 Hz proti 0.06-0.09 ms s 0.35 ms, cena ~4 % jadra pri 165 Hz). Nejdele 100 ms.
// coarse: bez dotoceni aktivni smyckou (posledni ~0,6 ms) - pri casnem zverejneni ceka na presny cas proxy
static void FgPtSleepUntil(LONGLONG target, bool coarse) {
    for (;;) {
        const LONGLONG now = FgQpc();
        if (now >= target) return;
        double ms = FgMs(now, target);
        if (ms > 100.0) return;
        if (coarse && ms <= 0.65) return;
        if (ms > 0.65 && g_ptTimer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((ms - 0.5) * 10000.0);
            if (SetWaitableTimer(g_ptTimer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(g_ptTimer, 100);
                continue;
            }
        } else if (ms > 2.0) {
            Sleep(1);
            continue;
        }
        YieldProcessor();
    }
}

// Rozvrh: dalsi present nejdriv v case t. Pri casnem zverejneni (zobrazuje proxy) se necheka tady, ale az po kopii a
// zverejneni obrazu v GpPresentImage - proxy tak ma na kopii do swapchainu cely cas cekani.
static void FgPtWaitFor(LONGLONG t) {
    if (g_gpOn && g_gpEarly) {
        if (t > g_gpWantQ) g_gpWantQ = t;
    } else {
        FgPtSleepUntil(t);
    }
}

static void FgPtPresentItem(const FgItem& it, bool behind) {  // behind: s Catchup=2 se urci az tady
    const ULONGLONG nowTick = GetTickCount64();
    g_ptFrameGap = g_ptLastFrameTick && nowTick - g_ptLastFrameTick > 1000 ? nowTick - g_ptLastFrameTick : 0;
    g_ptLastFrameTick = nowTick;
    // Prace hlavniho vlakna pro tento snimek (kopie / vyhodnoceni generatoru) musi byt na GPU hotova: obrazy se pak
    // ctou z jine fronty bez barier a pacing zacina az od skutecne dostupnosti snimku.
    // Byla prace uz hotova pred vyzvednutim polozky (vlakno bylo zaneprazdnene drivejsi polozkou - rozvrh A)? Pak cas
    // "hotovo" neni znamy (jen horni mez) a do odhadu pro rozvrh se nepocita.
    uint64_t evNow = 0;
    fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemEval, &evNow);
    const bool readyBefore = evNow >= it.seq;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &g_fgSemEval, &it.seq};
    const VkResult wr = fvk.vkWaitSemaphores(g_dev, &wi, 250000000ull);
    if (wr != VK_SUCCESS) {
        {
            std::lock_guard<std::mutex> lk(g_fgStatMx);
            ++g_ptSt.late;
        }
        static unsigned s_logs;
        if (++s_logs <= 10 || s_logs % 500 == 0) {
            uint64_t ev = 0, rd = 0, dn = 0;
            fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemEval, &ev);
            if (g_fgSemReady) fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemReady, &rd);
            if (g_fgSemDone) fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemDone, &dn);
            uint64_t sp = 0, sh = 0;  // most: A proxy (semP) a ProcessFrame helperu (semH)
            if (g_semP) fvk.vkGetSemaphoreCounterValue(g_dev, g_semP, &sp);
            if (g_semH) fvk.vkGetSemaphoreCounterValue(g_dev, g_semH, &sh);
            Log("prezentace: snimek N %llu (polozka %llu) nebyl do 250 ms na GPU hotov (%s; semafory: hotovo %llu, "
                "proxy zkopirovala %llu, prevzato %llu; most proxy %llu, helper %llu) - vynechan",
                (unsigned long long)it.N, (unsigned long long)it.seq, FgVkStr(wr), (unsigned long long)ev,
                (unsigned long long)rd, (unsigned long long)dn, (unsigned long long)sp, (unsigned long long)sh);
        }
        return;
    }
    const LONGLONG tReady = FgQpc();
    if (it.postQpc) {
        const double dn = FgMs(it.postQpc, tReady);
        if (!readyBefore && dn > 0.0 && dn < 100.0) g_adDoneMs = g_adDoneMs + 0.1 * (dn - g_adDoneMs);
        std::lock_guard<std::mutex> lk(g_fgStatMx);
        g_ptSt.done.add(dn);
    }
    const FgOut& o = g_fgOut[it.slot];
    if (it.fixedTimes) {
        // Adaptivni FSR, rozvrh A: kazdy snimek ve svem case mrizky. Pozdni slot (L jeste nestaci) jde hned, zpozdeni se
        // hlasi hlavnimu vlaknu (zvetsi L); slot pozde o vic nez 2 periody se zahodi (obsah uz je stary).
        const LONGLONG period = (LONGLONG)(it.intervalMs * (double)g_qpf.QuadPart / 1000.0);
        auto show = [&](VkImage img, uint32_t w, uint32_t h, LONGLONG when, bool genF, LONGLONG content) {
            const LONGLONG now = FgQpc();
            if (now > when) {
                const double late = FgMs(when, now);
                if (late > g_adLateMs) g_adLateMs = late;
                if (now > when + 2 * period) {
                    std::lock_guard<std::mutex> lk(g_fgStatMx);
                    ++g_ptSt.adDrop;
                    return;
                }
                if (now > when + period / 4) {
                    std::lock_guard<std::mutex> lk(g_fgStatMx);
                    ++g_ptSt.adLate;
                }
            } else {
                FgPtWaitFor(when);
            }
            if (!FgPtPresentImage(img, w, h, genF, content) && genF) {
                std::lock_guard<std::mutex> lk(g_fgStatMx);
                ++g_ptSt.genFail;
            }
        };
        for (int g = 0; g < it.nInterp; ++g)
            show(o.interp[g].img, o.interp[g].w, o.interp[g].h, it.disp[g], true, it.content[g]);
        if (it.showReal) show(o.real.img, o.real.w, o.real.h, it.dispReal ? it.dispReal : FgQpc(), false,
                              it.content[kFgMaxInterp]);
        FgPtWaitCmds(100000000ull);
        return;
    }
    // nasobek m = nInterp + 1: m snimku rovnomerne v intervalu skutecnych snimku I (krok I / m)
    const int m = it.nInterp + 1;
    // [fg] Catchup=2 (v24t): pozadu je vlakno jen o polozky, jejichz prace je UZ hotova na GPU. Polozka ve fronte se
    // zverejni pri odeslani (post), ale na GPU je hotova az ~post -> hotovo pozdeji; kdyz je to vic nez interval
    // skutecnych snimku (napr. 20 ms pri 52 FPS a 3x), ve fronte skoro vzdy ceka jedna NEHOTOVA polozka a pocitani
    // podle fronty (rezim 1) dohanelo (krok 60 %) skoro kazdy snimek -> nerovnomerne tempo, "liny" pohyb.
    if (g_ptCatchupMode == 2) {
        unsigned ready = 0;
        uint64_t evQ = 0;
        fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemEval, &evQ);
        {
            std::lock_guard<std::mutex> lq(g_fgMx);
            for (int i = 0; i < g_fgQn; ++i)
                if (g_fgQ[i].seq <= evQ) ++ready;
        }
        behind = ready >= 2;
        g_ptCatchup = ready == 1;
    }
    const bool catchup = g_ptCatchup;  // ceka dalsi polozka: krok 60 %, bez vyrovnavaci faze
    const double step = it.intervalMs / (double)m * (catchup ? 0.6 : 1.0);
    const LONGLONG stepQ = (LONGLONG)(step * (double)g_qpf.QuadPart / 1000.0);
    if (catchup && it.nInterp > 0 && !behind) {
        std::lock_guard<std::mutex> lk(g_fgStatMx);
        ++g_ptSt.catchup;
    }
    if (it.nInterp > 0 && !behind) {
        // Prvni generovany snimek co nejdriv, dalsi generovane a nakonec skutecny vzdy o I/m pozdeji. Kolisani casu
        // "snimek hotov na GPU" by slo primo do intervalu presentu, proto mala vyrovnavaci faze: cil = minuly prvni
        // generovany + 0.98 * I. Snimek hotovy driv na cil pocka (nejdele I/m), pozdni jde hned a fazi posune; cil se
        // o 2 % intervalu za snimek sam vraci k nejdrivejsimu moznemu casu (pridana latence = jen bezne kolisani).
        // Nikdy driv nez 0.9 * I/m po minulem presentu.
        LONGLONG ti = tReady;
        if (!catchup && g_ptNextInterpQpc > ti && FgMs(ti, g_ptNextInterpQpc) < step) ti = g_ptNextInterpQpc;
        if (g_ptLastCallQpc) {
            const LONGLONG minT = g_ptLastCallQpc + (LONGLONG)(0.9 * (double)stepQ);
            if (minT > ti && FgMs(ti, minT) < step) ti = minT;
        }
        FgPtWaitFor(ti);
        bool all = true;
        for (int g = 0; g < it.nInterp; ++g) {
            if (g) FgPtWaitFor(g_ptLastCallQpc + stepQ);
            const OwnImg& gi = o.interp[g];
            if (!FgPtPresentImage(gi.img, gi.w, gi.h, true, it.content[g])) {
                std::lock_guard<std::mutex> lk(g_fgStatMx);
                g_ptSt.genFail += (unsigned)(it.nInterp - g);
                all = false;
                break;
            }
            // rozvrh od ZACATKU volani presentu (ne od jeho konce)
            if (!g) g_ptNextInterpQpc = g_ptLastCallQpc + (LONGLONG)(0.98 * (double)m * (double)stepQ);
        }
        if (all) FgPtWaitFor(g_ptLastCallQpc + stepQ);
    } else {
        g_ptNextInterpQpc = 0;
        if (it.nInterp > 0) {
            std::lock_guard<std::mutex> lk(g_fgStatMx);
            g_ptSt.behind += (unsigned)it.nInterp;
        }
    }
    FgPtPresentImage(o.real.img, o.real.w, o.real.h, false, it.content[kFgMaxInterp]);
    // Blity nad obrazy slotu musi dobehnout, nez slot dostane hlavni vlakno znovu (jina fronta = zadne poradi)
    FgPtWaitCmds(100000000ull);
}

// Skutecny cas "post -> hotovo na GPU" (v24u). Vlakno presentu ho meri jen kdyz na polozku ceka; jinak (polozka
// hotova driv, nez ji vyzvedlo) je to jen horni mez. Toto vlakno ceka na kazdou hodnotu g_fgSemEval zvlast a cas
// zverejnuje proxy (BmsdShared::fgGpuDone) pro regulator limiteru podle casu GPU ([fg] LimiterAdapt=3).
static LONGLONG g_gwPost[64];  // postQpc polozky seq (index seq & 63), zapisuje hlavni vlakno pred odeslanim
static bool g_gwStarted;
static void FgGpuWatchThread() {
    uint64_t last = 0;
    fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemEval, &last);
    uint32_t pub = 0;
    for (;;) {  // bez konce (zarizeni helper nerusi; proces konci s hrou)
        if (g_fgDead) {  // prezentace skoncila (muze se znovu zapnout)
            Sleep(200);
            fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemEval, &last);
            continue;
        }
        uint64_t want = last + 1;
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &g_fgSemEval, &want};
        const VkResult r = fvk.vkWaitSemaphores(g_dev, &wi, 100000000ull);
        if (r == VK_TIMEOUT) continue;
        if (r != VK_SUCCESS) {
            Sleep(100);
            continue;
        }
        const LONGLONG t = FgQpc();
        uint64_t cur = want;
        fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemEval, &cur);
        // Vic hotovych najednou (vlakno bylo pozadu): presny cas jen pro posledni, starsi preskocit.
        if (cur < want) cur = want;
        last = cur;
        MemoryBarrier();
        const LONGLONG post = g_gwPost[cur & 63];
        if (cur != want || !post || t <= post) continue;
        const double ms = FgMs(post, t);
        if (ms <= 0.0 || ms > 500.0) continue;
        {
            std::lock_guard<std::mutex> lk(g_fgStatMx);
            g_ptSt.gpuDone.add(ms);
        }
        if (g_sh) {
            pub = (pub + 1) & 0xFFFFu;
            if (!pub) pub = 1;
            g_sh->fgGpuDone = (pub << 16) | (uint32_t)std::min(65535.0, ms * 100.0);
        }
    }
}

static void FgPtStop() {
    FgPtDestroySwapchain("zadost proxy / konec");
    FgPtDestroySurface();
    if (g_gpImg[0].img && FgPtWaitCmds(2000000000ull)) GpDestroy();  // zobrazeni v procesu hry: obrazy pryc
    g_ptStopped = true;
    if (g_ps && g_ps->helperState != BMSD_PST_FAILED) FgSetPresState(BMSD_PST_STOPPED);
}

static void FgPresentThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    std::unique_lock<std::mutex> lk(g_fgMx);
    for (;;) {
        g_fgCv.wait_for(lk, std::chrono::milliseconds(100),
                        [] { return g_fgQuit || g_fgQuiesce || g_fgStopReq || g_fgQn > 0; });
        if (g_fgQuit) break;
        if (g_fgQuiesce) {
            g_fgParked = true;
            g_fgCv.notify_all();
            g_fgCv.wait(lk, [] { return !g_fgQuiesce || g_fgQuit; });
            g_fgParked = false;
            continue;
        }
        if ((g_fgStopReq || g_ptFailed) && !g_ptStopped) {
            lk.unlock();
            FgPtStop();
            lk.lock();
            continue;
        }
        if (g_fgQn == 0) {
            if (g_fgStopReq) g_fgCv.wait_for(lk, std::chrono::milliseconds(50));
            continue;
        }
        const FgItem it = g_fgQ[0];
        g_fgQ[0] = g_fgQ[1];
        g_fgQ[1] = g_fgQ[2];
        --g_fgQn;
        // Za touto polozkou uz ceka dalsi: drive se vsechny generovane zahodily. Rozvrh ale trva skoro presne interval
        // skutecnych snimku, takze pri GPU blizko plneho vytizeni (4K, 3x) stacilo male kolisani a zahazovalo se stovky
        // snimku za 5 s (trhani, "FG se vypina"). [fg] Catchup=1: pri jedne cekajici polozce generovane snimky ukazat se
        // zkracenym krokem (dohnat), zahodit az pri dvou.
        if (g_ptCatchupMode < 0) {
            g_ptCatchupMode = (int)wcstol(IniStr(L"fg", L"Catchup", L"2").c_str(), nullptr, 0);
            if (g_ptCatchupMode < 0 || g_ptCatchupMode > 2) g_ptCatchupMode = 2;
            Log("prezentace: dohaneni generovanych snimku [fg] Catchup=%d", g_ptCatchupMode);
        }
        const int s_catchup = g_ptCatchupMode != 0;
        const bool behind = g_fgQn > 1 || (g_fgQn == 1 && !s_catchup);
        g_ptCatchup = g_fgQn == 1 && s_catchup;
        g_fgOut[it.slot].state = FGO_BUSY;
        lk.unlock();
        if (!g_ptStopped && !g_ptFailed) FgPtPresentItem(it, behind);
        lk.lock();
        g_fgOut[it.slot].state = FGO_FREE;
    }
    lk.unlock();
    if (g_ps && !g_ptStopped) FgPtStop();
}

// Vlakno presentu zaparkovat (nic z fronty, zadna prace nad obrazy) / pustit dal. Jen z hlavniho vlakna.
static void FgQuiesce() {
    if (!g_fgThread.joinable()) return;
    std::unique_lock<std::mutex> lk(g_fgMx);
    g_fgQuiesce = true;
    for (int i = 0; i < g_fgQn; ++i) g_fgOut[g_fgQ[i].slot].state = FGO_FREE;
    g_fgQn = 0;
    g_fgCv.notify_all();
    g_fgCv.wait(lk, [] { return g_fgParked; });
}
static void FgResume() {
    if (!g_fgThread.joinable()) return;
    std::lock_guard<std::mutex> lk(g_fgMx);
    g_fgQuiesce = false;
    g_fgCv.notify_all();
}

// ---------------------------------------------------------------------------------------------------------------
// Hlavni vlakno: zdroje
// ---------------------------------------------------------------------------------------------------------------
// GPU v klidu (vsechny fronty). Volat se zaparkovanym vlaknem presentu nebo aspon pod zamkem jeho fronty.
static bool FgWaitGpuIdle() {
    for (int i = 0; i < kSlots; ++i)
        if (g_fgFenceUsed[i]) {
            if (vk.vkWaitForFences(g_dev, 1, &g_fgFence[i], VK_TRUE, kGpuTimeoutNs) != VK_SUCCESS) return false;
            vk.vkResetFences(g_dev, 1, &g_fgFence[i]);
            g_fgFenceUsed[i] = false;
            g_fgTsUsed[i] = false;
        }
    DeviceWaitIdleLocked();
    return true;
}

// Uvolni generator (DLSS-G feature nebo kontext FSR FG). Volat jen kdyz GPU nic nezpracovava (FgWaitGpuIdle):
// ffx rusi sve obrazy a pipeline hned.
static void FgReleaseFeature(const char* why) {
    if (g_afgOk) {  // adaptivni FSR (vlastni AFG): GPU je v klidu (volajici)
        AfgDestroy();
        g_afgOk = false;
        g_afgLastSlot = -1;
        g_afgPrevPaired = false;
        g_ad.nextDisp = 0;
        Log("FG: kontext adaptivniho FSR uvolnen (%s)", why);
        g_fgSnapWanted = false;
    }
    if (!g_dlssg && !g_ffg) return;
    if (g_dlssg) {
        const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_ReleaseFeature(g_dlssg);
        Log("FG: DLSS-G feature uvolnena (%s; %s)", why, NgxStr(r).c_str());
        g_dlssg = nullptr;
    }
    if (g_ffg) {
        const ffxReturnCode_t r = g_ffxDestroy(&g_ffg, nullptr);
        Log("FG: kontext FSR FG uvolnen (%s; kod %u)", why, (unsigned)r);
        g_ffg = nullptr;
        g_ffgLastSlot = -1;
    }
    g_fgSnapWanted = false;
}

static void FgDestroySnaps() {
    for (FgSnap& s : g_fgSnap) {
        DestroyOwn(s.mv);
        DestroyOwn(s.depth);
        s.valid = false;
        s.B = 0;
    }
    g_fgSnapOk = false;
    g_fgSnapWanted = false;
}

// Most reimportuje zdroje (zmena rozliseni sceny): feature a snapshoty patri ke stare velikosti. Vola ImportResources
// po DrainGpu (GPU v klidu).
static void FgOnBridgeReimport() {
    if (!g_fgVkOk) return;
    for (int i = 0; i < kSlots; ++i)
        if (g_fgFenceUsed[i]) {  // po vkDeviceWaitIdle jsou signalizovane
            vk.vkResetFences(g_dev, 1, &g_fgFence[i]);
            g_fgFenceUsed[i] = false;
            g_fgTsUsed[i] = false;
        }
    FgReleaseFeature("reimport zdroju mostu");
    FgDestroySnaps();
}

static bool FgMakeDepthImage(OwnImg& o, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = g_depthFmt;  // jako DEPTH_IN (kopie hloubky jen mezi stejnymi formaty)
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.vkCreateImage(g_dev, &ci, nullptr, &o.img) != VK_SUCCESS) return false;
    VkMemoryRequirements req = {};
    vk.vkGetImageMemoryRequirements(g_dev, o.img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                               DeviceLocalType(req.memoryTypeBits)};
    if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(g_dev, &ai, nullptr, &o.mem) != VK_SUCCESS) return false;
    if (vk.vkBindImageMemory(g_dev, o.img, o.mem, 0) != VK_SUCCESS) return false;
    o.view = MakeView(o.img, g_depthFmt, VK_IMAGE_ASPECT_DEPTH_BIT);
    o.w = w;
    o.h = h;
    return o.view != VK_NULL_HANDLE;
}

static bool FgCreateSnaps() {
    if (g_fgSnapOk) return true;
    if (!g_ownOk || !g_w || !g_h) return false;
    for (FgSnap& s : g_fgSnap) {
        if (!MakeOwnImage(s.mv, kMvFormat,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, g_w,
                          g_h) ||
            !FgMakeDepthImage(s.depth, g_w, g_h)) {
            FgDestroySnaps();
            return false;
        }
        // set 1 pro primy zapis MV (bez nej snapshot MV kopiruje jako drive)
        if (!s.mvSet) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, g_descPool, 1,
                                              &g_dslMv};
            if (vk.vkAllocateDescriptorSets(g_dev, &ai, &s.mvSet) != VK_SUCCESS) s.mvSet = VK_NULL_HANDLE;
        }
        if (s.mvSet) {
            VkDescriptorImageInfo ii = {VK_NULL_HANDLE, s.mv.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.mvSet, 0, 0, 1,
                                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii};
            vk.vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);
        }
        s.valid = false;
    }
    g_fgSnapOk = true;
    return true;
}

// Doostreni generovanych snimku ([fg] SharpenGen, v25i): generovane snimky DLSS-G jsou na jemnem detailu o 16-18 %
// mekci nez skutecne, obraz pak pulzuje a ploty pri kazdem skutecnem snimku cuknou (mereni 2026-11-22, fgflick.ps1).
// Generovany snimek (BGRA8 - do toho shader psat neumi) -> kopie g_fsA (sampler) -> fgsharp.comp (RCAS) -> g_fsB RGBA8
// -> blit zpet do generovaneho snimku. Vlastni descriptor set, zapsany jednou pri vytvoreni.
static VkDescriptorSetLayout g_fsDsl;
static VkPipelineLayout g_fsPl;
static VkPipeline g_fsPipe;
static VkDescriptorPool g_fsPool;
static VkDescriptorSet g_fsSet;
static OwnImg g_fsA, g_fsB;
static bool g_fsOk;
static void FgSharpDestroy() {
    g_fsOk = false;
    DestroyOwn(g_fsA);
    DestroyOwn(g_fsB);
    if (g_fsPipe) vk.vkDestroyPipeline(g_dev, g_fsPipe, nullptr);
    if (g_fsPool) vk.vkDestroyDescriptorPool(g_dev, g_fsPool, nullptr);
    if (g_fsPl) vk.vkDestroyPipelineLayout(g_dev, g_fsPl, nullptr);
    if (g_fsDsl) vk.vkDestroyDescriptorSetLayout(g_dev, g_fsDsl, nullptr);
    g_fsPipe = VK_NULL_HANDLE;
    g_fsPool = VK_NULL_HANDLE;
    g_fsPl = VK_NULL_HANDLE;
    g_fsDsl = VK_NULL_HANDLE;
    g_fsSet = VK_NULL_HANDLE;
}
static void FgSharpCreate() {
    FgSharpDestroy();
    if (g_fgCfg.sharpenGen <= 0.0f) return;
    const char* why = nullptr;
    if (!FormatHas(g_fgFmt, VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        why = "format generovanych snimku neumi blit / sampler";
    else if (!FormatHas(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT))
        why = "RGBA8 neumi storage / blit";
    else if (!MakeOwnImage(g_fsA, g_fgFmt, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, g_fgBbW, g_fgBbH) ||
             !MakeOwnImage(g_fsB, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           g_fgBbW, g_fgBbH))
        why = "pomocne obrazy nejdou vytvorit";
    if (!why) {
        const VkDescriptorSetLayoutBinding b[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_sampler},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        const VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 2, b};
        const VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &g_fsDsl;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        const VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 2, ps};
        const VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof kFgSharpCompSpv,
                                             kFgSharpCompSpv};
        VkShaderModule mod = VK_NULL_HANDLE;
        bool ok = vk.vkCreateDescriptorSetLayout(g_dev, &dl, nullptr, &g_fsDsl) == VK_SUCCESS &&
                  vk.vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_fsPl) == VK_SUCCESS &&
                  vk.vkCreateDescriptorPool(g_dev, &dp, nullptr, &g_fsPool) == VK_SUCCESS &&
                  vk.vkCreateShaderModule(g_dev, &sm, nullptr, &mod) == VK_SUCCESS;
        if (ok) {
            VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main"};
            cp.layout = g_fsPl;
            ok = vk.vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cp, nullptr, &g_fsPipe) == VK_SUCCESS;
        }
        if (mod) vk.vkDestroyShaderModule(g_dev, mod, nullptr);
        if (ok) {
            const VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, g_fsPool, 1, &g_fsDsl};
            ok = vk.vkAllocateDescriptorSets(g_dev, &ai, &g_fsSet) == VK_SUCCESS;
        }
        if (ok) {
            const VkDescriptorImageInfo ii[2] = {{g_sampler, g_fsA.view, VK_IMAGE_LAYOUT_GENERAL},
                                                 {VK_NULL_HANDLE, g_fsB.view, VK_IMAGE_LAYOUT_GENERAL}};
            const VkWriteDescriptorSet w[2] = {
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_fsSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[0]},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_fsSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[1]}};
            vk.vkUpdateDescriptorSets(g_dev, 2, w, 0, nullptr);
        } else {
            why = "pipeline fgsharp.comp nejde vytvorit";
        }
    }
    if (why) {
        FgSharpDestroy();
        Log("WARN: doostreni generovanych snimku (SharpenGen) nepobezi: %s", why);
        return;
    }
    g_fsOk = true;
    Log("FG: doostreni generovanych snimku zapnuto (SharpenGen %.2f, RCAS, pomocne obrazy %ux%u)", g_fgCfg.sharpenGen,
        g_fgBbW, g_fgBbH);
}
// Do cmd: generovany snimek (GENERAL, prave zapsany generatorem) doostrit na miste
static void FgSharpRecord(VkCommandBuffer cmd, VkImage img) {
    const VkImageAspectFlags COLOR = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL;
    const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, XFER = VK_PIPELINE_STAGE_TRANSFER_BIT,
                               CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkAccessFlags MEM = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkImageMemoryBarrier b1[2] = {IB(img, COLOR, MEM, VK_ACCESS_TRANSFER_READ_BIT, GEN, GEN),
                                  IB(g_fsA.img, COLOR, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN)};
    PB(cmd, ALL, XFER, 2, b1);
    VkImageCopy rc = {};
    rc.srcSubresource = {COLOR, 0, 0, 1};
    rc.dstSubresource = {COLOR, 0, 0, 1};
    rc.extent = {g_fgBbW, g_fgBbH, 1};
    vk.vkCmdCopyImage(cmd, img, GEN, g_fsA.img, GEN, 1, &rc);
    VkImageMemoryBarrier b2[2] = {IB(g_fsA.img, COLOR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, GEN, GEN),
                                  IB(g_fsB.img, COLOR, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, GEN)};
    PB(cmd, XFER | ALL, CS, 2, b2);
    const float con = exp2f(-(2.0f - 2.0f * g_fgCfg.sharpenGen));
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_fsPipe);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_fsPl, 0, 1, &g_fsSet, 0, nullptr);
    vk.vkCmdPushConstants(cmd, g_fsPl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &con);
    vk.vkCmdDispatch(cmd, (g_fgBbW + 7) / 8, (g_fgBbH + 7) / 8, 1);
    VkImageMemoryBarrier b3[2] = {IB(g_fsB.img, COLOR, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, GEN, GEN),
                                  IB(img, COLOR, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, GEN, GEN)};
    PB(cmd, CS | XFER, XFER, 2, b3);
    VkImageBlit bl = {};
    bl.srcSubresource = {COLOR, 0, 0, 1};
    bl.srcOffsets[1] = {(int32_t)g_fgBbW, (int32_t)g_fgBbH, 1};
    bl.dstSubresource = {COLOR, 0, 0, 1};
    bl.dstOffsets[1] = {(int32_t)g_fgBbW, (int32_t)g_fgBbH, 1};
    vk.vkCmdBlitImage(cmd, g_fsB.img, GEN, img, GEN, 1, &bl, VK_FILTER_NEAREST);
    VkImageMemoryBarrier b4 = IB(img, COLOR, VK_ACCESS_TRANSFER_WRITE_BIT, MEM, GEN, GEN);
    PB(cmd, XFER, ALL, 1, &b4);
}

// Maska UI pro DLSS-G ([fg] DlssgUiAlpha, pokus v25u): uialpha.comp z backbufferu a HUD-less (oba v SHADER_READ_ONLY
// pri vyhodnoceni) -> g_uaImg R8 (alfa UI) -> DLSSG.UIAlpha + UserInterfaceRecompositionEnabled. Descriptor set na
// kazdy slot mostu k (pohledy vznikaji pri importu, pak se nemeni).
static VkDescriptorSetLayout g_uaDsl;
static VkPipelineLayout g_uaPl;
static VkPipeline g_uaPipe;
static VkDescriptorPool g_uaPool;
static VkDescriptorSet g_uaSet[kFgK][kFgSnap];
static VkImageView g_uaDepthView[kFgK][kFgSnap];  // pohled hloubky zapsany ve vazbe 3 (snapshot se muze znovu vytvorit)
static OwnImg g_uaImg;
static bool g_uaOk;
static void FgUiaDestroy() {
    g_uaOk = false;
    DestroyOwn(g_uaImg);
    if (g_uaPipe) vk.vkDestroyPipeline(g_dev, g_uaPipe, nullptr);
    if (g_uaPool) vk.vkDestroyDescriptorPool(g_dev, g_uaPool, nullptr);
    if (g_uaPl) vk.vkDestroyPipelineLayout(g_dev, g_uaPl, nullptr);
    if (g_uaDsl) vk.vkDestroyDescriptorSetLayout(g_dev, g_uaDsl, nullptr);
    g_uaPipe = VK_NULL_HANDLE;
    g_uaPool = VK_NULL_HANDLE;
    g_uaPl = VK_NULL_HANDLE;
    g_uaDsl = VK_NULL_HANDLE;
    for (auto& row : g_uaSet)
        for (VkDescriptorSet& s : row) s = VK_NULL_HANDLE;
    for (auto& row : g_uaDepthView)
        for (VkImageView& v : row) v = VK_NULL_HANDLE;
}
static void FgUiaCreate() {
    FgUiaDestroy();
    if (!g_fgCfg.dlssgUiAlpha) return;
    const char* why = nullptr;
    if (!g_fgHlOk) why = "chybi HUD-less";
    else if (!FormatHas(g_fgFmt, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) why = "backbuffer neumi sampler";
    else if (!FormatHas(VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        why = "R8 neumi storage";
    else if (!MakeOwnImage(g_uaImg, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           g_fgBbW, g_fgBbH))
        why = "obraz masky nejde vytvorit";
    for (int k = 0; k < kFgK && !why; ++k)
        if (!g_fgPresView[k] || !g_fgHlView[k]) why = "chybi pohled na obraz mostu";
    if (!why) {
        const VkDescriptorSetLayoutBinding b[4] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_sampler},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_sampler},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_sampler}};
        const VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, b};
        const VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &g_uaDsl;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kFgK * kFgSnap},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFgK * kFgSnap}};
        const VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kFgK * kFgSnap, 2, ps};
        const VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof kUiAlphaCompSpv,
                                             kUiAlphaCompSpv};
        VkShaderModule mod = VK_NULL_HANDLE;
        bool ok = vk.vkCreateDescriptorSetLayout(g_dev, &dl, nullptr, &g_uaDsl) == VK_SUCCESS &&
                  vk.vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_uaPl) == VK_SUCCESS &&
                  vk.vkCreateDescriptorPool(g_dev, &dp, nullptr, &g_uaPool) == VK_SUCCESS &&
                  vk.vkCreateShaderModule(g_dev, &sm, nullptr, &mod) == VK_SUCCESS;
        if (ok) {
            VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main"};
            cp.layout = g_uaPl;
            ok = vk.vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cp, nullptr, &g_uaPipe) == VK_SUCCESS;
        }
        if (mod) vk.vkDestroyShaderModule(g_dev, mod, nullptr);
        for (int k = 0; k < kFgK && ok; ++k)
            for (int s = 0; s < kFgSnap && ok; ++s) {
                const VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, g_uaPool, 1,
                                                        &g_uaDsl};
                VkDescriptorSet set = VK_NULL_HANDLE;
                ok = vk.vkAllocateDescriptorSets(g_dev, &ai, &set) == VK_SUCCESS;
                if (!ok) break;
                g_uaSet[k][s] = set;
                // vazba 3 zatim HUD-less (platny popisovac; hloubka se zapise pred prvnim pouzitim, FgUiaRecord)
                const VkDescriptorImageInfo ii[4] = {{g_sampler, g_fgPresView[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                     {g_sampler, g_fgHlView[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                     {VK_NULL_HANDLE, g_uaImg.view, VK_IMAGE_LAYOUT_GENERAL},
                                                     {g_sampler, g_fgHlView[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
                const VkWriteDescriptorSet w[4] = {
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[0]},
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[1]},
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[2]},
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[3]}};
                vk.vkUpdateDescriptorSets(g_dev, 4, w, 0, nullptr);
            }
        if (!ok) why = "pipeline uialpha.comp nejde vytvorit";
    }
    if (why) {
        FgUiaDestroy();
        Log("WARN: maska UI pro DLSS-G (DlssgUiAlpha) nepobezi: %s", why);
        return;
    }
    g_uaOk = true;
    Log("FG: maska UI pro DLSS-G zapnuta (DlssgUiAlpha, prah %.1f/255, %ux%u R8) - UIR", g_fgCfg.uiAlphaThr, g_fgBbW, g_fgBbH);
}
// Do cmd pred vyhodnocenim DLSS-G (backbuffer a HUD-less slotu k uz v SHADER_READ_ONLY). Bez HUD-less tohoto snimku
// (hl = false, DLSS-G pak dostane jako HUD-less backbuffer) prazdna maska - DLSS-G ji ma "vzdy" (jinak MissingInput).
static void FgUiaRecord(VkCommandBuffer cmd, int k, bool hl, const FgSnap& sn) {
    const VkImageAspectFlags COLOR = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL;
    const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (!hl) {
        VkImageMemoryBarrier c1 = IB(g_uaImg.img, COLOR, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_IMAGE_LAYOUT_UNDEFINED, GEN);
        PB(cmd, ALL, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, &c1);
        const VkClearColorValue zero = {};
        const VkImageSubresourceRange rr = {COLOR, 0, 1, 0, 1};
        vk.vkCmdClearColorImage(cmd, g_uaImg.img, GEN, &zero, 1, &rr);
        VkImageMemoryBarrier c2 = IB(g_uaImg.img, COLOR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, GEN, GEN);
        PB(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, ALL, 1, &c2);
        return;
    }
    VkImageMemoryBarrier b1 = IB(g_uaImg.img, COLOR, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED, GEN);
    PB(cmd, ALL, CS, 1, &b1);
    // snapshot hloubky (v SHADER_READ_ONLY po FgSnapshot): pohled zapsat do setu, kdyz se zmenil (novy snapshot
    // vznika jen pri vytvoreni feature, po klidu GPU - set se tedy nemeni, zatimco je v letu)
    const int s = (int)(&sn - g_fgSnap);
    const bool vmOn = g_fgCfg.uiAlphaWeapon && g_cfg.viewmodelDepth > 0.0f && s >= 0 && s < kFgSnap && sn.depth.view;
    VkDescriptorSet set = g_uaSet[k][s >= 0 && s < kFgSnap ? s : 0];
    if (vmOn && g_uaDepthView[k][s] != sn.depth.view) {
        g_uaDepthView[k][s] = sn.depth.view;
        const VkDescriptorImageInfo di = {g_sampler, sn.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0, 1,
                                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &di};
        vk.vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);
    }
    const float pcv[2] = {g_fgCfg.uiAlphaThr / 255.0f, vmOn && g_uaDepthView[k][s] ? g_cfg.viewmodelDepth : 0.0f};
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_uaPipe);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_uaPl, 0, 1, &set, 0, nullptr);
    vk.vkCmdPushConstants(cmd, g_uaPl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pcv);
    vk.vkCmdDispatch(cmd, (g_fgBbW + 7) / 8, (g_fgBbH + 7) / 8, 1);
    VkImageMemoryBarrier b2 = IB(g_uaImg.img, COLOR, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, GEN, GEN);
    PB(cmd, CS, ALL, 1, &b2);
}

static void FgDestroyOut() {
    FgSharpDestroy();
    FgUiaDestroy();
    for (FgOut& o : g_fgOut) {
        for (OwnImg& im : o.interp) DestroyOwn(im);
        DestroyOwn(o.real);
        o.state = FGO_FREE;
    }
}

static void FgDestroyPresImages() {
    for (int i = 0; i < kFgK; ++i) {
        for (Img* im : {&g_fgPres[i], &g_fgHl[i]}) {
            VkImageView& v = im == &g_fgPres[i] ? g_fgPresView[i] : g_fgHlView[i];
            if (v) vk.vkDestroyImageView(g_dev, v, nullptr);
            if (im->img) vk.vkDestroyImage(g_dev, im->img, nullptr);
            if (im->mem) vk.vkFreeMemory(g_dev, im->mem, nullptr);
            v = VK_NULL_HANDLE;
            *im = Img();
        }
    }
    g_fgImgOk = false;
    g_fgHlOk = false;
}

// Import jednoho sdileneho obrazu presentu (handle je jednorazovy: zavre se vzdy)
static VkResult FgImportOne(const BmsdImage& bi, Img& im, VkImageView& view) {
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    im.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
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
    if (r == VK_SUCCESS) {
        VkMemoryRequirements req = {};
        vk.vkGetImageMemoryRequirements(g_dev, im.img, &req);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, im.img};
        VkImportMemoryWin32HandleInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded, ht,
                                                (HANDLE)(uintptr_t)bi.handle};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, bi.allocSize,
                                   DeviceLocalType(req.memoryTypeBits)};
        r = vk.vkAllocateMemory(g_dev, &ai, nullptr, &im.mem);
        if (r == VK_SUCCESS) r = vk.vkBindImageMemory(g_dev, im.img, im.mem, 0);
    }
    CloseHandle((HANDLE)(uintptr_t)bi.handle);  // handle je jednorazovy (proxy ho zdvojila pro tuto generaci)
    if (r == VK_SUCCESS && (bi.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        view = MakeView(im.img, (VkFormat)bi.format, VK_IMAGE_ASPECT_COLOR_BIT);
    return r;
}

// Dlouha prace hlavniho vlakna (blokuje potvrzovani snimku mostu): proxy ji po tu dobu nebere jako selhani helperu
struct FgBusyScope {
    FgBusyScope() {
        if (g_sh) g_sh->helperBusyTick = GetTickCount() | 1u;
        MemoryBarrier();
    }
    ~FgBusyScope() {
        MemoryBarrier();
        if (g_sh) g_sh->helperBusyTick = 0;
    }
};

// Import sdilenych obrazu presentu (nova generace od proxy) a vlastni vystupni obrazy. false = presenter selhal.
static bool FgImport() {
    FgBusyScope busy;
    BmsdPresentShared* ps = g_ps;
    const int32_t gen = ps->generation;
    FgQuiesce();
    const bool idle = FgWaitGpuIdle();
    if (!idle) {
        FgResume();
        return FgPresFatal("GPU nedobehla pred importem obrazu presentu"), false;
    }
    FgReleaseFeature("nove obrazy presentu");
    FgDestroyOut();
    FgDestroyPresImages();
    bool ok = true;
    if (!g_fgSemReady) {
        if (!ImportSemaphore(ps->semReadyHandle, &g_fgSemReady) || !ImportSemaphore(ps->semDoneHandle, &g_fgSemDone)) {
            FgResume();
            return FgPresFatal("import semaforu presentu selhal"), false;
        }
    }
    for (int i = 0; i < kFgK; ++i) {
        const VkResult r = FgImportOne(ps->img[i], g_fgPres[i], g_fgPresView[i]);
        if (r != VK_SUCCESS) {
            Log("prezentace: import obrazu %d selhal (%s)", i, FgVkStr(r));
            ok = false;
        }
    }
    // HUD-less (K5): kdyz import selze, FG bezi bez HUD-less (jako v13)
    bool hl = ps->hudlessImages != 0;
    for (int i = 0; i < kFgK && ps->hudlessImages; ++i) {
        const VkResult r = FgImportOne(ps->hudless[i], g_fgHl[i], g_fgHlView[i]);
        if (r != VK_SUCCESS || !g_fgHlView[i] || ps->hudless[i].format != ps->img[i].format ||
            ps->hudless[i].width != ps->img[i].width || ps->hudless[i].height != ps->img[i].height) {
            if (hl) Log("prezentace: import obrazu HUD-less %d selhal / nesedi (%s) - FG bez HUD-less", i, FgVkStr(r));
            hl = false;
        }
    }
    g_fgHlOk = hl;
    g_fgGen = gen;
    if (!ok) {
        FgResume();
        return FgPresFatal("import obrazu presentu selhal"), false;
    }
    g_fgFmt = (VkFormat)ps->img[0].format;
    g_fgBbW = ps->width;
    g_fgBbH = ps->height;
    // Vystupy: STORAGE jen kdyz ho format umi (generator); bez nej jde jen kopie skutecnych snimku
    g_fgOutStorage = FormatHas(g_fgFmt, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    (g_fgOutStorage ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT : 0);
    // Obrazy interp jen podle prave pozadovaneho rezimu a nasobku (VRAM: kazdy je velikosti backbufferu x kFgOut);
    // vyssi nasobek DLSS-G nebo adaptivni FSR je doplni pozdeji (FgGrowInterp).
    g_fgGrowFailGen = -1;
    if (ps->fgMode == BMSD_FG_FSRADAPT) g_fgInterpN = kAfgMaxInterp;
    else if (ps->fgMode == BMSD_FG_DLSSG)
        g_fgInterpN = std::max(1, std::min(kFgMaxInterp, std::min((int)(ps->fgMultiplier & 0xFF), g_fgMultMax) - 1));
    else g_fgInterpN = 1;
    BeginInitCmd();
    for (FgOut& o : g_fgOut) {
        bool made = MakeOwnImage(o.real, g_fgFmt, usage, g_fgBbW, g_fgBbH);
        for (int g = 0; g < g_fgInterpN && made; ++g) made = MakeOwnImage(o.interp[g], g_fgFmt, usage, g_fgBbW, g_fgBbH);
        if (!made) {
            ok = false;
            break;
        }
        VkImageMemoryBarrier b[1 + kFgMaxInterp];
        b[0] = IB(o.real.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        for (int g = 0; g < g_fgInterpN; ++g)
            b[1 + g] = IB(o.interp[g].img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL);
        PB(g_initCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 1 + g_fgInterpN, b);
        o.state = FGO_FREE;
    }
    vk.vkEndCommandBuffer(g_initCmd);
    if (ok && SubmitInitCmd() != VK_SUCCESS) ok = false;
    if (!ok) {
        FgDestroyOut();
        FgResume();
        return FgPresFatal("vlastni obrazy presentu %ux%u nejdou vytvorit", g_fgBbW, g_fgBbH), false;
    }
    g_fgImgOk = true;
    g_fgFresh = true;
    FgSharpCreate();
    FgUiaCreate();
    Log("prezentace: obrazy importovany %ux%u x%d, VkFormat %d, usage 0x%X (generace %d)%s; vystupy x%d (generovane "
        "x%d = nasobek zatim az %dx, dalsi podle potreby)%s",
        g_fgBbW, g_fgBbH, kFgK, (int)g_fgFmt, ps->img[0].usage, gen, g_fgHlOk ? " + HUD-less" : " bez HUD-less", kFgOut,
        g_fgInterpN, g_fgInterpN + 1, g_fgOutStorage ? "" : " bez STORAGE (generator nepujde)");
    MemoryBarrier();
    ps->helperGeneration = gen;
    FgResume();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Generator: DLSS-G pres prime NGX (kod create / evaluate podle tools/spike_fgg/fgg_eval.cpp)
// ---------------------------------------------------------------------------------------------------------------
static void FgFail(const char* fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_fgFailedM[g_fgCurMode] = true;
    g_fgFailWhyM[g_fgCurMode] = buf;
    g_fgSnapWanted = false;
    Log("FG: SELHANI (%s) - %s; v tomto rezimu do konce hry jen skutecne snimky",
        g_fgCurMode == BMSD_FG_FSR2X ? "FSR FG" : g_fgCurMode == BMSD_FG_FSRADAPT ? "FSR AFG" : "DLSS-G", buf);
}

static bool FgHave(uint32_t mode) {
    return mode == BMSD_FG_FSRADAPT ? g_afgOk : mode == BMSD_FG_FSR2X ? g_ffg != nullptr : g_dlssg != nullptr;
}
static bool FgAvailFor(uint32_t mode) {
    return mode == BMSD_FG_FSRADAPT ? g_afgAvail : mode == BMSD_FG_FSR2X ? g_ffgAvail : g_fgAvail;
}

// Dostupnost z NGX (po InitNgx; caps v g_ngxCaps)
static void FgQueryAvail() {
    g_fgAvailKnown = true;
    g_fgAvail = false;
    if (!g_ngxInit || !g_ngxCaps) {
        g_fgAvailWhy = "NGX neni k dispozici (NVIDIA RTX a aktualni ovladac)";
        Log("FG: DLSS-G nedostupne - %s", g_fgAvailWhy.c_str());
        return;
    }
    int avail = 0, needs = 0, init = 0;
    unsigned mfMax = 0, dMaj = 0, dMin = 0;
    const NVSDK_NGX_Result ra = NVSDK_NGX_Parameter_GetI(g_ngxCaps, NVSDK_NGX_Parameter_FrameGeneration_Available, &avail);
    NVSDK_NGX_Parameter_GetI(g_ngxCaps, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs);
    const NVSDK_NGX_Result ri =
        NVSDK_NGX_Parameter_GetI(g_ngxCaps, NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init);
    NVSDK_NGX_Parameter_GetUI(g_ngxCaps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &dMaj);
    NVSDK_NGX_Parameter_GetUI(g_ngxCaps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &dMin);
    NVSDK_NGX_Parameter_GetUI(g_ngxCaps, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &mfMax);
    Log("FG: NGX FrameGeneration available=%d (%s), needsUpdatedDriver=%d (min %u.%u), featureInitResult=0x%X, "
        "MultiFrameCountMax=%u",
        avail, NVSDK_NGX_SUCCEED(ra) ? "ok" : "nelze zjistit", needs, dMaj, dMin, (unsigned)init, mfMax);
    // HAGS (hardwarove akcelerovane planovani GPU) - DLSS Frame Generation ho vyzaduje. Jen cteni registru:
    // HwSchMode 2 = zapnuto, 1 = vypnuto, chybi = system / ovladac ho nepodporuje.
    DWORD hags = 0, hsz = sizeof hags;
    const bool hagsKnown = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                                        L"HwSchMode", RRF_RT_REG_DWORD, nullptr, &hags, &hsz) == ERROR_SUCCESS;
    g_fgHagsOff = hagsKnown && hags == 1;
    Log("FG: HAGS (hardwarove akcelerovane planovani GPU): %s", !hagsKnown ? "nezjisteno (nepodporovano?)"
                                                               : hags == 2 ? "zapnuto" : "VYPNUTO");
    if (needs) {
        char b[96];
        snprintf(b, sizeof b, "DLSS-G vyzaduje novejsi ovladac (min %u.%u)", dMaj, dMin);
        g_fgAvailWhy = b;
    } else if ((NVSDK_NGX_FAILED(ra) || !avail) && g_fgHagsOff) {
        g_fgAvailWhy = "DLSS-G neni dostupne - zapni Hardwarove akcelerovane planovani GPU (Nastaveni > System > "
                       "Obrazovka > Grafika) a restartuj PC";
    } else if (NVSDK_NGX_FAILED(ra) || !avail) {
        g_fgAvailWhy = NVSDK_NGX_SUCCEED(ri) && NVSDK_NGX_FAILED((NVSDK_NGX_Result)init)
                           ? "DLSS-G neni dostupne: " + NgxStr((NVSDK_NGX_Result)init)
                           : std::string("DLSS-G neni dostupne (RTX 40 a novejsi, nvngx_dlssg.dll vedle helperu)");
    } else {
        g_fgAvail = true;
        // nasobek: MultiFrameCountMax = kolik generovanych snimku na skutecny umi (RTX 50: 3 = 4x; 1 / 0 = jen 2x)
        g_fgMultMax = std::max(2, std::min(kFgMaxInterp + 1, (int)mfMax + 1));
    }
    // Ladeni (v25m): [fg] DebugEmulateGpu=40 -> jako RTX 40 (DLSS-G jen 2x), =30 -> jako RTX 30 / AMD (bez DLSS-G)
    const int emu = (int)wcstol(IniStr(L"fg", L"DebugEmulateGpu", L"0").c_str(), nullptr, 0);
    if (emu == 40 && g_fgAvail) g_fgMultMax = 2;
    if (emu == 30 && g_fgAvail) {
        g_fgAvail = false;
        g_fgAvailWhy = "DLSS-G neni dostupne (ladeni: DebugEmulateGpu=30)";
    }
    if (emu == 30 || emu == 40) Log("FG: ladeni DebugEmulateGpu=%d", emu);
    if (!g_fgAvail) Log("FG: DLSS-G nedostupne - %s", g_fgAvailWhy.c_str());
    else Log("FG: DLSS-G nasobek az %dx", g_fgMultMax);
}

// Dostupnost FSR Frame Generation (FgInit, jednou): obe DLL AMD vedle helperu s platnym podpisem, loader ffx API
// a aspon jedna verze FG pro toto GPU (na vsech kartach FSR FG 3.1.x; kontext se vytvari az pri zapnuti).
static void FgQueryAvailFsr() {
    g_ffgAvail = false;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    std::string why;
    if (!FfxCheckDll(kFfxLoaderDll, &why) || !FfxCheckDll(kFfgDll, &why) || !FfxLoadLoader(&why)) {
        g_ffgAvailWhy = "FSR FG neni dostupne: " + why;
    } else if (!g_ffxConfigure) {
        g_ffgAvailWhy = "FSR FG neni dostupne: loader ffx API nema ffxConfigure (stara verze DLL)";
    } else {
        ffxQueryDescGetVersions q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        q.device = &g_ffxDev;  // (ukazatel na VkDeviceContext, ne VkDevice)
        uint64_t count = 0;
        q.outputCount = &count;
        ffxReturnCode_t r = g_ffxQuery(nullptr, &q.header);
        std::string list;
        if (r == FFX_API_RETURN_OK && count) {
            std::vector<uint64_t> ids((size_t)count);
            std::vector<const char*> names((size_t)count);
            q.versionIds = ids.data();
            q.versionNames = names.data();
            r = g_ffxQuery(nullptr, &q.header);
            for (uint64_t i = 0; i < count && r == FFX_API_RETURN_OK; ++i) {
                const std::string nm = names[i] ? names[i] : "?";
                list += std::string(i ? ", " : "") + nm;
                if (!g_ffg31Id && (!nm.compare(0, 4, "3.1.") || nm.find(" 3.1.") != std::string::npos)) {
                    g_ffg31Id = ids[i];
                    g_ffg31Name = nm;
                }
                if (!g_ffgForceId && !g_fgCfg.fsrVersion.empty() && nm.find(g_fgCfg.fsrVersion) != std::string::npos) {
                    g_ffgForceId = ids[i];
                    Log("FG: [fg] FsrVersion=%s -> %s", g_fgCfg.fsrVersion.c_str(), nm.c_str());
                }
            }
            if (!g_fgCfg.fsrVersion.empty() && !g_ffgForceId)
                Log("WARN: [fg] FsrVersion=%s mezi verzemi FSR FG neni - verzi vybere ffx API", g_fgCfg.fsrVersion.c_str());
        }
        if (r != FFX_API_RETURN_OK || !count) {
            char b[128];
            snprintf(b, sizeof b, "FSR FG neni dostupne: ffx API nenabizi zadnou verzi frame generation (kod %u)",
                     (unsigned)r);
            g_ffgAvailWhy = b;
        } else {
            g_ffgAvail = true;
            QueryPerformanceCounter(&t1);
            Log("FG: FSR Frame Generation dostupne (ffx API, %.0f ms), verze pro toto GPU: %s",
                (t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart, list.c_str());
        }
    }
    if (!g_ffgAvail) Log("FG: %s", g_ffgAvailWhy.c_str());
}

// Kontext FSR FG: displaySize = backbuffer, maxRenderSize = velikost obrazu MV a hloubky (most g_w x g_h; mensi
// render casti B je sub-rect a predava se az v PrepareV2::renderSize). Priznaky 0 = hloubka 0 near .. 1 far, konecna
// far, LDR, MV v rozliseni renderu bez jitteru. Popisy jsou staticke (ffx_api.h: ukazatele ziji do ffxDestroyContext).
// Prvni vytvoreni trva stovky ms (preklad pipeline), dalsi desitky ms. false = selhalo (FgFail).
static bool FgCreateFsr() {
    static ffxCreateBackendVKDesc be;
    static ffxOverrideVersion ov;
    static ffxCreateContextDescFrameGenerationVersion ver;
    static ffxCreateContextDescFrameGeneration cd;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    {  // hlaseni provideru FG do logu (globalni stav DLL efektu; chyby vzdy, s [upscale] FsrDebug=1 vse)
        ffxConfigureDescGlobalDebug gd = {};
        gd.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
        gd.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
        gd.fpMessage = FfxMsg;
        gd.debugLevel = g_cfg.fsrDebug ? FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE
                                       : FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
        const ffxReturnCode_t gr = g_ffxConfigure(nullptr, &gd.header);
        if (gr != FFX_API_RETURN_OK) Log("FG: registrace hlaseni FSR FG -> kod %u (pokracuji bez nich)", (unsigned)gr);
    }
    be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext = g_ffxDev;
    ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ver.header.pNext = &be.header;
    cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.header.pNext = &ver.header;
    cd.flags = g_cfg.fsrDebug ? (uint32_t)FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0u;
    cd.displaySize = {g_fgBbW, g_fgBbH};
    cd.maxRenderSize = {g_w, g_h};
    cd.backBufferFormat = ffxApiGetSurfaceFormatVK(g_fgFmt);
    // verze: [fg] FsrVersion, jinak vyber ffx API; kdyz ten nejde vytvorit (napr. ML FG na Radeonu bez swapchainu
    // AMD - neovereno), zkusit nejnovejsi 3.1.x
    auto create = [&](uint64_t verId) {
        ov = {};
        ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ov.versionId = verId;
        be.header.pNext = verId ? &ov.header : nullptr;
        g_ffg = nullptr;
        const ffxReturnCode_t cr = g_ffxCreate(&g_ffg, &cd.header, nullptr);
        if (cr != FFX_API_RETURN_OK) g_ffg = nullptr;
        return cr;
    };
    ffxReturnCode_t r = create(g_ffgForceId);
    if ((r != FFX_API_RETURN_OK || !g_ffg) && !g_ffgForceId && g_ffg31Id) {
        Log("FG: kontext FSR FG s verzi vybranou ffx API selhal (kod %u) - zkousim %s", (unsigned)r, g_ffg31Name.c_str());
        r = create(g_ffg31Id);
    }
    if (r != FFX_API_RETURN_OK || !g_ffg) {
        g_ffg = nullptr;
        FgDestroySnaps();
        FgFail("vytvoreni kontextu FSR FG selhalo: kod %u", (unsigned)r);
        return false;
    }
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const ffxReturnCode_t qr = g_ffxQuery(&g_ffg, &pv.header);
    g_ffgVersion = qr == FFX_API_RETURN_OK && pv.versionName ? pv.versionName : "";
    FfxApiEffectMemoryUsage mu = {};
    ffxQueryDescFrameGenerationGetGPUMemoryUsage mq = {};
    mq.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
    mq.gpuMemoryUsageFrameGeneration = &mu;
    if (g_ffxQuery(&g_ffg, &mq.header) != FFX_API_RETURN_OK) mu = {};
    QueryPerformanceCounter(&t1);
    g_ffgLastSlot = -1;
    Log("FG: kontext FSR FG vytvoren: verze %s, backbuffer %ux%u (VkFormat %d), MV/hloubka %ux%u, VRAM podle ffx %.0f "
        "MB, %.0f ms",
        g_ffgVersion.empty() ? "?" : g_ffgVersion.c_str(), g_fgBbW, g_fgBbH, (int)g_fgFmt, g_w, g_h,
        mu.totalUsageInBytes / 1048576.0, (t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart);
    return true;
}

// Generator rezimu mode (BMSD_FG_DLSSG / BMSD_FG_FSR2X) pro aktualni velikosti (backbuffer g_fgBbW x g_fgBbH, obrazy
// mostu g_w x g_h). Vytvari se az kdyz je FG pozadovane; pri zmene velikosti nebo rezimu znovu (druhy generator se
// nejdriv uvolni - existuje vzdy nejvys jeden). Trva stovky ms (proxy snimky mezitim jen ceka / vynechava).
// Doplneni obrazu interp ve vystupnich slotech na want (vyssi nasobek DLSS-G: nasobek - 1; adaptivni FSR:
// kFgMaxInterp). Pri importu vznikaji jen podle aktualniho pozadavku. Stavajici obrazy se nemeni (jen pribudou nove),
// vlakno presentu je behem toho zaparkovane.
static bool FgGrowInterp(int want, const char* why) {
    want = std::min(want, kFgMaxInterp);
    if (g_fgInterpN >= want) return true;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    (g_fgOutStorage ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT : 0);
    FgQuiesce();
    bool ok = true;
    BeginInitCmd();
    for (FgOut& o : g_fgOut) {
        VkImageMemoryBarrier b[kFgMaxInterp];
        int nb = 0;
        for (int g = g_fgInterpN; g < want && ok; ++g) {
            ok = MakeOwnImage(o.interp[g], g_fgFmt, usage, g_fgBbW, g_fgBbH);
            if (ok)
                b[nb++] = IB(o.interp[g].img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL);
        }
        if (nb) PB(g_initCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, nb, b);
        if (!ok) break;
    }
    vk.vkEndCommandBuffer(g_initCmd);
    if (SubmitInitCmd() != VK_SUCCESS) ok = false;
    if (ok) {
        Log("FG: %s - obrazy interp %d -> %d na vystupni slot (%ux%u, +%.0f MB)", why, g_fgInterpN, want, g_fgBbW,
            g_fgBbH, (double)(want - g_fgInterpN) * kFgOut * g_fgBbW * g_fgBbH * 4.0 / (1024.0 * 1024.0));
        g_fgInterpN = want;
    } else {  // co vzniklo, zrusit (pocet zustava)
        for (FgOut& o : g_fgOut)
            for (int g = g_fgInterpN; g < want; ++g) DestroyOwn(o.interp[g]);
    }
    FgResume();
    return ok;
}

static bool FgCreateAfg() {
    if (!FgGrowInterp(kAfgMaxInterp, "adaptivni FSR")) return FgFail("adaptivni FSR: vystupni obrazy %ux%u nejdou vytvorit", g_fgBbW, g_fgBbH), false;
    AfgDevice d;
    d.instance = g_inst;
    d.physicalDevice = g_pd;
    d.device = g_dev;
    d.getInstanceProcAddr = vk.vkGetInstanceProcAddr;
    d.getDeviceProcAddr = vk.vkGetDeviceProcAddr;
    d.queueFamily = g_qf;
    AfgCreateInfo ci;
    ci.displayW = g_fgBbW;
    ci.displayH = g_fgBbH;
    ci.maxRenderW = g_w;
    ci.maxRenderH = g_h;
    ci.colorFormat = g_fgFmt;
    ci.hudless = FgHlGen();
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    std::string err;
    if (!AfgCreate(d, ci, &err)) return FgFail("vytvoreni adaptivniho FSR selhalo: %s", err.c_str()), false;
    QueryPerformanceCounter(&t1);
    g_afgOk = true;
    g_afgLastSlot = -1;
    g_afgPrevPaired = false;
    g_ad.nextDisp = 0;
    Log("FG: adaptivni FSR (%s) vytvoreno: backbuffer %ux%u (VkFormat %d), MV/hloubka %ux%u, HUD-less %s, VRAM %.0f MB, "
        "%.0f ms",
        AfgVersionName(), g_fgBbW, g_fgBbH, (int)g_fgFmt, g_w, g_h, FgHlGen() ? "ano" : "ne",
        AfgVramBytes() / 1048576.0, (t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart);
    return true;
}

static void FgEnsureFeature(uint32_t mode) {
    if (g_fgFailedM[mode] || !FgAvailFor(mode) || !g_fgImgOk || !g_ownOk) return;
    if (FgHave(mode) && g_fgKeyBbW == g_fgBbW && g_fgKeyBbH == g_fgBbH && g_fgKeyW == g_w && g_fgKeyH == g_h) return;
    FgBusyScope busy;
    if (!g_fgOutStorage) return FgFail("format backbufferu %d neumi STORAGE (vystup generatoru)", (int)g_fgFmt);
    if (g_fgFmt != VK_FORMAT_B8G8R8A8_UNORM && g_fgFmt != VK_FORMAT_R8G8B8A8_UNORM)
        return FgFail("format backbufferu %d neni BGRA8 / RGBA8", (int)g_fgFmt);
    if (g_dlssg || g_ffg || g_afgOk) {
        if (!FgWaitGpuIdle()) return FgFail("GPU nedobehla pred zmenou feature");
        FgReleaseFeature(FgHave(mode) ? "zmena velikosti" : "zmena rezimu");
        FgDestroySnaps();
    }
    if (!FgCreateSnaps()) return FgFail("obrazy snapshotu MV / hloubky %ux%u nejdou vytvorit", g_w, g_h);
    if (mode == BMSD_FG_FSRADAPT) {
        if (!FgWaitGpuIdle()) return FgFail("GPU nedobehla pred vytvorenim adaptivniho FSR");
        if (!FgCreateAfg()) {
            FgDestroySnaps();
            return;
        }
        g_fgKeyBbW = g_fgBbW;
        g_fgKeyBbH = g_fgBbH;
        g_fgKeyW = g_w;
        g_fgKeyH = g_h;
        g_fgFresh = true;
        g_fgSnapWanted = true;
        LogVram("po vytvoreni adaptivniho FSR");
        return;
    }
    if (mode == BMSD_FG_FSR2X) {
        if (!FgCreateFsr()) return;
        g_fgKeyBbW = g_fgBbW;
        g_fgKeyBbH = g_fgBbH;
        g_fgKeyW = g_w;
        g_fgKeyH = g_h;
        g_fgFresh = true;
        g_fgSnapWanted = true;
        LogVram("po vytvoreni FSR FG");
        return;
    }
    if (!g_fgParams) {
        const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_AllocateParameters(&g_fgParams);
        if (NVSDK_NGX_FAILED(r) || !g_fgParams) return FgFail("NGX AllocateParameters: %s", NgxStr(r).c_str());
    }
    // Prvni vytvoreni DLSS-G (zadna predchozi feature): stejne jako pri nahrade a u adaptivniho FSR vlakno presentu
    // zaparkovat a GPU nechat dobehnout. NGX pri vytvoreni nahrava model a meni sdileny stav instance, zatimco na GPU
    // bezel ProcessFrame mostu s DLSS SR a vlakno presentu odesilalo na druhou frontu; vzacny MMU fault (TDR) pri startu
    // FG prisel prave v prvni praci po vytvoreni. [fg] DlssgCreateIdle=0 = puvodni chovani (jen pro srovnani).
    static int s_createIdle = -1;
    if (s_createIdle < 0) s_createIdle = wcstol(IniStr(L"fg", L"DlssgCreateIdle", L"1").c_str(), nullptr, 0) != 0 ? 1 : 0;
    bool parked = false;
    if (s_createIdle) {
        LARGE_INTEGER q0, q1;
        QueryPerformanceCounter(&q0);
        FgQuiesce();
        parked = true;
        if (!FgWaitGpuIdle()) {
            FgResume();
            FgDestroySnaps();
            return FgFail("GPU nedobehla pred vytvorenim DLSS-G");
        }
        QueryPerformanceCounter(&q1);
        Log("FG: pred vytvorenim DLSS-G GPU v klidu (vlakno presentu zaparkovano, %.1f ms)",
            (q1.QuadPart - q0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart);
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    bool dyn = g_fgCfg.dynRes != 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        NVSDK_NGX_Parameter* p = g_fgParams;
        NVSDK_NGX_DLSSG_Create_Params cp = {};
        cp.Width = g_fgBbW;
        cp.Height = g_fgBbH;
        cp.NativeBackbufferFormat = (unsigned)g_fgFmt;
        cp.RenderWidth = g_w;   // obrazy MV a hloubky (cast B: platny sub-rect se predava pri vyhodnoceni)
        cp.RenderHeight = g_h;
        cp.DynamicResolutionScaling = dyn;
        // HUD-less (K5): s obrazy od proxy se predava vzdy (snimek bez platneho HUD-less = backbuffer)
        const unsigned hlFlag = NVSDK_NGX_DLSSG_ResourceFlags_HUDLess;
        const bool uia = g_uaOk && FgHlGen();  // maska UI (DlssgUiAlpha, v25u)
        const unsigned uaFlag = NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha;
        const unsigned always = NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer | NVSDK_NGX_DLSSG_ResourceFlags_MVecs |
                                NVSDK_NGX_DLSSG_ResourceFlags_Depth | NVSDK_NGX_DLSSG_ResourceFlags_OutputInterpolated |
                                NVSDK_NGX_DLSSG_ResourceFlags_OutputReal | (FgHlGen() ? hlFlag : 0u) | (uia ? uaFlag : 0u);
        const unsigned never = NVSDK_NGX_DLSSG_ResourceFlags_UI | (uia ? 0u : uaFlag) |
                               NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField | (FgHlGen() ? 0u : hlFlag);
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, always);
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, never);
        // UIR (skladani UI z HUD-less a UI barva/alfa): UI barvu/alfu nedodavame; DlssgUir=1 jen pokus, zda ovladac
        // pak prijme preset B v NVIDIA App (v25t)
        const unsigned uir = g_fgCfg.dlssgUir || uia ? 1u : 0u;
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, uir);
        if (uir) Log("FG: DLSS-G UserInterfaceRecompositionEnabled = 1 (%s)", uia ? "maska UI DlssgUiAlpha" : "[fg] DlssgUir, pokus");
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_AsyncCreateEnabled, 0);
        // "lehci michani" (asi model presetu C ve NVIDIA App: rychlejsi, vic duchu); -1 = nechat na NGX / ovladaci
        if (g_fgCfg.dlssgLite >= 0) {
            NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_LiteCandidateBlendingEnabled, (unsigned)g_fgCfg.dlssgLite);
            Log("FG: DLSS-G LiteCandidateBlendingEnabled = %d ([fg] DlssgLiteBlending)", g_fgCfg.dlssgLite);
        }
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_Width, g_fgBbW);  // jinak varovani "deprecated Width/Height"
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_Height, g_fgBbH);
        BeginInitCmd();
        r = NGX_VK_CREATE_DLSSG(g_initCmd, 1, 1, &g_dlssg, p, &cp);
        vk.vkEndCommandBuffer(g_initCmd);
        const VkResult vr = SubmitInitCmd();
        if (NVSDK_NGX_SUCCEED(r) && g_dlssg && vr == VK_SUCCESS) break;
        Log("FG: NGX_VK_CREATE_DLSSG (DynamicResolutionScaling=%d) selhalo: %s, VkResult %d", (int)dyn,
            NgxStr(r).c_str(), (int)vr);
        if (g_dlssg && vr == VK_SUCCESS) NVSDK_NGX_VULKAN_ReleaseFeature(g_dlssg);
        g_dlssg = nullptr;
        if (vr != VK_SUCCESS || !dyn) break;
        dyn = false;  // druhy pokus bez dynamickeho rozliseni (v casti B pak FG stoji)
    }
    if (parked) FgResume();
    if (!g_dlssg) {
        FgDestroySnaps();
        if (g_fgHagsOff)
            return FgFail("vytvoreni DLSS-G feature selhalo: %s - zapni Hardwarove akcelerovane planovani GPU "
                          "(Nastaveni > System > Obrazovka > Grafika) a restartuj PC", NgxStr(r).c_str());
        return FgFail("vytvoreni DLSS-G feature selhalo: %s", NgxStr(r).c_str());
    }
    QueryPerformanceCounter(&t1);
    g_fgKeyBbW = g_fgBbW;
    g_fgKeyBbH = g_fgBbH;
    g_fgKeyW = g_w;
    g_fgKeyH = g_h;
    g_fgCfg.dynRes = dyn ? 1 : 0;
    g_fgFresh = true;
    g_fgSnapWanted = true;
    Log("FG: DLSS-G feature vytvorena: backbuffer %ux%u (VkFormat %d), MV/hloubka %ux%u, DynamicResolutionScaling=%d, "
        "MatrixTranspose=%d, HUD-less %s, %.0f ms",
        g_fgBbW, g_fgBbH, (int)g_fgFmt, g_w, g_h, (int)dyn, g_fgCfg.matrixTranspose, FgHlGen() ? "ano" : "ne",
        (t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart);
    LogVram("po vytvoreni DLSS-G");
    if (!g_fgDlssgVersionTried) {  // verze skutecne nactene knihovny pro menu (NVIDIA App override nacita vlastni)
        g_fgDlssgVersionTried = true;
        g_fgDlssgVersion = LoadedNgxVersion(L"nvngx_dlssg.dll", L"dlssg", "FG");
        Log("FG: DLSS-G verze: %s", g_fgDlssgVersion.empty() ? "nezjistena" : g_fgDlssgVersion.c_str());
    }
}

// Matice 4x4 po radcich (sloupcovy vektor, clip = M * v) do parametru NGX; guide: "row-major, post-multiplication"
// (v' = v * M) -> vychozi transpozice ([fg] MatrixTranspose=1).
static void FgPutMatrix(float dst[4][4], const double* m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) dst[r][c] = (float)(g_fgCfg.matrixTranspose ? m[c * 4 + r] : m[r * 4 + c]);
}

// Vyhodnoceni do cmd: backbuffer = sdileny obraz k (v SHADER_READ_ONLY), MV a hloubka = snapshot (SHADER_READ_ONLY),
// vystupy = obrazy slotu j (GENERAL).
// hl = HUD-less obrazu k je platny (jinak se s g_fgHlOk predava backbuffer).
// Nasobek (DLSS-MFG, guide "Presenting Multiple Generated Frames"): jedno vyhodnoceni = jeden generovany snimek;
// pro count generovanych snimku na skutecny se vola count-krat s idx = 1..count (vystup interp[idx - 1], cas idx /
// (count + 1) mezi minulym a aktualnim skutecnym snimkem), vstupy stejne. "real" NGX kopiruje pri kazdem volani; predava
// se pokazde (feature je vytvorena s OutputReal v ResourceAlwaysProvided, kopie stoji setiny ms). Reset plati jen
// s idx = 1 (ostatni ho ignoruji).
static NVSDK_NGX_Result FgRecordDlssg(VkCommandBuffer cmd, int k, int j, const FgSnap& sn, bool reset, bool hl, int idx,
                                      int count) {
    const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageSubresourceRange depthRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    NVSDK_NGX_Resource_VK rBB = NVSDK_NGX_Create_ImageView_Resource_VK(g_fgPresView[k], g_fgPres[k].img, colorRange,
                                                                       g_fgFmt, g_fgBbW, g_fgBbH, false);
    NVSDK_NGX_Resource_VK rDep = NVSDK_NGX_Create_ImageView_Resource_VK(
        sn.depth.view, sn.depth.img, depthRange, g_depthFmt, g_w, g_h, false);
    NVSDK_NGX_Resource_VK rMv =
        NVSDK_NGX_Create_ImageView_Resource_VK(sn.mv.view, sn.mv.img, colorRange, kMvFormat, g_w, g_h, false);
    FgOut& o = g_fgOut[j];
    const OwnImg& oi = o.interp[idx - 1];
    NVSDK_NGX_Resource_VK rInt =
        NVSDK_NGX_Create_ImageView_Resource_VK(oi.view, oi.img, colorRange, g_fgFmt, g_fgBbW, g_fgBbH, true);
    NVSDK_NGX_Resource_VK rReal =
        NVSDK_NGX_Create_ImageView_Resource_VK(o.real.view, o.real.img, colorRange, g_fgFmt, g_fgBbW, g_fgBbH, true);
    NVSDK_NGX_Resource_VK rHl = hl ? NVSDK_NGX_Create_ImageView_Resource_VK(g_fgHlView[k], g_fgHl[k].img, colorRange,
                                                                            g_fgFmt, g_fgBbW, g_fgBbH, false)
                                   : rBB;
    const VkImageSubresourceRange uaRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    NVSDK_NGX_Resource_VK rUa = NVSDK_NGX_Create_ImageView_Resource_VK(g_uaImg.view, g_uaImg.img, uaRange,
                                                                       VK_FORMAT_R8_UNORM, g_fgBbW, g_fgBbH, false);
    const bool uia = g_uaOk && FgHlGen();  // i bez HUD-less snimku (prazdna maska, FgUiaRecord)
    NVSDK_NGX_VK_DLSSG_Eval_Params ep = {&rBB, &rDep, &rMv, FgHlGen() ? &rHl : nullptr, nullptr, uia ? &rUa : nullptr,
                                         nullptr, &rInt, &rReal, nullptr};
    NVSDK_NGX_DLSSG_Opt_Eval_Params op = NVSDK_NGX_DLSSG_Opt_Eval_Params{};
    op.multiFrameCount = (unsigned)count;
    op.multiFrameIndex = (unsigned)idx;
    const double I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    double C2P[16], P2C[16], V2C[16] = {}, C2V[16];
    memcpy(C2P, I, sizeof I);
    memcpy(P2C, I, sizeof I);
    float n = 7.0f, f = 28000.0f, fov = 1.02f;  // zaloha jako u FSR (Source/BMS)
    const float aspect = (float)g_fgBbW / (float)g_fgBbH;
    float rep[16];
    if (sn.camOk && BuildReprojection(sn.cam, sn.prevCam, rep)) {
        for (int i = 0; i < 16; ++i) C2P[i] = rep[i];
        if (!Invert4(C2P, P2C)) {
            memcpy(C2P, I, sizeof I);
            memcpy(P2C, I, sizeof I);
        }
    }
    if (sn.camOk) CameraNearFarFov(sn.cam, &n, &f, &fov);
    {
        // perspektiva D3D LH (konvence cViewProj): x' = x / (a * tan), y' = y / tan, z' = A z + B, w' = z
        const double ty = tan((double)fov * 0.5), A = (double)f / ((double)f - n), B = -(double)n * f / ((double)f - n);
        V2C[0] = 1.0 / (aspect * ty);
        V2C[5] = 1.0 / ty;
        V2C[10] = A;
        V2C[11] = B;
        V2C[14] = 1.0;
        if (!Invert4(V2C, C2V)) memcpy(C2V, I, sizeof I);
    }
    FgPutMatrix(op.clipToPrevClip, C2P);
    FgPutMatrix(op.prevClipToClip, P2C);
    FgPutMatrix(op.cameraViewToClip, V2C);
    FgPutMatrix(op.clipToCameraView, C2V);
    FgPutMatrix(op.clipToLensClip, I);
    op.jitterOffset[0] = op.jitterOffset[1] = 0.0f;  // backbuffer i MV jsou bez jitteru
    op.mvecScale[0] = g_cfg.mvScaleX;                // MV v pixelech obrazu MV (minula - aktualni pozice)
    op.mvecScale[1] = g_cfg.mvScaleY;
    op.cameraPinholeOffset[0] = op.cameraPinholeOffset[1] = 0.0f;
    op.cameraUp[1] = 1.0f;
    op.cameraRight[0] = 1.0f;
    op.cameraFwd[2] = 1.0f;
    if (sn.camOk) {
        // oko = inverse(cam) * (0, 0, 1, 0) / w; osy kamery = linearni casti radku x, y, w (viz CameraMotion)
        double C[16], Ci[16];
        for (int i = 0; i < 16; ++i) C[i] = sn.cam[i];
        if (Invert4(C, Ci) && fabs(Ci[14]) > 1e-12) {
            for (int a = 0; a < 3; ++a) op.cameraPos[a] = (float)(Ci[a * 4 + 2] / Ci[14]);
            float* axes[3] = {op.cameraRight, op.cameraUp, op.cameraFwd};
            const int rows[3] = {0, 1, 3};
            for (int a = 0; a < 3; ++a) {
                const float* row = sn.cam + rows[a] * 4;
                const double len = sqrt((double)row[0] * row[0] + (double)row[1] * row[1] + (double)row[2] * row[2]);
                if (len > 1e-12)
                    for (int c = 0; c < 3; ++c) axes[a][c] = (float)(row[c] / len);
            }
        }
    }
    op.cameraNear = n;
    op.cameraFar = f;
    op.cameraFOV = fov;
    op.cameraAspectRatio = aspect;
    op.colorBuffersHDR = false;
    op.depthInverted = false;  // 0 = near
    op.cameraMotionIncluded = true;
    op.reset = reset;
    op.automodeOverrideReset = false;
    op.notRenderingGameFrames = false;
    op.orthoProjection = false;
    op.motionVectorsInvalidValue = 0.0f;
    op.motionVectorsDilated = false;
    op.menuDetectionEnabled = g_fgCfg.menuDetect != 0;
    op.mvecsSubrectSize = {sn.rW, sn.rH};
    op.depthSubrectSize = {sn.rW, sn.rH};
    NVSDK_NGX_Parameter_SetULL(g_fgParams, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, g_fgFrameId);
    NVSDK_NGX_Parameter_SetUI(g_fgParams, NVSDK_NGX_DLSSG_Parameter_MvecJittered, 0);
    return NGX_VK_EVALUATE_DLSSG(cmd, g_dlssg, g_fgParams, &ep, &op);
}

// Vyhodnoceni FSR FG do cmd (ffxConfigure + PrepareV2 + FrameGeneration; vse se jen zaznamenava do naseho cmd, zadny
// vlastni submit). Stavy jako u DLSS-G: backbuffer = sdileny obraz k, MV a hloubka = snapshot (SHADER_READ_ONLY =
// COMPUTE_READ), vystup = interp slotu j (GENERAL = UNORDERED_ACCESS); ffx je po dispatchi vrati do tehoz stavu.
// "real" si FSR nevyrabi (kopii presImg -> real dela volajici). Konvence jako FSR upscaler (DispatchFsr): MV = minula -
// aktualni pozice v pixelech renderu, hloubka 0 = near, D24S8 jako R32_UINT + DEPTH|STENCIL, 1 jednotka = 1 palec.
// Bez ffxConfigure se stejnym frameID by dispatch vratil 0, ale vystup by byl jen kopie. frameID = g_fgFrameId roste
// o 1 za kazdy skutecny snimek; mezera (pauza) = provider sam resetuje. HUD-less (K5): HUDLessColor = obraz od proxy
// (hl), jinak s g_fgHlOk backbuffer (UI = rozdil = nic), bez obrazu prazdne (jako v13). Format = backbuffer, takze
// ffxCreateContextDescFrameGenerationHudless netreba. *stage = ktere volani selhalo.
static ffxReturnCode_t FgRecordFsr(VkCommandBuffer cmd, int k, int j, const FgSnap& sn, bool reset, bool hl,
                                   const char** stage) {
    const uint32_t READ = FFX_API_RESOURCE_STATE_COMPUTE_READ;
    ffxConfigureDescFrameGeneration c = {};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = nullptr;  // zadny ffx swapchain: prezentuje nase vlakno presentu
    c.frameGenerationEnabled = true;
    c.allowAsyncWorkloads = false;
    c.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    c.onlyPresentGenerated = false;
    c.generationRect = {0, 0, (int32_t)g_fgBbW, (int32_t)g_fgBbH};
    c.frameID = g_fgFrameId;
    if (FgHlGen())
        c.HUDLessColor = FfxImage(hl ? g_fgHl[k].img : g_fgPres[k].img, g_fgFmt, g_fgBbW, g_fgBbH,
                                  FFX_API_RESOURCE_USAGE_READ_ONLY, READ);
    *stage = "ffxConfigure";
    ffxReturnCode_t r = g_ffxConfigure(&g_ffg, &c.header);
    if (r != FFX_API_RETURN_OK) return r;

    ffxDispatchDescFrameGenerationPrepareV2 p = {};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = g_fgFrameId;
    p.flags = 0;
    p.commandList = cmd;
    p.renderSize = {sn.rW, sn.rH};  // cast B: sub-rect od (0, 0) v obrazech g_w x g_h
    p.jitterOffset = {0.0f, 0.0f};  // backbuffer i MV jsou bez jitteru
    p.motionVectorScale = {g_cfg.mvScaleX, g_cfg.mvScaleY};
    p.frameTimeDelta = (float)g_fgIntervalMs;
    p.reset = reset;
    float n = 7.0f, f = 28000.0f, fov = 1.02f;  // zaloha jako u FSR upscaleru (Source / BMS)
    if (sn.camOk) CameraNearFarFov(sn.cam, &n, &f, &fov);
    p.cameraNear = n;
    p.cameraFar = f;
    p.cameraFovAngleVertical = fov;
    p.viewSpaceToMetersFactor = 0.0254f;
    p.depth = FfxImage(sn.depth.img, g_depthFmt, g_w, g_h,
                       FFX_API_RESOURCE_USAGE_DEPTHTARGET | FFX_API_RESOURCE_USAGE_STENCILTARGET, READ);
    p.motionVectors = FfxImage(sn.mv.img, kMvFormat, g_w, g_h, FFX_API_RESOURCE_USAGE_READ_ONLY, READ);
    p.cameraUp[1] = 1.0f;
    p.cameraRight[0] = 1.0f;
    p.cameraForward[2] = 1.0f;
    if (sn.camOk) {
        // oko = inverse(cam) * (0, 0, 1, 0) / w; osy kamery = linearni casti radku x, y, w (stejne jako FgRecordDlssg)
        double C[16], Ci[16];
        for (int i = 0; i < 16; ++i) C[i] = sn.cam[i];
        if (Invert4(C, Ci) && fabs(Ci[14]) > 1e-12) {
            for (int a = 0; a < 3; ++a) p.cameraPosition[a] = (float)(Ci[a * 4 + 2] / Ci[14]);
            float* axes[3] = {p.cameraRight, p.cameraUp, p.cameraForward};
            const int rows[3] = {0, 1, 3};
            for (int a = 0; a < 3; ++a) {
                const float* row = sn.cam + rows[a] * 4;
                const double len = sqrt((double)row[0] * row[0] + (double)row[1] * row[1] + (double)row[2] * row[2]);
                if (len > 1e-12)
                    for (int cc = 0; cc < 3; ++cc) axes[a][cc] = (float)(row[cc] / len);
            }
        }
    }
    *stage = "ffxDispatch(PrepareV2)";
    r = g_ffxDispatch(&g_ffg, &p.header);
    if (r != FFX_API_RETURN_OK) return r;

    ffxDispatchDescFrameGeneration d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    d.commandList = cmd;
    d.presentColor = FfxImage(g_fgPres[k].img, g_fgFmt, g_fgBbW, g_fgBbH, FFX_API_RESOURCE_USAGE_READ_ONLY, READ);
    d.outputs[0] = FfxImage(g_fgOut[j].interp[0].img, g_fgFmt, g_fgBbW, g_fgBbH, FFX_API_RESOURCE_USAGE_UAV,
                            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.numGeneratedFrames = 1;
    d.reset = reset;
    d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;  // LDR sRGB (minMaxLuminance se nepouzije)
    d.generationRect = {0, 0, (int32_t)g_fgBbW, (int32_t)g_fgBbH};
    d.frameID = g_fgFrameId;
    *stage = "ffxDispatch(FrameGeneration)";
    return g_ffxDispatch(&g_ffg, &d.header);
}

// Snapshot vstupu generatoru pro snimek mostu B: vola ProcessFrame po bariere "post" (MV v SHADER_READ_ONLY, DEPTH_IN
// v GENERAL, oba platne v sub-rectu g_rW x g_rH). Bez FG (g_fgSnapWanted = false) nic.
// Obraz MV snimku mostu B (vola ProcessFrame pred mv.comp, jen DLAA s presenterem): se snapshotem rovnou obraz MV
// snapshotu B % 3 a jeho set 1 - FgSnapshot pak MV nekopiruje. Jinak beze zmeny (g_mvImg).
static void FgMvTarget(uint64_t B, OwnImg** img, VkDescriptorSet* set) {
    if (!g_fgSnapWanted || !g_fgSnapOk) return;
    FgSnap& s = g_fgSnap[B % kFgSnap];
    if (!s.mvSet || !s.mv.img) return;
    s.valid = false;  // obraz se prave prepisuje (platny az po FgSnapshot tohoto snimku)
    *img = &s.mv;
    *set = s.mvSet;
}

static void FgSnapshot(VkCommandBuffer cmd, uint64_t B, bool camOk, bool reset) {
    if (!g_fgSnapWanted || !g_fgSnapOk) return;
    FgSnap& s = g_fgSnap[B % kFgSnap];
    const bool mvDirect = g_mvCur == &s.mv;  // MV uz zapsal mv.comp (FgMvTarget), v SHADER_READ_ONLY po bariere "mid"
    const VkImageAspectFlags COLOR = VK_IMAGE_ASPECT_COLOR_BIT, DEPTH = VK_IMAGE_ASPECT_DEPTH_BIT;
    const VkImageAspectFlags DS = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    const VkImageLayout SRO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, SRC = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        DST = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, XFER = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkImageMemoryBarrier pre[3] = {
        IB(s.depth.img, DS, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, DST),
        IB(g_mvImg.img, COLOR, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, SRO, SRC),
        IB(s.mv.img, COLOR, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, DST)};
    PB(cmd, ALL, XFER, mvDirect ? 1 : 3, pre);
    VkImageCopy c = {};
    c.extent = {g_rW, g_rH, 1};
    if (!mvDirect) {
        c.srcSubresource = {COLOR, 0, 0, 1};
        c.dstSubresource = {COLOR, 0, 0, 1};
        vk.vkCmdCopyImage(cmd, g_mvImg.img, SRC, s.mv.img, DST, 1, &c);
    }
    c.srcSubresource = {DEPTH, 0, 0, 1};
    c.dstSubresource = {DEPTH, 0, 0, 1};
    vk.vkCmdCopyImage(cmd, g_img[BMSD_IMG_DEPTH_IN].img, VK_IMAGE_LAYOUT_GENERAL, s.depth.img, DST, 1, &c);
    VkImageMemoryBarrier post[2] = {
        IB(s.depth.img, DS, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, DST, SRO),
        IB(s.mv.img, COLOR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, DST, SRO)};
    PB(cmd, XFER, ALL, mvDirect ? 1 : 2, post);
    s.B = B;
    s.valid = true;
    s.reset = reset;
    s.camOk = camOk;
    memcpy(s.cam, g_sh->cam, sizeof s.cam);
    memcpy(s.prevCam, g_sh->prevCam, sizeof s.prevCam);
    s.rW = g_rW;
    s.rH = g_rH;
}

// ---------------------------------------------------------------------------------------------------------------
// Ladeni: ulozeni obrazu (BMP 32 bit, shora dolu)
// ---------------------------------------------------------------------------------------------------------------
static bool FgDumpImage(VkImage img, uint32_t w, uint32_t h, const wchar_t* path) {
    const VkDeviceSize size = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vk.vkCreateBuffer(g_dev, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req = {};
    vk.vkGetBufferMemoryRequirements(g_dev, buf, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                               HostVisibleType(req.memoryTypeBits)};
    bool ok = ai.memoryTypeIndex != UINT32_MAX && vk.vkAllocateMemory(g_dev, &ai, nullptr, &mem) == VK_SUCCESS &&
              vk.vkBindBufferMemory(g_dev, buf, mem, 0) == VK_SUCCESS;
    if (ok) {
        BeginInitCmd();
        VkBufferImageCopy r = {};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        fvk.vkCmdCopyImageToBuffer(g_initCmd, img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
        vk.vkEndCommandBuffer(g_initCmd);
        ok = SubmitInitCmd() == VK_SUCCESS;
    }
    void* p = nullptr;
    if (ok) ok = vk.vkMapMemory(g_dev, mem, 0, size, 0, &p) == VK_SUCCESS && p;
    if (ok) {
        FILE* fp = _wfopen(path, L"wb");
        ok = fp != nullptr;
        if (fp) {
            BITMAPFILEHEADER fh = {};
            BITMAPINFOHEADER ih = {};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = (DWORD)(fh.bfOffBits + size);
            ih.biSize = sizeof ih;
            ih.biWidth = (LONG)w;
            ih.biHeight = -(LONG)h;
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fwrite(&fh, sizeof fh, 1, fp);
            fwrite(&ih, sizeof ih, 1, fp);
            if (g_fgFmt == VK_FORMAT_B8G8R8A8_UNORM) {
                fwrite(p, 1, (size_t)size, fp);
            } else {  // RGBA -> BGRA
                std::vector<uint8_t> row((size_t)w * 4);
                const uint8_t* s = (const uint8_t*)p;
                for (uint32_t y = 0; y < h; ++y, s += (size_t)w * 4) {
                    for (uint32_t x = 0; x < w; ++x) {
                        row[x * 4] = s[x * 4 + 2];
                        row[x * 4 + 1] = s[x * 4 + 1];
                        row[x * 4 + 2] = s[x * 4];
                        row[x * 4 + 3] = s[x * 4 + 3];
                    }
                    fwrite(row.data(), 1, row.size(), fp);
                }
            }
            fclose(fp);
        }
        fvk.vkUnmapMemory(g_dev, mem);
    }
    if (buf) fvk.vkDestroyBuffer(g_dev, buf, nullptr);
    if (mem) vk.vkFreeMemory(g_dev, mem, nullptr);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Hlavni vlakno: stav, statistika, zpravy
// ---------------------------------------------------------------------------------------------------------------
static void FgPublishState(int32_t state, uint32_t activeMode, const char* msg) {
    BmsdPresentShared* ps = g_ps;
    if (!ps) return;
    if (!msg) msg = "";
    if (strncmp(ps->fgMsg, msg, sizeof ps->fgMsg - 1)) {
        strncpy(ps->fgMsg, msg, sizeof ps->fgMsg - 1);
        ps->fgMsg[sizeof ps->fgMsg - 1] = 0;
    }
    ps->fgAvailMask = 1u | (g_fgAvail && !g_fgFailedM[BMSD_FG_DLSSG] ? 1u << BMSD_FG_DLSSG : 0u) |
                      (g_ffgAvail && !g_fgFailedM[BMSD_FG_FSR2X] ? 1u << BMSD_FG_FSR2X : 0u) |
                      (g_afgAvail && !g_fgFailedM[BMSD_FG_FSRADAPT] ? 1u << BMSD_FG_FSRADAPT : 0u);
    ps->fgAvailValid = g_fgAvailKnown ? 1 : 0;
    ps->adaptEngaged = activeMode == BMSD_FG_FSRADAPT && g_ad.engaged ? 1u : 0u;
    {  // nazev a verze generatoru, ktery existuje (menu: "DLSS-G 310.9.16 · 78 -> 156 FPS", "FSR FG 3.1.6 · ...")
        const std::string name = g_afgOk    ? std::string(ps->fgMode == BMSD_FG_FSR2X ? "FSR FG 3.1.4 (mod)" : "FSR adapt. 3.1.4")
                                 : g_ffg    ? (g_ffgVersion.empty() ? std::string("FSR FG") : "FSR FG " + g_ffgVersion)
                                 : !g_dlssg ? std::string()
                                 : g_fgDlssgVersion.empty() ? std::string("DLSS-G") : "DLSS-G " + g_fgDlssgVersion;
        if (strncmp(ps->fgName, name.c_str(), sizeof ps->fgName - 1)) {
            strncpy(ps->fgName, name.c_str(), sizeof ps->fgName - 1);
            ps->fgName[sizeof ps->fgName - 1] = 0;
        }
    }
    ps->fgActiveMode = activeMode;
    ps->fgActiveMult = activeMode ? (uint32_t)g_fgActMult : 0u;
    ps->fgMultMax = g_fgAvailKnown && g_fgAvail ? (uint32_t)g_fgMultMax : g_fgAvailKnown ? 2u : 0u;
    MemoryBarrier();
    ps->fgState = state;
    if (state != g_fgLoggedState || g_fgLoggedMsg != msg) {
        static const char* names[] = {"selhalo", "nedostupne", "vypnuto", "generuje", "pozastaveno"};
        // kratke prepnuti generuje <-> pozastaveno (reset pri strihu) se neloguje pokazde
        static unsigned s_logs;
        if (++s_logs <= 40 || s_logs % 200 == 0 || state <= 0)
            Log("FG: stav %s%s%s", names[state + 2], *msg ? " - " : "", msg);
        g_fgLoggedState = state;
        g_fgLoggedMsg = msg;
    }
}

static void FgCollectTs(int c) {
    if (!g_fgTsUsed[c] || !g_fgQpool) return;
    g_fgTsUsed[c] = false;
    uint64_t t[2] = {};
    if (vk.vkGetQueryPoolResults(g_dev, g_fgQpool, c * 2, 2, sizeof t, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) !=
        VK_SUCCESS)
        return;
    if (t[1] >= t[0]) {
        const double ms = (double)(t[1] - t[0]) * g_tsPeriod * 1e-6;
        g_fgSt.eval.add(ms);
        if (g_fgTsGen[c] >= 0 && ms < 100.0) {
            const double x = (double)g_fgTsGen[c];
            g_afgRegN += 1.0;
            g_afgRegSx += x;
            g_afgRegSy += ms;
            g_afgRegSxx += x * x;
            g_afgRegSxy += x * ms;
        }
    }
    g_fgTsGen[c] = -1;
}

static void FgPublishStats(LONGLONG now) {
    BmsdPresentShared* ps = g_ps;
    if (!ps) return;
    if (!g_fgStatQpc) g_fgStatQpc = now;
    const double el = FgMs(g_fgStatQpc, now);
    if (el < 5000.0) return;
    unsigned real, gen, recreate, late, dropped, behind, genFail, catchupN;
    FgAcc interval, pres, done, ivCall, lat, gpuDone;
    unsigned minIvWait, ivShort, adLate, adDrop, gpBusy;
    FgAcc dIv, dLat, dCont, dJud, dJudAbs;
    unsigned dN, dDbl, dLong, dLate, dGrid, dGridN, dNoTime, dSkipReq;
    {
        std::lock_guard<std::mutex> lk(g_fgStatMx);
        ivCall = g_ptSt.ivCall;
        lat = g_ptSt.lat;
        minIvWait = g_ptSt.minIvWait;
        ivShort = g_ptSt.ivShort;
        adLate = g_ptSt.adLate;
        adDrop = g_ptSt.adDrop;
        gpBusy = g_ptSt.gpBusy;
        g_ptSt.gpBusy = 0;
        dIv = g_ptSt.dIv, dLat = g_ptSt.dLat, dCont = g_ptSt.dCont, dJud = g_ptSt.dJud, dJudAbs = g_ptSt.dJudAbs;
        dN = g_ptSt.dN, dDbl = g_ptSt.dDbl, dLong = g_ptSt.dLong, dLate = g_ptSt.dLate, dGrid = g_ptSt.dGrid;
        dGridN = g_ptSt.dGridN, dNoTime = g_ptSt.dNoTime, dSkipReq = g_ptSt.dSkipReq;
        g_ptSt.dIv = g_ptSt.dLat = g_ptSt.dCont = g_ptSt.dJud = g_ptSt.dJudAbs = FgAcc();
        g_ptSt.dN = g_ptSt.dDbl = g_ptSt.dLong = g_ptSt.dLate = g_ptSt.dGrid = g_ptSt.dGridN = g_ptSt.dNoTime =
            g_ptSt.dSkipReq = 0;
        g_ptSt.ivCall = g_ptSt.lat = FgAcc();
        g_ptSt.minIvWait = g_ptSt.ivShort = g_ptSt.adLate = g_ptSt.adDrop = 0;
        real = g_ptSt.real;
        gen = g_ptSt.gen;
        recreate = g_ptSt.recreate;
        late = g_ptSt.late;
        dropped = g_ptSt.dropped;
        behind = g_ptSt.behind;
        genFail = g_ptSt.genFail;
        catchupN = g_ptSt.catchup;
        g_ptSt.catchup = 0;
        interval = g_ptSt.interval;
        pres = g_ptSt.pres;
        done = g_ptSt.done;
        gpuDone = g_ptSt.gpuDone;
        g_ptSt.gpuDone = FgAcc();
        g_ptSt.real = g_ptSt.gen = g_ptSt.recreate = g_ptSt.late = g_ptSt.dropped = g_ptSt.behind = g_ptSt.genFail = 0;
        g_ptSt.interval = g_ptSt.pres = g_ptSt.done = FgAcc();
    }
    ps->statFrames = g_fgSt.frames;
    ps->statPresented = real + gen;
    ps->statRecreate = recreate;
    ps->statWakeMs = (float)g_fgSt.wake.avg();
    ps->statWakeMax = (float)g_fgSt.wake.max;
    ps->statDoneMs = (float)done.avg();
    ps->statDoneMax = (float)done.max;
    ps->statPresentMs = (float)pres.avg();
    ps->statPresentMax = (float)pres.max;
    ps->statIntervalMs = (float)interval.avg();
    ps->statIntervalMax = (float)interval.max;
    ps->statFgReal = real;
    ps->statFgGen = gen;
    ps->statFgPaired = g_fgSt.paired;
    ps->statFgUnpaired = g_fgSt.unpaired;
    ps->statFgEvalMs = (float)g_fgSt.eval.avg();
    ps->statFgEvalMax = (float)g_fgSt.eval.max;
    ps->statFgIntSd = (float)interval.sd();
    ps->statFgRealFps = (float)(g_fgSt.frames * 1000.0 / el);
    ps->statFgOutFps = (float)((real + gen) * 1000.0 / el);
    ps->statFgHudless = g_fgSt.hudless;
    MemoryBarrier();
    ++ps->statSeq;
    if (g_fgCfg.log && g_fgSemReady) {  // ladeni: stav semaforu a fence (zaseknuti GPU)
        uint64_t ev = 0, rd = 0, dn = 0;
        fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemEval, &ev);
        fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemReady, &rd);
        fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemDone, &dn);
        uint64_t sp = 0, shh = 0;
        if (g_semP) fvk.vkGetSemaphoreCounterValue(g_dev, g_semP, &sp);
        if (g_semH) fvk.vkGetSemaphoreCounterValue(g_dev, g_semH, &shh);
        char fs[64] = "";
        for (int i = 0; i < kSlots; ++i)
            snprintf(fs + strlen(fs), sizeof fs - strlen(fs), " %d/%d", g_fgFenceUsed[i] ? (int)fvk.vkGetFenceStatus(g_dev, g_fgFence[i]) : -9,
                     g_fenceUsed[i] ? (int)fvk.vkGetFenceStatus(g_dev, g_fence[i]) : -9);
        Log("FG ladeni: posledni N %llu (polozka %llu), semafory: hotovo %llu, proxy zkopirovala %llu, prevzato %llu | "
            "most: semP %llu, semH %llu, frameValue %llu | fence present/most (0 = hotovo, 1 = ceka, -9 = nepouzita):%s",
            (unsigned long long)g_fgLastN, (unsigned long long)g_fgSeq, (unsigned long long)ev, (unsigned long long)rd,
            (unsigned long long)dn, (unsigned long long)sp, (unsigned long long)shh,
            (unsigned long long)g_sh->frameValue, fs);
    }
    if (g_fgSt.frames || real || gen) {
        const unsigned tot = g_fgSt.paired + g_fgSt.unpaired;
        Log("FG vykon %.1f s: nasobek %dx, prevzato %u (%.1f/s), zobrazeno %u skutecnych + %u generovanych (%.1f/s), %s %ux%u, "
            "swapchain znovu %u | parovani B %u/%u (%.1f %%), vyhodnoceni GPU %.2f / %.2f ms (%u) | interval presentu "
            "%.2f ms, sd %.3f, max %.2f | vkQueuePresent %.3f / %.2f ms | post -> hotovo na GPU %.2f / %.2f ms, "
            "GPU hotovo (mereno) %.2f / %.2f ms (%u), probuzeni %.3f / %.2f ms | pozde %u, zahozeno %u, generovany vynechan %u (pozadu) + %u (nezobrazen), dohnano %u, I %.2f ms"
            " | HUD-less %u / %u vyhodnoceni%s",
            el / 1000.0, g_fgLastMult ? g_fgLastMult : 2, g_fgSt.frames, g_fgSt.frames * 1000.0 / el, real, gen, (real + gen) * 1000.0 / el,
            FgModeStr((VkPresentModeKHR)(g_gpOn ? ps->gpPresentMode : ps->presentMode)), g_gpOn ? ps->gpSwapW : ps->swapW,
            g_gpOn ? ps->gpSwapH : ps->swapH, recreate, g_fgSt.paired, tot,
            tot ? g_fgSt.paired * 100.0 / tot : 0.0, g_fgSt.eval.avg(), g_fgSt.eval.max, g_fgSt.eval.n, interval.avg(),
            interval.sd(), interval.max, pres.avg(), pres.max, done.avg(), done.max, gpuDone.avg(), gpuDone.max, gpuDone.n, g_fgSt.wake.avg(),
            g_fgSt.wake.max, late, dropped, behind, genFail, catchupN, g_fgIntervalMs, g_fgSt.hudless, g_fgSt.evals,
            g_fgHlOk ? "" : " (obrazy HUD-less nejsou)");
        const double minIvMs = g_ptMinIvQ > 0 ? FgMs(0, g_ptMinIvQ) : 0.0;
        Log("FG rozvrh %.1f s: interval zacatku presentu %.3f ms, sd %.3f, max %.2f | nejkratsi povoleny %.3f ms (strop "
            "VRR): cekal %u x, kratsi %u x | latence obsah -> present %.2f ms (sd %.2f, max %.1f) | zobrazuje %s%s",
            el / 1000.0, ivCall.avg(), ivCall.sd(), ivCall.max, minIvMs, minIvWait, ivShort, lat.avg(), lat.sd(), lat.max,
            g_gpOn ? "proces hry (VRR)" : "helper (vlastni swapchain)",
            g_gpOn && gpBusy ? (std::string(", obraz jeste u proxy ") + std::to_string(gpBusy) + " x").c_str() : "");
        if (g_ptmDumpFull && !g_ptmDumpDone) {  // ladeni: CSV casu zobrazeni (vlakno presentu uz do vektoru nepise)
            g_ptmDumpDone = true;
            const std::wstring fn = g_dir + L"\\fg_present_timing.csv";
            if (FILE* f = _wfopen(fn.c_str(), L"w")) {
                fprintf(f, "id,call_ms,display_ms,content_ms,gen,qpf\n");
                const LONGLONG t0 = g_ptmDump.empty() ? 0 : g_ptmDump[0].call;
                for (const PtmDumpRec& d : g_ptmDump)
                    fprintf(f, "%llu,%.4f,%.4f,%.4f,%d,%lld\n", (unsigned long long)d.id, FgMs(t0, d.call), FgMs(t0, d.F),
                            d.content ? FgMs(t0, d.content) : 0.0, d.gen, (long long)g_qpf.QuadPart);
                fclose(f);
                Log("FG: casy zobrazeni (%zu) ulozeny do fg_present_timing.csv", g_ptmDump.size());
            }
        }
        if (g_ptmFuncs && (dN || dNoTime || dSkipReq)) {
            // VRR z namerenych casu: pevna frekvence = intervaly zobrazeni jsou nasobky periody obnovy
            const double grid = dGridN ? dGrid * 100.0 / dGridN : 0.0;
            Log("FG zobrazeni %.1f s: snimku s casem %u (bez casu %u, bez dotazu %u) | interval zobrazeni %.3f ms, sd %.3f, "
                "min %.2f, max %.2f | double tap %u, dlouhy %u, pozde %u | volani -> prvni pixel %.2f ms (sd %.3f, min %.2f, max "
                "%.2f) | chyba casu obsahu: sd %.3f ms, |prum| %.3f, max %.2f; zobrazeni - obsah %.2f ms (sd %.2f) | na "
                "mrizce obnovy %.1f %% (%u, perioda %.4f ms) -> %s",
                el / 1000.0, dN, dNoTime, dSkipReq, dIv.avg(), dIv.sd(), dIv.n ? dIv.min : 0.0, dIv.max, dDbl,
                dLong, dLate, dLat.avg(), dLat.sd(), dLat.n ? dLat.min : 0.0, dLat.max, dJud.sd(), dJudAbs.avg(), dJudAbs.max, dCont.avg(),
                dCont.sd(), grid, dGridN, g_ptmREst > 0.0 ? g_ptmREst : g_ptmRefreshMs,
                dGridN < 50 ? "VRR ?" : grid > 80.0 ? "pevna frekvence (bez VRR)" : grid < 40.0 ? "VRR" : "nejasne");
        }
        if (ps->fgMode == BMSD_FG_FSRADAPT && g_afgOk) {
            const unsigned tot = real + gen;
            char vbInfo[96] = "";
            if (g_fgCfg.vblankGrid)
                snprintf(vbInfo, sizeof vbInfo, " | vblank: perioda %.4f ms, chyba faze %.3f ms (max %.2f, %u)",
                         (double)g_vbPeriodMs, g_ad.vbErr.avg(), g_ad.vbErr.max, g_ad.vbErr.n);
            double regA = 0.0, regB = 0.0;  // regrese cas = A + B * n (par + mezisnimky)
            {
                const double d = g_afgRegN * g_afgRegSxx - g_afgRegSx * g_afgRegSx;
                if (g_afgRegN >= 10 && fabs(d) > 1e-9) {
                    regB = (g_afgRegN * g_afgRegSxy - g_afgRegSx * g_afgRegSy) / d;
                    regA = (g_afgRegSy - regB * g_afgRegSx) / g_afgRegN;
                }
            }
            Log("FG adaptivni %.1f s: rozvrh %s, cil %.2f FPS, %s, skutecne %.1f FPS (EMA), zobrazeno %u skutecnych + %u "
                "generovanych (%.0f %% gen.) | prepnuti %u, zobrazeno na skutecny %.2f, vypnuto pro neuzitecnost %u | A: slotu %u (skutecny %u, zahozeno %u), L %.2f ms (max %.2f), "
                "pozde %u, zahozeno %u | odchylka obsahu slotu %.3f ms (max %.2f), prichyceno %u "
                "(%.3f / %.2f ms) | GPU generatoru: par %.2f ms + %.2f ms na mezisnimek (regrese, %u)%s",
                el / 1000.0, "A", ps->adaptTargetMilli / 1000.0,
                g_ad.engaged ? "generuje" : "jen skutecne", g_ad.ivEma > 0 ? 1000.0 / g_ad.ivEma : 0.0, real, gen,
                tot ? gen * 100.0 / tot : 0.0, g_ad.switches, g_ad.lastGain, g_ad.uselessOff, g_ad.slots, g_ad.slotsReal, g_ad.slotsDrop, g_ad.Lacc.avg(),
                g_ad.Lacc.max, adLate, adDrop, g_ad.contErr.avg(), g_ad.contErr.max, g_ad.snapErr.n,
                g_ad.snapErr.avg(), g_ad.snapErr.max, regA, regB, (unsigned)g_afgRegN, vbInfo);
            g_afgRegN = g_afgRegSx = g_afgRegSy = g_afgRegSxx = g_afgRegSxy = 0.0;
        }
    }
    g_ad.switches = g_ad.slots = g_ad.slotsReal = g_ad.slotsDrop = 0;
    g_ad.Lacc = g_ad.snapErr = g_ad.contErr = g_ad.vbErr = FgAcc();
    g_fgSt = {};
    g_fgStatQpc = now;
}

// Uplne uvolneni zdroju presentu (po zastaveni vlakna presentu nebo na konci). GPU musi jit uklidnit.
static void FgFreeAll(const char* why) {
    if (g_fgFreed) return;
    g_fgFreed = true;
    FgQuiesce();
    if (FgWaitGpuIdle()) {
        FgReleaseFeature(why);
        FgDestroySnaps();
        FgDestroyOut();
        FgDestroyPresImages();
        Log("prezentace: zdroje uvolneny (%s)", why);
    } else {
        Log("prezentace: GPU nedobehla, zdroje neuvolnuji (%s)", why);
    }
    FgResume();
}

// Proxy zada konec prezentace (zaloha) nebo presenter selhal: vlakno presentu uvolni swapchain, zdroje pryc.
static void FgStop(const char* why) {
    if (g_fgDead) return;
    g_fgDead = true;
    g_fgSnapWanted = false;
    Log("prezentace: konec (%s)", why);
    {  // diagnostika: je zarizeni helperu v poradku? (stav fence mostu a presentu, semafory)
        char fs[96] = "";
        for (int i = 0; i < kSlots; ++i)
            snprintf(fs + strlen(fs), sizeof fs - strlen(fs), " %d/%d",
                     g_fgFenceUsed[i] ? (int)fvk.vkGetFenceStatus(g_dev, g_fgFence[i]) : -9,
                     g_fenceUsed[i] ? (int)fvk.vkGetFenceStatus(g_dev, g_fence[i]) : -9);
        uint64_t ev = 0, rd = 0, dn = 0, sp = 0, shh = 0;
        const VkResult r1 = g_fgSemEval ? fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemEval, &ev) : VK_SUCCESS;
        if (g_fgSemReady) fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemReady, &rd);
        if (g_fgSemDone) fvk.vkGetSemaphoreCounterValue(g_dev, g_fgSemDone, &dn);
        if (g_semP) fvk.vkGetSemaphoreCounterValue(g_dev, g_semP, &sp);
        if (g_semH) fvk.vkGetSemaphoreCounterValue(g_dev, g_semH, &shh);
        Log("prezentace: stav pri konci: posledni N %llu (polozka %llu), semafory hotovo %llu (VkResult %d), proxy "
            "zkopirovala %llu, prevzato %llu, most semP %llu semH %llu (snimku mostu %llu); fence present/most:%s",
            (unsigned long long)g_fgLastN, (unsigned long long)g_fgSeq, (unsigned long long)ev, (int)r1,
            (unsigned long long)rd, (unsigned long long)dn, (unsigned long long)sp, (unsigned long long)shh,
            (unsigned long long)g_framesDone, fs);
    }
    {
        std::lock_guard<std::mutex> lk(g_fgMx);
        g_fgStopReq = true;
        for (int i = 0; i < g_fgQn; ++i) g_fgOut[g_fgQ[i].slot].state = FGO_FREE;
        g_fgQn = 0;
        g_fgCv.notify_all();
    }
    // pockat (kratce) na uvolneni swapchainu, pak zdroje
    for (int i = 0; i < 200 && !g_ptStopped; ++i) Sleep(5);
    FgFreeAll(why);
    LogVram("po konci prezentace");
}

// FG zap za behu (BMSD_PRESENT_VERSION 5): proxy po konci na svou zadost (FG vypnuta) pripravila novy start - nove
// prezentacni okno (gameHwnd), stop = 0, restartSeq + 1. Vlakno presentu zacne znovu (stare uz swapchain uvolnilo
// a jen ceka), obrazy presentu prijdou v nove generaci. Hlavni vlakno.
static void FgRestart(BmsdPresentShared* ps) {
    g_fgRestartSeq = ps->restartSeq;
    {
        std::lock_guard<std::mutex> lk(g_fgMx);
        g_fgQuit = true;
        g_fgCv.notify_all();
    }
    if (g_fgThread.joinable()) g_fgThread.join();
    FgPtWaitCmds(2000000000ull);
    FgPtDestroySwapchain("znovu zapnuti");  // (po FgPtStop uz neni; pojistka)
    FgPtDestroySurface();
    {
        std::lock_guard<std::mutex> lk(g_fgMx);
        g_fgQuit = g_fgStopReq = g_fgQuiesce = g_fgParked = false;
        g_fgQn = 0;
        for (FgOut& o : g_fgOut) o.state = FGO_FREE;
    }
    g_ptStopped = g_ptFailed = false;
    g_ptNeedRecreate = g_ptResyncDone = g_ptInUseLogged = false;
    g_ptFirstPresentTick = g_ptLastFrameTick = g_ptFrameGap = g_ptInUseRetry = g_ptInUseSince = 0;
    g_ptLastPresQpc = g_ptLastCallQpc = g_ptNextInterpQpc = 0;
    g_fgDead = g_fgStopByProxy = g_fgFreed = false;
    g_fgWndGone = false;
    // Posledni generace obrazu, kterou helper pred koncem prezentace neimportoval (nova velikost a hned FG vyp): jeji
    // handly (zdvojene proxy do helperu) se nikdy nezavrely a drzely by VRAM do konce hry. Proxy behem znovuzapnuti
    // do obrazu nezapisuje (ceka na helperReady).
    if (ps->generation != g_fgGen && ps->generation != 0) {
        int n = 0;
        for (int i = 0; i < kFgK; ++i) {
            if (ps->img[i].handle) n += CloseHandle((HANDLE)(uintptr_t)ps->img[i].handle) ? 1 : 0;
            if (ps->hudlessImages && ps->hudless[i].handle)
                n += CloseHandle((HANDLE)(uintptr_t)ps->hudless[i].handle) ? 1 : 0;
        }
        Log("prezentace: neimportovana generace %d obrazu presentu - zavreno %d handlu", ps->generation, n);
    }
    g_fgGen = ps->generation;  // stara generace (handle zavrene): import az nove sady (proxy ji vytvori pri snimku)
    g_fgLastN = 0;
    g_fgLastPostQpc = 0;
    g_fgFresh = true;
    g_fgPrevOutSlot = -1;
    for (int m = 0; m < BMSD_FG_COUNT; ++m) {  // selhani rezimu z minuleho behu: zkusit znovu
        g_fgFailedM[m] = false;
        g_fgFailWhyM[m].clear();
    }
    g_ptWnd = (HWND)(uintptr_t)ps->gameHwnd;
    g_gpOn = ps->gpEnable != 0;
    g_gpEarly = (ps->gpEnable & 2u) != 0;
    g_gpFailLogged = false;
    ps->presentWaitOk = 0;
    ps->swapchainAlive = 0;
    FgPublishState(BMSD_FGS_OFF, 0, "");
    Log("prezentace: znovu zapnuta (zadost proxy %u, okno %p, pozadovany rezim FG %u, zobrazuje %s)", ps->restartSeq,
        (void*)g_ptWnd, ps->fgMode, g_gpOn ? "proces hry" : "helper");
    g_fgThread = std::thread(FgPresentThread);
    MemoryBarrier();
    ps->helperReady = 1;
}

// Pripojeni k prezentacnimu kanalu proxy (mapovani vznika pri prvnim Present hry). Zkousi se pri startu a pak
// periodicky, dokud neexistuje.
static void FgTryAttach() {
    if (g_ps || g_fgDead || !g_fgVkOk) return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_fgNextAttach) return;
    g_fgNextAttach = now + 200;
    wchar_t name[128];
    swprintf(name, 128, L"Local\\bms_dlss_%lu_present", g_fgPid);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) return;
    void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!view || !VirtualQuery(view, &mbi, sizeof mbi) || mbi.RegionSize < 280) {
        Log("prezentace: mapovani nejde otevrit");
        g_fgDead = true;
        return;
    }
    BmsdPresentShared* ps = (BmsdPresentShared*)view;
    if (ps->magic != BMSD_PRESENT_MAGIC || ps->version != BMSD_PRESENT_VERSION ||
        mbi.RegionSize < sizeof(BmsdPresentShared)) {
        // helperMsg a helperState jsou od verze 1 na stejnem miste: proxy prejde na zalohu
        char msg[160];
        snprintf(msg, sizeof msg, "ruzne verze prezentacniho kanalu: proxy %u, helper %u - nainstaluj d3d9.dll i helper "
                 "ze stejneho buildu", ps->version, (unsigned)BMSD_PRESENT_VERSION);
        Log("prezentace: CHYBA: %s", msg);
        if (ps->magic == BMSD_PRESENT_MAGIC) {
            strncpy(ps->helperMsg, msg, sizeof ps->helperMsg - 1);
            MemoryBarrier();
            ps->helperState = BMSD_PST_FAILED;
        }
        g_fgDead = true;
        return;
    }
    if (!ps->presenterInMain) {  // S2: prezentuje druha instance helperu, tady nic
        Log("prezentace: kanal patri druhe instanci helperu (HelperPresent=1) - hlavni helper neprezentuje");
        g_fgDead = true;
        return;
    }
    swprintf(name, 128, L"Local\\bms_dlss_%lu_pframe", g_fgPid);
    g_fgEvFrame = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    swprintf(name, 128, L"Local\\bms_dlss_%lu_pdone", g_fgPid);
    g_fgEvDone = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    if (!g_fgEvFrame || !g_fgEvDone) {
        Log("prezentace: eventy kanalu nejdou otevrit");
        strncpy(ps->helperMsg, "eventy prezentacniho kanalu nejdou otevrit", sizeof ps->helperMsg - 1);
        ps->helperState = BMSD_PST_FAILED;
        g_fgDead = true;
        return;
    }
    g_ps = ps;
    g_ptWnd = (HWND)(uintptr_t)ps->gameHwnd;
    g_gpOn = ps->gpEnable != 0;
    g_gpEarly = (ps->gpEnable & 2u) != 0;
    ps->presentWaitOk = 0;
    ps->swapchainAlive = 0;
    FgPublishState(BMSD_FGS_OFF, 0, "");
    Log("prezentace: kanal pripojen (okno %p, pozadovany rezim FG %u, strop %u FPS, zobrazuje %s)", (void*)g_ptWnd,
        ps->fgMode, ps->targetFps, g_gpOn ? "proces hry (VRR)" : "helper");
    g_fgRestartSeq = ps->restartSeq;
    if (ps->stop) {
        ps->helperState = BMSD_PST_STOPPED;
        g_fgDead = true;
        g_fgStopByProxy = true;
        g_ptStopped = true;
        return;
    }
    g_fgThread = std::thread(FgPresentThread);
    MemoryBarrier();
    ps->helperReady = 1;
}

// Inicializace po InitVulkan / InitNgx (jen s BmsdShared::fgPresenter). Selhani = helper neprezentuje (proxy po
// StartTimeoutMs prejde na zalohu), most DLSS bezi dal.
static void FgInit(DWORD pid) {
    g_fgPid = pid;
    auto num = [](const wchar_t* key, int def) {
        const std::wstring s = IniStr(L"fg", key, L"");
        return s.empty() ? def : (int)wcstol(s.c_str(), nullptr, 0);
    };
    g_fgCfg.matrixTranspose = num(L"MatrixTranspose", 1) != 0;
    g_fgCfg.dumpFrames = std::max(0, std::min(64, num(L"DumpFrames", 0)));
    g_fgCfg.dumpAfter = std::max(0, num(L"DumpAfter", 300));
    g_fgCfg.dumpTrigger = num(L"DumpTrigger", 0);
    g_fgCfg.sharpenGen = std::max(0.0f, std::min(1.0f, IniFloat(L"SharpenGen", 0.0f, L"fg")));
    g_fgCfg.dlssgLite = std::max(-1, std::min(1, num(L"DlssgLiteBlending", -1)));
    g_fgCfg.dlssgUir = num(L"DlssgUir", 0) ? 1 : 0;
    g_fgCfg.dlssgUiAlpha = num(L"DlssgUiAlpha", 0) ? 1 : 0;
    g_fgCfg.uiAlphaThr = std::max(0.5f, std::min(64.0f, IniFloat(L"UiAlphaThreshold", 3.0f, L"fg")));
    g_fgCfg.uiAlphaWeapon = num(L"UiAlphaWeapon", 1) ? 1 : 0;
    g_fgCfg.dynRes = num(L"DynamicRes", 1) != 0;
    g_fgCfg.menuDetect = num(L"MenuDetection", 0) != 0;
    g_fgCfg.fsrVersion = Narrow(IniStr(L"fg", L"FsrVersion", L"").c_str());
    while (!g_fgCfg.fsrVersion.empty() && g_fgCfg.fsrVersion.back() == ' ') g_fgCfg.fsrVersion.pop_back();
    g_fgCfg.log = num(L"Log", 0);
    g_fgCfg.hudlessUse = num(L"HudLessUse", 1) != 0;
    g_fgCfg.adaptSnapPct = std::max(0, std::min(30, num(L"AdaptSnapPct", 5)));
    {
        const std::wstring v = IniStr(L"fg", L"AdaptiveSnapMs", L"0");
        g_fgCfg.adaptSnapMs = std::max(0.0, std::min(3.0, _wtof(v.c_str())));
        if (g_fgCfg.adaptSnapMs > 0.0) Log("config [fg]: AdaptiveSnapMs=%.2f (rozvrh A: prichyceni na skutecny snimek)",
                                           g_fgCfg.adaptSnapMs);
        g_fgCfg.vblankGrid = num(L"VblankGrid", 0) != 0;
        g_ptmDumpN = (size_t)std::max(0, std::min(20000, num(L"PresentTimingDump", 0)));
        if (g_ptmDumpN) {
            g_ptmDump.reserve(g_ptmDumpN);
            g_ptmDumpFrom = GetTickCount64() + 15000;
        }
        const std::wstring lead = IniStr(L"fg", L"VblankLeadMs", L"3.0");
        g_fgCfg.vblankLeadMs = std::max(0.5, std::min(6.0, _wtof(lead.c_str())));
        if (g_fgCfg.vblankGrid)
            Log("config [fg]: VblankGrid=1 VblankLeadMs=%.2f (rozvrh A: mrizka podle vblanku, jen s PresentTiming=1)",
                g_fgCfg.vblankLeadMs);
    }
    // ladeni: pripojit se ke kanalu presentu az po tolika ms (most pak bezi driv nez present - poradi importu)
    if (const int d = num(L"AttachDelayMs", 0)) g_fgNextAttach = GetTickCount64() + (ULONGLONG)d;
    g_fgDumpLeft = g_fgCfg.dumpFrames;
    Log("config [fg]: MatrixTranspose=%d DumpFrames=%d DumpAfter=%d DynamicRes=%d MenuDetection=%d SeparateQueue=%d "
        "HudLessUse=%d (fronta presentu %s)",
        g_fgCfg.matrixTranspose, g_fgCfg.dumpFrames, g_fgCfg.dumpAfter, g_fgCfg.dynRes, g_fgCfg.menuDetect,
        g_fgCfg.separateQueue, g_fgCfg.hudlessUse, g_presQueue != g_queue ? "vlastni" : "sdilena s mostem");
#define LOADI(n)                                                   \
    fvk.n = (PFN_##n)vk.vkGetInstanceProcAddr(g_inst, #n);         \
    if (!fvk.n) return Log("prezentace: chybi funkce %s - helper neprezentuje", #n);
    FG_INST_FUNCS(LOADI)
#undef LOADI
#define LOADD(n)                                                   \
    fvk.n = (PFN_##n)vk.vkGetDeviceProcAddr(g_dev, #n);            \
    if (!fvk.n) return Log("prezentace: chybi funkce %s - helper neprezentuje", #n);
    FG_DEV_FUNCS(LOADD)
#undef LOADD
    g_fgCfg.separateQueue = g_presQueue != g_queue;
    if (g_presQueue == g_queue) g_presQMx = &g_queueMx;
    if (g_ptTimingExt) {  // [fg] PresentTiming=1 (rozsireni zapnuto v InitVulkan)
#define LOADX(f, n) ptx.f = (PFN_##n)vk.vkGetDeviceProcAddr(g_dev, #n);
        LOADX(setQueueSize, vkSetSwapchainPresentTimingQueueSizeEXT)
        LOADX(timingProps, vkGetSwapchainTimingPropertiesEXT)
        LOADX(domainProps, vkGetSwapchainTimeDomainPropertiesEXT)
        LOADX(past, vkGetPastPresentationTimingEXT)
        if (g_ptCalibExt) LOADX(calib, vkGetCalibratedTimestampsKHR)
#undef LOADX
        if (g_ptCaps2Ext)
            ptx.surfCaps2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)vk.vkGetInstanceProcAddr(
                g_inst, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
        g_ptmFuncs = ptx.setQueueSize && ptx.timingProps && ptx.domainProps && ptx.past;
        Log("config [fg]: PresentTiming=1 - mereni skutecneho zobrazeni %s", g_ptmFuncs ? "zapnuto" : "NE (chybi funkce)");
    }
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, kSlots};
    if (vk.vkAllocateCommandBuffers(g_dev, &cai, g_fgCmd) != VK_SUCCESS) return Log("prezentace: cmd buffery");
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int i = 0; i < kSlots; ++i)
        if (vk.vkCreateFence(g_dev, &fci, nullptr, &g_fgFence[i]) != VK_SUCCESS) return Log("prezentace: fence");
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf};
    if (vk.vkCreateCommandPool(g_dev, &pci, nullptr, &g_ptPool) != VK_SUCCESS) return Log("prezentace: command pool");
    cai.commandPool = g_ptPool;
    cai.commandBufferCount = 2;
    if (vk.vkAllocateCommandBuffers(g_dev, &cai, g_ptCmd) != VK_SUCCESS) return Log("prezentace: cmd buffery vlakna");
    VkSemaphoreCreateInfo bsi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < 2; ++i)
        if (vk.vkCreateFence(g_dev, &fci, nullptr, &g_ptFence[i]) != VK_SUCCESS ||
            vk.vkCreateSemaphore(g_dev, &bsi, nullptr, &g_ptAcq[i]) != VK_SUCCESS)
            return Log("prezentace: fence / semafor vlakna");
    VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
    if (vk.vkCreateSemaphore(g_dev, &sci, nullptr, &g_fgSemEval) != VK_SUCCESS) return Log("prezentace: timeline semafor");
    if (g_tsPeriod > 0.0f) {
        VkQueryPoolCreateInfo qpci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP,
                                      kSlots * 2};
        if (vk.vkCreateQueryPool(g_dev, &qpci, nullptr, &g_fgQpool) != VK_SUCCESS) g_fgQpool = VK_NULL_HANDLE;
    }
    g_ptTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
    if (!g_ptTimer) g_ptTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    FgQueryAvail();
    FgQueryAvailFsr();
    g_fgVkOk = true;
    FgTryAttach();
}

// Zprava "novy snimek" od proxy (event ..._pframe) nebo periodicke probuzeni. Hlavni vlakno.
// ---------------------------------------------------------------------------------------------------------------
// Adaptivni FSR: rozvrh (hlavni vlakno, jednou za skutecny snimek N). Casy jsou postQpc proxy (Present hry) = obsahovy
// cas skutecneho snimku; t mezisnimku se pocita zpetne ze skutecnych casu (rN-1, rN), odhad dalsiho snimku netreba.
//  A (pevny rytmus): mrizka slotu s periodou P = 1 / cil; slot zobrazeny v d ukazuje obsah c = d - L, t = (c - rN-1) /
//    (rN - rN-1). L (posun za realitou) >= nejdelsi nedavny interval + doba do "hotovo na GPU" + rezerva; roste o 10 % P
//    za slot (zpomaleni obsahu misto zaseku), klesa o 2 %. Slot s t >= 1 - AdaptSnapPct ukaze skutecny snimek. Hystereze:
//    generovat az pod hranici (auto 90 % cile) 0.5 s, prestat nad 95 % cile po 0.5 s (mrtva zona = jen skutecne).
//  B (skutecne + mezery): skutecny snimek vzdy; do mezery n generovanych v t = i / (n + 1), jen kdyz se vejdou nad
//    nejkratsi interval (strop VRR): n = floor(I / P) - 1, hystereze na EMA intervalu. Rozvrh jako nasobek (n + 1)x.
// ---------------------------------------------------------------------------------------------------------------
struct FgAdaptPlan {
    bool pair = false;         // zaznamenat AfgRecordPair (adaptivni generuje nebo navazuje)
    int n = 0;                 // generovanych snimku
    float t[kFgMaxInterp] = {};
    bool fixedTimes = false;   // rozvrh A
    double periodMs = 6.4;     // A: perioda mrizky (ms)
    bool showReal = true;
    LONGLONG disp[kFgMaxInterp] = {}, dispReal = 0;
    LONGLONG content[kFgMaxInterp + 1] = {};
};

static void FgAdaptReset(int pacing) {
    g_ad.active = false;
    g_ad.belowSince = g_ad.aboveSince = 0;
    g_ad.nextDisp = 0;
    g_ad.engaged = false;
    g_ad.lateBoost = 0.0;
    g_ad.lastPacing = pacing;
    g_ad.winStart = g_ad.activeSince = g_ad.blockUntil = 0;
    g_ad.lowGain = 0;
    g_ad.backoffS = 5.0;
}

static FgAdaptPlan FgAdaptMakePlan(const BmsdPresentShared* ps, LONGLONG now, LONGLONG rN, LONGLONG rPrev, double dtMs,
                                   bool ok, bool reset) {
    FgAdaptPlan pl;
    pl.content[kFgMaxInterp] = rN;
    const double qms = (double)g_qpf.QuadPart / 1000.0;  // QPC na ms
    double tgt = ps->adaptTargetMilli / 1000.0;
    if (tgt < 20.0) tgt = 150.0;
    double P = 1000.0 / tgt;
    const double minIv = g_ptMinIvQ > 0 ? (double)g_ptMinIvQ / qms : 0.0;
    if (P < minIv) P = minIv;  // rucni cil nad stropem VRR: mrizka na strop
    // VblankGrid (pokus): monitor bez VRR - perioda mrizky = namerena perioda obnovy, faze podle casu zobrazeni
    const double vbR = g_vbPeriodMs;
    const LONGLONG vbRef = g_vbRefQ;
    const bool vb = g_fgCfg.vblankGrid && vbRef && vbR > 2.0 && vbR < 50.0;
    g_ptCapOverrideQ = 0;
    if (vb) P = vbR;
    const int pacing = 0;  // jen rozvrh A (rozvrh B odstranen ve v24z4: ve 4K skubal, nikde se neosvedcil)
    if (pacing != g_ad.lastPacing) FgAdaptReset(pacing);
    if (dtMs >= 2.0 && dtMs <= 100.0) {
        g_ad.ivEma = g_ad.ivEma > 0.0 ? g_ad.ivEma + 0.1 * (dtMs - g_ad.ivEma) : dtMs;
        g_ad.ivHist[g_ad.ivPos++ & 7] = dtMs;
    }
    const double realFps = g_ad.ivEma > 0.0 ? 1000.0 / g_ad.ivEma : 0.0;
    const bool linked = !reset && g_afgPrevPaired && rN > rPrev;  // par navazuje na minuly (jinak jen par, bez mezisnimku)
    const double I = rN > rPrev ? (double)(rN - rPrev) / qms : 0.0;

    if (pacing == 0) {
        const double upper = 0.95 * tgt;
        double lower = ps->adaptFillMilli ? ps->adaptFillMilli / 1000.0 : 0.9 * tgt;
        if (lower > upper) lower = upper;
        const LONGLONG half = g_qpf.QuadPart / 2;
        const bool was = g_ad.active;
        // Uzitecnost generovani: kdyz GPU nestaci (hra s generovanim zpomali tak, ze se k cili nedostane, a sloty se
        // zahazuji), generovani snimky nepridava - hra s ~149 FPS pak s generovanim jela ~125 FPS a nikdy nevylezla
        // nad horni hranici. Okno 1 s: zobrazeno / skutecnych < 1.1 trikrat po sobe (po 1 s nabehu; kratky zaskub hry
        // nestaci) -> jen skutecne a znovu zapnout nejdriv za backoffS (5 s, pri opakovani dvojnasobek az 60 s; po 20 s uzitecneho generovani
        // zpet na 5 s). Pocita se jen okno, ve kterem se generovane snimky opravdu zobrazovaly (po zapnuti muze FG
        // jeste cekat na obrazy mostu / vytvoreni kontextu - to neni neuzitecnost).
        {
            ++g_ad.winReal;
            const uint64_t shown = ps->presCountGen + ps->presCountReal;
            if (!g_ad.winStart || now < g_ad.winStart) {
                g_ad.winStart = now;
                g_ad.winShown0 = shown;
                g_ad.winGen0 = ps->presCountGen;
                g_ad.winReal = 0;
            } else if (now - g_ad.winStart >= g_qpf.QuadPart) {
                const uint64_t ds = shown - g_ad.winShown0;
                const uint64_t dg = ps->presCountGen - g_ad.winGen0;
                g_ad.lastGain = g_ad.winReal ? (double)ds / (double)g_ad.winReal : 0.0;
                const bool ramped = g_ad.active && g_ad.activeSince && now - g_ad.activeSince >= g_qpf.QuadPart;
                if (ramped && dg > 0 && g_ad.lastGain < 1.1) ++g_ad.lowGain;
                else g_ad.lowGain = 0;
                if (ramped && g_ad.lastGain >= 1.1 && now - g_ad.activeSince >= 20 * g_qpf.QuadPart) g_ad.backoffS = 5.0;
                g_ad.winStart = now;
                g_ad.winShown0 = shown;
                g_ad.winGen0 = ps->presCountGen;
                g_ad.winReal = 0;
            }
            if (g_ad.active && g_ad.lowGain >= 3) {
                g_ad.active = false;
                g_ad.lowGain = 0;
                ++g_ad.uselessOff;
                g_ad.blockUntil = now + (LONGLONG)(g_ad.backoffS * (double)g_qpf.QuadPart);
                Log("FG: adaptivni FSR (A) generovani snimky nepridava (zobrazeno %.2f na skutecny, skutecne %.1f FPS, "
                    "cil %.1f) - jen skutecne, znovu nejdriv za %.0f s",
                    g_ad.lastGain, realFps, tgt, g_ad.backoffS);
                g_ad.backoffS = std::min(60.0, g_ad.backoffS * 2.0);
            }
        }
        if (!g_ad.active) {
            g_ad.aboveSince = 0;
            if (realFps > 0.0 && realFps < lower && now >= g_ad.blockUntil) {
                if (!g_ad.belowSince) g_ad.belowSince = now;
                if (now - g_ad.belowSince >= half) g_ad.active = true;
            } else {
                g_ad.belowSince = 0;
            }
        } else {
            g_ad.belowSince = 0;
            if (realFps > upper) {
                if (!g_ad.aboveSince) g_ad.aboveSince = now;
                if (now - g_ad.aboveSince >= half) g_ad.active = false;
            } else {
                g_ad.aboveSince = 0;
            }
        }
        if (was != g_ad.active) {
            ++g_ad.switches;
            g_ad.nextDisp = 0;
            g_ad.activeSince = g_ad.active ? now : 0;
            Log("FG: adaptivni FSR (A) %s - skutecne %.1f FPS, cil %.1f, hranice %.1f / %.1f", g_ad.active ? "generuje"
                                                                                                      : "jen skutecne",
                realFps, tgt, lower, upper);
        }
        g_ad.engaged = g_ad.active;
        if (!g_ad.active || !ok) {
            g_ad.nextDisp = 0;
            return pl;
        }
        pl.pair = true;
        if (!linked) {
            g_ad.nextDisp = 0;
            return pl;
        }
        // (v24z4 zkouseno: 3. nejdelsi misto nejdelsiho - L ve 4K neklesla, castejsi lateBoost)
        double maxI = 0.0;
        for (double v : g_ad.ivHist) maxI = std::max(maxI, v);
        maxI = std::min(std::max(maxI, I), 50.0);
        const double late = g_adLateMs;
        g_adLateMs = 0.0;
        g_ad.lateBoost = std::max(g_ad.lateBoost * 0.995, late > 0.25 * P ? std::min(late, 20.0) : 0.0);
        const double Lt = maxI + g_adDoneMs + 1.0 + g_ad.lateBoost;
        const LONGLONG Pq = (LONGLONG)(P * qms);
        if (vb) g_ptCapOverrideQ = Pq - (LONGLONG)(0.02 * qms);  // nikdy blize nez perioda obnovy (- 0.02 ms kolisani)
        if (g_ad.nextDisp && now - g_ad.nextDisp > 3 * Pq) g_ad.nextDisp = 0;  // mrizka daleko v minulosti: znovu
        if (!g_ad.nextDisp) {
            // start mrizky: prvni slot ukaze obsah tesne za minulym skutecnym snimkem (zadny skok zpet), L pak roste k Lt
            LONGLONG d0 = now + (LONGLONG)(g_adDoneMs * qms);
            const LONGLONG lastCall = g_ptLastCallQpc;
            if (lastCall && d0 < lastCall + Pq) d0 = lastCall + Pq;
            if (vb) {  // zarovnat na vblank: volani VblankLeadMs pred nim
                const LONGLONG lead = (LONGLONG)(g_fgCfg.vblankLeadMs * qms);
                const LONGLONG k = (d0 + lead - vbRef + Pq - 1) / Pq;
                d0 = vbRef + k * Pq - lead;
            }
            g_ad.nextDisp = d0;
            g_ad.L = std::max(0.0, (double)(d0 - rPrev) / qms - 0.05 * I);
        }
        // VblankGrid: drift faze mrizky proti vblanku dorovnat pomalu (nejvys 0.05 ms za slot)
        LONGLONG vbCorr = 0;
        if (vb) {
            const LONGLONG lead = (LONGLONG)(g_fgCfg.vblankLeadMs * qms);
            LONGLONG e = (g_ad.nextDisp + lead - vbRef) % Pq;
            if (e < 0) e += Pq;
            if (e > Pq / 2) e -= Pq;  // e > 0: volani pozde proti cilove fazi
            const LONGLONG lim = (LONGLONG)(0.05 * qms);
            vbCorr = std::max(-lim, std::min(lim, -e / 8));
            g_ad.vbErr.add(fabs((double)e / qms));
        }
        const double snap = g_fgCfg.adaptSnapPct / 100.0;
        const double snapMs = g_fgCfg.adaptSnapMs;
        pl.fixedTimes = true;
        pl.periodMs = P;
        pl.showReal = false;
        int n = 0;
        for (int guard = 0; guard < 64; ++guard) {
            const LONGLONG c = g_ad.nextDisp - (LONGLONG)(g_ad.L * qms);
            if (c > rN) {
                // A+ (AdaptiveSnapMs): slot s obsahem nejvys snapMs za skutecnym snimkem ukaze ten skutecny snimek
                // (jinak by ho dalsi par ukazal jako mezisnimek s t blizko 0)
                const double after = (double)(c - rN) / qms;
                if (snapMs > 0.0 && !pl.showReal && after <= snapMs) {
                    pl.showReal = true;
                    pl.dispReal = g_ad.nextDisp;
                    ++g_ad.slotsReal;
                    g_ad.snapErr.add(after);
                    g_ad.contErr.add(after);
                    ++g_ad.slots;
                    g_ad.nextDisp += Pq + vbCorr;
                    const double dL = Lt - g_ad.L;
                    g_ad.L += dL > 0.0 ? std::min(dL, 0.1 * P) : std::max(dL, -0.02 * P);
                    continue;
                }
                break;  // obsah slotu jeste neni (ceka na dalsi skutecny snimek)
            }
            double t = (double)(c - rPrev) / (double)(rN - rPrev);
            const double before = (double)(rN - c) / qms;
            if (snapMs > 0.0 ? before <= snapMs : t >= 1.0 - snap) {
                if (!pl.showReal) {
                    pl.showReal = true;
                    pl.dispReal = g_ad.nextDisp;
                    ++g_ad.slotsReal;
                    if (snapMs > 0.0) g_ad.snapErr.add(before);
                    g_ad.contErr.add(before);
                }
            } else if (n < kAfgMaxInterp) {
                if (t < 0.02) t = 0.02;
                pl.t[n] = (float)t;
                pl.disp[n] = g_ad.nextDisp;
                pl.content[n] = c;
                g_ad.contErr.add(t < 0.02 ? (0.02 - t) * I : 0.0);
                ++n;
            } else {
                ++g_ad.slotsDrop;  // skutecne pod cil / 5: vic slotu na par, nez je obrazu
            }
            ++g_ad.slots;
            g_ad.nextDisp += Pq + vbCorr;
            const double dL = Lt - g_ad.L;
            g_ad.L += dL > 0.0 ? std::min(dL, 0.1 * P) : std::max(dL, -0.02 * P);
        }
        g_ad.Lacc.add(g_ad.L);
        pl.n = n;
        return pl;
    }

    return pl;
}

// AFG: par + mezisnimky planu do cmd (vstupy jako FgRecordFsr: backbuffer / HUD-less v SHADER_READ_ONLY, snapshot MV
// a hloubky v SHADER_READ_ONLY, vystupy interp v GENERAL).
static bool FgRecordAfg(VkCommandBuffer cmd, int k, int j, const FgSnap& sn, bool reset, bool hl, const FgAdaptPlan& pl,
                        const char** stage) {
    AfgPairInfo p;
    p.color = g_fgPres[k].img;
    p.hudless = FgHlGen() ? (hl ? g_fgHl[k].img : g_fgPres[k].img) : VK_NULL_HANDLE;
    p.colorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    p.motionVectors = sn.mv.img;
    p.mvFormat = kMvFormat;
    p.depth = sn.depth.img;
    p.depthFormat = g_depthFmt;
    p.inputW = g_w;
    p.inputH = g_h;
    p.renderW = sn.rW;
    p.renderH = sn.rH;
    p.mvScaleX = g_cfg.mvScaleX;
    p.mvScaleY = g_cfg.mvScaleY;
    float n = 7.0f, f = 28000.0f, fov = 1.02f;
    if (sn.camOk) CameraNearFarFov(sn.cam, &n, &f, &fov);
    p.cameraNear = n;
    p.cameraFar = f;
    p.cameraFovY = fov;
    p.metersPerUnit = 0.0254f;
    p.frameTimeDeltaMs = (float)g_fgIntervalMs;
    p.reset = reset;
    p.frameId = ++g_afgFrameId;
    *stage = "AfgRecordPair";
    if (!AfgRecordPair(cmd, p)) return false;
    *stage = "AfgRecordInterp";
    // (v24z9-z10 zkouseno AdaptHalfRes - mezisnimky v 50-80 % velikosti: 50 % rozmazane, 75-80 % bez merit. zisku)
    for (int g = 0; g < pl.n; ++g)
        if (!AfgRecordInterp(cmd, pl.t[g], g_fgOut[j].interp[g].img, VK_NULL_HANDLE)) return false;
    return true;
}

static void FgOnPresentMsg(LONGLONG tWake) {
    if (!g_fgVkOk) return;
    if (!g_ps) FgTryAttach();
    BmsdPresentShared* ps = g_ps;
    // FG zap za behu: proxy chce prezentaci znovu (restartSeq). Proxy ceka na konec jen 1 s - kdyz hlavni vlakno
    // helperu v te dobe stalo (vytvareni generatoru, import), muze uz byt stop zase 0 s novym restartSeq, aniz helper
    // stop videl, nebo prezentace mezitim skoncila na znicenem okne. Obe varianty = konec a novy start.
    if (ps && (!g_fgDead || g_fgStopByProxy || g_fgWndGone)) {
        MemoryBarrier();
        if (!ps->stop && ps->restartSeq != g_fgRestartSeq) {
            if (!g_fgDead) {
                g_fgStopByProxy = true;
                FgStop("proxy zadala novy start (predchozi konec nezachycen)");
            }
            FgRestart(ps);
        }
    }
    if (!ps || g_fgDead) return;
    MemoryBarrier();
    if (ps->stop) {
        g_fgStopByProxy = true;
        return FgStop("zadost proxy (FG vypnuta nebo zaloha)");
    }
    if (g_ptFailed) return FgStop("chyba presenteru");
    FgPublishStats(tWake);
    {  // nejkratsi interval presentu (strop VRR / limiter ovladace) z frekvence monitoru od proxy - pro vsechny rezimy
        const double hz = ps->refreshMilli / 1000.0;
        g_ptMinIvQ = hz >= 30.0 ? (LONGLONG)((double)g_qpf.QuadPart / (hz - hz * hz / 3600.0)) : 0;
    }
    if (ps->generation != g_fgGen && ps->generation != 0) {
        if (!FgImport()) return FgStop("import obrazu");
    }
    if (!g_fgImgOk) return;
    // Snimek: seqlock nad postQpc (proxy: postQpc = 0 -> bridgeFrame -> frameValue -> postQpc = cas)
    uint64_t N = 0, B = 0, H = 0;
    LONGLONG posted = 0;
    bool got = false;
    for (int i = 0; i < 2000 && !got; ++i) {
        const uint64_t q1 = ps->postQpc;
        if (!q1) {
            YieldProcessor();
            MemoryBarrier();
            continue;
        }
        N = ps->frameValue;
        B = ps->bridgeFrame;
        H = ps->hudlessFrame;
        MemoryBarrier();
        got = ps->postQpc == q1;
        posted = (LONGLONG)q1;
    }
    if (!got || !N || N == g_fgLastN) return;
    // Most ma prednost: zprava mostu, kterou proxy zverejnila pred timto snimkem presentu (stejny nebo drivejsi snimek
    // hry), musi byt odeslana na GPU driv nez prace presentu - viz BridgeMessage (kruhove cekani front). Proxy
    // zverejnuje most pred presentem, takze po precteni N je pripadna nevyrizena zprava mostu uz signalizovana.
    while (WaitForSingleObject(g_evBridge, 0) == WAIT_OBJECT_0) {
        if (!BridgeMessage()) {
            g_bridgeFatal = true;
            return;
        }
    }
    const bool consecutive = N == g_fgLastN + 1;
    g_fgLastN = N;
    ++g_fgSt.frames;
    g_fgSt.wake.add(FgMs(posted, tWake));
    double realDtMs = 0.0;  // interval od minuleho skutecneho snimku (0 = neznamy / nenavazuje)
    const double intervalBefore = g_fgIntervalMs;
    if (consecutive && g_fgLastPostQpc) {
        const double dt = FgMs(g_fgLastPostQpc, posted);
        realDtMs = dt;
        if (dt >= 2.0 && dt <= 100.0) {
            const double c = std::min(50.0, std::max(4.0, dt));
            g_fgIntervalMs += 0.2 * (c - g_fgIntervalMs);
        }
    }
    const LONGLONG prevPosted = g_fgLastPostQpc;
    g_fgLastPostQpc = posted;
    const int k = (int)(N % kFgK);
    // HUD-less tohoto snimku od proxy (K5)
    const bool hl = g_fgHlOk && H == N && g_fgHlView[k];

    // Co s timto snimkem
    const uint32_t reqMode = ps->fgMode;
    const bool want = reqMode == BMSD_FG_DLSSG || reqMode == BMSD_FG_FSR2X || reqMode == BMSD_FG_FSRADAPT;
    const bool fsrGen = reqMode == BMSD_FG_FSR2X;
    const bool adapt = reqMode == BMSD_FG_FSRADAPT;
    // Nasobek: pozadavek proxy ([fg] Multiplier / menu, 2..4); FSR FG umi jen 2x, DLSS-G podle GPU (MultiFrameCountMax)
    // a poctu obrazu interp. nGen = generovanych snimku na skutecny.
    uint32_t reqMult = ps->fgMultiplier & 0xFF;
    const bool reqAuto = (ps->fgMultiplier & 0x100) != 0;  // proxy: Multiplier=auto (nasobek se muze menit)
    if (reqMult < 2 || reqMult > 6) reqMult = 2;
    // FSR nad 2x (v25n): vlastni generator AFG (jako adaptivni FSR) s pevnym nasobkem, mezisnimky v t = i / nasobek a
    // rozvrh jako DLSS-G. S auto i 2x pres AFG (zmena generatoru = nova feature = zaseknuti pri kazde zmene nasobku).
    // Kdyz AFG selze (g_fgFailedM[FSRADAPT]), FSR zustava na FSR FG 2x od AMD.
    const bool fsrMulti = fsrGen && (reqMult >= 3 || reqAuto) && g_afgAvail && !g_fgFailedM[BMSD_FG_FSRADAPT];
    const bool afg = adapt || fsrMulti;                                         // generator AFG
    const uint32_t genMode = fsrMulti ? (uint32_t)BMSD_FG_FSRADAPT : reqMode;  // feature, ktera generuje
    // Vyssi nasobek DLSS-G, nez na kolik jsou vystupni obrazy: doplnit (jednou za generaci; kdyz to selze, zustava
    // nizsi nasobek jako drive pri malem poctu obrazu)
    if (reqMode == BMSD_FG_DLSSG && !g_fgFailedM[reqMode] && g_fgGrowFailGen != g_fgGen) {
        const int needN = std::min((int)reqMult, g_fgMultMax) - 1;
        if (needN > g_fgInterpN && !FgGrowInterp(needN, "nasobek DLSS-G")) {
            g_fgGrowFailGen = g_fgGen;
            Log("FG: obrazy pro nasobek %ux nejdou vytvorit (VRAM?) - DLSS-G zustava na %dx", reqMult, g_fgInterpN + 1);
        }
    }
    const int mult = fsrMulti ? std::min((int)reqMult, kAfgMaxInterp + 1)
                     : fsrGen || adapt ? 2
                               : std::min((int)reqMult, std::min(g_fgMultMax, g_fgInterpN + 1));
    int nGen = mult - 1;  // adaptivni: podle rozvrhu (nize)
    if (want) g_fgCurMode = genMode;
    if (want && !g_fgFailedM[genMode] && FgAvailFor(genMode)) FgEnsureFeature(genMode);  // i prepnuti DLSS-G <-> FSR
    // generator pozadovaneho rezimu existuje a neselhal
    const bool have = want && FgHave(genMode) && !g_fgFailedM[genMode];
    if (!want && g_fgSnapWanted) g_fgSnapWanted = false;  // FG vypnuto v menu: snapshoty netreba
    // (Snapshot i v mrtve zone adaptivniho FSR: bez nej by snimek nemel MV, stav by presel na "pozastaveno" a rozvrh
    // by se znovu nezapnul - overeno 2026-11-20. Usetri se jen kopie HUD-less v proxy, [fg] AdaptIdleSkip.)
    if (have) g_fgSnapWanted = true;                      // zpet zapnuto
    const bool windowOk = ps->helperState == BMSD_PST_PRESENTING || ps->helperState == BMSD_PST_START;
    const FgSnap* sn = nullptr;
    if (have && B) {
        const FgSnap& c = g_fgSnap[B % kFgSnap];
        if (c.valid && c.B == B && g_fgPresView[k]) sn = &c;
    }
    // (FSR FG bere sub-rect renderu vzdy - PrepareV2::renderSize)
    const bool subOk = !sn || fsrGen || adapt || g_fgCfg.dynRes || (sn->rW == g_w && sn->rH == g_h);
    bool gen = sn && subOk && !ps->fgPause && windowOk;
    bool reset = false;
    // FSR FG: po zaseknuti hry (interval skutecnych snimku aspon 3x delsi nez obvykly a nad 40 ms) se mezisnimek
    // nedela - pres velky skok kamery ho FSR sklada spatne (pokrouceny obraz i HUD); bere se jako reset historie.
    // (DLSS-G zustava jako ve v12.)
    const bool hitch = (fsrGen || adapt) && realDtMs > std::max(40.0, 3.0 * intervalBefore);
    if (gen) reset = sn->reset || !sn->camOk || g_fgFresh || B != g_fgLastEvalB + 1 || hitch;
    // Adaptivni FSR: rozvrh rozhodne, jestli se generuje (mrtva zona / nad cilem = jen skutecny snimek hned)
    const bool adaptRunning = adapt && gen;
    FgAdaptPlan apl;
    if (!adapt) g_ptCapOverrideQ = 0;
    if (adapt) {
        apl = FgAdaptMakePlan(ps, tWake, posted, prevPosted, consecutive ? realDtMs : 0.0, gen, reset);
        if (!apl.pair) gen = false;
        nGen = apl.n;
    } else if (fsrMulti) {  // pevny nasobek: par vzdy, mezisnimky jen kdyz par navazuje na minuly
        apl.pair = true;
        apl.n = gen && !reset && g_afgPrevPaired ? mult - 1 : 0;
        for (int g = 0; g < apl.n; ++g) apl.t[g] = (float)(g + 1) / (float)mult;
        nGen = apl.n;
    }
    ++g_fgFrameId;  // +1 za kazdy skutecny snimek, i kdyz se negeneruje
    if (have) {
        if (gen && !reset) ++g_fgSt.paired;
        else ++g_fgSt.unpaired;
    }

    // Vystupni slot (kruh; kdyz je vse zabrane, zahodit nejstarsi cekajici polozku)
    int j = -1;
    {
        std::lock_guard<std::mutex> lk(g_fgMx);
        for (int i = 0; i < kFgOut && j < 0; ++i) {
            const int c = (g_fgNextOut + i) % kFgOut;
            if (g_fgOut[c].state == FGO_FREE) j = c;
        }
        if (j < 0 && g_fgQn > 0) {
            j = g_fgQ[0].slot;
            g_fgQ[0] = g_fgQ[1];
            g_fgQ[1] = g_fgQ[2];
            --g_fgQn;
            std::lock_guard<std::mutex> sl(g_fgStatMx);
            ++g_ptSt.dropped;
        }
        if (j >= 0) {
            g_fgOut[j].state = FGO_QUEUED;  // rezervace (do fronty az po odeslani)
            g_fgNextOut = (j + 1) % kFgOut;
        }
    }

    const int c = g_fgSlot;
    g_fgSlot = (g_fgSlot + 1) % kSlots;
    if (g_fgFenceUsed[c]) {
        if (vk.vkWaitForFences(g_dev, 1, &g_fgFence[c], VK_TRUE, kGpuTimeoutNs) != VK_SUCCESS) {
            if (j >= 0) {
                std::lock_guard<std::mutex> lk(g_fgMx);
                g_fgOut[j].state = FGO_FREE;
            }
            FgPresFatal("GPU nedobehla (snimek presentu)");
            return FgStop("GPU nedobehla");
        }
        vk.vkResetFences(g_dev, 1, &g_fgFence[c]);
        g_fgFenceUsed[c] = false;
        FgCollectTs(c);
    }
    if (j < 0) gen = false;
    // FSR FG: prikazy minuleho snimku s FSR FG musi na GPU dobehnout driv, nez se zaznamenava dalsi - ffx pri zaznamu
    // rusi sve pohledy na MV a hloubku minuleho snimku (tools/spike_ffg). Bezne uz davno dobehly (snimek stary ~13 ms);
    // fence se tu jen ceka, vynuluje se az pri pristim pouziti slotu.
    if (gen && fsrGen && !fsrMulti && g_ffgLastSlot >= 0 && g_ffgLastSlot != c && g_fgFenceUsed[g_ffgLastSlot] &&
        vk.vkWaitForFences(g_dev, 1, &g_fgFence[g_ffgLastSlot], VK_TRUE, kGpuTimeoutNs) != VK_SUCCESS) {
        {
            std::lock_guard<std::mutex> lk(g_fgMx);
            g_fgOut[j].state = FGO_FREE;
        }
        FgPresFatal("GPU nedobehla (minuly snimek FSR FG)");
        return FgStop("GPU nedobehla");
    }
    // AFG: nejvys 3 pary v letu (pohledy FFX) - stejne jako u FSR FG pockat na minuly par (bezne davno hotovy)
    if (gen && afg && g_afgLastSlot >= 0 && g_afgLastSlot != c && g_fgFenceUsed[g_afgLastSlot] &&
        vk.vkWaitForFences(g_dev, 1, &g_fgFence[g_afgLastSlot], VK_TRUE, kGpuTimeoutNs) != VK_SUCCESS) {
        {
            std::lock_guard<std::mutex> lk(g_fgMx);
            g_fgOut[j].state = FGO_FREE;
        }
        FgPresFatal("GPU nedobehla (minuly par adaptivniho FSR)");
        return FgStop("GPU nedobehla");
    }
    const VkImageAspectFlags COLOR = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageLayout GEN = VK_IMAGE_LAYOUT_GENERAL, SRO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const uint32_t EXT = VK_QUEUE_FAMILY_EXTERNAL;
    VkCommandBuffer cmd = g_fgCmd[c];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    bool evalDone = false;
    for (int pass = 0; pass < 2; ++pass) {
        vk.vkResetCommandBuffer(cmd, 0);
        vk.vkBeginCommandBuffer(cmd, &bi);
        // prevzeti sdileneho obrazu od proxy (GENERAL)
        VkImageMemoryBarrier acq[2] = {
            IB(g_fgPres[k].img, COLOR, 0, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT, GEN, GEN, EXT, g_qf),
            hl ? IB(g_fgHl[k].img, COLOR, 0, VK_ACCESS_SHADER_READ_BIT, GEN, GEN, EXT, g_qf) : VkImageMemoryBarrier{}};
        PB(cmd, ALL, ALL, hl ? 2 : 1, acq);
        if (gen) {
            VkImageMemoryBarrier pre[3 + kFgMaxInterp];
            uint32_t np = 0;
            pre[np++] = IB(g_fgPres[k].img, COLOR, 0, VK_ACCESS_SHADER_READ_BIT, GEN, SRO);
            pre[np++] = IB(g_fgOut[j].real.img, COLOR, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, GEN, GEN);
            for (int g = 0; g < nGen; ++g)
                pre[np++] = IB(g_fgOut[j].interp[g].img, COLOR, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                               GEN, GEN);
            if (hl) pre[np++] = IB(g_fgHl[k].img, COLOR, 0, VK_ACCESS_SHADER_READ_BIT, GEN, SRO);
            PB(cmd, ALL, ALL, np, pre);
            const bool ts = g_fgQpool != VK_NULL_HANDLE;
            if (ts) {
                vk.vkCmdResetQueryPool(cmd, g_fgQpool, c * 2, 2);
                vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_fgQpool, c * 2);
            }
            if (afg) {
                const char* stage = "";
                if (!FgRecordAfg(cmd, k, j, *sn, reset, hl && FgHlGen(), apl, &stage)) {
                    // cmd buffer je v neznamem stavu: zahodit a snimek poslat jen jako kopii
                    FgFail("vyhodnoceni adaptivniho FSR selhalo: %s - %s", stage, AfgLastError());
                    gen = false;
                    g_afgPrevPaired = false;
                    continue;
                }
                g_afgLastSlot = c;
            } else if (fsrGen) {
                const char* stage = "";
                const ffxReturnCode_t fr = FgRecordFsr(cmd, k, j, *sn, reset, hl && FgHlGen(), &stage);
                if (fr != FFX_API_RETURN_OK) {
                    // cmd buffer je v neznamem stavu: zahodit a snimek poslat jen jako kopii
                    FgFail("vyhodnoceni FSR FG selhalo: %s -> kod %u", stage, (unsigned)fr);
                    gen = false;
                    continue;
                }
                g_ffgLastSlot = c;
            } else {
                // nasobek > 2: nGen vyhodnoceni za sebou (idx 1..nGen) do jednoho command bufferu
                NVSDK_NGX_Result er = NVSDK_NGX_Result_Success;
                if (g_uaOk && FgHlGen()) FgUiaRecord(cmd, k, hl, *sn);  // maska UI (DlssgUiAlpha) + zbran (v25w)
                for (int g = 1; g <= nGen && NVSDK_NGX_SUCCEED(er); ++g)
                    er = FgRecordDlssg(cmd, k, j, *sn, reset, hl && FgHlGen(), g, nGen);
                if (NVSDK_NGX_FAILED(er)) {
                    // cmd buffer je v neznamem stavu: zahodit a snimek poslat jen jako kopii
                    FgFail("vyhodnoceni DLSS-G selhalo: %s", NgxStr(er).c_str());
                    gen = false;
                    continue;
                }
            }
            // [fg] SharpenGen: generovane snimky doostrit (vsechny rezimy generatoru)
            if (g_fsOk) {
                const int nS = afg ? apl.n : fsrGen ? 1 : nGen;
                for (int g = 0; g < nS; ++g) FgSharpRecord(cmd, g_fgOut[j].interp[g].img);
            }
            if (ts) {
                vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_fgQpool, c * 2 + 1);
                g_fgTsUsed[c] = true;
                g_fgTsGen[c] = adapt ? apl.n : -1;
            }
            VkImageMemoryBarrier post[2] = {
                IB(g_fgPres[k].img, COLOR, VK_ACCESS_SHADER_READ_BIT, 0, SRO, GEN),
                hl ? IB(g_fgHl[k].img, COLOR, VK_ACCESS_SHADER_READ_BIT, 0, SRO, GEN) : VkImageMemoryBarrier{}};
            PB(cmd, ALL, ALL, hl ? 2 : 1, post);
            if (hl && FgHlGen()) ++g_fgSt.hudless;
            if (fsrGen || adapt) {  // FSR si skutecny snimek nevyrabi: kopie backbufferu do "real" (jako bez generatoru)
                VkImageMemoryBarrier rb = IB(g_fgOut[j].real.img, COLOR, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                             VK_ACCESS_TRANSFER_WRITE_BIT, GEN, GEN);
                PB(cmd, ALL, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, &rb);
                VkImageCopy rc = {};
                rc.srcSubresource = {COLOR, 0, 0, 1};
                rc.dstSubresource = {COLOR, 0, 0, 1};
                rc.extent = {g_fgBbW, g_fgBbH, 1};
                vk.vkCmdCopyImage(cmd, g_fgPres[k].img, GEN, g_fgOut[j].real.img, GEN, 1, &rc);
            }
            evalDone = true;
        } else if (j >= 0) {
            VkImageMemoryBarrier pre = IB(g_fgOut[j].real.img, COLOR, 0, VK_ACCESS_TRANSFER_WRITE_BIT, GEN, GEN);
            PB(cmd, ALL, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, &pre);
            VkImageCopy rc = {};
            rc.srcSubresource = {COLOR, 0, 0, 1};
            rc.dstSubresource = {COLOR, 0, 0, 1};
            rc.extent = {g_fgBbW, g_fgBbH, 1};
            vk.vkCmdCopyImage(cmd, g_fgPres[k].img, GEN, g_fgOut[j].real.img, GEN, 1, &rc);
        }
        break;
    }
    VkImageMemoryBarrier rel[2] = {
        IB(g_fgPres[k].img, COLOR, VK_ACCESS_TRANSFER_READ_BIT, 0, GEN, GEN, g_qf, EXT),
        hl ? IB(g_fgHl[k].img, COLOR, VK_ACCESS_SHADER_READ_BIT, 0, GEN, GEN, g_qf, EXT) : VkImageMemoryBarrier{}};
    PB(cmd, ALL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, hl ? 2 : 1, rel);
    vk.vkEndCommandBuffer(cmd);

    const uint64_t seq = ++g_fgSeq;
    g_gwPost[seq & 63] = posted;
    MemoryBarrier();
    if (!g_gwStarted) {
        g_gwStarted = true;
        std::thread(FgGpuWatchThread).detach();
    }
    VkSemaphore sigS[2] = {g_fgSemDone, g_fgSemEval};
    uint64_t sigV[2] = {N, seq};
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = 1;
    ti.pWaitSemaphoreValues = &N;
    ti.signalSemaphoreValueCount = 2;
    ti.pSignalSemaphoreValues = sigV;
    VkPipelineStageFlags waitStage = ALL;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ti};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_fgSemReady;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = sigS;
    VkResult r;
    {
        std::lock_guard<std::mutex> lk(g_queueMx);
        r = vk.vkQueueSubmit(g_queue, 1, &si, g_fgFence[c]);
    }
    if (r != VK_SUCCESS) {
        if (j >= 0) {
            std::lock_guard<std::mutex> lk(g_fgMx);
            g_fgOut[j].state = FGO_FREE;
        }
        FgPresFatal("vkQueueSubmit (snimek presentu) %s", FgVkStr(r));
        return FgStop("odeslani snimku selhalo");
    }
    g_fgFenceUsed[c] = true;
    MemoryBarrier();
    ps->helperAck = N;
    SetEvent(g_fgEvDone);  // proxy muze obraz znovu pouzit (na GPU ceka na semDone)

    const bool showInterp = evalDone && !reset && nGen > 0;
    if (afg) g_afgPrevPaired = evalDone;  // dalsi par navazuje jen na snimek, ktery prosel AfgRecordPair
    if (evalDone && !adapt && mult != g_fgLastMult) {
        Log("FG: nasobek %dx (pozadovano %ux, %s umi az %dx)", mult, reqMult,
            fsrMulti ? "FSR (vlastni generator AFG)" : fsrGen ? "FSR FG" : "DLSS-G",
            fsrMulti ? kAfgMaxInterp + 1 : fsrGen ? 2 : g_fgMultMax);
        g_fgLastMult = mult;
    }
    if (evalDone) {
        g_fgLastEvalB = B;
        g_fgFresh = false;
        ++g_fgSt.evals;
        if (showInterp) ++g_fgGenTotal;
    } else {
        g_fgFresh = true;  // po kazdem snimku bez vyhodnoceni historie neplati
    }
    if (j >= 0) {
        std::lock_guard<std::mutex> lk(g_fgMx);
        // vlakno presentu nestiha: nejstarsi polozku zahodit (rozvrh A drzi polozky do jejich casu zobrazeni: az 3)
        if (g_fgQn >= (adapt && apl.fixedTimes ? 3 : 2)) {
            g_fgOut[g_fgQ[0].slot].state = FGO_FREE;
            g_fgQ[0] = g_fgQ[1];
            g_fgQ[1] = g_fgQ[2];
            --g_fgQn;
            std::lock_guard<std::mutex> sl(g_fgStatMx);
            ++g_ptSt.dropped;
        }
        FgItem it = {j, showInterp ? nGen : 0, seq, N, posted, g_fgIntervalMs};
        it.content[kFgMaxInterp] = posted;
        if (adapt && showInterp) {
            for (int g = 0; g < nGen; ++g) {
                it.disp[g] = apl.disp[g];
                it.content[g] = apl.content[g];
            }
        } else if (showInterp && consecutive && prevPosted) {
            for (int g = 0; g < nGen; ++g)
                it.content[g] = prevPosted + (posted - prevPosted) * (g + 1) / (nGen + 1);
        }
        if (adapt && evalDone && apl.fixedTimes) {  // rozvrh A: pevne casy (i kdyz se ukaze jen skutecny snimek)
            it.fixedTimes = true;
            it.nInterp = showInterp ? nGen : 0;
            it.showReal = apl.showReal;
            it.dispReal = apl.dispReal;
            it.intervalMs = apl.periodMs;
        }
        g_fgQ[g_fgQn++] = it;
        g_fgCv.notify_all();
    }

    // Stav pro proxy (limiter jen pri BMSD_FGS_ACTIVE) a menu
    if (!want) {
        const bool unimpl = reqMode != BMSD_FG_OFF;
        FgPublishState(unimpl ? BMSD_FGS_UNAVAILABLE : BMSD_FGS_OFF, 0, unimpl ? "tento rezim zatim neni implementovan" : "");
    } else if (!FgAvailFor(reqMode)) {
        FgPublishState(BMSD_FGS_UNAVAILABLE, 0, (fsrGen ? g_ffgAvailWhy : g_fgAvailWhy).c_str());
    } else if (g_fgFailedM[reqMode]) {
        FgPublishState(BMSD_FGS_FAILED, 0, g_fgFailWhyM[reqMode].c_str());
    } else if (showInterp || adaptRunning) {
        // adaptivni FSR bezi i v mrtve zone (jen skutecne snimky) - pro proxy stale ACTIVE (limiter na cil / strop)
        g_fgActMult = mult;
        FgPublishState(BMSD_FGS_ACTIVE, reqMode, adapt && !g_ad.engaged ? "hra stiha cil - jen skutecne snimky" : "");
    } else {
        const char* why = ps->fgPause        ? "otevrene menu modu"
                          : !windowOk        ? "okno hry neni videt"
                          : !have            ? "ceka se na obrazy mostu (DLSS / FSR musi bezet)"
                          : !B               ? "snimek bez mostu (menu hry, nacitani, nebo je mod vypnuty)"
                          : !sn              ? "pro snimek nejsou MV (upscaler DLSS / FSR nebezi)"
                          : !subOk           ? "zmenseny render (cast B) bez DynamicRes"
                          : hitch            ? "zaseknuti hry (dlouhy interval snimku)"
                          : reset            ? "reset historie (strih, prvni snimek)"
                                             : "";
        FgPublishState(BMSD_FGS_PAUSED, 0, why);
    }

    // Ladeni: ulozit dvojice interp / real
    if (showInterp && g_fgDumpLeft > 0 && g_fgGenTotal > (unsigned)g_fgCfg.dumpAfter && FgDumpArmed()) {
        if (vk.vkWaitForFences(g_dev, 1, &g_fgFence[c], VK_TRUE, 1000000000ull) == VK_SUCCESS) {
            wchar_t p1[MAX_PATH + 64], p2[MAX_PATH + 64], p0[MAX_PATH + 64];
            swprintf(p2, MAX_PATH + 64, L"%ls\\fgdump_%02d_real.bmp", g_dir.c_str(), g_fgDumpIdx);
            swprintf(p0, MAX_PATH + 64, L"%ls\\fgdump_%02d_prev.bmp", g_dir.c_str(), g_fgDumpIdx);
            // generovane snimky: 2x _interp.bmp, 3x / 4x _interp1.bmp .. _interp3.bmp (cas 1/m, 2/m, ...)
            bool ok = true;
            for (int g = 0; g < nGen && ok; ++g) {
                if (nGen == 1) swprintf(p1, MAX_PATH + 64, L"%ls\\fgdump_%02d_interp.bmp", g_dir.c_str(), g_fgDumpIdx);
                else swprintf(p1, MAX_PATH + 64, L"%ls\\fgdump_%02d_interp%d.bmp", g_dir.c_str(), g_fgDumpIdx, g + 1);
                ok = FgDumpImage(g_fgOut[j].interp[g].img, g_fgBbW, g_fgBbH, p1);
            }
            ok = ok && FgDumpImage(g_fgOut[j].real.img, g_fgBbW, g_fgBbH, p2);
            // predchozi skutecny snimek (N - 1): "real" minuleho vystupniho slotu - obsah drzi, dokud slot nedostane
            // dalsi snimek (zapisuje jen toto vlakno). Presna trojice minuly / generovany / aktualni pro kontrolu.
            const bool prevOk = g_fgPrevOutSlot >= 0 && g_fgPrevOutSlot != j && g_fgPrevOutN + 1 == N;
            if (ok && prevOk) ok = FgDumpImage(g_fgOut[g_fgPrevOutSlot].real.img, g_fgBbW, g_fgBbH, p0);
            // HUD-less snimku N (K5; obraz pak proxy prepise az se snimkem N + 3, dump je hned po fence)
            bool hlDump = false;
            if (ok && hl) {
                wchar_t p3[MAX_PATH + 64];
                swprintf(p3, MAX_PATH + 64, L"%ls\\fgdump_%02d_hudless.bmp", g_dir.c_str(), g_fgDumpIdx);
                hlDump = FgDumpImage(g_fgHl[k].img, g_fgBbW, g_fgBbH, p3);
            }
            Log("FG: dump %d (N %llu, B %llu, nasobek %dx) %s%s%s", g_fgDumpIdx, (unsigned long long)N,
                (unsigned long long)B, mult, ok ? "ulozen (fgdump_NN_interp*.bmp / _real.bmp vedle helperu)" : "SELHAL",
                prevOk ? " + _prev.bmp (minuly skutecny snimek)" : "", hlDump ? " + _hudless.bmp" : hl ? "" : " (bez HUD-less)");
            if (adapt) {
                char ts[96] = "";
                for (int g = 0; g < nGen; ++g) snprintf(ts + strlen(ts), sizeof ts - strlen(ts), " %.4f", apl.t[g]);
                Log("FG: dump %d adaptivni FSR rozvrh %s, t =%s", g_fgDumpIdx, apl.fixedTimes ? "A" : "B", ts);
            }
            ++g_fgDumpIdx;
            --g_fgDumpLeft;
        }
    }
    g_fgPrevOutSlot = j;  // pro dump pristiho snimku
    g_fgPrevOutN = N;
}

// Konec helperu (hra skoncila): vlakno presentu ukoncit, swapchain uvolnit.
static void FgShutdown() {
    if (!g_fgVkOk) return;
    if (g_fgThread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(g_fgMx);
            g_fgQuit = true;
            g_fgQuiesce = false;
            g_fgCv.notify_all();
        }
        g_fgThread.join();
    }
    if (FgWaitGpuIdle()) {
        FgReleaseFeature("konec");
        FgDestroySnaps();
    }
    if (g_fgParams) NVSDK_NGX_VULKAN_DestroyParameters(g_fgParams);
    g_fgParams = nullptr;
}
