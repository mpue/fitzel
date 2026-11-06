#include "fitzel/audio/Song.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "fitzel/asset/Vfs.hpp"

namespace fitzel::song {

using json = nlohmann::json;

const char* kindName(TrackKind k) {
    switch (k) {
    case TrackKind::Audio:      return "audio";
    case TrackKind::Instrument: return "instrument";
    case TrackKind::Midi:       return "midi";
    }
    return "instrument";
}

const char* instrumentKindName(InstrumentKind k) {
    switch (k) {
    case InstrumentKind::None:  return "none";
    case InstrumentKind::Synth: return "synth";
    case InstrumentKind::Drums: return "drums";
    case InstrumentKind::Vst3:  return "vst3";
    }
    return "none";
}

namespace {

TrackKind trackKindFrom(const std::string& s) {
    if (s == "audio") return TrackKind::Audio;
    if (s == "midi") return TrackKind::Midi;
    return TrackKind::Instrument;
}

InstrumentKind instrumentKindFrom(const std::string& s) {
    if (s == "synth") return InstrumentKind::Synth;
    if (s == "drums") return InstrumentKind::Drums;
    if (s == "vst3") return InstrumentKind::Vst3;
    return InstrumentKind::None;
}

// A plugin's state is bytes; in the JSON it is base64.
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string toBase64(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];  out += kB64[v & 63];
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string fromBase64(const std::string& in) {
    int map[256];
    std::fill(std::begin(map), std::end(map), -1);
    for (int i = 0; i < 64; ++i) map[static_cast<unsigned char>(kB64[i])] = i;
    std::string out;
    unsigned v = 0;
    int bits = 0;
    for (const char ch : in) {
        const int d = map[static_cast<unsigned char>(ch)];
        if (d < 0) continue;   // '=', line breaks
        v = (v << 6) | static_cast<unsigned>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((v >> bits) & 0xFF);
        }
    }
    return out;
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

template <class T>
T get(const json& j, const char* key, T fallback) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (const std::exception&) {
        return fallback;
    }
}

} // namespace

void Clip::sortNotes() {
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.pitch < b.pitch;
    });
}

void Track::sortClips() {
    std::stable_sort(clips.begin(), clips.end(),
                     [](const Clip& a, const Clip& b) { return a.start < b.start; });
}

void playedNotes(const Clip& c, std::vector<Note>& out) {
    if (c.length <= 0 || c.notes.empty()) return;
    const std::int64_t s = c.start, e = c.end();
    if (c.loop <= 0) {
        for (const Note& n : c.notes) {
            const std::int64_t t = s + n.tick - c.offset;
            if (t < s) continue;
            if (t >= e) break;
            Note m = n;
            m.tick   = t;
            m.length = std::max<std::int64_t>(1, std::min(n.length, e - t));
            out.push_back(m);
        }
        return;
    }
    const std::int64_t L = std::max<std::int64_t>(c.loop, 1);
    // A loop of a tick over a thousand bars is a mistake, not a song: refuse to
    // unroll a million copies of it.
    if ((e - s) / L > 20000) return;
    for (std::int64_t k = floorDiv(c.offset, L);; ++k) {
        const std::int64_t base = s + k * L - c.offset;   // song tick of content 0, this time round
        if (base >= e) break;
        for (const Note& n : c.notes) {
            if (n.tick >= L) break;   // past the loop: shown, not played
            const std::int64_t t = base + n.tick;
            if (t < s) continue;
            if (t >= e) break;
            Note m = n;
            m.tick   = t;
            m.length = std::max<std::int64_t>(1, std::min({n.length, L - n.tick, e - t}));
            out.push_back(m);
        }
    }
}

std::string Instrument::label() const {
    switch (kind) {
    case InstrumentKind::None:  return "No instrument";
    case InstrumentKind::Drums: return "Drum kit";
    case InstrumentKind::Synth:
        return patch.rfind("builtin:", 0) == 0 ? patch.substr(8) : patch;
    case InstrumentKind::Vst3:
        return pluginName.empty() ? std::string("VST3 plugin") : pluginName;
    }
    return {};
}

