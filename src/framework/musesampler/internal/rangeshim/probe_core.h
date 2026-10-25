// Range-probe core, shared by msr-probe.exe and MuseRangeShim.dll (which can
// run it inside MuseScore when the engine refuses to start elsewhere).
//
// msr-probe: measures, for every installed Muse Sounds instrument, the highest
// (and lowest) pitch that actually produces sound, per articulation, by
// rendering short test notes offline through the real engine. Writes
// ranges.ini next to the executable for MuseRangeShim.dll.
//
// Usage: msr-probe.exe [--only TEXT]... [--force] [--quick] [--jobs N] [--list]

#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <ctime>
#include <mutex>
#include <set>
#include <thread>

#pragma once
#include "common.h"

using namespace msr;

#define PROBE_FUNCS(X) \
    X(ms_get_version_major) X(ms_get_version_minor) X(ms_get_version_revision) \
    X(ms_init) X(ms_init_2) X(ms_deinit) X(ms_disable_reverb) X(ms_set_logging_callback) \
    X(ms_get_instrument_list) X(ms_InstrumentList_get_next) X(ms_Instrument_get_id) X(ms_Instrument_get_name) \
    X(ms_Instrument_get_category) X(ms_Instrument_get_pack_name) X(ms_Instrument_get_vendor_name) \
    X(ms_Instrument_get_info_json) X(ms_Instrument_is_online) X(ms_Instrument_get_preset_list) \
    X(ms_PresetList_get_next) X(ms_get_text_articulations) X(ms_get_drum_mapping) \
    X(ms_MuseSampler_create) X(ms_MuseSampler_destroy) X(ms_MuseSampler_init_2) X(ms_MuseSampler_add_track) \
    X(ms_MuseSampler_finalize_track) X(ms_MuseSampler_add_track_note_event_6) \
    X(ms_MuseSampler_add_track_dynamics_event_2) X(ms_MuseSampler_add_track_text_articulation_event) \
    X(ms_MuseSampler_start_offline_mode) X(ms_MuseSampler_stop_offline_mode) X(ms_MuseSampler_process_offline) \
    X(ms_MuseSampler_process) X(ms_MuseSampler_set_position) X(ms_MuseSampler_set_playing) \
    X(ms_MuseSampler_ready_to_play) X(ms_MuseSampler_is_ranged_articulation) \
    X(ms_MuseSampler_add_track_event_range_start) X(ms_MuseSampler_add_track_event_range_end)

struct Api {
#define DECL(n) n##_t n = nullptr;
    PROBE_FUNCS(DECL)
#undef DECL
};
static Api A;

static std::mutex g_out;
static FILE* g_report = nullptr;

static void defaultSink(const char* s)
{
    fputs(s, stdout);
    fflush(stdout);
}
static void (*g_sink)(const char*) = defaultSink;

static void say(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(g_out);
    g_sink(buf);
}

static void report(const std::string& s)
{
    std::lock_guard<std::mutex> l(g_out);
    if (g_report) {
        fputs(s.c_str(), g_report);
        fflush(g_report);
    }
}

static void engineLog(ms_ErrorLevel level, const char* msg)
{
    static const char* names[] = { "debug", "info", "warning", "error" };
    report(std::string("engine ") + names[(level >= 0 && level <= 3) ? level : 3] + ": " + (msg ? msg : "") + "\n");
}

// ---------------------------------------------------------------------------

struct Inst {
    int id = -1;
    std::string name, pack, category, vendor, key, infoJson, textArtsRaw, preset;
    bool online = false;
    bool drums = false;
    std::vector<std::string> textArts;
};

struct TestNote {
    int pitch;
    long long start, dur; // us
    uint64_t art;
    std::string cls; // class to attribute the measurement to ("" = not measured)
    long long mStart, mEnd; // measurement window (us)
    bool pitchCheck = false; // require the energy to be at this pitch, not the previous note's
    int rangeEdge = 0;       // +1: open a Slur range before this note, -1: close it after
};

struct TestText {
    long long at;
    std::string text;
};

static const double SR = 48000.0;
static const int BLOCK = 1024;
static bool g_liveFallback = false;

