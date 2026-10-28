#ifdef MSR_STUB
extern "C" void* msr_wrap(const char*, void* real) { return real; }
#else
// MuseRangeShim: a drop-in MuseSamplerCoreLib.dll proxy for MuseScore Studio.
//
// MuseScore loads the Muse Sounds engine from the path in MUSESAMPLER_PATH.
// launch.cmd points that at this DLL, which loads the real engine and
// forwards every call. Notes that the engine cannot play (above the sampled
// range of the instrument/articulation) are routed to a hidden "shadow"
// sampler instance transposed down, and that sampler's audio is pitch-shifted
// back up with Signalsmith Stretch and mixed into the output.
//
// Range data comes from ranges.ini (written by msr-probe.exe). Instruments
// without data are passed through untouched.

#include <atomic>
#include <io.h>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <memory>
#include <unordered_map>
#include <set>
#include <algorithm>

#include "common.h"
#include "signalsmith-stretch/signalsmith-stretch.h"
#include "probe_core.h"

using namespace msr;

#ifdef MSR_INTREE
// Built into MuseScore itself: the engine is loaded by MuseScore as usual and
// MuseSamplerLibHandler routes its function lookups through msr_wrap() below.
#define EXPORT static
#else
#define EXPORT extern "C" __declspec(dllexport)
#endif

// ---------------------------------------------------------------------------
// Locks

struct CritSec {
    CRITICAL_SECTION cs;
    CritSec() { InitializeCriticalSectionAndSpinCount(&cs, 2000); }
    ~CritSec() { DeleteCriticalSection(&cs); }
    void lock() { EnterCriticalSection(&cs); }
    void unlock() { LeaveCriticalSection(&cs); }
};

struct Guard {
    CritSec& c;
    explicit Guard(CritSec& c_) : c(c_) { c.lock(); }
    ~Guard() { c.unlock(); }
};

// ---------------------------------------------------------------------------
// Logging

static CritSec g_logCs;
static FILE* g_log = nullptr;
static int g_logLevel = 1;

static void logf(int level, const char* fmt, ...)
{
    if (level > g_logLevel || !g_log) return;
    Guard g(g_logCs);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            (unsigned long)GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// Open the log so other programs (launch.cmd, Notepad) can read it while MuseScore runs.
static FILE* openSharedLog(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    int fd = _open_osfhandle((intptr_t)h, 0);
    if (fd < 0) {
        CloseHandle(h);
        return nullptr;
    }
    return _fdopen(fd, "w");
}

static std::wstring g_statusPath;
// One-word status for launch.cmd, written to its own small file and closed at once.
static void writeStatus(const char* status)
{
    if (g_statusPath.empty()) return;
    FILE* f = openFile(g_statusPath, L"w");
    if (!f) return;
    fprintf(f, "%s\n", status);
    fclose(f);
}

#define LOGI(...) logf(1, __VA_ARGS__)
#define LOGD(...) logf(2, __VA_ARGS__)
#define LOGE(...) logf(0, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Real library

#define MS_FUNCS(X) \
    X(ms_get_version_major) X(ms_get_version_minor) X(ms_get_version_revision) X(ms_get_version_build_number) \
    X(ms_init) X(ms_init_2) X(ms_deinit) X(ms_disable_reverb) \
    X(ms_get_instrument_list) X(ms_get_matching_instrument_list) X(ms_InstrumentList_get_next) \
    X(ms_Instrument_get_id) X(ms_Instrument_get_name) X(ms_Instrument_get_category) X(ms_Instrument_get_pack_name) \
    X(ms_Instrument_get_vendor_name) X(ms_Instrument_get_info_json) X(ms_Instrument_get_musicxml_sound) \
    X(ms_Instrument_get_mpe_sound) X(ms_Instrument_get_reverb_level) X(ms_Instrument_get_preset_list) \
    X(ms_Instrument_is_online) X(ms_PresetList_get_next) X(ms_get_text_articulations) X(ms_get_drum_mapping) \
    X(ms_reload_all_instruments) X(ms_set_logging_callback) \
    X(ms_MuseSampler_create) X(ms_MuseSampler_destroy) X(ms_MuseSampler_init) X(ms_MuseSampler_init_2) \
    X(ms_MuseSampler_add_track) X(ms_MuseSampler_finalize_track) X(ms_MuseSampler_clear_track) \
    X(ms_MuseSampler_add_track_note_event_5) X(ms_MuseSampler_add_track_note_event_6) \
    X(ms_MuseSampler_add_track_dynamics_event_2) X(ms_MuseSampler_add_track_pedal_event_2) \
    X(ms_MuseSampler_is_ranged_articulation) X(ms_MuseSampler_add_track_event_range_start) \
    X(ms_MuseSampler_add_track_event_range_end) X(ms_MuseSampler_add_pitch_bend) X(ms_MuseSampler_add_vibrato) \
    X(ms_MuseSampler_add_track_syllable_event) X(ms_MuseSampler_add_track_syllable_event_2) \
    X(ms_MuseSampler_add_track_text_articulation_event) X(ms_MuseSampler_create_preset_change) \
    X(ms_MuseSampler_add_preset) X(ms_MuseSampler_start_audition_note_4) X(ms_MuseSampler_start_audition_note_5) \
    X(ms_MuseSampler_stop_audition_note) X(ms_MuseSampler_add_audition_cc_event) \
    X(ms_MuseSampler_start_offline_mode) X(ms_MuseSampler_stop_offline_mode) X(ms_MuseSampler_process_offline) \
    X(ms_MuseSampler_process) X(ms_MuseSampler_set_position) X(ms_MuseSampler_set_playing) \
    X(ms_MuseSampler_all_notes_off) X(ms_MuseSampler_ready_to_play) X(ms_MuseSampler_set_score_id) \
    X(ms_MuseSampler_set_auto_render_interval) X(ms_MuseSampler_trigger_render) \
    X(ms_MuseSampler_clear_online_cache) X(ms_MuseSampler_set_lazy_render) \
    X(ms_MuseSampler_set_rendering_state_changed_callback) X(ms_MuseSampler_set_rendering_state_changed_callback_2) \
    X(ms_RenderProgressInfo_get_next) X(ms_RenderProgressInfo2_get_next)

struct RealLib {
    HMODULE h = nullptr;
#define DECL(n) n##_t n = nullptr;
    MS_FUNCS(DECL)
#undef DECL
};

static RealLib R;

// ---------------------------------------------------------------------------
// Configuration

struct Config {
    bool enabled = true;
    bool octaveMode = true;   // false = "nearest": shift the top sample by the exact interval
    int maxShiftUp = 24;
    bool extendDown = false;
    int maxShiftDown = 12;
    bool legatoPhrases = true;
    double shifterBlockMs = 60.0;
    bool formant = false;
    double tonalityLimitHz = 0.0;
    float gain = 1.0f;
    double idleSeconds = 3.0;
    double staccatoScale = 1.0; // multiply written length of staccato(issimo) notes
    int legatoMaxLeap = 0;      // >0: no legato transition for leaps larger than this (semitones)
    bool legatoMaxLeapSoloOnly = true;
    long long runGapUs = 30000;    // notes this close count as one continuous run
    long long fastNoteUs = 160000; // notes this short can join a shifted run
    long long runJoinUs = 300000;  // ...if they are within this distance of an out-of-range note
    bool trillOctave = false;      // optional: shifted trills are played a whole octave down
    long long trillNoteUs = 80000; // shifted notes this short with the trill mark = a written-out trill
    int trillWrittenOut = 0;       // 1: semitone trills, 2: all trills -> plain slurred notes (exact interval)
    long long trillWrittenOutMaxUs = 250000; // longest single trill note considered part of a trill
    bool trillPriority = true;     // trill notes: drop accent/tenuto/marcato/staccato marks so the trill wins
    long long trillGapUs = 30000;  // gap between two back-to-back trills on different notes
    bool tremoloChords = true;     // chord tremolos: each chord note on its own engine instance
    int tremoloMaxLanes = 6;       // most notes of one tremolo chord that get their own instance
    bool tremoloBalance = true;    // each chord note at 1/sqrt(N) so the chord sums to one section's level
    float tremoloGain = 1.0f;      // extra gain on chord-tremolo notes (tremolo_chord_gain_db)
    bool refinalize = true;        // re-finalize an instance's other tracks after a track is added to it
    int tremoloDynSteps = -1;      // tremolo notes get the score's dynamic this many markings softer (0 = off)
    // Diagnostics: for instruments whose "pack|name" contains debugInst, log what every instance was sent
    // between debugFromUs and debugToUs (notes and dynamics), when the score is loaded.
    std::string debugInst;
    long long debugFromUs = -1, debugToUs = -1;
};

static Config g_cfg;
static std::wstring g_dir;
static std::map<std::string, InstRange> g_rangesByKey; // lower-case "pack|name"
static std::map<std::string, double> g_muteGainByKey; // "pack|name" -> linear gain while muted (con sord.)
static std::map<std::string, int> g_trillCentsByKey;  // "pack|name" -> pitch correction for trill notes (cents)
static std::map<int, InstRange> g_rangesById;
static std::string g_rangesLibVersion;
static std::string g_engineVer;

static CritSec g_instCs;
static std::unordered_map<int, std::string> g_instKeyById; // from the engine's instrument list
static bool g_instEnumerated = false;

static std::atomic<long long> g_synthId { (1LL << 52) };

static std::wstring moduleDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&moduleDir, &self);
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH * 4);
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static void loadConfigAndRanges()
{
    Ini cfg;
    cfg.load(g_dir + L"\\config.ini");
    g_logLevel = (int)cfg.geti("general", "log", 1);
    g_cfg.enabled = cfg.geti("general", "enabled", 1) != 0;
    g_cfg.octaveMode = lower(cfg.get("general", "mode", "octave")) != "nearest";
    g_cfg.maxShiftUp = (int)std::max<long long>(1, cfg.geti("general", "max_shift_up", 24));
    g_cfg.extendDown = cfg.geti("general", "extend_down", 0) != 0;
    g_cfg.maxShiftDown = (int)std::max<long long>(1, cfg.geti("general", "max_shift_down", 12));
    g_cfg.legatoPhrases = cfg.geti("general", "legato_phrases", 1) != 0;
    g_cfg.shifterBlockMs = std::min(250.0, std::max(20.0, cfg.getd("general", "shifter_block_ms", 60.0)));
    g_cfg.formant = cfg.geti("general", "formant_preserve", 0) != 0;
    g_cfg.tonalityLimitHz = cfg.getd("general", "tonality_limit_hz", 0.0);
    g_cfg.gain = (float)std::pow(10.0, cfg.getd("general", "gain_db", 0.0) / 20.0);
    g_cfg.staccatoScale = std::min(1.0, std::max(0.05, cfg.getd("general", "staccato_scale", 1.0)));
    g_cfg.legatoMaxLeap = (int)std::max<long long>(0, cfg.geti("general", "legato_max_leap", 0));
    g_cfg.legatoMaxLeapSoloOnly = cfg.geti("general", "legato_max_leap_solo_only", 1) != 0;
    g_cfg.runGapUs = 1000 * std::max<long long>(0, cfg.geti("general", "run_gap_ms", 30));
    g_cfg.fastNoteUs = 1000 * std::max<long long>(0, cfg.geti("general", "fast_note_ms", 160));
    g_cfg.runJoinUs = 1000 * std::max<long long>(0, cfg.geti("general", "run_join_ms", 300));
    g_cfg.trillOctave = cfg.geti("general", "trill_octave", 0) != 0;
    g_cfg.trillNoteUs = 1000 * std::max<long long>(0, cfg.geti("general", "trill_note_ms", 80));
    g_cfg.trillWrittenOut = (int)std::min<long long>(2, std::max<long long>(0, cfg.geti("general", "trill_written_out", 0)));
    g_cfg.trillPriority = cfg.geti("general", "trill_priority", 1) != 0;
    g_cfg.trillGapUs = 1000 * std::max<long long>(0, std::min<long long>(200, cfg.geti("general", "trill_gap_ms", 30)));
    g_cfg.tremoloChords = cfg.geti("general", "tremolo_chords", 1) != 0;
    g_cfg.tremoloMaxLanes = (int)std::max<long long>(2, std::min<long long>(12, cfg.geti("general", "tremolo_max_notes", 6)));
    g_cfg.tremoloBalance = cfg.geti("general", "tremolo_chord_balance", 1) != 0;
    g_cfg.refinalize = cfg.geti("general", "refinalize_instances", 1) != 0;
    g_cfg.tremoloDynSteps = (int)std::max<long long>(-4, std::min<long long>(2, cfg.geti("general", "tremolo_dynamic_steps", -1)));
    g_cfg.debugInst = lower(cfg.get("general", "debug_instrument", ""));
    g_cfg.debugFromUs = (long long)(cfg.getd("general", "debug_from_s", -1.0) * 1e6);
    g_cfg.debugToUs = (long long)(cfg.getd("general", "debug_to_s", -1.0) * 1e6);
    if (!g_cfg.debugInst.empty())
        LOGI("config: DEBUG report for instruments matching \"%s\" from %.2fs to %.2fs", g_cfg.debugInst.c_str(), g_cfg.debugFromUs / 1e6, g_cfg.debugToUs / 1e6);
    g_cfg.tremoloGain = (float)std::pow(10.0, std::max(-40.0, std::min(12.0, cfg.getd("general", "tremolo_chord_gain_db", 0.0))) / 20.0);

    Ini rng;
    if (!rng.load(g_dir + L"\\ranges.ini")) {
        LOGE("ranges.ini not found in the shim folder: run msr-probe.exe first. Passing everything through.");
    }
    g_rangesLibVersion = rng.get("meta", "lib_version");
    for (const auto& s : rng.order) {
        if (s == "meta" || s.empty()) continue;
        InstRange r;
        applyRangeSection(r, rng.sec[s]);
        g_rangesByKey[s] = r;
    }
    // config.ini sections named like ranges.ini sections override / extend them
    for (const auto& s : cfg.order) {
        if (s == "general" || s.empty()) continue;
        InstRange& r = g_rangesByKey[s];
        applyRangeSection(r, cfg.sec[s]);
        int tc = (int)cfg.geti(s, "trill_cents", 0);
        if (tc != 0) {
            g_trillCentsByKey[lower(s)] = std::max(-100, std::min(100, tc));
            LOGI("config: %s trill notes %+d cents", s.c_str(), tc);
        }
        double gdb = cfg.getd(s, "con_sord_gain_db", 0.0);
        if (gdb != 0.0) {
            g_muteGainByKey[lower(s)] = std::pow(10.0, std::max(-40.0, std::min(24.0, gdb)) / 20.0);
            LOGI("config: %s con sord. gain %+.1f dB", s.c_str(), gdb);
        }
    }
    for (const auto& p : g_rangesByKey) {
        if (p.second.id >= 0) g_rangesById[p.second.id] = p.second;
    }
    LOGI("config: staccato_scale=%.2f legato_max_leap=%d (solo only %d) trill_written_out=%d trill_priority=%d trill_gap_ms=%lld tremolo_chords=%d balance=%d gain=%.2f dynamic_steps=%d refinalize_instances=%d", g_cfg.staccatoScale, g_cfg.legatoMaxLeap, (int)g_cfg.legatoMaxLeapSoloOnly, g_cfg.trillWrittenOut, (int)g_cfg.trillPriority, g_cfg.trillGapUs / 1000, (int)g_cfg.tremoloChords, (int)g_cfg.tremoloBalance, g_cfg.tremoloGain, g_cfg.tremoloDynSteps, (int)g_cfg.refinalize);
    LOGI("config: enabled=%d mode=%s max_up=%d extend_down=%d legato_phrases=%d block_ms=%.0f formant=%d; %d instruments in ranges.ini (probed with lib %s)",
         (int)g_cfg.enabled, g_cfg.octaveMode ? "octave" : "nearest", g_cfg.maxShiftUp, (int)g_cfg.extendDown,
         (int)g_cfg.legatoPhrases, g_cfg.shifterBlockMs, (int)g_cfg.formant, (int)g_rangesByKey.size(),
         g_rangesLibVersion.empty() ? "?" : g_rangesLibVersion.c_str());
}

