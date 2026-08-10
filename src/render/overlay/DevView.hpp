/* DevView.hpp — the F1 developer UI, drawn with Dear ImGui.
 *
 * SINGLE RESPONSIBILITY: own the ImGui context and draw the dev controls.
 *
 * A TOOLBAR, NOT A WINDOW. There is far more here than one panel can hold
 * without becoming a scroll bar, so the top strip carries a button per
 * category and each opens its own window. Nothing is open at startup: the
 * default state of a dev UI should be out of the way.
 *
 * IT DECIDES NOTHING. Anything with a keyboard shortcut or a side effect
 * leaves as a DevRequest and is applied by Application exactly where the
 * keypress would have been applied, so there is one implementation of what
 * "toggle cover" means. What it does edit in place is state nothing else
 * observes: the view layers, and the tuning structs, which exist precisely so
 * a number can be moved without a rebuild.
 *
 * imgui.h is deliberately absent from this header. Nothing outside DevView.cpp
 * should have to know the UI library exists.
 */
#pragma once

#include "render/lighting/SunLight.hpp"
#include "render/overlay/Hud.hpp"
#include "render/overlay/RenderEffects.hpp"
#include "render/overlay/ViewLayers.hpp"
#include "render/post/AmbientOcclusion.hpp"
#include "render/ribbon/RibbonTuning.hpp"

#include <memory>
#include <optional>

namespace xcom {

class WebSurface;

/* One frame's worth of "the panel asked for this". Cleared by the reader. */
struct DevRequests {
    bool toggleCutaway   = false;
    bool toggleCover     = false;
    bool toggleLos       = false;
    bool toggleGrenade   = false;
    bool toggleOcclusion = false;
    bool toggleBake      = false;
    bool toggleFlatView  = false;
    bool cycleRing       = false;
    bool resetWorld      = false;

    /* Which probe the cubemap strip shows. Without it the strip can only ever
     * display probe 0 — the outdoor fallback — and the interior layers, which
     * are the ones worth checking, are unreachable. */
    bool cyclePreviewProbe = false;

    /* The baked sun cannot notice that the sun moved — re-running it is an
     * action, and this is the button. */
    bool rebakeSun       = false;

    std::optional<int>   isoLevel;       /* absolute, unlike the 1/2/3 keys */
    std::optional<float> sunAzimuth;     /* degrees, absolute              */
    std::optional<float> sunElevation;

    /* ---- the decal tool ----------------------------------------------------
     * The panel carries the SETTINGS and asks; Application does the picking,
     * the previewing and the placing, because only it has the camera, the mouse
     * ray and the world. Same division as every other request here — the UI
     * decides nothing. */
    struct DecalPlacement {
        int   material  = 0;      /* index into DevDecalTool::materialNames */
        float size      = 1.5f;   /* world units (tiles), square           */
        float rotation  = 0.0f;   /* degrees about the surface normal      */
        float opacity   = 1.0f;
        float roughness = 0.75f;
        float emissive  = 0.0f;

        /* Go round corners, or stop at the edge of the face. See Decal::wrap —
         * the two are different looks, not a quality setting. */
        bool  wrap      = true;
    };

    /* PRESENT MEANS ARMED, and it is re-sent every frame the tool is live —
     * not once when a button is clicked.
     *
     * That is forced by how requests work here: Application drains this struct
     * at the top of each frame, so anything it must keep knowing has to keep
     * arriving. It is also the right shape for the tool. Arming is a MODE, and
     * while it is on the brush's current settings are needed continuously,
     * because the preview under the cursor has to answer "what will I get" as
     * the sliders move — not what the settings were when the button was hit. */
    std::optional<DecalPlacement> decalBrush;

    bool clearDecals = false;
};

/* ---- what the decal tool needs to SHOW ------------------------------------
 * Read-only, rebuilt each frame by Application from the live DecalSet — the
 * same borrowed-pointer arrangement DevTextures uses, and for the same reason:
 * the panel needs to name things it does not own and must not outlive. */
struct DevDecalTool {
    static constexpr int kMaxMaterials = 16;

    const char* materialNames[kMaxMaterials] = {};
    int         materialCount = 0;

