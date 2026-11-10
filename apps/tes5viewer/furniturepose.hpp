#pragma once
#include <components/esm/position.hpp>
#include <components/misc/convert.hpp>

namespace TES5Viewer
{
    inline ESM::Position furnitureMarkerPose(const ESM::Position& furniture, const osg::Vec3f& offset,
        float heading, float scale = 1)
    {
        auto pose = furniture;
        const auto translated = Misc::Convert::makeOsgQuat(furniture) * (offset * scale);
        for (unsigned i = 0; i < 3; ++i) pose.pos[i] += translated[i];
        // NIF marker headings and ESM headings use the same convention. The
        // negative rotation into the right-handed scene belongs to makeOsgQuat.
        pose.rot[2] += heading;
        return pose;
    }
}
