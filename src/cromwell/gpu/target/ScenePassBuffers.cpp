#include "cromwell/gpu/target/ScenePassBuffers.hpp"

#include "cromwell/diag/Logger.hpp"

#include <algorithm>

namespace cromwell {

bool ScenePassBuffers::create(int width, int height)
{
    const int w = std::max(width, 1);
    const int h = std::max(height, 1);

    depth_.resize(w, h);
    if (!depth_.valid()) {
        LOGGER->warn("scene pass buffers: no depth prepass at {}x{}; screen-space effects "
                     "are unavailable for this view",
                     w, h);
        return false;
    }

    /* SHADERS ONCE, TARGETS PER RESIZE. load() compiles; resize() allocates.
     * Calling load() from resize would recompile the occlusion shaders every
     * time a window edge is dragged. */
    if (!loaded_) {
        occlusion_.load();
        loaded_ = true;
    }
    occlusion_.resize(w, h);

    /* PARTIAL SUCCESS IS FINE HERE, and is why this is not folded into the
     * return value: a view with a working prepass and no DBuffer is a view with
     * occlusion and no decals, which is a perfectly good picture. See the
     * header. */
    decals_.resize(w, h);

    return true;
}

bool ScenePassBuffers::resize(int width, int height)
{
    const int w = std::max(width, 1);
    const int h = std::max(height, 1);
    if (valid() && depth_.width() == w && depth_.height() == h) return true;
    return create(w, h);
}

}  // namespace cromwell
