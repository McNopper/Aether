#ifndef AETHER_TYPES_OPACITYMICROMAP_HPP
#define AETHER_TYPES_OPACITYMICROMAP_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aether {

/// One histogram bucket of an opacity micromap. Deliberately **not**
/// layout-compatible with `VkMicromapUsageEXT` (which is three `uint32_t`);
/// this is the compact CPU form, and the consumer (Harmonia) widens it when it
/// builds the Vulkan usage array.
struct OpacityMicromapUsage {
    std::uint32_t count = 0;            ///< base triangles in this (level, format) bucket
    std::uint16_t subdivisionLevel = 0; ///< 4^level microtriangles per base triangle
    std::uint16_t format = 0;           ///< 1 = 2-state (1 bit), 2 = 4-state (2 bits)
};

/// One base triangle's micromap record — layout-compatible with
/// `VkMicromapTriangleEXT` (`{dataOffset, subdivisionLevel, format}`, 8 bytes).
/// `dataOffset` is a byte offset into `OpacityMicromapData::dataBits`.
struct OpacityMicromapTriangle {
    std::uint32_t dataOffset = 0; ///< byte offset into the packed-state data buffer
    std::uint16_t subdivisionLevel = 0;
    std::uint16_t format = 0;
};

/// Per-base-triangle special lookup values (the micromapIndices buffer). A base
/// triangle is either fully uniform (no record consumed — one of these specials)
/// or references a record by its ordinal in `OpacityMicromapGroup::triangles`.
/// Encoded as the two's-complement bit pattern of the signed value when stored
/// in an unsigned index buffer (mirrors glTF `EXT_mesh_opacity_micromap`).
enum class OpacityMicromapSpecial : std::int32_t {
    Transparent = -1,
    Opaque = -2,
    UnknownTransparent = -3,
    UnknownOpaque = -4,
};

/// One OBJ group's baked opacity micromap — a self-contained micromap (each
/// group is its own BLAS, so each gets its own `VkMicromapEXT`). `dataBits`
/// holds the packed-state bytes for this group's records; `triangles[i].
/// dataOffset` is relative to `dataBits`. `micromapIndices` has one entry per
/// base triangle (record ordinal or a special).
struct OpacityMicromapGroup {
    std::string name;
    std::string sourceMaterial;
    std::uint32_t triangleCount = 0; ///< total base triangles (records + specials)
    /// Packed microtriangle-state bits for this group's records, LSB-first
    /// within each byte, microtriangles in space-filling-curve order — the
    /// Vulkan `data` buffer, upload-ready.
    std::vector<std::byte> dataBits;
    std::vector<OpacityMicromapUsage> usage;
    std::vector<OpacityMicromapTriangle> triangles;
    /// One per base triangle, in OBJ face order: the record ordinal, or a
    /// special (negative). Upload as a `VK_INDEX_TYPE_UINT32` array, encoding
    /// specials as their two's-complement `uint32_t` bit pattern.
    std::vector<std::int32_t> micromapIndices;
};

/// Fully parsed Aether opacity-micromap asset (a `.micromap.toml` + its `.omm`
/// text sidecar). GPU-agnostic — pure CPU buffers the consumer uploads, one
/// `OpacityMicromapGroup` (one future `VkMicromapEXT`) per baked OBJ group.
struct OpacityMicromapData {
    std::vector<OpacityMicromapGroup> groups;
    std::uint16_t defaultSubdivisionLevel = 2;
    std::uint16_t defaultFormat = 1;
};

} // namespace aether
#endif // AETHER_TYPES_OPACITYMICROMAP_HPP
