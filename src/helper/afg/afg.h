// Adaptivni FSR Frame Generation (AFG): vlastni varianta AMD FSR 3.1.4 frame interpolation (fork MIT zdrojaku
// FidelityFX SDK 1.1.4 v src\helper\afg\ffx) s libovolnym casem mezisnimku t v (0, 1) mezi minulym (t = 0) a aktualnim
// (t = 1) skutecnym snimkem. Staticky prelozeno (build_afg.ps1 -> build\x64\afg.lib), zadne DLL za behu.
//
// Rozhrani je ciste C++ bez typu FFX (helper.cpp includuje hlavicky ffx API 2.4 - zadna kolize jmen / maker);
// implementace je ve vlastni prekladove jednotce afg.cpp. Jedna instance na proces (globalni stav), jedno vlakno.
//
// Pouziti na jeden skutecny snimek:  AfgRecordPair (dilatace MV / hloubky + optical flow, ulozi si snimek)
//                                    -> 0..N x AfgRecordInterp(t) (mezisnimky mezi minulym a timto snimkem).
// Vse se zapisuje do command bufferu volajiciho, zadny vlastni submit ani cekani. Bariery pro vlastni obrazy si AFG
// dela samo, pro obrazy volajiciho viz popis u AfgPairInfo a AfgRecordInterp.
//
// Zivotnost (dano backendem FFX, viz ffx_vk.cpp ffxVkAfgAdvanceFrame):
//  - Prace GPU zaznamenana pro par P (AfgRecordPair P + jeho AfgRecordInterp) musi byt dokoncena drive, nez se zavola
//    AfgRecordPair P + 3 (pohledy FFX na obrazy zaregistrovane v paru P se ruseji pri AfgRecordPair P + 4; sady
//    deskriptoru se recykluji po 40 pouzitich pruchodu). Tj. nejvys 3 pary v letu a nejvys AFG_MAX_INTERP_PER_PAIR
//    mezisnimku na par.
//  - Obrazy volajiciho (MV, hloubka, vystup mezisnimku) musi zit, dokud muze GPU provadet prikazy, ktere na ne
//    odkazuji. Pred zrusenim / znovuvytvorenim techto obrazu: GPU v klidu + AfgReleaseViews (nebo AfgDestroy).
#pragma once

#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
#include <string>

#define AFG_MAX_INTERP_PER_PAIR 8

// Zarizeni helperu. Pozadavky: Vulkan 1.1+ (vkGetPhysicalDeviceProperties2 / Features2 v jadru), vlastnosti
// shaderStorageImageWriteWithoutFormat (vystupy FFX), separateDepthStencilLayouts (bariery hloubky D24S8 jen
// s aspektem DEPTH - jako u podepsaneho FSR FG); rozsireni navic zadna (helper zapina vse z 1.1 - 1.3).
struct AfgDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    uint32_t queueFamily = 0;  // fronta, do jejichz command bufferu se zaznamenava (vlastni obrazy EXCLUSIVE)
};

struct AfgCreateInfo {
    uint32_t displayW = 0, displayH = 0;      // backbuffer (vystup mezisnimku)
    uint32_t maxRenderW = 0, maxRenderH = 0;  // obrazy MV a hloubky (render je sub-rect od (0, 0), viz AfgPairInfo)
    VkFormat colorFormat = VK_FORMAT_B8G8R8A8_UNORM;  // backbuffer / HUD-less / vystup: B8G8R8A8_UNORM nebo
                                                      // R8G8B8A8_UNORM s hodnotami v sRGB (LDR)
    bool depthInverted = false;  // false = 0 near .. 1 far (D24S8 helperu)
    bool depthInfinite = false;  // false = konecna far
    bool hudless = false;        // bude se predavat obraz bez HUD (alokuje navic jeden obraz backbufferu)
};

