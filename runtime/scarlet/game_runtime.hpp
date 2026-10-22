#pragma once

#include <filesystem>

#include "../nswitch/runtime.hpp"

namespace Scarlet {

class GameRuntime {
public:
    explicit GameRuntime(std::filesystem::path installation);

    bool ValidateInstallation() const;
    bool PrepareAot();
    int Launch();

private:
    std::filesystem::path installation_;
    Nswitch::Runtime runtime_;
};

} // namespace Scarlet
