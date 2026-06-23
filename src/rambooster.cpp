// =============================================================================
//  RAMBOOSTER v1.2.1 - Winlator Edition
//  ModBy: Noysz | Base: RedZeroGotcha (github.com/RedZeroGotcha/RamBoosterSingle)
//  Target: Helio G80-G99 | Compatible: Box64 & FEXCore
//
//  [FIX]    GDI Leak #1 & #2: hBg dan hBgClass dikelola eksplisit.
//  [FIX]    Demand Paging 4KB: loop sentuhan setiap halaman fisik.
//  [FIX]    KillTimer di WM_DESTROY mencegah crash saat cleanup.
//  [FIX]    Working Directory: fix untuk GTA V & game launcher terpisah.
//  [LIVE]   THRESHOLD_CRISIS: aktif di RunSilentGuard.
//  [LIVE]   Core Pinning: configurable via CORE_AFFINITY_MASK.
//           Set 0x00 untuk SD 8 Elite/Oryon (skip pinning).
//  [REMOVE] lastRAMUsage & HIGH_PRIORITY_CLASS (dead code / tidak efektif).
//  [TUNED]  Chunk 8MB, hold 500ms, safety floor 12%/15% Box64/FEXCore.
//  [TUNED]  CLEAN_START_PERCENT default 35%, cap 60%.
//  [TUNED]  INTERVAL_MENIT default 15 menit.
//  [TUNED]  ZRAM Guard trigger 10%.
//  [NEW]    CRISIS Give Up: jika CRISIS terpicu 2x berturut-turut,
//           program backoff 2 menit. Mencegah spam boost sia-sia
//           saat LMK sudah kehabisan korban.
//  [NEW]    Visual Feedback: status label real-time menampilkan
//           kondisi Guard aktif (Idle, ZRAM, Pre-Crisis, CRISIS, Backoff).
//  [NEW]    Pre-Crisis Layer (PRE_CRISIS_LEVEL=83): lapisan tengah antara
//           ZRAM Guard dan CRISIS. Boost preventif 20% di 83% untuk
//           mencegah CRISIS 45% terpicu. Net CPU load LEBIH RENDAH
//           karena mencegah boost yang lebih besar dan agresif.
//           Set PRE_CRISIS_LEVEL=0 untuk nonaktifkan.
//  [NEW]    Adaptive Boost Interval: interval routine otomatis dipersingkat
//           jadi setengahnya jika RAM > 65% saat timer masuk.
//  [NEW]    Log Rotation: DEBUG_LOG.txt di-rotate ke DEBUG_LOG_OLD.txt
//           otomatis saat ukuran melebihi 1MB.
//  [NEW]    EMULATOR_MODE: 0=Box64 (Winlator), 1=FEXCore (GameHub).
// =============================================================================

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>     // v1.2.1: va_list buat setGuardStatusF
#include <process.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <stdlib.h>
#include <atomic>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")

// --- Konstanta ---
#define MY_COLOR_BG  RGB(25, 25, 25)
#define COLOR_TEXT   RGB(0, 255, 255)
#define TIMER_MAIN   1

// --- Variabel Global ---
char gamePath[MAX_PATH] = "";
char exeFolder[MAX_PATH] = "";

int thresholdCrisis;     // Trigger crisis boost (misal 90%)
int preCrisisLevel;      // Trigger pre-crisis boost (misal 83%)
int zramGuardLevel;
int intervalMenit;
int routineCleanPercent; // B4: porsi (%) routine cleaning periodik. 0 = matikan.
int isMuted, isZramGuard, isDWYOR, cleanStartPercent;
int emulatorMode;      // 0 = Box64 (Winlator), 1 = FEXCore (GameHub)
DWORD affinityMask;    // CPU affinity mask untuk Core Pinning

DWORD lastBoostTick      = 0;
DWORD lastRoutineTick    = 0;
DWORD lastPreCrisisTick  = 0; // Cooldown terpisah untuk Pre-Crisis
DWORD crisisBackoff      = 0;
int   crisisStreak       = 0;

// Status terakhir dari Silent Guard — ditampilkan di GUI label
char guardStatus[64] = "Idle";

std::atomic<bool> isBoosting(false);

// v1.2.1 [C1 FIX]: guardStatus diakses dari worker threads (boostThread,
// zramGuardThread) + UI thread (WM_TIMER). Tanpa lock = data race UB.
// Init di WM_CREATE, destroy di WM_DESTROY.
CRITICAL_SECTION g_statusLock;

// v1.2.1 [H1 FIX]: writeLog dipanggil concurrent dari beberapa thread.
// Tanpa lock, rotation truncate + append bisa interleave & corrupt log.
CRITICAL_SECTION g_logLock;

// v1.2.1 [M3 FIX]: font handle disimpan global biar bisa cleanup di WM_DESTROY.
HFONT hFont = NULL;

// hBg: brush untuk static controls (WM_CTLCOLORSTATIC)
// hBgClass: brush untuk background window class (WNDCLASSEX)
// Keduanya harus dihapus eksplisit di WM_DESTROY.
HBRUSH hBg      = NULL;
HBRUSH hBgClass = NULL;
HWND hStatusLabel = NULL;

// v1.2.1 [C1 FIX]: helper thread-safe untuk update/read guardStatus.
// Pakai vsnprintf ke buffer lokal, lalu copy ke global di bawah lock.
static void setGuardStatusF(const char* fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    tmp[sizeof(tmp) - 1] = '\0';
    EnterCriticalSection(&g_statusLock);
    strncpy(guardStatus, tmp, sizeof(guardStatus) - 1);
    guardStatus[sizeof(guardStatus) - 1] = '\0';
    LeaveCriticalSection(&g_statusLock);
}

static void getGuardStatus(char* out, size_t outsize) {
    EnterCriticalSection(&g_statusLock);
    strncpy(out, guardStatus, outsize - 1);
    out[outsize - 1] = '\0';
    LeaveCriticalSection(&g_statusLock);
}


