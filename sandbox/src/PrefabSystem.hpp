#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "ProjectIO.hpp"
#include "SceneTypes.hpp"

// Prefabs: a reusable object template, like Unity's. A prefab is an entity
// subtree (a root plus its descendants, with their components and asset-GUID
// references) saved to a self-contained <project>/prefabs/<name>-<guid8>.fprefab
// file, then instantiated into a scene any number of times.
//
// Entities inside a prefab use LOCAL ids (0..n, root = 0), so the file is
// portable; instantiation remaps them onto fresh scene ids. Every instantiated
// entity is tagged with a PrefabComponent (source GUID + local id) so an instance
// remembers where it came from -- the hook a future "linked instances / apply
// overrides" step builds on. For now an instance is a plain, freely editable copy.
//
// Scope: entities + components only. Roads and vegetation live outside the
// Document/Entity model and are not captured by a prefab.
namespace prefab {

// A prefab in memory. `entities` hold LOCAL ids: exactly one root (parent -1,
// local id 0) followed by its descendants in breadth-first order (parents precede
// children, ready for AddEntitiesCmd). `path` is its .fprefab file, "" if unsaved.
struct Prefab {
    fitzel::AssetId     guid;
    std::string         name;
    std::string         path;
    std::vector<Entity> entities;
};

// The prefabs folder for an open project (the folder that holds <name>.fitzel).
std::string prefabsDirIn(const std::string& projectFolder);

// Build a prefab from the scene subtree rooted at `rootId`: collect the root and
// every descendant, deep-copy them, remap their ids to local (root = 0), and drop
// any stale PrefabComponent. Child local transforms are preserved; the root's is
// replaced at instantiation. Returns nullopt if `rootId` is missing or is the Sun.
std::optional<Prefab> fromSubtree(const std::vector<Entity>& scene, int rootId,
                                  const std::string& name);

// Write `p` to <dir>/<safeName>-<guid8>.fprefab (creating `dir`). Mints a GUID if
// `p.guid` is invalid. On success sets `p.path`/`p.guid` and returns true.
bool save(const projectio::Context& ctx, Prefab& p, const std::string& dir);

// Read a .fprefab file. Recreates components and re-imports models via `ctx`
// (exactly like loading a scene). Returns nullopt if the file can't be read/parsed.
std::optional<Prefab> load(projectio::Context& ctx, const std::string& path);

// Every .fprefab in `dir` as (display name, full path), sorted by name.
std::vector<std::pair<std::string, std::string>> list(const std::string& dir);

// Instantiate `p` into a scene: deep-copy its entities, mint fresh ids from
// `entityCounter`, re-root the copy at world position `pos` (adding `yawDeg` to
// the root's yaw), and tag every entity with a PrefabComponent(source, localId).
// Returns the new entities in parent-before-child order (ready for AddEntitiesCmd);
// the root is entities.front(). Does not touch the scene itself.
std::vector<Entity> instantiate(const Prefab& p, int& entityCounter,
                                const glm::vec3& pos, float yawDeg);

} // namespace prefab
