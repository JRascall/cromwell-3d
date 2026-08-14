/* PlatformTests.cpp — the platform interfaces, checked by implementing them.
 *
 * THE SAME ARRANGEMENT AS RhiTests.cpp, and for the same two reasons. A null
 * backend overrides every pure virtual in ISurface, IInput, IClock,
 * IFileSystem and IImageDecoder, so a method added to any of them without a
 * thought for what a console would do with it fails on the machine of whoever
 * added it. And this target links cromwell_base, which never sees raylib — so
 * these headers compiling here IS the check that the platform layer has not
 * quietly re-acquired a windowing library.
 *
 * ================== WHY A NULL BACKEND IS NOT A WASTED HOUR ================
 *
 * It is also the thing that makes the engine testable headlessly and runnable
 * on a build machine. A null platform is what a replay verifier, an asset
 * cooker or a CI run of the simulation needs, and every one of those is
 * currently impossible because the loop reaches for a window. It falls out of
 * writing the interface honestly.
 *
 * WHAT IS NOT CHECKED: anything needing a real platform. There is no window to
 * open and no disc to spin, so what remains is the CONTRACTS that hold
 * regardless of backend — the clock's clamp, the storage kinds staying
 * distinct, text entry's state machine — and those are exactly the ones a port
 * is most likely to break.
 */
#include "cromwell/assets/IImageDecoder.hpp"
#include "cromwell/input/IInput.hpp"
#include "cromwell/platform/pc/PcFileSystem.hpp"
#include "cromwell/platform/FrameClock.hpp"
#include "cromwell/platform/IClock.hpp"
#include "cromwell/platform/IFileSystem.hpp"
#include "cromwell/platform/ISurface.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

bool nearly(float a, float b, float tolerance = 1e-5f)
{
    const float d = a - b;
    return (d < 0.0f ? -d : d) <= tolerance;
}

/* ---- a surface that is not a window ------------------------------------
 *
 * Modelled on a console deliberately: no resize, no cursor, no clipboard, and
 * a safe area inset from the display. If desktop-shaped assumptions have crept
 * into a call site, this is the shape that catches them. */
class NullSurface final : public ISurface {
public:
    NullSurface()
    {
        caps_.resizable = false;
        caps_.fullscreen = false;
        caps_.cursor = false;
        caps_.clipboard = false;
        caps_.title = false;
    }

    const SurfaceCapabilities& capabilities() const override { return caps_; }
    void size(int& width, int& height) const override { width = 1920; height = 1080; }
    float scaleFactor() const override { return 1.0f; }

    /* 5% inset, which is a realistic television overscan allowance. */
    SurfaceRect safeArea() const override { return { 96, 54, 1728, 972 }; }

    bool closeRequested() const override { return closeRequested_; }
    void cancelCloseRequest() override { closeRequested_ = false; }
    void pumpEvents() override { pumped++; }

    /* Counted, so a double-present during the renderer migration would show up
     * here rather than as a torn frame on someone else machine. */
    void present() override { presents++; }
    bool takeResized() override { const bool r = resized_; resized_ = false; return r; }
    bool active() const override { return true; }

    /* All no-ops, because the capabilities say so. */
    void setTitle(const char*) override {}
    void setFullscreen(bool) override {}
    bool fullscreen() const override { return false; }
    void setCursorVisible(bool) override {}

    /* RECORDED RATHER THAN IGNORED. On a console there is no window to
     * withhold, but the caller still reveals after its first present — so what
     * matters is that the call is harmless and reaches the backend, not that it
     * does anything here. */
    void setVisible(bool visible) override { visible_ = visible; }
    bool visible() const { return visible_; }

    const char* clipboardText() const override { return ""; }
    void setClipboardText(const char*) override {}

    void requestClose() { closeRequested_ = true; }
    void signalResize() { resized_ = true; }

    int pumped = 0;
    int presents = 0;

private:
    SurfaceCapabilities caps_;
    bool closeRequested_ = false;
    bool resized_ = false;
    bool visible_ = true;
};