// =============================================================================
//  LOGGING
// =============================================================================

void writeLog(const char* level, const char* message) {
    char logPath[MAX_PATH];
    if (strlen(exeFolder) == 0) return;
    snprintf(logPath, sizeof(logPath), "%s\\DEBUG_LOG.txt", exeFolder);

    // v1.2.1 [H1 FIX]: serialize seluruh rotation + append agar dua thread
    // ga interleave truncate/append.
    EnterCriticalSection(&g_logLock);

    // Log Rotation: jika file log sudah lebih dari 1MB, timpa isinya
    // dari awal (truncate). Mode "w" otomatis mengosongkan file tanpa
    // perlu delete atau rename — lebih simpel dan tidak meninggalkan
    // file sampah DEBUG_LOG_OLD.txt di folder game.
    FILE* fCheck = fopen(logPath, "r");
    if (fCheck) {
        fseek(fCheck, 0, SEEK_END);
        long fSize = ftell(fCheck);
        fclose(fCheck);
        if (fSize > 1024 * 1024) { // > 1MB
            FILE* fClear = fopen(logPath, "w");
            if (fClear) {
                fprintf(fClear, "[LOG] File lama dihapus otomatis (>1MB).\n");
                fclose(fClear);
            }
        }
    }

    FILE* f = fopen(logPath, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond,
            level, message);
        fclose(f);
    }

    LeaveCriticalSection(&g_logLock);
}


// =============================================================================
//  UTILITAS
// =============================================================================

void getExeFolder() {
    GetModuleFileName(NULL, exeFolder, MAX_PATH);
    char* lastSlash = strrchr(exeFolder, '\\');
    if (!lastSlash) lastSlash = strrchr(exeFolder, '/');
    if (lastSlash)  *lastSlash = '\0';
    else            strncpy(exeFolder, ".", MAX_PATH - 1);
}


// =============================================================================
//  INISIALISASI PENGATURAN
// =============================================================================

