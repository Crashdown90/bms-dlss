// bms-dlss: menu nastaveni primo ve hre (docs\navrh-menu.md, docs\menu-kroky-1-3.md, docs\menu-kroky-4-6.md): vstup
// (hacek klavesnice), kresleni (atlas pisma z GDI, prekryv pred Present), model menu (stranky Bezne / Pokrocile,
// odklad HEAVY zmen), OSD u Alt klaves a ukladani do ini na pracovnim vlakne (MenuSaveThread: dostava jen kopie
// hodnot pres frontu pod g_saveCs; zbytek pri ukonceni hry zapise MenuSaveAtExit). Vklada se do proxy.cpp (#include) pred OnFrameEnd
// a pouziva jeho akce Set* (spolecne s Alt klavesami), stav (g_lat, g_omEnabled, Interop_*) a originalni funkce
// zarizeni (o_*). Texty CZ/EN jsou v menu_text.inl (UTF-8); tento soubor je ASCII.
//
// VLAKNA (zavazne): okno hry ma vlastni vlakno zprav, Present bezi na vlakne renderu (zarizeni je MULTITHREADED,
// mat_queue_mode) a menu hry (Qt pres ANGLE) ma dalsi vlakno. Proto:
//   * hacky (MenuKbdProc, MenuGetMsgProc) bezi na vlakne zprav a NIC nekresli, nevolaji D3D ani nemeni stav modu.
//     Jen rozhodnou, jestli stisk spolknout, a zapisou ho do fronty g_mk* (atomiky; cteni g_menuOpen).
//   * vsechno ostatni (stav menu, akce Set*, atlas, kresleni, instalace hacku) dela jen vlakno Present hlavniho
//     zarizeni v OnFrameEnd: MenuFrameInput (fronta -> akce) a MenuFrameDraw (prekryv do backbufferu).
//
// VSTUP: SetWindowsHookEx(WH_KEYBOARD) pro vlakno okna hry (a pro vlakno okna v popredi, kdyz je jine). Navratova
// hodnota 1 zpravu zahodi driv, nez ji dostane TranslateMessage a WndProc hry -> zadne WM_KEYDOWN ani WM_CHAR.
//   * zavrene menu: spolkne se klavesa menu (Alt+N) a klavesy modu s modifikatorem (Alt+F8; s [keys] DebugKeys=1
//     i ladici, se zapnutymi zkratkami pro testovani Alt+F9 / Alt+F10; akci dal dela OnFrameEnd pres
//     GetAsyncKeyState). Ostatni Alt klavesy (Alt+F3, Alt+Home...) hra dostane,
//   * otevrene menu: spolkne se kazdy stisk krome F4 (dlss_vp, Alt+F4), klaves Win, PrintScreen a multimedii,
//   * uvolneni klavesy (WM_KEYUP) se propousti vzdy, jinak by klavesa drzena pri otevreni zustala ve hre zaseknuta,
//   * klavesa spolknuta pri stisku se polyka az do sveho uvolneni (drzeny Esc pri zavirani neotevre menu hry).
// Kdyz ma hra registrovany raw input klavesnice (MenuRawCheck, radek v logu), pridava se WH_GETMESSAGE, ktery
// WM_INPUT klavesnice se stejnym rozhodnutim prepise na WM_NULL. Bez hacku (instalace selhala, [menu] NoHook=1)
// menu ovlada GetAsyncKeyState: Alt+N, I/K, J/L, O - ty ale dostane i hra.
//
// KRESLENI: OverlayBuild udela seznam ctyruhelniku (pozice v pixelech cile, UV do atlasu, premultiplikovana barva)
// a na cili nezavisi; cil urcuje OverlaySink. Dnes jen SINK_BACKBUFFER: jeden draw do backbufferu na konci
// OnFrameEnd, tedy az po vystupu DLSS (most bere barvu sceny na konci sceny, menu se do ni nikdy nekresli) a pred
// Present i pred kopii backbufferu pro helper ([present] HelperPresent=1). S Frame Generation pribude SINK_LAYER
// (vlastni vrstva, kterou helper slozi az po interpolaci) - proto premultiplikovana alfa uz ted.
// Stav hry uklada state block D3DSBT_ALL a rucne RT0..3, DS, viewport a scissor (stejny vzor jako davka MV objektu);
// behem kresleni hooky proxy volani z tohoto vlakna jen propousteji (g_ovGuardThread, OmGuarded v objmv.inl).
//
// Ini: [keys] MenuKey=0x4E (N; s modifikatorem z [keys] Modifier), DebugKeys=0 (1 = ladici Alt klavesy),
// TestKeys=0 (zkratky pro testovani Alt+F9 / Alt+F10, i z menu); [menu] Enable=1 (0 = bez menu, hacku i OSD),
// SwallowKeys=1 (polykat i klavesy modu), Scale=1.0 (velikost textu 0.75-1.5), Language=auto|cs|en, Osd=1, Beep=1,
// NoHook=0, ladeni: TestOpenSec, TestKeys, TestShot (viz MenuTestStep).

#include "menu_text.inl"

// ---------------------------------------------------------------------------
// Konfigurace
// ---------------------------------------------------------------------------
static struct MenuCfg {
    int key = 'N';            // [keys] MenuKey
    bool enable = true;       // [menu] Enable
    bool swallowKeys = true;  // [menu] SwallowKeys: hacek polyka i klavesy modu (Alt+Ins uz neda hre +klook)
    bool noHook = false;      // [menu] NoHook: ladeni zalohy bez hacku
    float scale = 1.0f;       // [menu] Scale (0.5 - 1.5)
    int pos = 0;              // [menu] Position: 0 stred, 1 vlevo, 2 vpravo, 3 vlevo nahore, 4 vpravo nahore
    int lang = 0;             // 0 = cestina, 1 = anglictina (vysledek langSetting)
    int langSetting = 0;      // [menu] Language: 0 = auto, 1 = cs, 2 = en
    bool osd = true;          // [menu] Osd: kratky radek pri zmene Alt klavesou
    bool startHint = true;    // [menu] StartHint: po startu hry na 6 s radek "Nastaveni modu: Alt+N"
    bool fpsCounter = false;  // [menu] FpsCounter: ukazatel FPS vpravo nahore (skutecne / zobrazene)
    bool overlayVersions = false;  // [menu] OverlayVersions: pod nim radek s bezicim upscalerem a FG (verze, nasobek)
    bool diag = false;        // [menu] DiagOverlay (v103g): diagnostika plynulosti vpravo nahore (hra, monitor, preskoceno, FG, graf)
    int testOpenSec = 0;      // [menu] TestOpenSec: tolik sekund po prvnim Present otevrit menu bez klaves
    bool testShot = false;    // [menu] TestShot: po otevreni ulozit backbuffer do bms_dlss_menu_shot_N.bmp
    std::wstring testKeys;    // [menu] TestKeys: up,down,left,right,enter,esc,tab,del,menu,shot,w<ms>
} g_menuCfg;

static uint32_t g_modKeyBits[8];  // klavesy modu z [keys] (bit = VK); plni MenuLoadConfig, pak se jen cte

// Je jazyk hry cestina? (uzivatel 2026-11-19: menu podle jazyka hry, cestina -> cs, cokoli jineho -> en)
// 1) prikazova radka "-language czech" (ma v Source prednost pred Steamem), 2) cc_lang "czech" v bms\cfg\config.cfg
// (jazyk titulku/textu hry; config z minuleho spusteni, jen cteni), 3) jazyk hry ve Steamu (appmanifest_362890.acf,
// "language" v UserConfig, knihovna = ..\..\ od slozky hry). Jazyk Windows se uz nebere.
static bool MenuGameIsCzech() {
    auto lower = [](std::wstring s) {
        for (auto& ch : s) ch = (wchar_t)towlower(ch);
        return s;
    };
    const std::wstring cmd = lower(GetCommandLineW());
    const size_t p = cmd.find(L"-language");
    if (p != std::wstring::npos) {
        size_t q = p + 9;
        while (q < cmd.size() && (cmd[q] == L' ' || cmd[q] == L'"')) ++q;
        return cmd.compare(q, 5, L"czech") == 0;
    }
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring dir = exe;
    dir.erase(dir.find_last_of(L"\\/") + 1);  // ...\Black Mesa\ (vcetne lomitka)
    auto fileHas = [&](const std::wstring& path, const wchar_t* key) -> int {  // -1 soubor/klic nenalezen, 0/1
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return -1;
        char line[512];
        int r = -1;
        const size_t kl = wcslen(key);
        while (r < 0 && fgets(line, sizeof line, f)) {
            std::wstring w;
            for (const char* s = line; *s; ++s) w += (wchar_t)(unsigned char)*s;
            w = lower(w);
            size_t k = w.find_first_not_of(L" \t\"");
            if (k == std::wstring::npos || w.compare(k, kl, key) != 0) continue;
            k += kl;
            if (k < w.size() && w[k] != L' ' && w[k] != L'\t' && w[k] != L'"') continue;  // cc_lang vs cc_langX
            const size_t v = w.find_first_not_of(L" \t\"\r\n", k);
            if (v == std::wstring::npos) continue;  // cc_lang "" = jazyk Steamu -> dalsi zdroj
            r = w.compare(v, 5, L"czech") == 0 ? 1 : 0;
        }
        fclose(f);
        return r;
    };
    const int cc = fileHas(dir + L"bms\\cfg\\config.cfg", L"cc_lang");
    if (cc >= 0) return cc == 1;
    const int st = fileHas(dir + L"..\\..\\appmanifest_362890.acf", L"language");
    return st == 1;
}

// Jazyk menu: 0 = auto (podle jazyka hry, MenuGameIsCzech), 1 = cs, 2 = en
static void MenuSetLangSetting(int v) {
    MenuCfg& c = g_menuCfg;
    c.langSetting = v < 0 || v > 2 ? 0 : v;
    if (c.langSetting) {
        c.lang = c.langSetting - 1;
    } else {
        c.lang = MenuGameIsCzech() ? 0 : 1;
    }
}

static void MenuSaveInit();

static void MenuLoadConfig() {
    MenuCfg& c = g_menuCfg;
    c.key = (int)wcstol(IniStr(L"keys", L"MenuKey", L"0x4E").c_str(), nullptr, 0);
    c.enable = wcstol(IniStr(L"menu", L"Enable", L"1").c_str(), nullptr, 0) != 0 && c.key > 0 && c.key < 255;
    c.swallowKeys = wcstol(IniStr(L"menu", L"SwallowKeys", L"1").c_str(), nullptr, 0) != 0;
    c.noHook = wcstol(IniStr(L"menu", L"NoHook", L"0").c_str(), nullptr, 0) != 0;
    float sc = IniFloat(L"menu", L"Scale", 1.0f);
    if (sc > 10.0f) sc /= 100.0f;  // i v procentech (50-150)
    c.scale = std::min(1.5f, std::max(0.5f, sc));
    {
        std::wstring ps = IniStr(L"menu", L"Position", L"center");
        for (auto& ch : ps) ch = (wchar_t)towlower(ch);
        c.pos = ps == L"left" ? 1 : ps == L"right" ? 2 : ps == L"topleft" ? 3 : ps == L"topright" ? 4
                : (ps.size() == 1 && ps[0] >= L'0' && ps[0] <= L'4') ? ps[0] - L'0' : 0;
    }
    std::wstring lang = IniStr(L"menu", L"Language", L"auto");
    for (auto& ch : lang) ch = (wchar_t)towlower(ch);
    MenuSetLangSetting(lang == L"cs" || lang == L"cz" ? 1 : lang == L"en" ? 2 : 0);
    c.osd = wcstol(IniStr(L"menu", L"Osd", L"1").c_str(), nullptr, 0) != 0;
    c.startHint = wcstol(IniStr(L"menu", L"StartHint", L"1").c_str(), nullptr, 0) != 0;
    c.fpsCounter = wcstol(IniStr(L"menu", L"FpsCounter", L"0").c_str(), nullptr, 0) != 0;
    c.overlayVersions = wcstol(IniStr(L"menu", L"OverlayVersions", L"0").c_str(), nullptr, 0) != 0;
    c.diag = wcstol(IniStr(L"menu", L"DiagOverlay", L"0").c_str(), nullptr, 0) != 0;
    g_keyBeep = wcstol(IniStr(L"menu", L"Beep", L"1").c_str(), nullptr, 0) != 0;
    c.testOpenSec = std::max(0, (int)wcstol(IniStr(L"menu", L"TestOpenSec", L"0").c_str(), nullptr, 0));
    c.testShot = wcstol(IniStr(L"menu", L"TestShot", L"0").c_str(), nullptr, 0) != 0;
    c.testKeys = IniStr(L"menu", L"TestKeys", L"");
    memset(g_modKeyBits, 0, sizeof g_modKeyBits);
    const int keys[] = {g_cfg.captureKey,  g_cfg.jitterKey,     g_cfg.bridgeKey,   g_cfg.presetKey,   g_cfg.sharpenKey,
                        g_cfg.mipBiasKey,  g_cfg.inputModeKey,  g_cfg.depthModeKey, g_cfg.outScaleKey, g_cfg.phasesKey,
                        g_cfg.downsampleKey, g_cfg.pipelineKey, g_cfg.objMvKey,    g_lat.key};
    for (int k : keys)
        if (k > 0 && k < 255) g_modKeyBits[k >> 5] |= 1u << (k & 31);
    g_testKeysOn.store(g_cfg.testKeys, std::memory_order_relaxed);  // za behu meni menu (MenuSetTestKeys)
    MenuSaveInit();
    Log("config [menu]: Enable=%d menuKey=0x%X SwallowKeys=%d NoHook=%d Scale=%.2f jazyk=%s%s Osd=%d Beep=%d%s",
        (int)c.enable, c.key, (int)c.swallowKeys, (int)c.noHook, c.scale, c.lang ? "en" : "cs",
        c.langSetting ? "" : " (auto)", (int)c.osd, (int)g_keyBeep,
        c.testOpenSec ? " (test: TestOpenSec/TestKeys/TestShot)" : "");
}

static inline const wchar_t* Tr(StrId id) { return kMenuStr[id][g_menuCfg.lang]; }

// ---------------------------------------------------------------------------
// Vstup: fronta stisku (hacek -> vlakno Present) a hacky
// ---------------------------------------------------------------------------
// Udalost = VK (dolnich 8 bitu) + priznaky. Fronta ma jednoho ctenare (vlakno Present) a zapisuje do ni vlakno zprav
// okna hry. Zapisovatelu muze byt vyjimecne vic (hacek i na vlakne okna v popredi, test z vlakna Present), proto se
// zapis serializuje kratkym zamkem z atomic_flag; ctenar zamek nepotrebuje.
enum : uint16_t { MK_REPEAT = 0x0100, MK_TOGGLE = 0x8000 };
static const uint32_t kMkQueue = 64;
static uint16_t g_mkBuf[kMkQueue];
static std::atomic<uint32_t> g_mkHead{0}, g_mkTail{0};
static std::atomic<uint32_t> g_mkDropped{0}, g_mkSwallowed{0}, g_mkRawSwallowed{0};
static std::atomic_flag g_mkWriteLock = ATOMIC_FLAG_INIT;
static std::atomic<bool> g_menuOpen{false};    // zapisuje jen vlakno Present, hacek cte
// GetTickCount posledniho snimku s otevrenym menu. Kdyz Present stoji dele nez 2 s (nahravani, zaseknuti), menu
// nikdo nekresli ani neobsluhuje -> hacek do te doby klavesy hre zase propousti (klavesa menu se bere dal).
static std::atomic<uint32_t> g_menuFrameTick{0};
static inline bool MkMenuActive() {
    return g_menuOpen.load(std::memory_order_acquire) &&
           GetTickCount() - g_menuFrameTick.load(std::memory_order_relaxed) < 2000u;
}
static std::atomic<bool> g_menuHookOn{false};  // hacky maji rozhodovat (jinak vse propousteji)
static std::atomic<bool> g_rawNoLegacy{false}; // raw input klavesnice s RIDEV_NOLEGACY: udalosti dava hacek zprav
static std::atomic<uint32_t> g_mkHeldKbd[8];   // klavesy spolknute pri stisku (do uvolneni), WH_KEYBOARD
static std::atomic<uint32_t> g_mkHeldRaw[8];   // totez pro WM_INPUT
// Modifikatory podle WM_INPUT (jen hacek zprav): 1 = Alt, 2 = levy Ctrl, 4 = pravy Ctrl, 8 = Shift. U WM_KEYDOWN je
// stav v priznacich zpravy a v GetKeyState; raw input ho nema a GetAsyncKeyState uz muze byt o krok dal.
enum : uint32_t { MKM_ALT = 1, MKM_LCTRL = 2, MKM_RCTRL = 4, MKM_SHIFT = 8 };
static std::atomic<uint32_t> g_mkRawMods{0};

static void MkPush(uint16_t ev) {
    while (g_mkWriteLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
    const uint32_t t = g_mkTail.load(std::memory_order_relaxed);
    if (t - g_mkHead.load(std::memory_order_acquire) < kMkQueue) {
        g_mkBuf[t % kMkQueue] = ev;
        g_mkTail.store(t + 1, std::memory_order_release);
    } else {
        g_mkDropped.fetch_add(1, std::memory_order_relaxed);
    }
    g_mkWriteLock.clear(std::memory_order_release);
}

static bool MkPop(uint16_t* ev) {
    const uint32_t h = g_mkHead.load(std::memory_order_relaxed);
    if (h == g_mkTail.load(std::memory_order_acquire)) return false;
    *ev = g_mkBuf[h % kMkQueue];
    g_mkHead.store(h + 1, std::memory_order_release);
    return true;
}

// Modifikator klaves modu drzen (presne jeden, stejne jako v OnFrameEnd). altCtx = Alt podle priznaku zpravy,
// mods = MKM_* sledovane z WM_INPUT (raw input), async = GetAsyncKeyState misto GetKeyState.
static bool MkModDown(bool altCtx, bool async, uint32_t mods) {
    auto dn = [async](int vk) { return ((async ? GetAsyncKeyState(vk) : GetKeyState(vk)) & 0x8000) != 0; };
    if (g_cfg.modifier == 0) return true;
    const bool c = (mods & (MKM_LCTRL | MKM_RCTRL)) || dn(VK_CONTROL), a = altCtx || (mods & MKM_ALT) || dn(VK_MENU);
    const bool sh = (mods & MKM_SHIFT) || dn(VK_SHIFT);
    switch (g_cfg.modifier) {
    case VK_CONTROL: return c && !a && !sh;
    case VK_RCONTROL: return ((mods & MKM_RCTRL) || dn(VK_RCONTROL)) && !a && !sh;
    case VK_SHIFT: return sh && !c && !a;
    default: return a && !c && !sh;
    }
}
// Modifikator klavesy menu. S [keys] Modifier=none by holou klavesu menu (N) hra nikdy nedostala (psani do konzole,
// chatu), proto se menu pak otevira jen s Altem (oprava S3 revize menu/FSR).
static bool MkMenuModDown(bool altCtx, bool async, uint32_t mods) {
    if (g_cfg.modifier != 0) return MkModDown(altCtx, async, mods);
    auto dn = [async](int vk) { return ((async ? GetAsyncKeyState(vk) : GetKeyState(vk)) & 0x8000) != 0; };
    const bool c = (mods & (MKM_LCTRL | MKM_RCTRL)) || dn(VK_CONTROL), a = altCtx || (mods & MKM_ALT) || dn(VK_MENU);
    const bool sh = (mods & MKM_SHIFT) || dn(VK_SHIFT);
    return a && !c && !sh;
}

// Za otevreneho menu se hre propousti: F4 (dlss_vp; Alt+F4 zavre hru), klavesy Win, PrintScreen, Sleep a multimedia
// (hlasitost dela DefWindowProc z WM_KEYDOWN). Alt+Tab a Win resi system mimo zpravy.
static inline bool MkPassWhenOpen(UINT vk) {
    return vk == VK_F4 || vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS || vk == VK_SNAPSHOT || vk == VK_SLEEP ||
           (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2);
}
static inline bool MkIsModifier(UINT vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) ||
           vk == VK_CAPITAL || vk == VK_NUMLOCK;
}

// Rozhodnuti o jedne klavesove udalosti (vlakno zprav). true = spolknout. haveRepeat: repeat je priznak ze zpravy
// (WM_KEYDOWN bit 30); u raw inputu se opakovani pozna podle bitmapy spolknutych klaves. push = zapsat do fronty.
static bool MkDecide(UINT vk, bool down, bool repeatFlag, bool haveRepeat, bool altCtx, bool async, uint32_t mods,
                     std::atomic<uint32_t>* held, bool push) {
    if (vk == 0 || vk >= 255) return false;
    std::atomic<uint32_t>& word = held[vk >> 5];
    const uint32_t bit = 1u << (vk & 31);
    const bool wasHeld = (word.load(std::memory_order_relaxed) & bit) != 0;
    if (!down) {  // uvolneni vzdy propustit
        if (wasHeld) word.fetch_and(~bit, std::memory_order_relaxed);
        return false;
    }
    const bool repeat = haveRepeat ? repeatFlag : wasHeld;
    bool swallow = false;
    if ((int)vk == g_menuCfg.key && MkMenuModDown(altCtx, async, mods)) {
        if (!repeat && push) MkPush(MK_TOGGLE);
        swallow = true;
    } else if (MkMenuActive()) {
        if (!MkPassWhenOpen(vk)) {
            if (push && !MkIsModifier(vk)) MkPush((uint16_t)(vk | (repeat ? MK_REPEAT : 0)));
            swallow = true;
        }
    } else if (wasHeld && repeat) {
        swallow = true;  // opakovani klavesy spolknute za otevreneho menu (drzeny Esc pri zavirani)
    } else if (g_menuCfg.swallowKeys && g_cfg.modifier != 0 &&
               ((g_modKeyBits[vk >> 5] & bit) || TestKeyOwns((int)vk)) && MkModDown(altCtx, async, mods)) {
        // klavesa modu: akci dela OnFrameEnd (GetAsyncKeyState), hra ji nedostane. S Modifier=none se nepolyka
        // (holou F5, F9, Backspace... hra dostavala vzdy; oprava S3). Stisk si zapamatovat i s modifikatorem: kdyz je
        // do dalsiho snimku pusteny, OnFrameEnd ho jinak nevidi.
        swallow = true;
        if (!repeat) {
            KeyTap((int)vk);
            KeyTap(g_cfg.modifier);
            if (g_cfg.modifier == VK_RCONTROL) KeyTap(VK_CONTROL);
        }
    }
    if (swallow && !wasHeld) word.fetch_or(bit, std::memory_order_relaxed);
    if (!swallow && wasHeld) word.fetch_and(~bit, std::memory_order_relaxed);
    return swallow;
}

