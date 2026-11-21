// fitzelgeo: the local geometry cache behind the world generator's "Real place".
//
// The editor never fetches map data itself. It asks this server, on its own
// machine, over plain HTTP; the server answers from its cache on disk or fetches
// from the public services once, keeps what it got, and answers from the cache
// from then on -- the second look at a place streams in at disk speed, works
// offline, and costs the volunteers who run OpenStreetMap's servers nothing.
//
//   GET /status                      who is here, what is cached
//   GET /map/{z}/{x}/{y}.png         the map picture (tile.openstreetmap.org)
//   GET /dem/{z}/{x}/{y}.png         elevation, Terrarium-coded (AWS open terrain tiles)
//   GET /osm/{x}/{y}.json            geometry of one zoom-15 tile (Overpass, see GeoTiles.hpp)
//   GET /search?q=Kassel             places by name (Nominatim)
//   GET /has?x0=&y0=&x1=&y1=         which zoom-15 geometry tiles of a range are cached
//
// Being polite is the point of it as much as speed: map pictures at most two at
// a time (the OSM tile policy), one Overpass query at a time with a pause after
// each and a back-off when it says it is busy, a place search at most once a
// second, and everything identified by a User-Agent naming this program.
//
//   fitzelgeo [--port 8742] [--bind 127.0.0.1] [--cache DIR] [--offline]
//             [--overpass URL] [--contact TEXT] [--quiet]
//   fitzelgeo --probe [host:port]                is a server answering there? (exit 0/1)
//   fitzelgeo --prefetch LAT LON SIZE_M          fill the cache for an area, no server
//
// Bound to 127.0.0.1 by default: a shared cache for a LAN is --bind 0.0.0.0.
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "GeoNet.hpp"
#include "GeoTiles.hpp"
#include "UserDir.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kVersion = 1;

struct Config {
    int         port     = geo::kDefaultPort;
    std::string bind     = "127.0.0.1";
    fs::path    cache;
    bool        offline  = false;
    bool        quiet    = false;
    std::string overpass = "https://overpass-api.de/api/interpreter";
    std::string contact;
} g_cfg;

std::atomic<bool>      g_stop{false};
std::atomic<long long> g_hits{0}, g_misses{0}, g_fails{0};
std::atomic<int>       g_live{0};
geonet::Socket         g_listener = -1;

