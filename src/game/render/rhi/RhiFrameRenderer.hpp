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

#include "cromwell/diag/DeviceTexturePreviews.hpp"
#include "cromwell/lighting/SunLight.hpp"
#include "cromwell/overlay/RenderEffects.hpp"
#include "cromwell/post/AmbientOcclusion.hpp"
#include "cromwell/post/BloomTuning.hpp"
#include "cromwell/post/ToneMapPass.hpp"
#include "game/render/ribbon/RibbonTuning.hpp"
#include "cromwell/render/RenderAssets.hpp"
#include "cromwell/render/RenderScene.hpp"
#include "cromwell/render/ScenePipeline.hpp"
#include "cromwell/render/View.hpp"
#include "cromwell/ui/paint/DeviceUiPainter.hpp"
#include "cromwell/decal/DecalSet.hpp"
#include "game/render/rhi/RhiBodies.hpp"
#include "game/render/rhi/RhiDecals.hpp"
#include "game/render/dev/DevView.hpp"
#include "game/render/dev/DeviceImGuiRenderer.hpp"
#include "game/render/rhi/RhiOverlays.hpp"
#include "game/render/scene/CutawayView.hpp"
#include "game/render/rhi/RhiStatics.hpp"
#include "game/render/ui/WidgetGallery.hpp"

#include <vector>

namespace cromwell { class IPlatform; }

namespace game {

struct FrameView;
class GameUi;
class Unit;
class UnitRoster;
class World;

class RhiFrameRenderer final {
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
    /* NON-CONST SINCE THE DEV PANEL LANDED, and the header note above needs
     * amending rather than deleting: a renderer still only READS the world's
     * lighting to draw it. What writes is the dev panel, which the renderer
     * merely hosts - DevTunables holds a live SunLight& so a slider moves the
     * one sun in the process rather than a copy that reverts next frame. The
     * borrowing is unchanged; only the constness is. */
    /* THE ONE DECAL SET, BORROWED, and for the same reason as the sun and the
     * UI above. PlayerController places into it, the dev panel's brush previews
     * against it and Application clears it — all of them gameplay reaching for
     * game state, none of which should have to know which renderer is drawing.
     * This renderer MIRRORS it into the scene each frame; see RhiDecals. */
    RhiFrameRenderer(cromwell::IPlatform& platform, cromwell::SunLight& sun,
                     GameUi& ui, cromwell::DecalSet& decals);
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

    /* THE DEV PANEL, BORROWED. Null until Application hands it over, and a
     * frame without one simply draws no panel — which is what a --shot run and
     * a shipped build both want. See FrameRenderer::devView on why it is
     * borrowed rather than owned. */
    void setDevView(DevView* devView) { devView_ = devView; }

    /* Forwarded, not reimplemented: how finely the selection outline's stencil
     * is rasterised is the ENGINE's quality dial, and the game's only business
     * with it is handing over the number a player or a launch flag chose. See
     * cromwell::ScenePipeline::withOutlineSupersample for what it costs and why
     * it exists at all. */
    void setOutlineSupersample(uint32_t factor) { pipeline_.withOutlineSupersample(factor); }

    /* ---- WHAT THE PANEL'S BUTTONS ASKED FOR, taken and cleared -------------
     *
     * THE SAME SIGNATURE FrameRenderer HAS, and that is the point rather than a
     * coincidence: Application drains both renderers unconditionally and never
     * asks which one is drawing. See DevRequests::mergeFrom, which carries the
     * argument.
     *
     * WHY THIS ACCESSOR IS THE WHOLE FIX FOR HALF THE PANEL. The panel has two
     * mechanisms and they fail differently — a CHECKBOX writes a flag directly
     * and works the moment a pass reads it, while a BUTTON raises a request
     * that something else must act on. Nothing on this path took the requests,
     * so every button was inert while the checkbox beside it worked, and the two
     * sit a foot apart in the same window. It reads as a rendering bug and is
     * not one. rhi/MIGRATION.md §4.5 states it at length.
     *
     * WHAT IS NOT IN HERE, deliberately: the requests this renderer already
     * ANSWERED. Those belong to state this object owns — a layer flag on the
     * camera it holds, the preview slot on the probe set its scene holds — and
     * sending them up to Application would have them come back down to the same
     * field through code whose other branch drives the raylib renderer's own
     * objects. Everything that is the GAME's state — reset world, cycle ring,
     * toggle LOS, the bake, the camera and the decal tool — is passed through
     * untouched, because a renderer must not reach into it. */
    DevRequests takeDevRequests();

