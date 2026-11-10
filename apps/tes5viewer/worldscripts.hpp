#ifndef OPENMW_APPS_TES5VIEWER_WORLDSCRIPTS_HPP
#define OPENMW_APPS_TES5VIEWER_WORLDSCRIPTS_HPP

#include "recordattachment.hpp"
#include "scenescripts.hpp"

#include <components/esm/position.hpp>
#include <osg/Matrixd>
#include <osg/BoundingBox>

namespace TES5Viewer
{
    struct ArrowLoadout;
    class QuestScripts;
    struct ExteriorCell;

    struct WorldScriptPose
    {
        std::uint32_t cell = 0;
        ESM::Position position;

        bool operator==(const WorldScriptPose&) const = default;
    };

    using WorldScriptTransforms = std::map<std::uint32_t, WorldScriptPose>;

    struct WorldScriptAnimation
    {
        std::uint32_t reference = 0;
        std::uint32_t idle = 0;
        std::string behavior;
        std::string event;
        std::string clip;
        float speed = 1;
    };

    struct WorldScriptArrow
    {
        enum Command { Equip, Nock, Release, Lower };
        Command command;
        std::uint32_t actor, target = 0, shot = 0;
        std::string model;
        float speed = 0, gravity = 0, range = 0, radius = 0, lifetime = 0;
        bool alwaysHit = false, holdWhenBlocked = false;
    };

    struct WorldScriptFall
    {
        std::uint32_t actor;
        osg::Vec3f velocity;
        double delta;
    };

    struct WorldScriptFiringPosition
    {
        std::uint32_t actor, target, marker;
        float range, radius;
    };

    struct WorldScriptAudio
    {
        enum Command { Play, Stop, Volume };
        Command command;
        std::int32_t instance;
        std::uint32_t source = 0;
        std::string path;
        double delay = 0;
        float volume = 1, pitch = 1;
        float minDistance = 0, maxDistance = 0;
        bool spatial = true, loop = false;
        float fade = 0, loopBegin = 0, loopEnd = 0;
        std::uint32_t loopCount = 0;
        bool music = false;
        std::string finale;
        std::vector<float> cues;
        int emotion = -1;
        float emotionStrength = 0;
    };

    struct WorldScriptFace
    {
        int kind; // 0 target, 1 head tracking enabled, 2 expression override
        std::uint32_t actor, target;
        float value = 0;
    };

    struct WorldScriptTriggerBox
    {
        std::uint32_t id;
        osg::Matrixd worldToBox;
        osg::Vec3d halfBounds;

        bool intersects(const osg::Vec3d& from, const osg::Vec3d& to, const osg::Vec3d& padding = {}) const;
    };

    struct WorldScriptTriggerEvent
    {
        std::uint32_t trigger, actor;
        bool entering;
    };

    struct WorldScriptMovingReference
    {
        std::uint32_t base = 0, anchor = 0, recordType = 0;
        float scale = 1;
    };

    class WorldScripts
    {
        struct Impl;
        std::unique_ptr<Impl> mImpl;

    public:
        WorldScripts(ReadWinningRecord read, std::shared_ptr<const VFS::Manager> vfs, QuestScripts& quests,
            Papyrus::ErrorLog& log);
        ~WorldScripts();
        void checkpoint(Papyrus::Checkpoint& archive);
        void checkpointExtensions(Papyrus::Checkpoint& archive);

