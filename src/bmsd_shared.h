// Sdilena pamet mezi 32bitovou proxy (ve hre) a 64bitovym pomocnym procesem.
// Jmena objektu: Local\bms_dlss_<pid hry>        - file mapping s BmsdShared
//                Local\bms_dlss_<pid hry>_frame  - event: proxy -> helper (novy snimek / nove zdroje)
//                Local\bms_dlss_<pid hry>_done   - event: helper -> proxy (prace odeslana na GPU)
// Handly (pameti obrazu, semafory) proxy duplikuje primo do procesu helperu.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define BMSD_MAGIC 0x44534D42u  // "BMSD"
#define BMSD_VERSION 15  // 15 = helperBusyTick (dlouha prace hlavniho vlakna helperu); 14 = maska reaktivity
#define BMSD_HAS_POSTQPC 1  // BmsdShared::postQpc (doplneno ve verzi 8, viz konec struktury)

// Upscaler ([upscale] Upscaler=auto|dlss|fsr|xess|off, menu SetUpscaler; od verze 11, BmsdShared::upscaler). Cisla
// jsou shodna s menu (0 = vyp, 1 = DLSS, 2 = FSR, 3 = XeSS, 4 = auto). Helper zkousi pozadovany upscaler a pri selhani
// dalsi v poradi DLSS -> FSR (u DLSS jen kdyz je DLSS trvale nedostupne); nic = kopie. Skutecny stav: helperUpscaler.
enum {
    BMSD_UPSCALER_OFF = 0,   // bez upscaleru: helper jen kopiruje (dlaaStatus = -1, jitter se nevynucuje)
    BMSD_UPSCALER_DLSS = 1,  // NVIDIA DLSS / DLAA pres NGX (vychozi)
    BMSD_UPSCALER_FSR = 2,   // AMD FSR pres ffx API (podepsane DLL z FidelityFX SDK 2.4; na ne-AMD GPU FSR 3.1.x)
    BMSD_UPSCALER_XESS = 3,  // Intel XeSS - zatim neimplementovano (helper ho bere jako nedostupny, viz vyse)
    BMSD_UPSCALER_AUTO = 4,  // od verze 13 (vychozi): DLSS, kdyz jde, jinak FSR - bez hlasky o nahradnim upscaleru
    BMSD_UPSCALER_COUNT
};

// Sdilene obrazy (velikost sceny). OBJMV_IN (od verze 9, RGBA32F): motion vektory pohyblivych objektu, ktere proxy
// na konci sceny prehraje do vlastniho cile (src/objmv.inl, src/shaders/objmv.hlsl):
//   R, G = MV v pixelech vstupu (pozice v minulem snimku - pozice v tomto, bez jitteru, +x doprava, +y dolu),
//   B    = hloubka fragmentu z/w (0 = near, 1 = far; stejna skala jako DEPTH_IN),
//   A    = 0 nic, 1 pohyblivy objekt, 2 staticky objekt (jen rezim validace).
// Proxy do nej kopiruje jen kdyz objMvCount > 0; jinak je obsah neplatny a helper ho necte.
enum {
    BMSD_IMG_COLOR_IN,
    BMSD_IMG_DEPTH_IN,
    BMSD_IMG_LINDEPTH_IN,
    BMSD_IMG_COLOR_OUT,
    BMSD_IMG_OBJMV_IN,
    BMSD_IMG_MASK_IN,  // od verze 14 (B8G8R8A8): maska reaktivity proxy (src/reactive.inl) - RGB = pridana barva
                       // aditivnich draw, A = pokryti pruhlednych; plati jen pri maskDraws > 0
    BMSD_IMG_COUNT
};

// MV pohyblivych objektu (BmsdShared::objMvMode, bity; [mv] ObjectMV a Validate, klavesa Alt+Backspace)
enum {
    BMSD_OBJMV_OFF = 0,
    BMSD_OBJMV_ON = 1,        // helper bere MV z OBJMV_IN tam, kde A > 0.5 a sedi hloubka
    BMSD_OBJMV_VALIDATE = 2,  // navic: proxy prehrava i staticke objekty (A = 2), helper meri rozdil proti MV kamery
    BMSD_OBJMV_VIEWMODEL = 4  // proxy prehrava i zbran v ruce ([mv] Viewmodel): helper u ni bere MV z OBJMV_IN misto 0
};

