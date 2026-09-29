#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Component.hpp"   // ComponentBase, MeshComponent, Property
#include "EditMesh.hpp"
#include "SceneTypes.hpp"  // Entity

// --- The modifier stack ----------------------------------------------------------
// Blender's idea, for the meshes modelled here (MeshComponent): an object keeps
// the mesh it was modelled as, and a list of operations -- subdivide it, thin
// it out, turn its edges into struts, copy it in a row -- that is run over that
// mesh, top to bottom, every time it changes. What is drawn, collided with and
// walked on is the result; what is edited stays the plain mesh underneath, so
// a smooth subdivided surface is shaped by moving the few corners of its cage.
// Nothing is destroyed until a modifier is applied, which bakes it (and every
// one above it) into the mesh and takes it off the list.
//
// Open by construction, like the components: every kind registers itself (a
// class deriving from Modifier, its settings as Property metadata -- which gives
// it its inspector fields and its place in the scene file for free -- and one
// line in the registrar at the bottom of Modifiers.cpp). The stack, the
// inspector card and the file format know nothing about any particular kind.
namespace modifiers {

// One step of the stack.
class Modifier {
public:
    virtual ~Modifier() = default;
    virtual std::unique_ptr<Modifier> clone() const = 0;
    virtual const char* typeId() const = 0;        // stable id (the scene file)
    virtual const char* displayName() const = 0;   // the inspector's name for it
    // Its settings: the fields the card draws and the keys the file keeps.
    virtual const std::vector<Property>& props() const = 0;
    // Change `m` -- the result of every modifier above this one -- in place.
    virtual void apply(EditMesh& m) const = 0;

    bool enabled = true;   // off: skipped, settings and place in the list kept
};

// The boilerplate half of a kind: T only declares its settings and apply().
template <class T>
class ModifierOf : public Modifier {
public:
    std::unique_ptr<Modifier> clone() const override {
        return std::make_unique<T>(static_cast<const T&>(*this));
    }
};

struct TypeInfo {
    std::string typeId;
    std::string displayName;
    std::string tip;   // what it does, one or two lines for the Add menu
    std::function<std::unique_ptr<Modifier>()> make;
};
void registerType(TypeInfo info);
const std::vector<TypeInfo>& registry();
// A fresh modifier of this kind with its default settings, or null.
std::unique_ptr<Modifier> make(const std::string& typeId);

} // namespace modifiers

// The component that carries an object's stack. Only does anything on an
// object with a MeshComponent (a modelled mesh); anything else it waits on.
class ModifierStackComponent : public ComponentBase {
public:
    std::vector<std::unique_ptr<modifiers::Modifier>> stack;   // top first
    // Shading of the result. On by default, because the first thing a stack
    // usually does is make a surface curved: a subdivided cube drawn with a
    // normal per face is a golf ball. The angle keeps a real edge (a box's
    // ninety degrees, the corner of a strut) sharp.
    bool  smooth      = true;
    float smoothAngle = 35.0f;   // degrees

    ModifierStackComponent() = default;
    ModifierStackComponent(const ModifierStackComponent& o) { *this = o; }
    ModifierStackComponent& operator=(const ModifierStackComponent& o);

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<ModifierStackComponent>(*this);
    }
    const char* typeId() const override { return "modifiers"; }
    const char* displayName() const override { return "Modifiers"; }
    // Bespoke card (ModifierPanel.hpp); the list is not a property list.
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;
    }
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
};

namespace modifiers {

// `base` after the first `count` modifiers of the stack (all of them when
// negative), the disabled ones skipped. Geometry only: no frame, no shading.
EditMesh evaluate(const EditMesh& base, const ModifierStackComponent& ms, int count = -1);

// The mesh an object shows: its own, or what its stack makes of it -- in its
// own mesh's space (EditMesh::hasFrame), shaded as the stack says, with a
// revision of its own for the GPU cache. Evaluated only when the mesh or the
// stack changed since last asked; `key` names the object in that cache (an
// entity id, or any id a caller keeps unique, as the prefab flatteners do).
struct Shown {
    const EditMesh* mesh = nullptr;
    std::uint64_t   revision = 0;
};
Shown shown(int key, const MeshComponent& mc, const ModifierStackComponent* ms);
inline Shown shown(const Entity& e, const MeshComponent& mc) {
    return shown(e.id, mc, e.components.get<ModifierStackComponent>());
}

// Forget every evaluated mesh (a scene load: the ids start again).
void clearCache();

} // namespace modifiers
