#ifndef TESTS_FIXTURES_EMPTYSCENEBUILDER_HPP
#define TESTS_FIXTURES_EMPTYSCENEBUILDER_HPP

// A scene builder that keeps nothing, for an application whose renderer draws a
// frame of its own: the scene only has to load.

#include <cstdint>
#include <string_view>

#include "harmonia/DeviceContext.hpp"
#include "harmonia/core/CommandPool.hpp"
#include "harmonia/scene/ISceneBuilder.hpp"

class EmptySceneBuilder final : public harmonia::scene::ISceneBuilder {
  public:
    std::uint32_t addMaterial(harmonia::scene::Material&&) override { return 0; }
    std::uint32_t addTexture(harmonia::scene::Texture&&) override { return 0; }
    std::uint32_t addMesh(const harmonia::DeviceContext&,
                          const harmonia::core::CommandPool&,
                          harmonia::scene::MeshData&&,
                          const harmonia::scene::MeshOpacity&,
                          std::string_view) override {
        return 0;
    }
    std::uint32_t addSphereMesh(const harmonia::DeviceContext&,
                                const harmonia::core::CommandPool&,
                                float,
                                std::string_view) override {
        return 0;
    }
    std::uint32_t addInstance(std::uint32_t, const harmonia::scene::Transform&, std::uint32_t) override { return 0; }
    const aether::OpacityMicromapData& addOpacityMicromap(aether::OpacityMicromapData&&) override { return m_omm; }

  private:
    aether::OpacityMicromapData m_omm;
};

#endif // TESTS_FIXTURES_EMPTYSCENEBUILDER_HPP
