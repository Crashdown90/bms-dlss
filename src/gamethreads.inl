// bms-dlss: cas vlaken hry - jen mereni, chovani hry se nemeni ([debug] GameThreads=1, vychozi; 0 = nic z tohoto).
// Vklada ho present.cpp pred fgest.inl. Pouziva ho odhad FPS bez generatoru (src/fgest.inl, krok 0b navrhu
// docs/vyzkum-fg-kde-se-vyplati.md, kap. 14); casy jsou QPC jako ve zbytku proxy, takze je muze pouzit i mereni
// latence "vzorek vstupu -> Present" (docs/vyzkum-impulse-vylepseni.md, krok A).
//
// Hlavni vlakno = vlakno okna hry (pumpa zprav a simulace Source). Vlakno renderu = vlakno, ktere vola Present
// hlavniho zarizeni (Source s mat_queue_mode 2 kresli ve vlastnim vlakne).
//  - CPU cas obou vlaken za snimek: QueryThreadCycleTime (tiky TSC, prevod na ms podle __rdtsc proti QPC).
//  - Cekani hlavniho vlakna: hacky v tabulkach importu (IAT) modulu hry (bms.exe a DLL pod jeho adresarem, bez
//    bin\thirdparty) na WaitForSingleObject(Ex), WaitForMultipleObjects(Ex), MsgWaitForMultipleObjects(Ex),
//    Sleep(Ex), SleepConditionVariableCS / SRW a SwitchToThread. Na ostatnich vlaknech hacky jen predaji volani.
//    "Cekani na render" = cekani, ktere zacalo pred uvolnenim snimku vlaknem renderu (konec Present hry vcetne spanku
//    limiteru a regulatoru latence, Present_FrameReleased) a skoncilo nejvys 2 ms po nem. Spanek (Sleep > 0 ms, napr.
//    host_sleep) a ostatni cekani se pocitaji zvlast jako cas hlavniho vlakna.
//    M (cas hlavniho vlakna na snimek) = interval snimku - cekani na render.
//  - Vsechna vlakna procesu (CPU % jadra, jmeno vlakna) a nove nactene moduly k hakovani: vlastni vlakno jednou za
//    ~5 s, na vlakne renderu se nic z toho nedela.
//  - Vytizeni enginu GPU (citace jadra grafiky D3DKMTQueryStatistics, RunningTime uzlu adapteru okna hry, proces hry
//    + proces helperu; globalni citac uzlu 3D ovladac NVIDIA nevraci): vlakno mereni vypise vsechny uzly a vybere
//    nejvytizenejsi (3D), vlakno renderu ho cte jednou za snimek (cas behu enginu od minuleho snimku = prace GPU na snimek bez necinnosti, nezavisle na tom, kdy
//    v ramci snimku generator bezi).
// Hacky se nikdy neodstranuji; modul proxy je proto pripnuty (GET_MODULE_HANDLE_EX_FLAG_PIN).

enum { GTA_WREND = 0, GTA_WOTHER, GTA_SLEEP, GTA_YIELD, GTA_NWAIT, GTA_NWREND, GTA_H0, GTA_N = GTA_H0 + 7 };
static volatile LONGLONG g_gtAcc[GTA_N];  // jen hlavni vlakno pricita (Interlocked), vlakno renderu cte
static volatile LONGLONG g_gtRelQ;        // posledni uvolneni snimku vlaknem renderu (QPC)
static LONGLONG g_gtRwA, g_gtRwB;         // posledni cekani hlavniho vlakna na render: zacatek, konec (jen hlavni vlakno)
static volatile LONG g_gtMainTid, g_gtRendTid, g_gtWorkTid;
static volatile LONG g_gtHookedMods, g_gtHookedFns;
static volatile float g_gtTscPerMsF;  // pro vlakno mereni
static SRWLOCK g_gtTxtLock = SRWLOCK_INIT;
static char g_gtTxt[700];  // posledni prehled vlaken (vlakno mereni)
static char g_gtGpuTxt[300];  // posledni prehled uzlu GPU (vlakno mereni, pod g_gtTxtLock)
static volatile LONG g_gtNode = -1;  // nejvytizenejsi uzel GPU (-1 = nezname)
static volatile LONG g_gtLuidLo, g_gtLuidHi, g_gtNodes;  // adapter okna hry (vlakno renderu nastavi, mereni cte)
typedef NTSTATUS(APIENTRY* PFN_GtKmtOpen)(D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME*);
typedef NTSTATUS(APIENTRY* PFN_GtKmtClose)(const D3DKMT_CLOSEADAPTER*);
typedef NTSTATUS(APIENTRY* PFN_GtKmtQuery)(const D3DKMT_QUERYSTATISTICS*);
static PFN_GtKmtQuery g_gtKmtQuery;
static HANDLE g_gtHelperProc;  // proces helperu (jen pro prehled uzlu)