// Vstup DLSS ([dlaa] InputMode, Alt+Insert). Zmena se predava jako zmena presetu (DLSS feature znovu).
enum {
    BMSD_INPUT_LDR = 0,      // barva sceny primo (sRGB, RGBA8), IsHDR = 0
    BMSD_INPUT_HDR = 1,      // sRGB -> linearni RGBA16F, IsHDR, pevna expozice 1.0
    BMSD_INPUT_HDR_AUTO = 2, // jako 1, ale AutoExposure misto pevne expozice
    BMSD_INPUT_HDR_ITM = 3,  // jako 2 + inverzni tone mapping (k = [dlaa] ITMStrength, cte helper i proxy z ini)
    BMSD_INPUT_COUNT
};

// Hloubka pro DLSS ([dlaa] DepthMode, Alt+F5; pokus kvuli chveni pletiva s presety L/M). Predava se kazdy
// snimek (jako ostrost), DLSS feature se nemeni; helper pri zmene jen resetuje historii.
enum {
    BMSD_DEPTH_RAW = 0,    // DEPTH_IN (D24, aspekt hloubky) primo
    BMSD_DEPTH_MIN3 = 1,   // R32F = minimum hloubky v okoli 3x3 (nejblizsi povrch), pocita depthmin.comp
    BMSD_DEPTH_CONST = 2,  // diagnostika: R32F = 1.0 (far) vsude
    BMSD_DEPTH_COUNT
};

// Filtr zmenseni vystupu DLSS pri OutputScale > 1 ([upscale] DownsampleFilter, Alt+F1). Push konstanta
// downsample.comp, predava se kazdy snimek (DLSS feature se nemeni).
enum {
    BMSD_DOWN_LANCZOS2 = 0,
    BMSD_DOWN_CATMULLROM = 1,
    BMSD_DOWN_MITCHELL = 2,  // B = C = 1/3, bez prekmitu
    BMSD_DOWN_BOX = 3,       // plosny box (presny prekryv texelu se stopou vystupniho pixelu), jen pro porovnani
    BMSD_DOWN_COUNT
};

// Cisla rezimu jsou i hodnoty StartMode v bms_dlss.ini.
enum {
    BMSD_MODE_OFF = 0,
    BMSD_MODE_COPY = 1,     // ladeni: kopie tam a zpet
    BMSD_MODE_MIRROR = 2,   // ladeni: zrcadleni
    BMSD_MODE_DLAA = 3,     // DLAA pres NGX (pri selhani NGX helper jen kopiruje)
    BMSD_MODE_MVDEBUG = 4,  // ladeni: vizualizace motion vektoru misto obrazu
    BMSD_MODE_COUNT
};

#pragma pack(push, 8)
struct BmsdImage {
    uint64_t handle;     // NT handle pameti (platny v procesu helperu)
    uint64_t allocSize;  // velikost alokace (dedikovana)
    int32_t format;      // VkFormat
    uint32_t usage;      // VkImageUsageFlags
    uint32_t width;
    uint32_t height;
};

struct BmsdShared {
    // Od verze 3 jsou magic, version a helperMsg na pevnem miste, aby helper mohl nahlasit
    // i nesouhlas verzi (proxy a helper z ruznych buildu).
    uint32_t magic;
    uint32_t version;
    char helperMsg[256];       // posledni chyba nebo varovani helperu
    uint8_t deviceUUID[16];

    uint64_t semProxyHandle;   // timeline: proxy signalizuje "vstupy zkopirovany" = N
    uint64_t semHelperHandle;  // timeline: helper signalizuje "vystup hotov" = N
    BmsdImage img[BMSD_IMG_COUNT];

    uint64_t frameValue;       // N; 0 = zadna prace (jen import novych zdroju)
    uint64_t helperDoneValue;  // posledni N, ktere helper odeslal na GPU (0 po zprave bez snimku)
                               // Proxy smi zapsat dalsi snimek az po potvrzeni predchoziho: helper cte jen posledni
                               // frameValue, preskoceny snimek by GPU nikdy nesignalizovala (viz interop.cpp, pipeline).
    float cam[16];        // cViewProj tohoto snimku (bez jitteru), registry c8..c11
    float prevCam[16];    // totez z minuleho snimku
    float jitterX, jitterY;  // jitter pridany do matic v pixelech (+x doprava, +y dolu = posun obrazu)

