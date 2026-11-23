// Spike S2 (FG varianta 2): helper prezentuje do okna hry ([present] HelperPresent=1, vychozi 0).
// Pri HelperPresent=0 jsou vsechny funkce bez ucinku (proxy se chova jako bez spiku) - volat je jen pres
// Present_Enabled().
#pragma once
#include <d3d9.h>

// Nacte [present] z ini (DllMain); pri HelperPresent=0 nic nezaloguje.
void Present_LoadConfig(const wchar_t* iniPath);
bool Present_Enabled();

// CreateDevice(Ex): z parametru hry udela parametry pro DXVK (Windowed, skryte okno jako hDeviceWindow i focus,
// D3DCREATE_NOWINDOWCHANGES, bez VSync). Zarizeni, ktere by kreslilo do okna hry (hlavni a pripadne ANGLE), dostane
// vlastni skryte okno. *token predat do Present_AfterCreate.
void Present_PrepareCreate(HWND* focus, DWORD* flags, D3DPRESENT_PARAMETERS* pp, void** token);
void Present_AfterCreate(void* token, HRESULT hr, IDirect3DDevice9* dev);
// Reset(Ex): totez pro Reset (skryte okno dostane novou velikost). Pred/po Resetu hlavniho zarizeni dobehnout kopie.
bool Present_PrepareReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp);  // true = pp upraveno
void Present_BeforeReset(IDirect3DDevice9* dev);
void Present_AfterReset(IDirect3DDevice9* dev, HRESULT hr);
// Nejvnejsi Present / PresentEx / SwapChain::Present kazdeho zarizeni: u hlavniho predani snimku helperu (nebo
// zalozni cesta). Vraci hDestWindowOverride, se kterym ma proxy zavolat DXVK. caller = navratova adresa (log, kudy
// vola Steam overlay), ourHook = nas hook Present (kontrola vtable[17]).
HWND Present_OnPresent(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc, HWND override, bool isMain, void* caller,
                       void* ourHook);
// Po nejvnejsim Present (mereni doby o_Present na CPU hry - tam spi omezovac FPS ovladace; pri generovani snimku
// tu spi limiter skutecnych snimku na TargetFps / 2)
void Present_AfterPresent(IDirect3DDevice9* dev, bool isMain);