    /* ---- THE WORLD IS NOT THE ONE THIS RENDERER BUILT ---------------------
     *
     * Say so and the static geometry, the world bounds and the probe volumes
     * are all rebuilt from the next frame's world.
     *
     * IT EXISTS BECAUSE THE RESET BUTTON NOW ARRIVES. `state.reset()` generates
     * a new building; this renderer built its statics on the first frame and had
     * no reason to ever build them again, so wiring the button without this
     * would leave the OLD building on screen with the NEW units walking through
     * it — a request that half worked, which is worse than one that did not
     * arrive at all.
     *
     * AND IT CLOSES §4.3's FIRST LEFTOVER. That entry says probe re-placement
     * "belongs with whatever eventually tells that renderer the statics
     * changed, and a second answer to 'has the world changed' invented before
     * then is two answers that drift". This is that thing, and the re-flood
     * rides on it rather than being a second signal. */
    void worldChanged();

    /* ---- THE SIGNED-IN PLAYER'S AVATAR, THROUGH THE DEVICE ----------------
     *
     * Same bytes FrameRenderer::uploadSteamAvatar is given, decoded through the
     * platform's IImageDecoder and uploaded as a device texture.
     *
     * IT CANNOT BE THE RAYLIB ONE, and the reason is the same trap the texture
     * previews hit: `DevSteam` used to carry a raylib `Texture2D`, and this
     * backend's ImTextureID is an RHI handle. Handing one across samples
     * whatever RHI resource happens to share that number — a WRONG PICTURE
     * rather than an error, which is the failure that survives review. The
     * panel's ids are now backend-neutral and each renderer fills its own.
     *
     * Called on BOTH renderers by Application, like the gallery toggle, because
     * only one draws and which one is a startup argument. Decoding a 184-pixel
     * jpeg twice, once, is not worth a branch that states that fact again. */
    void uploadSteamAvatar(const std::vector<unsigned char>& jpegBytes);

private:
    /* ---- THIS CLASS NO LONGER IMPLEMENTS ANYTHING ----------------------
     *
     * It was `final : public cromwell::IGeometrySource`, with a submit() the
     * engine called back per pass and a worldBounds() it asked for the extent.
     * Both are gone with the seam — rhi/MIGRATION.md §4.12 step 5.
     *
     * WHAT IS LEFT IS COMPOSITION, WHICH IS THE WHOLE POINT. This builds the
     * game's device geometry, describes the frame, names a view, and hands both
     * to the engine. There is no pass callback, no encoder, no GeometryPass,
     * and nothing here knows what a shadow map wants. */

    /* The dev panel, after the UI and last of all - it is a tool over a
     * finished frame and must stay usable on top of everything else. */
    void drawDevPanel(const FrameView& view);

    /* THIS RENDERER'S OWN INTERMEDIATES, blitted into something a panel can
     * sample. Empty while the panel is shut, and empty when the previews could
     * not start — in both cases the texture tab says "nothing to show", which
     * is true. See DeviceTexturePreviews. */
    DevTextures buildDevTextures();

    /* The UI, drawn after the resolve because it is display colour over a
     * tone-mapped scene rather than radiance in it. See rhi/ui.fs.glsl. */
    void drawUi(const FrameView& view);

    cromwell::IPlatform&      platform_;
    cromwell::SunLight&       sun_;
    GameUi&                   ui_;
    cromwell::DecalSet&       decals_;

    /* ---- the three lifetimes, in declaration order ----------------------
     *
     * DEVICE, then WORLD, then VIEW — see RenderAssets.hpp, which names them.
     * The order is load-bearing rather than tidy: the scene borrows the assets'
     * material table and the pipeline borrows the assets, so both must be
     * destroyed before the thing they borrow from. Declaring them the other way
     * round would be a teardown crash of exactly the kind §5 already records
     * against Application::run, and it would appear only on exit.
     *
     * ALL THREE ARE HERE FOR NOW BECAUSE THERE IS ONE OF EACH. When there are N
     * players the assets move up to whatever owns the device, the scene moves
     * to the World, and only the pipeline stays with a renderer. Nothing about
     * their interfaces changes when they do, which is the point of separating
     * them now. */
    cromwell::RenderAssets    assets_;
    cromwell::RenderScene     scene_;
    cromwell::ScenePipeline   pipeline_;
    cromwell::ui::DeviceUiPainter uiPainter_;
    WidgetGallery             uiGallery_;
    bool                      uiReady_ = false;
    bool                      uiFailed_ = false;

    RhiStatics  statics_;
    RhiBodies   bodies_;

    /* THE INTERFACE DRAWN INTO THE WORLD, and the last thing on this path that
     * had no answer. §4.4 called it half done; it is done, as renderables. */
    RhiOverlays overlays_;

    /* The game's decals, converted into the scene's set once a frame. */
    RhiDecals   rhiDecals_;

    /* ---- the dev panel --------------------------------------------------
     *
     * The panel itself is borrowed; only its BACKEND is ours, because rlImGui's
     * renderer half is exactly what this path replaces. See
     * DeviceImGuiRenderer.hpp. */
    DevView*            devView_ = nullptr;
    DeviceImGuiRenderer imgui_;
    bool                imguiReady_ = false;
    bool                imguiFailed_ = false;

