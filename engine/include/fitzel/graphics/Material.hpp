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
    using Value = std::variant<int, float, glm::vec3, glm::vec4, glm::mat4>;

    explicit Material(Shader& shader) : m_shader(&shader) {}

    Shader* shader() const { return m_shader; }

    // Chainable setters.
    Material& set(const std::string& name, Value value);
    Material& setTexture(const std::string& name, const Texture& texture,
                         std::uint32_t unit);

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
