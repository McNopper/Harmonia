#include <gtest/gtest.h>

#include "fixtures/ShaderDispatchFixture.hpp"

namespace {

constexpr std::size_t kFields = 8U;
constexpr std::size_t kModes = 2U;

void expectField(const std::vector<sm::float4>& values, std::size_t mode, std::size_t field, sm::float4 expected) {
    const sm::float4 actual = values[mode * kFields + field];
    EXPECT_NEAR(actual.x, expected.x, 1.0e-6F);
    EXPECT_NEAR(actual.y, expected.y, 1.0e-6F);
    EXPECT_NEAR(actual.z, expected.z, 1.0e-6F);
    EXPECT_NEAR(actual.w, expected.w, 1.0e-6F);
}

} // namespace

TEST_F(ShaderDispatchFixture, ExtraMaterialMapsResolveConnectedInputs) {
    const auto values = runShader("extra_maps_test.spv", kModes * kFields);
    ASSERT_EQ(values.size(), kModes * kFields);
    expectField(values, 0U, 0U, {1.0F, 1.0F, 1.0F, 0.25F}); // coat weight
    expectField(values, 0U, 1U, {0.8F, 0.1F, 0.2F, 0.6F});  // transmission color and weight
    expectField(values, 0U, 2U, {0.2F, 0.4F, 0.6F, 0.35F}); // subsurface color and weight
    expectField(values, 0U, 3U, {0.3F, 0.5F, 0.7F, 0.0F});  // transmission scatter, anisotropy untouched
    expectField(values, 0U, 4U, {1.0F, 1.0F, 1.0F, 0.45F}); // emission color; luminance scale 0.5 * 0.9
    expectField(values, 0U, 5U, {1.0F, 1.0F, 1.0F, 0.45F}); // fuzz color and weight
    expectField(values, 0U, 6U, {0.8F, 0.8F, 0.8F, 1.0F});  // base color is not bound in this test
    expectField(values, 0U, 7U, {0.0F, 0.0F, 0.0F, 0.0F});  // metalness is not bound in this test

    // Control: without maps the zero-valued scalar inputs remain zero and colors stay at their defaults.
    expectField(values, 1U, 0U, {1.0F, 1.0F, 1.0F, 0.0F});
    expectField(values, 1U, 1U, {1.0F, 1.0F, 1.0F, 0.0F});
    expectField(values, 1U, 2U, {0.8F, 0.8F, 0.8F, 0.0F});
    expectField(values, 1U, 3U, {0.0F, 0.0F, 0.0F, 0.0F});
    expectField(values, 1U, 4U, {1.0F, 1.0F, 1.0F, 0.5F});
    expectField(values, 1U, 5U, {1.0F, 1.0F, 1.0F, 0.0F});
    expectField(values, 1U, 6U, {0.8F, 0.8F, 0.8F, 1.0F});
    expectField(values, 1U, 7U, {0.0F, 0.0F, 0.0F, 0.0F});
}
