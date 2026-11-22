# bms-dlss 1.0.0 – DLSS, DLAA, FSR and Frame Generation for Black Mesa

Unofficial mod for **Black Mesa** (Steam, Windows). Replaces the game's anti-aliasing with **NVIDIA DLSS / DLAA** or
**AMD FSR** and adds **frame generation** (NVIDIA DLSS Frame Generation or FSR) – a much cleaner image on fences,
wires and grates, and smooth high frame rates. Made by **Crashdown**. Česky níže.

## Features
- **DLAA** (DLSS at native resolution) and **DLSS supersampling** (renders larger and scales down, like DLDSR – the
  cleanest image on fences). DLSS presets K, L, M.
- **AMD FSR** upscaling for any card with Vulkan (native AA).
- **Frame Generation:**
  - **DLSS-G** (NVIDIA RTX 40 and newer): 2×, on RTX 50 up to **6×**.
  - **FSR** (any card with Vulkan, incl. RTX 20/30, AMD, Intel): 2× (AMD FSR FG) and **3×–5×** (the mod's own
    generator based on FSR 3.1.4).
  - **FSR adaptive:** generates only as many frames as are missing to reach the target.
  - **Auto multiplier:** picks the lowest multiplier that reaches the output frame rate – real frames have priority.
  - Output FPS cap (default: monitor refresh rate minus a small margin, works with G-SYNC / FreeSync).
- Motion vectors for moving objects and for the weapon in hand (fewer ghosting trails).
- Latency reduction similar to NVIDIA Reflex when the GPU is the bottleneck.
- In-game menu (**Alt+N**), Czech and English, settings are saved to `bms_dlss.ini`.

## Requirements
- Black Mesa from Steam, current Windows version (the game folder must contain
  `bin\thirdparty\dxvk-windows-x86\d3d9.dll` – it ships with the game).
- Graphics card with Vulkan. DLSS / DLAA: NVIDIA GeForce RTX. DLSS Frame Generation: RTX 40 or newer
  (more than 2×: RTX 50). FSR upscaling and FSR frame generation: any card with Vulkan.
- Windows 10/11, hardware-accelerated GPU scheduling on (Windows Settings > Display > Graphics) for DLSS-G.
- In the game options: **anti-aliasing (MSAA) off** – the mod replaces it.

## Installation
1. Close the game.
2. Steam > Black Mesa > Manage > Browse local files. Extract the zip into that folder (a `bms-dlss` folder appears
   next to `bms.exe`).
3. Run `bms-dlss\install.bat`. It keeps the game's DXVK as `d3d9_dxvk.dll` (backup `d3d9.dll.original`) and copies
   the mod next to it.
4. **Start the game with the Steam launch option "Play DXVK".** If Steam does not ask: Black Mesa > Properties >
   General > Launch options > DXVK. With the default "Play Black Mesa" the mod is not loaded at all (no error, just
   no effect).
5. In game: **Alt+N** = menu, **Alt+F8** = mod on/off (to compare with the original image).

After a game update or "Verify integrity of game files", Steam may restore the original `d3d9.dll`. Run
`install.bat` again – your settings are kept. **Uninstall:** `bms-dlss\uninstall.bat` (or verify the game files).

## Recommended settings
- **Image:** DLSS preset K at native (DLAA) or supersampling 4/3–1.5×; preset L at 2× for the calmest fences.
  Preset K smooths water and animated surfaces a little more than L.
- **Frame generation:** monitor 144–165 Hz: 2× or Auto. 240 Hz and more: 3×–4× or Auto. A higher multiplier means
  fewer real frames and higher latency – Auto keeps real frames as high as possible.
- **DLSS-G or FSR?** DLSS-G handles fast effects (beams, sparks) better. The mod's FSR generator keeps fences and
  wire mesh calmer when turning and is the way to 3×–5× on cards without multi frame generation.

## Known limitations
- Frame generation shows a thin halo around the weapon and hands during fast turns (the background behind the
  weapon has to be guessed). Less visible with more real frames.
- With DLSS-G the driver uses its own frame generation preset; NVIDIA App / Profile Inspector overrides of the
  frame generation preset may not apply to the mod.
- FSR frame generation 3×–5× blends very fast effects (e.g. the tau cannon beam can appear doubled in generated
  frames).
- `d3d9.dll` and `bms_dlss_helper.exe` are not code-signed; the menu uses a keyboard hook while the game runs.
  Some antivirus programs may warn. SHA256 checksums of all files are in `SHA256SUMS.txt`.
- Do not use the mod in multiplayer.

