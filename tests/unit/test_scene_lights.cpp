// Analytic lights through the real chain: Aether scene text -> aether::SceneParser
// -> harmonia::scene::makeLights -> Light::toGpu(). Pure CPU, no Vulkan.
//
// Scene-linear radiance is photometric everywhere in Harmonia: an analytic
// light, an emissive material and the environment with the same photometric
// value shade alike. toGpu() therefore passes the photometric value through
// unchanged, and these tests pin that against quantities computed from the
// definitions of the units (candela, lumen, nit, lux) rather than from the code
// under test.

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <slang-math/slang-math.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "aether/format/SceneParser.hpp"
#include "aether/types/SceneDesc.hpp"
#include "harmonia/GpuTypes.hpp"
#include "harmonia/scene/Light.hpp"
#include "harmonia/scene/MaterialLibrary.hpp"
#include "harmonia/scene/SceneLoader.hpp"

namespace {

namespace fs = std::filesystem;
using harmonia::scene::DirectionalLight;
using harmonia::scene::Light;
using harmonia::scene::PointLight;
using harmonia::scene::RectLight;
using harmonia::scene::SpotLight;
using Workspace = harmonia::utils::ColorSpace::WorkingColorSpace;

constexpr float kPi = 3.14159265358979323846F;
constexpr float kEps = 1.0e-4F;

// A scratch directory per test; removed with the fixture.
struct Scratch {
    fs::path dir;

    explicit Scratch(std::string_view name)
        : dir(fs::temp_directory_path() / ("harmonia_lights_" + std::string{name})) {
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Scratch() { fs::remove_all(dir); }

    void write(const char* file, std::string_view text) const {
        std::ofstream out(dir / file);
        out << text;
    }
};

// Parses scene text through the real Aether parser (a throwaway file).
std::optional<aether::SceneDesc> parse(const Scratch& scratch, std::string_view toml, std::string* error = nullptr) {
    scratch.write("s.scene.toml", toml);
    return aether::SceneParser::parse(scratch.dir / "s.scene.toml", error);
}

// Component-wise near-equality; the streamable form carries a message.
[[nodiscard]] ::testing::AssertionResult near3(const sm::float3& value, const sm::float3& expected, float eps) {
    if (std::abs(value.x - expected.x) <= eps && std::abs(value.y - expected.y) <= eps &&
        std::abs(value.z - expected.z) <= eps) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "[" << value.x << ", " << value.y << ", " << value.z << "] vs ["
                                         << expected.x << ", " << expected.y << ", " << expected.z << "]";
}

void expectNear(sm::float3 value, sm::float3 expected, float eps = kEps) {
    EXPECT_TRUE(near3(value, expected, eps));
}

std::uint32_t typeOf(const harmonia::GpuLight& g) {
    return std::bit_cast<std::uint32_t>(g.type);
}

// The rectangle a rect-sampling shader integrates over, rebuilt from the
// descriptor exactly as Theia's shadeRectLight builds it: the canonical
// in-plane frame of the normal, rolled by the descriptor's roll.
struct SampledRect {
    sm::float3 normal;
    sm::float3 axisU; ///< spans halfWidth (the light's local X axis)
    sm::float3 axisV; ///< spans halfHeight (perpendicular, in plane)
    float area;
};

SampledRect sampledRect(const harmonia::GpuLight& g) {
    const sm::float3 n = sm::normalize(g.direction);
    const sm::float3 up = std::abs(n.y) < 0.99f ? sm::float3{0.0f, 1.0f, 0.0f} : sm::float3{1.0f, 0.0f, 0.0f};
    const sm::float3 t0 = sm::normalize(sm::cross(up, n));
    const sm::float3 b0 = sm::cross(n, t0);
    const float roll = g.cosInner;
    SampledRect rect{};
    rect.normal = n;
    rect.axisU = std::cos(roll) * t0 + std::sin(roll) * b0;
    rect.axisV = sm::cross(n, rect.axisU);
    rect.area = 4.0F * g.halfWidth * g.halfHeight;
    return rect;
}

constexpr std::string_view kFourLights = R"([render]
environment_unit_nits = 2500.0

[[light]]
name = "Bulb"
type = "point"
translate = [1.0, 2.0, 3.0]
color = [1.0, 0.5, 0.25]
intensity = 800.0
unit = "lumen"
range = 5.0

[[light]]
name = "Cone"
type = "spot"
translate = [-1.0, 4.0, 0.0]
rotate_x = 90.0
intensity = 1000.0
unit = "candela"
inner_angle = 12.0
outer_angle = 30.0

[[light]]
name = "Panel"
type = "rect"
translate = [0.0, 3.0, 0.0]
intensity = 1500.0
unit = "nit"
half_extents = [0.5, 1.0]

[[light]]
name = "Sun"
type = "directional"
rotate = [0.3826834, 0.0, 0.0, 0.9238795]
intensity = 100000.0
unit = "lux"
)";

} // namespace

