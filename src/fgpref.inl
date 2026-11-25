// bms-dlss: "FG jen tam, kde se vyplati" - kroky 1 a 2 (docs/vyzkum-fg-kde-se-vyplati.md kap. 2.3, 3.1, 7, 10.3).
// [fg] Preference: kdy adaptivni rezimy (FSR adaptivni, DLSS-G adaptivni) generuji. Rozhoduje proxy podle FPS hry bez
// generovani a profilu hrace; helper (FgAdaptMakePlan) jen prepne GENERUJE <-> JEN SKUTECNE podle
// BmsdPresentShared::adaptPref. Generator, presenter ani swapchain se pri tom nerusi (jako dnesni prepnuti helperu).
// Pevne nasobky a Multiplier=auto se nemeni. Vklada ho present.cpp za fgest.inl (pouziva jeho snimky a kG).
//
// FPS hry bez generovani B:
//   - jen skutecne: zmereno, 1000 / prumerny interval snimku za posledni ~1 s; spal-li limiter > 3 % casu (hra drzena
//     na cili), plati vyssi z namereneho a odhadu schopnosti nize;
//   - generuje: odhad z kroku 0b (fgest.inl) B^ = 1000 / max(M, R, G - kG * c64): M = cas hlavniho vlakna na snimek,
//     R = CPU vlakna renderu, G = engine GPU (hra + helper) na skutecny snimek, kG jen z drivejsich prepnuti
//     (bez vzorku 1).
// Zisk zobrazenych FPS: generuje = vystup / min(B^, cil) - 1 (namereno); jen skutecne = predpoved O / min(B, cil) - 1,
//   O = min(cil, R^ x nasobek), R^ = skutecne FPS s generatorem (omezeno GPU 1000 / (1000 / B + kG * c), omezeno CPU B;
//   cena jeste nezmerena: R^ = B, prvni zapnuti je zkusebni); pod AdaptMinFps nasobek nejvys AdaptLowMult.
// Pravidla (kap. 2.3 se zmenami 10.3):
//   zapnout (jen skutecne -> generuje), kdyz plati vse (B < N_on, B < 0,97 x cil, zisk >= g) ted a aspon v 80 %
//     vyhodnoceni za posledni 3 s (v103f, K3: hra kolisajici v pasmu hystereze); hned po 1 s jen pri B < 0,8 x N_on;
//   vypnout, kdyz plati kterekoli ted a aspon v 80 % vyhodnoceni za posledni 4 s: B^ > N_off = N_on x (1 + hystereze);
//     (1 + zisk) < (1 + g) / (1 + hystereze), az s >= 2 vzorky kG (hra omezena CPU hned, k se tam nepouziva);
//     B^ > 1,08 x 0,95 x cil (pojistka v102t: hra by cil stihla sama);
//   v kazdem stavu aspon 3 s (vypnuti helperem kvuli neuzitecnosti plati hned); znovu zapnuto do 30 s po vypnuti podle
//   odhadu = cyklus (odhad nevysel / scena kolisa) -> dalsi vypnuti podle odhadu nejdriv za 10, 20, 40 .. 120 s
//   (backoff; po 120 s bez cyklu zpet 10 s).
// Neplatne okno (rozhodnuti se nemeni, casovace od nuly): Alt+Tab a ochranna doba po navratu (ResumeGuard), menu modu,
//   kurzor viditelny (menu hry, konzole), okno hry neni v popredi, FG pozastavena, snimek delsi nez max(300 ms,
//   3 x prumer okna) (nacitani), mene nez 5 snimku nebo 0,7 s dat v soucasnem stavu (0,3 s po prepnuti se nepocita).
//   Zadna doba ani prah neni odvozeny z rychlosti tohoto PC: pocty snimku a FPS jsou z mereni za behu.
// Bez casu vlaken hry ([debug] GameThreads=0) nebo bez casu GPU (citace jadra grafiky; casove znacky jen bez zamku)
//   preference nerozhoduje: chova se jako Preference=always (do v103d) a duvod je v logu a v menu.
// Radek "FG kdy generovat" (~5 s) za "odhad FG"; samokontroly SC_PREFGEN a SC_PREFFLAP.

// v103f2 (rozhodnuti uzivatele 2026-11-24): prahy v PROCENTECH CILE vystupu F (TargetFps, jinak strop zobrazeni);
// porovnava se dal FPS hry bez generovani (B / B^). Hystereze = dvojice prahu (zapnout pod / vypnout nad).
//   responsiveness 50 / 60 %, balanced (vychozi) 75 / 85 %, smooth 85 / 95 %, custom PreferencePct / + PreferenceHyst
//   procentnich bodu (vypnuti nejvys 97 %), steady 105 / 125 FPS (pokus, absolutne), always = jako v103d.
struct PfProf {
    int mode;          // PREF_*, ktery opravdu plati
    double nOn, nOff;  // prah zapnuti / vypnuti (FPS hry bez generovani)
    double pOn, pOff;  // tytez prahy v % cile (steady 0)
    double g;          // nejmensi zisk (podil, 0,25 = 25 %)
    bool gainRule;     // profil pouziva zisk (steady ne)
    const char* note;  // proc plati jiny profil, nez je v ini (nullptr = zadny)
};

static PfProf PfProfile(double F) {
    PfProf p = {};
    p.mode = cfg.prefMode;
    if (p.mode == PREF_STEADY) {
        if (g_fgMode != BMSD_FG_DLSSGADAPT) p.note = "steady jen s DLSS-G adaptivnim";
        else if (!FgLockOn()) p.note = "steady potrebuje zlomkovy zamek (AdaptLock)";
        else if (F < 100.0) p.note = "steady jen pro cil vystupu >= 100 FPS";
        if (p.note) p.mode = PREF_BAL;
    }
    p.gainRule = true;
    switch (p.mode) {
    case PREF_RESP: p.pOn = 50.0; p.pOff = 60.0; p.g = 0.40; break;
    case PREF_SMOOTH: p.pOn = 85.0; p.pOff = 95.0; p.g = 0.15; break;
    case PREF_CUSTOM: p.pOn = (double)cfg.prefPct; p.pOff = p.pOn + (double)cfg.prefHyst; p.g = 0.25; break;
    case PREF_STEADY: p.nOn = 105.0; p.nOff = 125.0; p.g = 0.0; p.gainRule = false; break;
    default: p.pOn = 75.0; p.pOff = 85.0; p.g = 0.25; break;
    }
    if (p.mode != PREF_STEADY) {
        p.pOff = std::min(97.0, std::max(p.pOn, p.pOff));
        if (g_scTest & 32) p.pOff = p.pOn;  // SelfCheckTest 32: hystereze 0
        p.nOn = F * p.pOn / 100.0;
        p.nOff = F * p.pOff / 100.0;
        if (cfg.prefMinGain >= 0) p.g = cfg.prefMinGain / 100.0;
    }
    return p;
}

