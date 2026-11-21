#include "GeoClient.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include <stb_image.h>

#include "GeoNet.hpp"
#include "UserDir.hpp"

namespace geoclient {

namespace {
double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

Client::Client(int workers) {
    geonet::init();
    for (int i = 0; i < std::max(1, workers); ++i) m_threads.emplace_back([this] { worker(); });
}

Client::~Client() {
    {
        std::lock_guard<std::mutex> lock(m_m);
        m_quit = true;
    }
    m_cv.notify_all();
    for (std::thread& t : m_threads) t.join();
}

void Client::setServer(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(m_m);
    if (host == m_host && port == m_port) return;
    m_host = host;
    m_port = port;
    m_server = Server::Unknown;
    for (auto it = m_slots.begin(); it != m_slots.end();)
        it = it->second.state == State::Failed ? m_slots.erase(it) : std::next(it);
}

std::string Client::host() const { std::lock_guard<std::mutex> lock(m_m); return m_host; }
int Client::port() const { std::lock_guard<std::mutex> lock(m_m); return m_port; }

std::string Client::serverNote() const { std::lock_guard<std::mutex> lock(m_m); return m_note; }

int Client::pending() const {
    std::lock_guard<std::mutex> lock(m_m);
    int n = 0;
    for (const auto& [k, s] : m_slots) n += s.state == State::Queued || s.state == State::Loading;
    return n;
}

void Client::retryServer() {
    {
        std::lock_guard<std::mutex> lock(m_startM);
        m_lastStart = -1e9;
    }
    m_server = Server::Unknown;
    std::lock_guard<std::mutex> lock(m_m);
    for (auto it = m_slots.begin(); it != m_slots.end();)
        it = it->second.state == State::Failed ? m_slots.erase(it) : std::next(it);
}

Client::Result Client::want(const std::string& path, int priority) {
    std::lock_guard<std::mutex> lock(m_m);
    auto it = m_slots.find(path);
    const double t = now();
    if (it == m_slots.end() || (it->second.state == State::Failed && t - it->second.done > 20.0 &&
                                m_server.load() != Server::Down)) {
        Slot s;
        s.priority = priority;
        s.asked = t;
        s.order = ++m_order;
        it = m_slots.insert_or_assign(path, s).first;
        m_cv.notify_one();
    } else {
        it->second.asked = t;
        if (it->second.state == State::Queued && priority > it->second.priority) it->second.priority = priority;
    }
    Result r;
    r.state = it->second.state;
    r.body = it->second.body;
    r.error = it->second.error;
    r.cached = it->second.cached;
    return r;
}

void Client::dropQueuedBelow(int priority) {
    std::lock_guard<std::mutex> lock(m_m);
    for (auto it = m_slots.begin(); it != m_slots.end();)
        it = it->second.state == State::Queued && it->second.priority < priority ? m_slots.erase(it) : std::next(it);
}

void Client::trim(std::size_t keep) {
    std::lock_guard<std::mutex> lock(m_m);
    if (m_slots.size() <= keep) return;
    std::vector<std::pair<double, std::string>> done;
    for (const auto& [k, s] : m_slots)
        if (s.state == State::Ready || s.state == State::Failed) done.emplace_back(s.asked, k);
    std::sort(done.begin(), done.end());
    const std::size_t drop = std::min(done.size(), m_slots.size() - keep);
    for (std::size_t i = 0; i < drop; ++i) m_slots.erase(done[i].second);
}

bool Client::ensureServer() {
    std::string host;
    int port = 0;
    {
        std::lock_guard<std::mutex> lock(m_m);
        host = m_host;
        port = m_port;
    }
    geonet::Response r;
    if (geonet::get(host, port, "/status", 3, r) && r.status == 200 && r.body.find("fitzelgeo") != std::string::npos) {
        m_server = Server::Up;
        return true;
    }
    // Only a server on this machine is ours to start.
    if (host != "127.0.0.1" && host != "localhost") {
        m_server = Server::Down;
        std::lock_guard<std::mutex> lock(m_m);
        m_note = "no fitzelgeo answers at " + host + ":" + std::to_string(port);
        return false;
    }
    std::lock_guard<std::mutex> start(m_startM);
    // Another worker may have started it while this one waited.
    if (geonet::get(host, port, "/status", 2, r) && r.status == 200) { m_server = Server::Up; return true; }
    if (now() - m_lastStart < 30.0) {
        if (m_server.load() != Server::Up) m_server = Server::Down;
        return false;
    }
    m_lastStart = now();
    m_server = Server::Starting;
    namespace fs = std::filesystem;
    std::error_code ec;
#ifdef _WIN32
    const char* exeName = "fitzelgeo.exe";
#else
    const char* exeName = "fitzelgeo";
#endif
    fs::path exe = fs::current_path(ec) / exeName;
    const std::string prog = fs::exists(exe, ec) ? exe.string() : std::string(exeName);
    const fs::path log = userdir::root() / "geocache" / "fitzelgeo.log";
    fs::create_directories(log.parent_path(), ec);
    const bool spawned = geonet::spawn({prog, "--port", std::to_string(port)}, log.string());
    for (int i = 0; spawned && i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (geonet::get(host, port, "/status", 2, r) && r.status == 200) {
            m_server = Server::Up;
            std::lock_guard<std::mutex> lock(m_m);
            m_note = "started fitzelgeo (" + prog + ")";
            return true;
        }
    }
    m_server = Server::Down;
    std::lock_guard<std::mutex> lock(m_m);
    m_note = spawned ? "fitzelgeo started but does not answer (see " + log.string() + ")"
                     : "fitzelgeo is not running and could not be started (" + prog + ")";
    return false;
}

void Client::worker() {
    for (;;) {
        std::string path, host;
        int port = 0;
        {
            std::unique_lock<std::mutex> lock(m_m);
            m_cv.wait(lock, [&] {
                if (m_quit) return true;
                for (const auto& [k, s] : m_slots)
                    if (s.state == State::Queued) return true;
                return false;
            });
            if (m_quit) return;
            // The most important first, the most recently asked among equals
            // (the map's tiles under the eye before the ones scrolled away).
            auto best = m_slots.end();
            for (auto it = m_slots.begin(); it != m_slots.end(); ++it) {
                if (it->second.state != State::Queued) continue;
                if (best == m_slots.end() || it->second.priority > best->second.priority ||
                    (it->second.priority == best->second.priority && it->second.asked > best->second.asked))
                    best = it;
            }
            if (best == m_slots.end()) continue;
            best->second.state = State::Loading;
            path = best->first;
            host = m_host;
            port = m_port;
        }
        geonet::Response r;
        std::string err;
        bool ok = false;
        if (m_server.load() != Server::Up && !ensureServer()) {
            err = serverNote();
        } else {
            // Geometry can take a while: Overpass answers a city's metatile in
            // tens of seconds when it is busy.
            const int timeout = path.rfind("/osm/", 0) == 0 ? 600 : 120;
            ok = geonet::get(host, port, path, timeout, r, &err);
            if (!ok && r.status == 0) {
                m_server = Server::Unknown;   // gone? the next request looks again
            }
            if (ok && r.status != 200) {
                ok = false;
                err = "server: " + std::to_string(r.status) + " " + r.body.substr(0, 200);
            }
        }
        std::lock_guard<std::mutex> lock(m_m);
        auto it = m_slots.find(path);
        if (it == m_slots.end()) continue;
        it->second.done = now();
        if (ok) {
            it->second.state = State::Ready;
            it->second.body = std::make_shared<const std::string>(std::move(r.body));
            const auto h = r.headers.find("x-cache");
            it->second.cached = h != r.headers.end() && h->second == "hit";
        } else {
            it->second.state = State::Failed;
            it->second.error = err;
        }
    }
}

// --- A square ----------------------------------------------------------------------------

bool decodeDem(const std::string& png, geo::DemTile& out) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(png.data()),
                                              static_cast<int>(png.size()), &w, &h, &n, 3);
    if (!px) return false;
    out.w = w;
    out.h = h;
    out.m.resize(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < out.m.size(); ++i)
        out.m[i] = geo::terrarium(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]);
    stbi_image_free(px);
    return true;
}