// ── class, placement and type-specific parameters ───────────────────────────

TEST(SceneLights, EveryTypeBecomesItsMatchingClassInDeclarationOrder) {
    Scratch scratch("types");
    const auto desc = parse(scratch, kFourLights);
    ASSERT_TRUE(desc.has_value());
    const auto lights = harmonia::scene::makeLights(*desc, Workspace::LinRec709);
    ASSERT_EQ(lights.size(), 4U);

    EXPECT_EQ(lights[0]->lightType(), harmonia::LightType::Point);
    EXPECT_EQ(lights[1]->lightType(), harmonia::LightType::Spot);
    EXPECT_EQ(lights[2]->lightType(), harmonia::LightType::Rect);
    EXPECT_EQ(lights[3]->lightType(), harmonia::LightType::Directional);
    EXPECT_EQ(lights[0]->name, "Bulb");
    EXPECT_EQ(lights[1]->name, "Cone");
    EXPECT_EQ(lights[2]->name, "Panel");
    EXPECT_EQ(lights[3]->name, "Sun");

    const auto* point = dynamic_cast<const PointLight*>(lights[0].get());
    const auto* spot = dynamic_cast<const SpotLight*>(lights[1].get());
    const auto* rect = dynamic_cast<const RectLight*>(lights[2].get());
    const auto* sun = dynamic_cast<const DirectionalLight*>(lights[3].get());
    ASSERT_NE(point, nullptr);
    ASSERT_NE(spot, nullptr);
    ASSERT_NE(rect, nullptr);
    ASSERT_NE(sun, nullptr);

    // point: position, range, unit
    expectNear(point->position, {1.0f, 2.0f, 3.0f});
    EXPECT_FLOAT_EQ(point->range, 5.0F);
    EXPECT_EQ(point->unit, harmonia::scene::IntensityUnit::Lumen);
    expectNear(point->color, {1.0F, 0.5F, 0.25F});

    // spot: direction = rotation * (0, 0, -1): rotate_x = 90 turns -Z onto +Y
    expectNear(spot->position, {-1.0f, 4.0f, 0.0f});
    expectNear(spot->direction, {0.0f, 1.0f, 0.0f});
    EXPECT_FLOAT_EQ(spot->innerAngleDeg, 12.0F);
    EXPECT_FLOAT_EQ(spot->outerAngleDeg, 30.0F);
    EXPECT_EQ(spot->unit, harmonia::scene::IntensityUnit::Candela);
    EXPECT_FLOAT_EQ(spot->range, 0.0F);

    // rect: emission normal straight down at identity rotation; local X / Y extents
    expectNear(rect->position, {0.0f, 3.0f, 0.0f});
    expectNear(rect->direction, {0.0f, 0.0f, -1.0f});
    EXPECT_FLOAT_EQ(rect->halfWidth, 0.5F);
    EXPECT_FLOAT_EQ(rect->halfHeight, 1.0F);
    EXPECT_TRUE(near3(rect->tangent, {1.0f, 0.0f, 0.0f}, kEps)) << "the loader sets the local X axis";

    // directional: travel direction = rotation * (0, 0, -1), 45 degrees about X
    expectNear(sun->direction, {0.0f, 0.7071068F, -0.7071068F});
}

