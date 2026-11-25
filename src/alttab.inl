// Alt+Tab: celoobrazovkovy flip swapchainu DXVK rizeny proxy (soucast present.cpp, v103i).
// Hra na cele obrazovce po ztrate popredi minimalizuje okno a hned prestane kreslit. Ovladac celoobrazovkovy flip uvolni
// jen podle okna nejvyssi urovne; swapchain DXVK na dcerinem okne proxy ([present] DxvkFse=1, [fg] OffWindow=1) drzel
// vystup monitoru dal a posledni snimek hry zustal pres plochu ("Alt+Tab nejde"). Proto ma swapchain DXVK na dcerinem
// okne rezim APPLICATION_CONTROLLED (VK_EXT_full_screen_exclusive): vylucny rezim ziska a uvolni vlakno oken proxy podle
// popredi (nezavisi na vlaknech hry ani na tom, jestli hra kresli). Po navratu ho ziska znovu = zase flip.
// POKUSNE, vychozi vypnuto ([present] DxvkFse=1 + AltTabRelease=1): Alt+Tab pak funguje (39 ze 40), ale pri rychlem navratu
// ovladac NVIDIA obcas spadne (nvoglv32 +8b2846, vlastni vlakno ovladace, 2x v 6 bezich). Bez funkci rozsireni DISALLOWED
// (kompozice, Alt+Tab spolehlivy). Presenter FG (gamepresent.inl) svuj swapchain pri minimalizaci nici sam.

int g_atRelease;

struct AtDev {
    VkDevice dev;
    PFN_vkCreateSwapchainKHR create;
    PFN_vkDestroySwapchainKHR destroy;
    PFN_vkAcquireFullScreenExclusiveModeEXT acquireFse;
    PFN_vkReleaseFullScreenExclusiveModeEXT releaseFse;
};
struct AtSc {  // swapchainy s rizenym vylucnym rezimem
    AtDev* ad;
    VkSwapchainKHR sc;
    HWND wnd;
    bool held;   // vylucny rezim drzen
    bool busy;   // vlakno oken je prave v ovladaci
    bool dying;  // DXVK ho chtel znicit behem busy: znici ho vlakno oken
    ULONGLONG retry;  // po neuspechu dalsi pokus az po tomto case
    LONG frame0, need;  // ziskat nejdriv need snimku po frame0 (g_atFrames)
};
AtDev g_atDev[8];
AtSc g_atSc[8];
CRITICAL_SECTION g_atCs;
CONDITION_VARIABLE g_atCv;
volatile LONG g_atCsInit;
volatile LONG g_atLostTop;  // hra neni v popredi, ale neminimalizovala se (nacitani) a kryje monitor okna v popredi (vlakno oken)
volatile LONG g_atFrames;  // snimky, ktere DXVK dostal do dcerineho okna (vlakno renderu)
// Novy swapchain: ziskat az po 2 snimcich. Po uvolneni (Alt+Tab) si DXVK pri navratu vytvori novy; stary se neziskava,
// ledaze by ho DXVK pouzival dal (30 snimku po navratu) - vylucny rezim nad prave nahrazovanym swapchainem = pad hry.
const LONG kAtNewFrames = 2, kAtOldFrames = 30;
ULONGLONG g_atLostAt;  // hra neni v popredi / minimalizovana od (0 = je v popredi); jen vlakno oken

struct AtLock {
    AtLock() {
        if (InterlockedCompareExchange(&g_atCsInit, 1, 0) == 0) {
            InitializeCriticalSection(&g_atCs);
            InitializeConditionVariable(&g_atCv);
            InterlockedExchange(&g_atCsInit, 2);
        }
        while (g_atCsInit != 2) Sleep(0);
        EnterCriticalSection(&g_atCs);
    }
    ~AtLock() { LeaveCriticalSection(&g_atCs); }
};

AtDev* AtDevOf(VkDevice dev) {
    for (AtDev& d : g_atDev)
        if (d.dev == dev) return &d;
    return nullptr;
}
AtDev* AtDevGet(VkDevice dev) {
    if (AtDev* d = AtDevOf(dev)) return d;
    for (AtDev& d : g_atDev)
        if (!d.dev) {
            d.dev = dev;
            return &d;
        }
    return nullptr;
}
bool AtChildWnd(HWND w) {
    wchar_t cls[64] = L"";
    return w && GetClassNameW(w, cls, 64) && !wcscmp(cls, L"bms_dlss_s2_child");
}
bool AtScAdd(AtDev* ad, VkSwapchainKHR sc, HWND wnd) {
    AtLock lk;
    for (AtSc& x : g_atSc)
        if (!x.ad) {
            x = AtSc();
            x.ad = ad;
            x.sc = sc;
            x.wnd = wnd;
            x.frame0 = g_atFrames;
            x.need = kAtNewFrames;
            return true;
        }
    return false;
}

