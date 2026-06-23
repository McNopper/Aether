cmake_minimum_required(VERSION 3.28)

project(aether VERSION 0.4.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Include by absolute path: when Aether is pulled in via FetchContent, a parent
# project may have its own cmake/Platform.cmake earlier on CMAKE_MODULE_PATH,
# which would otherwise shadow this one and leave aether_configure_target undefined.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Platform.cmake")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use the default dynamic CRT (MD) to match the vcpkg ecosystem and any parent
# project (Harmonia, GoogleTest with shared CRT). Do NOT force MultiThreaded
# (MT/static): it causes RuntimeLibrary mismatches when Aether is linked as a
# FetchContent subproject alongside vcpkg/gtest libraries.

# Dependencies (provided via vcpkg). Aether has no Vulkan / GPU deps.
#   GLM           — vector / quaternion math
#   tomlplusplus  — header-only TOML parser for the .scene.toml / .materials.toml formats
find_package(glm CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

set(AETHER_SOURCES
    src/aether/format/MaterialLibrary.cpp
    src/aether/format/ObjImporter.cpp
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
    PUBLIC glm::glm
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
