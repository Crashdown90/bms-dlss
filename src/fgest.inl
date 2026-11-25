// bms-dlss: krok 0 navrhu "FG jen tam, kde se vyplati" (docs/vyzkum-fg-kde-se-vyplati.md, kap. 2.2, 4.1, 4.2, 7).
// JEN MERENI DO LOGU: chovani modu se nemeni (zadne prepinani, prahy ani menu). Vklada ho present.cpp pred FgLimiter.
//
// Kazdy snimek hry (FgLimiter, i bez FG): interval snimku, cas snimku bez spanku limiteru FG (I_busy), GPU hotovo
// (helper: post -> prace pro snimek na GPU dokoncena), cena generatoru c a stav:
//   generuje      = adaptivni rezim prave generuje (helper ACTIVE, adaptEngaged),
//   jen skutecne  = adaptivni rezim negeneruje, FG pozastavena (menu modu s [fg] PauseInMenu=1) nebo FG vypnuta,
//   jine          = pevny nasobek, start presenteru, zaloha (nepocita se).
// Prepnuti generuje <-> jen skutecne: okno 1 s pred prepnutim a 1 s po nem, bez 0,3 s kolem prepnuti.
//   - omezeni: GPU hotovo < 0,85 x I_busy v okne "generuje" = hra omezena CPU, jinak GPU;
//   - odhad FPS bez generatoru z okna "generuje": GPU B^ = 1000 / (I_busy - k * c), CPU B^ = 1000 / I_busy; k jen
//     z drivejsich prepnuti (mimo vzorek). Pro srovnani dnesni odhad helperu (v102t) 1000 / (interval - EMA ceny);
//   - namereno z okna "jen skutecne": 1000 / prumerny interval snimku (FPS bez generatoru); spal-li tam limiter
//     (> 3 % casu), je hodnota jen dolni mez (strop cile) a do srovnani nepatri;
//   - vzorek k = (I_busy s generovanim - I_busy bez) / c, jen hra omezena GPU a platna okna; k = EMA (alfa 0,3) od
//     prvniho vzorku, orez 0 .. 1,1, zvlast pro FSR adaptivni a DLSS-G adaptivni; bez vzorku k = 1 (= dnesni odhad).
// c = dolni kvartil poslednich 64 casovych razitek GPU generatoru na skutecny snimek (helper, BMSD_PRESENT_VERSION 9).
// Radek "odhad FG" nejvys 1x za interval statistiky (~5 s, LogStats), jen s adaptivnim rezimem nebo po prepnuti.
//
// Krok 0b (kap. 14): odhad, ktery plati i se zlomkovym zamkem. Se zamkem limiter spi a hlavni vlakno hry (simulace)
// bezi soubezne se spankem, cas snimku bez spanku je pak jen prace vlakna renderu. Proto z casu vlaken
// (src/gamethreads.inl) a casu GPU snimku hry (casove znacky D3D9, latency.inl):
//   M = interval - cekani hlavniho vlakna na render (cas hlavniho vlakna na snimek, vcetne jeho spanku a jinych cekani)
//   R = CPU cas vlakna renderu na snimek
//   G = cas behu enginu GPU na skutecny snimek (hra + helper, citace jadra grafiky, src/gamethreads.inl) - kG * c64;
//       c64 = prumerna cena generatoru (64 vzorku helperu na konci okna; dolni kvartil skace u DLSS-G podle poctu volani
//       1,7 <-> 2,5 ms). Cas enginu nezavisi na tom, kdy v ramci snimku generator bezi (casove znacky snimku hry se
//       zamkem ano: 0,4 - 0,8 x cena). Bez citacu: cas GPU snimku hry z casovych znacek - kG * EMA ceny.
//       kG = (G s generovanim - G bez) / c jen z drivejsich prepnuti (mimo vzorek), EMA alfa 0,3, orez 0 .. 3, zvlast
//       podle zdroje, rezimu a toho, zda je zamek zapnuty
//   B^ = 1000 / max(M, R, G), omezeni podle toho, ktery clen je nejvetsi (GPU = G, jinak CPU).