TEST(SceneLights, SpotCosinesFollowTheAngles) {
    Scratch scratch("spot");
    const auto desc = parse(scratch, kFourLights);
    ASSERT_TRUE(desc.has_value());
    const auto lights = harmonia::scene::makeLights(*desc, Workspace::LinRec709);
    const harmonia::GpuLight g = lights[1]->toGpu();
    EXPECT_NEAR(g.cosInner, std::cos(sm::radians(12.0f)), 1.0e-6F);
    EXPECT_NEAR(g.cosOuter, std::cos(sm::radians(30.0f)), 1.0e-6F);
    EXPECT_GT(g.cosInner, g.cosOuter) << "the full-intensity cone lies inside the falloff cone";
    expectNear(g.direction, {0.0f, 1.0f, 0.0f});
}

TEST(SceneLights, RectDirectionAndExtentsReachTheGpuDescriptor) {
    Scratch scratch("rect");
    const auto desc = parse(scratch, kFourLights);
    ASSERT_TRUE(desc.has_value());
    const auto lights = harmonia::scene::makeLights(*desc, Workspace::LinRec709);
    const harmonia::GpuLight g = lights[2]->toGpu();
    EXPECT_EQ(typeOf(g), static_cast<std::uint32_t>(harmonia::LightType::Rect));
    EXPECT_FLOAT_EQ(g.halfWidth, 0.5F);
    EXPECT_FLOAT_EQ(g.halfHeight, 1.0F);
    expectNear(g.direction, {0.0f, 0.0f, -1.0f});
    EXPECT_FLOAT_EQ(g.range, 0.0F);
    EXPECT_FLOAT_EQ(g.cosOuter, 0.0F) << "a rect light leaves the outer-cone slot at 0";
}

// ── numbers: the photometric value reaches the shader unchanged ─────────────

TEST(SceneLights, PhotometricValuesReachTheDescriptorUnchanged) {
    Scratch scratch("values");
    const auto desc = parse(scratch, kFourLights);
    ASSERT_TRUE(desc.has_value());
    const auto lights = harmonia::scene::makeLights(*desc, Workspace::LinRec709);

    // lumen -> candela for an isotropic source is the only conversion, and it is
    // a unit change within the photometric system: 800 lm = 800 / 4pi cd.
    const harmonia::GpuLight bulb = lights[0]->toGpu();
    EXPECT_NEAR(bulb.intensity, 800.0F / (4.0F * kPi), 1.0e-4F);
    // Inverse square in photometric units: cd / d^2 is the illuminance in lux.
    for (const float d : {0.5F, 1.0F, 2.0F, 10.0F}) {
        EXPECT_NEAR(bulb.intensity / (d * d), 800.0F / (4.0F * kPi * d * d), 1.0e-4F) << "d = " << d;
    }
    EXPECT_FLOAT_EQ(bulb.range, 5.0F);

    // candela, nit and lux pass through one to one: no factor of 683, no 1/683.
    EXPECT_FLOAT_EQ(lights[1]->toGpu().intensity, 1000.0F) << "spot in candela";
    EXPECT_FLOAT_EQ(lights[2]->toGpu().intensity, 1500.0F) << "rect in nit";
    EXPECT_FLOAT_EQ(lights[3]->toGpu().intensity, 100000.0F) << "directional in lux";
}

TEST(SceneLights, RectAndPointAgreeWithTheLambertianDefinitions) {
    Scratch scratch("lambert");
    const auto desc = parse(scratch, kFourLights);
    ASSERT_TRUE(desc.has_value());
    const auto lights = harmonia::scene::makeLights(*desc, Workspace::LinRec709);
    const harmonia::GpuLight rect = lights[2]->toGpu();
    const SampledRect sampled = sampledRect(rect);

    // The three photometric units of an emitter, from the descriptor's numbers:
    // a Lambertian emitter of luminance L [cd/m^2] over area A [m^2] has the
    // exitance pi L [lm/m^2], the flux pi L A [lm] and, on its axis, the
    // intensity L A [cd]. Expected values written out: L = 1500, A = 2.
    EXPECT_NEAR(kPi * rect.intensity, 4712.389F, 1.0e-2F) << "exitance";
    EXPECT_NEAR(kPi * rect.intensity * sampled.area, 9424.778F, 1.0e-2F) << "flux";
    EXPECT_NEAR(rect.intensity * sampled.area, 3000.0F, 1.0e-3F) << "axial intensity";
    // An isotropic source of that flux would be a weaker point light: 9424.778 / 4pi cd.
    PointLight isotropic;
    isotropic.intensity = 9424.778F;
    isotropic.unit = harmonia::scene::IntensityUnit::Lumen;
    EXPECT_NEAR(isotropic.toGpu().intensity, 750.0F, 1.0e-3F);
}