static void engineLogCb(ms_ErrorLevel level, const char* msg);

static BOOL CALLBACK initOnceFn(PINIT_ONCE, PVOID, PVOID*)
{
#ifdef MSR_INTREE
    g_dir = envPath(L"MSR_DIR");
    if (g_dir.empty()) g_dir = envPath(L"LOCALAPPDATA") + L"\\MuseRangeShim";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_log = openSharedLog(g_dir + L"\\shim.log");
    g_statusPath = g_dir + L"\\shim_status.txt";
    writeStatus("loaded");
    LOGE("STATUS loaded");
#ifdef MSR_EXTDLL
    LOGI("MuseRangeShim external DLL (ext\\MuseRangeShim.dll, build %s); folder %s", MSR_EXT_BUILD, narrow(g_dir).c_str());
#else
    LOGI("MuseRangeShim built into MuseScore (in-tree mode); folder %s", narrow(g_dir).c_str());
#endif
    loadConfigAndRanges();
    return TRUE;
#endif
    HMODULE pinned = nullptr; // keep this DLL loaded: the engine holds callbacks into it
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&initOnceFn, &pinned);
    g_dir = moduleDir();
    g_log = openSharedLog(g_dir + L"\\shim.log");
    g_statusPath = g_dir + L"\\shim_status.txt";
    writeStatus("loaded");
    loadConfigAndRanges();

    Ini cfg;
    cfg.load(g_dir + L"\\config.ini");
    std::string custom = cfg.get("general", "real_dll");
    std::wstring path = custom.empty() ? defaultRealDllPath() : widen(custom);
    R.h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!R.h) {
        writeStatus("no-engine");
        LOGE("FATAL: could not load the real engine at %s (error %lu)", narrow(path).c_str(), GetLastError());
        return TRUE;
    }
#define LOADF(n) R.n = (n##_t)(void*)GetProcAddress(R.h, #n);
    MS_FUNCS(LOADF)
#undef LOADF
    if (R.ms_set_logging_callback) R.ms_set_logging_callback(engineLogCb); // capture messages during init too
    int maj = R.ms_get_version_major ? R.ms_get_version_major() : -1;
    int min = R.ms_get_version_minor ? R.ms_get_version_minor() : -1;
    int rev = R.ms_get_version_revision ? R.ms_get_version_revision() : -1;
    char ver[64];
    snprintf(ver, sizeof(ver), "%d.%d.%d", maj, min, rev);
    g_engineVer = ver;
    {
        std::string folder = ensureInstrumentFolderEnv(path);
        LOGI("instrument folder: %s", folder.empty() ? "(not found; engine decides)" : folder.c_str());
    }
    LOGI("loaded real engine %s from %s", ver, narrow(path).c_str());
    LOGE("STATUS loaded");
    if (!g_rangesLibVersion.empty() && g_rangesLibVersion != ver) {
        LOGI("NOTE: ranges.ini was probed with engine %s; current is %s. Consider re-running msr-probe.exe --force.",
             g_rangesLibVersion.c_str(), ver);
    }
    return TRUE;
}

static ms_logging_callback g_hostLogCb = nullptr;
static void engineLogCb(ms_ErrorLevel level, const char* msg)
{
    if (level >= ms_ErrorLevel_Info) LOGI("engine: %s", msg ? msg : "");
    else LOGD("engine: %s", msg ? msg : "");
    if (g_hostLogCb) g_hostLogCb(level, msg);
}

static INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
static inline void ensureReal()
{
    InitOnceExecuteOnce(&g_initOnce, initOnceFn, nullptr, nullptr);
}

template<typename T> struct Fail { static T v() { return T {}; } };
template<> struct Fail<ms_Result> { static ms_Result v() { return ms_Result_Error; } };
template<> struct Fail<void> { static void v() {} };

// Pure pass-through export (DLL mode only; in-tree, lookups return the real function).
#ifdef MSR_INTREE
#define FWD(ret, name, params, args)
#else
#define FWD(ret, name, params, args) \
    EXPORT ret name params \
    { \
        ensureReal(); \
        if (!R.name) return Fail<ret>::v(); \
        return R.name args; \
    }
#endif

// ---------------------------------------------------------------------------
// Instrument key lookup

static void enumerateInstruments()
{
    if (!R.ms_get_instrument_list || !R.ms_InstrumentList_get_next || !R.ms_Instrument_get_id) return;
    ms_InstrumentList list = R.ms_get_instrument_list();
    if (!list) return;
    int n = 0;
    while (ms_InstrumentInfo info = R.ms_InstrumentList_get_next(list)) {
        int id = R.ms_Instrument_get_id(info);
        const char* name = R.ms_Instrument_get_name ? R.ms_Instrument_get_name(info) : "";
        const char* pack = R.ms_Instrument_get_pack_name ? R.ms_Instrument_get_pack_name(info) : "";
        g_instKeyById[id] = lower(instrumentKey(pack, name));
        ++n;
    }
    LOGI("enumerated %d engine instruments", n);
}

static const InstRange* findRange(int instId, bool* isSolo = nullptr)
{
    Guard g(g_instCs);
    auto it = g_instKeyById.find(instId);
    if (it == g_instKeyById.end()) {
        if (!g_instEnumerated) {
            g_instEnumerated = true;
            enumerateInstruments();
            it = g_instKeyById.find(instId);
        }
    }
    const InstRange* r = nullptr;
    std::string key = it != g_instKeyById.end() ? it->second : std::string();
    if (isSolo) *isSolo = key.find("solo") != std::string::npos;
    if (!key.empty()) {
        auto rit = g_rangesByKey.find(key);
        if (rit != g_rangesByKey.end()) r = &rit->second;
    }
    if (!r) {
        auto iit = g_rangesById.find(instId);
        if (iit != g_rangesById.end()) r = &iit->second;
    }
    if (r && (r->off || r->classTop("sustain") < 0)) r = nullptr;
    LOGI("instrument %d (%s): %s", instId, key.empty() ? "unknown" : key.c_str(),
         r ? ("sustain top " + noteName(r->classTop("sustain")) + ", low " + noteName(r->low)).c_str()
           : "no range data -> pass-through");
    return r;
}

// ---------------------------------------------------------------------------
// Per-sampler state

typedef signalsmith::stretch::SignalsmithStretch<float> Stretch;

struct LogEntry {
    enum Kind { Dyn, Pedal, Text, PresetCreate, PresetAdd, Syll, RangeStart, RangeEnd } kind;
    long long t = 0;
    double v = 0;
    int voice = 0;
    uint64_t art = 0;
    long long handle = 0;
    std::string s;
    bool hyph = false;
};

struct NoteRec {
    ms_NoteEvent_5 ev;
    long long id = 0;
    std::string textArt;
    std::vector<ms_PitchBendInfo> bends;
    std::vector<ms_VibratoInfo> vibs;
};

struct QItem {
    bool isNote = true;
    NoteRec note;
    bool rangeStart = false;
    int voice = 0;
    uint64_t art = 0;
    bool drop = false; // range item made obsolete by trill_written_out
};

struct VoiceQ {
    std::vector<QItem> items;
    bool hasNotes = false;
    long long lastLoc = -1;
    long long lastEnd = -1;
    bool lastSlur = false;
};

struct IdRoute {
    int shift = 0;
    long long realId = 0;
    bool pending = true;
    int voice = 0;
    size_t idx = 0;
};

struct MirrorTrack {
    ms_Track t = nullptr;
    std::unordered_map<long long, ms_PresetChange> presetMap;
    int notes = 0;
};

struct TrackInfo {
    int instId = -1;
    bool rangeResolved = false;
    const InstRange* range = nullptr;
    bool solo = false;
    std::vector<LogEntry> log;
    std::string textArt;
    std::map<int, VoiceQ> voices;
    std::unordered_map<long long, IdRoute> ids;
    std::map<int, MirrorTrack> mirrors; // shift -> mirror track in that shadow
    bool trillCentsResolved = false;
    int trillCents = 0;
    bool muteResolved = false;
    double muteGain = 1.0;                                // con_sord_gain_db for this instrument
    std::vector<std::pair<long long, bool> > muteSpans;   // (time, muted from here on), sorted
    struct MuteNote { long long start, endTail; bool muted; bool operator<(const MuteNote& o) const { return start < o.start; } };
    std::vector<MuteNote> muteNotes;                       // notes with/without the Mute mark, sorted by start
    std::set<std::string> textsSeen;
    // What was actually sent to the engine (for the bad-audio report).
    struct SentNote { long long loc, dur; int pitch; uint64_t a1, a2; int voice, key; bool ok; };
    std::vector<SentNote> sent;
};

// Shadow samplers are keyed by laneKey(shift, lane): `shift` semitones of transposition
// (0 = none, no pitch shifter at all) on instance number `lane` (0 = the out-of-range
// shadows; 1.. = extra instances that each play one note of a tremolo chord).
static const int LANE_STRIDE = 1000;
static inline int laneKey(int shift, int lane) { return shift + lane * LANE_STRIDE; }
static inline int keyLane(int key) { return (key + LANE_STRIDE / 2) / LANE_STRIDE; }
static inline int keyShift(int key) { return key - keyLane(key) * LANE_STRIDE; }

struct Shadow {
    int shift = 0; // semitones of transposition (pitch shifter); 0 = played as is
    int lane = 0;
    int key = 0;
    // Tremolo-chord lanes: output gain over time (start, end incl. release, gain), sorted by start.
    struct GainSpan { long long start, end; float gain; bool operator<(const GainSpan& o) const { return start < o.start; } };
    std::vector<GainSpan> gainSpans;
    float laneGainNow = 1.0f;
    ms_MuseSampler h = nullptr;
    std::unique_ptr<Stretch> fx;
    int latency = 0;
    int la = 0; // look-ahead (multiple of block)
    int d = 0;  // extra delay so that latency + d == la
    bool needsPrime = true;
    bool untouchedSinceSet = false;
    long long lastSetPos = LLONG_MIN;
    long long expectedPos = LLONG_MIN;
    int notes = 0;
    int tracks = 0; // mirror tracks added to this instance
    int auditions = 0;
    long long idleSamples = 0;
    ULONGLONG createdTick = 0;
    bool everReady = false; // has reported ready at least once
    bool gaveUp = false;    // timed out once: never block playback on it again
    std::vector<float> in[2], out[2], dl[2];
    size_t dlPos = 0;
    double teleIn = 0, teleOut = 0; // sum of squares since last report
    long long teleN = 0;
};

struct Sampler {
    ms_MuseSampler s = nullptr;
    CritSec cs;
    double sr = 0;
    int block = 0;
    int ch = 2;
    bool legacyInit = false;
    bool inited = false;
    bool offline = false;
    bool playing = false;
    double offSr = 0;
    long long pos = 0;
    long long offPos = 0;
    std::string scoreId;
    std::map<int, std::unique_ptr<Shadow> > shadows;
    std::unordered_map<ms_Track, TrackInfo> tracks;
    std::map<std::pair<ms_Track, int>, int> audition; // (track, pitch) -> shift
    float muteGainNow = 1.0f; // current (ramped) con sord. gain on this sampler's output
    long long lastBadDetailUs = LLONG_MIN; // bad-audio report: last time a detailed report was written
    // debug_instrument audio levels: 100 ms windows of the main output and of every extra instance.
    int dbgLevel = -1; // -1 unknown, 0 no, 1 yes
    long long dbgWin = LLONG_MIN;
    double dbgMain = 0;
    std::map<int, double> dbgSh;
    long long dbgN = 0;
};

static CritSec g_mapCs;
static std::unordered_map<ms_MuseSampler, std::unique_ptr<Sampler> > g_samplers;

static Sampler* findS(ms_MuseSampler s)
{
    Guard g(g_mapCs);
    auto it = g_samplers.find(s);
    return it == g_samplers.end() ? nullptr : it->second.get();
}

static Sampler* getOrCreateS(ms_MuseSampler s)
{
    Guard g(g_mapCs);
    auto& p = g_samplers[s];
    if (!p) {
        p.reset(new Sampler());
        p->s = s;
    }
    return p.get();
}

// --- shifter ----------------------------------------------------------------

static void configureFx(Sampler& S, Shadow& sh)
{
    double sr = S.offline && S.offSr > 0 ? S.offSr : S.sr;
    if (sr <= 0) sr = 48000;
    if (sh.shift == 0) { // tremolo-chord lane: mixed in directly, no shifter, no latency
        sh.fx.reset();
        sh.latency = sh.la = sh.d = 0;
        sh.needsPrime = true;
        return;
    }
    int block = std::max(256, (int)(sr * g_cfg.shifterBlockMs / 1000.0));
    int interval = std::max(64, block / 4);
    sh.fx.reset(new Stretch(12345));
    sh.fx->configure(2, block, interval);
    double tl = g_cfg.tonalityLimitHz > 0 ? g_cfg.tonalityLimitHz / sr : 0;
    sh.fx->setTransposeSemitones((float)sh.shift, (float)tl);
    if (g_cfg.formant) sh.fx->setFormantFactor(1.0f, true);
    sh.latency = sh.fx->inputLatency() + sh.fx->outputLatency();
    sh.la = 0;
    sh.d = 0;
    sh.needsPrime = true;
}

static void resetFx(Shadow& sh, int n)
{
    sh.la = ((sh.latency + n - 1) / n) * n;
    sh.d = sh.la - sh.latency;
    sh.fx->reset();
    for (int c = 0; c < 2; ++c) sh.dl[c].assign(sh.d, 0.0f);
    sh.dlPos = 0;
}

static void ensureBuffers(Shadow& sh, int n)
{
    for (int c = 0; c < 2; ++c) {
        if ((int)sh.in[c].size() < n) sh.in[c].resize(n);
        if ((int)sh.out[c].size() < n) sh.out[c].resize(n);
    }
}

// Render one block of the shadow sampler into sh.in.
static void renderShadow(Shadow& sh, int n, bool offline, long long pos, bool playing = true)
{
    ensureBuffers(sh, n);
    std::fill(sh.in[0].begin(), sh.in[0].begin() + n, 0.0f);
    std::fill(sh.in[1].begin(), sh.in[1].begin() + n, 0.0f);
    float* chans[2] = { sh.in[0].data(), sh.in[1].data() };
    ms_OutputBuffer b;
    b._channels = chans;
    b._num_data_pts = n;
    b._num_channels = 2;
    if (offline) R.ms_MuseSampler_process_offline(sh.h, b);
    else R.ms_MuseSampler_process(sh.h, b, pos);
    if (offline || playing) sh.untouchedSinceSet = false;
    // One NaN/inf sample from the engine would poison the pitch shifter for ~100 ms (and
    // MuseScore then silences the whole mix), so clean the shifter's input.
    int bad = 0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            if (!std::isfinite(sh.in[c][i]) || std::fabs(sh.in[c][i]) > 32.0f) { sh.in[c][i] = 0.0f; ++bad; }
    static std::atomic<int> logs { 0 };
    if (bad && logs.fetch_add(1) < 200)
        LOGI("bad audio from the engine in shadow %+d: %d sample(s) zeroed before shifting", sh.shift, bad);
}

