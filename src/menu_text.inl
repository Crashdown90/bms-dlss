// bms-dlss: texty menu (cestina / anglictina). Soubor je v UTF-8 (build.ps1 preklada proxy s /utf-8); vklada ho
// src\menu.inl. Jeden radek = jeden text: X(identifikator, cesky, anglicky). Znaky mimo ASCII a ceskou abecedu, ktere
// se tu objevi, si generator atlasu pisma (OvAtlasBuild) vezme sam z teto tabulky. Log zustava ASCII cesky.
// Napoveda (S_HELP_*) ma nejvys 2 radky pri sirce panelu ~40 znaku velikosti pisma (delsi se zkrati "…").
#define MENU_STRINGS(X)                                                                                              \
    X(S_NONE, L"", L"")                                                                                              \
    /* hlavicka, stranky, klavesy */                                                                                 \
    X(S_TITLE, L"Nastavení", L"Settings")                                                                            \
    X(S_PAGE_COMMON, L"Běžné", L"Common")                                                                            \
    X(S_PAGE_ADV, L"Pokročilé", L"Advanced")                                                                         \
    X(S_HINT_CLOSE, L"zavřít", L"close")                                                                             \
    X(S_HINT_SELECT, L"výběr", L"select")                                                                            \
    X(S_HINT_VALUE, L"hodnota", L"value")                                                                            \
    X(S_HINT_APPLY, L"přepnout", L"cycle")                                                                       \
    X(S_HINT_DEFAULT, L"výchozí", L"default")                                                                        \
    X(S_HINT_PAGE, L"stránka", L"page")                                                                              \
    /* stavove radky */                                                                                              \
    X(S_ST_DLSS_OFF, L"DLSS / FSR vypnuto", L"DLSS / FSR off")                                                                   \
    X(S_ST_DLSS_WAIT, L"Upscaler se připravuje", L"Upscaler is starting")                                                    \
    X(S_ST_DEBUG, L"ladicí režim mostu", L"bridge debug mode")                                                       \
    X(S_ST_PRESET, L"preset", L"preset")                                                                             \
    X(S_ST_DEFAULT, L"výchozí", L"default")                                                                          \
    X(S_ST_DEFAULT_K, L"výchozí (K)", L"default (K)")                                                                          \
    X(S_ST_OUTPUT, L"výstup", L"output")                                                                             \
    X(S_ST_RENDER, L"render", L"render")                                                                             \
    X(S_ST_ECO, L"úsporný režim (exp.)", L"power saving (exp.)")                                                     \
    X(S_ST_RUNNING, L"Upscaler běží", L"Upscaler running")                                                                   \
    X(S_ST_IDLE, L"Upscaler neběží – viz bms_dlss_helper.log", L"Upscaler is not running – see bms_dlss_helper.log")         \
    X(S_ST_SAVE_FAIL, L"Uložení do bms_dlss.ini selhalo:", L"Saving bms_dlss.ini failed:")                           \
    X(S_ST_NATIVE, L"nativní", L"native")                                                                            \
    X(S_ST_OTHER, L"jiný", L"custom")                                                                                \
    /* sekce */                                                                                                      \
    X(S_SEC_IMAGE, L"Obraz", L"Image")                                                                               \
    X(S_SEC_ECO, L"Úsporný režim – experimentální (F4)", L"Power saving – experimental (F4)")                        \
    X(S_SEC_FG, L"Frame Generation", L"Frame Generation")                                                            \
    X(S_SEC_PERF, L"Výkon", L"Performance")                                                                          \
    X(S_SEC_DLSS, L"DLSS", L"DLSS")                                                                                  \
    X(S_SEC_DEBUG, L"Ladění (neukládá se)", L"Debugging (not saved)")                                                \
    X(S_SEC_MENU, L"Menu", L"Menu")                                                                                  \
    X(S_SEC_KEYS, L"Klávesy", L"Keys")                                                                               \
    /* polozky */                                                                                                    \
    X(S_IT_ENABLED, L"Zapnuto (Alt+F8)", L"Enabled (Alt+F8)")                                                        \
    X(S_IT_UPSCALER, L"Upscaler", L"Upscaler")                                                                       \
    X(S_IT_OUTSCALE, L"Výstup (supersampling)", L"Output (supersampling)")                                           \
    X(S_IT_PRESET, L"Preset DLSS", L"DLSS preset")                                                                   \
    X(S_IT_SHARP, L"Ostrost (RCAS)", L"Sharpening (RCAS)")                                                           \
    X(S_IT_DOWN, L"Filtr zmenšení", L"Downscale filter")                                                             \
    X(S_IT_MIPBIAS, L"Mip bias", L"Mip bias")                                                                        \
    X(S_IT_MIPBIAS_B, L"Mip bias (úsporný režim)", L"Mip bias (power saving)")                                       \
    X(S_IT_OBJMV, L"MV pohyblivých objektů", L"Object motion vectors")                                               \
    X(S_IT_ECO, L"Render", L"Render")                                                                                \
    X(S_IT_LIGHTSB, L"Opravy efektů (LightsB)", L"Effect fixes (LightsB)")                                           \
    X(S_IT_FG_MODE, L"Režim", L"Mode")                                                                               \
    X(S_IT_FG_MULT, L"Násobek", L"Multiplier")                                                                       \
    X(S_IT_FG_TARGET, L"Výstup FPS (strop)", L"Output FPS cap")                                                      \
    X(S_IT_FG_TARGET_AD, L"Výstup FPS (cíl)", L"Output FPS target")                                                  \
    X(S_IT_FG_REALCAP, L"Omezit hru na", L"Limit game to")                                                           \
    X(S_IT_FG_FILL, L"Generovat pod", L"Generate below")                                                             \
    X(S_IT_FG_MENUPAUSE, L"Pozastavit v menu", L"Pause in menu")                                                     \
    X(S_HELP_FG_MENUPAUSE, L"Zapnuto: s otevřeným menu se negeneruje (menu bez deformací). Vypnuto: změny FG "     \
                           L"a počet snímků jsou vidět hned.",                                                       \
      L"On: no frame generation while this menu is open (no menu artifacts). Off: FG changes and frame counts "      \
      L"show immediately.")                                                                                          \
    X(S_IT_FG_STATE, L"Stav", L"Status")                                                                             \
    X(S_IT_LATENCY, L"Snížení latence (≈ Reflex)", L"Latency reduction (≈ Reflex)")                                  \
    X(S_IT_FPSCOUNTER, L"Ukazatel FPS", L"FPS counter")                                                              \
    X(S_IT_OVERLAYVER, L"Zobrazit verze", L"Show versions")                                                          \
    X(S_OV_FG_OFF, L"FG vyp", L"FG off")                                                                             \
    X(S_OV_PAUSED, L"pauza", L"paused")                                                                              \
    X(S_IT_INPUT, L"Vstup DLSS", L"DLSS input")                                                                      \
    X(S_IT_DEPTH, L"Hloubka pro DLSS", L"Depth for DLSS")                                                            \
    X(S_IT_PHASES, L"Fáze jitteru", L"Jitter phases")                                                                \
    X(S_IT_PIPELINE, L"Synchronizace mostu", L"Bridge sync")                                                         \
    X(S_IT_JSIGN, L"Znaménko jitteru", L"Jitter sign")                                                               \
    X(S_IT_JITTER, L"Jitter bez DLSS", L"Jitter without DLSS")                                                       \
    X(S_IT_BRIDGE, L"Režim mostu", L"Bridge mode")                                                                   \
    X(S_IT_CAPTURE, L"Záchyt snímků", L"Frame capture")                                                              \
    X(S_IT_RESETADV, L"Výchozí (doporučené)", L"Defaults (recommended)")                                             \
    X(S_IT_REPORT, L"Uložit hlášení chyby", L"Save a bug report")                                                     \
    X(S_IT_LANG, L"Jazyk", L"Language")                                                                              \
    X(S_IT_OSD, L"OSD u kláves Alt", L"OSD for Alt keys")                                                            \
    X(S_IT_SCALE, L"Velikost textu", L"Text size")                                                                   \
    X(S_IT_MENUPOS, L"Poloha menu", L"Menu position")                                                                \
    X(S_POS_CENTER, L"uprostřed", L"center")                                                                         \
    X(S_POS_LEFT, L"vlevo", L"left")                                                                                  \
    X(S_POS_RIGHT, L"vpravo", L"right")                                                                               \
    X(S_POS_TL, L"vlevo nahoře", L"top left")                                                                         \
    X(S_POS_TR, L"vpravo nahoře", L"top right")                                                                       \
    X(S_IT_TESTKEYS, L"Zkratky pro testování", L"Testing shortcuts")                                                 \
    /* hodnoty */                                                                                                    \
    X(S_ON, L"zapnuto", L"on")                                                                                       \
    X(S_OFF, L"vypnuto", L"off")                                                                                     \
    X(S_AUTO, L"auto", L"auto")                                                                                      \
    X(S_UPS_AUTO, L"Auto", L"Auto")                                                                                  \
    X(S_UPS_DLSS, L"DLSS", L"DLSS")                                                                                  \
    X(S_UPS_FSR, L"FSR", L"FSR")                                                                                     \
    X(S_UPS_XESS, L"XeSS", L"XeSS")                                                                                  \
    X(S_FG_DLSSG, L"DLSS-G", L"DLSS-G")                                                                              \
    X(S_FG_FSR2X, L"FSR", L"FSR")                                                                                     \
    X(S_FG_FSRADAPT, L"FSR adaptivní", L"FSR adaptive")                                                              \
    X(S_FGS_ACTIVE, L"běží", L"running")                                                                             \
    X(S_FGS_STARTING, L"spouští se", L"starting")                                                                    \
    X(S_FGS_PAUSED, L"pozastaveno", L"paused")                                                                       \
    X(S_FGS_PAUSED_MENU, L"pozastaveno (menu)", L"paused (menu)")                                                    \
    X(S_FGS_UNAVAIL, L"nedostupné", L"unavailable")                                                                  \
    X(S_FGS_FAILED, L"selhalo – do restartu vypnuto", L"failed – off until restart")                                 \
    X(S_FGS_RESTART, L"po restartu hry", L"after game restart")                                                      \
    X(S_FGS_FALLBACK, L"záložní prezentace", L"fallback presentation")                                               \
    X(S_LAT_ADAPTIVE, L"adaptivní", L"adaptive")                                                                     \
    X(S_LAT_MEASURE, L"jen měření", L"measure only")                                                                 \
    X(S_IN_HDR, L"lineární HDR", L"linear HDR")                                                                      \
    X(S_IN_HDRAE, L"HDR + auto expozice", L"HDR + auto exposure")                                                    \
    X(S_IN_ITM, L"HDR + inverzní TM", L"HDR + inverse TM")                                                           \
    X(S_DEPTH_RAW, L"původní", L"original")                                                                          \
    X(S_DEPTH_CONST, L"konstantní (test)", L"constant (test)")                                                       \
    X(S_PH_AUTO, L"auto (8·S²)", L"auto (8·S²)")                                                                     \
    X(S_PIPE_SYNC, L"synchronní", L"synchronous")                                                                    \
    X(S_MIPB_0, L"log2(r) + bias", L"log2(r) + bias")                                                                \
    X(S_MIPB_1, L"jako DLAA", L"as DLAA")                                                                            \
    X(S_BR_COPY, L"kopie", L"copy")                                                                                  \
    X(S_BR_MIRROR, L"zrcadlení", L"mirror")                                                                          \
    X(S_BR_MVDEBUG, L"ladění MV", L"MV debug")                                                                       \
    X(S_LANG_CS, L"čeština", L"čeština")                                                                             \
    X(S_LANG_EN, L"English", L"English")                                                                             \
    X(S_ACT_RUN, L"spustit", L"start")                                                                               \
    /* proc polozka neplati (sede) */                                                                                \
    X(S_WHY_SOON, L"připravuje se", L"coming later")                                                                 \
    X(S_WHY_ECO, L"V úsporném režimu se nepoužívá.", L"Not used in power saving mode.")                              \
    X(S_WHY_S1, L"Jen při výstupu nad 1×.", L"Only with output above 1×.")                                           \
    X(S_WHY_DLAA, L"Jen když DLSS běží.", L"Only while DLSS is running.")                                            \
    X(S_WHY_FORCED, L"Když DLSS běží, je jitter vždy zapnutý.", L"Always on while DLSS is running.")                  \
    X(S_WHY_CAPBUSY, L"Záchyt právě běží.", L"A capture is in progress.")                                            \
    /* poznamky k napovede */                                                                                        \
    X(S_NOTE_RESTART, L"Zapnutí platí po restartu hry.", L"Turning it on takes effect after a game restart.")         \
    X(S_NOTE_DLSS_RTX, L"DLSS: jen NVIDIA RTX.", L"DLSS: NVIDIA RTX only.")                                          \
    X(S_NOTE_DLSS_BROKEN, L"DLSS nejde (viz bms_dlss_helper.log).", L"DLSS is unavailable (see bms_dlss_helper.log).") \
    X(S_NOTE_FSR_VCRT, L"FSR nedostupné: chybí Microsoft Visual C++ Redistributable x64.",                         \
      L"FSR unavailable: Microsoft Visual C++ Redistributable x64 is missing.")                                        \
    X(S_NOTE_GPU_IGPU, L"Běží na integrované GPU – Windows Grafika: bms.exe → Vysoký výkon.",                             L"On integrated GPU – Windows Graphics: bms.exe → High performance.")                                              X(S_NOTE_GPU_NODLSS, L"GPU bez DLSS – Windows Grafika: bms.exe → Vysoký výkon.",                                       L"GPU without DLSS – Windows Graphics: bms.exe → High performance.")                                               X(S_NOTE_NOSAVE, L"Neukládá se.", L"Not saved.")                                                                 \
    X(S_NOTE_FG_MULT_FSR, L"FSR: jen 2× (vlastní generátor nejde vytvořit).", L"FSR: 2× only (own generator failed).") \
    X(S_NOTE_FG_MULT_FSR5, L"FSR 3× až 5×: vlastní generátor; 6× jen DLSS-G.", L"FSR 3× to 5×: own generator; 6× DLSS-G only.") \
    X(S_NOTE_FG_ADAPT_IDLE, L"Hra teď stíhá cíl – jen skutečné snímky.", L"The game meets the target – real frames only.") \
    X(S_NOTE_FG_MULT_RTX50, L"DLSS-G 3×–6× jen RTX 50 (tato karta 2×); režim FSR umí až 5×.", L"DLSS-G 3×–6× RTX 50 only (this GPU: 2×); FSR mode does up to 5×.") \
    X(S_NOTE_FG_MULT_AUTO2, L"Auto: tato karta umí jen 2×, auto drží 2×.", L"Auto: this GPU does 2× only, auto stays at 2×.") \
    X(S_NOTE_FG_MULT_MAX3, L"4× a víc: tato karta umí nejvýš 3×.", L"4× and up: this GPU does at most 3×.")           \
    X(S_NOTE_FG_MULT_MAX4, L"5× a 6×: tato karta umí nejvýš 4×.", L"5× and 6×: this GPU does at most 4×.")             \
    X(S_NOTE_FG_MULT_AUTO, L"Auto: násobek podle rezervy GPU, jen tolik, kolik je potřeba.",                             \
      L"Auto: multiplier follows GPU headroom, only as high as needed.")                                              \
    X(S_LEGACY_KEY, L"Klávesa:", L"Key:")                                                                            \
    X(S_NOHOOK, L"Bez háčku klávesnice: I/K výběr, J/L hodnota, O použít, Alt+N zavřít (klávesy dostane i hra).",    \
      L"No keyboard hook: I/K select, J/L value, O apply, Alt+N close (the game receives the keys too).")            \
    /* napoveda k polozkam (1-2 radky dole) */                                                                       \
    X(S_HELP_ENABLED, L"Totéž co Alt+F8: zapne nebo vypne DLSS / FSR (porovnání s obrazem hry bez modu).",           \
      L"Same as Alt+F8: turns DLSS / FSR on or off (to compare with the game's own image).")                         \
    X(S_HELP_UPSCALER, L"Čím se dopočítává obraz (NVIDIA DLSS / AMD FidelityFX FSR). Auto: DLSS, kde jde, jinak FSR. Vypnutí se neukládá.",             \
      L"What reconstructs the image (NVIDIA DLSS / AMD FidelityFX FSR). Auto: DLSS where available, otherwise FSR. Off is not saved.")                          \
    X(S_HELP_OUTSCALE, L"DLSS vytvoří větší obraz a zmenší ho zpět (jako DLDSR): ostřejší, ale dražší. Hra kreslí "  \
                       L"vždy nativně, 1.0× = DLAA. FSR běží vždy nativně (Native AA).",                             \
      L"DLSS renders a larger image and scales it back down (like DLDSR): sharper but slower. The game always "      \
      L"renders at native resolution, 1.0× = DLAA. FSR always runs at native resolution (Native AA).")              \
    X(S_OUT_NATIVE, L"1.0× (nativní)", L"1.0× (native)")                                                             \
    X(S_ST_SUPERSAMPLE, L"supersampling", L"supersampling")                                                          \
    X(S_HELP_PRESET, L"K: nejklidnější ploty, nejlépe se supersamplem 4/3–1,5×. L: klidný až od 2× (drahé). "         \
                     L"M: jen nativně, se supersamplem bliká. S frame generation se neklid L i M zesílí – s FG K.",  \
      L"K: calmest fences, best with 4/3–1.5x supersampling. L: calm only from 2x (costly). "                        \
      L"M: native only, shimmers with supersampling. Frame generation amplifies L/M shimmer - use K with FG.")       \
    X(S_PRE_K, L"K – doporučeno", L"K – recommended")                                                                \
    X(S_PRE_L, L"L – experimentální", L"L – experimental")                                                          \
    X(S_PRE_M, L"M – experimentální", L"M – experimental")                                                          \
    X(S_HELP_SHARP, L"Doostření po DLSS / FSR (AMD RCAS).", L"Sharpening after DLSS / FSR (AMD RCAS).")                          \
    X(S_HELP_DOWN, L"Zmenšení výstupu nad 1×: Catmull-Rom a Lanczos-2 ostré, Mitchell měkčí, box jen pro porovnání.", \
      L"Downscaling of output above 1×: Catmull-Rom and Lanczos-2 sharp, Mitchell softer, box for comparison.")      \
    X(S_HELP_MIPBIAS, L"Ostřejší textury scény pro DLSS / FSR. −1 dělá moaré na mřížích, proto výchozí −0,5.",             \
      L"Sharper scene textures for DLSS / FSR. −1 causes moiré on grates, hence the default −0.5.")                        \
    X(S_HELP_MIPBIAS_B, L"Při zmenšeném renderu: log2(render/výstup) + bias, jako DLAA, jen log2(r), nebo vypnuto.", \
      L"With reduced render: log2(render/output) + bias, as DLAA, log2(r) only, or off.")                            \
    X(S_HELP_OBJMV, L"Motion vektory pro postavy, dveře, výtahy a vagonky: méně duchů za pohyblivými objekty.",      \
      L"Motion vectors for characters, doors, lifts and trams: less ghosting behind moving objects.")                \
    X(S_HELP_ECO, L"Experimentální: menší render, mod opravuje efekty enginu. F4 nebo dlss_q / dlss_b / dlss_nat.",   \
      L"Experimental: smaller render, the mod patches engine effects. F4 or dlss_q / dlss_b / dlss_nat.")            \
    X(S_HELP_LIGHTSB, L"Opravy světel, stínu slunce, částic a refrakce, když hra kreslí zmenšený obraz.",            \
      L"Fixes lights, sun shadow, particles and refraction when the game renders at reduced resolution.")            \
    X(S_HELP_FG, L"DLSS-G nebo FSR: hra kreslí část snímků, zbytek dopočítá GPU (násobek). Odezva je delší.",        \
      L"DLSS-G or FSR: the game renders some frames, the GPU generates the rest. Latency is higher.")                \
    X(S_HELP_FG_TARGET, L"Snímků zobrazených celkem (hra + generované); hra kreslí výstup / násobek. Auto = Hz − Hz²/3600.", \
      L"Total frames shown (game + generated); the game renders output / multiplier. Auto = Hz − Hz²/3600.")       \
    X(S_HELP_FG_TARGET_AD, L"Snímků zobrazených celkem; hra víc nekreslí, chybějící doplní FG. Auto = Hz − Hz²/3600.", \
      L"Total frames shown; the game never renders more, FG fills the gap. Auto = Hz − Hz²/3600.")                 \
    X(S_HELP_FG_REALCAP, L"Hra sama kreslí nejvýš tolik FPS, zbytek do výstupu doplní FG. Vyp = nejmenší odezva.",     \
      L"The game itself renders at most this; FG fills up to the output. Off = lowest latency.")                   \
    X(S_HELP_FG_FILL, L"Generovat, až hra klesne pod tuto hranici (auto 90 % výstupu); nad 95 % nic.",     \
      L"Generate once the game drops below this (auto 90 % of output); above 95 % nothing.")             \
    X(S_HELP_FG_STATE, L"Skutečné + generované = zobrazené FPS a čas generátoru na snímek. Při otevřeném menu jen s Pozastavit v menu.",   \
      L"Rendered + generated = displayed FPS and generator time per frame. Paused in the menu only with Pause in menu.")       \
    X(S_HELP_FG_MULT, L"Zobrazené snímky na 1 skutečný. Víc = méně skutečných, delší odezva; pro 240 Hz+ a úsporu.", \
      L"Displayed frames per rendered one. More = fewer real frames, more latency; for 240 Hz+.")                    \
    X(S_HELP_FPSCOUNTER, L"Malý řádek vpravo nahoře: skutečné / zobrazené FPS (bez FG jedno číslo).",                \
      L"Small line top-right: real / displayed FPS (one number without FG).")                                        \
    X(S_HELP_OVERLAYVER, L"Řádek vpravo nahoře: běžící upscaler a FG s verzí a násobkem (i bez ukazatele FPS).",     \
      L"Line top-right: running upscaler and FG with version and multiplier (also without FPS).")                    \
    X(S_HELP_LATENCY, L"Když hru brzdí grafická karta, mod zkrátí frontu snímků a odezva myši je rychlejší.",        \
      L"When the GPU is the bottleneck, the mod shortens the frame queue so input feels more responsive.")           \
    X(S_HELP_INPUT, L"Jak DLSS dostává barvu: LDR (8 bit), lineární HDR, s auto expozicí, nebo s inverzním TM.",     \
      L"How DLSS receives colour: LDR (8 bit), linear HDR, with auto exposure, or with inverse tone mapping.")        \
    X(S_HELP_DEPTH, L"Minimum 3×3 tlumí chvění pletiva. Konstantní je jen diagnostika (neukládá se).",               \
      L"Minimum 3×3 reduces shimmer on wire mesh. Constant is diagnostics only (not saved).")                        \
    X(S_HELP_PHASES, L"Délka sekvence jitteru. 8 je na plotech v BMS nejstabilnější, auto = 8·S².",                  \
      L"Jitter sequence length. 8 is the most stable on fences in BMS, auto = 8·S².")                                \
    X(S_HELP_PIPELINE, L"Pipeline: hra na helper nečeká (vyšší FPS). Synchronní je původní režim pro porovnání.",   \
      L"Pipeline: the game does not wait for the helper (higher FPS). Synchronous is the old mode for comparison.")  \
    X(S_HELP_JSIGN, L"Otočí znaménko jitteru předávaného DLSS (když se statický obraz vlní).",                       \
      L"Flips the sign of the jitter passed to DLSS (when a still image wobbles).")                                  \
    X(S_HELP_JITTER, L"Subpixelový jitter bez DLSS (obraz se jemně třese).",                                         \
      L"Sub-pixel jitter without DLSS (the image shakes slightly).")                                                 \
    X(S_HELP_BRIDGE, L"Kopie, zrcadlení a obraz motion vektorů pro hledání chyb.",                                   \
      L"Copy, mirror and motion vector view for debugging.")                                                         \
    X(S_HELP_REPORT, L"Enter: logy, ini a dxvk.conf do složky bms_dlss_hlaseni_… vedle logů (k přiložení k hlášení).",  \
      L"Enter: copies the logs, ini and dxvk.conf to a bms_dlss_hlaseni_… folder next to the logs (attach it to a report).") \
    X(S_HELP_CAPTURE, L"Enter: popis volání D3D9 dalších snímků do bms_dlss_capture_NNN.txt (menu se zavře).",       \
      L"Enter: writes the D3D9 calls of the next frames to bms_dlss_capture_NNN.txt (closes the menu).")            \
    X(S_HELP_LANG, L"Jazyk menu: auto podle jazyka hry (čeština, jinak angličtina), čeština, nebo angličtina.",      \
      L"Menu language: auto (from the game language: Czech, otherwise English), Czech or English.")                 \
    X(S_HELP_RESETADV, L"Enter: vrátí všechna pokročilá nastavení na výchozí (doporučené) hodnoty.",                 \
      L"Enter: resets all advanced settings to the default (recommended) values.")                                  \
    X(S_HELP_OSD, L"Krátký řádek vlevo nahoře při změně klávesou Alt (změny z kláves se neukládají).",               \
      L"A short line top-left when a setting is changed with an Alt key (key changes are not saved).")               \
    X(S_HELP_SCALE, L"Velikost textu menu a OSD.", L"Text size of the menu and OSD.")                                \
    X(S_HELP_MENUPOS, L"Kam se menu kreslí – mimo střed zůstane obraz volný pro porovnání nastavení.",                \
      L"Where the menu is drawn – off-centre leaves the picture free for comparing settings.")                      \
    X(S_HELP_TESTKEYS, L"Alt+F9: FG vyp → DLSS-G → FSR. Alt+F10: DLSS ↔ FSR. Změny z kláves se neukládají.",       \
      L"Alt+F9: FG off → DLSS-G → FSR. Alt+F10: DLSS ↔ FSR. Changes made with these keys are not saved.")         \
    /* OSD */                                                                                                        \
    X(S_OSD_WARN_B, L"DLSS nedopočítává zmenšený render – zapni DLSS (Alt+F8) nebo mat_viewportscale 1",             \
      L"DLSS is not upscaling the reduced render – enable DLSS (Alt+F8) or set mat_viewportscale 1")                \
    X(S_OSD_SAVE_FAIL, L"Nastavení z menu se nepodařilo uložit (viz bms_dlss.log)",                                  \
      L"Menu settings could not be saved (see bms_dlss.log)")                                                        \
    X(S_OSD_WARN_MSAA, L"MSAA ve hře je zapnuté – vypni ho v nastavení videa",                                       \
      L"In-game MSAA is on – turn it off in the video settings")                                                     \
    X(S_OSD_HINT, L"Nastavení modu bms-dlss: ", L"bms-dlss settings: ")                                              \
    X(S_OSD_FG_RESTART, L"Frame Generation vyžaduje restart – zapni ji v menu a restartuj hru",                      \
      L"Frame Generation requires a restart – turn it on in the menu and restart the game")                         \
    X(S_OSD_FAIL_FG, L"Frame Generation selhala – vypnuta do restartu hry, podrobnosti v bms_dlss.log",               \
      L"Frame Generation failed – off until the game restarts, details in bms_dlss.log")                            \
    X(S_OSD_FAIL_BRIDGE, L"DLSS / FSR přestalo běžet – vypnuto do restartu hry, podrobnosti v bms_dlss.log",          \
      L"DLSS / FSR stopped working – off until the game restarts, details in bms_dlss.log")                         \
    X(S_OSD_SAFESTART, L"Minulé spuštění skončilo chybou – Frame Generation je pro tento start vypnutá",             \
      L"The last session ended with an error – Frame Generation is off for this start")                             \
    X(S_OSD_RENDER, L"Render (úsporný režim)", L"Render (saver mode)")                                                \
    X(S_OSD_RENDER_NATIVE, L"nativní", L"native")                                                                     \
    X(S_OSD_REPORT, L"Hlášení chyby uloženo ve složce DXVK: ", L"Bug report saved in the DXVK folder: ")                \
    X(S_OSD_REPORT_FAIL, L"Hlášení chyby nejde uložit: ", L"The bug report could not be saved: ")                      \
    X(S_OSD_DXVK, L"bms-dlss: DXVK hry chybí nebo má neznámou verzi – mod je vypnutý (viz bms_dlss.log)",              \
      L"bms-dlss: the game's DXVK is missing or has an unknown version – the mod is off (see bms_dlss.log)")

enum StrId : uint16_t {
#define X(id, cs, en) id,
    MENU_STRINGS(X)
#undef X
    S_COUNT
};

static const wchar_t* const kMenuStr[S_COUNT][2] = {
#define X(id, cs, en) {cs, en},
    MENU_STRINGS(X)
#undef X
};

// Znaky, ktere atlas pisma obsahuje vzdy (krome ASCII 32-126 a znaku z tabulky vyse): cela ceska a slovenska abeceda
// a znacky pouzivane v rozvrzeni menu a v cislech.
static const wchar_t kMenuExtraChars[] =
    L"ÁČĎÉĚÍŇÓŘŠŤÚŮÝŽáčďéěíňóřšťúůýžÄĹĽÔŔäĺľôŕ‹›←→↑↓•…–−×≈°·²";
