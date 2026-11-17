#pragma once

#include "core/Config.hpp"
#include <filesystem>
#include <string>

namespace vela {

class LuaEngine {
public:
    bool LoadConfig(const std::filesystem::path& filePath, Config& outConfig, std::string& outError);
};

} // namespace vela