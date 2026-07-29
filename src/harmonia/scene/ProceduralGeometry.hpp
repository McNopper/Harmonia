#ifndef HARMONIA_SCENE_PROCEDURALGEOMETRY_HPP
#define HARMONIA_SCENE_PROCEDURALGEOMETRY_HPP

#include <slang-math/slang-math.hpp>

#include "harmonia/scene/Mesh.hpp"

namespace ProceduralGeometry {
[[nodiscard]] MeshData makeBox(sm::float3 halfExtent, const sm::float4x4& transform = sm::float4x4(1.0f));

/// Generate a UV sphere mesh centred at @p center with the given radius.
/// @p rings: latitude bands (default 32), @p slices: longitude segments (default 64).
/// Normals, tangents and UVs are generated analytically — no flat shading artefacts.
[[nodiscard]] MeshData makeSphere(sm::float3 center, float radius, uint32_t rings = 32, uint32_t slices = 64);

struct SphereAabb {
    sm::float3 min{};
    sm::float3 max{};
};

[[nodiscard]] SphereAabb makeSphereAabb(sm::float3 center, float radius) noexcept;
} // namespace ProceduralGeometry
#endif // HARMONIA_SCENE_PROCEDURALGEOMETRY_HPP