static std::vector<std::string> splitArts(const std::string& raw)
{
    std::vector<std::string> out;
    std::string cur;
    bool json = !raw.empty() && (raw[0] == '[' || raw[0] == '{');
    if (json) {
        bool in = false;
        for (char c : raw) {
            if (c == '"') {
                if (in) {
                    out.push_back(cur);
                    cur.clear();
                }
                in = !in;
            } else if (in) {
                cur += c;
            }
        }
    } else {
        for (char c : raw) {
            if (c == '|' || c == '\n' || c == ',' || c == ';') {
                out.push_back(trim(cur));
                cur.clear();
            } else {
                cur += c;
            }
        }
        out.push_back(trim(cur));
    }
    std::vector<std::string> clean;
    std::set<std::string> seen;
    for (auto& s : out) {
        std::string t = trim(s);
        if (t.empty() || t.find_first_of("=;[]") != std::string::npos) continue;
        if (seen.insert(lower(t)).second) clean.push_back(t);
    }
    if (clean.size() > 10) clean.resize(10);
    return clean;
}

// Renders the notes on a fresh sampler and returns RMS per note window.
static bool render(const Inst& in, const std::vector<TestNote>& notes, const std::vector<TestText>& texts,
                   std::vector<double>& rms, bool live, std::string& err)
{
    long long endUs = 0;
    for (auto& n : notes) endUs = std::max(endUs, n.start + n.dur);
    endUs += 1000000;

    ms_MuseSampler ms = A.ms_MuseSampler_create();
    if (!ms) {
        err = "create failed";
        return false;
    }
    struct Destroy {
        ms_MuseSampler m;
        ~Destroy() { A.ms_MuseSampler_destroy(m); }
    } destroyer { ms };

    if (A.ms_MuseSampler_init_2(ms, SR, BLOCK, 2) != ms_Result_OK) {
        err = "init failed";
        return false;
    }
    ms_Track tr = A.ms_MuseSampler_add_track(ms, in.id);
    if (!tr) {
        err = "add_track failed";
        return false;
    }
    A.ms_MuseSampler_add_track_dynamics_event_2(ms, tr, ms_DynamicsEvent_2 { 0, 64.0 / 127.0 });
    for (auto& t : texts) {
        ms_TextArticulationEvent ev;
        ev._start_us = t.at;
        ev._articulation = t.text.c_str();
        A.ms_MuseSampler_add_track_text_articulation_event(ms, tr, ev);
    }
    for (auto& n : notes) {
        ms_NoteEvent_5 ev {};
        ev._voice = 0;
        ev._location_us = n.start;
        ev._duration_us = n.dur;
        ev._pitch = n.pitch;
        ev._tempo = 120.0;
        ev._offset_cents = 0;
        ev._articulation = (ms_NoteArticulation)n.art;
        ev._articulation_2 = ms_NoteArticulation2_None;
        ev._notehead = ms_NoteHead_Normal;
        bool ranged = A.ms_MuseSampler_is_ranged_articulation && A.ms_MuseSampler_is_ranged_articulation(ms_NoteArticulation_Slur);
        if (ranged && n.rangeEdge > 0) A.ms_MuseSampler_add_track_event_range_start(ms, tr, 0, ms_NoteArticulation_Slur);
        long long id = 0;
        A.ms_MuseSampler_add_track_note_event_6(ms, tr, ev, id);
        if (ranged && n.rangeEdge < 0) A.ms_MuseSampler_add_track_event_range_end(ms, tr, 0, ms_NoteArticulation_Slur);
    }
    A.ms_MuseSampler_finalize_track(ms, tr);

    auto t0 = std::chrono::steady_clock::now();
    while (!A.ms_MuseSampler_ready_to_play(ms)) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(180)) {
            err = "engine never became ready (180 s)";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    long long total = (long long)(endUs * SR / 1e6);
    std::vector<float> L(BLOCK), Rr(BLOCK);
    std::vector<float> mono(total + BLOCK, 0.0f);
    float* ch[2] = { L.data(), Rr.data() };
    ms_OutputBuffer buf { ch, BLOCK, 2 };

    A.ms_MuseSampler_set_position(ms, 0);
    if (!live) A.ms_MuseSampler_start_offline_mode(ms, SR);
    A.ms_MuseSampler_set_playing(ms, 1);
    for (long long pos = 0; pos < total; pos += BLOCK) {
        std::fill(L.begin(), L.end(), 0.0f);
        std::fill(Rr.begin(), Rr.end(), 0.0f);
        ms_Result r = live ? A.ms_MuseSampler_process(ms, buf, pos) : A.ms_MuseSampler_process_offline(ms, buf);
        if (r != ms_Result_OK) {
            err = "process failed";
            break;
        }
        for (int i = 0; i < BLOCK; ++i) mono[pos + i] = 0.5f * (L[i] + Rr[i]);
        if (live) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    A.ms_MuseSampler_set_playing(ms, 0);
    if (!live) A.ms_MuseSampler_stop_offline_mode(ms);

    rms.assign(notes.size(), 0.0);
    for (size_t i = 0; i < notes.size(); ++i) {
        long long a = (long long)(notes[i].mStart * SR / 1e6), b = (long long)(notes[i].mEnd * SR / 1e6);
        a = std::max(0LL, std::min(a, total));
        b = std::max(a + 1, std::min(b, total));
        double acc = 0;
        for (long long k = a; k < b; ++k) acc += (double)mono[k] * mono[k];
        rms[i] = std::sqrt(acc / (double)(b - a));
        if (notes[i].pitchCheck && rms[i] > 0) {
            // Energy at this note's fundamental must beat the previous (semitone lower) note's,
            // otherwise what we hear is just the release tail of the first note.
            auto goertzel = [&](int pitch) {
                double f = 440.0 * std::pow(2.0, (pitch - 69) / 12.0), best = 0;
                for (int hmul = 1; hmul <= 2; ++hmul) {
                    double w = 2 * 3.14159265358979 * f * hmul / SR, c = 2 * std::cos(w), s1 = 0, s2 = 0;
                    for (long long k = a; k < b; ++k) { double s0 = mono[k] + c * s1 - s2; s2 = s1; s1 = s0; }
                    best = std::max(best, s1 * s1 + s2 * s2 - c * s1 * s2);
                }
                return best;
            };
            if (goertzel(notes[i].pitch) < 2.0 * goertzel(notes[i].pitch - 1)) rms[i] = 0;
        }
    }
    return err.empty();
}

struct Result {
    InstRange r;
    std::string diag;
    bool ok = false;
};

static const long long SPACING = 300000, DUR = 200000, MEAS_OFF = 30000;

static void addScan(std::vector<TestNote>& v, long long& t, const std::string& cls, uint64_t art, int from, int to)
{
    int step = from > to ? -1 : 1;
    for (int p = from;; p += step) {
        v.push_back(TestNote { p, t, DUR, art, cls, t + MEAS_OFF, t + DUR });
        t += SPACING;
        if (p == to) break;
    }
    t += 700000;
}

// Evaluate: per class, highest (or lowest) pitch whose window is clearly above the noise floor.
static void evaluate(const std::vector<TestNote>& notes, const std::vector<double>& rms,
                     std::map<std::string, int>& tops, std::map<std::string, int>& lows,
                     std::map<std::string, double>& peaks)
{
    std::map<std::string, std::map<int, bool> > sounding;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (notes[i].cls.empty()) continue;
        double& pk = peaks[notes[i].cls];
        pk = std::max(pk, rms[i]);
    }
    for (size_t i = 0; i < notes.size(); ++i) {
        const std::string& c = notes[i].cls;
        if (c.empty()) continue;
        double thr = std::max(peaks[c] * 0.01, 3e-5); // -40 dB re loudest note of the class
        if (peaks[c] < 3e-4) continue;                 // class produced (almost) nothing
        sounding[c][notes[i].pitch] = rms[i] > thr;
    }
    // Ignore isolated blips: the top (bottom) must have its lower (upper) neighbour sounding too.
    for (auto& cp : sounding) {
        auto& m = cp.second;
        for (auto it = m.rbegin(); it != m.rend(); ++it) {
            if (!it->second) continue;
            auto nb = m.find(it->first - 1);
            if (nb == m.end() || nb->second) { tops[cp.first] = it->first; break; }
        }
        for (auto it = m.begin(); it != m.end(); ++it) {
            if (!it->second) continue;
            auto nb = m.find(it->first + 1);
            if (nb == m.end() || nb->second) { lows[cp.first] = it->first; break; }
        }
    }
}