// Cas behu uzlu GPU (ms od startu; < 0 = chyba). q = pracovni struktura volajiciho vlakna (~800 B). proc = null:
// vsechny procesy (D3DKMT_QUERYSTATISTICS_NODE), jinak jen dany proces (PROCESS_NODE).
static LONG g_gtKmtLast;  // v103g: posledni NTSTATUS dotazu (diagnostika citace helperu -1)
static double GtNodeMs(D3DKMT_QUERYSTATISTICS& q, LONG node, HANDLE proc = nullptr) {
    memset(&q, 0, sizeof q);
    q.Type = proc ? D3DKMT_QUERYSTATISTICS_PROCESS_NODE : D3DKMT_QUERYSTATISTICS_NODE;
    q.AdapterLuid.LowPart = (DWORD)g_gtLuidLo;
    q.AdapterLuid.HighPart = g_gtLuidHi;
    q.hProcess = proc;
    if (proc) q.QueryProcessNode.NodeId = (ULONG)node;
    else q.QueryNode.NodeId = (ULONG)node;
    if (!g_gtKmtQuery) return -1.0;
    const NTSTATUS st = g_gtKmtQuery(&q);
    g_gtKmtLast = (LONG)st;
    if (st < 0) return -1.0;
    const LONGLONG t = proc ? q.QueryResult.ProcessNodeInformation.RunningTime.QuadPart
                            : q.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart;
    return (double)t / 10000.0;  // 100 ns
}

// Adapter monitoru okna hry (vlakno renderu, jednou)
// v103e: pri startu hry (okno jeste na jinem monitoru / bez adapteru) se otevreni obcas nepovede - zkusit znovu po ~2 s
// (driv jen jednou: v behu s0_bal_os3 pak citace GPU chybely cely beh a preference nerozhodovala)
static void GtGpuOpen() {
    static bool s_done, s_logged;
    static unsigned s_fails;
    static ULONGLONG s_next;
    if (s_done || !g_gameWnd) return;
    const ULONGLONG now = GetTickCount64();
    if (now < s_next) return;
    s_next = now + 2000;
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    auto op = g ? (PFN_GtKmtOpen)(void*)GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName") : nullptr;
    auto cl = g ? (PFN_GtKmtClose)(void*)GetProcAddress(g, "D3DKMTCloseAdapter") : nullptr;
    auto qu = g ? (PFN_GtKmtQuery)(void*)GetProcAddress(g, "D3DKMTQueryStatistics") : nullptr;
    if (!op || !cl || !qu) {
        s_done = true;  // system to neumi: dal se neptat
        Log("vlakna hry: citace jadra grafiky (D3DKMT) nejsou - vytizeni GPU se nemeri");
        return;
    }
    const char* why = nullptr;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    const HMONITOR mon = MonitorFromWindow(g_gameWnd, MONITOR_DEFAULTTONEAREST);
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME oa = {};
    static D3DKMT_QUERYSTATISTICS q;
    if (!mon || !GetMonitorInfoW(mon, &mi)) why = "monitor okna hry nezjisten";
    if (!why) {
        wcsncpy(oa.DeviceName, mi.szDevice, 31);
        if (op(&oa) < 0) why = "adapter monitoru nejde otevrit";
    }
    if (!why) {
        D3DKMT_CLOSEADAPTER ca = {};
        ca.hAdapter = oa.hAdapter;
        cl(&ca);
        memset(&q, 0, sizeof q);
        q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
        q.AdapterLuid = oa.AdapterLuid;
        if (qu(&q) < 0) why = "statistika adapteru selhala";
    }
    if (why) {
        ++s_fails;
        if (!s_logged || s_fails == 30) {  // 1. nezdar a po ~1 min jeste jednou; zkousi se dal
            s_logged = true;
            Log("vlakna hry: citace jadra grafiky zatim nejdou (%s, pokus %u) - zkusi se znovu za 2 s", why, s_fails);
        }
        return;
    }
    s_done = true;
    InterlockedExchange(&g_gtLuidLo, (LONG)oa.AdapterLuid.LowPart);
    InterlockedExchange(&g_gtLuidHi, oa.AdapterLuid.HighPart);
    g_gtKmtQuery = qu;
    InterlockedExchange(&g_gtNodes, (LONG)std::min<ULONG>(q.QueryResult.AdapterInformation.NodeCount, 64));
    Log("vlakna hry: vytizeni GPU z citacu jadra grafiky, adapter %08lX:%08lX, uzlu %ld", (unsigned long)g_gtLuidHi,
        (unsigned long)g_gtLuidLo, g_gtNodes);
}

// Prehled uzlu GPU (vlakno mereni, ~5 s): vytizeni kazdeho uzlu nad 0,5 %, vyber nejvytizenejsiho
static void GtGpuList(double el) {
    static double s_prev[3][64];
    static bool s_have;
    static D3DKMT_QUERYSTATISTICS q;
    const LONG nn = g_gtNodes;
    if (nn <= 0 || el <= 0.0) return;
    HANDLE hp = g_gtHelperProc;
    char txt[300];
    size_t pos = (size_t)snprintf(txt, sizeof txt, "GPU uzly %ld (vsechny procesy / hra / helper):", nn);
    double best = -1.0;
    LONG bi = -1;
    for (LONG i = 0; i < nn; ++i) {
        double pct[3] = {0, 0, 0};
        for (int w = 0; w < 3; ++w) {
            const double v = GtNodeMs(q, i, w == 0 ? nullptr : w == 1 ? GetCurrentProcess() : hp);
            if (v < 0.0 || (w == 2 && !hp)) {
                pct[w] = -1.0;
                continue;
            }
            pct[w] = s_have ? (v - s_prev[w][i]) * 100.0 / el : 0.0;
            s_prev[w][i] = v;
        }
        const double ph = std::max(0.0, pct[1]) + std::max(0.0, pct[2]);  // hra + helper (globalni citac uzlu 3D NVIDIA nevraci)
        if (ph > best) best = ph, bi = i;
        if ((pct[0] >= 0.5 || pct[1] >= 0.5 || pct[2] >= 0.5) && pos < sizeof txt - 32)
            pos += (size_t)snprintf(txt + pos, sizeof txt - pos, " %ld: %.0f / %.0f / %.0f %%", i, pct[0], pct[1], pct[2]);
    }
    if (s_have && bi >= 0 && best > 5.0) InterlockedExchange(&g_gtNode, bi);
    s_have = true;
    AcquireSRWLockExclusive(&g_gtTxtLock);
    memcpy(g_gtGpuTxt, txt, sizeof txt);
    ReleaseSRWLockExclusive(&g_gtTxtLock);
}

