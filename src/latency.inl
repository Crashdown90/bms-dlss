// Adaptivni obdoba NVIDIA Reflexu (ini [latency], klavesa Alt+F12) - vklada ho proxy.cpp. Navrh a rizika:
// docs\navrh-reflex.md.
//
// Princip: kazdy snimek se pred Present vyda D3D9 event query (+ dve casove znacky GPU: zacatek a konec snimku).
// Kdyz je hra omezena GPU (fronta: pri odeslani snimku n jeste GPU nedokoncilo snimek n-1, v >= 90 % snimku),
// proxy po Present pocka, az GPU dokonci snimek n-1, a pak jeste uspi vlakno o D ms, takze dalsi snimek se zacne
// pocitat co nejpozdeji (vstup cerstvejsi), ale stale o "okraj" m drive, nez GPU dodela snimek n (GPU nema hlad).
// D se ridi integracne: cekani W na snimek n-1 namerene v dalsim Present ma byt rovno m. Pod stropem FPS nebo kdyz
// je hra omezena CPU je fronta prazdna -> proxy nic nedela (jen 1 event + 2 casove znacky za snimek v rezimu 1/2).
// Pri jakekoli nejistote (okno neni v popredi, Reset, chyba dotazu, timeout, GPU necinne) se samo vypne.
//
// Znacky pro pozdejsi skutecny Reflex v helperu (VK_NV_low_latency2 / Streamline sl.reflex):
//   tBegin  = SIMULATION_START (zde: zacatek prace vlakna D3D9 na snimku, po nasem spanku)
//   tSubmit = RENDERSUBMIT_END / PRESENT_START (volani Present hrou)
//   tsB/tsE = GPU zacatek a konec snimku (casove znacky D3D9 = vkCmdWriteTimestamp v DXVK)
//   tDone   = kdy CPU poprve videlo snimek dokonceny (horni mez konce GPU)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

enum { LAT_OFF = 0, LAT_ADAPTIVE = 1, LAT_MEASURE = 2 };
enum { LS_IDLE = 0, LS_ACTIVE = 1 };

static const char* LatModeName(int m) {
    return m == LAT_ADAPTIVE ? "adaptivni" : m == LAT_MEASURE ? "jen mereni" : "vypnuto";
}

static const int kLatRing = 8;
struct LatSlot {
    IDirect3DQuery9 *ev, *tsBeg, *tsEnd;
    uint64_t n;         // poradove cislo snimku (g_latN)
    LONGLONG tBegin;    // zacatek prace CPU na snimku (navrat z predchoziho Present po spanku); 0 = nezname
    LONGLONG tSubmit;   // Present hry
    LONGLONG tDone;     // prvni pozorovani dokonceni na CPU (horni mez konce GPU)
    UINT64 tsB, tsE;    // casove znacky GPU (tiky)
    bool issued;        // event pro snimek n vydan
    bool evPending;     // ... a jeste nevideny jako dokonceny
    bool begIssued;     // casova znacka zacatku vydana (po predchozim Present)
    bool tsPending;     // casove znacky jeste neprectene
    bool active;        // snimek odeslan v aktivnim rezimu (pro statistiku necinnosti GPU)
    bool busy;          // pri odeslani GPU jeste pracovalo na predchozim snimku (fronta >= 1)
};

static struct {
    int mode = LAT_OFF;         // aktualni rezim (ini Mode, Alt+F12)
    int key = VK_F12;           // [keys] LatencyKey
    float marginMinMs = 0.5f;   // [latency] MarginMs: nejmensi okraj pred dokoncenim GPU
    bool keyWas;
    bool dxvkSleep;             // dxvk.conf: dxvk.latencySleep = True -> adaptivni jen meri (dva regulatory se tlucou)
    bool foreground;            // okno hry v popredi (z OnFrameEnd)
    bool fgOnly = true;         // [latency] ForegroundOnly (0 jen pro testy se skrytym oknem)
    bool res, broken, tsOk;
    double tsTicksPerMs;
    HANDLE timer;               // hi-res waitable timer (Win10 1803+); bez nej adaptivni rezim nebezi
    bool timerTried;
    LatSlot s[kLatRing];
    uint64_t n;                 // snimek, ktery hra prave kresli
    LONGLONG tBeginCur;         // zacatek aktualniho snimku (0 = nezname, napr. po Resetu)
    LONGLONG lastPoll, lastSubmit;
    // regulator
    int state = LS_IDLE;
    double D, margin, wEwma, wPresEwma, wMad, cEwma, cMad, intEwma, gapBase = -1.0;
    uint64_t busyBits, slackBits;  // poslednich 64 snimku: fronta pri odeslani / regulator bez prace
    unsigned framesOk, dwell;
    LONGLONG cooldownUntil;
    double penaltyS = 5.0;
    LONGLONG penaltyQ;          // posledni vypnuti s trestem (trest klesa: polovina za kazdych 60 s bez dalsiho)
    double lastIv;              // interval Present -> Present tohoto snimku (zaskub = nad 3x intEwma, timeout bez trestu)
    // okraj z minuleho zapnuti (nabeh po prepnuti FG, v103b): naucena hodnota, cas vypnuti, posledni okno bez hladu GPU
    double mLearned;
    LONGLONG mLearnedQ;
    bool mLearnedOk, lastWinOk;
    unsigned winFrames, winStarved;
    double winLostMs, winMs;
    unsigned strikes;
    UINT64 lastTsE;
    uint64_t lastTsEN;
    bool lastTsEValid;
    LONGLONG lastStateLog;
    bool warnedTimeout, warnedErr, warnedRing;
    // konec sceny (most): DXVK/ovladac tam muze blokovat, dokud GPU nedokonci predchozi snimek (sdilene obrazy)
    bool seSeen, seLeft, sePrevPending;
    LONGLONG seEnter;
    double seWait;
} g_lat;

