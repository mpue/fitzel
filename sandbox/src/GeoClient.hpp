#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "GeoPlan.hpp"
#include "GeoTiles.hpp"

// The editor's side of fitzelgeo, the local geometry cache (tools/fitzelgeo.cpp):
// requests queued by priority and answered on worker threads, so the map on the
// world generator's panel fills in tile by tile while the editor keeps drawing,
// and a square's geometry streams in behind it. If nothing answers at the
// server's address, the client starts fitzelgeo from the editor's own folder
// (once; it keeps running for the next session's cache hits).
namespace geoclient {

class Client {
public:
    explicit Client(int workers = 6);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void setServer(const std::string& host, int port);
    std::string host() const;
    int  port() const;

    enum class State { Queued, Loading, Ready, Failed };
    struct Result {
        State state = State::Queued;
        std::shared_ptr<const std::string> body;
        std::string error;
        bool cached = false;   // the server had it already
    };
    // Ask for `path` ("/map/12/2154/1370.png"). Never blocks: the first ask
    // queues it, later asks report how far it got. Higher priorities first. A
    // failed one is asked again after a while.
    Result want(const std::string& path, int priority = 0);
    // Forget what is still queued below `priority` (the map moved on).
    void dropQueuedBelow(int priority);
    // Keep at most `keep` finished answers (the least recently asked go).
    void trim(std::size_t keep = 1500);

    enum class Server { Unknown, Up, Starting, Down };
    Server server() const { return m_server.load(); }
    std::string serverNote() const;
    int  pending() const;
    // After Down: try (and start it) again on the next request.
    void retryServer();

private:
    struct Slot {
        State state = State::Queued;
        std::shared_ptr<const std::string> body;
        std::string error;
        bool   cached = false;
        int    priority = 0;
        double asked = 0.0, done = 0.0;
        std::uint64_t order = 0;
    };
    void worker();
    bool ensureServer();   // false: nothing there and it would not start

    mutable std::mutex m_m;
    std::condition_variable m_cv;
    std::map<std::string, Slot> m_slots;
    std::vector<std::thread> m_threads;
    bool m_quit = false;
    std::uint64_t m_order = 0;
    std::string m_host = "127.0.0.1";
    int m_port = geo::kDefaultPort;
    std::atomic<Server> m_server{Server::Unknown};
    std::mutex m_startM;
    double m_lastStart = -1e9;
    std::string m_note;
};

// A square of the map, fetched: every geometry tile and elevation tile it needs,
// then parsed and decoded (on a thread) into the AreaData the planner reads.
class AreaLoad {
public:
    void start(geo::LonLat c, float size);
    void cancel();
    // Once a frame: asks for what is missing, and assembles when all is there.
    void poll(Client& client);

    bool  active() const { return m_active; }
    bool  done() const { return m_done; }
    float progress() const;
    std::string status() const { return m_status; }
    geo::LonLat center() const { return m_center; }
    float size() const { return m_size; }
    // The tiles of the square and how they stand (for the map's overlay):
    // 0 waiting, 1 there, 2 failed.
    const std::vector<geo::TileId>& osmTiles() const { return m_need.osm; }
    int osmState(std::size_t i) const { return i < m_osmState.size() ? m_osmState[i] : 0; }
    // The result, once done (null if nothing came at all).
    std::shared_ptr<const geoplan::AreaData> area() const { return m_area; }
    int revision() const { return m_rev; }

private:
    geoplan::Need m_need;
    geo::LonLat m_center;
    float m_size = 0.0f;
    bool  m_active = false, m_done = false, m_assembling = false;
    int   m_have = 0, m_total = 0, m_failed = 0, m_rev = 0;
    std::vector<int> m_osmState;
    std::vector<std::shared_ptr<const std::string>> m_osm, m_dem, m_coarse;
    std::future<std::shared_ptr<geoplan::AreaData>> m_job;
    std::shared_ptr<const geoplan::AreaData> m_area;
    std::string m_status;
};

// Decode a Terrarium PNG into heights (false if it is not one).
bool decodeDem(const std::string& png, geo::DemTile& out);

} // namespace geoclient
