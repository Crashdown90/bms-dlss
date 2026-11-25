// bms-dlss: retezec latence "odber vstupu -> zacatek kresleni -> Present -> zobrazeni" (Impulse krok A, navrh
// docs/vyzkum-impulse-vylepseni.md kap. 5) - jen mereni do logu, chovani hry se nemeni ([debug] LatChain=1, vychozi;
// 0 = nic z tohoto). Vklada ho present.cpp za gamethreads.inl (pouziva jeho hlavni vlakno a cekani na render).
//
// Body (vse QPC, spolecne hodiny vsech procesu):
//  - odber vstupu (pumpa zprav hlavniho vlakna): vlakno renderu po uvolneni snimku f (konec H_Present) posle vlaknu
//    okna hry zpravu vlakna (PostThreadMessage, cislo f). PeekMessage vraci zaslane zpravy pred vstupem z hardwaru,
//    takze ji hacek WH_GETMESSAGE (CiGetMsgProc v menu.inl) dostane na zacatku pumpy, zapise cas a prepise ji na
//    WM_NULL. Vstup mysi odebrany v teze pumpe (WM_INPUT) se pocita k ni. Nejvys jedna nevyrizena znacka, po 250 ms
//    se posle nova.
//  - zacatek kresleni: prvni volani hlavniho zarizeni (SetVertexShaderConstantF, SetRenderTarget, Clear) na vlakne
//    renderu po uvolneni predchoziho snimku.
//  - Present: volani Present hrou (tSubmit regulatoru latence).
//  - zobrazeni: bez presenteru FG konec prace GPU na snimku (casova znacka D3D9 konce snimku prevedena na QPC podle
//    nejmensiho rozdilu proti prvnimu pozorovani dokonceni v intervalu; DXVK bez FG prezentuje hned, s tearingem); s
//    presenterem v procesu hry (src/gamepresent.inl) max(navrat vkQueuePresentKHR, konec kopie na GPU) slotu, ktery
//    obsah snimku ukazuje poprve (prvni odezva, i mezisnimek) a slotu se skutecnym snimkem (plna odezva). Obsah slotu
//    hlasi helper (gpRealQpc = postQpc skutecneho snimku, BMSD_PRESENT_VERSION 10).
//  - parovani: snimek f nese vstup z pumpy se znackou f - d. Hloubka d se zjistuje za behu: osamely vstup mysi
//    (4 pumpy pred nim bez mysi, kamera v klidu) -> prvni snimek s otocenim kamery; bez techto vzorku d = 2 pri
//    kresleni na jinem vlakne nez pumpa, jinak 1 (radek pise "predpoklad").
// Radek "latence vstupu" jednou za ~5 s; [debug] LatChainCsv=1: zaznamy snimku, pump a slotu do bms_dlss_latchain.csv.

static const int kLcN = 4096;
struct LcFrame {  // jen vlakno renderu
    uint32_t id;
    float cam;          // otoceni kamery v tomto snimku (st), < 0 = bez kamery
    LONGLONG call, first, rel, post, done;
    double gpuTs;       // casova znacka konce snimku na GPU (ms v hodinach GPU), 0 = nezname
};
struct LcPump {  // zapisuje hlavni vlakno (seqlock), cte vlakno renderu
    volatile LONG seq;
    uint32_t id, mouse;
    LONGLONG t, waitA, waitB;  // odber; nasledujici cekani hlavniho vlakna na render (0 = nezname)
};
struct LcDisp {  // zapisuje vlakno presenteru (seqlock), cte vlakno renderu
    volatile LONG seq;
    uint32_t id, gen;
    LONGLONG real, p0, p1, g1;
};
static LcFrame g_lcF[kLcN];
static LcPump g_lcP[kLcN];
static LcDisp g_lcD[kLcN];
static uint32_t g_lcFid;                // posledni snimek (vlakno renderu)
static volatile LONG g_lcDispW;         // zapsanych slotu (vlakno presenteru)
static UINT g_lcMsg;                    // RegisterWindowMessage
static volatile LONG g_lcMarkOut;       // 1 = znacka odeslana a dosud neodebrana
static volatile LONG g_lcPumps, g_lcPumpTid;
static DWORD g_lcRendTid;
static LONGLONG g_lcFirstQ;

