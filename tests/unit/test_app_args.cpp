#include <fstream>
#include <gtest/gtest.h>

#include "harmonia/app/App.hpp"

TEST(AppArgs, SceneArgumentNamesASceneInTheAssetsDirectory) {
    namespace fs = std::filesystem;
    const fs::path assets = fs::temp_directory_path() / "harmonia_scene_arg_assets";
    fs::create_directories(assets / "export");
    {
        std::ofstream(assets / "cornell_classic.scene.toml");
    }
    {
        std::ofstream(assets / "export" / "city.scene.toml");
    }
    harmonia::app::App::Config config;
    config.assetsDir = assets;

    // The bare name of a scene in the assets root ...
    EXPECT_EQ(harmonia::app::CliParser::resolveScenePath(config, "cornell_classic"),
              assets / "cornell_classic.scene.toml");
    // ... a path into a subdirectory of it (a Blender export is one folder) ...
    EXPECT_EQ(harmonia::app::CliParser::resolveScenePath(config, "export/city"), assets / "export" / "city.scene.toml");
    EXPECT_EQ(harmonia::app::CliParser::resolveScenePath(config, "export/city.scene.toml"),
              assets / "export" / "city.scene.toml");
    // ... and an existing path (absolute, or relative to the working directory) as-is.
    const fs::path existing = assets / "export" / "city.scene.toml";
    EXPECT_EQ(harmonia::app::CliParser::resolveScenePath(config, existing), existing);
    // Not found anywhere: against the assets root, where the loader diagnoses it.
    EXPECT_EQ(harmonia::app::CliParser::resolveScenePath(config, "no_such_scene"), assets / "no_such_scene.scene.toml");

    fs::remove_all(assets);
}

TEST(AppArgs, EnablesDisplayOverlayFlag) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--display-overlay";
    char* argv[] = {arg0, arg1};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 2, argv));
    EXPECT_TRUE(config.displayOverlay);
    EXPECT_EQ(i, 1);
}

TEST(AppArgs, EnablesDeterministicReplayFlag) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--deterministic-replay";
    char* argv[] = {arg0, arg1};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 2, argv));
    EXPECT_TRUE(config.deterministicReplay);
}

TEST(AppArgs, ParsesRngSeed) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--rng-seed";
    char arg2[] = "424242";
    char* argv[] = {arg0, arg1, arg2};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_EQ(config.rngSeed, 424242U);
    EXPECT_EQ(i, 2);
}

TEST(AppArgs, ParsesOffscreenFrames) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--offscreen-frames";
    char arg2[] = "64";
    char* argv[] = {arg0, arg1, arg2};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_EQ(config.offscreenFrames, 64U);
    EXPECT_EQ(i, 2);
}

TEST(AppArgs, SequenceFramesDefaultOff) {
    // ANI7: 0 = the classic single capture; N >= 1 enables the sequence render.
    harmonia::app::App::Config config;
    EXPECT_EQ(config.sequenceFrames, 0U);
}

TEST(AppArgs, ParsesSequenceFrames) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--sequence-frames";
    char arg2[] = "12";
    char* argv[] = {arg0, arg1, arg2};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_EQ(config.sequenceFrames, 12U);
    EXPECT_EQ(i, 2) << "the value token must be consumed";
}

TEST(AppArgs, SequenceFramesInvalidValueKeepsDefault) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--sequence-frames";
    char arg2[] = "not-a-number";
    char* argv[] = {arg0, arg1, arg2};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_EQ(config.sequenceFrames, 0U) << "invalid value keeps the disabled default";
    EXPECT_EQ(i, 2) << "the invalid token is still consumed (never re-parsed as a scene)";
}

TEST(AppArgs, SequenceFramesIndependentOfOffscreenFrames) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--offscreen-frames";
    char arg2[] = "8";
    char arg3[] = "--sequence-frames";
    char arg4[] = "3";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4};
    int argc = 5;
    // Consume every arg exactly like main()'s applyCommonArg delegation loop.
    for (int i = 1; i < argc; ++i) {
        ASSERT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, argc, argv))
            << "applyCommonArg rejected '" << argv[i] << "'";
    }

    EXPECT_EQ(config.offscreenFrames, 8U);
    EXPECT_EQ(config.sequenceFrames, 3U);
}

TEST(AppArgs, EnablesRngDebugFlag) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--rng-debug";
    char* argv[] = {arg0, arg1};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 2, argv));
    EXPECT_TRUE(config.rngDebug);
}

TEST(AppArgs, ParsesDenoiserStrengthAndIterations) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--denoiser-strength";
    char arg2[] = "0.7";
    char arg3[] = "--denoiser-iterations";
    char arg4[] = "5";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 5, argv));
    EXPECT_FLOAT_EQ(config.denoiser.strength, 0.7F);
    EXPECT_EQ(i, 2);

    i = 3;
    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 5, argv));
    EXPECT_EQ(config.denoiser.iterations, 5U);
    EXPECT_EQ(i, 4);
}

TEST(AppArgs, ParsesDenoiserHistoryOptions) {
    harmonia::app::App::Config config;
    config.denoiser.useHistory = true;

    char arg0[] = "app";
    char arg1[] = "--denoiser-history-blend";
    char arg2[] = "0.33";
    char arg3[] = "--denoiser-no-history";
    char* argv[] = {arg0, arg1, arg2, arg3};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 4, argv));
    EXPECT_FLOAT_EQ(config.denoiser.historyBlend, 0.33F);
    EXPECT_EQ(i, 2);

    i = 3;
    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 4, argv));
    EXPECT_FALSE(config.denoiser.useHistory);
}

TEST(AppArgs, ParsesCaptureEveryMs) {
    // ANI7 timing extension: time-based sequence sampling.
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--capture-every-ms";
    char arg2[] = "16.667";
    char* argv[] = {arg0, arg1, arg2};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_DOUBLE_EQ(config.captureIntervalMs, 16.667);
    EXPECT_EQ(i, 2) << "the value token must be consumed";
}

TEST(AppArgs, CaptureEveryMsInvalidValueKeepsDefault) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--capture-every-ms";
    char arg2[] = "-5";
    char* argv[] = {arg0, arg1, arg2};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_DOUBLE_EQ(config.captureIntervalMs, 0.0) << "non-positive interval keeps the tick-grid default";
    EXPECT_EQ(i, 2);

    i = 1;
    char arg2b[] = "not-a-number";
    argv[2] = arg2b;
    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_DOUBLE_EQ(config.captureIntervalMs, 0.0);
    EXPECT_EQ(i, 2) << "the invalid token is still consumed";
}

TEST(AppArgs, ParsesStartSeconds) {
    harmonia::app::App::Config config;
    EXPECT_LT(config.captureStartSeconds, 0.0) << "default = derive the start from --frame / fps";

    char arg0[] = "app";
    char arg1[] = "--start-seconds";
    char arg2[] = "2.5";
    char* argv[] = {arg0, arg1, arg2};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_DOUBLE_EQ(config.captureStartSeconds, 2.5);
    EXPECT_EQ(i, 2);
}

TEST(AppArgs, StartSecondsNegativeKeepsDefault) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--start-seconds";
    char arg2[] = "-1";
    char* argv[] = {arg0, arg1, arg2};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_LT(config.captureStartSeconds, 0.0) << "negative start keeps the derive-from-frame default";
    EXPECT_EQ(i, 2);
}