static LONGLONG GtRead(volatile LONGLONG* p) { return InterlockedCompareExchange64(p, 0, 0); }

// Konec cekani na hlavnim vlakne: a = zacatek (QPC). yield = Sleep(0) / SwitchToThread / timeout 0.
static void GtWaitEnd(LONGLONG a, bool sleep, bool yield) {
    const LONGLONG b = Qpc(), d = b - a;
    const LONGLONG rel = GtRead(&g_gtRelQ);
    const LONGLONG q2 = g_qpf.QuadPart / 500;  // 2 ms
    InterlockedIncrement64(&g_gtAcc[GTA_NWAIT]);
    if (!sleep && rel && a < rel && b >= rel) {
        static const double kEdge[6] = {0.1, 0.25, 0.5, 1.0, 2.0, 5.0};
        const double after = (double)(b - rel) * 1000.0 / (double)g_qpf.QuadPart;
        int h = 0;
        while (h < 6 && after >= kEdge[h]) ++h;
        InterlockedIncrement64(&g_gtAcc[GTA_H0 + h]);
        if (b - rel <= q2) {
            g_gtRwA = a;
            g_gtRwB = b;
            InterlockedExchangeAdd64(&g_gtAcc[GTA_WREND], d);
            InterlockedIncrement64(&g_gtAcc[GTA_NWREND]);
            return;
        }
    }
    InterlockedExchangeAdd64(&g_gtAcc[sleep ? GTA_SLEEP : yield ? GTA_YIELD : GTA_WOTHER], d);
}

static bool GtOnMain() { return (LONG)GetCurrentThreadId() == g_gtMainTid; }

typedef DWORD(WINAPI* PFN_GtWfso)(HANDLE, DWORD);
typedef DWORD(WINAPI* PFN_GtWfsoEx)(HANDLE, DWORD, BOOL);
typedef DWORD(WINAPI* PFN_GtWfmo)(DWORD, const HANDLE*, BOOL, DWORD);
typedef DWORD(WINAPI* PFN_GtWfmoEx)(DWORD, const HANDLE*, BOOL, DWORD, BOOL);
typedef DWORD(WINAPI* PFN_GtMsgW)(DWORD, const HANDLE*, BOOL, DWORD, DWORD);
typedef DWORD(WINAPI* PFN_GtMsgWEx)(DWORD, const HANDLE*, DWORD, DWORD, DWORD);
typedef void(WINAPI* PFN_GtSleep)(DWORD);
typedef DWORD(WINAPI* PFN_GtSleepEx)(DWORD, BOOL);
typedef BOOL(WINAPI* PFN_GtScvCs)(PCONDITION_VARIABLE, PCRITICAL_SECTION, DWORD);
typedef BOOL(WINAPI* PFN_GtScvSrw)(PCONDITION_VARIABLE, PSRWLOCK, DWORD, ULONG);
typedef BOOL(WINAPI* PFN_GtYield)();
typedef BOOL(WINAPI* PFN_GtPeekA)(LPMSG, HWND, UINT, UINT, UINT);
static PFN_GtWfso o_GtWfso;
static PFN_GtWfsoEx o_GtWfsoEx;
static PFN_GtWfmo o_GtWfmo;
static PFN_GtWfmoEx o_GtWfmoEx;
static PFN_GtMsgW o_GtMsgW;
static PFN_GtMsgWEx o_GtMsgWEx;
static PFN_GtSleep o_GtSleep;
static PFN_GtSleepEx o_GtSleepEx;
static PFN_GtScvCs o_GtScvCs;
static PFN_GtScvSrw o_GtScvSrw;
static PFN_GtYield o_GtYield;
static PFN_GtPeekA o_GtPeekA;
static volatile LONG g_gtPeekHooked;  // PeekMessageA v inputsystem.dll hakovany ([latency] SampleSync)

