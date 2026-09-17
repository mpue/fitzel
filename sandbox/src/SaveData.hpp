#pragma once

// A game's own saves: what a player earned, bought and unlocked, kept between
// sessions. One JSON file per slot, per game, in the user's data folder --
// never beside the executable, which an installed game may not write to, and
// never in the project, which is the author's and not the player's.
//
//   Windows   %APPDATA%\fitzel\saves\<game>\<slot>.json
//   elsewhere $HOME/.local/share/fitzel/saves/<game>/<slot>.json
//
// Lua reaches it as game.saveData(slot, value) / game.loadData(slot).

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace savedata {

// Letters, digits, '-' and '_' only: a slot or game name is a file name, and
// one taken from a script must not be able to climb out of the folder.
std::string safeName(const std::string& name);

std::filesystem::path folderFor(const std::string& game);

// Written to a temporary file and then renamed over the old one: a crash or a
// full disk halfway through leaves the previous save, not half of a new one.
bool write(const std::string& game, const std::string& slot, const nlohmann::json& value);

// nullopt when there is no save yet, or it cannot be read as JSON.
std::optional<nlohmann::json> read(const std::string& game, const std::string& slot);

} // namespace savedata