// Shift sh.in -> (latency) -> extra delay -> optionally mix into dst.
static void runFx(Shadow& sh, int n, ms_OutputBuffer* dst)
{
    float* ins[2] = { sh.in[0].data(), sh.in[1].data() };
    float* outs[2] = { sh.out[0].data(), sh.out[1].data() };
    sh.fx->process(ins, n, outs, n);
    if (sh.d > 0) {
        size_t p = sh.dlPos;
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 2; ++c) {
                float x = sh.out[c][i];
                sh.out[c][i] = sh.dl[c][p];
                sh.dl[c][p] = x;
            }
            if (++p == (size_t)sh.d) p = 0;
        }
        sh.dlPos = p;
    }
    if (g_logLevel >= 2) {
        for (int i = 0; i < n; ++i) {
            sh.teleIn += (double)sh.in[0][i] * sh.in[0][i];
            sh.teleOut += (double)sh.out[0][i] * sh.out[0][i];
        }
        sh.teleN += n;
    }
    if (dst && dst->_channels) {
        int nc = std::min(dst->_num_channels, 2);
        int nn = std::min(n, dst->_num_data_pts);
        const float g = g_cfg.gain;
        for (int c = 0; c < nc; ++c) {
            float* o = dst->_channels[c];
            const float* sbuf = sh.out[c].data();
            for (int i = 0; i < nn; ++i) o[i] += g * sbuf[i];
        }
        if (dst->_num_channels == 1) { // mono host: fold right in
            float* o = dst->_channels[0];
            for (int i = 0; i < nn; ++i) o[i] += g * sh.out[1][i];
        }
    }
}

// --- shadow lifecycle ---------------------------------------------------------

static Shadow* ensureShadow(Sampler& S, int key)
{
    const int shift = keyShift(key);
    auto it = S.shadows.find(key);
    if (it != S.shadows.end()) return it->second.get();
    if (!S.inited || !R.ms_MuseSampler_create) return nullptr;

    std::unique_ptr<Shadow> sh(new Shadow());
    sh->shift = shift;
    sh->lane = keyLane(key);
    sh->key = key;
    sh->h = R.ms_MuseSampler_create();
    if (!sh->h) {
        LOGE("could not create shadow sampler");
        return nullptr;
    }
    ms_Result r = S.legacyInit ? R.ms_MuseSampler_init(sh->h, S.sr, S.block, S.ch)
                               : R.ms_MuseSampler_init_2(sh->h, S.sr, S.block, S.ch);
    if (r != ms_Result_OK) {
        LOGE("could not init shadow sampler (sr %.0f block %d)", S.sr, S.block);
        R.ms_MuseSampler_destroy(sh->h);
        return nullptr;
    }
    if (!S.scoreId.empty() && R.ms_MuseSampler_set_score_id) R.ms_MuseSampler_set_score_id(sh->h, S.scoreId.c_str());
    long long p = S.offline ? S.offPos : S.pos;
    R.ms_MuseSampler_set_position(sh->h, p);
    sh->lastSetPos = p;
    sh->untouchedSinceSet = true;
    if (S.offline) R.ms_MuseSampler_start_offline_mode(sh->h, S.offSr);
    if (S.playing) R.ms_MuseSampler_set_playing(sh->h, 1);
    configureFx(S, *sh);
    sh->createdTick = GetTickCount64();
    if (sh->lane == 0)
        LOGI("sampler %p: created shadow for %+d semitones (shifter latency %d samples)", S.s, shift, sh->latency);
    else
        LOGI("sampler %p: created tremolo-chord instance %d%s", S.s, sh->lane,
             shift ? (" (also shifted " + std::to_string(shift) + " semitones)").c_str() : "");
    Shadow* raw = sh.get();
    S.shadows[key] = std::move(sh);
    return raw;
}

static std::string instName(int instId);
static std::string artNames(uint64_t a1, uint64_t a2);
static bool debugOn(const TrackInfo& ti, long long tUs)
{
    if (g_cfg.debugInst.empty() || g_cfg.debugToUs <= g_cfg.debugFromUs) return false;
    if (tUs < g_cfg.debugFromUs || tUs > g_cfg.debugToUs) return false;
    return lower(instName(ti.instId)).find(g_cfg.debugInst) != std::string::npos;
}

// Lanes 1..49 play only tremolo notes (see assignTremoloLanes): 1.. = chord ranks, 40 = unsplit tremolos.
static inline bool isTremoloLane(int lane) { return lane >= 1 && lane < 50; }
static const int SINGLE_TREMOLO_LANE = 40; // unsplit tremolos (own gain 1), never shares a lane with chord ranks

// The engine's dynamics values for MuseScore's markings (MuseSamplerSequencer::dynamicLevelRatio):
// ppppp pppp ppp pp p mp mf f ff fff. Shifts a value by `steps` markings, interpolating in between.
static double shiftDynamicMarkings(double v, int steps)
{
    static const double P[] = { 0.0, 1 / 127.0, 2 / 127.0, 4 / 127.0, 8 / 127.0, 16 / 127.0, 32 / 127.0, 64 / 127.0,
                                96 / 127.0, 120 / 127.0, 127 / 127.0 };
    const int N = (int)(sizeof(P) / sizeof(P[0]));
    if (steps == 0) return v;
    double x = N - 1;
    for (int i = 0; i + 1 < N; ++i) {
        if (v <= P[i + 1]) { x = i + (P[i + 1] > P[i] ? (v - P[i]) / (P[i + 1] - P[i]) : 0.0); break; }
    }
    x = std::max(0.0, std::min((double)(N - 1), x + steps));
    int i = std::min(N - 2, (int)x);
    return P[i] + (P[i + 1] - P[i]) * (x - i);
}

static void replayEntry(const LogEntry& e, ms_MuseSampler h, MirrorTrack& m, int lane = 0)
{
    switch (e.kind) {
    case LogEntry::Dyn: {
        double v = e.v;
        if (isTremoloLane(lane)) v = shiftDynamicMarkings(v, g_cfg.tremoloDynSteps);
        R.ms_MuseSampler_add_track_dynamics_event_2(h, m.t, ms_DynamicsEvent_2 { e.t, v });
        break;
    }
    case LogEntry::Pedal:
        R.ms_MuseSampler_add_track_pedal_event_2(h, m.t, ms_PedalEvent_2 { e.t, e.v });
        break;
    case LogEntry::Text: {
        ms_TextArticulationEvent ev;
        ev._start_us = e.t;
        ev._articulation = e.s.c_str();
        R.ms_MuseSampler_add_track_text_articulation_event(h, m.t, ev);
        break;
    }
    case LogEntry::PresetCreate:
        m.presetMap[e.handle] = R.ms_MuseSampler_create_preset_change(h, m.t, e.t);
        break;
    case LogEntry::PresetAdd: {
        auto it = m.presetMap.find(e.handle);
        if (it != m.presetMap.end()) R.ms_MuseSampler_add_preset(h, m.t, it->second, e.s.c_str());
        break;
    }
    case LogEntry::Syll: {
        ms_SyllableEvent2 ev;
        ev._text = e.s.c_str();
        ev._position_us = e.t;
        ev._hyphened_to_next = e.hyph;
        R.ms_MuseSampler_add_track_syllable_event_2(h, m.t, ev);
        break;
    }
    case LogEntry::RangeStart:
        R.ms_MuseSampler_add_track_event_range_start(h, m.t, e.voice, (ms_NoteArticulation)e.art);
        break;
    case LogEntry::RangeEnd:
        R.ms_MuseSampler_add_track_event_range_end(h, m.t, e.voice, (ms_NoteArticulation)e.art);
        break;
    }
}

static MirrorTrack* ensureMirror(Sampler& S, TrackInfo& ti, int shift)
{
    auto it = ti.mirrors.find(shift);
    if (it != ti.mirrors.end()) return &it->second;
    Shadow* sh = ensureShadow(S, shift);
    if (!sh) return nullptr;
    ms_Track t = R.ms_MuseSampler_add_track(sh->h, ti.instId);
    if (!t) {
        LOGE("could not add shadow track for instrument %d", ti.instId);
        return nullptr;
    }
    MirrorTrack& m = ti.mirrors[shift];
    m.t = t;
    sh->tracks++;
    for (const auto& e : ti.log) replayEntry(e, sh->h, m, sh->lane);
    LOGD("mirror track for instrument %d at %+d, replayed %d events", ti.instId, shift, (int)ti.log.size());
    return &m;
}

static void broadcast(Sampler& S, TrackInfo& ti, const LogEntry& e)
{
    for (auto& mp : ti.mirrors) {
        auto sit = S.shadows.find(mp.first);
        if (sit != S.shadows.end()) replayEntry(e, sit->second->h, mp.second, sit->second->lane);
    }
    ti.log.push_back(e);
}

static TrackInfo* trackInfo(Sampler& S, ms_Track t)
{
    auto it = S.tracks.find(t);
    return it == S.tracks.end() ? nullptr : &it->second;
}

static const InstRange* rangeFor(TrackInfo& ti)
{
    if (!ti.rangeResolved) {
        ti.rangeResolved = true;
        ti.range = ti.instId >= 0 ? findRange(ti.instId, &ti.solo) : nullptr;
    }
    return ti.range;
}

// con_sord_gain_db: per-instrument gain while a "con sord." / mute text articulation is active.
static double muteGainFor(TrackInfo& ti)
{
    if (!ti.muteResolved && ti.instId >= 0) {
        ti.muteResolved = true;
        rangeFor(ti); // makes sure instruments are enumerated
        Guard g(g_instCs);
        auto it = g_instKeyById.find(ti.instId);
        if (it != g_instKeyById.end()) {
            auto git = g_muteGainByKey.find(lower(it->second));
            if (git != g_muteGainByKey.end()) ti.muteGain = git->second;
        }
    }
    return ti.muteGain;
}

static int trillCentsFor(TrackInfo& ti)
{
    if (!ti.trillCentsResolved && ti.instId >= 0) {
        ti.trillCentsResolved = true;
        rangeFor(ti);
        Guard g(g_instCs);
        auto it = g_instKeyById.find(ti.instId);
        if (it != g_instKeyById.end()) {
            auto ct = g_trillCentsByKey.find(lower(it->second));
            if (ct != g_trillCentsByKey.end()) ti.trillCents = ct->second;
        }
    }
    return ti.trillCents;
}

static bool isMuteText(const std::string& text)
{
    std::string t = lower(text);
    if (t.find("senza") != std::string::npos || t.find("open") != std::string::npos || t.find("unmute") != std::string::npos) return false;
    return t.find("sord") != std::string::npos || t.find("mute") != std::string::npos;
}

static void applyMuteGain(Sampler& S, ms_OutputBuffer& buf, long long tUs)
{
    float target = 1.0f;
    for (auto& tp : S.tracks) {
        TrackInfo& ti = tp.second;
        if (ti.muteGain == 1.0) continue;
        bool muted = false;
        // MuseScore sends "con sord." as the Mute mark on each note: the latest note that
        // has started decides, until its end plus a release tail.
        if (!ti.muteNotes.empty()) {
            TrackInfo::MuteNote key { tUs, 0, false };
            auto it = std::upper_bound(ti.muteNotes.begin(), ti.muteNotes.end(), key);
            if (it != ti.muteNotes.begin()) {
                auto jt = std::prev(it);
                long long st = jt->start, endTail = jt->endTail;
                bool m = jt->muted;
                while (jt != ti.muteNotes.begin() && std::prev(jt)->start == st) { --jt; endTail = std::max(endTail, jt->endTail); m = m || jt->muted; }
                if (m && tUs <= endTail) muted = true;
            }
        }
        // ... or as a text articulation, for instruments that list one.
        if (!muted && !ti.muteSpans.empty()) {
            auto it = std::upper_bound(ti.muteSpans.begin(), ti.muteSpans.end(), std::make_pair(tUs, true));
            if (it != ti.muteSpans.begin() && std::prev(it)->second) muted = true;
        }
        if (muted) { target = (float)ti.muteGain; break; }
    }
    float g0 = S.muteGainNow;
    if (g0 == 1.0f && target == 1.0f) return;
    int n = buf._num_data_pts;
    if (buf._channels && n > 0) {
        for (int c = 0; c < buf._num_channels; ++c) {
            float* o = buf._channels[c];
            if (!o) continue;
            for (int i = 0; i < n; ++i) o[i] *= g0 + (target - g0) * (float)(i + 1) / (float)n;
        }
    }
    S.muteGainNow = target;
}

// --- routing ------------------------------------------------------------------

// Shift (in semitones, signed) needed for a single note, 0 if none / impossible.
static int shiftFor(int pitch, int top, int low, bool trill = false)
{
    if (top < 0) return 0;
    if (pitch > top) {
        int s = (g_cfg.octaveMode || (trill && g_cfg.trillOctave)) ? 12 * ((pitch - top + 11) / 12) : pitch - top;
        if (s > g_cfg.maxShiftUp) return 0;
        if (low >= 0 && pitch - s < low) return 0;
        return s;
    }
    if (g_cfg.extendDown && low >= 0 && pitch < low) {
        int s = g_cfg.octaveMode ? -12 * ((low - pitch + 11) / 12) : pitch - low;
        if (-s > g_cfg.maxShiftDown) return 0;
        if (pitch - s > top) return 0;
        return s;
    }
    return 0;
}

static void emitNote(Sampler& S, ms_Track T, TrackInfo& ti, NoteRec& n, int key, float laneGain = 1.0f)
{
    long long realId = 0;
    ms_MuseSampler dstS = S.s;
    ms_Track dstT = T;
    if (key != 0) {
        MirrorTrack* m = ensureMirror(S, ti, key);
        Shadow* sh = m ? S.shadows[key].get() : nullptr;
        if (m && sh) {
            dstS = sh->h;
            dstT = m->t;
            m->notes++;
            sh->notes++;
            sh->idleSamples = 0;
            if (sh->lane > 0) {
                Shadow::GainSpan gs { n.ev._location_us, n.ev._location_us + n.ev._duration_us + 400000, laneGain };
                sh->gainSpans.insert(std::upper_bound(sh->gainSpans.begin(), sh->gainSpans.end(), gs), gs);
            }
        } else {
            key = 0;
        }
    }
    const int shift = keyShift(key);
    ms_NoteEvent_5 ev = n.ev;
    ev._pitch -= shift;
    if (ev._articulation & ms_NoteArticulation_Trill) {
        int tc = trillCentsFor(ti);
        if (tc) ev._offset_cents = std::max(-100, std::min(100, ev._offset_cents + tc));
    }
    // MuseScore sends trills it can't hand to Muse Sounds natively as tiny written-out notes
    // that still carry the trill mark; Muse Sounds plays nothing for those, so send them plain.
    if (shift != 0 && (ev._articulation & ms_NoteArticulation_Trill) && ev._duration_us <= g_cfg.trillNoteUs) {
        ev._articulation = (ms_NoteArticulation)((uint64_t)ev._articulation & ~(uint64_t)ms_NoteArticulation_Trill);
    }
    bool sentOk = true;
    if (R.ms_MuseSampler_add_track_note_event_6(dstS, dstT, ev, realId) != ms_Result_OK) {
        // The engine refused the note. Find out which property it objects to by retrying
        // variants (longer / without articulation marks), and log what it accepted.
        // Variants, mildest first: slightly longer, then without articulation marks.
        static const long long lens[] = { 0, 60000, 80000, 120000 };
        int accepted = -1;
        long long acceptedLen = 0;
        for (int plain = 0; plain < 2 && accepted < 0; ++plain) {
            for (int li = plain ? 0 : 1; li < 4 && accepted < 0; ++li) {
                ms_NoteEvent_5 e2 = ev;
                if (lens[li] > 0) e2._duration_us = std::max<long long>(e2._duration_us, lens[li]);
                if (plain) {
                    e2._articulation = ms_NoteArticulation_None;
                    e2._articulation_2 = ms_NoteArticulation2_None;
                }
                if (R.ms_MuseSampler_add_track_note_event_6(dstS, dstT, e2, realId) == ms_Result_OK) {
                    accepted = plain;
                    acceptedLen = e2._duration_us;
                }
            }
        }
        sentOk = accepted >= 0;
        static int reported = 0;
        bool detail = g_logLevel >= 2 || reported < 40 || debugOn(ti, ev._location_us);
        if (detail) {
            ++reported;
            LOGI("engine rejected note %s @%.3fs dur %lldms art 0x%llx/0x%llx voice %d track %p shift %+d lane %d -> %s",
                 noteName(ev._pitch).c_str(), ev._location_us / 1e6, ev._duration_us / 1000,
                 (unsigned long long)ev._articulation, (unsigned long long)ev._articulation_2, ev._voice, dstT, shift, keyLane(key),
                 accepted < 0 ? "every variant rejected"
                              : ("accepted at " + std::to_string(acceptedLen / 1000) + "ms" + (accepted ? " without marks" : "")).c_str());
        }
    }
    {
        bool ok = sentOk; // (an event id of 0 is valid: the first note on a new track)
        ti.sent.push_back(TrackInfo::SentNote { ev._location_us, ev._duration_us, n.ev._pitch, (uint64_t)ev._articulation,
                                                (uint64_t)ev._articulation_2, ev._voice, key, ok });
    }
    if (key != 0) {
        LOGD("note %s @%lldus -> shadow %+d lane %d (plays %s) dur %lldms art 0x%llx/0x%llx", noteName(n.ev._pitch).c_str(),
             n.ev._location_us, shift, keyLane(key), noteName(ev._pitch).c_str(), ev._duration_us / 1000,
             (unsigned long long)n.ev._articulation, (unsigned long long)n.ev._articulation_2);
    }
    IdRoute& route = ti.ids[n.id];
    route.shift = key;
    route.realId = realId;
    route.pending = false;
    for (auto b : n.bends) {
        b.event_id = realId;
        R.ms_MuseSampler_add_pitch_bend(dstS, dstT, b);
    }
    for (auto v : n.vibs) {
        v.event_id = realId;
        R.ms_MuseSampler_add_vibrato(dstS, dstT, v);
    }
}

