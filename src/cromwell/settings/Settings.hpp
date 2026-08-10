/* Settings.hpp — a named bag of configuration values.
 *
 * SINGLE RESPONSIBILITY: store loosely-typed settings under string keys and
 * hand them back with a fallback.
 *
 * WHY LOOSELY TYPED. A struct of fields would be tidier and would mean the
 * engine's config header had to know every value any game would ever want —
 * which is exactly backwards, because the engine is the part that must not
 * learn about the game. A bag lets cromwell read the handful of keys it cares
 * about (see SettingKeys.hpp) while a game puts whatever else it likes in the
 * same place.
 *
 * EVERY READ TAKES A FALLBACK, and that is not laziness. A setting that has
 * never been written is the normal case — a fresh install, a config file from
 * an older build, the engine running with no game settings at all — so absence
 * has to be an ordinary answer rather than an error. There is no "get" without
 * a default.
 *
 * KEYBINDS LIVE HERE TOO, as ints, because a key code is a setting like any
 * other and giving them their own store would mean two files to load, two to
 * save and two places to look when a binding does not stick.
 *
 * MULTIPLE BAGS ARE EXPECTED. Register one under kGameSettings for what the
 * game ships with and another under kUserSettings for what the player changed;
 * they are the same class, told apart by their key in the service container.
 */
#pragma once

#include <string>
#include <unordered_map>

namespace cromwell {

class Settings {
public:
    /* ---- reads, all with a fallback ------------------------------------ */
    float       floatOr(const std::string& key, float fallback) const;
    int         intOr(const std::string& key, int fallback) const;
    bool        boolOr(const std::string& key, bool fallback) const;
    std::string textOr(const std::string& key, const std::string& fallback) const;

    /* A key binding, as whatever key code the platform layer uses. Separate
     * from intOr only so the intent reads at the call site. */
    int keyBind(const std::string& action, int fallback) const;

    /* ---- reads that THROW when the value is missing ---------------------
     * The fallback forms above are the normal way to read a setting: absence
     * is ordinary, and a default is nearly always the right answer.
     *
     * These are for the handful of values a game genuinely cannot start
     * without, where a silent default would mean running with the wrong
     * content path or the wrong server and discovering it much later. The
     * message names the key. */
    float       requireFloat(const std::string& key) const;
    int         requireInt(const std::string& key) const;
    bool        requireBool(const std::string& key) const;
    std::string requireText(const std::string& key) const;

    /* ---- writes --------------------------------------------------------- */
    void setFloat(const std::string& key, float value);
    void setInt(const std::string& key, int value);
    void setBool(const std::string& key, bool value);
    void setText(const std::string& key, std::string value);
    void setKeyBind(const std::string& action, int key);

    /* ---- housekeeping ---------------------------------------------------- */
    bool has(const std::string& key) const;
    bool remove(const std::string& key);
    void clear();
    std::size_t count() const;

private:
    /* One value, whichever kind it is. A variant would be tidier; this is
     * C++17-portable, trivially copyable and small enough that the union
     * discipline is easy to see. */
    enum class Kind { Float, Int, Bool, Text };

    struct Value {
        Kind        kind = Kind::Int;
        float       number = 0.0f;
        int         integer = 0;
        bool        flag = false;
        std::string text;
    };

    const Value* find(const std::string& key) const;

    std::unordered_map<std::string, Value> values_;
};

}  // namespace cromwell
