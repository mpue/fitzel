#include "fitzel/audio/SynthPoly.hpp"

#include <algorithm>
#include <cmath>

namespace fitzel::synth {

namespace {
// The longest piece a voice renders at once. process() may be asked for more
// (a device buffer can be large) and splits it; the scratch buffer is sized to
// this once, so nothing allocates while the song plays.
constexpr int kMaxChunk = 4096;
// Below this a released voice counts as gone and stops being rendered.
constexpr float kSilence = 1e-5f;
} // namespace

bool Poly::compile(const Patch& patch, double sampleRate, int voices, int maxBlock,
                   std::string* error) {
    m_voices.clear();
    voices = std::clamp(voices, 1, 32);
    std::vector<Voice> vs(static_cast<std::size_t>(voices));
    for (Voice& v : vs)
        if (!v.graph.compile(patch, sampleRate, maxBlock, error)) return false;

    m_voices = std::move(vs);
    m_tmp.assign(static_cast<std::size_t>(kMaxChunk), 0.0f);
    m_rate  = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_pitch = m_voices[0].graph.inputIndex("pitch");
    m_gate  = m_voices[0].graph.inputIndex("gate");
    m_vel   = m_voices[0].graph.inputIndex("velocity");
    // A patch with no gate is a drone: it sounds from the moment it runs. The
    // first voice is the one that plays; the others would only double it, so
    // they wait until a note asks for one.
    for (std::size_t i = 0; i < m_voices.size(); ++i)
        m_voices[i].silent = (m_gate >= 0) || i > 0;
    return true;
}

bool Poly::updateParams(const Patch& patch) {
    bool ok = true;
    for (Voice& v : m_voices) ok = v.graph.updateParams(patch) && ok;
    return ok;
}

void Poly::reset() {
    for (std::size_t i = 0; i < m_voices.size(); ++i) {
        Voice& v = m_voices[i];
        v.graph.reset();
        v.note   = -1;
        v.held   = false;
        v.silent = (m_gate >= 0) || i > 0;
        if (m_gate >= 0) v.graph.setInput(m_gate, 0.0f);
    }
    m_pos   = 0;
    m_clock = 0.0;
}

int Poly::inputIndex(const std::string& name) const {
    return m_voices.empty() ? -1 : m_voices[0].graph.inputIndex(name);
}

void Poly::setInput(int index, float value) {
    for (Voice& v : m_voices) v.graph.setInput(index, value);
}

void Poly::noteOn(int note, float velocity) {
    if (m_voices.empty()) return;
    // The same note struck again takes a fresh voice and lets the old one ring
    // out, which is what a piano does and what keeps a fast repeat from
    // chopping its own tail off.
    noteOff(note);

    // A voice that is done, else the one released longest ago, else steal the
    // oldest note still held.
    Voice* pick = nullptr;
    for (Voice& v : m_voices)
        if (!v.held && v.silent) { pick = &v; break; }
    if (!pick)
        for (Voice& v : m_voices)
            if (!v.held && (!pick || v.released < pick->released)) pick = &v;
    if (!pick)
        for (Voice& v : m_voices)
            if (!pick || v.started < pick->started) pick = &v;

    // A voice whose gate the graph has not yet seen drop -- stolen while held,
    // or let go earlier in this very block -- would never see an edge, and an
    // envelope that sees no edge never starts again. Starting it from silence
    // is the honest way to give it one.
    if (pick->held || pick->releasedBlock == m_block) pick->graph.reset();

    pick->note    = note;
    pick->held    = true;
    pick->silent  = false;
    pick->started = ++m_stamp;
    if (m_pitch >= 0) pick->graph.setInput(m_pitch, static_cast<float>(note));
    if (m_vel >= 0)   pick->graph.setInput(m_vel, std::clamp(velocity, 0.0f, 1.0f));
    if (m_gate >= 0)  pick->graph.setInput(m_gate, 1.0f);
}

void Poly::noteOff(int note) {
    for (Voice& v : m_voices)
        if (v.held && v.note == note) {
            v.held          = false;
            v.released      = ++m_stamp;
            v.releasedBlock = m_block;
            if (m_gate >= 0) v.graph.setInput(m_gate, 0.0f);
        }
}

void Poly::allNotesOff() {
    for (Voice& v : m_voices)
        if (v.held) noteOff(v.note);
}

int Poly::heldNotes() const {
    int n = 0;
    for (const Voice& v : m_voices) n += v.held ? 1 : 0;
    return n;
}

int Poly::soundingVoices() const {
    int n = 0;
    for (const Voice& v : m_voices) n += (v.held || !v.silent) ? 1 : 0;
    return n;
}

void Poly::setSequence(const MidiSequence* seq) {
    allNotesOff();
    m_seq     = seq;
    m_pos     = 0;
    m_clock   = 0.0;
    m_playing = false;
}

void Poly::play(bool fromStart) {
    // A song with nothing in it has nothing to play -- and an empty one set to
    // loop would restart itself forever without ever rendering a sample.
    if (!m_seq || m_seq->events.empty()) return;
    if (fromStart) {
        allNotesOff();
        m_pos   = 0;
        m_clock = 0.0;
    }
    m_playing = true;
}

void Poly::stop() {
    m_playing = false;
    allNotesOff();
}

void Poly::fireDue() {
    const std::vector<MidiEvent>& ev = m_seq->events;
    while (m_pos < ev.size() && ev[m_pos].time <= m_clock + halfSample()) {
        const MidiEvent& e = ev[m_pos++];
        if (m_skipDrums && e.channel == 9) continue;
        if (m_channel >= 0 && e.channel != m_channel) continue;
        if (e.on) noteOn(e.note, e.velocity);
        else      noteOff(e.note);
    }
}

void Poly::render(float* out, int frames) {
    ++m_block;
    for (Voice& v : m_voices) {
        if (!v.held && v.silent) continue;
        float peak = 0.0f;
        v.graph.process(m_tmp.data(), frames);
        for (int i = 0; i < frames; ++i) {
            out[i] += m_tmp[static_cast<std::size_t>(i)] * m_gain;
            peak = std::max(peak, std::fabs(m_tmp[static_cast<std::size_t>(i)]));
        }
        // A released voice that has gone quiet stops costing anything. Only
        // gated patches: a drone has nothing to be released from.
        if (m_gate >= 0 && !v.held && peak < kSilence) v.silent = true;
    }
}

void Poly::process(float* out, int frames) {
    if (!out || frames <= 0) return;
    std::fill(out, out + frames, 0.0f);
    if (m_voices.empty()) return;

    int done = 0;
    while (done < frames) {
        int chunk = std::min(frames - done, kMaxChunk);

        if (m_seq && m_playing) {
            fireDue();
            const std::vector<MidiEvent>& ev = m_seq->events;
            if (m_pos < ev.size()) {
                // Render up to the next event and no further, so it lands on
                // its own sample.
                const double until = (ev[m_pos].time - m_clock) * m_rate /
                                     static_cast<double>(m_tempo);
                chunk = std::clamp(static_cast<int>(std::lround(until)), 1, chunk);
            } else if (m_clock >= m_seq->length - halfSample()) {
                // The end of the song.
                // A song that takes no time (every event at zero) cannot loop:
                // it would restart forever without rendering a sample.
                if (m_loop && m_seq->length > 1e-6) {
                    allNotesOff();
                    m_pos   = 0;
                    m_clock = 0.0;
                    continue;
                }
                m_playing = false;
                allNotesOff();
            } else {
                // Past the last event but short of the end: play out the gap.
                const double until = (m_seq->length - m_clock) * m_rate /
                                     static_cast<double>(m_tempo);
                chunk = std::clamp(static_cast<int>(std::lround(until)), 1, chunk);
            }
        }

        render(out + done, chunk);
        if (m_seq && m_playing)
            m_clock += static_cast<double>(chunk) * static_cast<double>(m_tempo) / m_rate;
        done += chunk;
    }

    for (int i = 0; i < frames; ++i)
        out[i] = std::isfinite(out[i]) ? std::clamp(out[i], -1.0f, 1.0f) : 0.0f;
}

} // namespace fitzel::synth