enum { FE_OTHER = 0, FE_GEN = 1, FE_REAL = 2 };
enum { FEW_NONE = 0, FEW_HELPER = 1, FEW_MENU = 2, FEW_PAUSE = 3, FEW_FGOFF = 4 };  // proc jen skutecne
struct FeFrame {
    LONGLONG t;          // QPC snimku (volani FgLimiter po Present)
    float dt, busy;      // interval od minuleho snimku a z toho bez spanku limiteru (ms)
    float gd;            // GPU hotovo (ms, 0 = nezname / stare)
    float c, ce;         // cena generatoru: dolni kvartil a EMA helperu (ms, 0 = nezname)
    uint32_t lock;       // stupen zlomkoveho zamku (g_adLockWord)
    uint8_t st, why, fore, mode;  // FE_*, FEW_*, okno hry v popredi, rezim (BMSD_FG_*) generujici strany
    uint8_t tok, gsNew;  // cas vlaken zmeren (GtSample::ok); cas GPU v tomto snimku nove precteny
    float mc, rc, wr, ws;  // CPU hlavniho vlakna, CPU vlakna renderu, cekani hlavniho na render, jeho spanek (ms)
    float gs;            // cas GPU snimku hry z casovych znacek (ms, 0 = nezname)
    float gb;            // cas behu enginu GPU od minuleho snimku (citace jadra grafiky, ms, < 0 = nezname)
    float cm;            // prumerna cena generatoru (64 vzorku, helper; ms, 0 = nezname)
    uint8_t grd;         // v103e: 1 = Alt+Tab / ochrana po navratu (ResumeGuard), 2 = menu modu otevrene, 4 = kurzor
                         // viditelny (menu hry, konzole) - preference v takovych snimcich nerozhoduje
};
static const int kFeRing = 1024;
static FeFrame g_fe[kFeRing];
static unsigned g_feN;  // zapsanych snimku celkem
static struct {
    LONGLONG prevQ, stSince, gdAt;
    int st = -1, why;
    uint8_t mode;
    bool fore = true, curVis;
    uint32_t gdSeq;
    double gd;
    // probihajici prepnuti (ceka na okno po nem)
    bool pend;
    LONGLONG t0;
    int from, to, why0;
    unsigned idx0;  // prvni snimek v novem stavu
    // naucene k: [0] FSR adaptivni, [1] DLSS-G adaptivni
    double k[2] = {1.0, 1.0};
    unsigned kn[2];
    // krok 0b: kG [rezim * 2 + zamek zapnuty]
    double kg[8] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};  // [0..3] engine GPU, [4..7] casove znacky
    unsigned kgn[8];
    float kgs[8][5];  // v103f: poslednich 5 vzorku kG (robustni odhad, FeKgUpdate)
    uint32_t gsSeq;
    LONGLONG gsAt;
    float gsv;
    int cls = -1;  // posledni omezeni z okna "generuje": 0 GPU, 1 CPU
    // interval radku: soucty podle stavu
    double sDt[3], sBusy[3], sGd;
    double sTDt, sTWr, sTRc, sTGs, sTGb;  // krok 0b, jen stav "generuje": interval, cekani na render, CPU renderu, GPU, engine
    unsigned nT, nTGs, nTGb;
    unsigned n[3], nGd;
    uint64_t pres0;
    bool any;
    // hotova prepnuti pro dalsi radek
    char res[3][1024];
    int nres;
    unsigned more, total, valid;
} g_feS;

struct FeWin {
    unsigned n = 0, nGd = 0, nT = 0, nGs = 0;
    double sDt = 0, sDt2 = 0, sBusy = 0, sGd = 0, maxDt = 0, c = 0, ce = 0, cm = 0;
    double sTDt = 0, sMc = 0, sRc = 0, sWr = 0, sWs = 0, sGs = 0, sGb = 0;
    unsigned nGb = 0;
    LONGLONG first = 0, last = 0;
    bool lockSame = true, fore = true, stOk = true, menu = false;
    uint32_t lock = 0;
    uint8_t mode = 0;
    double dt() const { return n ? sDt / n : 0.0; }
    double busy() const { return n ? sBusy / n : 0.0; }
    double cv() const {
        if (n < 2) return 0.0;
        const double m = sDt / n, v = sDt2 / n - m * m;
        return m > 0.0 && v > 0.0 ? sqrt(v) / m : 0.0;
    }
    double gdm() const { return nGd ? sGd / nGd : 0.0; }
    bool thr() const { return n && nT * 10 >= n * 8; }  // cas vlaken aspon v 80 % snimku
    double M() const { return nT ? (sTDt - sWr) / nT : 0.0; }
    double mc() const { return nT ? sMc / nT : 0.0; }
    double rc() const { return nT ? sRc / nT : 0.0; }
    double wr() const { return nT ? sWr / nT : 0.0; }
    double ws() const { return nT ? sWs / nT : 0.0; }
    double gs() const { return nGs * 2 >= n ? sGs / nGs : 0.0; }
    double gb() const { return nGb * 10 >= n * 8 ? sGb / nGb : 0.0; }
};