/* ---- input, gamepad-first ----------------------------------------------*/

class NullInput final : public IInput {
public:
    void poll() override { polls++; }

    bool down(Key) const override { return false; }
    bool pressed(Key) const override { return false; }
    bool released(Key) const override { return false; }
    uint32_t modifiers() const override { return ModNone; }
    bool hasKeyboard() const override { return false; }

    bool down(MouseButton) const override { return false; }
    bool pressed(MouseButton) const override { return false; }
    bool released(MouseButton) const override { return false; }
    Vec2 pointerPosition() const override { return {}; }
    Vec2 pointerDelta() const override { return {}; }
    Vec2 wheelDelta() const override { return {}; }
    bool hasPointer() const override { return false; }

    bool gamepadConnected(int pad) const override { return pad == 0; }
    bool down(int pad, GamepadButton button) const override
    {
        return pad == 0 && button == held_;
    }
    bool pressed(int, GamepadButton) const override { return false; }
    bool released(int, GamepadButton) const override { return false; }
    float axis(int pad, GamepadAxis which) const override
    {
        if (pad != 0) return 0.0f;
        /* RAW, dead zone deliberately not applied - see IInput.hpp. */
        return which == GamepadAxis::LeftX ? 0.06f : 0.0f;
    }
    void setRumble(int, float, float) override {}

    bool beginTextEntry(const TextEntryRequest& request) override
    {
        state_ = TextEntryState::Active;
        pending_ = request.initialText ? request.initialText : "";
        return true;
    }
    TextEntryState textEntryState() const override { return state_; }
    const char* takeTextEntry() override
    {
        state_ = TextEntryState::Idle;
        return pending_.c_str();
    }
    void cancelTextEntry() override { state_ = TextEntryState::Cancelled; }

    /* Stands in for the player finishing on the system keyboard. */
    void commitTextEntry(const char* text) { pending_ = text; state_ = TextEntryState::Committed; }

    void holdButton(GamepadButton button) { held_ = button; }

    int polls = 0;

private:
    TextEntryState state_ = TextEntryState::Idle;
    std::string pending_;
    GamepadButton held_ = GamepadButton::Count;
};

/* ---- a clock with the clamp that matters -------------------------------*/

class NullClock final : public IClock {
public:
    void tick() override
    {
        frames_++;
        if (skip_) { raw_ = 0.0f; skip_ = false; }
        hitched_ = raw_ > maxDelta_;
        clamped_ = hitched_ ? maxDelta_ : raw_;
        elapsed_ += clamped_;
    }

    float delta() const override { return paused_ ? 0.0f : clamped_ * scale_; }
    float unscaledDelta() const override { return clamped_; }
    float rawDelta() const override { return raw_; }
    float maxDelta() const override { return maxDelta_; }
    void setMaxDelta(float seconds) override { maxDelta_ = seconds; }
    bool hitched() const override { return hitched_; }

    void setTimeScale(float scale) override { scale_ = scale; }
    float timeScale() const override { return scale_; }
    void setPaused(bool paused) override { paused_ = paused; }
    bool paused() const override { return paused_; }

    double elapsed() const override { return elapsed_; }
    uint64_t frameCount() const override { return frames_; }
    float framesPerSecond() const override { return clamped_ > 0.0f ? 1.0f / clamped_ : 0.0f; }
    void skipNextDelta() override { skip_ = true; }

    void measure(float seconds) { raw_ = seconds; }

private:
    float raw_ = 0.0f, clamped_ = 0.0f, maxDelta_ = 0.1f, scale_ = 1.0f;
    double elapsed_ = 0.0;
    uint64_t frames_ = 0;
    bool paused_ = false, hitched_ = false, skip_ = false;
};

/* ---- storage, with the kinds kept genuinely separate --------------------*/

