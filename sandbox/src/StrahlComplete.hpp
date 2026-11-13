#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// What the Strahl window offers while a script is typed: the names that fit
// where the caret is, best first, and the signature of the call it stands in.
//
// It reads the text the way the language does, but forgives everything: a
// script being typed is unfinished by definition, so there is no parse to lean
// on. A bracket-aware scan up to the caret says where it is -- in a sphere's
// block, after `material:`, in the third argument of marble(), at the start of
// a statement -- and that decides what is offered first: a sphere's own
// properties in its block, materials after `material:`, the parameter names in
// a call. The names come from the language's own tables (the kinds and what
// they accept, the built-ins), the prelude's source with its comments as the
// documentation, the files the script imports, and the script's own lets, fns,
// parameters and loop names.
//
// No ImGui: the window draws (StrahlPanel), this decides, and a test can ask
// what it would offer.
namespace strahl::complete {

enum class Kind { Keyword, Object, Property, Function, Value, Parameter, File, Snippet };

struct Item {
    std::string label;        // shown, and what the typed word is matched against
    std::string insert;       // replaces the typed word
    int         caret = -1;   // where in `insert` the caret ends up (-1: after it)
    std::string detail;       // greyed beside the label: a signature, a value
    std::string doc;          // a sentence or two, shown for the highlighted item
    Kind        kind = Kind::Value;
    int         prio = 0;     // how well it fits HERE: orders equally good matches
};

struct Signature {
    std::string label;                        // marble(base = #ebe6dd, vein = ...)
    std::vector<std::pair<int, int>> params;  // each parameter's byte range in label
    int         active = -1;                  // the one the caret is in
    std::string doc;
};

struct Request {
    std::string           text;        // the whole script
    std::size_t           caret = 0;   // byte offset into text
    std::filesystem::path file;        // the script's file ("" unsaved): imports beside it
    std::vector<std::filesystem::path> libraryDirs;   // ...and then here
    bool                  manual = false;   // asked for (Ctrl+Space): offer without a typed word
};

struct Result {
    std::vector<Item> items;      // best first; empty: nothing to offer here
    std::size_t       start = 0;  // the typed word is [start, caret): what an item replaces
    std::string       prefix;     // ...and that word
    bool              inString = false;   // offering file names inside an import's quotes
    bool              hasSignature = false;
    Signature         signature;
};

Result complete(const Request& r);

// The documentation completion shows for a name of the language itself -- a
// kind, a property, a built-in or a prelude definition -- or "" when it has
// none. For a test: every name the interpreter knows should have some.
std::string documentation(const std::string& name);

} // namespace strahl::complete