static DWORD WINAPI Gt_WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!GtOnMain()) return o_GtWfso(h, ms);
    const LONGLONG a = Qpc();
    const DWORD r = o_GtWfso(h, ms);
    GtWaitEnd(a, false, ms == 0);
    return r;
}
static DWORD WINAPI Gt_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL al) {
    if (!GtOnMain()) return o_GtWfsoEx(h, ms, al);
    const LONGLONG a = Qpc();
    const DWORD r = o_GtWfsoEx(h, ms, al);
    GtWaitEnd(a, false, ms == 0);
    return r;
}
static DWORD WINAPI Gt_WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) {
    if (!GtOnMain()) return o_GtWfmo(n, hs, all, ms);
    const LONGLONG a = Qpc();
    const DWORD r = o_GtWfmo(n, hs, all, ms);
    GtWaitEnd(a, false, ms == 0);
    return r;
}
static DWORD WINAPI Gt_WaitForMultipleObjectsEx(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, BOOL al) {
    if (!GtOnMain()) return o_GtWfmoEx(n, hs, all, ms, al);
    const LONGLONG a = Qpc();
    const DWORD r = o_GtWfmoEx(n, hs, all, ms, al);
    GtWaitEnd(a, false, ms == 0);
    return r;
}
static DWORD WINAPI Gt_MsgWaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, DWORD wm) {
    if (!GtOnMain()) return o_GtMsgW(n, hs, all, ms, wm);
    const LONGLONG a = Qpc();
    const DWORD r = o_GtMsgW(n, hs, all, ms, wm);
    GtWaitEnd(a, false, ms == 0);
    return r;
}
static DWORD WINAPI Gt_MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE* hs, DWORD ms, DWORD wm, DWORD fl) {
    if (!GtOnMain()) return o_GtMsgWEx(n, hs, ms, wm, fl);
    const LONGLONG a = Qpc();
    const DWORD r = o_GtMsgWEx(n, hs, ms, wm, fl);
    GtWaitEnd(a, false, ms == 0);
    return r;
}
static void WINAPI Gt_Sleep(DWORD ms) {
    if (!GtOnMain()) {
        o_GtSleep(ms);
        return;
    }
    const LONGLONG a = Qpc();
    o_GtSleep(ms);
    GtWaitEnd(a, ms > 0, ms == 0);
}
static DWORD WINAPI Gt_SleepEx(DWORD ms, BOOL al) {
    if (!GtOnMain()) return o_GtSleepEx(ms, al);
    const LONGLONG a = Qpc();
    const DWORD r = o_GtSleepEx(ms, al);
    GtWaitEnd(a, ms > 0, ms == 0);
    return r;
}
static BOOL WINAPI Gt_SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms) {
    if (!GtOnMain()) return o_GtScvCs(cv, cs, ms);
    const LONGLONG a = Qpc();
    const BOOL r = o_GtScvCs(cv, cs, ms);
    GtWaitEnd(a, false, ms == 0);
    return r;
}
static BOOL WINAPI Gt_SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG f) {
    if (!GtOnMain()) return o_GtScvSrw(cv, l, ms, f);
    const LONGLONG a = Qpc();
    const BOOL r = o_GtScvSrw(cv, l, ms, f);
    GtWaitEnd(a, false, ms == 0);
    return r;
}
static BOOL WINAPI Gt_SwitchToThread() {
    if (!GtOnMain()) return o_GtYield();
    const LONGLONG a = Qpc();
    const BOOL r = o_GtYield();
    GtWaitEnd(a, false, true);
    return r;
}

// [latency] SampleSync (src/latchain.inl, Impulse krok B): pumpa zprav vstupu = PeekMessageA z modulu hry na hlavnim
// vlakne (v Black Mesa ji vola engine.dll, jednou za snimek). Prvni volani po volani, ktere vratilo FALSE (fronta
// prazdna = konec minule pumpy), je zacatek pumpy: tam se spi pred odberem vstupu. Hacek se instaluje jen se
// SampleSync; na jinych vlaknech jen preda volani.
static LONGLONG SsPump(LONGLONG t);
bool g_ssOn;   // [latency] SampleSync 1 nebo 2
bool g_ssReg;  // [latency] SampleSync=2: i regulator latence (bez FG) spi v pumpe - pokus, kmita (docs kap. 12)
static uintptr_t g_gtInLo, g_gtInHi;  // rozsah inputsystem.dll
static volatile LONG g_gtPeekN[4];    // volani na hlavnim vlakne: z inputsystem.dll, jinde; jine vlakno; zacatky pumpy
                                      // (v Black Mesa pumpuje engine.dll, z inputsystem.dll 0)
static BOOL WINAPI Gt_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT f) {
    static bool s_empty = true;
    if (!GtOnMain()) {
        InterlockedIncrement(&g_gtPeekN[2]);
        return o_GtPeekA(m, h, a, b, f);
    }
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    const bool in = ra >= g_gtInLo && ra < g_gtInHi;
    InterlockedIncrement(&g_gtPeekN[in ? 0 : 1]);
    if (s_empty) {  // pumpu zprav vstupu vola engine.dll (ne inputsystem.dll): zacatek kazde pumpy na hlavnim vlakne
        InterlockedIncrement(&g_gtPeekN[3]);
        SsPump(Qpc());
    }
    const BOOL r = o_GtPeekA(m, h, a, b, f);
    s_empty = !r;
    return r;
}