// Jeden skutecny snimek. Layouty obrazu volajiciho PRED volanim AfgRecordPair a AFG je v nich i vrati:
//  - color, hudless: colorLayout (libovolny layout, ze ktereho jde kopirovat: SHADER_READ_ONLY_OPTIMAL, GENERAL,
//    TRANSFER_SRC_OPTIMAL ...; usage TRANSFER_SRC). AFG si oba zkopiruje (vkCmdCopyImage) do svych obrazu, bariera
//    pred kopii ceka na vsechny predchozi zapisy (ALL_COMMANDS / MEMORY_WRITE), po kopii zadny pristup AFG.
//  - motionVectors, depth: SHADER_READ_ONLY_OPTIMAL, usage SAMPLED, zapisy musi byt viditelne pro compute shader
//    (bariera volajiciho, jako pro FSR FG). Ctou se jen v ramci AfgRecordPair (pruchod dilatace).
struct AfgPairInfo {
    VkImage color = VK_NULL_HANDLE;    // aktualni backbuffer (s HUD), displayW x displayH, colorFormat
    VkImage hudless = VK_NULL_HANDLE;  // volitelne: stejny snimek bez HUD (jen s AfgCreateInfo::hudless); NULL = bez
    VkImageLayout colorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // layout color i hudless
    VkImage motionVectors = VK_NULL_HANDLE;  // MV v pixelech renderu: minula - aktualni pozice (+x vpravo, +y dolu)
    VkFormat mvFormat = VK_FORMAT_R32G32_SFLOAT;
    VkImage depth = VK_NULL_HANDLE;          // hloubka renderu (konvence podle AfgCreateInfo::depthInverted)
    VkFormat depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    uint32_t inputW = 0, inputH = 0;         // velikost obrazu MV a hloubky (<= maxRenderW x maxRenderH)
    uint32_t renderW = 0, renderH = 0;       // platny sub-rect od (0, 0) (cast B: 1707 x 960 v 2560 x 1440)
    float mvScaleX = 1.0f, mvScaleY = 1.0f;  // nasobek MV (jako InMVScale / motionVectorScale)
    float jitterX = 0.0f, jitterY = 0.0f;    // jitter renderu v pixelech (helper: 0)
    float cameraNear = 7.0f, cameraFar = 28000.0f;  // jednotky sceny
    float cameraFovY = 1.02f;                // svisle FOV v radianech
    float metersPerUnit = 0.0254f;           // viewSpaceToMetersFactor (Source: 1 jednotka = 1 palec)
    float frameTimeDeltaMs = 0.0f;           // interval skutecnych snimku
    bool reset = false;                      // strih / skok kamery: mezisnimky tohoto paru = kopie aktualniho
    uint64_t frameId = 0;                    // musi rust presne o 1, jinak AFG resetuje samo (jako FSR FG)
};

// Vytvoreni (trva desitky az stovky ms - pipeline). Pri chybe false a *err (muze byt nullptr). Druhe volani bez
// AfgDestroy = chyba.
bool AfgCreate(const AfgDevice& dev, const AfgCreateInfo& ci, std::string* err);
// Zruseni. GPU musi byt v klidu (zadne rozpracovane prikazy AFG).
void AfgDestroy();
bool AfgReady();
// VRAM kontextu (vnitrni obrazy FFX + sdilene obrazy + vlastni obrazy AFG + buffer konstant), 0 = neni kontext.
size_t AfgVramBytes();
// Nazev pro log / menu, napr. "FSR 3.1.4 AFG".
const char* AfgVersionName();
// Popis posledni chyby (AfgRecord* vraci false).
const char* AfgLastError();

// Novy skutecny snimek (viz AfgPairInfo). Prvni volani po AfgCreate je vzdy reset (mezisnimky = kopie aktualniho).
// Pri false (AfgLastError) mohl byt command buffer uz zcasti zapsany; AfgRecordInterp pak vraci false az do
// dalsiho uspesneho paru, ten je vhodne poslat s reset = true.
bool AfgRecordPair(VkCommandBuffer cmd, const AfgPairInfo& p);

// Mezisnimek v case t (0 = minuly skutecny snimek, 1 = aktualni z posledniho AfgRecordPair; hodnoty mimo [0, 1] se
// orezou) do obrazu out: displayW x displayH, format colorFormat, usage STORAGE, layout GENERAL pred i po volani.
// Bariera na zacatku ceka na vsechny predchozi pristupy k out (ALL_COMMANDS), bariera na konci zpristupni zapis
// pro jakykoli dalsi pristup (ALL_COMMANDS / MEMORY_READ | MEMORY_WRITE). outView se nepouziva (FFX si dela vlastni
// pohled), smi byt VK_NULL_HANDLE. Volat 0..AFG_MAX_INTERP_PER_PAIR x po kazdem AfgRecordPair; minuly ani aktualni
// snimek se mezi volanimi neposouva. Po resetu (AfgPairInfo::reset, mezera ve frameId, prvni snimek, zmena sceny
// podle optical flow) je vystup kopie aktualniho snimku.
bool AfgRecordInterp(VkCommandBuffer cmd, float t, VkImage out, VkImageView outView);

// Zrusi vsechny pohledy FFX na obrazy volajiciho (MV, hloubka, vystupy). GPU musi byt v klidu. Potom smi volajici
// sve obrazy zrusit; dalsi AfgRecordPair funguje normalne.
void AfgReleaseViews();