    int32_t generation;        // proxy zvysi pri nove sade zdroju
    int32_t helperGeneration;  // helper nastavi po importu
    int32_t helperReady;       // 1 = Vulkan v helperu inicializovan
    int32_t helperError;       // != 0 = helper selhal, viz helperMsg
    uint32_t mode;             // BMSD_MODE_*
    uint32_t camValid;         // cam i prevCam jsou platne
    uint32_t width, height;
    uint32_t resetHint;        // proxy: predchozi snimek hry nesel mostem -> historie DLAA neplati
    int32_t dlaaStatus;        // helper: 0 = zatim nic, 1 = DLAA pripraveno, -1 = DLAA nedostupne (helper kopiruje)
    uint32_t preset;           // proxy: preset DLSS (0 = vychozi, jinak NVSDK_NGX_DLSS_Hint_Render_Preset, K = 11)
                               // zmena se predava s frameValue = 0, helper pak znovu vytvori DLSS feature
    uint32_t helperPreset;     // helper: preset, ktery naposledy zpracoval (potvrzeni zmeny)
    float sharpness;           // proxy: doostreni po DLSS (RCAS) 0..1, 0 = vypnuto ([dlaa] Sharpness, Ctrl+Home)
    int32_t liveJitterSignX;   // proxy: znamenko jitteru z klavesy (Ctrl+F11 v DLAA), +-1, nasobi se s ini
    int32_t liveJitterSignY;
    // Od verze 5:
    uint32_t inputMode;        // proxy: vstup DLSS (BMSD_INPUT_*), zmena se predava s frameValue = 0 jako preset
    uint32_t helperInputMode;  // helper: inputMode, ktery naposledy zpracoval (potvrzeni zmeny; pri nepodporovanem
                               // RGBA16F helper bezi s LDR, potvrdi ale pozadovanou hodnotu)
    // Od verze 6:
    uint32_t depthMode;        // proxy: hloubka pro DLSS (BMSD_DEPTH_*), kazdy snimek; bez R32F helper pouzije 0
    // Od verze 7: supersampling uvnitr modu ([upscale] OutputScale, Alt+F3). Tri velikosti:
    //   vstup DLSS (render)   - width x height, v casti B (od verze 10) sub-rect renderWidth x renderHeight,
    //   vystup DLSS           - round(width * S) x round(height * S), jen v helperu (vlastni obrazy),
    //   finalni obraz         - width x height = sdilene obrazy; helper vystup DLSS zmensi zpet (downsample.comp).
    float outScale;            // proxy: pozadovane S (1 = DLAA, 4/3, 1.5, 1.724, 2, 3); zmena se predava
                               // s frameValue = 0 jako preset (helper znovu vytvori feature a obrazy velikosti vystupu)
    float helperOutScaleAck;   // helper: outScale, ktery naposledy zpracoval (potvrzeni pozadavku, i kdyz nesel)
    float helperOutScale;      // helper: S, se kterym DLSS skutecne bezi (1 pri fallbacku nebo bez DLSS)
    uint32_t helperOutWidth;   // helper: skutecny vystup DLSS (= width x height pri S = 1)
    uint32_t helperOutHeight;
    int32_t helperPerfQuality; // helper: NVSDK_NGX_PerfQuality_Value feature (5 = DLAA), -1 = DLSS nebezi
    // Od verze 8 (pomucky pro A/B test chveni plotu pri S > 1), proxy kazdy snimek:
    uint32_t downsampleFilter; // BMSD_DOWN_* ([upscale] DownsampleFilter, Alt+F1)
    uint32_t jitterPhases;     // pocet fazi jitteru tohoto snimku (0 = neznamy); zmena = helper resetuje historii DLSS
    uint32_t jitterPhasesAuto; // 1 = auto 8*S^2 ([jitter] Phases=0), 0 = pevny pocet (ini nebo Alt+F2); jen do logu
    // Doplneno ve verzi 8 (zpetne kompatibilni: starsi proxy tu ma nuly, starsi helper pole necte):
    uint64_t postQpc;          // proxy: QueryPerformanceCounter tesne pred SetEvent(frame) se snimkem N (0 = nezname);
                               // helper z nej meri zpozdeni probuzeni (QPC je spolecny pro vsechny procesy)
    // Od verze 9: MV pohyblivych objektu (obraz BMSD_IMG_OBJMV_IN). Proxy kazdy snimek:
    uint32_t objMvMode;        // BMSD_OBJMV_* (0 = vypnuto)
    float objMvTol;            // tolerance hloubky: objekt plati, kdyz |d_obj - d_buf| <= max(1e-6, tol * (1 - d_buf))
    uint32_t objMvCount;       // draw prehranych do OBJMV_IN v tomto snimku; 0 = obraz se nekopiroval, helper ho ignoruje
    // Helper -> proxy: statistika MV objektu za posledni interval logu helperu (~5 s); proxy ji jen pripise do logu
    uint32_t objMvStatSeq;     // poradove cislo statistiky (zmena = nova data)
    uint32_t objMvStatFrames;  // snimku s MV objektu v intervalu
    float objMvStatAccepted;   // pixelu na snimek prijatych z OBJMV_IN (A > 0.5 a hloubka sedi)
    float objMvStatRejected;   // pixelu na snimek s A > 0.5, ktere neprosly hloubkou (zakryte, nebo mala tolerance)
    float objMvStatValPixels;  // validace: pixelu na snimek ze statickych objektu (A = 2)
    float objMvStatValMean;    // validace: prumer |MV objektu - MV kamery| v pixelech (cil < 0.05)
    float objMvStatValMax;     // validace: maximum (orezano na 64 px)
    // Od verze 10: cast B = skutecny upscaling (hra kresli scenu mensi pres mat_viewportscale, DLSS ji dopocita na
    // W x H misto enginoveho roztazeni). Sdilene obrazy zustavaji W x H; platny vstup je jen sub-rect od (0, 0):
    //   render (vstup DLSS)  - renderWidth x renderHeight, proxy kopiruje jen tento region (barva, hloubka, OBJMV),
    //   vystup DLSS = finalni obraz - width x height (v casti B se nesupersampluje, outScale se nepouzije).
    // Zmena se predava jako preset (frameValue = 0, helper znovu vytvori feature, bez reimportu); pri novych
    // zdrojich se zapisuje pred ++generation. 0 nebo hodnota >= width / height = cely obraz (cast A).
    uint32_t renderWidth;       // proxy
    uint32_t renderHeight;
    uint32_t helperRenderAckW;  // helper: renderWidth / renderHeight, ktere naposledy zpracoval (potvrzeni pozadavku)
    uint32_t helperRenderAckH;
    int32_t helperRenderOk;     // helper: 1 = DLSS feature pro tento vstup existuje a bezi; 0 = ne (v casti B to neni
                                // trvale selhani: proxy pak nedela nic a obraz roztahne engine; novy pokus pri
                                // dalsi zmene renderu nebo reimportu)
    // Od verze 12 (drive reserved10): proxy pri spusteni helperu - 1 = prezentace bezi v tomto (hlavnim) helperu
    // ([fg] Mode != 0 pri startu hry): helper zapne rozsireni pro swapchain (VK_KHR_surface, win32_surface, swapchain),
    // druhou frontu pro present a hleda mapovani BmsdPresentShared. 0 = helper se chova presne jako ve verzi 11.
    uint32_t fgPresenter;
    // Od verze 11: volba upscaleru (BMSD_UPSCALER_*). Zmena se predava jako preset (frameValue = 0, helper uvolni
    // dosavadni DLSS feature / FSR kontext a vytvori novy; proxy ceka na helperUpscalerAck). Pri novych zdrojich se
    // zapisuje pred ++generation (jako preset).
    uint32_t upscaler;            // proxy: pozadovany upscaler
    uint32_t upscalerValid;       // proxy: 1 = pole upscaler plati; 0 (proxy bez podpory volby) = helper bere DLSS
    uint32_t helperUpscalerAck;   // helper: upscaler, ktery naposledy zpracoval (potvrzeni pozadavku, i kdyz nesel)
    int32_t helperUpscaler;       // helper: skutecne bezici upscaler (BMSD_UPSCALER_*); 0 = zadny (helper kopiruje)
    char helperUpscalerName[32];  // helper: nazev pro menu a log vcetne verze ("DLSS 310.9.16" = verze skutecne
                                  // nactene nvngx_dlss - i z NVIDIA App override, "FSR 3.1.6", ...; "" = zadny)
    // Od verze 13:
    int32_t helperDlssBroken;     // helper: 1 = DLSS je na tomto systemu trvale nedostupne (NGX init selhal, GPU bez
                                  // DLSS, chybi DLL) -> menu volbu DLSS sedi ("jen NVIDIA RTX"); 0 = jde / zatim nevi
    int32_t helperFsrBroken;      // helper (od v17, driv rezerva = 0): 1 = FSR a FSR FG 2x trvale nedostupne, protoze
                                  // chybi x64 Microsoft Visual C++ Redistributable (DLL AMD ho potrebuji); 0 = jde / nevi
    // Od verze 14: maska reaktivity (BMSD_IMG_MASK_IN). Proxy kazdy snimek: pocet pruhlednych / aditivnich draw
    // v masce; 0 = obraz se nekopiroval, helper masku nepouzije (DLSS / FSR jako ve verzi 13).
    uint32_t maskDraws;
    int32_t helperGpuHint;        // helper (od v18, driv rezerva = 0): 1 = hra (DXVK) bezi na integrovane grafice, prestoze
                                  // je v systemu diskretni; 2 = na GPU bez DLSS, prestoze je v systemu NVIDIA; 0 = v poradku
    // Od verze 15: helper -> proxy. GetTickCount() (| 1) zacatku dlouhe prace hlavniho vlakna helperu, ktera blokuje
    // potvrzovani snimku mostu (vytvoreni generatoru FG, import obrazu presentu); 0 = zadna. Proxy po tu dobu (max
    // 30 s) nepocita vynechane snimky jako selhani helperu a strazce helper neukoncuje.
    uint32_t helperBusyTick;
    // Od v24u (bez zmeny verze, driv rezerva = 0): helper -> proxy. Skutecny cas od Present hry (postQpc) do dokonceni
    // prace helperu pro snimek na GPU: horni 16 bitu = poradi mereni (1..65535, meni se s kazdym), dolni = cas v 0,01 ms.
    // 0 = zatim nemereno. Pouziva regulator limiteru FG podle casu GPU ([fg] LimiterAdapt=3).
    uint32_t fgGpuDone;
};
#pragma pack(pop)

