#include "cromwell/ui/core/UiText.hpp"

namespace cromwell::ui {

std::string toUpperAscii(std::string_view text)
{
    std::string out(text);
    for (char& character : out) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - ('a' - 'A'));
        }
    }
    return out;
}

std::string animateTrailingDots(std::string_view text, float stepSeconds, double timeSeconds)
{
    std::string base(text);
    int peakDots = 0;
    while (!base.empty() && base.back() == '.') {
        base.pop_back();
        ++peakDots;
    }
    if (peakDots == 0) {
        peakDots = 3;
    }

    const double step = static_cast<double>(stepSeconds < 0.05f ? 0.05f : stepSeconds);
    const int dots = 1 + static_cast<int>(timeSeconds / step) % peakDots;
    base.append(static_cast<std::size_t>(dots), '.');
    return base;
}

void TextMetrics::wrap(std::string_view text, float maxWidth, const TextStyle& style,
                       std::vector<std::string>& outLines) const
{
    outLines.clear();
    if (text.empty()) {
        return;
    }

    /* Greedy line filling: keep adding words while the measured line still
     * fits, and break before the word that would overflow. Greedy is what every
     * UI toolkit does — the alternative (Knuth-Plass, balancing raggedness
     * across the paragraph) needs the whole paragraph up front and is for
     * typesetting, not for a tip card. */
    std::string line;
    std::string candidate;

    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        /* Explicit newlines break unconditionally, before any width test — an
         * authored paragraph break is a statement, not a suggestion. */
        const std::size_t space = text.find_first_of(" \n", cursor);
        const std::size_t wordEnd = space == std::string_view::npos ? text.size() : space;
        const std::string_view word = text.substr(cursor, wordEnd - cursor);

        if (!word.empty()) {
            candidate = line;
            if (!candidate.empty()) {
                candidate.push_back(' ');
            }
            candidate.append(word);

            if (line.empty() || measure(candidate, style).x <= maxWidth) {
                line.swap(candidate);
            } else {
                outLines.push_back(line);
                line.assign(word);
            }
        }

        if (space == std::string_view::npos) {
            break;
        }
        if (text[space] == '\n') {
            outLines.push_back(line);
            line.clear();
        }
        cursor = space + 1;
    }

    /* The last line is only emitted here, so a paragraph ending without a
     * newline is not silently dropped. An empty trailing line IS emitted when
     * the text ended on a newline — a deliberate blank line is content. */
    outLines.push_back(line);
}

}  // namespace cromwell::ui