// Sends an articulation range marker only to the instances that play notes inside that range
// (0 = the normal instrument). Before 2026-10-28 every marker went to every instance, and a new
// instance replayed all earlier markers: hundreds of empty start/end pairs, after which Muse Sounds
// mishandled the next tremolo (Strudwick I:13 basses crescendoed through a diminuendo).
static void emitRangeTo(Sampler& S, ms_Track T, TrackInfo& ti, bool start, int voice, uint64_t art, const std::set<int>& targets)
{
    static const std::set<int> mainOnly { 0 };
    for (int key : targets.empty() ? mainOnly : targets) {
        ms_MuseSampler h = S.s;
        ms_Track t = T;
        if (key != 0) {
            MirrorTrack* m = ensureMirror(S, ti, key);
            auto sit = S.shadows.find(key);
            if (m && sit != S.shadows.end()) { h = sit->second->h; t = m->t; }
        }
        if (start) R.ms_MuseSampler_add_track_event_range_start(h, t, voice, (ms_NoteArticulation)art);
        else R.ms_MuseSampler_add_track_event_range_end(h, t, voice, (ms_NoteArticulation)art);
    }
}

// Optional (trill_written_out): Muse Sounds gets only a "trill" mark, not the interval, and
// picks its own half/whole-step recording. MuseScore writes trills out as alternating notes
// with the interval the score asks for (accidental included), so drop the mark and play
// those notes as written, slurred.
static void writeOutTrills(VoiceQ& q)
{
    if (g_cfg.trillWrittenOut <= 0) return;
    const uint64_t TR = (uint64_t)ms_NoteArticulation_Trill, SL = (uint64_t)ms_NoteArticulation_Slur;
    std::vector<size_t> notes;
    for (size_t i = 0; i < q.items.size(); ++i) if (q.items[i].isNote) notes.push_back(i);
    auto ev = [&](size_t n) -> ms_NoteEvent_5& { return q.items[notes[n]].note.ev; };
    auto single = [&](size_t n) {
        long long loc = ev(n)._location_us;
        return (n == 0 || ev(n - 1)._location_us != loc) && (n + 1 >= notes.size() || ev(n + 1)._location_us != loc);
    };
    auto trillNote = [&](size_t n) {
        return (ev(n)._articulation & TR) && ev(n)._duration_us <= g_cfg.trillWrittenOutMaxUs && single(n);
    };
    size_t n = 0;
    while (n < notes.size()) {
        if (!trillNote(n)) { ++n; continue; }
        size_t m = n;
        int pa = ev(n)._pitch, pb = -1;
        while (m + 1 < notes.size() && trillNote(m + 1)
               && ev(m + 1)._location_us - (ev(m)._location_us + ev(m)._duration_us) <= g_cfg.runGapUs) {
            int p = ev(m + 1)._pitch;
            if (p == ev(m)._pitch) break;
            if (pb < 0 && p != pa) pb = p;
            if (p != pa && p != pb) break;
            ++m;
        }
        int interval = pb < 0 ? 0 : std::abs(pb - pa);
        bool want = m >= n + 2 && interval > 0 && (g_cfg.trillWrittenOut == 2 || interval == 1);
        if (want) {
            for (size_t k = n; k <= m; ++k) {
                uint64_t a = (uint64_t)ev(k)._articulation & ~TR;
                if (k < m) a |= SL;
                ev(k)._articulation = (ms_NoteArticulation)a;
            }
            // The trill's articulation range markers no longer belong to any note.
            size_t lo = n == 0 ? 0 : notes[n - 1] + 1, hi = m + 1 < notes.size() ? notes[m + 1] : q.items.size();
            for (size_t i = lo; i < hi; ++i)
                if (!q.items[i].isNote && q.items[i].art == TR) q.items[i].drop = true;
            LOGD("trill at %lldus written out: %d notes, interval %d", ev(n)._location_us, (int)(m - n + 1), interval);
        }
        n = m + 1;
    }
}

// trill_priority: a trill note that also carries an accent (etc.) can come out as a plain
// accented note, so keep only the trill. trill_gap_ms: when one trill runs straight into a
// trill on another note, Muse Sounds may treat both as one trill and play the second note
// plain, so end the first trill slightly early.
static void prioritizeTrills(VoiceQ& q, int instId)
{
    const uint64_t TR = (uint64_t)ms_NoteArticulation_Trill;
    const uint64_t ATTACK = (uint64_t)(ms_NoteArticulation_Staccato | ms_NoteArticulation_Staccatissimo
                                       | ms_NoteArticulation_Accent | ms_NoteArticulation_Tenuto | ms_NoteArticulation_Marcato);
    int stripped = 0, gaps = 0;
    if (g_cfg.trillPriority) {
        for (auto& it : q.items) {
            if (!it.isNote) continue;
            uint64_t a = (uint64_t)it.note.ev._articulation;
            if ((a & TR) && (a & ATTACK)) { it.note.ev._articulation = (ms_NoteArticulation)(a & ~ATTACK); ++stripped; }
        }
    }
    if (g_cfg.trillGapUs > 0) {
        // trill notes in order; a trill alternates between two pitches, so a third pitch
        // right after it (also trilled) starts a new trill.
        size_t prev = SIZE_MAX;
        int pa = -1, pb = -1;
        for (size_t i = 0; i < q.items.size(); ++i) {
            if (!q.items[i].isNote) continue;
            ms_NoteEvent_5& ev = q.items[i].note.ev;
            if (!((uint64_t)ev._articulation & TR)) { prev = SIZE_MAX; pa = pb = -1; continue; }
            if (prev != SIZE_MAX) {
                ms_NoteEvent_5& pe = q.items[prev].note.ev;
                if (pe._location_us == ev._location_us) continue; // chord
                long long pend = pe._location_us + pe._duration_us;
                bool contiguous = ev._location_us - pend <= 1000;
                bool newTrill = pb >= 0 && ev._pitch != pa && ev._pitch != pb;
                if (contiguous && newTrill) {
                    long long cut = std::min(g_cfg.trillGapUs, pe._duration_us / 2);
                    pe._duration_us -= cut;
                    ++gaps;
                    pa = ev._pitch; pb = -1;
                } else if (!contiguous) {
                    pa = ev._pitch; pb = -1;
                } else if (pa < 0) pa = ev._pitch;
                else if (pb < 0 && ev._pitch != pa) pb = ev._pitch;
            } else {
                pa = ev._pitch; pb = -1;
            }
            prev = i;
        }
    }
    if (stripped || gaps)
        LOGI("instrument %d: trill priority: %d trill note(s) without accent/tenuto/staccato marks, %d gap(s) between back-to-back trills",
             instId, stripped, gaps);
}

// Decides the shift for every queued note of one voice (no engine calls).
static void planVoice(TrackInfo& ti, VoiceQ& q, std::vector<int>& shifts)
{
    shifts.assign(q.items.size(), 0);
    if (q.items.empty()) return;
    writeOutTrills(q);
    prioritizeTrills(q, ti.instId);
    const InstRange* r = ti.range;

    // Optional: no legato transition (slide) across large leaps.
    if (g_cfg.legatoMaxLeap > 0 && (!g_cfg.legatoMaxLeapSoloOnly || ti.solo)) {
        for (size_t i = 0; i < q.items.size(); ++i) {
            if (!q.items[i].isNote) continue;
            ms_NoteEvent_5& ev = q.items[i].note.ev;
            if (!(ev._articulation & ms_NoteArticulation_Slur)) continue;
            for (size_t j = i + 1; j < q.items.size(); ++j) {
                if (!q.items[j].isNote || q.items[j].note.ev._location_us == ev._location_us) continue;
                if (std::abs(q.items[j].note.ev._pitch - ev._pitch) > g_cfg.legatoMaxLeap) {
                    ev._articulation = (ms_NoteArticulation)((uint64_t)ev._articulation & ~(uint64_t)ms_NoteArticulation_Slur);
                }
                break;
            }
        }
    }

    // Legato context per note: group notes into steps by location.
    std::vector<int> tops(q.items.size(), -1);
    int low = r ? (r->low >= 0 ? r->low : std::max(0, r->classTop("sustain") - 36)) : -1;
    long long prevLoc = LLONG_MIN, curLoc = LLONG_MIN;
    bool prevStepSlur = false, curStepSlur = false;
    bool any = false;
    for (size_t i = 0; i < q.items.size(); ++i) {
        QItem& it = q.items[i];
        if (!it.isNote) continue;
        const ms_NoteEvent_5& ev = it.note.ev;
        if (ev._location_us != curLoc) {
            prevLoc = curLoc;
            prevStepSlur = curStepSlur;
            curLoc = ev._location_us;
            curStepSlur = false;
        }
        bool slur = (ev._articulation & ms_NoteArticulation_Slur) != 0;
        curStepSlur = curStepSlur || slur;
        int ctx = slur ? Legato_Slurred : (prevLoc != LLONG_MIN && prevStepSlur ? Legato_Target : Legato_None);
        tops[i] = r ? r->noteTop(ev._articulation, it.note.textArt, ctx) : -1;
        shifts[i] = shiftFor(ev._pitch, tops[i], low, (ev._articulation & ms_NoteArticulation_Trill) != 0);
        if (shifts[i] != 0) any = true;
    }

    // Steps: notes of the voice grouped by start time (chords / doublings share a step).
    struct Step {
        std::vector<size_t> idx;
        long long loc = 0, end = 0;
        bool slur = false;
    };
    std::vector<Step> steps;
    for (size_t i = 0; i < q.items.size(); ++i) {
        if (!q.items[i].isNote) continue;
        const ms_NoteEvent_5& ev = q.items[i].note.ev;
        if (steps.empty() || steps.back().loc != ev._location_us) {
            steps.push_back(Step());
            steps.back().loc = ev._location_us;
            steps.back().end = ev._location_us;
        }
        Step& st = steps.back();
        st.idx.push_back(i);
        st.end = std::max(st.end, ev._location_us + ev._duration_us);
        if (ev._articulation & ms_NoteArticulation_Slur) st.slur = true;
    }
    auto fits = [&](size_t i, int sh) {
        int p = q.items[i].note.ev._pitch - sh;
        return (low < 0 || p >= low) && (tops[i] < 0 || p <= tops[i]);
    };

    // (octave mode, legato_phrases=1) keep a slurred phrase together in one shadow.
    if (any && g_cfg.octaveMode && g_cfg.legatoPhrases) {
        size_t a0 = 0;
        while (a0 < steps.size()) {
            size_t b0 = a0;
            while (b0 + 1 < steps.size() && steps[b0].slur && std::llabs(steps[b0 + 1].loc - steps[b0].end) <= 60000) ++b0;
            if (b0 > a0) {
                int cand = 0;
                for (size_t k = a0; k <= b0; ++k)
                    for (size_t i : steps[k].idx) if (std::abs(shifts[i]) > std::abs(cand)) cand = shifts[i];
                bool ok = cand != 0;
                for (size_t k = a0; k <= b0 && ok; ++k)
                    for (size_t i : steps[k].idx) if (!fits(i, cand)) ok = false;
                if (ok) for (size_t k = a0; k <= b0; ++k) for (size_t i : steps[k].idx) shifts[i] = cand;
            }
            a0 = b0 + 1;
        }
    }

    // Fast runs and written-out trills that cross the top go through ONE shadow as a
    // continuous stream (a pitch shifter can't render isolated 90 ms fragments). Within
    // such a run the out-of-range notes share the largest shift, and short in-range notes
    // close to them join; long in-range notes stay on the real instrument.
    if (any) {
        size_t a0 = 0;
        while (a0 < steps.size()) {
            size_t b0 = a0;
            while (b0 + 1 < steps.size() && steps[b0 + 1].loc >= steps[b0].loc
                   && steps[b0 + 1].loc - steps[b0].end <= g_cfg.runGapUs) ++b0;
            if (b0 > a0) {
                int runShift = 0;
                std::vector<std::pair<long long, long long> > outSpans;
                for (size_t k = a0; k <= b0; ++k)
                    for (size_t i : steps[k].idx)
                        if (shifts[i] != 0) {
                            if (std::abs(shifts[i]) > std::abs(runShift)) runShift = shifts[i];
                            const ms_NoteEvent_5& ev = q.items[i].note.ev;
                            outSpans.push_back(std::make_pair(ev._location_us, ev._location_us + ev._duration_us));
                        }
                bool trillRun = false;
                for (size_t k = a0; k <= b0 && !trillRun; ++k)
                    for (size_t i : steps[k].idx)
                        if (shifts[i] != 0 && (q.items[i].note.ev._articulation & ms_NoteArticulation_Trill)) trillRun = true;
                if (runShift != 0 && trillRun && g_cfg.trillOctave && std::abs(runShift) % 12 != 0) {
                    // Trills at the very top of the range come out silent; play them an octave down.
                    int oct = 12 * ((std::abs(runShift) + 11) / 12);
                    runShift = runShift > 0 ? oct : -oct;
                    LOGD("trill run at %lldus: using %+d", steps[a0].loc, runShift);
                }
                if (runShift != 0) {
                    for (size_t k = a0; k <= b0; ++k)
                        for (size_t i : steps[k].idx) {
                            const ms_NoteEvent_5& ev = q.items[i].note.ev;
                            bool join = shifts[i] != 0;
                            if (!join && ev._duration_us <= g_cfg.fastNoteUs) {
                                long long s0 = ev._location_us, e0 = s0 + ev._duration_us;
                                for (auto& sp : outSpans) {
                                    long long d = s0 > sp.second ? s0 - sp.second : (sp.first > e0 ? sp.first - e0 : 0);
                                    if (d <= g_cfg.runJoinUs) { join = true; break; }
                                }
                            }
                            if (join && fits(i, runShift)) shifts[i] = runShift;
                        }
                }
            }
            a0 = b0 + 1;
        }
    }

}

