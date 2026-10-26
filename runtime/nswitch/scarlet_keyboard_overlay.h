#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ScarletPC::KeyboardOverlay {

struct Config {
    std::u16string header;
    std::u16string sub;
    std::u16string guide;
    std::u16string ok_text;
    std::uint32_t max_length{};
    std::uint32_t min_length{};
    std::uint32_t disable_flags{};
    bool password{};
    bool number_pad{};
    bool enable_return{};
    bool disable_cancel{};
};

struct Result {
    bool accepted{};
    bool silent{};
    std::u16string text;
};

void Show(Config config, std::u16string initial_text, std::u16string error_text);
void Hide(bool silent);
void UpdateText(std::u16string text);
void SetError(std::u16string error_text);
void TickInput();
bool PollResult(Result& result);

bool Visible();
std::uint64_t Version();

// Builds a full-frame BGRA8 image only when the overlay state or target extent changed.
// Returns true when out_pixels contains a new image that should be uploaded.
bool BuildBgraFrame(std::uint32_t width, std::uint32_t height, std::uint64_t known_version,
                    std::uint64_t& out_version, std::vector<std::uint8_t>& out_pixels);

} // namespace ScarletPC::KeyboardOverlay
