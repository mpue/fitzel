#pragma once

#include <map>
#include <string>
#include <vector>

// The circuit records: what has been driven on each track, kept between sessions.
//
// The file sits NEXT TO THE EXECUTABLE, like difficulty.json and the graphics
// settings, and for a harder reason than either of them: everything else a game
// ships -- content/, project/, assets/ -- goes into the encrypted archive on
// export, and an archive is read-only. A record that could not be written after
// the race that set it would not be a record.
//
// A circuit is named by its scene stem, which is the same name the showroom's
// Track carries and the same one a launch travels under, so recording a time and
// looking one up cannot disagree about what a track is.
namespace leaderboard {

// Rows kept per circuit. Ten because that is what fits on the screen without
// scrolling, and a table nobody can reach the bottom of is a table with a
// hiding place in it.
inline constexpr int kTop = 10;

// One finished race.
struct Entry {
    // What the table is ORDERED by. The best lap is the only figure here that
    // means the same thing in every row: a total time is a time over a distance,
    // and three laps of a circuit cannot be ranked against five of it. The total
    // is kept and shown beside it, but it does not decide the order.
    float       bestLap = 0.0f;   // seconds
    float       total   = 0.0f;   // seconds over the whole distance
    int         laps    = 0;      // the distance it was set over
    int         level   = 0;      // difficulty step (index into difficulty's ladder)
    std::string date;             // "2026-08-22", local -- enough to tell runs apart

    bool valid() const { return bestLap > 0.0f; }
};

struct Table {
    // Ordered rather than hashed so the file comes out the same twice running:
    // a settings file that reshuffles itself on every write is one nobody can
    // diff, and this one is small enough that the ordering costs nothing.
    std::map<std::string, std::vector<Entry>> byTrack;
};

// Missing file / unreadable / half-written -> an empty table rather than a
// failure. Losing the records is a disappointment; refusing to start the game
// over them would be worse.
Table load(const std::string& file);
void  save(const std::string& file, const Table& t);

// Add a finished race to `track`, keep the list sorted and trimmed to kTop.
// Returns the 1-based place it took, or 0 if it was not quick enough to make the
// list (or the entry has no completed lap in it).
int record(Table& t, const std::string& track, const Entry& e);

// One circuit's rows, fastest first. Empty when nothing has been driven there.
const std::vector<Entry>& rows(const Table& t, const std::string& track);

// m:ss.mmm, the clock the HUD writes lap times in. An empty or zero time comes
// back as "--:--.---" so a gap in a table reads as a gap and not as a zero.
std::string formatTime(float seconds);

// Today, as an Entry::date. Local time: these are somebody's own records on
// their own machine, and the day they set them is the day they remember.
std::string today();

} // namespace leaderboard
