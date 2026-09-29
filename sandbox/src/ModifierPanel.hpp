#pragma once

#include <vector>

#include "Modifiers.hpp"
#include "SceneTypes.hpp"

// The Modifiers component's card in the Inspector (Modifiers.hpp): the stack
// top to bottom -- each modifier on or off, moved up or down, applied or
// removed, its settings under it -- and "Add modifier" below, listing every
// registered kind. Editor only.
//
// Everything here edits the selected entity in place, inside the Inspector's
// own undo bracket, so each click (and each drag of a field) is one step back.
namespace modifierui {

// `e` carries `ms`; `entities` is the scene (Apply moves the object so that
// baking the stack into its mesh does not shift it).
void card(ModifierStackComponent& ms, Entity& e, std::vector<Entity>& entities);

} // namespace modifierui
