#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <vector>

#include "harmonia/app/App.hpp"

TEST(AppArgs, ParsesShutterAndTimeSamples) {
    harmonia::app::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--shutter";
    char arg2[] = "0.5";
    char arg3[] = "--time-samples";
    char arg4[] = "8";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 5, argv));
    EXPECT_FLOAT_EQ(config.shutter, 0.5F);
    EXPECT_EQ(i, 2); // like every flag with a value: the index rests on the value
    i = 3;           // --time-samples
    ASSERT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 5, argv));
    EXPECT_EQ(config.timeSamples, 8U);
    EXPECT_EQ(i, 4);
}

TEST(AppArgs, RejectsShutterOutsideUnitIntervalAndZeroTimeSamples) {
    harmonia::app::App::Config config;
    config.shutter = 0.25F;
    config.timeSamples = 4U;

    char arg0[] = "app";
    char arg1[] = "--shutter";
    char arg2[] = "1.5";
    char arg3[] = "--time-samples";
    char arg4[] = "0";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4};
    int i = 1;

    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 5, argv));
    EXPECT_FLOAT_EQ(config.shutter, 0.25F) << "out of range: unchanged";
    EXPECT_EQ(i, 2) << "a rejected value is still consumed";
    i = 3;
    EXPECT_TRUE(harmonia::app::CliParser::applyCommonArg(config, i, 5, argv));
    EXPECT_EQ(config.timeSamples, 4U) << "zero time samples: unchanged";
    EXPECT_EQ(i, 4);
}

// ── The sequence capture clock ─────────────────────────────────────────────────
// The pure function behind App::animationClock() during a sequence capture.

using harmonia::app::App;
using Clock = App::AnimationClock;

App::Config sequenceConfig() {
    App::Config config;
    config.sequenceFrames = 3U;
    config.rngSeed = 2026U;
    return config;
}

TEST(SequenceClock, WithoutAShutterEverySampleRendersTheFrameInstant) {
    const App::Config config = sequenceConfig();
    for (const std::uint32_t frame : {0U, 1U, 2U}) {
        for (std::uint32_t sample = 0U; sample < 8U; ++sample) {
            const Clock clock = App::sequenceSampleClock(config, 24.0, frame, sample);
            EXPECT_DOUBLE_EQ(clock.seconds, static_cast<double>(frame) / 24.0) << "sample " << sample;
            EXPECT_EQ(clock.key, frame);
        }
    }
}

TEST(SequenceClock, AShutterSpreadsTheSamplesOverTheOpenShutterInStrata) {
    App::Config config = sequenceConfig();
    config.shutter = 0.5F;
    config.timeSamples = 4U;
    const double frameInterval = 1.0 / 24.0;
    const double open = 0.5 * frameInterval;

    for (const std::uint32_t frame : {0U, 1U, 2U}) {
        const double frameTime = static_cast<double>(frame) * frameInterval;
        std::uint32_t distinctKeys = 0U;
        std::uint64_t lastKey = ~0ULL; // a value no time quantizes to
        for (std::uint32_t sample = 0U; sample < 16U; ++sample) {
            const Clock clock = App::sequenceSampleClock(config, 24.0, frame, sample);
            const std::uint32_t stratum = sample % 4U;
            // Inside the open shutter, and inside its own quarter of it.
            EXPECT_GE(clock.seconds, frameTime) << "sample " << sample;
            EXPECT_LT(clock.seconds, frameTime + open) << "sample " << sample;
            EXPECT_GE(clock.seconds, frameTime + open * static_cast<double>(stratum) / 4.0) << "sample " << sample;
            EXPECT_LT(clock.seconds, frameTime + open * static_cast<double>(stratum + 1U) / 4.0) << "sample " << sample;
            EXPECT_NE(clock.key, lastKey) << "consecutive samples share a time";
            if (clock.key != lastKey) {
                ++distinctKeys;
            }
            lastKey = clock.key;
        }
        EXPECT_EQ(distinctKeys, 16U) << "every sample gets its own time";
    }
}

