# bms-dlss 1.0.1 – DLSS, DLAA, FSR and Frame Generation for Black Mesa

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
    generator based on FSR 3.1.4). FSR frame generation has not been tested on AMD / Intel GPUs yet – reports welcome.
  - **FSR adaptive:** generates only as many frames as are missing to reach the target.
  - **Auto multiplier:** picks the lowest multiplier that reaches the output frame rate – real frames have priority.
  - Output FPS cap (default: monitor refresh rate minus a small margin, works with G-SYNC / FreeSync).
- Motion vectors for moving objects and for the weapon in hand (fewer ghosting trails).
- Latency reduction similar to NVIDIA Reflex when the GPU is the bottleneck.
- In-game menu (**Alt+N**), Czech and English, settings are saved to `bms_dlss.ini`.
- **F4** toggles an experimental power saving mode (lower render resolution, reconstructed by DLSS) – see below.

## Requirements
- Black Mesa from Steam, current Windows version (the game folder must contain
  `bin\thirdparty\dxvk-windows-x86\d3d9.dll` – it ships with the game). Windows only; Linux / Steam Deck (Proton) is
  not supported.
- Graphics card with Vulkan. DLSS / DLAA: NVIDIA GeForce RTX. DLSS Frame Generation: RTX 40 or newer
  (more than 2×: RTX 50). FSR upscaling and FSR frame generation: any card with Vulkan.
- Windows 10/11, hardware-accelerated GPU scheduling on (Windows Settings > Display > Graphics) for DLSS-G.
- In the game options: **anti-aliasing (MSAA) off** – the mod replaces it.

## Installation
1. Close the game.
2. Steam > Black Mesa > Manage > Browse local files. Extract the **whole** zip into that folder (a `bms-dlss` folder
   appears next to `bms.exe`).
3. Run `bms-dlss\install.bat`. It keeps the game's DXVK as `d3d9_dxvk.dll` (backup `d3d9.dll.original`), copies the
   mod next to it and sets up the F4 key (one line in `bms\cfg\autoexec.cfg`, see below).
4. **Start the game with the Steam launch option "DXVK"** (Steam shows "Play DXVK"). If Steam does not ask:
   Black Mesa > Properties > General > Selected launch option > DXVK. With the default "Play Black Mesa" the mod is
   not loaded at all (no error, just no effect).
5. In game: **Alt+N** = menu, **Alt+F8** = mod on/off (to compare with the original image).

After a game update or "Verify integrity of game files", Steam may restore the original `d3d9.dll`. Run
`install.bat` again – your settings are kept.

**Uninstall:** close the game and run `bms-dlss\uninstall.bat`. It restores the game's DXVK (if Steam has already
put a newer DXVK back, it is left unchanged), removes the mod's libraries, the F4 line from `autoexec.cfg` and gives
F4 back its previous binding. Settings, logs and the backup `d3d9.dll.original` stay in
`bin\thirdparty\dxvk-windows-x86`; delete them and the `bms-dlss` folder manually if you want. As a fallback you can
also verify the game files in Steam – the F4 line in `autoexec.cfg` then stays (remove the line
`exec bms_dlss_upscale` by hand).

## F4 power saving mode and autoexec.cfg
- **F4** cycles native → Quality (render 0.667) → Balanced (render 0.58). The game renders fewer pixels and DLSS
  reconstructs the full image – less GPU load, slightly softer image. Experimental. After a game start the render is
  always native.
- The installer copies `bms\cfg\bms_dlss_upscale.cfg` and adds the line `exec bms_dlss_upscale` to
  `bms\cfg\autoexec.cfg`. If F4 was bound to another command, the installer says so, remembers it and the uninstaller
  restores it.
- If the image stays blurry after uninstalling, type `mat_viewportscale 1` in the game console.

## Recommended settings
- **Image:** DLSS preset K at native (DLAA) or supersampling 4/3–1.5×. Preset L (experimental) at 2× gives the
  calmest fences but costs more; K smooths water and animated surfaces a little more than L. With frame generation
  use K.
- **Frame generation:** monitor 144–165 Hz: 2× or Auto. 240 Hz and more: 3×–4× or Auto. A higher multiplier means
  fewer real frames and higher latency – Auto keeps real frames as high as possible.
- **In-game V-Sync: off** – use the mod's FPS cap with G-SYNC / FreeSync. With V-Sync on (or exclusive fullscreen at a
  resolution other than the desktop) frame generation can only be started with a game restart.
