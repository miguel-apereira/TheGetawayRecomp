// SPDX-License-Identifier: MIT
//
// TheGetawayRecomp entry point.
//
// First run: ask the player for their own ISO, check that it is the supported
// build, and extract it once into the game data folder (next to the exe by
// default). Later runs start straight from that folder.
//
// After the game data is in place, bring up the SDL3 window with a Vulkan
// swapchain and, when TGR_WITH_RECOMPILED_CODE is enabled, start the
// recompiled game on its own thread. --headless runs the game with no window
// at all, for machines without a usable Vulkan driver and for automated runs.

#include "app/Log.h"
#include "disc/GameData.h"
#include "disc/GameDisc.h"
#include "disc/IsoImage.h"
#include "game/GameBuild.h"
#ifdef TGR_WITH_RECOMPILED_CODE
#include "game/RecompiledCode.h"
#endif
#include "host/VulkanRenderer.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;
using namespace tgr;

namespace
{

// SDL3 hands main() UTF-8 arguments on every platform.
fs::path pathFromUtf8(const char* s)
{
    return fs::path(reinterpret_cast<const char8_t*>(s));
}

struct Options
{
    std::optional<fs::path> isoPath;  // --iso: on first run, install from this image without asking
    std::optional<fs::path> dataPath; // --data: game data folder
    bool reinstall = false;           // --reinstall: extract again even if installed
    bool validation = false;          // --validation: Vulkan validation layer (Vulkan SDK, developers only)
    bool headless = false;            // --headless: no window, no dialogs; log only
    double seconds = 0;               // --seconds: stop the game after this long (0: run until it exits)
};

// Set from --headless: errors go to the log only, never to a message box.
bool g_headless = false;

Options parseArgs(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--iso" && i + 1 < argc)
            options.isoPath = pathFromUtf8(argv[++i]);
        else if (arg == "--data" && i + 1 < argc)
            options.dataPath = pathFromUtf8(argv[++i]);
        else if (arg == "--reinstall")
            options.reinstall = true;
        else if (arg == "--validation")
            options.validation = true;
        else if (arg == "--headless")
            options.headless = true;
        else if (arg == "--seconds" && i + 1 < argc)
            options.seconds = std::atof(argv[++i]);
        else
            log::warn("Unknown argument: {}", arg);
    }
    return options;
}

fs::path defaultDataPath()
{
    const char* base = SDL_GetBasePath();
    return (base ? pathFromUtf8(base) : fs::current_path()) / "game";
}

void showError(SDL_Window* window, const std::string& message)
{
    log::error("{}", message);
    if (!g_headless)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TheGetawayRecomp", message.c_str(), window);
}

// Shows a native "open file" dialog and waits for it while keeping the window
// responsive. Returns nothing if the player cancels or closes the window.
std::optional<fs::path> askForIso(SDL_Window* window)
{
    struct DialogState
    {
        std::mutex mutex;
        bool done = false;
        std::optional<fs::path> path;
    } state;

    static const SDL_DialogFileFilter kFilters[] = {{"PS2 disc image (*.iso)", "iso"}};
    SDL_ShowOpenFileDialog(
        [](void* userdata, const char* const* files, int) {
            auto* s = static_cast<DialogState*>(userdata);
            std::lock_guard lock(s->mutex);
            if (files && files[0])
                s->path = pathFromUtf8(files[0]);
            else if (!files)
                log::error("File dialog failed: {}", SDL_GetError());
            s->done = true;
        },
        &state, window, kFilters, 1, nullptr, false);

    for (;;)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_EVENT_QUIT)
                return std::nullopt;
        {
            std::lock_guard lock(state.mutex);
            if (state.done)
                return state.path;
        }
        SDL_Delay(16);
    }
}

// Opens and checks the ISO, then extracts it on a worker thread while the
// window title shows progress.
bool installFromIso(SDL_Window* window, const fs::path& isoPath, const fs::path& dataPath)
{
    std::string error;
    const auto iso = disc::IsoImage::open(isoPath, error);
    if (!iso)
    {
        showError(window, error);
        return false;
    }

    const disc::GameDiscCheck check = disc::checkGameDisc(*iso);
    if (!check.ok || !check.info)
    {
        showError(window, check.message);
        return false;
    }
    log::info("Disc: {}", check.message);
    log::info("Extracting to {}", dataPath.string());

    const std::uint64_t total = disc::gameDataSize(*iso);
    std::atomic<std::uint64_t> done{0};
    std::atomic<bool> finished{false};
    bool ok = false;

    std::thread worker([&] {
        ok = disc::installGameData(*iso, *check.info, dataPath, done, error);
        finished = true;
    });

    int lastPercent = -1;
    while (!finished)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            // Closing the window mid-extraction is ignored: the install would be left half done.
        }
        const int percent = total ? static_cast<int>(done * 100 / total) : 100;
        if (percent != lastPercent)
        {
            if (window)
                SDL_SetWindowTitle(window, std::format("The Getaway - extracting your disc: {}%", percent).c_str());
            else if (percent % 10 == 0)
                log::info("Extracting: {}%", percent);
            lastPercent = percent;
        }
        SDL_Delay(50);
    }
    worker.join();
    if (window)
        SDL_SetWindowTitle(window, "The Getaway");

    if (!ok)
        showError(window, error);
    return ok;
}