// Chord tremolos (tremolo_chords). Muse Sounds decides between a one-note and a two-note
// (fingered) tremolo from the notes it gets under the tremolo mark, so a chord with a
// one-chord tremolo (slashes on the stem) comes out as an alternation between the chord's
// notes. MuseScore sends such a chord as repeated steps that each hold all chord notes at
// once; a genuine two-chord (beamed) tremolo is sent as steps alternating between the two
// chords. Each simultaneous tremolo note gets its own engine instance ("lane") by its rank
// in the chord, top note first (lane 0 = the normal instrument): every lane then sees a
// single repeated note (one-note tremolo), or, for a two-chord tremolo between chords, one
// note of each chord alternating (two-note tremolo, as written).
static int assignTremoloLanes(TrackInfo& ti, VoiceQ& q, std::vector<int>& lanes, std::vector<float>& gains)
{
    lanes.assign(q.items.size(), 0);
    gains.assign(q.items.size(), 1.0f);
    if (!g_cfg.tremoloChords && g_cfg.tremoloDynSteps == 0) return 0;
    const uint64_t TREM = (uint64_t)(ms_NoteArticulation_Tremolo1 | ms_NoteArticulation_Tremolo2
                                     | ms_NoteArticulation_Tremolo3 | ms_NoteArticulation_BuzzTremolo);
    int split = 0, chords = 0;
    size_t i = 0;
    while (i < q.items.size()) {
        if (!q.items[i].isNote) { ++i; continue; }
        const long long loc = q.items[i].note.ev._location_us;
        std::vector<size_t> step;
        size_t j = i;
        for (; j < q.items.size(); ++j) {
            if (!q.items[j].isNote) continue;
            if (q.items[j].note.ev._location_us != loc) break;
            if ((uint64_t)q.items[j].note.ev._articulation & TREM) step.push_back(j);
        }
        if (step.size() >= 1 && (!g_cfg.tremoloChords || step.size() == 1)) {
            // Only for tremolo_dynamic_steps: single tremolo notes (or whole tremolo chords when chords
            // are not split) on lane 1, so their dynamic can be lowered without touching other notes.
            // No gain change: nothing is added here.
            if (g_cfg.tremoloDynSteps != 0)
                for (size_t k : step) lanes[k] = SINGLE_TREMOLO_LANE;
        } else if (step.size() >= 2) {
            std::stable_sort(step.begin(), step.end(), [&](size_t a, size_t b) {
                return q.items[a].note.ev._pitch > q.items[b].note.ev._pitch;
            });
            int rank = 0, lastPitch = INT_MIN;
            for (size_t k : step) {
                int p = q.items[k].note.ev._pitch;
                if (lastPitch != INT_MIN && p != lastPitch) ++rank; // unison doublings share a lane
                lastPitch = p;
                // Every chord note on its own instance (lanes 1..), the normal instrument rests, so
                // the instances can be balanced: N full sections would be N times too loud.
                lanes[k] = 1 + std::min(rank, g_cfg.tremoloMaxLanes - 1);
                ++split;
            }
            const int nNotes = std::min(rank + 1, g_cfg.tremoloMaxLanes);
            const float g = (g_cfg.tremoloBalance ? 1.0f / std::sqrt((float)nNotes) : 1.0f) * g_cfg.tremoloGain;
            for (size_t k : step) gains[k] = g;
            ++chords;
        }
        i = j;
    }
    if (chords) LOGD("instrument %d: %d tremolo chord step(s), %d note(s) on chord instances", ti.instId, chords, split);
    return split;
}


// debug_instrument / debug_from_s / debug_to_s: what each instance of this track was sent in the window.
static void debugReport(Sampler& S, ms_Track T, TrackInfo& ti)
{
    if (g_cfg.debugInst.empty() || g_cfg.debugToUs <= g_cfg.debugFromUs) return;
    const std::string name = instName(ti.instId);
    if (lower(name).find(g_cfg.debugInst) == std::string::npos) return;
    const long long a = g_cfg.debugFromUs, b = g_cfg.debugToUs;
    LOGI("DEBUG %s, sampler %p, track %p: %d notes sent in total, %d log entries; mirrors:", name.c_str(), (void*)S.s, (void*)T,
         (int)ti.sent.size(), (int)ti.log.size());
    for (auto& mp : ti.mirrors) LOGI("DEBUG   instance key %d = shift %+d lane %d (mirror track %p)", mp.first, keyShift(mp.first), keyLane(mp.first), (void*)mp.second.t);
    for (const auto& e : ti.log) {
        if (e.kind != LogEntry::Dyn || e.t < a - 2000000 || e.t > b) continue;
        LOGI("DEBUG   dynamics %.3fs = %.4f (tremolo instances get %.4f)", e.t / 1e6, e.v, shiftDynamicMarkings(e.v, g_cfg.tremoloDynSteps));
    }
    for (const auto& e : ti.log) {
        if (e.kind == LogEntry::Dyn || e.kind == LogEntry::Pedal) continue;
        if ((e.kind == LogEntry::Text || e.kind == LogEntry::PresetCreate || e.kind == LogEntry::Syll) && (e.t < a - 2000000 || e.t > b)) continue;
        static const char* K[] = { "dyn", "pedal", "text", "preset-create", "preset-add", "syllable", "range-start", "range-end" };
        if (e.kind == LogEntry::RangeStart || e.kind == LogEntry::RangeEnd) continue; // no times; too many
        LOGI("DEBUG   %s %.3fs %s", K[e.kind], e.t / 1e6, e.s.c_str());
    }
    for (const auto& n : ti.sent) {
        if (n.loc + n.dur < a || n.loc > b) continue;
        LOGI("DEBUG   note %s @%.3fs dur %lldms voice %d %s -> %s%s", noteName(n.pitch).c_str(), n.loc / 1e6, n.dur / 1000, n.voice,
             artNames(n.a1, n.a2).c_str(), n.key ? ("instance shift " + std::to_string(keyShift(n.key)) + " lane " + std::to_string(keyLane(n.key))).c_str() : "main",
             n.ok ? "" : " (REJECTED)");
    }
}

// Sends everything queued for a track. Each voice is planned as a whole, but the engine
// needs a track's events in time order (it rejects notes earlier than ones it already
// has), so the voices are merged back into one time-ordered stream before sending.
static void flushTrack(Sampler& S, ms_Track T, TrackInfo& ti)
{
    struct Out {
        long long t;
        size_t seq;
        VoiceQ* q;
        size_t idx;
        int shift;
    };
    std::vector<Out> outs;
    std::map<VoiceQ*, std::vector<int> > plans;
    std::map<VoiceQ*, std::vector<float> > laneGains;
    std::map<VoiceQ*, std::vector<std::set<int> > > rangeTargets;
    size_t seq = 0;
    int tremoloSplit = 0;
    for (auto& v : ti.voices) {
        VoiceQ& q = v.second;
        if (q.items.empty()) continue;
        std::vector<int>& shifts = plans[&q];
        planVoice(ti, q, shifts);
        std::vector<int> lanes;
        std::vector<float>& gains = laneGains[&q];
        tremoloSplit += assignTremoloLanes(ti, q, lanes, gains);
        for (size_t i = 0; i < shifts.size(); ++i) shifts[i] = laneKey(shifts[i], lanes[i]);
        // Which instances play notes inside each range (start/end matched per articulation, nesting allowed).
        std::vector<std::set<int> >& tgt = rangeTargets[&q];
        tgt.assign(q.items.size(), std::set<int>());
        {
            std::map<uint64_t, std::vector<size_t> > open;
            for (size_t i = 0; i < q.items.size(); ++i) {
                const QItem& it = q.items[i];
                if (it.isNote) {
                    for (auto& op : open)
                        for (size_t j : op.second) tgt[j].insert(shifts[i]);
                } else if (!it.drop) {
                    if (it.rangeStart) open[it.art].push_back(i);
                    else {
                        auto oit = open.find(it.art);
                        if (oit != open.end() && !oit->second.empty()) {
                            tgt[i] = tgt[oit->second.back()];
                            oit->second.pop_back();
                        }
                    }
                }
            }
        }
        // Non-note items (articulation ranges) take the time of the next note in their voice.
        long long nextT = LLONG_MIN, lastT = 0;
        std::vector<long long> keys(q.items.size(), 0);
        for (size_t i = q.items.size(); i-- > 0;) {
            if (q.items[i].isNote) nextT = q.items[i].note.ev._location_us;
            keys[i] = nextT;
        }
        for (size_t i = 0; i < q.items.size(); ++i) {
            if (q.items[i].isNote) lastT = q.items[i].note.ev._location_us;
            else if (keys[i] == LLONG_MIN) keys[i] = lastT;
            outs.push_back(Out { keys[i], seq++, &q, i, shifts[i] });
        }
    }
    std::stable_sort(outs.begin(), outs.end(), [](const Out& a, const Out& b) { return a.t < b.t; });
    for (auto& o : outs) {
        QItem& it = o.q->items[o.idx];
        if (it.isNote) emitNote(S, T, ti, it.note, o.shift, laneGains[o.q][o.idx]);
        else if (!it.drop) {
            const std::set<int>& targets = rangeTargets[o.q][o.idx];
            emitRangeTo(S, T, ti, it.rangeStart, it.voice, it.art, targets);
            if (debugOn(ti, o.t)) {
                std::string who;
                for (int k : targets) who += (who.empty() ? "" : ", ") + (k ? "lane " + std::to_string(keyLane(k)) + " shift " + std::to_string(keyShift(k)) : std::string("main"));
                LOGI("DEBUG   range %s %s voice %d (before the note at %.3fs) -> %s", it.rangeStart ? "START" : "END",
                     artNames(it.art, 0).c_str(), it.voice, o.t / 1e6, who.empty() ? "main (no notes inside)" : who.c_str());
            }
        }
    }
    if (tremoloSplit)
        LOGI("instrument %d: %d tremolo chord note(s) played on their own instance (one-note tremolo per chord note, balanced %d)",
             ti.instId, tremoloSplit, (int)g_cfg.tremoloBalance);
    debugReport(S, T, ti);
    for (auto& v : ti.voices) {
        VoiceQ& q = v.second;
        q.items.clear();
        q.hasNotes = false;
        q.lastLoc = q.lastEnd = -1;
        q.lastSlur = false;
    }
}

// --- live / offline mixing ------------------------------------------------------

static bool shadowWanted(Sampler& S, Shadow& sh, int n)
{
    if (sh.notes > 0 || sh.auditions > 0) {
        sh.idleSamples = 0;
        return true;
    }
    double sr = S.offline && S.offSr > 0 ? S.offSr : S.sr;
    if (sh.idleSamples < (long long)(g_cfg.idleSeconds * (sr > 0 ? sr : 48000))) {
        sh.idleSamples += n;
        return true;
    }
    sh.needsPrime = true;
    return false;
}

static void teleReport(Sampler& S, Shadow& sh, long long pos)
{
    double sr = S.offline && S.offSr > 0 ? S.offSr : (S.sr > 0 ? S.sr : 48000);
    if (g_logLevel < 2 || sh.teleN < (long long)sr) return;
    double ri = std::sqrt(sh.teleIn / sh.teleN), ro = std::sqrt(sh.teleOut / sh.teleN);
    auto db = [](double r) { return r > 1e-9 ? 20.0 * std::log10(r) : -180.0; };
    if (S.playing || S.offline)
        LOGD("level shadow %+d at %.1fs: engine %.0f dB, after shift %.0f dB", sh.shift, pos / sr, db(ri), db(ro));
    sh.teleIn = sh.teleOut = 0;
    sh.teleN = 0;
}

// Lane output gain at tUs: the latest-started chord-tremolo note still sounding decides.
static float laneGainAt(const Shadow& sh, long long tUs)
{
    if (sh.lane == 0 || sh.gainSpans.empty()) return 1.0f;
    Shadow::GainSpan key { tUs, 0, 0 };
    auto it = std::upper_bound(sh.gainSpans.begin(), sh.gainSpans.end(), key); // first span starting after tUs
    while (it != sh.gainSpans.begin()) {
        --it;
        if (it->end >= tUs) return it->gain;           // latest-started note still sounding
        if (tUs - it->start > 30000000) break;         // nothing that old is still sounding
    }
    return sh.laneGainNow;                             // between notes: keep the last value
}

// Multiplies sh.in (n frames) by the lane gain, ramped from the previous block's value.
static void applyLaneGain(Shadow& sh, int n, long long tUs)
{
    if (sh.lane == 0) return;
    float target = laneGainAt(sh, tUs), g0 = sh.laneGainNow;
    if (g0 == 1.0f && target == 1.0f) return;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) sh.in[c][i] *= g0 + (target - g0) * (float)(i + 1) / (float)n;
    sh.laneGainNow = target;
}

// Unshifted instance (tremolo-chord lane): render in step with the main sampler and add.
static void addDirect(Shadow& sh, int n, ms_OutputBuffer& dst)
{
    if (!dst._channels) return;
    int nc = std::min(dst._num_channels, 2);
    int nn = std::min(n, dst._num_data_pts);
    for (int c = 0; c < nc; ++c) {
        float* o = dst._channels[c];
        for (int i = 0; i < nn; ++i) o[i] += sh.in[c][i];
    }
    if (dst._num_channels == 1)
        for (int i = 0; i < nn; ++i) dst._channels[0][i] += sh.in[1][i];
}

static void mixShadowLive(Sampler& S, Shadow& sh, ms_OutputBuffer& out, long long pos)
{
    int n = out._num_data_pts;
    if (n <= 0) return;
    ensureBuffers(sh, n);
    if (!sh.fx) {
        bool discontinuous = S.playing && sh.expectedPos != LLONG_MIN && pos != sh.expectedPos;
        if (S.playing && (sh.needsPrime || discontinuous) && (!sh.untouchedSinceSet || sh.lastSetPos != pos)) {
            R.ms_MuseSampler_set_position(sh.h, pos);
            sh.lastSetPos = pos;
        }
        if (S.playing) sh.needsPrime = false;
        renderShadow(sh, n, false, pos, S.playing);
        applyLaneGain(sh, n, (long long)(pos / (S.sr > 0 ? S.sr : 48000.0) * 1e6));
        addDirect(sh, n, out);
        sh.expectedPos = S.playing ? pos + n : pos;
        return;
    }
    bool discontinuous = S.playing && sh.expectedPos != LLONG_MIN && pos != sh.expectedPos;
    if (sh.la == 0 || (S.playing && (sh.needsPrime || discontinuous)) || (sh.la % n) != 0) {
        resetFx(sh, n);
        if (S.playing) {
            if (!sh.untouchedSinceSet || sh.lastSetPos != pos) {
                R.ms_MuseSampler_set_position(sh.h, pos);
                sh.lastSetPos = pos;
            }
            for (long long p = pos; p < pos + sh.la; p += n) {
                renderShadow(sh, n, false, p);
                runFx(sh, n, nullptr);
            }
            sh.needsPrime = false;
        }
    }
    renderShadow(sh, n, false, pos + sh.la, S.playing);
    applyLaneGain(sh, n, (long long)((pos + sh.la) / (S.sr > 0 ? S.sr : 48000.0) * 1e6));
    runFx(sh, n, &out);
    if (sh.notes > 0) teleReport(S, sh, pos);
    sh.expectedPos = S.playing ? pos + n : pos;
}

static void mixShadowOffline(Sampler& S, Shadow& sh, ms_OutputBuffer& out)
{
    int n = out._num_data_pts;
    if (n <= 0) return;
    ensureBuffers(sh, n);
    if (!sh.fx) {
        if (sh.needsPrime) {
            R.ms_MuseSampler_set_position(sh.h, S.offPos);
            sh.lastSetPos = S.offPos;
            sh.needsPrime = false;
        }
        renderShadow(sh, n, true, 0);
        applyLaneGain(sh, n, (long long)(S.offPos / (S.offSr > 0 ? S.offSr : 44100.0) * 1e6));
        addDirect(sh, n, out);
        return;
    }
    if (sh.needsPrime || sh.la == 0 || (sh.la % n) != 0) {
        resetFx(sh, n);
        R.ms_MuseSampler_set_position(sh.h, S.offPos);
        sh.lastSetPos = S.offPos;
        for (int p = 0; p < sh.la; p += n) {
            renderShadow(sh, n, true, 0);
            runFx(sh, n, nullptr);
        }
        sh.needsPrime = false;
    }
    renderShadow(sh, n, true, 0);
    applyLaneGain(sh, n, (long long)((S.offPos + sh.la) / (S.offSr > 0 ? S.offSr : 44100.0) * 1e6));
    runFx(sh, n, &out);
    if (sh.notes > 0) teleReport(S, sh, S.offPos);
}