class NullFileSystem final : public IFileSystem {
public:
    StorageResult read(StorageKind kind, const char* name, std::vector<uint8_t>& out) override
    {
        const auto it = store_.find(key(kind, name));
        if (it == store_.end()) return StorageResult::NotFound;
        out.assign(it->second.begin(), it->second.end());
        return StorageResult::Ok;
    }

    StorageResult readText(StorageKind kind, const char* name, std::string& out) override
    {
        std::vector<uint8_t> bytes;
        const StorageResult result = read(kind, name, bytes);
        if (result == StorageResult::Ok) out.assign(bytes.begin(), bytes.end());
        return result;
    }

    bool exists(StorageKind kind, const char* name) const override
    {
        return store_.count(key(kind, name)) != 0;
    }

    StorageResult write(StorageKind kind, const char* name,
                        const void* data, size_t bytes) override
    {
        /* ASSETS ARE READ-ONLY EVEN HERE, so a save written to the wrong kind
         * fails on a developer's machine rather than at certification. */
        if (kind == StorageKind::Asset) return StorageResult::AccessDenied;

        const auto* first = static_cast<const uint8_t*>(data);
        store_[key(kind, name)].assign(first, first + bytes);
        return StorageResult::Ok;
    }

    StorageResult remove(StorageKind kind, const char* name) override
    {
        return store_.erase(key(kind, name)) != 0 ? StorageResult::Ok : StorageResult::NotFound;
    }

    StorageResult list(StorageKind kind, std::vector<std::string>& out) override
    {
        const std::string prefix = std::to_string(static_cast<int>(kind)) + "/";
        for (const auto& entry : store_)
            if (entry.first.rfind(prefix, 0) == 0)
                out.push_back(entry.first.substr(prefix.size()));
        return StorageResult::Ok;
    }

    bool busy() const override { return false; }
    void flush() override {}

    void seedAsset(const char* name, const char* text)
    {
        store_[key(StorageKind::Asset, name)].assign(text, text + std::strlen(text));
    }

private:
    static std::string key(StorageKind kind, const char* name)
    {
        return std::to_string(static_cast<int>(kind)) + "/" + name;
    }
    std::map<std::string, std::vector<uint8_t>> store_;
};

class NullImageDecoder final : public IImageDecoder {
public:
    ImageDecodeResult decode(const void* bytes, size_t length, DecodedImage& out) override
    {
        if (!recognises(bytes, length)) return ImageDecodeResult::UnknownFormat;
        out.width = 2;
        out.height = 2;
        out.pixels.assign(2 * 2 * 4, 0xFF);
        return ImageDecodeResult::Ok;
    }

    ImageDecodeResult probe(const void* bytes, size_t length, int& width, int& height) override
    {
        if (!recognises(bytes, length)) return ImageDecodeResult::UnknownFormat;
        width = 2;
        height = 2;
        return ImageDecodeResult::Ok;
    }

    bool recognises(const void* bytes, size_t length) const override
    {
        /* Sniffs the magic rather than trusting an extension there may not be. */
        if (length < 4) return false;
        const auto* b = static_cast<const uint8_t*>(bytes);
        return b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
    }
};

/* ---- checks -------------------------------------------------------------*/

void surfaceWithoutAWindowStillWorks()
{
    NullSurface surface;

    int width = 0, height = 0;
    surface.size(width, height);
    CHECK(width == 1920 && height == 1080, "the surface reports its drawable size in pixels");

    /* The desktop affordances are absent, and calling them anyway is safe —
     * that is the contract, so a caller never has to branch on platform. */
    CHECK(!surface.capabilities().cursor, "a console surface has no cursor");
    surface.setTitle("ignored");
    surface.setCursorVisible(true);
    surface.setFullscreen(true);
    CHECK(!surface.fullscreen(), "and asking for fullscreen changes nothing there");

    const SurfaceRect safe = surface.safeArea();
    CHECK(safe.width < width && safe.height < height,
          "the safe area is inset from the display on a television");
    CHECK(safe.x > 0 && safe.y > 0, "and offset from the corner");
}