// Makes sure the game data folder holds an extraction of the player's disc.
bool ensureGameData(SDL_Window* window, const Options& options, const fs::path& dataPath)
{
    if (!options.reinstall && disc::isGameDataInstalled(dataPath))
    {
        log::info("Game data: {}", dataPath.string());
        return true;
    }

    std::optional<fs::path> isoPath = options.isoPath;
    if (!isoPath && options.headless)
    {
        showError(window, std::format("No game data in {}. Run once with --iso <your .iso> to extract it.",
                                      dataPath.string()));
        return false;
    }
    if (!isoPath)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "TheGetawayRecomp",
                                 "TheGetawayRecomp needs your own copy of The Getaway (USA, SCUS-97133).\n\n"
                                 "Choose the .iso you dumped from your disc. It is extracted once and is never "
                                 "uploaded or shared.",
                                 window);
        isoPath = askForIso(window);
        if (!isoPath)
            return false;
    }
    return installFromIso(window, *isoPath, dataPath);
}

#ifdef TGR_WITH_RECOMPILED_CODE
// Starts the recompiled game on its own thread.
bool startGuest(SDL_Window* window, game::Guest& guest, const fs::path& dataPath)
{
    std::string error;
    if (!guest.start(dataPath, error))
    {
        showError(window, error);
        return false;
    }
    log::info("Game started at guest PC 0x{:08X}", guest.pc());
    return true;
}
#endif

// No window: runs the game until it exits, --seconds runs out, or the process
// is killed, logging the guest PC every two seconds.
int runHeadless(const Options& options, const fs::path& dataPath)
{
    if (!ensureGameData(nullptr, options, dataPath))
        return 1;

#ifdef TGR_WITH_RECOMPILED_CODE
    game::Guest guest;
    if (!startGuest(nullptr, guest, dataPath))
        return 1;

    using Clock = std::chrono::steady_clock;
    const Clock::time_point begin = Clock::now();
    Clock::time_point nextReport = begin + std::chrono::seconds(2);
    while (!guest.finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const Clock::time_point now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - begin).count();
        if (now >= nextReport)
        {
            log::info("[{:.0f}s] guest PC 0x{:08X}", elapsed, guest.pc());
            nextReport += std::chrono::seconds(2);
        }
        if (options.seconds > 0 && elapsed >= options.seconds)
        {
            log::info("Stopping the game after {} s (--seconds)", options.seconds);
            break;
        }
    }
    if (guest.finished())
        log::info("The game thread returned at guest PC 0x{:08X}", guest.pc());
    guest.stop();
#else
    log::info("Built without the recompiled game code: nothing to run");
#endif
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const Options options = parseArgs(argc, argv);
    g_headless = options.headless;
    log::info("TheGetawayRecomp: {} {} ({})", game::kTitle, game::kSerial, game::kRegion);
#ifdef TGR_WITH_RECOMPILED_CODE
    const game::RecompiledCodeInfo recompiled = game::recompiledCodeInfo();
    log::info("Recompiled code: {} entry points, guest PC 0x{:08X}-0x{:08X}", recompiled.entryCount,
              recompiled.tableBase, recompiled.tableEnd);
#endif

    const fs::path dataPath = options.dataPath ? *options.dataPath : defaultDataPath();
    if (options.headless)
        return runHeadless(options, dataPath);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
    {
        showError(nullptr, std::string("SDL_Init failed: ") + SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("The Getaway", 1280, 896, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        showError(nullptr, std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // --- The player's own disc, extracted once ----------------------------------
    if (!ensureGameData(window, options, dataPath))
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // --- Graphics ------------------------------------------------------------------
    host::VulkanRenderer renderer;
    if (!renderer.init(window, options.validation))
    {
        showError(window, "Vulkan initialisation failed. Make sure your GPU driver is up to date.");
        renderer.shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // --- The game ------------------------------------------------------------------
#ifdef TGR_WITH_RECOMPILED_CODE
    game::Guest guest;
    if (!startGuest(window, guest, dataPath))
    {
        renderer.shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    bool guestReported = false;
#endif

    bool running = true;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                renderer.requestSwapchainRebuild();
                break;
            default:
                break;
            }
        }

#ifdef TGR_WITH_RECOMPILED_CODE
        // The window stays open after the game thread stops, so the log can be read.
        if (!guestReported && guest.finished())
        {
            log::info("The game thread returned at guest PC 0x{:08X}", guest.pc());
            guestReported = true;
        }
#endif

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(16);
            continue;
        }

        // Placeholder frame until the GS replacement draws real output.
        renderer.drawFrame(0.05f, 0.05f, 0.08f);
    }

#ifdef TGR_WITH_RECOMPILED_CODE
    guest.stop();
#endif
    renderer.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
