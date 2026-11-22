// bms-dlss: motion vektory pohyblivych objektu (BMSD_VERSION 9), cast proxy. Vklada se do proxy.cpp (#include) a
// pouziva jeho stinovy stav (g_vsConst, g_curInfo, g_curDecl...) a originalni funkce zarizeni (o_*).
// Podklady: docs\mv-objekty-krok0.md (overene vstupy, zavazne), docs\advokat-mv-objektu.md kap. 7 (davkovy replay).
//
// Helper pocita MV jen z kamery, takze NPC, dvere, vytahy a vagonky maji s DLSS ghosting. Proxy proto:
//   1. ZAZNAM (OmRecord): behem G-bufferu hlavniho pohledu (RT0 = R32F velikosti backbufferu, pred koncem sceny,
//      cely viewport se Z 0..1, tedy bez zbrane v ruce) si u kazdeho DrawIndexedPrimitive objektu zapamatuje
//      geometrii (VB streamu 0 + offset + stride, IB, deklaraci, argumenty; AddRef na VB a IB do konce davky) a matice
//      bez jitteru ze stinu g_vsConst: brush c4..c7, rigid a skinned c8..c11, c58..c60, u skinned celou paletu
//      c58..c216 (engine ji posila po kouscich, proto vzdy ze stinu). Vynechava okluzni krabicky a vse bez cModel
//      (podle CTAB), draw bez zapisu hloubky nebo barvy, FVF, dynamicky VB (lana) a nezname formaty vah.
//   2. PLATNOST DAT: hook Lock vertex a index bufferu; zapisovy Lock (bez NOOVERWRITE/READONLY, tedy DISCARD nebo
//      prepis) bufferu po zaznamu draw vyradi z davky. Brushe maji dynamicky IB, ktery se ve snimku plni jen dopredu.
//      Sleduje se az do dokresleni davky: Lock z jineho vlakna behem davky vyradi dalsi draw, a kdyz prisel po
//      kontrole uz prehraneho draw, cela davka se zahodi (OBJMV se ten snimek nekopiruje).
//   2b. KAMERA: na konci sceny (OmFilterCamera) zustanou jen zaznamy, jejichz cViewProj c8..c11 je kamera hlavniho
//      pohledu (g_cam ze SelectCamera); u brushe navic c4 = c8 * c58..c60. Jine pohledy do R32F velikosti backbufferu
//      (odraz, monitor, skybox) jdou pryc z davky i z historie (pocitadlo "jina kamera").
//   3. PAROVANI s minulym snimkem (OmPair): klic = deklarace, VB, offset, stride, typ, base, min, pocet vrcholu
//      a primitiv a cesta; u statickeho IB i IB a StartIndex (u dynamickeho se meni s viditelnosti). Nejdriv pary
//      s bitove shodnou matici c58..c60 (objekt stoji), zbytek podle nejblizsi translace (sloupec w c58..c60, u skinned
//      kost 0) s limitem [mv] TeleportDist. Bez paru se neprehrava (helper pouzije MV kamery). Historie plati jen
//      z bezprostredne predchoziho snimku a zahazuje se pri Resetu, zmene zarizeni a vypnuti funkce.
//   4. DAVKA (OmBatch, v TriggerSceneEnd pred kopii A mostu): pohnute rigid objekty (c58..c60 jine nez u paru)
//      a sparovane skinned, jejichz paleta c58..c216 neni bitove shodna s parem (registry nad poctem kosti se meni
//      i u stojicich, kdyz je na scene animovane NPC - pak se prehraji zbytecne, ale spravne) se prehraji vlastnimi
//      shadery (src\shaders\objmv.hlsl) do vlastniho cile OBJMV A32B32G32R32F s vlastnim D24S8. Kdyz neni co
//      prehrat, davka (state block, Clear, kopie OBJMV) odpadne cela. Stav hry uklada state block D3DSBT_ALL
//      (jednou za snimek) a rucne RT0..3, DS, viewport a scissor. Volani v davce jdou originalnimi funkcemi mimo
//      hooky proxy; behem davky hooky volani z jejiho vlakna jen propousteji (OmGuarded), aby se stiny neprepsaly.
//      Kdyz nejde nastavit cil OBJMV nebo jeho D24S8, davka se bez Clear preskoci (jinak by smazala scenu hry).
// Helper pak v mv.comp vezme MV objektu tam, kde A > 0.5 a hloubka sedi s DEPTH_IN (tolerance [mv] DepthTol).
// Rezim [mv] Validate=1 prehrava i staticke objekty (A = 2): helper u nich meri rozdil proti MV kamery (cil < 0.05 px)
// a podil jejich pixelu odmitnutych hloubkou; tim se bez pohledu overi znamenko, meritko, matice a skinning, chyba
// jitteru nebo rasterizace replaye se projevi hlavne odmitnutim na hranach.
// Cast B (od BMSD_VERSION 10, mat_viewportscale < 1): G-buffer se kresli do leveho horniho rohu plneho cile. Zaznam
// bere i mensi viewport od (0, 0) a pamatuje si ho; do davky jdou jen zaznamy s viewportem = render snimku
// (DetectRender v proxy.cpp). Davka kresli do sub-rectu OBJMV (viewport, Clear i meritko MV v pixelech renderu,
// jitter prepocteny na render), most pak kopiruje jen sub-rect. Cile OBJMV zustavaji velke jako scena.

#include "objmv_vs_brush.h"  // build\gen (fxc ze src\shaders\objmv.hlsl): kObjMvVsBrush
#include "objmv_vs_rigid.h"  // kObjMvVsRigid
#include "objmv_vs_skin.h"   // kObjMvVsSkin
#include "objmv_ps.h"        // kObjMvPs

static const UINT kOmPalRegs = 159;             // paleta kosti c58..c216 = 53 kosti x 3 radky
static const UINT kOmPalFloats = kOmPalRegs * 4;
static const UINT kOmPalW = 160;                // textura palet: texel x = registr c58+x
static const UINT kOmPalH = 512;                // radky: 2 na skinned draw (aktualni a minula paleta)
static const size_t kOmMaxSkin = kOmPalH / 2;
static const size_t kOmMaxRecs = 8192;          // strop zaznamu za snimek
static const size_t kOmMaxInval = 256;          // vic zapisovych Locku mezi zaznamem a davkou: snimek se neprehraje
static const size_t kOmMaxDecls = 256;

enum : uint8_t {
    OMF_DEAD = 1,    // buffer prepsan po zaznamu (Lock) - neprehravat
    OMF_MOVED = 2,   // par ma jinou matici c58..c60 (objekt se pohnul)
    OMF_STATIC = 4,  // prehrano jen kvuli validaci (A = 2)
    OMF_DRAWN = 8    // v davce uz prehrano (Lock po nem = davku zahodit)
};

struct OmRec {
    // Klic: identita geometrie mezi snimky (ukazatele jen jako identita; v historii uz nejsou drzene)
    void* decl;                    // deklarace hry (referenci drzi cache deklaraci)
    IDirect3DVertexBuffer9* vb;    // stream 0; AddRef od zaznamu do konce davky
    IDirect3DIndexBuffer9* ib;     // AddRef od zaznamu do konce davky
    UINT vbOff, stride;
    D3DPRIMITIVETYPE type;
    INT base;
    UINT minIdx, numVerts, startIdx, prims;
    uint64_t hash;                 // hash klice
    uint32_t seq;                  // poradi zaznamu ve snimku (platnost bufferu po Lock)
    int declIdx;                   // index do g_omDecls (nase deklarace pro replay)
    int pal;                       // skinned: index palety v g_omPalCur / g_omPalPrev, jinak -1
    int pair;                      // index paru v g_omPrev, -1 = bez paru
    uint8_t path;                  // OM_BRUSH / OM_RIGID / OM_SKINNED
    uint8_t ibDyn;                 // dynamicky IB: IB a StartIndex nejsou v klici
    uint8_t cull;                  // D3DRS_CULLMODE hry
    uint8_t flags;                 // OMF_*
    uint8_t vm;                    // zbran v ruce ([mv] Viewmodel): viewport Z minZ..maxZ (0..0.1), vlastni FOV
    UINT vpW, vpH;                 // viewport draw pri zaznamu (od (0, 0); cast B: mensi nez cil)
    float minZ, maxZ;              // viewport Z pri zaznamu (svet 0..1, zbran 0..0.1)
    float clip[16];                // brush: c4..c7 (cModelViewProj), jinak c8..c11 (cViewProj); stin, bez jitteru
    float vp[16];                  // c8..c11 (cViewProj) pri zaznamu, bez jitteru: kontrola kamery (u brushe engine
                                   // c8 nastavuje taky, krok0: c4 = c8 * c58..c60 u 64/64 draw)
    float model[12];               // c58..c60 (matice modelu, u skinned kost 0)
};

