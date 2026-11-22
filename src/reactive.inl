// bms-dlss: maska reaktivity (BMSD_VERSION 14), cast proxy. Vklada se do proxy.cpp (#include) a pouziva jeho stinovy
// stav (g_rt0, g_sceneRT, g_sceneDS, g_rs*, g_vp) a originalni funkce zarizeni (o_*).
//
// DLSS a FSR skladaji obraz z historie podle MV. Castice, kour, ohen, jiskry, zablesky vystrelu a sprity ale nemaji
// vlastni MV (kresli se pres neco jineho, co se hybe jinak) -> za nimi zustava stopa (ghosting). Proxy proto behem
// sceny oznaci pixely, kam se kresli pruhledne a aditivni draw, do vlastniho cile MASKA (A8R8G8B8 velikosti sceny):
//   RGB = soucet pridane barvy aditivnich draw (DESTBLEND = ONE: ohen, jiskry, zablesky, zare),
//   A   = pokryti pruhlednych draw (DESTBLEND = INVSRCALPHA: kour, prach, sklo): a' = a + a_dst * (1 - a).
// Helper z ni udela jednokanalovou masku (max(RGB, A) * [upscale] ReactiveScale) a preda ji DLSS (Bias Current Color
// Mask) a FSR (reactive).
//
// Jak: tesne PRED kazdym draw do barvy sceny (pred koncem sceny, perspektivni matice, zapis barvy) se tentyz draw
// zopakuje s cilem MASKA - stejny VS, PS, textury, konstanty, hloubka hry (D24S8 sceny, test hloubky beze zmeny, bez
// zapisu), jen jiny blend:
//   ADD   (aditivni):   blend hry, zapis jen RGB,
//   ALPHA (pruhledny):  zapis jen A, SRCBLENDALPHA = ONE, DESTBLENDALPHA = INVSRCALPHA (pokryti),
//   CLEAR (nepruhledny draw po prvnim pruhlednem): blend ZERO / ZERO = vynulovat - neco nepruhledneho (zbran v ruce,
//         entita razena mezi pruhlednymi) zakrylo pruhledne, co bylo pod nim. Pred prvnim pruhlednym draw snimku se
//         nepruhledne draw neopakuji (maska je tam stejne nulova).
// Vynechava se: decaly a overlaye (DEPTHBIAS / SLOPESCALEDEPTHBIAS != 0 - staticke na stenach, tem historie pomaha),
// modulate (DESTCOLOR x SRCCOLOR - stiny, decaly), jiny blend a draw s MRT. Replay pred puvodnim draw: hloubka je jeste
// pred jeho zapisem, takze test hloubky dopadne stejne jako u hry.
// Mazani hloubky sceny (zacatek 3D skyboxu i sveta) masku "nabije": prvni replay po nem cil smaze. Pruhledne veci
// skyboxu, ktere svet zakryl, tak v masce nezustanou.
// Cena: jedno prepnuti cile + ~10 stavu a draw navic na kazdy pruhledny draw (a nepruhledny po nem). V casti B se
// kresli do sub-rectu (viewport hry), most kopiruje jen render.

static IDirect3DTexture9* g_rmTex;
static IDirect3DSurface9* g_rmRT;
static UINT g_rmW, g_rmH;
static bool& g_rmOn = g_cfg.reactiveMask;  // [upscale] ReactiveMask (za behu jen pres ini)
static bool g_rmBroken;       // vytvoreni cile selhalo -> do Resetu nic
static bool g_rmArmed;        // tento snimek: hloubka sceny smazana, replay povolen
static bool g_rmCleared;      // od nabiti uz maska smazana (prvni replay)
static bool g_rmDirty;        // od smazani uz byl pruhledny replay (nepruhledne draw ji pak nuluji)
static bool g_rmPersp;        // posledni PreDraw: perspektivni draw hlavniho pohledu
static unsigned g_rmDraws;    // pruhlednych replayu od posledniho smazani masky (0 = maska se neposila)
static bool g_rmLoggedArm, g_rmLoggedFirst, g_rmLoggedGlass;
static IDirect3DSurface9* g_rmLastDS;  // depth-stencil sceny minuleho snimku (jen identita; g_sceneDS se nuluje v Present)
// Statistika intervalu radku "vykon" (RmPerf)
static struct {
    unsigned frames, masked, add, alpha, clear, decal, modulate, mrt, fail, glass;
    uint64_t qpc;
} g_rmSt;

