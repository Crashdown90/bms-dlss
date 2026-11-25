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
    X(S_ST_DEBUG, L"ladicí režim helperu", L"helper debug mode")                                                       \
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
    X(S_IT_OBJMV, L"Pohybové vektory objektů", L"Object motion vectors")                                               \
    X(S_IT_ECO, L"Render", L"Render")                                                                                \
    X(S_IT_LIGHTSB, L"Opravy efektů", L"Effect fixes")                                           \
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
    X(S_IT_FG_PREF, L"Kdy generovat", L"When to generate")                                                           \
    X(S_IT_FG_PREFFPS, L"Vlastní: generovat pod", L"Custom: generate below")                                         \
    X(S_IT_FG_PREFSTATE, L"Rozhodnutí", L"Decision")                                                                 \
    X(S_IT_LATENCY, L"Snížení latence", L"Latency reduction")                                                      \
    X(S_IT_FPSCOUNTER, L"Ukazatel FPS", L"FPS counter")                                                              \
    X(S_IT_OVERLAYVER, L"Zobrazit verze", L"Show versions")                                                          \
    X(S_OV_FG_OFF, L"FG vyp", L"FG off")                                                                             \
    X(S_OV_PAUSED, L"pauza", L"paused")                                                                              \
    X(S_IT_DIAG, L"Diagnostika plynulosti", L"Smoothness diagnostics")                                               \
    X(S_IT_DG_GAME, L"  Hra", L"  Game")                                                                             \
    X(S_IT_DG_MON, L"  Monitor", L"  Monitor")                                                                       \
    X(S_IT_DG_SKIP, L"  Přeskočeno", L"  Skipped")                                                                   \
    X(S_IT_DG_FG, L"  FG", L"  FG")                                                                                  \
    X(S_IT_DG_GRAPH, L"  Graf", L"  Graph")                                                                          \
    X(S_DG_GAME_TXT, L"FPS vykreslené hrou", L"FPS rendered by the game")                                            \
    X(S_DG_MON_TXT, L"obrazy/s na monitoru, i generované", L"frames/s on the monitor, incl. generated")              \
    X(S_DG_SKIP_TXT, L"hotové obrazy, které monitor nedostal (~1 s)", L"finished frames the monitor never got (~1 s)") \
    X(S_DG_FG_TXT, L"generuje N× / jen skutečné a proč", L"generating N× / real frames only, and why")                \
    X(S_DG_GRAPH_TXT, L"mezery mezi obrazy na monitoru; plynulé = rovná čára", L"gaps between frames on the monitor; smooth = flat line")     X(S_IT_DG_OUT, L"  Krátké / dlouhé", L"  Short / long")                                                             X(S_DG_OUT_TXT, L"intervaly < 0,75× / > 1,33× mediánu za ~1 s (cíl 0)", L"gaps < 0.75× / > 1.33× the median, last ~1 s (aim: 0)") \
    X(S_DG_L1, L"Hra %ls · Monitor %ls FPS", L"Game %ls · Monitor %ls FPS")                                          \
    X(S_DG_DXVK, L"Hra %ls · Monitor %ls FPS (bez FG: Present DXVK)", L"Game %ls · Monitor %ls FPS (no FG: DXVK Present)") \
    X(S_DG_L2, L"Přeskočeno %d / s (%ls %%)", L"Skipped %d / s (%ls%%)")                                             \
    X(S_DG_L2NA, L"Přeskočeno n/a (bez FG)", L"Skipped n/a (no FG)")                                                 \
    X(S_DG_IV, L"Interval %ls ms · sd %ls · krátké %d / dlouhé %d", L"Interval %ls ms · sd %ls · short %d / long %d") \
    X(S_IT_INPUT, L"Vstup DLSS", L"DLSS input")                                                                      \
    X(S_IT_DEPTH, L"Hloubka pro DLSS", L"Depth for DLSS")                                                            \
    X(S_IT_PHASES, L"Fáze jitteru", L"Jitter phases")                                                                \
    X(S_IT_PIPELINE, L"Synchronizace helperu", L"Helper sync")                                                         \
    X(S_IT_JSIGN, L"Znaménko jitteru", L"Jitter sign")                                                               \
    X(S_IT_JITTER, L"Jitter bez DLSS", L"Jitter without DLSS")                                                       \
    X(S_IT_BRIDGE, L"Režim helperu", L"Helper mode")                                                                   \
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
    X(S_FG_DLSSGADAPT, L"DLSS-G adaptivní", L"DLSS-G adaptive")                                                      \
    X(S_FGS_ACTIVE, L"běží", L"running")                                                                             \
    X(S_FGS_STARTING, L"spouští se", L"starting")                                                                    \
    X(S_FGS_PAUSED, L"pozastaveno", L"paused")                                                                       \
    X(S_FGS_PAUSED_MENU, L"pozastaveno (menu)", L"paused (menu)")                                                    \
    X(S_FGS_UNAVAIL, L"nedostupné", L"unavailable")                                                                  \
    X(S_FGS_FAILED, L"selhalo – do restartu vypnuto", L"failed – off until restart")                                 \
    X(S_FGS_RESTART, L"po restartu hry", L"after game restart")                                                      \
    X(S_FGS_FALLBACK, L"záložní prezentace", L"fallback presentation")                                               \
    X(S_PREF_RESP, L"Přednost odezvě", L"Responsiveness")                                                            \
    X(S_PREF_BAL, L"Vyvážené", L"Balanced")                                                                          \
    X(S_PREF_SMOOTH, L"Plynulost", L"Smoothness")                                                                    \
    X(S_PREF_ALWAYS, L"Vždy", L"Always")                                                                             \
    X(S_PREF_CUSTOM, L"Vlastní", L"Custom")                                                                          \
    X(S_PREF_STEADY, L"Stálé FPS (ini)", L"Steady FPS (ini)")                                                        \
    X(S_PFS_GEN, L"generuje", L"generating")                                                                         \
    X(S_PFS_REAL, L"jen skutečné", L"real frames only")                                                              \
    X(S_PFR_GEN, L"hra %ls FPS pod %ls, zisk +%ls %%", L"game %ls FPS below %ls, gain +%ls%%")                       \
    X(S_PFR_ABOVE, L"hra %ls FPS nad prahem %ls", L"game %ls FPS above threshold %ls")                               \
    X(S_PFR_GAIN, L"zisk jen %ls %% (min. %ls %%)", L"gain only %ls%% (min. %ls%%)")                                 \
    X(S_PFR_TARGET, L"hra stíhá cíl sama (%ls FPS)", L"game meets the target (%ls FPS)")                             \
    X(S_PFR_HOLD, L"hra %ls FPS, pásmo %ls–%ls: drží stav", L"game %ls FPS, band %ls–%ls: state kept")               \
    X(S_PFR_ROUGH, L"hra bez FG ≥ %ls FPS (hrubý odhad), práh %ls", L"game without FG ≥ %ls FPS (rough estimate), threshold %ls") \
    X(S_PFR_GUARD, L"pauza / menu: beze změny", L"pause / menu: unchanged")                                          \
    X(S_PFR_USELESS, L"generování nepřidávalo, zkusí znovu", L"generating added nothing, will retry")               \
    X(S_PFR_BLOCK, L"odhad nevyšel, chvíli generuje dál", L"estimate missed, keeps generating for now")              \
    X(S_PFR_ALWAYS, L"Vždy: doplňuje do cíle", L"Always: fills up to the target")                                    \
    X(S_PFR_NOEST, L"odhad FPS chybí – jako Vždy", L"no FPS estimate – acts as Always")                              \
    X(S_PFR_WAIT, L"měří se…", L"measuring…")                                                                        \
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
    X(S_NOTE_FG_MULT_FSR5, L"FSR 3× až 6×: vlastní generátor.", L"FSR 3× to 6×: own generator.") \
    X(S_NOTE_FG_DGADAPT_GPU, L"DLSS-G adaptivní jen na kartách s DLSS-G nad 2× (RTX 50); jinde FSR adaptivní.",       \
      L"DLSS-G adaptive needs a GPU with DLSS-G above 2× (RTX 50); use FSR adaptive elsewhere.")                    \
    X(S_NOTE_FG_ADAPT_IDLE, L"Hra teď stíhá cíl – jen skutečné snímky.", L"The game meets the target – real frames only.") \
    X(S_NOTE_FG_MULT_RTX50, L"DLSS-G 3×–6× jen RTX 50 (tato karta 2×); režim FSR umí až 6×.", L"DLSS-G 3×–6× RTX 50 only (this GPU: 2×); FSR mode does up to 6×.") \
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
      L"DLSS renders a larger image and scales it down (like DLDSR): sharper, slower. 1.0× = DLAA. "                \
      L"FSR: native only (Native AA).")                                                                              \
    X(S_OUT_NATIVE, L"1.0× (nativní)", L"1.0× (native)")                                                             \
    X(S_ST_SUPERSAMPLE, L"supersampling", L"supersampling")                                                          \
    X(S_HELP_PRESET, L"K: nejklidnější ploty (nejlépe 4/3–1,5×). L: klidný od 2× (drahé). M: jen nativně. "            \
                     L"S Frame Generation používej K.",                                                              \
      L"K: calmest fences (best 4/3–1.5×). L: calm from 2× (costly). M: native only. "                              \
      L"Use K with Frame Generation.")                                                                               \
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
    X(S_HELP_OBJMV, L"Pohybové vektory pro postavy, dveře, výtahy a vagonky: méně duchů za pohyblivými objekty.",      \
      L"Motion vectors for characters, doors, lifts and trams: less ghosting behind moving objects.")                \
    X(S_HELP_ECO, L"Experimentální: menší render, mod opravuje efekty enginu. F4 nebo dlss_q / dlss_b / dlss_nat.",   \
      L"Experimental: smaller render, the mod patches engine effects. F4 or dlss_q / dlss_b / dlss_nat.")            \
    X(S_HELP_LIGHTSB, L"Opravy světel, stínu slunce, částic a refrakce, když hra kreslí zmenšený obraz.",            \
      L"Fixes lights, sun shadow, particles and refraction when the game renders at reduced resolution.")            \
    X(S_HELP_FG, L"DLSS-G nebo FSR: hra kreslí část snímků, zbytek dopočítá GPU (násobek; adaptivní = jen co chybí do cíle). Odezva je delší.", \
      L"DLSS-G or FSR: the game renders some frames, the GPU generates the rest (adaptive = only what is missing to the target). Latency is higher.") \
    X(S_HELP_FG_TARGET, L"Snímků celkem (hra + generované), hra kreslí výstup / násobek. Auto = Hz − Hz²/3600 (doporučeno); nad Hz se obraz může trhat.", \
      L"Total frames (game + generated); the game renders output / multiplier. Auto = Hz − Hz²/3600 (recommended); above Hz the image may tear.") \
    X(S_HELP_FG_TARGET_AD, L"Snímků celkem, chybějící doplní FG (nejvýš 6×). Auto = Hz − Hz²/3600 (doporučeno); nad Hz monitor celé neukáže, obraz se může trhat.", \
      L"Total frames; FG fills the gap (6× at most). Auto = Hz − Hz²/3600 (recommended); above Hz frames are shown in part and the image may tear.") \
    X(S_HELP_FG_REALCAP, L"Hra sama kreslí nejvýš tolik FPS, zbytek do výstupu doplní FG. Vyp = nejmenší odezva.",     \
      L"The game itself renders at most this; FG fills up to the output. Off = lowest latency.")                   \
    X(S_HELP_FG_FILL, L"Generovat, až hra klesne pod tuto hranici (auto 90 % výstupu); nad 95 % nic.",     \
      L"Generate once the game drops below this (auto 90% of output); above 95% nothing.")             \
    X(S_HELP_FG_STATE, L"Skutečné + generované = zobrazené FPS a čas generátoru na snímek. Při otevřeném menu jen s Pozastavit v menu.",   \
      L"Rendered + generated = displayed FPS and generator time per frame. Paused in the menu only with Pause in menu.")       \
    X(S_HELP_FG_PREF, L"Generovat jen pod prahem FPS hry v % cíle: odezva 50, vyvážené 75, plynulost 85; Vždy = do cíle vždy.", \
      L"Generate only below a game FPS threshold in % of the target: 50 / 75 / 85; Always = always fill.")             \
    X(S_HELP_FG_PREFFPS, L"Vlastní práh: generovat, jen když hra bez generování jede pod tolik % cíle.",                \
      L"Custom threshold: generate only while the game runs below this % of the target without it.")                       \
    X(S_HELP_FG_PREFSTATE, L"Proč adaptivní režim teď generuje, nebo ne (FPS hry bez generování, práh, zisk).",     \
      L"Why the adaptive mode generates now or not (game FPS without generation, threshold, gain).")                \
    X(S_HELP_FG_MULT, L"Zobrazené snímky na 1 skutečný. Víc = méně skutečných, delší odezva; pro 240 Hz+ a úsporu.", \
      L"Displayed frames per rendered one. More = fewer real frames, more latency; for 240 Hz+.")                    \
    X(S_HELP_FPSCOUNTER, L"Malý řádek vpravo nahoře: skutečné / zobrazené FPS (bez FG jedno číslo).",                \
      L"Small line top-right: real / displayed FPS (one number without FG).")                                        \
    X(S_HELP_OVERLAYVER, L"Řádek vpravo nahoře: běžící upscaler a FG s verzí a násobkem (i bez ukazatele FPS).",     \
      L"Line top-right: running upscaler and FG with version and multiplier (also without FPS).")                    \
    X(S_HELP_DIAG, L"Vpravo nahoře čísla a graf: co hra vykreslí a co opravdu jde na monitor. Popis níže.",          \
      L"Top-right numbers and graph: what the game renders and what really reaches the monitor. See below.")         \
    X(S_HELP_DG, L"Význam čísla v diagnostice plynulosti (vpravo nahoře).",                                          \
      L"Meaning of this number in the smoothness diagnostics (top-right).")                                          \
    X(S_HELP_LATENCY, L"Když hru brzdí grafická karta, mod zkrátí frontu snímků a odezva je rychlejší (podobné NVIDIA Reflex).",        \
      L"When the GPU is the bottleneck, the mod shortens the frame queue for faster input (similar to NVIDIA Reflex).")           \
    X(S_HELP_INPUT, L"Jak DLSS dostává barvu: LDR (8 bit), lineární HDR, s auto expozicí, nebo s inverzním TM.",     \
      L"How DLSS receives color: LDR (8 bit), linear HDR, with auto exposure, or with inverse tone mapping.")        \
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
      L"Where the menu is drawn – off-center leaves the picture free for comparing settings.")                      \
    X(S_HELP_TESTKEYS, L"Alt+F9: režimy FG dokola (pořadí jako v menu). Alt+F10: DLSS ↔ FSR. Změny z kláves se neukládají.", \
      L"Alt+F9: cycles FG modes (menu order). Alt+F10: DLSS ↔ FSR. Changes made with these keys are not saved.")   \
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
    X(S_OSD_FG_SUBST, L"DLSS-G adaptivní na této kartě není (jen RTX 50) – použit náhradní režim FG",                \
      L"DLSS-G adaptive is not available on this GPU (RTX 50 only) – using a substitute FG mode")                   \
    X(S_OSD_FAIL_FG, L"Frame Generation selhala – vypnuta do restartu hry, podrobnosti v bms_dlss.log",               \
      L"Frame Generation failed – off until the game restarts, details in bms_dlss.log")                            \
    X(S_OSD_FAIL_FG_RETRY, L"Frame Generation se nestihla spustit – nový pokus: vypni a zapni ji v menu",           \
      L"Frame Generation did not start in time – to retry, turn it off and on in the menu")                         \
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
