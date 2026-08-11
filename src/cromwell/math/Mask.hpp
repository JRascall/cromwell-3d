/* Mask.hpp — a set of up to 32 named categories, and the ids that index it.
 *
 * SINGLE RESPONSIBILITY: be a bitset with a type on it, plus somewhere to
 * record what the bits are called.
 *
 * ===================== WHY THIS IS A TEMPLATE ON A TAG =====================
 *
 * THE ENGINE HAS MORE THAN ONE KIND OF LAYER AND THEY MUST NOT MIX. Collision
 * layers say what a trace can hit; draw layers say what a camera renders. Both
 * are "up to 32 categories the GAME names", both want the same set algebra, and
 * an id from one is meaningless in the other — passing a collision layer where
 * a draw layer belongs is a bug that a plain `int` or a shared enum would
 * accept silently.
 *
 * So the mechanism is written once and stamped out per kind:
 *
 *     struct DrawLayerTag {};
 *     using DrawLayerId   = MaskId<DrawLayerTag>;
 *     using DrawLayerMask = Mask<DrawLayerTag>;
 *
 * Two distinct types, one implementation, and the compiler refuses the mix-up.
 *
 * ==================== WHY 32, AND WHY THE ENGINE NAMES NONE ================
 *
 * THIRTY-TWO is Unity's number for exactly this job and has been enough there
 * for projects far larger than any of ours. It also fits a mask in a register,
 * which is what lets one be passed by value into a per-candidate test without
 * anyone thinking about it.
 *
 * THE ENGINE DEFINES NO CATEGORIES. There is no `kUnits` here and there must not
 * be: what a layer MEANS is a project's decision, and an engine that shipped a
 * "units" layer would have decided that every game embedding it has units. A
 * shooter has none; an RTS has hundreds; a puzzle game has neither units nor
 * movement rings nor cover shields. The game declares its own ids and registers
 * names for them — see MaskNames, and the game's own draw-layer header for a
 * worked example.
 *
 * ============================ VALUE TYPES ==================================
 *
 * MaskId and Mask are pure values with total operations — there is no
 * combination of bits that is an invalid set — so they follow Vec2/Vec3's
 * documented exception and carry their representation as their interface.
 * MaskNames is not: it owns strings and outlives a frame, so it is a proper
 * class with accessors.
 *
 * FILED UNDER math/ because it is a small algebraic value type of the same
 * family as the vectors, not because a bitset is arithmetic. It has no better
 * home, and a folder holding one header would be worse.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace cromwell {

/* One category, as an index in [0, 32). Not an int, so two kinds of layer
 * cannot be swapped; not an enum, because the values are the project's. */
template <typename Tag>
class MaskId {
public:
    static constexpr int kCount = 32;

    constexpr MaskId() = default;
    constexpr explicit MaskId(int index) : index_(static_cast<std::uint8_t>(index)) {}

    constexpr int index() const { return index_; }
    constexpr bool valid() const { return index_ < kCount; }

    constexpr bool operator==(const MaskId& rhs) const { return index_ == rhs.index_; }
    constexpr bool operator!=(const MaskId& rhs) const { return index_ != rhs.index_; }

private:
    /* Defaults to an INVALID id rather than to category 0, so something that was
     * never assigned a layer cannot silently pass a filter that happens to
     * include the first one. An unset id should match nothing, and this makes
     * that the cost-free default. */
    std::uint8_t index_ = kCount;
};

/* A set of them. */
template <typename Tag>
class Mask {
public:
    using Id = MaskId<Tag>;

    constexpr Mask() = default;
    constexpr explicit Mask(std::uint32_t bits) : bits_(bits) {}

    static constexpr Mask none() { return Mask{ 0u }; }
    static constexpr Mask all() { return Mask{ 0xFFFFFFFFu }; }

    static constexpr Mask of(Id id)
    {
        return id.valid() ? Mask{ 1u << id.index() } : none();
    }

    constexpr std::uint32_t bits() const { return bits_; }
    constexpr bool empty() const { return bits_ == 0u; }

    constexpr bool has(Id id) const
    {
        return id.valid() && (bits_ & (1u << id.index())) != 0u;
    }

    /* `with`/`without` rather than operators, because `mask - other` would read
     * as arithmetic on something that is not a number. */
    constexpr Mask with(Id id) const { return Mask{ bits_ | Mask::of(id).bits_ }; }
    constexpr Mask without(Id id) const { return Mask{ bits_ & ~Mask::of(id).bits_ }; }

    /* Sets or clears by value, for a checkbox bound to one category. */
    constexpr Mask set(Id id, bool on) const { return on ? with(id) : without(id); }

    constexpr Mask operator|(Mask rhs) const { return Mask{ bits_ | rhs.bits_ }; }
    constexpr Mask operator&(Mask rhs) const { return Mask{ bits_ & rhs.bits_ }; }
    constexpr Mask operator~() const { return Mask{ ~bits_ }; }

    constexpr bool operator==(const Mask& rhs) const { return bits_ == rhs.bits_; }
    constexpr bool operator!=(const Mask& rhs) const { return bits_ != rhs.bits_; }

private:
    std::uint32_t bits_ = 0u;
};

/* What the categories are called.
 *
 * NAMES ARE NOT DECORATION. A mask that returns nothing, or everything, is
 * debugged by printing what it was, and thirty-two bits of hex is not an
 * answer. They are also what lets a dev panel ENUMERATE a project's layers
 * rather than hard-code a checkbox per category — which is the difference
 * between an engine feature and a list somebody has to maintain twice.
 *
 * Registered once at startup. Cold code; never read in a loop. */
template <typename Tag>
class MaskNames {
public:
    using Id = MaskId<Tag>;

    void name(Id id, std::string_view text)
    {
        if (!id.valid()) return;
        names_[static_cast<std::size_t>(id.index())] = std::string(text);
        if (id.index() >= highest_) highest_ = id.index() + 1;
    }

    /* Empty for a category nobody registered. Callers that want a placeholder
     * ask for `labelOf`. */
    std::string_view nameOf(Id id) const
    {
        if (!id.valid()) return {};
        return names_[static_cast<std::size_t>(id.index())];
    }

    /* Never empty: falls back to "layer N", because a blank label in a panel is
     * worse than a placeholder. */
    std::string labelOf(Id id) const
    {
        if (!id.valid()) return "invalid";
        const std::string& text = names_[static_cast<std::size_t>(id.index())];
        return text.empty() ? ("layer " + std::to_string(id.index())) : text;
    }

    /* Invalid id when nothing matches. For a console command or a config file —
     * NOT for per-frame code, which should hold the id it was given. */
    Id find(std::string_view text) const
    {
        for (int index = 0; index < Id::kCount; ++index) {
            if (names_[static_cast<std::size_t>(index)] == text) return Id(index);
        }
        return Id{};
    }

    /* One past the highest registered index, so a caller can walk exactly the
     * categories this project declared rather than all thirty-two. */
    int declaredCount() const { return highest_; }

    /* Every registered category, in index order — the enumeration a settings
     * panel is built from. `visit(id, name)`. */
    template <typename Visit>
    void forEach(Visit&& visit) const
    {
        for (int index = 0; index < highest_; ++index) {
            const std::string& text = names_[static_cast<std::size_t>(index)];
            if (text.empty()) continue;
            visit(Id(index), text);
        }
    }

private:
    std::array<std::string, MaskId<Tag>::kCount> names_{};
    int highest_ = 0;
};

}  // namespace cromwell
