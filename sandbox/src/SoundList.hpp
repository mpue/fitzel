#pragma once

#include <algorithm>
#include <random>
#include <string>
#include <vector>

// A sound field that may name SEVERAL samples, one of which is played each time.
//
// One impact sample is one impact sample: by the third crash the ear has it
// memorised and stops hearing a collision at all, which is the opposite of what
// a crash sound is for. The fields that fire over and over -- the hull thud above
// all -- therefore take a list, and a pick is made per hit.
//
// The list travels as a comma-separated string in the same field it has always
// been ("thud_a.wav, thud_b.wav"). That is the point of doing it this way: a
// field that stays a string serialises, snapshots, undoes and rides inside a
// prefab exactly as it did before, with no schema change and no migration -- and
// a project naming one sound is simply a list of one, so nothing about an
// existing craft behaves differently. Nobody is asked to type the commas: the
// editor draws the field as rows of the same sound picker every other sound
// field uses (see gliderui::inspector).
namespace soundlist {

// The field's entries, in order, with the blanks and the padding dropped.
inline std::vector<std::string> split(const std::string& field) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i <= field.size()) {
        const std::size_t c = field.find(',', i);
        const std::size_t e = (c == std::string::npos) ? field.size() : c;
        std::size_t a = i, b = e;
        while (a < b && (field[a] == ' ' || field[a] == '\t')) ++a;
        while (b > a && (field[b - 1] == ' ' || field[b - 1] == '\t')) --b;
        if (b > a) out.push_back(field.substr(a, b - a));
        if (c == std::string::npos) break;
        i = c + 1;
    }
    return out;
}

// Back into one field. The separator carries a space so the raw text stays
// readable wherever a plain text field shows it.
inline std::string join(const std::vector<std::string>& list) {
    std::string out;
    for (const std::string& s : list) {
        if (s.empty()) continue;
        if (!out.empty()) out += ", ";
        out += s;
    }
    return out;
}

// One entry at random, or "" for an empty field (and then the caller stays
// silent, exactly as it did when the field was blank).
inline std::string pick(const std::string& field) {
    const std::vector<std::string> all = split(field);
    if (all.empty())      return std::string();
    if (all.size() == 1)  return all.front();
    // Fixed seed: a crash sequence that is the same from one run to the next
    // costs nothing here and keeps a session reproducible when something has to
    // be chased down.
    static thread_local std::mt19937 rng{0xC0FFEEu};
    static thread_local std::string  last;
    std::uniform_int_distribution<std::size_t> d(0, all.size() - 1);
    std::string s = all[d(rng)];
    // Never the same sample twice running. With two entries a fair coin repeats
    // half the time, and a repeat is the one thing a list of samples exists to
    // avoid -- one redraw is enough to make that rare instead of routine.
    if (s == last) s = all[d(rng)];
    last = s;
    return s;
}

} // namespace soundlist