double seconds() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void say(const char* fmt, ...) {
    if (g_cfg.quiet) return;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stdout, fmt, a);
    va_end(a);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::string userAgent() {
    std::string ua = "Fitzel-geo/" + std::to_string(kVersion) + " (Fitzel world generator; local tile cache)";
    if (!g_cfg.contact.empty()) ua += " " + g_cfg.contact;
    return ua;
}

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool writeFile(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".part" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

// --- Taking turns ------------------------------------------------------------------

// At most `n` at once, and at least `gap` seconds between two starts.
class Gate {
public:
    Gate(int n, double gap) : m_n(n), m_gap(gap) {}
    void enter() {
        std::unique_lock<std::mutex> lock(m_m);
        m_cv.wait(lock, [&] { return m_in < m_n; });
        ++m_in;
        const double wait = m_last + m_gap - seconds();
        m_last = std::max(seconds(), m_last + m_gap);
        lock.unlock();
        if (wait > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
    }
    void leave() {
        std::lock_guard<std::mutex> lock(m_m);
        --m_in;
        m_cv.notify_one();
    }

private:
    std::mutex m_m;
    std::condition_variable m_cv;
    int m_n, m_in = 0;
    double m_gap, m_last = -1e9;
};

Gate g_mapGate(2, 0.0), g_demGate(6, 0.0), g_overpassGate(1, 1.0), g_searchGate(1, 1.1);

// One fetch per file: a second request for something already on its way waits
// for the first instead of fetching it again.
std::mutex g_flightM;
std::condition_variable g_flightCv;
std::set<std::string> g_inFlight;

struct Flight {
    std::string key;
    explicit Flight(std::string k) : key(std::move(k)) {
        std::unique_lock<std::mutex> lock(g_flightM);
        g_flightCv.wait(lock, [&] { return g_inFlight.count(key) == 0; });
        g_inFlight.insert(key);
    }
    ~Flight() {
        std::lock_guard<std::mutex> lock(g_flightM);
        g_inFlight.erase(key);
        g_flightCv.notify_all();
    }
};

// --- Upstream ----------------------------------------------------------------------------

// curl, as a child: no TLS library in this build (see GeoNet.hpp). `post` is a
// file whose contents go as the form field `data` (Overpass). 0 = fetched, else
// curl's exit code (22 = the server said no).
int curl(const std::string& url, const fs::path& out, int timeoutSec, const fs::path& post = {}) {
    std::vector<std::string> argv = {"curl", "-sS", "-f", "-L", "--compressed",
                                     "--max-time", std::to_string(timeoutSec),
                                     "-A", userAgent(), "-o", out.string()};
    if (!post.empty()) {
        argv.push_back("--data-urlencode");
        argv.push_back("data@" + post.string());
    }
    argv.push_back(url);
    return geonet::run(argv, timeoutSec + 15);
}

// Fetch `url` into `file` (atomically). False when it could not be had.
bool fetchTo(const std::string& url, const fs::path& file, Gate& gate, int timeoutSec, std::string& why) {
    if (g_cfg.offline) { why = "offline"; return false; }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".dl";
    gate.enter();
    const double t0 = seconds();
    const int rc = curl(url, tmp, timeoutSec);
    gate.leave();
    if (rc != 0) {
        fs::remove(tmp, ec);
        why = rc == 22 ? "the server refused" : rc < 0 ? "curl did not run (is it installed?)" : "curl failed (" + std::to_string(rc) + ")";
        ++g_fails;
        return false;
    }
    fs::rename(tmp, file, ec);
    if (ec) { why = "cannot write the cache"; return false; }
    say("  fetched %s (%.1f s)", url.c_str(), seconds() - t0);
    return true;
}

// --- The kinds of thing kept --------------------------------------------------------------

fs::path mapFile(int z, int x, int y) { return g_cfg.cache / "map" / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png"); }
fs::path demFile(int z, int x, int y) { return g_cfg.cache / "dem" / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png"); }
fs::path osmFile(int x, int y) { return g_cfg.cache / "osm" / std::to_string(geo::kGeoZoom) / std::to_string(x) / (std::to_string(y) + ".json"); }

bool olderThanDays(const fs::path& p, int days) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return true;
    return fs::file_time_type::clock::now() - t > std::chrono::hours(24 * days);
}

// A picture of the map: kept a month, then fetched anew (a stale one is still
// served if the network is gone).
bool mapTile(int z, int x, int y, std::string& body, bool& hit, std::string& why) {
    const fs::path f = mapFile(z, x, y);
    std::error_code ec;
    hit = fs::exists(f, ec) && !(olderThanDays(f, 30) && !g_cfg.offline);
    if (!hit) {
        Flight fl(f.string());
        hit = fs::exists(f, ec) && !(olderThanDays(f, 30) && !g_cfg.offline);
        if (!hit) {
            const std::string url = "https://tile.openstreetmap.org/" + std::to_string(z) + "/" +
                                    std::to_string(x) + "/" + std::to_string(y) + ".png";
            if (!fetchTo(url, f, g_mapGate, 30, why) && !fs::exists(f, ec)) return false;
        }
    }
    return readFile(f, body);
}

bool demTile(int z, int x, int y, std::string& body, bool& hit, std::string& why) {
    const fs::path f = demFile(z, x, y);
    std::error_code ec;
    hit = fs::exists(f, ec);
    if (!hit) {
        Flight fl(f.string());
        hit = fs::exists(f, ec);
        if (!hit) {
            const std::string url = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/" + std::to_string(z) +
                                    "/" + std::to_string(x) + "/" + std::to_string(y) + ".png";
            if (!fetchTo(url, f, g_demGate, 60, why)) return false;
        }
    }
    return readFile(f, body);
}

// One Overpass query for the whole metatile, split into its geometry tiles.
bool fetchMeta(const geo::TileId& meta, std::string& why) {
    if (g_cfg.offline) { why = "offline"; return false; }
    const geo::BBox b = geo::tileBounds(meta);
    const fs::path dir = g_cfg.cache / "overpass";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string stem = std::to_string(meta.z) + "_" + std::to_string(meta.x) + "_" + std::to_string(meta.y);
    const fs::path qf = dir / (stem + ".query"), out = dir / (stem + ".json.dl");
    if (!writeFile(qf, geo::overpassQuery(b))) { why = "cannot write the cache"; return false; }
    // Busy (429), timed out (504) or cut off: wait and try again, a little longer
    // each time, as the API asks.
    const double backoff[] = {0.0, 15.0, 45.0, 90.0};
    for (double wait : backoff) {
        if (wait > 0.0) {
            say("  overpass busy, waiting %.0f s", wait);
            std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        }
        g_overpassGate.enter();
        const double t0 = seconds();
        const int rc = curl(g_cfg.overpass, out, 200, qf);
        g_overpassGate.leave();
        if (rc != 0) { why = "Overpass refused (" + std::to_string(rc) + ")"; ++g_fails; continue; }
        std::string text;
        readFile(out, text);
        fs::remove(out, ec);
        std::vector<geo::Feature> all;
        if (!geo::fromOverpass(text, all, &why)) { ++g_fails; continue; }
        for (const geo::TileId& t : geo::childrenAt(meta, geo::kGeoZoom))
            writeFile(osmFile(t.x, t.y), geo::writeTile(geo::forTile(all, t), t));
        say("  overpass %d/%d/%d: %zu features, %.1f MB (%.1f s)", meta.z, meta.x, meta.y, all.size(),
            static_cast<double>(text.size()) / 1e6, seconds() - t0);
        fs::remove(qf, ec);
        return true;
    }
    return false;
}

bool osmTile(int x, int y, std::string& body, bool& hit, std::string& why) {
    const fs::path f = osmFile(x, y);
    std::error_code ec;
    hit = fs::exists(f, ec);
    if (!hit) {
        const geo::TileId meta = geo::metaOf({geo::kGeoZoom, x, y});
        Flight fl("meta " + std::to_string(meta.x) + "/" + std::to_string(meta.y));
        hit = fs::exists(f, ec);
        if (!hit && !fetchMeta(meta, why)) return false;
    }
    return readFile(f, body);
}

bool search(const std::string& q, std::string& body, bool& hit, std::string& why) {
    std::string key;
    for (char c : q) key += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
    if (key.size() > 80) key = key.substr(0, 60) + "_" + std::to_string(std::hash<std::string>{}(q));
    const fs::path f = g_cfg.cache / "search" / (key + ".json");
    std::error_code ec;
    hit = fs::exists(f, ec);
    if (!hit) {
        Flight fl(f.string());
        const std::string url = "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=8&q=" + geo::urlEncode(q);
        if (!fetchTo(url, f, g_searchGate, 30, why)) return false;
    }
    return readFile(f, body);
}

// --- Serving -----------------------------------------------------------------------------

bool parseTilePath(const std::string& rest, int& z, int& x, int& y, const char* ext) {
    char tail[16] = {0};
    if (std::sscanf(rest.c_str(), "%d/%d/%d%15s", &z, &x, &y, tail) != 4) return false;
    if (std::strcmp(tail, ext) != 0) return false;
    return z >= 0 && z <= 20 && x >= 0 && y >= 0 && x < (1 << z) && y < (1 << z);
}

void serve(geonet::Socket s) {
    geonet::Request req;
    if (!geonet::readRequest(s, req)) { geonet::closeSocket(s); --g_live; return; }
    const double t0 = seconds();
    int status = 200;
    std::string type = "application/json", body, why;
    bool hit = false;
    const std::string& p = req.path;
    int z = 0, x = 0, y = 0;
    if (req.method != "GET") {
        status = 400; body = R"({"error":"GET only"})";
    } else if (p == "/status") {
        nlohmann::json j = {{"ok", true}, {"server", "fitzelgeo"}, {"version", kVersion},
                            {"offline", g_cfg.offline}, {"cache", g_cfg.cache.string()},
                            {"hits", g_hits.load()}, {"misses", g_misses.load()}, {"failures", g_fails.load()}};
        body = j.dump();
    } else if (p.rfind("/map/", 0) == 0 && parseTilePath(p.substr(5), z, x, y, ".png") && z <= 19) {
        type = "image/png";
        if (!mapTile(z, x, y, body, hit, why)) status = 502;
    } else if (p.rfind("/dem/", 0) == 0 && parseTilePath(p.substr(5), z, x, y, ".png") && z <= 15) {
        type = "image/png";
        if (!demTile(z, x, y, body, hit, why)) status = 502;
    } else if (p.rfind("/osm/", 0) == 0 &&
               std::sscanf(p.c_str() + 5, "%d/%d.json", &x, &y) == 2 && x >= 0 && y >= 0 &&
               x < (1 << geo::kGeoZoom) && y < (1 << geo::kGeoZoom)) {
        if (!osmTile(x, y, body, hit, why)) status = 502;
    } else if (p == "/search" && req.query.count("q")) {
        if (!search(req.query["q"], body, hit, why)) status = 502;
    } else if (p == "/has") {
        const int x0 = std::atoi(req.query["x0"].c_str()), x1 = std::atoi(req.query["x1"].c_str());
        const int y0 = std::atoi(req.query["y0"].c_str()), y1 = std::atoi(req.query["y1"].c_str());
        nlohmann::json tiles = nlohmann::json::array();
        std::error_code ec;
        if (x1 - x0 <= 64 && y1 - y0 <= 64)
            for (int ty = y0; ty <= y1; ++ty)
                for (int tx = x0; tx <= x1; ++tx)
                    if (fs::exists(osmFile(tx, ty), ec)) tiles.push_back({tx, ty});
        body = nlohmann::json{{"tiles", tiles}}.dump();
    } else {
        status = 404; body = R"({"error":"no such thing"})";
    }
    if (status == 502) {
        type = "application/json";
        body = nlohmann::json{{"error", why.empty() ? "could not be fetched" : why}}.dump();
    }
    if (status == 200 && p != "/status" && p != "/has") (hit ? g_hits : g_misses)++;
    geonet::respond(s, status, type, body, {{"X-Cache", hit ? "hit" : "miss"}});
    geonet::closeSocket(s);
    if (!hit || status != 200) say("%d %s%s (%.0f ms)", status, p.c_str(), hit ? " [cache]" : "", (seconds() - t0) * 1000.0);
    --g_live;
}

void onSignal(int) {
    g_stop = true;
    geonet::closeSocket(g_listener);
    g_listener = -1;
}

int probe(const std::string& target) {
    std::string host = "127.0.0.1";
    int port = geo::kDefaultPort;
    if (!target.empty()) {
        const std::size_t c = target.rfind(':');
        host = target.substr(0, c);
        if (c != std::string::npos) port = std::atoi(target.c_str() + c + 1);
    }
    geonet::Response r;
    std::string err;
    if (!geonet::get(host, port, "/status", 5, r, &err) || r.status != 200) {
        std::fprintf(stderr, "probe %s:%d: %s\n", host.c_str(), port, err.empty() ? "not a fitzelgeo" : err.c_str());
        return 1;
    }
    std::printf("%s\n", r.body.c_str());
    return 0;
}

// Fill the cache for an area without a server: the same tiles the editor asks for.
int prefetch(double lat, double lon, double size) {
    const geo::BBox b = geo::squareAround({lon, lat}, size);
    int ok = 0, bad = 0;
    std::string body, why;
    bool hit = false;
    for (const geo::TileId& t : geo::tilesCovering(b, geo::kGeoZoom))
        (osmTile(t.x, t.y, body, hit, why) ? ok : bad)++;
    for (const geo::TileId& t : geo::tilesCovering(b, 14))
        (demTile(t.z, t.x, t.y, body, hit, why) ? ok : bad)++;
    std::printf("prefetch: %d tiles ready, %d failed%s%s\n", ok, bad, bad ? " -- " : "", bad ? why.c_str() : "");
    return bad ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    g_cfg.cache = userdir::root() / "geocache";
    std::string probeTarget;
    bool doProbe = false;
    double pre[3] = {0, 0, 0};
    bool doPrefetch = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--port") g_cfg.port = std::atoi(next().c_str());
        else if (a == "--bind") g_cfg.bind = next();
        else if (a == "--cache") g_cfg.cache = next();
        else if (a == "--offline") g_cfg.offline = true;
        else if (a == "--quiet") g_cfg.quiet = true;
        else if (a == "--overpass") g_cfg.overpass = next();
        else if (a == "--contact") g_cfg.contact = next();
        else if (a == "--probe") {
            doProbe = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') probeTarget = next();
        } else if (a == "--prefetch" && i + 3 < argc) {
            doPrefetch = true;
            pre[0] = std::atof(argv[++i]); pre[1] = std::atof(argv[++i]); pre[2] = std::atof(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            std::printf("fitzelgeo [--port %d] [--bind 127.0.0.1] [--cache DIR] [--offline]\n"
                        "          [--overpass URL] [--contact TEXT] [--quiet]\n"
                        "fitzelgeo --probe [host:port]\n"
                        "fitzelgeo --prefetch LAT LON SIZE_M\n", geo::kDefaultPort);
            return 0;
        } else {
            std::fprintf(stderr, "fitzelgeo: unknown option %s (--help)\n", a.c_str());
            return 2;
        }
    }
    geonet::init();
    if (doProbe) return probe(probeTarget);
    std::error_code ec;
    fs::create_directories(g_cfg.cache, ec);
    if (doPrefetch) return prefetch(pre[0], pre[1], pre[2]);

    std::string err;
    g_listener = geonet::listenOn(g_cfg.bind, g_cfg.port, &err);
    if (g_listener < 0) {
        std::fprintf(stderr, "fitzelgeo: %s\n", err.c_str());
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    say("fitzelgeo %d on %s:%d, cache %s%s", kVersion, g_cfg.bind.c_str(), g_cfg.port,
        g_cfg.cache.string().c_str(), g_cfg.offline ? " (offline)" : "");
    say("Map data (c) OpenStreetMap contributors, ODbL. Elevation: AWS Terrain Tiles (Mapzen/Terrarium).");
    while (!g_stop) {
        const geonet::Socket c = geonet::acceptOne(g_listener);
        if (c < 0) {
            if (g_stop) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (g_live.load() >= 96) {
            geonet::respond(c, 503, "application/json", R"({"error":"busy"})");
            geonet::closeSocket(c);
            continue;
        }
        ++g_live;
        std::thread(serve, c).detach();
    }
    say("fitzelgeo: stopped");
    return 0;
}
