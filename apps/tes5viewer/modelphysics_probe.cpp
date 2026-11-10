#include "modelmechanics.hpp"
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/nif/physics.hpp>
#include <fstream>
#include <iostream>
#include <osg/io_utils>
int main(int argc, char** argv)
{
    if (argc < 2 || argc > 4) return 2;
    try {
        Nif::NIFFile file(VFS::Path::NormalizedView("probe.nif"));
        Nif::Reader(file, nullptr).parse(std::make_unique<std::ifstream>(argv[1], std::ios::binary));
        if (argc >= 3)
        {
            if (argc == 4)
                for (std::size_t i=0; i<file.mRecords.size(); ++i)
                    if (auto* node=dynamic_cast<Nif::NiAVObject*>(file.mRecords[i].get())) node->mName="AnonymousNode"+std::to_string(i);
            const auto model=TES5Viewer::readModelMechanics(file);
            std::ofstream(argv[2]) << model.json;
            std::ofstream(std::string(argv[2])+".collision.json") << model.collision;
            return 0;
        }
        const auto id = [&](const Nif::Record* r) { for (std::size_t i=0; i<file.mRecords.size(); ++i) if(file.mRecords[i].get()==r) return int(i); return -1; };
        const auto visit=[&](auto&& self,const Nif::NiAVObject& node,const osg::Matrixf& parent)->void {
            const auto world=node.mTransform.toMatrix()*parent;
            if (node.mName=="HorseSpine2") std::cout << "ANCHOR " << node.mName << " transform=" << world << '\n';
            if (const auto* group=dynamic_cast<const Nif::NiNode*>(&node))
                for (const auto& child:group->mChildren) if(!child.empty()) self(self,child.get(),world);
        };
        for(const auto* root:file.mRoots) if(const auto* n=dynamic_cast<const Nif::NiAVObject*>(root)) visit(visit,*n,osg::Matrixf{});
        for (const auto& record : file.mRecords) {
            const auto* r=record.get();
            if (const auto* n=dynamic_cast<const Nif::NiAVObject*>(r); n && !n->mCollision.empty())
                if (const auto* c=dynamic_cast<const Nif::bhkCollisionObject*>(n->mCollision.getPtr()))
                    std::cout << "NODE " << id(r) << ' ' << n->mName << " body=" << id(c->mBody.getPtr()) << '\n';
            if (const auto* body=dynamic_cast<const Nif::bhkRigidBody*>(r)) {
                const auto& v=body->mInfo;
                std::cout << "BODY " << id(r) << ' ' << r->mRecordName << " mass=" << v.mMass << " motion=" << int(v.mMotionType)
                    << " pos=" << v.mTranslation << " rot=" << v.mRotation << " center=" << v.mCenter << " damp=" << v.mLinearDamping << ',' << v.mAngularDamping
                    << " friction=" << v.mFriction << " restitution=" << v.mRestitution << " inertia=";
                for (const auto& row : v.mInertiaTensor.mValues) for (float element : row) std::cout << element << ',';
                std::cout << '\n';
            }
            if (const auto* c=dynamic_cast<const Nif::bhkConstraint*>(r)) {
                std::cout << "JOINT " << id(r) << ' ' << r->mRecordName << " a=" << id(c->mInfo.mEntityA.getPtr()) << " b=" << id(c->mInfo.mEntityB.getPtr()) << '\n';
                if (const auto* h=dynamic_cast<const Nif::bhkHingeConstraint*>(c)) std::cout << " A " << h->mConstraint.mDataA.mPivot << " axis=" << h->mConstraint.mDataA.mAxis << " B " << h->mConstraint.mDataB.mPivot << " axis=" << h->mConstraint.mDataB.mAxis << '\n';
                if (const auto* h=dynamic_cast<const Nif::bhkLimitedHingeConstraint*>(c)) std::cout << " A " << h->mConstraint.mDataA.mPivot << " axis=" << h->mConstraint.mDataA.mAxis << " B " << h->mConstraint.mDataB.mPivot << " axis=" << h->mConstraint.mDataB.mAxis << " limits=" << h->mConstraint.mMinAngle << ',' << h->mConstraint.mMaxAngle << '\n';
                if (const auto* h=dynamic_cast<const Nif::bhkMalleableConstraint*>(c)) std::cout << " malleable " << int(h->mConstraint.mType) << " tau=" << h->mConstraint.mTau << " damp=" << h->mConstraint.mDamping << " strength=" << h->mConstraint.mStrength << '\n';
            }
        }
    } catch(const std::exception& e) { std::cerr << e.what(); return 1; }
}
