#include "Leaderboard.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>

#include <nlohmann/json.hpp>

namespace leaderboard {

namespace {

// Fastest lap first. Ties broken by the total time, then by the distance, so two
// identical laps still come out in a stable order rather than swapping places
// every time the file is rewritten.
bool quicker(const Entry& a, const Entry& b) {
    if (a.bestLap != b.bestLap) return a.bestLap < b.bestLap;
    if (a.total   != b.total)   return a.total   < b.total;
    return a.laps > b.laps;
}

const std::vector<Entry> kNone;

} // namespace

Table load(const std::string& file) {
    Table t;
    std::ifstream in(file);
    if (!in) return t;
    nlohmann::json j;
    try { in >> j; }
    catch (const nlohmann::json::exception&) { return t; }   // hand-edited to bits
    if (!j.is_object()) return t;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_array()) continue;
        std::vector<Entry> rows;
        for (const auto& r : it.value()) {
            if (!r.is_object()) continue;
            Entry e;
            e.bestLap = r.value("bestLap", 0.0f);
            e.total   = r.value("total",   0.0f);
            e.laps    = r.value("laps",    0);
            e.level   = r.value("level",   0);
            e.date    = r.value("date",    std::string{});
            if (e.valid()) rows.push_back(std::move(e));
        }
        // Sorted and trimmed on the way in as well as on the way out: the file is
        // plain text next to the exe, so it may well have been typed into.
        std::sort(rows.begin(), rows.end(), quicker);
        if (static_cast<int>(rows.size()) > kTop) rows.resize(kTop);
        if (!rows.empty()) t.byTrack[it.key()] = std::move(rows);
    }
    return t;
}

void save(const std::string& file, const Table& t) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [track, rows] : t.byTrack) {
        nlohmann::json arr = nlohmann::json::array();
        for (const Entry& e : rows)
            arr.push_back({{"bestLap", e.bestLap}, {"total", e.total},
                           {"laps", e.laps}, {"level", e.level}, {"date", e.date}});
        j[track] = std::move(arr);
    }
    std::ofstream f(file);
    if (f) f << j.dump(2) << '\n';
}

int record(Table& t, const std::string& track, const Entry& e) {
    if (track.empty() || !e.valid()) return 0;
    std::vector<Entry>& rows = t.byTrack[track];
    rows.push_back(e);
    std::sort(rows.begin(), rows.end(), quicker);
    if (static_cast<int>(rows.size()) > kTop) rows.resize(kTop);
    // Which row IS the one just added: compare by identity of the whole entry
    // rather than by time, so a run that matches an existing record exactly is
    // reported at its own place instead of at the older row's.
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].bestLap == e.bestLap && rows[i].total == e.total &&
            rows[i].laps == e.laps && rows[i].level == e.level &&
            rows[i].date == e.date)
            return static_cast<int>(i) + 1;
    return 0;   // pushed straight back out by ten faster rows
}

const std::vector<Entry>& rows(const Table& t, const std::string& track) {
    const auto it = t.byTrack.find(track);
    return it == t.byTrack.end() ? kNone : it->second;
}

std::string formatTime(float seconds) {
    if (!(seconds > 0.0f)) return "--:--.---";
    const int total = static_cast<int>(seconds);
    const int mm    = total / 60;
    const int ss    = total % 60;
    const int ms    = static_cast<int>((seconds - static_cast<float>(total)) * 1000.0f);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d.%03d", mm, ss, ms);
    return buf;
}

std::string today() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

} // namespace leaderboard