// Stejne rozlozeni v 32bitove proxy i 64bitovem helperu (mapovani ma presne tuto velikost).
static_assert(sizeof(BmsdImage) == 32, "BmsdImage");
static_assert(sizeof(BmsdShared) == 896, "BmsdShared");

// ---------------------------------------------------------------------------------------------------------------
// Prezentacni kanal (spike S2 a od verze 2 Frame Generation), nezavisly na mostu DLSS. DXVK prezentuje do skryteho
// okna, helper ma Vulkan swapchain na prezentacnim okne (dcerine okno proxy pres klientskou plochu hry, nebo HWND
// hry) a kazdy finalni snimek (backbuffer v Present, i s HUD a menu) do nej zkopiruje (blit). Jmena:
// Local\bms_dlss_<pid>_present (mapovani), ..._pframe (proxy -> helper: novy snimek), ..._pdone (helper -> proxy:
// snimek prevzat). Dve podoby:
//   * [present] HelperPresent=1 a [fg] Mode=0: druha instance helperu "bms_dlss_helper.exe <pid> present"
//     (src/helper/present.cpp), jen kopie snimku (S2);
//   * Frame Generation zapnuta (pri startu nebo za behu, presenterInMain = 1): prezentuje hlavni helper (src/helper/fg.inl) - ma MV,
//     hloubku a kameru z mostu a umi mezi skutecne snimky vkladat generovane (DLSS-G; FSR FG pozdeji na stejnem
//     rozhrani generatoru).
// Snimek N lezi v obrazu img[N % BMSD_PRESENT_IMAGES]. Proxy: kopie backbufferu -> img, signal semReady = N.
// Helper: ceka semReady >= N, obraz precte (blit do swapchainu / vstup generatoru + kopie do vlastniho obrazu),
// signal semDone = N (i kdyz snimek nezobrazi - minimalizace, okno obsazene), pak helperAck = N. Proxy obraz snimku
// M znovu pouzije az pri helperAck >= M (na GPU pak ceka semDone >= M); helper bere vzdy jen posledni zverejneny
// snimek.
#define BMSD_PRESENT_MAGIC 0x50534D42u  // "BMSP"
#define BMSD_PRESENT_VERSION 7  // 7 = zobrazeni v procesu hry (gp*), 3 = HUD-less vstup FG (K5, hudless[]), 4 = nasobek FG 2x / 3x / 4x a citace snimku,
                                // 5 = FG zap / vyp za behu (restartSeq), 6 = adaptivni FSR FG (adapt*)