static struct {
    bool gate;               // preference rozhoduje (helper posloucha adaptPref)
    int want = -1;           // pozadovany stav: 1 generovat, 0 jen skutecne
    int act = -1;            // stav helperu (adaptEngaged) pri poslednim vyhodnoceni
    LONGLONG actSince, evalQ, onSince, offSince, offAt, offBlockUntil, lastGuardQ, curSince;
    bool curIgnore;
    double backoffS = 10.0;
    LONGLONG lastCycQ;
    // zdroj odhadu: 0 = zatim nevime, 1 = k dispozici, -1 = chybi (srcWhy)
    int src;
    unsigned tries, thrOk, gpuOk, badBlocks;
    const char* srcWhy = "zatim se meri";
    // posledni platne vyhodnoceni
    int why = PFI_WAIT, prof = PREF_BAL;
    double B, Bm, Bc, M, R, G, kg, c, Rh, O, gain, out, F, nOn, nOff, g, dL, inState;
    double capFps;           // strop skutecnych FPS ve stavu jen skutecne (capK x moznosti GPU; 0 = jen cil)
    double capK = 1.0;       // podil moznosti GPU pro strop (1 = bez stropu; snizuje se az po namerenem preskakovani)
    double capCeil = 1.0;    // kam se k vraci (k pri poslednim preskakovani - 0,01; po 30 s bez preskakovani +0,01)
    LONGLONG skQ, kFreezeUntil, engLostQ;
    uint32_t sk0, pr0;
    unsigned skBadRun;       // cistych 2s oken po sobe s preskocenymi > 1 %
    double skOkS, skCeilS;   // sekund cistych oken bez preskakovani (navrat k / stropu navratu)
    bool skDirty;            // v okne byl zaskub nebo neplatne okno -> do ladeni k se nepocita
    double serK0, serSh0;    // serie snizovani: k pred prvnim snizenim a podil preskocenych tehdy
    LONGLONG capGiveQ;       // v103g: strop na dolni mezi nepomohl -> generovat (0 = ne)
    double capGiveS = 120.0; // ... a pak tak dlouho nevypinat podle odhadu (dalsi x2, nejvys 600 s)
    uint64_t skIntS, skIntP; // interval radku: preskoceno / celkem ve stavu jen skutecne
    // historie podminek prepnuti (K3): platna vyhodnoceni v soucasnem stavu
    struct { LONGLONG t; uint8_t on, off; } hist[64];
    unsigned hn;
    unsigned kgn;
    bool cpu, est, gainRule, gainKnown, valid;
    const char* bad = "zatim zadne okno";
    const char* profNote;
    // interval radku
    unsigned nEval, nBad, nSw, nSwPref, nOnReq, nOffReq;
    const char* badLast;
    // zobrazeno celkem (presCount*) po ~100 ms - vystup za posledni ~1 s
    LONGLONG pq[24];
    uint64_t pc[24];
    unsigned pn;
    // samokontroly
    LONGLONG genAgainstSince;
    double genAgainstMs, genAgainstB;
    LONGLONG swT[16];        // casy prepnuti helperu mimo udalosti (zadna ochrana / menu / pauza 2 s pred nim)
    unsigned swN;
    uint32_t pub;
} g_pf;

static void PfPublish(uint32_t w) {
    if (g_pf.pub == w || !g_sh) return;
    g_pf.pub = w;
    g_sh->adaptPref = w;
    MemoryBarrier();
}

// v103g: zakladni k ([debug] PrefCapK, 1 = bez stropu)
static double PfKBase() { return cfg.prefCap ? std::min(1.0, std::max(0.85, cfg.prefCapK)) : 1.0; }
// Strop "jen skutecne" znovu od zakladniho k (PrefCapK; 1 = bez stropu): prepnuti stavu, nacitani / zmena sceny, start rozhodovani
static void PfCapReset(const char* why) {
    auto& P = g_pf;
    const double kb = PfKBase();
    if (why && (fabs(P.capK - kb) > 0.001 || fabs(P.capCeil - kb) > 0.001))
        Log("FG kdy generovat: strop jen skutecne k %.2f -> %.2f (%s) - %s", P.capK, kb, kb < 0.999 ? "zakladni" : "bez stropu", why);
    P.capK = P.capCeil = kb;
    P.capFps = 0.0;
    P.skBadRun = 0;
    P.skOkS = P.skCeilS = 0.0;
    P.serK0 = 0.0;
    P.skQ = P.kFreezeUntil = 0;
}

// Historie podminek (K3): podil vyhodnoceni s podminkou za poslednich sec s; span = jak daleko historie saha
static void PfHistPush(LONGLONG now, bool on, bool off) {
    auto& h = g_pf.hist[g_pf.hn++ % 64u];
    h.t = now;
    h.on = on ? 1 : 0;
    h.off = off ? 1 : 0;
}
static double PfHistFrac(LONGLONG now, double sec, bool off, double* span) {
    const LONGLONG q1s = g_qpf.QuadPart, from = now - (LONGLONG)(sec * (double)q1s);
    unsigned n = 0, k = 0;
    LONGLONG old = now;
    for (unsigned i = 0; i < 64u && i < g_pf.hn; ++i) {
        const auto& h = g_pf.hist[(g_pf.hn - 1 - i) % 64u];
        if (h.t < from) break;
        ++n;
        if (off ? h.off : h.on) ++k;
        old = h.t;
    }
    *span = (double)(now - old) / (double)q1s;
    return n >= 5 ? (double)k / n : 0.0;
}