// Pocitadla radku "vykon" (interval ~5 s)
static struct {
    unsigned frames, active, busy, starved, timeouts, errors, engage, disengage, waited;
    double qSum, wSum, dSum, dMax, latSum, latMax, gpuSum, gapLost;
    unsigned latN, gpuN, gapN;
    const char* lastReason;
} g_latSt;

static LONGLONG LatQpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
static double LatMs(LONGLONG a, LONGLONG b) {
    static LONGLONG f;
    if (!f) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = q.QuadPart;
    }
    return (double)(b - a) * 1000.0 / (double)f;
}
static LONGLONG LatAddMs(LONGLONG t, double ms) {
    LARGE_INTEGER q;
    QueryPerformanceFrequency(&q);
    return t + (LONGLONG)(ms * (double)q.QuadPart / 1000.0);
}

// dxvk.conf (pracovni slozka hry nebo DXVK_CONFIG_FILE): je zapnute vestavene uspavani DXVK?
static bool DxvkConfLatencySleep() {
    std::vector<std::wstring> paths;
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"DXVK_CONFIG_FILE", buf, MAX_PATH);
    if (n && n < MAX_PATH) paths.push_back(buf);
    n = GetCurrentDirectoryW(MAX_PATH, buf);
    if (n && n < MAX_PATH) paths.push_back(std::wstring(buf) + L"\\dxvk.conf");
    for (const auto& p : paths) {
        FILE* f = _wfopen(p.c_str(), L"rt");
        if (!f) continue;
        char line[512];
        bool on = false;
        while (fgets(line, sizeof line, f)) {
            std::string t;
            for (const char* c = line; *c && *c != '#'; ++c)
                if (!isspace((unsigned char)*c)) t.push_back((char)tolower((unsigned char)*c));
            if (t.rfind("dxvk.latencysleep=", 0) == 0) on = t.find("true") != std::string::npos;
        }
        fclose(f);
        if (on) {
            Log("WARN: latence: %ls ma dxvk.latencySleep = True - adaptivni rezim jen meri (dva regulatory by se tloukly)",
                p.c_str());
            return true;
        }
    }
    return false;
}

// Vychozi [latency] Mode, kdyz ho ini nema (uzivatelovo ini install.ps1 neprepisuje). Zapnuti pro vsechny = zmenit
// tento radek na LAT_ADAPTIVE (a Mode= v sablone bms_dlss.ini) - az po mereni ve hre (PROGRESS.md).
static const int kLatDefaultMode = LAT_ADAPTIVE;  // 2026-11-18: ve hre bez ztraty FPS (Blue Shift S=1.5), zapnuto vychozi

static void LatLoadConfig() {
    g_lat.mode = (int)wcstol(IniStr(L"latency", L"Mode", std::to_wstring(kLatDefaultMode).c_str()).c_str(), nullptr, 0);
    if (g_lat.mode < LAT_OFF || g_lat.mode > LAT_MEASURE) {
        Log("WARN: [latency] Mode=%d neni 0, 1 ani 2 - pouzivam 0 (vypnuto)", g_lat.mode);
        g_lat.mode = LAT_OFF;
    }
    g_lat.key = (int)wcstol(IniStr(L"keys", L"LatencyKey", L"0x7B").c_str(), nullptr, 0);
    if (!g_cfg.debugKeys) g_lat.key = 0;  // Alt+F12 jen s [keys] DebugKeys=1 (rezim je v menu)
    g_lat.marginMinMs = std::min(5.0f, std::max(0.1f, IniFloat(L"latency", L"MarginMs", 0.5f)));
    g_lat.fgOnly = wcstol(IniStr(L"latency", L"ForegroundOnly", L"1").c_str(), nullptr, 0) != 0;
    g_lat.dxvkSleep = DxvkConfLatencySleep();
    Log("config [latency]: Mode=%d (%s) MarginMs=%.2f ForegroundOnly=%d latencyKey=0x%X", g_lat.mode,
        LatModeName(g_lat.mode), g_lat.marginMinMs, (int)g_lat.fgOnly, g_lat.key);
}

static void LatResetController() {
    g_lat.state = LS_IDLE;
    g_lat.D = 0.0;
    g_lat.margin = g_lat.marginMinMs;
    g_lat.wEwma = g_lat.wMad = 0.0;
    g_lat.busyBits = g_lat.slackBits = 0;
    g_lat.framesOk = g_lat.dwell = 0;
    g_lat.winFrames = g_lat.winStarved = 0;
    g_lat.winLostMs = g_lat.winMs = 0.0;
    g_lat.strikes = 0;
    g_lat.tBeginCur = 0;
    g_lat.lastTsEValid = false;
    g_lat.seSeen = g_lat.seLeft = g_lat.sePrevPending = false;
    g_lat.seWait = 0.0;
}