// --- Song ------------------------------------------------------------------------

Track& Song::addTrack(TrackKind kind, const std::string& name) {
    Track t;
    t.uid  = newUid();
    t.kind = kind;
    if (kind == TrackKind::Audio) t.instrument.kind = InstrumentKind::None;
    if (kind == TrackKind::Midi) {
        t.instrument.kind = InstrumentKind::None;
        for (const Track& o : tracks)
            if (o.kind == TrackKind::Instrument) { t.target = o.uid; break; }
    }
    if (name.empty()) {
        int n = 1;
        for (const Track& o : tracks)
            if (o.kind == kind) ++n;
        const char* base = kind == TrackKind::Audio ? "Audio" : kind == TrackKind::Midi ? "MIDI" : "Instrument";
        t.name = std::string(base) + " " + std::to_string(n);
    } else {
        t.name = name;
    }
    tracks.push_back(std::move(t));
    return tracks.back();
}

Track* Song::track(int uid) {
    for (Track& t : tracks)
        if (t.uid == uid) return &t;
    return nullptr;
}
const Track* Song::track(int uid) const {
    for (const Track& t : tracks)
        if (t.uid == uid) return &t;
    return nullptr;
}

Clip* Song::clip(int uid, Track** owner) {
    for (Track& t : tracks)
        for (Clip& c : t.clips)
            if (c.uid == uid) {
                if (owner) *owner = &t;
                return &c;
            }
    return nullptr;
}
const Clip* Song::clip(int uid, const Track** owner) const {
    for (const Track& t : tracks)
        for (const Clip& c : t.clips)
            if (c.uid == uid) {
                if (owner) *owner = &t;
                return &c;
            }
    return nullptr;
}

void Song::fixUids() {
    std::set<int> seen;
    int top = 0;
    for (const Track& t : tracks) {
        top = std::max(top, t.uid);
        for (const Clip& c : t.clips) top = std::max(top, c.uid);
    }
    nextUid = std::max(nextUid, top + 1);
    for (Track& t : tracks) {
        if (t.uid <= 0 || !seen.insert(t.uid).second) t.uid = newUid(), seen.insert(t.uid);
        for (Clip& c : t.clips)
            if (c.uid <= 0 || !seen.insert(c.uid).second) c.uid = newUid(), seen.insert(c.uid);
    }
}

std::int64_t Song::tickAt(double sec) const {
    return static_cast<std::int64_t>(std::llround(sec / secondsPerTick()));
}

std::int64_t Song::contentEnd() const {
    std::int64_t e = 0;
    for (const Track& t : tracks)
        for (const Clip& c : t.clips) e = std::max(e, c.end());
    return e;
}

