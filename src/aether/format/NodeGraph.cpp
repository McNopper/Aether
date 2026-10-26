#include "aether/format/NodeGraph.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether {
namespace {

/// Sentinel parent index for root nodes.
constexpr std::size_t kNoParent = std::numeric_limits<std::size_t>::max();

} // namespace

bool resolveNodes(SceneDesc& desc, std::string* error) {
    if (desc.nodes.empty()) {
        return true;
    }

    // ── Validate: unique ids, known parents ─────────────────────────────────
    std::unordered_map<std::string, std::size_t> index;
    index.reserve(desc.nodes.size());
    for (std::size_t i = 0; i < desc.nodes.size(); ++i) {
        const auto [it, inserted] = index.emplace(desc.nodes[i].id, i);
        if (!inserted) {
            if (error != nullptr) {
                *error = "duplicate node id '" + desc.nodes[i].id + "'";
            }
            return false;
        }
    }

    std::vector<std::size_t> parentIndex(desc.nodes.size(), kNoParent);
    for (std::size_t i = 0; i < desc.nodes.size(); ++i) {
        const std::string& parent = desc.nodes[i].parent;
        if (parent.empty()) {
            continue; // root
        }
        const auto it = index.find(parent);
        if (it == index.end()) {
            if (error != nullptr) {
                *error = "node '" + desc.nodes[i].id + "' references unknown parent '" + parent + "'";
            }
            return false;
        }
        parentIndex[i] = it->second;
    }

    // ── Validate: cycle-free (color-marked parent-chain walks) ──────────────
    enum class Mark : std::uint8_t { Unvisited, InProgress, Done };
    std::vector<Mark> marks(desc.nodes.size(), Mark::Unvisited);
    for (std::size_t start = 0; start < desc.nodes.size(); ++start) {
        if (marks[start] != Mark::Unvisited) {
            continue;
        }
        std::vector<std::size_t> path;
        std::size_t cur = start;
        while (cur != kNoParent && marks[cur] == Mark::Unvisited) {
            marks[cur] = Mark::InProgress;
            path.push_back(cur);
            cur = parentIndex[cur];
        }
        // InProgress can only stem from this same walk: the chain revisited it.
        if (cur != kNoParent && marks[cur] == Mark::InProgress) {
            if (error != nullptr) {
                *error = "node cycle detected at '" + desc.nodes[cur].id + "'";
            }
            return false;
        }
        for (const std::size_t n : path) {
            marks[n] = Mark::Done;
        }
    }

    // ── Resolve: depth-first compound, emit meshed nodes as instances ───────
    std::vector<std::vector<std::size_t>> children(desc.nodes.size());
    std::vector<std::size_t> roots;
    for (std::size_t i = 0; i < desc.nodes.size(); ++i) {
        if (parentIndex[i] == kNoParent) {
            roots.push_back(i);
        } else {
            children[parentIndex[i]].push_back(i);
        }
    }

    // World = parent_world * local (T × R × S, the instance convention): the
    // local translation is scaled then rotated into the parent frame, the
    // local rotation composes on the parent's, the scales multiply per-axis.
    std::vector<Vec3> worldT(desc.nodes.size());
    std::vector<Quat> worldR(desc.nodes.size());
    std::vector<Vec3> worldS(desc.nodes.size());

    // Pre-order DFS (children in declaration order): a parent's world TRS is
    // always stored before any of its children is visited.
    std::vector<std::size_t> stack(roots.rbegin(), roots.rend());
    while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();

        const std::size_t p = parentIndex[i];
        const Vec3 pT = (p == kNoParent) ? Vec3{0.0F, 0.0F, 0.0F} : worldT[p];
        const Quat pR = (p == kNoParent) ? Quat{0.0F, 0.0F, 0.0F, 1.0F} : worldR[p];
        const Vec3 pS = (p == kNoParent) ? Vec3{1.0F, 1.0F, 1.0F} : worldS[p];

        const NodeDesc& node = desc.nodes[i];
        worldS[i] = pS * node.scale;
        worldR[i] = pR * node.rotation;
        worldT[i] = pT + pR * (pS * node.translation);

        if (!node.meshName.empty()) {
            InstanceDesc inst{};
            inst.meshName = node.meshName;
            inst.translation = worldT[i];
            inst.rotation = worldR[i];
            inst.scale = worldS[i];
            inst.materialName = node.materialName;
            inst.groupMaterials = node.groupMaterials;
            desc.instances.push_back(std::move(inst));
        }

        for (std::size_t c = children[i].size(); c > 0; --c) {
            stack.push_back(children[i][c - 1]); // reversed: pop-back keeps declaration order
        }
    }

    return true;
}

} // namespace aether