// Nase deklarace k deklaraci hry: jen prvky streamu 0, ktere ctou nase VS (POSITION, BLENDWEIGHT, BLENDINDICES).
struct OmDecl {
    IDirect3DVertexDeclaration9* game;  // drzena reference, aby se ukazatel nemohl recyklovat pro jinou deklaraci
    IDirect3DVertexDeclaration9* mine;  // nullptr = nejde prehrat (bez POSITION FLOAT3/4 ve streamu 0); FLOAT4: rigid
                                        // a skinned berou w z dat jako hra, brush w = 1 (objmv.hlsl)
    bool blend;                         // BLENDWEIGHT SHORT2 + BLENDINDICES D3DCOLOR ve streamu 0 (overeny format)
    bool blendBad;                      // vahy nebo indexy v jinem formatu: skinned draw vynechat
};

// Pocitadla: g_omF za tento snimek, g_omI soucet za interval radku "vykon" (interop.cpp, ~5 s)
struct OmStat {
    unsigned frames;                 // konce sceny se zapnutou funkci
    unsigned seen;                   // DIP v G-bufferu hlavniho pohledu
    unsigned recorded, otherCam, paired, moved, unpaired, teleport, dead;
    unsigned replayed, replaySkin, replayStatic, replayVm;
    unsigned dropped;                // davky zahozene (cil nejde nastavit, Lock behem davky): OBJMV se nekopiruje
    unsigned skipVS, skipState, skipDecl, skipDyn, skipSkin, skipFull;
    LONGLONG recQpc, batchQpc, batchMaxQpc;
};

static bool g_omEnabled;     // [mv] ObjectMV, Alt+Backspace
static bool g_omValidate;    // [mv] Validate
static bool g_omActive;      // plati pro tento snimek: zapnuto, zdroje v poradku a most v rezimu DLAA nebo ladeni MV
static bool g_omBroken;      // zdroje nejdou vytvorit: funkce vypnuta do Resetu zarizeni (DLSS bezi dal)
static bool g_omSkinOff;     // textura palet (VTF) nejde: bez skinned objektu
static DWORD g_omGuardThread;  // vlakno, ktere prave dela davku (0 = zadne): jeho volani hooky jen propousteji
static unsigned g_omGuardHits;
// Totez pro kresleni menu / OSD pred Present (src\menu.inl, OverlayRender): vlakno, ktere prave kresli prekryv
static DWORD g_ovGuardThread;
static unsigned g_ovGuardHits;
static bool g_omGuardLogged, g_omFailLogged, g_omTargetLogged, g_omDeclFullLogged;

static IDirect3DDevice9* g_omDev;          // zarizeni, na kterem vznikly objekty nize
static IDirect3DTexture9* g_omTex;         // OBJMV: A32B32G32R32F, render target velikosti sceny
static IDirect3DSurface9* g_omRT;          // jeho uroven 0 (jde do kopie A mostu)
static IDirect3DSurface9* g_omDS;          // vlastni D24S8 stejne velikosti
static IDirect3DTexture9* g_omPalTex;      // palety kosti pro vertex texture fetch (dynamicka A32B32G32R32F)
static IDirect3DVertexShader9* g_omVS[OM_PATHS];
static IDirect3DPixelShader9* g_omPS;
static UINT g_omW, g_omH;
static std::vector<OmDecl> g_omDecls;
static int g_omDeclLast = -1;
static unsigned g_omDeclLogs;

static std::vector<OmRec> g_omCur, g_omPrev;         // zaznamy tohoto a minuleho snimku
static std::vector<float> g_omPalCur, g_omPalPrev;   // palety skinned zaznamu (kOmPalFloats na zaznam)
static std::vector<std::pair<uint64_t, int>> g_omPrevIdx;  // (hash, index do g_omPrev), serazeno podle hashe
static std::vector<std::pair<uint64_t, int>> g_omTmp;
static std::vector<uint8_t> g_omPrevUsed;
static std::vector<int> g_omList;                    // indexy zaznamu k prehrani
static bool g_omPrevValid;
static uint64_t g_omPrevFrame;                       // g_frame snimku, ze ktereho je g_omPrev
static uint32_t g_omSeq;

// Zapisove Locky bufferu mezi prvnim zaznamem ve snimku a davkou: buffer a poradi posledniho zaznamu pred Lockem
// (jeden zaznam na buffer, plati posledni Lock). Zarizeni hry je D3DCREATE_MULTITHREADED, Lock muze prijit i z jineho
// vlakna nez kresli - proto zamek; poradi vuci zaznamum pak neni zname a zneplatni se vsechny draw toho bufferu.
struct OmInval {
    void* buf;
    uint32_t seq;
};
static std::mutex g_omInvalMx;
static std::vector<OmInval> g_omInval;
static bool g_omInvalAll;                     // seznam pretekl: nic neprehravat
static std::atomic<uint32_t> g_omInvalGen;    // pocet zmen g_omInval (podle nej davka pozna Lock behem sebe)
static uint32_t g_omInvalSeen;                // g_omInvalGen pri poslednim OmApplyInval
static std::atomic<bool> g_omWindow;          // okno od prvniho zaznamu do dokresleni davky (jen v nem se Locky sleduji)
static DWORD g_omThread;                      // vlakno, ktere zaznamenava (kresli)
static bool g_omVBLockHooked, g_omIBLockHooked;
// Posledni dotazany VB a IB a jejich D3DUSAGE_DYNAMIC (plati jen v ramci snimku)
static void* g_omLastVB;
static void* g_omLastIB;
static bool g_omLastVBDyn, g_omLastIBDyn;

static OmStat g_omF, g_omI;
static LARGE_INTEGER g_omQpf;

