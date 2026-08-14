/* RaylibPlatform.hpp — the desktop backend, assembled in the one order that works.
 *
 * SINGLE RESPONSIBILITY: open the window, construct the platform services
 * against it in the required order, and tear them down in reverse.
 *
 * ====================== THE ORDER IS THE WHOLE JOB ========================
 *
 * It is not guessable and getting it wrong fails in ways that do not name the
 * cause:
 *
 *   1. CONFIG FLAGS BEFORE THE WINDOW. raylib's SetConfigFlags only takes
 *      effect at InitWindow — vsync, MSAA and resizability set afterwards are
 *      silently ignored, which reads as "the vsync setting does nothing".
 *   2. THE WINDOW BEFORE THE RENDER DEVICE. The device needs a live GL context,
 *      and the context is created by InitWindow. There is no useful error for
 *      getting this backwards; GL calls simply do nothing.
 *   3. THE WINDOW BEFORE INPUT. Input polls that window's event queue.
 *   4. STORAGE AND THE CLOCK ANY TIME. Neither touches the window, which is
 *      exactly why neither has a raylib implementation at all.
 *
 * Teardown reverses it, and the window closes last — a device destroyed after
 * its context is gone leaks every GPU object it owned, and on some drivers
 * crashes on the way out.
 *
 * ================= WHY THE FACTORY LIVES IN THIS FILE =====================
 *
 * IPlatform::create is declared once and DEFINED by exactly one backend per
 * build. Which one is a CMakeLists decision, not a runtime branch, because a
 * console build must not contain desktop code at all — not merely avoid calling
 * it. Two backends linked together is a duplicate-symbol error, which is the
 * correct and immediate way to find out.
 */
#pragma once

#include "cromwell/platform/pc/PcFileSystem.hpp"
#include "cromwell/platform/FrameClock.hpp"
#include "cromwell/platform/IPlatform.hpp"
#include "cromwell/platform/pc/raylib/RaylibImageDecoder.hpp"
#include "cromwell/platform/pc/raylib/RaylibInput.hpp"
#include "cromwell/platform/pc/raylib/RaylibSurface.hpp"
#include "cromwell/rhi/pc/opengl/OpenGlRenderDevice.hpp"

#include <memory>

namespace cromwell {

class RaylibPlatform final : public IPlatform {
public:
    /* Null when the window could not be opened. Prefer IPlatform::create. */
    static std::unique_ptr<RaylibPlatform> open(const PlatformDesc& desc);

    ~RaylibPlatform() override;

    RaylibPlatform(const RaylibPlatform&) = delete;
    RaylibPlatform& operator=(const RaylibPlatform&) = delete;

    ISurface&           surface() override { return surface_; }
    IInput&             input() override { return input_; }
    IClock&             clock() override { return clock_; }
    IFileSystem&        files() override { return *files_; }
    IImageDecoder&      images() override { return images_; }
    rhi::IRenderDevice& device() override;

    void beginFrame() override;
    void endFrame() override;

private:
    /* Tells the device how big the screen is. See the definition. */
    void refreshBackbufferSize();

public:

    Lifecycle takeLifecycleChange() override;

private:
    RaylibPlatform();

    RaylibSurface       surface_;
    RaylibInput         input_;
    FrameClock          clock_;
    RaylibImageDecoder  images_;

    /* By pointer only because it is constructed with the application name,
     * which arrives with the descriptor rather than at member-init time. */
    std::unique_ptr<PcFileSystem> files_;

    /* DECLARED LAST SO IT IS DESTROYED FIRST. It owns GL objects, and the
     * window — whose context those objects live in — is closed in this class's
     * destructor body, which runs before any member is destroyed. Ordering the
     * other way round would release textures into a dead context, which leaks
     * on a good driver and crashes on the rest. */
    std::unique_ptr<rhi::OpenGlRenderDevice> device_;

    /* Desktop has no suspend, so this only ever reports Exiting. Latched here
     * rather than derived from the surface each call, so that taking it clears
     * it — the same take-once shape as ISurface::takeResized. */
    Lifecycle pending_ = Lifecycle::Running;
    bool      exitReported_ = false;
};

}  // namespace cromwell
