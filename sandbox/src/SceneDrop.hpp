#pragma once

#include <fitzel/asset/AssetId.hpp>

struct EditorContext;
struct ViewportFrame;

// An asset dragged from the Assets browser (or a material from the Materials
// panel) and released over the scene: a model lands on the ground under the
// cursor, a material dresses the face or the object under it, a texture becomes
// a fresh material on the object under it. Every change is one undo step.
// Editor only.
namespace scenedrop {

// Call with the dropped asset's id, while the scene image's drop target is open.
void dropOnScene(EditorContext& ed, const ViewportFrame& view, const fitzel::AssetId& gid);

} // namespace scenedrop