static void RmReleaseTargets() {
    if (g_rmRT) g_rmRT->Release();
    if (g_rmTex) g_rmTex->Release();
    g_rmRT = nullptr;
    g_rmTex = nullptr;
    g_rmW = g_rmH = 0;
    g_rmArmed = g_rmCleared = g_rmDirty = false;
    g_rmDraws = 0;
}

static bool RmEnsure(IDirect3DDevice9* d, UINT w, UINT h) {
    if (g_rmBroken || !w || !h) return false;
    if (g_rmRT && (g_rmW != w || g_rmH != h)) RmReleaseTargets();
    if (g_rmRT) return true;
    HRESULT hr = d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_rmTex, nullptr);
    if (SUCCEEDED(hr) && g_rmTex) hr = g_rmTex->GetSurfaceLevel(0, &g_rmRT);
    if (FAILED(hr) || !g_rmRT) {
        RmReleaseTargets();
        g_rmBroken = true;
        Log("WARN: maska reaktivity: vytvoreni cile %ux%u A8R8G8B8 selhalo (0x%08X) - maska vypnuta do Resetu", w, h,
            (unsigned)hr);
        return false;
    }
    g_rmW = w;
    g_rmH = h;
    Log("maska reaktivity: cil %ux%u A8R8G8B8 vytvoren", w, h);
    return true;
}

// Clear hloubky sceny pred koncem sceny (H_Clear): maska znovu od nuly (3D skybox, svet)
static inline void RmOnZClear(IDirect3DDevice9* d) {
    IDirect3DSurface9* sds = g_sceneDS ? g_sceneDS : g_rmLastDS;
    if (!g_rmOn || d != g_main || g_sceneEndDone || !sds || g_ds != sds) return;
    if (Interop_GetMode() != BMSD_MODE_DLAA) return;
    {  // hloubka sceny s MSAA ([msaa] StripMsaa=0): cil masky je bez MSAA a s ni nejde pouzit (most MSAA stejne nebere)
        D3DSURFACE_DESC dd = {};
        if (FAILED(sds->GetDesc(&dd)) || dd.MultiSampleType != D3DMULTISAMPLE_NONE) {
            static bool s_logged;
            if (!s_logged) {
                s_logged = true;
                Log("maska reaktivity: hloubka sceny s MSAA (%d) - maska se nekresli", (int)dd.MultiSampleType);
            }
            return;
        }
    }
    g_rmArmed = true;
    g_rmCleared = g_rmDirty = false;
    g_rmDraws = 0;
    if (!g_rmLoggedArm) {
        g_rmLoggedArm = true;
        Log("maska reaktivity: prvni nabiti (mazani hloubky sceny %p, draw %u snimku)", (void*)sds, g_drawSeq);
    }
}

enum { RM_NONE = 0, RM_ADD = 1, RM_ALPHA = 2, RM_CLEAR = 3 };

static inline DWORD RmGetRS(IDirect3DDevice9* d, D3DRENDERSTATETYPE st) {
    DWORD v = 0;
    d->GetRenderState(st, &v);
    return v;
}