void closeRequestCanBeDeclined()
{
    NullSurface surface;
    CHECK(!surface.closeRequested(), "nothing is asking to close yet");

    surface.requestClose();
    CHECK(surface.closeRequested(), "the platform asked");

    /* The "really quit?" prompt the player cancels. Without this the prompt
     * would re-open every frame forever. */
    surface.cancelCloseRequest();
    CHECK(!surface.closeRequested(), "declining clears the request");
}

void resizeIsATakeNotAPoll()
{
    NullSurface surface;
    surface.signalResize();
    CHECK(surface.takeResized(), "the resize is reported once");
    CHECK(!surface.takeResized(), "and cleared, so targets are not rebuilt every frame after one");
}

void gamepadAxesAreRawSoTheGameOwnsItsDeadZone()
{
    NullInput input;
    CHECK(input.gamepadConnected(0), "pad zero is present");

    /* A small off-centre reading survives to the caller. A backend applying its
     * own dead zone would make a small one unreachable, and the right size
     * genuinely differs between a twin-stick aim and a menu cursor. */
    CHECK(nearly(input.axis(0, GamepadAxis::LeftX), 0.06f),
          "a small stick deflection is reported raw, not zeroed");

    input.holdButton(GamepadButton::FaceDown);
    CHECK(input.down(0, GamepadButton::FaceDown), "the held face button reads down");
    CHECK(!input.down(0, GamepadButton::FaceRight), "and its neighbour does not");
    CHECK(!input.down(1, GamepadButton::FaceDown), "an absent pad reports nothing");
}

void textEntryIsAStateMachineNotAKeyQueue()
{
    NullInput input;
    CHECK(input.textEntryState() == TextEntryState::Idle, "nothing being typed");

    TextEntryRequest request;
    request.prompt = "Name your squad";
    request.initialText = "Bravo";
    CHECK(input.beginTextEntry(request), "the request was accepted");
    CHECK(input.textEntryState() == TextEntryState::Active,
          "console: the system keyboard is up and the game is waiting");

    input.commitTextEntry("Charlie");
    CHECK(input.textEntryState() == TextEntryState::Committed, "the player finished");
    CHECK(std::strcmp(input.takeTextEntry(), "Charlie") == 0, "and the text came back");
    CHECK(input.textEntryState() == TextEntryState::Idle, "taking it resets the machine");

    /* CANCELLED IS NOT AN EMPTY STRING, and conflating them is how a rename
     * dialog silently blanks the thing it was renaming. */
    input.beginTextEntry(request);
    input.cancelTextEntry();
    CHECK(input.textEntryState() == TextEntryState::Cancelled,
          "backing out is distinguishable from entering nothing");
}

void theClockClampsAHitch()
{
    NullClock clock;
    clock.setMaxDelta(0.1f);

    clock.measure(0.016f);
    clock.tick();
    CHECK(nearly(clock.delta(), 0.016f), "an ordinary frame passes through");
    CHECK(!clock.hitched(), "and is not a hitch");

    /* The window was dragged, or a console resumed, or a breakpoint was
     * stepped. Unclamped, this is the frame a unit teleports through a wall. */
    clock.measure(3.0f);
    clock.tick();
    CHECK(nearly(clock.delta(), 0.1f), "a three-second stall is clamped (%.3f)", clock.delta());
    CHECK(clock.hitched(), "and reported as a hitch");
    CHECK(nearly(clock.rawDelta(), 3.0f),
          "while the raw measurement survives for the profiler (%.3f)", clock.rawDelta());
}

void skipDiscardsAResumeGap()
{
    NullClock clock;
    clock.measure(30.0f);        /* suspended to the system menu */
    clock.skipNextDelta();
    clock.tick();

    CHECK(nearly(clock.delta(), 0.0f),
          "a resume produces no frame at all, not one slow one (%.3f)", clock.delta());
}