// DXVK swapchain prave nahrazuje / nici: pockat na vlakno oken a dal na nej nesahat. false = vlakno oken je porad
// v ovladaci (ceka na okno, vlakno okna mozna na nas).
bool AtForget(VkSwapchainKHR sc, bool destroy) {
    AtLock lk;
    for (AtSc& e : g_atSc) {
        if (!e.ad || e.sc != sc) continue;
        const ULONGLONG t0 = GetTickCount64();
        while (e.busy && GetTickCount64() - t0 < 2000) SleepConditionVariableCS(&g_atCv, &g_atCs, 100);
        if (e.busy) {
            e.dying = e.dying || destroy;
            Log("alt+tab: WARN - vlakno oken je pres 2 s v ovladaci (%s swapchainu)", destroy ? "zniceni" : "nahrazeni");
            return false;
        }
        e = AtSc();
    }
    return true;
}

VKAPI_ATTR void VKAPI_CALL AtDestroySwapchain(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* al) {
    AtDev* ad = AtDevOf(dev);
    if (!ad || !ad->destroy) return;
    if (AtForget(sc, true)) ad->destroy(dev, sc, al);  // jinak ho znici vlakno oken, az se vrati z ovladace
}

// Ziskat / uvolnit vylucny rezim jednoho swapchainu (vlakno oken; zamek se behem volani ovladace nedrzi)
void AtFse(AtSc& e, bool want) {
    const AtSc c = e;
    e.busy = true;
    LeaveCriticalSection(&g_atCs);
    const LONGLONG t0 = Qpc();
    const VkResult r = want ? c.ad->acquireFse(c.ad->dev, c.sc) : c.ad->releaseFse(c.ad->dev, c.sc);
    const double ms = Ms(t0, Qpc());
    EnterCriticalSection(&g_atCs);
    e.busy = false;
    if (want && r != VK_SUCCESS) {
        e.retry = GetTickCount64() + 500;
    } else {
        e.held = want;
        e.frame0 = g_atFrames;
        e.need = kAtOldFrames;
    }
    static unsigned s_n;
    if (s_n < 400 && (r == VK_SUCCESS || s_n < 40)) {
        ++s_n;
        Log("alt+tab: celoobrazovkovy flip DXVK %s za %.1f ms -> %d", want ? "ziskan" : "uvolnen", ms, (int)r);
    }
    if (e.dying) {
        const AtSc d = e;
        e = AtSc();
        d.ad->destroy(d.ad->dev, d.sc, nullptr);
    }
    WakeAllConditionVariable(&g_atCv);
}

// Okno hry pres cely monitor (v okne se vylucny rezim neziskava)
bool AtCovers() {
    MONITORINFO mi = {sizeof mi};
    RECT cr = {};
    POINT p = {};
    HMONITOR mon = MonitorFromWindow(g_gameWnd, MONITOR_DEFAULTTONULL);
    return mon && GetMonitorInfoW(mon, &mi) && GetClientRect(g_gameWnd, &cr) && ClientToScreen(g_gameWnd, &p) &&
           p.x <= mi.rcMonitor.left && p.y <= mi.rcMonitor.top && p.x + cr.right >= mi.rcMonitor.right &&
           p.y + cr.bottom >= mi.rcMonitor.bottom;
}

// Drzi tento proces vystup monitoru vyhradne? (jadro grafiky; samokontrola)
bool AtOwnsOutput() {
    struct Q {
        HANDLE process;
        UINT source;
        LUID luid;
        UINT owner;
    };
    typedef LONG(WINAPI * Fn)(Q*);
    static Fn s_fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTQueryVidPnExclusiveOwnership");
    Q q = {GetCurrentProcess()};
    return s_fn && s_fn(&q) == 0 && q.owner >= 2;
}