// ---------------------------------------------------------------------------
// Exports: pass-through

FWD(int, ms_get_version_major, (), ())
FWD(int, ms_get_version_minor, (), ())
FWD(int, ms_get_version_revision, (), ())
FWD(int, ms_get_version_build_number, (), ())
FWD(ms_Result, ms_disable_reverb, (), ())
FWD(ms_InstrumentList, ms_get_instrument_list, (), ())
FWD(ms_InstrumentList, ms_get_matching_instrument_list, (const char* a, const char* b), (a, b))
FWD(ms_InstrumentInfo, ms_InstrumentList_get_next, (ms_InstrumentList l), (l))
FWD(int, ms_Instrument_get_id, (ms_InstrumentInfo i), (i))
FWD(const char*, ms_Instrument_get_name, (ms_InstrumentInfo i), (i))
FWD(const char*, ms_Instrument_get_category, (ms_InstrumentInfo i), (i))
FWD(const char*, ms_Instrument_get_pack_name, (ms_InstrumentInfo i), (i))
FWD(const char*, ms_Instrument_get_vendor_name, (ms_InstrumentInfo i), (i))
FWD(const char*, ms_Instrument_get_info_json, (ms_InstrumentInfo i), (i))
FWD(const char*, ms_Instrument_get_musicxml_sound, (ms_InstrumentInfo i), (i))
FWD(const char*, ms_Instrument_get_mpe_sound, (ms_InstrumentInfo i), (i))
FWD(float, ms_Instrument_get_reverb_level, (ms_InstrumentInfo i), (i))
FWD(ms_PresetList, ms_Instrument_get_preset_list, (ms_InstrumentInfo i), (i))
FWD(bool, ms_Instrument_is_online, (ms_InstrumentInfo i), (i))
FWD(const char*, ms_PresetList_get_next, (ms_PresetList l), (l))
FWD(const char*, ms_get_text_articulations, (int id, const char* preset), (id, preset))
FWD(const char*, ms_get_drum_mapping, (int id), (id))
FWD(ms_MuseSampler, ms_MuseSampler_create, (), ())
FWD(int, ms_MuseSampler_is_ranged_articulation, (ms_NoteArticulation a), (a))
FWD(ms_Result, ms_MuseSampler_add_track_note_event_5, (ms_MuseSampler ms, ms_Track t, ms_NoteEvent_4 ev, long long& id), (ms, t, ev, id))
FWD(ms_Result, ms_MuseSampler_add_track_syllable_event, (ms_MuseSampler ms, ms_Track t, ms_SyllableEvent ev), (ms, t, ev))
FWD(ms_Result, ms_MuseSampler_start_audition_note_4, (ms_MuseSampler ms, ms_Track t, ms_AuditionStartNoteEvent_4 ev), (ms, t, ev))
FWD(void, ms_MuseSampler_set_auto_render_interval, (ms_MuseSampler ms, double s), (ms, s))
FWD(void, ms_MuseSampler_trigger_render, (ms_MuseSampler ms), (ms))
FWD(void, ms_MuseSampler_clear_online_cache, (ms_MuseSampler ms), (ms))
FWD(void, ms_MuseSampler_set_lazy_render, (ms_MuseSampler ms, bool e), (ms, e))
FWD(void, ms_MuseSampler_set_rendering_state_changed_callback, (ms_MuseSampler ms, ms_rendering_state_changed_callback cb, void* ud), (ms, cb, ud))
FWD(void, ms_MuseSampler_set_rendering_state_changed_callback_2, (ms_MuseSampler ms, ms_rendering_state_changed_callback cb, void* ud), (ms, cb, ud))
FWD(ms_RenderRangeInfo, ms_RenderProgressInfo_get_next, (ms_RenderingRangeList l), (l))
FWD(ms_RenderRangeInfo2, ms_RenderProgressInfo2_get_next, (ms_RenderingRangeList l), (l))

// ---------------------------------------------------------------------------
// Exports: library lifecycle

// ---------------------------------------------------------------------------
// Calibration mode: launch.cmd sets MSR_CALIBRATE=1 when the Muse Sounds engine
// refuses to start in the standalone probe. Then the range probe runs here,
// right after MuseScore has initialised the engine and before it uses it.

static void progressSink(const char* text)
{
    FILE* f = openFile(g_dir + L"\\probe_progress.txt", L"ab");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void calibrateIfRequested()
{
    if (envPath(L"MSR_CALIBRATE") != L"1") return;
    static bool done = false;
    if (done) return;
    done = true;
#define COPYF(n) A.n = R.n;
    PROBE_FUNCS(COPYF)
#undef COPYF
    LOGE("STATUS calibrating");
    writeStatus("calibrating");
    progressSink("Engine started inside MuseScore; measuring ranges...\r\n");
    int rc = 1;
    try {
        ProbeOptions o;
        o.dir = g_dir;
        if (g_engineVer.empty() && R.ms_get_version_major) {
            char v[64];
            snprintf(v, sizeof(v), "%d.%d.%d", R.ms_get_version_major(), R.ms_get_version_minor(), R.ms_get_version_revision());
            g_engineVer = v;
        }
        o.ver = g_engineVer;
        o.force = envPath(L"MSR_CALIBRATE_FORCE") == L"1";
        g_sink = progressSink;
        rc = runProbe(o);
    } catch (...) {
        LOGE("exception during calibration");
    }
    progressSink(rc == 0 ? "CALIBRATION DONE\r\n" : "CALIBRATION FAILED\r\n");
    g_rangesByKey.clear();
    g_rangesById.clear();
    loadConfigAndRanges();
    {
        Guard g(g_instCs);
        g_instKeyById.clear();
        g_instEnumerated = false;
    }
}

// MuseScore only calls init after its own API compatibility check has passed,
// so this marks the shim as accepted (launch.cmd watches for it).
static ms_Result noteInit(ms_Result r)
{
    if (r == ms_Result_OK) calibrateIfRequested();
    LOGE("STATUS %s", r == ms_Result_OK ? "accepted" : "init-failed");
    writeStatus(r == ms_Result_OK ? "accepted" : "init-failed");
    return r;
}

EXPORT ms_Result ms_init()
{
    ensureReal();
    return noteInit(R.ms_init ? R.ms_init() : ms_Result_Error);
}

EXPORT ms_Result ms_init_2()
{
    ensureReal();
    return noteInit(R.ms_init_2 ? R.ms_init_2() : ms_Result_Error);
}

EXPORT ms_Result ms_reload_all_instruments()
{
    ensureReal();
    if (!R.ms_reload_all_instruments) return ms_Result_Error;
    ms_Result r = R.ms_reload_all_instruments();
    Guard g(g_instCs);
    g_instKeyById.clear();
    g_instEnumerated = false;
    return r;
}


EXPORT void ms_set_logging_callback(ms_logging_callback cb)
{
    ensureReal();
    if (!R.ms_set_logging_callback) return;
    g_hostLogCb = cb;
    R.ms_set_logging_callback(engineLogCb);
}

EXPORT ms_Result ms_deinit()
{
    ensureReal();
    {
        Guard g(g_mapCs);
        for (auto& p : g_samplers) {
            for (auto& sp : p.second->shadows) R.ms_MuseSampler_destroy(sp.second->h);
            p.second->shadows.clear();
        }
        g_samplers.clear();
    }
    LOGI("deinit");
    return R.ms_deinit ? R.ms_deinit() : ms_Result_OK;
}

// ---------------------------------------------------------------------------
// Exports: sampler lifecycle

EXPORT void ms_MuseSampler_destroy(ms_MuseSampler ms)
{
    ensureReal();
    std::unique_ptr<Sampler> st;
    {
        Guard g(g_mapCs);
        auto it = g_samplers.find(ms);
        if (it != g_samplers.end()) {
            st = std::move(it->second);
            g_samplers.erase(it);
        }
    }
    if (st) {
        Guard g(st->cs);
        for (auto& sp : st->shadows) R.ms_MuseSampler_destroy(sp.second->h);
        st->shadows.clear();
    }
    R.ms_MuseSampler_destroy(ms);
}

static ms_Result initCommon(ms_MuseSampler ms, double sr, int block, int ch, bool legacy)
{
    ensureReal();
#ifdef MSR_EXTDLL
    // ms_init itself is not intercepted in the DLL (see msr_wrap_builtin): note it here, once.
    static INIT_ONCE s_noted = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&s_noted, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL { noteInit(ms_Result_OK); return TRUE; }, nullptr, nullptr);
#endif
    ms_Result r = legacy ? R.ms_MuseSampler_init(ms, sr, block, ch) : R.ms_MuseSampler_init_2(ms, sr, block, ch);
    if (r != ms_Result_OK || !g_cfg.enabled) return r;
    Sampler* S = getOrCreateS(ms);
    Guard g(S->cs);
    S->sr = sr;
    S->block = block;
    S->ch = ch;
    S->legacyInit = legacy;
    S->inited = true;
    for (auto& sp : S->shadows) {
        Shadow& sh = *sp.second;
        if (legacy) R.ms_MuseSampler_init(sh.h, sr, block, ch);
        else R.ms_MuseSampler_init_2(sh.h, sr, block, ch);
        configureFx(*S, sh);
    }
    LOGD("sampler %p init sr=%.0f block=%d ch=%d", ms, sr, block, ch);
    return r;
}

EXPORT ms_Result ms_MuseSampler_init(ms_MuseSampler ms, double sr, int block, int ch)
{
    return initCommon(ms, sr, block, ch, true);
}

EXPORT ms_Result ms_MuseSampler_init_2(ms_MuseSampler ms, double sr, int block, int ch)
{
    return initCommon(ms, sr, block, ch, false);
}

EXPORT void ms_MuseSampler_set_score_id(ms_MuseSampler ms, const char* id)
{
    ensureReal();
    if (R.ms_MuseSampler_set_score_id) R.ms_MuseSampler_set_score_id(ms, id);
    Sampler* S = findS(ms);
    if (!S) return;
    Guard g(S->cs);
    S->scoreId = id ? id : "";
    for (auto& sp : S->shadows) R.ms_MuseSampler_set_score_id(sp.second->h, id);
}

// ---------------------------------------------------------------------------
// Exports: tracks and score events

EXPORT ms_Track ms_MuseSampler_add_track(ms_MuseSampler ms, int instrumentId)
{
    ensureReal();
    ms_Track t = R.ms_MuseSampler_add_track(ms, instrumentId);
    Sampler* S = findS(ms);
    if (t && S) {
        Guard g(S->cs);
        TrackInfo& ti = S->tracks[t];
        ti.instId = instrumentId;
        S->dbgLevel = -1;
    }
    return t;
}

EXPORT ms_Result ms_MuseSampler_clear_track(ms_MuseSampler ms, ms_Track t)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_clear_track(ms, t);
    Sampler* S = findS(ms);
    if (!S) return r;
    Guard g(S->cs);
    TrackInfo* ti = trackInfo(*S, t);
    if (!ti) return r;
    ti->voices.clear();
    ti->ids.clear();
    ti->log.clear();
    ti->textArt.clear();
    ti->muteSpans.clear();
    ti->muteNotes.clear();
    ti->sent.clear();
    for (auto& sp : S->shadows) if (sp.second->lane > 0) sp.second->gainSpans.clear();
    for (auto& mp : ti->mirrors) {
        auto sit = S->shadows.find(mp.first);
        if (sit == S->shadows.end()) continue;
        R.ms_MuseSampler_clear_track(sit->second->h, mp.second.t);
        sit->second->notes -= mp.second.notes;
        mp.second.notes = 0;
        mp.second.presetMap.clear();
    }
    return r;
}

EXPORT ms_Result ms_MuseSampler_finalize_track(ms_MuseSampler ms, ms_Track t)
{
    ensureReal();
    Sampler* S = findS(ms);
    if (S) {
        Guard g(S->cs);
        TrackInfo* ti = trackInfo(*S, t);
        if (ti) {
            std::map<int, int> tracksBefore;
            for (auto& sp : S->shadows) tracksBefore[sp.first] = sp.second->tracks;
            try {
                flushTrack(*S, t, *ti);
            } catch (...) {
                LOGE("exception while flushing notes");
            }
            // An instance that gets a new track after other tracks on it were finalized: finalize those
            // again (in MuseScore's own instances every track exists before the first finalize; the
            // engine seems to drop the earlier tracks' dynamics otherwise - Strudwick I:13 basses).
            if (g_cfg.refinalize) {
                for (auto& sp : S->shadows) {
                    auto bt = tracksBefore.find(sp.first);
                    if (bt == tracksBefore.end() || bt->second <= 0 || sp.second->tracks <= bt->second) continue;
                    int again = 0;
                    for (auto& tp : S->tracks) {
                        if (&tp.second == ti) continue;
                        auto mit = tp.second.mirrors.find(sp.first);
                        if (mit == tp.second.mirrors.end()) continue;
                        R.ms_MuseSampler_finalize_track(sp.second->h, mit->second.t);
                        ++again;
                    }
                    if (again) LOGI("instance shift %+d lane %d got a new track: finalized its %d other track(s) again", keyShift(sp.first), keyLane(sp.first), again);
                }
            }
            for (auto& mp : ti->mirrors) {
                auto sit = S->shadows.find(mp.first);
                if (sit != S->shadows.end()) R.ms_MuseSampler_finalize_track(sit->second->h, mp.second.t);
            }
            for (auto& sp : S->shadows) sp.second->createdTick = GetTickCount64();
            int shadowed = 0;
            for (auto& mp : ti->mirrors) shadowed += mp.second.notes;
            if (shadowed) LOGI("track %p (instrument %d): %d note(s) routed to shadow samplers", t, ti->instId, shadowed);
        }
    }
    return R.ms_MuseSampler_finalize_track(ms, t);
}

EXPORT ms_Result ms_MuseSampler_add_track_note_event_6(ms_MuseSampler ms, ms_Track t, ms_NoteEvent_5 ev, long long& eventId)
{
    ensureReal();
    if (g_cfg.enabled && g_cfg.staccatoScale < 1.0
        && (ev._articulation & (ms_NoteArticulation_Staccato | ms_NoteArticulation_Staccatissimo))) {
        ev._duration_us = std::max(30000LL, (long long)(ev._duration_us * g_cfg.staccatoScale));
    }
    Sampler* S = g_cfg.enabled ? findS(ms) : nullptr;
    if (!S) return R.ms_MuseSampler_add_track_note_event_6(ms, t, ev, eventId);
    Guard g(S->cs);
    TrackInfo* ti = trackInfo(*S, t);
    if (ti && muteGainFor(*ti) != 1.0) {
        TrackInfo::MuteNote mn { ev._location_us, ev._location_us + ev._duration_us + 300000,
                                 (ev._articulation & ms_NoteArticulation_Mute) != 0 };
        ti->muteNotes.insert(std::upper_bound(ti->muteNotes.begin(), ti->muteNotes.end(), mn), mn);
        if (mn.muted && ti->textsSeen.insert("<mute mark>").second)
            LOGI("instrument %d: notes with the mute mark (con sord.) -> con sord. gain", ti->instId);
    }
    if (!ti || !rangeFor(*ti)) {
        ms_Result r = R.ms_MuseSampler_add_track_note_event_6(ms, t, ev, eventId);
        if (ti) ti->sent.push_back(TrackInfo::SentNote { ev._location_us, ev._duration_us, ev._pitch, (uint64_t)ev._articulation,
                                                         (uint64_t)ev._articulation_2, ev._voice, 0, r == ms_Result_OK });
        return r;
    }
    try {
        VoiceQ& q = ti->voices[ev._voice];
        const bool slur = (ev._articulation & ms_NoteArticulation_Slur) != 0;
        const long long loc = ev._location_us;
        const long long end = loc + ev._duration_us;
        // The whole voice is held until finalize_track so that runs and phrases can be
        // judged as a whole (MuseScore always finalizes after loading a track).
        QItem item;
        item.isNote = true;
        item.note.ev = ev;
        item.note.id = g_synthId.fetch_add(1);
        item.note.textArt = ti->textArt;
        if (q.hasNotes && loc == q.lastLoc) {
            q.lastSlur = q.lastSlur || slur;
            q.lastEnd = std::max(q.lastEnd, end);
        } else {
            q.lastLoc = loc;
            q.lastEnd = end;
            q.lastSlur = slur;
        }
        q.hasNotes = true;
        IdRoute route;
        route.pending = true;
        route.voice = ev._voice;
        route.idx = q.items.size();
        ti->ids[item.note.id] = route;
        eventId = item.note.id;
        q.items.push_back(std::move(item));
        return ms_Result_OK;
    } catch (...) {
        LOGE("exception in add_note; passing through");
        return R.ms_MuseSampler_add_track_note_event_6(ms, t, ev, eventId);
    }
}

