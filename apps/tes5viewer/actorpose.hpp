#ifndef OPENMW_APPS_TES5VIEWER_ACTORPOSE_HPP
#define OPENMW_APPS_TES5VIEWER_ACTORPOSE_HPP

#include <components/nifosg/nifloader.hpp>

namespace TES5Viewer
{
    enum class ActorPose
    {
        Standing,
        Prisoner,
        Driver,
        Rider
    };

    NifOsg::SkinPose makeActorPose(Nif::FileView file, ActorPose pose);
}

#endif