std::string Song::toJson() const {
    json j;
    j["format"]       = "fitzel-song";
    j["version"]      = 1;
    j["name"]         = name;
    j["bpm"]          = bpm;
    j["beatsPerBar"]  = beatsPerBar;
    j["beatUnit"]     = beatUnit;
    j["loop"]         = loop;
    j["loopStart"]    = loopStart;
    j["loopEnd"]      = loopEnd;
    j["endTick"]      = endTick;
    j["masterVolume"] = masterVolume;
    j["nextUid"]      = nextUid;
    json tj = json::array();
    for (const Track& t : tracks) {
        json o;
        o["uid"]       = t.uid;
        o["kind"]      = kindName(t.kind);
        o["name"]      = t.name;
        o["color"]     = t.color;
        o["volume"]    = t.volume;
        o["pan"]       = t.pan;
        o["mute"]      = t.mute;
        o["solo"]      = t.solo;
        o["transpose"] = t.transpose;
        o["height"]    = t.height;
        if (t.kind == TrackKind::Instrument) {
            json in;
            in["kind"]   = instrumentKindName(t.instrument.kind);
            in["patch"]  = t.instrument.patch;
            in["voices"] = t.instrument.voices;
            if (t.instrument.kind == InstrumentKind::Vst3) {
                in["pluginUid"]  = t.instrument.pluginUid;
                in["pluginName"] = t.instrument.pluginName;
                in["state"]      = toBase64(t.instrument.state);
            }
            o["instrument"] = in;
        }
        if (t.kind == TrackKind::Midi) {
            o["target"]  = t.target;
            o["channel"] = t.channel;
        }
        json cj = json::array();
        for (const Clip& c : t.clips) {
            json k;
            k["uid"]    = c.uid;
            k["start"]  = c.start;
            k["length"] = c.length;
            k["name"]   = c.name;
            k["color"]  = c.color;
            k["mute"]   = c.mute;
            if (t.kind == TrackKind::Audio) {
                k["file"]         = c.file;
                k["sourceOffset"] = c.sourceOffset;
                k["gain"]         = c.gain;
                k["fadeIn"]       = c.fadeIn;
                k["fadeOut"]      = c.fadeOut;
            } else {
                k["offset"] = c.offset;
                k["loop"]   = c.loop;
                // [tick, length, pitch, velocity]: a song has thousands of them.
                json nj = json::array();
                for (const Note& n : c.notes) nj.push_back({n.tick, n.length, n.pitch, n.velocity});
                k["notes"] = std::move(nj);
            }
            cj.push_back(std::move(k));
        }
        o["clips"] = std::move(cj);
        tj.push_back(std::move(o));
    }
    j["tracks"] = std::move(tj);
    return j.dump(1);
}

