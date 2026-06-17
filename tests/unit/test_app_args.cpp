#include <gtest/gtest.h>

#include "harmonia/app/App.hpp"

TEST(AppArgs, EnablesDisplayOverlayFlag) {
    harmonia::App::Config config;

    char arg0[] = "app";
    char arg1[] = "--display-overlay";
    char* argv[] = {arg0, arg1};
    int i = 1;

    EXPECT_TRUE(harmonia::App::applyCommonArg(config, i, 2, argv));
    EXPECT_TRUE(config.displayOverlay);
    EXPECT_EQ(i, 1);
}
