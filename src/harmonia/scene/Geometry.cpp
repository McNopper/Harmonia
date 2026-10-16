#include "harmonia/scene/Geometry.hpp"
#include <volk/volk.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <slang-math/slang-math.hpp>

#include "harmonia/scene/Mesh.hpp"

namespace harmonia::scene {
sm::float4x4 Transform::matrix() const noexcept {
    return sm::translate(sm::float4x4(1.0f), translation) * sm::toFloat4x4(rotation) *
           sm::scale(sm::float4x4(1.0f), scale);
}

sm::float4x4 Transform::inverseMatrix() const noexcept {
    return sm::inverse(matrix());
}

VkTransformMatrixKHR Transform::toVkTransform() const noexcept {
    const sm::float4x4 transform = matrix();
    return VkTransformMatrixKHR{{
        {transform[0][0], transform[0][1], transform[0][2], transform[0][3]},
        {transform[1][0], transform[1][1], transform[1][2], transform[1][3]},
        {transform[2][0], transform[2][1], transform[2][2], transform[2][3]},
    }};
}

Aabb worldAabbFromInstance(const Aabb& object, const Transform& transform) noexcept {
    const sm::float4x4 m = transform.matrix();
    const sm::float3 corners[8]{
        {object.min.x, object.min.y, object.min.z},
        {object.min.x, object.min.y, object.max.z},
        {object.min.x, object.max.y, object.min.z},
        {object.min.x, object.max.y, object.max.z},
        {object.max.x, object.min.y, object.min.z},
        {object.max.x, object.min.y, object.max.z},
        {object.max.x, object.max.y, object.min.z},
        {object.max.x, object.max.y, object.max.z},
    };
    sm::float3 mn = static_cast<sm::float3>(m * sm::float4(corners[0], 1.0f));
    sm::float3 mx = mn;
    for (std::size_t i = 1; i < std::size(corners); ++i) {
        const sm::float3 p = static_cast<sm::float3>(m * sm::float4(corners[i], 1.0f));
        mn = sm::min(mn, p);
        mx = sm::max(mx, p);
    }
    return Aabb{.min = mn, .max = mx};
}

} // namespace harmonia::scene