TEST(SequenceClock, TimeSamplesAreReproducibleFromTheSeedAndWellDistributed) {
    App::Config config = sequenceConfig();
    config.shutter = 0.5F;
    config.timeSamples = 4U;

    // Deterministic for the same arguments ...
    for (std::uint32_t sample = 0U; sample < 4U; ++sample) {
        EXPECT_EQ(App::sequenceSampleClock(config, 24.0, 1U, sample).key,
                  App::sequenceSampleClock(config, 24.0, 1U, sample).key);
    }
    // ... and the jitter fills its stratum uniformly over many frames (mean 0.5 of
    // the stratum width, well away from the stratum edges).
    double sum = 0.0;
    int count = 0;
    for (std::uint32_t frame = 0U; frame < 4096U; ++frame) {
        const double frameTime = static_cast<double>(frame) / 24.0;
        const double stratumWidth = 0.5 / 24.0 / 4.0;
        const double inStratum = (App::sequenceSampleClock(config, 24.0, frame, 2U).seconds - frameTime) / stratumWidth;
        EXPECT_GE(inStratum, 2.0);
        EXPECT_LT(inStratum, 3.0);
        sum += inStratum - 2.0;
        ++count;
    }
    EXPECT_NEAR(sum / static_cast<double>(count), 0.5, 0.03);
}

TEST(SequenceClock, IntervalCaptureShuttersOverTheCaptureInterval) {
    App::Config config = sequenceConfig();
    config.captureIntervalMs = 100.0; // every 100 ms of animation time
    config.captureStartSeconds = 2.0;
    config.shutter = 1.0F;
    config.timeSamples = 2U;

    const Clock first = App::sequenceSampleClock(config, 24.0, 0U, 0U);
    EXPECT_GE(first.seconds, 2.0);
    EXPECT_LT(first.seconds, 2.05);
    const Clock second = App::sequenceSampleClock(config, 24.0, 1U, 1U);
    EXPECT_GE(second.seconds, 2.0 + 0.1 + 0.05);
    EXPECT_LT(second.seconds, 2.0 + 0.2);
    EXPECT_NE(first.key, second.key);
}

TEST(SequenceClock, SeekOffsetAppliesToShutteredFrames) {
    App::Config config = sequenceConfig();
    config.animationStartFrame = 10U;
    config.shutter = 0.5F;
    config.timeSamples = 2U;

    const Clock clock = App::sequenceSampleClock(config, 24.0, 0U, 1U);
    EXPECT_GE(clock.seconds, 10.0 / 24.0 + 0.25 / 24.0);
    EXPECT_LT(clock.seconds, 10.0 / 24.0 + 0.5 / 24.0);
}

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

TEST(SequenceClock, AccumulationFramesRunGroupedByTimeSample) {
    // 256 accumulation frames over 8 instants: 32 consecutive frames per instant.
    for (std::uint32_t frame = 0U; frame < 256U; ++frame) {
        EXPECT_EQ(harmonia::app::App::timeSampleOfFrame(frame, 256U, 8U), frame / 32U) << "frame " << frame;
    }
}

TEST(SequenceClock, GroupingKeepsEveryInstantItsShareOfFramesAndStampsOncePerInstant) {
    for (std::uint32_t timeSamples = 2U; timeSamples <= 16U; ++timeSamples) {
        for (std::uint32_t count = timeSamples; count <= 70U; ++count) {
            std::vector<std::uint32_t> alternating(timeSamples, 0U);
            std::vector<std::uint32_t> grouped(timeSamples, 0U);
            std::uint32_t instantsVisited = 0U;
            std::uint32_t last = timeSamples; // no stratum
            for (std::uint32_t frame = 0U; frame < count; ++frame) {
                ++alternating[frame % timeSamples];
                const std::uint32_t sample = harmonia::app::App::timeSampleOfFrame(frame, count, timeSamples);
                ASSERT_LT(sample, timeSamples) << "count " << count << ", instants " << timeSamples;
                ++grouped[sample];
                if (sample != last) {
                    ++instantsVisited;
                }
                last = sample;
            }
            // The exposure is integrated with the weights the alternating order had ...
            EXPECT_EQ(alternating, grouped) << "count " << count << ", instants " << timeSamples;
            // ... and the scene is stamped with a new time once per instant, not once per frame.
            EXPECT_EQ(instantsVisited, timeSamples) << "count " << count << ", instants " << timeSamples;
        }
    }
    // Without a shutter the index is the frame; with fewer frames than instants the strata alternate.
    EXPECT_EQ(harmonia::app::App::timeSampleOfFrame(7U, 16U, 1U), 7U);
    EXPECT_EQ(harmonia::app::App::timeSampleOfFrame(5U, 3U, 8U), 5U);
}