bool Song::fromJson(const std::string& text, Song& out, std::string* error) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        if (error) *error = std::string("not JSON: ") + e.what();
        return false;
    }
    if (!j.is_object() || get<std::string>(j, "format", "") != "fitzel-song") {
        if (error) *error = "not a fitzel song";
        return false;
    }
    Song s;
    s.name         = get<std::string>(j, "name", "Song");
    s.bpm          = std::clamp(get<double>(j, "bpm", 120.0), 10.0, 999.0);
    s.beatsPerBar  = std::clamp(get<int>(j, "beatsPerBar", 4), 1, 32);
    s.beatUnit     = get<int>(j, "beatUnit", 4);
    if (s.beatUnit != 2 && s.beatUnit != 4 && s.beatUnit != 8 && s.beatUnit != 16) s.beatUnit = 4;
    s.loop         = get<bool>(j, "loop", false);
    s.loopStart    = std::max<std::int64_t>(0, get<std::int64_t>(j, "loopStart", 0));
    s.loopEnd      = std::max(s.loopStart + 1, get<std::int64_t>(j, "loopEnd", kPpq * 16));
    s.endTick      = std::max<std::int64_t>(0, get<std::int64_t>(j, "endTick", 0));
    s.masterVolume = std::clamp(get<float>(j, "masterVolume", 1.0f), 0.0f, 2.0f);
    s.nextUid      = std::max(1, get<int>(j, "nextUid", 1));
    if (auto tj = j.find("tracks"); tj != j.end() && tj->is_array()) {
        for (const json& o : *tj) {
            if (!o.is_object()) continue;
            Track t;
            t.uid       = get<int>(o, "uid", 0);
            t.kind      = trackKindFrom(get<std::string>(o, "kind", "instrument"));
            t.name      = get<std::string>(o, "name", "Track");
            t.color     = get<std::uint32_t>(o, "color", 0u);
            t.volume    = std::clamp(get<float>(o, "volume", 0.8f), 0.0f, 2.0f);
            t.pan       = std::clamp(get<float>(o, "pan", 0.0f), -1.0f, 1.0f);
            t.mute      = get<bool>(o, "mute", false);
            t.solo      = get<bool>(o, "solo", false);
            t.transpose = std::clamp(get<int>(o, "transpose", 0), -48, 48);
            t.height    = std::clamp(get<float>(o, "height", 64.0f), 24.0f, 400.0f);
            t.target    = get<int>(o, "target", 0);
            t.channel   = std::clamp(get<int>(o, "channel", 0), 0, 15);
            t.instrument.kind = t.kind == TrackKind::Instrument ? InstrumentKind::Synth : InstrumentKind::None;
            if (auto in = o.find("instrument"); in != o.end() && in->is_object()) {
                t.instrument.kind       = instrumentKindFrom(get<std::string>(*in, "kind", "synth"));
                t.instrument.patch      = get<std::string>(*in, "patch", "builtin:Saw Lead");
                t.instrument.voices     = std::clamp(get<int>(*in, "voices", 12), 1, 32);
                t.instrument.pluginUid  = get<std::string>(*in, "pluginUid", "");
                t.instrument.pluginName = get<std::string>(*in, "pluginName", "");
                t.instrument.state      = fromBase64(get<std::string>(*in, "state", ""));
            }
            if (auto cj = o.find("clips"); cj != o.end() && cj->is_array()) {
                for (const json& k : *cj) {
                    if (!k.is_object()) continue;
                    Clip c;
                    c.uid          = get<int>(k, "uid", 0);
                    c.start        = std::max<std::int64_t>(0, get<std::int64_t>(k, "start", 0));
                    c.length       = std::max<std::int64_t>(1, get<std::int64_t>(k, "length", kPpq * 4));
                    c.name         = get<std::string>(k, "name", "");
                    c.color        = get<std::uint32_t>(k, "color", 0u);
                    c.mute         = get<bool>(k, "mute", false);
                    c.offset       = get<std::int64_t>(k, "offset", 0);
                    c.loop         = std::max<std::int64_t>(0, get<std::int64_t>(k, "loop", 0));
                    c.file         = get<std::string>(k, "file", "");
                    c.sourceOffset = std::max(0.0, get<double>(k, "sourceOffset", 0.0));
                    c.gain         = std::clamp(get<float>(k, "gain", 1.0f), 0.0f, 4.0f);
                    c.fadeIn       = std::max(0.0f, get<float>(k, "fadeIn", 0.0f));
                    c.fadeOut      = std::max(0.0f, get<float>(k, "fadeOut", 0.0f));
                    if (auto nj = k.find("notes"); nj != k.end() && nj->is_array()) {
                        c.notes.reserve(nj->size());
                        for (const json& n : *nj) {
                            if (!n.is_array() || n.size() < 4) continue;
                            Note note;
                            note.tick     = n[0].get<std::int64_t>();
                            note.length   = std::max<std::int64_t>(1, n[1].get<std::int64_t>());
                            note.pitch    = std::clamp(n[2].get<int>(), 0, 127);
                            note.velocity = std::clamp(n[3].get<int>(), 1, 127);
                            c.notes.push_back(note);
                        }
                    }
                    c.sortNotes();
                    t.clips.push_back(std::move(c));
                }
            }
            t.sortClips();
            s.tracks.push_back(std::move(t));
        }
    }
    s.fixUids();
    out = std::move(s);
    return true;
}

bool Song::load(const std::string& path, Song& out, std::string* error) {
    if (!vfs::exists(path)) {
        if (error) *error = "no such file: " + path;
        return false;
    }
    return fromJson(vfs::readText(path), out, error);
}

bool Song::save(const std::string& path, std::string* error) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    f << toJson();
    if (!f) {
        if (error) *error = "writing " + path + " failed";
        return false;
    }
    return true;
}

std::string barBeatTick(const Song& s, std::int64_t tick) {
    const std::int64_t bar  = s.ticksPerBar();
    const std::int64_t beat = s.ticksPerBeat();
    const bool neg = tick < 0;
    if (neg) tick = -tick;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s%lld.%lld.%03lld", neg ? "-" : "",
                  static_cast<long long>(tick / bar + 1),
                  static_cast<long long>((tick % bar) / beat + 1),
                  static_cast<long long>(tick % beat));
    return buf;
}

std::string clockTime(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const long long ms = std::llround(seconds * 1000.0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return buf;
}

std::string noteName(int pitch) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    pitch = std::clamp(pitch, 0, 127);
    return std::string(names[pitch % 12]) + std::to_string(pitch / 12 - 1);
}

