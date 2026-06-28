#include "fitzel/graphics/Material.hpp"

#include "fitzel/graphics/Shader.hpp"
#include "fitzel/graphics/Texture.hpp"

namespace fitzel {

Material& Material::set(const std::string& name, Value value) {
    m_uniforms[name] = value;
    return *this;
}

Material& Material::setTexture(const std::string& name, const Texture& texture,
                               std::uint32_t unit) {
    m_textures[name] = {&texture, unit};
    return *this;
}

void Material::apply() const {
    m_shader->bind();

    for (const auto& [name, value] : m_uniforms) {
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, int>)             m_shader->setInt(name, v);
            else if constexpr (std::is_same_v<T, float>)      m_shader->setFloat(name, v);
            else if constexpr (std::is_same_v<T, glm::vec3>)  m_shader->setVec3(name, v);
            else if constexpr (std::is_same_v<T, glm::vec4>)  m_shader->setVec4(name, v);
            else if constexpr (std::is_same_v<T, glm::mat4>)  m_shader->setMat4(name, v);
        }, value);
    }

    for (const auto& [name, binding] : m_textures) {
        binding.texture->bind(binding.unit);
        m_shader->setInt(name, static_cast<int>(binding.unit));
    }
}

} // namespace fitzel