## Credits and licenses
- Mod code: MIT License (`LICENSE`), © 2026 Crashdown.
- **NVIDIA DLSS** (DLSS Super Resolution, DLSS Frame Generation) – NVIDIA RTX SDK. **AMD FidelityFX FSR** – AMD
  FidelityFX SDK. **DXVK** (game's Vulkan renderer). Details and license texts: `THIRD_PARTY_NOTICES.md`.
- Unofficial mod, not affiliated with or endorsed by Crowbar Collective, NVIDIA or AMD.

---

# bms-dlss 1.0.0 – DLSS, DLAA, FSR a Frame Generation pro Black Mesa (česky)

Neoficiální mod pro **Black Mesa** (Steam, Windows). Nahradí vyhlazování hry za **NVIDIA DLSS / DLAA** nebo
**AMD FSR** a přidá **generování snímků** (NVIDIA DLSS Frame Generation nebo FSR) – výrazně čistší ploty, dráty
a mříže a plynulé vysoké FPS. Autor: **Crashdown**.

## Co umí
- **DLAA** (DLSS v nativním rozlišení) a **supersampling DLSS** (vykreslí větší obraz a zmenší ho, jako DLDSR –
  nejčistší ploty). Presety K, L, M.
- **AMD FSR** pro všechny karty s Vulkanem (nativní vyhlazování).
- **Frame Generation:**
  - **DLSS-G** (NVIDIA RTX 40 a novější): 2×, na RTX 50 až **6×**.
  - **FSR** (všechny karty s Vulkanem, i RTX 20/30, AMD, Intel): 2× (FSR FG od AMD) a **3×–5×** (vlastní generátor
    modu založený na FSR 3.1.4).
  - **FSR adaptivní:** doplní jen tolik snímků, kolik chybí do cíle.
  - **Násobek Auto:** volí nejnižší násobek, se kterým se dosáhne výstupních FPS – skutečné snímky mají přednost.
  - Strop výstupních FPS (výchozí: obnovovací frekvence monitoru s malou rezervou, funguje s G-SYNC / FreeSync).
- Pohybové vektory pro pohyblivé objekty a zbraň v ruce (méně duchů).
- Snížení latence podobné NVIDIA Reflex, když brzdí grafická karta.
- Menu ve hře (**Alt+N**), česky a anglicky, nastavení se ukládá do `bms_dlss.ini`.

## Požadavky
- Black Mesa ze Steamu, aktuální verze pro Windows (ve složce hry musí být
  `bin\thirdparty\dxvk-windows-x86\d3d9.dll` – je součástí hry).
- Grafická karta s Vulkanem. DLSS / DLAA: NVIDIA GeForce RTX. DLSS Frame Generation: RTX 40 a novější (víc než 2×:
  RTX 50). FSR a FSR Frame Generation: libovolná karta s Vulkanem.
- Windows 10/11, pro DLSS-G zapnuté hardwarově akcelerované plánování GPU (Nastavení > Obrazovka > Grafika).
- V nastavení hry **vypni vyhlazování (MSAA)** – mod ho nahrazuje.

## Instalace
1. Zavři hru.
2. Steam > Black Mesa > Spravovat > Procházet místní soubory. Do této složky rozbal zip (vznikne `bms-dlss` vedle
   `bms.exe`).
3. Spusť `bms-dlss\install.bat`. DXVK hry ponechá jako `d3d9_dxvk.dll` (záloha `d3d9.dll.original`) a vedle něj
   nakopíruje mod.
4. **Hru spouštěj volbou Steamu „Hrát DXVK“.** Když se Steam neptá: Black Mesa > Vlastnosti > Obecné > Možnosti
   spuštění > DXVK. Při výchozí volbě „Hrát Black Mesa“ se mod vůbec nenačte (žádná chyba, jen nic nedělá).
5. Ve hře: **Alt+N** = menu, **Alt+F8** = mod zap/vyp (porovnání s původním obrazem).

Po aktualizaci hry nebo „Ověřit integritu souborů“ může Steam vrátit původní `d3d9.dll` – spusť znovu `install.bat`
(nastavení zůstane). **Odinstalace:** `bms-dlss\uninstall.bat` (nebo ověření integrity souborů).

## Doporučené nastavení
- **Obraz:** preset K nativně (DLAA) nebo se supersamplem 4/3–1,5×; preset L při 2× pro nejklidnější ploty. Preset K
  o něco víc vyhlazuje vodu a animované plochy než L.
- **Frame Generation:** monitor 144–165 Hz: 2× nebo Auto. 240 Hz a víc: 3×–4× nebo Auto. Vyšší násobek = méně
  skutečných snímků a delší odezva – Auto drží skutečné snímky co nejvýš.
- **DLSS-G, nebo FSR?** DLSS-G lépe zvládá rychlé efekty (paprsky, jiskry). Generátor FSR v modu drží ploty a pletivo
  při otáčení klidnější a na kartách bez více násobného generování je cestou k 3×–5×.

## Známá omezení
- Při rychlém otáčení je s Frame Generation kolem zbraně a rukou tenký lem (pozadí za zbraní se musí dopočítat).
  S více skutečnými snímky je menší.
- U DLSS-G použije ovladač vlastní preset generování snímků; přepsání presetu FG v NVIDIA App / Profile Inspectoru
  se na mod nemusí uplatnit.
- FSR Frame Generation 3×–5× velmi rychlé efekty prolíná (paprsek tau kanónu může být v generovaných snímcích
  dvakrát).
- `d3d9.dll` a `bms_dlss_helper.exe` nejsou podepsané a menu používá háček klávesnice (jen za běhu hry). Některé
  antiviry mohou varovat. Kontrolní součty SHA256 všech souborů jsou v `SHA256SUMS.txt`.
- Mod nepoužívej v multiplayeru.

## Poděkování a licence
- Kód modu: licence MIT (`LICENSE`), © 2026 Crashdown.
- **NVIDIA DLSS** (DLSS Super Resolution, DLSS Frame Generation) – NVIDIA RTX SDK. **AMD FidelityFX FSR** – AMD
  FidelityFX SDK. **DXVK** (Vulkan renderer hry). Podrobnosti a licenční texty: `THIRD_PARTY_NOTICES.md`.
- Neoficiální mod, bez vazby na Crowbar Collective, NVIDIA nebo AMD a bez jejich podpory.