static void FeAdd(FeWin& w, const FeFrame& f, int st) {
    if (f.st != st) {
        w.stOk = false;
        return;
    }
    if (!w.n) w.lock = f.lock;
    if (f.lock != w.lock) w.lockSame = false;
    if (!f.fore || (f.grd & 1u)) w.fore = false;  // v103e: ani v ochranne dobe po Alt+Tabu / pauze (kap. 5.4)
    if (f.grd & 6u) w.menu = true;                 // v103f: menu modu / kurzor viditelny (menu hry: cena generatoru jina)
    ++w.n;
    w.sDt += f.dt;
    w.sDt2 += (double)f.dt * f.dt;
    w.sBusy += f.busy;
    if (f.dt > w.maxDt) w.maxDt = f.dt;
    if (f.gd > 0.0f) {
        w.sGd += f.gd;
        ++w.nGd;
    }
    if (f.tok) {
        ++w.nT;
        w.sTDt += f.dt;
        w.sMc += f.mc;
        w.sRc += f.rc;
        w.sWr += f.wr;
        w.sWs += f.ws;
    }
    if (f.gs > 0.0f) {
        w.sGs += f.gs;
        ++w.nGs;
    }
    if (f.gb >= 0.0f) {
        w.sGb += f.gb;
        ++w.nGb;
    }
    if (f.c > 0.0f) w.c = f.c;  // posledni znama
    if (f.ce > 0.0f) w.ce = f.ce;
    if (f.cm > 0.0f && (!w.cm || f.t >= w.last)) w.cm = f.cm;  // posledni (nejnovejsi) snimek okna: prumer pres okno
    if (f.mode) w.mode = f.mode;
    if (!w.first || f.t < w.first) w.first = f.t;  // nejstarsi a nejnovejsi snimek okna
    if (f.t > w.last) w.last = f.t;
}

// Okno [a, b) pred prepnutim (snimky pred idx0, pozpatku) nebo po nem (od idx0, dopredu; konci prvnim snimkem
// v jinem stavu)
static FeWin FeWindow(bool before, LONGLONG a, LONGLONG b, int st) {
    FeWin w;
    if (before) {
        for (unsigned i = g_feS.idx0; i-- > 0 && g_feN - i < (unsigned)kFeRing - 2;) {
            const FeFrame& f = g_fe[i % kFeRing];
            if (f.t < a) break;
            if (f.t < b) FeAdd(w, f, st);
            else if (f.st != st) w.stOk = false;  // stav mezi oknem a prepnutim
        }
    } else {
        for (unsigned i = g_feS.idx0; i < g_feN; ++i) {
            const FeFrame& f = g_fe[i % kFeRing];
            if (f.st != st || f.t >= b) break;
            if (f.t >= a) FeAdd(w, f, st);
        }
    }
    return w;
}

// Odhad FPS bez generatoru z okna "generuje" (ms snimku bez spanku, cena, k)
static double FeEstGpu(double busy, double k, double c) { return busy - k * c > 1.0 ? 1000.0 / (busy - k * c) : 0.0; }

// [debug] FgEstCsv=1 (vychozi 0): snimky kolem kazdeho prepnuti do bms_dlss_fgest.csv (rozbor mimo hru)
static FILE* g_feCsv;
static void FeCsv(unsigned sw, bool off, LONGLONG t0) {
    if (!g_feCsvInit) return;
    if (!g_feCsv) {
        const std::wstring p = std::wstring(ProxyDir()) + L"\\bms_dlss_fgest.csv";
        g_feCsv = _wfopen(p.c_str(), L"w");
        if (!g_feCsv) {
            g_feCsvInit = false;
            return;
        }
        fprintf(g_feCsv, "sw,smer,t_ms,st,dt,busy,gd,gs,gsNew,mc,rc,wr,ws,tok,lock,c,ce,mode,fore,gb,cm\n");
    }
    const LONGLONG q1s = g_qpf.QuadPart, a = t0 - q1s * 16 / 10, b = t0 + q1s * 16 / 10;
    const unsigned from = g_feN > (unsigned)kFeRing - 2 ? g_feN - (kFeRing - 2) : 0;
    for (unsigned i = from; i < g_feN; ++i) {
        const FeFrame& f = g_fe[i % kFeRing];
        if (f.t < a || f.t > b) continue;
        fprintf(g_feCsv, "%u,%s,%.3f,%u,%.3f,%.3f,%.2f,%.3f,%u,%.3f,%.3f,%.3f,%.3f,%u,%u/%u,%.3f,%.3f,%u,%u,%.3f,%.3f\n", sw,
                off ? "vyp" : "zap", (double)(f.t - t0) * 1000.0 / (double)q1s, f.st, f.dt, f.busy, f.gd, f.gs, f.gsNew,
                f.mc, f.rc, f.wr, f.ws, f.tok, f.lock >> 8 & 0xFFu, f.lock & 0xFFu, f.c, f.ce, f.mode, f.fore, f.gb, f.cm);
    }
    fflush(g_feCsv);
}

// v103f (S4): kG robustne - vzorek omezeny na 0,3-1,5 (namereno 0,66-1,27, kap. 14.3 / 15.3), 1 vzorek = jen ten,
// 2 vzorky = mensi z nich (bezpecny smer: B^ nizsi = generuje dele), od 3 (dolni) median poslednich 5. Jeden odlehly vzorek tak
// odhad neposune mimo ostatni. Preference pouzije naucene kG pri generovani az od 2 vzorku (fgpref.inl).
static void FeKgUpdate(int gi, double v) {
    auto& S = g_feS;
    const float vc = (float)std::min(1.5, std::max(0.3, v));
    S.kgs[gi][S.kgn[gi] % 5u] = vc;
    ++S.kgn[gi];
    const unsigned n = std::min(5u, S.kgn[gi]);
    float a[5];
    for (unsigned i = 0; i < n; ++i) a[i] = S.kgs[gi][i];
    std::sort(a, a + n);
    S.kg[gi] = a[(n - 1) / 2];  // 1 -> ten, 2 -> mensi, 3-5 -> (dolni) median
}

