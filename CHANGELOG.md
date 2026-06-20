# Changelog

All notable changes to RamBooster-Winlator. Format: [keep-a-changelog](https://keepachangelog.com/en/1.1.0/). Versioning: [SemVer](https://semver.org/).

## [1.2.1] — 2026-06-20

Pre-release audit patches sebelum public push. Behavior algoritma inti tetap sama.

### Fixed

- **C1**: Race condition pada `guardStatus` antara UI thread (WM_TIMER) dan worker threads (`boostThread`, `zramGuardThread`). Sebelumnya unprotected → undefined behavior. Sekarang dibungkus `CRITICAL_SECTION g_statusLock` + helper `setGuardStatusF` / `getGuardStatus`.
- **H1**: `writeLog` race antara rotate (truncate) dan append dari worker threads concurrent. Bisa corrupt log baris-baris. Sekarang seluruh rotate+append diserialisasi pakai `g_logLock`.
- **H3**: `boostThread` bisa alokasi sampai 3.6GB di HP 8GB (CRISIS 45%) — wall-clock ~13 detik saat game paling butuh. Sekarang absolute cap 1.5GB → ~4-5 detik. Logged ke `DEBUG_LOG.txt` kalau target di-cap.
- **C2**: `InitSettings` ga validate range. `INTERVAL_MENIT=0` bisa bikin spam boost tiap WM_TIMER tick. Threshold di luar 0-100 → guard misfire. Sekarang clamp: `INTERVAL_MENIT` 1-60, `THRESHOLD_CRISIS` 50-100, `PRE_CRISIS_LEVEL` 0-100, `ZRAM_GUARD_LEVEL` 50-95.
- **M3**: HFONT yang dibuat di WM_CREATE tidak pernah di-`DeleteObject` → GDI leak setiap kali window dibuka/tutup. Sekarang disimpan ke global `hFont`, di-cleanup di WM_DESTROY.

### Skipped (sengaja tidak diapply)

- CreateProcess argument escaping — `gamePath` dari `GetOpenFileName` dialog (user-controlled, bukan attacker)
- Wide-string (-DUNICODE) conversion — mingw32 default ANSI, jalan di Wine ANSI di Winlator
- Compiler hardening flags (-fstack-protector, /GS-equivalent) — nice-to-have, bukan blocker

## [1.2] — Upstream snapshot (pre-fork)

Versi base dari [RedZeroGotcha/RamBoosterSingle](https://github.com/RedZeroGotcha/RamBoosterSingle).
Sudah include: GDI leak fix (hBg/hBgClass), KillTimer di WM_DESTROY, working directory fix buat GTA V, demand paging 4KB, CRISIS give-up mechanism, Pre-Crisis layer, adaptive interval, log rotation 1MB, EMULATOR_MODE (Box64/FEXCore), core pinning configurable.

Untuk history upstream sebelum v1.2, lihat repo asli.