TEST(SceneLights, DirectionalIrradianceIsTheLuxValue) {
    Scratch scratch("sun");
    const auto desc = parse(scratch, kFourLights);
    ASSERT_TRUE(desc.has_value());
    const auto lights = harmonia::scene::makeLights(*desc, Workspace::LinRec709);
    const harmonia::GpuLight g = lights[3]->toGpu();
    EXPECT_EQ(typeOf(g), static_cast<std::uint32_t>(harmonia::LightType::Directional));
    // The illuminance on a surface facing the light is the lux value itself.
    EXPECT_FLOAT_EQ(g.intensity, 100000.0F);
    expectNear(g.direction, {0.0f, 0.7071068F, -0.7071068F});
}

// ── one photometric scale for analytic lights, emissive materials and the environment ──

TEST(SceneLights, AnalyticEmittersShareThePhotometricScaleWithEmissiveAndEnvironment) {
    // One photometric value V = 2500 cd/m^2 carried by three different sources
    // of the scene. What the shaders read must be the same number for all
    // three: GpuLight::intensity (color * intensity), GpuMaterial::
    // emissionColorLum.w (emission_color * emission_luminance) and the
    // environment scale (envLuminanceScale * environment sample, which the
    // applications set from SceneConfig::envUnitNits unchanged).
    constexpr float kV = 2500.0F;
    Scratch scratch("scale");
    scratch.write("l.materials.toml", R"(model = "openpbr"
colorspace = "lin_rec709_scene"

[Lamp]
base_color = [0.5, 0.5, 0.5]
emission_color = [1.0, 1.0, 1.0]
emission_luminance = 2500.0
)");
    harmonia::scene::MaterialLibrary library;
    ASSERT_TRUE(library.load(scratch.dir / "l.materials.toml", Workspace::LinRec709));
    const auto lamp = library.get("Lamp");
    ASSERT_TRUE(lamp.has_value());
    EXPECT_FLOAT_EQ(lamp->gpu().emissionColorLum.w, kV) << "emissive material, real library path";

    // The same value as an analytic rect light and as the environment of the scene.
    const std::string toml = std::string{kFourLights} + R"(
[[light]]
name = "Tube"
type = "rect"
intensity = 2500.0
unit = "nit"
half_extents = [0.5, 0.5]
)";
    const auto desc = parse(scratch, toml);
    ASSERT_TRUE(desc.has_value());
    ASSERT_TRUE(desc->envUnitNits.has_value());
    EXPECT_FLOAT_EQ(*desc->envUnitNits, kV) << "environment_unit_nits, handed to the apps unchanged";

    const auto lights = harmonia::scene::makeLights(*desc, Workspace::LinRec709);
    ASSERT_EQ(lights.size(), 5U);
    const harmonia::GpuLight tube = lights[4]->toGpu();
    EXPECT_FLOAT_EQ(tube.intensity, kV) << "analytic rect light";
    EXPECT_FLOAT_EQ(tube.intensity, lamp->gpu().emissionColorLum.w);
    EXPECT_FLOAT_EQ(tube.intensity, *desc->envUnitNits);

    // A Lambertian SPHERE of luminance L and radius r has the luminous intensity
    // L pi r^2 (its projected area is pi r^2) - the rule Theia applies to an
    // emissive sphere it turns into a light. A point light of that many cd is
    // the same emitter seen from afar. 2500 * pi * 0.0625 = 490.8739.
    const float r = 0.25F;
    PointLight sphereEquivalent;
    sphereEquivalent.intensity = kV * kPi * r * r;
    EXPECT_NEAR(sphereEquivalent.toGpu().intensity, 490.8739F, 1.0e-3F);
}