static void LcPumpWrite(const LcPump& v) {
    LcPump& p = g_lcP[v.id % kLcN];
    InterlockedIncrement(&p.seq);
    MemoryBarrier();
    p.id = v.id;
    p.mouse = v.mouse;
    p.t = v.t;
    p.waitA = v.waitA;
    p.waitB = v.waitB;
    MemoryBarrier();
    InterlockedIncrement(&p.seq);
}
static bool LcPumpGet(uint32_t id, LcPump& o) {
    const LcPump& p = g_lcP[id % kLcN];
    const LONG a = p.seq;
    MemoryBarrier();
    o.id = p.id;
    o.mouse = p.mouse;
    o.t = p.t;
    o.waitA = p.waitA;
    o.waitB = p.waitB;
    MemoryBarrier();
    return a == p.seq && !(a & 1) && o.id == id && o.t;
}

// ---------------------------------------------------------------------------------------------------------------
// Impulse krok B, [latency] SampleSync=1 (vychozi 0; docs/vyzkum-impulse-vylepseni.md kap. 5 a 12): spanek limiteru FG
// a regulatoru latence se presouva z vlakna renderu (po Present) do pumpy zprav hlavniho vlakna, tesne pred odber
// vstupu. Vlakno renderu za snimek f spocita, jak dlouho by spalo, a pred znackou f zverejni:
//   - konec slotu limiteru FG (QPC, absolutne; faze mrizky zustava ve FgLimiter),
//   - zpozdeni regulatoru latence D (od prichodu znacky do pumpy).
// Spi se na zacatku pumpy zprav vstupu (prvni PeekMessageA z inputsystem.dll po prazdne fronte, hacek IAT
// Gt_PeekMessageA v gamethreads.inl) do max(konec slotu, zacatek pumpy + D) posledniho dosud neprospaneho zverejneni,
// nejvys 50 ms; hlasi se zpet spanek, nestih slotu a odchylka probuzeni. Kazde zverejneni prospi nejvys jedna pumpa.
// Spanek pri odberu znacky (v103b0/b1) nefungoval: hlavni vlakno pumpuje hned po predani snimku vlaknu renderu, takze
// znacka uvolneni prichazela bud az behem spanku (a ta sama pumpa ji po spanku odebrala a spala podruhe), nebo vubec
// -> dvojice snimku tesne po sobe a mezera 2 intervaly, 59 misto 78 FPS. Zaloha: kdyz pumpa nezacala do 100 ms,
// znacka neni odebrana do 100 ms (nacitani, modalni smycka) nebo hacek pumpy chybi, spi vlakno renderu jako driv.
static struct {
    volatile LONG seq;
    uint32_t n;     // poradi zverejneni (jen s obsahem)
    LONGLONG q;     // cas zverejneni
    LONGLONG abs;   // konec slotu limiteru (0 = bez)
    LONGLONG dly;   // zpozdeni regulatoru v QPC (0 = bez)
} g_ssPub;
static LONGLONG g_ssAbsF, g_ssDlyF;  // vlakno renderu: co zverejnit se znackou tohoto snimku
static HANDLE g_ssTimer;             // jen hlavni vlakno
static volatile LONGLONG g_ssLoopQ;  // posledni zacatek pumpy zprav (hlavni vlakno, pred spankem)
static volatile LONGLONG g_ssSlCum, g_ssSlStart;  // soucet spanku v pumpe (QPC); zacatek probihajiciho (0 = nespi)
static bool g_ssTimerTried;
// hlaseni hacku (hlavni vlakno zapisuje, vlakno renderu cte)
static volatile LONG g_ssNSleep, g_ssNLim, g_ssNLate, g_ssNReg;
static volatile LONGLONG g_ssSumQ, g_ssLimSleptQ, g_ssErrSumQ, g_ssMaxQ;
// vlakno renderu: zaloha (spanek po Present) a log
static unsigned g_ssFallback, g_ssFallbackLog, g_ssUsed;
static bool g_ssWasReady;

static void SsSetAbs(LONGLONG t) { g_ssAbsF = t; }
static void SsSetDelayMs(double ms) { g_ssDlyF = ms > 0.0 ? (LONGLONG)(ms * (double)g_qpf.QuadPart / 1000.0) : 0; }

// Vlakno renderu, tesne pred odeslanim znacky snimku
static void SsCommit() {
    if (!g_ssAbsF && !g_ssDlyF) return;
    InterlockedIncrement(&g_ssPub.seq);
    MemoryBarrier();
    ++g_ssPub.n;
    g_ssPub.q = Qpc();
    g_ssPub.abs = g_ssAbsF;
    g_ssPub.dly = g_ssDlyF;
    MemoryBarrier();
    InterlockedIncrement(&g_ssPub.seq);
    ++g_ssUsed;
    g_ssAbsF = g_ssDlyF = 0;
}