static LONGLONG OmQpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Hooky proxy, ktere drzi stin stavu hry: behem davky (state block Capture/Apply muze v DXVK volat Set* pres
// patchovanou vtable) volani z vlakna davky jen propousteji dal, jinak by si proxy prepsala stiny hodnotami davky.
// Stejne behem kresleni menu / OSD do backbufferu (g_ovGuardThread, src\menu.inl).
static inline bool OmGuarded() {
    if (!(g_omGuardThread | g_ovGuardThread)) return false;  // bezny pripad: zadna davka ani prekryv
    const DWORD tid = GetCurrentThreadId();
    if (g_omGuardThread == tid) {
        ++g_omGuardHits;
        return true;
    }
    if (g_ovGuardThread == tid) {
        ++g_ovGuardHits;
        return true;
    }
    return false;
}

template <class T>
static void OmSafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// Reference na VB a IB zaznamu tohoto snimku (po davce, nebo kdyz konec sceny nenastal).
static void OmReleaseRefs() {
    for (OmRec& r : g_omCur) {
        if (r.vb) r.vb->Release();
        if (r.ib) r.ib->Release();
    }
    g_omCur.clear();
    g_omPalCur.clear();
}

static void OmClearHistory() {
    g_omPrev.clear();
    g_omPalPrev.clear();
    g_omPrevIdx.clear();
    g_omPrevValid = false;
}

// Objekty zavisle na velikosti sceny
static void OmReleaseTargets() {
    OmSafeRelease(g_omRT);
    OmSafeRelease(g_omTex);
    OmSafeRelease(g_omDS);
    g_omW = g_omH = 0;
}

// Vse vazane na zarizeni (pred Reset/ResetEx, pri novem zarizeni a po selhani): zaznamy, historie, cile, textura
// palet, shadery a deklarace. Vsechno se vytvori znovu pri dalsi davce.
static void OmReleaseDevice(const char* why) {
    const bool had = g_omDev != nullptr;
    OmReleaseRefs();
    OmClearHistory();
    OmReleaseTargets();
    OmSafeRelease(g_omPalTex);
    for (IDirect3DVertexShader9*& vs : g_omVS) OmSafeRelease(vs);
    OmSafeRelease(g_omPS);
    for (OmDecl& dc : g_omDecls) {
        OmSafeRelease(dc.mine);
        OmSafeRelease(dc.game);
    }
    g_omDecls.clear();
    g_omDeclLast = -1;
    g_omDev = nullptr;
    g_omWindow = false;
    {
        std::lock_guard<std::mutex> lk(g_omInvalMx);
        g_omInval.clear();
        g_omInvalAll = false;
    }
    g_omLastVB = g_omLastIB = nullptr;
    if (had && why) Log("MV objektu: zdroje uvolneny (%s)", why);
}

// Selhani vypne jen MV objektu (do Resetu zarizeni), DLSS bezi dal s MV kamery.
static void OmFail(const char* what, HRESULT hr) {
    OmReleaseDevice(nullptr);
    g_omBroken = true;
    g_omActive = false;
    if (!g_omFailLogged) {
        g_omFailLogged = true;
        Log("WARN: MV objektu: %s selhalo (0x%08X) - MV objektu vypnute do Resetu zarizeni, DLSS bezi dal s MV kamery",
            what, (unsigned)hr);
    }
}

static HRESULT OmCreateVS(IDirect3DDevice9* d, const BYTE* code, size_t size, IDirect3DVertexShader9** out) {
    std::vector<DWORD> buf((size + 3) / 4);  // zarovnana kopie bytecode
    memcpy(buf.data(), code, size);
    return o_CreateVS(d, buf.data(), out);   // mimo H_CreateVS (shader neni hry)
}

// Cile, shadery a textura palet. false = funkce nejde (OmFail uz zalogoval).
static bool OmEnsure(IDirect3DDevice9* d, UINT w, UINT h) {
    if (g_omBroken || !w || !h) return false;
    g_omDev = d;  // (objekty jineho zarizeni uz uvolnil OmBatch)
    if (g_omRT && (g_omW != w || g_omH != h)) OmReleaseTargets();
    HRESULT hr;
    if (!g_omRT) {
        hr = d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &g_omTex, nullptr);
        if (FAILED(hr) || !g_omTex) return OmFail("vytvoreni cile OBJMV (A32B32G32R32F)", hr), false;
        hr = g_omTex->GetSurfaceLevel(0, &g_omRT);
        if (FAILED(hr) || !g_omRT) return OmFail("GetSurfaceLevel cile OBJMV", hr), false;
        hr = d->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_omDS, nullptr);
        if (FAILED(hr) || !g_omDS) return OmFail("vytvoreni depth bufferu OBJMV (D24S8)", hr), false;
        g_omW = w;
        g_omH = h;
        Log("MV objektu: cil OBJMV %ux%u A32B32G32R32F + D24S8 vytvoren", w, h);
    }
    if (!g_omPS) {
        std::vector<DWORD> buf((sizeof kObjMvPs + 3) / 4);
        memcpy(buf.data(), kObjMvPs, sizeof kObjMvPs);
        hr = d->CreatePixelShader(buf.data(), &g_omPS);
        if (FAILED(hr) || !g_omPS) return OmFail("vytvoreni pixel shaderu", hr), false;
    }
    if (!g_omVS[OM_BRUSH]) {
        hr = OmCreateVS(d, kObjMvVsBrush, sizeof kObjMvVsBrush, &g_omVS[OM_BRUSH]);
        if (FAILED(hr) || !g_omVS[OM_BRUSH]) return OmFail("vytvoreni VS (brush)", hr), false;
    }
    if (!g_omVS[OM_RIGID]) {
        hr = OmCreateVS(d, kObjMvVsRigid, sizeof kObjMvVsRigid, &g_omVS[OM_RIGID]);
        if (FAILED(hr) || !g_omVS[OM_RIGID]) return OmFail("vytvoreni VS (rigid)", hr), false;
    }
    if (!g_omPalTex && !g_omSkinOff) {
        // Vertex texture fetch: hra ho nepouziva (D3DVERTEXTEXTURESAMPLER0..3 jsou volne), format overit
        const char* what = nullptr;
        hr = D3D_OK;
        IDirect3D9* d3d = nullptr;
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        D3DDISPLAYMODE dm = {};
        if (SUCCEEDED(d->GetDirect3D(&d3d)) && d3d && SUCCEEDED(d->GetCreationParameters(&cp))) {
            if (FAILED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &dm))) dm.Format = D3DFMT_X8R8G8B8;
            hr = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, D3DUSAGE_QUERY_VERTEXTEXTURE,
                                        D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F);
            if (FAILED(hr)) what = "CheckDeviceFormat(D3DUSAGE_QUERY_VERTEXTEXTURE, A32B32G32R32F)";
        }
        if (d3d) d3d->Release();
        if (!what) {
            hr = OmCreateVS(d, kObjMvVsSkin, sizeof kObjMvVsSkin, &g_omVS[OM_SKINNED]);
            if (FAILED(hr) || !g_omVS[OM_SKINNED]) what = "vytvoreni VS (skinned)";
        }
        if (!what) {
            hr = d->CreateTexture(kOmPalW, kOmPalH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT,
                                  &g_omPalTex, nullptr);
            if (FAILED(hr) || !g_omPalTex) what = "vytvoreni textury palet kosti";
        }
        if (what) {
            OmSafeRelease(g_omPalTex);
            OmSafeRelease(g_omVS[OM_SKINNED]);
            g_omSkinOff = true;
            Log("WARN: MV objektu: %s selhalo (0x%08X) - skinned objekty (NPC) zustanou s MV kamery, rigid a brushe "
                "bezi dal",
                what, (unsigned)hr);
        } else {
            Log("MV objektu: textura palet kosti %ux%u A32B32G32R32F (vertex texture fetch) vytvorena", kOmPalW, kOmPalH);
        }
    }
    return true;
}

// Nase deklarace k deklaraci hry (index do g_omDecls, -1 = cache plna). Format se vypise jednou.
static int OmFindDecl(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* game) {
    if (g_omDeclLast >= 0 && g_omDecls[g_omDeclLast].game == game) return g_omDeclLast;
    for (size_t i = 0; i < g_omDecls.size(); ++i)
        if (g_omDecls[i].game == game) return g_omDeclLast = (int)i;
    if (g_omDecls.size() >= kOmMaxDecls) {
        if (!g_omDeclFullLogged) {
            g_omDeclFullLogged = true;
            Log("WARN: MV objektu: cache deklaraci je plna (%u) - modely s novou deklaraci maji do Resetu zarizeni jen "
                "MV kamery (vynechano: deklarace)",
                (unsigned)kOmMaxDecls);
        }
        return -1;
    }
    OmDecl dc = {};
    dc.game = game;
    std::string desc;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    D3DVERTEXELEMENT9 mine[4];
    UINT n = 0, m = 0;
    bool pos = false, bw = false, bi = false;
    D3DVERTEXELEMENT9 eBw = {}, eBi = {};
    if (SUCCEEDED(game->GetDeclaration(nullptr, &n)) && n && n <= MAXD3DDECLLENGTH + 1 &&
        SUCCEEDED(game->GetDeclaration(el, &n))) {
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
            const D3DVERTEXELEMENT9& e = el[i];
            if (e.UsageIndex != 0 || (e.Usage != D3DDECLUSAGE_POSITION && e.Usage != D3DDECLUSAGE_BLENDWEIGHT &&
                                      e.Usage != D3DDECLUSAGE_BLENDINDICES))
                continue;
            char b[64];
            snprintf(b, sizeof b, "%ss%u+%u %s %s", desc.empty() ? "" : ", ", e.Stream, e.Offset, DeclTypeName(e.Type),
                     DeclUsageName(e.Usage));
            desc += b;
            if (e.Usage == D3DDECLUSAGE_POSITION) {
                if (e.Stream == 0 && (e.Type == D3DDECLTYPE_FLOAT3 || e.Type == D3DDECLTYPE_FLOAT4) && !pos) {
                    pos = true;
                    mine[m++] = e;
                }
            } else if (e.Usage == D3DDECLUSAGE_BLENDWEIGHT) {
                if (e.Stream == 0 && e.Type == D3DDECLTYPE_SHORT2 && !bw) {
                    bw = true;
                    eBw = e;
                } else {
                    dc.blendBad = true;
                }
            } else {
                if (e.Stream == 0 && e.Type == D3DDECLTYPE_D3DCOLOR && !bi) {
                    bi = true;
                    eBi = e;
                } else {
                    dc.blendBad = true;
                }
            }
        }
    }
    if (bw != bi) dc.blendBad = true;  // jen vahy nebo jen indexy: neznama kombinace
    dc.blend = bw && bi && !dc.blendBad;
    if (pos) {
        if (dc.blend) {
            mine[m++] = eBw;
            mine[m++] = eBi;
        }
        const D3DVERTEXELEMENT9 end = D3DDECL_END();
        mine[m] = end;
        const HRESULT hr = d->CreateVertexDeclaration(mine, &dc.mine);
        if (FAILED(hr)) dc.mine = nullptr;
    }
    game->AddRef();
    if (g_omDeclLogs < 64) {
        ++g_omDeclLogs;
        Log("MV objektu: deklarace %p: %s -> %s", (void*)game, desc.empty() ? "(bez pozice)" : desc.c_str(),
            !dc.mine ? "nejde prehrat (pozice neni FLOAT3/4 ve streamu 0)"
                     : dc.blend ? "rigid i skinned (vahy SHORT2, indexy D3DCOLOR)"
                                : dc.blendBad ? "jen rigid (neznamy format vah nebo indexu kosti - skinned draw vynechany)"
                                              : "rigid (bez vah)");
    }
    g_omDecls.push_back(dc);
    return g_omDeclLast = (int)g_omDecls.size() - 1;
}

static bool OmVBDynamic(IDirect3DVertexBuffer9* vb) {
    if (vb != g_omLastVB) {
        D3DVERTEXBUFFER_DESC desc = {};
        g_omLastVBDyn = FAILED(vb->GetDesc(&desc)) || (desc.Usage & D3DUSAGE_DYNAMIC) != 0;
        g_omLastVB = vb;
    }
    return g_omLastVBDyn;
}

static bool OmIBDynamic(IDirect3DIndexBuffer9* ib) {
    if (ib != g_omLastIB) {
        D3DINDEXBUFFER_DESC desc = {};
        g_omLastIBDyn = FAILED(ib->GetDesc(&desc)) || (desc.Usage & D3DUSAGE_DYNAMIC) != 0;
        g_omLastIB = ib;
    }
    return g_omLastIBDyn;
}

