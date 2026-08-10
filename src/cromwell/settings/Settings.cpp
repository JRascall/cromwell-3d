#include "cromwell/settings/Settings.hpp"

#include <stdexcept>
#include <utility>

namespace cromwell {

const Settings::Value* Settings::find(const std::string& key) const
{
    const auto it = values_.find(key);
    return it == values_.end() ? nullptr : &it->second;
}

float Settings::floatOr(const std::string& key, float fallback) const
{
    const Value* value = find(key);
    if (!value) return fallback;

    /* An int stored where a float is read is answered rather than refused: a
     * config file that says `2` for a value meaning 2.0 is not a mistake worth
     * failing over. The reverse is not offered - truncating silently is. */
    if (value->kind == Kind::Float) return value->number;
    if (value->kind == Kind::Int)   return static_cast<float>(value->integer);
    return fallback;
}

int Settings::intOr(const std::string& key, int fallback) const
{
    const Value* value = find(key);
    if (!value || value->kind != Kind::Int) return fallback;
    return value->integer;
}

bool Settings::boolOr(const std::string& key, bool fallback) const
{
    const Value* value = find(key);
    if (!value || value->kind != Kind::Bool) return fallback;
    return value->flag;
}

std::string Settings::textOr(const std::string& key, const std::string& fallback) const
{
    const Value* value = find(key);
    if (!value || value->kind != Kind::Text) return fallback;
    return value->text;
}

int Settings::keyBind(const std::string& action, int fallback) const
{
    return intOr(action, fallback);
}

void Settings::setFloat(const std::string& key, float value)
{
    Value stored;
    stored.kind = Kind::Float;
    stored.number = value;
    values_[key] = std::move(stored);
}

void Settings::setInt(const std::string& key, int value)
{
    Value stored;
    stored.kind = Kind::Int;
    stored.integer = value;
    values_[key] = std::move(stored);
}

void Settings::setBool(const std::string& key, bool value)
{
    Value stored;
    stored.kind = Kind::Bool;
    stored.flag = value;
    values_[key] = std::move(stored);
}

void Settings::setText(const std::string& key, std::string value)
{
    Value stored;
    stored.kind = Kind::Text;
    stored.text = std::move(value);
    values_[key] = std::move(stored);
}

void Settings::setKeyBind(const std::string& action, int key) { setInt(action, key); }

namespace {

/* One shape of message for every required read, so a missing setting always
 * reads the same way in a log whichever type wanted it. */
[[noreturn]] void missing(const std::string& key, const char* wanted)
{
    throw std::runtime_error("required setting '" + key + "' (" + wanted +
                             ") is not set");
}

}  // namespace

float Settings::requireFloat(const std::string& key) const
{
    const Value* value = find(key);
    if (!value || (value->kind != Kind::Float && value->kind != Kind::Int))
        missing(key, "float");
    return value->kind == Kind::Float ? value->number
                                      : static_cast<float>(value->integer);
}

int Settings::requireInt(const std::string& key) const
{
    const Value* value = find(key);
    if (!value || value->kind != Kind::Int) missing(key, "int");
    return value->integer;
}

bool Settings::requireBool(const std::string& key) const
{
    const Value* value = find(key);
    if (!value || value->kind != Kind::Bool) missing(key, "bool");
    return value->flag;
}

std::string Settings::requireText(const std::string& key) const
{
    const Value* value = find(key);
    if (!value || value->kind != Kind::Text) missing(key, "text");
    return value->text;
}

bool Settings::has(const std::string& key) const { return find(key) != nullptr; }
bool Settings::remove(const std::string& key) { return values_.erase(key) > 0; }
void Settings::clear() { values_.clear(); }
std::size_t Settings::count() const { return values_.size(); }

}  // namespace cromwell
