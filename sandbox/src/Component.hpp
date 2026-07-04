#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "Property.hpp"

// A component is an optional, self-describing capability attached to an entity.
// It exposes its fields through the same Property metadata as entities, so the
// auto-inspector and JSON serialization work on it for free. Components are
// type-erased (cloneable) so an entity stays a copyable value -- which keeps the
// undo/redo snapshot mechanism working unchanged.
//
// This is the extensibility backbone: a new capability (a sound emitter, a
// trigger, a spinner) is a new ComponentBase subclass + a registration -- no
// central enum or switch to touch.
class ComponentBase {
public:
    virtual ~ComponentBase() = default;
    virtual std::unique_ptr<ComponentBase> clone() const = 0;
    virtual const char* typeId() const = 0;       // stable id (serialization + registry)
    virtual const char* displayName() const = 0;  // label in the inspector
    virtual const std::vector<Property>& props() const = 0; // field metadata over *this
};

// Holds an entity's components with value semantics: copying deep-clones, so an
// Entity remains copyable (undo snapshots) despite owning polymorphic parts.
class ComponentList {
public:
    ComponentList() = default;
    ComponentList(const ComponentList& o) { *this = o; }
    ComponentList& operator=(const ComponentList& o) {
        items.clear();
        items.reserve(o.items.size());
        for (const auto& c : o.items) items.push_back(c->clone());
        return *this;
    }
    ComponentList(ComponentList&&) = default;
    ComponentList& operator=(ComponentList&&) = default;

    std::vector<std::unique_ptr<ComponentBase>> items;
};

// True if two component lists are value-equal (used by undo to drop no-op edits).
bool componentsEqual(const ComponentList& a, const ComponentList& b);

// --- Type registry: every component kind registers a factory + label, so the
// "Add Component" menu and deserialization are open (no central switch). --------
namespace components {

struct TypeInfo {
    std::string typeId;
    std::string displayName;
    std::function<std::unique_ptr<ComponentBase>()> make;
};

std::vector<TypeInfo>&           registry();
void                             registerType(TypeInfo info);
std::unique_ptr<ComponentBase>   create(const std::string& typeId);

} // namespace components

// --- Built-in component: Spin (rotates the entity while playing) -------------
class SpinComponent : public ComponentBase {
public:
    glm::vec3 axis{0.0f, 1.0f, 0.0f}; // rotation axis weights
    float     speed = 90.0f;          // degrees per second

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<SpinComponent>(*this);
    }
    const char* typeId() const override { return "spin"; }
    const char* displayName() const override { return "Spin"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
};
