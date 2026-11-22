// bms-dlss: motion vektory pohyblivych objektu - shadery davkoveho replaye v proxy (D3D9, vs_3_0 / ps_3_0).
//
// Proxy si behem G-bufferu hlavniho pohledu zaznamena draw cally objektu a na konci sceny je prehraje do vlastniho
// cile OBJMV (A32B32G32R32F, velikost sceny) s vlastnim depth bufferem (src/objmv.inl). Vystup pixelu:
//   R, G = motion vektor v pixelech vstupu: pozice v minulem snimku - pozice v tomto snimku, oboji bez jitteru,
//          +x doprava, +y dolu (stejna konvence jako helper/mv.comp; DLSS: aktualni + MV = minuly)
//   B    = hloubka fragmentu MinZ + z/w * (MaxZ - MinZ) (jitterovany clip, stejna skala jako D24 hry; zbran v ruce
//          ma viewport Z 0..0.1)
//   A    = 1 pohyblivy objekt, 2 staticky objekt (jen rezim validace), 0 = neplatne
//
// Pozice vrcholu presne podle disassembly hernich VS (docs/mv-objekty-krok0.md, kap. 3 a 9); flex se ignoruje.
// Pozice FLOAT3 se v deklaraci doplni na w = 1; u FLOAT4 bere rigid i skinned w z dat jako hra, brush vzdy w = 1.
// Matice jsou po radcich (registry): v' = (dot(v, R0), dot(v, R1), dot(v, R2), dot(v, R3)).
// Aktualni i minula pozice se pocitaji stejnymi instrukcemi ze stejnych vstupu, takze nehybny objekt pri nehybne
// kamere ma MV nulove (az na zaokrouhleni v poslednim bitu).
//
// Preklad: build.ps1 (fxc z Windows SDK -> build\gen\objmv_*.h).

// VS konstanty: nahrava proxy pro kazdy draw. Behem davky jsou vsechny registry nase, stav hry vraci state block.
float4 cClipJit[4]   : register(c0);   // clip matice s jitterem (rasterizace jako hra); brush: MVP (c4), jinak VP (c8)
float4 cClipCur[4]   : register(c4);   // totez bez jitteru
float4 cClipPrev[4]  : register(c8);   // clip matice paru z minuleho snimku, bez jitteru
float4 cModelCur[3]  : register(c12);  // rigid: matice modelu (c58..c60 hry) tohoto snimku
float4 cModelPrev[3] : register(c15);  // rigid: matice modelu paru z minuleho snimku
float4 cPal          : register(c18);  // skinned: (1 / sirka textury palet, v aktualni palety, v minule palety, 0)

// D3DVERTEXTEXTURESAMPLER0: palety kosti, radek = paleta, texel x = registr c58+x hry (x, y, z, w), filtr POINT
sampler2D sPal : register(s0);

// PS konstanta: (0.5 * sirka, -0.5 * vyska, hodnota A, nejvetsi platne |MV|^2 v pixelech)
float4 cOut : register(c0);
// (MaxZ - MinZ, MinZ, 0, 0) viewportu: hloubka v B jako v depth bufferu hry (svet 0..1, zbran v ruce 0..0.1)
float4 cZ : register(c1);

struct VSOut {
    float4 pos  : POSITION;
    float3 cur  : TEXCOORD0;  // clip tohoto snimku bez jitteru (x, y, w)
    float3 prev : TEXCOORD1;  // clip minuleho snimku bez jitteru (x, y, w)
    float2 zw   : TEXCOORD2;  // z, w jitterovaneho clipu (hloubka; v ps_3_0 neni primo k dispozici)
};

VSOut Project(float4 wCur, float4 wPrev)
{
    VSOut o;
    o.pos = float4(dot(wCur, cClipJit[0]), dot(wCur, cClipJit[1]), dot(wCur, cClipJit[2]), dot(wCur, cClipJit[3]));
    o.cur = float3(dot(wCur, cClipCur[0]), dot(wCur, cClipCur[1]), dot(wCur, cClipCur[3]));
    o.prev = float3(dot(wPrev, cClipPrev[0]), dot(wPrev, cClipPrev[1]), dot(wPrev, cClipPrev[3]));
    o.zw = o.pos.zw;
    return o;
}