struct GtFn {
    const char* name;
    void* hook;
    void** orig;
    bool user32;
    bool input;  // jen modul inputsystem.dll a jen se SampleSync
};
static const GtFn kGtFns[] = {
    {"WaitForSingleObject", (void*)&Gt_WaitForSingleObject, (void**)&o_GtWfso, false},
    {"WaitForSingleObjectEx", (void*)&Gt_WaitForSingleObjectEx, (void**)&o_GtWfsoEx, false},
    {"WaitForMultipleObjects", (void*)&Gt_WaitForMultipleObjects, (void**)&o_GtWfmo, false},
    {"WaitForMultipleObjectsEx", (void*)&Gt_WaitForMultipleObjectsEx, (void**)&o_GtWfmoEx, false},
    {"MsgWaitForMultipleObjects", (void*)&Gt_MsgWaitForMultipleObjects, (void**)&o_GtMsgW, true},
    {"MsgWaitForMultipleObjectsEx", (void*)&Gt_MsgWaitForMultipleObjectsEx, (void**)&o_GtMsgWEx, true},
    {"Sleep", (void*)&Gt_Sleep, (void**)&o_GtSleep, false},
    {"SleepEx", (void*)&Gt_SleepEx, (void**)&o_GtSleepEx, false},
    {"SleepConditionVariableCS", (void*)&Gt_SleepConditionVariableCS, (void**)&o_GtScvCs, false},
    {"SleepConditionVariableSRW", (void*)&Gt_SleepConditionVariableSRW, (void**)&o_GtScvSrw, false},
    {"SwitchToThread", (void*)&Gt_SwitchToThread, (void**)&o_GtYield, false},
    {"PeekMessageA", (void*)&Gt_PeekMessageA, (void**)&o_GtPeekA, true, true},
};
static const int kGtFnN = (int)(sizeof kGtFns / sizeof kGtFns[0]);

static bool GtResolve() {
    HMODULE k = GetModuleHandleW(L"kernel32.dll"), u = GetModuleHandleW(L"user32.dll");
    for (const GtFn& f : kGtFns) {
        HMODULE m = f.user32 ? u : k;
        *f.orig = m ? (void*)GetProcAddress(m, f.name) : nullptr;
        if (!*f.orig) return false;
    }
    return true;
}

// Imports modulu m: funkce z kGtFns z kernel32 / kernelbase / api-ms-win-core-* / user32 -> hacky. Vraci pocet.
static int GtPatchModule(HMODULE m, bool input) {
    auto base = (const BYTE*)m;
    auto dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (auto imp = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress); dd.VirtualAddress && imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk || !imp->FirstThunk) continue;
        const char* dll = (const char*)(base + imp->Name);
        const bool u = !_stricmp(dll, "user32.dll");
        if (!u && _stricmp(dll, "kernel32.dll") && _stricmp(dll, "kernelbase.dll") && _strnicmp(dll, "api-ms-win-core-", 16))
            continue;
        auto names = (const IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            for (const GtFn& f : kGtFns) {
                if (f.user32 != u || strcmp((const char*)ibn->Name, f.name) != 0) continue;
                if (f.input && !g_ssOn) break;
                if ((void*)(uintptr_t)addrs->u1.Function == f.hook) break;
                DWORD old = 0;
                if (!VirtualProtect(&addrs->u1.Function, sizeof addrs->u1.Function, PAGE_READWRITE, &old)) break;
                InterlockedExchange((volatile LONG*)&addrs->u1.Function, (LONG)(uintptr_t)f.hook);
                VirtualProtect(&addrs->u1.Function, sizeof addrs->u1.Function, old, &old);
                ++n;
                if (f.input) InterlockedExchange(&g_gtPeekHooked, 1);
                break;
            }
        }
    }
    return n;
}

// Moduly hry (pod adresarem bms.exe, bez bin\thirdparty) dosud bez hacku
static void GtPatchGame() {
    static HMODULE s_done[256];
    static int s_nDone;
    static wchar_t s_root[MAX_PATH];
    static size_t s_rootN;
    if (!s_rootN) {
        GetModuleFileNameW(nullptr, s_root, MAX_PATH);
        wchar_t* sl = wcsrchr(s_root, L'\\');
        if (!sl) return;
        sl[1] = 0;
        s_rootN = wcslen(s_root);
    }
    HMODULE mods[512];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    const int cnt = (int)std::min<DWORD>(need / sizeof(HMODULE), 512);
    for (int i = 0; i < cnt && s_nDone < 256; ++i) {
        bool seen = false;
        for (int j = 0; j < s_nDone && !seen; ++j) seen = s_done[j] == mods[i];
        if (seen) continue;
        s_done[s_nDone++] = mods[i];
        wchar_t p[MAX_PATH];
        if (!GetModuleFileNameW(mods[i], p, MAX_PATH) || _wcsnicmp(p, s_root, s_rootN) != 0) continue;
        _wcslwr_s(p, MAX_PATH);
        if (wcsstr(p + s_rootN, L"thirdparty")) continue;
        const size_t pl = wcslen(p);
        const bool input = pl > 16 && !wcscmp(p + pl - 16, L"\\inputsystem.dll");
        if (input) {
            auto nt = (const IMAGE_NT_HEADERS*)((const BYTE*)mods[i] + ((const IMAGE_DOS_HEADER*)mods[i])->e_lfanew);
            g_gtInLo = (uintptr_t)mods[i];
            g_gtInHi = g_gtInLo + nt->OptionalHeader.SizeOfImage;
        }
        const int n = GtPatchModule(mods[i], input);
        if (n > 0) {
            InterlockedIncrement(&g_gtHookedMods);
            InterlockedExchangeAdd(&g_gtHookedFns, n);
        }
    }
}