// ---- Frame Generation (od BMSD_VERSION 12; [fg] Mode != 0 pri startu hry = prezentuje hlavni helper) ----
// Rezimy jsou BMSD_FG_* z bmsd_shared.h (0 vyp, 1 DLSS-G, 2 FSR 2x, 3 FSR adaptivni), stavy BMSD_FGS_*.
// Pozadovany rezim (ini pri startu, pak menu). SetMode: plati za behu (vyp <-> FG zapne / zastavi presenter pri
// nejblizsim Present, [fg] LiveSwitch); jen kdyz zarizeni hry neni presmerovane, plati po restartu hry.
int Present_FgGetMode();
void Present_FgSetMode(int mode);
// Prepinani FG je zive: cesta presenteru v hlavnim helperu existuje (zarizeni presmerovane) a neni v zaloze.
bool Present_FgPresenterActive();
bool Present_FgRetryable();  // zaloha z casoveho limitu startu: vyp / zap FG = novy pokus
// Frame Generation presla za behu do zalohy (jednou true, pak false): proxy ukaze OSD
int Present_TakeFallbackNote();  // 0 = nic, 1 = FG selhala, 2 = jen casovy limit startu (jde znovu)
// Bezpecny start: pred Present_LoadConfig - duvod (minule spusteni skoncilo chybou), "" = ne. SafeStartHit = FG
// pro tento start vypnuta
void Present_SetSafeStart(const char* why);
bool Present_SafeStartHit();
// Rezim jde nabidnout: DLSS-G, FSR 2x a FSR adaptivni ano, dokud helper nerekne opak (fgAvailValid / fgAvailMask).
bool Present_FgModeAvailable(int mode);
// Otevrene menu modu: FG pozastavit (jen s [fg] PauseInMenu=1). Volat kazdy snimek.
void Present_FgSetPause(bool menuOpen);
// [fg] PauseInMenu (menu: Pozastavit v menu), vychozi vypnuto
bool Present_FgGetPauseInMenu();
void Present_FgSetPauseInMenu(bool on);
// Strop / cil vystupu [fg] TargetFps (0 = auto z obnovovaci frekvence monitoru f - f^2/3600, jako Reflex: 165 -> 157,4);
// hra pri generovani N x bezi na 1/N (minus rezerva LimiterMargin).
int Present_FgGetTargetFps();
void Present_FgSetTargetFps(int fps);
// Nasobek FG 2 / 3 / 4 ([fg] Multiplier): pozadovany (helper ho omezi podle generatoru a GPU). MultMax(mode) =
// nejvyssi nasobek rezimu (FSR 2x: 2; DLSS-G: podle helperu - NGX MultiFrameCountMax + 1, dokud nevi 4).
// MultKnown = helper uz nasobek DLSS-G na tomto GPU nahlasil.
int Present_FgGetMult();
void Present_FgSetMult(int mult);
int Present_FgMultMax(int mode);
bool Present_FgMultAuto();          // [fg] Multiplier=auto (Present_FgGetMult pak vraci 0)
void Present_FgSetMultEff(int mult);  // zvoleny nasobek bez zmeny rezimu auto
bool Present_FgMultKnown();
// Adaptivni FSR ([fg] Mode=fsradaptive): strop skutecnych FPS ([fg] RealCap, 0 = vyp; limiter v proxy), hranice
// "doplnovat pod X FPS" pro pacing A ([fg] FillBelow, 0 = auto 90 % cile) a pacing ([fg] Pacing 0 = A pevny rytmus,
// 1 = B skutecne + generovane do mezer). Meni se za behu (helper bere od dalsiho snimku).
int Present_FgGetRealCap();
void Present_FgSetRealCap(int fps);
int Present_FgGetFillBelow();
void Present_FgSetFillBelow(int fps);
// Ukazatel FPS ([menu] FpsCounter): Tick kazdy Present hlavniho zarizeni (pred kreslenim prekryvu), Get = posledni
// hodnoty (okno ~0.5 s): real = skutecne zobrazene snimky / s, total = vsechny zobrazene (s FG vcetne generovanych),
// split = v okne byly generovane snimky. false = zatim nic.
void Present_FpsTick();
bool Present_FpsGet(float* real, float* total, bool* split);
// Limiter FG v poslednich 64 snimcich spal aspon v 8: strop drzi mod, regulator latence (latency.inl) ma jen merit.
bool Present_FgLimiting();
// Stav pro menu (stavovy radek) a log. Retezce jsou ASCII (cesky bez diakritiky, jako log); plati do dalsiho volani.
struct PresentFgInfo {
    int requested;               // pozadovany rezim (BMSD_FG_*)
    bool presenter;              // cesta presenteru v hlavnim helperu existuje (FG jde zapnout za behu)
    bool fallback;               // presenter spadl do zalohy (DXVK prezentuje primo; FG az po restartu hry)
    const char* fallbackReason;  // duvod zalohy
    bool helperReady;            // helper se k prezentacnimu kanalu pripojil
    bool menuPause;              // FG je pozastavene kvuli otevrenemu menu modu ([fg] PauseInMenu)
    int state;                   // BMSD_FGS_* od helperu (0, kdyz presenter nebezi)
    int activeMode;              // rezim, ktery helper prave generuje (BMSD_FG_*), 0 = zadny
    int activeMult;              // nasobek, se kterym helper generuje (2..4), 0 = nic
    const char* msg;             // duvod stavu pozastaveno / nedostupne / selhalo ("" = nic)
    unsigned targetFps;          // strop vystupu (0 = zatim neznamy)
    float realFps, outFps;       // skutecne / zobrazene snimky za sekundu (statistika helperu, 0 = zatim nic)
    float evalMs;                // GPU cas generatoru na snimek
    bool genShare;               // v poslednim intervalu statistiky helper opravdu generoval (>= 80 % snimku)
    const char* name;            // nazev a verze generatoru od helperu ("DLSS-G 310.9.16", "FSR FG 3.1.6"; "" = zadny)
    // Posledni interval statistiky, ve kterem pozadovany rezim generoval (menu je pri otevreni pozastavi): 0 = zatim nic
    float lastRealFps, lastOutFps, lastEvalMs;
    int refreshHz;               // obnovovaci frekvence monitoru okna hry (0 = nezjistena)
    float adaptTarget;           // adaptivni FSR: cil vystupu (FPS, po rezerve) a hranice doplnovani A (auto 90 %)
    float adaptFill;
    bool adaptEngaged;           // adaptivni FSR prave generuje (jinak jen skutecne snimky: nad cilem / mrtva zona)
};
void Present_FgInfo(PresentFgInfo* out);

// ---- HUD-less vstup FG (K5, BMSD_PRESENT_VERSION 3, [fg] HudLess=1) ----
// Vlastni RT velikosti a formatu backbufferu, do ktereho proxy v bode "konec post-processingu, pred HUD" zkopiruje
// barvu sceny (StretchRect). nullptr = HUD-less ted netreba / nejde (FG negeneruje, menu, zaloha, [fg] HudLess=0).
IDirect3DSurface9* Present_HudlessTarget(IDirect3DDevice9* d);
// Z OnFrameEnd proxy pred kreslenim menu: valid = RT obsahuje HUD-less prave konciciho snimku (snaps = pocet kopii
// do RT v tomto snimku, statistika). Predava se se snimkem v nejblizsim PostFrame.
void Present_HudlessFrame(bool valid, unsigned snaps);
void Present_NoteScene();  // konec sceny v tomto snimku ([fg] PresentStartSkip)