        void setRecordIndex(std::shared_ptr<const WinningRecordIndex> index);
        void setCartHorseOffset(const osg::Vec3f& offset);
        void addScenes(std::span<const std::uint32_t> sceneIds);
        void setExteriorCells(std::uint32_t world, std::span<const ExteriorCell> cells);
        void addPlacements(const WorldScriptTransforms& placements);
        void teleportPlayer(const WorldScriptPose& pose, std::uint32_t world = 0);
        bool isDead(std::uint32_t actor) const;
        bool isInCombat(std::uint32_t actor) const;
        std::pair<std::int32_t, double> playTopicLine(const RecordPayload& info, std::uint32_t actor,
            std::uint32_t number, int emotion, float strength);
        void stopTopicLine(std::int32_t instance);
        void topicFragment(const RecordPayload& info, std::uint32_t quest, std::uint32_t actor, bool begin);
        bool wasKilled(std::uint32_t actor) const;
        bool strike(std::uint32_t target, std::uint32_t source, std::uint32_t weapon, float damage);
        bool restoreActorValue(std::uint32_t actor, std::uint32_t value, float amount);
        std::uint32_t registerPlayerArrow(const ArrowLoadout& loadout);
        void setTriggerActorBounds(std::uint32_t actor, const osg::BoundingBoxd& bounds);
        void setTriggers(std::span<const PreparedSceneScript> scripts, const std::map<std::uint32_t, std::uint32_t>& actors,
            const std::map<std::uint32_t, osg::BoundingBoxd>& bounds = {});
        std::optional<WorldScriptPose> authoredPlacement(std::uint32_t id) const;
        std::uint32_t editorCell(std::uint32_t id) const;
        std::optional<WorldScriptPose> placement(std::uint32_t id) const;
        const WorldScriptTransforms& transforms() const;
        const std::map<std::uint32_t, std::uint32_t>& relocations() const;
        const std::map<std::uint32_t, WorldScriptMovingReference>& movingReferences() const;
        std::uint32_t actorBase(std::uint32_t id);
        std::optional<float> getActorValue(std::uint32_t actor, std::uint32_t value) const;
        std::uint32_t vehicle(std::uint32_t id) const;
        float visualScale(std::uint32_t actor);
        void checkpointFurniture(Papyrus::Checkpoint& ar);
        void checkpointFlightPermissions(Papyrus::Checkpoint& ar);
        void checkpointGhosts(Papyrus::Checkpoint& ar);
        void checkpointSocialState(Papyrus::Checkpoint& ar);
        void checkpointCombat(Papyrus::Checkpoint& ar);
        std::vector<std::uint32_t> loadedActors() const;
        bool sameSpace(std::uint32_t a, std::uint32_t cellA, std::uint32_t b, std::uint32_t cellB) const;
        void deletePlacement(std::uint32_t id);
        std::vector<std::array<std::uint32_t, 3>> takeSpellCasts();
        void playCastSounds(std::uint32_t spell, std::uint32_t caster);
        void animationPose(std::uint32_t actor, const ESM::Position& pose, bool active);
        void physicsPose(std::uint32_t reference, const ESM::Position& pose, bool active);
        int motionType(std::uint32_t reference) const;
        void animationEvent(std::uint32_t actor, std::string_view event);
        void fallResult(std::uint32_t actor, const osg::Vec3f& position, const osg::Vec3f& velocity, bool grounded);
        void firingPosition(std::uint32_t actor, std::uint32_t target, const osg::Vec3f& position);
        std::vector<WorldScriptFall> takeFalls();
        std::vector<WorldScriptFiringPosition> takeFiringPositions();
        void arrowImpact(std::uint32_t shot, std::uint32_t target);
        std::uint32_t owningQuest(std::uint32_t id);
        std::optional<Papyrus::Value> callFormMethod(
            std::uint32_t id, std::string_view name, const std::vector<Papyrus::Value>& args);
        std::optional<Papyrus::Value> callSoundStatic(std::string_view name, const std::vector<Papyrus::Value>& args);
        std::optional<Papyrus::Value> getFormProperty(std::uint32_t id, std::string_view name) const;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> takeFurnitureActivations();
        void playPlayerExecution(std::uint32_t actor, bool player);
        std::uint32_t playExecution(std::uint32_t victim, std::span<const std::uint32_t> dependents);
        void recoverPackages();
        void tick(double time);
        std::vector<SceneScriptEvent> takeEvents();
        std::vector<WorldScriptAnimation> takeAnimations();
        std::vector<WorldScriptArrow> takeArrows();
        std::vector<WorldScriptAudio> takeAudio();
        std::vector<WorldScriptFace> takeFaces();
        void audioFinished(std::int32_t instance, bool played);
        std::vector<WorldScriptTriggerEvent> takeTriggerEvents();
    };
}

#endif