// Uvolni dotazy (Reset, nove zarizeni, Mode 0). Stav regulatoru zpet na necinny.
static void LatRelease(const char* why) {
    bool had = g_lat.res;
    for (auto& s : g_lat.s) {
        if (s.ev) s.ev->Release();
        if (s.tsBeg) s.tsBeg->Release();
        if (s.tsEnd) s.tsEnd->Release();
        s = LatSlot{};
    }
    g_lat.res = false;
    g_lat.broken = false;
    g_lat.tsOk = false;
    if (g_lat.state == LS_ACTIVE) ++g_latSt.disengage;
    LatResetController();
    if (had) Log("latence: dotazy uvolneny (%s)", why);
}

static bool LatCreate(IDirect3DDevice9* d) {
    if (g_lat.res) return true;
    if (g_lat.broken) return false;
    for (auto& s : g_lat.s) {
        s = LatSlot{};
        if (FAILED(d->CreateQuery(D3DQUERYTYPE_EVENT, &s.ev)) || !s.ev) {
            Log("WARN: latence: D3DQUERYTYPE_EVENT nejde vytvorit - mereni a regulator vypnuty do Resetu");
            g_lat.res = true;  // LatRelease uvolni to, co vzniklo
            LatRelease("chyba");
            g_lat.broken = true;
            return false;
        }
    }
    // Casove znacky GPU: jen pro statistiku a pojistku "GPU necinne mezi snimky"; bez nich se pouzije jen event.
    bool ts = true;
    IDirect3DQuery9* fq = nullptr;
    UINT64 freq = 0;
    if (SUCCEEDED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &fq)) && fq) {
        fq->Issue(D3DISSUE_END);
        for (int i = 0; i < 200 && fq->GetData(&freq, sizeof freq, D3DGETDATA_FLUSH) == S_FALSE; ++i) Sleep(0);
        fq->Release();
    }
    if (!freq) ts = false;
    for (auto& s : g_lat.s) {
        if (!ts) break;
        if (FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.tsBeg)) || !s.tsBeg ||
            FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.tsEnd)) || !s.tsEnd)
            ts = false;
    }
    if (!ts)
        for (auto& s : g_lat.s) {
            if (s.tsBeg) s.tsBeg->Release();
            if (s.tsEnd) s.tsEnd->Release();
            s.tsBeg = s.tsEnd = nullptr;
        }
    g_lat.tsOk = ts;
    g_lat.tsTicksPerMs = (double)freq / 1000.0;
    g_lat.res = true;
    LatResetController();
    if (!g_lat.timerTried) {
        g_lat.timerTried = true;
        g_lat.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!g_lat.timer)
            Log("WARN: latence: presny casovac (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) neni - adaptivni rezim jen meri");
    }
    Log("latence: dotazy vytvoreny (%d snimku), casove znacky GPU %s", kLatRing,
        ts ? "ano" : "ne (jen event, pojistka necinnosti GPU jen z eventu)");
    return true;
}

// Chyba dotazu: vypnout do Resetu / zmeny rezimu (nejistota = nic nedelat).
static void LatError(const char* what, HRESULT hr) {
    ++g_latSt.errors;
    if (!g_lat.warnedErr) {
        g_lat.warnedErr = true;
        Log("WARN: latence: %s -> 0x%08X - regulator vypnut do Resetu (dalsi jen v radku vykon)", what, (unsigned)hr);
    }
    g_lat.broken = true;
}

// Casove znacky dokonceneho snimku (az po eventu: pak uz jsou zapsane, GetData hned uspeje)
// true = znacky jeste nejsou k dispozici (dalsi snimky pak necist, mezera se pocita v poradi)
// Posledni precteny cas GPU snimku hry (casova znacka po Present -> pred dalsim Present; pro odhad FG, src/fgest.inl)
static float g_latSpanMs;
static uint32_t g_latSpanSeq;
float ProxyLatGpuSpanMs(uint32_t* seq) {
    *seq = g_latSpanSeq;
    return g_latSpanMs;
}

static bool LatReadTs(LatSlot& s) {
    if (!s.tsPending) return false;
    UINT64 e = 0, b = 0;
    HRESULT he = s.tsEnd->GetData(&e, sizeof e, 0);
    if (he == S_FALSE) return true;
    HRESULT hb = s.begIssued ? s.tsBeg->GetData(&b, sizeof b, 0) : S_OK;
    if (hb == S_FALSE) return true;
    s.tsPending = false;
    if (FAILED(he) || FAILED(hb)) {  // znacky jsou jen statistika: chybu ignorovat
        g_lat.lastTsEValid = false;
        return false;
    }
    s.tsE = e;
    if (g_lat.tsTicksPerMs > 0.0) Lc_GpuFrame(s.tSubmit, (double)e / g_lat.tsTicksPerMs, s.tDone);
    if (s.begIssued && e >= b) {
        g_latSt.gpuSum += (double)(e - b) / g_lat.tsTicksPerMs;
        ++g_latSt.gpuN;
        g_latSpanMs = (float)((double)(e - b) / g_lat.tsTicksPerMs);
        ++g_latSpanSeq;
    }
    // necinnost GPU mezi koncem predchoziho a zacatkem tohoto snimku
    if (s.begIssued && g_lat.lastTsEValid && g_lat.lastTsEN + 1 == s.n && b >= g_lat.lastTsE) {
        const double gap = (double)(b - g_lat.lastTsE) / g_lat.tsTicksPerMs;
        if (gap < 100.0) {
            if (!s.active && s.busy && g_lat.state == LS_IDLE)  // GPU plne vytizene: zakladni mezera (blit Present)
                g_lat.gapBase = g_lat.gapBase < 0.0 ? gap : g_lat.gapBase + 0.05 * (gap - g_lat.gapBase);
            if (s.active) {
                const double lost = std::max(0.0, gap - std::max(0.0, g_lat.gapBase) - 0.05);
                g_lat.winLostMs += lost;
                g_latSt.gapLost += lost;
                ++g_latSt.gapN;
            }
        }
    }
    g_lat.lastTsE = e;
    g_lat.lastTsEN = s.n;
    g_lat.lastTsEValid = true;
    return false;
}