void InitSettings() {
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%s\\settings.ini", exeFolder);

    if (GetFileAttributes(ini) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = fopen(ini, "w");
        if (f) {
            fprintf(f,
                "[SETTINGS]\n"
                "MUTE=0\n"
                "CLEAN_START_PERCENT=35\n"
                "THRESHOLD_CRISIS=90\n"
                "; PRE_CRISIS_LEVEL: lapisan tengah antara ZRAM Guard dan CRISIS.\n"
                "; Set 0 untuk nonaktifkan Pre-Crisis.\n"
                "; Rekomendasi: 5-7 poin di bawah THRESHOLD_CRISIS.\n"
                "; Contoh: CRISIS=90 → PRE_CRISIS=83\n"
                "PRE_CRISIS_LEVEL=83\n"
                "; INTERVAL_MENIT: jeda routine cleaning (menit). Otomatis jadi\n"
                "; setengahnya kalau RAM > 65%% saat timer masuk.\n"
                "INTERVAL_MENIT=15\n"
                "; ROUTINE_CLEAN_PERCENT: porsi RAM (%% dari total) untuk routine\n"
                "; cleaning ringan tiap INTERVAL_MENIT. Kecil = halus tapi efek\n"
                "; bersih sedikit. Set 0 untuk MATIKAN routine cleaning.\n"
                "; Rekomendasi: 5 (HP RAM kecil) - 10 (umum) - 15 (RAM lega). Maks 30.\n"
                "ROUTINE_CLEAN_PERCENT=10\n"
                "ZRAM_GUARD=1\n"
                "ZRAM_GUARD_LEVEL=75\n"
                "DWYOR_MODE=0\n"
                "; EMULATOR_MODE: 0=Box64 (Winlator), 1=FEXCore (GameHub/BannerHub)\n"
                "; Box64  = safety floor 12%%, sleep 25ms antar chunk\n"
                "; FEXCore = safety floor 15%%, sleep 35ms antar chunk\n"
                "EMULATOR_MODE=0\n"
                ";\n"
                "; CORE_AFFINITY_MASK — Sesuaikan dengan chipset HP kamu!\n"
                "; 0x00 = NONAKTIFKAN Core Pinning (SD 8 Elite/Oryon, semua core setara)\n"
                ";\n"
                "; --- Helio ---\n"
                "; Helio G80/G85/G88/G91/G96/G99          = 0xC0\n"
                ";\n"
                "; --- Dimensity ---\n"
                "; Dimensity 6100/6300/7050/7300           = 0xC0\n"
                "; Dimensity 8200/8300/9200/9300/9400      = 0x80\n"
                ";\n"
                "; --- Snapdragon 6xx ---\n"
                "; SD 662/665/680/685                      = 0xC0\n"
                "; SD 6 Gen 1/Gen 3/Gen 4                  = 0x80\n"
                ";\n"
                "; --- Snapdragon 7xx ---\n"
                "; SD 720G/730G/750G/778G/780G             = 0xC0\n"
                "; SD 7 Gen 1/7+Gen2/7+Gen3                = 0x80\n"
                "; SD 7 Gen 3/7s Gen 2/7s Gen 3            = 0xF0\n"
                ";\n"
                "; --- Snapdragon 8xx ---\n"
                "; SD 845/855/865/870                      = 0x80\n"
                "; SD 888/888+                             = 0x80\n"
                "; SD 8 Gen 1/8+Gen1                       = 0x80\n"
                "; SD 8 Gen 2/8 Gen 3/8s Gen 3             = 0x80\n"
                "; SD 8 Elite (Oryon)                      = 0x00 (nonaktif)\n"
                ";\n"
                "; Tidak tahu chipset? Default 0xC0 (aman untuk Helio)\n"
                "CORE_AFFINITY_MASK=0xC0\n"
            );
            fclose(f);
            writeLog("INFO", "settings.ini dibuat (Winlator-Optimized v1.2).");
        }
    }

    isMuted           = GetPrivateProfileInt("SETTINGS", "MUTE",               0,  ini);
    cleanStartPercent = GetPrivateProfileInt("SETTINGS", "CLEAN_START_PERCENT", 35, ini);

    // Cap di 60%: di atas 60% berisiko LMK memilih Winlator sebagai korban.
    if (cleanStartPercent > 60) {
        cleanStartPercent = 60;
        writeLog("WARNING", "CLEAN_START_PERCENT dibatasi ke 60% untuk keamanan.");
    }

    thresholdCrisis = GetPrivateProfileInt("SETTINGS", "THRESHOLD_CRISIS", 90, ini);
    preCrisisLevel  = GetPrivateProfileInt("SETTINGS", "PRE_CRISIS_LEVEL", 83, ini);
    intervalMenit   = GetPrivateProfileInt("SETTINGS", "INTERVAL_MENIT",   15, ini);

    // B4: porsi routine cleaning (configurable / tweakable). 0 = mati.
    // Clamp 0-30: di atas 30 bukan "routine ringan" lagi, dan berisiko
    // bentrok dengan boost utama. (Backward-compat: kalau settings.ini lama
    // masih punya ROUTINE_METHOD, itu diabaikan — pakai default 10.)
    routineCleanPercent = GetPrivateProfileInt("SETTINGS", "ROUTINE_CLEAN_PERCENT", 10, ini);
    if (routineCleanPercent < 0)  routineCleanPercent = 0;
    if (routineCleanPercent > 30) {
        routineCleanPercent = 30;
        writeLog("WARNING", "ROUTINE_CLEAN_PERCENT dibatasi ke 30% (routine harus ringan).");
    }

    isZramGuard     = GetPrivateProfileInt("SETTINGS", "ZRAM_GUARD",        1,  ini);
    zramGuardLevel  = GetPrivateProfileInt("SETTINGS", "ZRAM_GUARD_LEVEL",  75, ini);
    isDWYOR         = GetPrivateProfileInt("SETTINGS", "DWYOR_MODE",         0,  ini);
    emulatorMode    = GetPrivateProfileInt("SETTINGS", "EMULATOR_MODE",      0,  ini);

    // v1.2.1: range clamp untuk cegah DoS-self & invalid threshold.
    // INTERVAL_MENIT=0 → spam boost tiap WM_TIMER tick.
    // Threshold di luar 0..100 → guard misfire (selalu trigger atau silent skip).
    if (intervalMenit < 1)  { intervalMenit = 1;  writeLog("WARNING", "INTERVAL_MENIT < 1, di-clamp 1."); }
    if (intervalMenit > 60) { intervalMenit = 60; writeLog("WARNING", "INTERVAL_MENIT > 60, di-clamp 60."); }
    if (thresholdCrisis < 50 || thresholdCrisis > 100) {
        thresholdCrisis = 90; writeLog("WARNING", "THRESHOLD_CRISIS invalid, reset 90.");
    }
    if (preCrisisLevel < 0 || preCrisisLevel > 100) {
        preCrisisLevel = 83; writeLog("WARNING", "PRE_CRISIS_LEVEL invalid, reset 83.");
    }
    if (zramGuardLevel < 50 || zramGuardLevel > 95) {
        zramGuardLevel = 75; writeLog("WARNING", "ZRAM_GUARD_LEVEL invalid, reset 75.");
    }

    // Baca CORE_AFFINITY_MASK sebagai string hex lalu convert ke DWORD.
    // Nilai 0x00 = sengaja skip Core Pinning (contoh: SD 8 Elite / Oryon).
    // Nilai 0 dari parsing gagal dibedakan dengan cek string aslinya.
    char maskStr[16] = "0xC0";
    GetPrivateProfileString("SETTINGS", "CORE_AFFINITY_MASK", "0xC0",
        maskStr, sizeof(maskStr), ini);
    affinityMask = (DWORD)strtoul(maskStr, NULL, 16);
    // Fallback ke 0xC0 HANYA jika string tidak bisa di-parse sama sekali
    // (bukan angka valid). Jika user sengaja tulis 0x00, hormati itu.
    if (affinityMask == 0 &&
        maskStr[0] != '0' &&
        maskStr[0] != 'x' &&
        maskStr[0] != 'X') {
        affinityMask = 0xC0;
    }
}


// =============================================================================
//  MESIN BOOST UTAMA (berjalan di thread terpisah via _beginthread)
// =============================================================================

