#pragma once

#include <filesystem>

namespace Nswitch {

struct RuntimeConfig {
    bool aot_primary = true;
    bool dynamic_fallback = true;
    bool cpu_video_decode = true;
};

class Runtime {
public:
    explicit Runtime(RuntimeConfig config = {});

    const RuntimeConfig& Config() const noexcept;
    bool Initialize();
    bool LoadGame(const std::filesystem::path& game_path);
    int Run();

private:
    RuntimeConfig config_;
    std::filesystem::path game_path_;
    bool initialized_ = false;
};

} // namespace Nswitch