// Co s draw udelat (volat jen pro hlavni zarizeni po PreDraw, ktery draw pustil dal)
static inline int RmClassify(IDirect3DDevice9* d) {
    if (!g_rmArmed || d != g_main || OmGuarded() || g_sceneEndDone || !g_rmPersp || !g_sceneRT || g_rt0 != g_sceneRT ||
        !(g_rsColorWrite & 7))
        return RM_NONE;
    // Otevreny okluzni dotaz hry: replay do masky by mu pricetl vzorky podruhe
    if (g_nOcclOpen > 0) return RM_NONE;
    if (!g_rsAlphaBlend) return g_rmDirty ? RM_CLEAR : RM_NONE;
    // Jen bezne michani (ADD); MIN / MAX / (REV)SUBTRACT nejsou pruhlednost ani aditivni svetlo
    if (RmGetRS(d, D3DRS_BLENDOP) != D3DBLENDOP_ADD) {
        ++g_rmSt.modulate;
        return RM_NONE;
    }
    if (RmGetRS(d, D3DRS_DEPTHBIAS) || RmGetRS(d, D3DRS_SLOPESCALEDEPTHBIAS)) {
        ++g_rmSt.decal;
        return RM_NONE;
    }
    const DWORD sb = RmGetRS(d, D3DRS_SRCBLEND), db = RmGetRS(d, D3DRS_DESTBLEND);
    if (db == D3DBLEND_ONE && sb != D3DBLEND_ZERO) return RM_ADD;
    if (db == D3DBLEND_INVSRCALPHA && (sb == D3DBLEND_SRCALPHA || sb == D3DBLEND_ONE)) {
        // Pruhledne castice (kour, prach, ohen) maji VS s cProj nebo ViewportTransformScaled (spritecard). Ostatni
        // pruhledne draw (sklo oken a aut, pruhledne modely) stoji ve svete a historie jim pomaha: jen s
        // [upscale] ReactiveGlass=1.
        bool particle = g_curInfo && g_curInfo->vpScaledReg >= 0;
        for (uint8_t i = 0; g_curInfo && i < g_curInfo->nClip && !particle; ++i)
            particle = g_curInfo->clipKind[i] == CK_PROJ;
        if (!particle && !g_cfg.reactiveGlass) {
            ++g_rmSt.glass;
            if (!g_rmLoggedGlass && g_curInfo) {
                g_rmLoggedGlass = true;
                Log("maska reaktivity: pruhledny draw bez castic (sklo apod.) se vynechava, napr. VS %s",
                    g_curInfo->text.c_str());
            }
            return RM_NONE;
        }
        return RM_ALPHA;
    }
    ++g_rmSt.modulate;
    return RM_NONE;
}

// Prepnout zarizeni na cil MASKA (a po draw zpet). Vraci false = replay se nedela (MRT, chyba).
struct RmSaved {
    D3DVIEWPORT9 vp;
    RECT sc;
    DWORD cw, zw, ab, sb, db, bop, sep, sba, dba, bopa, sp, sf, szf, ccwp, ccwf, ccwzf;
};