// Prehled vsech vlaken procesu (vlakno mereni, ~5 s): CPU % jednoho jadra, jmeno
static void GtThreadList(double el) {
    struct T {
        DWORD tid;
        HANDLE h;
        ULONG64 cyc;
        bool seen;
    };
    static T s_t[192];
    static int s_n;
    typedef HRESULT(WINAPI * PFN_GTD)(HANDLE, PWSTR*);
    static PFN_GTD s_gtd = (PFN_GTD)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    const float tsc = g_gtTscPerMsF;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    for (int i = 0; i < s_n; ++i) s_t[i].seen = false;
    struct R {
        double pct;
        DWORD tid;
        int idx;
    } r[192];
    int nr = 0, total = 0;
    double sum = 0.0;
    THREADENTRY32 te = {sizeof te};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        ++total;
        int i = 0;
        while (i < s_n && s_t[i].tid != te.th32ThreadID) ++i;
        if (i == s_n) {
            if (s_n >= 192) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            s_t[i] = {te.th32ThreadID, h, 0, false};
            ULONG64 c = 0;
            if (QueryThreadCycleTime(h, &c)) s_t[i].cyc = c;
            ++s_n;
            s_t[i].seen = true;
            continue;  // nove vlakno: az od dalsiho prehledu
        }
        s_t[i].seen = true;
        ULONG64 c = 0;
        if (!QueryThreadCycleTime(s_t[i].h, &c)) continue;
        const double ms = tsc > 0.0f ? (double)(c - s_t[i].cyc) / tsc : 0.0;
        s_t[i].cyc = c;
        const double pct = el > 0.0 ? ms * 100.0 / el : 0.0;
        sum += pct;
        if (nr < 192) r[nr++] = {pct, te.th32ThreadID, i};
    }
    CloseHandle(snap);
    std::sort(r, r + nr, [](const R& a, const R& b) { return a.pct > b.pct; });
    char txt[700];
    size_t pos = (size_t)snprintf(txt, sizeof txt, "vlakna procesu %d, CPU celkem %.0f %% jadra:", total, sum);
    for (int i = 0; i < nr && i < 8 && r[i].pct >= 0.5 && pos < sizeof txt - 80; ++i) {
        char nm[48] = "";
        if ((LONG)r[i].tid == g_gtMainTid) snprintf(nm, sizeof nm, "hlavni");
        else if ((LONG)r[i].tid == g_gtRendTid) snprintf(nm, sizeof nm, "render");
        else if ((LONG)r[i].tid == g_gtWorkTid) snprintf(nm, sizeof nm, "mereni vlaken");
        else {
            PWSTR d = nullptr;
            if (s_gtd && SUCCEEDED(s_gtd(s_t[r[i].idx].h, &d)) && d) {
                int k = 0;
                for (; d[k] && k < 40; ++k) nm[k] = d[k] < 128 && d[k] >= 32 ? (char)d[k] : '?';
                nm[k] = 0;
            }
            if (d) LocalFree(d);
            if (!nm[0]) snprintf(nm, sizeof nm, "tid %lu", r[i].tid);
        }
        pos += (size_t)snprintf(txt + pos, sizeof txt - pos, "%s %s %.0f", i ? "," : "", nm, r[i].pct);
    }
    for (int i = 0; i < s_n;) {  // skoncena vlakna (az po jmenech: indexy r[].idx plati jen do ted)
        if (s_t[i].seen) {
            ++i;
            continue;
        }
        CloseHandle(s_t[i].h);
        s_t[i] = s_t[--s_n];
    }
    AcquireSRWLockExclusive(&g_gtTxtLock);
    memcpy(g_gtTxt, txt, sizeof txt);
    ReleaseSRWLockExclusive(&g_gtTxtLock);
}

static DWORD WINAPI GtWorker(void*) {
    InterlockedExchange(&g_gtWorkTid, (LONG)GetCurrentThreadId());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    LONGLONG prev = Qpc();
    for (;;) {
        GtPatchGame();
        o_GtSleep(5000);
        const LONGLONG now = Qpc();
        GtThreadList(Ms(prev, now));
        GtGpuList(Ms(prev, now));
        prev = now;
    }
}

// Za snimek (vlakno renderu, FgEstFrame): rozdily od minuleho volani v ms
struct GtSample {
    float mainCpu, rendCpu;          // CPU cas vlaken (0 = nezname)
    float wRend, wOther, sleep, yld;  // hlavni vlakno: cekani na render, jine cekani, spanek, Sleep(0) / yield
    float gpuBusy;                    // cas behu nejvytizenejsiho uzlu GPU od minuleho snimku (ms, -1 = nezname)
    bool ok;                          // hlavni vlakno zname a hacky aktivni
};
static struct {
    bool init, failed;
    HANDLE hMain;
    LONG tidMain;
    ULONG64 cMain, cRend;
    LONGLONG acc[GTA_N], accLog[GTA_N];
    LONGLONG tsc0q, lastTid;
    unsigned long long tsc0;
    double tscPerMs;
    double nodeMs;
    LONG node = -1;
    // interval radku (vsechny snimky)
    double sDt, sMc, sRc, sWr, sWo, sSl, sY;
    unsigned n, nOk;
} g_gt;

static void GtInit() {
    g_gt.init = true;
    if (!GtResolve()) {
        g_gt.failed = true;
        Log("vlakna hry: funkce kernel32 / user32 nenalezeny - mereni vlaken vypnuto");
        return;
    }
    HMODULE self = nullptr;  // hacky zustavaji do konce procesu
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)(void*)&GtInit,
                       &self);
    g_gt.tsc0 = __rdtsc();
    g_gt.tsc0q = Qpc();
    HANDLE t = CreateThread(nullptr, 64 * 1024, GtWorker, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else Log("vlakna hry: vlakno mereni nevzniklo - jen CPU cas a cekani bez prehledu vlaken");
    Log("vlakna hry: mereni zapnuto ([debug] GameThreads=1): CPU cas hlavniho vlakna a vlakna renderu, cekani hlavniho "
        "vlakna (hacky IAT modulu hry), prehled vlaken");
}

