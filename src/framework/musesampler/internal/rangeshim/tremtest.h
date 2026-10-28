// Tremolo test bench (config [general] tremolo_test=1): renders a batch of tremolo variants
// through the real Muse Sounds engine, offline, right after MuseScore has started the engine
// (same place as the range calibration), and writes one WAV per case plus tremtest_out\results.csv.
// Purpose: find out, without listening to every attempt, which input makes Muse Sounds play a
// smeared (bowed) tremolo on every note instead of alternating, ratcheting or trilling.
#pragma once

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

namespace tremtest {

static const double TSR = 48000.0;
static const int TBLOCK = 512;

struct TNote {
    long long at, dur;
    int pitch;
    uint64_t art;
    int rangeOpen = 0;  // number of range starts (for `rangeArt`) sent just before this note
    int rangeClose = 0; // number of range ends sent just after this note
};

struct TCase {
    std::string name;
    int inst = -1;
    double dyn = 16 / 127.0;
    std::vector<TNote> notes;
    uint64_t rangeArt = 0;
    long long winA = 0, winB = 0; // analysis window (us)
    int p1 = 0, p2 = 0;           // the pitches the score asks for (p2 = 0: single note)
};

static void writeWav(const std::wstring& path, const std::vector<float>& x)
{
    FILE* f = openFile(path, L"wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    uint32_t n = (uint32_t)x.size();
    fwrite("RIFF", 1, 4, f); u32(36 + n * 2); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1); u32((uint32_t)TSR); u32((uint32_t)TSR * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(n * 2);
    for (float s : x) {
        int v = (int)std::lround(std::max(-1.0f, std::min(1.0f, s * 4.0f)) * 32767.0f); // +12 dB so quiet cases are audible
        int16_t w = (int16_t)v;
        fwrite(&w, 2, 1, f);
    }
    fclose(f);
}

static bool renderCase(const TCase& c, std::vector<float>& mono, std::string& err)
{
    long long endUs = 0;
    for (auto& n : c.notes) endUs = std::max(endUs, n.at + n.dur);
    endUs += 1200000;
    ms_MuseSampler ms = A.ms_MuseSampler_create();
    if (!ms) { err = "create failed"; return false; }
    struct D { ms_MuseSampler m; ~D() { A.ms_MuseSampler_destroy(m); } } d { ms };
    if (A.ms_MuseSampler_init_2(ms, TSR, TBLOCK, 2) != ms_Result_OK) { err = "init failed"; return false; }
    ms_Track tr = A.ms_MuseSampler_add_track(ms, c.inst);
    if (!tr) { err = "add_track failed"; return false; }
    A.ms_MuseSampler_add_track_dynamics_event_2(ms, tr, ms_DynamicsEvent_2 { 0, c.dyn });
    for (auto& n : c.notes) {
        ms_NoteEvent_5 ev {};
        ev._voice = 0;
        ev._location_us = n.at;
        ev._duration_us = n.dur;
        ev._pitch = n.pitch;
        ev._tempo = 90.0;
        ev._offset_cents = 0;
        ev._articulation = (ms_NoteArticulation)n.art;
        ev._articulation_2 = ms_NoteArticulation2_None;
        ev._notehead = ms_NoteHead_Normal;
        for (int k = 0; k < n.rangeOpen; ++k) A.ms_MuseSampler_add_track_event_range_start(ms, tr, 0, (ms_NoteArticulation)c.rangeArt);
        long long id = 0;
        if (A.ms_MuseSampler_add_track_note_event_6(ms, tr, ev, id) != ms_Result_OK) err += "note rejected; ";
        for (int k = 0; k < n.rangeClose; ++k) A.ms_MuseSampler_add_track_event_range_end(ms, tr, 0, (ms_NoteArticulation)c.rangeArt);
    }
    A.ms_MuseSampler_finalize_track(ms, tr);
    auto t0 = std::chrono::steady_clock::now();
    while (!A.ms_MuseSampler_ready_to_play(ms)) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120)) { err += "never ready"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    long long total = (long long)(endUs * TSR / 1e6);
    std::vector<float> L(TBLOCK), Rr(TBLOCK);
    mono.assign(total + TBLOCK, 0.0f);
    float* ch[2] = { L.data(), Rr.data() };
    ms_OutputBuffer buf { ch, TBLOCK, 2 };
    A.ms_MuseSampler_set_position(ms, 0);
    A.ms_MuseSampler_start_offline_mode(ms, TSR);
    A.ms_MuseSampler_set_playing(ms, 1);
    for (long long pos = 0; pos < total; pos += TBLOCK) {
        std::fill(L.begin(), L.end(), 0.0f);
        std::fill(Rr.begin(), Rr.end(), 0.0f);
        if (A.ms_MuseSampler_process_offline(ms, buf) != ms_Result_OK) { err += "process failed"; break; }
        for (int i = 0; i < TBLOCK; ++i) mono[pos + i] = 0.5f * (L[i] + Rr[i]);
    }
    A.ms_MuseSampler_set_playing(ms, 0);
    A.ms_MuseSampler_stop_offline_mode(ms);
    mono.resize(total);
    return true;
}

// Level, pulsing at the 32nd rate (ratchet) and pitch content (trill/alternation) in the window.
static std::string analyse(const TCase& c, const std::vector<float>& x)
{
    long long a = (long long)(c.winA * TSR / 1e6), b = (long long)(c.winB * TSR / 1e6);
    a = std::max(0LL, std::min(a, (long long)x.size()));
    b = std::max(a + 1, std::min(b, (long long)x.size()));
    double acc = 0;
    for (long long k = a; k < b; ++k) acc += (double)x[k] * x[k];
    double rms = std::sqrt(acc / (double)(b - a));
    // envelope: RMS over 4 ms, hop 2 ms; modulation depth at 12 Hz (r32 at q=90) and 6 Hz (alternation pairs)
    const int W = (int)(0.004 * TSR), H = (int)(0.002 * TSR);
    std::vector<double> env;
    for (long long k = a; k + W <= b; k += H) {
        double s = 0;
        for (int i = 0; i < W; ++i) s += (double)x[k + i] * x[k + i];
        env.push_back(std::sqrt(s / W));
    }
    double mean = 0;
    for (double e : env) mean += e;
    mean = env.empty() ? 0 : mean / env.size();
    auto modAt = [&](double hz) {
        double re = 0, im = 0, fs = 1.0 / 0.002;
        for (size_t i = 0; i < env.size(); ++i) {
            double ph = 2 * 3.14159265358979 * hz * i / fs;
            re += (env[i] - mean) * std::cos(ph);
            im += (env[i] - mean) * std::sin(ph);
        }
        return mean > 0 && !env.empty() ? 2 * std::sqrt(re * re + im * im) / env.size() / mean : 0.0;
    };
    auto g = [&](int pitch) {
        double f = 440.0 * std::pow(2.0, (pitch - 69) / 12.0), best = 0;
        for (int hm = 1; hm <= 3; ++hm) {
            double w = 2 * 3.14159265358979 * f * hm / TSR, cc = 2 * std::cos(w), s1 = 0, s2 = 0;
            for (long long k = a; k < b; ++k) { double s0 = x[k] + cc * s1 - s2; s2 = s1; s1 = s0; }
            best += s1 * s1 + s2 * s2 - cc * s1 * s2;
        }
        return best;
    };
    auto db = [](double v) { return v > 1e-12 ? 10 * std::log10(v) : -120.0; };
    double e1 = g(c.p1), e1u = g(c.p1 + 1), e1d = g(c.p1 - 1), e1w = g(c.p1 + 2);
    char out[512];
    snprintf(out, sizeof(out), "%.1f,%.3f,%.3f,%.1f,%.1f,%.1f,%.1f", rms > 1e-9 ? 20 * std::log10(rms) : -180.0, modAt(12.0), modAt(6.0),
             db(e1u) - db(e1), db(e1d) - db(e1), db(e1w) - db(e1), c.p2 ? db(g(c.p2)) - db(e1) : 0.0);
    return out;
}

static int findInstrument(const char* key)
{
    ms_InstrumentList l = A.ms_get_instrument_list();
    while (ms_InstrumentInfo i = A.ms_InstrumentList_get_next(l)) {
        if (lower(instrumentKey(A.ms_Instrument_get_pack_name(i), A.ms_Instrument_get_name(i))) == key) return A.ms_Instrument_get_id(i);
    }
    return -1;
}

// The cases. Step = a 32nd at q=90 (83.3 ms), tremolo = 16 steps (a half note), starting at 1.0 s.
static std::vector<TCase> buildCases()
{
    const uint64_t T3 = (uint64_t)ms_NoteArticulation_Tremolo3, T2 = (uint64_t)ms_NoteArticulation_Tremolo2,
                   BZ = (uint64_t)ms_NoteArticulation_BuzzTremolo, SP = (uint64_t)ms_NoteArticulation_SulPonticello;
    const long long STEP = 83333, START = 1000000;
    const int STEPS = 16;
    struct InstDef { const char* key; const char* tag; int lo, hi; };
    const InstDef insts[] = { { "muse strings|violins 1", "vn1", 57, 66 }, { "muse strings|contrabasses", "cb", 35, 42 } };
    const struct { const char* tag; double v; } dyns[] = { { "pp", 8 / 127.0 }, { "p", 16 / 127.0 }, { "mf", 64 / 127.0 } };
    std::vector<TCase> cases;
    for (auto& in : insts) {
        int id = findInstrument(in.key);
        if (id < 0 && envPath(L"MSR_TREMTEST_ANY") == L"1") id = 1; // mock engine in the test harness
        if (id < 0) continue;
        for (int tech = 0; tech < 2; ++tech) {
            for (auto& dy : dyns) {
                bool full = std::string(in.tag) == "vn1";
                if (!full && (tech == 1 || std::string(dy.tag) == "pp")) continue;
                const uint64_t base = tech ? SP : 0;
                auto mk = [&](const std::string& pat) {
                    TCase c;
                    c.name = std::string(in.tag) + "_" + (tech ? "sulpont" : "ord") + "_" + dy.tag + "_" + pat;
                    c.inst = id;
                    c.dyn = dy.v;
                    c.rangeArt = T3;
                    c.p1 = in.lo;
                    c.winA = START + 150000;
                    c.winB = START + STEPS * STEP - 50000;
                    return c;
                };
                auto rep = [&](TCase& c, int pitch, uint64_t trem, bool ranges, int nested) {
                    c.rangeArt = trem;
                    for (int k = 0; k < STEPS; ++k) {
                        TNote n { START + k * STEP, STEP, pitch, base | trem };
                        if (ranges && k == 0) n.rangeOpen = nested;
                        if (ranges && k == STEPS - 1) n.rangeClose = nested;
                        c.notes.push_back(n);
                    }
                };
                // R1: one pitch as MuseScore sends a one-note tremolo (16 repetitions, range around them) = a lane
                { TCase c = mk("R1_rep_range"); rep(c, in.lo, T3, true, 1); cases.push_back(c); }
                // R1n: same without the range markers
                { TCase c = mk("R1n_rep_norange"); rep(c, in.lo, T3, false, 0); cases.push_back(c); }
                // P1: R1 preceded by a plain note on the same track (the context the normal instrument has)
                { TCase c = mk("P1_rep_range_after_note"); c.notes.push_back(TNote { 200000, START - 200000, in.lo + 2, base });
                  rep(c, in.lo, T3, true, 1); cases.push_back(c); }
                // H1: one held note with the tremolo mark (4d2e7a2) - heard as a half-step trill
                { TCase c = mk("H1_held_range"); TNote n { START, STEPS * STEP, in.lo, base | T3, 1, 1 }; c.notes.push_back(n); cases.push_back(c); }
                { TCase c = mk("H1n_held_norange"); TNote n { START, STEPS * STEP, in.lo, base | T3 }; c.notes.push_back(n); cases.push_back(c); }
                // HB: one held note with the buzz (unmeasured) tremolo mark
                { TCase c = mk("HB_held_buzz_range"); c.rangeArt = BZ; TNote n { START, STEPS * STEP, in.lo, base | BZ, 1, 1 }; c.notes.push_back(n); cases.push_back(c); }
                { TCase c = mk("HBn_held_buzz_norange"); TNote n { START, STEPS * STEP, in.lo, base | BZ }; c.notes.push_back(n); cases.push_back(c); }
                // RB: repetitions with the buzz mark
                { TCase c = mk("RB_rep_buzz_range"); rep(c, in.lo, BZ, true, 1); cases.push_back(c); }
                // R2: repetitions with the 16th tremolo mark
                { TCase c = mk("R2_rep_t16_range"); rep(c, in.lo, T2, true, 1); cases.push_back(c); }
                // C2: the chord as MuseScore sends it (both notes every step, a range per note) - stock alternation
                { TCase c = mk("C2_chord_stock"); c.p2 = in.hi; c.rangeArt = T3;
                  for (int k = 0; k < STEPS; ++k)
                      for (int pi = 0; pi < 2; ++pi) {
                          TNote n { START + k * STEP, STEP, pi ? in.hi : in.lo, base | T3 };
                          if (k == 0) n.rangeOpen = 1;
                          if (k == STEPS - 1) n.rangeClose = 1;
                          c.notes.push_back(n);
                      }
                  cases.push_back(c); }
            }
        }
    }
    return cases;
}

static void run(const std::wstring& dir, void (*say)(const char*))
{
    std::wstring out = dir + L"\\tremtest_out";
    CreateDirectoryW(out.c_str(), nullptr);
    FILE* csv = openFile(out + L"\\results.csv", L"w");
    if (!csv) return;
    fprintf(csv, "case,level_db,pulse12,pulse6,semitone_up_db,semitone_down_db,whole_up_db,second_note_db,error\n");
    fflush(csv);
    std::vector<TCase> cases = buildCases();
    char line[256];
    snprintf(line, sizeof(line), "tremolo test: %d cases\r\n", (int)cases.size());
    say(line);
    int i = 0;
    for (auto& c : cases) {
        std::vector<float> x;
        std::string err;
        bool ok = renderCase(c, x, err);
        std::string m = ok ? analyse(c, x) : ",,,,,,";
        fprintf(csv, "%s,%s,%s\n", c.name.c_str(), m.c_str(), err.c_str());
        fflush(csv);
        if (ok) writeWav(out + L"\\" + widen(c.name) + L".wav", x);
        snprintf(line, sizeof(line), "  %d/%d %s %s\r\n", ++i, (int)cases.size(), c.name.c_str(), err.c_str());
        say(line);
    }
    fclose(csv);
    if (FILE* f = openFile(out + L"\\complete.txt", L"w")) { fputs("done\n", f); fclose(f); }
    say("TREMOLO TEST DONE\r\n");
}

} // namespace tremtest
