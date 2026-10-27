#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// The editor's line to a language model: Ollama's HTTP API on this machine or
// the LAN (http://127.0.0.1:11434 by default). The code editors' assistance
// (CodeAssist.hpp) and the Assistant window (AssistantPanel.hpp) both ask
// through here.
//
// The game has a client of its own -- ScriptLlm.hpp, the Lua `llm` table --
// which ships in the player and in a dozen check tools. This one is the
// editor's: it streams, it cancels, and it never ships.
//
// Nothing here blocks the frame. Every request runs on a thread of its own and
// fills a Reply that the UI reads each frame -- the text so far while it
// streams, then done. Cancelling cuts the connection; Ollama stops generating
// when the socket goes. Editor only.
namespace ollama {

struct Settings {
    std::string url = "http://127.0.0.1:11434";
    std::string chatModel;   // the Assistant window's ("" = the first installed)
    std::string codeModel;   // completions and edits in the editors ("" = chatModel)
    // num_ctx, sent with EVERY request: Ollama reloads a model whenever the
    // context size asked for changes, so the window and the editors must not
    // take turns asking for different ones.
    int   contextSize    = 32768;
    float temperature    = 0.2f;
    bool  inlineAuto     = true;    // suggest code as soon as typing pauses
    float inlineDelay    = 0.45f;   // ...this many seconds of it
    int   inlineMaxLines = 12;
    // The Assistant asks before each change to the scene or a file.
    bool  confirmChanges = false;
};

// One request's answer, filled by its thread and read by the UI. Read it under
// `lock` (or through the copies below).
struct Reply {
    mutable std::mutex lock;
    std::string    content;     // the text so far (streamed)
    std::string    thinking;    // a thinking model's reasoning, kept apart
    nlohmann::json toolCalls = nlohmann::json::array();
    nlohmann::json last;        // the last object received: the stats, or the whole answer
    std::string    error;
    int            status  = 0;
    bool           done    = false;
    bool           ok      = false;
    double         seconds = 0.0;   // when done: how long it took
    long long      tokens  = 0;     // eval_count
    std::atomic<bool> cancelled{false};
    // The connection, so cancel() can cut a blocked read (guarded by lock).
    std::intptr_t  sock = -1;

    std::string text() const    { std::lock_guard<std::mutex> g(lock); return content; }
    bool        finished() const { std::lock_guard<std::mutex> g(lock); return done; }
};
using ReplyPtr = std::shared_ptr<Reply>;

// How a model fills a hole between a prefix and a suffix (inline completion).
enum class Fim { Suffix, Qwen, CodeLlama, DeepSeek, StarCoder, Chat };

class Client {
public:
    Settings settings;

    ~Client();

    // ollama.json in the user's folder (UserDir.hpp).
    void load();
    void save() const;

    // POST /api/chat, streamed. `model` defaults to the chat model; options
    // get num_ctx and the temperature unless they are given.
    ReplyPtr chat(nlohmann::json body);
    // POST /api/generate, streamed ("model" defaults to the code model).
    ReplyPtr generate(nlohmann::json body);
    // A plain request whose whole answer lands in Reply::last.
    ReplyPtr request(const std::string& method, const std::string& path,
                     const nlohmann::json& body = nullptr);
    void cancel(const ReplyPtr& r);

    // The installed models (GET /api/tags), refreshed when asked -- and once,
    // the first time anybody looks. poll() picks the answer up.
    void refreshModels();
    const std::vector<std::string>& models();
    const std::string& modelsError() const { return m_modelsError; }
    bool modelsPending() const { return (bool)m_tags; }
    // What a model can do (POST /api/show: "completion", "tools", "insert",
    // "thinking"...), asked once per model. Empty while unknown.
    const std::vector<std::string>& capabilities(const std::string& model);
    Fim fimFor(const std::string& model);

    std::string chatModel();
    std::string codeModel();

    // Once a frame: answers to the model list and capability questions.
    void poll();

private:
    ReplyPtr start(const std::string& method, const std::string& path, std::string body,
                   bool streamed);
    void reap();

    std::mutex m_lock;
    std::vector<std::pair<std::thread, ReplyPtr>> m_threads;

    std::vector<std::string> m_models;
    std::string m_modelsError;
    ReplyPtr    m_tags;
    bool        m_askedModels = false;
    double      m_modelsAt = -1e9;   // when the list was last asked for
    std::map<std::string, std::vector<std::string>> m_caps;
    std::map<std::string, ReplyPtr> m_showing;
};

// The editor's one client.
Client& client();

// The text without a thinking model's <think>...</think>, trimmed.
std::string withoutThinking(std::string s);

// The code in the first ``` fence of a model's answer (the whole answer when
// it has none, or an unclosed one: still streaming).
std::string firstCodeBlock(const std::string& s);

} // namespace ollama
