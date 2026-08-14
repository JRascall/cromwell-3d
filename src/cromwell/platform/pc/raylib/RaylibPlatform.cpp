#include "cromwell/platform/pc/raylib/RaylibPlatform.hpp"

#include "cromwell/diag/Logger.hpp"
#include "cromwell/rhi/IRenderDevice.hpp"

#include "raylib.h"

namespace cromwell {
namespace {

/* GLFW's PROC-ADDRESS GETTER, DECLARED RATHER THAN INCLUDED.
 *
 * The render device needs entry points the window backend's GL loader did not
 * resolve — see the note on glClipControl in OpenGlRenderDevice.cpp — and only
 * the thing that CREATED the context can find them. raylib's window is a GLFW
 * window, so GLFW's getter is the right answer here and would be
 * wglGetProcAddress or eglGetProcAddress in a backend that made its own
 * context.
 *
 * Declared by hand because raylib does not re-export it and does not put GLFW's
 * headers on its public include path — the library is linked into this binary
 * either way, so this resolves at link time. `glfwGetProcAddress` has had this
 * signature since GLFW 3.0 and is not going to move.
 *
 * IT LIVES IN THE BACKEND, which is the whole point: the device is handed a
 * function pointer and never learns that GLFW exists, so a console backend
 * hands it something else and nothing under rhi/ changes. */
extern "C" void (*glfwGetProcAddress(const char* procname))();

rhi::OpenGlRenderDevice::ProcAddress loadGlProc(const char* name)
{
    return glfwGetProcAddress(name);
}

}  // namespace

RaylibPlatform::RaylibPlatform() = default;

std::unique_ptr<RaylibPlatform> RaylibPlatform::open(const PlatformDesc& desc)
{
    /* 1. FLAGS FIRST. These are read by InitWindow and ignored afterwards, so
     *    setting vsync or MSAA later is a setting that silently does nothing. */
    unsigned int flags = FLAG_MSAA_4X_HINT;
    if (desc.vsync)       flags |= FLAG_VSYNC_HINT;
    if (desc.resizable)   flags |= FLAG_WINDOW_RESIZABLE;
    if (desc.startHidden) flags |= FLAG_WINDOW_HIDDEN;
    SetConfigFlags(flags);

    /* 2. THE WINDOW, which is also the GL context every later step needs. */
    InitWindow(desc.width, desc.height, desc.applicationName);
    if (!IsWindowReady()) {
        LOGGER.error("RaylibPlatform: the window could not be opened - no display, "
                     "no GL context, or a driver that refused the requested flags");
        return nullptr;
    }

    /* raylib installs its own ESC-quits handler, which makes Escape unusable as
     * a game binding and closes the window out from under a modal dialogue. The
     * engine reports close requests through ISurface instead, where they can be
     * declined. */
    SetExitKey(KEY_NULL);

    if (desc.targetFrameRate > 0) SetTargetFPS(desc.targetFrameRate);

    /* `new` rather than make_unique because the constructor is private — the
     * point of that being that the ORDER above cannot be skipped by
     * constructing one directly. */
    std::unique_ptr<RaylibPlatform> platform(new RaylibPlatform());

    /* Pacing is the surface's because present() is where it is applied — see
     * RaylibSurface::present. SetTargetFPS above still covers the raylib path,
     * which paces inside EndDrawing. */
    platform->surface_.setTargetFrameRate(desc.targetFrameRate);

    /* 3. THE RENDER DEVICE, which needs the context InitWindow just made. This
     *    is the step whose ordering has no useful error attached to it: build
     *    a device before the window and every GL call silently does nothing. */
    platform->device_ = rhi::OpenGlRenderDevice::create(&loadGlProc, desc.debugGraphics);
    if (!platform->device_) {
        LOGGER.error("RaylibPlatform: the render device could not be created - "
                     "the machine does not meet the GL 4.3 feature level");
        return nullptr;
    }

    /* THE SCREEN'S SIZE, BEFORE ANY FRAME RUNS, so the first one is right
     * rather than relying on the device's viewport fallback. */
    platform->refreshBackbufferSize();

    /* 4. The rest, which touch neither the window nor the context. */
    platform->files_ = std::make_unique<PcFileSystem>(desc.applicationName);

    if (desc.fullscreen) platform->surface_.setFullscreen(true);

    LOGGER.info("RaylibPlatform: {}x{} window, vsync {}, assets at {}",
                GetRenderWidth(), GetRenderHeight(), desc.vsync ? "on" : "off",
                platform->files_->rootOf(StorageKind::Asset));

    return platform;
}

RaylibPlatform::~RaylibPlatform()
{
    /* REVERSE ORDER, and the window closes last. Anything holding GPU objects
     * must be destroyed while its context is still alive — after CloseWindow
     * the handles are dangling, which leaks at best and crashes on the way out
     * on some drivers.
     *
     * THIS BODY RUNS BEFORE ANY MEMBER IS DESTROYED, so the device has to be
     * released explicitly here rather than left to the compiler — otherwise
     * CloseWindow below would destroy the context first and every texture,
     * buffer and program the device owns would be freed into nothing. */
    device_.reset();
    files_.reset();

    if (IsWindowReady()) CloseWindow();
}

rhi::IRenderDevice& RaylibPlatform::device() { return *device_; }

void RaylibPlatform::refreshBackbufferSize()
{
    /* THE DEVICE CANNOT ASK. It does not own the window — that is the whole
     * point of the platform layer — and it needs the answer for any pass that
     * targets the screen rather than a texture. Without it the GL backend fell
     * back to reading the current viewport, which is whatever the LAST PASS
     * set: the screen was being drawn with the supersampled scene target's
     * viewport, at twice its width and height. See IRenderDevice.
     *
     * GetRenderWidth, via ISurface::size, so this is REAL PIXELS on a high-DPI
     * display rather than the logical size — a render target must be sized in
     * the units it is rasterised in, and the two differ by the scale factor. */
    if (!device_) return;

    int width = 0;
    int height = 0;
    surface_.size(width, height);
    if (width <= 0 || height <= 0) return;

    device_->setBackbufferSize(static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height));
}