// WH_KEYBOARD (vlakno zprav okna hry): wParam = VK, lParam = priznaky WM_KEYDOWN. HC_NOREMOVE (PeekMessage bez
// odebrani) se jen propousti; rozhoduje se az pri odebrani zpravy.
static LRESULT CALLBACK MenuKbdProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_menuHookOn.load(std::memory_order_relaxed)) {
        const bool up = (lp & 0x80000000) != 0;
        if (MkDecide((UINT)wp, !up, (lp & 0x40000000) != 0, true, (lp & 0x20000000) != 0, false, 0, g_mkHeldKbd,
                     true)) {
            g_mkSwallowed.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// WH_GETMESSAGE (jen kdyz ma hra registrovany raw input klavesnice): odebirany WM_INPUT klavesnice, ktery se ma
// spolknout, se prepise na WM_NULL. Udalosti do fronty dava jen pri RIDEV_NOLEGACY (jinak je dava MenuKbdProc).
static LRESULT CALLBACK MenuGetMsgProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == PM_REMOVE && lp && g_menuHookOn.load(std::memory_order_relaxed)) {
        MSG* m = (MSG*)lp;
        if (m->message == WM_INPUT) {
            RAWINPUTHEADER hd;
            UINT n = sizeof hd;
            if (GetRawInputData((HRAWINPUT)m->lParam, RID_HEADER, &hd, &n, sizeof hd) == sizeof hd &&
                hd.dwType == RIM_TYPEKEYBOARD) {
                RAWINPUT ri;
                n = sizeof ri;
                if (GetRawInputData((HRAWINPUT)m->lParam, RID_INPUT, &ri, &n, sizeof hd) != (UINT)-1) {
                    const RAWKEYBOARD& k = ri.data.keyboard;
                    const bool down = !(k.Flags & RI_KEY_BREAK);
                    const uint32_t mbit = k.VKey == VK_MENU ? MKM_ALT
                                          : k.VKey == VK_CONTROL ? ((k.Flags & RI_KEY_E0) ? MKM_RCTRL : MKM_LCTRL)
                                          : k.VKey == VK_SHIFT ? MKM_SHIFT : 0u;
                    if (mbit && down) g_mkRawMods.fetch_or(mbit, std::memory_order_relaxed);
                    if (mbit && !down) g_mkRawMods.fetch_and(~mbit, std::memory_order_relaxed);
                    if (MkDecide(k.VKey, down, false, false, false, true,
                                 g_mkRawMods.load(std::memory_order_relaxed), g_mkHeldRaw,
                                 g_rawNoLegacy.load(std::memory_order_relaxed))) {
                        m->message = WM_NULL;
                        m->wParam = 0;
                        m->lParam = 0;
                        g_mkRawSwallowed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Hacky (vse nize jen vlakno Present)
struct MenuHook {
    DWORD tid;
    HHOOK kbd, getMsg;
};
static const int kMenuMaxHooks = 6;
static MenuHook g_mhooks[kMenuMaxHooks];
static int g_nMhooks;
static HWND g_menuGameWnd;     // okno hry z CreateDevice (hDeviceWindow nebo focus)
static DWORD g_menuGameTid;    // jeho vlakno zprav
static bool g_menuGameHooked;  // pokus o hacek pro g_menuGameTid probehl
static bool g_rawKbd;          // hra ma registrovany raw input klavesnice
static uint32_t g_rawSig = ~0u;
static bool g_mhookFailLogged;

static void MenuHookGetMsg(MenuHook& h) {
    if (h.getMsg || !g_rawKbd) return;
    h.getMsg = SetWindowsHookExW(WH_GETMESSAGE, MenuGetMsgProc, nullptr, h.tid);
    if (h.getMsg)
        Log("menu: hacek zprav (WH_GETMESSAGE, raw input klavesnice) pro vlakno %lu", h.tid);
    else
        Log("WARN: menu: hacek zprav (WH_GETMESSAGE) pro vlakno %lu nejde nainstalovat (chyba %lu) - raw input "
            "klavesnice se za otevreneho menu nepolyka",
            h.tid, GetLastError());
}

// Vstup mysi, ktery si vlakno hry vyzvedlo z fronty (WH_GETMESSAGE, jen pocita): raw input mysi (udalosti a soucet
// |dx| + |dy| relativniho pohybu) a WM_MOUSEMOVE. Cast radku "vykon" a samokontrola "mys neotaci kamerou".
static std::atomic<uint32_t> g_ciMouseEv, g_ciMouseMove;
static std::atomic<uint64_t> g_ciMouseDist;
// Aktivace hry podle zprav, ktere dostalo okno hry (WM_ACTIVATEAPP; Source podle ni povoluje mys a bez ni spi
// engine_no_focus_sleep): 1 = aktivni, 0 = neaktivni, -1 = zatim zadna; cas posledni zmeny (GetTickCount64)
static std::atomic<int> g_ciAppActive{-1};
static std::atomic<uint64_t> g_ciAppActTick;
static std::atomic<uint32_t> g_ciAppActN;
static void CiNoteActivate(HWND h, WPARAM wp) {
    if (h != g_menuGameWnd) return;
    g_ciAppActive.store(wp ? 1 : 0, std::memory_order_relaxed);
    g_ciAppActTick.store(GetTickCount64(), std::memory_order_relaxed);
    g_ciAppActN.fetch_add(1, std::memory_order_relaxed);
}
static LRESULT CALLBACK CiCallWndProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && lp) {
        const CWPSTRUCT* c = (const CWPSTRUCT*)lp;
        if (c->message == WM_ACTIVATEAPP) CiNoteActivate(c->hwnd, c->wParam);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
static LRESULT CALLBACK CiGetMsgProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == PM_REMOVE && lp) {
        MSG* m = (MSG*)lp;
        if (Lc_OnPumpMsg(m, false)) return CallNextHookEx(nullptr, code, wp, lp);  // znacka mereni latence -> WM_NULL
        if (m->message == WM_ACTIVATEAPP) CiNoteActivate(m->hwnd, m->wParam);
        if (m->message == WM_INPUT) {
            RAWINPUT ri;
            UINT n = sizeof ri;
            if (GetRawInputData((HRAWINPUT)m->lParam, RID_INPUT, &ri, &n, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_ciMouseEv.fetch_add(1, std::memory_order_relaxed);
                g_ciMouseDist.fetch_add((uint64_t)(labs(ri.data.mouse.lLastX) + labs(ri.data.mouse.lLastY)),
                                        std::memory_order_relaxed);
                if (ri.data.mouse.lLastX || ri.data.mouse.lLastY) Lc_OnPumpMsg(m, true);
            }
        } else if (m->message == WM_MOUSEMOVE) {
            g_ciMouseMove.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
static bool g_ciHooked;
static uint64_t g_ciLastDist;
static uint32_t g_ciLastEv, g_ciLastMove;
static double g_ciLastTurn;
static unsigned g_ciLastCamFrames;

// Cast radku "vykon" (interop.cpp): otaceni kamery a vstup mysi za interval
void ProxyCamInputPerf(char* buf, size_t n) {
    if (!n) return;
    const uint64_t dist = g_ciMouseDist.load(std::memory_order_relaxed);
    const uint32_t ev = g_ciMouseEv.load(std::memory_order_relaxed), mv = g_ciMouseMove.load(std::memory_order_relaxed);
    CURSORINFO ci = {sizeof ci};
    const bool cur = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
    const int act = g_ciAppActive.load(std::memory_order_relaxed);
    snprintf(buf, n, " | kamera: otoceni %.1f st (%u snimku s kamerou), mys%s: %u udalosti, pohyb %llu bodu, WM_MOUSEMOVE %u, "
             "kurzor %s, hra aktivni (WM_ACTIVATEAPP) %s (zmen %u)",
             g_ciTurnDeg - g_ciLastTurn, g_ciCamFrames - g_ciLastCamFrames, g_ciHooked ? "" : " (bez hacku)", ev - g_ciLastEv,
             (unsigned long long)(dist - g_ciLastDist), mv - g_ciLastMove, cur ? "viditelny" : "skryty",
             act < 0 ? "?" : act ? "ano" : "NE", g_ciAppActN.load(std::memory_order_relaxed));
    g_ciLastTurn = g_ciTurnDeg;
    g_ciLastCamFrames = g_ciCamFrames;
    g_ciLastDist = dist;
    g_ciLastEv = ev;
    g_ciLastMove = mv;
}

// Pro samokontrolu (present.cpp, vlakno hry): kumulativni hodnoty od startu
void Proxy_CamInputTotals(double* turnDeg, unsigned* camFrames, uint64_t* mouseDist, bool* hooked, int* appActive) {
    *turnDeg = g_ciTurnDeg;
    *camFrames = g_ciCamFrames;
    *mouseDist = g_ciMouseDist.load(std::memory_order_relaxed);
    *hooked = g_ciHooked;
    *appActive = g_ciAppActive.load(std::memory_order_relaxed);
}

static void MenuHookThread(DWORD tid, const char* what) {
    if (!tid) return;
    for (int i = 0; i < g_nMhooks; ++i)
        if (g_mhooks[i].tid == tid) return;
    if (!g_ciHooked && tid == g_menuGameTid && SetWindowsHookExW(WH_GETMESSAGE, CiGetMsgProc, nullptr, tid)) {
        g_ciHooked = true;
        SetWindowsHookExW(WH_CALLWNDPROC, CiCallWndProc, nullptr, tid);
    }
    if (g_nMhooks >= kMenuMaxHooks) return;
    const HHOOK h = SetWindowsHookExW(WH_KEYBOARD, MenuKbdProc, nullptr, tid);
    if (!h) {
        if (!g_mhookFailLogged) {
            g_mhookFailLogged = true;
            Log("WARN: menu: hacek klavesnice (WH_KEYBOARD) pro vlakno %lu (%s) nejde nainstalovat (chyba %lu) - bez "
                "nej menu klavesy hre nepolyka (ovladani I/K, J/L, Alt+N)",
                tid, what, GetLastError());
        }
        return;
    }
    MenuHook& mh = g_mhooks[g_nMhooks++];
    mh.tid = tid;
    mh.kbd = h;
    mh.getMsg = nullptr;
    Log("menu: hacek klavesnice (WH_KEYBOARD) pro vlakno %lu (%s)%s", tid, what,
        tid == GetCurrentThreadId() ? ", stejne vlakno jako Present" : ", jine vlakno nez Present");
    MenuHookGetMsg(mh);
}

// Raw input (docs\navrh-menu.md 4.3): ma hra registrovanou klavesnici (UsagePage 1, Usage 6)? Radek do logu pri
// prvni kontrole a pri kazde zmene. Vola se pri instalaci hacku, pri otevreni menu a obcas za behu.
static void MenuRawCheck(const char* when) {
    UINT n = 0;
    GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
    std::vector<RAWINPUTDEVICE> dev(n ? n : 1);
    UINT got = n ? GetRegisteredRawInputDevices(dev.data(), &n, sizeof(RAWINPUTDEVICE)) : 0;
    if (got == (UINT)-1) got = 0;
    bool kbd = false, noLegacy = false;
    DWORD kbdFlags = 0;
    uint32_t sig = 2166136261u ^ got;
    char list[400] = "";
    size_t pos = 0;
    for (UINT i = 0; i < got; ++i) {
        const RAWINPUTDEVICE& r = dev[i];
        sig = (sig ^ r.usUsagePage) * 16777619u;
        sig = (sig ^ r.usUsage) * 16777619u;
        sig = (sig ^ r.dwFlags) * 16777619u;
        if (pos < sizeof list - 1)
            pos += (size_t)std::max(0, snprintf(list + pos, sizeof list - pos, "%s%u/%u flags 0x%lX", i ? ", " : "",
                                                r.usUsagePage, r.usUsage, r.dwFlags));
        const bool isKbd = r.usUsagePage == 1 &&
                           (r.usUsage == 6 || r.usUsage == 7 || (r.usUsage == 0 && (r.dwFlags & RIDEV_PAGEONLY)));
        if (isKbd && !(r.dwFlags & RIDEV_REMOVE)) {
            kbd = true;
            kbdFlags = r.dwFlags;
            if (r.dwFlags & RIDEV_NOLEGACY) noLegacy = true;
        }
    }
    if (sig == g_rawSig) return;
    g_rawSig = sig;
    g_rawKbd = kbd;
    g_rawNoLegacy.store(noLegacy, std::memory_order_relaxed);
    if (kbd)
        Log("menu: raw input (%s): %u zarizeni [%s] - klavesnice JE registrovana (flags 0x%lX%s): WM_INPUT klavesnice "
            "polyka hacek zprav WH_GETMESSAGE",
            when, got, list, kbdFlags, noLegacy ? ", NOLEGACY = bez WM_KEYDOWN" : "");
    else
        Log("menu: raw input (%s): %u zarizeni [%s] - klavesnice neni registrovana, staci hacek WH_KEYBOARD", when, got,
            list);
    if (kbd)
        for (int i = 0; i < g_nMhooks; ++i) MenuHookGetMsg(g_mhooks[i]);
}

// Okno hry (H_CreateDevice / H_CreateDeviceEx, pred presmerovanim spiku S2). Hacek se instaluje az v OnFrameEnd.
static void MenuSetGameWindow(HWND w) {
    if (!w || w == g_menuGameWnd) return;
    g_menuGameWnd = w;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId()) return;  // cizi okno: bez hacku (zustava okno v popredi)
    if (tid != g_menuGameTid) {
        g_menuGameTid = tid;
        g_menuGameHooked = false;
    }
}

// DLL_PROCESS_DETACH: hacky ukazuji do teto DLL -> pri FreeLibrary odinstalovat (pri konci procesu je uklidi system).
// Pri ukonceni hry navic zapise, co z menu jeste neni v ini (MenuSaveAtExit).
static void MenuSaveAtExit();
static void MenuShutdown(bool processExit) {
    g_menuHookOn.store(false, std::memory_order_relaxed);
    MenuSaveAtExit();
    if (processExit) return;
    for (int i = 0; i < g_nMhooks; ++i) {
        if (g_mhooks[i].kbd) UnhookWindowsHookEx(g_mhooks[i].kbd);
        if (g_mhooks[i].getMsg) UnhookWindowsHookEx(g_mhooks[i].getMsg);
    }
    g_nMhooks = 0;
}

// ---------------------------------------------------------------------------
// Prekryv: atlas pisma z GDI
// ---------------------------------------------------------------------------
struct OvGlyph {
    uint16_t x, y, w, h;  // bunka v atlasu (w = 0: znak bez kresby, napr. mezera)
    int16_t ox;           // levy okraj bunky vuci peru
    uint8_t ok;           // znak v pismu je
    float adv;            // posun pera (zlomkovy: pri nadvzorkovani je sirka znaku v atlasu advHi / k)
};
enum { OVF_REG, OVF_BOLD, OVF_TITLE, OVF_COUNT };
struct OvFont {
    int px, height, ascent;
    OvGlyph lat[0x180];                              // U+0000..U+017F (ASCII, Latin-1, Latin Extended-A)
    std::vector<std::pair<wchar_t, OvGlyph>> ext;    // ostatni (sipky, uvozovky...)
};
// Spojity tvar pro 9 dilu: roh n x n texelu + sloupec a radek s profilem rovne hrany + texel vnitrku.
struct OvSprite {
    uint16_t x, y, n;
};
enum { OVS_PANEL, OVS_RING, OVS_ROW, OVS_SHADOW, OVS_COUNT };

static struct OvAtlas {
    int px;  // velikost zakladniho pisma, pro kterou atlas plati (0 = zadny)
    UINT w, h;
    std::vector<uint32_t> pix;  // A8R8G8B8, premultiplikovana bila (vsechny kanaly = pokryti)
    OvFont font[OVF_COUNT];
    OvSprite spr[OVS_COUNT];
    float whiteX, whiteY;       // stred bileho bloku (plne plochy)
    wchar_t face[LF_FACESIZE];
    uint32_t hash;
    IDirect3DTexture9* tex;
    IDirect3DDevice9* texDev;
    bool texDefaultPool;
    bool texFailLogged;
} g_ovAtlas;

static void OvReleaseTexture() {
    if (g_ovAtlas.tex) g_ovAtlas.tex->Release();
    g_ovAtlas.tex = nullptr;
    g_ovAtlas.texDev = nullptr;
}

// Nahrada znaku, ktery v pismu chybi (chude fonty, Wine): cestina bez diakritiky, znacky ASCII.
static wchar_t OvFold(wchar_t c) {
    static const wchar_t from[] =
        L"\u00C1\u010C\u010E\u00C9\u011A\u00CD\u0147\u00D3\u0158\u0160\u0164\u00DA\u016E\u00DD\u017D"
        L"\u00E1\u010D\u010F\u00E9\u011B\u00ED\u0148\u00F3\u0159\u0161\u0165\u00FA\u016F\u00FD\u017E"
        L"\u00C4\u0139\u013D\u00D4\u0154\u00E4\u013A\u013E\u00F4\u0155"
        L"\u2039\u203A\u2190\u2192\u2191\u2193\u2022\u2026\u2013\u00D7\u2248\u00B0\u00B7\u2212\u00B2";
    static const wchar_t to[] = L"ACDEEINORSTUUYZ"
                                L"acdeeinorstuuyz"
                                L"ALLORallor"
                                L"<><>^v*.-x~o--2";
    for (int i = 0; from[i]; ++i)
        if (from[i] == c) return to[i];
    return L'?';
}

static const OvGlyph* OvFindGlyphRaw(const OvFont& f, wchar_t c) {
    if (c < 0x180) return f.lat[c].ok ? &f.lat[c] : nullptr;
    for (const auto& e : f.ext)
        if (e.first == c) return &e.second;
    return nullptr;
}
static const OvGlyph* OvFindGlyph(const OvFont& f, wchar_t c) {
    if (const OvGlyph* g = OvFindGlyphRaw(f, c)) return g;
    if (const OvGlyph* g = OvFindGlyphRaw(f, OvFold(c))) return g;
    return OvFindGlyphRaw(f, L'?');
}

// Jednoducha police: radky zleva doprava, vyska radku = nejvyssi bunka v nem. Mezi bunkami 1 texel.
struct OvPacker {
    UINT w, x, y, rowH;
    bool Place(UINT cw, UINT ch, uint16_t* ox, uint16_t* oy) {
        if (cw + 1 > w) return false;
        if (x + cw + 1 > w) {
            x = 1;
            y += rowH + 1;
            rowH = 0;
        }
        *ox = (uint16_t)x;
        *oy = (uint16_t)y;
        x += cw + 1;
        rowH = std::max(rowH, ch);
        return true;
    }
};

static inline float OvSat(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// Tvar do atlasu: kind 0 = vypln se zaoblenym rohem o polomeru n, 1 = obrys tloustky t, 2 = mekky stin.
static void OvDrawSprite(std::vector<uint32_t>& pix, UINT aw, const OvSprite& s, int kind, float t) {
    const int n = s.n;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            const float dx = x < n ? (float)n - ((float)x + 0.5f) : 0.0f;
            const float dy = y < n ? (float)n - ((float)y + 0.5f) : 0.0f;
            const float d = sqrtf(dx * dx + dy * dy);
            float c;
            if (kind == 2) {
                const float a = OvSat(1.0f - d / (float)n);
                c = a * a * (3.0f - 2.0f * a);
            } else {
                c = OvSat((float)n - d + 0.5f);
                if (kind == 1) c *= x == n && y == n ? 0.0f : OvSat(d - ((float)n - t) + 0.5f);
            }
            const uint32_t v = (uint32_t)(c * 255.0f + 0.5f);
            pix[(size_t)(s.y + y) * aw + s.x + x] = v << 24 | v << 16 | v << 8 | v;
        }
}

// Atlas pro zakladni pismo px: tri rezy (text, tucny, nadpis) a tvary. false = GDI selhalo (menu se nekresli).
// Male pismo se kresli k-krat vetsi a zmensuje prumerem k x k (nadvzorkovani): GDI ma ve stupnich sede jen 17 urovni
// a u malych velikosti hinting rozhazuje mezery mezi znaky; takhle jsou tvary i roztece rovnomerne. Velke pismo
// (od 34 px, tj. od ~1600 radku obrazu) se kresli primo.
// Neuspesne stavby atlasu (oprava D9): dalsi pokus nejdriv za kOvAtlasRetryMs, po kOvAtlasMaxFails pokusech se
// vzdava (menu a OSD se jen nekresli); WARN do logu jen pri prvnim selhani a pri vzdani.
static const int kOvAtlasMaxFails = 5;
static const ULONGLONG kOvAtlasRetryMs = 5000;
static int g_ovAtlasFails;
static ULONGLONG g_ovAtlasRetryAt;
static bool OvAtlasBuild(int px) {
    OvAtlas& A = g_ovAtlas;
    OvReleaseTexture();
    A.px = 0;
    A.pix.clear();
    const LONGLONG t0 = OmQpc();
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        if (++g_ovAtlasFails == 1 || g_ovAtlasFails == kOvAtlasMaxFails)
            Log("WARN: menu: atlas pisma: CreateCompatibleDC selhal (pokus %d/%d) - menu se nekresli", g_ovAtlasFails,
                kOvAtlasMaxFails);
        return false;
    }
    // Pismo: prvni z rady, ktere v systemu je (GDI jinak tise vybere nahradu)
    static const wchar_t* const kFaces[] = {L"Segoe UI", L"Tahoma", L"Arial", L""};
    A.face[0] = 0;
    for (const wchar_t* face : kFaces) {
        HFONT f = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        if (!f) continue;
        HGDIOBJ old = SelectObject(dc, f);
        wchar_t got[LF_FACESIZE] = L"";
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        DeleteObject(f);
        if (!face[0] || !_wcsicmp(got, face)) {
            wcsncpy(A.face, got, LF_FACESIZE - 1);
            A.face[LF_FACESIZE - 1] = 0;
            break;
        }
    }
    const int k = px < 20 ? 3 : px < 34 ? 2 : 1;  // nadvzorkovani
    const int sizes[OVF_COUNT] = {px, px, (int)lroundf((float)px * 1.5f)};
    const int weights[OVF_COUNT] = {FW_NORMAL, FW_SEMIBOLD, FW_SEMIBOLD};
    HFONT fonts[OVF_COUNT] = {};
    for (int i = 0; i < OVF_COUNT; ++i)
        fonts[i] = CreateFontW(-sizes[i] * k, 0, 0, 0, weights[i], FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, A.face);

    // Znaky: ASCII, ceska a slovenska abeceda, znacky a vse z tabulky textu
    std::vector<wchar_t> chars;
    {
        std::vector<uint8_t> seen(0x10000, 0);
        auto add = [&](wchar_t c) {
            if (c >= 32 && !seen[c]) {
                seen[c] = 1;
                chars.push_back(c);
            }
        };
        for (wchar_t c = 32; c < 127; ++c) add(c);
        for (const wchar_t* p = kMenuExtraChars; *p; ++p) add(*p);
        for (int i = 0; i < S_COUNT; ++i)
            for (int l = 0; l < 2; ++l)
                for (const wchar_t* p = kMenuStr[i][l]; *p; ++p) add(*p);
    }

    // Mereni a rozmisteni
    A.w = px <= 34 ? 1024 : 2048;
    OvPacker pk = {A.w, 1, 1, 0};
    bool ok = true;
    uint16_t wx = 0, wy = 0;
    ok = pk.Place(8, 8, &wx, &wy) && ok;  // bily blok
    A.whiteX = (float)wx + 4.0f;
    A.whiteY = (float)wy + 4.0f;
    const int radius[OVS_COUNT] = {std::max(4, (int)lroundf((float)px * 0.6f)), std::max(4, (int)lroundf((float)px * 0.6f)),
                                   std::max(2, (int)lroundf((float)px * 0.3f)),
                                   std::max(6, (int)lroundf((float)px * 1.6f))};
    for (int i = 0; i < OVS_COUNT; ++i) {
        A.spr[i].n = (uint16_t)radius[i];
        ok = pk.Place(radius[i] + 1, radius[i] + 1, &A.spr[i].x, &A.spr[i].y) && ok;
    }
    int missing = 0;
    for (int fi = 0; fi < OVF_COUNT && ok; ++fi) {
        OvFont& F = A.font[fi];
        memset(F.lat, 0, sizeof F.lat);
        F.ext.clear();
        if (!fonts[fi]) {
            ok = false;
            break;
        }
        HGDIOBJ old = SelectObject(dc, fonts[fi]);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        F.px = sizes[fi];
        F.height = (tm.tmHeight + k - 1) / k;
        F.ascent = (tm.tmAscent + k / 2) / k;
        for (wchar_t c : chars) {
            WORD gi = 0xFFFF;
            if (GetGlyphIndicesW(dc, &c, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) gi = 0;
            if (gi == 0xFFFF) {
                if (fi == OVF_REG) ++missing;
                continue;
            }
            ABC abc = {};
            if (!GetCharABCWidthsW(dc, c, c, &abc)) {
                INT cw = 0;
                GetCharWidth32W(dc, c, c, &cw);
                abc.abcA = 0;
                abc.abcB = (UINT)std::max(0, cw);
                abc.abcC = 0;
            }
            OvGlyph g = {};
            g.ok = 1;
            const int advHi = abc.abcA + (int)abc.abcB + abc.abcC;
            g.adv = (float)advHi / (float)k;
            if (c != L' ' && abc.abcB > 0) {
                // bunka v pixelech atlasu: od leveho presahu (zaokrouhleno dolu) po pravy okraj kresby, +1 texel okraj
                const int lo = std::min(0, abc.abcA), hi = std::max(advHi, abc.abcA + (int)abc.abcB);
                const int left = (lo - (k - 1)) / k - 1;  // lo <= 0: deleni k zaokrouhlene dolu
                const int right = (hi + k - 1) / k + 1;
                g.ox = (int16_t)left;
                g.w = (uint16_t)(right - left);
                g.h = (uint16_t)F.height;
                if (!pk.Place(g.w, g.h, &g.x, &g.y)) {
                    ok = false;
                    break;
                }
            }
            if (c < 0x180)
                F.lat[c] = g;
            else
                F.ext.push_back(std::make_pair(c, g));
        }
        SelectObject(dc, old);
    }
    UINT need = pk.y + pk.rowH + 2, th = 64;
    while (th < need) th *= 2;
    if (th > 4096) ok = false;
    A.h = th;

    // Kresba: bile znaky na cerne v 32bitovem DIB (k-krat vetsim nez atlas), pokryti = kanal G
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    const UINT dibW = A.w * (UINT)k, dibH = A.h * (UINT)k;
    if (ok) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = (LONG)dibW;
        bi.bmiHeader.biHeight = -(LONG)dibH;  // shora dolu
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) ok = false;
    }
    if (ok) {
        memset(bits, 0, (size_t)dibW * dibH * 4);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkColor(dc, RGB(0, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
        for (int fi = 0; fi < OVF_COUNT; ++fi) {
            const OvFont& F = A.font[fi];
            HGDIOBJ old = SelectObject(dc, fonts[fi]);
            auto draw = [&](wchar_t c, const OvGlyph& g) {
                if (!g.ok || !g.w) return;
                const RECT rc = {g.x * k, g.y * k, (g.x + g.w) * k, (g.y + g.h) * k};
                ExtTextOutW(dc, (g.x - g.ox) * k, g.y * k, ETO_CLIPPED, &rc, &c, 1, nullptr);
            };
            for (int c = 0; c < 0x180; ++c) draw((wchar_t)c, F.lat[c]);
            for (const auto& e : F.ext) draw(e.first, e.second);
            SelectObject(dc, old);
        }
        GdiFlush();
        A.pix.resize((size_t)A.w * A.h);
        const uint8_t* src = (const uint8_t*)bits;
        const uint32_t kk = (uint32_t)(k * k);
        for (UINT y = 0; y < A.h; ++y)
            for (UINT x = 0; x < A.w; ++x) {
                uint32_t sum = 0;
                for (int sy = 0; sy < k; ++sy) {
                    const uint8_t* row = src + ((size_t)(y * k + sy) * dibW + (size_t)x * k) * 4 + 1;
                    for (int sx = 0; sx < k; ++sx) sum += row[sx * 4];
                }
                const uint32_t v = (sum + kk / 2) / kk;
                A.pix[(size_t)y * A.w + x] = v << 24 | v << 16 | v << 8 | v;
            }
        SelectObject(dc, oldBmp);
        for (UINT y = 0; y < 8; ++y)
            for (UINT x = 0; x < 8; ++x) A.pix[(size_t)(wy + y) * A.w + wx + x] = 0xFFFFFFFFu;
        const float ring = (float)std::max(1, (int)lroundf((float)px / 16.0f));
        OvDrawSprite(A.pix, A.w, A.spr[OVS_PANEL], 0, 0.0f);
        OvDrawSprite(A.pix, A.w, A.spr[OVS_RING], 1, ring);
        OvDrawSprite(A.pix, A.w, A.spr[OVS_ROW], 0, 0.0f);
        OvDrawSprite(A.pix, A.w, A.spr[OVS_SHADOW], 2, 0.0f);
        uint32_t h = 2166136261u;
        for (uint32_t v : A.pix) h = (h ^ (v & 0xFF)) * 16777619u;
        A.hash = h;
    }
    if (bmp) DeleteObject(bmp);
    for (HFONT f : fonts)
        if (f) DeleteObject(f);
    DeleteDC(dc);
    if (!ok) {
        A.pix.clear();
        if (++g_ovAtlasFails == 1)
            Log("WARN: menu: atlas pisma pro %d px nejde vytvorit (GDI) - menu se nekresli, dalsi pokus za %u s", px,
                (unsigned)(kOvAtlasRetryMs / 1000));
        else if (g_ovAtlasFails == kOvAtlasMaxFails)
            Log("WARN: menu: atlas pisma nejde vytvorit ani po %d pokusech - menu a OSD se do restartu nekresli",
                kOvAtlasMaxFails);
        return false;
    }
    g_ovAtlasFails = 0;
    A.px = px;
    if (!g_omQpf.QuadPart) QueryPerformanceFrequency(&g_omQpf);
    Log("menu: atlas pisma %ux%u, pismo \"%ls\" %d px (nadpis %d px, nadvzorkovani %dx), %u znaku%s, hash %08X, "
        "%.1f ms",
        A.w, A.h, A.face, px, sizes[OVF_TITLE], k, (unsigned)chars.size(),
        missing ? " - nektere v pismu chybi (nahrada bez diakritiky)" : "", A.hash, (double)(OmQpc() - t0) * 1000.0 / (double)g_omQpf.QuadPart);
    return true;
}

// Textura atlasu na zarizeni d. MANAGED prezije Reset; zarizeni D3D9Ex MANAGED neumi -> dynamicka DEFAULT, ktera se
// pred Resetem pousti (MenuBeforeReset) a pak vytvori znovu z A.pix.
static bool OvEnsureTexture(IDirect3DDevice9* d) {
    OvAtlas& A = g_ovAtlas;
    if (A.tex && A.texDev == d) return true;
    OvReleaseTexture();
    if (!A.px || A.pix.empty()) return false;
    IDirect3DTexture9* t = nullptr;
    bool def = false;
    HRESULT hr = d->CreateTexture(A.w, A.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr);
    if (FAILED(hr) || !t) {
        t = nullptr;
        def = true;
        hr = d->CreateTexture(A.w, A.h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, nullptr);
    }
    D3DLOCKED_RECT lr = {};
    if (SUCCEEDED(hr) && t) hr = t->LockRect(0, &lr, nullptr, def ? D3DLOCK_DISCARD : 0);
    if (FAILED(hr) || !t || !lr.pBits) {
        if (t) t->Release();
        if (!A.texFailLogged) {
            A.texFailLogged = true;
            Log("WARN: menu: textura atlasu %ux%u nejde vytvorit (0x%08X) - menu se nekresli", A.w, A.h, (unsigned)hr);
        }
        return false;
    }
    for (UINT y = 0; y < A.h; ++y)
        memcpy((uint8_t*)lr.pBits + (size_t)y * lr.Pitch, &A.pix[(size_t)y * A.w], (size_t)A.w * 4);
    t->UnlockRect(0);
    A.tex = t;
    A.texDev = d;
    A.texDefaultPool = def;
    Log("menu: textura atlasu %ux%u vytvorena (%s)", A.w, A.h,
        def ? "D3DPOOL_DEFAULT dynamicka, pred Resetem se pousti" : "D3DPOOL_MANAGED");
    return true;
}

// ---------------------------------------------------------------------------
// Prekryv: seznam ctyruhelniku (nezavisly na cili) a kresleni do cile
// ---------------------------------------------------------------------------
struct OvVtx {
    float x, y, z, rhw;
    DWORD col;  // premultiplikovana ARGB
    float u, v;
};
static const DWORD kOvFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
struct OvList {
    std::vector<OvVtx> v;
    std::vector<uint16_t> idx;
    void Clear() {
        v.clear();
        idx.clear();
    }
};

static inline DWORD OvCol(unsigned r, unsigned g, unsigned b, unsigned a) {
    return (DWORD)(a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255));
}

// Ctyruhelnik v pixelech cile (x1, y1 vne), UV v texelech atlasu. Posun -0.5 = stred pixelu podle D3D9.
static void OvQuad(OvList& L, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                   DWORD col) {
    if (L.v.size() + 4 > 65000 || x1 <= x0 || y1 <= y0) return;
    const float iw = 1.0f / (float)g_ovAtlas.w, ih = 1.0f / (float)g_ovAtlas.h;
    const uint16_t b = (uint16_t)L.v.size();
    L.v.push_back({x0 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, col, u0 * iw, v0 * ih});
    L.v.push_back({x1 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, col, u1 * iw, v0 * ih});
    L.v.push_back({x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, col, u1 * iw, v1 * ih});
    L.v.push_back({x0 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, col, u0 * iw, v1 * ih});
    const uint16_t q[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
    L.idx.insert(L.idx.end(), q, q + 6);
}

static void OvRect(OvList& L, int x0, int y0, int x1, int y1, DWORD col) {
    const float u = g_ovAtlas.whiteX, v = g_ovAtlas.whiteY;
    OvQuad(L, (float)x0, (float)y0, (float)x1, (float)y1, u, v, u, v, col);
}

// Obdelnik z 9 dilu tvaru s (rohy 1:1, hrany a stred roztazene z jednoho radku / sloupce / texelu).
static void OvNine(OvList& L, int x0, int y0, int x1, int y1, const OvSprite& s, DWORD col, bool center) {
    if (x1 <= x0 || y1 <= y0) return;
    const int n = std::min((int)s.n, std::min((x1 - x0) / 2, (y1 - y0) / 2));
    const float sx = (float)s.x, sy = (float)s.y, N = (float)s.n, c = N + 0.5f;
    const float X0 = (float)x0, X1 = (float)x1, Y0 = (float)y0, Y1 = (float)y1, fn = (float)n;
    if (n > 0) {
        OvQuad(L, X0, Y0, X0 + fn, Y0 + fn, sx, sy, sx + N, sy + N, col);
        OvQuad(L, X1 - fn, Y0, X1, Y0 + fn, sx + N, sy, sx, sy + N, col);
        OvQuad(L, X0, Y1 - fn, X0 + fn, Y1, sx, sy + N, sx + N, sy, col);
        OvQuad(L, X1 - fn, Y1 - fn, X1, Y1, sx + N, sy + N, sx, sy, col);
        OvQuad(L, X0 + fn, Y0, X1 - fn, Y0 + fn, sx + c, sy, sx + c, sy + N, col);
        OvQuad(L, X0 + fn, Y1 - fn, X1 - fn, Y1, sx + c, sy + N, sx + c, sy, col);
        OvQuad(L, X0, Y0 + fn, X0 + fn, Y1 - fn, sx, sy + c, sx + N, sy + c, col);
        OvQuad(L, X1 - fn, Y0 + fn, X1, Y1 - fn, sx + N, sy + c, sx, sy + c, col);
    }
    if (center) OvQuad(L, X0 + fn, Y0 + fn, X1 - fn, Y1 - fn, sx + c, sy + c, sx + c, sy + c, col);
}

static int OvTextW(int font, const wchar_t* s) {
    const OvFont& F = g_ovAtlas.font[font];
    float w = 0.0f;
    for (; *s; ++s)
        if (const OvGlyph* g = OvFindGlyph(F, *s)) w += g->adv;
    return (int)ceilf(w - 0.01f);
}

// Text od (x, y) = levy horni roh radku pisma; vraci x za textem.
// (pero je zlomkove, kazdy znak se klade na cely pixel: bunky atlasu se kresli 1:1)
static int OvText(OvList& L, int font, int x, int y, const wchar_t* s, DWORD col) {
    const OvFont& F = g_ovAtlas.font[font];
    float pen = (float)x;
    for (; *s; ++s) {
        const OvGlyph* g = OvFindGlyph(F, *s);
        if (!g) continue;
        if (g->w) {
            const float gx = floorf(pen + 0.5f) + (float)g->ox;
            OvQuad(L, gx, (float)y, gx + (float)g->w, (float)(y + g->h), (float)g->x, (float)g->y,
                   (float)(g->x + g->w), (float)(g->y + g->h), col);
        }
        pen += g->adv;
    }
    return (int)ceilf(pen - 0.01f);
}

// Cil kresleni. SINK_LAYER (az s Frame Generation): vlastni vrstva A8R8G8B8, kterou helper slozi po interpolaci
// nebo preda DLSS-G jako UI - do backbufferu (vstupu interpolace) se pak menu kreslit nesmi.
enum OverlaySink { SINK_BACKBUFFER, SINK_LAYER };
static OverlaySink OverlayPickSink() { return SINK_BACKBUFFER; }

static bool g_ovStateFailLogged;

// Nakresli seznam do povrchu target (w x h) jednim draw a vrati stav zarizeni presne do puvodniho stavu.
static bool OverlayRender(IDirect3DDevice9* d, IDirect3DSurface9* target, UINT w, UINT h, const OvList& L) {
    if (L.idx.empty() || !g_ovAtlas.tex) return false;
    g_ovGuardThread = GetCurrentThreadId();
    IDirect3DStateBlock9* sb = nullptr;
    HRESULT hr = d->CreateStateBlock(D3DSBT_ALL, &sb);
    if (FAILED(hr) || !sb) {
        g_ovGuardThread = 0;
        if (!g_ovStateFailLogged) {
            g_ovStateFailLogged = true;
            Log("WARN: menu: CreateStateBlock(D3DSBT_ALL) selhalo (0x%08X) - menu se nekresli", (unsigned)hr);
        }
        return false;
    }
    IDirect3DSurface9* oldRT[4] = {};
    IDirect3DSurface9* oldDS = nullptr;
    for (DWORD i = 0; i < 4; ++i)
        if (FAILED(d->GetRenderTarget(i, &oldRT[i]))) oldRT[i] = nullptr;
    if (FAILED(d->GetDepthStencilSurface(&oldDS))) oldDS = nullptr;
    D3DVIEWPORT9 oldVP = {};
    const bool haveVP = SUCCEEDED(d->GetViewport(&oldVP));
    RECT oldSc = {};
    const bool haveSc = SUCCEEDED(d->GetScissorRect(&oldSc));
    const bool scene = SUCCEEDED(d->BeginScene());  // Present je mimo scenu hry; uvnitr sceny BeginScene selze

    for (DWORD i = 3; i >= 1; --i)
        if (oldRT[i]) o_SetRT(d, i, nullptr);
    const HRESULT hrRT = o_SetRT(d, 0, target);
    HRESULT hrDraw = hrRT;
    if (SUCCEEDED(hrRT)) {
        o_SetDS(d, nullptr);
        const D3DVIEWPORT9 vp = {0, 0, w, h, 0.0f, 1.0f};
        o_SetViewport(d, &vp);
        o_SetVS(d, nullptr);
        d->SetPixelShader(nullptr);
        o_SetFVF(d, kOvFvf);
        o_SetStreamSourceFreq(d, 0, 1);
        static const struct {
            D3DRENDERSTATETYPE st;
            DWORD v;
        } kRS[] = {{D3DRS_ZENABLE, FALSE},
                   {D3DRS_ZWRITEENABLE, FALSE},
                   {D3DRS_STENCILENABLE, FALSE},
                   {D3DRS_ALPHATESTENABLE, FALSE},
                   {D3DRS_CULLMODE, D3DCULL_NONE},
                   {D3DRS_FILLMODE, D3DFILL_SOLID},
                   {D3DRS_SHADEMODE, D3DSHADE_GOURAUD},
                   {D3DRS_FOGENABLE, FALSE},
                   {D3DRS_LIGHTING, FALSE},
                   {D3DRS_SPECULARENABLE, FALSE},
                   {D3DRS_CLIPPLANEENABLE, 0},
                   {D3DRS_SCISSORTESTENABLE, FALSE},
                   {D3DRS_MULTISAMPLEANTIALIAS, FALSE},  // MSAA backbuffer: text 1:1 bez rozmazani mezi vzorky
                   {D3DRS_SRGBWRITEENABLE, FALSE},
                   {D3DRS_COLORWRITEENABLE, 0xF},
                   {D3DRS_ALPHABLENDENABLE, TRUE},
                   {D3DRS_SEPARATEALPHABLENDENABLE, FALSE},
                   {D3DRS_BLENDOP, D3DBLENDOP_ADD},
                   {D3DRS_SRCBLEND, D3DBLEND_ONE},  // premultiplikovana alfa
                   {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA},
                   {D3DRS_WRAP0, 0}};
        for (const auto& rs : kRS) o_SetRenderState(d, rs.st, rs.v);
        o_SetTexture(d, 0, g_ovAtlas.tex);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        static const struct {
            D3DSAMPLERSTATETYPE st;
            DWORD v;
        } kSS[] = {{D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
                   {D3DSAMP_MINFILTER, D3DTEXF_POINT},    {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
                   {D3DSAMP_MIPFILTER, D3DTEXF_NONE},     {D3DSAMP_MIPMAPLODBIAS, 0},
                   {D3DSAMP_MAXMIPLEVEL, 0},              {D3DSAMP_SRGBTEXTURE, FALSE}};
        for (const auto& ss : kSS) o_SetSamplerState(d, 0, ss.st, ss.v);
        hrDraw = o_DrawIdxPrimUP(d, D3DPT_TRIANGLELIST, 0, (UINT)L.v.size(), (UINT)L.idx.size() / 3, L.idx.data(),
                                 D3DFMT_INDEX16, L.v.data(), sizeof(OvVtx));
    }
    if (scene) d->EndScene();

    // Zpet stav hry: cile, pak state block, nakonec viewport a scissor rucne (SetRenderTarget je meni)
    o_SetRT(d, 0, oldRT[0]);
    for (DWORD i = 1; i < 4; ++i)
        if (oldRT[i]) o_SetRT(d, i, oldRT[i]);
    o_SetDS(d, oldDS);
    hr = sb->Apply();
    sb->Release();
    if (haveVP) o_SetViewport(d, &oldVP);
    if (haveSc) d->SetScissorRect(&oldSc);
    g_ovGuardThread = 0;
    for (IDirect3DSurface9*& s : oldRT) OmSafeRelease(s);
    OmSafeRelease(oldDS);
    if ((FAILED(hr) || FAILED(hrDraw)) && !g_ovStateFailLogged) {
        g_ovStateFailLogged = true;
        Log("WARN: menu: kresleni prekryvu selhalo (cil 0x%08X, draw 0x%08X, obnova stavu 0x%08X)", (unsigned)hrRT,
            (unsigned)hrDraw, (unsigned)hr);
    }
    return SUCCEEDED(hr) && SUCCEEDED(hrDraw);
}

// ---------------------------------------------------------------------------
// Model menu (docs\navrh-menu.md 6.1-6.4). Polozka = radek s hodnotou, sekce = pole polozek na jedne strance (Bezne /
// Pokrocile). Nova polozka nebo sekce = dalsi radek v polich nize; kresleni, vstup, OSD ani ukladani se nemeni.
// ---------------------------------------------------------------------------
enum ItemKind : uint8_t {
    IK_CHOICE,  // vyber z voleb (choices)
    IK_TOGGLE,  // vypnuto / zapnuto (get vraci 0 / 1)
    IK_ACTION,  // Enter neco provede (zachyt snimku)
    IK_INFO     // jen ke cteni (info)
};
enum : uint8_t { MIF_HEAVY = 1 };  // zmena = nova DLSS feature v helperu: sipky s odkladem kMenuHeavyMs, Enter hned
enum : uint8_t {
    CHF_SOON = 1,   // volba zatim neni k dispozici (pri listovani se preskakuje)
    CHF_NOSAVE = 2  // volba plati jen do restartu, do ini se neuklada (DLSS vypnuto, konstantni hloubka)
};
enum MenuPage : uint8_t { PAGE_COMMON, PAGE_ADVANCED, PAGE_COUNT };
static const int kNoDef = -0x7FFFFFFF;
static const ULONGLONG kMenuHeavyMs = 350;

struct Choice {
    int id;           // hodnota pro get / set
    StrId label;      // S_NONE = text je v txt (cisla, jmena)
    const char* ini;  // hodnota do ini; nullptr = v iniBuf
    uint8_t flags;    // CHF_*
    wchar_t txt[24];
    char iniBuf[16];
};
static Choice ChS(int id, StrId label, const char* ini, uint8_t fl = 0) {
    Choice c = {};
    c.id = id;
    c.label = label;
    c.ini = ini;
    c.flags = fl;
    return c;
}
static Choice ChT(int id, const wchar_t* txt, const char* ini, uint8_t fl = 0) {
    Choice c = ChS(id, S_NONE, nullptr, fl);
    wcsncpy(c.txt, txt, 23);
    if (ini) strncpy(c.iniBuf, ini, 15);
    return c;
}
static inline const wchar_t* ChText(const Choice& c) { return c.label != S_NONE ? Tr(c.label) : c.txt; }
static inline const char* ChIni(const Choice& c) { return c.ini ? c.ini : c.iniBuf; }
static int MenuCopyChoices(const Choice* src, int n, Choice* out, int max) {
    n = std::min(n, max);
    for (int i = 0; i < n; ++i) out[i] = src[i];
    return n;
}

// Cislo pro zobrazeni (txt: desetinna carka v cestine, minus U+2212) a do ini (tecka). dec = nejvys desetinnych mist,
// koncove nuly se usekaji (zustane aspon jedno misto).
static void MenuFmtNum(float v, int dec, wchar_t* txt, size_t ntxt, char* ini, size_t nini) {
    char b[32];
    snprintf(b, sizeof b, "%.*f", dec, fabsf(v));
    std::string r = b;
    if (r.find('.') != std::string::npos)
        while (r.size() > 3 && r.back() == '0' && r[r.size() - 2] != '.') r.pop_back();
    const bool neg = v < 0.0f && strtod(r.c_str(), nullptr) != 0.0;
    if (ini && nini) snprintf(ini, nini, "%s%s", neg ? "-" : "", r.c_str());
    if (txt && ntxt) {
        std::wstring w = neg ? L"−" : L"";
        for (char c : r) w.push_back(c == '.' && g_menuCfg.lang == 0 ? L',' : (wchar_t)c);
        wcsncpy(txt, w.c_str(), ntxt - 1);
        txt[ntxt - 1] = 0;
    }
}

// Seznam id (vzestupne) doplneny o aktualni hodnotu, kdyz v nem neni (hodnota z ini mimo nabidku)
static int MenuIdsWith(const int* ids, int n, int cur, int* out) {
    int k = 0;
    bool have = false;
    for (int i = 0; i < n; ++i) have |= ids[i] == cur;
    for (int i = 0; i < n; ++i) {
        if (!have && cur != kNoDef && cur < ids[i]) {
            out[k++] = cur;
            have = true;
        }
        out[k++] = ids[i];
    }
    if (!have && cur != kNoDef) out[k++] = cur;
    return k;
}

struct MenuItem {
    StrId label, help;
    ItemKind kind;
    uint8_t flags;                         // MIF_*
    int osd;                               // OSD_* Alt klavesy, ktera polozku meni (-1 = zadna)
    const char *iniSec, *iniKey, *iniDef;  // iniSec == nullptr: jen za behu, neuklada se; iniDef = hodnota bez klice
    int def;                               // id vychozi volby (Del), kNoDef = zadna
    int (*get)();                          // id aktualni volby (-1 = zadna z nabidky)
    void (*set)(int id, Src src);          // akce Set* (spolecne s Alt klavesami)
    int (*choices)(Choice* out, int max);  // seznam voleb (dynamicky: podle stavu, schopnosti helperu a GPU)
    bool (*visible)();                     // nullptr = vzdy
    bool (*applies)(StrId* why);           // false = sede + duvod v napovede; nullptr = vzdy plati
    StrId (*note)();                       // poznamka k napovede podle stavu (S_NONE = zadna)
    const char* (*noteText)();             // poznamka textem za behu (ASCII, napr. duvod od helperu); nullptr = zadna
    void (*info)(wchar_t* out, int n);     // IK_INFO: text hodnoty
    void (*action)();                      // IK_ACTION
    const char* legacyKey;                 // dosavadni klavesa do napovedy ("Alt+F9")

    MenuItem Ini(const char* sec, const char* key, const char* dflt) const {
        MenuItem r = *this;
        r.iniSec = sec;
        r.iniKey = key;
        r.iniDef = dflt;
        return r;
    }
    MenuItem Def(int d) const { MenuItem r = *this; r.def = d; return r; }
    MenuItem Key(const char* k) const { MenuItem r = *this; r.legacyKey = k; return r; }
    MenuItem Osd(int o) const { MenuItem r = *this; r.osd = o; return r; }
    MenuItem Heavy() const { MenuItem r = *this; r.flags |= MIF_HEAVY; return r; }
    MenuItem Vis(bool (*f)()) const { MenuItem r = *this; r.visible = f; return r; }
    MenuItem App(bool (*f)(StrId*)) const { MenuItem r = *this; r.applies = f; return r; }
    MenuItem Note(StrId (*f)()) const { MenuItem r = *this; r.note = f; return r; }
    MenuItem NoteTxt(const char* (*f)()) const { MenuItem r = *this; r.noteText = f; return r; }
    MenuItem Info(void (*f)(wchar_t*, int)) const { MenuItem r = *this; r.info = f; return r; }
    MenuItem Act(void (*f)()) const { MenuItem r = *this; r.action = f; return r; }
};
static MenuItem MI(ItemKind kind, StrId label, StrId help, int (*get)() = nullptr, void (*set)(int, Src) = nullptr,
                   int (*choices)(Choice*, int) = nullptr) {
    MenuItem r = {};
    r.kind = kind;
    r.label = label;
    r.help = help;
    r.get = get;
    r.set = set;
    r.choices = choices;
    r.def = kNoDef;
    r.osd = -1;
    return r;
}
struct MenuSection {
    StrId title;
    const MenuItem* items;
    int n;
    bool (*visible)();
    uint8_t page;  // MenuPage
};

// --- spolecne podminky
static bool VisDlss() {  // polozky jen pro DLSS (pri FSR se skryji; u "auto" podle skutecne beziciho upscaleru)
    const int u = GetUpscaler();
    if (u == UPS_AUTO) return Interop_ActiveUpscaler(nullptr) != BMSD_UPSCALER_FSR;
    return u != UPS_FSR && u != UPS_XESS;
}
static bool AppNotEco(StrId* why) {  // v casti B (zmenseny render) se nepouziva
    if (!g_pathB) return true;
    *why = S_WHY_ECO;
    return false;
}
static bool AppliesSoon(StrId* why) {
    *why = S_WHY_SOON;
    return false;
}
static bool DlssRunning() { return Interop_GetMode() == BMSD_MODE_DLAA && Interop_DlaaReady(); }

// --- Obraz
// Auto (vychozi) = DLSS, kdyz jde, jinak FSR. DLSS je sede (preskakuje se), kdyz ho helper hlasi jako trvale nedostupne
// (karty bez NGX: "jen NVIDIA RTX" v napovede, UpscalerNote); XeSS se pripravuje.
static int ChUpscaler(Choice* o, int max) {
    static const int ids[] = {UPS_AUTO, UPS_DLSS, UPS_FSR, UPS_XESS, UPS_OFF};
    static const StrId lb[] = {S_UPS_AUTO, S_UPS_DLSS, S_UPS_FSR, S_UPS_XESS, S_OFF};
    static const char* const ini[] = {"auto", "dlss", "fsr", "xess", "off"};
    int n = 0;
    for (int i = 0; i < 5 && n < max; ++i) {
        if (ids[i] == UPS_XESS && !UpscalerAvailable(UPS_XESS)) continue;  // XeSS se zatim nenabizi (ini: = auto)
        o[n++] = ChS(ids[i], lb[i], ini[i],
                     (uint8_t)((UpscalerAvailable(ids[i]) ? 0 : CHF_SOON) | (ids[i] == UPS_OFF ? CHF_NOSAVE : 0)));
    }
    return n;
}
static StrId UpscalerNote() {
    const int g = Interop_GpuHint();  // hybridni notebook: vysvetluje i nedostupne DLSS
    if (g) return g == 1 ? S_NOTE_GPU_IGPU : S_NOTE_GPU_NODLSS;
    if (Interop_FsrBroken() == 1) return S_NOTE_FSR_VCRT;  // (to jde opravit; DLSS na kartach bez RTX ne)
    const int b = Interop_DlssBroken();
    return b == 1 ? S_NOTE_DLSS_RTX : b ? S_NOTE_DLSS_BROKEN : S_NONE;
}

static int ChOutScale(Choice* o, int max) {
    // Bez nazvu rezimu DLSS (Quality / Performance...): hra kresli vzdy nativne, rezim je jen vnitrni pomer upscaleru
    // a u FSR by "DLAA" nesedelo (uzivatel 2026-11-21)
    static const char* const kIni[] = {"1.0", "4/3", "1.5", "1.724", "2.0", "3.0"};
    int n = 0;
    for (int i = 0; i < 6 && n < max; ++i) {
        const float s = kScaleAllowed[i];
        if (i == 5 && g_outScale < 2.9f && g_cfg.outScale < 2.9f) continue;  // 3.0 jen kdyz je v ini
        if (i == 0) {
            o[n++] = ChS((int)lroundf(s * 100.0f), S_OUT_NATIVE, kIni[i]);
            continue;
        }
        wchar_t num[16], t[24];
        if (i == 1) wcscpy(num, L"4/3");
        else MenuFmtNum(s, i == 3 ? 2 : 1, num, 16, nullptr, 0);
        swprintf(t, 24, L"%ls×", num);
        o[n++] = ChT((int)lroundf(s * 100.0f), t, kIni[i]);
    }
    return n;
}
static int GetOutScale() { return (int)lroundf(g_outScale * 100.0f); }
static void SetOutScaleId(int id, Src src) {
    for (float s : kScaleAllowed)
        if ((int)lroundf(s * 100.0f) == id) SetOutScale(s, src);
}

static int ChPreset(Choice* o, int max) {
    const Choice k[] = {ChS(0, S_ST_DEFAULT_K, "0"), ChS(11, S_PRE_K, "K"), ChS(12, S_PRE_L, "L"), ChS(13, S_PRE_M, "M")};
    return MenuCopyChoices(k, 4, o, max);
}

static int ChSharp(Choice* o, int max) {
    int n = 0;
    for (int i = 0; i <= 10 && n < max; ++i) {
        Choice c = i ? ChS(i, S_NONE, nullptr) : ChS(0, S_OFF, "0.0");
        if (i) MenuFmtNum((float)i / 10.0f, 1, c.txt, 24, c.iniBuf, 16);
        o[n++] = c;
    }
    return n;
}

static int ChDown(Choice* o, int max) {
    const Choice k[] = {ChT(BMSD_DOWN_CATMULLROM, L"Catmull-Rom", "1"), ChT(BMSD_DOWN_LANCZOS2, L"Lanczos-2", "0"),
                        ChT(BMSD_DOWN_MITCHELL, L"Mitchell", "2"), ChT(BMSD_DOWN_BOX, L"box", "3")};
    return MenuCopyChoices(k, 4, o, max);
}

static int GetMipBias() { return g_mipBiasEnabled ? (int)lroundf(-g_mipBiasBase * 100.0f) : 0; }
static int ChMipBias(Choice* o, int max) {
    static const int ids[] = {0, 25, 50, 75, 100};
    int all[8];
    const int k = MenuIdsWith(ids, 5, GetMipBias(), all);
    int n = 0;
    for (int i = 0; i < k && n < max; ++i) {
        Choice c = all[i] ? ChS(all[i], S_NONE, nullptr) : ChS(0, S_OFF, "0");
        if (all[i]) MenuFmtNum(-(float)all[i] / 100.0f, 2, c.txt, 24, c.iniBuf, 16);
        o[n++] = c;
    }
    return n;
}

static int ChMipBiasB(Choice* o, int max) {
    const Choice k[] = {ChS(0, S_MIPB_0, nullptr), ChS(1, S_MIPB_1, nullptr), ChT(2, L"log2(r)", nullptr),
                        ChS(3, S_OFF, nullptr)};
    return MenuCopyChoices(k, 4, o, max);
}

static int GetObjMv() { return g_omEnabled ? 1 : 0; }
static void SetObjMv(int id, Src src) { OmSetEnabled(id != 0, src); }

// --- Usporny rezim (cast B)
static void EcoInfo(wchar_t* out, int n) {
    if (!g_pathB || !g_renderW || !g_renderSurfW) {
        swprintf(out, n, L"%ls", Tr(S_ST_NATIVE));
        return;
    }
    const float r = RenderRatio();
    const wchar_t* q = fabsf(r - 0.667f) < 0.01f  ? L"Quality"
                       : fabsf(r - 0.58f) < 0.01f ? L"Balanced"
                       : fabsf(r - 0.5f) < 0.01f  ? L"Performance"
                                                  : Tr(S_ST_OTHER);
    wchar_t num[16];
    MenuFmtNum(r, 2, num, 16, nullptr, 0);
    swprintf(out, n, L"%ls %ls (%u×%u)", q, num, g_renderW, g_renderH);
}

// --- Frame Generation (ini [fg] Mode=0|dlssg|fsr2x|fsradaptive|dlssgadaptive, TargetFps). Stav a prepinani: src\present.cpp
// (Present_Fg*). DLSS-G 2x a FSR 2x generuje hlavni helper; zapnuti i vypnuti plati hned (presenter v helperu se
// zapne / zastavi za behu, [fg] LiveSwitch). FSR adaptivni zatim "pripravuje se". Jen kdyz zarizeni hry nejde
// presmerovat (VSync ve hre, cela obrazovka v jinem rozliseni, LiveSwitch=0) nebo presenter spadl do zalohy, plati
// zapnuti az po restartu hry (poznamka).
enum FgMode { FG_OFF, FG_DLSSG, FG_FSR2X, FG_FSR_ADAPTIVE, FG_DLSSG_ADAPTIVE, FG_COUNT };
static_assert(FG_OFF == BMSD_FG_OFF && FG_DLSSG == BMSD_FG_DLSSG && FG_FSR2X == BMSD_FG_FSR2X &&
                  FG_FSR_ADAPTIVE == BMSD_FG_FSRADAPT && FG_DLSSG_ADAPTIVE == BMSD_FG_DLSSGADAPT &&
                  FG_COUNT == BMSD_FG_COUNT,
              "FgMode = BMSD_FG_*");
static int ChFgMode(Choice* out, int max) {
    // poradi v menu: pevne nasobky, pak adaptivni (cisla rezimu jsou BMSD_FG_*). DLSS-G adaptivni jen na GPU s DLSS-G
    // nad 2x (RTX 50) - jinde seda, duvod v poznamce.
    const Choice k[] = {ChS(FG_OFF, S_OFF, "0"),
                        ChS(FG_DLSSG, S_FG_DLSSG, "dlssg", Present_FgModeAvailable(BMSD_FG_DLSSG) ? 0 : CHF_SOON),
                        ChS(FG_FSR2X, S_FG_FSR2X, "fsr2x", Present_FgModeAvailable(BMSD_FG_FSR2X) ? 0 : CHF_SOON),
                        ChS(FG_DLSSG_ADAPTIVE, S_FG_DLSSGADAPT, "dlssgadaptive",
                            Present_FgModeAvailable(BMSD_FG_DLSSGADAPT) ? 0 : CHF_SOON),
                        ChS(FG_FSR_ADAPTIVE, S_FG_FSRADAPT, "fsradaptive",
                            Present_FgModeAvailable(BMSD_FG_FSRADAPT) ? 0 : CHF_SOON)};
    return MenuCopyChoices(k, FG_COUNT, out, max);
}
static int GetFgMode() { return Present_FgGetMode(); }
static bool FgAdaptSel() {
    const int m = Present_FgGetMode();
    return m == BMSD_FG_FSRADAPT || m == BMSD_FG_DLSSGADAPT;
}
static void SetFgMode(int id, Src src) {
    if (id < FG_OFF || id >= FG_COUNT || !Present_FgModeAvailable(id)) return;
    Present_FgSetMode(id);
    Log("frame generation: %s%s%s",
        id == FG_DLSSG ? "DLSS-G" : id == FG_FSR2X ? "FSR" : id == FG_FSR_ADAPTIVE ? "FSR adaptivni"
        : id == FG_DLSSG_ADAPTIVE ? "DLSS-G adaptivni" : "vypnuto",
        SrcTag(src),
        id != FG_OFF && !Present_FgPresenterActive() && !Present_FgRetryable()
            ? " - plati az po restartu hry (za behu nejde, viz log prezentace)"
            : "");
}
static StrId FgModeNote() {
    if (Present_FgGetMode() != BMSD_FG_OFF && !Present_FgPresenterActive() && !Present_FgRetryable()) return S_NOTE_RESTART;
    // DLSS-G na karte je, ale jen 2x (RTX 40): proc je "DLSS-G adaptivni" sede
    return Present_FgModeAvailable(BMSD_FG_DLSSG) && !Present_FgModeAvailable(BMSD_FG_DLSSGADAPT) ? S_NOTE_FG_DGADAPT_GPU
                                                                                                  : S_NONE;
}
// Nasobek ([fg] Multiplier=auto|2..6): zobrazene snimky na jeden skutecny (5x a 6x od v25j, auto = FgAutoMult). FSR 2x umi jen 2x; DLSS-G podle GPU (helper hlasi
// NGX MultiFrameCountMax: RTX 50 4x, RTX 40 2x). Volby nad moznosti rezimu jsou sede (pri listovani se preskakuji),
// duvod v poznamce; s vypnutym FG se nabizi podle DLSS-G. Zmena plati hned (helper i limiter hry).
static int FgMultMaxNow() {
    const int m = Present_FgGetMode();
    // s vypnutym FG podle DLSS-G; bez DLSS-G (RTX 30 a starsi, AMD, Intel) podle FSR (2x)
    if (m == BMSD_FG_OFF) return Present_FgMultMax(Present_FgModeAvailable(BMSD_FG_DLSSG) ? BMSD_FG_DLSSG : BMSD_FG_FSR2X);
    return Present_FgMultMax(m);
}
static int ChFgMult(Choice* o, int max) {
    const int mx = FgMultMaxNow();
    // Auto jde vybrat vzdy (v25m): na karte s nejvys 2x drzi 2x (RTX 40 s DLSS-G); FSR 2x-5x (v25n)
    const Choice k[] = {ChT(0, L"Auto", "auto", 0), ChT(2, L"2×", "2", 0),
                        ChT(3, L"3×", "3", mx >= 3 ? 0 : CHF_SOON), ChT(4, L"4×", "4", mx >= 4 ? 0 : CHF_SOON),
                        ChT(5, L"5×", "5", mx >= 5 ? 0 : CHF_SOON), ChT(6, L"6×", "6", mx >= 6 ? 0 : CHF_SOON)};
    return MenuCopyChoices(k, 6, o, max);
}
static void SetFgMult(int id, Src src) {
    if (id == 0) {
        Present_FgSetMult(0);
        Log("frame generation: nasobek auto%s", SrcTag(src));
        return;
    }
    if (id < 2 || id > 6 || id > FgMultMaxNow()) return;
    Present_FgSetMult(id);
    Log("frame generation: nasobek %dx%s", id, SrcTag(src));
}
static StrId FgMultNote() {
    const int m = Present_FgGetMode();
    const bool autoOn = Present_FgMultAuto();
    const bool fsr = m == BMSD_FG_FSR2X || (m == BMSD_FG_OFF && !Present_FgModeAvailable(BMSD_FG_DLSSG));
    if (fsr) {
        if (Present_FgMultMax(BMSD_FG_FSR2X) <= 2) return S_NOTE_FG_MULT_FSR;
        return autoOn ? S_NOTE_FG_MULT_AUTO : S_NOTE_FG_MULT_FSR5;
    }
    if (Present_FgMultKnown()) {
        const int mx = Present_FgMultMax(BMSD_FG_DLSSG);
        if (mx <= 2) return autoOn ? S_NOTE_FG_MULT_AUTO2 : S_NOTE_FG_MULT_RTX50;
        if (mx == 3) return S_NOTE_FG_MULT_MAX3;
        if (mx == 4) return S_NOTE_FG_MULT_MAX4;
    }
    if (Present_FgMultAuto()) return S_NOTE_FG_MULT_AUTO;
    return S_NONE;
}
// Ciselne polozky FPS (od v24z4, uzivatel: "plynule po jednom az do 1000"): volby jen okolo aktualni hodnoty -
// 0 (auto / vyp), predchozi, aktualni, dalsi. Sipka tedy posune o krok; drzena sipka krok zrychli (g_menuNumStep:
// 1 -> 5 -> 10 -> 25, zaokrouhleno na nasobky kroku). Z 0 se jde na base (napr. skutecna auto hodnota).
static int g_menuNumStep = 1;
static void MenuNumAccel(int dir) {
    static ULONGLONG s_t;
    static int s_dir, s_cnt;
    const ULONGLONG now = GetTickCount64();
    if (dir == s_dir && now - s_t < 250) ++s_cnt;
    else s_cnt = 0;
    s_dir = dir;
    s_t = now;
    g_menuNumStep = s_cnt >= 45 ? 25 : s_cnt >= 25 ? 10 : s_cnt >= 10 ? 5 : 1;
}
static int MenuNumChoices(Choice* o, int max, int cur, int lo, int hi, int base, StrId zeroLabel, const wchar_t* zeroTxt) {
    int v[4], k = 0;
    v[k++] = 0;
    if (cur <= 0) {
        v[k++] = std::min(hi, std::max(lo, base));
    } else {
        cur = std::min(hi, std::max(lo, cur));
        const int st = g_menuNumStep;
        const int prev = std::max(lo, ((cur + st - 1) / st - 1) * st);
        const int next = std::min(hi, (cur / st + 1) * st);
        if (prev < cur) v[k++] = prev;
        v[k++] = cur;
        if (next > cur) v[k++] = next;
    }
    int n = 0;
    for (int i = 0; i < k && n < max; ++i) {
        Choice c = ChS(v[i], v[i] ? S_NONE : zeroLabel, nullptr);
        if (v[i]) swprintf(c.txt, 24, L"%d", v[i]);
        else if (zeroTxt) swprintf(c.txt, 24, L"%ls", zeroTxt);
        snprintf(c.iniBuf, sizeof c.iniBuf, "%d", v[i]);
        o[n++] = c;
    }
    return n;
}
// Strop vystupu (zobrazenych FPS); hra pri generovani bezi na vystup / nasobek. 0 = auto (Hz - Hz^2/3600).
static int ChFgTarget(Choice* o, int max) {
    PresentFgInfo fi;
    Present_FgInfo(&fi);
    wchar_t autoTxt[24];
    if (Present_FgGetTargetFps() == 0 && fi.targetFps) swprintf(autoTxt, 24, L"%ls (%u)", Tr(S_AUTO), fi.targetFps);
    else swprintf(autoTxt, 24, L"%ls", Tr(S_AUTO));
    return MenuNumChoices(o, max, Present_FgGetTargetFps(), 20, kFgTargetMax, fi.targetFps ? (int)fi.targetFps : 158, S_NONE,
                          autoTxt);
}
static void SetFgTarget(int id, Src src) {
    Present_FgSetTargetFps(id);
    if (id > 0) Log("frame generation: strop vystupu %d FPS%s", id, SrcTag(src));
    else Log("frame generation: strop vystupu auto%s", SrcTag(src));
}
// Adaptivni FSR: strop skutecnych FPS (limiter v proxy), hranice doplnovani (rozvrh A) a rozvrh A / B
static int ChFgRealCap(Choice* o, int max) {
    return MenuNumChoices(o, max, Present_FgGetRealCap(), 20, 1000, 60, S_OFF, nullptr);
}
static void SetFgRealCap(int id, Src src) {
    Present_FgSetRealCap(id);
    Log("frame generation: adaptivni - strop skutecnych FPS %d%s%s", id, id ? "" : " (vyp)", SrcTag(src));
}
static int ChFgFill(Choice* o, int max) {
    PresentFgInfo fi;
    Present_FgInfo(&fi);
    wchar_t autoTxt[24];
    if (Present_FgGetFillBelow() == 0 && fi.adaptFill > 1.0f)
        swprintf(autoTxt, 24, L"%ls (%ld)", Tr(S_AUTO), lroundf(fi.adaptFill));
    else swprintf(autoTxt, 24, L"%ls", Tr(S_AUTO));
    return MenuNumChoices(o, max, Present_FgGetFillBelow(), 20, 1000, fi.adaptFill > 1.0f ? (int)lroundf(fi.adaptFill) : 140,
                          S_NONE, autoTxt);
}
static void SetFgFill(int id, Src src) {
    Present_FgSetFillBelow(id);
    Log("frame generation: adaptivni - doplnovat pod %d FPS%s%s", id, id ? "" : " (auto)", SrcTag(src));
}
static StrId FgAdaptNote() {
    PresentFgInfo fi;
    Present_FgInfo(&fi);
    return (fi.requested == BMSD_FG_FSRADAPT || fi.requested == BMSD_FG_DLSSGADAPT) && fi.state == BMSD_FGS_ACTIVE &&
                   !fi.adaptEngaged && Present_FgGetPref() == PREF_ALWAYS  // jinak duvod v radku "Rozhodnuti"
               ? S_NOTE_FG_ADAPT_IDLE
               : S_NONE;
}
// Radek "Stav": co helper prave dela (skutecne -> zobrazene FPS a cas generatoru), nebo proc negeneruje
static void FgStateInfo(wchar_t* out, int n) {
    PresentFgInfo fi;
    Present_FgInfo(&fi);
    StrId id = S_FGS_STARTING;
    if (fi.requested == BMSD_FG_OFF) id = S_OFF;
    else if (!fi.presenter) id = S_FGS_RESTART;
    else if (fi.fallback) id = S_FGS_FALLBACK;
    else if (!fi.helperReady) id = S_FGS_STARTING;
    else if (fi.state == BMSD_FGS_ACTIVE || (fi.state == BMSD_FGS_PAUSED && fi.menuPause)) {
        // generuje: "DLSS-G 310.9.16 · 78 → 156 FPS · 1,8 ms" (nazev a verze generatoru od helperu). S otevrenym menu je
        // FG pozastavene ([fg] PauseInMenu): cisla z posledniho intervalu, kdy generovalo (jinak by je nebylo kde videt).
        const bool act = fi.state == BMSD_FGS_ACTIVE;
        const float real = act ? fi.realFps : fi.lastRealFps, outF = act ? fi.outFps : fi.lastOutFps;
        if (outF > 1.0f && (!act || fi.genShare)) {
            // "skutecne + generovane = celkem" (generovane = celkem - skutecne, aby soucet sedel po zaokrouhleni)
            wchar_t ms[16];
            MenuFmtNum(act ? fi.evalMs : fi.lastEvalMs, 1, ms, 16, nullptr, 0);
            const long r = lroundf(real), t = lroundf(outF);
            swprintf(out, n, L"%hs%ls%ld + %ld gen. = %ld FPS · %ls ms", fi.name, *fi.name ? L" · " : L"", r,
                     t > r ? t - r : 0L, t, ms);
            return;
        }
        id = act ? S_FGS_ACTIVE : S_FGS_PAUSED_MENU;
    } else if (fi.state == BMSD_FGS_PAUSED) id = S_FGS_PAUSED;
    else if (fi.state == BMSD_FGS_UNAVAILABLE) id = S_FGS_UNAVAIL;
    else if (fi.state == BMSD_FGS_FAILED) id = S_FGS_FAILED;
    if (fi.presenter && !fi.fallback && fi.helperReady && *fi.name && fi.state != BMSD_FGS_UNAVAILABLE &&
        fi.state != BMSD_FGS_FAILED && fi.requested != BMSD_FG_OFF)
        swprintf(out, n, L"%hs · %ls", fi.name, Tr(id));  // "DLSS-G 310.9.16 · pozastaveno"
    else
        swprintf(out, n, L"%ls", Tr(id));
}
// Duvod od helperu / zalohy do napovedy radku "Stav" (ASCII cesky jako log; v anglictine odkaz na log)
static const char* FgStateNote() {
    PresentFgInfo fi;
    Present_FgInfo(&fi);
    if (fi.requested == BMSD_FG_OFF || !fi.presenter) return nullptr;
    const char* t = fi.fallback ? fi.fallbackReason
                    : (fi.state == BMSD_FGS_PAUSED && !fi.menuPause) || fi.state == BMSD_FGS_UNAVAILABLE ||
                              fi.state == BMSD_FGS_FAILED
                          ? fi.msg : nullptr;
    if (!t || !*t) return nullptr;
    return g_menuCfg.lang == 0 ? t : "see bms_dlss_helper.log / bms_dlss.log";
}
// Kdy adaptivni rezim generuje ([fg] Preference, v103e)
static int ChFgPref(Choice* o, int max) {
    const Choice k[] = {ChS(PREF_RESP, S_PREF_RESP, "responsiveness"), ChS(PREF_BAL, S_PREF_BAL, "balanced"),
                        ChS(PREF_SMOOTH, S_PREF_SMOOTH, "smooth"), ChS(PREF_ALWAYS, S_PREF_ALWAYS, "always"),
                        ChS(PREF_CUSTOM, S_PREF_CUSTOM, "custom"), ChS(PREF_STEADY, S_PREF_STEADY, "steady")};
    return MenuCopyChoices(k, Present_FgGetPref() == PREF_STEADY ? 6 : 5, o, max);  // steady jen z ini (pokus)
}
static void SetFgPref(int id, Src src) {
    Present_FgSetPref(id);
    static const char* const nm[] = {"always", "responsiveness", "balanced", "smooth", "custom", "steady"};
    Log("frame generation: adaptivni - kdy generovat %s%s", id >= 0 && id < PREF_COUNT ? nm[id] : "?", SrcTag(src));
}
static int ChFgPrefFps(Choice* o, int max) {  // v103f2: % cile
    const int n = MenuNumChoices(o, max, Present_FgGetPrefPct(), 20, 95, 75, S_NONE, nullptr);
    for (int i = 0; i < n; ++i)
        if (o[i].id > 0) swprintf(o[i].txt, 24, L"%d %%", o[i].id);
    return n;
}
static void SetFgPrefFps(int id, Src src) {
    Present_FgSetPrefPct(id);
    Log("frame generation: adaptivni - vlastni prah %d %% cile%s", Present_FgGetPrefPct(), SrcTag(src));
}
static bool VisFgPrefCustom() { return FgAdaptSel() && Present_FgGetPref() == PREF_CUSTOM; }
static bool VisFgFill() { return FgAdaptSel() && Present_FgGetPref() == PREF_ALWAYS; }  // hranice plati jen pro Vzdy
// Radek "Rozhodnuti": stav helperu a duvod ("jen skutecne · hra 132 FPS nad prahem 100")
static void FgPrefInfo(wchar_t* out, int n) {
    PresentFgPref pi;
    Present_FgPrefInfo(&pi);
    wchar_t a[16], b[16], c[16], r[96] = L"";
    auto num = [](float v, wchar_t* s) { swprintf(s, 16, L"%ld", lroundf(v)); };
    // v103f (S10): pri generovani bez 2 vzorku kG je FPS hry jen hruby odhad (spis nizko) - ne jako presne cislo
    const int why = pi.rough && (pi.why == BMSD_PFW_GEN || pi.why == BMSD_PFW_HOLD) ? -1000 : pi.why;
    switch (why) {
    case -1000:
        num(pi.base, a), num(pi.nOff, b);
        swprintf(r, 96, Tr(S_PFR_ROUGH), a, b);
        break;
    case BMSD_PFW_GEN:
        num(pi.base, a), num(pi.nOn, b), num(pi.gain, c);
        swprintf(r, 96, Tr(S_PFR_GEN), a, b, c);
        break;
    case BMSD_PFW_ABOVE:
        num(pi.base, a), num(pi.gen ? pi.nOff : pi.nOn, b);
        swprintf(r, 96, Tr(S_PFR_ABOVE), a, b);
        break;
    case BMSD_PFW_GAIN:
        num(pi.gain, a), num(pi.gainMin, b);
        swprintf(r, 96, Tr(S_PFR_GAIN), a, b);
        break;
    case BMSD_PFW_TARGET:
        num(pi.base, a);
        swprintf(r, 96, Tr(S_PFR_TARGET), a);
        break;
    case BMSD_PFW_HOLD:
        num(pi.base, a), num(pi.nOn, b), num(pi.nOff, c);
        swprintf(r, 96, Tr(S_PFR_HOLD), a, b, c);
        break;
    case BMSD_PFW_GUARD: swprintf(r, 96, L"%ls", Tr(S_PFR_GUARD)); break;
    case BMSD_PFW_USELESS: swprintf(r, 96, L"%ls", Tr(S_PFR_USELESS)); break;
    case BMSD_PFW_BLOCK: swprintf(r, 96, L"%ls", Tr(S_PFR_BLOCK)); break;
    case PFI_ALWAYS: swprintf(r, 96, L"%ls", Tr(S_PFR_ALWAYS)); break;
    case PFI_NOEST: swprintf(r, 96, L"%ls", Tr(S_PFR_NOEST)); break;
    default: swprintf(r, 96, L"%ls", Tr(S_PFR_WAIT)); break;
    }
    swprintf(out, n, L"%ls · %ls", Tr(pi.gen ? S_PFS_GEN : S_PFS_REAL), r);
}

// --- Vykon
static int ChLatency(Choice* out, int max) {
    const Choice k[] = {ChS(LAT_ADAPTIVE, S_LAT_ADAPTIVE, "1"), ChS(LAT_MEASURE, S_LAT_MEASURE, "2"),
                        ChS(LAT_OFF, S_OFF, "0")};
    return MenuCopyChoices(k, 3, out, max);
}
static int GetLatency() { return g_lat.mode; }
static void SetLatency(int id, Src src) { LatSetMode(id, src); }

// --- Pokrocile: DLSS
static int ChInput(Choice* o, int max) {
    const Choice k[] = {ChT(BMSD_INPUT_LDR, L"LDR", "0"), ChS(1, S_IN_HDR, "1"), ChS(2, S_IN_HDRAE, "2"),
                        ChS(BMSD_INPUT_HDR_ITM, S_IN_ITM, "3")};
    return MenuCopyChoices(k, 4, o, std::min(max, (int)BMSD_INPUT_COUNT));
}
static int ChDepth(Choice* o, int max) {
    const Choice k[] = {ChS(BMSD_DEPTH_RAW, S_DEPTH_RAW, "0"), ChT(1, L"min 3×3", "1"),
                        ChS(2, S_DEPTH_CONST, "2", CHF_NOSAVE)};
    return MenuCopyChoices(k, 3, o, std::min(max, (int)BMSD_DEPTH_COUNT));
}
static int ChPhases(Choice* o, int max) {
    static const int ids[] = {0, 8, 12, 16, 32};
    int all[8];
    const int k = MenuIdsWith(ids, 5, g_phasesSetting, all);
    int n = 0;
    for (int i = 0; i < k && n < max; ++i) {
        char b[16];
        wchar_t w[16];
        snprintf(b, sizeof b, "%d", all[i]);
        swprintf(w, 16, L"%d", all[i]);
        o[n++] = all[i] ? ChT(all[i], w, b) : ChS(0, S_PH_AUTO, "0");
    }
    return n;
}
static int ChPipeline(Choice* o, int max) {
    const Choice k[] = {ChT(1, L"pipeline", "1"), ChS(0, S_PIPE_SYNC, "0")};
    return MenuCopyChoices(k, 2, o, max);
}

// --- Pokrocile: ladeni (jen za behu)
static int ChJitterSign(Choice* o, int max) {
    const Choice k[] = {ChT(0, L"(+,+)", nullptr), ChT(1, L"(−,+)", nullptr), ChT(2, L"(+,−)", nullptr),
                        ChT(3, L"(−,−)", nullptr)};
    return MenuCopyChoices(k, 4, o, max);
}
static int ChBridge(Choice* o, int max) {
    const Choice k[] = {ChS(BMSD_MODE_DLAA, S_UPS_DLSS, nullptr), ChS(BMSD_MODE_COPY, S_BR_COPY, nullptr),
                        ChS(BMSD_MODE_MIRROR, S_BR_MIRROR, nullptr), ChS(BMSD_MODE_MVDEBUG, S_BR_MVDEBUG, nullptr),
                        ChS(BMSD_MODE_OFF, S_OFF, nullptr)};
    return MenuCopyChoices(k, 5, o, max);
}
static void MenuClose(const char* why);
static void MenuCaptureAction() {
    MenuClose("zachyt snimku");
    if (g_capState == CAP_IDLE) ArmCapture("menu");
}

// --- Pokrocile: menu
static int ChMenuPos(Choice* o, int max) {
    const Choice k[] = {ChS(0, S_POS_CENTER, "center"), ChS(1, S_POS_LEFT, "left"), ChS(2, S_POS_RIGHT, "right"),
                        ChS(3, S_POS_TL, "topleft"), ChS(4, S_POS_TR, "topright")};
    return MenuCopyChoices(k, 5, o, max);
}
static int ChLang(Choice* o, int max) {
    const Choice k[] = {ChS(0, S_AUTO, "auto"), ChS(1, S_LANG_CS, "cs"), ChS(2, S_LANG_EN, "en")};
    return MenuCopyChoices(k, 3, o, max);
}
static int ChScale(Choice* o, int max) {
    static const int ids[] = {50, 60, 75, 90, 100, 110, 125, 150};
    int all[10];
    const int k = MenuIdsWith(ids, 8, (int)lroundf(g_menuCfg.scale * 100.0f), all);
    int n = 0;
    for (int i = 0; i < k && n < max; ++i) {
        Choice c = ChS(all[i], S_NONE, nullptr);
        swprintf(c.txt, 24, L"%d %%", all[i]);
        MenuFmtNum((float)all[i] / 100.0f, 2, nullptr, 0, c.iniBuf, 16);
        o[n++] = c;
    }
    return n;
}

// "Zapnuto (Alt+F8)" (uzivatel 2026-11-19): totez co Alt+F8 - most vypnuty <-> DLSS / FSR (s DebugCycle=1 je ladici
// rezim mostu taky "zapnuto"). Neuklada se, stejne jako Alt+F8 a volba Upscaler "vypnuto".
static int GetModEnabled() { return Interop_GetMode() != BMSD_MODE_OFF ? 1 : 0; }
static void SetModEnabled(int id, Src src) { SetBridgeMode(id ? BMSD_MODE_DLAA : BMSD_MODE_OFF, src); }
static bool VisNever() { return false; }

// Polozky. Poradi = poradi v menu. Napoveda v menu_text.inl.
static const MenuItem kItemsImage[] = {
    MI(IK_TOGGLE, S_IT_ENABLED, S_HELP_ENABLED, GetModEnabled, SetModEnabled).Def(1),
    MI(IK_CHOICE, S_IT_UPSCALER, S_HELP_UPSCALER, GetUpscaler, SetUpscaler, ChUpscaler)
        .Ini("upscale", "Upscaler", "auto").Def(UPS_AUTO).Heavy().Osd(OSD_UPSCALER).Note(UpscalerNote),
    MI(IK_CHOICE, S_IT_OUTSCALE, S_HELP_OUTSCALE, GetOutScale, SetOutScaleId, ChOutScale)
        .Ini("upscale", "OutputScale", "1.0").Def(100).Heavy().Vis(VisDlss).App(AppNotEco).Key("Alt+F3")
        .Osd(OSD_OUTSCALE),  // supersampling jen s DLSS (FSR bezi vzdy 1.0, SyncOutScale)
    MI(IK_CHOICE, S_IT_PRESET, S_HELP_PRESET, []() { return (int)g_preset; },
       [](int id, Src s) { SetPreset((unsigned)id, s); }, ChPreset)
        .Ini("dlaa", "Preset", "0").Def(0).Heavy().Vis(VisDlss).Key("Alt+F9").Osd(OSD_PRESET),
    MI(IK_CHOICE, S_IT_SHARP, S_HELP_SHARP, []() { return (int)lroundf(g_sharpness * 10.0f); },
       [](int id, Src s) { SetSharpness((float)id / 10.0f, s); }, ChSharp)
        .Ini("dlaa", "Sharpness", "0.0").Def(0).Key("Alt+Home").Osd(OSD_SHARP),
    MI(IK_CHOICE, S_IT_MIPBIAS, S_HELP_MIPBIAS, GetMipBias,
       [](int id, Src s) { SetMipBias(id > 0 ? -(float)id / 100.0f : 0.0f, s); }, ChMipBias)
        .Ini("dlaa", "MipBias", "-0.5").Def(50).Vis([]() { return !g_pathB; }).Key("Alt+Del").Osd(OSD_MIPBIAS),
    MI(IK_CHOICE, S_IT_MIPBIAS_B, S_HELP_MIPBIAS_B, []() { return g_mipBIdx; }, SetMipBiasB, ChMipBiasB)
        .Def(0).Vis([]() { return g_pathB; }).Key("Alt+Del").Osd(OSD_MIPBIAS_B),
    MI(IK_TOGGLE, S_IT_OBJMV, S_HELP_OBJMV, GetObjMv, SetObjMv)
        .Ini("mv", "ObjectMV", "1").Def(1).Key("Alt+Backspace").Osd(OSD_OBJMV),
};
static const MenuItem kItemsEco[] = {
    MI(IK_INFO, S_IT_ECO, S_HELP_ECO).Info(EcoInfo),
    MI(IK_TOGGLE, S_IT_LIGHTSB, S_HELP_LIGHTSB, []() { return g_cfg.lightsB ? 1 : 0; },
       [](int id, Src s) { SetLightsB(id != 0, s); })
        .Ini("upscale", "LightsB", "1").Def(1)
        .Note([]() { return g_cfg.lightsB && !o_CreatePS ? S_NOTE_RESTART : S_NONE; }),
};
static const MenuItem kItemsFg[] = {
    MI(IK_CHOICE, S_IT_FG_MODE, S_HELP_FG, GetFgMode, SetFgMode, ChFgMode)
        .Ini("fg", "Mode", "0").Def(FG_OFF).Heavy().Osd(OSD_FG).Note(FgModeNote),
    MI(IK_CHOICE, S_IT_FG_MULT, S_HELP_FG_MULT, Present_FgGetMult, SetFgMult, ChFgMult)
        .Ini("fg", "Multiplier", "2").Def(2).Note(FgMultNote).Vis([]() { return !FgAdaptSel(); }),
    MI(IK_CHOICE, S_IT_FG_TARGET, S_HELP_FG_TARGET, Present_FgGetTargetFps, SetFgTarget, ChFgTarget)
        .Ini("fg", "TargetFps", "0").Def(0).Vis([]() { return !FgAdaptSel(); }),
    // adaptivni FSR: stejny klic TargetFps jako "Strop FPS", jen jiny popis
    MI(IK_CHOICE, S_IT_FG_TARGET_AD, S_HELP_FG_TARGET_AD, Present_FgGetTargetFps, SetFgTarget, ChFgTarget)
        .Ini("fg", "TargetFps", "0").Def(0).Vis(FgAdaptSel),
    MI(IK_CHOICE, S_IT_FG_REALCAP, S_HELP_FG_REALCAP, Present_FgGetRealCap, SetFgRealCap, ChFgRealCap)
        .Ini("fg", "RealCap", "0").Def(0).Vis(FgAdaptSel),
    MI(IK_CHOICE, S_IT_FG_PREF, S_HELP_FG_PREF, Present_FgGetPref, SetFgPref, ChFgPref)
        .Ini("fg", "Preference", "balanced").Def(PREF_BAL).Vis(FgAdaptSel),
    MI(IK_CHOICE, S_IT_FG_PREFFPS, S_HELP_FG_PREFFPS, Present_FgGetPrefPct, SetFgPrefFps, ChFgPrefFps)
        .Ini("fg", "PreferencePct", "75").Def(75).Vis(VisFgPrefCustom),
    MI(IK_CHOICE, S_IT_FG_FILL, S_HELP_FG_FILL, Present_FgGetFillBelow, SetFgFill, ChFgFill)
        .Ini("fg", "FillBelow", "0").Def(0).Vis(VisFgFill),
    MI(IK_TOGGLE, S_IT_FG_MENUPAUSE, S_HELP_FG_MENUPAUSE, []() { return Present_FgGetPauseInMenu() ? 1 : 0; },
       [](int id, Src s) {
           Present_FgSetPauseInMenu(id != 0);
           Log("frame generation: pozastavit v menu %s%s", id ? "zapnuto" : "vypnuto", SrcTag(s));
       })
        .Ini("fg", "PauseInMenu", "0").Def(0),
    MI(IK_INFO, S_IT_FG_STATE, S_HELP_FG_STATE).Info(FgStateInfo).NoteTxt(FgStateNote).Note(FgAdaptNote),
    MI(IK_INFO, S_IT_FG_PREFSTATE, S_HELP_FG_PREFSTATE).Info(FgPrefInfo).Vis(FgAdaptSel),
};
static void MenuSetFpsCounter(int id, Src s) {
    g_menuCfg.fpsCounter = id != 0;
    Log("menu: ukazatel FPS %s%s", id ? "zapnuto" : "vypnuto", SrcTag(s));
}
static void MenuSetOverlayVersions(int id, Src s) {
    g_menuCfg.overlayVersions = id != 0;
    Log("menu: radek s verzemi %s%s", id ? "zapnuto" : "vypnuto", SrcTag(s));
}
static void MenuSetDiag(int id, Src s) {
    g_menuCfg.diag = id != 0;
    Log("menu: diagnostika plynulosti %s%s", id ? "zapnuto" : "vypnuto", SrcTag(s));
}
static bool VisDiag() { return g_menuCfg.diag; }
// Hlaseni chyby: logy (i minuleho spusteni a uschovane logy padu), ini a dxvk.conf do slozky
// bms_dlss_report_<datum_cas> vedle logu. Kopiruje vlastni vlakno (ne vlakno hry); vysledek OSD.
static wchar_t g_reportName[64];
static DWORD WINAPI MenuReportThread(LPVOID) {
    const std::wstring dir = std::wstring(g_dir) + L"\\" + g_reportName;
    int n = 0;
    if (CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
        const wchar_t* files[] = {L"bms_dlss.log", L"bms_dlss.log.prev", L"bms_dlss_helper.log",
                                  L"bms_dlss_helper.log.prev", L"bms_dlss.ini", L"dxvk.conf"};
        for (const wchar_t* f : files)
            n += CopyFileW((std::wstring(g_dir) + L"\\" + f).c_str(), (dir + L"\\" + f).c_str(), FALSE) ? 1 : 0;
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW((std::wstring(g_dir) + L"\\bms_dlss.crash-*.log").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do n += CopyFileW((std::wstring(g_dir) + L"\\" + fd.cFileName).c_str(), (dir + L"\\" + fd.cFileName).c_str(),
                              FALSE) ? 1 : 0;
            while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
    }
    Log("menu: hlaseni chyby - %d souboru do %ls", n, dir.c_str());
    g_reportDone.store(n > 0 ? OSD_REPORT : OSD_REPORT_FAIL);  // OSD az vlakno Present (FailNotesFrameEnd)
    return 0;
}
static void MenuReportAction() {
    MenuClose("hlaseni chyby");
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(g_reportName, 64, L"bms_dlss_report_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
    Log("menu: ukladam hlaseni chyby do %ls", g_reportName);
    if (HANDLE t = CreateThread(nullptr, 0, MenuReportThread, nullptr, 0, nullptr)) CloseHandle(t);
}

static const MenuItem kItemsPerf[] = {
    MI(IK_CHOICE, S_IT_LATENCY, S_HELP_LATENCY, GetLatency, SetLatency, ChLatency)
        .Ini("latency", "Mode", "1").Def(LAT_ADAPTIVE).Key("Alt+F12").Osd(OSD_LATENCY),
    MI(IK_TOGGLE, S_IT_FPSCOUNTER, S_HELP_FPSCOUNTER, []() { return g_menuCfg.fpsCounter ? 1 : 0; }, MenuSetFpsCounter)
        .Ini("menu", "FpsCounter", "0").Def(0),
    MI(IK_TOGGLE, S_IT_OVERLAYVER, S_HELP_OVERLAYVER, []() { return g_menuCfg.overlayVersions ? 1 : 0; },
       MenuSetOverlayVersions)
        .Ini("menu", "OverlayVersions", "0").Def(0),
    MI(IK_TOGGLE, S_IT_DIAG, S_HELP_DIAG, []() { return g_menuCfg.diag ? 1 : 0; }, MenuSetDiag)
        .Ini("menu", "DiagOverlay", "0").Def(0),
    MI(IK_INFO, S_IT_DG_GAME, S_HELP_DG).Info([](wchar_t* o, int n) { swprintf(o, n, L"%ls", Tr(S_DG_GAME_TXT)); }).Vis(VisDiag),
    MI(IK_INFO, S_IT_DG_MON, S_HELP_DG).Info([](wchar_t* o, int n) { swprintf(o, n, L"%ls", Tr(S_DG_MON_TXT)); }).Vis(VisDiag),
    MI(IK_INFO, S_IT_DG_SKIP, S_HELP_DG).Info([](wchar_t* o, int n) { swprintf(o, n, L"%ls", Tr(S_DG_SKIP_TXT)); }).Vis(VisDiag),
    MI(IK_INFO, S_IT_DG_FG, S_HELP_DG).Info([](wchar_t* o, int n) { swprintf(o, n, L"%ls", Tr(S_DG_FG_TXT)); }).Vis(VisDiag),
    MI(IK_INFO, S_IT_DG_OUT, S_HELP_DG).Info([](wchar_t* o, int n) { swprintf(o, n, L"%ls", Tr(S_DG_OUT_TXT)); }).Vis(VisDiag),
    MI(IK_INFO, S_IT_DG_GRAPH, S_HELP_DG).Info([](wchar_t* o, int n) { swprintf(o, n, L"%ls", Tr(S_DG_GRAPH_TXT)); }).Vis(VisDiag),
    MI(IK_ACTION, S_IT_REPORT, S_HELP_REPORT).Act(MenuReportAction),
};
static void MenuResetAdvanced();
// Vstup DLSS a filtr zmenseni (uzivatel 2026-11-19): pevne 1 = linearni HDR a Catmull-Rom, v menu nejsou (VisNever).
// Polozky zustavaji skryte: "Vychozi (doporucene)" (MenuResetAdvanced, i skryte polozky stranky) je vraci na 1 a ulozi,
// OSD ladicich klaves (DebugKeys=1: Alt+Ins, Alt+F1) z nich bere nazev a hodnotu. Klic v ini zustava pro pokrocile.
static const MenuItem kItemsDlss[] = {
    MI(IK_ACTION, S_IT_RESETADV, S_HELP_RESETADV).Act(MenuResetAdvanced),
    MI(IK_CHOICE, S_IT_INPUT, S_HELP_INPUT, []() { return (int)EffInputMode(); },
       [](int id, Src s) { SetInputMode((unsigned)id, s); }, ChInput)
        .Ini("dlaa", "InputMode", "1").Def(BMSD_INPUT_HDR).Heavy().Vis(VisNever).Key("Alt+Ins").Osd(OSD_INPUT),
    MI(IK_CHOICE, S_IT_DOWN, S_HELP_DOWN, []() { return (int)g_downFilter; },
       [](int id, Src s) { SetDownFilter((unsigned)id, s); }, ChDown)
        .Ini("upscale", "DownsampleFilter", "1").Def(BMSD_DOWN_CATMULLROM).Vis(VisNever).Key("Alt+F1").Osd(OSD_DOWN),
    MI(IK_CHOICE, S_IT_DEPTH, S_HELP_DEPTH, []() { return (int)g_depthMode; },
       [](int id, Src s) { SetDepthMode((unsigned)id, s); }, ChDepth)
        .Ini("dlaa", "DepthMode", "0").Def(BMSD_DEPTH_RAW).App(AppNotEco).Vis(VisDlss).Key("Alt+F5").Osd(OSD_DEPTH),
    MI(IK_CHOICE, S_IT_PHASES, S_HELP_PHASES, []() { return g_phasesSetting; }, SetPhases, ChPhases)
        .Ini("jitter", "Phases", "8").Def(8).Key("Alt+F2").Osd(OSD_PHASES),
    MI(IK_CHOICE, S_IT_PIPELINE, S_HELP_PIPELINE, []() { return Interop_GetPipeline() ? 1 : 0; },
       [](int id, Src s) { SetPipeline(id != 0, s); }, ChPipeline)
        .Ini("bridge", "Pipeline", "1").Def(1).Key("Alt+ScrollLock").Osd(OSD_PIPELINE),
};
static bool AppJitterSign(StrId* why) {
    if (DlssRunning()) return true;
    *why = S_WHY_DLAA;
    return false;
}
static bool AppJitterOn(StrId* why) {
    if (!DlssRunning()) return true;
    *why = S_WHY_FORCED;
    return false;
}
static bool AppCapture(StrId* why) {
    if (g_capState == CAP_IDLE) return true;
    *why = S_WHY_CAPBUSY;
    return false;
}
static const MenuItem kItemsDebug[] = {
    MI(IK_CHOICE, S_IT_JSIGN, S_HELP_JSIGN, []() { return g_jitterSignIdx; }, SetJitterSign, ChJitterSign)
        .Def(0).Key("Alt+F11").Osd(OSD_JSIGN).App(AppJitterSign),
    MI(IK_TOGGLE, S_IT_JITTER, S_HELP_JITTER, []() { return g_jitterOn.load() ? 1 : 0; },
       [](int id, Src s) { SetJitterOn(id != 0, s); })
        .Def(0).Key("Alt+F11").Osd(OSD_JITTER).App(AppJitterOn),
    MI(IK_CHOICE, S_IT_BRIDGE, S_HELP_BRIDGE, []() { return (int)Interop_GetMode(); },
       [](int id, Src s) { SetBridgeMode((uint32_t)id, s); }, ChBridge)
        .Def(BMSD_MODE_DLAA).Key("Alt+F8"),
    MI(IK_ACTION, S_IT_CAPTURE, S_HELP_CAPTURE).Act(MenuCaptureAction).Key("Alt+F10").App(AppCapture),
};
static void MenuSetLang(int id, Src s) {
    MenuSetLangSetting(id);
    Log("menu: jazyk %s%s%s", id == 1 ? "cs" : id == 2 ? "en" : "auto", id ? "" : g_menuCfg.lang ? " (en)" : " (cs)",
        SrcTag(s));
}
static void MenuSetOsd(int id, Src s) {
    g_menuCfg.osd = id != 0;
    Log("menu: OSD %s%s", id ? "zapnuto" : "vypnuto", SrcTag(s));
}
static void MenuSetScale(int id, Src s) {
    g_menuCfg.scale = std::min(1.5f, std::max(0.5f, (float)id / 100.0f));
    Log("menu: velikost textu %.2f%s", g_menuCfg.scale, SrcTag(s));
}
static void MenuSetPos(int id, Src s) {
    g_menuCfg.pos = id >= 0 && id <= 4 ? id : 0;
    Log("menu: poloha %d%s", g_menuCfg.pos, SrcTag(s));
}
static void MenuSetTestKeys(int id, Src s) {
    g_cfg.testKeys = id != 0;
    g_testKeysOn.store(g_cfg.testKeys, std::memory_order_relaxed);
    Log("klavesy: zkratky pro testovani (Alt+F9 FG, Alt+F10 upscaler) %s%s", id ? "zapnuto" : "vypnuto", SrcTag(s));
}
static const MenuItem kItemsKeys[] = {
    MI(IK_TOGGLE, S_IT_TESTKEYS, S_HELP_TESTKEYS, []() { return g_testKeysOn.load() ? 1 : 0; }, MenuSetTestKeys)
        .Ini("keys", "TestKeys", "0").Def(0),
};
static const MenuItem kItemsMenu[] = {
    MI(IK_CHOICE, S_IT_LANG, S_HELP_LANG, []() { return g_menuCfg.langSetting; }, MenuSetLang, ChLang)
        .Ini("menu", "Language", "auto").Def(0),
    MI(IK_TOGGLE, S_IT_OSD, S_HELP_OSD, []() { return g_menuCfg.osd ? 1 : 0; }, MenuSetOsd)
        .Ini("menu", "Osd", "1").Def(1),
    MI(IK_CHOICE, S_IT_SCALE, S_HELP_SCALE, []() { return (int)lroundf(g_menuCfg.scale * 100.0f); }, MenuSetScale,
       ChScale)
        .Ini("menu", "Scale", "1.0").Def(100),
    MI(IK_CHOICE, S_IT_MENUPOS, S_HELP_MENUPOS, []() { return g_menuCfg.pos; }, MenuSetPos, ChMenuPos)
        .Ini("menu", "Position", "center").Def(0),
};
#define MENU_N(a) (int)(sizeof a / sizeof a[0])
static const MenuSection kMenuSections[] = {
    {S_SEC_IMAGE, kItemsImage, MENU_N(kItemsImage), nullptr, PAGE_COMMON},
    {S_SEC_ECO, kItemsEco, MENU_N(kItemsEco), nullptr, PAGE_COMMON},
    {S_SEC_FG, kItemsFg, MENU_N(kItemsFg), nullptr, PAGE_COMMON},
    {S_SEC_PERF, kItemsPerf, MENU_N(kItemsPerf), nullptr, PAGE_COMMON},
    {S_SEC_DLSS, kItemsDlss, MENU_N(kItemsDlss), nullptr, PAGE_ADVANCED},
    {S_SEC_DEBUG, kItemsDebug, MENU_N(kItemsDebug), []() { return g_cfg.debugKeys; }, PAGE_ADVANCED},  // jen [keys] DebugKeys=1
    {S_SEC_MENU, kItemsMenu, MENU_N(kItemsMenu), nullptr, PAGE_ADVANCED},
    {S_SEC_KEYS, kItemsKeys, MENU_N(kItemsKeys), nullptr, PAGE_ADVANCED},
};
static const int kMenuMaxItems = 64;  // pocet vsech polozek nejvys (MenuItemIndex)
static_assert(MENU_N(kItemsImage) + MENU_N(kItemsEco) + MENU_N(kItemsFg) + MENU_N(kItemsPerf) + MENU_N(kItemsDlss) +
                      MENU_N(kItemsDebug) + MENU_N(kItemsMenu) + MENU_N(kItemsKeys) <=
                  kMenuMaxItems,
              "kMenuMaxItems");

static int MenuItemIndex(const MenuItem* it) {
    int base = 0;
    for (const MenuSection& s : kMenuSections) {
        if (std::less_equal<const MenuItem*>()(s.items, it) && std::less<const MenuItem*>()(it, s.items + s.n))
            return base + (int)(it - s.items);
        base += s.n;
    }
    return -1;
}

static const int kMenuMaxChoices = 16;
static int MenuChoices(const MenuItem& it, Choice* out) {
    if (it.kind == IK_TOGGLE) {
        out[0] = ChS(0, S_OFF, "0");
        out[1] = ChS(1, S_ON, "1");
        return 2;
    }
    return it.choices ? it.choices(out, kMenuMaxChoices) : 0;
}
static bool MenuApplies(const MenuItem& it, StrId* why) {
    *why = S_NONE;
    return !it.applies || it.applies(why);
}
static bool MenuEditable(const MenuItem& it) {
    StrId why;
    return (it.kind == IK_CHOICE || it.kind == IK_TOGGLE) && it.get && it.set && MenuApplies(it, &why);
}

// ---------------------------------------------------------------------------
// Stav menu (jen vlakno Present)
// ---------------------------------------------------------------------------
struct MenuLine {
    const MenuSection* sec;  // nadpis sekce (item == nullptr), nebo polozka
    const MenuItem* item;
};
static struct MenuState {
    bool open;
    int page;                 // MenuPage
    int selPage[PAGE_COUNT];  // vybrana polozka (poradi mezi viditelnymi polozkami) na kazde strance
    int scroll;               // prvni viditelny radek seznamu (kdyz se cely nevejde)
    bool dirty;               // prestavet geometrii
    uint64_t frames;          // pocitadlo snimku (MenuFrameInput)
    uint64_t builtFrame;
    UINT builtW, builtH;
    bool wasForeground;
    DWORD lastFgTid;
    bool pollKeyWas;
    bool rawChecked;
    bool fallbackLogged;
    unsigned opens;
    int drawFails;            // snimku po sobe, kdy se otevrene menu nepodarilo nakreslit
    // odklad HEAVY: zobrazena hodnota pendId, akce Set* az v pendDue (nebo Enter, zavreni, zmena jine HEAVY polozky)
    const MenuItem* pendItem;
    int pendId;
    ULONGLONG pendDue;
    // ukladani: id volby pri otevreni a priznak "menilo menu" (index = MenuItemIndex)
    int openId[kMenuMaxItems];
    bool touched[kMenuMaxItems];
    // FPS (vlastni mereni QPC, prumer za 0.5 s)
    LONGLONG fpsT0;
    int fpsN;
    float fps, fpsMs;
    int saveFailsSeen;
    // test ([menu] TestOpenSec / TestKeys / TestShot)
    ULONGLONG firstMs, testNextMs;
    bool testStarted, testOpened, testDone;
    size_t testPos;
    std::vector<std::pair<int, int>> testSteps;  // (udalost nebo -1 = snimek, -2 = cekani; ms, -1 = soucasne)
    int shotPending, shotIndex;
    OvList list;
    std::vector<MenuLine> lines;
} g_menu;

static int& MenuSel() { return g_menu.selPage[g_menu.page]; }
static int MenuItemCount() {
    int n = 0;
    for (const MenuLine& l : g_menu.lines)
        if (l.item) ++n;
    return n;
}
static void MenuCollectLines() {
    g_menu.lines.clear();
    for (const MenuSection& s : kMenuSections) {
        if (s.page != g_menu.page || (s.visible && !s.visible())) continue;
        bool head = false;
        for (int i = 0; i < s.n; ++i) {
            const MenuItem& it = s.items[i];
            if (it.visible && !it.visible()) continue;
            if (!head) {
                head = true;
                g_menu.lines.push_back({&s, nullptr});
            }
            g_menu.lines.push_back({&s, &it});
        }
    }
    const int n = MenuItemCount();
    int& sel = MenuSel();
    sel = n ? std::min(std::max(sel, 0), n - 1) : 0;
}
static const MenuItem* MenuSelected() {
    int k = 0;
    for (const MenuLine& l : g_menu.lines)
        if (l.item && k++ == MenuSel()) return l.item;
    return nullptr;
}
// Seznam radku znovu podle aktualni viditelnosti (F4 = cast A/B, zmena upscaleru...), vyber zustane na stejne
// polozce. Vola se kazdy snimek pred frontou klaves a po kazde klavese, aby sipka nikdy nezmenila polozku podle
// zastaraleho seznamu (oprava D10). Pri zmene struktury dirty = prestavet geometrii.
static void MenuRefreshLines() {
    static std::vector<MenuLine> prev;
    prev = g_menu.lines;  // prirazeni pouzije existujici kapacitu (bez alokace kazdy snimek)
    const MenuItem* selIt = MenuSelected();
    MenuCollectLines();
    bool same = prev.size() == g_menu.lines.size();
    for (size_t i = 0; same && i < prev.size(); ++i)
        same = prev[i].sec == g_menu.lines[i].sec && prev[i].item == g_menu.lines[i].item;
    if (same) return;
    if (selIt) {
        int k = 0;
        for (const MenuLine& l : g_menu.lines)
            if (l.item) {
                if (l.item == selIt) {
                    MenuSel() = k;
                    break;
                }
                ++k;
            }
    }
    g_menu.dirty = true;
}
// Zobrazena hodnota polozky: cekajici (odlozena) volba, jinak skutecny stav
static int MenuCur(const MenuItem& it) {
    if (g_menu.pendItem == &it) return g_menu.pendId;
    return it.get ? it.get() : -1;
}

// ---------------------------------------------------------------------------
// Ukladani do ini (docs\navrh-menu.md kap. 7). Pri zavreni menu se do bms_dlss.ini zapisi jen polozky, ktere menu
// zmenilo (touched) a jejichz hodnota je jina nez pri otevreni; ladici polozky (bez iniSec) a volby CHF_NOSAVE se
// nezapisuji. Zapis dela pracovni vlakno (Present na disk neceka): kopie ini -> WritePrivateProfileString do kopie
// -> MoveFileEx pres puvodni soubor. Nahrada je atomicka, takze pad nebo ukonceni hry behem zapisu necha bud stary,
// nebo novy soubor, nikdy rozepsany. WritePrivateProfileString meni jen dany radek (komentare a poradi zustanou),
// chybejici klic prida na konec sekce. Alt klavesy do ini nezapisuji (rozhodnuti uzivatele Q1).
// ---------------------------------------------------------------------------
struct IniWrite {
    std::string sec, key, val, def;
};
static CRITICAL_SECTION g_saveCs;
static bool g_saveCsInit;
static std::vector<IniWrite> g_saveQueue, g_saveInFlight;  // pod g_saveCs
static HANDLE g_saveEvent, g_saveThread;
static std::atomic<int> g_saveFails{0};
static char g_saveErr[160];  // posledni chyba (pod g_saveCs)
// radek "Ulozeni selhalo" v menu: true po selhani, false po dalsim uspesnem ulozeni (oprava D8; g_saveFails dal
// jen pocita selhani pro OSD)
static std::atomic<bool> g_saveFailShown{false};

static void MenuSaveInit() {
    if (g_saveCsInit) return;
    InitializeCriticalSection(&g_saveCs);
    g_saveCsInit = true;
}

static std::wstring MenuWide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// Zapis do bms_dlss.ini. Vraci pocet zapsanych klicu (0 = vse uz tak bylo), -1 = chyba (err).
static int MenuIniApply(const std::vector<IniWrite>& ws, std::string* done, char* err, size_t nerr) {
    const std::wstring ini = IniPath(), tmp = ini + L".menu.tmp";
    // posledni zapis tehoz klice plati (fronta muze mit vic zavreni menu za sebou)
    std::vector<const IniWrite*> todo;
    for (size_t i = 0; i < ws.size(); ++i) {
        bool later = false;
        for (size_t j = i + 1; j < ws.size(); ++j)
            later |= !_stricmp(ws[i].sec.c_str(), ws[j].sec.c_str()) && !_stricmp(ws[i].key.c_str(), ws[j].key.c_str());
        if (later) continue;
        wchar_t buf[256];
        GetPrivateProfileStringW(MenuWide(ws[i].sec).c_str(), MenuWide(ws[i].key).c_str(), L"\x1F", buf, 256,
                                 ini.c_str());
        const bool missing = !wcscmp(buf, L"\x1F");
        if (missing ? !_stricmp(ws[i].val.c_str(), ws[i].def.c_str()) : !_wcsicmp(buf, MenuWide(ws[i].val).c_str()))
            continue;  // v ini uz je (chybejici klic = vychozi hodnota)
        todo.push_back(&ws[i]);
    }
    if (todo.empty()) return 0;
    const DWORD attr = GetFileAttributesW(ini.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) {
        snprintf(err, nerr, "bms_dlss.ini je jen pro cteni");
        return -1;
    }
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileW(ini.c_str(), tmp.c_str(), FALSE)) {
            snprintf(err, nerr, "kopie bms_dlss.ini selhala (chyba %lu)", GetLastError());
            return -1;
        }
    } else {
        DeleteFileW(tmp.c_str());
    }
    std::string list;
    for (const IniWrite* w : todo) {
        if (!WritePrivateProfileStringW(MenuWide(w->sec).c_str(), MenuWide(w->key).c_str(), MenuWide(w->val).c_str(),
                                        tmp.c_str())) {
            snprintf(err, nerr, "zapis [%s] %s selhal (chyba %lu)", w->sec.c_str(), w->key.c_str(), GetLastError());
            DeleteFileW(tmp.c_str());
            return -1;
        }
        list += (list.empty() ? "[" : ", [") + w->sec + "] " + w->key + "=" + w->val;
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, tmp.c_str());
    // Data kopie na disk pred prejmenovanim (oprava D6): MOVEFILE_WRITE_THROUGH plati jen pro metadata, po vypadku
    // proudu by jinak mohlo zustat ini nulove delky. Selhani flush neni duvod neulozit (jen log).
    {
        const HANDLE fh = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE || !FlushFileBuffers(fh))
            Log("WARN: menu: FlushFileBuffers bms_dlss.ini.menu.tmp selhal (chyba %lu)", GetLastError());
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    }
    DWORD e = 0;
    for (int i = 0; i < (g_procExiting ? 1 : 10); ++i) {  // editor nebo antivirus muze soubor kratce drzet
        if (i) Sleep(50);
        if (MoveFileExW(tmp.c_str(), ini.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (done) *done = list;
            return (int)todo.size();
        }
        e = GetLastError();
    }
    DeleteFileW(tmp.c_str());
    snprintf(err, nerr, "prepsani bms_dlss.ini selhalo (chyba %lu)", e);
    return -1;
}

static DWORD WINAPI MenuSaveThread(LPVOID) {
    // Neuspesne ulozeni (soubor drzi antivirus, synchronizace, editor): davka se vrati do fronty a zkusi se znovu za
    // 2 s (nejvys 5x po sobe), pak pri dalsim ulozeni z menu a pri konci hry. Soubor jen pro cteni se neopakuje.
    int retries = 0;
    for (;;) {
        const bool timedRetry = retries > 0 && retries <= 5;
        if (WaitForSingleObject(g_saveEvent, timedRetry ? 2000 : INFINITE) == WAIT_TIMEOUT)
            Log("menu: ukladani bms_dlss.ini - opakovani %d", retries);
        std::vector<IniWrite> batch;
        EnterCriticalSection(&g_saveCs);
        g_saveInFlight.insert(g_saveInFlight.end(), g_saveQueue.begin(), g_saveQueue.end());
        g_saveQueue.clear();
        batch = g_saveInFlight;
        LeaveCriticalSection(&g_saveCs);
        if (batch.empty()) {
            retries = 0;
            continue;
        }
        std::string done;
        char err[160] = "";
        const int r = MenuIniApply(batch, &done, err, sizeof err);
        if (r > 0)
            Log("menu: ulozeno do bms_dlss.ini: %s", done.c_str());
        else if (r == 0)
            Log("menu: bms_dlss.ini uz tyto hodnoty ma, nic se nezapsalo");
        else
            Log("WARN: menu: ulozeni nastaveni selhalo: %s", err);
        bool keep = false;
        if (r < 0) {
            const DWORD at = GetFileAttributesW((std::wstring(ProxyDir()) + L"\\bms_dlss.ini").c_str());
            keep = at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_READONLY);
        }
        EnterCriticalSection(&g_saveCs);
        // zpet do fronty pred pripadne novejsi zmeny (ty se aplikuji az po nich, takze vyhraji)
        if (keep) g_saveQueue.insert(g_saveQueue.begin(), g_saveInFlight.begin(), g_saveInFlight.end());
        g_saveInFlight.clear();
        if (r < 0) strncpy(g_saveErr, err, sizeof g_saveErr - 1);
        LeaveCriticalSection(&g_saveCs);
        g_saveFailShown.store(r < 0, std::memory_order_relaxed);
        if (r < 0) g_saveFails.fetch_add(1);
        retries = keep ? retries + 1 : 0;
    }
}

// Zmeny z menu -> zapisy (usePending: cekajici HEAVY hodnota misto skutecne, pri ukonceni hry)
static void MenuCollectWrites(std::vector<IniWrite>& out, bool usePending) {
    MenuState& M = g_menu;
    for (const MenuSection& s : kMenuSections)
        for (int i = 0; i < s.n; ++i) {
            const MenuItem& it = s.items[i];
            const int idx = MenuItemIndex(&it);
            if (idx < 0 || !M.touched[idx] || !it.get) continue;
            const int cur = usePending ? MenuCur(it) : it.get();
            if (cur == M.openId[idx]) continue;
            if (!it.iniSec || !it.iniKey) continue;  // ladici polozka: jen za behu
            Choice ch[kMenuMaxChoices];
            const int n = MenuChoices(it, ch);
            int k = 0;
            while (k < n && ch[k].id != cur) ++k;
            if (k == n || (ch[k].flags & (CHF_NOSAVE | CHF_SOON)) || !*ChIni(ch[k])) {
                Log("menu: [%s] %s se neuklada (volba plati jen do restartu)", it.iniSec, it.iniKey);
                continue;
            }
            out.push_back({it.iniSec, it.iniKey, ChIni(ch[k]), it.iniDef ? it.iniDef : ""});
        }
    // v103f (D2): "Vlastni: generovat pod" se uklada do PreferencePct; cislo v Preference (napr. Preference=70) by ho po
    // restartu prepsalo -> s PreferencePct zapsat i Preference=custom
    bool fps = false, pref = false;
    for (const IniWrite& w : out) {
        if (w.key == "PreferencePct") fps = true;
        if (w.key == "Preference") pref = true;
    }
    if (fps && !pref && Present_FgGetPref() == PREF_CUSTOM) out.push_back({"fg", "Preference", "custom", "balanced"});
}

static void MenuSaveChanges() {
    std::vector<IniWrite> w;
    MenuCollectWrites(w, false);
    memset(g_menu.touched, 0, sizeof g_menu.touched);
    if (w.empty() || !g_saveCsInit) return;
    EnterCriticalSection(&g_saveCs);
    g_saveQueue.insert(g_saveQueue.end(), w.begin(), w.end());
    LeaveCriticalSection(&g_saveCs);
    if (!g_saveEvent) g_saveEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_saveEvent && !g_saveThread) g_saveThread = CreateThread(nullptr, 0, MenuSaveThread, nullptr, 0, nullptr);
    if (g_saveEvent && g_saveThread) {
        SetEvent(g_saveEvent);
        return;
    }
    // bez vlakna (nemelo by nastat): zapsat hned
    std::vector<IniWrite> batch;
    EnterCriticalSection(&g_saveCs);
    batch.swap(g_saveQueue);
    LeaveCriticalSection(&g_saveCs);
    std::string done;
    char err[160] = "";
    if (MenuIniApply(batch, &done, err, sizeof err) < 0) {
        Log("WARN: menu: ulozeni nastaveni selhalo: %s", err);
        EnterCriticalSection(&g_saveCs);
        strncpy(g_saveErr, err, sizeof g_saveErr - 1);
        LeaveCriticalSection(&g_saveCs);
        g_saveFailShown.store(true, std::memory_order_relaxed);
        g_saveFails.fetch_add(1);
    } else {
        Log("menu: ulozeno do bms_dlss.ini (bez vlakna): %s", done.c_str());
        g_saveFailShown.store(false, std::memory_order_relaxed);
    }
}

// DLL_PROCESS_DETACH pri ukonceni hry: ostatni vlakna uz nebezi. Zapsat, co zustalo ve fronte (pracovni vlakno
// mohlo skoncit uprostred - nahrada souboru je atomicka, tak se zapis zopakuje) a zmeny v otevrenem menu.
static void MenuSaveAtExit() {
    std::vector<IniWrite> w;
    if (g_saveCsInit && TryEnterCriticalSection(&g_saveCs)) {  // (zamek drzeny ukoncenym vlaknem: radeji nic)
        w = g_saveInFlight;
        w.insert(w.end(), g_saveQueue.begin(), g_saveQueue.end());
        g_saveQueue.clear();
        g_saveInFlight.clear();
        LeaveCriticalSection(&g_saveCs);
    }
    if (g_menu.open) MenuCollectWrites(w, true);
    if (w.empty()) return;
    std::string done;
    char err[160] = "";
    const int r = MenuIniApply(w, &done, err, sizeof err);
    if (r < 0)
        Log("WARN: menu: ulozeni pri ukonceni hry selhalo: %s", err);
    else
        Log("menu: ulozeno pri ukonceni hry: %s", r ? done.c_str() : "(beze zmeny)");
}

// ---------------------------------------------------------------------------
// Zmeny hodnot, otevreni a zavreni
// ---------------------------------------------------------------------------
static void MenuCommitPending() {
    MenuState& M = g_menu;
    if (!M.pendItem) return;
    const MenuItem& it = *M.pendItem;
    M.pendItem = nullptr;
    if (it.get() != M.pendId) it.set(M.pendId, Src::MENU);
    M.dirty = true;
}

// Nastavit polozce volbu id: HEAVY s odkladem (now = false), jinak hned.
static void MenuApply(const MenuItem& it, int id, bool now) {
    MenuState& M = g_menu;
    const int idx = MenuItemIndex(&it);
    if (idx >= 0) M.touched[idx] = true;
    M.dirty = true;
    if ((it.flags & MIF_HEAVY) && !now) {
        if (M.pendItem && M.pendItem != &it) MenuCommitPending();
        if (id == it.get()) {  // zpet na skutecnou hodnotu: nic menit
            M.pendItem = nullptr;
            return;
        }
        M.pendItem = &it;
        M.pendId = id;
        M.pendDue = GetTickCount64() + kMenuHeavyMs;
        return;
    }
    if (M.pendItem == &it) M.pendItem = nullptr;
    if (it.get() != id) it.set(id, Src::MENU);
}

// Dalsi / predchozi volba dokola; nedostupne volby se preskakuji.
static void MenuStep(const MenuItem* it, int dir, bool now) {
    if (!it || !MenuEditable(*it)) return;
    Choice ch[kMenuMaxChoices];
    const int n = MenuChoices(*it, ch);
    if (n < 1) return;
    const int cur = MenuCur(*it);
    int idx = -1;
    for (int i = 0; i < n; ++i)
        if (ch[i].id == cur) idx = i;
    for (int k = 0; k < n; ++k) {
        idx = idx < 0 ? (dir > 0 ? 0 : n - 1) : (idx + dir + n) % n;
        if (ch[idx].flags & CHF_SOON) continue;
        if (ch[idx].id != cur || now) MenuApply(*it, ch[idx].id, now);
        break;
    }
}

// Del: vychozi hodnota
static void MenuDefault(const MenuItem* it) {
    if (!it || it->def == kNoDef || !MenuEditable(*it)) return;
    Choice ch[kMenuMaxChoices];
    const int n = MenuChoices(*it, ch);
    for (int i = 0; i < n; ++i)
        if (ch[i].id == it->def && !(ch[i].flags & CHF_SOON)) MenuApply(*it, it->def, true);
}

// Pokrocile -> "Vychozi (doporucene)": Del na vsech polozkach stranky Pokrocile (uzivatel 2026-11-19)
static void MenuResetAdvanced() {
    int k = 0;
    for (const MenuSection& sec : kMenuSections)
        if (sec.page == PAGE_ADVANCED)
            for (int i = 0; i < sec.n; ++i)
                if (sec.items[i].kind != IK_ACTION) {
                    MenuDefault(&sec.items[i]);
                    ++k;
                }
    Log("menu: pokrocile -> vychozi (%d polozek)", k);
}

static void MenuOpen(const char* why) {
    MenuState& M = g_menu;
    M.open = true;
    M.dirty = true;
    M.drawFails = 0;
    M.pendItem = nullptr;
    M.fpsT0 = 0;
    M.fps = 0.0f;
    ++M.opens;
    for (const MenuSection& s : kMenuSections)
        for (int i = 0; i < s.n; ++i) {
            const int idx = MenuItemIndex(&s.items[i]);
            if (idx < 0) continue;
            M.openId[idx] = s.items[i].get ? s.items[i].get() : kNoDef;
            M.touched[idx] = false;
        }
    MenuCollectLines();
    g_menuFrameTick.store(GetTickCount(), std::memory_order_relaxed);
    g_menuOpen.store(true, std::memory_order_release);
    Log("menu: otevreno (%s)", why);
    if (g_menuHookOn.load(std::memory_order_relaxed)) MenuRawCheck("otevreni menu");
}
static void MenuClose(const char* why) {
    if (!g_menu.open) return;
    MenuCommitPending();  // odlozena HEAVY zmena plati i pri zavreni (Esc nic neodvolava)
    g_menu.open = false;
    g_menuOpen.store(false, std::memory_order_release);
    // (pocty od startu: podle nich je v logu videt, ze hacek opravdu polyka)
    Log("menu: zavreno (%s); hacek zatim spolkl %u zprav klavesnice a %u WM_INPUT, ztraceno udalosti %u", why,
        g_mkSwallowed.load(std::memory_order_relaxed), g_mkRawSwallowed.load(std::memory_order_relaxed),
        g_mkDropped.load(std::memory_order_relaxed));
    MenuSaveChanges();
}

static void MenuKey(UINT vk, bool repeat) {
    MenuState& M = g_menu;
    const int n = MenuItemCount();
    int& sel = MenuSel();
    switch (vk) {
    case VK_UP:
        if (n) sel = (sel + n - 1) % n;
        break;
    case VK_DOWN:
        if (n) sel = (sel + 1) % n;
        break;
    case VK_HOME:
    case VK_PRIOR:
        sel = 0;
        break;
    case VK_END:
    case VK_NEXT:
        sel = n ? n - 1 : 0;
        break;
    case VK_LEFT:
        MenuNumAccel(-1);
        MenuStep(MenuSelected(), -1, false);
        break;
    case VK_RIGHT:
        MenuNumAccel(+1);
        MenuStep(MenuSelected(), +1, false);
        break;
    case VK_RETURN:
    case VK_SPACE:
        if (!repeat) {
            const MenuItem* it = MenuSelected();
            StrId why;
            if (it && M.pendItem == it) {
                MenuCommitPending();  // Enter = odlozenou zmenu hned
            } else if (it && it->kind == IK_ACTION) {
                if (it->action && MenuApplies(*it, &why)) it->action();
            } else {
                MenuStep(it, +1, true);
            }
        }
        break;
    case VK_DELETE:
        if (!repeat) MenuDefault(MenuSelected());
        break;
    case VK_TAB:
        if (!repeat) {
            M.page = (M.page + 1) % PAGE_COUNT;
            M.scroll = 0;
            MenuCollectLines();
        }
        break;
    case VK_ESCAPE:
        if (!repeat) MenuClose("Esc");
        break;
    default: break;  // ostatni klavesy hra nedostane, menu je ignoruje
    }
    M.dirty = true;
}

// ---------------------------------------------------------------------------
// OSD (docs\navrh-menu.md kap. 9): zmena Alt klavesou -> jeden radek "polozka: hodnota" vlevo nahore na 2 s
// (posledni 0.3 s se stmiva), varovani 6 s. Text a hodnotu bere z polozky menu se stejnym OSD_*. Jen vlakno Present.
// ---------------------------------------------------------------------------
static struct OsdState {
    int what = -1;
    ULONGLONG until = 0;
} g_osd;

static void OsdNotify(int what) {
    if (!g_menuCfg.enable) return;  // [menu] Enable=0: zadny prekryv (mod jako driv)
    const bool warn = what == OSD_WARN_B || what == OSD_SAVE_FAIL || what == OSD_WARN_MSAA || what == OSD_FAIL_FG ||
                      what == OSD_FG_SUBST || what == OSD_FAIL_BRIDGE || what == OSD_SAFESTART || what == OSD_DXVK || what == OSD_REPORT_FAIL;
    if (what == OSD_REPORT && !g_menuCfg.enable) return;
    // "vyzaduje restart" vysvetluje, proc Alt+F9 nic neudelal: ukazat i s [menu] Osd=0, ale jen 2 s
    if (!g_menuCfg.osd && !warn && what != OSD_FG_RESTART && what != OSD_HINT && what != OSD_REPORT) return;
    g_osd.what = what;
    g_osd.until = GetTickCount64() + (warn || what == OSD_HINT || what == OSD_REPORT ? 6000 : 2000);
    if (warn || what == OSD_HINT || what == OSD_REPORT || what == OSD_RENDER) Log("menu: OSD %d zobrazeno", what);
}

// Klavesa menu jako text ("Alt+N"), stejne jako v zahlavi menu
static std::wstring MenuKeyLabel() {
    std::wstring mk = g_cfg.modifier == VK_MENU ? L"Alt+" : g_cfg.modifier == VK_SHIFT ? L"Shift+"
                      : g_cfg.modifier == 0 ? L"Alt+" : L"Ctrl+";
    const UINT ch = MapVirtualKeyW((UINT)g_menuCfg.key, MAPVK_VK_TO_CHAR) & 0x7FFF;
    mk.push_back(ch >= 32 && ch < 127 ? (wchar_t)ch : L'?');
    return mk;
}

// Po startu hry jednou na 6 s "Nastaveni modu: Alt+N" (uzivatel 2026-11-19), az se vykresli ~240 snimku
// (konec nacitani okna hry). [menu] StartHint=0 vypne. Jen vlakno Present.
static std::wstring MenuStatusHelper(int* level);
static std::wstring g_hintStatus;  // stav do uvodni hlasky ("DLSS 310.9.16 bezi"), prazdny = jen klavesa
static void MenuStartHintFrame() {
    static unsigned frames;
    static bool done;
    if (done || !g_menuCfg.enable || !g_menuCfg.startHint) return;
    if (++frames < 240) return;
    // Uvodni hlaska ukaze i stav upscaleru, az bezi (most startuje az chvili po prvni scene): pockat nejvys do ~900
    // snimku, pak jen klavesa. Chyby hlasi zvlast OSD_FAIL_* / OSD_DXVK.
    int level = 1;
    std::wstring st = Interop_GetMode() == BMSD_MODE_DLAA ? MenuStatusHelper(&level) : std::wstring();
    if ((st.empty() || level) && frames < 900 && Interop_GetMode() == BMSD_MODE_DLAA) return;
    done = true;
    g_hintStatus = !st.empty() && !level ? st : L"";
    if (!g_menu.open) OsdNotify(OSD_HINT);
}

static const MenuItem* MenuItemByOsd(int what) {
    if (what == OSD_UPSCALER && GetUpscaler() < 0) return &kItemsDebug[2];  // ladici rezim mostu (DebugCycle)
    for (const MenuSection& s : kMenuSections)
        for (int i = 0; i < s.n; ++i)
            if (s.items[i].osd == what) return &s.items[i];
    return nullptr;
}

// Zkratky pro testovani (uzivatel 2026-11-19; [keys] TestKeys / Pokrocile -> Zkratky pro testovani). Jen vlakno
// Present (OnFrameEnd), modKey = modifikator klaves modu drzen a hra v popredi. Zmeny jsou Src::KEY: OSD 2 s, do ini se
// neukladaji (plati do restartu, uklada jen menu). Hacek tytez stisky polyka (MkDecide, TestKeyOwns).
//   Alt+F9: Frame Generation vyp -> DLSS-G -> DLSS-G adaptivni -> FSR -> FSR adaptivni -> vyp (poradi jako v menu),
//           nedostupne rezimy se preskakuji (za behu, i kdyz hra
//           startovala bez FG). Jen kdyz za behu nejde (zarizeni nepresmerovano, zaloha): nic se nemeni, OSD
//           "vyzaduje restart".
//   Alt+F10: upscaler DLSS <-> FSR (z "auto" podle skutecne beziciho; vypnuty mod zapne jako volba v menu).
static bool g_testFgKeyWas, g_testUpsKeyWas;
static void MenuTestKeysFrame(bool modKey) {
    const bool on = g_testKeysOn.load(std::memory_order_relaxed);
    if (KeyEdge(on && modKey && KeyDown(kTestKeyFg), g_testFgKeyWas)) {
        if (!Present_FgPresenterActive() && !Present_FgRetryable()) {
            Log("zkratka Alt+F9: Frame Generation za behu nejde (zarizeni hry nepresmerovano nebo zaloha prezentace), "
                "vyzaduje restart");
            SrcBeep(Src::KEY, MB_ICONHAND);
            OsdNotify(OSD_FG_RESTART);
        } else {
            static const int kOrder[] = {FG_OFF, FG_DLSSG, FG_DLSSG_ADAPTIVE, FG_FSR2X, FG_FSR_ADAPTIVE};
            const int nOrder = (int)(sizeof kOrder / sizeof kOrder[0]);
            const int cur = Present_FgGetMode();
            int i = 0;
            while (i < nOrder && kOrder[i] != cur) ++i;  // (neznamy -> jako vyp)
            if (i == nOrder) i = 0;
            int next = FG_OFF;
            for (int k = 1; k <= nOrder; ++k) {
                const int m = kOrder[(i + k) % nOrder];
                if (Present_FgModeAvailable(m)) {
                    next = m;
                    break;
                }
            }
            SetFgMode(next, Src::KEY);
            SrcBeep(Src::KEY, next != FG_OFF ? MB_OK : MB_ICONHAND);
            OsdNotify(OSD_FG);
        }
    }
    if (KeyEdge(on && modKey && KeyDown(kTestKeyUpscaler), g_testUpsKeyWas)) {
        const int u = GetUpscaler();
        const bool fsrNow = u == UPS_FSR || (u != UPS_DLSS && Interop_ActiveUpscaler(nullptr) == BMSD_UPSCALER_FSR);
        const bool wasOn = Interop_GetMode() != BMSD_MODE_OFF;  // (vypnuty mod: pipne a ukaze OSD uz SetBridgeMode)
        SetUpscaler(fsrNow ? UPS_DLSS : UPS_FSR, Src::KEY);  // DLSS nedostupne: SetUpscaler zaloguje a nic nezmeni
        if (wasOn) SrcBeep(Src::KEY, MB_OK);
        OsdNotify(OSD_UPSCALER);
    }
}

// Text hodnoty polozky (pro radek menu i OSD)
static std::wstring MenuValueText(const MenuItem& it, int cur) {
    if (it.kind == IK_INFO) {
        wchar_t b[64] = L"";
        if (it.info) it.info(b, 64);
        return b;
    }
    if (it.kind == IK_ACTION) return Tr(S_ACT_RUN);
    Choice ch[kMenuMaxChoices];
    const int n = MenuChoices(it, ch);
    for (int k = 0; k < n; ++k)
        if (ch[k].id == cur) return ChText(ch[k]);
    return &it == &kItemsImage[1] && cur < 0 ? Tr(S_ST_DEBUG) : L"–";  // [1] = Upscaler (za "Zapnuto")
}

// ---------------------------------------------------------------------------
// Test bez klaves: [menu] TestOpenSec=N otevre menu N sekund po prvnim Present, TestKeys=down,shot,esc... pak po
// 300 ms posila udalosti stejnou frontou jako hacek (shot = ulozit backbuffer, w<ms> = cekat, krok*N = N udalosti
// v jednom snimku, napr. right*5), TestShot=1 = snimek hned po otevreni.
// ---------------------------------------------------------------------------
static void MenuTestParse() {
    MenuState& M = g_menu;
    M.testSteps.clear();
    if (g_menuCfg.testShot) M.testSteps.push_back(std::make_pair(-1, 0));
    std::wstring tok;
    auto flush = [&]() {
        if (tok.empty()) return;
        int rep = 1;
        const size_t star = tok.find(L'*');
        if (star != std::wstring::npos) {
            rep = std::min(64, std::max(1, (int)wcstol(tok.c_str() + star + 1, nullptr, 10)));
            tok.resize(star);
        }
        static const struct {
            const wchar_t* name;
            int ev;
        } kMap[] = {{L"up", VK_UP},       {L"down", VK_DOWN},  {L"left", VK_LEFT},    {L"right", VK_RIGHT},
                    {L"enter", VK_RETURN}, {L"esc", VK_ESCAPE}, {L"tab", VK_TAB},     {L"del", VK_DELETE},
                    {L"home", VK_HOME},    {L"end", VK_END},    {L"menu", MK_TOGGLE}, {L"shot", -1},
                    {L"osd", -3}};  // osd = jako Alt+Home (ostrost klavesou: OSD, do ini se neuklada)
        bool found = false;
        for (const auto& m : kMap)
            if (tok == m.name) {
                for (int r = 0; r < rep; ++r) M.testSteps.push_back(std::make_pair(m.ev, r + 1 < rep ? -1 : 0));
                found = true;
            }
        if (!found && !tok.empty() && tok[0] == L'w') {
            M.testSteps.push_back(std::make_pair(-2, std::max(0, (int)wcstol(tok.c_str() + 1, nullptr, 10))));
            found = true;
        }
        if (!found) Log("WARN: menu: [menu] TestKeys - neznamy krok '%ls'", tok.c_str());
        tok.clear();
    };
    for (wchar_t c : g_menuCfg.testKeys) {
        if (c == L',' || c == L';') flush();
        else if (!iswspace(c)) tok.push_back((wchar_t)towlower(c));
    }
    flush();
}

static void MenuTestStep(ULONGLONG now) {
    MenuState& M = g_menu;
    if (!M.testStarted) {
        if (now - M.firstMs < (ULONGLONG)g_menuCfg.testOpenSec * 1000) return;
        M.testStarted = M.testOpened = true;
        MenuTestParse();
        MkPush(MK_TOGGLE);
        M.testNextMs = now + 400;
        Log("menu: test - otevreni bez klaves (TestOpenSec=%d), kroku %u", g_menuCfg.testOpenSec,
            (unsigned)M.testSteps.size());
        return;
    }
    while (!M.testDone && now >= M.testNextMs) {
        if (M.testPos >= M.testSteps.size()) {
            M.testDone = true;
            break;
        }
        const std::pair<int, int> st = M.testSteps[M.testPos++];
        if (st.first == -2) {
            M.testNextMs = now + (ULONGLONG)st.second;
            break;
        }
        if (st.first == -1) ++M.shotPending;
        else if (st.first == -3) SetSharpness(NextSharpness(), Src::KEY);
        else MkPush((uint16_t)st.first);
        if (st.second == -1) continue;  // dalsi udalost tehoz kroku (krok*N) jeste v tomto snimku
        M.testNextMs = now + 300;
        break;
    }
}

// Zaloha bez hacku: GetAsyncKeyState s vlastnim opakovanim (400 / 70 ms). Klavesy dostane i hra, proto jen pismena,
// ktera hra ve vychozim nastaveni nema: I/K = nahoru/dolu, J/L = hodnota, O = potvrdit; zavira Alt+N.
static void MenuPollFallback(bool foreground, bool modKey, ULONGLONG now) {
    MenuState& M = g_menu;
    // s Modifier=none jen Alt+klavesa menu (jako MkMenuModDown)
    const bool menuMod = g_cfg.modifier != 0
                             ? modKey
                             : foreground && KeyDown(VK_MENU) && !KeyDown(VK_CONTROL) && !KeyDown(VK_SHIFT);
    if (KeyEdge(menuMod && KeyDown(g_menuCfg.key), M.pollKeyWas)) MkPush(MK_TOGGLE);
    static struct {
        int vk;
        uint16_t ev;
        bool was;
        ULONGLONG next;
    } keys[] = {{'I', VK_UP}, {'K', VK_DOWN}, {'J', VK_LEFT}, {'L', VK_RIGHT}, {'O', VK_RETURN}};
    for (auto& k : keys) {
        const bool dn = M.open && foreground && !modKey && KeyDown(k.vk);
        if (dn && !k.was) {
            MkPush(k.ev);
            k.next = now + 400;
        } else if (dn && now >= k.next) {
            MkPush((uint16_t)(k.ev | MK_REPEAT));
            k.next = now + 70;
        }
        k.was = dn;
    }
}

// OnFrameEnd, cast klaves: hacky, fronta stisku -> stav menu a akce Set*; odlozene HEAVY zmeny, FPS. foreground = v
// popredi je okno tohoto procesu (fgTid = jeho vlakno), modKey = modifikator klaves modu drzen a hra v popredi.
static void MenuFrameInput(bool foreground, bool modKey, DWORD fgTid) {
    if (!g_menuCfg.enable) return;
    MenuState& M = g_menu;
    const bool hookMode = !g_menuCfg.noHook;
    if (!M.frames++) {
        M.firstMs = GetTickCount64();
        g_menuHookOn.store(hookMode, std::memory_order_relaxed);
    }
    if (hookMode) {
        if (g_menuGameTid && !g_menuGameHooked) {
            g_menuGameHooked = true;
            MenuHookThread(g_menuGameTid, "okno hry");
        }
        if (foreground && fgTid && fgTid != M.lastFgTid) {  // jine okno procesu ma fokus (menu hry v Qt...)
            M.lastFgTid = fgTid;
            MenuHookThread(fgTid, "okno v popredi");
        }
        if (g_nMhooks && (!M.rawChecked || M.frames % 1024 == 0)) {
            M.rawChecked = true;
            MenuRawCheck(M.frames < 1024 ? "instalace hacku" : "za behu");
        }
    }
    const bool fallback = !hookMode || (g_nMhooks == 0 && (g_menuGameHooked || M.lastFgTid));
    if (fallback && !M.fallbackLogged) {
        M.fallbackLogged = true;
        Log("menu: bez hacku klavesnice (%s) - Alt+N a ovladani I/K, J/L, O pres GetAsyncKeyState, klavesy dostane "
            "i hra",
            hookMode ? "instalace selhala" : "[menu] NoHook=1");
    }
    const bool needClock = fallback || (g_menuCfg.testOpenSec > 0 && !M.testDone) || M.open;
    const ULONGLONG now = needClock ? GetTickCount64() : 0;
    if (g_menuCfg.testOpenSec > 0 && !M.testDone) MenuTestStep(now);
    if (fallback) MenuPollFallback(foreground, modKey, now);

    if (M.open) g_menuFrameTick.store(GetTickCount(), std::memory_order_relaxed);
    uint16_t ev;
    if (M.open) MenuRefreshLines();
    while (MkPop(&ev)) {
        if (ev & MK_TOGGLE) {
            if (M.open) MenuClose("klavesa menu");
            else MenuOpen("klavesa menu");
        } else if (M.open) {
            MenuKey(ev & 0xFF, (ev & MK_REPEAT) != 0);
            if (M.open) MenuRefreshLines();
        }
    }
    // Ztrata popredi (alt-tab): menu zavrit; bitmapy spolknutych klaves vynulovat (uvolneni uz neprijde)
    if (M.wasForeground && !foreground) {
        for (auto& w : g_mkHeldKbd) w.store(0, std::memory_order_relaxed);
        for (auto& w : g_mkHeldRaw) w.store(0, std::memory_order_relaxed);
        g_mkRawMods.store(0, std::memory_order_relaxed);
        if (M.open && !M.testOpened) MenuClose("okno hry neni v popredi");
    }
    M.wasForeground = foreground;

    if (M.open) {
        if (M.pendItem && now >= M.pendDue) MenuCommitPending();
        // FPS: prumer za 0.5 s (vlastni QPC, nezavisle na radku vykon)
        if (!g_omQpf.QuadPart) QueryPerformanceFrequency(&g_omQpf);
        const LONGLONG q = OmQpc();
        if (!M.fpsT0) {
            M.fpsT0 = q;
            M.fpsN = 0;
        } else if (++M.fpsN, (double)(q - M.fpsT0) >= 0.5 * (double)g_omQpf.QuadPart) {
            const double dt = (double)(q - M.fpsT0) / (double)g_omQpf.QuadPart;
            M.fps = (float)(M.fpsN / dt);
            M.fpsMs = (float)(dt * 1000.0 / M.fpsN);
            M.fpsT0 = q;
            M.fpsN = 0;
        }
    }
    // ulozeni selhalo (pracovni vlakno) -> OSD
    const int fails = g_saveFails.load(std::memory_order_relaxed);
    if (fails != M.saveFailsSeen) {
        M.saveFailsSeen = fails;
        OsdNotify(OSD_SAVE_FAIL);
        M.dirty = true;
    }
    static bool failShownWas = false;  // radek chyby zmizel/pribyl -> prestavet hned
    const bool failShown = g_saveFailShown.load(std::memory_order_relaxed);
    if (failShown != failShownWas) {
        failShownWas = failShown;
        M.dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Rozvrzeni a geometrie menu (docs\navrh-menu.md 5.5, 6.2)
// ---------------------------------------------------------------------------
static std::wstring MenuW(const char* s) { return std::wstring(s, s + strlen(s)); }

// Stavovy radek 1: rezim DLSS a preset
static std::wstring MenuStatusMode() {
    const uint32_t mode = Interop_GetMode();
    const std::wstring dot = L" · ";
    std::wstring s;
    if (mode == BMSD_MODE_OFF) return Tr(S_ST_DLSS_OFF);
    if (mode != BMSD_MODE_DLAA) return Tr(S_ST_DEBUG);
    const bool fsr = Interop_ActiveUpscaler(nullptr) == BMSD_UPSCALER_FSR;  // skutecne bezici upscaler (helper)
    const std::wstring ups = fsr ? L"FSR " : L"DLSS ";
    if (!Interop_DlaaReady()) {
        s = Tr(S_ST_DLSS_WAIT);
    } else if (g_pathB && g_renderW && g_renderH) {
        const float r = RenderRatio();
        s = ups + (r < 0.54f ? L"Performance" : r < 0.62f ? L"Balanced" : L"Quality") + dot + Tr(S_ST_ECO);
    } else {
        const float sc = Interop_ActiveOutScale();
        if (sc < 1.0001f) {
            s = fsr ? L"FSR Native AA" : L"DLAA";
        } else {
            wchar_t num[16];
            MenuFmtNum(sc, 2, num, 16, nullptr, 0);
            s = ups + Tr(S_ST_SUPERSAMPLE) + L" " + num + L"×";
        }
    }
    if (!fsr)
        s += dot + Tr(S_ST_PRESET) + L" " + (g_preset ? MenuW(PresetName(g_preset)) : std::wstring(Tr(S_ST_DEFAULT_K)));
    return s;
}
// Stavovy radek 2: rozliseni vstup -> vystup DLSS -> obraz
static std::wstring MenuStatusRes(UINT bbW, UINT bbH) {
    auto res = [](UINT w, UINT h) { return std::to_wstring(w) + L"×" + std::to_wstring(h); };
    const std::wstring ar = L" → ";
    if (DlssRunning()) {
        if (g_pathB && g_renderW && g_renderH)
            return Tr(S_ST_RENDER) + std::wstring(L" ") + res(g_renderW, g_renderH) + ar + res(bbW, bbH);
        const float sc = Interop_ActiveOutScale();
        if (sc > 1.0001f)
            return res(bbW, bbH) + ar + res((UINT)lroundf((float)bbW * sc), (UINT)lroundf((float)bbH * sc)) + ar +
                   res(bbW, bbH);
    }
    return res(bbW, bbH);
}
// Vpravo na radku 2: stav helperu (level 0 = v poradku, 1 = varovani)
static std::wstring MenuStatusHelper(int* level) {
    *level = 0;
    if (Interop_GetMode() != BMSD_MODE_DLAA) return L"";
    if (Interop_DlaaReady()) {
        // nazev s verzi od helperu: "DLSS 310.9.16 bezi" (verze skutecne nactene knihovny), "FSR 3.1.6 bezi";
        // u volby auto, co se vybralo: "Auto: DLSS 310.9.16" / "Auto: FSR 3.1.6"
        const char* name = nullptr;
        const int act = Interop_ActiveUpscaler(&name);
        if ((act == BMSD_UPSCALER_FSR || act == BMSD_UPSCALER_DLSS) && name && *name) {
            if (GetUpscaler() == UPS_AUTO) return std::wstring(Tr(S_UPS_AUTO)) + L": " + MenuW(name);
            return MenuW(name) + (g_menuCfg.lang == 0 ? std::wstring(L" běží") : std::wstring(L" running"));
        }
        return Tr(S_ST_RUNNING);
    }
    const char* why = Interop_WhyIdle();
    *level = 1;
    if (!why) return Tr(S_ST_DLSS_WAIT);
    return g_menuCfg.lang == 0 ? MenuW(why) : std::wstring(Tr(S_ST_IDLE));
}
// Radek 3 (jen kdyz je co hlasit): MSAA hry blokuje most (zaloha, kdyz ho mod neodebral), chyba ulozeni
static std::wstring MenuStatusWarn() {
    if (Interop_MsaaBlocked()) return Tr(S_OSD_WARN_MSAA);
    if (!g_saveFailShown.load(std::memory_order_relaxed) || !g_saveCsInit) return L"";
    char e[160] = "";
    EnterCriticalSection(&g_saveCs);
    memcpy(e, g_saveErr, sizeof e);
    LeaveCriticalSection(&g_saveCs);
    e[sizeof e - 1] = 0;
    return std::wstring(Tr(S_ST_SAVE_FAIL)) + L" " + MenuW(e);
}

// Text zkraceny na sirku maxW ("…" na konci)
static std::wstring OvFit(int font, const std::wstring& s, int maxW) {
    if (maxW <= 0) return L"";
    if (OvTextW(font, s.c_str()) <= maxW) return s;
    std::wstring t = s;
    while (!t.empty() && OvTextW(font, (t + L"…").c_str()) > maxW) t.pop_back();
    while (!t.empty() && t.back() == L' ') t.pop_back();
    return t + L"…";
}

// Zalomeni textu na nejvys maxLines radku sirky maxW (po mezerach); co se nevejde, konci "…"
static void MenuWrap(int font, const std::wstring& text, int maxW, int maxLines, std::vector<std::wstring>& out) {
    out.clear();
    std::wstring line, word;
    bool over = false;
    auto put = [&]() {
        if (word.empty()) return;
        if (over) {
            line += L" " + word;
        } else {
            const std::wstring cand = line.empty() ? word : line + L" " + word;
            if (!line.empty() && OvTextW(font, cand.c_str()) > maxW) {
                if ((int)out.size() + 1 >= maxLines) {
                    over = true;
                    line = cand;  // OvFit nize zkrati
                } else {
                    out.push_back(line);
                    line = word;
                }
            } else {
                line = cand;
            }
        }
        word.clear();
    };
    for (wchar_t c : text) {
        if (c == L' ') put();
        else word.push_back(c);
    }
    put();
    if (!line.empty()) out.push_back(OvFit(font, line, maxW));
}

// Velikost zakladniho pisma podle vysky cile: bbH / 48 * Scale (720p 15, 1080p 22, 1440p 30, 2160p 45), meze 12-72.
static int MenuBasePx(UINT bbH) {
    return std::min(72, std::max(12, (int)lroundf((float)bbH / 48.0f * g_menuCfg.scale)));
}

// Rozmery rozvrzeni odvozene od velikosti pisma px (vse v pixelech cile). Height a MenuBuild pocitaji stejne.
struct MenuMetrics {
    int px, lineH, rowH, secH, pad, titleH, tabH, footH, helpLines;
    explicit MenuMetrics(int p) {
        px = p;
        lineH = (int)lroundf((float)p * 1.4f);
        rowH = (int)lroundf((float)p * 1.75f);
        secH = (int)lroundf((float)p * 1.7f);
        pad = (int)lroundf((float)p * 1.1f);
        titleH = (int)lroundf((float)p * 2.1f);
        tabH = (int)lroundf((float)p * 1.8f);
        footH = (int)lroundf((float)p * 1.8f);
        helpLines = 2;
    }
    int Head(int nStatus) const { return pad * 3 / 4 + titleH + tabH + px / 4 + nStatus * lineH + px / 2 + px / 2; }
    int Foot() const { return px / 2 + px / 2 + helpLines * lineH + px / 2 + footH + pad * 3 / 4; }
    // vyska panelu pro seznam vysky listH a nStatus stavovych radku
    int Height(int listH, int nStatus) const { return Head(nStatus) + listH + Foot(); }
    int Width(UINT bbW) const { return std::min((int)((float)bbW * 0.94f), px * 40); }
};

static int MenuStatusLines() { return MenuStatusWarn().empty() ? 2 : 3; }

// Vyska seznamu (nadpisy sekci + polozky) nejvyssi stranky: panel ma pro obe stranky stejnou vysku (Tab necuka).
static int MenuListHeight(const MenuMetrics& mt) {
    int best = 0;
    for (int p = 0; p < PAGE_COUNT; ++p) {
        int h = 0;
        for (const MenuSection& s : kMenuSections) {
            if (s.page != p || (s.visible && !s.visible())) continue;
            int n = 0;
            for (int i = 0; i < s.n; ++i)
                if (!s.items[i].visible || s.items[i].visible()) ++n;
            if (n) h += mt.secH + n * mt.rowH;
        }
        best = std::max(best, h);
    }
    return best;
}

// Barvy (premultiplikovane); a = pruhlednost celku (stmivani OSD)
struct MenuColors {
    DWORD shadow, panel, ring, sep, accent, sel, text, dim, off, sec, warn, err, ok, key;
    explicit MenuColors(float a = 1.0f) {
        auto c = [a](unsigned r, unsigned g, unsigned b, unsigned al) {
            return OvCol(r, g, b, (unsigned)((float)al * a + 0.5f));
        };
        shadow = c(0, 0, 0, 120);
        panel = c(15, 18, 25, 238);
        ring = c(255, 255, 255, 40);
        sep = c(255, 255, 255, 26);
        accent = c(64, 160, 255, 255);
        sel = c(64, 160, 255, 70);
        text = c(236, 240, 246, 255);
        dim = c(158, 167, 181, 255);
        off = c(100, 107, 120, 255);
        sec = c(118, 190, 255, 255);
        warn = c(255, 204, 72, 255);
        err = c(255, 110, 100, 255);
        ok = c(120, 220, 140, 255);
        key = c(255, 255, 255, 30);
    }
};

// Geometrie menu pro cil bbW x bbH do seznamu L (pozice v pixelech cile). Vola se s platnym atlasem pro px.
static void MenuBuild(OvList& L, UINT bbW, UINT bbH, int px) {
    MenuState& M = g_menu;
    const OvAtlas& A = g_ovAtlas;
    L.Clear();
    MenuCollectLines();
    const MenuMetrics mt(px);
    const std::wstring warnLine = MenuStatusWarn();
    const int nStatus = warnLine.empty() ? 2 : 3;
    int listH = MenuListHeight(mt);

    // Seznam se nevejde ani s nejmensim pismem: okno viditelnych radku, vyber drzet uvnitr
    const int maxH = (int)((float)bbH * 0.94f);
    int first = 0, last = (int)M.lines.size();
    int panelH = mt.Height(listH, nStatus);
    if (panelH > maxH && !M.lines.empty()) {
        const int fixed = mt.Height(0, nStatus);
        const int rows = std::max(2, (maxH - fixed) / mt.rowH);
        int selLine = 0, k = 0;
        for (int i = 0; i < (int)M.lines.size(); ++i)
            if (M.lines[i].item && k++ == MenuSel()) selLine = i;
        if (selLine < M.scroll + 1) M.scroll = std::max(0, selLine - 1);  // i nadpis sekce nad vyberem
        if (selLine >= M.scroll + rows) M.scroll = selLine - rows + 1;
        M.scroll = std::max(0, std::min(M.scroll, (int)M.lines.size() - rows));
        first = M.scroll;
        last = std::min((int)M.lines.size(), first + rows);
        listH = rows * mt.rowH;
        panelH = fixed + listH;
    } else {
        M.scroll = 0;
    }

    const int panelW = mt.Width(bbW);
    // Poloha ([menu] Position): stred, vlevo / vpravo (svisle uprostred), vlevo / vpravo nahore - stred obrazu volny
    const int mg = px * 2, pos = g_menuCfg.pos;
    const int x0 = pos == 1 || pos == 3 ? mg : pos == 2 || pos == 4 ? std::max(0, (int)bbW - panelW - mg)
                                                                    : ((int)bbW - panelW) / 2;
    const int y0 = pos >= 3 ? std::min(mg * 2, std::max(0, (int)bbH - panelH)) : std::max(0, ((int)bbH - panelH) / 2);
    const int x1 = x0 + panelW, y1 = y0 + panelH;
    const int inL = x0 + mt.pad, inR = x1 - mt.pad;
    const MenuColors C;

    // Stin, panel, obrys
    const int sh = A.spr[OVS_SHADOW].n;
    OvNine(L, x0 - sh * 2 / 3, y0 - sh / 2, x1 + sh * 2 / 3, y1 + sh, A.spr[OVS_SHADOW], C.shadow, true);
    OvNine(L, x0, y0, x1, y1, A.spr[OVS_PANEL], C.panel, true);
    OvNine(L, x0, y0, x1, y1, A.spr[OVS_RING], C.ring, false);

    const OvFont& fReg = A.font[OVF_REG];
    const OvFont& fBold = A.font[OVF_BOLD];
    const OvFont& fTitle = A.font[OVF_TITLE];
    auto textY = [](int top, int boxH, const OvFont& f) { return top + (boxH - f.height) / 2; };

    // Klavesa v ramecku + popis; vraci x za popisem
    auto keyHint = [&](int x, int top, int boxH, const wchar_t* key, const wchar_t* label) {
        const int kw = OvTextW(OVF_REG, key), kp = px / 2, kh = (int)lroundf((float)px * 1.45f);
        const int ky = top + (boxH - kh) / 2;
        OvNine(L, x, ky, x + kw + 2 * kp, ky + kh, A.spr[OVS_ROW], C.key, true);
        OvText(L, OVF_REG, x + kp, textY(ky, kh, fReg), key, C.text);
        x += kw + 2 * kp + px / 2;
        x = OvText(L, OVF_REG, x, textY(top, boxH, fReg), label, C.dim);
        return x + px;
    };
    auto keyHintW = [&](const wchar_t* key, const wchar_t* label) {
        return OvTextW(OVF_REG, key) + 2 * (px / 2) + px / 2 + OvTextW(OVF_REG, label) + px;
    };
    const bool noHook = g_menuCfg.noHook || g_nMhooks == 0;

    // Hlavicka: nadpis, verze, vpravo klavesa pro zavreni
    int y = y0 + mt.pad * 3 / 4;
    {
        const int ty = textY(y, mt.titleH, fTitle);
        int x = OvText(L, OVF_TITLE, inL, ty, Tr(S_TITLE), C.text);
        wchar_t ver[48];
        swprintf(ver, 48, L"bms-dlss %hs", BMSDLSS_VER_STR);  // verze vydani (BMSD_VERSION je verze rozhrani)
        OvText(L, OVF_REG, x + px * 3 / 4, ty + fTitle.ascent - fReg.ascent, ver, C.dim);
        std::wstring mk = g_cfg.modifier == VK_MENU ? L"Alt+" : g_cfg.modifier == VK_SHIFT ? L"Shift+"
                          : g_cfg.modifier == 0 ? L"Alt+" : L"Ctrl+";
        const UINT ch = MapVirtualKeyW((UINT)g_menuCfg.key, MAPVK_VK_TO_CHAR) & 0x7FFF;
        mk.push_back(ch >= 32 && ch < 127 ? (wchar_t)ch : L'?');
        keyHint(inR - keyHintW(mk.c_str(), Tr(S_HINT_CLOSE)) + px, y, mt.titleH, mk.c_str(), Tr(S_HINT_CLOSE));
    }
    y += mt.titleH;
    // Stranky (Tab)
    {
        int x = inL;
        const StrId pages[PAGE_COUNT] = {S_PAGE_COMMON, S_PAGE_ADV};
        const int th = (int)lroundf((float)px * 1.55f), ty0 = y + (mt.tabH - th) / 2;
        for (int p = 0; p < PAGE_COUNT; ++p) {
            const bool cur = p == M.page;
            const int w = OvTextW(OVF_BOLD, Tr(pages[p])) + px * 3 / 2;
            OvNine(L, x, ty0, x + w, ty0 + th, A.spr[OVS_ROW], cur ? C.sel : C.key, true);
            if (cur) OvRect(L, x + px / 2, ty0 + th - std::max(2, px / 8), x + w - px / 2, ty0 + th, C.accent);
            OvText(L, cur ? OVF_BOLD : OVF_REG, x + px * 3 / 4, textY(ty0, th, fBold), Tr(pages[p]),
                   cur ? C.text : C.dim);
            x += w + px / 2;
        }
        if (!noHook) keyHint(x + px / 2, y, mt.tabH, L"Tab", Tr(S_HINT_PAGE));
    }
    y += mt.tabH + px / 4;
    // Stavove radky
    {
        wchar_t fps[48] = L"";
        if (M.fps > 0.0f) {
            wchar_t ms[16];
            MenuFmtNum(M.fpsMs, 1, ms, 16, nullptr, 0);
            swprintf(fps, 48, L"%.0f FPS · %ls ms", M.fps, ms);
        }
        const int fw = OvTextW(OVF_REG, fps);
        OvText(L, OVF_REG, inL, textY(y, mt.lineH, fReg), OvFit(OVF_REG, MenuStatusMode(), inR - inL - fw - px).c_str(),
               C.text);
        OvText(L, OVF_REG, inR - fw, textY(y, mt.lineH, fReg), fps, C.dim);
        y += mt.lineH;
        int level = 0;
        const std::wstring hs = OvFit(OVF_REG, MenuStatusHelper(&level), (inR - inL) / 2);
        const int hw = OvTextW(OVF_REG, hs.c_str());
        OvText(L, OVF_REG, inL, textY(y, mt.lineH, fReg),
               OvFit(OVF_REG, MenuStatusRes(bbW, bbH), inR - inL - hw - px).c_str(), C.dim);
        OvText(L, OVF_REG, inR - hw, textY(y, mt.lineH, fReg), hs.c_str(), level ? C.warn : C.ok);
        y += mt.lineH;
        if (nStatus > 2) {
            OvText(L, OVF_REG, inL, textY(y, mt.lineH, fReg), OvFit(OVF_REG, warnLine, inR - inL).c_str(), C.err);
            y += mt.lineH;
        }
    }
    y += px / 2;
    OvRect(L, inL, y, inR, y + std::max(1, px / 16), C.sep);
    y += px / 2;

    // Sekce a polozky
    const int colW = std::min(px * 14, (inR - inL) * 46 / 100);  // sloupec hodnoty vpravo: < hodnota >
    const MenuItem* selItem = nullptr;
    int itemIdx = 0;
    const int listTop = y;
    for (int i = 0; i < (int)M.lines.size(); ++i) {
        const MenuLine& ln = M.lines[i];
        const bool vis = i >= first && i < last;
        if (!ln.item) {
            if (!vis) continue;
            std::wstring t = Tr(ln.sec->title);
            CharUpperBuffW(&t[0], (DWORD)t.size());
            OvText(L, OVF_BOLD, inL, y + mt.secH - fBold.height - px / 5, t.c_str(), C.sec);
            y += mt.secH;
            continue;
        }
        const MenuItem& it = *ln.item;
        const bool sel = itemIdx++ == MenuSel();
        StrId why = S_NONE;
        const bool infoRow = it.kind == IK_INFO;
        const bool active = MenuApplies(it, &why);
        if (sel) selItem = &it;
        if (!vis) continue;
        if (sel) {
            OvNine(L, x0 + mt.pad / 2, y + 1, x1 - mt.pad / 2, y + mt.rowH - 1, A.spr[OVS_ROW], C.sel, true);
            const int bw = std::max(3, px / 5), inset = A.spr[OVS_ROW].n;
            OvRect(L, x0 + mt.pad / 2, y + 1 + inset, x0 + mt.pad / 2 + bw, y + mt.rowH - 1 - inset, C.accent);
        }
        const int ty = textY(y, mt.rowH, fReg);
        const int lf = sel ? OVF_BOLD : OVF_REG;
        const int vx0 = inR - colW;
        const int lx0 = inL + px * 3 / 4;
        OvText(L, lf, lx0, ty, OvFit(lf, Tr(it.label), vx0 - lx0 - px / 2).c_str(),
               !active ? C.off : sel ? C.text : C.dim);
        // hodnota: < volba > (odlozena HEAVY zmena s "…"), info bez sipek, akce jako klavesa
        const int cur = MenuCur(it);
        const bool pending = M.pendItem == &it;
        std::wstring val = MenuValueText(it, cur);
        if (it.kind == IK_ACTION) {
            const int kw = keyHintW(L"Enter", val.c_str()) - px;
            keyHint(vx0 + (colW - kw) / 2, y, mt.rowH, L"Enter", val.c_str());
        } else {
            Choice ch[kMenuMaxChoices];
            const int n = MenuChoices(it, ch);
            int avail = 0;
            bool curSoon = false;  // aktualni volba neni k dispozici (DLSS z ini na karte bez NGX): seda
            for (int k = 0; k < n; ++k) {
                if (!(ch[k].flags & CHF_SOON)) ++avail;
                else if (ch[k].id == cur && !pending) curSoon = true;
            }
            const bool arrows = active && avail > 1 && it.set;
            const int aw = OvTextW(OVF_BOLD, L"‹");
            // info radek (stav FG s nazvem a verzi generatoru) smi presahnout sloupec hodnoty doleva az k popisku
            const int labelEnd = lx0 + OvTextW(lf, Tr(it.label)) + px;
            const int maxV = arrows ? colW - 2 * aw - px : infoRow ? std::max(colW, inR - labelEnd) : colW;
            if (pending) val += L" …";
            val = OvFit(lf, val, maxV);
            const int vw = OvTextW(lf, val.c_str());
            OvText(L, lf, vw > colW ? inR - vw : vx0 + (colW - vw) / 2, ty, val.c_str(),
                   !active || curSoon ? C.off : pending ? C.accent : infoRow ? C.dim : C.text);
            if (arrows) {
                const DWORD ca = sel ? C.accent : C.off;
                OvText(L, OVF_BOLD, vx0, ty, L"‹", ca);
                OvText(L, OVF_BOLD, inR - OvTextW(OVF_BOLD, L"›"), ty, L"›", ca);
            }
        }
        y += mt.rowH;
    }
    // (znacky, ze seznam pokracuje mimo okno)
    if (first > 0) OvText(L, OVF_REG, (x0 + x1) / 2, listTop - px / 2, L"↑", C.dim);
    if (last < (int)M.lines.size()) OvText(L, OVF_REG, (x0 + x1) / 2, y - px / 2, L"↓", C.dim);

    y = std::max(y, listTop + listH);  // kratsi stranka: volne misto pod seznamem
    y += px / 2;
    OvRect(L, inL, y, inR, y + std::max(1, px / 16), C.sep);
    y += px / 2;

    // Napoveda k vybrane polozce (2 radky): proc neplati, text, poznamka, dosavadni klavesa
    {
        std::wstring help;
        DWORD col = C.dim;
        if (noHook) {
            help = Tr(S_NOHOOK);
            col = C.warn;
        } else if (selItem) {
            StrId why = S_NONE;
            if (!MenuApplies(*selItem, &why) && why != S_NONE && why != S_WHY_SOON) {
                help = std::wstring(Tr(why)) + L" ";
                col = C.warn;
            }
            help += Tr(selItem->help);
            const StrId note = selItem->note ? selItem->note() : S_NONE;
            if (note != S_NONE) {
                help += std::wstring(L" ") + Tr(note);
                // ("DLSS: jen NVIDIA RTX" je u volby auto / FSR jen informace, varovani az kdyz je DLSS vybrane)
                if (note != S_NOTE_DLSS_RTX || GetUpscaler() == UPS_DLSS) col = C.warn;
            }
            if (const char* nt = selItem->noteText ? selItem->noteText() : nullptr) {
                help += std::wstring(L" ") + MenuW(nt) + L".";
                col = C.warn;
            }
            if (!selItem->iniSec && selItem->kind != IK_INFO && selItem->kind != IK_ACTION &&
                M.page != PAGE_ADVANCED)  // (na strance Pokrocile to rika nadpis sekce Ladeni)
                help += std::wstring(L" ") + Tr(S_NOTE_NOSAVE);
            // (ladici Alt klavesy plati jen s [keys] DebugKeys=1; Alt+F8 vzdy)
            if (selItem->legacyKey && (g_cfg.debugKeys || !strcmp(selItem->legacyKey, "Alt+F8")))
                help += std::wstring(L" ") + Tr(S_LEGACY_KEY) + L" " + MenuW(selItem->legacyKey) + L".";
        }
        std::vector<std::wstring> lines;
        MenuWrap(OVF_REG, help, inR - inL, mt.helpLines, lines);
        for (int i = 0; i < (int)lines.size() && i < mt.helpLines; ++i)
            OvText(L, OVF_REG, inL, textY(y + i * mt.lineH, mt.lineH, fReg), lines[i].c_str(), col);
    }
    y += mt.helpLines * mt.lineH + px / 2;

    // Napoveda klaves (bez hacku plati nahradni klavesy, viz MenuPollFallback); co se nevejde, vypadne (Del, Enter)
    {
        struct Hint {
            const wchar_t* key;
            StrId label;
        };
        std::vector<Hint> h;
        if (noHook)
            h = {{L"I K", S_HINT_SELECT}, {L"J L", S_HINT_VALUE}, {L"O", S_HINT_APPLY}};
        else
            h = {{L"↑↓", S_HINT_SELECT},
                 {L"←→", S_HINT_VALUE},
                 {L"Enter", S_HINT_APPLY},
                 {L"Del", S_HINT_DEFAULT},
                 {L"Esc", S_HINT_CLOSE}};
        auto total = [&]() {
            int w = 0;
            for (const Hint& x : h) w += keyHintW(x.key, Tr(x.label));
            return w - px;
        };
        while (h.size() > 3 && total() > inR - inL) h.erase(h.end() - 2);  // Del, pak Enter (Esc zustava)
        int x = inL;
        for (const Hint& hh : h) x = keyHint(x, y, mt.footH, hh.key, Tr(hh.label));
    }
}

// OSD: jeden radek vlevo nahore (pod HUD DXVK), alpha = stmivani
static void OsdBuild(OvList& L, UINT w, UINT h, int px, float alpha) {
    L.Clear();
    std::wstring text;
    bool warn = false;
    if (g_osd.what == OSD_FAIL_FG || g_osd.what == OSD_FAIL_BRIDGE || g_osd.what == OSD_SAFESTART ||
        g_osd.what == OSD_DXVK) {
        text = std::wstring(Tr(g_osd.what == OSD_FAIL_FG       ? (g_osdFgRetry ? S_OSD_FAIL_FG_RETRY : S_OSD_FAIL_FG)
                               : g_osd.what == OSD_FAIL_BRIDGE ? S_OSD_FAIL_BRIDGE
                               : g_osd.what == OSD_SAFESTART   ? S_OSD_SAFESTART
                                                               : S_OSD_DXVK));
        if (g_osd.what != OSD_DXVK) text += L" (" + MenuKeyLabel() + L")";
        warn = true;
    } else if (g_osd.what == OSD_WARN_B || g_osd.what == OSD_SAVE_FAIL || g_osd.what == OSD_WARN_MSAA ||
        g_osd.what == OSD_FG_RESTART || g_osd.what == OSD_FG_SUBST) {
        text = Tr(g_osd.what == OSD_WARN_B      ? S_OSD_WARN_B
                  : g_osd.what == OSD_WARN_MSAA ? S_OSD_WARN_MSAA
                  : g_osd.what == OSD_FG_RESTART ? S_OSD_FG_RESTART
                  : g_osd.what == OSD_FG_SUBST ? S_OSD_FG_SUBST
                                                : S_OSD_SAVE_FAIL);
        warn = true;
    } else if (g_osd.what == OSD_HINT) {
        text = g_hintStatus.empty() ? std::wstring(Tr(S_OSD_HINT)) + MenuKeyLabel()
                                    : L"bms-dlss: " + g_hintStatus + L" · " + MenuKeyLabel();
    } else if (g_osd.what == OSD_RENDER) {
        text = g_osdRender;
    } else if (g_osd.what == OSD_REPORT || g_osd.what == OSD_REPORT_FAIL) {
        text = std::wstring(Tr(g_osd.what == OSD_REPORT ? S_OSD_REPORT : S_OSD_REPORT_FAIL)) + g_reportName;
        warn = g_osd.what == OSD_REPORT_FAIL;
    } else if (const MenuItem* it = MenuItemByOsd(g_osd.what)) {
        // (polozka "Rezim" v sekci Frame Generation: v OSD nazev sekce, "Rezim: DLSS-G" by nerikalo, co se zmenilo)
        text = std::wstring(Tr(g_osd.what == OSD_FG ? S_SEC_FG : it->label)) + L": " +
               MenuValueText(*it, it->get ? it->get() : -1);
        if (g_osd.what == OSD_OUTSCALE && DlssRunning() && !g_pathB &&
            fabsf(Interop_ActiveOutScale() - g_outScale) > 0.01f)
            text += L" …";  // helper novou velikost jeste nepotvrdil
    }
    if (text.empty()) return;
    const MenuColors C(alpha);
    const int pad = px * 3 / 4, lineH = (int)lroundf((float)px * 1.6f), bar = std::max(3, px / 5);
    text = OvFit(OVF_REG, text, (int)w - 4 * px - 2 * pad - bar);
    const int tw = OvTextW(OVF_REG, text.c_str());
    const int x0 = px * 2, y0 = std::max(px * 3, (int)h / 9);
    const int x1 = x0 + bar + tw + 2 * pad, y1 = y0 + lineH;
    const OvSprite& row = g_ovAtlas.spr[OVS_ROW];
    OvNine(L, x0, y0, x1, y1, row, C.panel, true);
    OvRect(L, x0, y0 + row.n, x0 + bar, y1 - row.n, warn ? C.warn : C.accent);
    OvText(L, OVF_REG, x0 + bar + pad, y0 + (lineH - g_ovAtlas.font[OVF_REG].height) / 2, text.c_str(),
           warn ? C.warn : C.text);
}

// Radek s verzemi ([menu] OverlayVersions): skutecne bezici upscaler a FG s verzi (stejne zdroje jako stavovy radek
// menu: verze knihovny, kterou helper opravdu nacetl) a nasobkem, napr. "DLSS 310.9.16 · DLSS-G 310.9.16 4×",
// "FSR 3.1.6 · FSR FG 3.1.6 2×", "DLSS 310.9.16 · FG vyp".
static std::wstring OverlayVersionsText() {
    std::wstring up;
    if (Interop_GetMode() == BMSD_MODE_DLAA) {
        const char* name = nullptr;
        Interop_ActiveUpscaler(&name);
        up = Interop_DlaaReady() && name && *name ? MenuW(name) : std::wstring(Tr(S_ST_DLSS_WAIT));
    } else {
        up = Tr(S_ST_DLSS_OFF);
    }
    PresentFgInfo fi;
    Present_FgInfo(&fi);
    std::wstring fg;
    if (fi.requested == BMSD_FG_OFF || !fi.presenter || fi.fallback || !fi.helperReady) {
        fg = Tr(S_OV_FG_OFF);
    } else if (fi.state == BMSD_FGS_ACTIVE && *fi.name) {
        wchar_t m[16];
        if (fi.activeMode == BMSD_FG_FSRADAPT || fi.activeMode == BMSD_FG_DLSSGADAPT)  // adaptivni: zadny pevny nasobek - skutecny pomer vystup / hra
            swprintf(m, 16, fi.realFps > 1 && fi.outFps > fi.realFps ? L" ~%.1f×" : L"",
                     (double)fi.outFps / (double)(fi.realFps > 1 ? fi.realFps : 1));
        else
            swprintf(m, 16, L" %d×", fi.activeMult >= 2 ? fi.activeMult : 2);
        fg = MenuW(fi.name) + m;
    } else {
        fg = (*fi.name ? MenuW(fi.name) : std::wstring(L"FG")) + L" (" + Tr(S_OV_PAUSED) + L")";
    }
    return up + L" · " + fg;
}

// Roh vpravo nahore: ukazatel FPS ([menu] FpsCounter) "FPS 78 / 156" (skutecne / zobrazene; bez generovanych snimku
// jedno cislo) a pod nim radek s verzemi ([menu] OverlayVersions; kazdy jde i sam). Kresli se do backbufferu po
// kopii HUD-less (= UI, generator ho neposouva se scenou); s FG prezentuje helper i generovane snimky, cisla FPS jsou
// z jeho citacu (Present_FpsGet). Pridava do L (za OSD).
// v103g: diagnostika plynulosti ([menu] DiagOverlay) - cislo s desetinnou carkou (cestina)
static std::wstring DgNum(double v, int dec) {
    wchar_t b[32];
    swprintf(b, 32, L"%.*f", dec, v);
    if (g_menuCfg.lang == 0)
        for (wchar_t* p = b; *p; ++p)
            if (*p == L'.') *p = L',';
    return b;
}
// Radek stavu FG pro diagnostiku: "FG ~2,0× · generuje · <duvod>", "FG · jen skutecne · <duvod>", "FG vyp"
static std::wstring DiagFgText() {
    PresentFgInfo fi;
    Present_FgInfo(&fi);
    if (fi.requested == BMSD_FG_OFF || !fi.presenter || fi.fallback || !fi.helperReady) return Tr(S_OV_FG_OFF);
    if (fi.state != BMSD_FGS_ACTIVE)
        return (*fi.name ? MenuW(fi.name) : std::wstring(L"FG")) + L" (" + Tr(S_OV_PAUSED) + L")";
    const bool adapt = fi.activeMode == BMSD_FG_FSRADAPT || fi.activeMode == BMSD_FG_DLSSGADAPT;
    if (!adapt) return L"FG " + DgNum(fi.activeMult >= 2 ? fi.activeMult : 2, 0) + L"× · " + Tr(S_PFS_GEN);
    wchar_t r[160];
    FgPrefInfo(r, 160);
    PresentFgPref pi;
    Present_FgPrefInfo(&pi);
    std::wstring m = L"FG";
    if (pi.gen && fi.realFps > 1.0f && fi.outFps > fi.realFps) m += L" ~" + DgNum((double)fi.outFps / fi.realFps, 1) + L"×";
    return m + L" · " + r;
}
// Graf intervalu obrazu na monitoru (sloupce, nejnovejsi vpravo): vyska = interval, mez nahore 2,5 x median; mimo pasmo
// 0,75-1,33 x median zlute, nad 2,5 x cervene; tenka cara = median.
static void DiagGraph(OvList& L, int x0, int y0, int x1, int y1, const PresentDiag& d, const MenuColors& C) {
    const OvSprite& row = g_ovAtlas.spr[OVS_ROW];
    OvNine(L, x0, y0, x1, y1, row, C.panel, true);
    const int pad = std::max(2, (int)row.n);
    const int gx0 = x0 + pad, gx1 = x1 - pad, gy0 = y0 + pad, gy1 = y1 - pad;
    if (gx1 - gx0 < 8 || gy1 - gy0 < 8 || d.n <= 0 || d.ivMed <= 0.0f) return;
    const int bw = std::max(2, (gx1 - gx0) / 150);
    const int nb = std::min(d.n, (gx1 - gx0) / bw);
    const double top = 2.5 * d.ivMed;
    const int ym = gy1 - (int)lround((gy1 - gy0) * (d.ivMed / top));
    OvRect(L, gx0, ym, gx1, ym + 1, C.dim);
    for (int i = 0; i < nb; ++i) {
        const float v = d.iv[d.n - nb + i];
        const int hgt = std::max(1, (int)lround((gy1 - gy0) * std::min(1.0, v / top)));
        const int bx = gx1 - (nb - i) * bw;
        const DWORD col = v > 2.5f * d.ivMed ? C.err : v > 1.33f * d.ivMed || v < 0.75f * d.ivMed ? C.warn : C.accent;
        OvRect(L, bx, gy1 - hgt, bx + bw - (bw > 2 ? 1 : 0), gy1, col);
    }
}

static void CornerBuild(OvList& L, UINT w, UINT h, int px, bool fps, bool ver, bool dg) {
    wchar_t ftext[48] = L"";
    if (fps) {
        float real = 0, total = 0;
        bool split = false;
        if (!Present_FpsGet(&real, &total, &split)) fps = false;
        else if (split) swprintf(ftext, 48, L"FPS %ld / %ld", lroundf(real), lroundf(total));
        else swprintf(ftext, 48, L"FPS %ld", lroundf(total));
    }
    const std::wstring vtext = ver ? OvFit(OVF_REG, OverlayVersionsText(), (int)w / 2) : std::wstring();
    PresentDiag dd;
    if (dg && !Present_DiagGet(&dd)) dg = false;
    if (!fps && vtext.empty() && !dg) return;
    const MenuColors C(1.0f);
    const int pad = px / 2, lineH = (int)lroundf((float)px * 1.4f);
    const int x1 = (int)w - px;
    int y = px;
    const OvSprite& row = g_ovAtlas.spr[OVS_ROW];
    auto line = [&](const wchar_t* t, DWORD col) {
        const int tw = OvTextW(OVF_REG, t);
        const int x0 = x1 - tw - 2 * pad;
        if (x0 < 0 || y + lineH >= (int)h) return;
        OvNine(L, x0, y, x1, y + lineH, row, C.panel, true);
        OvText(L, OVF_REG, x0 + pad, y + (lineH - g_ovAtlas.font[OVF_REG].height) / 2, t, col);
        y += lineH + px / 4;
    };
    if (fps) line(ftext, C.text);
    if (!vtext.empty()) line(vtext.c_str(), C.dim);
    if (dg) {  // v103g: diagnostika plynulosti
        wchar_t t[160];
        const std::wstring g = DgNum(dd.gameFps, 0), m = DgNum(dd.monFps, 0);
        swprintf(t, 160, Tr(dd.presenter ? S_DG_L1 : S_DG_DXVK), g.c_str(), m.c_str());
        line(t, C.text);
        if (dd.presenter && dd.skipped >= 0) {
            const int tot = dd.skipped + std::max(0, dd.shown);
            const std::wstring pc = DgNum(tot > 0 ? 100.0 * dd.skipped / tot : 0.0, 1);
            swprintf(t, 160, Tr(S_DG_L2), dd.skipped, pc.c_str());
            line(t, dd.skipped > 0 ? C.warn : C.text);
        } else {
            line(Tr(S_DG_L2NA), C.dim);
        }
        line(OvFit(OVF_REG, DiagFgText(), (int)w / 2).c_str(), C.text);
        const std::wstring a = DgNum(dd.ivMed, 1), b = DgNum(dd.ivSd, 2);
        swprintf(t, 160, Tr(S_DG_IV), a.c_str(), b.c_str(), dd.ivShort, dd.ivLong);
        line(t, dd.ivSd > 1.0f || dd.ivShort + dd.ivLong > 0 ? C.warn : C.text);
        const int gw = std::min((int)w / 3, std::max(px * 14, 300)), gh = lineH * 3;
        if (y + gh < (int)h && x1 - gw >= 0) {
            DiagGraph(L, x1 - gw, y, x1, y + gh, dd, C);
            y += gh + px / 4;
        }
    }
}

// Vsechno, co se ma kreslit pres obraz hry (menu nebo OSD), jako seznam ctyruhelniku pro cil w x h.
// Na cili (backbuffer / vrstva pro FG) nezavisi. false = neni co kreslit.
static bool OverlayBuild(IDirect3DDevice9* d, UINT w, UINT h) {
    MenuState& M = g_menu;
    bool osd = false;
    float osdA = 1.0f;
    if (!M.open && g_osd.what >= 0) {
        const ULONGLONG now = GetTickCount64();
        if (now >= g_osd.until) {
            g_osd.what = -1;
        } else {
            osd = true;
            const ULONGLONG left = g_osd.until - now;
            if (left < 300) osdA = (float)left / 300.0f;
        }
    }
    float fr, ft;
    bool fs;
    const bool fps = !M.open && g_menuCfg.fpsCounter && Present_FpsGet(&fr, &ft, &fs);
    const bool ver = !M.open && g_menuCfg.overlayVersions;
    const bool dg = !M.open && g_menuCfg.diag;
    if (!M.open && !osd && !fps && !ver && !dg) return false;
    int px = MenuBasePx(h);
    if (M.open) {
        // pismo: zmensit, dokud se panel nevejde (az na 12 px), pak se seznam posouva
        const int nStatus = MenuStatusLines();
        auto fits = [&](int p) {
            const MenuMetrics m(p);
            return m.Height(MenuListHeight(m), nStatus) <= (int)((float)h * 0.94f) && m.Width(w) >= p * 40;
        };
        while (px > 12 && !fits(px)) --px;
    } else if (g_ovAtlas.px && abs(g_ovAtlas.px - px) * 4 <= px) {
        px = g_ovAtlas.px;  // OSD: atlas menu staci (bez prestavby kvuli par pixelum)
    }
    if (g_ovAtlas.px != px) {
        if (g_ovAtlasFails >= kOvAtlasMaxFails) return false;  // vzdano (D9)
        const ULONGLONG t = GetTickCount64();
        if (g_ovAtlasFails && t < g_ovAtlasRetryAt) return false;  // GDI neopakovat kazdy snimek
        if (!OvAtlasBuild(px)) {
            g_ovAtlasRetryAt = t + kOvAtlasRetryMs;
            return false;
        }
        M.dirty = true;
    }
    if (!OvEnsureTexture(d)) return false;
    if (!M.open) {
        if (osd) OsdBuild(M.list, w, h, px, osdA);
        else M.list.Clear();
        if (fps || ver || dg) CornerBuild(M.list, w, h, px, fps, ver, dg);
        M.dirty = true;  // menu se pri otevreni prestavi
    } else if (M.dirty || M.builtW != w || M.builtH != h || M.frames - M.builtFrame >= 20) {
        MenuBuild(M.list, w, h, px);
        M.builtW = w;
        M.builtH = h;
        M.builtFrame = M.frames;
        M.dirty = false;
    }
    return !M.list.idx.empty();
}

// Test: backbuffer (po nakresleni menu, pred Present) do bms_dlss_menu_shot_N.bmp vedle proxy.
static void MenuSaveShot(IDirect3DDevice9* d, IDirect3DSurface9* bb, const D3DSURFACE_DESC& desc) {
    MenuState& M = g_menu;
    IDirect3DSurface9* sys = nullptr;
    HRESULT hr = d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr) && sys) hr = d->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT lr = {};
    if (SUCCEEDED(hr) && sys) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    wchar_t path[MAX_PATH + 48];
    swprintf(path, MAX_PATH + 48, L"%ls\\bms_dlss_menu_shot_%d.bmp", g_dir, M.shotIndex);
    bool ok = false;
    if (SUCCEEDED(hr) && lr.pBits && (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8)) {
        if (FILE* f = _wfopen(path, L"wb")) {
            const UINT rowBytes = (desc.Width * 3 + 3) & ~3u;
            BITMAPFILEHEADER fh = {};
            BITMAPINFOHEADER ih = {};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + rowBytes * desc.Height;
            ih.biSize = sizeof ih;
            ih.biWidth = (LONG)desc.Width;
            ih.biHeight = (LONG)desc.Height;  // zdola nahoru
            ih.biPlanes = 1;
            ih.biBitCount = 24;
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            std::vector<uint8_t> row(rowBytes, 0);
            for (UINT y = desc.Height; y-- > 0;) {
                const uint8_t* s = (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch;
                for (UINT x = 0; x < desc.Width; ++x) memcpy(&row[x * 3], s + x * 4, 3);
                fwrite(row.data(), rowBytes, 1, f);
            }
            fclose(f);
            ok = true;
        }
    }
    if (lr.pBits) sys->UnlockRect();
    if (sys) sys->Release();
    if (ok)
        Log("menu: test - snimek %d: %ux%u, menu %s (stranka %d), atlas %d px hash %08X -> %ls", M.shotIndex,
            desc.Width, desc.Height, M.open ? "otevrene" : "zavrene", M.page, g_ovAtlas.px, g_ovAtlas.hash, path);
    else
        Log("WARN: menu: test - snimek %d se nepodarilo ulozit (0x%08X, format %d)", M.shotIndex, (unsigned)hr,
            (int)desc.Format);
    ++M.shotIndex;
}

// OnFrameEnd, uplne nakonec (po jitteru a mip biasu pro dalsi snimek): prekryv do cile podle sinku. Zavrene menu bez
// OSD = jedna podminka, zadny draw, textura ani state block.
static void MenuFrameDraw(IDirect3DDevice9* d) {
    MenuState& M = g_menu;
    if (g_menuCfg.fpsCounter && g_menuCfg.enable) Present_FpsTick();  // ukazatel FPS: pocitat kazdy snimek
    Present_DiagTick(g_menuCfg.diag && g_menuCfg.enable);             // v103g: diagnostika plynulosti (casy Present)
    const bool corner = (g_menuCfg.fpsCounter || g_menuCfg.overlayVersions || g_menuCfg.diag) && g_menuCfg.enable;
    if ((!M.open && !M.shotPending && g_osd.what < 0 && !corner) || !d) return;
    if (OverlayPickSink() != SINK_BACKBUFFER) return;  // SINK_LAYER: az s Frame Generation
    // (originalni funkce zarizeni zna proxy az po HookDevice; bez nich se nekresli a menu se samo zavre nize)
    const bool hooksOk = o_SetRT && o_SetDS && o_SetViewport && o_SetVS && o_SetFVF && o_SetStreamSourceFreq &&
                         o_SetRenderState && o_SetTexture && o_SetSamplerState && o_DrawIdxPrimUP;
    IDirect3DSurface9* bb = nullptr;
    bool drawn = false;
    if (hooksOk && SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(bb->GetDesc(&desc)) && desc.Width && desc.Height) {
            if (OverlayBuild(d, desc.Width, desc.Height))
                drawn = OverlayRender(d, bb, desc.Width, desc.Height, M.list);
            if (M.shotPending) {
                --M.shotPending;
                MenuSaveShot(d, bb, desc);
            }
        }
        bb->Release();
    }
    // Menu, ktere nejde nakreslit (atlas, textura, state block), nesmi zustat otevrene: polykalo by klavesy naslepo
    if (!M.open || drawn) {
        M.drawFails = 0;
    } else if (++M.drawFails >= 60) {
        M.drawFails = 0;
        Log("WARN: menu: 60 snimku po sobe se nepodarilo nakreslit - zaviram ho");
        MenuClose("kresleni selhalo");
    }
}

// Pred Reset/ResetEx hlavniho zarizeni: textura v D3DPOOL_DEFAULT se musi pustit (MANAGED Reset prezije).
static void MenuBeforeReset() {
    if (g_ovAtlas.tex && g_ovAtlas.texDefaultPool) OvReleaseTexture();
    g_menu.dirty = true;
}

// Nove hlavni zarizeni: textura patri staremu.
static void MenuOnNewMainDevice() {
    OvReleaseTexture();
    g_ovAtlas.texFailLogged = false;
    g_ovStateFailLogged = false;
    g_menu.dirty = true;
}