    int  placedCount = 0;

    /* False when the decal shader or its buffer failed to come up, in which
     * case the panel says so rather than offering a button that does nothing. */
    bool available = false;

    /* Whether the cursor is currently over a surface a decal could go on. The
     * place button is disabled otherwise, so "nothing happened" is visible
     * BEFORE the click rather than being mistaken for a broken tool. */
    bool cursorOnSurface = false;
};

/* ---- the texture inspector -------------------------------------------------
 *
 * Every intermediate the renderer produces, shown as an image. The shadow map,
 * the transmission plane, the occlusion buffer, the lightmap atlas, the
 * reflection probe: all of them are textures whose contents have been argued
 * about from the lit image, repeatedly and slowly. A sampler that was never
 * bound, an atlas seven times coarser than the map it replaced, a colour plane
 * reading zero everywhere — each of those cost a debugging session and each
 * would have been obvious here in a second.
 *
 * Names and notes are borrowed pointers to string literals; nothing is copied
 * and nothing is owned. Rebuilt each frame by Application, which is the only
 * place that knows what exists. */
struct DevTextureView {
    const char* name    = "";
    const char* note    = "";
    Texture2D   texture = {};
};

struct DevTextures {
    static constexpr int kMaximum = 16;

    DevTextureView entries[kMaximum];
    int            count = 0;

    void add(const char* name, Texture2D texture, const char* note = "")
    {
        if (count < kMaximum) entries[count++] = DevTextureView{ name, note, texture };
    }
};

/* Everything the panels edit directly, gathered so draw() takes four arguments
 * rather than a dozen. References, so there is no copy to write back. */
struct DevTunables {
    SunLight&                 sun;
    RibbonTuning&             ribbon;
    AmbientOcclusion::Tuning& occlusion;
    float&                    exposure;

    /* One switch per LIGHTING TERM, edited in place by the rendering panel.
     * Distinct from ViewLayers, which is one switch per PASS — see
     * RenderEffects.hpp for why that distinction earns its own struct. */
    RenderEffects&            effects;
};

class DevView {
public:
    /* Declared, not defaulted: BrowserInput below is an incomplete type here
     * and unique_ptr needs to see its destructor. Both live in DevView.cpp. */
    DevView();
    ~DevView();

    DevView(const DevView&) = delete;
    DevView& operator=(const DevView&) = delete;

    /* After InitWindow, before the first frame. False if the backend failed,
     * in which case every other call here is a no-op and the app runs on. */
    bool setup(int storeys);
    void shutdown();

    /* Between BeginDrawing and EndDrawing, after everything it describes.
     *
     * `browser` may be null — CEF is optional at build time and degrades at
     * run time, and the tab simply says so when there is nothing behind it. */
    void draw(const HudModel& model, ViewLayers& layers,
              DevTunables tunables, const DevTextures& textures,
              const DevDecalTool& decalTool,
              DevRequests& requests, WebSurface* browser = nullptr);

    void setVisible(bool visible) { visible_ = visible; }
    void toggleVisible() { visible_ = !visible_; }
    bool visible() const { return visible_; }

    /* True when the cursor or the keyboard belongs to the UI this frame.
     * Application uses them to stop a click on a slider from also ordering a
     * soldier across the map. Valid from the frame after the first draw. */
    bool wantsMouse() const;
    bool wantsKeyboard() const;

    /* How much of the top of the screen the toolbar is occupying, so whatever
     * else draws up there can get out of its way. Zero while hidden. */
    float toolbarHeight() const { return visible_ ? toolbarHeight_ : 0.0f; }

private:
    /* The strip along the top. Returns nothing; it edits open_. */
    void drawToolbar();

    /* One per category button. Each begins and ends its own window. */
    void drawLayersPanel(ViewLayers& layers);

