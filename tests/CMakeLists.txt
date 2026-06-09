add_executable(harmonia_unit_tests
    unit/test_colorspace.cpp
    unit/test_tonemap.cpp
    unit/test_math.cpp
)

target_link_libraries(harmonia_unit_tests
    PRIVATE harmonia_lib harmonia_project_options GTest::gtest GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(harmonia_unit_tests)
