#pragma once

/// Aether — renderer-agnostic scene & material file-format library.
///
/// Single convenience header that exposes the full public API: the parsed CPU
/// data types and the three parsers (.scene.toml, .materials.toml, OBJ). Aether depends only
/// on slang-math and never touches Vulkan, the GPU, or a renderer.

#include "aether/format/MaterialLibrary.hpp"
#include "aether/format/ObjImporter.hpp"
#include "aether/format/SceneParser.hpp"
#include "aether/types/MaterialDesc.hpp"
#include "aether/types/Math.hpp"
#include "aether/types/MeshData.hpp"
#include "aether/types/SceneDesc.hpp"
#include "aether/types/TextureColorSpace.hpp"