// ---------------------------------------------------------------------------
// Platnost dat: Lock vertex a index bufferu
// ---------------------------------------------------------------------------
// Zapisovy Lock (DISCARD nebo prepis bez NOOVERWRITE) mezi zaznamem a davkou znamena, ze data zaznamenanych draw
// v tom bufferu uz nemusi platit (v DXVK DISCARD prejmenuje slice). Lock s NOOVERWRITE jen pridava za pouzita data.
static void OmNoteLock(void* buf, DWORD flags) {
    if (!g_omWindow.load(std::memory_order_relaxed) || (flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) return;
    // z vlakna, ktere kresli: zneplatni jen draw zaznamenane pred timto Lockem; z jineho vlakna vsechny
    const uint32_t seq = GetCurrentThreadId() == g_omThread ? g_omSeq : UINT32_MAX;
    std::lock_guard<std::mutex> lk(g_omInvalMx);
    // generace pred zmenou (pod zamkem): davka po svem draw pozna, ze Lock prisel driv nez ona data pouzila
    g_omInvalGen.fetch_add(1, std::memory_order_relaxed);
    for (OmInval& iv : g_omInval)
        if (iv.buf == buf) {
            if (seq > iv.seq) iv.seq = seq;
            return;
        }
    if (g_omInval.size() >= kOmMaxInval) {
        g_omInvalAll = true;
        return;
    }
    g_omInval.push_back({buf, seq});
}

// Zapisove Locky od posledniho vyhodnoceni -> OMF_DEAD u zasazenych zaznamu; seznam se vyprazdni, okno sledovani
// zustava otevrene (davka vola znovu, kdyz se g_omInvalGen zmeni).
static void OmApplyInval() {
    std::lock_guard<std::mutex> lk(g_omInvalMx);
    g_omInvalSeen = g_omInvalGen.load(std::memory_order_relaxed);
    if (g_omInvalAll) {
        for (OmRec& r : g_omCur) r.flags |= OMF_DEAD;
    } else {
        for (const OmInval& iv : g_omInval)
            for (OmRec& r : g_omCur)
                if (r.seq <= iv.seq && (r.vb == iv.buf || r.ib == iv.buf)) r.flags |= OMF_DEAD;
    }
    g_omInval.clear();
    g_omInvalAll = false;
}

// Konec sledovani Locku (po dokresleni davky nebo na konci snimku)
static void OmCloseWindow() {
    g_omWindow = false;
    std::lock_guard<std::mutex> lk(g_omInvalMx);
    g_omInval.clear();
    g_omInvalAll = false;
}

static HRESULT STDMETHODCALLTYPE H_VBLock(IDirect3DVertexBuffer9* b, UINT off, UINT size, void** data, DWORD flags) {
    OmNoteLock(b, flags);
    return o_VBLock(b, off, size, data, flags);
}

static HRESULT STDMETHODCALLTYPE H_IBLock(IDirect3DIndexBuffer9* b, UINT off, UINT size, void** data, DWORD flags) {
    OmNoteLock(b, flags);
    return o_IBLock(b, off, size, data, flags);
}

// Vsechny VB (a vsechny IB) DXVK sdileji jednu vtable: hook staci jednou, z prvniho bufferu, ktery hra navaze.
static void OmHookVBLock(IDirect3DVertexBuffer9* vb) {
    if (g_omVBLockHooked || !vb) return;
    g_omVBLockHooked = true;
    HookVtbl(vb, 11, (PFN_VBLock)H_VBLock, o_VBLock, "VertexBuffer::Lock");
}

static void OmHookIBLock(IDirect3DIndexBuffer9* ib) {
    if (g_omIBLockHooked || !ib) return;
    g_omIBLockHooked = true;
    HookVtbl(ib, 11, (PFN_IBLock)H_IBLock, o_IBLock, "IndexBuffer::Lock");
}

// ---------------------------------------------------------------------------
// Zaznam (vola H_DrawIdxPrim po PreDraw, pred skutecnym draw callem hry)
// ---------------------------------------------------------------------------
static uint64_t OmMix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

static void OmRecord(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT base, UINT minIdx, UINT numVerts, UINT startIdx,
                     UINT prims) {
    // G-buffer hlavniho pohledu: RT0 = R32F velikosti backbufferu, pred koncem sceny
    if (!g_omActive || g_sceneEndDone || g_rt0Fmt != D3DFMT_R32F || !g_bbW || g_rt0W != g_bbW || g_rt0H != g_bbH) return;
    const LONGLONG t0 = OmQpc();
    OmStat& st = g_omF;
    ++st.seen;
    do {
        const ShaderInfo* si = g_curInfo;
        if (!si || si->omPath == OM_NONE) {  // okluzni krabicky, sprity, VS bez cModel
            ++st.skipVS;
            break;
        }
        // Viewport od leveho horniho rohu se Z 0..1: cely cil, nebo pri mat_viewportscale < 1 mensi obdelnik (render;
        // do davky jdou jen zaznamy s viewportem = render snimku, viz OmFilterCamera). Zbran v ruce ma MaxZ 0.1.
        // Draw pise hloubku i barvu a neni pruhledny; bez instancovani
        // [mv] Viewmodel: i zbran v ruce (viewport Z 0..MaxZ < 0.5; hra ji kresli s MaxZ 0.1 a vlastnim FOV)
        const bool vm = g_cfg.objMvViewmodel && g_vp.MinZ == 0.0f && g_vp.MaxZ > 0.0f && g_vp.MaxZ < 0.5f;
        if (!g_vpKnown || g_vp.X || g_vp.Y || !g_vp.Width || !g_vp.Height || g_vp.Width > g_rt0W ||
            g_vp.Height > g_rt0H || g_vp.MinZ != 0.0f || (g_vp.MaxZ != 1.0f && !vm) || !g_rsZEnable ||
            !g_rsZWrite || !(g_rsColorWrite & 0xF) || g_rsAlphaBlend || g_curFreq0 != 1 || !prims) {
            ++st.skipState;
            break;
        }
        uint8_t path = si->omPath;
        if (vm && path == OM_BRUSH) {  // zbran je model (rigid / skinned); brush s jinym Z rozsahem neznamy
            ++st.skipState;
            break;
        }
        const float* clip = &g_vsConst[(path == OM_BRUSH ? 4 : 8) * 4];
        // perspektivni matice (stejna podminka jako jitter v PreDraw: hra tento draw jitterovala prave kdyz bezi jitter)
        if (clip[12] * clip[12] + clip[13] * clip[13] + clip[14] * clip[14] <= 1e-10f) {
            ++st.skipState;
            break;
        }
        if (!g_curDecl || !g_curVB0 || !g_curVB0Stride || !g_curIB) {  // FVF, draw bez deklarace nebo bufferu
            ++st.skipDecl;
            break;
        }
        const int di = OmFindDecl(d, g_curDecl);
        if (di < 0 || !g_omDecls[di].mine) {
            ++st.skipDecl;
            break;
        }
        if (path == OM_SKINNED) {
            if (g_omDecls[di].blendBad) {
                ++st.skipDecl;
                break;
            }
            // VS s paletou, vrcholy bez vah a indexu: hra pocita s vychozimi hodnotami prvku = kost 0 s vahou 1
            if (!g_omDecls[di].blend) path = OM_RIGID;
        }
        if (path == OM_SKINNED && (g_omSkinOff || g_omPalCur.size() >= kOmMaxSkin * kOmPalFloats)) {
            ++st.skipSkin;
            break;
        }
        if (OmVBDynamic(g_curVB0)) {  // lana a kabely: geometrie ve svetovych souradnicich, MV kamery staci
            ++st.skipDyn;
            break;
        }
        if (g_omCur.size() >= kOmMaxRecs) {
            ++st.skipFull;
            break;
        }
        g_omCur.emplace_back();
        OmRec& r = g_omCur.back();
        r.decl = g_curDecl;
        r.vb = g_curVB0;
        r.ib = g_curIB;
        r.vbOff = g_curVB0Off;
        r.stride = g_curVB0Stride;
        r.type = type;
        r.base = base;
        r.minIdx = minIdx;
        r.numVerts = numVerts;
        r.startIdx = startIdx;
        r.prims = prims;
        r.seq = ++g_omSeq;
        r.declIdx = di;
        r.pal = -1;
        r.pair = -1;
        r.path = path;
        r.ibDyn = OmIBDynamic(g_curIB) ? 1 : 0;
        r.cull = (uint8_t)g_rsCull;
        r.flags = 0;
        r.vm = vm ? 1 : 0;
        r.vpW = g_vp.Width;
        r.vpH = g_vp.Height;
        r.minZ = g_vp.MinZ;
        r.maxZ = g_vp.MaxZ;
        memcpy(r.clip, clip, sizeof r.clip);
        memcpy(r.vp, &g_vsConst[8 * 4], sizeof r.vp);
        memcpy(r.model, &g_vsConst[58 * 4], sizeof r.model);
        if (path == OM_SKINNED) {
            r.pal = (int)(g_omPalCur.size() / kOmPalFloats);
            g_omPalCur.insert(g_omPalCur.end(), &g_vsConst[58 * 4], &g_vsConst[58 * 4] + kOmPalFloats);
        }
        uint64_t h = OmMix((uint64_t)(uintptr_t)r.decl, (uint64_t)(uintptr_t)r.vb);
        h = OmMix(h, ((uint64_t)r.vbOff << 32) | r.stride);
        h = OmMix(h, ((uint64_t)(uint32_t)r.type << 32) | (uint32_t)r.base);
        h = OmMix(h, ((uint64_t)r.minIdx << 32) | r.numVerts);
        h = OmMix(h, ((uint64_t)r.prims << 32) | ((uint32_t)r.vm << 16) | ((uint32_t)r.path << 8) | r.ibDyn);
        if (!r.ibDyn) h = OmMix(h, ((uint64_t)(uintptr_t)r.ib << 32) | r.startIdx);
        r.hash = h;
        r.vb->AddRef();
        r.ib->AddRef();
        if (st.recorded++ == 0) {  // prvni zaznam snimku: odted sledovat zapisove Locky
            g_omThread = GetCurrentThreadId();
            g_omWindow = true;
        }
    } while (false);
    st.recQpc += OmQpc() - t0;
}

// ---------------------------------------------------------------------------
// Kamera hlavniho pohledu: jen jeji zaznamy se prehraji a jdou do historie
// ---------------------------------------------------------------------------
// cViewProj zaznamu = kamera sceny. Normalne bitove shodne (G-buffer i barva sceny dostanou od enginu stejne c8..c11,
// krok0: 184/184 draw), tolerance jen na zaokrouhleni. Jiny pohled (odraz vody, monitor, 3D skybox) se lisi o rady vic.
static bool OmSameCam(const float* a, const float* b) {
    if (!memcmp(a, b, 16 * sizeof(float))) return true;
    for (int i = 0; i < 16; ++i)
        if (!Near(a[i], b[i], 1e-5f, (i & 3) == 3 ? 1e-3f : 1e-5f)) return false;  // sloupec 3 = posunuti
    return true;
}

// Brush kresli jen pres c4 (cModelViewProj); kamera z c8 plati jen kdyz c4 = c8 * c58..c60 (engine c8 nastavuje
// pri kazde zmene pohledu, krok0: 64/64 draw, rozdil 1e-7 relativne k soucinum). Tolerance 1e-4 ze souctu |soucinu|.
static bool OmBrushMvpOk(const OmRec& r) {
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) {
            const double t0 = (double)r.vp[i * 4 + 0] * r.model[0 + k], t1 = (double)r.vp[i * 4 + 1] * r.model[4 + k],
                         t2 = (double)r.vp[i * 4 + 2] * r.model[8 + k], t3 = k == 3 ? (double)r.vp[i * 4 + 3] : 0.0;
            const double sum = t0 + t1 + t2 + t3, mag = fabs(t0) + fabs(t1) + fabs(t2) + fabs(t3);
            if (fabs(sum - (double)r.clip[i * 4 + k]) > 1e-4 * mag + 1e-6) return false;
        }
    return true;
}

