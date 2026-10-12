#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A project's C# side, as Unity lays it out: a solution Visual Studio opens,
// one project that compiles every .cs under scripts/ into one assembly, and the
// engine's API to compile against.
//
//   <project>/<Name>.sln
//   <project>/Assembly-CSharp.csproj      scripts/**/*.cs -> Assembly-CSharp.dll
//   <project>/Directory.Build.props       build clutter into Library/, not the root
//   <project>/Library/Fitzel.dll + .xml   the API (copied from <exe>/dotnet)
//   <project>/Library/ScriptAssemblies/   the build's output, read at Play
//   <project>/scripts/*.cs                the scripts
//
// The editor builds the csproj itself (`dotnet build`, in the background) when a
// .cs changes, so the edit-save-Play loop does not need Visual Studio running;
// building in Visual Studio writes the same file, and either is picked up.
// Editor only.
namespace csproject {

// Is this project folder set up for C# (has the .csproj)?
bool isCSharp(const std::string& projectDir);

// The .NET SDK's `dotnet`, empty when there is none.
std::filesystem::path dotnetPath();

// Write the solution, project and props, copy the API in, and -- when scripts/
// has no .cs yet -- a first script to start from. Leaves files that exist
// alone (the solution may have been edited in Visual Studio since). False +
// `err` when a file could not be written.
bool create(const std::string& projectDir, const std::string& name, std::string& err);

// Keep Library/Fitzel.dll the engine's own: copied when the engine's differs
// (a newer editor), so the scripts compile against the API they will meet.
void syncApi(const std::string& projectDir);

// The new-script template: a Behaviour named `className`.
std::string scriptTemplate(const std::string& className);

// Open the solution in Visual Studio (or whatever owns .sln here); with `file`,
// that file in the running Visual Studio. False when nothing would open it.
bool openInIde(const std::string& projectDir, const std::string& file = {});

// --- Background build ---------------------------------------------------------------
struct BuildMessage {
    std::string file;    // as dotnet printed it (absolute)
    int         line = 0;
    bool        error = true;
    std::string text;    // "CS0103: The name 'x' does not exist ..."
};

class Builder {
public:
    ~Builder();

    // Every frame or so: when the project's .cs files (or the .csproj) differ
    // from what the last build saw, start a build. Cheap between changes (one
    // directory walk a second).
    void poll(const std::string& projectDir);
    // Build now, changed or not (the panel's Rebuild).
    void start(const std::string& projectDir);
    // Play is starting: a running build is waited for, so Play gets its result.
    void wait();

    bool running() const { return m_running.load(); }
    // Results of the last finished build.
    bool                      ok() const;
    std::vector<BuildMessage> messages() const;
    std::string               log() const;
    double                    finishedAt() const;   // glfw-free wall clock, seconds
    int                       generation() const { return m_generation.load(); }

private:
    void run(std::string projectDir, std::uint64_t stamp);

    std::thread              m_thread;
    std::atomic<bool>        m_running{false};
    std::atomic<int>         m_generation{0};
    mutable std::mutex       m_lock;
    bool                     m_ok = false;
    std::vector<BuildMessage> m_messages;
    std::string              m_log;
    double                   m_finishedAt = 0.0;
    std::uint64_t            m_builtStamp = 0;   // the sources the last build saw
    std::string              m_dir;              // the project it was for
    double                   m_lastPoll = -1e9;
};

// The "C# Scripts" window: build state, errors (click: open there), Rebuild and
// Open in Visual Studio. `runtimeProblem` is CSharpScripts::runtimeProblem().
// `openFile` opens an error's file at its line (the C# editor); unset, they
// open in Visual Studio.
void drawPanel(bool& open, Builder& b, const std::string& projectDir,
               const std::string& runtimeProblem,
               const std::function<void(const std::string& file, int line)>& openFile = {});

} // namespace csproject
