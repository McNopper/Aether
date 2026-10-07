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
    # gtest v1.15.2 has an implicit char8_t->char32_t conversion in C++23;
    # suppress it - but only where the compiler actually KNOWS
    # -Wno-character-conversion. clang added that warning group after the
    # runner images' bundled clang; an unknown -Wno-* option is silent until
    # -Werror, where it hard-fails (observed in CI). The check runs with
    # -Werror so it fails exactly like the real build would - and a compiler
    # whose check fails also cannot emit the warning, so flag and warning
    # stay coupled (verified: clang-cl 22 warns AND knows the flag; the
    # runner's clang does neither).
    include(CheckCXXCompilerFlag)
    set(_aether_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
    check_cxx_compiler_flag("-Wno-character-conversion" AETHER_HAVE_WNO_CHARACTER_CONVERSION)
    set(CMAKE_REQUIRED_FLAGS "${_aether_saved_required_flags}")
    if(AETHER_HAVE_WNO_CHARACTER_CONVERSION)
        foreach(_gt_tgt IN ITEMS gtest gtest_main gmock gmock_main)
            if(TARGET ${_gt_tgt})
                target_compile_options(${_gt_tgt} PRIVATE
                    $<$<CXX_COMPILER_ID:Clang,MSVC>:-Wno-character-conversion>
                )
            endif()
        endforeach()
    endif()
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
    test_opacity_micromap.cpp
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