// Zaznamy jine kamery nez g_cam (nebo kdyz kamera sceny neni znama) pryc: z davky i z historie, protoze jejich c8
// by pristi snimek slouzilo parovanemu zaznamu jako minula kamera. Palety skinned zustavaji (indexy r.pal plati).
// Stejne tak zaznamy s jinym viewportem, nez je render snimku (cast B: davka kresli do sub-rectu renderu; jiny
// viewport by mel jine meritko pixelu) - ty se pocitaji do "vynechano: stav".
static void OmFilterCamera(OmStat& st) {
    size_t n = 0;
    for (size_t i = 0; i < g_omCur.size(); ++i) {
        const OmRec& r = g_omCur[i];
        const bool vpOk = r.vpW == g_renderW && r.vpH == g_renderH;
        // Zbran v ruce ma vlastni projekci (FOV): kamera se u ni neporovnava, jen musi byt znama kamera sceny
        const bool camOk = r.vm ? g_camSet : g_camSet && OmSameCam(r.vp, g_cam);
        if (!vpOk || !camOk || (r.path == OM_BRUSH && !OmBrushMvpOk(r))) {
            if (!vpOk) {
                ++st.skipState;
                if (st.recorded) --st.recorded;
            } else {
                ++st.otherCam;
            }
            if (r.vb) r.vb->Release();
            if (r.ib) r.ib->Release();
            continue;
        }
        if (n != i) g_omCur[n] = r;
        ++n;
    }
    g_omCur.resize(n);
}

// ---------------------------------------------------------------------------
// Parovani s minulym snimkem
// ---------------------------------------------------------------------------
static bool OmKeyEq(const OmRec& a, const OmRec& b) {
    return a.hash == b.hash && a.decl == b.decl && a.vb == b.vb && a.vbOff == b.vbOff && a.stride == b.stride &&
           a.type == b.type && a.base == b.base && a.minIdx == b.minIdx && a.numVerts == b.numVerts &&
           a.prims == b.prims && a.path == b.path && a.ibDyn == b.ibDyn && a.vm == b.vm &&
           (a.ibDyn || (a.ib == b.ib && a.startIdx == b.startIdx));
}

// Vzdalenost translaci (sloupec w radku c58..c60; u skinned kost 0)
static float OmDist2(const OmRec& a, const OmRec& b) {
    const float dx = a.model[3] - b.model[3], dy = a.model[7] - b.model[7], dz = a.model[11] - b.model[11];
    return dx * dx + dy * dy + dz * dz;
}

static bool OmHashLess(const std::pair<uint64_t, int>& a, const std::pair<uint64_t, int>& b) { return a.first < b.first; }

static void OmPair(OmStat& st) {
    static const float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const size_t nPrev = g_omPrev.size();
    if (!nPrev) {
        st.unpaired += (unsigned)g_omCur.size();
        return;
    }
    g_omPrevUsed.assign(nPrev, 0);
    g_omTmp.clear();
    // 1. pruchod: stejny klic a bitove shodna matice = objekt stoji. Staticke objekty si tak vezmou sve pary driv,
    // nez je muze sebrat pohyblivy objekt se stejnym klicem (instance tehoz modelu, plochy sveta v dynamickem IB).
    for (size_t i = 0; i < g_omCur.size(); ++i) {
        OmRec& r = g_omCur[i];
        const auto rg = std::equal_range(g_omPrevIdx.begin(), g_omPrevIdx.end(), std::make_pair(r.hash, 0), OmHashLess);
        bool any = false;
        for (auto it = rg.first; it != rg.second; ++it) {
            const int j = it->second;
            if (g_omPrevUsed[j]) continue;
            const OmRec& p = g_omPrev[j];
            if (!OmKeyEq(r, p)) continue;
            any = true;
            if (!memcmp(r.model, p.model, sizeof r.model)) {
                r.pair = j;
                g_omPrevUsed[j] = 1;
                break;
            }
        }
        if (r.pair >= 0) {
            ++st.paired;
            continue;
        }
        // Brush s jednotkovou matici je svet (nebo entita presne ve vychozi poloze): nikdy ho neparovat s pohnutym
        // zaznamem jine plochy - klic brushu v dynamickem IB je slaby (cely VB sveta, lisi se jen pocet primitiv)
        // a staticka plocha by dostala cizi MV. Entita, ktera prave dojela do vychozi polohy, prijde o MV v 1 snimku.
        if (!any || (r.path == OM_BRUSH && !memcmp(r.model, kIdentity, sizeof r.model))) {
            ++st.unpaired;
            continue;
        }
        g_omTmp.push_back(std::make_pair(r.hash, (int)i));
    }
    // 2. pruchod: zbytek (pohnute objekty) podle nejblizsi translace. V kazde skupine se stejnym klicem se bere
    // vzdy globalne nejblizsi dvojice, dokud je pod limitem teleportu.
    std::sort(g_omTmp.begin(), g_omTmp.end());
    const float lim2 = g_cfg.objMvTeleport * g_cfg.objMvTeleport;
    for (size_t a = 0; a < g_omTmp.size();) {
        size_t b = a;
        while (b < g_omTmp.size() && g_omTmp[b].first == g_omTmp[a].first) ++b;
        const auto rg = std::equal_range(g_omPrevIdx.begin(), g_omPrevIdx.end(), std::make_pair(g_omTmp[a].first, 0),
                                         OmHashLess);
        size_t left = b - a;
        for (int iter = 0; iter < 64 && left; ++iter) {
            float best = lim2;
            int bc = -1, bj = -1;
            for (size_t c = a; c < b; ++c) {
                if (g_omTmp[c].second < 0) continue;
                const OmRec& r = g_omCur[g_omTmp[c].second];
                for (auto it = rg.first; it != rg.second; ++it) {
                    const int j = it->second;
                    if (g_omPrevUsed[j] || !OmKeyEq(r, g_omPrev[j])) continue;
                    const float d2 = OmDist2(r, g_omPrev[j]);
                    if (d2 <= best) {
                        best = d2;
                        bc = (int)c;
                        bj = j;
                    }
                }
            }
            if (bc < 0) break;
            OmRec& r = g_omCur[g_omTmp[bc].second];
            r.pair = bj;
            r.flags |= OMF_MOVED;
            g_omPrevUsed[bj] = 1;
            g_omTmp[bc].second = -1;
            --left;
            ++st.paired;
            ++st.moved;
        }
        for (size_t c = a; c < b; ++c) {
            if (g_omTmp[c].second < 0) continue;
            ++st.unpaired;
            // kandidat se stejnym klicem zbyl, ale dal nez limit: teleport (nebo jiny objekt tehoz modelu)
            const OmRec& r = g_omCur[g_omTmp[c].second];
            for (auto it = rg.first; it != rg.second; ++it)
                if (!g_omPrevUsed[it->second] && OmKeyEq(r, g_omPrev[it->second])) {
                    ++st.teleport;
                    break;
                }
        }
        a = b;
    }
}