static void FeFinish(LONGLONG now) {
    auto& S = g_feS;
    S.pend = false;
    const LONGLONG q1s = g_qpf.QuadPart, q03 = q1s * 3 / 10;
    const bool off = S.from == FE_GEN;  // vypnuti generovani (jinak zapnuti)
    const FeWin wb = FeWindow(true, S.t0 - q1s - q03, S.t0 - q03, S.from);
    const FeWin wa = FeWindow(false, S.t0 + q03, S.t0 + q1s + q03, S.to);
    const FeWin& g = off ? wb : wa;
    const FeWin& r = off ? wa : wb;
    const int mi = g.mode == BMSD_FG_DLSSGADAPT ? 1 : 0;
    const double qms = (double)q1s / 1000.0;
    const double durB = wb.n ? (double)(S.t0 - q03 - wb.first) / qms : 0.0;
    const double durA = wa.n ? (double)(wa.last - (S.t0 + q03)) / qms : 0.0;
    // platnost: okna pokryta, stav beze zmeny, bez pauzy, okno hry v popredi, stupen zamku beze zmeny
    const char* bad = nullptr;
    if (wb.n < 15 || durB < 950.0 || !wb.stOk) bad = "kratky stav pred prepnutim";
    else if (wa.n < 15 || durA < 500.0) bad = "kratky stav po prepnuti";
    else if (wb.maxDt > 100.0 || wa.maxDt > 100.0) bad = "pauza snimku > 100 ms";
    else if (!wb.fore || !wa.fore) bad = "okno hry neni v popredi";
    else if (wb.menu || wa.menu) bad = "menu modu / menu hry (kurzor)";
    else if (!g.lockSame) bad = "zmena stupne zamku";
    else if (g.cv() > 0.2 || r.cv() > 0.2) bad = "nepravidelne snimky (sd > 20 %)";
    else if (g.c <= 0.0) bad = "cena generatoru neznama";
    const bool cpu = g.nGd >= 5 && g.gdm() < 0.85 * g.busy();
    const double kp = S.k[mi];
    const unsigned kn = S.kn[mi];
    const double est = cpu ? (g.busy() > 1.0 ? 1000.0 / g.busy() : 0.0) : FeEstGpu(g.busy(), kp, g.c);
    const double estDnes = g.ce > 0.0 && g.dt() > g.ce + 1.0 ? 1000.0 / (g.dt() - g.ce) : 0.0;
    const double meas = r.dt() > 0.0 ? 1000.0 / r.dt() : 0.0;
    const double slept = r.dt() > 0.0 ? 1.0 - r.busy() / r.dt() : 0.0;
    const bool capped = slept > 0.03;
    const double err = meas > 0.0 && est > 0.0 ? (est / meas - 1.0) * 100.0 : 0.0;
    const double errD = meas > 0.0 && estDnes > 0.0 ? (estDnes / meas - 1.0) * 100.0 : 0.0;
    // vzorek k (az po odhadu: predpoved pouziva jen drivejsi prepnuti)
    char ks[96] = "";
    if (!bad && !cpu && g.c > 0.0) {
        const double v = (g.busy() - r.busy()) / g.c, vc = std::min(1.1, std::max(0.0, v));
        S.k[mi] = S.kn[mi] ? S.k[mi] + 0.3 * (vc - S.k[mi]) : vc;
        ++S.kn[mi];
        snprintf(ks, sizeof ks, "vzorek k %.2f -> k %.2f (%u)", v, S.k[mi], S.kn[mi]);
    } else {
        snprintf(ks, sizeof ks, "bez vzorku k (%s)", bad ? bad : cpu ? "omezeno CPU" : "cena neznama");
    }
    // krok 0b: B^ = 1000 / max(M, R, G - kG * c), kG jen z drivejsich prepnuti
    // zdroj G: cas behu enginu GPU (hra + helper, citace jadra grafiky) a prumerna cena c64; bez citacu casove znacky
    // snimku hry a EMA ceny. k zvlast podle zdroje, rezimu a zamku.
    const bool eng = g.gb() > 0.0 && r.gb() > 0.0 && g.cm > 0.0;
    const double gGs = eng ? g.gb() : g.gs(), rGs = eng ? r.gb() : r.gs(), cc = eng ? g.cm : g.ce;
    const int gi = (eng ? 0 : 4) + mi * 2 + (g.lock ? 1 : 0);
    const double kgp = S.kg[gi];
    const unsigned kgnp = S.kgn[gi];
    const bool thrOk = g.thr() && r.thr() && gGs > 0.0 && rGs > 0.0 && cc > 0.0;
    const double gM = g.M(), gR = g.rc(), gG = gGs - kgp * cc;
    const bool gpu0b = gG >= gM && gG >= gR;
    const double gMax = std::max(gG, std::max(gM, gR));
    const double est0b = thrOk && gMax > 1.0 ? 1000.0 / gMax : 0.0;
    const double err0b = meas > 0.0 && est0b > 0.0 ? (est0b / meas - 1.0) * 100.0 : 0.0;
    char kgs[96] = "";
    // vzorek kG: plati, jen kdyz je hra v obou oknech omezena GPU (G bez generovani je nejvetsi clen)
    const double rG = rGs;
    // v103e: se stropem limiteru (stav jen skutecne s preferenci drzi hru na 92 % moznosti GPU) plati vzorek z enginu GPU
    // dal - cas enginu na snimek na strope nezavisi
    if (!bad && (!capped || eng) && thrOk && rG >= r.M() && rG >= r.rc()) {
        const double v = (gGs - rG) / cc;
        FeKgUpdate(gi, v);
        snprintf(kgs, sizeof kgs, "vzorek kG %.2f -> kG %.2f (%u)", v, S.kg[gi], S.kgn[gi]);
    } else {
        snprintf(kgs, sizeof kgs, "bez vzorku kG (%s)", bad ? bad : capped ? "strop limiteru" : !thrOk ? "cas vlaken / GPU neznamy" :
                                                        "bez generovani omezeno CPU");
    }
    char t0b[480];
    snprintf(t0b, sizeof t0b,
             "0b: generuje M %.2f (CPU hl. %.2f, ceka na render %.2f, spanek %.2f), R %.2f, GPU %.2f ms, engine GPU %.2f ms | jen "
             "skutecne M %.2f (CPU hl. %.2f, ceka %.2f, spanek %.2f), R %.2f, GPU %.2f ms, engine GPU %.2f ms | 0b odhad %.1f FPS (%s, kG %.2f, %u vzorku, cE %.2f, %s), "
             "chyba %+.1f %%, %s",
             gM, g.mc(), g.wr(), g.ws(), gR, g.gs(), g.gb(), r.M(), r.mc(), r.wr(), r.ws(), r.rc(), r.gs(), r.gb(), est0b,
             !thrOk ? "nezname" : gpu0b ? "GPU" : gM >= gR ? "CPU hlavni" : "CPU render", gpu0b ? kgp : 0.0,
             gpu0b ? kgnp : 0u, cc, eng ? "engine - kG x c64" : "znacky - kG x cEMA", err0b, kgs);
    if (!bad) S.cls = cpu ? 1 : 0;
    ++S.total;
    if (!bad && !capped) ++S.valid;
    FeCsv(S.total, off, S.t0);
    static const char* whyTxt[] = {"", "helper", "menu modu", "pozastaveno", "FG vyp"};
    char line[1024];
    snprintf(line, sizeof line,
             "[%s (%s) %s %s | generuje: snimek %.2f ms, bez spanku %.2f, GPU hotovo %.2f, c %.2f ms, zamek %s%u/%u, %u sn. | "
             "jen skutecne: %.2f ms, bez spanku %.2f, %u sn. | bez FG odhad %.1f FPS (k %.2f, %u vzorku), namereno %.1f FPS%s, "
             "chyba %+.1f %% | dnes %.1f FPS (%+.1f %%) | %s | %s%s%s]",
             off ? "vyp" : "zap", whyTxt[S.why0 >= 0 && S.why0 <= 4 ? S.why0 : 0],
             FgModeName(g.mode ? (int)g.mode : (int)g_fgMode), cpu ? "CPU" : "GPU", g.dt(), g.busy(), g.gdm(), g.c,
             g.lock ? "" : "vyp ", g.lock >> 8 & 0xFFu, g.lock & 0xFFu, g.n, r.dt(), r.busy(), r.n, est, cpu ? 0.0 : kp,
             cpu ? 0u : kn, meas, capped ? " (spal limiter - jen dolni mez)" : "", err, estDnes, errD, t0b, ks,
             bad ? " | NEPLATNE: " : "", bad ? bad : "");
    if (S.nres < 3) {
        memcpy(S.res[S.nres], line, sizeof line);
        ++S.nres;
    } else {
        ++S.more;
    }
    (void)now;
}

