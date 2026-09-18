#include "fitzel/audio/Midi.hpp"

#include <algorithm>
#include <map>

#include "fitzel/asset/Vfs.hpp"

namespace fitzel::synth {

namespace {

// A cursor over the file that refuses to read past the end, so a truncated or
// hostile file stops the parse instead of reading memory it does not own.
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
    // A variable-length quantity: seven bits a byte, the high bit saying "more".
    // Four bytes at most, which is what the format allows.
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

struct RawEvent {
    std::uint64_t tick = 0;
    int           order = 0;       // file order, to keep ties stable
    std::uint8_t  channel = 0, note = 0, velocity = 0;
    bool          on = false;
};

} // namespace

bool MidiSequence::parse(const std::vector<std::uint8_t>& bytes, MidiSequence& out,
                         std::string* error) {
    out = MidiSequence{};
    auto fail = [&](const char* why) {
        if (error) *error = why;
        out = MidiSequence{};
        return false;
    };

    Reader r{bytes.data(), bytes.data() + bytes.size()};
    if (!r.has(14) || r.u32() != 0x4D546864u)   // "MThd"
        return fail("not a MIDI file (no MThd header)");
    const std::uint32_t hdrLen   = r.u32();
    const std::uint32_t format   = r.u16();
    const std::uint32_t ntracks  = r.u16();
    const std::uint32_t division = r.u16();
    if (hdrLen > 6) r.skip(hdrLen - 6);
    if (!r.ok || format > 2) return fail("unsupported MIDI header");
    if (division == 0) return fail("MIDI file has no time division");

    // Ticks to seconds. PPQ: a tick is a fraction of a beat, and the beat's
    // length is the tempo at the time. SMPTE: a tick is a fixed fraction of a
    // second and tempo does not apply.
    const bool   smpte = (division & 0x8000u) != 0;
    double       tickSeconds = 0.0;   // SMPTE only
    std::uint32_t ppq = 0;
    if (smpte) {
        const int fps = -static_cast<std::int8_t>(division >> 8);
        const int tpf = static_cast<int>(division & 0xFFu);
        if (fps <= 0 || tpf <= 0) return fail("bad SMPTE time division");
        tickSeconds = 1.0 / (static_cast<double>(fps) * static_cast<double>(tpf));
    } else {
        ppq = division;
    }

    std::vector<RawEvent>                   raw;
    std::map<std::uint64_t, std::uint32_t> tempo;   // tick -> microseconds per beat
    std::uint64_t                           lastTick = 0;
    int                                     order = 0;

    for (std::uint32_t t = 0; t < ntracks && r.ok; ++t) {
        if (!r.has(8)) break;
        const std::uint32_t id  = r.u32();
        const std::uint32_t len = r.u32();
        if (!r.has(len)) return fail("MIDI track runs past the end of the file");
        if (id != 0x4D54726Bu) {   // not "MTrk": a chunk we do not know, skip it
            r.skip(len);
            continue;
        }
        Reader tr{r.p, r.p + len};
        r.skip(len);

        std::uint64_t tick    = 0;
        std::uint8_t  running = 0;
        while (tr.ok && tr.p < tr.end) {
            tick += tr.vlq();
            std::uint8_t status = tr.u8();
            if (status < 0x80u) {
                // Running status: the byte just read is data for the last
                // status, which carries on without being repeated.
                if (!running) return fail("MIDI data byte with no status before it");
                --tr.p;
                status = running;
            }
            if (status == 0xFFu) {                      // meta event
                const std::uint8_t  type = tr.u8();
                const std::uint32_t mlen = tr.vlq();
                if (type == 0x51u && mlen == 3) {       // tempo
                    const std::uint32_t us = (static_cast<std::uint32_t>(tr.u8()) << 16) |
                                             (static_cast<std::uint32_t>(tr.u8()) << 8) |
                                             tr.u8();
                    if (us > 0) tempo[tick] = us;
                } else {
                    tr.skip(mlen);
                }
                if (type == 0x2Fu) break;               // end of track
                continue;
            }
            if (status == 0xF0u || status == 0xF7u) {   // SysEx: read past it
                tr.skip(tr.vlq());
                continue;
            }
            running = status;
            const std::uint8_t kind = status & 0xF0u;
            const std::uint8_t ch   = status & 0x0Fu;
            if (kind == 0x80u || kind == 0x90u) {
                const std::uint8_t note = tr.u8() & 0x7Fu;
                const std::uint8_t vel  = tr.u8() & 0x7Fu;
                RawEvent e;
                e.tick     = tick;
                e.order    = order++;
                e.channel  = ch;
                e.note     = note;
                e.velocity = vel;
                // A note-on at velocity 0 is a note-off, and files use it that
                // way constantly -- it lets a whole track run on one status.
                e.on       = (kind == 0x90u && vel > 0);
                raw.push_back(e);
            } else if (kind == 0xC0u || kind == 0xD0u) {
                tr.skip(1);                              // program, channel pressure
            } else {
                tr.skip(2);                              // aftertouch, CC, pitch bend
            }
            lastTick = std::max(lastTick, tick);
        }
        if (!tr.ok) return fail("MIDI track is cut short");
        lastTick = std::max(lastTick, tick);
    }
    if (!r.ok) return fail("MIDI file is cut short");

    // The tempo map, as (tick, seconds at that tick, seconds per tick after it).
    struct Seg { std::uint64_t tick; double at; double perTick; };
    std::vector<Seg> segs;
    if (!smpte) {
        std::uint32_t us   = 500000;   // 120 BPM until a file says otherwise
        std::uint64_t from = 0;
        double        at   = 0.0;
        auto perTick = [&](std::uint32_t u) {
            return static_cast<double>(u) / 1e6 / static_cast<double>(ppq);
        };
        segs.push_back({0, 0.0, perTick(us)});
        for (const auto& [tk, u] : tempo) {
            at  += static_cast<double>(tk - from) * perTick(us);
            from = tk;
            us   = u;
            if (!segs.empty() && segs.back().tick == tk) segs.back() = {tk, at, perTick(us)};
            else                                         segs.push_back({tk, at, perTick(us)});
        }
    }
    auto seconds = [&](std::uint64_t tick) {
        if (smpte) return static_cast<double>(tick) * tickSeconds;
        std::size_t k = 0;
        while (k + 1 < segs.size() && segs[k + 1].tick <= tick) ++k;
        return segs[k].at + static_cast<double>(tick - segs[k].tick) * segs[k].perTick;
    };

    std::stable_sort(raw.begin(), raw.end(), [](const RawEvent& a, const RawEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        // Offs first at a tie: a repeated note is "off, then on again", and
        // the other order would end the new note the instant it started.
        if (a.on != b.on) return !a.on;
        return a.order < b.order;
    });

    out.events.reserve(raw.size());
    for (const RawEvent& e : raw) {
        MidiEvent m;
        m.time     = seconds(e.tick);
        m.channel  = e.channel;
        m.note     = e.note;
        m.velocity = e.on ? static_cast<float>(e.velocity) / 127.0f : 0.0f;
        m.on       = e.on;
        out.events.push_back(m);
    }
    out.length = seconds(lastTick);
    out.format = static_cast<int>(format);
    out.tracks = static_cast<int>(ntracks);
    return true;
}

bool MidiSequence::load(const std::string& path, MidiSequence& out, std::string* error) {
    const std::vector<std::uint8_t> bytes = vfs::read(path);
    if (bytes.empty()) {
        if (error) *error = "cannot read " + path;
        out = MidiSequence{};
        return false;
    }
    return parse(bytes, out, error);
}

} // namespace fitzel::synth