// ---- brush (3C93C010): oPos = (pos, 1) * cModelViewProj ----
VSOut VSBrush(float4 pos : POSITION)
{
    float4 P = float4(pos.xyz, 1.0);
    return Project(P, P);
}

// ---- rigid model (2D489148): P = v0 (w z dat), world = P * cModel, oPos = world * cViewProj ----
VSOut VSRigid(float4 pos : POSITION)
{
    float4 P = pos;
    float4 wc = float4(dot(P, cModelCur[0]), dot(P, cModelCur[1]), dot(P, cModelCur[2]), 1.0);
    float4 wp = float4(dot(P, cModelPrev[0]), dot(P, cModelPrev[1]), dot(P, cModelPrev[2]), 1.0);
    return Project(wc, wp);
}

// ---- skinned (2D488658): 3 kosti, nejdriv se smichaji radky matic, pak se nasobi pozice ----
float4 PalRow(float reg, float v)
{
    return tex2Dlod(sPal, float4((reg + 0.5) * cPal.x, v, 0.0, 0.0));  // stred texelu
}

float4 SkinWorld(float4 P, float3 a, float3 w, float v)
{
    // poradi jako hra: R = w1 * c[a.y]; R = c[a.x] * w0 + R; R = c[a.z] * w2 + R
    float4 r0 = w.y * PalRow(a.y, v);
    r0 = PalRow(a.x, v) * w.x + r0;
    r0 = PalRow(a.z, v) * w.z + r0;
    float4 r1 = w.y * PalRow(a.y + 1.0, v);
    r1 = PalRow(a.x + 1.0, v) * w.x + r1;
    r1 = PalRow(a.z + 1.0, v) * w.z + r1;
    float4 r2 = w.y * PalRow(a.y + 2.0, v);
    r2 = PalRow(a.x + 2.0, v) * w.x + r2;
    r2 = PalRow(a.z + 2.0, v) * w.z + r2;
    return float4(dot(P, r0), dot(P, r1), dot(P, r2), 1.0);
}

VSOut VSSkinned(float4 pos : POSITION,
                float4 bw  : BLENDWEIGHT,    // SHORT2 -> (s0, s1, 0, 1), nenormalizovane
                float4 bi  : BLENDINDICES)   // D3DCOLOR -> (R, G, B, A) v 0..1; .zyx = bajty 0, 1, 2 vrcholu
{
    float4 P = pos;                                        // hra: P.w = v0.w
    float2 w01 = (bw.xy + 1.0) * 3.05175781e-5;            // (s + 1) / 32768
    float3 w = float3(w01.x, w01.y, 1.0 - (w01.y + w01.x));
    float3 a = floor(bi.zyx * 765.005859 + 0.5);           // 3 * index kosti (hra: mova = zaokrouhleni)
    return Project(SkinWorld(P, a, w, cPal.y), SkinWorld(P, a, w, cPal.z));
}

// ---- pixel shader ----
float4 PSMain(float3 cur : TEXCOORD0, float3 prev : TEXCOORD1, float2 zw : TEXCOORD2) : COLOR0
{
    // NDC -> pixely: x = (ndc.x + 1) * W / 2, y = (1 - ndc.y) * H / 2; v rozdilu zbyde jen meritko
    float2 mv = (prev.xy / prev.z - cur.xy / cur.z) * cOut.xy;
    // minula pozice za kamerou nebo nesmyslne velky skok -> neplatne (helper pouzije MV kamery)
    float a = (prev.z > 1e-6 && dot(mv, mv) <= cOut.w) ? cOut.z : 0.0;
    return float4(mv, cZ.y + (zw.x / zw.y) * cZ.x, a);
}