// Route an event_id-bound call (pitch bend / vibrato).
template<typename Info, typename Fn, typename Attach>
static ms_Result routeById(ms_MuseSampler ms, ms_Track t, Info info, Fn realFn, Attach attach)
{
    Sampler* S = findS(ms);
    if (!S) return realFn(ms, t, info);
    Guard g(S->cs);
    TrackInfo* ti = trackInfo(*S, t);
    if (!ti) return realFn(ms, t, info);
    auto it = ti->ids.find(info.event_id);
    if (it == ti->ids.end()) return realFn(ms, t, info);
    IdRoute& r = it->second;
    if (r.pending) {
        auto vit = ti->voices.find(r.voice);
        if (vit != ti->voices.end() && r.idx < vit->second.items.size()) {
            attach(vit->second.items[r.idx].note, info);
            return ms_Result_OK;
        }
        return ms_Result_Error;
    }
    info.event_id = r.realId;
    if (r.shift == 0) return realFn(ms, t, info);
    auto sit = S->shadows.find(r.shift);
    auto mit = ti->mirrors.find(r.shift);
    if (sit == S->shadows.end() || mit == ti->mirrors.end()) return ms_Result_Error;
    return realFn(sit->second->h, mit->second.t, info);
}

EXPORT ms_Result ms_MuseSampler_add_pitch_bend(ms_MuseSampler ms, ms_Track t, ms_PitchBendInfo info)
{
    ensureReal();
    return routeById(ms, t, info, R.ms_MuseSampler_add_pitch_bend,
                     [](NoteRec& n, const ms_PitchBendInfo& i) { n.bends.push_back(i); });
}

EXPORT ms_Result ms_MuseSampler_add_vibrato(ms_MuseSampler ms, ms_Track t, ms_VibratoInfo info)
{
    ensureReal();
    return routeById(ms, t, info, R.ms_MuseSampler_add_vibrato,
                     [](NoteRec& n, const ms_VibratoInfo& i) { n.vibs.push_back(i); });
}

static ms_Result rangeEvent(ms_MuseSampler ms, ms_Track t, int voice, ms_NoteArticulation art, bool start)
{
    ensureReal();
    auto direct = [&]() {
        return start ? R.ms_MuseSampler_add_track_event_range_start(ms, t, voice, art)
                     : R.ms_MuseSampler_add_track_event_range_end(ms, t, voice, art);
    };
    Sampler* S = findS(ms);
    if (!S) return direct();
    Guard g(S->cs);
    TrackInfo* ti = trackInfo(*S, t);
    if (!ti) return direct();
    try {
        // Tracks whose notes are queued (instruments with range data): queue the marker with the
        // voice's notes, also before the voice's first note, so flushTrack can send it only to the
        // instances that play inside the range. Pass-through tracks: straight to the engine.
        if (rangeFor(*ti)) {
            QItem item;
            item.isNote = false;
            item.rangeStart = start;
            item.voice = voice;
            item.art = (uint64_t)art;
            ti->voices[voice].items.push_back(std::move(item));
            return ms_Result_OK;
        }
        return direct();
    } catch (...) {
        LOGE("exception in range event");
        return ms_Result_Error;
    }
}

EXPORT ms_Result ms_MuseSampler_add_track_event_range_start(ms_MuseSampler ms, ms_Track t, int voice, ms_NoteArticulation art)
{
    return rangeEvent(ms, t, voice, art, true);
}

EXPORT ms_Result ms_MuseSampler_add_track_event_range_end(ms_MuseSampler ms, ms_Track t, int voice, ms_NoteArticulation art)
{
    return rangeEvent(ms, t, voice, art, false);
}

// Generic: forward to the main sampler, then mirror + log.
template<typename Fn>
static void mirrorEvent(ms_MuseSampler ms, ms_Track t, Fn makeEntry)
{
    Sampler* S = findS(ms);
    if (!S) return;
    Guard g(S->cs);
    TrackInfo* ti = trackInfo(*S, t);
    if (!ti) return;
    try {
        LogEntry e = makeEntry(*ti);
        broadcast(*S, *ti, e);
    } catch (...) {
        LOGE("exception while mirroring an event");
    }
}

EXPORT ms_Result ms_MuseSampler_add_track_dynamics_event_2(ms_MuseSampler ms, ms_Track t, ms_DynamicsEvent_2 ev)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_add_track_dynamics_event_2(ms, t, ev);
    mirrorEvent(ms, t, [&](TrackInfo&) {
        LogEntry e;
        e.kind = LogEntry::Dyn;
        e.t = ev._location_us;
        e.v = ev._value;
        return e;
    });
    return r;
}

EXPORT ms_Result ms_MuseSampler_add_track_pedal_event_2(ms_MuseSampler ms, ms_Track t, ms_PedalEvent_2 ev)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_add_track_pedal_event_2(ms, t, ev);
    mirrorEvent(ms, t, [&](TrackInfo&) {
        LogEntry e;
        e.kind = LogEntry::Pedal;
        e.t = ev._location_us;
        e.v = ev._value;
        return e;
    });
    return r;
}

EXPORT ms_Result ms_MuseSampler_add_track_text_articulation_event(ms_MuseSampler ms, ms_Track t, ms_TextArticulationEvent ev)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_add_track_text_articulation_event(ms, t, ev);
    mirrorEvent(ms, t, [&](TrackInfo& ti) {
        LogEntry e;
        e.kind = LogEntry::Text;
        e.t = ev._start_us;
        e.s = ev._articulation ? ev._articulation : "";
        ti.textArt = e.s;
        if (muteGainFor(ti) != 1.0) {
            auto span = std::make_pair(ev._start_us, isMuteText(e.s));
            ti.muteSpans.insert(std::upper_bound(ti.muteSpans.begin(), ti.muteSpans.end(), span), span);
            if (ti.textsSeen.insert(e.s).second)
                LOGI("instrument %d: text articulation \"%s\" -> %s", ti.instId, e.s.c_str(), span.second ? "con sord. gain" : "normal");
        }
        return e;
    });
    return r;
}

EXPORT ms_Result ms_MuseSampler_add_track_syllable_event_2(ms_MuseSampler ms, ms_Track t, ms_SyllableEvent2 ev)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_add_track_syllable_event_2(ms, t, ev);
    mirrorEvent(ms, t, [&](TrackInfo&) {
        LogEntry e;
        e.kind = LogEntry::Syll;
        e.t = ev._position_us;
        e.s = ev._text ? ev._text : "";
        e.hyph = ev._hyphened_to_next;
        return e;
    });
    return r;
}

EXPORT ms_PresetChange ms_MuseSampler_create_preset_change(ms_MuseSampler ms, ms_Track t, long long loc)
{
    ensureReal();
    ms_PresetChange h = R.ms_MuseSampler_create_preset_change(ms, t, loc);
    mirrorEvent(ms, t, [&](TrackInfo&) {
        LogEntry e;
        e.kind = LogEntry::PresetCreate;
        e.t = loc;
        e.handle = h;
        return e;
    });
    return h;
}

EXPORT ms_Result ms_MuseSampler_add_preset(ms_MuseSampler ms, ms_Track t, ms_PresetChange pc, const char* name)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_add_preset(ms, t, pc, name);
    mirrorEvent(ms, t, [&](TrackInfo&) {
        LogEntry e;
        e.kind = LogEntry::PresetAdd;
        e.handle = pc;
        e.s = name ? name : "";
        return e;
    });
    return r;
}

// ---------------------------------------------------------------------------
// Exports: audition (clicking notes, MIDI input)

EXPORT ms_Result ms_MuseSampler_start_audition_note_5(ms_MuseSampler ms, ms_Track t, ms_AuditionStartNoteEvent_5 ev)
{
    ensureReal();
    Sampler* S = g_cfg.enabled ? findS(ms) : nullptr;
    if (!S) return R.ms_MuseSampler_start_audition_note_5(ms, t, ev);
    Guard g(S->cs);
    TrackInfo* ti = trackInfo(*S, t);
    const InstRange* r = ti ? rangeFor(*ti) : nullptr;
    if (r) {
        std::string text = ev._active_text_articulation ? ev._active_text_articulation : "";
        int top = r->noteTop(ev._articulation, text, Legato_None);
        int lowA = r->low >= 0 ? r->low : std::max(0, r->classTop("sustain") - 36);
        int shift = shiftFor(ev._pitch, top, lowA);
        auto key0 = std::make_pair(t, ev._pitch);
        auto old = S->audition.find(key0);
        if (old != S->audition.end() && old->second != shift) { // re-trigger with different routing
            auto sit = S->shadows.find(old->second);
            auto mit = ti->mirrors.find(old->second);
            if (sit != S->shadows.end() && mit != ti->mirrors.end()) {
                R.ms_MuseSampler_stop_audition_note(sit->second->h, mit->second.t, ms_AuditionStopNoteEvent { ev._pitch - old->second });
                if (sit->second->auditions > 0) sit->second->auditions--;
            }
            S->audition.erase(old);
        }
        if (shift != 0) {
            MirrorTrack* m = ensureMirror(*S, *ti, shift);
            if (m) {
                Shadow* sh = S->shadows[shift].get();
                ms_AuditionStartNoteEvent_5 ev2 = ev;
                ev2._pitch -= shift;
                auto key = std::make_pair(t, ev._pitch);
                if (!S->audition.count(key)) sh->auditions++;
                S->audition[key] = shift;
                sh->idleSamples = 0;
                LOGD("audition %s -> shadow %+d", noteName(ev._pitch).c_str(), shift);
                return R.ms_MuseSampler_start_audition_note_5(sh->h, m->t, ev2);
            }
        }
    }
    return R.ms_MuseSampler_start_audition_note_5(ms, t, ev);
}

EXPORT ms_Result ms_MuseSampler_stop_audition_note(ms_MuseSampler ms, ms_Track t, ms_AuditionStopNoteEvent ev)
{
    ensureReal();
    Sampler* S = findS(ms);
    if (S) {
        Guard g(S->cs);
        auto it = S->audition.find(std::make_pair(t, ev._pitch));
        if (it != S->audition.end()) {
            int shift = it->second;
            S->audition.erase(it);
            TrackInfo* ti = trackInfo(*S, t);
            auto sit = S->shadows.find(shift);
            if (ti && sit != S->shadows.end()) {
                auto mit = ti->mirrors.find(shift);
                if (sit->second->auditions > 0) sit->second->auditions--;
                if (mit != ti->mirrors.end()) {
                    ms_AuditionStopNoteEvent ev2 { ev._pitch - shift };
                    return R.ms_MuseSampler_stop_audition_note(sit->second->h, mit->second.t, ev2);
                }
            }
        }
    }
    return R.ms_MuseSampler_stop_audition_note(ms, t, ev);
}

EXPORT ms_Result ms_MuseSampler_add_audition_cc_event(ms_MuseSampler ms, ms_Track t, int cc, float value)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_add_audition_cc_event(ms, t, cc, value);
    Sampler* S = findS(ms);
    if (!S) return r;
    Guard g(S->cs);
    TrackInfo* ti = trackInfo(*S, t);
    if (!ti) return r;
    for (auto& mp : ti->mirrors) {
        auto sit = S->shadows.find(mp.first);
        if (sit != S->shadows.end()) R.ms_MuseSampler_add_audition_cc_event(sit->second->h, mp.second.t, cc, value);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Exports: transport and rendering

EXPORT ms_Result ms_MuseSampler_all_notes_off(ms_MuseSampler ms)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_all_notes_off(ms);
    Sampler* S = findS(ms);
    if (!S) return r;
    Guard g(S->cs);
    for (auto& sp : S->shadows) {
        R.ms_MuseSampler_all_notes_off(sp.second->h);
        sp.second->auditions = 0;
    }
    S->audition.clear();
    return r;
}

EXPORT void ms_MuseSampler_set_position(ms_MuseSampler ms, long long samples)
{
    ensureReal();
    R.ms_MuseSampler_set_position(ms, samples);
    Sampler* S = findS(ms);
    if (!S) return;
    Guard g(S->cs);
    S->pos = samples;
    S->offPos = samples;
    for (auto& sp : S->shadows) {
        Shadow& sh = *sp.second;
        if (sh.notes > 0 || sh.auditions > 0) {
            R.ms_MuseSampler_set_position(sh.h, samples);
            sh.lastSetPos = samples;
            sh.untouchedSinceSet = true;
        }
        sh.needsPrime = true;
    }
}

EXPORT void ms_MuseSampler_set_playing(ms_MuseSampler ms, int playing)
{
    ensureReal();
    R.ms_MuseSampler_set_playing(ms, playing);
    Sampler* S = findS(ms);
    if (!S) return;
    Guard g(S->cs);
    S->playing = playing != 0;
    for (auto& sp : S->shadows) {
        R.ms_MuseSampler_set_playing(sp.second->h, playing);
        if (playing) sp.second->needsPrime = true;
    }
}

EXPORT ms_Result ms_MuseSampler_start_offline_mode(ms_MuseSampler ms, double sr)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_start_offline_mode(ms, sr);
    Sampler* S = findS(ms);
    if (!S) return r;
    Guard g(S->cs);
    S->offline = true;
    S->offSr = sr;
    S->offPos = S->pos;
    for (auto& sp : S->shadows) {
        R.ms_MuseSampler_start_offline_mode(sp.second->h, sr);
        configureFx(*S, *sp.second);
    }
    LOGI("sampler %p: offline mode at %.0f Hz (%d shadow(s))", ms, sr, (int)S->shadows.size());
    return r;
}

EXPORT ms_Result ms_MuseSampler_stop_offline_mode(ms_MuseSampler ms)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_stop_offline_mode(ms);
    Sampler* S = findS(ms);
    if (!S) return r;
    Guard g(S->cs);
    S->offline = false;
    for (auto& sp : S->shadows) {
        R.ms_MuseSampler_stop_offline_mode(sp.second->h);
        configureFx(*S, *sp.second);
    }
    return r;
}

// A NaN/inf (or absurd) sample from any one instrument turns MuseScore's whole mix silent.
// Zero such samples and log where they came from ("engine" = Muse Sounds itself,
// "shift" = our shadow/pitch-shift mix), so the rest of the mix survives.
static std::atomic<int> g_badLogs { 0 };
static std::string instName(int instId)
{
    Guard g(g_instCs);
    auto it = g_instKeyById.find(instId);
    return it != g_instKeyById.end() ? it->second : ("instrument " + std::to_string(instId));
}