// Dotaz na dokonceni snimku; true = dokoncen (nebo chyba). now = cas pozorovani.
static bool LatDone(LatSlot& s, LONGLONG now) {
    if (!s.evPending) return true;
    BOOL v = FALSE;
    const HRESULT hr = s.ev->GetData(&v, sizeof v, 0);
    if (hr == S_FALSE) return false;
    s.evPending = false;
    if (hr != S_OK) {
        LatError("GetData(event)", hr);
        return true;
    }
    s.tDone = now;
    if (s.tBegin) {
        const double lat = LatMs(s.tBegin, now);
        if (lat > 0.0 && lat < 1000.0) {
            g_latSt.latSum += lat;
            if (lat > g_latSt.latMax) g_latSt.latMax = lat;
            ++g_latSt.latN;
        }
    }
    return true;
}

// Projde nevyrizene snimky od nejstarsiho (GPU je dokoncuje v poradi); u prvniho nedokonceneho skonci.
static void LatPoll(LONGLONG now) {
    g_lat.lastPoll = now;
    const uint64_t from = g_lat.n >= (uint64_t)kLatRing ? g_lat.n - kLatRing + 1 : 0;
    for (uint64_t k = from; k <= g_lat.n; ++k) {
        LatSlot& s = g_lat.s[k % kLatRing];
        if (s.issued && s.n == k && !LatDone(s, now)) break;
    }
    if (!g_lat.tsOk) return;
    for (uint64_t k = from; k <= g_lat.n; ++k) {  // casove znacky dokoncenych snimku, v poradi
        LatSlot& s = g_lat.s[k % kLatRing];
        if (!s.issued || s.n != k || !s.tsPending) continue;
        if (s.evPending || LatReadTs(s)) break;
    }
}

// Levne prubezne pozorovani (SetRenderTarget, nejvys jednou za 0.25 ms): presnejsi cas dokonceni pro odhad latence.
static void LatPollCheap() {
    const LONGLONG now = LatQpc();
    if (LatMs(g_lat.lastPoll, now) < 0.25) return;
    g_lat.lastPoll = now;
    const uint64_t from = g_lat.n >= (uint64_t)kLatRing ? g_lat.n - kLatRing + 1 : 0;
    for (uint64_t k = from; k <= g_lat.n; ++k) {
        LatSlot& s = g_lat.s[k % kLatRing];
        if (s.issued && s.n == k && s.evPending) {
            LatDone(s, now);
            return;  // jen nejstarsi nevyrizeny
        }
    }
}

static LatSlot* LatPrev() {
    LatSlot& p = g_lat.s[(g_lat.n + kLatRing - 1) % kLatRing];
    return g_lat.n > 0 && p.issued && p.n == g_lat.n - 1 ? &p : nullptr;
}

// Most (Interop_OnSceneEnd) muze vlakno hry zablokovat uprostred snimku, dokud GPU nedokonci predchozi snimek
// (namereno ve falesne hre: zaznam kopie do sdileneho obrazu ceka ~ G - C). I to je cekani na GPU: kdyz predchozi
// snimek pri vstupu jeste nebyl hotovy a pri odchodu uz ano, pocita se cas mostu do cekani W a snimek do fronty.
static void LatSceneEndEnter() {
    if (!g_lat.res || g_lat.broken || g_lat.seSeen) return;  // jen prvni konec sceny ve snimku
    g_lat.seSeen = true;
    const LONGLONG now = LatQpc();
    LatPoll(now);
    const LatSlot* p = LatPrev();
    g_lat.sePrevPending = p && p->evPending;
    g_lat.seEnter = now;
}
static void LatSceneEndLeave() {
    if (!g_lat.res || g_lat.broken || !g_lat.sePrevPending || g_lat.seLeft) return;
    g_lat.seLeft = true;
    const LONGLONG now = LatQpc();
    LatPoll(now);
    const LatSlot* p = LatPrev();
    if (p && !p->evPending) g_lat.seWait = LatMs(g_lat.seEnter, now);
}

