#ifndef AETHER_FORMAT_NODEGRAPH_HPP
#define AETHER_FORMAT_NODEGRAPH_HPP

#include <string>

#include "aether/types/SceneDesc.hpp"

namespace aether {

/// Validate and resolve the node graph (NH1): compound parent transforms down
/// the tree and append one flat instance per meshed node, so SceneDesc keeps
/// exposing the same fully-resolved flat instance list the consumers read.
///
/// Shared machinery, not public format API: SceneParser runs it once per
/// parse; SceneAnimator::evaluate re-runs it on every evaluation, after the
/// sampled TRS has been written into the node locals.
///
/// Errors (reported in @p error with the offending id named; @c false return):
/// a duplicate node id, an unknown parent id, a parent cycle. All validation
/// happens before any mutation, so a @c false return leaves @p desc
/// untouched. Scenes without nodes are untouched — their flat instance list
/// is exactly as authored.
[[nodiscard]] bool resolveNodes(SceneDesc& desc, std::string* error);

} // namespace aether
#endif // AETHER_FORMAT_NODEGRAPH_HPP
