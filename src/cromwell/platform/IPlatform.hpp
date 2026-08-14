/* IPlatform.hpp — everything the machine provides, behind one door.
 *
 * SINGLE RESPONSIBILITY: own the platform's services for the life of the
 * process, and be the single thing a backend has to supply to run cromwell
 * somewhere new.
 *
 * ====================== WHAT PORTING COSTS, STATED ========================
 *
 * That is the only honest measure of whether this abstraction is worth having,
 * so here it is: a new platform implements ISurface, IInput, IClock,
 * IFileSystem, IImageDecoder and rhi::IRenderDevice, and writes one factory
 * that returns an IPlatform holding them. Nothing else in the engine or the
 * game changes.
 *
 * Six interfaces is not a small number. It is, however, an enumerable one — and
 * before this existed the answer was "raylib appears in 107 of 400 files, and
 * you find out which ones by breaking the build". The interfaces did not create
 * that work; they made it countable, and they let the port be written and
 * tested alongside the working one instead of replacing it.
 *
 * ======================= WHY ONE ROOT AND NOT SIX =========================
 *
 * Because the services have a shared lifetime and a required construction
 * ORDER, and that order is not guessable. The surface has to exist before the
 * render device (which needs its native handle and its size), the device before
 * anything that creates a texture, and input after the surface (it polls that
 * surface's message queue). A caller assembling six objects itself would get
 * that order right by copying an example, which lasts exactly until someone
 * writes the seventh.
 *
 * So the root owns the ordering, and `create()` either hands back a fully
 * constructed platform or nothing at all.
 *
 * ==================== ACCESSORS RETURN REFERENCES ==========================
 *
 * Not pointers, and not optionals. Every one of these exists for the whole life
 * of the platform — a device that failed to create means `create()` returned
 * null and there is no IPlatform to ask. That keeps `platform.input().down(...)`
 * free of a null check that could never fire, which matters because these are
 * the most-called accessors in the codebase.
 *
 * The one genuinely optional service is the image decoder on a stripped build,
 * and it is a null implementation returning UnknownFormat rather than a null
 * pointer — the failure is then reported through the same path as a corrupt
 * file, which callers already handle.
 */
#pragma once

#include <memory>

namespace cromwell {

namespace rhi { class IRenderDevice; }

class ISurface;
class IInput;
class IClock;
class IFileSystem;
class IImageDecoder;

/* What to ask for at startup. The backend may not honour all of it — a console
 * ignores the size and gives you the display's — and what you actually got is
 * read back off ISurface afterwards rather than assumed from this. */
struct PlatformDesc {
    const char* applicationName = "cromwell";

    int  width      = 1280;
    int  height     = 720;
    bool fullscreen = false;
    bool resizable  = true;
    bool vsync      = true;

    /* CREATE THE SURFACE HIDDEN, to be revealed once the first frame has been
     * presented — see ISurface::setVisible for why that is worth the two extra
     * calls. Ignored where a surface cannot be hidden. */
    bool startHidden = false;

    /* 0 leaves the platform's own pacing alone, which with vsync on is what
     * almost everything wants. A non-zero value caps the frame rate for a
     * machine where vsync is off or unavailable. */
    int targetFrameRate = 0;


    /* Ask the device for a debug context: validation layers, object labels,
     * synchronous error reporting. Costs performance and is worth it in a
     * development build — every backend has some form of it and none of them
     * enable it by default. */
    bool debugGraphics = false;
};

class IPlatform {
public:
    virtual ~IPlatform() = default;

    /* THE ONE FACTORY. Implemented by exactly one backend per build — which one
     * is a link-time decision in CMakeLists, not a runtime branch, because a
     * console build must not contain a desktop backend's code at all.
     *
     * Returns null when the platform could not be brought up, having already
     * logged why. That is a real outcome: no GPU meeting the feature level, a
     * display server that is not running, a console storage mount that failed.
     * The caller's job is to report it and exit, not to continue. */
    static std::unique_ptr<IPlatform> create(const PlatformDesc& desc);

    virtual ISurface&           surface() = 0;
    virtual IInput&             input() = 0;
    virtual IClock&             clock() = 0;
    virtual IFileSystem&        files() = 0;
    virtual IImageDecoder&      images() = 0;
    virtual rhi::IRenderDevice& device() = 0;

    /* ---- the frame's edges ----------------------------------------------
     *
     * beginFrame pumps the surface's events, polls input and ticks the clock,
     * IN THAT ORDER. It exists so the order is written once rather than at the
     * top of every loop that ever runs — a tool, a test harness, a replay
     * player — because getting it wrong is subtle: polling input before pumping
     * events reads last frame's state, and ticking the clock first means the
     * delta excludes the time the events took.
     *
     * endFrame presents. Kept separate from IRenderDevice::present so that a
     * backend needing to do something else at the frame boundary — a console
     * suspend check, a frame-pacing wait — has somewhere to put it. */
    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;

    /* ---- suspend and resume ---------------------------------------------
     *
     * THE CONSOLE LIFECYCLE, WHICH DESKTOP DOES NOT HAVE. A console application
     * can be suspended to the system menu, have its user change, be told a
     * controller disconnected mid-match, or be asked to release the GPU
     * entirely. None of that is optional to handle — several are certification
     * requirements — and all of it arrives as a state change the loop has to
     * notice.
     *
     * A poll rather than callbacks, matching ISurface::takeResized and for the
     * same reason: the game reacts at a defined point in its frame rather than
     * re-entrantly from inside a system callback on an unknown thread.
     *
     * Desktop backends return Running forever, so a loop written against this
     * costs one comparison there and is already correct when it ships to a
     * console. */
    enum class Lifecycle { Running, Suspending, Resumed, Exiting };
    virtual Lifecycle takeLifecycleChange() = 0;
};

}  // namespace cromwell