- **DLSS-G or FSR?** DLSS-G handles fast effects (beams, sparks) better. The mod's FSR generator keeps fences and
  wire mesh calmer when turning and is the way to 3×–5× on cards without multi frame generation.

## Troubleshooting
- **The mod does nothing (no Alt+N menu):** the game was not started with the launch option "DXVK" ("Play DXVK"), or
  a game update restored the original `d3d9.dll` – run `install.bat` again.
- **The menu says DLSS / FSR is not running:** turn off MSAA in the game options. Details are in the logs.
- **Frame generation turned itself off:** the menu (Status line) and the log show the reason. If it only did not start
  in time (slow first start), turn it off and on again in the menu.
- **Logs:** `bms_dlss.log` (game side) and `bms_dlss_helper.log` (DLSS / FSR / frame generation) in
  `bin\thirdparty\dxvk-windows-x86`, previous run as `.prev`. The menu item "Save a bug report" (Performance section)
  collects them into one folder.
- **Reporting a bug:** on Nexus Mods or GitHub (link below) – describe what you did, your GPU and attach both logs.
  The logs contain your Windows version, GPU, driver and the game folder path (which may include your Windows user
  name).

## Known limitations
- Frame generation shows a thin halo around the weapon and hands during fast turns (the background behind the
  weapon has to be guessed). Less visible with more real frames.
- With DLSS-G the driver uses its own frame generation preset; NVIDIA App / Profile Inspector overrides of the
  frame generation preset may not apply to the mod.
- FSR frame generation 3×–5× blends very fast effects (e.g. the tau cannon beam can appear doubled in generated
  frames).
- `d3d9.dll` and `bms_dlss_helper.exe` are not code-signed. The menu hotkeys use a keyboard hook limited to the game
  window while the game runs (not system-wide). The mod does not connect to the internet. Some antivirus programs may
  still warn; SHA256 checksums of all files are in `SHA256SUMS.txt`.
- Do not use the mod in multiplayer.

## Changelog
**1.0.1**
- Fixed: after one GPU device loss (driver reset) frame generation was turned off at every following game start.
- Fixed: the uninstaller could replace a newer DXVK (after a Steam update) with the old backup; running it twice
  is now safe.
- Frame generation start: a longer time limit that ignores loading pauses; if it still does not start in time it can
  be retried from the menu (turn it off and on) instead of staying off until a restart.
- Installer: works when the Steam library path contains accented characters, clear messages instead of PowerShell
  errors, a hint when `install.bat` is run directly from the zip, the previous F4 binding is restored on uninstall.
- `bms_dlss.ini` template in English, menu text fixes, the debug section of the menu is hidden (shown with
  `[keys] DebugKeys=1`), the menu header shows the mod version.
- The helper loads system libraries (Vulkan, version, wintrust, crypt32) only from the Windows system folder.

**1.0.0** – first release.

## Building from source
This repository contains the full source code of the mod. How to build it: [BUILDING.md](BUILDING.md).

## Source code and credits
- Source code and build instructions: https://github.com/Crashdown90/bms-dlss (`BUILDING.md`).
- Mod code: MIT License (`LICENSE`), © 2026 Crashdown.
- **NVIDIA DLSS** (DLSS Super Resolution, DLSS Frame Generation) – NVIDIA RTX SDK. **AMD FidelityFX FSR** – AMD
  FidelityFX SDK. **DXVK** (game's Vulkan renderer). Details and license texts: `THIRD_PARTY_NOTICES.md`.
- Unofficial mod, not affiliated with or endorsed by Crowbar Collective, NVIDIA or AMD.

---

# bms-dlss 1.0.1 – DLSS, DLAA, FSR a Frame Generation pro Black Mesa (česky)

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
    modu založený na FSR 3.1.4). FSR Frame Generation zatím není vyzkoušené na kartách AMD / Intel – hlášení vítána.
  - **FSR adaptivní:** doplní jen tolik snímků, kolik chybí do cíle.
  - **Násobek Auto:** volí nejnižší násobek, se kterým se dosáhne výstupních FPS – skutečné snímky mají přednost.
  - Strop výstupních FPS (výchozí: obnovovací frekvence monitoru s malou rezervou, funguje s G-SYNC / FreeSync).