#define BMSD_PRESENT_IMAGES 3
#define BMSD_GP_IMAGES 3

// BmsdPresentShared::helperState
enum {
    BMSD_PST_START = 0,      // helper startuje / zatim zadny swapchain
    BMSD_PST_PRESENTING = 1, // swapchain na prezentacnim okne existuje a prezentuje
    BMSD_PST_NOWINDOW = 2,   // okno je minimalizovane nebo 0x0 - ceka (snimky jen potvrzuje)
    BMSD_PST_INUSE = 3,      // vkCreateSwapchainKHR: VK_ERROR_NATIVE_WINDOW_IN_USE_KHR - zkousi znovu
    BMSD_PST_STOPPED = 4,    // presenterInMain: proxy pozadala o konec (stop = 1), swapchain je uvolnen; proces helperu
                             // bezi dal (most DLSS)
    BMSD_PST_FAILED = -1     // trvala chyba: helper uvolni swapchain (S2: a skonci)
};

// Frame Generation ([fg] Mode, menu; BmsdPresentShared::fgMode / fgActiveMode). Cisla jsou shodna s menu.
enum {
    BMSD_FG_OFF = 0,
    BMSD_FG_DLSSG = 1,     // NVIDIA DLSS Frame Generation 2x pres prime NGX (nvngx_dlssg.dll vedle helperu)
    BMSD_FG_FSR2X = 2,     // AMD FSR Frame Generation 2x pres ffx API bez swapchainu (amd_fidelityfx_framegeneration_vk.dll
                           // vedle helperu; na vsech GPU FSR FG 3.1.x)
    BMSD_FG_FSRADAPT = 3,  // FSR FG adaptivni: vlastni FSR 3.1.4 s libovolnym casem t (src/helper/afg, staticky v helperu)
                           // doplni jen tolik generovanych snimku, kolik chybi do cile FPS
    BMSD_FG_COUNT
};