// Okno posledni ~1 s v soucasnem stavu (snimky z fgest.inl), bez 0,3 s po prepnuti
struct PfWin {
    const char* bad;  // nullptr = platne
    unsigned n, nT, nG, nGs;
    double sDt, sBusy, sM, sR, sGb, sGs, maxDt, cm, ce, span;
    uint32_t lock;
    bool lockSame;
    uint8_t grd, mode;
};
static PfWin PfWindow(LONGLONG now, int st, LONGLONG since, uint8_t grdMask) {
    PfWin w = {};
    w.lockSame = true;
    const LONGLONG q1s = g_qpf.QuadPart;
    const LONGLONG from = std::max(now - q1s, since + q1s * 3 / 10);
    LONGLONG tNew = 0, tOld = 0;
    for (unsigned i = g_feN; i-- > 0 && g_feN - i < (unsigned)kFeRing - 2;) {
        const FeFrame& f = g_fe[i % kFeRing];
        if (f.t < from) break;
        if (!w.n) {
            tNew = f.t;
            w.lock = f.lock;
        }
        tOld = f.t;
        if (f.st != st || (st == FE_REAL && f.why != FEW_HELPER)) {
            w.bad = "FG pozastavena nebo jiny stav";
            return w;
        }
        if (!f.fore) w.bad = "okno hry neni v popredi";
        w.grd |= f.grd & grdMask;
        if (f.lock != w.lock) w.lockSame = false;
        ++w.n;
        w.sDt += f.dt;
        w.sBusy += f.busy;
        if (f.dt > w.maxDt) w.maxDt = f.dt;
        if (f.tok && f.dt > 0.0f && f.rc <= 2.0f * f.dt && f.mc <= 2.0f * f.dt) {  // CPU cas obcas nesmyslny (zmena vlakna)
            const double m = (g_ssOn ? (double)f.busy : (double)f.dt) - (double)f.wr;
            w.sM += std::max(m, (double)f.mc);
            w.sR += f.rc;
            ++w.nT;
        }
        if (f.gb >= 0.0f) {
            w.sGb += f.gb;
            ++w.nG;
        }
        if (f.gs > 0.0f) {
            w.sGs += f.gs;
            ++w.nGs;
        }
        if (w.cm <= 0.0 && f.cm > 0.0f) w.cm = f.cm;  // nejnovejsi
        if (w.ce <= 0.0 && f.ce > 0.0f) w.ce = f.ce;
        if (!w.mode && f.mode) w.mode = f.mode;
    }
    w.span = tNew > tOld ? (double)(tNew - tOld) * 1000.0 / (double)q1s : 0.0;
    if (w.bad) return w;
    if (w.grd & 1u) w.bad = "Alt+Tab / ochrana po navratu";
    else if (w.grd & 2u) w.bad = "menu modu otevrene";
    else if (w.grd & 4u) w.bad = "kurzor viditelny (menu hry, konzole)";
    else if (w.n < 5 || w.span < 700.0) w.bad = "malo dat v soucasnem stavu";
    else if (w.maxDt > std::max(300.0, 3.0 * w.sDt / w.n)) w.bad = "dlouhy snimek (nacitani, pauza)";
    else if (st == FE_GEN && !w.lockSame) w.bad = "zmena stupne zamku";  // v103f (S5): cena c64 se zpozduje za stupnem
    return w;
}

static const char* PfWhyTxt(int why) {
    switch (why) {
    case BMSD_PFW_GEN: return "hra pod prahem profilu, zisk staci";
    case BMSD_PFW_ABOVE: return "hra nad prahem profilu";
    case BMSD_PFW_GAIN: return "zisk zobrazenych FPS pod minimem profilu";
    case BMSD_PFW_TARGET: return "hra stiha cil sama";
    case BMSD_PFW_HOLD: return "v pasmu hystereze nebo ceka na casovac - stav drzi";
    case BMSD_PFW_GUARD: return "pauza / nacitani / Alt+Tab / menu - stav se nemeni";
    case BMSD_PFW_USELESS: return "helper vypnul generovani jako neuzitecne (backoff)";
    case BMSD_PFW_BLOCK: return "vypnuti podle odhadu blokovane (odhad nevysel, backoff)";
    case PFI_ALWAYS: return "Preference=always: dolni / horni hranice jako do v103d";
    case PFI_NOEST: return "odhad FPS bez generovani neni k dispozici - chova se jako always";
    case PFI_WAIT: return "meri se";
    case PFI_OFF: return "adaptivni rezim nebezi";
    default: return "?";
    }
}