void pauseAndScaleLeaveRealTimeAlone()
{
    NullClock clock;
    clock.measure(0.02f);
    clock.tick();

    clock.setPaused(true);
    CHECK(nearly(clock.delta(), 0.0f), "simulation stops while paused");
    CHECK(nearly(clock.unscaledDelta(), 0.02f),
          "but UI animation keeps running - a frozen pause-menu spinner is the bug this prevents");

    clock.setPaused(false);
    clock.setTimeScale(0.5f);
    CHECK(nearly(clock.delta(), 0.01f), "slow motion scales game time");
    CHECK(nearly(clock.unscaledDelta(), 0.02f), "and leaves real time alone");
}

void storageKindsAreSeparateNamespaces()
{
    NullFileSystem files;
    files.seedAsset("shaders/pbr.frag", "void main() {}");

    std::string source;
    CHECK(files.readText(StorageKind::Asset, "shaders/pbr.frag", source) == StorageResult::Ok,
          "a shipped asset reads");

    /* THE SAME NAME IN ANOTHER KIND IS ANOTHER THING. On desktop both might
     * land in one folder; the interface refuses to let that be assumed. */
    CHECK(!files.exists(StorageKind::Save, "shaders/pbr.frag"),
          "the same name in save data is a different object entirely");

    CHECK(files.write(StorageKind::Asset, "shaders/pbr.frag", "x", 1)
              == StorageResult::AccessDenied,
          "shipped content is read-only even on desktop, so a misrouted save "
          "fails on a dev machine rather than at certification");

    CHECK(files.write(StorageKind::Save, "campaign1", "state", 5) == StorageResult::Ok,
          "save data is writable");

    std::vector<std::string> saves;
    files.list(StorageKind::Save, saves);
    CHECK(saves.size() == 1 && saves[0] == "campaign1", "and enumerable for a load menu");
}

void missingFileIsDistinguishableFromAFailure()
{
    NullFileSystem files;
    std::vector<uint8_t> bytes;

    /* NotFound for an optional asset is routine; QuotaExceeded needs a message
     * to the player. A bool would make those the same. */
    CHECK(files.read(StorageKind::Save, "nothing", bytes) == StorageResult::NotFound,
          "an absent file says so specifically");
}