    /* ---- what the texture panel shows on this path ----------------------
     *
     * ENGINE-SIDE, unlike the ImGui backend above, and the split is the one
     * DeviceImGuiRenderer's header draws: a backend has to live here because
     * cromwell may not link a UI toolkit, while turning a D32F shadow map into
     * something a panel can display names no toolkit at all and is work a
     * second project would otherwise write again. See DeviceTexturePreviews.
     *
     * THE PREVIEWS ARE ONLY BUILT WHILE THE PANEL IS OPEN. Nothing inside them
     * knows whether anybody is looking, and a dozen blits a frame for a window
     * that is shut is the kind of cost that gets found in a capture months
     * later. */
    cromwell::DeviceTexturePreviews previews_;
    bool                            previewsReady_ = false;
    bool                            previewsFailed_ = false;

    /* WHICH PROBE THE CUBEMAP STRIP SHOWS, cycled by the panel's button.
     *
     * HERE RATHER THAN ON DeviceProbeSet, which is where the raylib set keeps
     * it. A probe set describes a WORLD — see the lifetime argument in
     * RenderAssets.hpp — and which layer somebody happens to be looking at is
     * neither the world's business nor shared between two players looking at
     * it. It belongs with the panel that asks, and this is the object hosting
     * that panel. */
    int  previewProbe_ = 0;

    /* THE PROBE STRIP'S NOTE, in a member buffer rather than a formatted
     * temporary: DevTextures borrows its name and note POINTERS and copies
     * nothing, so a string that died at the end of the statement would be
     * describing somebody else's texture by the time the panel drew it. The
     * raylib path keeps the same buffer for the same reason. */
    char probePreviewNote_[192] = {};

    /* ---- the signed-in player's avatar, as a device texture --------------
     *
     * Ours rather than FrameRenderer's raylib one, because the two ImGui
     * backends do not share an id space — see uploadSteamAvatar above and the
     * note on DevTextureView. */
    cromwell::rhi::TextureHandle steamAvatar_;
    int                          steamAvatarWidth_ = 0;
    int                          steamAvatarHeight_ = 0;

    /* WHAT THE PANEL ASKED FOR. Collected so the panel's buttons do not write
     * into a temporary; ACTING on them is Application's, and on this path
     * nothing reads them yet - a button that does nothing is honest while the
     * systems behind it are unconverted, and a request written to a local
     * would not even be inspectable. */
    DevRequests devRequests_;

    /* ---- what the panel's sliders edit on THIS path ---------------------
     *
     * DevTunables holds live references, so the panel needs real objects to
     * point at. The sun is the process's one sun, borrowed. These three are
     * this renderer's own because the raylib renderer's copies drive passes
     * this path does not run.
     *
     * THE OCCLUSION TUNING IS §4.10'S DEBT ITEM ARRIVING EARLY, and it is a
     * genuine fix rather than a side effect: ScenePipeline::drawOcclusion still
     * has radius, bias and strength as copied constants, and the note in §5
     * records that inventing them here instead of borrowing was what made the
     * device path's SSAO visibly different. Now there is a live object for the
     * panel to move; wiring the pipeline to READ it is the rest of that item. */
    cromwell::AmbientOcclusion::Tuning occlusionTuning_;

    /* THE BLOOM'S FOUR KNOBS, live, for the same reason the occlusion tuning is
     * an object rather than four fields on the frame: DevTunables holds
     * references, and a slider writing into something rebuilt every frame would
     * move and revert. See BloomTuning. */
    cromwell::BloomTuning              bloomTuning_;
    RibbonTuning                       ribbonTuning_;
    float                              exposure_ = cromwell::ToneMapPass::kDefaultExposure;

    /* ONE SWITCH PER LIGHTING TERM, as opposed to ViewLayers' one per PASS -
     * see RenderEffects.hpp for why that distinction earns its own struct. The
     * device path does not read these yet either; they exist so the panel has
     * something real to write, and so that wiring them is a read rather than a
     * plumbing job. */
    cromwell::RenderEffects            effects_;
    bool       staticsBuilt_ = false;

    /* THE UNIT CUBE, WHICH SURVIVES A WORLD REBUILD. Separate from
     * staticsBuilt_ because worldChanged() clears that one: the building is
     * generated afresh, the cube every body is drawn with is not, and rebuilding
     * it would destroy the mesh the scene's body renderables still name. */
    bool       bodiesBuilt_ = false;
    bool       ready_ = false;
    bool       failed_ = false;
    bool       bodiesReported_ = false;

    /* ---- what one frame's body sync needs -------------------------------
     *
     * THESE USED TO BE LATCHED FOR A CALLBACK. The pipeline called submit()
     * with an encoder and a pass and nothing else, so every per-frame fact the
     * bodies needed had to be parked on this object first. Now they are read
     * within a single render() and could be locals — they stay members only
     * because the walking-body block below fills them in one place and the sync
     * reads them in another, which is one function's worth of state rather than
     * a callback's.
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
