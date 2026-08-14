/* PcFileSystem.hpp — storage on a machine that has directories.
 *
 * SINGLE RESPONSIBILITY: implement IFileSystem for Windows, Linux and macOS by
 * mapping each StorageKind onto a real directory.
 *
 * ================= WHY ONE CLASS FOR THREE DESKTOP TARGETS ================
 *
 * Because std::filesystem covers all three, and the only genuine difference is
 * WHERE the per-user directories live: %APPDATA% on Windows,
 * ~/.local/share on Linux, ~/Library/Application Support on macOS. That is one
 * function, not one class.
 *
 * There is deliberately no raylib in here. raylib offers LoadFileData and
 * GetApplicationDirectory, and using them would tie the one part of the
 * platform layer that is genuinely portable to a windowing library — which is
 * how a headless asset cooker ends up needing a GPU.
 *
 * ================= THE ASSET ROOT IS PROBED, NOT ASSUMED ==================
 *
 * The same probe ShaderLibrary already does, and for the same reason: the
 * executable is run both from the staging directory and from the project root
 * during development, so the assets are one, two or three levels up depending
 * on who launched it. Probing is what makes both work without a launch
 * configuration.
 *
 * ============== WHAT THIS DELIBERATELY DOES NOT DO: PRETEND ===============
 *
 * Writes to StorageKind::Asset return AccessDenied even though the directory is
 * perfectly writable here. Shipped content is read-only on every console, and a
 * save misrouted to it must fail on a developer's machine — where it costs a
 * minute — rather than at certification, where it costs a submission.
 *
 * Equally, `busy()` is always false and `flush()` does nothing, because these
 * calls really are synchronous here. A console backend will not be, which is
 * why callers are expected to handle StorageResult::Busy even though nothing
 * on desktop ever returns it.
 */
#pragma once

#include "cromwell/platform/IFileSystem.hpp"

#include <string>

namespace cromwell {

class PcFileSystem final : public IFileSystem {
public:
    /* `applicationName` names the per-user directories — save data and settings
     * land under it, so two projects built from this engine do not overwrite
     * each other's saves. */
    explicit PcFileSystem(const char* applicationName);

    StorageResult read(StorageKind kind, const char* name,
                       std::vector<uint8_t>& out) override;
    StorageResult readText(StorageKind kind, const char* name, std::string& out) override;
    bool          exists(StorageKind kind, const char* name) const override;

    StorageResult write(StorageKind kind, const char* name,
                        const void* data, size_t bytes) override;
    StorageResult remove(StorageKind kind, const char* name) override;
    StorageResult list(StorageKind kind, std::vector<std::string>& out) override;

    bool busy() const override { return false; }
    void flush() override {}

    /* Where each kind resolved to, for the startup log. Worth printing: "my
     * save did not appear" is nearly always a path question, and the answer
     * should not require attaching a debugger. */
    const std::string& rootOf(StorageKind kind) const;

private:
    /* Empty when `name` escapes its root — see the note in the .cpp on why a
     * path is validated rather than trusted. */
    std::string resolve(StorageKind kind, const char* name) const;

    std::string assets_;
    std::string saves_;
    std::string settings_;
    std::string diagnostics_;
};

}  // namespace cromwell
