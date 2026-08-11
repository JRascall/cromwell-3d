/* PointerFocus.hpp — who owns the cursor and the keyboard this frame.
 *
 * SINGLE RESPONSIBILITY: collect claims from whatever is on screen and answer
 * one question — may the world have this input?
 *
 * THE BUG THIS EXISTS TO STOP is the oldest one in games with a UI: you drag a
 * slider and the camera orbits underneath it, you click a button and the unit
 * beneath it walks somewhere, you type in a text field and the storey cut
 * changes. Every one of them is the same fault — two systems both decided the
 * same event was theirs.
 *
 * Unity calls the question `EventSystem.IsPointerOverGameObject`, Unreal calls
 * it an input mode, ImGui calls it `WantCaptureMouse`. Under the names they are
 * all the same arrangement: everything that could swallow the pointer says so,
 * and the world asks before acting.
 *
 * WHY A CLAIM LIST RATHER THAN A BOOL. Because the moment there are two UI
 * surfaces — the dev panel AND the widget kit, an embedded browser AND a
 * tooltip — a single flag has to be set by whoever remembers, and the failure is
 * silent in both directions: an unset flag lets clicks fall through to the
 * world, and a stuck one makes the game unresponsive with nothing on screen to
 * explain why. Claims are additive, they are named, and the name is readable
 * from the dev panel, so "why can't I click anything" is a question with a
 * printed answer.
 *
 * ================== THE ONE-FRAME LAG, AND WHY IT IS FINE ==================
 *
 * AN IMMEDIATE-MODE UI DOES NOT KNOW WHAT IT IS OVER UNTIL IT DRAWS. A widget's
 * bounds exist only inside the call that draws it, so hover is resolved during
 * the render, which happens AFTER input arbitration. A frame therefore
 * arbitrates on the claims the last frame's widgets made.
 *
 * That is a frame of lag on the answer, and it is genuinely fine: it costs a
 * wrong decision only when the cursor crosses a widget's edge on the exact
 * frame of a press, and it is what ImGui itself does. The alternative is a
 * retained hit-test tree maintained alongside the immediate-mode draw, which is
 * two descriptions of the same layout that can disagree — a far worse bug for a
 * far rarer case.
 *
 * So the cycle is GATHER, RESOLVE, ASK. Claims accumulate — some pushed by
 * surfaces as they draw, some polled from surfaces that cache their own answer
 * — `resolve()` closes the set, and the queries then report it until the next
 * resolve. Deliberately the same vocabulary as UiContext::endFrame, which
 * closes a frame of widgets by resolving which one was active.
 *
 * CLAIMANT NAMES MUST OUTLIVE THE FRAME. Stored as pointers, not copied — pass
 * string literals, the same discipline UiContext::id uses. A name built into a
 * local buffer will dangle.
 *
 * COLD CODE. A handful of claims and a handful of queries per frame, all of it
 * comparisons. No zone of its own — it runs inside whatever zone the caller's
 * input step already has.
 */
#pragma once

namespace cromwell {

class PointerFocus {
public:
    /* Closes the claim set: what has been claimed since the last resolve becomes
     * what the queries below report, and a fresh set starts accumulating.
     *
     * Call it once a frame, after the claims that can be gathered synchronously
     * and before the world is asked anything. Claims that arrive later — a
     * widget claiming as it draws — land in the fresh set and are answered from
     * the next resolve, which is the one-frame lag the header describes and
     * which is unavoidable for an immediate-mode UI. */
    void resolve();

    /* ---- claiming ------------------------------------------------------- */

    /* "I am under the cursor and clicks here are mine." Called by every UI
     * surface that hit-tests: the dev panel when ImGui wants the mouse, the
     * widget kit when a control is hot, an embedded page when the pointer is
     * inside it.
     *
     * LAST CLAIM WINS for the recorded name, which matches how the surfaces are
     * layered — later ones draw on top. The captured/not-captured answer does
     * not depend on order. */
    void claimMouse(const char* claimant);

    /* "Keystrokes here are mine." A text field, a console, a rename box. Kept
     * separate from the mouse because they genuinely diverge: a hovered button
     * takes the pointer and no keys, a focused text field takes keys and no
     * pointer. Collapsing them into one flag makes hovering a button eat the
     * hotkeys. */
    void claimKeyboard(const char* claimant);

    /* ---- what the game asks --------------------------------------------- */

    /* True when something on screen is under the cursor. Unity's
     * IsPointerOverGameObject, by another name. */
    bool mouseOverUi() const { return published_.mouse != nullptr; }
    bool keyboardInUi() const { return published_.keyboard != nullptr; }

    /* The same answers phrased the way the game reads them, because at the call
     * site the interesting subject is the world, not the UI. `if
     * (focus.worldTakesPointer())` says what the branch is for; `if
     * (!focus.mouseOverUi())` makes the reader invert it. */
    bool worldTakesPointer() const { return !mouseOverUi(); }
    bool worldTakesKeys() const { return !keyboardInUi(); }

    /* Who has it, or nullptr. For the dev panel and the log — this is the
     * string that turns "input is dead" into "the web panel still thinks the
     * cursor is inside it". */
    const char* mouseClaimant() const { return published_.mouse; }
    const char* keyboardClaimant() const { return published_.keyboard; }

private:
    struct Claims {
        const char* mouse = nullptr;
        const char* keyboard = nullptr;
    };

    /* Two sets, because some claims arrive during the render and are read during
     * the next frame's input step. Merging them into one would mean reading a
     * set that is half rebuilt — the pointer would flicker free for the part of
     * the frame before the UI had drawn. */
    Claims pending_;
    Claims published_;
};

}  // namespace cromwell
