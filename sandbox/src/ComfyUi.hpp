#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// The image editor's line to ComfyUI: a picture made from a sentence, by a
// ComfyUI on this machine or on the LAN (http://127.0.0.1:8188 by default).
// The Image editor's "New image from prompt" window asks through here.
//
// What goes to the server is a workflow in ComfyUI's API format with the
// window's prompt, size, steps and seed written into it. The built-in one is
// ComfyUI's own "Text to Image (Z-Image-Turbo)" template; any other is a file
// saved with ComfyUI's File > Export (API) -- the places the values go are
// found by what the nodes are (the text that feeds the sampler's positive side,
// the empty latent's size, the sampler's seed and steps).
//
// Nothing here blocks the frame. A generation runs on a thread of its own and
// fills a Job the window reads each frame: what is happening (queued, loading,
// sampling step 5 of 8), the server's preview of the picture while it samples,
// then the pictures, decoded. The progress comes over ComfyUI's WebSocket;
// a server whose socket cannot be had is asked for its history instead.
// Cancelling takes the prompt off the server's queue, or interrupts it there.
// Editor only.
namespace comfy {

struct Settings {
    std::string url = "http://127.0.0.1:8188";
    std::vector<std::string> recent;   // other servers used before, newest first
    // Run when the server does not answer ("systemctl start comfyui", a script
    // ...); the generation then waits for it to come up. "" = say so and stop.
    std::string startCommand;
    std::string workflow;              // an exported .json; "" = the built-in Z-Image Turbo
    // The window's last choices.
    std::string prompt;
    int       width = 1024, height = 1024;
    int       steps = 0;               // 0 = what the workflow says
    int       count = 1;
    bool      randomSeed = true;
    long long seed = 0;
};

// Where in a workflow one of the request's values goes: node id, input name.
struct Slot {
    std::string node, input;
};

struct Workflow {
    std::string    name;
    nlohmann::json graph;   // API format: {"id": {"class_type": ..., "inputs": {...}}}
    std::vector<Slot> prompt, negative, width, height, seed, steps, count;
    int defWidth = 1024, defHeight = 1024, defSteps = 0;
};

// ComfyUI's "Text to Image (Z-Image-Turbo)": 8 steps, CFG 1, res_multistep.
Workflow builtinWorkflow();
// A workflow from ComfyUI's File > Export (API), or one with its slots named
// ({"graph": ..., "set": {"prompt": ["5", "text"], ...}}, as the Leitstand's).
// False and why when it is neither -- the UI's own format (nodes and links)
// says so, and so does a graph without a prompt to fill.
bool parseWorkflow(const nlohmann::json& j, Workflow& out, std::string& err);
bool loadWorkflow(const std::string& path, Workflow& out, std::string& err);

struct Request {
    std::string prompt, negative;
    int       width = 1024, height = 1024;
    int       steps = 0;               // 0 = leave the workflow's
    int       count = 1;               // pictures in one go (the latent's batch)
    long long seed = 0;
};
// The workflow's graph with the request written into it. Sizes are rounded to
// what latents take (multiples of 16).
nlohmann::json fill(const Workflow& wf, const Request& r);
// A random seed, as ComfyUI's "randomize" would pick one.
long long randomSeed();

struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;      // RGBA8, top row first
    std::string file;                  // its name in the server's output folder
};

// One generation (or one look at the server), filled by its thread and read by
// the UI under `lock`.
struct Job {
    mutable std::mutex lock;
    std::string stage;                 // what is happening, in words
    int step = 0, steps = 0;           // the sampler's progress
    Image preview;                     // while sampling, when the server sends previews
    std::uint32_t previewVersion = 0;
    std::vector<Image> images;
    long long seed = 0;
    std::string info;                  // a look at the server: what it is and what it lacks
    std::string error;
    bool   done = false, ok = false;
    double seconds = 0.0;              // when done: how long it took
    std::atomic<bool> cancelled{false};

    bool finished() const { std::lock_guard<std::mutex> g(lock); return done; }
};
using JobPtr = std::shared_ptr<Job>;

class Client {
public:
    Settings settings;
    bool persist = true;               // false: the check harnesses leave the user's file alone

    ~Client();

    // comfyui.json in the user's folder (UserDir.hpp).
    void load();
    void save() const;
    // Puts the current server first in `recent`.
    void rememberServer();

    // Queues the workflow with the request in it and brings the pictures back.
    JobPtr generate(const Workflow& wf, const Request& r);
    // Asks the server what it is (version, graphics card) and whether it has
    // the files the workflow loads. The answer lands in Job::info.
    JobPtr probe(const Workflow& wf);
    void cancel(const JobPtr& j);

private:
    JobPtr start(std::function<void(Job&)> work);
    void reap();

    std::mutex m_lock;
    std::vector<std::pair<std::thread, JobPtr>> m_threads;
};

// The editor's one client (its settings loaded the first time it is asked for).
Client& client();

} // namespace comfy
