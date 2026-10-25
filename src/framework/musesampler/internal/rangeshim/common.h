// Shared helpers for MuseRangeShim and msr-probe: INI parsing, articulation
// classes, and the per-instrument range table.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "msapi.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace msr {

inline std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

inline std::string lower(std::string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

#ifdef _WIN32
inline std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline FILE* openFile(const std::wstring& path, const wchar_t* mode)
{
    return _wfopen(path.c_str(), mode);
}

inline std::wstring envPath(const wchar_t* var)
{
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(var, buf, 32768);
    return (n > 0 && n < 32768) ? std::wstring(buf, n) : std::wstring();
}

inline std::wstring defaultRealDllPath()
{
    return envPath(L"LOCALAPPDATA") + L"\\MuseSampler\\lib\\MuseSamplerCoreLib.dll";
}

// The engine finds its instruments through MuseSampler\.config (next to its lib
// folder) or the MUSESAMPLER_INSTRUMENT_FOLDER environment variable, and refuses
// to initialise if it can't. Outside its usual host it may not find the .config,
// so pass the folder explicitly. Returns the folder used ("" if none).
inline std::string ensureInstrumentFolderEnv(const std::wstring& realDll)
{
    std::wstring cur = envPath(L"MUSESAMPLER_INSTRUMENT_FOLDER");
    if (!cur.empty()) return narrow(cur);
    std::vector<std::wstring> candidates;
    size_t s = realDll.find_last_of(L"\\/");
    if (s != std::wstring::npos) {
        std::wstring libDir = realDll.substr(0, s);
        size_t s2 = libDir.find_last_of(L"\\/");
        if (s2 != std::wstring::npos) candidates.push_back(libDir.substr(0, s2) + L"\\.config");
    }
    candidates.push_back(envPath(L"LOCALAPPDATA") + L"\\MuseSampler\\.config");
    for (const auto& c : candidates) {
        FILE* f = openFile(c, L"rb");
        if (!f) continue;
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        std::string folder = trim(std::string(buf));
        size_t nl = folder.find_first_of("\r\n");
        if (nl != std::string::npos) folder = trim(folder.substr(0, nl));
        if (folder.empty()) continue;
        std::wstring w = widen(folder);
        SetEnvironmentVariableW(L"MUSESAMPLER_INSTRUMENT_FOLDER", w.c_str());
        // The engine reads the variable through the Universal CRT, which keeps its own copy.
        if (HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll")) {
            typedef int(__cdecl * wputenv_s_t)(const wchar_t*, const wchar_t*);
            if (auto fn = (wputenv_s_t)(void*)GetProcAddress(ucrt, "_wputenv_s")) fn(L"MUSESAMPLER_INSTRUMENT_FOLDER", w.c_str());
        }
        return folder;
    }
    return std::string();
}
#endif

// ---------------------------------------------------------------------------
// Minimal INI: [section] / key=value ; comments start with ; or #.
// Section and key names are matched case-insensitively. Values keep case.
struct Ini {
    std::vector<std::string> order; // section order as written (lower-case keys)
    std::map<std::string, std::string> displayName; // lower -> original
    std::map<std::string, std::map<std::string, std::string> > sec;

    bool parse(const std::string& text)
    {
        std::string cur;
        size_t i = 0;
        while (i <= text.size()) {
            size_t j = text.find('\n', i);
            if (j == std::string::npos) j = text.size();
            std::string line = trim(text.substr(i, j - i));
            i = j + 1;
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '[') {
                size_t e = line.rfind(']');
                std::string name = trim(line.substr(1, e == std::string::npos ? std::string::npos : e - 1));
                cur = lower(name);
                if (!sec.count(cur)) {
                    order.push_back(cur);
                    displayName[cur] = name;
                    sec[cur];
                }
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = lower(trim(line.substr(0, eq)));
            std::string v = trim(line.substr(eq + 1));
            // strip trailing " ; comment"
            size_t sc = v.find(" ;");
            if (sc != std::string::npos) v = trim(v.substr(0, sc));
            sec[cur][k] = v;
        }
        return true;
    }

#ifdef _WIN32
    bool load(const std::wstring& path)
    {
        FILE* f = openFile(path, L"rb");
        if (!f) return false;
        std::string text;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text = text.substr(3); // UTF-8 BOM
        return parse(text);
    }
#endif

    const std::map<std::string, std::string>* section(const std::string& s) const
    {
        auto it = sec.find(lower(s));
        return it == sec.end() ? nullptr : &it->second;
    }

    std::string get(const std::string& s, const std::string& k, const std::string& def = "") const
    {
        auto* m = section(s);
        if (!m) return def;
        auto it = m->find(lower(k));
        return it == m->end() || it->second.empty() ? def : it->second;
    }

    long long geti(const std::string& s, const std::string& k, long long def) const
    {
        std::string v = get(s, k);
        if (v.empty()) return def;
        return std::strtoll(v.c_str(), nullptr, 10);
    }

    double getd(const std::string& s, const std::string& k, double def) const
    {
        std::string v = get(s, k);
        if (v.empty()) return def;
        return std::strtod(v.c_str(), nullptr);
    }
};

// ---------------------------------------------------------------------------
// Articulation classes. The probe measures the highest sounding pitch for each
// class; the shim maps a note's articulation bits to classes and uses the
// most restrictive (lowest) top among them.

struct ProbeClass {
    const char* name;
    uint64_t bits;
};

static const ProbeClass kProbeClasses[] = {
    { "sustain", 0 },
    { "staccato", ms_NoteArticulation_Staccato },
    { "staccatissimo", ms_NoteArticulation_Staccatissimo },
    { "tenuto", ms_NoteArticulation_Tenuto },
    { "accent", ms_NoteArticulation_Accent },
    { "marcato", ms_NoteArticulation_Marcato },
    { "harmonics", ms_NoteArticulation_Harmonics },
    { "mute", ms_NoteArticulation_Mute },
    { "trill", ms_NoteArticulation_Trill },
    { "tremolo", ms_NoteArticulation_Tremolo3 },
    { "pizzicato", ms_NoteArticulation_Pizzicato },
    { "snap_pizzicato", ms_NoteArticulation_SnapPizzicato },
    { "col_legno", ms_NoteArticulation_ColLegno },
    { "sul_tasto", ms_NoteArticulation_SulTasto },
    { "sul_ponticello", ms_NoteArticulation_SulPonticello },
};

struct BitClass {
    uint64_t bits;
    const char* name;
};

// Note bit -> class used for the range lookup.
static const BitClass kBitClasses[] = {
    { ms_NoteArticulation_Staccato, "staccato" },
    { ms_NoteArticulation_Staccatissimo, "staccatissimo" },
    { ms_NoteArticulation_Tenuto, "tenuto" },
    { ms_NoteArticulation_Accent, "accent" },
    { ms_NoteArticulation_Marcato, "marcato" },
    { ms_NoteArticulation_Harmonics, "harmonics" },
    { ms_NoteArticulation_Mute, "mute" },
    { ms_NoteArticulation_Trill, "trill" },
    // Tremolo1/2 are rendered by Muse Sounds as subdivided staccato notes.
    { (uint64_t)ms_NoteArticulation_Tremolo1 | (uint64_t)ms_NoteArticulation_Tremolo2, "staccato" },
    { (uint64_t)ms_NoteArticulation_Tremolo3 | (uint64_t)ms_NoteArticulation_BuzzTremolo, "tremolo" },
    { ms_NoteArticulation_Pizzicato, "pizzicato" },
    { ms_NoteArticulation_SnapPizzicato, "snap_pizzicato" },
    { ms_NoteArticulation_ColLegno, "col_legno" },
    { ms_NoteArticulation_SulTasto, "sul_tasto" },
    { ms_NoteArticulation_SulPonticello, "sul_ponticello" },
};

enum LegatoCtx { Legato_None = 0, Legato_Slurred = 1, Legato_Target = 2 };

struct InstRange {
    int id = -1;
    int low = -1; // lowest sounding pitch (sustain), -1 = unknown
    std::map<std::string, int> top; // class -> highest sounding MIDI pitch
    bool off = false;

    int classTop(const std::string& c) const
    {
        auto it = top.find(c);
        return it == top.end() ? -1 : it->second;
    }

    // Highest sounding pitch for a note with these properties, or -1.
    int noteTop(uint64_t art1, const std::string& textArt, int legatoCtx) const
    {
        int sus = classTop("sustain");
        int best = -1;
        auto consider = [&](int t) {
            if (t >= 0 && (best < 0 || t < best)) best = t;
        };
        for (const auto& bc : kBitClasses) {
            if (art1 & bc.bits) consider(classTop(bc.name));
        }
        if (legatoCtx == Legato_Slurred) consider(classTop("legato"));
        if (legatoCtx == Legato_Target) consider(classTop("legato_target"));
        if (!textArt.empty()) consider(classTop("text:" + lower(textArt)));
        if (best < 0) best = sus;
        return best;
    }
};

inline void applyRangeSection(InstRange& r, const std::map<std::string, std::string>& kv)
{
    for (const auto& p : kv) {
        if (p.second.empty()) continue;
        if (p.first == "id") r.id = std::atoi(p.second.c_str());
        else if (p.first == "low") r.low = std::atoi(p.second.c_str());
        else if (p.first == "off") r.off = std::atoi(p.second.c_str()) != 0;
        else if (p.first == "category" || p.first == "name" || p.first == "pack") continue;
        else if (std::isdigit((unsigned char)p.second[0]) || p.second[0] == '-') r.top[p.first] = std::atoi(p.second.c_str());
    }
}

inline std::string instrumentKey(const char* pack, const char* name)
{
    return std::string(pack ? pack : "") + "|" + std::string(name ? name : "");
}

inline std::string noteName(int midi)
{
    static const char* N[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (midi < 0) return "?";
    return std::string(N[midi % 12]) + std::to_string(midi / 12 - 1);
}

} // namespace msr