    /* Every contribution to a surface's colour, in one place: the shader terms
     * from RenderEffects plus the four that live elsewhere because they are
     * whole passes (shadows, probes, SSAO, the sun bake). Gathering them is the
     * point — chasing an artefact means removing terms one at a time, and
     * hunting four panels for the switches is most of the cost of doing it. */
    void drawRenderingPanel(const HudModel& model, ViewLayers& layers,
                            DevTunables& tunables, DevRequests& requests);
    void drawViewPanel(const HudModel& model, DevRequests& requests);
    void drawSunPanel(const HudModel& model, DevTunables& tunables,
                      DevRequests& requests);
    void drawRibbonPanel(DevTunables& tunables);
    void drawPostPanel(const HudModel& model, ViewLayers& layers,
                       DevTunables& tunables, DevRequests& requests);
    void drawScenePanel(const HudModel& model, DevRequests& requests);
    void drawTexturePanel(const DevTextures& textures);

    /* Point at the world and stick a decal to it. The one panel here that
     * AUTHORS something rather than inspecting it, which is why the placement
     * settings live on the panel and the placement itself does not. */
    void drawDecalPanel(const DevDecalTool& tool, ViewLayers& layers,
                        DevRequests& requests);

    /* The embedded browser: a URL bar and the page, sized so that one texel of
     * the page lands on one pixel of the screen. */
    void drawBrowserPanel(WebSurface* browser);
#if XC_HAVE_WEB
    /* The half that needs a live surface, split out so the window frame and
     * its "CEF did not start" message are written once. */
    void drawBrowserContents(WebSurface* browser);
#endif

    /* Places a category window under the toolbar the first time it opens,
     * cascaded by `slot` so two of them do not land on top of each other. */
    void placePanel(int slot, float width) const;

    bool ready_    = false;

    /* Closed until asked for. A dev panel that opens itself is in the way of
     * every run that was not about the panel — including every screenshot. */
    bool visible_  = false;
    bool showDemo_ = false;
    int  storeys_  = 1;

    /* Measured from the drawn toolbar, so the panels sit under it whatever the
     * font and padding turn out to be. */
    float toolbarHeight_ = 32.0f;

    struct OpenPanels {
        bool layers = false;
        bool rendering = false;
        bool view   = false;
        bool sun    = false;
        bool ribbon = false;
        bool post   = false;
        bool scene  = false;
        bool textures = false;
        bool decals   = false;
        bool browser  = false;
    } open_;

    /* The decal tool's live settings. They persist across frames because they
     * are a BRUSH — you set a size and a material once and then place several,
     * and a panel that reset itself after every placement would be unusable. */
    DevRequests::DecalPlacement decalBrush_;

    /* Whether the tool is armed. A MODE, not an action: while it is on, a ghost
     * of the decal follows the cursor and a click in the world commits it.
     *
     * The button used to place immediately, which is the wrong shape for the
     * job — the one thing you need to know before committing a projected decal
     * is where it lands and how it wraps, and a button that places it sight
     * unseen makes that a matter of placing, looking, undoing and guessing
     * again. It also silently placed onto whatever the cursor happened to be
     * over while you were reaching for the panel. */
    bool decalArmed_ = false;

    /* Preview height in the inspector, so a 4096 shadow map and a 1x1 fallback
     * can sit in the same list without either being useless. */
    float previewHeight_ = 180.0f;

    /* ---- the browser tab ----------------------------------------------- */

    /* Edited by the URL bar. Only pushed into the page on enter, and only
     * refreshed from the page while the field is not being typed into — a box
     * that rewrites itself mid-word is unusable. */
    char urlField_[1024] = "https://www.google.com";
    bool urlEditing_ = false;

    /* A resize is a full relayout and repaint inside Chromium, so dragging the
     * window edge must not order one per frame. The wanted size is remembered
     * and applied once it has stopped changing. */
    /* Every character handed to the page since the panel opened. The one part
       of the path a headless test cannot reach, because it starts in ImGui's
       input queue: if this stays at zero while keys are pressed, the problem
       is here and not in the browser. */
    int    charsForwarded_ = 0;

    int    pendingWidth_  = 0;
    int    pendingHeight_ = 0;
    double pendingSince_  = 0.0;

    /* Carried between frames by the pointer router: focus and double-click
     * timing outlive a frame, hover does not. Opaque here on purpose — the
     * type lives in WebInput.hpp and this header does not want it. */
    struct BrowserInput;
    std::unique_ptr<BrowserInput> browserInput_;
};

}  // namespace xcom
