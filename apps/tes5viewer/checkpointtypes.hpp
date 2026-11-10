#pragma once
#include "../papyrustool/checkpoint.hpp"
#include "scenescripts.hpp"
#include "worldscripts.hpp"
#include "questscripts.hpp"
#include "cartroute.hpp"
#include "archery.hpp"
#include <components/vfs/manager.hpp>
#include <components/misc/strings/algorithm.hpp>

namespace ESM
{
    inline void checkpoint(Papyrus::Checkpoint& ar, Position& value) { ar(value.pos, value.rot); }
}
namespace osg
{
    inline void checkpoint(Papyrus::Checkpoint& ar, Vec3f& value) { for (int i = 0; i < 3; ++i) ar(value[i]); }
    inline void checkpoint(Papyrus::Checkpoint& ar, Vec3d& value) { for (int i = 0; i < 3; ++i) ar(value[i]); }
    inline void checkpoint(Papyrus::Checkpoint& ar, Matrixd& value) { for (int i = 0; i < 16; ++i) ar(value.ptr()[i]); }
    inline void checkpoint(Papyrus::Checkpoint& ar, BoundingBoxd& value) { ar(value._min, value._max); }
}
namespace TES5Viewer
{
    inline void checkpoint(Papyrus::Checkpoint& ar, SceneScript& v) { ar(v.owner, v.name, v.source); }
    inline void checkpoint(Papyrus::Checkpoint& ar, SceneScriptEvent& v)
    { ar(v.owner, v.script, v.event, v.traces, v.time, v.waitSeconds, v.invocation); }
    inline void checkpoint(Papyrus::Checkpoint& ar, ScenePapyrusReport& v) { ar(v.scripts, v.events, v.visibility); }
    inline void checkpoint(Papyrus::Checkpoint& ar, WorldScriptPose& v) { ar(v.cell, v.position); }
    inline void checkpoint(Papyrus::Checkpoint& ar, WorldScriptMovingReference& v) { ar(v.base, v.anchor, v.recordType, v.scale); }
    inline void checkpoint(Papyrus::Checkpoint& ar, WorldScriptAnimation& v) { ar(v.reference, v.idle, v.behavior, v.event, v.clip, v.speed); }
    inline void checkpoint(Papyrus::Checkpoint& ar, WorldScriptFace& v) { ar(v.kind, v.actor, v.target, v.value); }
    inline void checkpoint(Papyrus::Checkpoint& ar, WorldScriptArrow& v)
    { ar(v.command, v.actor, v.target, v.shot, v.model, v.speed, v.gravity, v.range, v.radius, v.lifetime, v.alwaysHit, v.holdWhenBlocked); }
    inline void checkpoint(Papyrus::Checkpoint& ar, WorldScriptAudio& v)
    {
        ar(v.command, v.instance, v.source, v.path, v.delay, v.volume, v.pitch, v.minDistance, v.maxDistance,
            v.spatial, v.loop, v.fade, v.loopBegin, v.loopEnd, v.loopCount, v.music, v.finale, v.cues, v.emotion, v.emotionStrength);
    }
    inline void checkpoint(Papyrus::Checkpoint& ar, WorldScriptTriggerBox& v) { ar(v.id, v.worldToBox, v.halfBounds); }
    inline void checkpoint(Papyrus::Checkpoint& ar, WorldScriptTriggerEvent& v) { ar(v.trigger, v.actor, v.entering); }
    inline void checkpoint(Papyrus::Checkpoint& ar, QuestCreatedReference& v) { ar(v.base, v.anchor, v.actor, v.initiallyDisabled); }
    inline void checkpoint(Papyrus::Checkpoint& ar, CartRoutePoint& v) { ar(v.referenceId, v.position); }
    inline void checkpoint(Papyrus::Checkpoint& ar, CartRoute& v) { ar(v.packageId, v.points, v.preferredSpeed); }
    inline void checkpoint(Papyrus::Checkpoint& ar, ArrowLoadout& v)
    { ar(v.bow, v.ammo, v.projectile, v.count, v.releaseSound, v.bowModel, v.ammoModel, v.projectileModel,
        v.speed, v.gravity, v.range, v.radius, v.lifetime, v.damage, v.drawSpeed); }

    inline std::shared_ptr<const Papyrus::Script> checkpointScript(const VFS::Manager& vfs, std::string name)
    {
        if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
            throw std::runtime_error("Invalid saved script name");
        auto pex = std::make_shared<Papyrus::PexFile>(Papyrus::readPex(*vfs.get(VFS::Path::Normalized("scripts/" + name + ".pex"))));
        for (const auto& script : pex->scripts)
            if (Misc::StringUtils::ciEqual(script.name, name)) return {pex, &script};
        throw std::runtime_error("Saved script class absent from mounted PEX");
    }
}
