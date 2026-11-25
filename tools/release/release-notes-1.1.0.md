# bms-dlss 1.1.0

Unofficial mod for Black Mesa (Steam, Windows): NVIDIA DLSS / DLAA, AMD FSR and frame generation.

## New since 1.0.1
- **DLSS-G adaptive** (RTX 50) next to FSR adaptive: generates only as many frames as are missing to the target.
- **FSR frame generation up to 6×** (2× AMD FSR FG, 3×–6× the mod's own generator based on FSR 3.1.4).
- **Any output target:** a number in `[fg] TargetFps` is kept exactly, also above the monitor refresh rate. If it cannot be
  reached (at most 6×; at most 3× below 40 real FPS), the output is a whole multiple of the real frame rate.
- **When to generate** (`[fg] Preference`: balanced, responsiveness, smooth, custom, always): the adaptive modes show real
  frames only when the game alone is close enough to the target.
- **Direct flip without frame generation:** FPS above the refresh rate and no desktop compositor. The mod releases the flip
  before frame generation starts and takes it back when it is turned off.
- **Alt+Tab:** the desktop shows up in all tested states (frame generation off, DLSS-G, DLSS-G adaptive, FSR adaptive and
  right after switching). Switching frame generation while playing no longer leaves a frozen image.
- Output without skipped frames at the display limit, steadier fractional frame pacing, shorter slowdown after Alt+Tab.
- Optional `[latency] SampleSync=1`: lower input latency with frame generation (off by default).
- Diagnostic overlay (menu, off by default) and self-checks written to `bms_dlss.log` (lines starting with `KONTROLA:`).
- Slow PCs: temporary problems (slow start, busy GPU) are retried instead of turning frame generation off until a restart.

## Not verified
- Other GPUs than an RTX 5080: AMD, Intel and other NVIDIA cards (DLSS-G adaptive on RTX 50 only, others fall back to FSR
  adaptive).
- Monitors without VRR; 4K with the adaptive modes.
- Slow PCs were only emulated (fewer CPU cores, paused helper).

## Known limitations
- Thin halo around the weapon and hands with frame generation during fast turns.
- FSR 3×–6× blends very fast effects (tau cannon beam).
- After Alt+Tab while the game does not respond (long loading), its last image stays until the game minimizes.
- If the game image stays on the screen after Alt+Tab, set `[present] DxvkFse=0` and please report it with the log.

## Installation
Extract the zip and run `install.bat`; details are in `README.md`. Files are not code-signed, checksums are in
`SHA256SUMS.txt`. Do not use the mod in multiplayer.