void boostThread(void* param) {
    double target = *(double*)param;
    free(param);

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        writeLog("ERROR", "Gagal membaca status RAM awal. Boost dibatalkan.");
        isBoosting = false;
        return;
    }

    double startMB = (double)ms.ullAvailPhys / 1048576.0;

    // =========================================================================
    //  SAFETY FLOOR DINAMIS: Box64 vs FEXCore
    // =========================================================================
    // Box64 (Winlator): overhead syscall lebih ringan, floor 12% cukup.
    //
    // FEXCore (GameHub): overhead translasi syscall lebih tinggi dari Box64
    // karena FEXCore menerjemahkan setiap instruksi x86 secara JIT yang lebih
    // agresif. Tekanan memori dari Rambooster bersaing dengan JIT cache FEXCore
    // yang juga butuh RAM. Floor 15% memberi ruang napas lebih besar agar
    // JIT cache tidak ikut tersapu dan membuat game stuttering.
    double floorPct   = (emulatorMode == 1) ? 0.15 : 0.12;
    int    chunkSleep = (emulatorMode == 1) ? 35   : 25;

    SIZE_T safetyFloor = (SIZE_T)((double)ms.ullTotalPhys * floorPct);

    // B1: ambil page size asli dari sistem (4KB di kebanyakan device, tapi
    // sebagian ARM modern 16KB). Menyentuh 1 byte per halaman sudah cukup
    // memaksa commit fisik — pakai langkah = page size biar tidak menyentuh
    // halaman yang sama berkali-kali (hemat CPU di device page-besar).
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    SIZE_T pageStep = sysInfo.dwPageSize;
    if (pageStep == 0) pageStep = 4096; // fallback defensif

    int allocatedBlocks = 0; // B3: berapa blok yang BENAR-BENAR teralokasi

    // B4: dulu gerbang ini "routineMethod == 1 || target > 20.0" yang bikin
    // Pre-Crisis (target 20) ikut mati kalau routineMethod=0 — efek samping
    // tersembunyi. Sekarang boostThread cukup hormatin target apa adanya:
    // selama target > 0, alokasi jalan. Porsi tiap lapisan diatur di pemanggil.
    if (target > 0.0) {
        SIZE_T totalToAlloc = (SIZE_T)((double)ms.ullTotalPhys * (target / 100.0));

        // v1.2.1 [H3 FIX]: cap absolute 1.5GB.
        // CRISIS 45% di HP 8GB = 3.6GB → wall-clock ~13 detik saat game paling
        // butuh resource. 1.5GB cukup buat pressure LMK di mayoritas HP +
        // wall-clock ~4-5 detik. Lebih responsif.
        SIZE_T absoluteCap = (SIZE_T)1536 * 1024 * 1024; // 1.5 GiB
        if (totalToAlloc > absoluteCap) {
            char capMsg[96];
            snprintf(capMsg, sizeof(capMsg),
                "Target %.0f%% (%.0f MB) di-cap ke 1536 MB.",
                target, (double)totalToAlloc / 1048576.0);
            writeLog("INFO", capMsg);
            totalToAlloc = absoluteCap;
        }

        SIZE_T bSize = 8 * 1024 * 1024; // 8MB per chunk

        // B2: hitung jumlah slot dari kebutuhan nyata, bukan angka ajaib 6000.
        // +8 margin untuk jaga-jaga kalau bSize mengecil (VirtualAlloc gagal →
        // chunk dibagi 2 → butuh lebih banyak slot dari perkiraan awal).
        SIZE_T maxBlocks = (totalToAlloc / bSize) + 8;
        void** blocks = (void**)malloc(sizeof(void*) * maxBlocks);
        if (blocks) {
            int    count     = 0;
            SIZE_T allocated = 0;

            while (allocated < totalToAlloc && count < (int)maxBlocks) {

                // Cek safety floor setiap 4 blok untuk mengurangi overhead
                // GlobalMemoryStatusEx yang harus melewati translasi Wine → Android.
                if (count % 4 == 0) {
                    MEMORYSTATUSEX msCheck;
                    msCheck.dwLength = sizeof(msCheck);
                    if (GlobalMemoryStatusEx(&msCheck)) {
                        if (msCheck.ullAvailPhys < safetyFloor) {
                            char msg[128];
                            snprintf(msg, sizeof(msg),
                                "Safety floor tercapai (%.0f MB free). "
                                "Alokasi dihentikan di %d blok.",
                                (double)msCheck.ullAvailPhys / 1048576.0, count);
                            writeLog("SAFETY", msg);
                            break;
                        }
                    }
                }

                void* m = VirtualAlloc(NULL, bSize,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!m) {
                    bSize /= 2;
                    if (bSize < 1 * 1024 * 1024) break;
                    continue;
                }

                // ============================================================
                //  DEMAND PAGING FIX — WAJIB UNTUK TEKANAN RAM NYATA
                // ============================================================
                // Linux tidak commit halaman fisik sampai alamat diakses.
                // Menulis hanya [0] = 1 hanya commit 1 halaman dari 8MB blok.
                // Loop ini menyentuh setiap halaman (pageStep) sehingga seluruh
                // blok benar-benar masuk ke RAM fisik dan LMK merasakannya.
                // ============================================================
                for (SIZE_T offset = 0; offset < bSize; offset += pageStep) {
                    ((char*)m)[offset] = 1;
                }

                blocks[count++] = m;
                allocated += bSize;

                // Sleep dinamis: Box64=25ms, FEXCore=35ms.
                // FEXCore butuh jeda lebih panjang karena overhead JIT-nya
                // membuat kernel lebih lambat memproses tekanan memori.
                if (!isDWYOR) Sleep(chunkSleep);
            }

            // Tahan 500ms agar LMK punya waktu:
            // 1. Deteksi threshold terlampaui
            // 2. Pilih korban (background apps, OOM score tinggi)
            // 3. Kirim SIGKILL dan tunggu proses benar-benar exit
            Sleep(500);

            allocatedBlocks = count; // B3: simpan untuk pelaporan

            for (int i = 0; i < count; i++) VirtualFree(blocks[i], 0, MEM_RELEASE);
            free(blocks);
        }
    }

    // Tunggu 1 detik agar kernel settle layout memori sebelum ukur hasil.
    Sleep(1000);

    if (!GlobalMemoryStatusEx(&ms)) {
        writeLog("ERROR", "Gagal membaca status RAM akhir.");
        isBoosting = false;
        return;
    }

    double endMB = (double)ms.ullAvailPhys / 1048576.0;
    double netGain = endMB - startMB;
    char   res[160];

    // B3: laporkan jumlah blok yang BENAR-BENAR teralokasi (kontribusi nyata
    // booster) terpisah dari Net RAM (yang ikut terpengaruh game & sistem lain).
    // Dulu cuma "Gain: X MB" yang menyesatkan — angka itu bercampur noise.
    double pushedMB = (double)allocatedBlocks * 8.0; // tiap blok 8MB
    if (allocatedBlocks == 0)
        snprintf(res, sizeof(res),
            "Boost Selesai (Target %.0f%%) | 0 blok (LMK kehabisan korban) | Net: %+.0f MB",
            target, netGain);
    else
        snprintf(res, sizeof(res),
            "Boost Selesai (Target %.0f%%) | Tekanan: %d blok (~%.0f MB) | Net RAM: %+.0f MB",
            target, allocatedBlocks, pushedMB, netGain);

    writeLog("ACTION", res);

    // B5: lastBoostTick TIDAK lagi di-set di sini. Sekarang di-set di titik
    // keputusan (RunSilentGuard, sebelum spawnBoost) agar cooldown akurat dari
    // saat boost DIPUTUSKAN, bukan saat selesai (yang molor beberapa detik).
    isBoosting = false;
}


