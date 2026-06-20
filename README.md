# RamBooster Winlator Edition

RAM management tool buat emulator PC di Android — fork dari [RedZeroGotcha/RamBoosterSingle](https://github.com/RedZeroGotcha/RamBoosterSingle) yang gw setel ulang biar fokus jalan di Winlator (Box64) dan GameHub/BannerHub (FEXCore).

Versi sekarang: **v1.2.1**. Tested di Helio G80-G99. Harusnya jalan di chipset lain juga, tinggal sesuain `CORE_AFFINITY_MASK` di `settings.ini`.

## Kredit

Source asli: **RedZeroGotcha** — https://github.com/RedZeroGotcha/RamBoosterSingle
Lisensi upstream: MIT. Repo ini melanjutkan lisensi itu.

Gw cuma nge-port + patch buat use case Android emulator. Algoritma inti (VirtualAlloc-pressure-to-LMK) tetap punya RedZeroGotcha.

## Cara kerjanya

Singkat: program ini mengalokasi RAM secara sengaja, menyentuh tiap halaman (4KB / 16KB tergantung device), lalu nge-rilis. Tekanan memori itu memaksa Android Low Memory Killer milih korban dari background — biasanya app yang lagi nganggur. Hasilnya: RAM bebas buat game di foreground naik.

Karena jalan di dalam Wine, `VirtualAlloc` → diterjemahkan ke `mmap` Linux → kerasa langsung sama Android LMK. Itu sebabnya tool ini bisa "menggerakkan" LMK dari dalam emulator.

Tiga lapisan guard:

1. **ZRAM Guard** (default 75%) — sentuhan ringan ~10% total RAM
2. **Pre-Crisis** (default 83%) — boost preventif 20%, mencegah CRISIS terpicu
3. **CRISIS** (default 90%) — boost barbar 45%, last resort

Plus routine cleaning tiap `INTERVAL_MENIT` (default 15 menit) dengan porsi `ROUTINE_CLEAN_PERCENT` (default 10%).

## Pakai

Taruh `RamBooster_v1.2.1.exe` di folder mana aja di dalam container Winlator/GameHub. Jalanin via shortcut atau langsung.

Pertama kali run, dia bikin `settings.ini` di folder yang sama. Tweak sesuai chipset (lihat komentar di file `settings.ini` — udah ada cheatsheet `CORE_AFFINITY_MASK` per chipset).

Workflow normal:

1. Klik **SELECT GAME** → pilih `.exe` game
2. Klik **BOOST & LAUNCH** → tool nge-boost dulu, baru launch game
3. Window minimize otomatis, Silent Guard nyala di background

Log ditulis ke `DEBUG_LOG.txt` di folder yang sama. Auto-rotate kalau >1MB.

## settings.ini — yang penting

```
THRESHOLD_CRISIS=90       # boost barbar di RAM ≥ 90%
PRE_CRISIS_LEVEL=83       # boost preventif di 83% (0 = matikan)
INTERVAL_MENIT=15         # routine cleaning tiap 15 menit
ROUTINE_CLEAN_PERCENT=10  # porsi routine. 0 = matikan
ZRAM_GUARD_LEVEL=75       # ZRAM Guard trigger
EMULATOR_MODE=0           # 0 = Box64 (Winlator), 1 = FEXCore (GameHub)
CORE_AFFINITY_MASK=0xC0   # Helio default. 0x00 = nonaktif (SD 8 Elite)
```

`EMULATOR_MODE` ngatur safety floor + jeda antar chunk:
- Box64 → floor 12%, sleep 25ms
- FEXCore → floor 15%, sleep 35ms (JIT cache FEX butuh ruang napas lebih)

Kalau gak tau chipset, biarin `CORE_AFFINITY_MASK=0xC0`. Aman buat Helio dan kebanyakan SD 6xx-7xx.

## Apa yang dirubah dari upstream

v1.2.1 ini fork yang udah dipatch buat audit pra-rilis. Yang berubah dibanding v1.2:

| Patch | Fix | Kategori |
|-------|-----|----------|
| C1 | Lock pada `guardStatus` (CRITICAL_SECTION `g_statusLock`) — cegah data race UI ↔ worker thread | Concurrency |
| H1 | Lock pada `writeLog` rotate+append (`g_logLock`) — cegah log interleave/corrupt | Concurrency |
| H3 | Absolute cap 1.5GB di `boostThread` — CRISIS 45% di HP 8GB ga lagi alokasi 3.6GB | Sanity |
| C2 | Range clamp di `InitSettings`: `INTERVAL_MENIT` 1-60, threshold 50-100, dll. | Validation |
| M3 | HFONT global + DeleteObject di WM_DESTROY — fix GDI leak | Memory |

Sengaja **tidak** diapply (overengineering buat use case offline):
- CreateProcess argument escaping (gamePath dari file picker, bukan input attacker)
- Wide-string conversion (mingw32 default ANSI, jalan di Wine ANSI)
- Compiler hardening flags (-fstack-protector dll) — nice-to-have, bukan blocker

Detail lengkap diff lihat `src/v1.2.1-from-v1.2.patch`.

## Build dari source

Cross-compile pakai mingw32:

```bash
i686-w64-mingw32-g++ -O2 -static -mwindows \
    -o RamBooster_v1.2.1.exe src/rambooster.cpp \
    -lgdi32 -luser32 -lkernel32 -lcomctl32 -lshell32 -lpsapi -lwinmm
```

Target: 32-bit Windows GUI. Static link biar ga butuh DLL Wine yg suka beda versi antar fork.

Verifikasi:

```bash
file RamBooster_v1.2.1.exe
# PE32 executable for MS Windows 6.00 (GUI), Intel i386
```

## Yang ga akan gw saranin

- **Mobox / ExaGear** — usang, ga support
- **Project gaib** kayak "Cassia" dll — abandonware
- **DRM bypass** atau "cara crack game" — ga akan dibahas. Mau pakai game bajakan atau ori urusan lo, tool ini ga peduli.

## Yang gw saranin

- Winlator (mainstream / fork yang aktif: GLibc, Cmod, Frost, dll.)
- GameHub / BannerHub (basis FEXCore)
- Update Box64/FEXCore standalone susah di mobile — ganti emulator/fork lebih praktis daripada nge-patch Wine sendiri

## Disclaimer

Tool ini ngalokasi RAM secara sengaja. Di kasus ekstrim (RAM HP udah penuh + safety floor terlewat) bisa bikin sistem kurang stabil — meskipun udah ada 3 lapisan safety (floor dinamis, CRISIS backoff 2 menit, give-up mechanism).

DWYOR (Do What You Own Risk). Crash game, app kepilih korban LMK, dll. — itu konsekuensi normal dari nge-manage RAM. Kalau ga mau ambil resiko, jangan pakai.

Lo tanggung jawab atas device lo sendiri.

## Lisensi

MIT — lihat [LICENSE](LICENSE). Inherits dari RedZeroGotcha/RamBoosterSingle.

## Issues

Repo ini fork, jadi laporan bug yang spesifik ke patch v1.2.1 ke sini. Bug yang ada di upstream juga (atau pertanyaan algoritma inti) lapor ke [upstream](https://github.com/RedZeroGotcha/RamBoosterSingle).