static void SsSleepUntil(LONGLONG target) {
    if (!g_ssTimerTried) {
        g_ssTimerTried = true;
        g_ssTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                           TIMER_ALL_ACCESS);
    }
    for (;;) {
        const LONGLONG now = Qpc();
        const double rem = Ms(now, target);
        if (rem <= 0.0) return;
        if (rem > 0.7 && g_ssTimer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((rem - 0.4) * 10000.0);
            if (SetWaitableTimer(g_ssTimer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_ssTimer, 60);
            else SwitchToThread();
        } else if (rem > 1.5) {
            Sleep(1);
        } else {
            for (int i = 0; i < 32; ++i) YieldProcessor();
            SwitchToThread();
        }
    }
}

// Hlavni vlakno, zacatek pumpy zprav vstupu v case t (Gt_PeekMessageA): spanek podle posledniho neprospaneho
// zverejneni; vraci cas po spanku
static LONGLONG SsPump(LONGLONG t) {
    static uint32_t s_done;
    if (!g_ssOn) return t;
    InterlockedExchange64(&g_ssLoopQ, t);
    const LONG a = g_ssPub.seq;
    MemoryBarrier();
    const uint32_t pn = g_ssPub.n;
    const LONGLONG pq = g_ssPub.q, abs = g_ssPub.abs, dly = g_ssPub.dly;
    MemoryBarrier();
    const LONGLONG q50 = g_qpf.QuadPart / 20;
    if (a != g_ssPub.seq || (a & 1) || pn == s_done) return t;
    s_done = pn;
    if (t - pq > 2 * q50 || (!abs && !dly)) return t;  // zverejneni starsi nez 100 ms (pauza, nacitani): nespat
    LONGLONG end = dly ? t + dly : 0;
    if (abs) {
        const bool late = t >= abs;
        if (late) InterlockedIncrement(&g_ssNLate);
        end = std::max(end, abs);
    }
    if (end > t + q50) end = t + q50;
    if (end <= t) return t;
    InterlockedExchange64(&g_ssSlStart, t);
    SsSleepUntil(end);
    const LONGLONG w = Qpc(), sl = w - t;
    InterlockedExchangeAdd64(&g_ssSlCum, sl);
    InterlockedExchange64(&g_ssSlStart, 0);
    InterlockedIncrement(&g_ssNSleep);
    if (abs && abs > t) {
        InterlockedIncrement(&g_ssNLim);
        InterlockedExchange64(&g_ssLimSleptQ, sl);
    }
    if (dly) InterlockedIncrement(&g_ssNReg);
    InterlockedExchangeAdd64(&g_ssSumQ, sl);
    InterlockedExchangeAdd64(&g_ssErrSumQ, w > end ? w - end : end - w);
    if (sl > g_ssMaxQ) InterlockedExchange64(&g_ssMaxQ, sl);
    // pro odhad FG (src/fgest.inl): spanek hlavniho vlakna v pumpe = drivejsi cekani na render (spal render), M = prace
    InterlockedExchangeAdd64(&g_gtAcc[GTA_WREND], sl);
    return w;
}

// Vlakno renderu (FgLimiter): kolik hlavni vlakno v pumpe naspalo od minuleho volani (dokoncene + probihajici spanek)
static LONGLONG SsSleptDelta() {
    static LONGLONG s_last;
    const LONGLONG now = Qpc();
    LONGLONG st = 0, cum = 0;
    for (int i = 0; i < 4; ++i) {
        st = InterlockedCompareExchange64(&g_ssSlStart, 0, 0);
        cum = InterlockedCompareExchange64(&g_ssSlCum, 0, 0);
        if (InterlockedCompareExchange64(&g_ssSlStart, 0, 0) == st) break;
    }
    const LONGLONG v = cum + (st && now > st ? now - st : 0);
    const LONGLONG d = s_last && v >= s_last ? v - s_last : 0;
    s_last = v;
    return d;
}

// Vlakno renderu (FgLimiter): hlaseni hacku od minuleho volani -> bit "limiter spal", spanek, nestih
static bool SsLimReport(LONGLONG* slept, bool* late) {
    static LONG s_nLim, s_nLate;
    const LONG nl = g_ssNLim, nt = g_ssNLate;
    const bool sl = nl != s_nLim;
    *late = nt != s_nLate;
    *slept = sl ? (LONGLONG)InterlockedCompareExchange64(&g_ssLimSleptQ, 0, 0) : 0;
    s_nLim = nl;
    s_nLate = nt;
    return sl;
}