// =============================================================================
//  HELPER SPAWN BOOST (FIX A2)
// =============================================================================
// Dulu tiap call site memanggil _beginthread(boostThread, ...) tanpa mengecek
// nilai kembaliannya. Kalau _beginthread GAGAL (return -1), thread tidak pernah
// lahir, sehingga isBoosting (yang sudah di-set true oleh pemanggil) TIDAK
// PERNAH di-reset ke false → seluruh Silent Guard lumpuh permanen sampai app
// di-restart. Helper ini memusatkan pengecekan itu di satu tempat.
//
// PENTING: panggil HANYA setelah isBoosting di-set true (pemanggil pegang lock).
// Return true kalau thread berhasil dibuat; false (dan isBoosting di-reset)
// kalau gagal.
static bool spawnBoost(double pct) {
    double* p = (double*)malloc(sizeof(double));
    if (p == NULL) {
        writeLog("ERROR", "malloc gagal untuk parameter boost. Guard di-reset.");
        isBoosting = false;
        return false;
    }
    *p = pct;
    // boostThread yang akan free(p) saat thread sukses berjalan.
    if (_beginthread(boostThread, 0, p) == (uintptr_t)-1) {
        writeLog("ERROR", "_beginthread gagal. Boost dibatalkan, guard di-reset.");
        free(p); // thread tak pernah lahir → kita yang wajib bebaskan param
        isBoosting = false;
        return false;
    }
    return true;
}


// =============================================================================
//  ZRAM GUARD THREAD (FIX A1 + A3)
// =============================================================================
// A1: Dulu logika ZRAM Guard berjalan INLINE di UI thread (di dalam
//     RunSilentGuard yang dipanggil WM_TIMER). VirtualAlloc 10% RAM + loop
//     sentuh tiap 4KB + Sleep(200) di UI thread = window MEMBEKU tiap trigger.
//     Sekarang dijalankan di thread terpisah, konsisten dengan 3 lapisan lain.
// A3: Dulu ZRAM alokasi 10% TANPA cek safety floor (boostThread punya cek itu).
//     Sekarang ZRAM juga menghormati floor agar tidak ikut memicu OOM.
//
// Tidak butuh parameter — semua dibaca sendiri di dalam thread.
// Mereset isBoosting = false saat selesai (seperti boostThread).
void zramGuardThread(void* param) {
    (void)param;

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        isBoosting = false;
        return;
    }

    int    currentUsage = (int)ms.dwMemoryLoad;
    double before       = (double)ms.ullAvailPhys / 1048576.0;

    // Safety floor mengikuti emulatorMode (Box64 12% / FEXCore 15%) — sama
    // prinsipnya dengan boostThread.
    double floorPct    = (emulatorMode == 1) ? 0.15 : 0.12;
    SIZE_T safetyFloor = (SIZE_T)((double)ms.ullTotalPhys * floorPct);

    // Kalau RAM bebas sudah di/under floor, jangan alokasi apa pun.
    // (lastBoostTick TIDAK di-set di sini — sudah di-set di titik keputusan
    //  saat spawn, sesuai B5.)
    if (ms.ullAvailPhys <= safetyFloor) {
        setGuardStatusF("ZRAM Guard | skip (RAM %d%%, low)", currentUsage);
        writeLog("GUARD", "ZRAM Guard dilewati — RAM sudah di bawah safety floor.");
        isBoosting = false;
        return;
    }

    // B1: page size asli dari sistem (lihat catatan di boostThread).
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    SIZE_T pageStep = sysInfo.dwPageSize;
    if (pageStep == 0) pageStep = 4096;

    SIZE_T triggerSize = (SIZE_T)((double)ms.ullTotalPhys * 0.10);
    // Batasi agar alokasi tidak menembus floor (sisakan ruang aman).
    SIZE_T maxSafe = (SIZE_T)(ms.ullAvailPhys - safetyFloor);
    if (triggerSize > maxSafe) triggerSize = maxSafe;

    void* tempBlock = VirtualAlloc(NULL, triggerSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (tempBlock) {
        // Sentuh tiap halaman agar tekanan nyata secara fisik.
        for (SIZE_T offset = 0; offset < triggerSize; offset += pageStep) {
            ((char*)tempBlock)[offset] = 1;
        }
        Sleep(200);
        VirtualFree(tempBlock, 0, MEM_RELEASE);
    }

    GlobalMemoryStatusEx(&ms);
    double gain = ((double)ms.ullAvailPhys / 1048576.0) - before;

    if (gain > 5.0) {
        char z[128];
        snprintf(z, sizeof(z),
            "ZRAM Guard (RAM: %d%%) | Cleaned: %.0f MB", currentUsage, gain);
        writeLog("GUARD", z);
        setGuardStatusF("ZRAM Guard | +%.0f MB", gain);
    } else {
        setGuardStatusF("ZRAM Guard | RAM %d%%", currentUsage);
    }

    isBoosting = false; // lastBoostTick sudah di-set saat spawn (B5)
}


// =============================================================================
//  SILENT GUARD (dipanggil WM_TIMER setiap 2 detik)
// =============================================================================

