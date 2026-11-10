#ifndef OPENMW_APPS_TES5VIEWER_QUESTSCRIPTS_H
#define OPENMW_APPS_TES5VIEWER_QUESTSCRIPTS_H

#include "recordattachment.hpp"
#include "scenescripts.hpp"

#include <set>

namespace TES5Viewer
{
    Papyrus::ResolveQuestAlias questAliasResolver(const ReadWinningRecord& read);
    void collectQuestTargets(const ReadWinningRecord& read, const std::map<std::uint32_t, std::uint32_t>& actors,
        std::set<std::uint32_t>& targets, const ScriptDiagnostic& diagnostic);

    struct QuestCreatedReference
    {
        std::uint32_t base = 0, anchor = 0;
        bool actor = false, initiallyDisabled = false;
    };

    class QuestScripts
    {
        struct Impl;
        std::unique_ptr<Impl> mImpl;

    public:
        QuestScripts(ReadWinningRecord read, std::shared_ptr<const VFS::Manager> vfs, Papyrus::HostCallbacks base,
            Papyrus::ErrorLog& log);
        ~QuestScripts();
        void checkpoint(Papyrus::Checkpoint& archive);
        void checkpointInventoryFilters(Papyrus::Checkpoint& archive);
        void checkpointLocationAndTimers(Papyrus::Checkpoint& archive);
        void checkpointInventoryReferences(Papyrus::Checkpoint& archive);
        void checkpointRepeatUpdates(Papyrus::Checkpoint& archive);
        Papyrus::HostCallbacks host() const;
        void setRecordIndex(std::shared_ptr<const WinningRecordIndex> index);
        void addActors(const std::map<std::uint32_t, std::uint32_t>& actors);
        void setLoadedReferences(std::span<const std::uint32_t> references);
        void startGame(std::optional<std::uint32_t> onlyQuest = {});
        void startDialogueQuest(std::uint32_t id);
        void cellLoaded();
        void updateLocations();
        void animationEvent(std::uint32_t sender, std::string_view event);
        void actorDeath(std::uint32_t actor, std::uint32_t killer);
        void actorHit(std::uint32_t actor, std::uint32_t aggressor, std::uint32_t source, std::uint32_t projectile);
        void inventoryChanged(std::uint32_t reference, std::uint32_t item, std::int32_t count,
            std::uint32_t other, bool added);
        void equipmentChanged(std::uint32_t actor, std::uint32_t item, bool equipped);
        bool isRunning(std::uint32_t quest) const;
        std::uint32_t referenceAlias(std::uint32_t quest, std::uint32_t alias) const;
        bool isOptionalAlias(std::uint32_t quest, std::uint32_t alias) const;
        std::vector<std::uint32_t> packagesForActor(std::uint32_t actor) const;
        std::uint32_t packageQuestForActor(std::uint32_t actor, std::uint32_t pack) const;
        bool testConditions(std::uint32_t quest, const RecordPayload& source,
            std::span<const std::span<const std::byte>> conditions, std::uint32_t subject = 0, std::uint32_t target = 0);
        const std::map<std::uint32_t, QuestCreatedReference>& createdReferences() const;
        const std::map<std::uint32_t, std::uint32_t>& inventoryReferences() const;
        void transferInventoryReferences(std::uint32_t base, std::uint32_t previous,
            std::uint32_t next, std::int32_t count);
        std::uint32_t placeReference(std::uint32_t base, std::uint32_t anchor, bool disabled);
        bool deleteReference(std::uint32_t id);
        void tick(double time);
        std::vector<std::uint32_t> objectiveTargets();
        float globalValue(std::string_view name, float fallback) const;
        std::set<std::uint32_t> takeChangedPackageQuests();
        std::vector<SceneScriptEvent> takeEvents();
    };
}

#endif