TEST(SceneLights, FractionalTransmissionDoesNotSelectFullGlassMode) {
    Scratch scratch("fractional_transmission");
    scratch.write("l.materials.toml", R"(model = "openpbr"
colorspace = "lin_rec709_scene"

[Quarter]
transmission_weight = 0.25

[Half]
transmission_weight = 0.5

[ThreeQuarters]
transmission_weight = 0.75

[Full]
transmission_weight = 1.0

[Metallic]
transmission_weight = 1.0
base_metalness = 0.25
)");
    harmonia::scene::MaterialLibrary library;
    ASSERT_TRUE(library.load(scratch.dir / "l.materials.toml", Workspace::LinRec709));
    for (const char* name : {"Quarter", "Half", "ThreeQuarters", "Metallic"}) {
        const auto material = library.get(name);
        ASSERT_TRUE(material.has_value()) << name;
        EXPECT_FLOAT_EQ(material->gpu().opacityFlagsPad.y, 0.0F) << name;
    }
    const auto full = library.get("Full");
    ASSERT_TRUE(full.has_value());
    EXPECT_FLOAT_EQ(full->gpu().opacityFlagsPad.y, 2.0F);
}

TEST(SceneLights, TexturedEmissionUsesHitOnlyOwnershipAndKeepsItsScale) {
    Scratch scratch("textured_emission");
    scratch.write("l.materials.toml", R"(model = "openpbr"
colorspace = "lin_rec709_scene"

[Textured]
emission_luminance = 1.806468
map_emission_luminance = "emission.png"

[Legacy]
map_emission_luminance = "legacy.png"

[Constant]
emission_luminance = 4.0
)");
    harmonia::scene::MaterialLibrary library;
    ASSERT_TRUE(library.load(scratch.dir / "l.materials.toml", Workspace::LinRec709));
    const auto textured = library.get("Textured");
    ASSERT_TRUE(textured.has_value());
    EXPECT_FALSE(textured->emissiveAsLightSource()) << "no constant-radiance NEE for a textured EDF";
    EXPECT_FLOAT_EQ(textured->gpu().baseMetalnessDiffRough.z, 1.0F) << "BSDF hits own its emission at every depth";
    EXPECT_FLOAT_EQ(textured->gpu().emissionColorLum.w, 1.806468F) << "the authored HDR scale survives";
    const auto legacy = library.get("Legacy");
    ASSERT_TRUE(legacy.has_value());
    EXPECT_FLOAT_EQ(legacy->gpu().emissionColorLum.w, 1.0F) << "legacy maps keep their historical unit scale";
    const auto constant = library.get("Constant");
    ASSERT_TRUE(constant.has_value());
    EXPECT_TRUE(constant->emissiveAsLightSource());
    EXPECT_FLOAT_EQ(constant->gpu().baseMetalnessDiffRough.z, 0.0F);
}

TEST(SceneLights, RectWithoutOrientationKeepsTheCanonicalFrame) {
    RectLight rect; // tangent zero: no orientation given
    rect.direction = {0.0f, 0.0f, -1.0f};
    rect.halfWidth = 0.5F;
    rect.halfHeight = 1.0F;
    const harmonia::GpuLight g = rect.toGpu();
    EXPECT_FLOAT_EQ(g.cosInner, 0.0F) << "roll 0";
    const SampledRect sampled = sampledRect(g);
    expectNear(sampled.normal, {0.0f, 0.0f, -1.0f});
    EXPECT_FLOAT_EQ(sampled.area, 2.0F);
    // The canonical frame of that normal: up = +Y, U = cross(up, n).
    expectNear(sampled.axisU, {-1.0f, 0.0f, 0.0f});
    expectNear(sampled.axisV, {0.0f, 1.0f, 0.0f});
}

