#include "cromwell/platform/pc/PcFileSystem.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace cromwell {
namespace {

namespace fs = std::filesystem;

/* WHERE PER-USER DATA GOES, which is the only thing that genuinely differs
 * between the three desktop targets. Falls back to a local directory when the
 * environment says nothing — a portable build, a CI runner with no home
 * directory — because failing to start over a missing environment variable
 * would be a worse outcome than writing beside the executable. */
fs::path userDataRoot(const std::string& applicationName)
{
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"))
        return fs::path(appData) / applicationName;
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / "Library" / "Application Support" / applicationName;
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"))
        return fs::path(xdg) / applicationName;
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / ".local" / "share" / applicationName;
#endif
    return fs::path("userdata") / applicationName;
}

/* THE SAME PROBE ShaderLibrary DOES, and it has to stay the same: the
 * executable is launched both from builds/win and from the project root, so the
 * asset tree is one, two or three levels up depending on who started it.
 * Returns "assets" unfound rather than empty, so a missing tree surfaces as a
 * NotFound on the first read with a path in the message. */
fs::path probeAssetRoot()
{
    const fs::path candidates[] = {
        "assets", "../assets", "../../assets", "../../../assets",
    };

    std::error_code ec;
    for (const fs::path& candidate : candidates)
        if (fs::is_directory(candidate, ec)) return fs::absolute(candidate, ec);

    return fs::path("assets");
}

bool ensureDirectory(const fs::path& path)
{
    std::error_code ec;
    if (fs::is_directory(path, ec)) return true;
    fs::create_directories(path, ec);
    return !ec;
}

}  // namespace

PcFileSystem::PcFileSystem(const char* applicationName)
{
    const std::string name = (applicationName != nullptr && *applicationName != '\0')
                                 ? applicationName
                                 : "cromwell";

    const fs::path userRoot = userDataRoot(name);

    assets_      = probeAssetRoot().string();
    saves_       = (userRoot / "saves").string();
    settings_    = (userRoot / "settings").string();
    diagnostics_ = (userRoot / "diagnostics").string();

    /* CREATED EAGERLY, and only the writable ones. A save that fails because
     * its directory did not exist is a failure the player sees at the worst
     * possible moment; making them at startup means a permissions problem
     * surfaces while there is still nothing to lose. The asset root is not
     * created — if it is missing, that is a broken install and inventing an
     * empty directory would only turn a clear error into a confusing one. */
    ensureDirectory(saves_);
    ensureDirectory(settings_);
    ensureDirectory(diagnostics_);
}

const std::string& PcFileSystem::rootOf(StorageKind kind) const
{
    switch (kind) {
        case StorageKind::Save:        return saves_;
        case StorageKind::Settings:    return settings_;
        case StorageKind::Diagnostics: return diagnostics_;
        case StorageKind::Asset:
        default:                       return assets_;
    }
}

std::string PcFileSystem::resolve(StorageKind kind, const char* name) const
{
    if (name == nullptr || *name == '\0') return {};

    const fs::path root = rootOf(kind);
    const fs::path full = root / name;

    /* A NAME MAY NOT ESCAPE ITS ROOT.
     *
     * StorageKind exists to keep shipped content, save data and settings
     * genuinely separate — that separation is enforced by the platform on
     * console and by nothing at all here, so it is enforced here instead. A
     * name like "../../settings/x" would otherwise let a save write reach the
     * settings container, and the whole point of the kinds is that it cannot.
     *
     * Lexical rather than canonical, deliberately: canonical needs the file to
     * exist, which is false for every write and for every exists() that returns
     * false. */
    const fs::path normalisedRoot = root.lexically_normal();
    const fs::path normalisedFull = full.lexically_normal();

    const std::string rootText = normalisedRoot.string();

    /* Non-const so the return moves rather than copies — every read and write
     * goes through here. */
    std::string fullText = normalisedFull.string();

    if (fullText.size() < rootText.size()) return {};
    if (fullText.compare(0, rootText.size(), rootText) != 0) return {};

    return fullText;
}