// --- Standard MIDI Files ------------------------------------------------------------

namespace {

// A cursor that refuses to read past the end (as Midi.cpp's does).
struct Reader {
    const std::uint8_t* p   = nullptr;
    const std::uint8_t* end = nullptr;
    bool                ok  = true;

    bool has(std::size_t n) const { return ok && static_cast<std::size_t>(end - p) >= n; }
    std::uint8_t u8() {
        if (!has(1)) { ok = false; return 0; }
        return *p++;
    }
    std::uint32_t u16() { const std::uint32_t a = u8(); return (a << 8) | u8(); }
    std::uint32_t u32() { const std::uint32_t a = u16(); return (a << 16) | u16(); }
    std::uint32_t vlq() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t b = u8();
            v = (v << 7) | (b & 0x7Fu);
            if (!(b & 0x80u)) return v;
        }
        ok = false;
        return 0;
    }
    void skip(std::size_t n) {
        if (!has(n)) { ok = false; p = end; return; }
        p += n;
    }
};

// The factory patch a General MIDI program sounds closest to.
std::string patchForProgram(int program) {
    if (program < 0) return "builtin:Keys";
    switch (program / 8) {
    case 0:  return "builtin:Keys";         // pianos
    case 1:  return "builtin:Bell";         // chromatic percussion
    case 2:  return "builtin:Organ";
    case 3:  return "builtin:Pluck";        // guitars
    case 4:  return "builtin:Bass";
    case 5:
    case 6:  return "builtin:Strings";      // strings, ensembles
    case 7:  return "builtin:Saw Lead";     // brass
    case 8:
    case 9:  return "builtin:Square Lead";  // reeds, pipes
    case 10: return "builtin:Saw Lead";     // synth leads
    case 11:
    case 12: return "builtin:Soft Pad";     // pads, synth effects
    case 13: return "builtin:Pluck";        // ethnic
    case 14: return "builtin:Bell";         // percussive
    default: return "builtin:Pluck";
    }
}

} // namespace