// Presne cekani do casu (hi-res casovac, poslednich ~0.4 ms aktivne)
static void LatSleepUntil(LONGLONG deadline) {
    for (;;) {
        const LONGLONG now = LatQpc();
        const double rem = LatMs(now, deadline);
        if (rem <= 0.0) return;
        if (rem > 0.7 && g_lat.timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((rem - 0.4) * 10000.0);
            if (SetWaitableTimer(g_lat.timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(g_lat.timer, 200);
            else
                SwitchToThread();
        } else {
            for (int i = 0; i < 32; ++i) YieldProcessor();
            SwitchToThread();
        }
    }
}

// Zmena stavu do logu nejvys jednou za 2 s (jinak jen pocty v radku vykon)
static bool LatStateLogOk() {
    const LONGLONG now = LatQpc();
    if (g_lat.lastStateLog && LatMs(g_lat.lastStateLog, now) < 2000.0) return false;
    g_lat.lastStateLog = now;
    return true;
}

static unsigned LatPop(uint64_t v) {
    unsigned c = 0;
    for (; v; v &= v - 1) ++c;
    return c;
}

static void LatDisengage(const char* why, double penaltyS) {
    if (g_lat.state != LS_ACTIVE) return;
    // naucany okraj pro dalsi zapnuti (FG zap / vyp, menu s pauzou FG): jen z aspon jednoho celeho okna 64 snimku
    if (g_lat.dwell >= 64) {
        g_lat.mLearned = g_lat.margin;
        g_lat.mLearnedQ = LatQpc();
        g_lat.mLearnedOk = g_lat.lastWinOk && penaltyS <= 0.0;
    }
    g_lat.state = LS_IDLE;
    g_lat.D = 0.0;
    g_lat.dwell = 0;
    g_lat.busyBits = g_lat.slackBits = 0;
    ++g_latSt.disengage;
    g_latSt.lastReason = why;
    const LONGLONG now = LatQpc();
    // trest roste pri opakovani (10 -> 20 -> ... 120 s) a klesa: za kazdych 60 s od posledniho vypnuti s trestem na
    // polovinu (nejmene 5 s) - jinak by po nekolika zaskubech (nacitani, HDD) byl Impulse po zbytek sezeni skoro stale
    // vypnuty. Bez trestu jen 2 s proti preskakovani tam a zpet.
    if (penaltyS > 0.0 && g_lat.penaltyQ) {
        for (double el = LatMs(g_lat.penaltyQ, now); el >= 60000.0 && g_lat.penaltyS > 5.0; el -= 60000.0)
            g_lat.penaltyS = std::max(5.0, g_lat.penaltyS * 0.5);
    }
    const double wait = penaltyS > 0.0 ? std::max(penaltyS, g_lat.penaltyS) : 2.0;
    if (penaltyS > 0.0) {
        g_lat.penaltyS = std::min(120.0, wait * 2.0);
        g_lat.penaltyQ = now;
    }
    g_lat.cooldownUntil = LatAddMs(now, wait * 1000.0);
    if (LatStateLogOk()) Log("latence: regulator vypnut (%s), dalsi pokus nejdriv za %.0f s", why, wait);
}

// Pred Present hry (snimek n je hotovy na CPU): pozorovani fronty a vydani dotazu konce snimku.
static void LatBeforePresent(IDirect3DDevice9* d) {
    if (g_lat.mode == LAT_OFF || g_lat.broken) return;
    if (!LatCreate(d)) return;
    const LONGLONG now = LatQpc();
    LatPoll(now);
    LatSlot& s = g_lat.s[g_lat.n % kLatRing];
    // predchozi snimek: GPU na nem pri odeslani tohoto jeste pracuje? (fronta >= 1)
    const LatSlot& p = g_lat.s[(g_lat.n + kLatRing - 1) % kLatRing];
    const bool prevKnown = g_lat.n > 0 && p.issued && p.n == g_lat.n - 1;
    // (nebo jeste nebyl hotovy pri vstupu do mostu - pak na nej most cekal)
    const bool busy = prevKnown && (p.evPending || (g_lat.seSeen && g_lat.sePrevPending));
    unsigned q = 0;
    for (const auto& o : g_lat.s)
        if (o.issued && o.evPending && o.n < g_lat.n) ++q;
    ++g_latSt.frames;
    g_latSt.qSum += q;
    if (busy) ++g_latSt.busy;
    g_lat.busyBits = (g_lat.busyBits << 1) | (busy ? 1u : 0u);
    if (g_lat.state == LS_ACTIVE) {
        ++g_latSt.active;
        // GPU dokoncilo predchozi snimek driv, nez hra tento odeslala -> GPU melo hlad (ztrata FPS)
        if (prevKnown && !busy) {
            ++g_latSt.starved;
            ++g_lat.winStarved;
            g_lat.D *= 0.5;
        }
    }
    // CPU cas snimku (zacatek -> Present) a interval Present -> Present
    if (g_lat.tBeginCur) {
        const double c = LatMs(g_lat.tBeginCur, now);
        if (c >= 0.0 && c < 500.0) {
            if (g_lat.cEwma <= 0.0) g_lat.cEwma = c;
            g_lat.cMad += 0.05 * (fabs(c - g_lat.cEwma) - g_lat.cMad);
            g_lat.cEwma += 0.05 * (c - g_lat.cEwma);
        }
    }
    g_lat.lastIv = 0.0;
    if (g_lat.lastSubmit) {
        const double iv = LatMs(g_lat.lastSubmit, now);
        g_lat.lastIv = iv;
        if (iv > 0.0 && iv < 500.0) {
            g_lat.intEwma = g_lat.intEwma <= 0.0 ? iv : g_lat.intEwma + 0.05 * (iv - g_lat.intEwma);
            if (g_lat.state == LS_ACTIVE) g_lat.winMs += iv;
        }
    }
    g_lat.lastSubmit = now;
    // slot snimku n: kdyby GPU jeste nedokoncilo snimek n-8, je neco spatne (DXVK pusti nejvys ~3 snimky dopredu)
    if (s.issued && s.evPending && s.n != g_lat.n) {
        ++g_latSt.errors;
        if (!g_lat.warnedRing) {
            g_lat.warnedRing = true;
            Log("WARN: latence: snimek %llu jeste neni dokonceny po %d snimcich - regulator vypnut",
                (unsigned long long)s.n, kLatRing);
        }
        LatDisengage("GPU o 8 snimku pozadu", 30.0);
        g_lat.tBeginCur = 0;  // tento snimek bez odhadu latence
    }
    const bool beg = s.begIssued && s.n == g_lat.n;
    s.n = g_lat.n;
    s.tBegin = g_lat.tBeginCur;
    s.tSubmit = now;
    s.tDone = 0;
    s.begIssued = beg;
    s.active = g_lat.state == LS_ACTIVE;
    s.busy = busy;
    if (g_lat.tsOk) {
        s.tsEnd->Issue(D3DISSUE_END);
        s.tsPending = true;
    }
    s.ev->Issue(D3DISSUE_END);
    s.issued = true;
    s.evPending = true;
}

// Po Present hry: rozhodnuti regulatoru, cekani na GPU a spanek pred dalsim snimkem.
static void LatAfterPresent(IDirect3DDevice9* d, HRESULT hr) {
    if (g_lat.mode == LAT_OFF) {
        if (g_lat.res) LatRelease("vypnuto");
        return;
    }
    if (!g_lat.res) return;
    LONGLONG now = LatQpc();
    if (!g_lat.broken) LatPoll(now);  // DXVK casto blokuje v Present az do dokonceni predchoziho snimku
    const bool presentOk = hr == S_OK;
    ++g_lat.framesOk;
    ++g_lat.dwell;
    // Frame Generation: kdyz strop skutecnych snimku drzi limiter FG (src\present.cpp, spi po Present tesne pred timto
    // volanim), GPU ma rezervu a regulator nema co delat - navic by jeho mereni cekani na GPU zahrnulo spanek limiteru.
    // Jen meri; kdyz hra strop nestiha (limiter nespi), regulator pracuje jako bez FG.
    const bool fgLimit = Present_FgLimiting();
    const bool allow = g_lat.mode == LAT_ADAPTIVE && !g_lat.dxvkSleep && !g_lat.broken && g_lat.timer &&
                       (g_lat.foreground || !g_lat.fgOnly) && presentOk && g_lat.framesOk >= 120 &&
                       now >= g_lat.cooldownUntil && !fgLimit;
    if (g_lat.state == LS_ACTIVE && !allow) {
        LatDisengage(g_lat.broken ? "chyba dotazu" : !g_lat.foreground && g_lat.fgOnly ? "okno neni v popredi" :
                     !presentOk ? "Present neuspel" : fgLimit ? "strop drzi limiter Frame Generation" : "rezim zmenen",
                     g_lat.broken ? 30.0 : 0.0);
    }
    if (g_lat.state == LS_IDLE && allow && g_lat.dwell >= 60) {
        // zapnout: GPU je uzke hrdlo (fronta pri odeslani) v >= 58 z poslednich 64 snimku
        if (LatPop(g_lat.busyBits) >= 58) {
            g_lat.state = LS_ACTIVE;
            g_lat.dwell = 0;
            g_lat.D = 0.0;
            g_lat.slackBits = 0;
            // Nabeh (v103b): driv okraj vzdy od minima 0,5 ms -> po kazdem prepnuti FG (vypnuti generovani, menu s
            // pauzou FG) integrator z cekani ~6 ms rychle narostl na plny spanek, GPU 1-1,5 s hladovelo a FPS klesly
            // o 5-20 %. Ted zacina z horni meze okraje (0,3 intervalu) a klesa jen v oknech bez hladu (x0,95 za 64
            // snimku); naucany okraj z minuleho zapnuti do 60 s (bez hladu v poslednim okne) se prevezme.
            const double mTop = std::max((double)g_lat.marginMinMs, 0.3 * g_lat.intEwma);
            const bool take = g_lat.mLearnedOk && g_lat.mLearnedQ && LatMs(g_lat.mLearnedQ, now) < 60000.0;
            g_lat.margin = take ? std::min(mTop, std::max((double)g_lat.marginMinMs, g_lat.mLearned)) : mTop;
            g_lat.lastWinOk = false;
            g_lat.wMad = 0.0;
            g_lat.winFrames = g_lat.winStarved = 0;
            g_lat.winLostMs = g_lat.winMs = 0.0;
            g_lat.wEwma = std::max(0.0, g_lat.intEwma - g_lat.cEwma);  // ocekavane cekani na GPU bez spanku
            g_lat.wPresEwma = 0.0;
            ++g_latSt.engage;
            if (LatStateLogOk())
                Log("latence: regulator zapnut (GPU je uzke hrdlo: snimek %.2f ms, CPU %.2f ms, okraj %.2f ms - %s)",
                    g_lat.intEwma, g_lat.cEwma, g_lat.margin, take ? "prevzaty z minuleho zapnuti" : "nabeh od horni meze");
        }
    }

    if (g_lat.state == LS_ACTIVE) {
        // 1) pockat, az GPU dokonci predchozi snimek (fronta <= 1 snimek: GPU ted pracuje na tomto). Cekani W se
        //    meri od volani Present hrou: DXVK v Present samo blokuje (maxFrameLatency), dokud GPU predchozi snimek
        //    nedokonci - i to je cekani na GPU, ktere ma regulator zmensit na okraj m.
        LatSlot& p = g_lat.s[(g_lat.n + kLatRing - 1) % kLatRing];
        const LatSlot& cur = g_lat.s[g_lat.n % kLatRing];
        const bool havePrev = g_lat.n > 0 && p.issued && p.n == g_lat.n - 1 && cur.issued && cur.n == g_lat.n;
        double w = 0.0;
        bool waited = false, timeout = false;
        if (havePrev && p.evPending) {
            const LONGLONG t0 = now;
            // limit podle intervalu snimku (pomale PC: pod 10 FPS je cekani na predchozi snimek bezne pres 100 ms)
            const double limit = std::min(250.0, std::max(20.0, 4.0 * g_lat.intEwma));
            for (;;) {
                const LONGLONG t = LatQpc();
                if (LatDone(p, t)) {
                    now = t;
                    break;
                }
                const double el = LatMs(t0, t);
                if (el > limit) {
                    timeout = true;
                    now = t;
                    break;
                }
                // ocekavany zbytek cekani po Present (bez casti v mostu); casovac jen na delsi cekani, jinak aktivne
                const double expect = g_lat.wPresEwma - LatMs(cur.tSubmit, t);
                if (expect > 0.9 && g_lat.timer) LatSleepUntil(LatAddMs(t, std::min(2.0, expect - 0.5)));
                else {
                    for (int i = 0; i < 32; ++i) YieldProcessor();
                    SwitchToThread();
                }
            }
        }
        // cekani v mostu + od odeslani tohoto snimku do dokonceni predchoziho (hlad GPU viz LatBeforePresent)
        if (havePrev && !timeout) {
            const double wp = p.tDone > cur.tSubmit ? LatMs(cur.tSubmit, p.tDone) : 0.0;
            g_lat.wPresEwma += 0.1 * (wp - g_lat.wPresEwma);
            w = g_lat.seWait + wp + Present_BpWaitMs();  // v103g: + cekani zpetneho tlaku ([debug] PrefBpReg)
            waited = w > 0.0;
        }
        if (timeout) {
            ++g_latSt.timeouts;
            if (!g_lat.warnedTimeout) {
                g_lat.warnedTimeout = true;
                Log("WARN: latence: GPU nedokoncilo snimek do %.0f ms - regulator vypnut (dalsi jen v radku vykon)",
                    std::min(250.0, std::max(20.0, 4.0 * g_lat.intEwma)));
            }
            // Zaskub (interval snimku nad 3x prumer: nacitani, autosave, HDD) neni chyba regulatoru: bez trestu
            const bool hitch = g_lat.intEwma > 0.0 && g_lat.lastIv > 3.0 * g_lat.intEwma;
            LatDisengage(hitch ? "timeout cekani na GPU pri zaskubu" : "timeout cekani na GPU", hitch ? 0.0 : 10.0);
        } else if (!g_lat.broken) {
            if (waited) {
                ++g_latSt.waited;
                g_lat.wMad += 0.05 * (fabs(w - g_lat.wEwma) - g_lat.wMad);  // rozptyl cekani = rozptyl G - C
                g_lat.wEwma += 0.1 * (w - g_lat.wEwma);
                g_latSt.wSum += w;
                // integracni regulator: cekani ma byt rovno okraji
                g_lat.D += 0.2 * (w - g_lat.margin);
            } else {
                // predchozi snimek byl hotovy uz pri odeslani tohoto (GPU melo hlad): okraj vycerpany, ubrat
                g_lat.wEwma *= 0.9;
                g_lat.D -= g_lat.margin;
            }
            const double dMax = std::min(50.0, 0.85 * std::max(0.0, g_lat.intEwma));
            g_lat.D = std::min(dMax, std::max(0.0, g_lat.D));
            const bool slack = g_lat.D < 0.05 && (!waited || w < 0.5 * g_lat.margin);
            g_lat.slackBits = (g_lat.slackBits << 1) | (slack ? 1u : 0u);
            // 2) spanek: dalsi snimek zacne co nejpozdeji. SampleSync: spi pumpa zprav hlavniho vlakna D ms od
            //    odberu znacky tohoto snimku (pred odberem vstupu), vlakno renderu snimek hned uvolni.
            if (g_lat.D > 0.0) {
                if (!Ss_Delay(g_lat.D)) {
                    LatSleepUntil(LatAddMs(now, g_lat.D));
                    now = LatQpc();
                }
                g_latSt.dSum += g_lat.D;
                if (g_lat.D > g_latSt.dMax) g_latSt.dMax = g_lat.D;
            }
            // 3) okno 64 snimku: pojistka proti hladu GPU (ztrata FPS) a vypnuti, kdyz GPU uz neni uzke hrdlo
            if (++g_lat.winFrames >= 64) {
                const double lostFrac = g_lat.winMs > 0.0 ? g_lat.winLostMs / g_lat.winMs : 0.0;
                const double mMin = std::max((double)g_lat.marginMinMs, 3.0 * g_lat.wMad);
                const double mCap = std::max(mMin, 0.3 * g_lat.intEwma);
                // hlad GPU ve >= 2 z 64 snimku (nebo > 0.5 % casu necinne) -> vetsi okraj; trvale i s nejvetsim
                // okrajem (>= 6 z 64 nebo > 1 %) -> 3 okna po sobe = vypnout
                const bool bad = g_lat.winStarved >= 2 || lostFrac > 0.005;
                const bool veryBad = g_lat.winStarved >= 6 || lostFrac > 0.01;
                if (bad) {
                    if (veryBad && g_lat.margin >= mCap - 1e-6) ++g_lat.strikes;
                    else if (!veryBad) g_lat.strikes = 0;
                    g_lat.margin = std::min(mCap, g_lat.margin * 1.25);
                } else {
                    g_lat.strikes = 0;
                    if (g_lat.winStarved == 0 && lostFrac < 0.001) g_lat.margin = std::max(mMin, g_lat.margin * 0.95);
                    else g_lat.margin = std::max(mMin, g_lat.margin);
                }
                g_lat.lastWinOk = !bad;
                g_lat.winFrames = g_lat.winStarved = 0;
                g_lat.winLostMs = g_lat.winMs = 0.0;
                if (g_lat.strikes >= 3) LatDisengage("GPU ma hlad i s nejvetsim okrajem", 30.0);
            }
            if (g_lat.state == LS_ACTIVE && g_lat.dwell >= 120 && LatPop(g_lat.slackBits) >= 48)
                LatDisengage("GPU uz neni uzke hrdlo", 0.0);
            else if (g_lat.state == LS_ACTIVE && g_lat.dwell >= 120 && g_lat.D <= 0.0 && LatPop(g_lat.busyBits) < 32)
                LatDisengage("fronta prazdna", 0.0);
        }
    }

    // zacatek dalsiho snimku: cas a casova znacka GPU
    g_lat.seSeen = g_lat.seLeft = g_lat.sePrevPending = false;
    g_lat.seWait = 0.0;
    ++g_lat.n;
    g_lat.tBeginCur = presentOk ? now : 0;
    LatSlot& nx = g_lat.s[g_lat.n % kLatRing];
    nx.begIssued = false;
    if (g_lat.tsOk && !g_lat.broken && !(nx.issued && (nx.evPending || nx.tsPending))) {
        nx.n = g_lat.n;  // (issued zustava z minula; LatPoll kontroluje s.n == k)
        nx.issued = false;
        nx.tsBeg->Issue(D3DISSUE_END);
        nx.begIssued = true;
    }
    (void)d;
}

// Rezim regulatoru (LAT_*) z klavesy nebo z menu; stav regulatoru se vynuluje. Jen z vlakna Present (OnFrameEnd).
static void LatSetMode(int mode, Src src) {
    g_lat.mode = mode == LAT_ADAPTIVE || mode == LAT_MEASURE ? mode : LAT_OFF;
    if (g_lat.state == LS_ACTIVE) LatDisengage("rezim zmenen", 0.0);
    g_lat.cooldownUntil = 0;
    g_lat.penaltyS = 5.0;
    g_lat.mLearnedQ = 0;
    g_lat.broken = false;
    g_lat.warnedErr = g_lat.warnedTimeout = g_lat.warnedRing = false;
    SrcBeep(src, g_lat.mode == LAT_ADAPTIVE ? MB_OK : g_lat.mode == LAT_MEASURE ? MB_ICONASTERISK : MB_ICONHAND);
    SrcNotify(src, OSD_LATENCY);
    Log("latence: %s (%s)%s", LatModeName(g_lat.mode), src == Src::KEY ? "klavesa" : "menu",
        g_lat.mode == LAT_ADAPTIVE && g_lat.dxvkSleep ? " - dxvk.latencySleep je zapnute, jen meri" : "");
}

// Alt+F12: adaptivni -> jen mereni -> vypnuto -> adaptivni
static void LatKeys(bool ctrl) {
    if (KeyEdge(ctrl && KeyDown(DbgKey(g_lat.key)), g_lat.keyWas))
        LatSetMode(g_lat.mode == LAT_ADAPTIVE ? LAT_MEASURE : g_lat.mode == LAT_MEASURE ? LAT_OFF : LAT_ADAPTIVE,
                   Src::KEY);
}

// Cast radku "vykon": regulator a odhad latence za interval; pak vynuluje.
void ProxyLatencyPerf(char* buf, size_t n) {
    if (!n) return;
    buf[0] = 0;
    const auto st = g_latSt;
    g_latSt = {};
    if (g_lat.mode == LAT_OFF && !st.frames) return;
    if (!st.frames) {
        snprintf(buf, n, " | latence (%s): bez dat", LatModeName(g_lat.mode));
        return;
    }
    const double f = st.frames;
    char lat[96] = "-", gpu[160] = "";
    if (st.latN) snprintf(lat, sizeof lat, "prum %.2f / max %.2f ms", st.latSum / st.latN, st.latMax);
    if (st.gpuN) {
        int k = snprintf(gpu, sizeof gpu, ", GPU prace %.2f ms/snimek", st.gpuSum / st.gpuN);
        if (st.gapN && k > 0 && k < (int)sizeof gpu)
            snprintf(gpu + k, sizeof gpu - k, ", GPU necinne navic %.3f ms/akt. snimek", st.gapLost / st.gapN);
    }
    snprintf(buf, n,
             " | latence (%s%s): aktivni %.0f %% snimku, fronta pri odeslani %.2f snimku (GPU pozadu %.0f %%), "
             "cekani na GPU %.2f ms, spanek prum %.2f / max %.2f ms, okraj %.2f ms, odhad latence (zacatek snimku -> "
             "konec GPU) %s%s; hlad GPU %u, zapnuto %ux, vypnuto %ux%s%s, timeout %u, chyby %u",
             LatModeName(g_lat.mode), g_lat.dxvkSleep && g_lat.mode == LAT_ADAPTIVE ? ", dxvk.latencySleep" : "",
             100.0 * st.active / f, st.qSum / f, 100.0 * st.busy / f, st.waited ? st.wSum / st.waited : 0.0,
             st.active ? st.dSum / st.active : 0.0, st.dMax, g_lat.margin, lat, gpu, st.starved, st.engage,
             st.disengage, st.lastReason ? " - " : "", st.lastReason ? st.lastReason : "", st.timeouts, st.errors);
}
