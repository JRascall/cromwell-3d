/* MaterialId.hpp — what a renderable calls the surface it is made of.
 *
 * SINGLE RESPONSIBILITY: name a material in a way that carries no material
 * SYSTEM with it, so the scene API can mention one without depending on how
 * materials are stored today.
 *
 * ===================== PUBLIC API — see RenderScene.hpp ====================
 *
 * This type appears in RenderableDesc, so it ships to licensees and is
 * expensive to change later. That is the whole reason it exists rather than
 * SurfaceKind being written into the renderable.
 *
 * ================== WHY NOT JUST PASS SurfaceKind AROUND? =================
 *
 * Because SurfaceKind is a closed enum of ELEVEN THINGS THIS GAME HAS — road,
 * grass, wall, window, cover, ramp, block, canopy, portal, ladder, body — and
 * MIGRATION.md §4.7 is the plan to replace it with authored `.mat` files, per
 * material shaders and material instances. A scene API written against the
 * enum would have to change on the day that lands, and it is the one part of
 * the renderer that cannot change once a studio has shipped against it.
 *
 * So the renderable names a material by an OPAQUE ID and the material system
 * decides what an id means. Today DeviceMaterials answers it out of an array
 * indexed by SurfaceKind; tomorrow it is an index into a table of loaded
 * materials, and nothing in the scene, the culler or the sort notices.
 *
 * ========================== ZERO IS ALWAYS NULL ===========================
 *
 * The same convention rhi/Handles.hpp states at length, for the same reason:
 * a default-constructed id must be invalid so that a struct nobody remembered
 * to fill fails loudly rather than silently drawing as material zero — which
 * on this board would be `Road`, and would read as a shading bug rather than
 * as a missing assignment. The material system adds one on the way out and
 * subtracts it on the way in; that is one arithmetic operation in the one
 * place that knows the interior meaning.
 *
 * A renderable carrying an invalid id still DRAWS — with the pipeline's default
 * material block — and is complained about once. See the note in
 * RenderScene.hpp on why silence is the wrong failure here; Source's
 * RENDER_GROUP_OTHER is the counterexample this project is deliberately
 * copying the opposite of.
 */
#pragma once

#include <cstdint>

namespace cromwell {

/* One integer, one meaning, and no way to pass it where an unrelated index is
 * wanted. The same trick Handles.hpp plays: these are all "an unsigned int"
 * underneath, so a transposed pair of arguments would compile, run and produce
 * a wrong picture rather than an error. */
struct MaterialId {
    std::uint32_t value = 0;

    constexpr bool valid() const { return value != 0; }
    constexpr explicit operator bool() const { return valid(); }

    friend constexpr bool operator==(MaterialId a, MaterialId b) { return a.value == b.value; }
    friend constexpr bool operator!=(MaterialId a, MaterialId b) { return a.value != b.value; }

    /* ORDERED, because the opaque sort groups by material to cut state changes
     * and needs to compare two of them. It is a comparison over the id and
     * carries no claim about the materials themselves — two ids that sort
     * adjacently are not similar, they are merely batched. */
    friend constexpr bool operator<(MaterialId a, MaterialId b) { return a.value < b.value; }
};

}  // namespace cromwell
