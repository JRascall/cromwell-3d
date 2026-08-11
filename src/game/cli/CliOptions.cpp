#include "game/cli/CliOptions.hpp"

#include <cstdlib>
#include <cstring>

namespace game {


CliOptions CliOptions::parse(int argc, char** argv)
{
    CliOptions options;

    const auto hasValues = [&](int index, int count) { return index + count < argc; };

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (!std::strcmp(arg, "--shot") && hasValues(i, 1)) {
            options.screenshotPath = argv[++i];
        } else if (!std::strcmp(arg, "--shot-frame") && hasValues(i, 1)) {
            options.screenshotFrame = std::atoi(argv[++i]);
        } else if (!std::strcmp(arg, "--web-selftest") && hasValues(i, 1)) {
            options.webSelfTestPath = argv[++i];
        } else if (!std::strcmp(arg, "--web-url") && hasValues(i, 1)) {
            options.webSelfTestUrl = argv[++i];
        } else if (!std::strcmp(arg, "--web-type") && hasValues(i, 1)) {
            options.webSelfTestType = argv[++i];
        } else if (!std::strcmp(arg, "--compute-selftest")) {
            /* Takes an OPTIONAL path, unlike the flags around it: the report
             * is short enough to want on the console alone, and requiring a
             * file to run a smoke test is friction at exactly the wrong
             * moment. A following token that looks like another flag is left
             * for the next iteration to parse. */
            if (hasValues(i, 1) && argv[i + 1][0] != '-') {
                options.computeSelfTestPath = argv[++i];
            } else {
                options.computeSelfTestPath = std::string();
            }
        } else if (!std::strcmp(arg, "--log") && hasValues(i, 1)) {
            options.logPath = argv[++i];
        } else if (!std::strcmp(arg, "--log-level") && hasValues(i, 1)) {
            options.logLevel = argv[++i];
        } else if (!std::strcmp(arg, "--no-ao")) {
            options.ambientOcclusion = false;
        } else if (!std::strcmp(arg, "--no-steam")) {
            options.steam = false;
        } else if (!std::strcmp(arg, "--bake-benchmark")) {
            options.bakeBenchmark = true;
        } else if (!std::strcmp(arg, "--dev-view")) {
            options.forceDevView = true;
        } else if (!std::strcmp(arg, "--splash")) {
            options.forceSplash = true;
        } else if (!std::strcmp(arg, "--no-splash")) {
            options.skipSplash = true;
            options.forceSplash = false;   /* in case both were given */
        } else if (!std::strcmp(arg, "--budget") && hasValues(i, 1)) {
            options.moveBudget = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(arg, "--debug-view") && hasValues(i, 1)) {
            options.debugView = std::atoi(argv[++i]);
        } else if (!std::strcmp(arg, "--iso") && hasValues(i, 1)) {
            options.isoLevel = std::atoi(argv[++i]);
        } else if (!std::strcmp(arg, "--select") && hasValues(i, 1)) {
            options.selectedUnit = std::atoi(argv[++i]);
        } else if (!std::strcmp(arg, "--decals")) {
            options.decalDemo = true;
        } else if (!std::strcmp(arg, "--sprint")) {
            options.forceBothRings = true;
        } else if (!std::strcmp(arg, "--los")) {
            options.losMode = true;
        } else if (!std::strcmp(arg, "--stairs")) {
            options.cameraPreset = CameraPreset::Staircase;
        } else if (!std::strcmp(arg, "--cam") && hasValues(i, 6)) {
            for (float& value : options.freeCamera)
                value = static_cast<float>(std::atof(argv[++i]));
            options.cameraPreset = CameraPreset::Free;
        } else if (!std::strcmp(arg, "--mouse") && hasValues(i, 2)) {
            options.mouseX = std::atoi(argv[++i]);
            options.mouseY = std::atoi(argv[++i]);
        } else if (!std::strcmp(arg, "--boom") && hasValues(i, 3)) {
            Cell cell;
            cell.x = std::atoi(argv[++i]);
            cell.y = std::atoi(argv[++i]);
            cell.z = std::atoi(argv[++i]);
            options.detonateAt = cell;
        }
    }
    return options;
}

}  // namespace game
