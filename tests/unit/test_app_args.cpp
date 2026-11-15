#include <gtest/gtest.h>

#include "harmonia/app/App.hpp"

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