// ---------------------------------------------------------------------------
// Davka: prehrani vybranych zaznamu do OBJMV
// ---------------------------------------------------------------------------
// Prehrani g_omList do OBJMV (cil a DS uz nastavene): stav jako G-buffer hry, Clear, draw. Zapisove Locky se
// vyhodnocuji pred kazdym draw a po poslednim: Lock (z jineho vlakna) pred draw ho vyradi; Lock, ktery prisel az po
// kontrole uz prehraneho draw (data mohla byt nova), zahodi celou davku (*drop). Lock az po draw nevadi: DXVK data
// prijateho draw drzi (DISCARD prejmenuje slice, prepis bez DISCARD pocka na GPU; zarizeni MULTITHREADED = zamek).
static unsigned OmDrawList(IDirect3DDevice9* d, bool skinOk, OmStat& st, bool* drop) {
    // Render snimku: cast A cely cil, cast B sub-rect od (0, 0) - stejny viewport, do jakeho hra kreslila G-buffer.
    // Clear bez rectu maze jen viewport (D3D9), mimo sub-rect se OBJMV necte ani nekopiruje.
    const UINT rw = g_renderW && g_renderW <= g_omW ? g_renderW : g_omW;
    const UINT rh = g_renderH && g_renderH <= g_omH ? g_renderH : g_omH;
    const D3DVIEWPORT9 vp = {0, 0, rw, rh, 0.0f, 1.0f};
    o_SetViewport(d, &vp);
    // Jako G-buffer hry (krok0 kap. 6): hloubka LESSEQUAL se zapisem, bez blendu, alpha testu, sRGB a stencilu
    static const struct {
        D3DRENDERSTATETYPE st;
        DWORD v;
    } kRS[] = {{D3DRS_ZENABLE, D3DZB_TRUE},        {D3DRS_ZWRITEENABLE, TRUE},       {D3DRS_ZFUNC, D3DCMP_LESSEQUAL},
               {D3DRS_ALPHABLENDENABLE, FALSE},    {D3DRS_ALPHATESTENABLE, FALSE},   {D3DRS_SRGBWRITEENABLE, FALSE},
               {D3DRS_STENCILENABLE, FALSE},       {D3DRS_COLORWRITEENABLE, 0xF},    {D3DRS_FOGENABLE, FALSE},
               {D3DRS_SCISSORTESTENABLE, FALSE},   {D3DRS_CLIPPLANEENABLE, 0},       {D3DRS_FILLMODE, D3DFILL_SOLID},
               {D3DRS_DEPTHBIAS, 0},               {D3DRS_SLOPESCALEDEPTHBIAS, 0},   {D3DRS_CULLMODE, D3DCULL_CCW}};
    for (const auto& rs : kRS) o_SetRenderState(d, rs.st, rs.v);
    o_Clear(d, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);

    // PS: (0.5 * sirka, -0.5 * vyska, A, nejvetsi platne |MV|^2) v pixelech renderu; MV delsi nez pul sirky = neplatne
    float psC[4] = {0.5f * (float)rw, -0.5f * (float)rh, 1.0f, 0.25f * (float)rw * (float)rw};
    // Jitter v clip prostoru pro viewport davky - vyslovne z renderu, ne z posledni hodnoty g_jx / g_jy (ta patri
    // viewportu posledniho draw hry); stejny vzorec jako PreDraw, takze rasterizace sedi na G-buffer hry
    float jx = 0.0f, jy = 0.0f;
    JitterForViewport(rw, rh, &jx, &jy);
    d->SetPixelShader(g_omPS);
    d->SetPixelShaderConstantF(0, psC, 1);
    // c1: hloubka do B = MinZ + z/w * (MaxZ - MinZ) jako viewport hry (svet 0..1, zbran v ruce 0..0.1)
    float psZ[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    d->SetPixelShaderConstantF(1, psZ, 1);
    float curMinZ = 0.0f, curMaxZ = 1.0f;
    if (skinOk) {
        o_SetTexture(d, D3DVERTEXTEXTURESAMPLER0, g_omPalTex);
        static const struct {
            D3DSAMPLERSTATETYPE t;
            DWORD v;
        } kSS[] = {{D3DSAMP_MINFILTER, D3DTEXF_POINT},   {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
                   {D3DSAMP_MIPFILTER, D3DTEXF_NONE},    {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
                   {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP}, {D3DSAMP_SRGBTEXTURE, 0},
                   {D3DSAMP_MIPMAPLODBIAS, 0},           {D3DSAMP_MAXMIPLEVEL, 0}};
        for (const auto& ss : kSS) o_SetSamplerState(d, D3DVERTEXTEXTURESAMPLER0, ss.t, ss.v);
    }
    o_SetStreamSourceFreq(d, 0, 1);

    OmApplyInval();  // Locky od zacatku OmBatch az sem
    unsigned drawn = 0, drawnSkin = 0, drawnStatic = 0, drawnVm = 0;
    size_t k = 0;
    int lastPath = -1;
    IDirect3DVertexDeclaration9* lastDecl = nullptr;
    IDirect3DVertexBuffer9* lastVB = nullptr;
    IDirect3DIndexBuffer9* lastIB = nullptr;
    UINT lastOff = 0, lastStride = 0;
    DWORD lastCull = D3DCULL_CCW;
    for (int idx : g_omList) {
        OmRec& r = g_omCur[idx];
        const OmRec& p = g_omPrev[r.pair];
        const size_t row = r.path == OM_SKINNED ? k++ : 0;  // radek palety plati i pro vynechany draw (poradi nahrani)
        if (g_omInvalGen.load(std::memory_order_relaxed) != g_omInvalSeen) OmApplyInval();
        if (r.flags & OMF_DEAD) {  // Lock behem davky
            ++st.dead;
            continue;
        }
        if (r.path == OM_SKINNED && !skinOk) continue;
        // c0..c3 clip s jitterem (stejny vypocet jako PreDraw), c4..c7 bez jitteru, c8..c11 minuly snimek
        float c[19 * 4];
        memcpy(c, r.clip, 64);
        if (g_jitterActive) JitterMatrixBy(c, jx, jy);
        memcpy(c + 16, r.clip, 64);
        memcpy(c + 32, p.clip, 64);
        UINT nReg = 12;
        if (r.path == OM_RIGID) {
            memcpy(c + 48, r.model, 48);  // c12..c14
            memcpy(c + 60, p.model, 48);  // c15..c17
            nReg = 18;
        } else if (r.path == OM_SKINNED) {
            memset(c + 48, 0, 96);
            c[72] = 1.0f / (float)kOmPalW;  // c18
            c[73] = ((float)(2 * row) + 0.5f) / (float)kOmPalH;
            c[74] = ((float)(2 * row) + 1.5f) / (float)kOmPalH;
            c[75] = 0.0f;
            nReg = 19;
        }
        o_SetVSConstF(d, 0, c, nReg);
        if (r.path != lastPath) {
            o_SetVS(d, g_omVS[r.path]);
            lastPath = r.path;
        }
        IDirect3DVertexDeclaration9* decl = g_omDecls[r.declIdx].mine;
        if (decl != lastDecl) {
            o_SetVDecl(d, decl);
            lastDecl = decl;
        }
        if (r.vb != lastVB || r.vbOff != lastOff || r.stride != lastStride) {
            o_SetStreamSource(d, 0, r.vb, r.vbOff, r.stride);
            lastVB = r.vb;
            lastOff = r.vbOff;
            lastStride = r.stride;
        }
        if (r.ib != lastIB) {
            o_SetIndices(d, r.ib);
            lastIB = r.ib;
        }
        if (r.cull != lastCull) {  // cull jako hra (G-buffer: CCW)
            o_SetRenderState(d, D3DRS_CULLMODE, r.cull);
            lastCull = r.cull;
        }
        if (r.minZ != curMinZ || r.maxZ != curMaxZ) {  // zbran v ruce: Z rozsah viewportu jako hra
            curMinZ = r.minZ;
            curMaxZ = r.maxZ;
            const D3DVIEWPORT9 vz = {0, 0, rw, rh, curMinZ, curMaxZ};
            o_SetViewport(d, &vz);
            psZ[0] = curMaxZ - curMinZ;
            psZ[1] = curMinZ;
            d->SetPixelShaderConstantF(1, psZ, 1);
        }
        const float alpha = (r.flags & OMF_STATIC) ? 2.0f : 1.0f;
        if (alpha != psC[2]) {
            psC[2] = alpha;
            d->SetPixelShaderConstantF(0, psC, 1);
        }
        o_DrawIdxPrim(d, r.type, r.base, r.minIdx, r.numVerts, r.startIdx, r.prims);
        r.flags |= OMF_DRAWN;
        ++drawn;
        if (r.vm) ++drawnVm;
        if (r.path == OM_SKINNED) ++drawnSkin;
        if (r.flags & OMF_STATIC) ++drawnStatic;
    }
    // Lock mezi kontrolou a draw (jine vlakno): prehrany draw mohl vzit nova data -> OBJMV tento snimek nepouzit
    if (drawn && g_omInvalGen.load(std::memory_order_relaxed) != g_omInvalSeen) {
        OmApplyInval();
        for (int idx : g_omList)
            if ((g_omCur[idx].flags & (OMF_DRAWN | OMF_DEAD)) == (OMF_DRAWN | OMF_DEAD)) {
                ++st.dead;
                *drop = true;
            }
    }
    if (!*drop) {
        st.replaySkin += drawnSkin;
        st.replayStatic += drawnStatic;
        st.replayVm += drawnVm;
    }
    return drawn;
}

static unsigned OmDraw(IDirect3DDevice9* d, size_t nSkin, OmStat& st) {
    // Palety kosti (aktualni a minula) do textury, jednou za snimek. Poradi radku = poradi skinned v g_omList.
    bool skinOk = false;
    if (nSkin && g_omPalTex) {
        D3DLOCKED_RECT lr = {};
        const HRESULT hr = g_omPalTex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD);
        if (SUCCEEDED(hr) && lr.pBits && lr.Pitch >= (INT)(kOmPalFloats * 4)) {
            size_t k = 0;
            for (int idx : g_omList) {
                const OmRec& r = g_omCur[idx];
                if (r.path != OM_SKINNED) continue;
                uint8_t* row = (uint8_t*)lr.pBits + (2 * k) * (size_t)lr.Pitch;
                memcpy(row, &g_omPalCur[(size_t)r.pal * kOmPalFloats], kOmPalFloats * 4);
                memcpy(row + lr.Pitch, &g_omPalPrev[(size_t)g_omPrev[r.pair].pal * kOmPalFloats], kOmPalFloats * 4);
                ++k;
            }
            g_omPalTex->UnlockRect(0);
            skinOk = true;
        } else {
            if (SUCCEEDED(hr)) g_omPalTex->UnlockRect(0);
            static bool s_logged;
            if (!s_logged) {
                s_logged = true;
                Log("WARN: MV objektu: Lock textury palet selhal (0x%08X) - skinned objekty tento snimek bez MV",
                    (unsigned)hr);
            }
        }
    }

    // Stav hry: state block (vse krome cilu) + rucne RT0..3, DS, viewport, scissor
    g_omGuardThread = GetCurrentThreadId();
    IDirect3DStateBlock9* sb = nullptr;
    HRESULT hr = d->CreateStateBlock(D3DSBT_ALL, &sb);
    if (FAILED(hr) || !sb) {
        g_omGuardThread = 0;
        OmFail("CreateStateBlock(D3DSBT_ALL)", hr);
        return 0;
    }
    IDirect3DSurface9* oldRT[4] = {};
    IDirect3DSurface9* oldDS = nullptr;
    for (DWORD i = 0; i < 4; ++i)
        if (FAILED(d->GetRenderTarget(i, &oldRT[i]))) oldRT[i] = nullptr;
    if (FAILED(d->GetDepthStencilSurface(&oldDS))) oldDS = nullptr;
    if (!oldRT[0] && g_rt0) {  // Get* selhalo: stin proxy
        oldRT[0] = g_rt0;
        g_rt0->AddRef();
    }
    D3DVIEWPORT9 oldVP = {};
    const bool haveVP = SUCCEEDED(d->GetViewport(&oldVP));
    RECT oldSc = {};
    const bool haveSc = SUCCEEDED(d->GetScissorRect(&oldSc));

    for (DWORD i = 3; i >= 1; --i)
        if (oldRT[i]) o_SetRT(d, i, nullptr);
    // Cil i DS davky musi opravdu platit: jinak by RT0 nebo DS zustal hry a Clear by smazal jeji barvu nebo hloubku.
    // Pri chybe se davka (i Clear) preskoci, stav se vrati nize a snimek ma MV kamery.
    const HRESULT hrRT = o_SetRT(d, 0, g_omRT);
    const HRESULT hrDS = SUCCEEDED(hrRT) ? o_SetDS(d, g_omDS) : S_OK;
    unsigned drawn = 0;
    bool drop = false;
    if (SUCCEEDED(hrRT) && SUCCEEDED(hrDS)) {
        drawn = OmDrawList(d, skinOk, st, &drop);
    } else {
        drop = true;
        if (!g_omTargetLogged) {
            g_omTargetLogged = true;
            Log("WARN: MV objektu: %s pro davku selhalo (0x%08X) - davka preskocena bez Clear, snimek s MV kamery "
                "(dalsi vyskyty jen v radku vykon: zahozene davky)",
                FAILED(hrRT) ? "SetRenderTarget(0, OBJMV)" : "SetDepthStencilSurface(D24S8 OBJMV)",
                (unsigned)(FAILED(hrRT) ? hrRT : hrDS));
        }
    }

    // Zpet stav hry: cile, pak state block (viewport a scissor uz po SetRenderTarget), nakonec viewport a scissor
    // jeste rucne (kdyby je state block v teto verzi DXVK neobsahoval)
    o_SetRT(d, 0, oldRT[0]);
    for (DWORD i = 1; i < 4; ++i)
        if (oldRT[i]) o_SetRT(d, i, oldRT[i]);
    o_SetDS(d, oldDS);
    hr = sb->Apply();
    sb->Release();
    if (haveVP) o_SetViewport(d, &oldVP);
    if (haveSc) d->SetScissorRect(&oldSc);
    g_omGuardThread = 0;
    for (IDirect3DSurface9*& s : oldRT) OmSafeRelease(s);
    OmSafeRelease(oldDS);
    if (FAILED(hr)) {
        OmFail("obnova stavu hry (StateBlock::Apply)", hr);
        return 0;
    }
    if (g_capState == CAP_ACTIVE)
        Cap("# MV objektu: davka %u draw do OBJMV %p%s (zaznamu %u, state block volal hooky %ux)", drawn, (void*)g_omRT,
            drop ? " ZAHOZENA" : "", (unsigned)g_omCur.size(), g_omGuardHits);
    if (drop) {
        ++st.dropped;
        return 0;
    }
    return drawn;
}

// Konec sceny (TriggerSceneEnd, pred Interop_OnSceneEnd): sparovat, prehrat, zaznamy -> historie.
// Vraci pocet prehranych draw (0 = OBJMV se nekopiruje a helper ho ignoruje).
static unsigned OmBatch(IDirect3DDevice9* d) {
    if (!g_omActive || d != g_main) return 0;
    if (g_omDev && g_omDev != d) {  // pojistka: objekty stareho zarizeni (jinak je pousti HookDevice)
        OmReleaseDevice("jine zarizeni");
        return 0;
    }
    const LONGLONG t0 = OmQpc();
    OmStat& st = g_omF;
    ++st.frames;
    // Historie jen z bezprostredne predchoziho snimku hry (jinak by MV zahrnovalo vic snimku)
    if (!g_omPrevValid || g_omPrevFrame + 1 != g_frame) OmClearHistory();

    // Jen zaznamy kamery hlavniho pohledu (g_cam: SelectCamera tesne pred davkou); ostatni pryc i z historie
    OmFilterCamera(st);

    // Platnost bufferu: zapisove Locky po zaznamu. Okno sledovani zustava otevrene az do dokresleni davky (OmDraw
    // vyhodnocuje znovu pred kazdym draw a po poslednim); tady jen kvuli vyberu.
    OmApplyInval();

    OmPair(st);

    // Co prehrat: pohnute rigid objekty a skinned se zmenenou paletou; pri validaci i staticke (A = 2)
    g_omList.clear();
    size_t nSkin = 0;
    for (size_t i = 0; i < g_omCur.size(); ++i) {
        OmRec& r = g_omCur[i];
        if (r.flags & OMF_DEAD) {
            ++st.dead;
            continue;
        }
        if (r.pair < 0) continue;
        // Zbran v ruce vzdy: helper u ni MV kamery nepocita (dal by 0), a i se stejnou matici modelu se muze zmenit
        // jeji projekce (c8..c11, otoceni kamery)
        const bool moved = (r.flags & OMF_MOVED) != 0 || r.vm;
        bool stat = false;
        if (r.path == OM_SKINNED) {
            if (g_omSkinOff || nSkin >= kOmMaxSkin) continue;
            // Cela paleta bitove shodna s parem = objekt se urcite nehnul (postacujici podminka): MV kamery je presne,
            // prehrava se jen pri validaci. Registry nad poctem kosti meshe se meni i u stojicich objektu, kdyz je na
            // scene animovane NPC (krok0 kap. 5); pak se prehraje zbytecne, ale spravne.
            if (!moved && !memcmp(&g_omPalCur[(size_t)r.pal * kOmPalFloats],
                                  &g_omPalPrev[(size_t)g_omPrev[r.pair].pal * kOmPalFloats], kOmPalFloats * 4)) {
                if (!g_omValidate) continue;
                stat = true;
            }
            ++nSkin;
        } else if (!moved) {
            if (!g_omValidate) continue;  // stoji: MV kamery je presne
            stat = true;
        }
        if (stat) r.flags |= OMF_STATIC;
        g_omList.push_back((int)i);
    }

    // Nic k prehrani: bez state blocku, Clear i kopie OBJMV (objMvCount = 0, helper OBJMV necte)
    unsigned drawn = 0;
    if (!g_omList.empty() && OmEnsure(d, g_bbW, g_bbH)) drawn = OmDraw(d, nSkin, st);
    st.replayed += drawn;
    OmCloseWindow();  // dalsi Locky uz davku neovlivni

    // Zaznamy -> historie pro dalsi snimek (ukazatele uz jen jako identita)
    if (!g_omBroken) {
        for (OmRec& r : g_omCur) {
            if (r.vb) r.vb->Release();
            if (r.ib) r.ib->Release();
        }
        g_omPrev.swap(g_omCur);
        g_omPalPrev.swap(g_omPalCur);
        g_omCur.clear();
        g_omPalCur.clear();
        g_omPrevIdx.clear();
        for (size_t i = 0; i < g_omPrev.size(); ++i) g_omPrevIdx.push_back(std::make_pair(g_omPrev[i].hash, (int)i));
        std::sort(g_omPrevIdx.begin(), g_omPrevIdx.end());
        g_omPrevValid = true;
        g_omPrevFrame = g_frame;
    }

    const LONGLONG dt = OmQpc() - t0;
    st.batchQpc += dt;
    if (dt > st.batchMaxQpc) st.batchMaxQpc = dt;
    return drawn;
}

// Rezim pro helper (BmsdShared::objMvMode)
static uint32_t OmMode() {
    return g_omActive ? (uint32_t)(BMSD_OBJMV_ON | (g_omValidate ? BMSD_OBJMV_VALIDATE : 0) |
                                   (g_cfg.objMvViewmodel ? BMSD_OBJMV_VIEWMODEL : 0))
                      : (uint32_t)BMSD_OBJMV_OFF;
}

// Alt+Backspace / menu: MV objektu zapnuto/vypnuto (porovnani A/B). Jen z vlakna Present (OnFrameEnd).
static void OmSetEnabled(bool on, Src src) {
    g_omEnabled = on;
    SrcBeep(src, g_omEnabled ? MB_OK : MB_ICONHAND);
    SrcNotify(src, OSD_OBJMV);
    const uint32_t m = Interop_GetMode();
    Log("MV objektu: %s%s%s%s", g_omEnabled ? "ZAPNUTO" : "vypnuto", g_omValidate && g_omEnabled ? " (validace)" : "",
        SrcTag(src),
        g_omEnabled && g_omBroken ? " - zdroje ale selhaly (viz WARN), zkusi se znovu po Resetu zarizeni"
                                  : m == BMSD_MODE_DLAA || m == BMSD_MODE_MVDEBUG ? ""
                                                                                  : " - projevi se v rezimu DLAA");
}

// Konec snimku (OnFrameEnd): uklid, klavesa, stav pro dalsi snimek. modKey = modifikator klaves drzen a hra v popredi.
static void OmFrameEnd(bool modKey) {
    // Zaznamy bez davky (konec sceny v tomto snimku nenastal): pustit reference. Historie tim zastara sama.
    if (!g_omCur.empty()) OmReleaseRefs();
    OmCloseWindow();
    g_omSeq = 0;
    g_omLastVB = g_omLastIB = nullptr;

    OmStat& f = g_omF;
    OmStat& a = g_omI;
    a.frames += f.frames;
    a.seen += f.seen;
    a.recorded += f.recorded;
    a.otherCam += f.otherCam;
    a.paired += f.paired;
    a.moved += f.moved;
    a.unpaired += f.unpaired;
    a.teleport += f.teleport;
    a.dead += f.dead;
    a.replayed += f.replayed;
    a.replaySkin += f.replaySkin;
    a.replayStatic += f.replayStatic;
    a.replayVm += f.replayVm;
    a.dropped += f.dropped;
    a.skipVS += f.skipVS;
    a.skipState += f.skipState;
    a.skipDecl += f.skipDecl;
    a.skipDyn += f.skipDyn;
    a.skipSkin += f.skipSkin;
    a.skipFull += f.skipFull;
    a.recQpc += f.recQpc;
    a.batchQpc += f.batchQpc;
    if (f.batchMaxQpc > a.batchMaxQpc) a.batchMaxQpc = f.batchMaxQpc;
    f = OmStat();

    if (g_omGuardHits && !g_omGuardLogged) {
        g_omGuardLogged = true;
        Log("MV objektu: state block DXVK vola Set* pres hooky proxy (%u volani v prvni davce) - propousteji se beze "
            "zmeny stinu",
            g_omGuardHits);
    }

    // Alt+Backspace: MV objektu zapnuto/vypnuto (porovnani A/B)
    if (KeyEdge(modKey && KeyDown(DbgKey(g_cfg.objMvKey)), g_objMvKeyWas)) OmSetEnabled(!g_omEnabled, Src::KEY);

    // Zaznamenava se jen kdyz helper MV opravdu pouzije (DLAA bezi, nebo ladeni MV)
    const uint32_t mode = Interop_GetMode();
    const bool active = g_omEnabled && !g_omBroken &&
                        (mode == BMSD_MODE_MVDEBUG || (mode == BMSD_MODE_DLAA && Interop_DlaaReady()));
    if (!active && g_omActive) {
        OmClearHistory();
        // VRAM jen kdyz funkce bezi: cil OBJMV (59 MB pri 1440p), jeho D24S8 a textura palet se pri dalsim zapnuti
        // vytvori znovu (OmEnsure). Slot mostu drzi na OBJMV vlastni referenci, dokud jeho kopie nedobehne.
        if (g_omRT || g_omPalTex) Log("MV objektu: cile uvolneny (funkce neaktivni)");
        OmReleaseTargets();
        OmSafeRelease(g_omPalTex);
    }
    g_omActive = active;
}

// Cast radku "vykon" (interop.cpp, kazdych ~5 s): prumery na snimek s koncem sceny a CPU casy; pak vynuluje.
void ProxyObjMvPerf(char* buf, size_t n) {
    if (!n) return;
    OmStat& s = g_omI;
    if (!g_omEnabled) {
        snprintf(buf, n, "MV objektu vypnute");
    } else if (!s.frames) {
        snprintf(buf, n, "MV objektu: zadny snimek%s", g_omBroken ? " (zdroje selhaly, viz WARN)" : "");
    } else {
        if (!g_omQpf.QuadPart) QueryPerformanceFrequency(&g_omQpf);
        const double f = (double)s.frames, ms = 1000.0 / (double)g_omQpf.QuadPart;
        snprintf(buf, n,
                 "MV objektu%s: DIP v G-bufferu %.0f, zaznam %.1f (z toho jina kamera %.2f), par %.1f (pohnute %.2f, "
                 "bez paru %.2f, z toho teleport %.2f), prehrano %.1f (skinned %.1f, staticke %.1f, zbran %.1f), zneplatneno "
                 "Lockem %.2f, zahozene davky %u, vynechano: VS %.0f, stav %.0f, deklarace %.1f, dyn. VB %.1f, skinned "
                 "%.1f, plno %.1f; CPU zaznam %.3f ms, davka prum %.3f / max %.2f ms",
                 g_omValidate ? " (validace)" : "", s.seen / f, s.recorded / f, s.otherCam / f, s.paired / f,
                 s.moved / f, s.unpaired / f, s.teleport / f, s.replayed / f, s.replaySkin / f, s.replayStatic / f,
                 s.replayVm / f,
                 s.dead / f, s.dropped, s.skipVS / f, s.skipState / f, s.skipDecl / f, s.skipDyn / f, s.skipSkin / f,
                 s.skipFull / f, (double)s.recQpc * ms / f, (double)s.batchQpc * ms / f, (double)s.batchMaxQpc * ms);
    }
    s = OmStat();
}
