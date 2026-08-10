#include "game/render/dev/DevView.hpp"

#include <cstdint>

#include "game/lattice/Lattice.hpp"
#include "cromwell/ribbon/RibbonConstants.hpp"

#if XC_HAVE_WEB
#include "cromwell/web/surface/WebInput.hpp"
#include "cromwell/web/surface/WebSurface.hpp"
#endif

#include "imgui.h"
#include "rlImGui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */
namespace {

constexpr float kPanelWidth = 340.0f;

/* A view-mode checkbox reports a TOGGLE, never a value: the panel does not own
 * that state, it only asks for the same flip the key would have caused. */
bool toggled(const char* label, bool current)
{
    bool value = current;
    return ImGui::Checkbox(label, &value);
}

/* Two layer switches on one line. Layers are a long list of short labels and
 * one per row pushes everything below them off the screen. */
void layerPair(const char* leftLabel, bool* left, const char* rightLabel, bool* right)
{
    const float column = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.5f;
    ImGui::Checkbox(leftLabel, left);
    if (!right) return;
    ImGui::SameLine(column);
    ImGui::Checkbox(rightLabel, right);
}

/* Ribbon lengths are authored in unreal units and the study file quotes them
 * that way, so the sliders speak uu and convert at the edge. A slider in
 * 0.052-of-a-tile is a slider nobody can check against the source. */
bool sliderUnrealUnits(const char* label, float* worldUnits, float minUu, float maxUu)
{
    float uu = *worldUnits / kUnrealUnit;
    if (!ImGui::SliderFloat(label, &uu, minUu, maxUu, "%.1f uu")) return false;
    *worldUnits = uu * kUnrealUnit;
    return true;
}

bool colourEdit(const char* label, Color* colour)
{
    float rgba[4] = { colour->r / 255.0f, colour->g / 255.0f,
                      colour->b / 255.0f, colour->a / 255.0f };
    if (!ImGui::ColorEdit4(label, rgba)) return false;

    colour->r = static_cast<unsigned char>(rgba[0] * 255.0f + 0.5f);
    colour->g = static_cast<unsigned char>(rgba[1] * 255.0f + 0.5f);
    colour->b = static_cast<unsigned char>(rgba[2] * 255.0f + 0.5f);
    colour->a = static_cast<unsigned char>(rgba[3] * 255.0f + 0.5f);
    return true;
}

bool tintEdit(const char* label, Vector3* tint)
{
    float rgb[3] = { tint->x, tint->y, tint->z };
    if (!ImGui::ColorEdit3(label, rgb)) return false;
    *tint = Vector3{ rgb[0], rgb[1], rgb[2] };
    return true;
}

}  // namespace

bool DevView::setup(int storeys)
{
    storeys_ = storeys > 0 ? storeys : 1;

    rlImGuiSetup(true);
    if (ImGui::GetCurrentContext() == nullptr) return false;

    /* No imgui.ini. Layout that persists between runs would mean a screenshot
     * depends on where the panels were left last session, and reproducible
     * frames are the whole point of the --shot path. */
    ImGui::GetIO().IniFilename = nullptr;

    ready_ = true;
    return true;
}

void DevView::shutdown()
{
    if (!ready_) return;
    rlImGuiShutdown();
    ready_ = false;
}

bool DevView::wantsMouse() const
{
    return ready_ && visible_ && ImGui::GetIO().WantCaptureMouse;
}

bool DevView::wantsKeyboard() const
{
    return ready_ && visible_ && ImGui::GetIO().WantCaptureKeyboard;
}

/* A GRID, NOT A CASCADE. Six stacked windows offset by a title bar each is
 * unreadable the one time it matters — when several are open at once — so the
 * first-use position spreads them three across and two down. They are movable
 * afterwards; this only has to be a sane opening hand. */
void DevView::placePanel(int slot, float width) const
{
    constexpr float kColumnStride = 392.0f;
    constexpr float kRowStride    = 300.0f;

    const float column = static_cast<float>(slot % 3);
    const float row    = static_cast<float>(slot / 3);

    ImGui::SetNextWindowPos(ImVec2(8.0f + column * kColumnStride,
                                   toolbarHeight_ + 8.0f + row * kRowStride),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_FirstUseEver);
}

