#include "actorpose.hpp"

#include <components/nif/node.hpp>

#include <osg/Quat>

namespace TES5Viewer
{
    namespace
    {
        std::string_view jointName(std::string_view name)
        {
            return name.substr(0, name.find(" ["));
        }

        using JointMatrices = std::map<std::string_view, osg::Matrixf>;

        void readBindJoints(const Nif::NiAVObject& node, const osg::Matrixf& parent, JointMatrices& joints)
        {
            const auto matrix = node.mTransform.toMatrix() * parent;
            if (!joints.emplace(jointName(node.mName), matrix).second)
                return;
            if (const auto* branch = dynamic_cast<const Nif::NiNode*>(&node))
                for (const auto& child : branch->mChildren)
                    if (!child.empty())
                        readBindJoints(*child.getPtr(), matrix, joints);
        }

        struct Aim
        {
            std::string child;
            osg::Vec3f direction;
        };

        Aim jointAim(std::string_view name, ActorPose pose)
        {
            if (!name.starts_with("NPC L ") && !name.starts_with("NPC R "))
                return {};
            const std::string side(name.substr(0, 6));
            const float outward = name[4] == 'L' ? -1.f : 1.f;
            const auto limb = name.substr(6);
            if (limb == "UpperArm")
                return { side + "Forearm",
                    pose == ActorPose::Standing
                        ? osg::Vec3f(outward * .16f, 0, -1.f)
                        : osg::Vec3f(outward * .15f, pose == ActorPose::Prisoner ? .15f : .48f, -1.f) };
            if (limb == "Forearm")
                return { side + "Hand",
                    pose == ActorPose::Standing
                        ? osg::Vec3f(0, .12f, -1.f)
                        : osg::Vec3f(-outward * .37f, 1.f, pose == ActorPose::Prisoner ? -.45f : -.08f) };
            if (pose == ActorPose::Standing)
                return {};
            if (limb == "Thigh")
                return { side + "Calf",
                    pose == ActorPose::Rider ? osg::Vec3f(outward * .85f, .37f, -.55f)
                    : osg::Vec3f(outward * .17f, 1.f, .03f) };
            if (limb == "Calf")
                return { side + "Foot", pose == ActorPose::Rider ? osg::Vec3f(0, -.2f, -1.f)
                    : osg::Vec3f(0, -.70f, -.72f) };
            if (limb == "Foot")
                return { side + "Toe0", osg::Vec3f(0, 1.f, -.1f) };
            return {};
        }

        void poseJoints(const Nif::NiAVObject& node, const osg::Matrixf& parent, const JointMatrices& bindJoints,
            ActorPose pose, NifOsg::SkinPose& result)
        {
            const auto name = jointName(node.mName);
            const auto bind = bindJoints.find(name);
            if (bind == bindJoints.end() || result.contains(node.mName))
                return;
            auto matrix = node.mTransform.toMatrix() * parent;
            const auto invBind = osg::Matrixf::inverse(bind->second);
            const auto aim = jointAim(name, pose);
            if (!aim.child.empty())
                if (const auto child = bindJoints.find(aim.child); child != bindJoints.end())
                {
                    const auto pivot = matrix.getTrans();
                    const auto direction = child->second.getTrans() * invBind * matrix - pivot;
                    if (direction.length2() > .001f)
                    {
                        osg::Quat turn;
                        turn.makeRotate(direction, osg::Vec3d(aim.direction));
                        matrix *= osg::Matrixf::translate(-pivot) * osg::Matrixf::rotate(turn)
                            * osg::Matrixf::translate(pivot);
                    }
                }
            result.emplace(node.mName, invBind * matrix);
            if (const auto* branch = dynamic_cast<const Nif::NiNode*>(&node))
                for (const auto& child : branch->mChildren)
                    if (!child.empty())
                        poseJoints(*child.getPtr(), matrix, bindJoints, pose, result);
        }
    }

    NifOsg::SkinPose makeActorPose(Nif::FileView file, ActorPose pose)
    {
        JointMatrices bindJoints;
        for (std::size_t i = 0; i < file.numRoots(); ++i)
            if (const auto* root = dynamic_cast<const Nif::NiAVObject*>(file.getRoot(i)))
                readBindJoints(*root, osg::Matrixf{}, bindJoints);
        NifOsg::SkinPose result;
        for (std::size_t i = 0; i < file.numRoots(); ++i)
            if (const auto* root = dynamic_cast<const Nif::NiAVObject*>(file.getRoot(i)))
                poseJoints(*root, osg::Matrixf{}, bindJoints, pose, result);
        return result;
    }
}
