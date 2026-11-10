#ifndef OPENMW_APPS_TES5VIEWER_EXPORTPREVIEW_HPP
#define OPENMW_APPS_TES5VIEWER_EXPORTPREVIEW_HPP

#include "recordattachment.hpp"

#include <filesystem>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <osg/Vec3d>

namespace osg
{
    class Node;
}
namespace VFS
{
    class Manager;
}
namespace Resource
{
    class NifFileManager;
}

namespace TES5Viewer
{
    struct LandTile;
    using PreviewBundle = std::map<std::string, std::string, std::less<>>;
    struct PreviewObject
    {
        std::string name;
        std::string group;
        osg::Node* node;
        const LandTile* land = nullptr;
        const std::set<std::pair<int, int>>* replaceLodCells = nullptr;
        bool preserveSkin = false;
        std::string skeleton;
        std::uint32_t reference = 0;
        std::set<std::string> animatedNodes;
        std::string animationProject;
        bool directShapesOnly = false;
        std::map<std::string, std::string, std::less<>> expressionMorphs;
        std::map<std::string, std::vector<std::string>, std::less<>> creationMorphs;
        std::map<std::string, std::array<std::string,8>, std::less<>> skinTextures;
    };

    void exportPreview(PreviewBundle& bundle, const VFS::Manager& vfs, Resource::NifFileManager& nifFiles,
        const ReadWinningRecord& readRecord, const osg::Vec3d& origin, const std::vector<PreviewObject>& objects);
}

#endif