/* ------------------------------------------------------------- the toolbar */
void DevView::drawToolbar()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, 0.0f));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##dev toolbar", nullptr, flags)) {
        /* An open category's button stays lit, so the strip doubles as the
         * answer to "what am I looking at". */
        const auto tab = [](const char* label, bool* open) {
            if (*open)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(label)) *open = !*open;
            if (*open) ImGui::PopStyleColor();
            ImGui::SameLine();
        };

        ImGui::TextDisabled("dev view");
        ImGui::SameLine();

        tab("layers", &open_.layers);
        tab("rendering", &open_.rendering);
        tab("view",   &open_.view);
        tab("sun",    &open_.sun);
        tab("ribbon", &open_.ribbon);
        tab("post",   &open_.post);
        tab("scene",  &open_.scene);
        tab("textures", &open_.textures);
        tab("decals", &open_.decals);
        tab("browser", &open_.browser);
        tab("steam", &open_.steam);

        ImGui::TextDisabled("|  %.0f fps  %.2f ms",
                            static_cast<double>(ImGui::GetIO().Framerate),
                            static_cast<double>(1000.0f / ImGui::GetIO().Framerate));

        toolbarHeight_ = ImGui::GetWindowHeight();
    }
    ImGui::End();
}

/* ---------------------------------------------------------------- panels */
void DevView::drawLayersPanel(ViewLayers& layers)
{
    placePanel(0, kPanelWidth);
    if (ImGui::Begin("layers", &open_.layers)) {
        layerPair("sky",      &layers.sky,      "world",    &layers.statics);
        layerPair("props",    &layers.props,    "units",    &layers.units);
        layerPair("shadows",  &layers.shadows,  "overlays", &layers.overlays);
        layerPair("rings",    &layers.ribbons,  "glow",     &layers.glow);
        layerPair("text HUD", &layers.hudText,  "reflections", &layers.reflections);

        ImGui::Separator();
        if (ImGui::Button("all on")) layers = ViewLayers{};
        ImGui::SameLine();
        if (ImGui::Button("clean")) {
            /* The world and nothing else — how a render is judged. */
            layers.overlays = false;
            layers.ribbons  = false;
            layers.glow     = false;
            layers.hudText  = false;
        }
        ImGui::TextDisabled("a layer off is off in every pass,\nthe shadow map included");
    }
    ImGui::End();
}

void DevView::drawRenderingPanel(const HudModel& model, ViewLayers& layers,
                                 DevTunables& tunables, DevRequests& requests)
{
    /* Slot 7, not 1: every other slot is already spoken for, and two panels
     * sharing one would open exactly on top of each other. */
    placePanel(7, kPanelWidth);
    if (ImGui::Begin("rendering", &open_.rendering)) {
        RenderEffects& effects = tunables.effects;

        ImGui::TextDisabled("what contributes to a surface's colour.\n"
                            "switch terms off one at a time to find\n"
                            "which one an artefact belongs to.");
        ImGui::Separator();

        ImGui::SeparatorText("direct");
        ImGui::Checkbox("sun", &effects.directSun);
        ImGui::SameLine();
        ImGui::Checkbox("shadow map", &layers.shadows);
        if (toggled("baked sun (B)", model.bakedSun)) requests.toggleBake = true;
        ImGui::SameLine();
        ImGui::TextDisabled("lightmap, replaces the shadow map");

        ImGui::SeparatorText("ambient");
        ImGui::Checkbox("sky diffuse", &effects.ambientDiffuse);
        ImGui::SameLine();
        ImGui::Checkbox("sky specular", &effects.ambientSpecular);
        ImGui::Checkbox("reflection probes", &layers.reflections);
        ImGui::TextDisabled("probes ride INSIDE sky specular:\n"
                            "specular off hides them too");

        ImGui::SeparatorText("occlusion");
        if (toggled("SSAO (O)", model.occlusionActive)) requests.toggleOcclusion = true;
        ImGui::SameLine();
        ImGui::Checkbox("baked AO", &effects.bakedOcclusion);

        ImGui::SeparatorText("through-surface");
        ImGui::Checkbox("transmission", &effects.transmission);

        ImGui::Separator();
        if (ImGui::Button("all on")) effects = RenderEffects{};
        ImGui::SameLine();
        if (ImGui::Button("ambient only")) {
            /* The fastest way to ask "is this thing lit, or is it ambient?" —
             * which was the question that took longest to answer by hand. */
            effects = RenderEffects{};
            effects.directSun = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("direct only")) {
            effects = RenderEffects{};
            effects.ambientDiffuse  = false;
            effects.ambientSpecular = false;
        }

        if (!effects.allOn())
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "TERMS DISABLED - this is not the real image");
    }
    ImGui::End();
}