// Ladeni k stropu "jen skutecne" (K2, v103f) podle presenteru v procesu hry, po 2s oknech. Okno se zaskubem (snimek
// > 2 x prumer a > 25 ms), neplatnym vyhodnocenim (Alt+Tab, menu, nacitani, pauza) nebo bez dost obrazu se nepocita.
//  - preskoceno > 1 % a GPU na hrane (1000 / G < 1,2 x cil) ve 2 oknech po sobe (> 5 % v jednom) -> k - 0,02
//    (pri > 5 % nejvys 0,95: t1 bez stropu 10-15 % pri GPU 100 %, t1c 94-97 % -> 0,01-0,8 %); strop navratu =
//    k pred snizenim - 0,01;
//  - snizeni o 0,04 v jedne serii nezlepsilo podil (>= 0,7 x puvodniho): preskakovani ma jinou pricinu -> k zpet
//    a ladeni na 60 s stoji;
//  - okno s < 0,5 % preskocenych: po 5 s k + 0,01 (nejvys strop navratu), po 30 s na stropu navratu strop + 0,01;
//  - nacitani (snimek > 1 s), prepnuti stavu a start rozhodovani: k = 1 (PfCapReset).
static void PfCapTune(LONGLONG now, int act, const PfWin& w, double G, double F) {
    auto& P = g_pf;
    const LONGLONG q1s = g_qpf.QuadPart;
    if (act || !P.gate || !cfg.prefCap || !g_sh->gpEnable) {
        P.skQ = 0;
        return;
    }
    if (w.n && w.maxDt > 1000.0) PfCapReset("snimek > 1 s (nacitani, zmena sceny)");
    if (w.bad || (w.n && w.maxDt > std::max(25.0, 2.0 * w.sDt / w.n))) P.skDirty = true;  // zaskub: > 2 x prumer a > 25 ms
    const uint32_t sk = g_sh->gpSkipped, pr = g_sh->gpPresented;
    if (!P.skQ || now - P.skQ > 3 * q1s || sk < P.sk0 || pr < P.pr0) {
        P.skQ = now;
        P.sk0 = sk;
        P.pr0 = pr;
        P.skDirty = w.bad != nullptr;
        return;
    }
    const double win = (double)(now - P.skQ) / (double)q1s;
    if (win < 2.0) return;
    const uint32_t ds = sk - P.sk0, dp = pr - P.pr0;
    P.skIntS += ds;
    P.skIntP += (uint64_t)ds + dp;
    if (!P.skDirty && ds + dp >= 20) {
        const double sh = (double)ds / (double)(ds + dp);
        const bool edge = G > 0.5 && 1000.0 / G < 1.2 * F;
        if (sh > 0.01 && edge) {
            P.skOkS = P.skCeilS = 0.0;
            ++P.skBadRun;
            if (now >= P.kFreezeUntil && (P.skBadRun >= 2 || sh > 0.05)) {
                P.skBadRun = 0;
                // v103g (pozadavek uzivatele): pri trvalem preskakovani se k nevraci na 1 (v103f "snizeni nepomohlo" ->
                // 60 s ~10 % preskocenych), ale klesa dal az na 0,85; kdyz ani dolni mez nepomuze, radeji generovat
                // (capGiveQ: preference zapne generovani a 120 s ho podle odhadu nevypne)
                if (P.capK <= 0.851) {
                    if (!P.capGiveQ) {
                        P.capGiveQ = now;
                        Log("FG kdy generovat: strop jen skutecne k %.2f (dolni mez) a presenter dal preskakuje %.1f %% za %.1f s - "
                            "radeji generovat (120 s bez vypnuti podle odhadu)",
                            P.capK, sh * 100.0, win);
                    }
                } else if (P.capK > 0.851) {
                    if (P.serK0 <= 0.0) {
                        P.serK0 = P.capK;
                        P.serSh0 = sh;
                    }
                    const double k0 = P.capK;
                    P.capCeil = std::min(P.capCeil, k0 - 0.01);
                    P.capK = std::max(0.85, sh > 0.05 ? std::min(k0 - 0.05, 0.95) : k0 - 0.02);
                    Log("FG kdy generovat: strop jen skutecne k %.2f -> %.2f - presenter preskocil %u z %u obrazu (%.1f %%) za %.1f s, "
                        "GPU %.2f ms na snimek (moznost %.1f FPS), navrat nejvys k %.2f",
                        k0, P.capK, ds, ds + dp, sh * 100.0, win, G, 1000.0 / G, P.capCeil);
                }
            }
        } else if (sh < 0.005) {
            P.skBadRun = 0;
            P.serK0 = 0.0;
            P.skOkS += win;
            if (P.capK < P.capCeil - 1e-6) {
                P.skCeilS = 0.0;
                if (P.skOkS >= 5.0) {
                    P.capK = std::min(P.capCeil, P.capK + 0.01);
                    P.skOkS = 0.0;
                    if (P.capK >= P.capCeil - 1e-6)
                        Log("FG kdy generovat: strop jen skutecne k zpet na %.2f (bez preskakovani)", P.capK);
                }
            } else if (P.capCeil < PfKBase() - 0.001) {
                P.skCeilS += win;
                if (P.skCeilS >= 30.0) {
                    P.capCeil = std::min(PfKBase(), P.capCeil + 0.01);
                    P.capK = P.capCeil;
                    P.skCeilS = 0.0;
                    if (P.capK >= PfKBase() - 0.001)
                        Log("FG kdy generovat: strop jen skutecne k %.2f (%s) - 30 s bez preskakovani", P.capK,
                            P.capK >= 0.999 ? "bez stropu" : "zakladni");
                }
            }
        } else {
            P.skBadRun = 0;
            P.skOkS = P.skCeilS = 0.0;
        }
    }
    P.skQ = now;
    P.sk0 = sk;
    P.pr0 = pr;
    P.skDirty = w.bad != nullptr;
}

