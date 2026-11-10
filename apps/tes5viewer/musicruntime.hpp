#ifndef OPENMW_APPS_TES5VIEWER_MUSICRUNTIME_HPP
#define OPENMW_APPS_TES5VIEWER_MUSICRUNTIME_HPP

#include "worldscripts.hpp"

#include <memory>

namespace TES5Viewer
{
    class MusicRuntime
    {
        struct Impl;
        std::unique_ptr<Impl> mImpl;

    public:
        MusicRuntime(const ReadWinningRecord& read, const VFS::Manager& vfs, QuestScripts& quests, Papyrus::ErrorLog& log);
        ~MusicRuntime();
        void checkpoint(Papyrus::Checkpoint& archive);

        bool set(const RecordPayload& type, bool added);
        void audioFinished(std::int32_t instance, bool played);
        void tick(double time, const std::function<std::int32_t(WorldScriptAudio)>& play,
            std::vector<WorldScriptAudio>& audio, std::vector<SceneScriptEvent>& events);
    };
}

#endif