void AreaLoad::start(geo::LonLat c, float size) {
    cancel();
    m_center = c;
    m_size = size;
    m_need = geoplan::tilesFor(c, size);
    m_osm.assign(m_need.osm.size(), nullptr);
    m_dem.assign(m_need.dem.size(), nullptr);
    m_coarse.assign(m_need.coarse.size(), nullptr);
    m_osmState.assign(m_need.osm.size(), 0);
    m_total = static_cast<int>(m_need.osm.size() + m_need.dem.size() + m_need.coarse.size());
    m_have = m_failed = 0;
    m_active = true;
    m_done = false;
    m_assembling = false;
    m_status = "Asking fitzelgeo for " + std::to_string(m_total) + " tiles...";
}

void AreaLoad::cancel() {
    if (m_assembling && m_job.valid()) m_job.wait();
    m_assembling = false;
    m_active = false;
}

float AreaLoad::progress() const {
    if (!m_active && m_done) return 1.0f;
    if (m_total <= 0) return 0.0f;
    const float f = static_cast<float>(m_have + m_failed) / static_cast<float>(m_total);
    return m_assembling ? 0.95f : 0.9f * f;
}

void AreaLoad::poll(Client& client) {
    if (!m_active) return;
    if (m_assembling) {
        if (m_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto a = m_job.get();
        m_assembling = false;
        m_active = false;
        m_done = true;
        m_area = a;
        ++m_rev;
        if (a) {
            int buildings = 0;
            for (const geo::Feature& f : a->features) buildings += f.has("building");
            m_status = std::to_string(a->features.size()) + " map features (" + std::to_string(buildings) +
                       " buildings), " + std::to_string(a->dem.size()) + " elevation tiles";
            if (a->missing > 0) m_status += " -- " + std::to_string(a->missing) + " tiles missing";
        } else {
            m_status = "Nothing could be had: " + m_status;
        }
        return;
    }
    int have = 0, failed = 0, cachedN = 0;
    std::string lastError;
    auto ask = [&](const std::vector<geo::TileId>& tiles, std::vector<std::shared_ptr<const std::string>>& got,
                   const char* kind, int prio, std::vector<int>* state) {
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (got[i]) { ++have; if (state) (*state)[i] = 1; continue; }
            const geo::TileId& t = tiles[i];
            const std::string path = std::string(kind) == "osm"
                ? "/osm/" + std::to_string(t.x) + "/" + std::to_string(t.y) + ".json"
                : "/dem/" + std::to_string(t.z) + "/" + std::to_string(t.x) + "/" + std::to_string(t.y) + ".png";
            const Client::Result r = client.want(path, prio);
            if (r.state == Client::State::Ready) {
                got[i] = r.body;
                ++have;
                cachedN += r.cached;
                if (state) (*state)[i] = 1;
            } else if (r.state == Client::State::Failed) {
                ++failed;
                lastError = r.error;
                if (state) (*state)[i] = 2;
            }
        }
    };
    ask(m_need.osm, m_osm, "osm", 100, &m_osmState);
    ask(m_need.dem, m_dem, "dem", 99, nullptr);
    ask(m_need.coarse, m_coarse, "dem", 98, nullptr);
    m_have = have;
    m_failed = failed;
    if (have + failed < m_total) {
        m_status = "Streaming the square: " + std::to_string(have) + " of " + std::to_string(m_total) + " tiles";
        if (failed) m_status += ", " + std::to_string(failed) + " failed (" + lastError + ")";
        if (client.server() == Client::Server::Starting) m_status = "Starting fitzelgeo, the local map cache...";
        return;
    }
    if (have == 0) {
        m_active = false;
        m_done = true;
        m_area.reset();
        ++m_rev;
        m_status = "Nothing came: " + lastError;
        return;
    }
    // All there (or given up on): assemble on a thread.
    m_status = "Reading the map...";
    m_assembling = true;
    const geo::LonLat c = m_center;
    const float size = m_size;
    const geoplan::Need need = m_need;
    auto osm = m_osm;
    auto dem = m_dem;
    auto coarse = m_coarse;
    m_job = std::async(std::launch::async, [c, size, need, osm, dem, coarse]() -> std::shared_ptr<geoplan::AreaData> {
        auto a = std::make_shared<geoplan::AreaData>();
        a->center = c;
        a->size = size;
        a->tiles = static_cast<int>(need.osm.size());
        for (std::size_t i = 0; i < osm.size(); ++i) {
            std::vector<geo::Feature> fs;
            if (!osm[i] || !geo::parseTile(*osm[i], fs)) { ++a->missing; continue; }
            geoplan::addTile(*a, std::move(fs));
        }
        auto decodeAll = [](const std::vector<geo::TileId>& ids, const std::vector<std::shared_ptr<const std::string>>& bodies,
                            std::vector<geo::DemTile>& out) {
            for (std::size_t i = 0; i < ids.size(); ++i) {
                geo::DemTile t;
                t.id = ids[i];
                if (bodies[i] && decodeDem(*bodies[i], t)) out.push_back(std::move(t));
            }
        };
        decodeAll(need.dem, dem, a->dem);
        decodeAll(need.coarse, coarse, a->demCoarse);
        return a;
    });
}

} // namespace geoclient
