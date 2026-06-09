include(FetchContent)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

if(TARGET gtest)
    set_property(TARGET gtest PROPERTY SYSTEM ON)
endif()
if(TARGET gtest_main)
    set_property(TARGET gtest_main PROPERTY SYSTEM ON)
endif()
if(NOT TARGET GTest::gtest AND TARGET gtest)
    add_library(GTest::gtest ALIAS gtest)
endif()
if(NOT TARGET GTest::gtest_main AND TARGET gtest_main)
    add_library(GTest::gtest_main ALIAS gtest_main)
endif()

add_executable(aether_tests
    test_material_library.cpp
    test_obj_importer.cpp
    test_scene_parser.cpp
)

target_link_libraries(aether_tests
    PRIVATE aether::aether aether_project_options GTest::gtest GTest::gtest_main
)

# Aether owns the canonical assets; tests parse real .scene / .mtlx fixtures.
target_compile_definitions(aether_tests
    PRIVATE AETHER_ASSETS_DIR="${AETHER_ASSETS_DIR}"
)

include(GoogleTest)
gtest_discover_tests(aether_tests)