void decoderSniffsRatherThanTrustingAName()
{
    NullImageDecoder decoder;

    const uint8_t png[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    const uint8_t junk[8] = { 'n', 'o', 't', 'a', 'n', 'i', 'm', 'g' };

    CHECK(decoder.recognises(png, sizeof png), "the magic bytes are recognised");
    CHECK(!decoder.recognises(junk, sizeof junk), "and junk is not");

    DecodedImage image;
    CHECK(decoder.decode(png, sizeof png, image) == ImageDecodeResult::Ok, "a PNG decodes");
    CHECK(image.valid() && image.pixels.size() == static_cast<size_t>(image.width * image.height * 4),
          "to tightly packed RGBA");

    DecodedImage untouched;
    CHECK(decoder.decode(junk, sizeof junk, untouched) == ImageDecodeResult::UnknownFormat,
          "and a non-image is refused");
    CHECK(!untouched.valid(),
          "leaving the output untouched, so a failed decode cannot look like half an image");
}

/* ================== the real FrameClock, on a driven time source ===========
 *
 * The checks above exercise a stub so the INTERFACE's contract is pinned even
 * where no implementation exists. These exercise the shipping class, driven by
 * a time source the test moves by hand — which is exactly what setTimeSource is
 * for, and means the clamp being tested is the one the game runs. */

class DrivenTime {
public:
    double now() const { return seconds_; }
    void advance(double by) { seconds_ += by; }

private:
    double seconds_ = 1000.0;   /* not zero, so an accidental absolute read shows up */
};

void frameClockFirstTickMeasuresNothing()
{
    DrivenTime time;
    FrameClock clock;
    clock.setTimeSource([&time] { return time.now(); });

    /* On a cold start the previous sample is the process launch, and using it
     * would make frame one as long as the shader compile — the worst possible
     * first delta to hand a simulation. */
    clock.tick();
    CHECK(nearly(clock.delta(), 0.0f), "the first tick measures nothing (%.4f)", clock.delta());
    CHECK(!clock.hitched(), "and is not reported as a hitch");
}

void frameClockClampsAndKeepsTheRawSpike()
{
    DrivenTime time;
    FrameClock clock;
    clock.setTimeSource([&time] { return time.now(); });
    clock.setMaxDelta(0.1f);
    clock.tick();

    time.advance(0.016);
    clock.tick();
    CHECK(nearly(clock.delta(), 0.016f, 1e-4f), "an ordinary frame passes through");

    /* The window was dragged, or a breakpoint stepped. */
    time.advance(2.5);
    clock.tick();
    CHECK(nearly(clock.delta(), 0.1f, 1e-4f), "a stall is clamped (%.4f)", clock.delta());
    CHECK(clock.hitched(), "and flagged");
    CHECK(nearly(clock.rawDelta(), 2.5f, 1e-3f),
          "while the profiler still sees the true spike (%.3f)", clock.rawDelta());
}

void frameClockElapsedFollowsTheClampedTime()
{
    DrivenTime time;
    FrameClock clock;
    clock.setTimeSource([&time] { return time.now(); });
    clock.setMaxDelta(0.1f);
    clock.tick();

    time.advance(0.02);
    clock.tick();
    time.advance(5.0);          /* a hitch */
    clock.tick();

    /* SUMMING THE RAW VALUE WOULD DRIFT. Anything animating from elapsed()
     * would gain five seconds on anything integrating delta(), and the two
     * would disagree by the total of every hitch all session — invisible until
     * something compares them. */
    CHECK(nearly(static_cast<float>(clock.elapsed()), 0.12f, 1e-3f),
          "elapsed sums what the game believes happened (%.4f)", clock.elapsed());
}

void frameClockSkipDiscardsAResume()
{
    DrivenTime time;
    FrameClock clock;
    clock.setTimeSource([&time] { return time.now(); });
    clock.tick();

    /* Suspended to the system menu for half a minute. Clamping alone still
     * yields one full-length frame of everything happening at once. */
    time.advance(30.0);
    clock.skipNextDelta();
    clock.tick();

    CHECK(nearly(clock.delta(), 0.0f), "a resume produces no frame at all (%.4f)", clock.delta());
    CHECK(!clock.hitched(), "and is not a hitch, because nothing was dropped");
}

void frameClockRejectsNonsenseSettings()
{
    FrameClock clock;

    /* A ceiling of zero would freeze the simulation while every clock reading
     * still advanced — everything animates, nothing moves. */
    clock.setMaxDelta(0.0f);
    CHECK(clock.maxDelta() > 0.0f, "a zero ceiling is refused (%.4f)", clock.maxDelta());

    clock.setMaxDelta(-1.0f);
    CHECK(clock.maxDelta() > 0.0f, "and so is a negative one");

    /* Rewinding is done by replaying, not by integrating backwards; every
     * consumer of delta assumes it goes forwards. */
    clock.setTimeScale(-2.0f);
    CHECK(nearly(clock.timeScale(), 0.0f), "negative time scale is clamped to zero");
}

void frameClockSurvivesANonMonotonicSource()
{
    DrivenTime time;
    FrameClock clock;
    clock.setTimeSource([&time] { return time.now(); });
    clock.tick();

    time.advance(-5.0);   /* a source that went backwards */
    clock.tick();

    CHECK(clock.delta() >= 0.0f, "a backwards jump never produces negative time (%.3f)",
          clock.delta());
}

/* ================== the real PcFileSystem =============================*/

void desktopStorageRefusesToWriteAssets()
{
    PcFileSystem files("cromwell_tests");

    /* The console rule, enforced on the developer's machine even though the
     * directory is perfectly writable. */
    CHECK(files.write(StorageKind::Asset, "shaders/x.frag", "x", 1) == StorageResult::AccessDenied,
          "shipped content is read-only here too");
    CHECK(files.remove(StorageKind::Asset, "shaders/x.frag") == StorageResult::AccessDenied,
          "and cannot be deleted");
}

void desktopStorageKeepsKindsApart()
{
    PcFileSystem files("cromwell_tests");

    CHECK(files.write(StorageKind::Save, "slot1", "alpha", 5) == StorageResult::Ok,
          "a save writes");
    CHECK(files.exists(StorageKind::Save, "slot1"), "and is there");

    /* Same name, different kind: a genuinely different object, which is the
     * whole point of naming storage by kind rather than by path. */
    CHECK(!files.exists(StorageKind::Settings, "slot1"),
          "the same name under settings is a different object");

    std::string text;
    CHECK(files.readText(StorageKind::Save, "slot1", text) == StorageResult::Ok
              && text == "alpha",
          "it reads back exactly (%s)", text.c_str());

    CHECK(files.remove(StorageKind::Save, "slot1") == StorageResult::Ok, "and deletes");
}

void desktopStorageRefusesToEscapeItsRoot()
{
    PcFileSystem files("cromwell_tests");

    /* StorageKind's separation is enforced by the platform on console and by
     * nothing at all here — so it is enforced here instead. Without this a save
     * could write into the settings container by name alone. */
    CHECK(files.write(StorageKind::Save, "../settings/stolen", "x", 1)
              == StorageResult::AccessDenied,
          "a name cannot climb out of its storage root");
    CHECK(!files.exists(StorageKind::Save, "../../anything"),
          "and cannot read out of it either");
}

void desktopStorageWriteIsAtomic()
{
    PcFileSystem files("cromwell_tests");

    CHECK(files.write(StorageKind::Save, "atomic", "original", 8) == StorageResult::Ok, "");
    CHECK(files.write(StorageKind::Save, "atomic", "replacement", 11) == StorageResult::Ok,
          "overwriting an existing save succeeds");

    std::string text;
    files.readText(StorageKind::Save, "atomic", text);
    CHECK(text == "replacement", "and fully replaces it (%s)", text.c_str());

    /* The half-written temporary must never appear in a save list — a load
     * menu offering "slot1.tmp" is the visible half of this bug. */
    std::vector<std::string> saves;
    files.list(StorageKind::Save, saves);
    bool sawTemporary = false;
    for (const std::string& name : saves)
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) sawTemporary = true;
    CHECK(!sawTemporary, "no temporary files are listed");

    files.remove(StorageKind::Save, "atomic");
}

}  // namespace

int main()
{
    surfaceWithoutAWindowStillWorks();
    closeRequestCanBeDeclined();
    resizeIsATakeNotAPoll();

    gamepadAxesAreRawSoTheGameOwnsItsDeadZone();
    textEntryIsAStateMachineNotAKeyQueue();

    theClockClampsAHitch();
    skipDiscardsAResumeGap();
    pauseAndScaleLeaveRealTimeAlone();

    storageKindsAreSeparateNamespaces();
    missingFileIsDistinguishableFromAFailure();
    decoderSniffsRatherThanTrustingAName();

    frameClockFirstTickMeasuresNothing();
    frameClockClampsAndKeepsTheRawSpike();
    frameClockElapsedFollowsTheClampedTime();
    frameClockSkipDiscardsAResume();
    frameClockRejectsNonsenseSettings();
    frameClockSurvivesANonMonotonicSource();

    desktopStorageRefusesToWriteAssets();
    desktopStorageKeepsKindsApart();
    desktopStorageRefusesToEscapeItsRoot();
    desktopStorageWriteIsAtomic();

    if (g_failures == 0) {
        std::printf("platform: all checks passed\n");
        return 0;
    }
    std::printf("platform: %d check(s) failed\n", g_failures);
    return 1;
}
