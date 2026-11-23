#pragma once
#include <d3d9.h>
#include <stdint.h>

void Log(const char* fmt, ...);
const wchar_t* ProxyDir();
// MV pohyblivych objektu (proxy.cpp / objmv.inl): cast radku "vykon" - pocty zaznamenanych, sparovanych a prehranych
// draw na snimek a CPU casy zaznamu a davky za uplynuly interval; volani pocitadla vynuluje.
void ProxyObjMvPerf(char* buf, size_t n);
// Cast B (proxy.cpp): cast radku "vykon" - snimky se zmensenym renderem, potlacena roztazeni enginu a vyjimky za
// uplynuly interval (prazdny retezec, kdyz se v intervalu cast B neobjevila); volani pocitadla vynuluje.
void ProxyPartBPerf(char* buf, size_t n);
// Regulator latence (proxy.cpp / latency.inl): cast radku "vykon" - rezim, podil aktivnich snimku, fronta, spanek,
// odhad latence; prazdny retezec pri [latency] Mode=0. Volani pocitadla vynuluje.
void ProxyLatencyPerf(char* buf, size_t n);
// Maska reaktivity (proxy.cpp / reactive.inl): cast radku "vykon" - podil snimku s maskou, draw na snimek, CPU;
// prazdny retezec pri [upscale] ReactiveMask=0. Volani pocitadla vynuluje.
void ProxyReactivePerf(char* buf, size_t n);

struct SceneEndInfo {
    IDirect3DDevice9* dev;
    IDirect3DSurface9* color;  // barva sceny (RGBA8, velikost backbufferu)
    IDirect3DSurface9* ds;     // depth-stencil sceny (D24S8)
    // Render = platna cast povrchu sceny od (0, 0): cast A cely povrch (0 = cely), cast B sub-rect, do ktereho hra
    // kresli pri mat_viewportscale < 1. Kopiruje se jen on, helper z nej dela cely obraz (BmsdShared::renderWidth).
    uint32_t renderW, renderH;
    const float* cam;          // cViewProj tohoto snimku (bez jitteru)
    const float* prevCam;      // cViewProj minuleho snimku
    bool camValid;
    float jitterX, jitterY;    // jitter v pixelech renderu (0 = bez jitteru)
    uint64_t frame;            // poradove cislo snimku hry (mezera = reset historie DLAA)
    // MV pohyblivych objektu (BMSD_IMG_OBJMV_IN):
    IDirect3DSurface9* objMv;  // cil OBJMV proxy (A32B32G32R32F, velikost sceny); null = v tomto snimku nic
    uint32_t objMvCount;       // pocet prehranych draw (0 = nekopirovat, helper obraz ignoruje)
    uint32_t objMvMode;        // BMSD_OBJMV_*
    float objMvTol;            // tolerance hloubky pro helper ([mv] DepthTol)
    // Maska reaktivity (BMSD_IMG_MASK_IN, src/reactive.inl):
    IDirect3DSurface9* mask;   // cil MASKA proxy (A8R8G8B8, velikost sceny); null = v tomto snimku bez masky
    uint32_t maskDraws;        // pruhlednych / aditivnich draw v masce (0 = nekopirovat, helper masku nepouzije)
    // Funkce zapnute (sdilene obrazy OBJMV_IN / MASK_IN v plne velikosti; vypnute = 1x1, setri VRAM):
    bool objMvOn;              // [mv] ObjectMV / Alt+Backspace
    bool maskOn;               // [upscale] ReactiveMask
};

// Volano jednou za snimek: v casti A kdyz je hlavni scena dokreslena (pred post-processingem), v casti B misto
// enginoveho roztazeni sceny (po post-processingu, pred HUD). true = vysledek mostu se zapise do cele barvy sceny
// (kopie B je odeslana); false = most snimek nezpracoval (vypnuty, timeout, helper nepripraven, v casti B DLSS pro
// dany render nebezi) a scena zustava, jak je - v casti B pak roztazeni musi nakreslit engine.
bool Interop_OnSceneEnd(const SceneEndInfo& info);
// Proc most snimky (v casti B) nezpracovava - text pro jednorazove varovani proxy; nullptr = nic mu nebrani.
const char* Interop_WhyIdle();

