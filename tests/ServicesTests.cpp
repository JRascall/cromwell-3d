/* ServicesTests.cpp — headless verification of the DI container and settings.
 *
 *   1. a service resolves under the key it was registered with
 *   2. the SAME type registered under two keys stays two instances
 *   3. an absent key resolves to null rather than crashing
 *   4. a key resolved as the WRONG type resolves to null, not garbage
 *   5. providing over a key replaces rather than accumulates
 *   6. settings reads fall back when a key was never written
 *   7. an int read as a float is answered; the mismatches that hide bugs are not
 *   8. keybinds round-trip
 *   9. require() throws by name when a service or setting is missing
 *
 * Point 2 is the reason the container is keyed by name at all, and point 4 is
 * what makes that safe: string keys with no type check would hand back a
 * reinterpreted pointer and fail somewhere else entirely.
 */
#include "cromwell/services/ServiceKey.hpp"
#include "cromwell/services/Services.hpp"
#include "cromwell/settings/SettingKeys.hpp"
#include "cromwell/settings/Settings.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace cromwell;

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do {                                     \
    if (!(cond)) { g_failures++;                                  \
        std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

constexpr ServiceKey kAlpha { "test.alpha" };
constexpr ServiceKey kBeta  { "test.beta" };
constexpr ServiceKey kAbsent{ "test.absent" };

void resolvesUnderItsKey()
{
    Services::clear();
    Settings& provided = Services::provide<Settings>(kAlpha);
    provided.setInt("answer", 42);

    Settings* found = Services::tryGet<Settings>(kAlpha);
    CHECK(found == &provided, "should resolve to the instance that was provided");
    CHECK(found && found->intOr("answer", 0) == 42, "and carry its contents");
    CHECK(Services::has(kAlpha), "has() should see it");
}

void twoKeysHoldTwoInstances()
{
    Services::clear();
    Services::provide<Settings>(kAlpha).setInt("which", 1);
    Services::provide<Settings>(kBeta).setInt("which", 2);

    CHECK(Services::get<Settings>(kAlpha).intOr("which", 0) == 1, "alpha keeps its own");
    CHECK(Services::get<Settings>(kBeta).intOr("which", 0) == 2, "beta keeps its own");
    CHECK(&Services::get<Settings>(kAlpha) != &Services::get<Settings>(kBeta),
          "two keys must be two instances, not one shared");
    CHECK(Services::count() == 2, "expected 2 services, got %zu", Services::count());
}

void absentKeyIsNull()
{
    Services::clear();
    CHECK(Services::tryGet<Settings>(kAbsent) == nullptr, "absent key should be null");
    CHECK(!Services::has(kAbsent), "has() should be false");
}

void wrongTypeIsNull()
{
    Services::clear();
    Services::provide<Settings>(kAlpha);

    /* Registered, but not as this type. Reinterpreting would compile and then
     * fail somewhere else entirely. */
    CHECK(Services::tryGet<std::string>(kAlpha) == nullptr,
          "a key resolved as the wrong type must be null");
    CHECK(Services::has(kAlpha), "but something IS registered under that key");
}

void provideReplaces()
{
    Services::clear();
    Services::provide<Settings>(kAlpha).setInt("gen", 1);
    Services::provide<Settings>(kAlpha).setInt("gen", 2);

    CHECK(Services::count() == 1, "replacing must not accumulate, got %zu", Services::count());
    CHECK(Services::get<Settings>(kAlpha).intOr("gen", 0) == 2, "the newer one should win");
}

void removeAndClear()
{
    Services::clear();
    Services::provide<Settings>(kAlpha);
    CHECK(Services::remove(kAlpha), "remove should report success");
    CHECK(!Services::has(kAlpha), "and it should be gone");
    CHECK(!Services::remove(kAlpha), "removing twice should report false");
}

void settingsFallBackWhenUnset()
{
    Settings config;
    CHECK(config.floatOr("missing", 1.5f) == 1.5f, "unset float should fall back");
    CHECK(config.intOr("missing", 7) == 7, "unset int should fall back");
    CHECK(config.boolOr("missing", true), "unset bool should fall back");
    CHECK(config.textOr("missing", "none") == "none", "unset text should fall back");
    CHECK(!config.has("missing"), "has() should be false for an unset key");
}

void settingsTypesAreChecked()
{
    Settings config;
    config.setInt("count", 3);
    config.setBool("on", true);

    /* An int read as a float is answered: a config file that says 2 for a
     * value meaning 2.0 is not a mistake worth failing over. */
    CHECK(config.floatOr("count", 0.0f) == 3.0f, "int should read as float");

    /* A bool read as an int is NOT, because that conversion hides mistakes. */
    CHECK(config.intOr("on", -1) == -1, "a bool must not silently read as an int");
    CHECK(config.boolOr("count", false) == false, "an int must not silently read as a bool");
}

void keyBindsRoundTrip()
{
    Settings config;
    config.setKeyBind("move.forward", 87);   /* W */

    CHECK(config.keyBind("move.forward", 0) == 87, "keybind should round-trip");
    CHECK(config.keyBind("move.back", 83) == 83, "unbound action should fall back");
}

void requireThrowsWhenMissing()
{
    Services::clear();

    bool threw = false;
    try { Services::require<Settings>(kAbsent); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "require() should throw for an unregistered service");

    /* Registered, but as another type - still a failure, not a reinterpret. */
    Services::provide<Settings>(kAlpha);
    threw = false;
    try { Services::require<std::string>(kAlpha); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "require() should throw on a type mismatch");

    /* And returns normally when it is there. */
    Services::require<Settings>(kAlpha).setInt("ok", 1);
    CHECK(Services::get<Settings>(kAlpha).intOr("ok", 0) == 1,
          "require() should hand back a usable service");
    Services::clear();
}

void requiredSettingsThrow()
{
    Settings config;
    config.setInt("count", 3);

    bool threw = false;
    try { config.requireFloat("missing"); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "requireFloat should throw when unset");

    /* Same widening the fallback form allows: an int answers a float. */
    CHECK(config.requireFloat("count") == 3.0f, "requireFloat should widen an int");

    threw = false;
    try { config.requireBool("count"); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "requireBool should throw on a type mismatch");
}

void engineReadsItsOwnKey()
{
    Services::clear();
    Services::provide<Settings>(kGameSettings).setFloat(settings::kThinkInterval, 0.25f);

    const Settings& config = Services::get<Settings>(kGameSettings);
    CHECK(config.floatOr(settings::kThinkInterval, 0.1f) == 0.25f,
          "the engine's own key should read back what the game wrote");
    Services::clear();
}

}  // namespace

int main()
{
    resolvesUnderItsKey();
    twoKeysHoldTwoInstances();
    absentKeyIsNull();
    wrongTypeIsNull();
    provideReplaces();
    removeAndClear();
    settingsFallBackWhenUnset();
    settingsTypesAreChecked();
    keyBindsRoundTrip();
    requireThrowsWhenMissing();
    requiredSettingsThrow();
    engineReadsItsOwnKey();

    if (g_failures == 0) std::printf("services tests passed\n");
    else                 std::printf("%d services check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