void RaylibPlatform::beginFrame()
{
    /* THE ORDER IS THE REASON THIS FUNCTION EXISTS — see IPlatform.hpp.
     *
     * Events first, because polling input before pumping reads last frame's
     * state. The clock last, so its delta includes the time the events and the
     * input poll took rather than excluding it. Written once here rather than
     * at the top of every loop that will ever run. */
    surface_.pumpEvents();
    input_.poll();
    clock_.tick();

    refreshBackbufferSize();

    /* A RESIZE OR A RETURN FROM A MODAL DRAG IS A TIME DISCONTINUITY. Windows
     * blocks inside its own modal loop while a window is dragged or resized, so
     * the first frame afterwards measures however long the player held the
     * mouse. The clamp would turn that into one slow frame; discarding it
     * entirely is what that case actually wants.
     *
     * Checked AFTER the tick so the discarded frame is the next one, which is
     * the frame that follows the stall rather than the one containing it. */
    if (surface_.takeResized() || clock_.hitched()) clock_.skipNextDelta();
}

void RaylibPlatform::endFrame()
{
    /* THE SCREEN'S SIZE, REFRESHED FOR THE NEXT FRAME.
     *
     * It belongs in beginFrame and is repeated here because nothing calls
     * beginFrame yet — Application still pumps events, polls input and ticks
     * the clock itself, and adopting beginFrame means restructuring the main
     * loop, which is not a change to make in passing when the input path is
     * this sensitive to double-pumping.
     *
     * So: open() sets it once so the first frame is right, and this sets it
     * after every frame so a resize is picked up. A resize is therefore one
     * frame late, which is invisible — the surface reports the new size in the
     * same frame the swap chain gets it, and the frame in between is the one
     * ISurface::takeResized already tells the loop to discard.
     *
     * DELETE THIS the moment Application calls beginFrame. */
    refreshBackbufferSize();

    /* SUBMIT, THEN SHOW — two steps, on two objects, deliberately. The device
     * flushes what was recorded; the surface swaps the buffer. See
     * ISurface::present for why presenting belongs to windowing rather than to
     * the graphics API.
     *
     * ONLY CALLED ON FRAMES THE DEVICE DREW, while the migration is under way:
     * raylib's EndDrawing already swaps, so calling this after one would swap
     * twice — a black or torn frame depending on the driver. Application calls
     * it only under the rhi renderer, and unconditionally once the raylib path
     * is gone. */
    if (device_) device_->present();
    surface_.present();
}

IPlatform::Lifecycle RaylibPlatform::takeLifecycleChange()
{
    /* DESKTOP HAS NO SUSPEND. There is no system menu to be pushed into and no
     * user change mid-session, so the only transition is the one out. A console
     * backend reports the rest through this same call, and a loop written
     * against it here is already correct there. */
    if (surface_.closeRequested() && !exitReported_) {
        exitReported_ = true;
        return Lifecycle::Exiting;
    }

    /* Cleared if the request was declined, so a "really quit?" the player backs
     * out of can be asked again next time. */
    if (!surface_.closeRequested()) exitReported_ = false;

    return Lifecycle::Running;
}

/* THE ONE DEFINITION, per build. A second backend linked alongside this is a
 * duplicate-symbol error at link time, which is the correct way to discover it
 * — better than a runtime branch that could pick the wrong one. */
std::unique_ptr<IPlatform> IPlatform::create(const PlatformDesc& desc)
{
    return RaylibPlatform::open(desc);
}

}  // namespace cromwell
