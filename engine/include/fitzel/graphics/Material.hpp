#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>

#include <glm/glm.hpp>

namespace fitzel {

class Shader;
class Texture;

// Pairs a Shader with a set of named parameter values (uniforms + textures).
// apply() binds the shader and uploads everything. Per-frame/per-object
// uniforms (camera, light, model, shadows) are owned by the Renderer, not the
// Material -- a Material describes surface appearance only.
class Material {
public:
    using Value = std::variant<int, float, glm::vec2, glm::vec3, glm::vec4, glm::mat4>;

    explicit Material(Shader& shader) : m_shader(&shader) {}

    Shader* shader() const { return m_shader; }

    // Chainable setters.
    Material& set(const std::string& name, Value value);
    Material& setTexture(const std::string& name, const Texture& texture,
                         std::uint32_t unit);

    // Read a stored parameter back, or nullptr if this material never set one
    // by that name. For consumers that have to interpret a surface rather than
    // draw it -- the path tracer reads uAlbedo/uRoughness/uReflectivity here
    // instead of keeping a parallel description of every material in the scene,
    // which is the version that drifts.
    const Value*   uniform(const std::string& name) const;
    const Texture* texture(const std::string& name) const;

    // Convenience for the same: the stored value if it is of type T, else
    // `fallback`. Saves every caller writing the same get-then-visit dance --
    // and a material that simply never set a parameter is the normal case, not
    // an error (the shader's own default applies).
    template <typename T>
    T get(const std::string& name, T fallback) const {
        const Value* v = uniform(name);
        if (!v) return fallback;
        if (const T* t = std::get_if<T>(v)) return *t;
        return fallback;
    }

    // Bind the shader and upload all stored parameters.
    void apply() const;

private:
    struct TextureBinding {
        const Texture* texture;
        std::uint32_t  unit;
    };

    Shader* m_shader = nullptr;
    std::unordered_map<std::string, Value>          m_uniforms;
    std::unordered_map<std::string, TextureBinding> m_textures;
};

} // namespace fitzel
