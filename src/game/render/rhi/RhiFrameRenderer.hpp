/* RhiFrameRenderer.hpp — this game's side of the device renderer.
 *
 * SINGLE RESPONSIBILITY: own this game's device geometry, describe the frame,
 * and hand both to the engine's ScenePipeline.
 *
 * ===================== WHAT IS NOT IN THIS FILE ===========================
 *
 * The render targets, the shaders, the pipelines, the passes and the order they
 * run in. All of that is cromwell/render/ScenePipeline, because none of it is
 * about this game — a shadow map is a depth-only pass over the world from the
 * sun's point of view whether the world contains soldiers or tanks.
 *
 * IT USED TO BE HERE, and that was the mistake this file now exists to avoid
 * repeating. An engine whose frame sequence lives in the game is one that gets
 * copy-pasted into the next project and then diverges. Everything below is
 * either a mesh or a number the game genuinely decides.
 *
 * WATCH THIS FILE'S SIZE as passes are converted. If it grows when the depth
 * prepass or the lit pass lands, something engine-shaped has leaked back into
 * it — the pipeline should absorb those and this should not change at all.
 *
 * ======================= WHY TWO RENDERERS EXIST ==========================
 *
 * raylib binds shader inputs by its own naming convention, so a shader
 * converted to explicit bindings and a std140 block cannot be driven by it —
 * which means the pass must bind its own resources, which means its render
 * target must be a device texture, which pulls in whatever else writes that
 * target. The chain closes over most of the renderer, so converting in place
 * would leave the tree unable to draw a correct frame until every link was
 * done. Both exist, one is chosen at startup (`--renderer rhi`), and the raylib
 * path is deleted at parity.
 */
#pragma once

#include "cromwell/lighting/SunLight.hpp"
#include "cromwell/render/IGeometrySource.hpp"
#include "cromwell/render/ScenePipeline.hpp"
#include "cromwell/ui/paint/DeviceUiPainter.hpp"
#include "game/render/rhi/RhiBodies.hpp"
#include "game/render/rhi/RhiStatics.hpp"
#include "game/render/ui/WidgetGallery.hpp"

namespace cromwell { class IPlatform; }

namespace game {

struct FrameView;
class GameUi;
class Unit;
class UnitRoster;
class World;

class RhiFrameRenderer final : public cromwell::IGeometrySource {
public:
    /* THE SUN IS BORROWED, NOT OWNED — and that is the whole point of taking it
     * as an argument rather than declaring one below.
     *
     * There is exactly one sun in the process, it lives on FrameRenderer, and
     * the dev panel's azimuth and elevation sliders and the keyboard's nudge
     * keys write to it directly. A second SunLight here would be a second
     * answer to "where is the sun", and the symptom would be a device frame
     * that ignored every sun control while the raylib frame beside it obeyed
     * them — read as a broken lit pass rather than as two lights.
     *
     * `const` because a renderer reads the world's lighting and does not set
     * it. When the raylib path goes, this ownership moves to whatever survives
     * it; the reference does not change. */
    /* `ui` is BORROWED and WRITTEN THROUGH, unlike the sun. There is one UI
     * surface in the process — one font set, one context holding the hover and
     * drag state that makes a control feel continuous across frames — and a
     * second would be a second copy of every atlas and half the interaction
     * state. So this points the existing one at the device painter rather than
     * building its own; see GameUi::setPainter. */
    RhiFrameRenderer(cromwell::IPlatform& platform, const cromwell::SunLight& sun,
                     GameUi& ui);
    ~RhiFrameRenderer();

    void render(const FrameView& view);

    /* THE WIDGET KIT ON ONE SCREEN (F2, or --ui-gallery), and on this path it
     * is the only screen that draws text at all.
     *
     * That is not a coincidence, it is the reason it is here. A migration whose
     * UI can only be judged on the renderer being replaced can only be judged
     * by argument; the gallery's size ladder is the instrument that answers "is
     * the text sharp" with a picture, and the two renderers drawing the SAME
     * screen is what turns that from a matter of taste into a diff. See
     * rhi/MIGRATION.md §4.1.
     *
     * ITS OWN INSTANCE rather than FrameRenderer's, in the same spirit as
     * RhiStatics beside StaticsMesh: one renderer runs per process, the
     * duplication lasts until parity, and sharing it would mean hoisting
     * ownership into Application for the sake of a diagnostic. */
    void toggleUiGallery() { uiGallery_.toggleVisible(); }

private:
    /* ---- IGeometrySource: this game's world, for the engine's passes ---- */
    void submit(cromwell::rhi::ICommandEncoder& encoder,
                cromwell::GeometryPass pass) override;
    void worldBounds(cromwell::Vec3& minimum, cromwell::Vec3& maximum) const override;

    /* The bodies, at whatever storey cut the calling pass works under. Split
     * out because the sun's pass and the camera's differ only in that number
     * and in whether glass is drawn — and writing the call twice is how the two
     * eventually stop agreeing about which units exist. */
    /* `materials` is null for the sun's depth pass, which has no material block
     * to bind — and non-null for every shaded one, or the bodies inherit
     * whichever surface kind the statics bound last. See RhiBodies::submit. */
    void submitBodies(cromwell::rhi::ICommandEncoder& encoder, int maxStorey,
                      const cromwell::DeviceMaterials* materials) const;


    /* The UI, drawn after the resolve because it is display colour over a
     * tone-mapped scene rather than radiance in it. See rhi/ui.fs.glsl. */
    void drawUi(const FrameView& view);

    cromwell::IPlatform&      platform_;
    const cromwell::SunLight& sun_;
    GameUi&                   ui_;
    cromwell::ScenePipeline   pipeline_;
    cromwell::ui::DeviceUiPainter uiPainter_;
    WidgetGallery             uiGallery_;
    bool                      uiReady_ = false;
    bool                      uiFailed_ = false;

    RhiStatics statics_;
    RhiBodies  bodies_;
    bool       staticsBuilt_ = false;
    bool       ready_ = false;
    bool       failed_ = false;
    bool       bodiesReported_ = false;

    /* The cutaway the camera passes draw under. Held because submit() is called
     * back from inside the pipeline and has no FrameView in scope — the same
     * arrangement, and the same reason, as FrameRenderer::view_. */
    CutawayView cutaway_;

    /* ---- what submit() needs and cannot ask for -------------------------
     *
     * The pipeline calls back into submit() with an encoder and a pass, and
     * nothing else — by design, because a geometry source that could reach the
     * FrameView would be one that starts making decisions the pass already
     * made. So the handful of per-frame facts the bodies need are latched at
     * the top of render(), exactly as `cutaway_` above is.
     *
     * The roster is a pointer rather than a reference because there is no world
     * before the first frame, and a renderer that cannot be constructed until
     * there is one would have to be built somewhere other than startup. */
    const UnitRoster* roster_ = nullptr;
    const World*      world_ = nullptr;

    /* THE WALKING BODY, at the position the animator says rather than the cell
     * it logically stands on. Null whenever no move is playing, which is nearly
     * always — a move animation is a fraction of a second at the end of a turn
     * that lasts as long as the player takes. */
    const Unit* animating_ = nullptr;
    float       animatedX_ = 0.0f;
    float       animatedHeight_ = 0.0f;
    float       animatedY_ = 0.0f;

    /* The world's extent, cached at rebuild. The pipeline asks for it every
     * shadow pass and it only changes when the world does. */
    cromwell::Vec3 boundsMinimum_;
    cromwell::Vec3 boundsMaximum_;
};

}  // namespace game