bool MidiFile::parse(const std::vector<std::uint8_t>& bytes, MidiFile& out, std::string* error) {
    out = MidiFile{};
    auto fail = [&](const char* why) {
        if (error) *error = why;
        out = MidiFile{};
        return false;
    };
    Reader r{bytes.data(), bytes.data() + bytes.size()};
    if (!r.has(14) || std::string(reinterpret_cast<const char*>(r.p), 4) != "MThd") return fail("not a MIDI file");
    r.skip(4);
    const std::uint32_t hlen = r.u32();
    const int format   = static_cast<int>(r.u16());
    const int ntracks  = static_cast<int>(r.u16());
    const std::uint32_t division = r.u16();
    if (hlen > 6) r.skip(hlen - 6);
    if (!r.ok || format > 2) return fail("unsupported MIDI header");

    // SMPTE time is seconds, not beats: it is turned into ticks at the tempo
    // the file states (or 120), which is the best a grid can do with it.
    const bool   smpte = (division & 0x8000u) != 0;
    double       ticksPerSecondSmpte = 0.0;
    double       scale = 1.0;   // file ticks -> kPpq ticks
    if (smpte) {
        const int fps = -static_cast<int>(static_cast<std::int8_t>(division >> 8));
        const int tpf = static_cast<int>(division & 0xFF);
        ticksPerSecondSmpte = static_cast<double>(std::max(1, fps) * std::max(1, tpf));
    } else {
        if (division == 0) return fail("MIDI file with a division of 0");
        scale = static_cast<double>(kPpq) / static_cast<double>(division);
    }

    struct Raw {
        std::uint64_t tick;
        int           channel, pitch, velocity;
        bool          on;
        int           order;
    };
    struct Open { std::int64_t tick; int velocity; };
    std::map<std::pair<int, int>, MidiPart> parts;   // (file track, channel)
    std::map<int, int> programOfChannel;              // for format 0
    int order = 0;

    for (int ti = 0; ti < ntracks && r.ok; ++ti) {
        if (!r.has(8)) break;
        const std::string tag(reinterpret_cast<const char*>(r.p), 4);
        r.skip(4);
        const std::uint32_t len = r.u32();
        if (!r.has(len)) return fail("a track runs past the end of the file");
        if (tag != "MTrk") { r.skip(len); continue; }
        Reader tr{r.p, r.p + len};
        r.skip(len);

        std::string trackName;
        std::vector<Raw> raw;
        std::map<int, int> program;   // this track's first program change per channel
        std::uint64_t tick = 0;
        std::uint8_t  status = 0;
        while (tr.ok && tr.p < tr.end) {
            tick += tr.vlq();
            std::uint8_t b = tr.u8();
            if (b == 0xFF) {
                const std::uint8_t type = tr.u8();
                const std::uint32_t mlen = tr.vlq();
                if (!tr.has(mlen)) break;
                const std::uint8_t* d = tr.p;
                if (type == 0x51 && mlen >= 3 && !out.hasTempo) {
                    const std::uint32_t us = (static_cast<std::uint32_t>(d[0]) << 16) | (d[1] << 8) | d[2];
                    if (us > 0) {
                        // To a thousandth: 140 BPM written as 428571 us reads back as 140.
                        out.bpm = std::clamp(std::round(60000000.0 / us * 1000.0) / 1000.0, 10.0, 999.0);
                        out.hasTempo = true;
                    }
                } else if (type == 0x58 && mlen >= 2 && !out.hasMeter) {
                    out.beatsPerBar = std::clamp(static_cast<int>(d[0]), 1, 32);
                    const int den   = 1 << std::min<int>(d[1], 4);
                    out.beatUnit    = (den == 2 || den == 4 || den == 8 || den == 16) ? den : 4;
                    out.hasMeter    = true;
                } else if (type == 0x03 && trackName.empty()) {
                    trackName.assign(reinterpret_cast<const char*>(d), mlen);
                } else if (type == 0x2F) {
                    tr.skip(mlen);
                    break;
                }
                tr.skip(mlen);
                continue;
            }
            if (b == 0xF0 || b == 0xF7) {
                tr.skip(tr.vlq());
                continue;
            }
            std::uint8_t d1;
            if (b & 0x80) {
                status = b;
                d1 = tr.u8();
            } else {
                if (!status) break;   // data with no status: give up on the track
                d1 = b;
            }
            const int kind = status & 0xF0, ch = status & 0x0F;
            switch (kind) {
            case 0x80:
            case 0x90: {
                const std::uint8_t vel = tr.u8();
                const bool on = kind == 0x90 && vel > 0;
                raw.push_back({tick, ch, d1 & 0x7F, vel & 0x7F, on, order++});
                break;
            }
            case 0xA0: case 0xB0: case 0xE0: tr.u8(); break;
            case 0xC0:
                if (!program.count(ch)) program[ch] = d1 & 0x7F;
                if (!programOfChannel.count(ch)) programOfChannel[ch] = d1 & 0x7F;
                break;
            case 0xD0: break;
            default: break;
            }
        }

        // Pair ons and offs per (channel, pitch), oldest first.
        auto toTicks = [&](std::uint64_t t) -> std::int64_t {
            if (smpte) {
                const double sec = static_cast<double>(t) / ticksPerSecondSmpte;
                return static_cast<std::int64_t>(std::llround(sec * out.bpm / 60.0 * kPpq));
            }
            return static_cast<std::int64_t>(std::llround(static_cast<double>(t) * scale));
        };
        std::map<std::pair<int, int>, std::vector<Open>> open;
        for (const Raw& e : raw) {
            const std::int64_t t = toTicks(e.tick);
            auto key = std::make_pair(e.channel, e.pitch);
            if (e.on) {
                open[key].push_back({t, e.velocity});
                continue;
            }
            auto it = open.find(key);
            if (it == open.end() || it->second.empty()) continue;
            const Open o = it->second.front();
            it->second.erase(it->second.begin());
            MidiPart& part = parts[{ti, e.channel}];
            Note n;
            n.tick     = o.tick;
            n.length   = std::max<std::int64_t>(1, t - o.tick);
            n.pitch    = e.pitch;
            n.velocity = std::clamp(o.velocity, 1, 127);
            part.notes.push_back(n);
        }
        // Notes never let go: held to the end of the track.
        const std::int64_t last = toTicks(tick);
        for (auto& [key, list] : open)
            for (const Open& o : list) {
                Note n;
                n.tick     = o.tick;
                n.length   = std::max<std::int64_t>(kPpq / 4, last - o.tick);
                n.pitch    = key.second;
                n.velocity = std::clamp(o.velocity, 1, 127);
                parts[{ti, key.first}].notes.push_back(n);
            }
        for (auto& [key, part] : parts) {
            if (key.first != ti) continue;
            part.channel = key.second;
            const auto pr = program.find(key.second);
            part.program = pr != program.end() ? pr->second : -1;
            part.name    = trackName;
        }
    }

    for (auto& [key, part] : parts) {
        if (part.notes.empty()) continue;
        if (part.program < 0) {
            const auto pr = programOfChannel.find(part.channel);
            if (pr != programOfChannel.end()) part.program = pr->second;
        }
        std::stable_sort(part.notes.begin(), part.notes.end(), [](const Note& a, const Note& b) {
            return a.tick != b.tick ? a.tick < b.tick : a.pitch < b.pitch;
        });
        for (const Note& n : part.notes) out.length = std::max(out.length, n.end());
        out.parts.push_back(std::move(part));
    }
    if (out.parts.empty()) return fail("the MIDI file has no notes");
    return true;
}

