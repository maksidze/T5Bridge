#ifndef OPENMW_APPS_TES5VIEWER_ARCHERY_H
#define OPENMW_APPS_TES5VIEWER_ARCHERY_H

#include "recordattachment.hpp"

namespace TES5Viewer
{
    struct ArrowLoadout
    {
        std::uint32_t bow = 0, ammo = 0, projectile = 0, count = 0, releaseSound = 0;
        std::string bowModel, ammoModel, projectileModel;
        float speed = 0, gravity = 0, range = 0, radius = 0, lifetime = 0, damage = 0, drawSpeed = 1;
    };

    std::optional<ArrowLoadout> readArrowLoadout(const ReadWinningRecord& read, std::uint32_t actorBase,
        std::uint32_t requestedBow = 0, std::uint32_t requestedAmmo = 0);
}
#endif
