/* IFileSystem.hpp — reading and writing, where those words mean four different
 * things.
 *
 * SINGLE RESPONSIBILITY: get bytes in and out of the platform's storage, under
 * names that do not assume a filesystem exists.
 *
 * ================ WHY A PATH STRING IS NOT ENOUGH ON CONSOLE ==============
 *
 * On desktop, "read a file" and "write a file" are the same operation in
 * opposite directions, against one namespace, synchronously, and they either
 * work or the file was missing. Almost none of that survives a console port:
 *
 *   - GAME CONTENT is read-only, mounted at a platform-specific root, and often
 *     inside an archive rather than loose on a disc.
 *
 *   - SAVE DATA is a separate, quota'd, per-user container that must be opened
 *     and committed as a transaction. It is not a directory. Writing a save by
 *     path is the single most common thing a desktop-first codebase gets wrong,
 *     because the console API wants "here is the whole save, commit it" and the
 *     desktop code wants an fwrite loop.
 *
 *   - SETTINGS may be per-user too, and the user can CHANGE mid-session on a
 *     console where controllers come and go.
 *
 *   - Nothing is guaranteed synchronous. A read can take a disc spin-up.
 *
 * So the interface names the KIND of storage rather than taking a path and
 * hoping. `readAsset("shaders/pbr.frag")` is a different operation from
 * `readSave("campaign1")` even where both land in the same folder on Windows,
 * and having them be the same call is exactly what makes the port expensive.
 *
 * ===================== BLOBS, NOT STREAMS, FOR NOW ========================
 *
 * Every current consumer reads a whole file and parses it — a shader, a model,
 * a settings blob, a lightmap. None of them streams, so a stream abstraction
 * would be four extra methods per backend serving nobody. When something does
 * need to stream (audio is the usual first), it gets its own interface rather
 * than complicating this one; see the note in ISceneSource.hpp about interfaces
 * growing methods for hypothetical callers.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cromwell {

/* WHICH NAMESPACE A NAME IS IN. Not a directory — a KIND of storage, which the
 * backend maps to whatever its platform actually has. */
enum class StorageKind : uint8_t {
    /* Shipped game content. Read-only on every target, and read-only here even
     * on desktop where it physically is not, so a write that should have gone
     * to save data fails on the developer's machine rather than on console. */
    Asset,

    /* Per-user save data. A transaction on console; a file on desktop. */
    Save,

    /* Per-user configuration. Separate from Save because platforms treat them
     * differently — settings usually survive a save-data wipe. */
    Settings,

    /* Logs, crash dumps, profile captures. May be a no-op on a retail console
     * build, which is the correct behaviour rather than a failure. */
    Diagnostics,
};

/* Why a read or write did not happen. An enum rather than a bool because the
 * caller's response genuinely differs: NotFound for an optional asset is
 * routine, QuotaExceeded needs a message to the player, and Busy means try
 * again next frame. */
enum class StorageResult : uint8_t {
    Ok = 0,
    NotFound,
    AccessDenied,
    QuotaExceeded,
    Corrupt,
    Busy,          /* asynchronous operation still in flight */
    Unsupported,   /* e.g. Diagnostics writes on a retail console build */
};

class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    /* ---- reading --------------------------------------------------------
     *
     * Appends to `out` rather than returning a vector, so a caller reading many
     * files can reuse one buffer — the allocation-in-a-loop rule from
     * CLAUDE.md, applied where it is cheap to honour. */
    virtual StorageResult read(StorageKind kind, const char* name,
                               std::vector<uint8_t>& out) = 0;

    /* Text convenience. Same storage, same rules; exists because most callers
     * here want a shader source or a JSON document and would otherwise write
     * the same three lines converting. */
    virtual StorageResult readText(StorageKind kind, const char* name,
                                   std::string& out) = 0;

    virtual bool exists(StorageKind kind, const char* name) const = 0;

    /* ---- writing --------------------------------------------------------
     *
     * WHOLE-BLOB, and that is the console save model rather than a limitation.
     * A save is opened, written and committed as one transaction; offering an
     * incremental write here would be an API that only desktop can honour. */
    virtual StorageResult write(StorageKind kind, const char* name,
                                const void* data, size_t bytes) = 0;

    virtual StorageResult remove(StorageKind kind, const char* name) = 0;

    /* Names present in a storage kind. Appends. Used by the save-game list and
     * by asset manifests; not a directory walk, because there may be no
     * directories. */
    virtual StorageResult list(StorageKind kind, std::vector<std::string>& out) = 0;

    /* ---- the asynchronous reality ---------------------------------------
     *
     * True while any operation is still in flight. The desktop backend always
     * returns false because its calls complete before returning; a console
     * backend may return Busy from a read and finish it later.
     *
     * A caller that cannot cope with Busy should call flush(), which blocks —
     * that is the honest way to write loading code that is synchronous today
     * and can be made asynchronous later, rather than pretending the platform
     * is synchronous and discovering otherwise at certification. */
    virtual bool busy() const = 0;
    virtual void flush() = 0;
};

}  // namespace cromwell