StorageResult PcFileSystem::read(StorageKind kind, const char* name,
                                      std::vector<uint8_t>& out)
{
    const std::string path = resolve(kind, name);
    if (path.empty()) return StorageResult::AccessDenied;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return StorageResult::NotFound;

    const std::streamsize size = file.tellg();
    if (size < 0) return StorageResult::Corrupt;
    file.seekg(0, std::ios::beg);

    /* APPENDS, so a caller reading many files can reuse one buffer — the
     * allocation-in-a-loop rule, honoured where it is free to honour. */
    const size_t start = out.size();
    out.resize(start + static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(out.data() + start), size)) {
        out.resize(start);
        return StorageResult::Corrupt;
    }
    return StorageResult::Ok;
}

StorageResult PcFileSystem::readText(StorageKind kind, const char* name, std::string& out)
{
    std::vector<uint8_t> bytes;
    const StorageResult result = read(kind, name, bytes);
    if (result != StorageResult::Ok) return result;

    out.assign(bytes.begin(), bytes.end());
    return StorageResult::Ok;
}

bool PcFileSystem::exists(StorageKind kind, const char* name) const
{
    const std::string path = resolve(kind, name);
    if (path.empty()) return false;

    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

StorageResult PcFileSystem::write(StorageKind kind, const char* name,
                                       const void* data, size_t bytes)
{
    /* READ-ONLY EVEN THOUGH THE DIRECTORY IS WRITABLE. See the header: this is
     * the console rule enforced on the developer's machine, so a save routed to
     * the wrong kind fails here rather than at submission. */
    if (kind == StorageKind::Asset) return StorageResult::AccessDenied;

    const std::string path = resolve(kind, name);
    if (path.empty()) return StorageResult::AccessDenied;

    /* A name may carry a subdirectory ("campaign/slot1"), and it will not exist
     * the first time. */
    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty() && !ensureDirectory(parent)) return StorageResult::AccessDenied;

    /* WRITTEN BESIDE, THEN MOVED INTO PLACE.
     *
     * A save truncated by a crash or a power cut mid-write is worse than no
     * save at all: the player loses the progress AND the previous save that
     * was overwritten to store it. Writing to a temporary and renaming makes
     * the replacement atomic on every desktop filesystem, so an interrupted
     * write leaves the old save intact.
     *
     * This is also the shape a console transaction wants — build the whole
     * thing, then commit — so the call sites are already written the right way
     * round when that backend arrives. */
    const std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return StorageResult::AccessDenied;

        if (bytes > 0)
            file.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));

        if (!file.good()) {
            file.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return StorageResult::QuotaExceeded;
        }
    }

    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        /* Windows will not rename onto an existing file on some filesystems. */
        fs::remove(path, ec);
        fs::rename(temporary, path, ec);
    }
    if (ec) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return StorageResult::AccessDenied;
    }
    return StorageResult::Ok;
}

StorageResult PcFileSystem::remove(StorageKind kind, const char* name)
{
    if (kind == StorageKind::Asset) return StorageResult::AccessDenied;

    const std::string path = resolve(kind, name);
    if (path.empty()) return StorageResult::AccessDenied;

    std::error_code ec;
    if (!fs::remove(path, ec)) return StorageResult::NotFound;
    return ec ? StorageResult::AccessDenied : StorageResult::Ok;
}

StorageResult PcFileSystem::list(StorageKind kind, std::vector<std::string>& out)
{
    const fs::path root = rootOf(kind);

    std::error_code ec;
    if (!fs::is_directory(root, ec)) return StorageResult::NotFound;

    /* RECURSIVE, and relative to the root, so a name that comes back can be
     * handed straight to read() — a listing that returned absolute paths would
     * be useless to an interface that does not take them. */
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const fs::path relative = fs::relative(entry.path(), root, ec);
        if (ec) continue;

        std::string name = relative.generic_string();

        /* Not the half-written files from an interrupted save. */
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) continue;

        out.push_back(std::move(name));
    }
    return StorageResult::Ok;
}

}  // namespace cromwell
