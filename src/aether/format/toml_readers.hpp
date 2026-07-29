#ifndef AETHER_FORMAT_TOML_READERS_HPP
#define AETHER_FORMAT_TOML_READERS_HPP

#include <optional>
#include <toml++/toml.hpp>

#include "aether/types/Math.hpp"

namespace aether {

[[nodiscard]] inline std::optional<Vec3> asVec3(const toml::node& n) {
    const toml::array* arr = n.as_array();
    if (arr == nullptr || arr->size() != 3) {
        return std::nullopt;
    }
    const auto x = (*arr)[0].value<double>();
    const auto y = (*arr)[1].value<double>();
    const auto z = (*arr)[2].value<double>();
    if (!x || !y || !z) {
        return std::nullopt;
    }
    return Vec3{static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)};
}

} // namespace aether

#endif // AETHER_FORMAT_TOML_READERS_HPP