- Pohybové vektory pro pohyblivé objekty a zbraň v ruce (méně duchů).
- Snížení latence podobné NVIDIA Reflex, když brzdí grafická karta.
- Menu ve hře (**Alt+N**), česky a anglicky, nastavení se ukládá do `bms_dlss.ini`.
- **F4** přepíná experimentální úsporný režim (nižší rozlišení renderu dopočítané DLSS) – viz níže.

## Požadavky
- Black Mesa ze Steamu, aktuální verze pro Windows (ve složce hry musí být
  `bin\thirdparty\dxvk-windows-x86\d3d9.dll` – je součástí hry). Jen Windows; Linux / Steam Deck (Proton) není
  podporovaný.
- Grafická karta s Vulkanem. DLSS / DLAA: NVIDIA GeForce RTX. DLSS Frame Generation: RTX 40 a novější (víc než 2×:
  RTX 50). FSR a FSR Frame Generation: libovolná karta s Vulkanem.
- Windows 10/11, pro DLSS-G zapnuté hardwarově akcelerované plánování GPU (Nastavení > Obrazovka > Grafika).
- V nastavení hry **vypni vyhlazování (MSAA)** – mod ho nahrazuje.

## Instalace
1. Zavři hru.
2. Steam > Black Mesa > Spravovat > Procházet místní soubory. Do této složky rozbal **celý** zip (vznikne `bms-dlss`
   vedle `bms.exe`).
3. Spusť `bms-dlss\install.bat`. DXVK hry ponechá jako `d3d9_dxvk.dll` (záloha `d3d9.dll.original`), vedle něj
   nakopíruje mod a nastaví klávesu F4 (jeden řádek v `bms\cfg\autoexec.cfg`, viz níže).
4. **Hru spouštěj volbou Steamu „DXVK“** (Steam ukazuje „Hrát DXVK“). Když se Steam neptá: Black Mesa > Vlastnosti >
   Obecné > Zvolená možnost spuštění > DXVK. Při výchozí volbě „Hrát Black Mesa“ se mod vůbec nenačte (žádná chyba,
   jen nic nedělá).
5. Ve hře: **Alt+N** = menu, **Alt+F8** = mod zap/vyp (porovnání s původním obrazem).

Po aktualizaci hry nebo „Ověřit integritu souborů“ může Steam vrátit původní `d3d9.dll` – spusť znovu `install.bat`
(nastavení zůstane).

**Odinstalace:** zavři hru a spusť `bms-dlss\uninstall.bat`. Vrátí DXVK hry (když už Steam vrátil novější DXVK,
nechá ho), odebere knihovny modu, řádek F4 z `autoexec.cfg` a klávese F4 vrátí původní přiřazení. Nastavení, logy
a záloha `d3d9.dll.original` zůstanou v `bin\thirdparty\dxvk-windows-x86`; ty a složku `bms-dlss` můžeš smazat ručně.
Nouzově jde mod odstranit i ověřením souborů ve Steamu – řádek F4 v `autoexec.cfg` pak zůstane (řádek
`exec bms_dlss_upscale` smaž ručně).

## Úsporný režim F4 a autoexec.cfg
- **F4** přepíná nativní → Quality (render 0,667) → Balanced (render 0,58). Hra kreslí méně pixelů a DLSS dopočítá
  celý obraz – menší zátěž GPU, obraz o něco měkčí. Experimentální. Po startu hry je render vždy nativní.
- Instalace nakopíruje `bms\cfg\bms_dlss_upscale.cfg` a do `bms\cfg\autoexec.cfg` přidá řádek
  `exec bms_dlss_upscale`. Když byla na F4 jiná akce, instalace to ohlásí, zapamatuje si ji a odinstalace ji vrátí.
- Když po odinstalaci zůstane obraz rozmazaný, zadej v konzoli hry `mat_viewportscale 1`.

## Doporučené nastavení
- **Obraz:** preset K nativně (DLAA) nebo se supersamplem 4/3–1,5×. Preset L (experimentální) při 2× dává
  nejklidnější ploty, ale je dražší; K o něco víc vyhlazuje vodu a animované plochy než L. S Frame Generation
  používej K.
- **Frame Generation:** monitor 144–165 Hz: 2× nebo Auto. 240 Hz a víc: 3×–4× nebo Auto. Vyšší násobek = méně
  skutečných snímků a delší odezva – Auto drží skutečné snímky co nejvýš.
- **V-Sync ve hře: vypnutý** – používej strop FPS modu s G-SYNC / FreeSync. Se zapnutým V-Sync (nebo s výhradní
  celou obrazovkou v jiném rozlišení než plocha) jde Frame Generation zapnout jen s restartem hry.
