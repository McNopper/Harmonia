#include "harmonia/scene/SceneBase.hpp"

#include <utility>

namespace harmonia {

uint32_t SceneBase::addMaterial(Material&& mat) {
    m_materials.push_back(std::move(mat));
    return static_cast<uint32_t>(m_materials.size() - 1);
}

uint32_t SceneBase::addTexture(Texture&& texture) {
    const auto idx = static_cast<uint32_t>(m_textures.size());
    m_textures.push_back(std::move(texture));
    return idx;
}

uint32_t SceneBase::addLight(std::unique_ptr<Light> light) {
    const uint32_t index = static_cast<uint32_t>(m_lights.size());
    m_lights.push_back(std::move(light));
    return index;
}

} // namespace harmonia