void DevView::drawViewPanel(const HudModel& model, DevRequests& requests)
{
    placePanel(1, kPanelWidth);
    if (ImGui::Begin("view", &open_.view)) {
        int storey = model.isoLevel;
        if (ImGui::SliderInt("storey", &storey, 0, storeys_ - 1))
            requests.isoLevel = storey;

        if (toggled("cutaway (C)", model.softCutaway))   requests.toggleCutaway = true;
        if (toggled("cover (V)", model.showCover))       requests.toggleCover = true;
        if (toggled("line of sight (L)", model.losMode)) requests.toggleLos = true;
        /* F cycles rather than toggles — off, geometry only, reflection probe,
         * rooms — so the button reports which view is up rather than a bool. */
        if (toggled("debug view (F)", model.debugView != 0)) requests.toggleFlatView = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", model.debugView == 1 ? "geometry"
                                : model.debugView == 2 ? "probes"
                                : model.debugView == 3 ? "rooms"
                                : model.debugView == 4 ? "roughness"
                                : model.debugView == 5 ? "occlusion" : "off");
        if (model.debugView == 4)
            ImGui::TextDisabled("  the value actually shaded with, not the\n"
                                "  authored one. black = mirror, white = matt.\n"
                                "  GREEN = 0.12-0.55, the probe fade band.\n"
                                "  lighter than green reflects NO probe.");
        if (model.debugView == 2)
            ImGui::TextDisabled("  a chrome ball at each capture point.\n"
                                "  warm rim = interior, cool = outdoor.\n"
                                "  %d probes", model.probeCount);

        /* Beside the view that raises the question, not buried in the texture
         * gallery: "which room is this cubemap of" is unanswerable while the
         * strip is nailed to layer 0. */
        if (model.probeCount > 1 &&
            ImGui::SmallButton("cycle probe strip"))
            requests.cyclePreviewProbe = true;
        if (model.debugView == 3)
            ImGui::TextDisabled("  one hue per room; dark = blending two;\n"
                                "  magenta = no probe. a wall should be TWO\n"
                                "  colours, one per side. %d rooms", model.probeCount);

        ImGui::Separator();
        ImGui::Text("ring override: %s", model.ringOverrideName);
        ImGui::SameLine();
        if (ImGui::SmallButton("cycle (TAB)")) requests.cycleRing = true;

        ImGui::TextDisabled("move   %d loops / %d edges", model.moveLoops, model.moveEdges);
        ImGui::TextDisabled("sprint %d loops / %d edges", model.sprintLoops, model.sprintEdges);
    }
    ImGui::End();
}

void DevView::drawSunPanel(const HudModel& model, DevTunables& tunables,
                           DevRequests& requests)
{
    SunLight::Tuning& tuning = tunables.sun.tuning();

    placePanel(2, 380.0f);
    if (ImGui::Begin("sun", &open_.sun)) {
        ImGui::SeparatorText("where it is");
        float azimuth = model.sunAzimuth;
        if (ImGui::SliderFloat("azimuth", &azimuth, 0.0f, 360.0f, "%.0f deg"))
            requests.sunAzimuth = azimuth;

        float elevation = model.sunElevation;
        if (ImGui::SliderFloat("height", &elevation, 4.0f, 86.0f, "%.0f deg"))
            requests.sunElevation = elevation;

        ImGui::SeparatorText("how bright");
        ImGui::SliderFloat("sun", &tuning.peak, 0.0f, 12.0f, "%.2f");
        tintEdit("sun tint", &tuning.tint);
        ImGui::SliderFloat("ambient", &tuning.ambient, 0.0f, 1.5f, "%.2f");
        tintEdit("sky tint", &tuning.skyTint);
        ImGui::TextDisabled("colour comes from the height; the tints\nmultiply what it decided");

        ImGui::SeparatorText("how soft");
        ImGui::SliderFloat("angular radius", &tuning.angularRadius,
                           0.0005f, 0.05f, "%.4f rad");
        ImGui::TextDisabled("physical sun is 0.0047; bigger blurs\nevery shadow and grains the filter");

        ImGui::SeparatorText("what came out");
        const Vector3 radiance = tunables.sun.radiance();
        const Vector3 zenith   = tunables.sun.zenithColour();
        const Vector3 horizon  = tunables.sun.horizonColour();
        const Vector3 ground   = tunables.sun.groundColour();
        ImGui::TextDisabled("radiance %.2f %.2f %.2f",
                            static_cast<double>(radiance.x), static_cast<double>(radiance.y),
                            static_cast<double>(radiance.z));
        ImGui::TextDisabled("zenith   %.2f %.2f %.2f",
                            static_cast<double>(zenith.x), static_cast<double>(zenith.y),
                            static_cast<double>(zenith.z));
        ImGui::TextDisabled("horizon  %.2f %.2f %.2f",
                            static_cast<double>(horizon.x), static_cast<double>(horizon.y),
                            static_cast<double>(horizon.z));
        ImGui::TextDisabled("ground   %.2f %.2f %.2f",
                            static_cast<double>(ground.x), static_cast<double>(ground.y),
                            static_cast<double>(ground.z));

        ImGui::SeparatorText("the bake");
        if (toggled("baked sun (B)", model.bakedSun)) requests.toggleBake = true;
        ImGui::SameLine();
        if (ImGui::Button("rebake")) requests.rebakeSun = true;
        if (model.bakedSun)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "the bake is static - rebake after moving the sun");

        ImGui::Separator();
        if (ImGui::Button("reset sun")) tuning = SunLight::Tuning{};
    }
    ImGui::End();
}