static void GtFrame(LONGLONG now, GtSample* o) {
    *o = GtSample{};
    if (!g_gtOn) return;
    if (!g_gt.init) GtInit();
    if (g_gt.failed) return;
    const LONG rt = (LONG)GetCurrentThreadId();
    if (rt != g_gtRendTid) {  // kresleni presunuto na jine vlakno (napr. po nacitani)
        static unsigned s_logs;
        if (g_gtRendTid && g_gt.tidMain && ++s_logs <= 20)
            Log("vlakna hry: vlakno renderu (Present) %ld, hlavni vlakno %ld - %s", rt, g_gt.tidMain,
                rt == g_gt.tidMain ? "stejne vlakno" : "ruzna vlakna");
        InterlockedExchange(&g_gtRendTid, rt);
    }
    // prevod tiku TSC na ms (QueryThreadCycleTime pocita tiky TSC)
    const double el0 = Ms(g_gt.tsc0q, now);
    if (el0 > 500.0) {
        g_gt.tscPerMs = (double)(__rdtsc() - g_gt.tsc0) / el0;
        g_gtTscPerMsF = (float)g_gt.tscPerMs;
    }
    // hlavni vlakno = vlakno okna hry (overit nejvys 1x za ~1 s)
    if (g_gameWnd && (!g_gt.hMain || now - g_gt.lastTid > g_qpf.QuadPart)) {
        g_gt.lastTid = now;
        const LONG tid = (LONG)GetWindowThreadProcessId(g_gameWnd, nullptr);
        if (tid && tid != g_gt.tidMain) {
            if (g_gt.hMain) CloseHandle(g_gt.hMain);
            g_gt.hMain = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)tid);
            g_gt.tidMain = tid;
            g_gt.cMain = 0;
            InterlockedExchange(&g_gtMainTid, tid);
            Log("vlakna hry: hlavni vlakno (okno hry) %ld, vlakno renderu (Present) %lu - %s", tid, GetCurrentThreadId(),
                (DWORD)tid == GetCurrentThreadId() ? "stejne vlakno" : "ruzna vlakna");
        }
    }
    GtGpuOpen();
    // v103g: dotaz na uzel helperu po case selhaval (k2 v103f: po Esc -1 do konce behu, uzel 4 dal 0 %) -> zkusit novy
    // handle procesu helperu (OpenProcess podle PID), nejvys jednou za 2 s; stav dotazu do logu
    static HANDLE s_hp2;
    static DWORD s_hp2Pid;
    static ULONGLONG s_hpRetry;
    static unsigned s_hpFails, s_hpLogged;
    const DWORD hpid = g_proc ? GetProcessId(g_proc) : 0;
    if (s_hp2 && s_hp2Pid != hpid) {
        CloseHandle(s_hp2);
        s_hp2 = nullptr;
    }
    HANDLE hp = s_hp2 ? s_hp2 : g_proc;
    g_gtHelperProc = hp;
    o->gpuBusy = -1.0f;
    const LONG node = g_gtNode;
    if (node >= 0) {  // hra + helper na uzlu (globalni citac uzlu 3D ovladac NVIDIA nevraci)
        static D3DKMT_QUERYSTATISTICS q;
        const double a = GtNodeMs(q, node, GetCurrentProcess());
        double b = hp ? GtNodeMs(q, node, hp) : -1.0;
        if (hp && b < 0.0) {
            const LONG st = g_gtKmtLast;
            ++s_hpFails;
            const ULONGLONG t = GetTickCount64();
            if (hpid && t >= s_hpRetry) {
                s_hpRetry = t + 2000;
                HANDLE h2 = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, hpid);
                if (!h2) h2 = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, hpid);
                const double b2 = h2 ? GtNodeMs(q, node, h2) : -1.0;
                if (s_hpLogged < 5) {
                    ++s_hpLogged;
                    Log("vlakna hry: citac GPU helperu na uzlu %ld selhal (NTSTATUS 0x%08lX, %u x) - novy handle procesu %s%s",
                        node, (unsigned long)st, s_hpFails, h2 ? "otevren" : "nejde otevrit",
                        b2 >= 0.0 ? ", citac zase jde" : h2 ? " (citac dal nejde, zkusi se znovu za 2 s)" : "");
                }
                if (b2 >= 0.0) {
                    if (s_hp2) CloseHandle(s_hp2);
                    s_hp2 = h2;
                    s_hp2Pid = hpid;
                    g_gtHelperProc = h2;
                    b = b2;
                    s_hpFails = 0;
                } else if (h2) {
                    CloseHandle(h2);
                }
            }
        }
        const double v = a >= 0.0 && b >= 0.0 ? a + b : -1.0;  // (b z noveho handle: nodeMs se prepise, prvni rozdil zahozen)
        if (v >= 0.0 && node == g_gt.node && g_gt.nodeMs > 0.0 && v >= g_gt.nodeMs) o->gpuBusy = (float)(v - g_gt.nodeMs);
        g_gt.node = node;
        g_gt.nodeMs = v;
    }
    ULONG64 cm = 0, cr = 0;
    const bool hm = g_gt.hMain && QueryThreadCycleTime(g_gt.hMain, &cm);
    const bool hr = QueryThreadCycleTime(GetCurrentThread(), &cr) != 0;
    const double k = g_gt.tscPerMs > 0.0 ? 1.0 / g_gt.tscPerMs : 0.0;
    if (hm && g_gt.cMain && k > 0.0) o->mainCpu = (float)((double)(cm - g_gt.cMain) * k);
    if (hr && g_gt.cRend && k > 0.0) o->rendCpu = (float)((double)(cr - g_gt.cRend) * k);
    if (hm) g_gt.cMain = cm;
    if (hr) g_gt.cRend = cr;
    LONGLONG a[GTA_N];
    for (int i = 0; i < GTA_N; ++i) a[i] = GtRead(&g_gtAcc[i]);
    const double q = 1000.0 / (double)g_qpf.QuadPart;
    o->wRend = (float)((double)(a[GTA_WREND] - g_gt.acc[GTA_WREND]) * q);
    o->wOther = (float)((double)(a[GTA_WOTHER] - g_gt.acc[GTA_WOTHER]) * q);
    o->sleep = (float)((double)(a[GTA_SLEEP] - g_gt.acc[GTA_SLEEP]) * q);
    o->yld = (float)((double)(a[GTA_YIELD] - g_gt.acc[GTA_YIELD]) * q);
    o->ok = hm && g_gtHookedFns > 0 && o->mainCpu > 0.0f && a[GTA_NWAIT] > 0;
    memcpy(g_gt.acc, a, sizeof a);
}

