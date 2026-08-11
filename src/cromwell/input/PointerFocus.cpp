#include "cromwell/input/PointerFocus.hpp"

namespace cromwell {

void PointerFocus::resolve()
{
    published_ = pending_;
    pending_ = Claims{};
}

void PointerFocus::claimMouse(const char* claimant)
{
    /* A null name would read as "no claim" and silently drop the capture, which
     * is the failure mode this class exists to prevent. Substituting a marker
     * keeps the capture and makes the caller's mistake visible in the dev panel
     * rather than invisible in the input. */
    pending_.mouse = claimant != nullptr ? claimant : "unnamed";
}

void PointerFocus::claimKeyboard(const char* claimant)
{
    pending_.keyboard = claimant != nullptr ? claimant : "unnamed";
}

}  // namespace cromwell
