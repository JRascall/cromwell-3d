#include "cromwell/ui/core/UiContext.hpp"

#include <algorithm>

namespace cromwell::ui {
namespace {

/* Widget state is dropped after this many frames without being drawn. At 60 Hz
 * that is five seconds — long enough that switching tabs and switching back
 * keeps a fade, short enough that a session cannot accumulate state for screens
 * it visited once. */
constexpr std::uint64_t kStateLifetimeFrames = 300;

/* How often the sweep runs. Every frame would be a full map walk for nothing;
 * once a second is invisible and keeps the map from growing without bound. */
constexpr std::uint64_t kRetireIntervalFrames = 60;

}  // namespace

HoverFade& WidgetState::auxFade(int index)
{
    const int slot = std::clamp(index, 0, static_cast<int>(std::size(auxFades_)) - 1);
    return auxFades_[slot];
}

void UiContext::beginFrame(const UiInput& input)
{
    input_ = input;
    ++frameIndex_;

    /* Clamped rather than trusted. A zero — which is what a driver reports
     * before it has resolved the monitor's content scale, and what a
     * default-constructed input carries — would collapse every dimension in the
     * UI to nothing, and the symptom would be an invisible interface rather
     * than an obviously wrong one. The upper bound is a guard on the font
     * atlas: scale multiplies into rasterisation sizes. */
    scale_ = std::clamp(input.scale, 0.25f, 8.0f);

    drawList_.clear();

    /* The hot id is rebuilt from scratch every frame as widgets are submitted;
     * the active id is NOT, because it has to survive between the press and the
     * release that ends it. */
    hotId_ = 0;

    /* A press that ended anywhere releases the capture. Doing it here rather
     * than inside interact() means a control that stopped being drawn while
     * held does not strand the capture forever. */
    if (!input_.mouseDown) {
        activeId_ = 0;
    }

    retireStaleState();
}

void UiContext::endFrame()
{
    /* Nothing deferred today. The call exists so beginFrame has a visible
     * partner at every call site, and so anything that later needs to run after
     * all widgets have been submitted — a tooltip layer, a deferred popup — has
     * one obvious place to go rather than being bolted onto the painter. */
}

UiId UiContext::id(std::string_view name)
{
    /* FNV-1a, 64-bit. */
    std::uint64_t hash = 1469598103934665603ull;
    for (const char character : name) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 1099511628211ull;
    }
    /* Zero is the "no widget" sentinel, so a name that hashes to it is nudged
     * rather than allowed to mean "nothing is hot". */
    return hash == 0 ? 1ull : hash;
}

UiId UiContext::id(std::string_view name, int index)
{
    std::uint64_t hash = UiContext::id(name);
    hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(index));
    hash *= 1099511628211ull;
    return hash == 0 ? 1ull : hash;
}

UiId UiContext::childId(UiId parent, std::string_view part)
{
    std::uint64_t hash = parent;
    for (const char character : part) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1ull : hash;
}

WidgetState& UiContext::state(UiId id)
{
    WidgetState& entry = states_[id];
    entry.setLastFrame(frameIndex_);
    return entry;
}

bool UiContext::isHovered(const UiRect& bounds) const
{
    /* Geometric, and clipped by whatever the draw list is clipping to — a
     * control scrolled out of a panel is not hoverable even though its rect
     * still says where it would have been. */
    return bounds.contains(input_.mousePosition)
        && drawList_.clip().contains(input_.mousePosition);
}

InteractionResult UiContext::interact(UiId id, const UiRect& bounds)
{
    InteractionResult result;
    result.hovered = isHovered(bounds);

    if (result.hovered) {
        /* Last claim wins — see the header note on painter's order. */
        hotId_ = id;
    }

    if (result.hovered && input_.mousePressed && activeId_ == 0) {
        activeId_ = id;
        result.clicked = true;   /* fires on press; see the header */
    }

    result.held = (activeId_ == id) && input_.mouseDown;

    return result;
}

void UiContext::retireStaleState()
{
    if (frameIndex_ % kRetireIntervalFrames != 0) {
        return;
    }
    for (auto it = states_.begin(); it != states_.end();) {
        const bool stale = frameIndex_ > it->second.lastFrame() + kStateLifetimeFrames;
        /* Never retire the control currently holding the pointer, however long
         * the press has lasted. */
        it = (stale && it->first != activeId_) ? states_.erase(it) : std::next(it);
    }
}

}  // namespace cromwell::ui