void DevView::drawRibbonPanel(DevTunables& tunables)
{
    RibbonTuning& ribbon = tunables.ribbon;

    placePanel(3, 380.0f);
    if (ImGui::Begin("ribbon", &open_.ribbon)) {
        ImGui::SeparatorText("colour");
        colourEdit("move", &ribbon.moveColour);
        colourEdit("sprint", &ribbon.sprintColour);

        ImGui::SeparatorText("shape");
        sliderUnrealUnits("width", &ribbon.width, 0.5f, 30.0f);
        sliderUnrealUnits("lift",  &ribbon.lift,  0.0f, 30.0f);
        ImGui::TextDisabled("XCOM ships width 5uu, lift 4uu;\nboth rebuild the strips when moved");

        ImGui::SeparatorText("glow");
        ImGui::SliderFloat("emissive", &ribbon.emissive, 0.0f, 12.0f, "%.2f");
        ImGui::SliderInt("steps", &ribbon.glowSteps, 0, 6);
        ImGui::SliderFloat("gain", &ribbon.glowGain, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("falloff", &ribbon.glowFalloff, 0.05f, 0.95f, "%.2f");
        ImGui::TextDisabled("falloff high spreads the halo into a haze\nthat reads as a fatter ribbon, not a brighter one");

        ImGui::SeparatorText("motion");
        ImGui::SliderFloat("pan speed", &ribbon.panSpeed, -2.0f, 2.0f, "%.2f");

        ImGui::Separator();
        if (ImGui::Button("reset ribbon")) ribbon = RibbonTuning{};
    }
    ImGui::End();
}

void DevView::drawPostPanel(const HudModel& model, ViewLayers& layers,
                            DevTunables& tunables, DevRequests& requests)
{
    placePanel(4, kPanelWidth);
    if (ImGui::Begin("post", &open_.post)) {
        ImGui::SeparatorText("tone map");
        ImGui::SliderFloat("exposure", &tunables.exposure, 0.25f, 16.0f, "%.2f");
        ImGui::TextDisabled("set against the sun's radiance so a lit\nwall lands near mid grey - move both or neither");

        ImGui::SeparatorText("occlusion");
        if (toggled("enabled (O)", model.occlusionActive)) requests.toggleOcclusion = true;
        ImGui::SliderFloat("radius", &tunables.occlusion.radius, 0.05f, 2.0f, "%.2f tiles");
        ImGui::SliderFloat("strength", &tunables.occlusion.strength, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("bias", &tunables.occlusion.bias, 0.0f, 0.05f, "%.4f");
        ImGui::TextDisabled("bias above ~0.05 rejects every tap and\nturns the effect off silently");

        ImGui::SeparatorText("shadows");
        ImGui::Checkbox("shadow map", &layers.shadows);
        ImGui::TextDisabled("%s", model.shadowsActive ? "loaded" : "unavailable");

        ImGui::Separator();
        if (ImGui::Button("reset occlusion")) tunables.occlusion = AmbientOcclusion::Tuning{};
    }
    ImGui::End();
}

void DevView::drawScenePanel(const HudModel& model, DevRequests& requests)
{
    placePanel(5, kPanelWidth);
    if (ImGui::Begin("scene", &open_.scene)) {
        ImGui::SeparatorText("selection");
        ImGui::Text("%s at (%d,%d) storey %d", model.selectedName.c_str(),
                    model.selectedCell.x, model.selectedCell.y,
                    Lattice::storeyOfZ(model.selectedCell.z) + 1);

        if (model.hoverCell) {
            char cost[24] = "-";
            if (model.hoverCost)
                std::snprintf(cost, sizeof(cost), "%.1f", static_cast<double>(*model.hoverCost));

            ImGui::TextColored(model.hoverRestOk ? ImVec4(0.6f, 1.0f, 0.4f, 1.0f)
                                                 : ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "hover (%d,%d) z=%d  cost %s%s",
                               model.hoverCell->x, model.hoverCell->y, model.hoverCell->z,
                               cost, model.hoverRestOk ? "" : "  cannot stop here");
        } else {
            ImGui::TextDisabled("no hovered tile");
        }

        ImGui::SeparatorText("actions");
        if (ImGui::Button(model.grenadeArmed ? "disarm grenade (G)" : "arm grenade (G)"))
            requests.toggleGrenade = true;
        ImGui::SameLine();
        if (ImGui::Button("reset world (R)")) requests.resetWorld = true;

        if (!model.status.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", model.status.c_str());
        }

        ImGui::Separator();
        ImGui::Checkbox("ImGui demo window", &showDemo_);
    }
    ImGui::End();
}

/* ----------------------------------------------------------------- frame */
/* THE TEXTURE INSPECTOR.
 *
 * Every intermediate the renderer produces, as an image. Nothing here computes
 * anything — it exists because the alternative is arguing about a sampler's
 * contents from the lit frame, which this project has now done often enough to
 * know the cost. A lightmap index that was never bound, an atlas seven times
 * too coarse, a transmission plane reading zero everywhere: all three were
 * expensive to find and all three are a glance in this panel. */
/* ---- the decal tool -------------------------------------------------------
 * Point at the world, press place, look at it. The one panel here that authors
 * something rather than inspecting it.
 *
 * IT EXISTS BECAUSE THE SCATTERED DEMO WAS NOT ANSWERABLE. A pass that renders
 * thirty marks at coordinates nobody chose gives you one question — "is it
 * working?" — and no way to narrow it: a decal that fails to appear could be
 * the shader, the buffer, the blend, the angle fade, or simply a mark that
 * landed behind the building. Placing one deliberately, on a surface you are
 * looking at, at a size you set, turns that into an observation. */
void DevView::drawDecalPanel(const DevDecalTool& tool, ViewLayers& layers,
                             DevRequests& requests)
{
    placePanel(9, kPanelWidth);
    if (ImGui::Begin("decals", &open_.decals)) {

        if (!tool.available) {
            ImGui::TextDisabled("the decal pass did not come up.\n"
                                "check the log for DECAL: warnings");
            ImGui::End();
            return;
        }

        ImGui::Checkbox("decals on", &layers.decals);
        ImGui::SameLine();
        ImGui::TextDisabled("| %d placed", tool.placedCount);

        ImGui::Separator();

        if (tool.materialCount == 0) {
            ImGui::TextDisabled("no decal materials are loaded");
            ImGui::End();
            return;
        }

        if (decalBrush_.material >= tool.materialCount) decalBrush_.material = 0;
        ImGui::Combo("material", &decalBrush_.material,
                     tool.materialNames, tool.materialCount);

        /* THE ONE CONTROL THAT CHANGES WHAT KIND OF DECAL THIS IS, so it sits
         * with the material rather than down among the fades. Off is a flat
         * sheet that stops at the edge of the face it is printed on; on is a
         * mark thrown at the world that runs over whatever it hits. */
        ImGui::Checkbox("wrap round corners", &decalBrush_.wrap);
        ImGui::TextDisabled(decalBrush_.wrap
                            ? "per-pixel projection, unwrapped across folds"
                            : "one fixed axis; stops where the surface turns away");

        ImGui::SliderFloat("size",      &decalBrush_.size,      0.25f, 8.0f, "%.2f tiles");
        ImGui::SliderFloat("rotation",  &decalBrush_.rotation,  0.0f, 360.0f, "%.0f deg");
        ImGui::SliderFloat("opacity",   &decalBrush_.opacity,   0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("roughness", &decalBrush_.roughness, 0.045f, 1.0f, "%.2f");
        ImGui::SliderFloat("emissive",  &decalBrush_.emissive,  0.0f, 1.0f, "%.2f");

        ImGui::Separator();

        /* ARM, PREVIEW, COMMIT — a mode, not a button that fires.
         *
         * While armed a real decal is projected under the cursor every frame,
         * through the actual pass with the actual settings, so what is on screen
         * IS what a click will leave behind. Moving a slider moves the ghost.
         * That is the only way to answer the question the tool exists for —
         * where does it land and how does it wrap — before committing. */
        if (decalArmed_) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button("placing - click in the world", ImVec2(230.0f, 0.0f)))
                decalArmed_ = false;
            ImGui::PopStyleColor();
        } else if (ImGui::Button("place at cursor", ImVec2(230.0f, 0.0f))) {
            decalArmed_ = true;
        }

        /* Present means armed — Application drains requests every frame, so the
         * brush has to keep arriving for the preview to keep tracking. */
        if (decalArmed_) requests.decalBrush = decalBrush_;

        if (ImGui::Button("clear all")) requests.clearDecals = true;

        if (decalArmed_) {
            if (tool.cursorOnSurface)
                ImGui::TextDisabled("click to commit, or press the button\n"
                                    "again to cancel. sliders move the ghost.");
            else
                ImGui::TextDisabled("point at a floor or a wall —\n"
                                    "nothing under the cursor to stick to");
        } else {
            ImGui::TextDisabled("arms a ghost that follows the cursor;\n"
                                "a click in the world commits it");
        }

        ImGui::Separator();
        ImGui::TextDisabled("a decal is projected through a box, so it\n"
                            "wraps over whatever it lands on and cannot\n"
                            "z-fight. windows take none: pbr.fs.glsl\n"
                            "refuses decals on blended surfaces.");
    }
    ImGui::End();
}

void DevView::drawSteamPanel(DevSteam steam)
{
    if (ImGui::Begin("steam", &open_.steam)) {
        if (!steam.running) {
            ImGui::TextDisabled("not connected");
            if (!steam.reason.empty()) ImGui::TextWrapped("%s", steam.reason.c_str());
        } else {
            ImGui::Text("persona : %s", steam.persona.c_str());
            ImGui::Text("steamid : %llu", static_cast<unsigned long long>(steam.steamId));
        }

        ImGui::Separator();
        ImGui::Text("avatar  : %s", steam.avatarState.c_str());

        if (!steam.avatarUrl.empty()) {
            /* Wrapped: the CDN url is longer than any sensible panel width, and
             * it is worth seeing in full when the fetch has gone wrong. */
            ImGui::TextWrapped("%s", steam.avatarUrl.c_str());
        }

        if (steam.avatar.id != 0) {
            /* Drawn at its native size - the whole point of going to the
             * community site rather than the SDK was the 184x184, so showing it
             * scaled down here would hide whether that actually arrived. */
            /* rlImGuiImage rather than ImGui::Image: it is the raylib backend's
             * own helper and already knows how to turn a Texture2D into
             * whatever ImTextureID happens to be in this build. */
            rlImGuiImage(&steam.avatar);
            ImGui::Text("%d x %d", steam.avatar.width, steam.avatar.height);
        }
    }
    ImGui::End();
}

void DevView::drawTexturePanel(const DevTextures& textures)
{
    placePanel(6, kPanelWidth * 1.6f);
    if (ImGui::Begin("textures", &open_.textures)) {
        ImGui::SliderFloat("preview height", &previewHeight_, 60.0f, 480.0f, "%.0f px");
        ImGui::Separator();

        if (textures.count == 0) ImGui::TextDisabled("nothing to show");

        for (int i = 0; i < textures.count; i++) {
            const DevTextureView& entry = textures.entries[i];

            ImGui::PushID(i);
            if (entry.texture.id == 0) {
                ImGui::TextDisabled("%s  -  not allocated", entry.name);
                ImGui::PopID();
                continue;
            }

            ImGui::Text("%s", entry.name);
            ImGui::SameLine();
            ImGui::TextDisabled("%dx%d", entry.texture.width, entry.texture.height);
            if (entry.note[0] != '\0') ImGui::TextDisabled("%s", entry.note);

            /* Aspect preserved from the source, so a 6:1 cubemap strip and a
             * square shadow map are both readable at one height setting. */
            const float aspect = (entry.texture.height > 0)
                               ? static_cast<float>(entry.texture.width) /
                                 static_cast<float>(entry.texture.height)
                               : 1.0f;
            const int height = static_cast<int>(previewHeight_);
            const int width  = static_cast<int>(previewHeight_ * aspect);

            /* Drawn AS GIVEN. GL's origin is bottom-left and ImGui's is
             * top-left, so a framebuffer shown here needs exactly one flip —
             * and TexturePreviews already applied it, on the way into the copy
             * it hands over. Flipping again here is what made every preview
             * upside down; the correction belongs where the source's
             * orientation is actually known, not here. */
            const Rectangle source{ 0.0f, 0.0f,
                                    static_cast<float>(entry.texture.width),
                                    static_cast<float>(entry.texture.height) };
            rlImGuiImageRect(&entry.texture, width, height, source);

            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::End();
}

/* ---------------------------------------------------------------- browser */

/* Held by pointer from the header so that WebInput.hpp does not have to be
 * included there. Empty when the build has no CEF, which keeps the member,
 * the constructor and the destructor identical either way. */
struct DevView::BrowserInput {
#if XC_HAVE_WEB
    WebInputState state;
#endif
};

DevView::DevView() : browserInput_(std::make_unique<BrowserInput>()) {}
DevView::~DevView() = default;

void DevView::drawBrowserPanel(WebSurface* browser)
{
    placePanel(8, 1100.0f);
    ImGui::SetNextWindowSize(ImVec2(1100.0f, 780.0f), ImGuiCond_FirstUseEver);

    /* NoScrollWithMouse and NoScrollbar: the wheel belongs to the page, and an
     * ImGui scroll region wrapped around a browser would scroll the wrong
     * thing and clip the other. The page is sized to the window instead. */
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin("browser", &open_.browser, flags)) {
        /* SNAPPED TO WHOLE PIXELS, AND THIS IS LOAD-BEARING. ImGui window
         * positions are floats and a drag leaves them fractional. Half a pixel
         * of offset under a page of antialiased text is the difference between
         * crisp glyphs and a uniform smear, because every texel then straddles
         * two pixels. Everything else about the sizing below is arranged so
         * that one page texel lands on one screen pixel; this is what stops
         * that being undone by where the window happens to sit. */
        const ImVec2 position = ImGui::GetWindowPos();
        const ImVec2 snapped(std::floor(position.x), std::floor(position.y));
        if (snapped.x != position.x || snapped.y != position.y)
            ImGui::SetWindowPos(snapped);

#if XC_HAVE_WEB
        if (browser == nullptr || !browser->valid()) {
            ImGui::TextDisabled("no browser - CEF did not start");
            ImGui::TextDisabled("check that libcef.dll and cromwell_web_helper.exe");
            ImGui::TextDisabled("are beside the executable");
        } else {
            drawBrowserContents(browser);
        }
#else
        (void)browser;
        ImGui::TextDisabled("built without CEF (XC_BUILD_WEB=OFF)");
#endif
    }
    ImGui::End();
}

#if XC_HAVE_WEB
void DevView::drawBrowserContents(WebSurface* browser)
{
    /* ---- the address bar ---- */
    const float buttonWidth = ImGui::GetFrameHeight();

    ImGui::BeginDisabled(!browser->canGoBack());
    if (ImGui::Button("<", ImVec2(buttonWidth, 0.0f))) browser->goBack();
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(!browser->canGoForward());
    if (ImGui::Button(">", ImVec2(buttonWidth, 0.0f))) browser->goForward();
    ImGui::EndDisabled();
    ImGui::SameLine();

    if (ImGui::Button(browser->loading() ? "x" : "r", ImVec2(buttonWidth, 0.0f)))
        browser->reload();
    ImGui::SameLine();

    /* EnterReturnsTrue, so the field commits on enter and not on every
     * keystroke — otherwise typing an address would navigate to each prefix
     * of it in turn. */
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputText("##url", urlField_, sizeof(urlField_),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        /* No need to release the field afterwards: EnterReturnsTrue also
         * deactivates it, so the keyboard goes back to the page on its own. */
        browser->loadUrl(urlField_);
    }
    urlEditing_ = ImGui::IsItemActive();

    /* The address the page is actually at, which is not the one that was typed
     * once a redirect or a link has moved it. Only written back while the
     * field is idle: a box that rewrites itself mid-word cannot be typed in. */
    if (!urlEditing_) {
        const std::string live = browser->currentUrl();
        if (!live.empty() && live != urlField_)
            std::snprintf(urlField_, sizeof(urlField_), "%s", live.c_str());
    }

    /* ---- the page ---- */

    /* ONE PAGE TEXEL PER SCREEN PIXEL. The browser is resized to the space it
     * will occupy rather than laid out at some nominal width and scaled to
     * fit, because scaling is what makes the text mushy — see
     * WebSurface::resize. */
    /* One line held back for the status readout at the bottom. The window has
     * no scrollbar, so anything the page does not leave room for is simply
     * not drawn. */
    const float statusHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 available   = ImGui::GetContentRegionAvail();
    const int wantWidth  = std::max(64, static_cast<int>(available.x));
    const int wantHeight = std::max(64, static_cast<int>(available.y - statusHeight));

    /* Debounced: a resize is a full relayout and repaint inside Chromium, and
     * dragging the window edge would otherwise order one every frame. */
    constexpr double kResizeSettleSeconds = 0.15;
    if (wantWidth != pendingWidth_ || wantHeight != pendingHeight_) {
        pendingWidth_  = wantWidth;
        pendingHeight_ = wantHeight;
        pendingSince_  = ImGui::GetTime();
    }
    if (pendingSince_ > 0.0 && ImGui::GetTime() - pendingSince_ >= kResizeSettleSeconds) {
        pendingSince_ = 0.0;
        if (pendingWidth_ != browser->width() || pendingHeight_ != browser->height())
            browser->resize(pendingWidth_, pendingHeight_);
    }

    const Texture2D page = browser->texture();
    if (page.id == 0) {
        ImGui::TextDisabled("waiting for the first paint");
        return;
    }

    /* Drawn at the texture's own size. Not ImageSize, not a fit-to-window
     * variant: either would reintroduce the scale this whole path exists to
     * avoid. Until a pending resize settles the page is briefly the old size
     * and simply does not fill the window, which is the right trade — a gap
     * for two frames beats a blur for all of them. */
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    rlImGuiImage(&page);

    /* AN INVISIBLE BUTTON OVER THE IMAGE, because ImGui::Image adds its item
     * with an id of zero and an id of zero is, to ImGui, not an item at all.
     * A press on it therefore reads as a press on blank window space, which
     * by default starts dragging the window — so a click meant for a link
     * moves the panel instead. The button gives the area a real id, which
     * both swallows the press and makes IsItemHovered answer about the page
     * rather than about the window.
     *
     * All three mouse buttons, because right and middle clicks belong to the
     * page too. */
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##page",
                           ImVec2(static_cast<float>(page.width),
                                  static_cast<float>(page.height)),
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);

    const bool   hovered   = ImGui::IsItemHovered();
    const ImVec2 rectangle = ImGui::GetItemRectMin();
    const Rectangle bounds{ rectangle.x, rectangle.y,
                            static_cast<float>(page.width),
                            static_cast<float>(page.height) };

    /* WantCaptureKeyboard is how the address bar wins: while it is active
     * ImGui owns the keys and the page must not also see them. */
    const WebInputClaim claim =
        routeWebInput(*browser, bounds, browserInput_->state,
                      hovered, !ImGui::GetIO().WantCaptureKeyboard);

    /* Typed text is read from raylib inside routeWebInput, not from ImGui's
     * InputQueueCharacters here. That queue looked like the obvious source and
     * is in fact ALWAYS EMPTY in this case: rlImGui only fills it inside
     * `if (io.WantCaptureKeyboard)`, which is exactly when the page must not
     * be typed into. Reading it was why typing did nothing at all. */
    const ImGuiIO& io = ImGui::GetIO();
    charsForwarded_ += claim.characters;

    /* WHY TYPING IS OR IS NOT WORKING, ON SCREEN. Four bools decide it and
     * they fail in different places: "click" is ours and comes from the
     * pointer, "field" is Chromium's and arrives from the render process,
     * "imgui" is the address bar holding the keyboard, and "typing" is the
     * conjunction that actually gates the characters. Reading them off the
     * screen turns "I cannot type in the search box" into one glance. */
    /* typing is what actually gates the characters; chars is how many got
       through. field and sig are Chromium's opinion of whether a text box has
       the caret - reported because it is informative, but no longer trusted
       to decide anything. See WebSurface::wantsKeyboard. */
    ImGui::TextDisabled("focus %d  typing %d  chars %d  |  imgui %d  field %d  sig %d  |  %dx%d  esc releases",
                        browser->pageFocused() ? 1 : 0,
                        claim.keyboard ? 1 : 0,
                        charsForwarded_,
                        io.WantCaptureKeyboard ? 1 : 0,
                        browser->editableFocused() ? 1 : 0,
                        browser->editableSignals(),
                        page.width, page.height);
}
#endif

void DevView::draw(const HudModel& model, ViewLayers& layers,
                   DevTunables tunables, const DevTextures& textures,
                   const DevDecalTool& decalTool, const DevSteam& steam,
                   DevRequests& requests, WebSurface* browser)
{
    if (!ready_) return;

    /* The frame is begun even while hidden: rlImGuiBegin is what feeds ImGui
     * the mouse, and skipping it would leave WantCaptureMouse frozen at
     * whatever it was when the UI was last up. */
    rlImGuiBegin();

    if (!visible_) {
        rlImGuiEnd();
        return;
    }

    drawToolbar();

    if (open_.layers) drawLayersPanel(layers);
    if (open_.rendering) drawRenderingPanel(model, layers, tunables, requests);
    if (open_.view)   drawViewPanel(model, requests);
    if (open_.sun)    drawSunPanel(model, tunables, requests);
    if (open_.ribbon) drawRibbonPanel(tunables);
    if (open_.post)   drawPostPanel(model, layers, tunables, requests);
    if (open_.scene)  drawScenePanel(model, requests);
    if (open_.textures) drawTexturePanel(textures);
    if (open_.decals)   drawDecalPanel(decalTool, layers, requests);
    if (open_.browser)  drawBrowserPanel(browser);
    if (open_.steam)    drawSteamPanel(steam);

    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);

    rlImGuiEnd();
}

}  // namespace game