static void FgPrefFrame(LONGLONG now);  // src/fgpref.inl (preference, v103e)
static void FgPrefLog(double el);

static void FgEstFrame(LONGLONG now, LONGLONG sleptQ) {
    auto& S = g_feS;
    const LONGLONG q1s = g_qpf.QuadPart;
    int st = FE_OTHER, why = FEW_NONE;
    uint8_t mode = 0;
    if (g_state == PS_OFF || g_state == PS_IDLE) {
        st = FE_REAL;
        why = FEW_FGOFF;
    } else if (g_state == PS_HELPER && g_sh) {
        MemoryBarrier();
        const int32_t fs = g_sh->fgState;
        const uint32_t am = g_sh->fgActiveMode;
        if (fs == BMSD_FGS_ACTIVE && FgIsAdapt((int)am)) {
            st = g_sh->adaptEngaged ? FE_GEN : FE_REAL;
            why = g_sh->adaptEngaged ? FEW_NONE : FEW_HELPER;
            mode = (uint8_t)am;
        } else if (fs == BMSD_FGS_PAUSED) {
            st = FE_REAL;
            why = g_fgPause ? FEW_MENU : FEW_PAUSE;
        }
    }
    double dt = 0.0, busy = 0.0;
    if (S.prevQ) {
        dt = Ms(S.prevQ, now);
        busy = std::max(0.0, dt - (double)sleptQ * 1000.0 / (double)q1s);
    }
    S.prevQ = now;
    const uint32_t gw = Interop_FgGpuDone();
    if ((gw >> 16) && (gw >> 16) != S.gdSeq) {
        S.gdSeq = gw >> 16;
        S.gd = (double)(gw & 0xFFFFu) / 100.0;
        S.gdAt = now;
    }
    const bool gdOk = S.gdAt && now - S.gdAt < q1s / 4;
    GtSample ts;
    GtFrame(now, &ts);
    uint32_t gsq = 0;
    const uint32_t f0Seq = S.gsSeq;
    const float gsv = ProxyLatGpuSpanMs(&gsq);
    if (gsq != S.gsSeq) {
        S.gsSeq = gsq;
        S.gsv = gsv;
        S.gsAt = now;
    }
    const bool gsOk = S.gsAt && now - S.gsAt < q1s / 4;
    if ((g_feN & 15u) == 0) {
        S.fore = ScForeground();
        CURSORINFO ci = {sizeof ci};
        S.curVis = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);  // hra kurzor skryva, menu hry a konzole ne
    }
    float c = 0.0f, ce = 0.0f, cm = 0.0f;
    if (g_sh && g_sh->fgCostCount && FgIsAdapt((int)g_sh->fgCostMode)) {
        c = g_sh->fgCostQ1Ms;
        ce = g_sh->fgCostEmaMs;
        cm = g_sh->fgCostMeanMs;
    }
    FeFrame& f = g_fe[g_feN % kFeRing];
    f.t = now;
    f.dt = (float)dt;
    f.busy = (float)busy;
    f.gd = gdOk ? (float)S.gd : 0.0f;
    f.c = c;
    f.ce = ce;
    f.cm = cm;
    f.lock = g_adLockWord;
    f.st = (uint8_t)st;
    f.why = (uint8_t)why;
    f.fore = S.fore ? 1 : 0;
    f.mode = mode;
    f.tok = ts.ok && dt > 0.0 ? 1 : 0;
    f.mc = ts.mainCpu;
    f.rc = ts.rendCpu;
    f.wr = ts.wRend;
    f.ws = ts.sleep;
    f.gs = gsOk ? S.gsv : 0.0f;
    f.gsNew = gsq != f0Seq ? 1 : 0;
    f.gb = ts.gpuBusy;
    f.grd = (uint8_t)((g_rgBg || g_rgProt ? 1u : 0u) | (g_menuOpenNow ? 2u : 0u) | (S.curVis ? 4u : 0u));
    ++g_feN;
    if (dt > 0.0 && dt < 100.0) {
        GtAddInterval(dt, ts);
        S.sDt[st] += dt;
        S.sBusy[st] += busy;
        ++S.n[st];
        if (st == FE_GEN && gdOk) {
            S.sGd += S.gd;
            ++S.nGd;
        }
        if (st == FE_GEN && f.tok) {
            S.sTDt += dt;
            S.sTWr += f.wr;
            S.sTRc += f.rc;
            ++S.nT;
        }
        if (st == FE_GEN && f.gs > 0.0f) {
            S.sTGs += f.gs;
            ++S.nTGs;
        }
        if (st == FE_GEN && f.gb >= 0.0f) {
            S.sTGb += f.gb;
            ++S.nTGb;
        }
    }
    if (st == FE_GEN) S.mode = mode;
    if (st != S.st) {
        if (S.pend) FeFinish(now);  // stav se zmenil drive, nez bylo okno po prepnuti cele
        if ((S.st == FE_GEN && st == FE_REAL) || (S.st == FE_REAL && st == FE_GEN)) {
            S.pend = true;
            S.t0 = now;
            S.from = S.st;
            S.to = st;
            S.why0 = st == FE_REAL ? why : S.why;
            S.idx0 = g_feN - 1;
            S.any = true;
        }
        S.st = st;
        S.stSince = now;
    }
    S.why = why;
    if (S.pend && now - S.t0 >= q1s * 13 / 10) FeFinish(now);
    FgPrefFrame(now);
}