static void FgPrefFrame(LONGLONG now) {
    auto& P = g_pf;
    const LONGLONG q1s = g_qpf.QuadPart;
    const bool adapt = cfg.inMain && g_sh && g_state == PS_HELPER && FgIsAdapt((int)g_fgMode);
    if (!adapt || cfg.prefMode == PREF_ALWAYS) {
        if (P.gate) Log("FG kdy generovat: preference nerozhoduje (%s)", !adapt ? "adaptivni rezim nebezi" : "Preference=always");
        P.gate = false;
        P.want = -1;
        P.act = -1;
        P.why = adapt ? PFI_ALWAYS : PFI_OFF;
        PfCapReset(nullptr);
        P.hn = 0;
        g_bpWant = false;
        PfPublish(0u);
        return;
    }
    if (P.evalQ && now - P.evalQ < q1s / 10) return;  // vyhodnoceni po ~100 ms
    P.evalQ = now;
    MemoryBarrier();
    const int fs = g_sh->fgState;
    const uint32_t am = g_sh->fgActiveMode;
    {
        const unsigned k = P.pn++ % 24u;
        P.pq[k] = now;
        P.pc[k] = g_sh->presCountReal + g_sh->presCountGen;
    }
    if (g_feN) {
        const uint8_t gl = g_fe[(g_feN - 1) % kFeRing].grd;
        if (gl & (P.curIgnore ? 3u : 7u)) P.lastGuardQ = now;
        // kurzor viditelny trvale (prekryv jineho programu, hra s kurzorem): po 60 s ho jako menu hry nebrat
        if (!(gl & 4u)) {
            P.curSince = 0;
            P.curIgnore = false;
        } else if (!P.curSince) {
            P.curSince = now;
        } else if (!P.curIgnore && now - P.curSince > 60 * q1s) {
            P.curIgnore = true;
            Log("FG kdy generovat: kurzor viditelny uz 60 s - dal se nebere jako menu hry");
        }
    }
    if (fs != BMSD_FGS_ACTIVE || !FgIsAdapt((int)am)) {  // FG pozastavena (menu modu, minimalizace) nebo startuje
        P.onSince = P.offSince = 0;
        P.hn = 0;
        P.skDirty = true;
        g_bpWant = false;
        P.genAgainstSince = 0;
        P.genAgainstMs = 0.0;
        P.why = BMSD_PFW_GUARD;
        return;
    }
    const int act = g_sh->adaptEngaged ? 1 : 0;
    if (act != P.act) {
        if (P.act >= 0) {
            ++P.nSw;
            if (now - P.lastGuardQ > 2 * q1s) {  // mimo udalost (samokontrola kmitani)
                P.swT[P.swN % 16u] = now;
                ++P.swN;
            }
        }
        if (P.act == 1 && !act) PfCapReset("prepnuti na jen skutecne");
        P.act = act;
        P.actSince = now;
        P.onSince = P.offSince = 0;
        P.hn = 0;
        P.genAgainstSince = 0;
        P.genAgainstMs = 0.0;
    }
    const bool tst = (g_scTest & 32) != 0;  // SelfCheckTest 32: hystereze 0, casy 0,1 s (vyvola kmitani)
    const double tOn = tst ? 0.1 : 1.0, tOff = tst ? 0.1 : 2.0, minSt = tst ? 0.0 : 3.0;
    const double F = FgTgtEff();
    const PfProf pr = PfProfile(F);
    if (pr.note && pr.note != P.profNote) Log("FG kdy generovat: %s - plati balanced", pr.note);
    P.profNote = pr.note;
    P.prof = pr.mode;
    P.F = F;
    P.nOn = pr.nOn;
    P.nOff = pr.nOff;
    P.g = pr.g;
    P.gainRule = pr.gainRule;
    P.inState = (double)(now - P.actSince) / (double)q1s;
    ++P.nEval;

    PfWin w = PfWindow(now, act ? FE_GEN : FE_REAL, P.actSince, P.curIgnore ? 3u : 7u);
    if (!w.bad && F < 20.0) w.bad = "cil vystupu neznamy";
    // odhad schopnosti hry (0b) a zdroj
    const int mi = (w.mode ? (int)w.mode : (int)g_fgMode) == BMSD_FG_DLSSGADAPT ? 1 : 0;
    const bool thr = w.n && w.nT * 10 >= w.n * 8;
    const bool eng = w.n && w.nG * 10 >= w.n * 8 && (!act || w.cm > 0.0);
    const bool stamps = w.n && w.nGs * 2 >= w.n && (!act || w.ce > 0.0) && !FgLockOn();  // znacky se zamkem +-7 %
    const bool gpuOk = eng || stamps;
    const double M = w.nT ? w.sM / w.nT : 0.0, R = w.nT ? w.sR / w.nT : 0.0;
    const double Gr = eng ? w.sGb / w.nG : stamps ? w.sGs / w.nGs : 0.0;
    const int gi = (eng ? 0 : 4) + mi * 2 + ((act ? w.lock != 0 : FgLockOn()) ? 1 : 0);
    const double kg = g_feS.kg[gi];
    const unsigned kgn = g_feS.kgn[gi];
    const double cc = act ? (eng ? w.cm : w.ce) : 0.0;
    // Bez vzorku kG (zadne platne prepnuti v teto skupine) se pri generovani odecita jen 0,6 x cena: kG vychazelo 0,66
    // (t8b, TargetFps 138, c 4,56 ms) az 1,27 (kap. 14.3) a s kG 1 byl odhad o 25 % vys (133,9 proti ~107) -> chybne
    // vypnuti v pasmu hystereze. 0,6 = odhad spis nizko = generuje dele (jako do v103d). v103f (S4): naucene kG az od
    // 2 vzorku (fgest.inl FeKgUpdate: 2 -> mensi, od 3 median), do te doby nejvys 0,6.
    const double kgUse = act && kgn < 2 ? std::min(kg, 0.6) : kg;
    const double G = Gr - kgUse * cc;
    const bool est = thr && gpuOk && Gr > 0.0 && std::max(G, std::max(M, R)) > 0.5;
    const double Bc = est ? 1000.0 / std::max(G, std::max(M, R)) : 0.0;
    const bool cpu = est && std::max(M, R) > G;
    // zdroj odhadu: po 30 platnych oknech rozhodnout (aspon 2/3 s casem vlaken a GPU)
    if (!w.bad) {
        ++P.tries;
        if (thr) ++P.thrOk;
        if (gpuOk) ++P.gpuOk;
    }
    const int srcOld = P.src;
    if (!g_gtOn) {
        P.src = -1;
        P.srcWhy = "cas vlaken hry vypnuty ([debug] GameThreads=0)";
    } else if (P.tries >= 30) {
        if (P.thrOk < 20) P.srcWhy = "cas vlaken hry chybi (hacky cekacich funkci / vlakno okna hry)";
        else if (P.gpuOk < 20) P.srcWhy = FgLockOn() ? "cas GPU chybi (citace jadra grafiky; casove znacky se zamkem nestaci)"
                                                       : "cas GPU chybi (citace jadra grafiky ani casove znacky)";
        else P.srcWhy = "k dispozici";
        // k dispozici hned po prvnim dobrem bloku; chybi az po 3 spatnych blocich v rade (citace GPU se pri startu
        // hry hledaji nekolik sekund) - do te doby plati posledni stav
        const bool ok = P.thrOk >= 20 && P.gpuOk >= 20;
        P.badBlocks = ok ? 0u : P.badBlocks + 1u;
        if (ok) P.src = 1;
        else if (P.badBlocks >= 3) P.src = -1;
        P.tries = P.thrOk = P.gpuOk = 0;
    }
    if (P.src != srcOld && P.src != 0)
        Log("FG kdy generovat: odhad FPS bez generovani %s", P.src > 0 ? "k dispozici - rozhoduje preference" : P.srcWhy);
    if (P.src > 0 && !P.gate) {
        P.gate = true;
        P.want = act;
        PfCapReset("start rozhodovani");
        Log("FG kdy generovat: Preference=%s rozhoduje (generovat pod %.0f FPS hry, vypnout nad %.0f = %.0f / %.0f %% cile %.1f, "
            "zisk aspon %.0f %%%s), helper ted %s",
            PrefName(pr.mode), pr.nOn, pr.nOff, pr.pOn, pr.pOff, F, pr.g * 100.0, pr.gainRule ? "" : " - nepouziva se",
            act ? "generuje" : "jen skutecne");
    } else if (P.src < 0 && P.gate) {
        P.gate = false;
        Log("FG kdy generovat: preference nerozhoduje - %s; chova se jako Preference=always", P.srcWhy);
    }

    PfCapTune(now, act, w, G, F);
    P.valid = !w.bad;
    P.bad = w.bad;
    if (w.bad) {
        ++P.nBad;
        P.badLast = w.bad;
        P.onSince = P.offSince = 0;
        P.hn = 0;
        P.genAgainstSince = 0;
        P.genAgainstMs = 0.0;
        if (w.grd || !w.n) P.why = BMSD_PFW_GUARD;
    } else {
        // B, zisk, predpoved
        P.M = M;
        P.R = R;
        P.G = G;
        P.kg = kgUse;
        P.kgn = kgn;
        P.est = est;
        P.cpu = cpu;
        P.Bc = Bc;
        const double dt = w.sDt / w.n, busy = w.sBusy / w.n;
        P.Bm = dt > 0.0 ? 1000.0 / dt : 0.0;
        // vystup za posledni ~1 s (vzorek 0,8 - 1,5 s stary, jen ze soucasneho stavu)
        double out = 0.0;
        for (unsigned i = 1; i < 24u && i < P.pn; ++i) {
            const unsigned k = (P.pn - 1 - i) % 24u;
            const double age = (double)(now - P.pq[k]) / (double)q1s;
            if (P.pq[k] < P.actSince + q1s * 3 / 10 || age > 1.5) break;
            if (age >= 0.8) {
                out = (double)(P.pc[(P.pn - 1) % 24u] - P.pc[k]) / age;
                break;
            }
        }
        P.out = out;
        const double minEff = cfg.adaptMinFps > 0 ? std::min((double)cfg.adaptMinFps, F / 2.0) : 0.0;
        double c = 0.0;
        if (g_sh->fgCostCount && g_sh->fgCostMode == (uint32_t)g_fgMode) c = g_sh->fgCostMeanMs;  // posledni znama (i stara)
        P.c = act ? cc : c;
        double B = 0.0;
        if (!act) {
            const double slept = dt > 0.0 ? 1.0 - busy / dt : 0.0;
            // limiter drzi hru na cili nebo na strope jen skutecne (S8: strop nesmi sam stlacit B pod prah): aspon odhad
            // schopnosti
            B = (slept > 0.03 || (P.capFps > 0.0 && P.Bm >= 0.97 * P.capFps)) && Bc > P.Bm ? Bc : P.Bm;
            if (cfg.adaptCap > 0 && B > (double)cfg.adaptCap) B = (double)cfg.adaptCap;  // RealCap: hra rychleji nesmi
            const double mmax = mi ? (g_sh->fgMultMax >= 2 ? (double)g_sh->fgMultMax : 4.0) : 5.0;
            P.Rh = cpu || c <= 0.0 || B <= 0.0 ? B : 1000.0 / (1000.0 / B + kg * c);
            P.O = std::min(F, P.Rh * (P.Rh < minEff ? (double)cfg.adaptLowMult : mmax));
            P.gainKnown = B > 0.0 && F > 0.0;
            P.gain = P.gainKnown ? P.O / std::min(B, F) - 1.0 : 0.0;
        } else {
            B = Bc;
            if (cfg.adaptCap > 0 && B > (double)cfg.adaptCap) B = (double)cfg.adaptCap;
            P.Rh = P.Bm;
            P.O = out;
            P.gainKnown = B > 0.0 && out > 0.0 && F > 0.0;
            P.gain = P.gainKnown ? out / std::min(B, F) - 1.0 : 0.0;
        }
        P.B = B;
        // latence navic (model kap. 2.5, neovereno, jen pro log): 1,5 x (T_r - T_b) + (T_r - P / q) + 1 ms
        {
            const double Tb = B > 0.0 ? 1000.0 / B : 0.0, Tr = P.Rh > 0.0 ? 1000.0 / P.Rh : 0.0;
            const int q = act && w.lock ? (int)(w.lock >> 8 & 0xFFu) : 1;
            const double Pg = F > 0.0 ? 1000.0 / F : 0.0;
            P.dL = Tb > 0.0 && Tr > 0.0 ? 1.5 * std::max(0.0, Tr - Tb) + std::max(0.0, Tr - Pg / std::max(1, q)) + 1.0 : 0.0;
        }
        // Strop "jen skutecne" (v103e1 / v103f): bez stropu (k = 1), dokud presenter v procesu hry nezacne prokazatelne
        // preskakovat; pak capK x 1000 / (engine GPU na snimek). Ladeni k viz PfCapTune. Cas enginu na snimek na strope
        // nezavisi (t1c), takze bez zpetne vazby. Jen se stavem jen skutecne a s preferenci (always = jako v103d).
        if (!act && P.gate && cfg.prefCap && P.capK < 0.999) {
            if (eng && G > 0.5) {
                P.engLostQ = 0;
                const double cap = P.capK * 1000.0 / G;
                P.capFps = P.capFps > 0.0 ? P.capFps + 0.3 * (cap - P.capFps) : cap;
                if (P.capFps >= 0.995 * F) P.capFps = 0.0;  // GPU ma rezervu i na cili: strop = cil
            } else if (!eng) {  // S7: bez casu enginu GPU strop neaktualni -> po 2 s pryc
                if (!P.engLostQ) P.engLostQ = now;
                else if (now - P.engLostQ > 2 * q1s) P.capFps = 0.0;
            }
        } else {
            P.capFps = 0.0;
        }
        const bool helperBlk = (g_sh->adaptReason & 0x100u) != 0 && (g_sh->adaptReason & 0xFFu) == BMSD_ADR_USELESS;
        if (!act) {
            P.genAgainstSince = 0;
            P.genAgainstMs = 0.0;
            const bool onCond = B > 0.0 && B < pr.nOn && B < 0.97 * F && (!pr.gainRule || P.gain >= pr.g);
            P.why = B >= 0.97 * F ? BMSD_PFW_TARGET : B >= pr.nOn ? BMSD_PFW_ABOVE
                    : pr.gainRule && P.gain < pr.g ? BMSD_PFW_GAIN : helperBlk ? BMSD_PFW_USELESS : BMSD_PFW_HOLD;
            if (!onCond) P.onSince = 0;
            else if (!P.onSince) P.onSince = now;
            // K3 (v103f): rozhoduje podil vyhodnoceni s podminkou za 3 s (>= 80 %), ne posledni 1s okno; hned (po 1 s)
            // jen opravdu pomala scena
            PfHistPush(now, onCond, false);
            double span = 0.0;
            const double fr = PfHistFrac(now, tst ? 0.3 : 3.0, false, &span);
            const bool fast = onCond && B < 0.8 * pr.nOn && (double)(now - P.onSince) / (double)q1s >= tOn;
            const bool smooth = onCond && fr >= 0.8 && span >= (tst ? 0.1 : 2.5);
            const bool give = P.capGiveQ != 0;  // v103g: strop na dolni mezi nepomohl - generovat (presenter by preskakoval)
            const bool go = ((fast || smooth) && P.inState >= minSt) || give;
            if (give && P.want != 1 && P.gate) {
                P.offBlockUntil = now + (LONGLONG)(P.capGiveS * (double)q1s);
                Log("FG kdy generovat: -> generovat (strop jen skutecne nepomohl proti preskakovani), vypnuti podle odhadu nejdriv za %.0f s",
                    P.capGiveS);
                P.capGiveS = std::min(600.0, P.capGiveS * 2.0);
                P.capGiveQ = 0;
            }
            if (go && P.want != 1 && P.gate) {
                if (P.offAt) {  // znovu zapnuto do 30 s po vypnuti podle odhadu: cyklus (odhad nevysel nebo scena kolisa)
                    const double offS = (double)(now - P.offAt) / (double)q1s;
                    if (P.lastCycQ && now - P.lastCycQ > 120 * q1s) P.backoffS = 10.0;  // 2 min bez cyklu: od zacatku
                    if (offS < 30.0) {
                        P.offBlockUntil = now + (LONGLONG)(P.backoffS * (double)q1s);
                        P.lastCycQ = now;
                        Log("FG kdy generovat: znovu generovat %.1f s po vypnuti podle odhadu - cyklus (odhad nevysel nebo scena "
                            "kolisa), dalsi vypnuti podle odhadu nejdriv za %.0f s",
                            offS, P.backoffS);
                        P.backoffS = std::min(120.0, P.backoffS * 2.0);
                    }
                    P.offAt = 0;
                }
                ++P.nOnReq;
                Log("FG kdy generovat: -> generovat (%s): hra %.1f FPS < prah %.0f, zisk predpoved %+.0f %% (aspon %.0f %%), "
                    "s FG skutecne %.1f, vystup %.1f, cil %.1f; %.1f s ve stavu",
                    PrefName(pr.mode), B, pr.nOn, P.gain * 100.0, pr.g * 100.0, P.Rh, P.O, F, P.inState);
            }
            if (go) P.why = BMSD_PFW_GEN;
            P.want = go ? 1 : 0;
        } else {
            const bool kOk = cpu || kgn >= 2;
            const bool o1 = est && B > pr.nOff;
            const bool o2 = est && pr.gainRule && kOk && P.gainKnown && 1.0 + P.gain < (1.0 + pr.g) / (pr.nOn > 0.0 ? pr.nOff / pr.nOn : 1.0);
            const bool o3 = est && B > 1.08 * 0.95 * F;
            const bool blocked = !tst && now < P.offBlockUntil;  // SelfCheckTest 32: bez backoffu (vyvola SC_PREFFLAP)
            const bool offCond = (o1 || o2 || o3) && !blocked && !(g_scTest & 8);  // SelfCheckTest 8: nevypinat (SC_PREFGEN)
            P.why = !est ? BMSD_PFW_HOLD : (o1 || o2 || o3) && blocked ? BMSD_PFW_BLOCK : o3 ? BMSD_PFW_TARGET : o1 ? BMSD_PFW_ABOVE
                    : o2 ? BMSD_PFW_GAIN : B >= pr.nOn ? BMSD_PFW_HOLD : BMSD_PFW_GEN;
            if (!offCond) P.offSince = 0;
            else if (!P.offSince) P.offSince = now;
            // K3 (v103f): vypnout, kdyz podminka plati ted a aspon v 80 % vyhodnoceni za posledni 4 s
            PfHistPush(now, false, o1 || o2 || o3);
            double span = 0.0;
            const double fr = PfHistFrac(now, tst ? 0.3 : 4.0, true, &span);
            const bool stop = offCond && fr >= 0.8 && span >= (tst ? 0.1 : 3.5) && (double)(now - P.offSince) / (double)q1s >= tOff &&
                              P.inState >= minSt;
            if (stop && P.want != 0 && P.gate) {
                P.offAt = now;
                ++P.nOffReq;
                Log("FG kdy generovat: -> jen skutecne (%s): %s - hra bez FG odhad %.1f FPS (%s; M %.2f, R %.2f, G %.2f ms, kG %.2f / "
                    "%u vzorku, c %.2f), prah vypnuti %.0f, zisk %+.0f %% (aspon %.0f %%), vystup %.1f, cil %.1f; %.1f s ve stavu",
                    PrefName(pr.mode), o3 ? "hra by cil stihla sama" : o1 ? "hra nad prahem" : "maly zisk", B,
                    cpu ? "omezeno CPU" : "omezeno GPU", M, R, G, kg, kgn, cc, pr.nOff, P.gain * 100.0, pr.g * 100.0, out, F,
                    P.inState);
            }
            P.want = stop ? 0 : 1;
            // samokontrola: generuje, ackoli je hra podle odhadu nad prahem vypnuti / stihla by cil (mimo BLOK)
            if ((o1 || o3) && !blocked && P.gate) {
                if (!P.genAgainstSince) P.genAgainstSince = now;
                P.genAgainstMs = (double)(now - P.genAgainstSince) * 1000.0 / (double)q1s;
                P.genAgainstB = B;
            } else {
                P.genAgainstSince = 0;
                P.genAgainstMs = 0.0;
            }
        }
    }
    if (!P.gate) {
        g_bpWant = false;
        P.why = P.src < 0 ? PFI_NOEST : PFI_WAIT;
        P.capFps = 0.0;
        PfPublish(0u);
        return;
    }
    if (P.want < 0) P.want = act;
    // v103f3: zpetny tlak ve stavu jen skutecne (test S1); nejdel 2 intervaly snimku hry (nejvys 100 ms)
    g_bpMaxMs = std::min(100.0, 2000.0 / std::max(20.0, P.Bm > 0.0 ? P.Bm : 60.0));
    g_bpWant = cfg.prefBp && !act;
    PfPublish(1u | (P.want ? 2u : 0u) | ((uint32_t)P.why & 0xFFu) << 8);
}

