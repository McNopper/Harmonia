#include <gtest/gtest.h>

#include "harmonia/app/App.hpp"

TEST(AppArgs, EnablesDisplayOverlayFlag) {
    harmonia::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--display-overlay";
    char* argv[] = {arg0, arg1};
    int i = 1;

    EXPECT_TRUE(harmonia::CliParser::applyCommonArg(config, i, 2, argv));
    EXPECT_TRUE(config.displayOverlay);
    EXPECT_EQ(i, 1);
}

TEST(AppArgs, EnablesDeterministicReplayFlag) {
    harmonia::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--deterministic-replay";
    char* argv[] = {arg0, arg1};
    int i = 1;

    EXPECT_TRUE(harmonia::CliParser::applyCommonArg(config, i, 2, argv));
    EXPECT_TRUE(config.deterministicReplay);
}

TEST(AppArgs, ParsesRngSeed) {
    harmonia::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--rng-seed";
    char arg2[] = "424242";
    char* argv[] = {arg0, arg1, arg2};
    int i = 1;

    EXPECT_TRUE(harmonia::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_EQ(config.rngSeed, 424242U);
    EXPECT_EQ(i, 2);
}

TEST(AppArgs, ParsesOffscreenFrames) {
    harmonia::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--offscreen-frames";
    char arg2[] = "64";
    char* argv[] = {arg0, arg1, arg2};
    int i = 1;

    EXPECT_TRUE(harmonia::CliParser::applyCommonArg(config, i, 3, argv));
    EXPECT_EQ(config.offscreenFrames, 64U);
    EXPECT_EQ(i, 2);
}

TEST(AppArgs, EnablesRngDebugFlag) {
    harmonia::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--rng-debug";
    char* argv[] = {arg0, arg1};
    int i = 1;

    EXPECT_TRUE(harmonia::CliParser::applyCommonArg(config, i, 2, argv));
    EXPECT_TRUE(config.rngDebug);
}

TEST(AppArgs, ParsesDenoiserStrengthAndIterations) {
    harmonia::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--denoiser-strength";
    char arg2[] = "0.7";
    char arg3[] = "--denoiser-iterations";
    char arg4[] = "5";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4};
    int i = 1;

    EXPECT_TRUE(harmonia::CliParser::applyCommonArg(config, i, 5, argv));
    EXPECT_FLOAT_EQ(config.denoiser.strength, 0.7F);
    EXPECT_EQ(i, 2);

    i = 3;
    EXPECT_TRUE(harmonia::CliParser::applyCommonArg(config, i, 5, argv));
    EXPECT_EQ(config.denoiser.iterations, 5U);
    EXPECT_EQ(i, 4);
}

TEST(AppArgs, ParsesDenoiserHistoryOptions) {
    harmonia::App::Config config;
    config.denoiser.useHistory = true;

    char arg0[] = "app";
    char arg1[] = "--denoiser-history-blend";
    char arg2[] = "0.33";
    char arg3[] = "--denoiser-no-history";
    char* argv[] = {arg0, arg1, arg2, arg3};
    int i = 1;

    EXPECT_TRUE(harmonia::CliParser::applyCommonArg(config, i, 4, argv));
    EXPECT_FLOAT_EQ(config.denoiser.historyBlend, 0.33F);
    EXPECT_EQ(i, 2);

    i = 3;
    EXPECT_TRUE(harmonia::CliParser::applyCommonArg(config, i, 4, argv));
    EXPECT_FALSE(config.denoiser.useHistory);
}