// Hlavni vlakno (hacek WH_GETMESSAGE, PM_REMOVE): true = zprava byla znacka (prepsana na WM_NULL)
static bool LcOnPumpMsg(MSG* m, bool mouse) {
    static bool s_open;
    static LcPump s_cur;
    if (!g_lcOn) return false;
    if (g_lcMsg && m->message == g_lcMsg) {
        InterlockedExchange(&g_lcMarkOut, 0);
        InterlockedExchange(&g_lcPumpTid, (LONG)GetCurrentThreadId());
        const LONGLONG t = Qpc();  // se SampleSync uz po spanku (zacatek pumpy, Gt_PeekMessageA)
        if (s_open) {
            const LONGLONG a = g_gtRwA, b = g_gtRwB;  // posledni cekani hlavniho vlakna na render (gamethreads.inl)
            if (a >= s_cur.t && b >= a && b <= t) s_cur.waitA = a, s_cur.waitB = b;
            LcPumpWrite(s_cur);
        }
        s_cur = LcPump{};
        s_cur.id = (uint32_t)m->wParam;
        s_cur.t = t;
        s_open = true;
        InterlockedIncrement(&g_lcPumps);
        m->message = WM_NULL;
        m->wParam = 0;
        m->lParam = 0;
        return true;
    }
    if (mouse && s_open) ++s_cur.mouse;
    return false;
}

// Vlakno renderu: prvni volani zarizeni po uvolneni snimku
static void LcFirst() {
    if (GetCurrentThreadId() != g_lcRendTid) return;
    g_lcFirstQ = Qpc();
    g_lcWantFirst = false;
}

// Vlakno renderu, Present hry (po OnFrameEnd a LatBeforePresent): tSubmit = cas regulatoru (0 = nezname)
static void LcPresentEnter(LONGLONG tSubmit, float camDeg) {
    if (!g_lcOn) return;
    LcFrame& r = g_lcF[++g_lcFid % kLcN];
    r = LcFrame{};
    r.id = g_lcFid;
    r.call = tSubmit ? tSubmit : Qpc();
    r.first = g_lcFirstQ && g_lcFirstQ <= r.call ? g_lcFirstQ : r.call;
    r.cam = camDeg;
    g_lcFirstQ = 0;
    g_lcWantFirst = false;
}
static void LcSetPost(LONGLONG pq) {
    if (g_lcOn && g_lcFid) g_lcF[g_lcFid % kLcN].post = pq;
}
// Vlakno renderu (latency.inl, LatReadTs): konec snimku na GPU (casova znacka v ms hodin GPU) a prvni pozorovani
static void LcGpuFrame(LONGLONG tSubmit, double gpuTsMs, LONGLONG tDone) {
    if (!g_lcOn || !tSubmit) return;
    for (uint32_t k = 0; k < 24 && k < g_lcFid; ++k) {
        LcFrame& r = g_lcF[(g_lcFid - k) % kLcN];
        if (r.id == g_lcFid - k && r.call == tSubmit) {
            r.gpuTs = gpuTsMs;
            r.done = tDone;
            return;
        }
    }
}

// Vlakno presenteru FG (gamepresent.inl): slot seq s obsahem skutecneho snimku real (postQpc), generovany?
static int LcDispBegin(LONGLONG real, bool gen) {
    if (!g_lcOn || !real) return -1;
    const LONG id = g_lcDispW + 1;
    LcDisp& d = g_lcD[id % kLcN];
    InterlockedIncrement(&d.seq);
    MemoryBarrier();
    d.id = (uint32_t)id;
    d.gen = gen ? 1u : 0u;
    d.real = real;
    d.p0 = d.p1 = d.g1 = 0;
    MemoryBarrier();
    InterlockedIncrement(&d.seq);
    InterlockedExchange(&g_lcDispW, id);
    return id % kLcN;
}
static void LcDispSet(int i, LONGLONG p0, LONGLONG p1, LONGLONG g1) {
    if (i < 0) return;
    LcDisp& d = g_lcD[i];
    InterlockedIncrement(&d.seq);
    MemoryBarrier();
    if (p0) d.p0 = p0, d.p1 = p1;
    if (g1) d.g1 = g1;
    MemoryBarrier();
    InterlockedIncrement(&d.seq);
}
static bool LcDispGet(uint32_t id, LcDisp& o) {
    const LcDisp& d = g_lcD[id % kLcN];
    const LONG a = d.seq;
    MemoryBarrier();
    o.id = d.id;
    o.gen = d.gen;
    o.real = d.real;
    o.p0 = d.p0;
    o.p1 = d.p1;
    o.g1 = d.g1;
    MemoryBarrier();
    return a == d.seq && !(a & 1) && o.id == id;
}