TEST(SceneLights, RectRollReproducesTheLightLocalAxes) {
    // For any normal and any roll, the axis the shader samples halfWidth along
    // is the light's local X axis, and the other axis completes the frame.
    struct Pose {
        sm::float3 normal;
        sm::float3 tangent;
        const char* what;
    };
    const Pose poses[]{
        {{0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, "down, X to the right"},
        {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, "down, X to the back: a 90 degree roll"},
        {{0.0f, 0.0f, -1.0f}, {0.7071068f, 0.7071068f, 0.0f}, "down, 45 degrees"},
        {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, "straight up: the canonical frame switches its up vector"},
        {{0.0f, -0.995f, -0.1f}, {1.0f, 0.0f, 0.0f}, "near the canonical switch (|n.y| > 0.99)"},
        {{0.0f, -0.98f, -0.199f}, {1.0f, 0.0f, 0.0f}, "just below the canonical switch"},
        {{0.5773503f, 0.5773503f, -0.5773503f}, {0.7071068f, -0.7071068f, 0.0f}, "tilted normal, tangent in plane"},
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, "normal along +X"},
    };
    for (const Pose& pose : poses) {
        RectLight rect;
        rect.direction = pose.normal;
        rect.tangent = pose.tangent; // any in-plane vector: normalized against the normal
        rect.halfWidth = 0.25F;
        rect.halfHeight = 0.75F;
        const harmonia::GpuLight g = rect.toGpu();
        const SampledRect sampled = sampledRect(g);
        const sm::float3 wantU = sm::normalize(pose.tangent);
        EXPECT_NEAR(sm::dot(sampled.axisU, wantU), 1.0F, 2.0e-5F) << pose.what;
        expectNear(sampled.axisU, wantU, 2.0e-5F);
        EXPECT_TRUE(near3(sampled.normal, sm::normalize(pose.normal), 1.0e-6F)) << pose.what;
        EXPECT_NEAR(sm::length(sampled.axisU), 1.0F, 1.0e-6F) << pose.what;
        EXPECT_NEAR(sm::length(sampled.axisV), 1.0F, 1.0e-6F) << pose.what;
        EXPECT_NEAR(sm::dot(sampled.axisU, sampled.normal), 0.0F, 2.0e-5F) << pose.what;
        EXPECT_NEAR(sm::dot(sampled.axisV, sampled.normal), 0.0F, 2.0e-5F) << pose.what;
        EXPECT_NEAR(sm::dot(sampled.axisU, sampled.axisV), 0.0F, 2.0e-5F) << pose.what;
        // The extents follow their axes: a non-square rectangle stays non-square.
        EXPECT_FLOAT_EQ(g.halfWidth, 0.25F) << pose.what;
        EXPECT_FLOAT_EQ(g.halfHeight, 0.75F) << pose.what;
        EXPECT_FLOAT_EQ(sampled.area, 0.75F) << pose.what; // 4 * 0.25 * 0.75
    }
}

TEST(SceneLights, RectNinetyDegreeRollChangesAxesNotNormalOrArea) {
    RectLight unrolled;
    unrolled.direction = {0.0f, 0.0f, -1.0f};
    unrolled.tangent = {1.0f, 0.0f, 0.0f};
    unrolled.halfWidth = 0.25F;
    unrolled.halfHeight = 0.75F;
    RectLight rolled = unrolled;
    rolled.tangent = {0.0f, 1.0f, 0.0f}; // the local X axis turned 90 degrees in plane

    const SampledRect a = sampledRect(unrolled.toGpu());
    const SampledRect b = sampledRect(rolled.toGpu());
    const harmonia::GpuLight ga = unrolled.toGpu();
    const harmonia::GpuLight gb = rolled.toGpu();

    // The roll itself differs by a quarter turn (modulo 2 pi and direction).
    const float quarterTurn = std::abs(gb.cosInner - ga.cosInner);
    EXPECT_NEAR(quarterTurn, kPi / 2.0F, 1.0e-5F);

    // Axes: the rolled U axis lies along the old V axis - the long side now
    // spans where the short one did - while normal and area are untouched.
    EXPECT_NEAR(std::abs(sm::dot(b.axisU, a.axisV)), 1.0F, 1.0e-5F);
    EXPECT_NEAR(sm::dot(b.axisU, a.axisU), 0.0F, 1.0e-5F);
    expectNear(b.normal, a.normal, 1.0e-6F);
    EXPECT_FLOAT_EQ(b.area, a.area);
    EXPECT_FLOAT_EQ(gb.halfWidth, ga.halfWidth);
    EXPECT_FLOAT_EQ(gb.halfHeight, ga.halfHeight);
    EXPECT_FLOAT_EQ(gb.intensity, ga.intensity);
}

