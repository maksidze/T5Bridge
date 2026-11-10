#ifndef OPENMW_APPS_TES5VIEWER_MODELMECHANICS_H
#define OPENMW_APPS_TES5VIEWER_MODELMECHANICS_H

#include <set>
#include <string>
#include <osg/BoundingBox>

namespace Nif
{
    class FileView;
}

namespace TES5Viewer
{
    struct ModelMechanics
    {
        std::set<std::string> animatedNodes;
        std::string json;
        std::string collision;
        std::string ragdoll;
        osg::BoundingBoxd actorBounds;
    };

    ModelMechanics readModelMechanics(Nif::FileView file, bool actorSkeleton = false,
        const std::set<std::string>& clipNodes = {});
}
#endif