static std::string artNames(uint64_t a1, uint64_t a2)
{
    std::string r;
    uint64_t known = 0;
    static const struct { uint64_t bit; const char* name; } trem[] = {
        { (uint64_t)ms_NoteArticulation_Tremolo1, "tremolo8" }, { (uint64_t)ms_NoteArticulation_Tremolo2, "tremolo16" },
        { (uint64_t)ms_NoteArticulation_Tremolo3, "tremolo32" }, { (uint64_t)ms_NoteArticulation_BuzzTremolo, "buzz" } };
    for (const auto& tr : trem)
        if (a1 & tr.bit) { known |= tr.bit; r += std::string(r.empty() ? "" : "+") + tr.name; }
    for (const auto& bc : kBitClasses) {
        if (bc.bits & known) continue;
        if (a1 & bc.bits) {
            known |= bc.bits;
            if (r.find(bc.name) == std::string::npos) r += std::string(r.empty() ? "" : "+") + bc.name;
        }
    }
    if (a1 & ms_NoteArticulation_Slur) { known |= ms_NoteArticulation_Slur; r += std::string(r.empty() ? "" : "+") + "slur"; }
    char hex[48];
    if ((a1 & ~known) || a2) {
        snprintf(hex, sizeof(hex), "%s0x%llx/0x%llx", r.empty() ? "" : "+", (unsigned long long)(a1 & ~known), (unsigned long long)a2);
        r += hex;
    }
    return r.empty() ? "plain" : r;
}

// Which instrument(s) this sampler plays, and what the engine had around tUs: the notes it was
// sent (with their articulations and lengths as sent) and the dynamics values. Written once
// per sampler per 1.5 s of score time, so a cluster of bad blocks gives one report.
static void reportBadAudio(Sampler& S, long long tUs, long long spanUs)
{
    static std::atomic<int> reports { 0 };
    if (S.lastBadDetailUs != LLONG_MIN && std::llabs(tUs - S.lastBadDetailUs) < 1500000) return;
    S.lastBadDetailUs = tUs;
    if (reports.fetch_add(1) >= 60) return;
    const long long a = tUs - 600000, b = tUs + spanUs + 300000;
    for (auto& tp : S.tracks) {
        TrackInfo& ti = tp.second;
        LOGI("  bad-audio report for %s at %.3fs (notes sent to the engine from %.2fs to %.2fs):",
             instName(ti.instId).c_str(), tUs / 1e6, a / 1e6, b / 1e6);
        int shown = 0;
        for (const auto& n : ti.sent) {
            if (n.loc + n.dur < a || n.loc > b) continue;
            if (++shown > 24) { LOGI("    ..."); break; }
            LOGI("    %s @%.3fs dur %lldms voice %d %s%s%s", noteName(n.pitch).c_str(), n.loc / 1e6, n.dur / 1000, n.voice,
                 artNames(n.a1, n.a2).c_str(), n.key ? (" -> shadow " + std::to_string(keyShift(n.key)) + "/lane " + std::to_string(keyLane(n.key))).c_str() : "",
                 n.ok ? "" : " (REJECTED by engine)");
        }
        if (!shown) LOGI("    (no notes)");
        double before = -1;
        std::string dyn;
        for (const auto& e : ti.log) {
            if (e.kind != LogEntry::Dyn) continue;
            if (e.t <= a) before = e.v;
            else if (e.t <= b) { char buf[64]; snprintf(buf, sizeof(buf), " %.3fs=%.3f", e.t / 1e6, e.v); dyn += buf; }
        }
        LOGI("    dynamics: %s%s", before >= 0 ? ("level " + std::to_string(before) + " before;").c_str() : "none before;", dyn.empty() ? " no changes in the window" : dyn.c_str());
    }
}

static void sanitize(Sampler& S, ms_OutputBuffer& buf, const char* stage, double posSec)
{
    if (!buf._channels) return;
    int bad = 0, nans = 0, infs = 0, first = -1, last = -1;
    for (int c = 0; c < buf._num_channels; ++c) {
        float* o = buf._channels[c];
        if (!o) continue;
        for (int i = 0; i < buf._num_data_pts; ++i) {
            float x = o[i];
            if (!std::isfinite(x) || std::fabs(x) > 32.0f) {
                if (std::isnan(x)) ++nans; else if (std::isinf(x)) ++infs;
                if (first < 0 || i < first) first = i;
                if (i > last) last = i;
                o[i] = 0.0f;
                ++bad;
            }
        }
    }
    if (!bad) return;
    if (g_badLogs.fetch_add(1) < 400) {
        std::string who;
        for (auto& tp : S.tracks) who += (who.empty() ? "" : ", ") + instName(tp.second.instId);
        LOGI("bad audio from %s (%s; %d shadow(s)) at %.3fs: %d sample(s) zeroed (%d NaN, %d inf, %d huge), frames %d-%d of %d",
             stage, who.empty() ? "no tracks" : who.c_str(), (int)S.shadows.size(), posSec, bad, nans, infs, bad - nans - infs,
             first, last, buf._num_data_pts);
    }
    if (std::strcmp(stage, "engine") == 0) {
        double sr = S.offline && S.offSr > 0 ? S.offSr : (S.sr > 0 ? S.sr : 48000);
        reportBadAudio(S, (long long)(posSec * 1e6), (long long)(buf._num_data_pts / sr * 1e6));
    }
}

// ---- debug_instrument: measured audio levels (what was actually heard), 100 ms windows ----
static bool samplerDebug(Sampler& S)
{
    if (S.dbgLevel < 0) {
        S.dbgLevel = 0;
        if (!g_cfg.debugInst.empty() && g_cfg.debugToUs > g_cfg.debugFromUs)
            for (auto& tp : S.tracks)
                if (lower(instName(tp.second.instId)).find(g_cfg.debugInst) != std::string::npos) S.dbgLevel = 1;
    }
    return S.dbgLevel == 1;
}

static double sumSq(float* const* ch, int nch, int n)
{
    double s = 0;
    for (int c = 0; c < nch; ++c) {
        if (!ch[c]) continue;
        for (int i = 0; i < n; ++i) s += (double)ch[c][i] * ch[c][i];
    }
    return s;
}

static void dbgLevelBlock(Sampler& S, long long tUs, int n, double mainSq, const std::map<int, double>& shSq, const char* mode)
{
    static std::atomic<int> lines { 0 };
    const long long win = tUs / 100000;
    if (win != S.dbgWin) {
        const long long w0 = S.dbgWin * 100000;
        if (S.dbgWin != LLONG_MIN && S.dbgN > 0 && w0 >= g_cfg.debugFromUs && w0 <= g_cfg.debugToUs && lines.fetch_add(1) < 3000) {
            auto db = [&](double sq) { double r = std::sqrt(sq / (2.0 * S.dbgN)); return r > 1e-7 ? 20.0 * std::log10(r) : -140.0; };
            std::string sh;
            for (auto& p : S.dbgSh) {
                char b[64];
                snprintf(b, sizeof(b), " | lane %d shift %+d: %6.1f dB", keyLane(p.first), keyShift(p.first), db(p.second));
                sh += b;
            }
            LOGI("DEBUG level %s %.1fs: engine (main) %6.1f dB%s", mode, w0 / 1e6, db(S.dbgMain), sh.c_str());
        }
        S.dbgWin = win;
        S.dbgMain = 0;
        S.dbgSh.clear();
        S.dbgN = 0;
    }
    S.dbgMain += mainSq;
    for (auto& p : shSq) S.dbgSh[p.first] += p.second;
    S.dbgN += n;
}

EXPORT ms_Result ms_MuseSampler_process_offline(ms_MuseSampler ms, ms_OutputBuffer buf)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_process_offline(ms, buf);
    if (r != ms_Result_OK) return r;
    Sampler* S = findS(ms);
    if (!S) return r;
    Guard g(S->cs);
    const double offSec = S->offPos / (S->offSr > 0 ? S->offSr : 44100.0);
    sanitize(*S, buf, "engine", offSec);
    const bool dbg = samplerDebug(*S);
    const double mainSq = dbg ? sumSq(buf._channels, std::min(buf._num_channels, 2), buf._num_data_pts) : 0;
    std::map<int, double> shSq;
    try {
        for (auto& sp : S->shadows) {
            bool w = shadowWanted(*S, *sp.second, buf._num_data_pts);
            if (w) mixShadowOffline(*S, *sp.second, buf);
            if (dbg && sp.second->notes > 0) {
                float* ch[2] = { sp.second->in[0].data(), sp.second->in[1].data() };
                shSq[sp.first] = w ? sumSq(ch, 2, buf._num_data_pts) : 0;
            }
        }
    } catch (...) {
        LOGE("exception in offline shadow render");
    }
    applyMuteGain(*S, buf, (long long)(offSec * 1e6));
    if (!S->shadows.empty()) sanitize(*S, buf, "shift", offSec);
    if (dbg) dbgLevelBlock(*S, (long long)(offSec * 1e6), buf._num_data_pts, mainSq, shSq, "export");
    S->offPos += buf._num_data_pts;
    return r;
}

EXPORT ms_Result ms_MuseSampler_process(ms_MuseSampler ms, ms_OutputBuffer buf, long long samples)
{
    ensureReal();
    ms_Result r = R.ms_MuseSampler_process(ms, buf, samples);
    if (r != ms_Result_OK) return r;
    Sampler* S = findS(ms);
    if (!S) return r;
    Guard g(S->cs);
    S->pos = samples;
    const double liveSec = samples / (S->sr > 0 ? S->sr : 48000.0);
    sanitize(*S, buf, "engine", liveSec);
    const bool dbg = S->playing && samplerDebug(*S);
    const double mainSq = dbg ? sumSq(buf._channels, std::min(buf._num_channels, 2), buf._num_data_pts) : 0;
    std::map<int, double> shSq;
    try {
        for (auto& sp : S->shadows) {
            bool w = shadowWanted(*S, *sp.second, buf._num_data_pts);
            if (w) mixShadowLive(*S, *sp.second, buf, samples);
            if (dbg && sp.second->notes > 0) {
                float* ch[2] = { sp.second->in[0].data(), sp.second->in[1].data() };
                shSq[sp.first] = w ? sumSq(ch, 2, buf._num_data_pts) : 0;
            }
        }
    } catch (...) {
        LOGE("exception in shadow render");
    }
    applyMuteGain(*S, buf, (long long)(liveSec * 1e6));
    if (!S->shadows.empty()) sanitize(*S, buf, "shift", liveSec);
    if (dbg) dbgLevelBlock(*S, (long long)(liveSec * 1e6), buf._num_data_pts, mainSq, shSq, "live");
    return r;
}

EXPORT bool ms_MuseSampler_ready_to_play(ms_MuseSampler ms)
{
    ensureReal();
    bool ready = R.ms_MuseSampler_ready_to_play(ms);
    if (!ready) return false;
    Sampler* S = findS(ms);
    if (!S) return true;
    Guard g(S->cs);
    for (auto& sp : S->shadows) {
        Shadow& sh = *sp.second;
        if (sh.notes <= 0 || sh.gaveUp) continue;
        if (R.ms_MuseSampler_ready_to_play(sh.h)) {
            sh.everReady = true;
            continue;
        }
        if (GetTickCount64() - sh.createdTick < (sh.everReady ? 15000ULL : 8000ULL)) return false;
        if (!sh.everReady) {
            sh.gaveUp = true;
            LOGI("shadow %+d does not report ready; not waiting for it again", sh.shift);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------

#ifndef MSR_INTREE
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
    // The real engine is intentionally not freed here (no FreeLibrary under the loader lock).
    (void)reason;
    (void)reserved;
    return TRUE;
}
#endif

#ifdef MSR_INTREE
// Called by MuseSamplerLibHandler::libFunc for every engine function it looks up.
// Records the real function and returns the range-extending wrapper where there is one.
#define MSR_INTERCEPTED(X) \
    X(ms_init) X(ms_init_2) X(ms_deinit) X(ms_reload_all_instruments) X(ms_set_logging_callback) \
    X(ms_MuseSampler_destroy) X(ms_MuseSampler_init) X(ms_MuseSampler_init_2) X(ms_MuseSampler_set_score_id) \
    X(ms_MuseSampler_add_track) X(ms_MuseSampler_clear_track) X(ms_MuseSampler_finalize_track) \
    X(ms_MuseSampler_add_track_note_event_6) X(ms_MuseSampler_add_pitch_bend) X(ms_MuseSampler_add_vibrato) \
    X(ms_MuseSampler_add_track_event_range_start) X(ms_MuseSampler_add_track_event_range_end) \
    X(ms_MuseSampler_add_track_dynamics_event_2) X(ms_MuseSampler_add_track_pedal_event_2) \
    X(ms_MuseSampler_add_track_text_articulation_event) X(ms_MuseSampler_add_track_syllable_event_2) \
    X(ms_MuseSampler_create_preset_change) X(ms_MuseSampler_add_preset) \
    X(ms_MuseSampler_start_audition_note_5) X(ms_MuseSampler_stop_audition_note) X(ms_MuseSampler_add_audition_cc_event) \
    X(ms_MuseSampler_all_notes_off) X(ms_MuseSampler_set_position) X(ms_MuseSampler_set_playing) \
    X(ms_MuseSampler_start_offline_mode) X(ms_MuseSampler_stop_offline_mode) X(ms_MuseSampler_process_offline) \
    X(ms_MuseSampler_process) X(ms_MuseSampler_ready_to_play)

// The extension can also live in a separate DLL (same source, built with -DMSR_EXTDLL), so it
// can be rebuilt and swapped without rebuilding MuseScore: if <MSR_DIR>\ext\MuseRangeShim.dll
// exists and exports msr_wrap_ext, every lookup goes there instead of the built-in copy below.
static void* msr_wrap_builtin(const char* name, void* real);

#ifdef MSR_EXTDLL
extern "C" __declspec(dllexport) void* msr_wrap_ext(const char* name, void* real)
{
    return msr_wrap_builtin(name, real);
}
extern "C" __declspec(dllexport) const char* msr_ext_build()
{
    return MSR_EXT_BUILD;
}
#else
typedef void* (*msr_wrap_fn)(const char*, void*);
static msr_wrap_fn loadExternalShim()
{
    std::wstring dir = envPath(L"MSR_DIR");
    if (dir.empty()) dir = envPath(L"LOCALAPPDATA") + L"\\MuseRangeShim";
    std::wstring path = dir + L"\\ext\\MuseRangeShim.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) return nullptr;
    return (msr_wrap_fn)(void*)GetProcAddress(h, "msr_wrap_ext");
}

extern "C" void* msr_wrap(const char* name, void* real)
{
    static msr_wrap_fn ext = loadExternalShim(); // thread-safe one-time init
    if (ext) return ext(name, real);
    return msr_wrap_builtin(name, real);
}
#endif

static void* msr_wrap_builtin(const char* name, void* real)
{
    if (!name || !real) return real;
#define MSR_SETR(n) if (std::strcmp(name, #n) == 0) R.n = (n##_t)real;
    MS_FUNCS(MSR_SETR)
#undef MSR_SETR
    ensureReal();
    if (!g_cfg.enabled) return real;
#ifdef MSR_EXTDLL
    // Muse Sounds refuses to initialise when ms_init/ms_init_2 is called from anywhere but MuseScore's own
    // code (2026-10-28: "init-failed" with these wrapped in the DLL). Let MuseScore call them directly; the
    // DLL does its start-up work (status, calibration) when the first sampler is initialised instead.
    if (std::strcmp(name, "ms_init") == 0 || std::strcmp(name, "ms_init_2") == 0) return real;
#endif
#define MSR_WRAP(n) if (std::strcmp(name, #n) == 0) return (void*)&n;
    MSR_INTERCEPTED(MSR_WRAP)
#undef MSR_WRAP
    return real;
}
#endif

#endif // MSR_STUB
