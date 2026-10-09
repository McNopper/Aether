cmake_minimum_required(VERSION 3.28)

project(aether VERSION 0.7.5 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Include by absolute path: when Aether is pulled in via FetchContent, a parent
# project may have its own cmake/Platform.cmake earlier on CMAKE_MODULE_PATH,
# which would otherwise shadow this one and leave aether_configure_target undefined.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Platform.cmake")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# AI-first static analysis (clang-tidy / cppcheck / clang-format)
#
# Canonical setup shared by every repo in the pipeline, ported from the
# Hephaestus cpp/ template. Analysis runs only when the generator emits a
# compile database (Ninja/Makefiles) and the compiler is Clang/GNU-
# compatible; otherwise it is skipped and the project configures/builds
# normally. The analysis *targets* are opt-in; in-build analysis is OFF by
# default so the build is never blocked by findings.
#
# Guarded on PROJECT_IS_TOP_LEVEL so the targets are only defined when this
# project is built standalone: several of these repos are consumed via
# FetchContent by downstream pipeline repos, where redefining the targets
# would clash with the consumer's own.
# ============================================================
if(PROJECT_IS_TOP_LEVEL)
    set(PROJECT_REPORTS_DIR ${CMAKE_BINARY_DIR}/reports)
    file(MAKE_DIRECTORY ${PROJECT_REPORTS_DIR})

    set(PROJECT_ANALYSIS_AVAILABLE FALSE)
    if(CMAKE_EXPORT_COMPILE_COMMANDS
       AND (CMAKE_GENERATOR MATCHES "Ninja" OR CMAKE_GENERATOR MATCHES "Makefiles")
       AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU"))
        set(PROJECT_ANALYSIS_AVAILABLE TRUE)
        message(STATUS "AI-first static analysis enabled (${CMAKE_GENERATOR}, ${CMAKE_CXX_COMPILER_ID})")
    else()
        message(STATUS "AI-first static analysis skipped for this toolchain "
            "(${CMAKE_GENERATOR}, ${CMAKE_CXX_COMPILER_ID}); "
            "use Ninja/Makefiles with a Clang- or GNU-compatible compiler to enable it")
    endif()

    file(GENERATE
        OUTPUT ${PROJECT_REPORTS_DIR}/analysis-status.txt
        CONTENT "analysis=$<BOOL:${PROJECT_ANALYSIS_AVAILABLE}>\ngenerator=${CMAKE_GENERATOR}\ncompiler=${CMAKE_CXX_COMPILER_ID}\n"
    )

    option(ENABLE_CLANG_TIDY "Enable clang-tidy static analysis" ON)
    option(ENABLE_CLANG_TIDY_IN_BUILD "Run clang-tidy during compilation" OFF)
    option(ENABLE_CLANG_FORMAT "Enable clang-format targets" ON)
    option(ENABLE_CPPCHECK "Enable cppcheck static analysis target" ON)
    option(ENABLE_CPPCHECK_IN_BUILD "Run cppcheck alongside the compiler" OFF)

    # Sanitizer lane (Clang/GCC only):
    #   cmake -DAETHER_SANITIZER=address|undefined|thread ...
    set(AETHER_SANITIZER "" CACHE STRING "Sanitizer: address, undefined, or thread (Clang/GCC only)")
    if(AETHER_SANITIZER)
        if(MSVC)
            message(FATAL_ERROR "AETHER_SANITIZER requires Clang/GCC (use a Ninja+clang or WSL configure)")
        endif()
        add_compile_options("-fsanitize=${AETHER_SANITIZER}" -fno-omit-frame-pointer)
        add_link_options("-fsanitize=${AETHER_SANITIZER}")
    endif()

    # All first-party C/C++ sources (clang-format); compiled translation units
    # only (clang-tidy) — headers are analysed transitively via HeaderFilterRegex.
    file(GLOB_RECURSE PROJECT_CXX_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hxx"
    )
    file(GLOB_RECURSE PROJECT_CXX_TUS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cxx"
    )

    # ----- clang-tidy -----
    if(ENABLE_CLANG_TIDY AND PROJECT_ANALYSIS_AVAILABLE)
        find_program(CLANG_TIDY_EXE NAMES clang-tidy)
        if(CLANG_TIDY_EXE)
            if(ENABLE_CLANG_TIDY_IN_BUILD)
                set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
                message(STATUS "clang-tidy enabled during build: ${CLANG_TIDY_EXE}")
            else()
                message(STATUS "clang-tidy target enabled: ${CLANG_TIDY_EXE}")
            endif()

            set(CLANG_TIDY_REPORT_DIR ${PROJECT_REPORTS_DIR}/clang-tidy)
            file(MAKE_DIRECTORY ${CLANG_TIDY_REPORT_DIR})

            add_custom_target(tidy
                COMMAND ${CLANG_TIDY_EXE}
                    -p ${CMAKE_BINARY_DIR}
                    --export-fixes=${CLANG_TIDY_REPORT_DIR}/fixes.yaml
                    ${PROJECT_CXX_TUS}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Running clang-tidy; fixes -> ${CLANG_TIDY_REPORT_DIR}/fixes.yaml"
                VERBATIM
            )
        else()
            message(STATUS "clang-tidy not found")
        endif()
    endif()

    # ----- cppcheck -----
    if(ENABLE_CPPCHECK AND PROJECT_ANALYSIS_AVAILABLE)
        list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
        include(cppcheck)
    endif()

    # ----- clang-format -----
    if(ENABLE_CLANG_FORMAT)
        find_program(CLANG_FORMAT_EXE NAMES clang-format)
        if(CLANG_FORMAT_EXE)
            add_custom_target(format
                COMMAND ${CLANG_FORMAT_EXE}
                    -i
                    --style=file
                    ${PROJECT_CXX_SOURCES}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Formatting source files with clang-format"
                VERBATIM
            )

            add_custom_target(format-check
                COMMAND ${CLANG_FORMAT_EXE}
                    --dry-run
                    --Werror
                    --style=file
                    ${PROJECT_CXX_SOURCES}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Checking source formatting with clang-format"
                VERBATIM
            )

            message(STATUS "clang-format enabled: ${CLANG_FORMAT_EXE}")
        else()
            message(STATUS "clang-format not found")
        endif()
    endif()

    # ----- Aggregate verification targets -----
    # verify:      build + test (fast default).
    # verify-full: verify + format-check + tidy + cppcheck (opt-in strict).
    add_custom_target(verify
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG>
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --build-config $<CONFIG>
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Fast verification: build + test"
        VERBATIM
    )

    add_custom_target(verify-full
        COMMENT "Full verification: verify + format-check + tidy + cppcheck"
    )
    add_dependencies(verify-full verify)
    if(TARGET format-check)
        add_dependencies(verify-full format-check)
    endif()
    if(TARGET cppcheck)
        add_dependencies(verify-full cppcheck)
    endif()
    if(TARGET tidy)
        add_dependencies(verify-full tidy)
    endif()
endif()

# Use the default dynamic CRT (MD) to match the vcpkg ecosystem and any parent
# project (Harmonia, GoogleTest with shared CRT). Do NOT force MultiThreaded
# (MT/static): it causes RuntimeLibrary mismatches when Aether is linked as a
# FetchContent subproject alongside vcpkg/gtest libraries.

# Dependencies. Aether has no Vulkan / GPU deps.
#   slang-math   — vector / quaternion math
#   tomlplusplus — header-only TOML parser for the .scene.toml / .materials.toml formats
include(FetchContent)
FetchContent_Declare(
    slang_math
    GIT_REPOSITORY https://github.com/McNopper/slang-math.git
    GIT_TAG v0.2.1
)
FetchContent_MakeAvailable(slang_math)
find_package(tomlplusplus CONFIG REQUIRED)

set(AETHER_SOURCES
    src/aether/format/MaterialLibrary.cpp
    src/aether/format/ObjImporter.cpp
    src/aether/format/OmmImporter.cpp
    src/aether/format/SceneParser.cpp
)

add_library(aether_project_options INTERFACE)
aether_configure_target(aether_project_options)

add_library(aether STATIC ${AETHER_SOURCES})
add_library(aether::aether ALIAS aether)

target_include_directories(aether
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(aether
    PUBLIC slang-math::slang-math
    PRIVATE aether_project_options tomlplusplus::tomlplusplus
)

# Path to the canonical asset collection that Aether owns (scene/material
# format files plus the geometry/environment/texture assets they reference).
# Consumers receive this so the renderer can resolve referenced assets.
set(AETHER_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH "Aether canonical assets directory")

# Tests only build when Aether is the top-level project.
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    if(BUILD_TESTING)
        enable_testing()
        add_subdirectory(tests)
    endif()
endif()

# Static-analysis ctest lane: parallel clang-tidy over
# build/compile_commands.json, classified per the .clang-tidy WarningsAsErrors
# contract. Skips (77) when clang-tidy / Python3 / the database is unavailable.
if(PROJECT_IS_TOP_LEVEL)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME test_tidy
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_tidy.py)
        set_tests_properties(test_tidy PROPERTIES SKIP_RETURN_CODE 77 LABELS analysis)
    endif()
endif()