// Soucty intervalu radku "vlakna hry" (z FgEstFrame, snimky s dt < 100 ms)
static void GtAddInterval(double dt, const GtSample& s) {
    if (!g_gtOn || g_gt.failed) return;
    g_gt.sDt += dt;
    ++g_gt.n;
    if (!s.ok) return;
    g_gt.sMc += s.mainCpu;
    g_gt.sRc += s.rendCpu;
    g_gt.sWr += s.wRend;
    g_gt.sWo += s.wOther;
    g_gt.sSl += s.sleep;
    g_gt.sY += s.yld;
    ++g_gt.nOk;
}

// Radek "vlakna hry" (FgEstLog, ~5 s)
static void GtLog(double el) {
    if (!g_gtOn || !g_gt.init || g_gt.failed) return;
    LONGLONG a[GTA_N];
    for (int i = 0; i < GTA_N; ++i) a[i] = GtRead(&g_gtAcc[i]);
    LONGLONG d[GTA_N];
    for (int i = 0; i < GTA_N; ++i) d[i] = a[i] - g_gt.accLog[i];
    memcpy(g_gt.accLog, a, sizeof a);
    char txt[700], gtx[300];
    AcquireSRWLockShared(&g_gtTxtLock);
    memcpy(txt, g_gtTxt, sizeof txt);
    memcpy(gtx, g_gtGpuTxt, sizeof gtx);
    ReleaseSRWLockShared(&g_gtTxtLock);
    txt[sizeof txt - 1] = 0;
    gtx[sizeof gtx - 1] = 0;
    const double n = g_gt.nOk ? (double)g_gt.nOk : 1.0, dt = g_gt.n ? g_gt.sDt / g_gt.n : 0.0;
    const double M = g_gt.nOk ? dt - g_gt.sWr / n : 0.0;
    Log("vlakna hry %.1f s: snimku %u (s merenim %u), interval %.2f ms | hlavni vlakno: CPU %.2f ms/snimek, cekani na render "
        "%.2f, jine cekani %.2f, spanek %.2f, yield %.3f ms/snimek -> M = %.2f ms (%.1f FPS) | render: CPU %.2f ms/snimek | "
        "cekani hlavniho vlakna %lld (na render %lld), konec po uvolneni snimku <0,1 / 0,25 / 0,5 / 1 / 2 / 5 / vic ms: "
        "%lld / %lld / %lld / %lld / %lld / %lld / %lld | hacky IAT %ld modulu, %ld funkci | %s | %s (snimek cte uzel %ld)",
        el / 1000.0, g_gt.n, g_gt.nOk, dt, g_gt.sMc / n, g_gt.sWr / n, g_gt.sWo / n, g_gt.sSl / n, g_gt.sY / n, M,
        M > 0.5 ? 1000.0 / M : 0.0, g_gt.sRc / n, d[GTA_NWAIT], d[GTA_NWREND], d[GTA_H0], d[GTA_H0 + 1], d[GTA_H0 + 2],
        d[GTA_H0 + 3], d[GTA_H0 + 4], d[GTA_H0 + 5], d[GTA_H0 + 6], g_gtHookedMods, g_gtHookedFns,
        txt[0] ? txt : "prehled vlaken zatim neni", gtx[0] ? gtx : "GPU bez citacu", g_gtNode);
    g_gt.sDt = g_gt.sMc = g_gt.sRc = g_gt.sWr = g_gt.sWo = g_gt.sSl = g_gt.sY = 0.0;
    g_gt.n = g_gt.nOk = 0;
}

static void GtRelease() {
    if (g_gtOn && g_gt.init) InterlockedExchange64(&g_gtRelQ, Qpc());
}