// Prepnuti helperu mimo udalosti za posledni 2 min
static unsigned PfSw120() {
    const LONGLONG now = Qpc(), q = 120 * g_qpf.QuadPart;
    unsigned n = 0;
    for (unsigned i = 0; i < 16u && i < g_pf.swN; ++i)
        if (now - g_pf.swT[(g_pf.swN - 1 - i) % 16u] <= q) ++n;
    return n;
}

// Radek "FG kdy generovat" (~5 s, z FgEstLog)
static void FgPrefLog(double el) {
    auto& P = g_pf;
    if (!cfg.inMain || !FgIsAdapt((int)g_fgMode)) return;
    char st[64] = "?";
    const bool gen = g_sh && g_sh->adaptEngaged && g_sh->fgState == BMSD_FGS_ACTIVE;
    snprintf(st, sizeof st, "%s", !g_sh || g_sh->fgState != BMSD_FGS_ACTIVE ? "FG nebezi / pozastavena" : gen ? "generuje" : "jen skutecne");
    if (cfg.prefMode == PREF_ALWAYS || !P.gate) {
        Log("FG kdy generovat %.1f s: %s - %s (Preference=%s%s%s)", el / 1000.0, st, PfWhyTxt(P.why), PrefName(cfg.prefMode),
            cfg.prefMode != PREF_ALWAYS ? "; odhad: " : "", cfg.prefMode != PREF_ALWAYS ? P.srcWhy : "");
    } else {
        Log("FG kdy generovat %.1f s: %s - %s | Preference=%s%s: generovat pod %.0f, vypnout nad %.0f FPS hry (%.0f / %.0f %% cile), zisk aspon %.0f %%, "
            "cil %.1f | hra bez FG %.1f FPS (%s; zmereno %.1f, odhad %.1f, M %.2f, R %.2f, G %.2f ms, omezeni %s, kG %.2f / %u "
            "vzorku, c %.2f ms) | s FG: skutecne %.1f, vystup %.1f, zisk %s%+.0f %% | latence navic ~%.0f ms (jen odhad modelu kap. 2.5; "
            "namereno kap. 16.3) | strop jen skutecne %.1f (k %.2f x moznosti GPU, navrat nejvys %.2f; 0 = bez stropu), presenter preskocil %llu z %llu "
            "(%.2f %%) | %.0f s ve stavu, prepnuti helperu %u (zadosti zap %u / vyp %u; mimo udalosti za 2 min %u), vypnuti podle odhadu %s | okna "
            "neplatna %u z %u%s%s",
            el / 1000.0, st, PfWhyTxt(P.why), PrefName(cfg.prefMode), P.prof != cfg.prefMode ? " (plati balanced)" : "", P.nOn,
            P.nOff, P.F > 0.0 ? 100.0 * P.nOn / P.F : 0.0, P.F > 0.0 ? 100.0 * P.nOff / P.F : 0.0, P.gainRule ? P.g * 100.0 : 0.0,
            P.F, P.B,
            gen ? "odhad" : fabs(P.B - P.Bm) < 0.05 ? "zmereno" : "odhad schopnosti - limiter drzi hru na cili, nebo RealCap", P.Bm, P.Bc, P.M, P.R, P.G,
            !P.est ? "nezname" : P.cpu ? "CPU" : "GPU", P.kg, P.kgn, P.c, P.Rh, P.O, gen ? "namereno " : "predpoved ",
            P.gainKnown ? P.gain * 100.0 : 0.0, P.dL, P.capFps, P.capK, P.capCeil, (unsigned long long)P.skIntS,
            (unsigned long long)P.skIntP, P.skIntP ? 100.0 * (double)P.skIntS / (double)P.skIntP : 0.0, P.inState, P.nSw, P.nOnReq,
            P.nOffReq, PfSw120(), Qpc() < P.offBlockUntil ? "blokovane (backoff)" : "povolene", P.nBad, P.nEval,
            P.badLast ? ", posledni: " : "", P.badLast ? P.badLast : "");
    }
    P.skIntS = P.skIntP = 0;
    P.nEval = P.nBad = P.nSw = P.nOnReq = P.nOffReq = 0;
    P.badLast = nullptr;
}