static Result probeOne(const Inst& in, bool quick)
{
    Result res;
    res.r.id = in.id;
    std::string err;
    char line[256];

    // Pass 1: sustain, descending for the top (clean silence above), ascending for the bottom.
    std::vector<TestNote> n1;
    long long t = 500000;
    addScan(n1, t, "sustain_top", 0, 112, 21);
    addScan(n1, t, "sustain_low", 0, 21, 112);
    std::vector<double> rms1;
    bool live = g_liveFallback;
    if (!render(in, n1, {}, rms1, live, err)) {
        res.diag = "render failed: " + err;
        return res;
    }
    std::map<std::string, int> tops, lows;
    std::map<std::string, double> peaks;
    evaluate(n1, rms1, tops, lows, peaks);
    if (!tops.count("sustain_top")) {
        res.diag = "no sound at any pitch (peak rms " + std::to_string(peaks["sustain_top"]) + ")";
        return res;
    }
    int top = tops["sustain_top"];
    int low = lows.count("sustain_low") ? lows["sustain_low"] : -1;
    res.r.top["sustain"] = top;
    res.r.low = low;
    snprintf(line, sizeof(line), "sustain %s-%s", noteName(low).c_str(), noteName(top).c_str());
    res.diag = line;

    // Pass 2: other articulations near the sustain top.
    std::vector<TestNote> n2;
    std::vector<TestText> texts;
    t = 500000;
    // Articulation tops differ from the sustain top by a few semitones at most.
    int hi = std::min(120, top + 13);
    int lo = std::max(low >= 0 ? low : 0, top - 8);
    if (!quick) {
        for (const auto& pc : kProbeClasses) {
            if (pc.bits == 0) continue;
            addScan(n2, t, pc.name, pc.bits, hi, lo);
        }
        for (const auto& ta : in.textArts) {
            texts.push_back(TestText { t - 400000, ta });
            addScan(n2, t, "text:" + lower(ta), 0, hi, lo);
            texts.push_back(TestText { t - 400000, "" });
        }
    }
    // Legato: slurred pair (p-1 -> p); measure both notes.
    for (int p = hi; p >= std::max(lo, 1); --p) {
        TestNote a { p - 1, t, 250000, ms_NoteArticulation_Slur, "legato", t + MEAS_OFF, t + 250000 };
        TestNote b { p, t + 250000, 250000, 0, "legato_target", t + 330000, t + 500000 };
        a.rangeEdge = 1;
        b.rangeEdge = -1;
        b.pitchCheck = true;
        n2.push_back(a);
        n2.push_back(b);
        t += 1000000;
    }
    std::vector<double> rms2;
    if (!render(in, n2, texts, rms2, live, err)) {
        res.diag += "; pass 2 failed: " + err;
        res.ok = true;
        return res;
    }
    std::map<std::string, int> tops2, lows2;
    std::map<std::string, double> peaks2;
    evaluate(n2, rms2, tops2, lows2, peaks2);
    for (auto& p : tops2) {
        // A class whose measured top sits at the scan ceiling is "at least" that high.
        res.r.top[p.first] = p.second;
    }
    res.ok = true;
    return res;
}