struct LcStat {
    std::vector<float> v;
    void add(double x) { v.push_back((float)x); }
    double avg() const {
        double s = 0.0;
        for (float x : v) s += x;
        return v.empty() ? 0.0 : s / (double)v.size();
    }
    double pct(double q) {
        if (v.empty()) return 0.0;
        const size_t k = std::min(v.size() - 1, (size_t)(q * (double)v.size()));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    }
};
static struct {
    uint32_t lastF;            // posledni vyhodnoceny snimek
    uint32_t lastD;            // posledni zpracovany slot zobrazeni
    unsigned hist[8];          // hloubka d podle skoku kamery (od startu)
    unsigned lost;             // znacek ztraceno (nevyrizeno do 250 ms) za interval
    LONGLONG logQ, markQ, tidQ;
    DWORD tid;
    FILE* csv;
    bool csvFailed;
} g_lc;

static double LcQms(LONGLONG q) { return (double)q * 1000.0 / (double)g_qpf.QuadPart; }

// Vyhodnoceni intervalu a radek do logu (vlakno renderu, ~5 s)
static void LcAnalyze(double el) {
    const uint32_t lag = 24;  // snimky mladsi nez ~24 nemusi mit hotove GPU / zobrazeni
    if (g_lcFid <= lag) return;
    const uint32_t hi = g_lcFid - lag;
    uint32_t lo = g_lc.lastF + 1;
    if (hi < lo) return;
    if (hi - lo > (uint32_t)kLcN - 64) lo = hi - ((uint32_t)kLcN - 64);
    g_lc.lastF = hi;
    // prevod hodin GPU na QPC: nejmensi (prvni pozorovani dokonceni - casova znacka) v intervalu
    double off = 1e300;
    for (uint32_t f = lo; f <= hi; ++f) {
        const LcFrame& r = g_lcF[f % kLcN];
        if (r.id == f && r.gpuTs > 0.0 && r.done) off = std::min(off, LcQms(r.done) - r.gpuTs);
    }
    const bool gpuOk = off < 1e299;
    // zobrazeni (presenter FG): prvni a skutecny slot podle obsahu (postQpc), dva ukazatele (oboje roste)
    static LONGLONG s_dispFirst[kLcN], s_dispFull[kLcN];
    for (uint32_t f = lo; f <= hi; ++f) s_dispFirst[f % kLcN] = s_dispFull[f % kLcN] = 0;
    const LONG dw = g_lcDispW;
    unsigned nDisp = 0, nDispTs = 0;
    uint32_t fi = lo;
    uint32_t di = g_lc.lastD + 1;
    if ((uint32_t)dw > di + (uint32_t)kLcN - 64) di = (uint32_t)dw - ((uint32_t)kLcN - 64);
    for (; di <= (uint32_t)dw; ++di) {
        LcDisp d;
        if (!LcDispGet(di, d)) break;
        if (!d.p0) {  // jeste nezobrazeno: pristi interval; starsi bez presentu = present selhal / okno bez obrazu
            if (di + 8 < (uint32_t)dw) {
                g_lc.lastD = di;
                continue;
            }
            break;
        }
        while (fi <= hi && (g_lcF[fi % kLcN].id != fi || !g_lcF[fi % kLcN].post || g_lcF[fi % kLcN].post < d.real)) ++fi;
        if (fi > hi) break;  // slot patri pozdejsimu snimku
        g_lc.lastD = di;
        const LcFrame& r = g_lcF[fi % kLcN];
        if (r.post != d.real) continue;  // obsah snimku mimo zaznam
        const LONGLONG t = d.g1 ? std::max(d.g1, d.p1) : d.p1;
        ++nDisp;
        if (d.g1) ++nDispTs;
        LONGLONG& a = s_dispFirst[fi % kLcN];
        if (!a || t < a) a = t;
        if (!d.gen) s_dispFull[fi % kLcN] = t;
    }
    // hloubka: osamely vstup mysi -> prvni snimek s otocenim kamery
    for (uint32_t m = lo > 8 ? lo : 8; m + 8 <= hi; ++m) {
        LcPump p, q;
        if (!LcPumpGet(m, p) || !p.mouse) continue;
        bool iso = true;
        for (uint32_t k = 1; k <= 4 && iso; ++k) iso = LcPumpGet(m - k, q) && !q.mouse;
        for (uint32_t k = 0; k < 2 && iso; ++k) {
            const LcFrame& r = g_lcF[(m - k) % kLcN];
            iso = r.id == m - k && r.cam >= 0.0f && r.cam < 0.01f;
        }
        if (!iso) continue;
        for (uint32_t k = 1; k <= 7; ++k) {
            const LcFrame& r = g_lcF[(m + k) % kLcN];
            if (r.id != m + k || r.cam < 0.0f) break;
            if (r.cam >= 0.01f) {
                ++g_lc.hist[k];
                break;
            }
        }
    }
    unsigned hn = 0, hb = 0;
    for (int k = 1; k < 8; ++k) {
        hn += g_lc.hist[k];
        if (g_lc.hist[k] > g_lc.hist[hb]) hb = (unsigned)k;
    }
    const bool twoThreads = g_lcPumpTid && (DWORD)g_lcPumpTid != g_lcRendTid;
    const uint32_t dep = hn >= 5 && hb ? hb : twoThreads ? 2u : 1u;
    // histogram jen z posledniho obdobi: hloubka se meni s rezimem (SampleSync: d = 1, kdyz pumpa spi; d = 2, kdyz
    // hlavni vlakno zase ceka na render, napr. bez generovani nad cilem)
    if (hn > 40)
        for (int k = 1; k < 8; ++k) g_lc.hist[k] >>= 1;
    LcStat sStart, sPres, sDisp, sDispFull, sPresDisp, sAge, sWork, sWait, sWake, sIdle;
    unsigned nF = 0, nNoPump = 0, nNoDisp = 0, nFg = 0;
    for (uint32_t f = lo; f <= hi; ++f) {
        const LcFrame& r = g_lcF[f % kLcN];
        if (r.id != f) continue;
        ++nF;
        const LcFrame& pr = g_lcF[(f - 1) % kLcN];
        if (pr.id == f - 1 && pr.rel && r.first >= pr.rel && Ms(pr.rel, r.first) < 100.0) sIdle.add(Ms(pr.rel, r.first));
        LcPump p, p0;
        if (f <= dep || !LcPumpGet(f - dep, p) || p.t > r.first || Ms(p.t, r.call) > 250.0) {
            ++nNoPump;
            continue;
        }
        sStart.add(Ms(p.t, r.first));
        sPres.add(Ms(p.t, r.call));
        if (LcPumpGet(f - dep - 1, p0) && p.t > p0.t && Ms(p0.t, p.t) < 100.0) {
            sAge.add(0.5 * Ms(p0.t, p.t));
            if (p0.waitB && p0.waitB <= p.t) sWake.add(Ms(p0.waitB, p.t));
        }
        if (p.waitA) {
            sWork.add(Ms(p.t, p.waitA));
            sWait.add(Ms(p.waitA, p.waitB));
        }
        LONGLONG disp = 0;
        if (s_dispFirst[f % kLcN]) {
            ++nFg;
            disp = s_dispFirst[f % kLcN];
            if (s_dispFull[f % kLcN]) sDispFull.add(Ms(p.t, s_dispFull[f % kLcN]));
        } else if (!nDisp) {  // bez presenteru FG v intervalu: konec GPU (DXVK prezentuje hned)
            if (gpuOk && r.gpuTs > 0.0) disp = (LONGLONG)((r.gpuTs + off) * (double)g_qpf.QuadPart / 1000.0);
        }
        if (disp && disp > r.call && Ms(r.call, disp) < 250.0) {
            sDisp.add(Ms(p.t, disp));
            sPresDisp.add(Ms(r.call, disp));
        } else {
            ++nNoDisp;
        }
    }
    const LONG pumps = InterlockedExchange(&g_lcPumps, 0);
    char hs[64];
    snprintf(hs, sizeof hs, "%u/%u/%u/%u", g_lc.hist[1], g_lc.hist[2], g_lc.hist[3], g_lc.hist[4]);
    char fg[200] = "";
    if (nFg)
        snprintf(fg, sizeof fg, ", skutecny snimek (plna odezva) %.2f / %.2f", sDispFull.avg(), sDispFull.pct(0.95));
    Log("latence vstupu %.1f s (jen mereni): snimku %u, hloubka fronty d = %u (%s; skok kamery d=1/2/3/4: %s), "
        "kresleni %s | od odberu vstupu (pumpa zprav) prum / p95: -> zacatek kresleni %.2f / %.2f, -> Present %.2f / %.2f, "
        "-> zobrazeni %.2f / %.2f ms%s (%s) | Present -> zobrazeni %.2f / %.2f ms | stari vstupu pri odberu ~%.2f ms (pul "
        "intervalu pump) | hlavni vlakno po odberu: prace %.2f, ceka na render %.2f (p95 %.2f), probuzeni -> pumpa %.3f ms | "
        "render: necinnost po uvolneni snimku %.2f ms | pump se znackou %ld (%.2f na snimek), znacek ztraceno %u, bez "
        "parovani %u, bez zobrazeni %u",
        el / 1000.0, nF, dep, hn >= 5 && hb ? "zmereno" : "predpoklad", hs,
        twoThreads ? "na jinem vlakne nez pumpa" : "na vlakne pumpy", sStart.avg(), sStart.pct(0.95), sPres.avg(),
        sPres.pct(0.95), sDisp.avg(), sDisp.pct(0.95), fg,
        nFg ? (nDispTs ? "presenter FG, prvni slot s obsahem, konec kopie na GPU" : "presenter FG, prvni slot, navrat presentu")
            : gpuOk ? "konec GPU" : "nezname",
        sPresDisp.avg(), sPresDisp.pct(0.95), sAge.avg(), sWork.avg(), sWait.avg(), sWait.pct(0.95), sWake.avg(), sIdle.avg(),
        pumps, nF ? (double)pumps / (double)nF : 0.0, g_lc.lost, nNoPump, nNoDisp);
    g_lc.lost = 0;
    // CSV (ladeni): snimky, pumpy a sloty intervalu
    if (g_lcCsv && !g_lc.csvFailed) {
        if (!g_lc.csv) {
            const std::wstring path = std::wstring(ProxyDir()) + L"\\bms_dlss_latchain.csv";
            g_lc.csv = _wfopen(path.c_str(), L"w");
            if (!g_lc.csv) {
                g_lc.csvFailed = true;
                return;
            }
            fprintf(g_lc.csv, "# qpf=%lld; F,id,call,first,rel,post,cam,gpuEnd,dispFirst,dispFull; P,id,t,mouse,waitA,waitB; "
                    "D,id,real,gen,p0,p1,g1\n", (long long)g_qpf.QuadPart);
        }
        for (uint32_t f = lo; f <= hi; ++f) {
            const LcFrame& r = g_lcF[f % kLcN];
            if (r.id != f) continue;
            const LONGLONG ge = gpuOk && r.gpuTs > 0.0 ? (LONGLONG)((r.gpuTs + off) * (double)g_qpf.QuadPart / 1000.0) : 0;
            fprintf(g_lc.csv, "F,%u,%lld,%lld,%lld,%lld,%.4f,%lld,%lld,%lld\n", f, r.call, r.first, r.rel, r.post, r.cam, ge,
                    s_dispFirst[f % kLcN], s_dispFull[f % kLcN]);
            LcPump p;
            if (LcPumpGet(f, p)) fprintf(g_lc.csv, "P,%u,%lld,%u,%lld,%lld\n", f, p.t, p.mouse, p.waitA, p.waitB);
        }
        static uint32_t s_dCsv;
        for (uint32_t i = std::max(s_dCsv + 1, g_lc.lastD > (uint32_t)kLcN - 64 ? g_lc.lastD - ((uint32_t)kLcN - 64) : 1u);
             i <= g_lc.lastD; ++i) {
            LcDisp d;
            if (LcDispGet(i, d)) fprintf(g_lc.csv, "D,%u,%lld,%u,%lld,%lld,%lld\n", i, d.real, d.gen, d.p0, d.p1, d.g1);
        }
        s_dCsv = g_lc.lastD;
        fflush(g_lc.csv);
    }
}