// BmsdPresentShared::fgState (helper)
enum {
    BMSD_FGS_OFF = 0,           // FG se nepozaduje: prezentuji se jen skutecne snimky
    BMSD_FGS_ACTIVE = 1,        // generuje (generovane + skutecny snimek); JEN v tomto stavu proxy omezuje hru na
                                // targetFps / fgActiveMult
    BMSD_FGS_PAUSED = 2,        // pozadovano, ale ted jen skutecne snimky (menu modu, snimek bez mostu / MV, okno
                                // minimalizovane, prvni snimek po resetu); duvod ve fgMsg
    BMSD_FGS_UNAVAILABLE = -1,  // pozadovany rezim na tomto systemu neni (chybi DLL, NGX ho odmita, neimplementovano)
    BMSD_FGS_FAILED = -2        // vytvoreni nebo vyhodnoceni selhalo: FG je do restartu hry vypnute (jen skutecne)
};

#pragma pack(push, 8)
struct BmsdPresentShared {
    uint32_t magic;
    uint32_t version;
    char helperMsg[256];       // posledni chyba helperu
    uint8_t deviceUUID[16];
    uint64_t gameHwnd;         // prezentacni okno (cil helperu)
    uint64_t semReadyHandle;   // timeline: proxy "snimek N zkopirovan" (handle v procesu helperu)
    uint64_t semDoneHandle;    // timeline: helper "snimek N precten / preskocen"
    BmsdImage img[BMSD_PRESENT_IMAGES];
    int32_t generation;        // proxy zvysi pri nove sade obrazu
    int32_t helperGeneration;  // helper po importu
    uint32_t width, height;    // velikost obrazu (= backbuffer)
    uint32_t presentInterval;  // proxy: 0 = bez VSync (D3DPRESENT_INTERVAL_IMMEDIATE), 1 = VSync
    uint32_t stop;             // proxy -> helper: uvolnit swapchain a skoncit (presenterInMain: jen prestat
                               // prezentovat, stav BMSD_PST_STOPPED)
    uint64_t frameValue;       // posledni zverejneny snimek N (0 = zadny)
    uint64_t postQpc;          // QPC tesne pred SetEvent(pframe) se snimkem N
    uint64_t helperAck;        // posledni N, pro ktery helper odeslal signal semDone
    int32_t helperReady;       // 1 = Vulkan v helperu inicializovan
    int32_t helperState;       // BMSD_PST_*
    int32_t swapchainAlive;    // 1 = helper drzi swapchain na prezentacnim okne (pri cistem konci vzdy 0)
    uint32_t presentMode;      // helper: skutecny VkPresentModeKHR
    uint32_t swapW, swapH;     // helper: velikost swapchainu
    // Statistika helperu za posledni interval (~5 s); statSeq se zvysi po zapisu
    uint32_t statSeq;
    uint32_t statFrames;       // prevzatych snimku
    uint32_t statPresented;    // z toho zobrazeno (vkQueuePresentKHR); s FG vcetne generovanych
    uint32_t statRecreate;     // znovu vytvoreni swapchainu
    float statWakeMs, statWakeMax;      // postQpc -> probuzeni helperu
    float statDoneMs, statDoneMax;      // postQpc -> blit helperu na GPU dokoncen (fence)
    float statPresentMs, statPresentMax; // trvani vkQueuePresentKHR
    float statIntervalMs, statIntervalMax; // interval mezi presenty helperu
    uint32_t measureDisplay;   // proxy: 1 = helper po presentu ceka na blit (semDone) a zobrazeni (present_wait)
    float statDispMs, statDispMax;      // postQpc -> snimek zobrazen (vkWaitForPresentKHR), jen s measureDisplay
    uint32_t presentWaitOk;    // helper: VK_KHR_present_wait zapnuto
    // ---- Od verze 2: prezentace v hlavnim helperu a Frame Generation ----
    uint32_t presenterInMain;  // proxy: 1 = prezentuje hlavni helper (mapovani vzniklo pred jeho spustenim nebo za behu)
    uint32_t fgMode;           // proxy: pozadovany rezim BMSD_FG_* (menu / ini; meni se za behu)
    uint32_t fgPause;          // proxy: 1 = FG pozastavit (otevrene menu modu, [fg] PauseInMenu)
    uint32_t targetFps;        // proxy: strop vystupu ([fg] TargetFps; 0 = auto z obnovovaci frekvence); jen pro log
    uint64_t bridgeFrame;      // proxy: frameValue mostu (B), ktery patri k tomuto snimku hry (MV, hloubka, kamera);
                               // 0 = snimek hry nesel mostem (menu hry, nacitani, most vypnuty). Zapisuje se PRED
                               // frameValue.
    int32_t fgState;           // helper: BMSD_FGS_*
    uint32_t fgAvailMask;      // helper: bit m = rezim BMSD_FG_m je k dispozici (bit 0 vzdy); plati az s fgAvailValid
    uint32_t fgAvailValid;     // helper: 1 = fgAvailMask je znamy (NGX inicializovano)
    uint32_t fgActiveMode;     // helper: rezim, ktery prave generuje (BMSD_FG_*), 0 = zadny
    char fgMsg[128];           // helper: duvod stavu PAUSED / UNAVAILABLE / FAILED (pro menu a log), "" = nic
    // Statistika FG za posledni interval (plati se statSeq)
    uint32_t statFgReal;       // zobrazenych skutecnych snimku
    uint32_t statFgGen;        // zobrazenych generovanych snimku
    uint32_t statFgPaired;     // snimku s platnym B a B = predchozi + 1 (jen kdyz je FG pozadovano)
    uint32_t statFgUnpaired;   // snimku bez B / s mezerou / bez MV (jen kdyz je FG pozadovano)
    float statFgEvalMs, statFgEvalMax;  // GPU cas vyhodnoceni generatoru
    float statFgIntSd;         // smerodatna odchylka intervalu presentu helperu (ms)
    float statFgRealFps;       // skutecnych snimku za sekundu (prevzatych)
    float statFgOutFps;        // zobrazenych snimku za sekundu (skutecne + generovane)
    char fgName[20];           // helper (drive reserved2, starsi helper = ""): nazev a verze generatoru, ktery ma
                               // vytvoreny ("DLSS-G 310.9.16" = skutecne nactena nvngx_dlssg, "FSR FG 3.1.6"); "" = zadny
    // ---- Od verze 3: HUD-less vstup Frame Generation (K5, docs/fg-hudless-vysledek.md) ----
    // Proxy v bode "konec post-processingu, pred HUD" zkopiruje barvu sceny do vlastniho RT a v PostFrame ho spolu
    // s backbufferem zkopiruje do hudless[N % K] (stejny format a velikost jako img[]). Helper ho preda generatoru
    // (DLSS-G HUDless, FSR FG HUDLessColor); rozdil backbuffer - HUD-less = UI (HUD, titulky, menu a OSD modu), ktere
    // se do vlozeneho snimku nepresouva se scenou.
    uint32_t hudlessImages;    // proxy: 1 = obrazy hudless[] v teto generaci existuji ([fg] HudLess=1)
    uint32_t statFgHudless;    // helper: z vyhodnocenych snimku intervalu statistiky s platnym HUD-less
    uint64_t hudlessFrame;     // proxy: N, kdyz hudless[N % K] obsahuje HUD-less snimku N; 0 = snimek bez HUD-less
                               // (bod pred HUD nenalezen). Zapisuje se v seqlocku spolu s bridgeFrame PRED frameValue.
    BmsdImage hudless[BMSD_PRESENT_IMAGES];
    // ---- Od verze 4: nasobek Frame Generation ([fg] Multiplier, menu) a ukazatel FPS ----
    uint32_t fgMultiplier;     // proxy: pozadovany nasobek 2..6 (skutecne + generovane na skutecny snimek)
    uint32_t fgMultMax;        // helper: nejvyssi nasobek, ktery umi DLSS-G na tomto GPU (NGX MultiFrameCountMax + 1;
                               // 2 = jen 2x, 0 = zatim nevi). FSR FG umi jen 2x.
    uint32_t fgActiveMult;     // helper: nasobek, se kterym prave generuje (2..4; plati s fgState = BMSD_FGS_ACTIVE)
    // Od verze 5 (drive reserved4): FG zap / vyp za behu. Proxy po vypnuti FG pozada o konec prezentace (stop = 1 ->
    // BMSD_PST_STOPPED, helper uvolni swapchain, generator, snapshoty a obrazy); pri dalsim zapnuti nastavi gameHwnd
    // (nove prezentacni okno), helperReady = 0, helperState = START, stop = 0 a zvysi restartSeq -> helper prezentaci
    // znovu zapne (nove vlakno presentu, import obrazu nove generace). Semafory a mapovani zustavaji.
    uint32_t restartSeq;
    uint64_t presCountReal;   // helper: zobrazenych skutecnych snimku od startu (roste stale; ukazatel FPS v proxy)
    uint64_t presCountGen;     // helper: zobrazenych generovanych snimku od startu
    // ---- Od verze 6: adaptivni FSR FG (BMSD_FG_FSRADAPT, docs/adaptivni-fsr-vysledek.md) ----
    uint32_t adaptTargetMilli; // proxy: cil vystupu v tisicinach FPS (TargetFps nebo auto f - f^2/3600 minus rezerva)
    uint32_t refreshMilli;     // proxy: obnovovaci frekvence monitoru okna hry v mHz (0 = nezjistena)
    uint32_t adaptFillMilli;   // proxy: A - generovat az pod touto hranici skutecnych FPS (0 = auto 90 % cile)
    uint32_t adaptPacing;      // proxy: vzdy 0 = rozvrh A (B odstranen ve v24z4; pole zustava kvuli rozlozeni)
    uint32_t adaptRealCap;     // proxy: strop skutecnych FPS (limiter v proxy; 0 = jen cil); helperu jen pro log
    uint32_t adaptEngaged;     // helper: 1 = adaptivni rezim prave generuje, 0 = jen skutecne (nad cilem / mrtva zona)
    // ---- Od verze 7: zobrazeni v procesu hry ([fg] GamePresent=1, vychozi) ----
    // Ovladac zapina VRR (G-SYNC / FreeSync v okne) jen procesu, kteremu patri okno v popredi; present z helperu (jiny
    // proces) jde vzdy na pevnou frekvenci monitoru. S gpEnable = 1 helper do swapchainu neprezentuje: kazdy snimek
    // (skutecny i generovany) v case presentu zkopiruje do gpImg[seq % BMSD_GP_IMAGES] (sdilene obrazy helperu, handle
    // zdvojene do procesu hry), signal gpSemReady = seq, zverejni gpPub a SetEvent(Localms_dlss_<pid>_gpframe).
    // Vlakno presenteru v proxy ceka gpSemReady >= seq, obraz zkopiruje do vlastniho swapchainu na prezentacnim okne,
    // signal gpSemDone = seq (i bez zobrazeni) a vkQueuePresentKHR. Helper slot znovu pouzije az pri gpSemDone >= seq.
    uint32_t gpEnable;          // proxy: 1 = zobrazuje proxy (zapisuje se pred startem / znovuzapnutim presenteru)
    int32_t gpGeneration;       // helper: zvysi po zapisu nove sady gpImg a semaforu (zmena velikosti, start)
    int32_t gpProxyGeneration;  // proxy: generace, kterou naposledy importovala (jeji handle zavrela)
    int32_t gpState;            // proxy: stav presenteru proxy (BMSD_PST_*)
    uint64_t gpSemReadyHandle;  // helper: timeline "obraz seq zapsan" (handle v procesu hry)
    uint64_t gpSemDoneHandle;   // helper: timeline "proxy obraz seq precetla"
    BmsdImage gpImg[BMSD_GP_IMAGES];  // helper: obrazy pro zobrazeni (handle v procesu hry)
    uint64_t gpPub;             // helper: posledni zverejneny obraz: (generace & 0xFFFF) << 48 | seq (atomicky zapis)
    uint64_t gpPostQpc;         // helper: QPC zverejneni (QPC je spolecne vsem procesum)
    uint64_t gpDoneSeq;         // proxy: posledni zpracovany seq
    uint32_t gpFlags;           // helper: bit 0 = posledni zverejneny je generovany (jen statistika)
    uint32_t gpPresentMode;     // proxy: VkPresentModeKHR swapchainu
    uint32_t gpSwapW, gpSwapH;  // proxy: velikost swapchainu
    uint32_t gpPresented;       // proxy: zobrazenych celkem (vkQueuePresentKHR uspesne)
    uint32_t gpRecreate;        // proxy: vytvorenych swapchainu celkem
    float gpPresMs, gpPresMax;  // proxy: trvani vkQueuePresentKHR za posledni interval (~5 s)
    float gpWakeMs, gpWakeMax;  // proxy: gpPostQpc -> probuzeni vlakna presenteru
    uint32_t gpStatSeq;         // proxy: zvysi po zapisu statistiky
    uint32_t gpSkipped;         // proxy: preskocenych obrazu celkem (vlakno pozadu, jina generace)
    char gpMsg[128];            // proxy: posledni chyba presenteru proxy
};
#pragma pack(pop)
static_assert(sizeof(BmsdPresentShared) == 1256, "BmsdPresentShared");
static_assert(offsetof(BmsdPresentShared, gpPub) % 8 == 0, "gpPub");