- **DLSS-G, nebo FSR?** DLSS-G lépe zvládá rychlé efekty (paprsky, jiskry). Generátor FSR v modu drží ploty a pletivo
  při otáčení klidnější a na kartách bez více násobného generování je cestou k 3×–5×.

## Řešení potíží
- **Mod nic nedělá (menu Alt+N se neotevře):** hra neběží s volbou spuštění „DXVK“ („Hrát DXVK“), nebo aktualizace
  hry vrátila původní `d3d9.dll` – spusť znovu `install.bat`.
- **Menu hlásí, že DLSS / FSR neběží:** vypni v nastavení hry MSAA. Podrobnosti jsou v lozích.
- **Frame Generation se sama vypnula:** důvod ukazuje menu (řádek Stav) a log. Když se jen nestihla spustit (pomalý
  první start), vypni a zapni ji v menu znovu.
- **Logy:** `bms_dlss.log` (strana hry) a `bms_dlss_helper.log` (DLSS / FSR / Frame Generation)
  v `bin\thirdparty\dxvk-windows-x86`, minulé spuštění jako `.prev`. Položka menu „Uložit hlášení chyby“ (oddíl
  Výkon) je sebere do jedné složky.
- **Nahlášení chyby:** na Nexus Mods nebo GitHubu (odkaz níže) – popiš, co jsi dělal, jakou máš kartu, a přilož oba
  logy. Logy obsahují verzi Windows, GPU, ovladač a cestu ke složce hry (může v ní být jméno uživatele Windows).

## Známá omezení
- Při rychlém otáčení je s Frame Generation kolem zbraně a rukou tenký lem (pozadí za zbraní se musí dopočítat).
  S více skutečnými snímky je menší.
- U DLSS-G použije ovladač vlastní preset generování snímků; přepsání presetu FG v NVIDIA App / Profile Inspectoru
  se na mod nemusí uplatnit.
- FSR Frame Generation 3×–5× velmi rychlé efekty prolíná (paprsek tau kanónu může být v generovaných snímcích
  dvakrát).
- `d3d9.dll` a `bms_dlss_helper.exe` nejsou podepsané. Klávesy menu používají háček klávesnice jen pro okno hry a jen
  za jejího běhu (ne pro celý systém). Mod se nepřipojuje k internetu. Některé antiviry přesto mohou varovat;
  kontrolní součty SHA256 všech souborů jsou v `SHA256SUMS.txt`.
- Mod nepoužívej v multiplayeru.

## Změny
**1.0.1**
- Opraveno: po jedné ztrátě zařízení GPU (reset ovladače) se Frame Generation vypínala při každém dalším startu hry.
- Opraveno: odinstalace mohla přepsat novější DXVK (po aktualizaci Steamu) starou zálohou; opakované spuštění je teď
  bezpečné.
- Start Frame Generation: delší časový limit, který nepočítá pauzy při načítání; když se FG přesto nestihne spustit,
  jde ji v menu zkusit znovu (vypnout a zapnout) místo vypnutí až do restartu.
- Instalace: funguje i s diakritikou v cestě ke knihovně Steamu, srozumitelné hlášky místo chyb PowerShellu,
  upozornění při spuštění `install.bat` přímo ze zipu, odinstalace vrátí původní přiřazení F4.
- Šablona `bms_dlss.ini` v angličtině, opravy textů menu, ladicí sekce menu je skrytá (zobrazí ji
  `[keys] DebugKeys=1`), záhlaví menu ukazuje verzi modu.
- Helper načítá systémové knihovny (Vulkan, version, wintrust, crypt32) jen ze systémové složky Windows.

**1.0.0** – první vydání.

## Zdrojový kód a poděkování
- Zdrojový kód a postup překladu: https://github.com/Crashdown90/bms-dlss (`BUILDING.md`).
- Kód modu: licence MIT (`LICENSE`), © 2026 Crashdown.
- **NVIDIA DLSS** (DLSS Super Resolution, DLSS Frame Generation) – NVIDIA RTX SDK. **AMD FidelityFX FSR** – AMD
  FidelityFX SDK. **DXVK** (Vulkan renderer hry). Podrobnosti a licenční texty: `THIRD_PARTY_NOTICES.md`.
- Neoficiální mod, bez vazby na Crowbar Collective, NVIDIA nebo AMD a bez jejich podpory.