static bool RmBegin(IDirect3DDevice9* d, int kind, RmSaved& s) {
    IDirect3DSurface9* rt1 = nullptr;
    if (SUCCEEDED(d->GetRenderTarget(1, &rt1)) && rt1) {
        rt1->Release();
        ++g_rmSt.mrt;
        return false;
    }
    if (!RmEnsure(d, g_sceneW, g_sceneH)) return false;
    if (FAILED(d->GetViewport(&s.vp))) return false;
    d->GetScissorRect(&s.sc);
    s.cw = RmGetRS(d, D3DRS_COLORWRITEENABLE);
    s.zw = RmGetRS(d, D3DRS_ZWRITEENABLE);
    s.ab = RmGetRS(d, D3DRS_ALPHABLENDENABLE);
    s.sb = RmGetRS(d, D3DRS_SRCBLEND);
    s.db = RmGetRS(d, D3DRS_DESTBLEND);
    s.bop = RmGetRS(d, D3DRS_BLENDOP);
    s.sep = RmGetRS(d, D3DRS_SEPARATEALPHABLENDENABLE);
    s.sba = RmGetRS(d, D3DRS_SRCBLENDALPHA);
    s.dba = RmGetRS(d, D3DRS_DESTBLENDALPHA);
    s.bopa = RmGetRS(d, D3DRS_BLENDOPALPHA);
    s.sp = RmGetRS(d, D3DRS_STENCILPASS);
    s.sf = RmGetRS(d, D3DRS_STENCILFAIL);
    s.szf = RmGetRS(d, D3DRS_STENCILZFAIL);
    s.ccwp = RmGetRS(d, D3DRS_CCW_STENCILPASS);
    s.ccwf = RmGetRS(d, D3DRS_CCW_STENCILFAIL);
    s.ccwzf = RmGetRS(d, D3DRS_CCW_STENCILZFAIL);
    if (FAILED(o_SetRT(d, 0, g_rmRT))) {
        ++g_rmSt.fail;
        o_SetRT(d, 0, g_sceneRT);
        o_SetViewport(d, &s.vp);
        d->SetScissorRect(&s.sc);
        return false;
    }
    if (!g_rmCleared) {
        // SetRenderTarget nastavil viewport na cely cil -> Clear smaze celou masku
        o_Clear(d, 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        g_rmCleared = true;
    }
    o_SetViewport(d, &s.vp);  // SetRenderTarget(0) viewport i scissor resetoval
    d->SetScissorRect(&s.sc);
    o_SetRenderState(d, D3DRS_ZWRITEENABLE, FALSE);
    o_SetRenderState(d, D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    o_SetRenderState(d, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    o_SetRenderState(d, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    o_SetRenderState(d, D3DRS_CCW_STENCILPASS, D3DSTENCILOP_KEEP);
    o_SetRenderState(d, D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
    o_SetRenderState(d, D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_KEEP);
    if (kind == RM_ADD) {
        o_SetRenderState(d, D3DRS_COLORWRITEENABLE, s.cw & 7);
    } else if (kind == RM_ALPHA) {
        o_SetRenderState(d, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA);
        o_SetRenderState(d, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        o_SetRenderState(d, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        o_SetRenderState(d, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        o_SetRenderState(d, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    } else {  // RM_CLEAR
        o_SetRenderState(d, D3DRS_COLORWRITEENABLE, 0xF);
        o_SetRenderState(d, D3DRS_ALPHABLENDENABLE, TRUE);
        o_SetRenderState(d, D3DRS_SRCBLEND, D3DBLEND_ZERO);
        o_SetRenderState(d, D3DRS_DESTBLEND, D3DBLEND_ZERO);
        o_SetRenderState(d, D3DRS_BLENDOP, D3DBLENDOP_ADD);  // ZERO/ZERO mazou jen s ADD (MIN / MAX faktory ignoruji)
        o_SetRenderState(d, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    }
    return true;
}

static void RmEnd(IDirect3DDevice9* d, int kind, const RmSaved& s) {
    o_SetRT(d, 0, g_sceneRT);
    o_SetViewport(d, &s.vp);
    d->SetScissorRect(&s.sc);
    o_SetRenderState(d, D3DRS_COLORWRITEENABLE, s.cw);
    o_SetRenderState(d, D3DRS_ZWRITEENABLE, s.zw);
    o_SetRenderState(d, D3DRS_STENCILPASS, s.sp);
    o_SetRenderState(d, D3DRS_STENCILFAIL, s.sf);
    o_SetRenderState(d, D3DRS_STENCILZFAIL, s.szf);
    o_SetRenderState(d, D3DRS_CCW_STENCILPASS, s.ccwp);
    o_SetRenderState(d, D3DRS_CCW_STENCILFAIL, s.ccwf);
    o_SetRenderState(d, D3DRS_CCW_STENCILZFAIL, s.ccwzf);
    if (kind == RM_ALPHA) {
        o_SetRenderState(d, D3DRS_SEPARATEALPHABLENDENABLE, s.sep);
        o_SetRenderState(d, D3DRS_SRCBLENDALPHA, s.sba);
        o_SetRenderState(d, D3DRS_DESTBLENDALPHA, s.dba);
        o_SetRenderState(d, D3DRS_BLENDOPALPHA, s.bopa);
    } else if (kind == RM_CLEAR) {
        o_SetRenderState(d, D3DRS_ALPHABLENDENABLE, s.ab);
        o_SetRenderState(d, D3DRS_SRCBLEND, s.sb);
        o_SetRenderState(d, D3DRS_DESTBLEND, s.db);
        o_SetRenderState(d, D3DRS_BLENDOP, s.bop);
        o_SetRenderState(d, D3DRS_SEPARATEALPHABLENDENABLE, s.sep);
    }
}

// Replay draw do masky (volat z H_Draw* po PreDraw, pred puvodnim draw). draw = volani originalni funkce draw.
template <class F>
static inline void RmReplay(IDirect3DDevice9* d, F draw) {
    if (!g_rmArmed) return;  // rychla cesta: maska vypnuta / snimek bez sceny
    const int kind = RmClassify(d);
    if (kind == RM_NONE) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    RmSaved s;
    if (RmBegin(d, kind, s)) {
        draw();
        RmEnd(d, kind, s);
        if (kind == RM_CLEAR) {
            ++g_rmSt.clear;
        } else {
            g_rmDirty = true;
            ++g_rmDraws;
            ++(kind == RM_ADD ? g_rmSt.add : g_rmSt.alpha);
            if (!g_rmLoggedFirst) {
                g_rmLoggedFirst = true;
                Log("maska reaktivity: prvni %s draw do masky (cil %p %ux%u, viewport %u,%u %ux%u)",
                    kind == RM_ADD ? "aditivni" : "pruhledny", (void*)g_rmRT, g_rmW, g_rmH, (unsigned)s.vp.X,
                    (unsigned)s.vp.Y, (unsigned)s.vp.Width, (unsigned)s.vp.Height);
            }
        }
    }
    QueryPerformanceCounter(&t1);
    g_rmSt.qpc += (uint64_t)(t1.QuadPart - t0.QuadPart);
}

// Most (SceneBridge): cil masky pro tento snimek, nullptr = bez masky
static IDirect3DSurface9* RmForBridge(uint32_t* draws) {
    *draws = 0;
    if (!g_rmOn || !g_rmRT || !g_rmCleared || !g_rmDraws || g_rmW != g_sceneW || g_rmH != g_sceneH) return nullptr;
    *draws = g_rmDraws;
    ++g_rmSt.masked;
    return g_rmRT;
}

static void RmFrameEnd() {
    if (g_rmOn) ++g_rmSt.frames;
    if (g_sceneDS) g_rmLastDS = g_sceneDS;
    g_rmArmed = g_rmCleared = g_rmDirty = false;
    g_rmDraws = 0;
    g_rmPersp = false;
}

static void RmBeforeReset() {
    RmReleaseTargets();
    g_rmLastDS = nullptr;
    g_rmBroken = false;
}

// Cast radku "vykon" (interop.cpp): prazdne pri vypnute masce; volani pocitadla vynuluje
void ProxyReactivePerf(char* buf, size_t n) {
    if (!n) return;
    buf[0] = 0;
    if (g_rmOn && g_rmSt.frames) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        const double fr = (double)g_rmSt.frames;
        snprintf(buf, n,
                 " | maska: %.0f %% snimku, na snimek adit %.1f pruhl %.1f nulovani %.1f (vynech. decal %.1f sklo %.1f "
                 "blend %.1f MRT %u chyb %u), CPU %.3f ms",
                 100.0 * g_rmSt.masked / fr, g_rmSt.add / fr, g_rmSt.alpha / fr, g_rmSt.clear / fr, g_rmSt.decal / fr,
                 g_rmSt.glass / fr, g_rmSt.modulate / fr, g_rmSt.mrt, g_rmSt.fail,
                 1000.0 * (double)g_rmSt.qpc / (double)f.QuadPart / fr);
    }
    memset(&g_rmSt, 0, sizeof g_rmSt);
}
