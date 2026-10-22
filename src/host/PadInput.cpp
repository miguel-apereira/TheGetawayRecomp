// SPDX-License-Identifier: MIT
#include "host/PadInput.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string>

namespace tgr::host
{

namespace
{

constexpr double kScriptedPressSeconds = 0.25;

struct NamedButton
{
    std::string_view name;
    PadInput::Button button;
};

constexpr NamedButton kButtonNames[] = {
    {"SELECT", PadInput::Select}, {"L3", PadInput::L3},         {"R3", PadInput::R3},
    {"START", PadInput::Start},   {"UP", PadInput::Up},         {"RIGHT", PadInput::Right},
    {"DOWN", PadInput::Down},     {"LEFT", PadInput::Left},     {"L2", PadInput::L2},
    {"R2", PadInput::R2},         {"L1", PadInput::L1},         {"R1", PadInput::R1},
    {"TRIANGLE", PadInput::Triangle}, {"CIRCLE", PadInput::Circle}, {"CROSS", PadInput::Cross},
    {"SQUARE", PadInput::Square},
};

struct KeyButton
{
    SDL_Scancode key;
    PadInput::Button button;
};

// Same layout as PS2Recomp's own runner.
constexpr KeyButton kKeys[] = {
    {SDL_SCANCODE_UP, PadInput::Up},          {SDL_SCANCODE_W, PadInput::Up},
    {SDL_SCANCODE_DOWN, PadInput::Down},      {SDL_SCANCODE_S, PadInput::Down},
    {SDL_SCANCODE_LEFT, PadInput::Left},      {SDL_SCANCODE_A, PadInput::Left},
    {SDL_SCANCODE_RIGHT, PadInput::Right},    {SDL_SCANCODE_D, PadInput::Right},
    {SDL_SCANCODE_X, PadInput::Cross},        {SDL_SCANCODE_SPACE, PadInput::Cross},
    {SDL_SCANCODE_C, PadInput::Circle},       {SDL_SCANCODE_ESCAPE, PadInput::Circle},
    {SDL_SCANCODE_Z, PadInput::Square},       {SDL_SCANCODE_V, PadInput::Triangle},
    {SDL_SCANCODE_Q, PadInput::L1},           {SDL_SCANCODE_E, PadInput::R1},
    {SDL_SCANCODE_LSHIFT, PadInput::L2},      {SDL_SCANCODE_RSHIFT, PadInput::R2},
    {SDL_SCANCODE_RETURN, PadInput::Start},   {SDL_SCANCODE_TAB, PadInput::Select},
};

struct GamepadButton
{
    SDL_GamepadButton sdl;
    PadInput::Button button;
};

constexpr GamepadButton kGamepadButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, PadInput::Cross},
    {SDL_GAMEPAD_BUTTON_EAST, PadInput::Circle},
    {SDL_GAMEPAD_BUTTON_WEST, PadInput::Square},
    {SDL_GAMEPAD_BUTTON_NORTH, PadInput::Triangle},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PadInput::L1},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PadInput::R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, PadInput::L3},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PadInput::R3},
    {SDL_GAMEPAD_BUTTON_START, PadInput::Start},
    {SDL_GAMEPAD_BUTTON_BACK, PadInput::Select},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, PadInput::Up},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PadInput::Down},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PadInput::Left},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PadInput::Right},
};

// SDL axis (-32768..32767) to a pad stick byte (0..255, 0x80 centred).
std::uint32_t stickByte(Sint16 value)
{
    return static_cast<std::uint32_t>((static_cast<int>(value) + 32768) >> 8);
}

} // namespace

PadInput::~PadInput()
{
    if (gamepad_)
        SDL_CloseGamepad(gamepad_);
}

void PadInput::handleEvent(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_GAMEPAD_ADDED && !gamepad_)
        gamepad_ = SDL_OpenGamepad(event.gdevice.which);
    else if (event.type == SDL_EVENT_GAMEPAD_REMOVED && gamepad_ &&
             SDL_GetGamepadID(gamepad_) == event.gdevice.which)
    {
        SDL_CloseGamepad(gamepad_);
        gamepad_ = nullptr;
    }
}

void PadInput::update()
{
    std::uint16_t pressed = scriptedButtons_;

    int keyCount = 0;
    if (const bool* keys = SDL_GetKeyboardState(&keyCount))
    {
        for (const KeyButton& key : kKeys)
        {
            if (key.key < keyCount && keys[key.key])
                pressed |= key.button;
        }
    }

    std::uint32_t sticks = 0x80808080;
    if (gamepad_)
    {
        for (const GamepadButton& b : kGamepadButtons)
        {
            if (SDL_GetGamepadButton(gamepad_, b.sdl))
                pressed |= b.button;
        }
        constexpr Sint16 kTriggerThreshold = 16384;
        if (SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kTriggerThreshold)
            pressed |= L2;
        if (SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kTriggerThreshold)
            pressed |= R2;
        sticks = stickByte(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHTX)) |
                 stickByte(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHTY)) << 8 |
                 stickByte(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX)) << 16 |
                 stickByte(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY)) << 24;
    }

    buttons_.store(pressed, std::memory_order_relaxed);
    sticks_.store(sticks, std::memory_order_relaxed);
}

bool PadInput::addScriptedPress(std::string_view text)
{
    const std::size_t at = text.find('@');
    if (at == std::string_view::npos)
        return false;
    std::string name(text.substr(0, at));
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const auto named = std::find_if(std::begin(kButtonNames), std::end(kButtonNames),
                                    [&](const NamedButton& b) { return b.name == name; });
    double seconds = 0;
    const std::string_view time = text.substr(at + 1);
    if (named == std::end(kButtonNames) ||
        std::from_chars(time.data(), time.data() + time.size(), seconds).ec != std::errc{})
        return false;
    script_.push_back({named->button, seconds});
    return true;
}

void PadInput::updateScript(double seconds)
{
    scriptedButtons_ = 0;
    for (const ScriptedPress& press : script_)
    {
        if (seconds >= press.at && seconds < press.at + kScriptedPressSeconds)
            scriptedButtons_ |= press.button;
    }
    buttons_.store(scriptedButtons_, std::memory_order_relaxed);
}

bool PadInput::read(std::uint8_t* data, std::size_t size) const
{
    if (!data || size < 32)
        return false;
    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = 0x73; // analog (DualShock) mode, 3 halfwords of data
    const std::uint16_t released = static_cast<std::uint16_t>(~buttons_.load(std::memory_order_relaxed));
    data[2] = static_cast<std::uint8_t>(released & 0xFF);
    data[3] = static_cast<std::uint8_t>(released >> 8);
    const std::uint32_t sticks = sticks_.load(std::memory_order_relaxed);
    std::memcpy(data + 4, &sticks, 4);
    return true;
}

} // namespace tgr::host