bool MidiFile::load(const std::string& path, MidiFile& out, std::string* error) {
    if (!vfs::exists(path)) {
        if (error) *error = "no such file: " + path;
        return false;
    }
    return parse(vfs::read(path), out, error);
}

std::vector<int> importMidi(Song& song, const MidiFile& file, std::int64_t at, bool adoptTempo) {
    if (adoptTempo) {
        if (file.hasTempo) song.bpm = file.bpm;
        if (file.hasMeter) {
            song.beatsPerBar = file.beatsPerBar;
            song.beatUnit    = file.beatUnit;
        }
    }
    std::vector<int> made;
    const std::int64_t bar = song.ticksPerBar();
    for (const MidiPart& part : file.parts) {
        const bool drums = part.channel == 9;
        std::string name = part.name;
        if (name.empty()) name = drums ? "Drums" : "Channel " + std::to_string(part.channel + 1);
        Track& t = song.addTrack(TrackKind::Instrument, name);
        if (drums) {
            t.instrument.kind = InstrumentKind::Drums;
        } else {
            t.instrument.kind  = InstrumentKind::Synth;
            t.instrument.patch = patchForProgram(part.program);
        }
        Clip c;
        c.uid    = song.newUid();
        c.start  = at;
        c.name   = name;
        c.notes  = part.notes;
        std::int64_t end = 0;
        for (const Note& n : c.notes) end = std::max(end, n.end());
        // Whole bars, so the clip ends where the music does on the grid.
        c.length = std::max(bar, ((end + bar - 1) / bar) * bar);
        t.clips.push_back(std::move(c));
        made.push_back(t.uid);
    }
    return made;
}