TEST(SceneLights, LoaderCarriesTheRectOrientationFromTheSceneTransform) {
    // A rect light turned 90 degrees about its own normal in the scene file:
    // local X becomes -Y of the world. The descriptor must hand the shader that
    // axis as the half-width axis.
    Scratch scratch("roll_loader");
    const auto desc = parse(scratch, R"([[light]]
name = "Panel"
type = "rect"
translate = [0.0, 3.0, 0.0]
rotate = [0.0, 0.0, 0.7071068, 0.7071068]
intensity = 1000.0
unit = "nit"
half_extents = [0.25, 0.75]
)");
    ASSERT_TRUE(desc.has_value());
    const auto lights = harmonia::scene::makeLights(*desc, Workspace::LinRec709);
    ASSERT_EQ(lights.size(), 1U);
    const auto* rect = dynamic_cast<const RectLight*>(lights[0].get());
    ASSERT_NE(rect, nullptr);
    // Quaternion (0, 0, sin45, cos45): a +90 degree turn about Z.
    expectNear(rect->direction, {0.0f, 0.0f, -1.0f});
    expectNear(rect->tangent, {0.0f, 1.0f, 0.0f});
    const SampledRect sampled = sampledRect(rect->toGpu());
    expectNear(sampled.axisU, {0.0f, 1.0f, 0.0f}, 2.0e-5F);
    expectNear(sampled.axisV, {1.0f, 0.0f, 0.0f}, 2.0e-5F);
    EXPECT_FLOAT_EQ(sampled.area, 0.75F); // 4 * 0.25 * 0.75
}

// ── color path ───────────────────────────────────────────────────────────────

TEST(SceneLights, ColorFollowsTheMaterialColorPath) {
    Scratch scratch("color");
    const std::string toml = std::string{kFourLights} + R"(
[[light]]
name = "WideRed"
type = "point"
color = [1.0, 0.0, 0.0]
color_space = "lin_rec2020_scene"
intensity = 100.0
unit = "candela"
)";
    const auto desc = parse(scratch, toml);
    ASSERT_TRUE(desc.has_value());

    // Rec.709 source, Rec.709 working space: unchanged.
    auto sameSpace = harmonia::scene::makeLights(*desc, Workspace::LinRec709);
    expectNear(sameSpace[0]->color, {1.0F, 0.5F, 0.25F});
    // Rec.709 source, Rec.2020 working space: rec709ToRec2020, the material path.
    auto toWide = harmonia::scene::makeLights(*desc, Workspace::LinRec2020);
    expectNear(toWide[0]->color, harmonia::utils::ColorSpace::rec709ToRec2020({1.0F, 0.5F, 0.25F}), 1.0e-6F);
    // Rec.2020 source, Rec.709 working space: rec2020ToRec709.
    expectNear(sameSpace[4]->color, harmonia::utils::ColorSpace::rec2020ToRec709({1.0F, 0.0F, 0.0F}), 1.0e-6F);
    // Rec.2020 source, Rec.2020 working space: unchanged (a Rec.2020 primary is out of Rec.709).
    expectNear(toWide[4]->color, {1.0F, 0.0F, 0.0F});
    EXPECT_GT(harmonia::utils::ColorSpace::rec709ToRec2020({1.0F, 0.5F, 0.25F}).g, 0.5F)
        << "the conversion must actually change the value";
}

// ── compatibility ────────────────────────────────────────────────────────────

TEST(SceneLights, LightFreeSceneMakesNoLight) {
    Scratch scratch("none");
    const auto bad = parse(scratch, "[[light]]\n");
    EXPECT_FALSE(bad.has_value()) << "an empty light entry is a parse error, not a light";
    const auto empty = parse(scratch, "[camera]\ntranslate = [0.0, 0.0, 5.0]\nrotate_x = 0.0\n");
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(harmonia::scene::makeLights(*empty, Workspace::LinRec2020).empty());
    EXPECT_FALSE(empty->envUnitNits.has_value()) << "no [render] keys: nothing is invented";
}