// Radek "odhad FG" (LogStats, interval ~5 s; el v ms)
static void FgEstLog(double el) {
    auto& S = g_feS;
    GtLog(el);
    const bool adapt = FgIsAdapt((int)g_fgMode);
    const uint64_t pres = g_sh ? g_sh->presCountReal + g_sh->presCountGen : 0;
    const double outFps = S.pres0 && pres >= S.pres0 && el > 0.0 ? (double)(pres - S.pres0) * 1000.0 / el : 0.0;
    S.pres0 = pres;
    if (adapt || S.any || S.nres) {
        const int st = S.st;
        const int mi = (S.mode ? S.mode : (uint8_t)g_fgMode) == BMSD_FG_DLSSGADAPT ? 1 : 0;
        const double F = g_sh && g_sh->adaptTargetMilli >= 20000u ? g_sh->adaptTargetMilli / 1000.0 : 0.0;
        const double upper = 0.95 * F;
        double lower = g_sh && g_sh->adaptFillMilli ? g_sh->adaptFillMilli / 1000.0 : 0.9 * F;
        if (lower > upper) lower = upper;
        const float c = g_sh ? g_sh->fgCostQ1Ms : 0.0f;
        const unsigned cn = g_sh ? g_sh->fgCostCount : 0u;
        char why[200] = "";
        const uint32_t ar = g_sh ? g_sh->adaptReason : 0u;
        const char* blk = ar & 0x100u ? ", dalsi zapnuti blokovane (backoff)" : "";
        switch (st == FE_GEN ? FEW_NONE : S.why) {
        case FEW_MENU: snprintf(why, sizeof why, "menu modu ([fg] PauseInMenu)"); break;
        case FEW_FGOFF: snprintf(why, sizeof why, "FG vypnuta"); break;
        case FEW_PAUSE: snprintf(why, sizeof why, "helper pozastavil FG"); break;
        default:
            switch (ar & 0xFFu) {
            case BMSD_ADR_BELOW: snprintf(why, sizeof why, "skutecne FPS pod dolni hranici %.1f (cil %.1f)", lower, F); break;
            case BMSD_ADR_ABOVE: snprintf(why, sizeof why, "skutecne FPS nad horni hranici %.1f%s", upper, blk); break;
            case BMSD_ADR_COST: snprintf(why, sizeof why, "hra by cil stihla bez generovani (odhad helperu nad %.1f)%s", 1.08 * upper, blk); break;
            case BMSD_ADR_USELESS: snprintf(why, sizeof why, "generovani snimky nepridavalo%s", blk); break;
            case BMSD_ADR_PREF_ON: snprintf(why, sizeof why, "podle preference (radek \"kdy generovat\")"); break;
            case BMSD_ADR_PREF_OFF: snprintf(why, sizeof why, "podle preference (radek \"kdy generovat\")%s", blk); break;
            default: snprintf(why, sizeof why, "zatim nezapnuto (skutecne FPS nad dolni hranici %.1f)%s", lower, blk); break;
            }
        }
        char body[720] = "";
        if (st == FE_GEN && S.n[FE_GEN]) {
            const double busy = S.sBusy[FE_GEN] / S.n[FE_GEN], dt = S.sDt[FE_GEN] / S.n[FE_GEN];
            const double gd = S.nGd ? S.sGd / S.nGd : 0.0;
            const bool cpu = S.nGd >= 5 && gd < 0.85 * busy;
            const double bG = FeEstGpu(busy, S.k[mi], c), bC = busy > 1.0 ? 1000.0 / busy : 0.0;
            const double b = cpu ? bC : bG;
            const double ce = g_sh ? g_sh->fgCostEmaMs : 0.0;
            const double dnes = ce > 0.0 && dt > ce + 1.0 ? 1000.0 / (dt - ce) : 0.0;
            const double base = F > 0.0 ? std::min(b, F) : b;
            // krok 0b: B^ = 1000 / max(M, R, G - kG * cE) za interval radku
            char b0[200] = "0b: cas vlaken nebo GPU neznamy";
            const double ceN = g_sh ? g_sh->fgCostEmaMs : 0.0;
            const double cmN = g_sh ? g_sh->fgCostMeanMs : 0.0;
            const bool engN = S.nTGb * 10 >= S.n[FE_GEN] * 8 && S.nTGb && cmN > 0.0;
            if (S.nT * 10 >= S.n[FE_GEN] * 8 && S.nT && (engN || (S.nTGs * 2 >= S.n[FE_GEN] && ceN > 0.0))) {
                const double M = (S.sTDt - S.sTWr) / S.nT, R = S.sTRc / S.nT;
                const double kgv = S.kg[(engN ? 0 : 4) + mi * 2 + (g_adLockWord ? 1 : 0)];
                const double G = (engN ? S.sTGb / S.nTGb - kgv * cmN : S.sTGs / S.nTGs - kgv * ceN), mx = std::max(G, std::max(M, R));
                const double b0v = mx > 1.0 ? 1000.0 / mx : 0.0, base0 = F > 0.0 ? std::min(b0v, F) : b0v;
                snprintf(b0, sizeof b0,
                         "0b: M %.2f, R %.2f, G %.2f ms (%s, kG %.2f, c %.2f) -> bez FG %.1f FPS, omezeni %s, zisk %+.0f %%", M, R, G,
                         engN ? "engine" : "znacky", kgv, engN ? cmN : ceN, b0v, G >= M && G >= R ? "GPU" : M >= R ? "CPU hlavni" : "CPU render",
                         base0 > 0.0 && outFps > 0.0 ? (outFps / base0 - 1.0) * 100.0 : 0.0);
            }
            snprintf(body, sizeof body,
                     "omezeni %s (GPU hotovo %.2f ms, snimek bez spanku %.2f ms, interval %.2f ms) | bez FG odhad: hra omezena "
                     "GPU %.1f FPS, omezena CPU %.1f FPS -> %.1f (dnesni odhad helperu %.1f) | zisk: vystup %.1f / bez FG %.1f = %+.0f %% | %s",
                     cpu ? "CPU" : "GPU", gd, busy, dt, bG, bC, b, dnes, outFps, base,
                     base > 0.0 && outFps > 0.0 ? (outFps / base - 1.0) * 100.0 : 0.0, b0);
        } else if (st == FE_REAL && S.n[FE_REAL]) {
            const double dt = S.sDt[FE_REAL] / S.n[FE_REAL], busy = S.sBusy[FE_REAL] / S.n[FE_REAL];
            const double B = 1000.0 / dt;
            // predpoved pri zapnuti: hra omezena GPU skutecne 1000 / (1000 / B + k * c), CPU skutecne = B; vystup cil,
            // pod AdaptMinFps nejvys AdaptLowMult x
            const bool cpu = S.cls == 1;
            const double R = cpu || c <= 0.0f ? B : 1000.0 / (dt + S.k[mi] * c);
            const double minEff = F > 0.0 && cfg.adaptMinFps > 0 ? std::min((double)cfg.adaptMinFps, F / 2.0) : 0.0;
            const double O = F > 0.0 ? std::min(F, R < minEff ? R * cfg.adaptLowMult : 1e9) : 0.0;
            const double base = F > 0.0 ? std::min(B, F) : B;
            snprintf(body, sizeof body,
                     "hra %.1f FPS (interval %.2f ms, bez spanku %.2f ms, limiter spal %.0f %%) | s FG predpoved: skutecne "
                     "%.1f FPS (omezeni %s), vystup %.1f | zisk: predpoved %+.0f %%",
                     B, dt, busy, dt > 0.0 ? (1.0 - busy / dt) * 100.0 : 0.0, R,
                     S.cls < 0 ? "nezname, bere se GPU" : cpu ? "CPU" : "GPU", O,
                     base > 0.0 && O > 0.0 ? (O / base - 1.0) * 100.0 : 0.0);
        } else {
            snprintf(body, sizeof body, "bez mereni (pevny nasobek, start nebo zaloha)");
        }
        char tr[3300] = "";
        size_t pos = 0;
        for (int i = 0; i < S.nres && pos < sizeof tr - 2; ++i)
            pos += (size_t)snprintf(tr + pos, sizeof tr - pos, " %s", S.res[i]);
        if (S.more && pos < sizeof tr - 2) snprintf(tr + pos, sizeof tr - pos, " (+%u dalsich)", S.more);
        Log("odhad FG %.1f s: %s (%s; %s) | %s | cena c %.2f ms (dolni kvartil 64 vzorku, prumer %.2f, EMA %.2f; vzorku "
            "%u) | k FSR %.2f (%u), DLSS-G %.2f (%u) | prepnuti %u (platnych %u)%s",
            el / 1000.0, st == FE_GEN ? "generuje" : st == FE_REAL ? "jen skutecne" : "jine",
            FgModeName(st == FE_GEN && S.mode ? (int)S.mode : (int)g_fgMode), why, body, c, g_sh ? g_sh->fgCostMeanMs : 0.0f,
            g_sh ? g_sh->fgCostEmaMs : 0.0f, cn, S.k[0], S.kn[0], S.k[1], S.kn[1], S.total, S.valid, tr);
    }
    FgPrefLog(el);
    S.any = false;
    S.nres = 0;
    S.more = 0;
    for (int i = 0; i < 3; ++i) S.sDt[i] = S.sBusy[i] = 0.0, S.n[i] = 0;
    S.sTDt = S.sTWr = S.sTRc = S.sTGs = S.sTGb = 0.0;
    S.nT = S.nTGs = S.nTGb = 0;
    S.sGd = 0.0;
    S.nGd = 0;
}