// BMSD_MODE_* z bmsd_shared.h: 0 = vypnuto, 1 = kopie, 2 = zrcadleni, 3 = DLAA, 4 = ladeni MV
void Interop_SetMode(uint32_t mode);
uint32_t Interop_GetMode();
// Helper hlasi, ze DLAA / DLSS (NGX) je pripravene a bezi - i pro aktualni render (v casti B to pro nektere
// velikosti nejde; pak se jitter ani mip bias nevynucuji a most v casti B nic nedela).
bool Interop_DlaaReady();
// Preset DLSS (0 = vychozi, 11..13 = K..M). Helper pri zmene znovu vytvori DLSS feature.
void Interop_SetPreset(uint32_t preset);
// Vstup DLSS (BMSD_INPUT_*: 0 LDR, 1 linearni HDR, 2 linearni HDR + auto expozice). Zmena = DLSS feature znovu.
void Interop_SetInputMode(uint32_t mode);
// Doostreni po DLSS (RCAS) 0..1, 0 = vypnuto. Predava se helperu kazdy snimek.
void Interop_SetSharpness(float sharpness);
// Hloubka pro DLSS (BMSD_DEPTH_*: 0 puvodni, 1 min 3x3, 2 konstantni 1.0). Predava se helperu kazdy snimek.
void Interop_SetDepthMode(uint32_t mode);
// Znamenko jitteru predavaneho DLSS z klavesy (+-1), helper ho nasobi znamenky z ini.
void Interop_SetJitterSign(int32_t sx, int32_t sy);
// Supersampling ([upscale] OutputScale, Alt+F3): pozadovane S (1 = DLAA). Zmena = DLSS feature znovu (jako preset).
void Interop_SetOutputScale(float scale);
// S, se kterym DLSS v helperu skutecne bezi (potvrzene helperem; 1 = DLAA, fallback, nebo DLAA nebezi).
float Interop_ActiveOutScale();
// Upscaler ([upscale] Upscaler, menu; BMSD_UPSCALER_*: 0 vyp, 1 DLSS, 2 FSR, 3 XeSS, 4 auto = DLSS, jinak FSR).
// Zmena = feature / kontext znovu (jako preset). Skutecne bezici upscaler (0 = zadny, helper kopiruje) a jeho nazev
// s verzi ("DLSS 310.9.16", "FSR 3.1.6") pro menu / OSD.
void Interop_SetUpscaler(uint32_t upscaler);
int Interop_ActiveUpscaler(const char** name);
// DLSS je podle helperu trvale nedostupne (od verze 13, BmsdShared::helperDlssBroken): 0 = jde / helper se jeste
// nevyjadril, 1 = NGX / DLSS na tomto systemu neni (jen NVIDIA RTX), 2 = jine trvale selhani (viz log helperu).
int Interop_DlssBroken();
// Most se za behu vypnul chybou (jednou true, pak false): proxy ukaze OSD
bool Interop_TakeFailNote();
// Helper hlasi dlouhou praci hlavniho vlakna (helperBusyTick, nejvys 30 s) - limity prezentace pri tom stoji
bool Interop_HelperBusy();
// DXVK hry s neznamou verzi rozhrani interop: proxy ho pak nevola (most ani presenter FG)
void Interop_BlockVk();
bool Interop_VkBlocked();
// Hra (DXVK) bezi na integrovane grafice (1) nebo na GPU bez DLSS (2), prestoze je v systemu vykonnejsi / NVIDIA karta
int Interop_GpuHint();
// BmsdShared::fgGpuDone (helper: skutecny cas post -> hotovo na GPU; regulator [fg] LimiterAdapt=3), 0 = nemereno
uint32_t Interop_FgGpuDone();
// 1 = FSR (a FSR FG 2x) nejde, chybi x64 Microsoft Visual C++ Redistributable (menu: poznamka u upscaleru)
int Interop_FsrBroken();
// Vysledek mostu se v danem snimku hry zapsal zpet do sceny (cela cesta tam a zpet probehla).
bool Interop_FrameBridged(uint64_t frame);
// Most v poslednich ~2 s vynechal konec sceny, protoze barva sceny ma MSAA (hra s MSAA, mod ho neodebral)
bool Interop_MsaaBlocked();
// Frame Generation (od verze 12, src/present.cpp): prezentace v hlavnim helperu.
//   Interop_SetFgPresenter - volat pred spustenim helperu (DllMain): helper dostane BmsdShared::fgPresenter = 1
//     (zapne swapchain a hleda prezentacni kanal BmsdPresentShared).
//   Interop_EnsureHelper - spusti hlavni helper, pokud jeste nebezi (jinak startuje az pri prvnim konci sceny),
//     nezavisle na rezimu mostu. Vraci handle procesu helperu (vlastni ho most, nezavirat - pro vlastni drzeni
//     DuplicateHandle), nullptr = most selhal / je vypnuty natrvalo / jine zarizeni. Jen z vlakna hry.
//   Interop_TakeBridgeN - frameValue (N) snimku mostu, jehoz vysledek je ve scene tohoto snimku hry, od minuleho
//     volani (pak se nuluje); 0 = snimek hry nesel mostem. Volat pri kazdem Present hlavniho zarizeni.
//   Interop_FgPresenterStarting - presenter se zapina (pred Interop_EnsureHelper): kdyz most jeste nezpracoval zadny
//     snimek (helper by bezel uz od prvniho Present), vynecha prvnich [fg] BridgeStartSkip koncu sceny.
void Interop_SetFgPresenter(bool on);
void Interop_FgPresenterStarting();
HANDLE Interop_EnsureHelper(IDirect3DDevice9* dev);
uint64_t Interop_TakeBridgeN();
// Filtr zmenseni pri S > 1 (BMSD_DOWN_*, Alt+F1) a pocet fazi jitteru (Alt+F2; jen log helperu a reset historie
// DLSS pri zmene). Predava se helperu kazdy snimek.
void Interop_SetDownsampleFilter(uint32_t filter);
void Interop_SetJitterPhases(uint32_t phases, bool autoMode);
// Volat PRED skutecnym Reset/ResetEx hlavniho zarizeni: pocka (s limitem) na odeslanou praci mostu, uvolni drzene
// povrchy hry a zapomene stav snimku (historie DLSS se resetuje, most prvni snimky po Resetu vynecha).
void Interop_BeforeReset();
// Synchronizace s helperem: true = pipeline (CPU hry neceka, B ceka na GPU), false = synchronni (puvodni).
// [bridge] Pipeline, za behu klavesa. Nevyrizeny snimek se dokonci v obou rezimech.
void Interop_SetPipeline(bool on);
bool Interop_GetPipeline();
// Volat pri kazdem Present hlavniho zarizeni: cas snimku a kazdych ~5 s radek "vykon" do logu (FPS, cas snimku,
// casy mostu na CPU).
void Interop_OnPresent();