// Samokontroly (SelfCheck, ~5 s)
static void FgPrefSelfCheck() {
    auto& P = g_pf;
    if (!P.gate) return;
    if (P.genAgainstMs > 10000.0)
        ScReport(SC_PREFGEN, "generuje, i kdyz podle profilu nema | hra bez FG odhad %.1f FPS > prah vypnuti %.0f (nebo nad 1,08 x "
                 "0,95 x cil %.1f) uz %.0f s; Preference=%s, vystup %.1f FPS",
                 P.genAgainstB, P.nOff, P.F, P.genAgainstMs / 1000.0, PrefName(P.prof), P.out);
    // v103f (K3): >= 3 cykly (6 prepnuti) za 2 min mimo udalosti; backoff je ma pri kolisajici scene zastavit
    const unsigned n = PfSw120();
    if (n >= 6)
        ScReport(SC_PREFFLAP, "prepinani generuje <-> jen skutecne %u x za 2 min mimo udalosti | Preference=%s, hra bez FG %.1f FPS, "
                 "prahy %.0f / %.0f, zisk %+.0f %%, dalsi vypnuti podle odhadu nejdriv za %.0f s (backoff)",
                 n, PrefName(P.prof), P.B, P.nOn, P.nOff, P.gain * 100.0, P.backoffS);
}
