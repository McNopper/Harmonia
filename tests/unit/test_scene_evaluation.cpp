#include <gtest/gtest.h>
#include <optional>
#include <vector>

#include "harmonia/scene/SceneBase.hpp"

namespace {

class EvaluationScene final : public harmonia::scene::SceneBase {
  public:
    using SceneBase::addInstance;
    using SceneBase::attachAnimation;

    [[nodiscard]] std::uint32_t addSphereMesh(const harmonia::DeviceContext&,
                                              const harmonia::core::CommandPool&,
                                              float,
                                              std::string_view) override {
        return 0U;
    }

  protected:
    [[nodiscard]] VkResult buildSceneBuffers(const harmonia::DeviceContext&,
                                             const harmonia::core::CommandPool&) override {
        return VK_SUCCESS;
    }

    [[nodiscard]] std::unique_ptr<harmonia::scene::Geometry> createTriangleMesh(const harmonia::DeviceContext&,
                                                                                const harmonia::core::CommandPool&,
                                                                                harmonia::scene::MeshData&&,
                                                                                const harmonia::scene::MeshOpacity&,
                                                                                std::string_view) override {
        return nullptr;
    }

    [[nodiscard]] VkResult finishBuild(const harmonia::DeviceContext&, const harmonia::core::CommandPool&) override {
        return VK_SUCCESS;
    }
};

TEST(SceneAnimationEvaluation, ReusesOneEvaluatedSceneForInstancesAndCamera) {
    aether::SceneDesc source;
    source.animationFps = 30.0;
    aether::NodeDesc cameraNode;
    cameraNode.id = "Camera";
    cameraNode.meshName = "mesh";
    source.nodes.push_back(cameraNode);
    source.camera.nodeId = "Camera";
    aether::AnimationTrack track;
    track.nodeIndex = 0;
    track.parameter = aether::AnimationTrack::Parameter::Translation;
    track.times = {0.0F, 1.0F};
    track.values = {0.0F, 0.0F, 0.0F, 1.0F, 2.0F, 3.0F};
    source.animations.push_back(track);
    aether::InstanceDesc flat;
    flat.meshName = "mesh";
    source.instances.push_back(flat);
    source.instances.push_back(flat); // the parser's initial node resolution also emits this slot

    EvaluationScene scene;
    EXPECT_EQ(scene.addInstance(0U, harmonia::scene::Transform{}, 0U), 0U);
    scene.attachAnimation(std::move(source), {1U}); // instance 0 is authored; the meshed node emits instance 1
    const aether::SceneDesc* attached = scene.attachedScene();
    ASSERT_NE(attached, nullptr);
    EXPECT_EQ(scene.evaluatedScene(), attached) << "before the first evaluation the authored scene is the fallback";

    scene.setAnimationTime(1.0F);
    const aether::SceneDesc* evaluated = scene.evaluatedScene();
    ASSERT_NE(evaluated, attached);
    const sm::float4x4 matrix = scene.instances()[0].transform.matrix();
    EXPECT_FLOAT_EQ(matrix[0][3], 1.0F);
    EXPECT_FLOAT_EQ(matrix[1][3], 2.0F);
    EXPECT_FLOAT_EQ(matrix[2][3], 3.0F);
    EXPECT_FLOAT_EQ(attached->nodes[0].translation.x, 0.0F) << "the immutable source is not stamped";
    const aether::CameraPose expected = aether::SceneAnimator::evaluateCamera(*attached, 1.0F).value();
    const aether::CameraPose actual = aether::SceneAnimator::cameraPoseFromEvaluated(*evaluated).value();
    EXPECT_EQ(actual.position, expected.position);
    EXPECT_EQ(actual.rotation, expected.rotation);

    scene.setAnimationTime(0.5F);
    EXPECT_EQ(scene.evaluatedScene(), evaluated) << "the evaluated SceneDesc storage is reused";
    const sm::float4x4 midpoint = scene.instances()[0].transform.matrix();
    EXPECT_FLOAT_EQ(midpoint[0][3], 0.5F);
    EXPECT_FLOAT_EQ(midpoint[1][3], 1.0F);
    EXPECT_FLOAT_EQ(midpoint[2][3], 1.5F);
}

} // namespace
