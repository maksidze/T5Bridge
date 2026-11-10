#ifndef OPENMW_APPS_TES5VIEWER_RUNTIMEEXPORT_HPP
#define OPENMW_APPS_TES5VIEWER_RUNTIMEEXPORT_HPP

#include "assetresources.hpp"
#include "cellscene.hpp"
#include "exportpreview.hpp"
#include "scenescripts.hpp"
#include "worldscripts.hpp"

#include <set>

namespace TES5Viewer
{
    struct RuntimeExportOptions
    {
        std::filesystem::path data;
        std::filesystem::path runtimeCacheDirectory;
        std::string runtimeCacheKey;
        std::filesystem::path resources;
        AssetPlan assetPlan;
        std::shared_ptr<VFS::Manager> runtimeVfs;
        const std::map<std::uint32_t, std::string>* projectileModels = nullptr;
    };

    struct RuntimeScriptBatch
    {
        std::vector<PreparedSceneScript> scripts;
        std::vector<std::pair<std::string, std::string>> errors;
        std::map<std::uint32_t, bool> initialDisabled;
        std::map<std::uint32_t, std::pair<std::uint32_t, bool>> enableParents;
        std::set<std::uint32_t> targets;
        std::map<std::uint32_t, std::uint32_t> actors;
        std::map<std::uint32_t, osg::BoundingBoxd> actorBounds;
        WorldScriptTransforms placements;
        std::map<std::uint32_t, float> doors;
        std::uint32_t cell = 0;
        std::uint32_t worldspace = 0;
        std::vector<ExteriorCell> exteriorCells;
        std::shared_ptr<const VFS::Manager> vfs;
    };

    void exportRuntimeLocation(const CellScene& scene, const RuntimeExportOptions& options, const ReadWinningRecord& read,
        const WorldspaceInfo* lodWorld, int cellX, int cellY, PreviewBundle& bundle,
        RuntimeScriptBatch* scripts = nullptr, std::span<const std::uint32_t> tracked = {});
    void addRuntimeStaticPlacement(CellScene& scene, const ExteriorReference& reference, const ReadWinningRecord& read);
    void exportPlayerView(const VFS::Manager& vfs, const ReadWinningRecord& read, std::uint32_t player,
        const std::set<std::uint32_t>& equipped, const std::string& weaponModel, PreviewBundle& bundle);
    std::string exportAnimationObject(const VFS::Manager& vfs, const ReadWinningRecord& read,
        const std::string& model, PreviewBundle& bundle, bool requireSocket = true);
}

#endif