// Vlakno renderu: lze tento snimek spat v pumpe? Pumpa na vlakne okna hry bezi (zacatek pumpy do 100 ms; hlavni vlakno
// muze prave spat pred odberem znacky) a znacky se odebiraji (nevyrizena nejvys 100 ms).
static bool SsReady() {
    if (!g_ssOn || !g_lcOn || !g_lcMsg || !g_lc.tid || !g_gtPeekHooked) return false;
    const LONGLONG now = Qpc(), q100 = g_qpf.QuadPart / 10;
    const bool ok = (DWORD)g_lcPumpTid == g_lc.tid && now - InterlockedCompareExchange64(&g_ssLoopQ, 0, 0) < q100 &&
                    (g_lcMarkOut == 0 || now - g_lc.markQ < q100);
    if (ok != g_ssWasReady) {
        g_ssWasReady = ok;
        if (++g_ssFallbackLog <= 20 || g_ssFallbackLog % 100 == 0)
            Log("latence: SampleSync %s", ok ? "spi v pumpe zprav (pred odberem vstupu)"
                                            : "- znacka pumpy neodebrana, spanek po Present (zaloha)");
    }
    if (!ok) ++g_ssFallback;
    return ok;
}
// Vlakno renderu, uvolneni snimku (konec H_Present po vsech spancich proxy): znacka pro pumpu, radek do logu
static void LcRelease() {
    if (!g_lcOn || !g_lcFid) return;
    const LONGLONG now = Qpc();
    g_lcF[g_lcFid % kLcN].rel = now;
    g_lcRendTid = GetCurrentThreadId();
    g_lcFirstQ = 0;
    g_lcWantFirst = true;
    if (!g_lcMsg) {
        g_lcMsg = RegisterWindowMessageW(L"bms_dlss_latchain_mark");
        if (!g_lcMsg) {
            g_lcOn = false;
            Log("latence vstupu: RegisterWindowMessage selhal (%lu) - mereni vypnuto", GetLastError());
            return;
        }
        g_lc.logQ = now;
        Log("latence vstupu: mereni zapnuto ([debug] LatChain=1): znacka pumpy zprav, zacatek kresleni, Present, "
            "zobrazeni%s", g_lcCsv ? ", CSV bms_dlss_latchain.csv" : "");
    }
    if (g_gameWnd && (!g_lc.tid || now - g_lc.tidQ > g_qpf.QuadPart)) {
        g_lc.tidQ = now;
        g_lc.tid = GetWindowThreadProcessId(g_gameWnd, nullptr);
    }
    if (g_ssOn) SsCommit();  // pred znackou: co ma pumpa se znackou tohoto snimku prospat
    if (g_lc.tid) {
        const bool out = g_lcMarkOut != 0;
        if (!out || now - g_lc.markQ > g_qpf.QuadPart / 4) {
            if (out) ++g_lc.lost;
            if (PostThreadMessageW(g_lc.tid, g_lcMsg, (WPARAM)g_lcFid, 0)) {
                InterlockedExchange(&g_lcMarkOut, 1);
                g_lc.markQ = now;
            }
        }
    }
    const double el = Ms(g_lc.logQ, now);
    if (el >= 5000.0) {
        g_lc.logQ = now;
        LcAnalyze(el);
        if (g_ssOn) {
            static LONG s_nl, s_nt;  // g_ssNLim / g_ssNLate se nenuluji (cte je i FgLimiter jako rozdil)
            const LONG n = InterlockedExchange(&g_ssNSleep, 0), nr = InterlockedExchange(&g_ssNReg, 0),
                       nl = g_ssNLim - s_nl, nt = g_ssNLate - s_nt;
            s_nl += nl;
            s_nt += nt;
            const double sum = LcQms(InterlockedExchange64(&g_ssSumQ, 0)), err = LcQms(InterlockedExchange64(&g_ssErrSumQ, 0)),
                         mx = LcQms(InterlockedExchange64(&g_ssMaxQ, 0));
            const LONG pk0 = InterlockedExchange(&g_gtPeekN[0], 0), pk1 = InterlockedExchange(&g_gtPeekN[1], 0),
                       pk2 = InterlockedExchange(&g_gtPeekN[2], 0), pk3 = InterlockedExchange(&g_gtPeekN[3], 0);
            Log("latence: SampleSync %.1f s: spanek v pumpe zprav %ld x (limiter FG %ld, regulator %ld), prum %.2f / max %.2f "
                "ms, odchylka probuzeni %.3f ms, slot limiteru nestihnut %ld, zverejneno %u snimku, zaloha (spanek po "
                "Present) %u snimku | PeekMessageA hlavniho vlakna %ld x (zacatku pumpy %ld, z inputsystem.dll %ld), jinych "
                "vlaken %ld",
                el / 1000.0, n, nl, nr, n ? sum / n : 0.0, mx, n ? err / n : 0.0, nt, g_ssUsed, g_ssFallback,
                pk0 + pk1, pk3, pk0, pk2);
            g_ssUsed = g_ssFallback = 0;
        }
    }
}
