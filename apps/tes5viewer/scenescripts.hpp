#ifndef OPENMW_APPS_TES5VIEWER_SCENESCRIPTS_H
#define OPENMW_APPS_TES5VIEWER_SCENESCRIPTS_H

#include "../papyrustool/vm.hpp"
#include "../papyrustool/vmadbinding.hpp"

#include <deque>
#include <set>

namespace VFS
{
    class Manager;
}

namespace Papyrus
{
    class ErrorLog;
}

namespace TES5Viewer
{
    struct CellScene;

    struct SceneScript
    {
        std::uint32_t owner;
        std::string name;
        std::string source;
    };

    struct SceneScriptEvent
    {
        std::uint32_t owner;
        std::string script;
        std::string event;
        std::vector<Papyrus::Trace> traces;
        double time;
        std::optional<double> waitSeconds;
        std::uint64_t invocation;
    };

    struct ScenePapyrusReport
    {
        std::vector<SceneScript> scripts;
        std::vector<SceneScriptEvent> events;
        std::map<std::uint32_t, bool> visibility;
    };

    struct PreparedSceneScript
    {
        std::uint32_t owner;
        std::shared_ptr<const Papyrus::Script> script;
        std::string source;
        std::map<std::string, Papyrus::Value> properties;
    };

    using ScriptDiagnostic = std::function<void(std::string_view, std::string_view)>;
    std::vector<PreparedSceneScript> prepareSceneScripts(
        const CellScene& scene, const VFS::Manager& vfs, const ScriptDiagnostic& diagnostic = {},
        const std::map<std::uint32_t, bool>* visible = nullptr, const Papyrus::ResolveQuestAlias& aliases = {});

    class SceneScripts
    {
    public:
        SceneScripts(const CellScene& scene, const VFS::Manager& vfs, Papyrus::HostCallbacks host,
            ScenePapyrusReport& report, Papyrus::ErrorLog* log);
        SceneScripts(Papyrus::HostCallbacks host, ScenePapyrusReport& report, Papyrus::ErrorLog* log,
            bool tolerateErrors);
        void checkpoint(Papyrus::Checkpoint& archive, const VFS::Manager& vfs);
        void checkpointLifecycle(Papyrus::Checkpoint& archive);
        void checkpointAnimationEvents(Papyrus::Checkpoint& archive);
        void checkpointCalls(Papyrus::Checkpoint& archive);
        void install(const std::vector<PreparedSceneScript>& scripts);
        void addEffect(std::uint32_t owner, std::uint32_t target, std::uint32_t caster, double expires,
            const std::vector<PreparedSceneScript>& scripts);
        std::uint32_t effectActor(std::uint32_t owner, bool caster) const;
        bool hasScripts(std::uint32_t owner) const;
        void activate(std::uint32_t owner, std::uint32_t activator = 0, bool immediate = true);
        void trigger(std::uint32_t owner, std::uint32_t actor, bool entering);
        void animationEvent(std::uint32_t sender, std::string_view event);
        void destructionStage(std::uint32_t owner, std::int32_t previous, std::int32_t next);
        void tick(double time);
        std::vector<SceneScriptEvent> takeEvents();

    private:
        enum class Event
        {
            Init,
            Activate,
            Update,
            TriggerEnter,
            TriggerLeave,
            Load,
            EffectStart,
            EffectFinish,
            Animation,
            Unload,
            CellLoad,
            DestructionStage
        };

        struct Waiting
        {
            Event event;
            double due;
            std::uint64_t continuation;
            std::uint64_t invocation;
            std::shared_ptr<const bool> nativeWait;
            void checkpoint(Papyrus::Checkpoint& ar);
        };

        struct Pending
        {
            Event event;
            std::uint32_t activator = 0;
            std::string animation;
            std::int32_t nextStage = 0;
            void checkpoint(Papyrus::Checkpoint& ar);
        };

        struct Instance
        {
            std::string name;
            Papyrus::ScriptInstance vm;
            bool initializing = true;
            bool failed = false;
            std::deque<Waiting> waiting{};
            std::deque<Pending> pending{};
        };

        Papyrus::HostCallbacks mHost;
        Papyrus::HostCallbacks mStaged;
        ScenePapyrusReport& mReport;
        Papyrus::ErrorLog* mLog;
        bool mTolerateErrors;
        std::map<std::uint32_t, std::vector<Instance>> mInstances;
        struct Call
        {
            std::uint32_t owner = 0;
            std::string script, method;
            std::vector<Papyrus::Value> arguments;
            std::shared_ptr<bool> finished;
            std::shared_ptr<Papyrus::Value> returned;
            double due = 0;
            std::optional<std::uint64_t> continuation;
            std::shared_ptr<const bool> nativeWait;
            void checkpoint(Papyrus::Checkpoint& ar);
        };
        std::deque<Call> mCalls;
        struct Effect
        {
            std::uint32_t target = 0, caster = 0;
            double expires = 0;
            bool finishing = false;
            void checkpoint(Papyrus::Checkpoint& ar);
        };
        std::map<std::uint32_t, Effect> mEffects;
        std::set<std::uint32_t> mAttached, mLoaded;
        using AnimationKey = std::pair<std::uint32_t, std::string>;
        std::map<AnimationKey, std::set<AnimationKey>> mAnimationEvents;
        std::map<std::uint32_t, bool> mPendingDisabled;
        std::map<std::uint32_t, double> mUpdates;
        std::map<std::uint32_t, double> mPendingUpdates;
        double mTime = 0;
        std::uint64_t mNextInvocation = 1;
        std::size_t mTraceBytes = 0;
        static void enqueue(std::vector<Instance>& instances, Pending event);
        void installInstances(const std::vector<PreparedSceneScript>& scripts);
        Papyrus::HostCallbacks instanceHost(std::uint32_t owner, const std::string& script);
        void updateLoaded(std::uint32_t owner, std::vector<Instance>& instances);
        void startNext(std::uint32_t owner, Instance& instance);
        void tickCalls();
        void commitNativeChanges();
        void step(std::uint32_t owner, Instance& instance, Event kind, std::uint64_t invocation,
            std::optional<std::uint64_t> continuation = std::nullopt, std::uint32_t activator = 0,
            std::string_view animation = {}, std::int32_t nextStage = 0);
    };
}

#endif
