#pragma once

struct lua_State;

// The Lua `llm` and `json` tables: a language model on this machine (Ollama's
// HTTP API, by default http://127.0.0.1:11434) for game characters that think
// for themselves.
//
// A model takes seconds to answer, a frame has milliseconds -- so nothing here
// blocks. llm.chat() queues a request and returns its id at once; one worker
// thread posts the requests in order (a local model answers one at a time
// anyway), and llm.poll() hands the script whatever has come back since. Like
// `net`, it is polled from the script's own update: nothing reaches the VM
// behind the frame's back, and stopping Play (reset) drops the queue and every
// answer still on its way.
namespace scriptllm {

// Register the globals `llm` and `json` in a fresh VM.
void install(lua_State* L);
// Forget every queued request and every answer not yet polled (Play stopped,
// or a new VM). A request already being answered finishes, but its answer is
// thrown away.
void reset();

} // namespace scriptllm
