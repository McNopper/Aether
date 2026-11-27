#ifndef AETHER_FORMAT_NODEGRAPH_HPP
#define AETHER_FORMAT_NODEGRAPH_HPP

#include <cstddef>
#include <limits>
#include <string>

#include "aether/types/SceneDesc.hpp"

namespace aether {

/// Validate and resolve the node graph (NH1): compound parent transforms down
/// the tree, append one flat instance per meshed node, and record every node's
/// world matrix into `desc.nodeWorlds` (ANI4 - the skin evaluation reads the
/// grouping/joint nodes that emit no instance), so SceneDesc keeps
/// exposing the same fully-resolved flat instance list the consumers read.
///
/// Shared machinery, not public format API: SceneParser runs it once per
/// parse; SceneAnimator::evaluate re-runs it on every evaluation, after the
/// sampled TRS has been written into the node locals.
///
/// Errors (reported in @p error with the offending id named; @c false return):
/// a duplicate node id, an unknown parent id, a parent cycle. All validation
/// happens before any mutation, so a @c false return leaves @p desc
/// untouched (including `nodeWorlds`). Scenes without nodes keep their flat
/// instance list exactly as authored and `nodeWorlds` empty.
///
/// @p authoredInstanceCount is the number of authored flat instances at the
/// front of `desc.instances`; the resolution appends one instance per meshed
/// node after them. Entries beyond that count when the function is entered
/// are node-derived instances of an earlier resolution: they are overwritten
/// in place, in the same order, instead of being destroyed and re-created, so
/// a repeated evaluation (SceneAnimator::evaluate into a reused output)
/// allocates nothing for them. The list ends at `authoredInstanceCount` plus
/// one entry per meshed node. The default treats every existing entry as
/// authored (the parse, which only appends).
[[nodiscard]] bool resolveNodes(SceneDesc& desc,
                                std::string* error,
                                std::size_t authoredInstanceCount = std::numeric_limits<std::size_t>::max());

} // namespace aether
#endif // AETHER_FORMAT_NODEGRAPH_HPP
