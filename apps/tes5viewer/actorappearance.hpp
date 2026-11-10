#ifndef OPENMW_APPS_TES5VIEWER_ACTORAPPEARANCE_HPP
#define OPENMW_APPS_TES5VIEWER_ACTORAPPEARANCE_HPP

#include "cellscene.hpp"

#include <map>
#include <array>
#include <optional>
#include <set>

#include <osg/Vec3f>

namespace VFS
{
    class Manager;
}

namespace TES5Viewer
{
    struct ActorAppearance
    {
        struct WeightBlend
        {
            std::string otherModel;
            float fraction;
        };

        std::string editorId;
        std::vector<std::string> models;
        std::map<std::string, WeightBlend, std::less<>> weightBlends;
        // TXST skin overrides belong to the selected ARMA, not the shared NIF.
        std::map<std::string, std::array<std::string,8>, std::less<>> skinTextures;
        std::map<std::string, std::string, std::less<>> expressionMorphs;
        std::map<std::string, std::vector<std::string>, std::less<>> creationMorphs;
        std::optional<osg::Vec3f> skinTone;
        std::string skeleton, animationProject;
        float height = 1.f;
    };

    ActorAppearance loadActorAppearance(const ReadWinningRecord& read, const VFS::Manager& vfs,
        std::span<const PluginLoadInfo> plugins, std::uint32_t npc, const std::set<std::uint32_t>& equipped, bool firstPerson = false);
    struct ActorBodySize
    {
        float height = 1.f;
        bool child = false;
    };
    ActorBodySize actorBodySize(const ReadWinningRecord& read, std::uint32_t npc);
    std::string actorHeadBone(const ReadWinningRecord& read, std::uint32_t npc);
}

#endif
