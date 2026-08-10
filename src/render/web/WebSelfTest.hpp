/* WebSelfTest.hpp — does typing into a web surface actually work.
 *
 * SINGLE RESPONSIBILITY: answer that question without a person at the keyboard.
 *
 * WHY THIS EXISTS. "I clicked the search box and could not type" has three
 * completely different causes — the click did not focus the field, the field
 * focused but Chromium never told us, or it told us and the characters were
 * dropped — and from the outside all three look identical. Guessing between
 * them cost two build-and-ask cycles, each of which needed someone to run the
 * game, click a box and read four numbers back.
 *
 * So the interaction is scripted instead. The surface loads a page with one
 * text field at a known position, the test clicks it, types, and asks the page
 * what it received. Every stage is recorded, so the failure names itself.
 *
 * This is measurement, not judgement: the output is text, and nothing here
 * looks at a picture.
 */
#pragma once

#include <string>

namespace xcom {

class WebRuntime;

/* Runs the whole sequence and writes a report to `logPath`. Also returns it, so
 * a caller can print it. Needs a live GL context — the surface it creates owns
 * a texture — so call it after InitWindow. */
std::string runWebSelfTest(WebRuntime& runtime, const std::string& logPath,
                           const std::string& soakUrl = std::string(),
                           const std::string& typeText = std::string());

}  // namespace xcom