void RunSilentGuard(HWND hwnd) {
    if (isBoosting.exchange(true)) return;

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        isBoosting = false;
        return;
    }

    int   currentUsage = (int)ms.dwMemoryLoad;
    DWORD now          = GetTickCount();

    // =========================================================================
    //  TUGAS 0: THRESHOLD_CRISIS — PRIORITAS TERTINGGI
    // =========================================================================
    // Give Up Mechanism: jika CRISIS terpicu 2x berturut-turut tanpa RAM
    // kembali normal di antaranya, program berhenti mencoba selama 2 menit.
    // Ini mencegah CRISIS spam seperti di log Sleeping Dogs di mana 4x
    // terpicu berturut-turut tapi 0 blok teralokasi — situasi di mana
    // LMK Android sudah kehabisan korban dan boost tidak bisa membantu lebih.
    // RAM sudah di bawah crisis threshold → reset streak.
    if (currentUsage < thresholdCrisis) {
        crisisStreak = 0;
    }

    if (currentUsage >= thresholdCrisis &&
        (now - lastBoostTick > 30000) &&
        (now > crisisBackoff))
    {
        crisisStreak++;

        if (crisisStreak >= 2) {
            // Dua CRISIS berturut-turut — beri jeda 2 menit
            // sebelum mencoba lagi agar tidak spam sia-sia
            crisisBackoff = now + 120000;
            crisisStreak  = 0;
            setGuardStatusF("Crisis: Backoff 2 menit");
            writeLog("CRITICAL", "CRISIS 2x berturut-turut. Backoff 2 menit.");
            isBoosting = false;
            return;
        }

        char cLog[128];
        snprintf(cLog, sizeof(cLog),
            "CRISIS ALERT! RAM %d%% >= threshold %d%%. Boost barbar 45%%! (streak: %d)",
            currentUsage, thresholdCrisis, crisisStreak);
        writeLog("CRITICAL", cLog);
        setGuardStatusF("CRISIS! RAM %d%%", currentUsage);
        lastBoostTick = now; // B5: set di titik keputusan, bukan di akhir thread
        spawnBoost(45.0);    // reset isBoosting sendiri kalau gagal
        return;
    }

    // Jika sedang dalam backoff, tampilkan status dan skip
    if (now <= crisisBackoff) {
        DWORD sisaDetik = (crisisBackoff - now) / 1000;
        setGuardStatusF("Crisis Backoff: %ds", (int)sisaDetik);
        isBoosting = false;
        return;
    }

    // =========================================================================
    //  TUGAS 0.5: PRE-CRISIS — LAPISAN TENGAH (BARU)
    // =========================================================================
    // Bekerja di zona 83-89% (antara ZRAM Guard dan CRISIS).
    // Tujuannya MENCEGAH CRISIS terpicu, bukan menambah boost baru.
    //
    // MENGAPA INI TIDAK MEMPERBURUK PERFORMA:
    // Tanpa Pre-Crisis: RAM 75% → ZRAM Guard → RAM naik lagi → 90% → CRISIS 45%
    //                  → CPU spike besar saat game paling butuh resource
    // Dengan Pre-Crisis: RAM 75% → ZRAM Guard → 83% → Pre-Crisis 20% → RAM turun
    //                  → CRISIS tidak pernah terpicu → tidak ada CPU spike besar
    //
    // Pre-Crisis 20% lebih ringan dari CRISIS 45%, dan mencegah yang lebih berat.
    // Net effect: CPU load LEBIH RENDAH dibanding tanpa Pre-Crisis.
    //
    // Cooldown 60 detik: lebih panjang dari CRISIS (30s) karena ini bukan darurat.
    // Set PRE_CRISIS_LEVEL=0 di settings.ini untuk nonaktifkan.
    if (preCrisisLevel > 0 &&
        currentUsage >= preCrisisLevel &&
        currentUsage < thresholdCrisis &&
        (now - lastPreCrisisTick > 60000))
    {
        lastPreCrisisTick = now;
        lastBoostTick     = now; // B5: jaga cooldown ZRAM tetap akurat
        char pcLog[128];
        snprintf(pcLog, sizeof(pcLog),
            "Pre-Crisis! RAM %d%% >= %d%%. Boost preventif 20%%.",
            currentUsage, preCrisisLevel);
        writeLog("PRE-CRISIS", pcLog);
        setGuardStatusF("Pre-Crisis | RAM %d%%", currentUsage);
        spawnBoost(20.0);
        return;
    }

    // =========================================================================
    //  TUGAS 1: ROUTINE CLEANING
    // =========================================================================
    // Adaptive interval: jika RAM sudah di atas 65% saat timer masuk,
    // efektifkan interval menjadi setengahnya untuk sesi ini saja.
    // Contoh: INTERVAL_MENIT=15, RAM=70% → efektif jadi 7.5 menit.
    // Ini tidak mengubah nilai settings — hanya logika timer lokal.
    // B4: routine cleaning cuma jalan kalau porsinya > 0 (user bisa matikan
    // dengan ROUTINE_CLEAN_PERCENT=0). Porsi sekarang configurable, bukan 20% mati.
    DWORD effectiveInterval = (DWORD)(intervalMenit * 60000);
    if (currentUsage > 65) effectiveInterval /= 2;

    if (routineCleanPercent > 0 && now - lastRoutineTick >= effectiveInterval) {
        lastRoutineTick = now;
        lastBoostTick   = now; // B5: jaga cooldown ZRAM tetap akurat
        char rLog[80];
        snprintf(rLog, sizeof(rLog),
            "Cleaning cycle (RAM: %d%%, porsi: %d%%, interval: %ds).",
            currentUsage, routineCleanPercent, (int)(effectiveInterval / 1000));
        writeLog("ROUTINE", rLog);
        setGuardStatusF("Routine Clean %d%% | RAM %d%%", routineCleanPercent, currentUsage);
        spawnBoost((double)routineCleanPercent);
        return;
    }

    // =========================================================================
    //  TUGAS 2: ZRAM GUARD
    // =========================================================================
    if (isZramGuard &&
        currentUsage > zramGuardLevel &&
        (now - lastBoostTick > 45000))
    {
        // FIX A1: jalankan ZRAM Guard di thread terpisah (dulu inline di UI
        // thread → window freeze). Set lastBoostTick di sini (titik keputusan)
        // agar cooldown 45s akurat walau kerja sebenarnya async.
        lastBoostTick = now;
        // FIX A2: cek _beginthread; kalau gagal, reset isBoosting biar tak nyangkut.
        if (_beginthread(zramGuardThread, 0, NULL) == (uintptr_t)-1) {
            writeLog("ERROR", "_beginthread ZRAM gagal. Guard di-reset.");
            isBoosting = false;
        }
        // Sukses → zramGuardThread yang akan mereset isBoosting saat selesai.
        return;
    }

    // Idle — tidak ada guard yang terpicu.
    setGuardStatusF("Idle | RAM %d%%", currentUsage);
    isBoosting = false;
}