namespace {

void putVlq(std::vector<std::uint8_t>& b, std::uint32_t v) {
    std::uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    while (v >>= 7) tmp[n++] = static_cast<std::uint8_t>((v & 0x7F) | 0x80);
    while (n) b.push_back(tmp[--n]);
}

void putChunk(std::vector<std::uint8_t>& out, const char* tag, const std::vector<std::uint8_t>& body) {
    out.insert(out.end(), tag, tag + 4);
    const std::uint32_t n = static_cast<std::uint32_t>(body.size());
    out.push_back(static_cast<std::uint8_t>(n >> 24));
    out.push_back(static_cast<std::uint8_t>(n >> 16));
    out.push_back(static_cast<std::uint8_t>(n >> 8));
    out.push_back(static_cast<std::uint8_t>(n));
    out.insert(out.end(), body.begin(), body.end());
}

void putName(std::vector<std::uint8_t>& b, const std::string& name) {
    putVlq(b, 0);
    b.push_back(0xFF);
    b.push_back(0x03);
    putVlq(b, static_cast<std::uint32_t>(name.size()));
    b.insert(b.end(), name.begin(), name.end());
}

void putEnd(std::vector<std::uint8_t>& b) {
    putVlq(b, 0);
    b.push_back(0xFF);
    b.push_back(0x2F);
    b.push_back(0x00);
}

} // namespace

std::vector<std::uint8_t> exportMidi(const Song& song) {
    std::vector<std::vector<std::uint8_t>> chunks;

    // The tempo track: name, tempo, meter.
    {
        std::vector<std::uint8_t> b;
        putName(b, song.name);
        const std::uint32_t us = static_cast<std::uint32_t>(std::llround(60000000.0 / std::max(song.bpm, 1.0)));
        putVlq(b, 0);
        b.insert(b.end(), {0xFF, 0x51, 0x03, static_cast<std::uint8_t>(us >> 16),
                           static_cast<std::uint8_t>(us >> 8), static_cast<std::uint8_t>(us)});
        int pow2 = 0;
        while ((1 << pow2) < song.beatUnit && pow2 < 6) ++pow2;
        putVlq(b, 0);
        b.insert(b.end(), {0xFF, 0x58, 0x04, static_cast<std::uint8_t>(song.beatsPerBar),
                           static_cast<std::uint8_t>(pow2), 24, 8});
        putEnd(b);
        chunks.push_back(std::move(b));
    }

    int nextChannel = 0;
    for (const Track& t : song.tracks) {
        if (!t.hasNotes()) continue;
        int ch;
        if (t.kind == TrackKind::Midi) {
            ch = t.channel;
        } else if (t.instrument.kind == InstrumentKind::Drums) {
            ch = 9;
        } else {
            if (nextChannel == 9) ++nextChannel;
            ch = nextChannel % 16;
            ++nextChannel;
        }
        struct Ev { std::int64_t tick; bool on; int pitch, vel; };
        std::vector<Ev> evs;
        std::vector<Note> notes;
        for (const Clip& c : t.clips) {
            if (c.mute) continue;
            notes.clear();
            playedNotes(c, notes);
            for (const Note& n : notes) {
                const int p = std::clamp(n.pitch + t.transpose, 0, 127);
                evs.push_back({n.tick, true, p, n.velocity});
                evs.push_back({n.end(), false, p, 0});
            }
        }
        if (evs.empty()) continue;
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            return !a.on && b.on;   // offs first at a tie: a repeated note re-strikes
        });
        std::vector<std::uint8_t> b;
        putName(b, t.name);
        std::int64_t last = 0;
        for (const Ev& e : evs) {
            putVlq(b, static_cast<std::uint32_t>(std::max<std::int64_t>(0, e.tick - last)));
            last = e.tick;
            b.push_back(static_cast<std::uint8_t>((e.on ? 0x90 : 0x80) | ch));
            b.push_back(static_cast<std::uint8_t>(e.pitch));
            b.push_back(static_cast<std::uint8_t>(e.on ? std::clamp(e.vel, 1, 127) : 64));
        }
        putEnd(b);
        chunks.push_back(std::move(b));
    }

    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> head = {0, 1, static_cast<std::uint8_t>(chunks.size() >> 8),
                                      static_cast<std::uint8_t>(chunks.size() & 0xFF),
                                      static_cast<std::uint8_t>(kPpq >> 8), static_cast<std::uint8_t>(kPpq & 0xFF)};
    putChunk(out, "MThd", head);
    for (const auto& c : chunks) putChunk(out, "MTrk", c);
    return out;
}

} // namespace fitzel::song