// ---------------------------------------------------------------------------

static std::wstring probeExeDir()
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static std::string sectionText(const Inst& in, const InstRange& r)
{
    std::string s = "[" + in.key + "]\n";
    s += "id=" + std::to_string(in.id) + "\n";
    if (!in.category.empty()) s += "category=" + in.category + "\n";
    s += "low=" + std::to_string(r.low) + " ; " + noteName(r.low) + "\n";
    auto put = [&](const std::string& k) {
        auto it = r.top.find(k);
        if (it != r.top.end()) s += k + "=" + std::to_string(it->second) + " ; " + noteName(it->second) + "\n";
    };
    put("sustain");
    for (const auto& pc : kProbeClasses) if (pc.bits) put(pc.name);
    put("legato");
    put("legato_target");
    for (const auto& p : r.top) if (p.first.compare(0, 5, "text:") == 0) put(p.first);
    return s + "\n";
}

struct ProbeOptions {
    std::wstring dir;
    std::string ver;
    std::vector<std::string> only;
    bool force = false, quick = false, listOnly = false;
    int jobs = 1;
};

// Runs after the engine has been initialised. Writes dir\\ranges.ini.
static int runProbe(ProbeOptions o)
{
    const std::wstring& dir = o.dir;
    const char* ver = o.ver.c_str();
    std::vector<std::string>& only = o.only;
    bool force = o.force, quick = o.quick, listOnly = o.listOnly;
    int jobs = o.jobs;
    if (!g_report) g_report = openFile(dir + L"\\probe_report.txt", L"a");
    report(std::string("\n==== probe run, engine ") + ver + " ====\n");
    if (A.ms_disable_reverb) A.ms_disable_reverb();

    std::vector<Inst> insts;
    ms_InstrumentList list = A.ms_get_instrument_list();
    while (ms_InstrumentInfo info = list ? A.ms_InstrumentList_get_next(list) : nullptr) {
        Inst in;
        in.id = A.ms_Instrument_get_id(info);
        in.name = A.ms_Instrument_get_name(info) ? A.ms_Instrument_get_name(info) : "";
        in.pack = A.ms_Instrument_get_pack_name(info) ? A.ms_Instrument_get_pack_name(info) : "";
        in.category = A.ms_Instrument_get_category(info) ? A.ms_Instrument_get_category(info) : "";
        in.vendor = A.ms_Instrument_get_vendor_name(info) ? A.ms_Instrument_get_vendor_name(info) : "";
        in.infoJson = A.ms_Instrument_get_info_json ? (A.ms_Instrument_get_info_json(info) ? A.ms_Instrument_get_info_json(info) : "") : "";
        in.online = A.ms_Instrument_is_online ? A.ms_Instrument_is_online(info) : false;
        const char* dm = A.ms_get_drum_mapping ? A.ms_get_drum_mapping(in.id) : nullptr;
        in.drums = dm && *dm;
        if (A.ms_Instrument_get_preset_list && A.ms_PresetList_get_next) {
            ms_PresetList pl = A.ms_Instrument_get_preset_list(info);
            const char* p0 = pl ? A.ms_PresetList_get_next(pl) : nullptr;
            if (p0) in.preset = p0;
            while (pl && p0) p0 = A.ms_PresetList_get_next(pl); // drain
        }
        const char* ta = A.ms_get_text_articulations ? A.ms_get_text_articulations(in.id, in.preset.c_str()) : nullptr;
        in.textArtsRaw = ta ? ta : "";
        in.textArts = splitArts(in.textArtsRaw);
        in.key = instrumentKey(in.pack.c_str(), in.name.c_str());
        insts.push_back(in);
    }
    std::sort(insts.begin(), insts.end(), [](const Inst& a, const Inst& b) { return a.key < b.key; });

    for (auto& in : insts) {
        report("instrument " + std::to_string(in.id) + " [" + in.key + "] category=" + in.category + " vendor=" + in.vendor +
               (in.online ? " ONLINE" : "") + (in.drums ? " DRUMS" : "") + "\n  preset0=" + in.preset +
               "\n  text_articulations_raw=" + in.textArtsRaw + "\n  info_json=" + in.infoJson + "\n");
    }

    if (listOnly) {
        for (auto& in : insts)
            say("%4d  %-45s %-14s%s%s  text arts: %d\n", in.id, in.key.c_str(), in.category.c_str(),
                   in.online ? " [online]" : "", in.drums ? " [drums]" : "", (int)in.textArts.size());
        return 0;
    }

    // Existing results
    std::wstring outPath = dir + L"\\ranges.ini";
    Ini existing;
    existing.load(outPath);
    std::string oldVer = existing.get("meta", "lib_version");
    if (!oldVer.empty() && oldVer != ver && !force) {
        say("ranges.ini was made with engine %s; re-probing everything for %s.\n", oldVer.c_str(), ver);
        force = true;
    }
    std::map<std::string, std::string> sections; // display key -> text
    if (!force || !only.empty()) {
        // keep existing sections (as text) unless they are re-probed
        for (const auto& s : existing.order) {
            if (s == "meta" || s.empty()) continue;
            std::string txt = "[" + existing.displayName[s] + "]\n";
            for (const auto& kv : existing.sec[s]) {
                int v = std::atoi(kv.second.c_str());
                bool num = !kv.second.empty() && (std::isdigit((unsigned char)kv.second[0]) || kv.second[0] == '-');
                txt += kv.first + "=" + kv.second + ((num && kv.first != "id") ? " ; " + noteName(v) : "") + "\n";
            }
            sections[existing.displayName[s]] = txt + "\n";
        }
    }

    std::vector<Inst*> todo;
    for (auto& in : insts) {
        if (in.online || in.drums) continue;
        if (!only.empty()) {
            bool m = false;
            for (auto& o : only) if (lower(in.key).find(o) != std::string::npos) m = true;
            if (!m) continue;
        } else if (!force && sections.count(in.key)) {
            continue;
        }
        todo.push_back(&in);
    }
    say("%d instrument(s) to probe (%d installed; online and drum-kit instruments are skipped).\n",
           (int)todo.size(), (int)insts.size());

    auto writeAll = [&](bool complete) {
        FILE* f = openFile(outPath + L".tmp", L"wb");
        if (!f) return;
        time_t now = time(nullptr);
        char ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", localtime(&now));
        fprintf(f, "; Muse Sounds playable ranges for MuseRangeShim, written by msr-probe.exe.\n"
                   "; Values are MIDI note numbers (60 = middle C = C4). You can edit them, or put\n"
                   "; overrides in config.ini under the same [Pack|Instrument] heading.\n\n"
                   "[meta]\nlib_version=%s\nsample_rate=%.0f\nmode=%s\nupdated=%s\ncomplete=%d\n\n",
                ver, SR, g_liveFallback ? "live" : "offline", ts, complete ? 1 : 0);
        for (auto& p : sections) fputs(p.second.c_str(), f);
        fclose(f);
        MoveFileExW((outPath + L".tmp").c_str(), outPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    };

    // Sanity check the render path on the first instrument before going wide.
    if (!todo.empty() && !g_liveFallback) {
        Result r = probeOne(*todo[0], true);
        if (!r.ok && r.diag.find("no sound") != std::string::npos) {
            say("Offline render produced silence for %s; trying live mode...\n", todo[0]->key.c_str());
            g_liveFallback = true;
            Result r2 = probeOne(*todo[0], true);
            if (!r2.ok) {
                say("Live mode is silent too; staying with offline rendering.\n");
                g_liveFallback = false;
            }
        }
    }

    std::atomic<size_t> next { 0 };
    std::atomic<int> done { 0 };
    auto start = std::chrono::steady_clock::now();
    auto worker = [&]() {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= todo.size()) return;
            Inst& in = *todo[i];
            Result r = probeOne(in, quick);
            int d = ++done;
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            double eta = el / d * (todo.size() - d);
            {
                std::lock_guard<std::mutex> l(g_out);
                if (r.ok) sections[in.key] = sectionText(in, r.r);
            }
            say("[%3d/%3d] %-45s %s   (%.0fs elapsed, ~%.0f min left)\n", d, (int)todo.size(), in.key.c_str(),
                r.diag.c_str(), el, eta / 60.0);
            report("probe " + in.key + ": " + r.diag + "\n");
            std::lock_guard<std::mutex> l(g_out);
            writeAll(false);
        }
    };
    std::vector<std::thread> threads;
    for (int j = 0; j < jobs; ++j) threads.emplace_back(worker);
    for (auto& th : threads) th.join();
    {
        std::lock_guard<std::mutex> l(g_out);
        writeAll(true);
    }
    say("Done. Wrote %s\n", narrow(outPath).c_str());
    return 0;
}
