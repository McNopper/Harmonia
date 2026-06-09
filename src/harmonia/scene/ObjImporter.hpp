#pragma once

#include <filesystem>

#include "harmonia/scene/ISceneImporter.hpp"

class ObjImporter final : public ISceneImporter {
  public:
    bool import(const std::filesystem::path& path,
                ISceneBuilder& scene,
                const DeviceContext& ctx,
                const CommandPool& pool,
                const ImportOptions& options = {}) override;
};