void AtTick() {
    if (!g_gameWnd || !cfg.on) return;
    DWORD fp = 0;
    const HWND fw = GetForegroundWindow();
    if (fw) GetWindowThreadProcessId(fw, &fp);
    const bool icon = IsIconic(g_gameWnd) != FALSE;
    const bool lost = icon || fp != GetCurrentProcessId();
    const ULONGLONG now = GetTickCount64();
    static bool s_icon;
    if (lost != (g_atLostAt != 0) || icon != s_icon)
        Log("alt+tab: okno hry %s%s", lost ? "neni v popredi" : "v popredi", icon ? ", minimalizovane" : "");
    s_icon = icon;
    if (!lost) g_atLostAt = 0;
    else if (!g_atLostAt) g_atLostAt = now;
    // Pri nacitani se hra po Alt+Tabu neminimalizuje a kresli dal: flip by drzel jeji obraz pres okno v popredi. Po 250 ms
    // presenter i DXVK swapchain znovu bez flipu (mimo popredi = kompozice), po navratu zase s flipem.
    const LONG top = lost && !icon && fw && now - g_atLostAt >= 250 && IsWindowVisible(g_gameWnd) && AtCovers() &&
                     MonitorFromWindow(fw, MONITOR_DEFAULTTONULL) == MonitorFromWindow(g_gameWnd, MONITOR_DEFAULTTONULL);
    if (InterlockedExchange(&g_atLostTop, top) != top && top)
        Log("alt+tab: hra se neminimalizovala - zobrazeni zatim bez flipu");
    // Hra, ktera se po ztrate popredi jinak minimalizuje, zustala nahore (vzacne): minimalizovat ji, jednou za ztratu popredi
    static bool s_mins, s_forced;
    if (icon) s_mins = true;
    if (!top) s_forced = false;
    if (top && s_mins && !s_forced && now - g_atLostAt >= 700) {
        s_forced = true;
        ShowWindowAsync(g_gameWnd, SW_MINIMIZE);
        Log("alt+tab: hra se ani po 0,7 s neminimalizovala - minimalizuji");
    }
    {
        AtLock lk;
        const HWND child = g_child;
        const PState st = g_state;
        RECT cr = {}, gr = {};
        const bool full = child && !lost && IsWindowVisible(child) && GetClientRect(child, &cr) &&
                          GetClientRect(g_gameWnd, &gr) && cr.right == gr.right && cr.bottom == gr.bottom && AtCovers();
        const bool dx = st == PS_OFF || st == PS_STARTING || (st == PS_FALLBACK && g_fbOffWin);
        for (AtSc& e : g_atSc) {
            if (!e.ad || e.busy) continue;
            if (lost) e.frame0 = g_atFrames;
            // Uvolnit az po minimalizaci okna hry: uvolneni mezi ztratou popredi a minimalizaci (ovladac pri nem saha na
            // okno) hre minimalizaci nekdy prerusilo a jeji okno zustalo nahore (1 z 12). Hra, ktera se neminimalizuje
            // (nacitani, zaseknute hlavni vlakno): po 1 s mimo popredi.
            if (lost && !icon && now - g_atLostAt < 1000) continue;
            const bool want = full && dx && e.wnd == child && (e.held || g_atFrames - e.frame0 >= e.need);
            if (want != e.held && (!want || now >= e.retry)) AtFse(e, want);
        }
    }
    // Samokontrola: hra > 1 s neni v popredi (minimalizovana) a proces hry dal drzi vystup monitoru / swapchain flip
    if (g_scOn && icon && g_atLostAt && now - g_atLostAt > 1000) {
        static ULONGLONG s_chk;
        static unsigned s_bad;
        if (now - s_chk >= 500) {
            s_chk = now;
            bool held = false;
            {
                AtLock lk;
                for (const AtSc& e : g_atSc) held |= e.ad && e.held;
            }
            const bool own = AtOwnsOutput();
            const HWND c = g_child;
            const bool vis = c && (GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE);
            // neridene dcerine okno s flipem (AltTabRelease=0): ovladac ho po minimalizaci neuvolni (zmereno 12 z 12)
            RECT cr = {};
            const bool unc = g_dxFse == 1 && !g_atRelease && vis && GetClientRect(c, &cr) && cr.right > 16 &&
                             (g_state == PS_OFF || (g_state == PS_FALLBACK && g_fbOffWin));
            s_bad = own || held || unc || g_gpSc ? s_bad + 1 : 0;
            if (s_bad >= 2)
                ScReport(SC_ALTTAB, "hra je minimalizovana, ale jeji obraz dal drzi vystup monitoru (Alt+Tab) | mimo popredi "
                         "%.1f s, vystup drzi proces hry %s, rizeny flip DXVK %s, swapchain presenteru %s, dcerine okno "
                         "DXVK %s, stav %d",
                         (now - g_atLostAt) / 1000.0, own ? "ANO" : "ne", held ? "DRZEN" : "ne", g_gpSc ? "EXISTUJE" : "ne",
                         vis ? "viditelne" : "skryte / zadne", (int)g_state);
        }
    }
}

void CALLBACK AtWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) { AtTick(); }
