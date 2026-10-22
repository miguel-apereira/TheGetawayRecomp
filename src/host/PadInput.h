// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

union SDL_Event;
struct SDL_Gamepad;

namespace tgr::host
{

// The DualShock 2 in port 1, as the game reads it: buttons and sticks from
// the first SDL gamepad and the keyboard, plus scripted presses for headless
// runs. update() runs on the main thread; read() may run on any thread.
class PadInput
{
public:
    // Pad buttons, as bits of the 16-bit button word (a pad report clears them).
    enum Button : std::uint16_t
    {
        Select = 0x0001,
        L3 = 0x0002,
        R3 = 0x0004,
        Start = 0x0008,
        Up = 0x0010,
        Right = 0x0020,
        Down = 0x0040,
        Left = 0x0080,
        L2 = 0x0100,
        R2 = 0x0200,
        L1 = 0x0400,
        R1 = 0x0800,
        Triangle = 0x1000,
        Circle = 0x2000,
        Cross = 0x4000,
        Square = 0x8000,
    };

    ~PadInput();

    // Opens and closes gamepads as they come and go.
    void handleEvent(const SDL_Event& event);
    // Samples the keyboard and gamepad.
    void update();

    // "START@20" or "CROSS@31.5": hold the button for a quarter of a second
    // from that many seconds into the run. False if the text is not one.
    bool addScriptedPress(std::string_view text);
    // Applies scripted presses for this point of a headless run.
    void updateScript(double seconds);

    // Fills a 32-byte pad report.
    bool read(std::uint8_t* data, std::size_t size) const;

private:
    struct ScriptedPress
    {
        std::uint16_t button = 0;
        double at = 0;
    };

    SDL_Gamepad* gamepad_ = nullptr;
    std::vector<ScriptedPress> script_;
    std::uint16_t scriptedButtons_ = 0;
    std::atomic<std::uint16_t> buttons_{0}; // pressed buttons
    std::atomic<std::uint32_t> sticks_{0x80808080}; // rx, ry, lx, ly
};

} // namespace tgr::host