// =============================================================================
//  WIN32 WINDOW PROCEDURE
// =============================================================================

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

        case WM_CREATE: {
            // v1.2.1: init CRITICAL_SECTIONs SEBELUM apapun yg bisa nge-log
            // atau update guardStatus. getExeFolder & InitSettings di bawah
            // memanggil writeLog → wajib g_logLock siap.
            InitializeCriticalSection(&g_statusLock);
            InitializeCriticalSection(&g_logLock);

            hBg = CreateSolidBrush(MY_COLOR_BG);

            getExeFolder();
            InitSettings();
            writeLog("INFO", emulatorMode == 1
                ? "Mode: FEXCore (GameHub/BannerHub) — floor 15%, sleep 35ms"
                : "Mode: Box64 (Winlator) — floor 12%, sleep 25ms");
            lastRoutineTick = GetTickCount();

            // v1.2.1 [M3 FIX]: simpan ke global agar bisa DeleteObject di WM_DESTROY.
            hFont = CreateFont(18, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                1, 0, 0, 3, 2, "Segoe UI");

            HWND hTitle = CreateWindow("STATIC", "RAMBOOSTER v1.2 Winlator",
                WS_VISIBLE | WS_CHILD | SS_CENTER,
                20, 15, 340, 30, hwnd, NULL, NULL, NULL);
            SendMessage(hTitle, WM_SETFONT, (WPARAM)hFont, TRUE);

            hStatusLabel = CreateWindow("STATIC", "Ready to Guard...",
                WS_VISIBLE | WS_CHILD | SS_CENTER,
                20, 50, 340, 20, hwnd, NULL, NULL, NULL);
            SendMessage(hStatusLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

            CreateWindow("BUTTON", "SELECT GAME",
                WS_VISIBLE | WS_CHILD,
                100, 85, 180, 35, hwnd, (HMENU)1, NULL, NULL);

            CreateWindow("BUTTON", "BOOST & LAUNCH",
                WS_VISIBLE | WS_CHILD,
                100, 130, 180, 35, hwnd, (HMENU)2, NULL, NULL);

            HWND hWatermark = CreateWindow("STATIC", "ModBy: Noysz",
                WS_VISIBLE | WS_CHILD | SS_CENTER,
                20, 210, 340, 20, hwnd, NULL, NULL, NULL);
            SendMessage(hWatermark, WM_SETFONT, (WPARAM)hFont, TRUE);

            if (!SetTimer(hwnd, TIMER_MAIN, 2000, NULL)) {
                writeLog("CRITICAL", "Gagal membuat Timer. Silent Guard lumpuh.");
            }
            break;
        }

        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)wp, COLOR_TEXT);
            SetBkColor((HDC)wp, MY_COLOR_BG);
            return (LRESULT)hBg;

        case WM_COMMAND:
            // --- Tombol 1: SELECT GAME ---
            if (LOWORD(wp) == 1) {
                OPENFILENAME ofn   = {sizeof(ofn)};
                char szF[MAX_PATH] = "";
                ofn.hwndOwner   = hwnd;
                ofn.lpstrFile   = szF;
                ofn.nMaxFile    = MAX_PATH;
                ofn.lpstrFilter = "Executables\0*.exe\0";
                ofn.Flags       = OFN_FILEMUSTEXIST;
                if (GetOpenFileName(&ofn)) {
                    strncpy(gamePath, szF, MAX_PATH - 1);
                    gamePath[MAX_PATH - 1] = '\0';
                }
            }

            // --- Tombol 2: BOOST & LAUNCH ---
            if (LOWORD(wp) == 2 && strlen(gamePath) > 0) {
                InitSettings();

                if (!isBoosting.exchange(true)) {
                    lastRoutineTick = GetTickCount();
                    lastBoostTick   = GetTickCount(); // B5: tandai boost terakhir
                                                      // biar guard tak langsung trigger setelah launch

                    if (!isMuted)
                        PlaySound(MAKEINTRESOURCE(301),
                            GetModuleHandle(NULL),
                            SND_RESOURCE | SND_ASYNC);

                    // FIX A2: spawnBoost menangani gagalnya malloc & _beginthread,
                    // dan mereset isBoosting bila gagal. Boost bersifat best-effort —
                    // game tetap diluncurkan di bawah walau boost gagal.
                    spawnBoost((double)cleanStartPercent);

                    // Ekstrak working directory dari gamePath.
                    // Ini WAJIB untuk game seperti GTA V yang menggunakan
                    // PlayGTAV.exe / launcher terpisah — game mencari file
                    // aset (*.rpf, config, dll) secara RELATIF terhadap
                    // folder tempatnya berada. Kalau working directory salah
                    // (misal folder Rambooster), game langsung crash saat launch.
                    // Dengan meng-set workDir ke folder game, perilakunya
                    // identik dengan membuka .exe langsung dari file manager.
                    char workDir[MAX_PATH];
                    strncpy(workDir, gamePath, MAX_PATH - 1);
                    workDir[MAX_PATH - 1] = '\0';
                    char* lastSep = strrchr(workDir, '\\');
                    if (!lastSep) lastSep = strrchr(workDir, '/');
                    if (lastSep) *lastSep = '\0';
                    else         strncpy(workDir, ".", MAX_PATH - 1);

                    // [H2 + SEC FIX] Dua bug di satu call site:
                    //  H2: CreateProcess boleh MEMODIFIKASI lpCommandLine in-place
                    //      (MSDN). Dulu gamePath (global) dioper langsung → bisa
                    //      ke-corrupt setelah launch pertama. Sekarang pakai copy
                    //      lokal yang writable.
                    //  SEC: lpApplicationName=NULL + path tak di-quote = unquoted
                    //      path hijack — "C:\Program Files\g.exe" coba "C:\Program.exe"
                    //      dulu. Set lpApplicationName=gamePath (path eksak, TANPA
                    //      parsing spasi) mematikan hijack; lpCommandLine di-quote
                    //      untuk argv[0] yang benar.
                    char cmdLine[MAX_PATH + 2];
                    snprintf(cmdLine, sizeof(cmdLine), "\"%s\"", gamePath);

                    STARTUPINFO        si = {sizeof(si)};
                    PROCESS_INFORMATION pi;
                    if (CreateProcess(gamePath, cmdLine, NULL, NULL, FALSE,
                        NORMAL_PRIORITY_CLASS, NULL, workDir, &si, &pi))
                    {
                        char launchMsg[MAX_PATH + 32];
                        snprintf(launchMsg, sizeof(launchMsg),
                            "Game launched. WorkDir: %s", workDir);
                        writeLog("LAUNCH", launchMsg);

                        // Core Pinning — nilai mask dari CORE_AFFINITY_MASK settings.ini.
                        // 0x00 = sengaja dinonaktifkan (contoh: SD 8 Elite/Oryon
                        // yang semua core-nya setara, pinning justru kontraproduktif).
                        if (affinityMask != 0) {
                            if (SetProcessAffinityMask(pi.hProcess, affinityMask)) {
                                char pinLog[64];
                                snprintf(pinLog, sizeof(pinLog),
                                    "Core Pinning 0x%02X berhasil diterapkan ke Game!",
                                    (unsigned int)affinityMask);
                                writeLog("TWEAK", pinLog);
                            } else {
                                writeLog("WARNING",
                                    "Core Pinning gagal. Android mungkin override.");
                            }
                        } else {
                            writeLog("INFO",
                                "Core Pinning dinonaktifkan (0x00). Semua core bebas.");
                        }

                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                    } else {
                        writeLog("ERROR", "Gagal meluncurkan game.");
                    }

                    ShowWindow(hwnd, SW_MINIMIZE);
                }
            }
            break;

        case WM_TIMER: {
            MEMORYSTATUSEX ms;
            ms.dwLength = sizeof(ms);
            if (GlobalMemoryStatusEx(&ms)) {
                // v1.2.1 [C1 FIX]: baca guardStatus via lock.
                char statusCopy[64];
                getGuardStatus(statusCopy, sizeof(statusCopy));
                char buf[160];
                snprintf(buf, sizeof(buf), "RAM: %d%% | Free: %.0f MB | %s",
                    (int)ms.dwMemoryLoad,
                    (double)ms.ullAvailPhys / 1048576.0,
                    statusCopy);
                SetWindowText(hStatusLabel, buf);
            }
            RunSilentGuard(hwnd);
            break;
        }

        case WM_DESTROY:
            // KillTimer dulu sebelum apapun — mencegah WM_TIMER masuk
            // saat hStatusLabel sudah tidak valid.
            KillTimer(hwnd, TIMER_MAIN);

            // Hapus semua GDI resource secara eksplisit.
            // hBg: brush static controls
            // hBgClass: brush window class background
            if (hBg)      { DeleteObject(hBg);      hBg      = NULL; }
            if (hBgClass) { DeleteObject(hBgClass);  hBgClass = NULL; }

            // v1.2.1 [M3 FIX]: cleanup font.
            if (hFont)    { DeleteObject(hFont);    hFont    = NULL; }

            // [C2 FIX] Drain worker thread sebelum delete CRITICAL_SECTION.
            // Di Wine/ntdll, DeleteCriticalSection saat thread lain BLOCKED di
            // EnterCriticalSection bisa corrupt futex state → SIGABRT di worker
            // yang masih jalan (bukan sekadar "OS cleanup" seperti asumsi lama).
            // Spin-wait dgn cap keras 3 detik: worst-case exit molor 3s, tapi
            // mencegah crash. isBoosting di-reset worker saat selesai.
            {
                DWORD drainDeadline = GetTickCount() + 3000;
                while (isBoosting.load() && GetTickCount() < drainDeadline)
                    Sleep(50);
            }
            DeleteCriticalSection(&g_statusLock);
            DeleteCriticalSection(&g_logLock);

            PostQuitMessage(0);
            break;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}


// =============================================================================
//  ENTRY POINT
// =============================================================================

int WINAPI WinMain(HINSTANCE hI, HINSTANCE hP, LPSTR a, int s) {
    WNDCLASSEX wc    = {0};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hI;

    // Simpan ke hBgClass agar bisa dihapus di WM_DESTROY.
    // Sebelumnya brush ini di-inline langsung ke wc.hbrBackground
    // sehingga tidak pernah dihapus = GDI leak sejak v1.0.
    hBgClass = CreateSolidBrush(MY_COLOR_BG);
    wc.hbrBackground = hBgClass;

    wc.lpszClassName = "RamBoosterV12";
    wc.hIcon         = LoadIcon(hI, MAKEINTRESOURCE(201));
    RegisterClassEx(&wc);

    HWND h = CreateWindow(
        "RamBoosterV12",
        "RAMBOOSTER v1.2 | ModBy: Noysz",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
        100, 100, 400, 320,
        NULL, NULL, hI, NULL);

    ShowWindow(h, s);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    return 0;
}
