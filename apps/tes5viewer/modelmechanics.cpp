#include "modelmechanics.hpp"

#include <components/misc/constants.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nif/controller.hpp>
#include <components/nif/data.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/nif/physics.hpp>
#include <components/nifosg/controller.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <span>
#include <sstream>

namespace TES5Viewer
{
    namespace
    {
        using Hull = std::vector<osg::Vec3f>;

        struct TriangleMesh
        {
            Hull vertices;
            std::vector<std::array<std::uint32_t, 3>> faces;
        };

        struct Collision
        {
            std::vector<Hull> hulls;
            TriangleMesh mesh;
            struct RoundShape
            {
                osg::Vec3f a, b;
                float radiusA, radiusB;
            };
            std::vector<RoundShape> round;
            bool empty() const { return hulls.empty() && mesh.faces.empty() && round.empty(); }
        };

        enum class Layer : unsigned
        {
            Biped = 8, Water = 11, Trigger = 12, NonCollidable = 15, CloudTrap = 16, Portal = 18,
            AcousticSpace = 21, ActorZone = 22, ProjectileZone = 23, GasTrap = 24, CharacterController = 30,
            DeadBiped = 32, BipedNoController = 33, AvoidBox = 34, CameraSphere = 36, DoorDetection = 37,
            CameraPick = 39, ItemPick = 40, DeadActorZone = 47, FallingTrapTrigger = 48, NavCut = 49,
        };

        enum class Contact { Solid, Pick, Camera, Ignore };

        Contact contactForLayer(unsigned value)
        {
            switch (static_cast<Layer>(value))
            {
                case Layer::NonCollidable: case Layer::Water: case Layer::Trigger: case Layer::CloudTrap:
                case Layer::Portal: case Layer::AcousticSpace: case Layer::ActorZone: case Layer::ProjectileZone:
                case Layer::GasTrap: case Layer::AvoidBox: case Layer::DoorDetection: case Layer::DeadActorZone:
                case Layer::FallingTrapTrigger: case Layer::NavCut: return Contact::Ignore;
                case Layer::Biped: case Layer::DeadBiped: case Layer::BipedNoController: case Layer::CharacterController:
                case Layer::ItemPick: return Contact::Pick;
                case Layer::CameraSphere: case Layer::CameraPick: return Contact::Camera;
                default: return Contact::Solid;
            }
        }

        osg::Vec3f xyz(const osg::Vec4f& point) { return {point.x(), point.y(), point.z()}; }

        osg::Matrixf rotationMatrix(const osg::Quat& rotation)
        {
            const auto length = rotation.length2();
            return std::isfinite(length) && length > 0 ? osg::Matrixf::rotate(rotation) : osg::Matrixf{};
        }

        void face(TriangleMesh& mesh, std::uint32_t base, std::uint32_t count, unsigned a, unsigned b, unsigned c)
        {
            if (a < count && b < count && c < count && a != b && b != c && a != c)
                mesh.faces.push_back({base + a, base + b, base + c});
        }

        void indices(TriangleMesh& mesh, std::uint32_t base, std::uint32_t count, std::span<const unsigned short> list,
            std::span<const unsigned short> stripLengths = {})
        {
            std::size_t start = 0;
            for (const auto length : stripLengths)
            {
                if (length > list.size() - start)
                    return;
                for (std::size_t i = 2; i < length; ++i)
                    face(mesh, base, count, list[start + i - 2 + (i % 2)], list[start + i - 1 - (i % 2)], list[start + i]);
                start += length;
            }
            for (std::size_t i = start; i + 2 < list.size(); i += 3)
                face(mesh, base, count, list[i], list[i + 1], list[i + 2]);
        }

        void strips(TriangleMesh& mesh, const Nif::NiTriStripsData& data, const osg::Matrixf& transform)
        {
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            for (const auto& vertex : data.mVertices)
                mesh.vertices.push_back(vertex * transform);
            for (const auto& strip : data.mStrips)
            {
                const auto length = static_cast<unsigned short>(strip.size());
                indices(mesh, base, static_cast<std::uint32_t>(data.mVertices.size()), strip, std::span(&length, 1));
            }
        }

        using Contacts = std::map<Contact, Collision>;

        void compressedMesh(const Nif::bhkCompressedMeshShapeData& data, const osg::Matrixf& transform, Contact contact,
            Contacts& result)
        {
            const auto material = [&](unsigned index) {
                return index < data.mMaterials.size() && data.mMaterials[index].mHavokFilter.mLayer != 0
                    ? contactForLayer(data.mMaterials[index].mHavokFilter.mLayer) : contact;
            };
            std::map<Contact, std::uint32_t> bigVertexBases;
            for (const auto& triangle : data.mBigTris)
            {
                const auto kind = material(triangle.mMaterial);
                if (kind == Contact::Ignore)
                    continue;
                auto& mesh = result[kind].mesh;
                const auto [entry, inserted] = bigVertexBases.try_emplace(kind, static_cast<std::uint32_t>(mesh.vertices.size()));
                if (inserted)
                    for (const auto& vertex : data.mBigVerts)
                        mesh.vertices.push_back(xyz(vertex) * transform);
                face(mesh, entry->second, static_cast<std::uint32_t>(data.mBigVerts.size()),
                    triangle.mTriangle[0], triangle.mTriangle[1], triangle.mTriangle[2]);
            }
            for (const auto& chunk : data.mChunks)
            {
                const auto kind = material(chunk.mMaterialIndex);
                if (kind == Contact::Ignore)
                    continue;
                const auto* source = &chunk;
                for (std::size_t i = 0; source->mReference < data.mChunks.size() && i < data.mChunks.size(); ++i)
                    source = &data.mChunks[source->mReference];
                if (source->mReference < data.mChunks.size())
                    continue;
                auto& mesh = result[kind].mesh;
                auto matrix = osg::Matrixf::translate(xyz(source->mTranslation));
                if (chunk.mTransformIndex < data.mChunkTransforms.size())
                {
                    const auto& placement = data.mChunkTransforms[chunk.mTransformIndex];
                    matrix = matrix * rotationMatrix(placement.mRotation)
                        * osg::Matrixf::translate(xyz(placement.mTranslation));
                }
                matrix = matrix * transform;
                const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                for (std::size_t i = 0; i + 2 < source->mVertices.size(); i += 3)
                    mesh.vertices.push_back(osg::Vec3f(source->mVertices[i], source->mVertices[i + 1],
                        source->mVertices[i + 2]) * data.mError * matrix);
                indices(mesh, base, static_cast<std::uint32_t>(source->mVertices.size() / 3), source->mIndices, source->mStrips);
            }
        }

        void shapes(const Nif::bhkShape& shape, const osg::Matrixf& transform, Contact contact,
            Contacts& result, std::set<std::string>& unsupported, unsigned depth = 0)
        {
            if (contact == Contact::Ignore || depth > 64)
                return;
            const auto child = [&](const auto& ref, const osg::Matrixf& matrix, Contact kind) {
                if (!ref.empty())
                    shapes(ref.get(), matrix, kind, result, unsupported, depth + 1);
            };
            if (const auto* tree = dynamic_cast<const Nif::bhkBvTreeShape*>(&shape))
                child(tree->mShape, transform, contact);
            else if (const auto* offset = dynamic_cast<const Nif::bhkConvexTransformShape*>(&shape))
            {
                // Havok stores affine transform columns as padded vectors.
                auto matrix = offset->mTransform;
                matrix(0, 3) = matrix(1, 3) = matrix(2, 3) = 0.f;
                matrix(3, 3) = 1.f;
                child(offset->mShape, matrix * transform, contact);
            }
            else if (const auto* list = dynamic_cast<const Nif::bhkListShape*>(&shape))
            {
                for (std::size_t i = 0; i < list->mSubshapes.size(); ++i)
                {
                    const auto layer = i < list->mHavokFilters.size() ? list->mHavokFilters[i].mLayer : 0;
                    child(list->mSubshapes[i], transform, layer ? contactForLayer(layer) : contact);
                }
            }
            else if (const auto* convexList = dynamic_cast<const Nif::bhkConvexListShape*>(&shape))
            {
                for (const auto& part : convexList->mSubShapes)
                    child(part, transform, contact);
            }
            else if (const auto* compressed = dynamic_cast<const Nif::bhkCompressedMeshShape*>(&shape))
            {
                if (!compressed->mData.empty())
                    compressedMesh(compressed->mData.get(), transform, contact, result);
            }
            else if (const auto* packed = dynamic_cast<const Nif::bhkPackedNiTriStripsShape*>(&shape))
            {
                if (packed->mData.empty())
                    return;
                const auto& data = packed->mData.get();
                const auto matrix = osg::Matrixf::scale(xyz(packed->mScale)) * transform;
                std::vector<Contact> layers(data.mVertices.size(), contact);
                std::size_t vertexOffset = 0;
                for (const auto& part : data.mSubshapes.empty() ? packed->mSubshapes : data.mSubshapes)
                {
                    const auto count = std::min<std::size_t>(part.mNumVertices, layers.size() - vertexOffset);
                    if (part.mHavokFilter.mLayer)
                        std::fill_n(layers.begin() + vertexOffset, count, contactForLayer(part.mHavokFilter.mLayer));
                    vertexOffset += count;
                }
                std::map<Contact, std::uint32_t> bases;
                for (const auto& triangle : data.mTriangles)
                {
                    if (triangle.mTriangle[0] >= layers.size())
                        continue;
                    const auto kind = layers[triangle.mTriangle[0]];
                    if (kind == Contact::Ignore)
                        continue;
                    auto& mesh = result[kind].mesh;
                    const auto [base, inserted] = bases.try_emplace(kind, static_cast<std::uint32_t>(mesh.vertices.size()));
                    if (inserted)
                        for (const auto& vertex : data.mVertices)
                            mesh.vertices.push_back(vertex * matrix);
                    face(mesh, base->second, static_cast<std::uint32_t>(data.mVertices.size()),
                        triangle.mTriangle[0], triangle.mTriangle[1], triangle.mTriangle[2]);
                }
            }
            else if (const auto* mesh = dynamic_cast<const Nif::bhkNiTriStripsShape*>(&shape))
            {
                // NiTriStripsData uses NIF units, unlike the Havok-packed variants.
                const auto matrix = osg::Matrixf::scale(xyz(mesh->mScale) / Constants::UnitsPerMeter) * transform;
                for (const auto& data : mesh->mData)
                    if (!data.empty())
                        strips(result[contact].mesh, data.get(), matrix);
            }
            else if (const auto* convex = dynamic_cast<const Nif::bhkConvexVerticesShape*>(&shape))
            {
                if (convex->mVertices.size() < 4)
                    return;
                auto& hull = result[contact].hulls.emplace_back();
                for (const auto& point : convex->mVertices)
                    hull.push_back(xyz(point) * transform);
            }
            else if (const auto* box = dynamic_cast<const Nif::bhkBoxShape*>(&shape))
            {
                auto& hull = result[contact].hulls.emplace_back();
                for (const float x : {-1.f, 1.f})
                    for (const float y : {-1.f, 1.f})
                        for (const float z : {-1.f, 1.f})
                            hull.push_back(osg::Vec3f(x * box->mExtents.x(), y * box->mExtents.y(), z * box->mExtents.z())
                                * transform);
            }
            else if (const auto* cylinder = dynamic_cast<const Nif::bhkCylinderShape*>(&shape))
            {
                auto axis = xyz(cylinder->mVertexB) - xyz(cylinder->mVertexA);
                if (axis.normalize() == 0.f || cylinder->mCylinderRadius <= 0.f)
                    return;
                auto x = axis ^ (std::abs(axis.z()) < .9f ? osg::Z_AXIS : osg::X_AXIS);
                x.normalize();
                const auto y = axis ^ x;
                auto& hull = result[contact].hulls.emplace_back();
                constexpr unsigned sides = 24;
                for (unsigned i = 0; i < sides; ++i)
                {
                    const auto angle = float(2. * osg::PI * i / sides);
                    const auto edge = (x * std::cos(angle) + y * std::sin(angle)) * cylinder->mCylinderRadius;
                    hull.push_back((xyz(cylinder->mVertexA) + edge) * transform);
                    hull.push_back((xyz(cylinder->mVertexB) + edge) * transform);
                }
            }
            else
            {
                const auto scale = static_cast<float>(transform.getScale().x());
                const auto round = [&](const osg::Vec3f& a, const osg::Vec3f& b, float ra, float rb) {
                    if (ra > 0.f && rb > 0.f)
                        result[contact].round.push_back({a * transform, b * transform, ra * scale, rb * scale});
                };
                if (const auto* capsule = dynamic_cast<const Nif::bhkCapsuleShape*>(&shape))
                    round(capsule->mPoint1, capsule->mPoint2, capsule->mRadius1, capsule->mRadius2);
                else if (const auto* spheres = dynamic_cast<const Nif::bhkMultiSphereShape*>(&shape))
                    for (const auto& sphere : spheres->mSpheres)
                        round(sphere.center(), sphere.center(), sphere.radius(), sphere.radius());
                else if (shape.mRecordType == Nif::RC_bhkSphereShape)
                {
                    const auto radius = static_cast<const Nif::bhkSphereShape&>(shape).mRadius;
                    round({}, {}, radius, radius);
                }
                else
                    unsupported.insert(shape.mRecordName);
            }
        }

        void vector(std::ostream& out, const osg::Vec3f& value)
        {
            out << '[' << value.x() << ',' << value.y() << ',' << value.z() << ']';
        }

        void pose(std::ostream& out, const osg::Vec3f& position, const osg::Quat& rotation, double scale)
        {
            out << '[' << position.x() << ',' << position.y() << ',' << position.z() << ',' << rotation.x() << ','
                << rotation.y() << ',' << rotation.z() << ',' << rotation.w() << ',' << scale << ']';
        }

        void hulls(std::ostream& out, const std::vector<Hull>& data)
        {
            out << '[';
            bool firstHull = true;
            for (const auto& hull : data)
            {
                if (!firstHull)
                    out << ',';
                firstHull = false;
                out << '[';
                bool firstPoint = true;
                for (const auto& point : hull)
                {
                    if (!firstPoint)
                        out << ',';
                    firstPoint = false;
                    vector(out, point);
                }
                out << ']';
            }
            out << ']';
        }

        void writeCollision(std::ostream& out, const Collision& data)
        {
            out << "\"hulls\":";
            hulls(out, data.hulls);
            out << ",\"vertices\":[";
            for (std::size_t i = 0; i < data.mesh.vertices.size(); ++i)
            {
                if (i)
                    out << ',';
                vector(out, data.mesh.vertices[i]);
            }
            out << "],\"faces\":[";
            for (std::size_t i = 0; i < data.mesh.faces.size(); ++i)
            {
                const auto& f = data.mesh.faces[i];
                out << (i ? "," : "") << '[' << f[0] << ',' << f[1] << ',' << f[2] << ']';
            }
            out << "],\"round\":[";
            for (std::size_t i = 0; i < data.round.size(); ++i)
            {
                const auto& shape = data.round[i];
                out << (i ? "," : "") << '[';
                vector(out, shape.a);
                out << ',';
                vector(out, shape.b);
                out << ',' << shape.radiusA << ',' << shape.radiusB << ']';
            }
            out << ']';
        }

        Collision transformed(Collision data, const osg::Matrixf& matrix)
        {
            for (auto& hull : data.hulls)
                for (auto& point : hull)
                    point = point * matrix;
            for (auto& point : data.mesh.vertices)
                point = point * matrix;
            for (auto& shape : data.round)
            {
                shape.a = shape.a * matrix;
                shape.b = shape.b * matrix;
                const auto scale = static_cast<float>(matrix.getScale().x());
                shape.radiusA *= scale;
                shape.radiusB *= scale;
            }
            return data;
        }

        void removeInvalidGeometry(Collision& data)
        {
            const auto valid = [](const osg::Vec3f& point) {
                return std::isfinite(point.x()) && std::isfinite(point.y()) && std::isfinite(point.z());
            };
            std::erase_if(data.hulls, [&](const auto& hull) { return !std::ranges::all_of(hull, valid); });
            std::erase_if(data.round, [&](const auto& shape) {
                return !valid(shape.a) || !valid(shape.b) || !std::isfinite(shape.radiusA) || !std::isfinite(shape.radiusB);
            });
            if (std::ranges::all_of(data.mesh.vertices, valid))
                return;
            std::erase_if(data.mesh.faces, [&](const auto& face) {
                return !std::ranges::all_of(face, [&](auto index) { return valid(data.mesh.vertices[index]); });
            });
            for (auto& point : data.mesh.vertices)
                if (!valid(point))
                    point.set(0.f, 0.f, 0.f); // Unreferenced after discarding invalid triangles.
        }

        struct Node
        {
            const Nif::NiAVObject* source;
            osg::Matrixf transform;
            osg::Matrixf parent;
            const Node* parentNode;
            Contacts collision;
        };

        void legacyTriangles(const Nif::NiAVObject& node, TriangleMesh& mesh)
        {
            if (const auto* geometry = dynamic_cast<const Nif::NiGeometry*>(&node); geometry && !geometry->mData.empty())
            {
                const auto& data = geometry->mData.get();
                if (const auto* triangles = dynamic_cast<const Nif::NiTriShapeData*>(&data))
                {
                    mesh.vertices = triangles->mVertices;
                    indices(mesh, 0, static_cast<std::uint32_t>(mesh.vertices.size()), triangles->mTriangles);
                }
                else if (const auto* stripData = dynamic_cast<const Nif::NiTriStripsData*>(&data))
                    strips(mesh, *stripData, {});
            }
            else if (const auto* tri = dynamic_cast<const Nif::BSTriShape*>(&node))
            {
                for (const auto& vertex : tri->mVertData)
                {
                    if (tri->mVertDesc.mFlags & Nif::BSVertexDesc::Full_Precision)
                        mesh.vertices.push_back(xyz(vertex.mVertex));
                    else
                        mesh.vertices.emplace_back(static_cast<float>(vertex.mHalfVertex[0]),
                            static_cast<float>(vertex.mHalfVertex[1]), static_cast<float>(vertex.mHalfVertex[2]));
                }
                indices(mesh, 0, static_cast<std::uint32_t>(mesh.vertices.size()), tri->mTriangles);
            }
        }
    }

    ModelMechanics readModelMechanics(Nif::FileView file, bool actorSkeleton, const std::set<std::string>& clipNodes)
    {
        std::list<Node> entries;
        std::map<std::string, const Node*> nodes;
        std::map<std::string, const Nif::NiControllerSequence*> sequences;
        std::set<std::string> unsupported;
        constexpr float units = Constants::UnitsPerMeter;
        const auto visit = [&](auto&& self, const Nif::NiAVObject& node, const osg::Matrixf& parent,
                               const Node* ancestor, bool legacy, bool active) -> void {
            const auto transform = node.mTransform.toMatrix() * parent;
            auto& entry = entries.emplace_back(Node{ &node, transform, parent, ancestor, {} });
            nodes.try_emplace(node.mName, &entry);
            active &= node.mRecordType != Nif::RC_AvoidNode
                && (node.mRecordType != Nif::RC_NiCollisionSwitch || node.collisionActive());
            legacy |= node.mRecordType == Nif::RC_RootCollisionNode;
            if (active && legacy)
                legacyTriangles(node, entry.collision[Contact::Solid].mesh);
            if (active && !node.mCollision.empty())
            {
                if (const auto* object = dynamic_cast<const Nif::bhkCollisionObject*>(node.mCollision.getPtr()))
                {
                    if (!object->mBody.empty() && !object->mBody->mShape.empty()
                        && object->mBody->mWorldObjectInfo.mPhaseType != Nif::BroadPhaseType::BroadPhase_Phantom)
                    {
                        const auto& body = object->mBody.get();
                        osg::Matrixf local;
                        if (body.mRecordType == Nif::RC_bhkRigidBodyT)
                        {
                            const auto& info = static_cast<const Nif::bhkRigidBody&>(body).mInfo;
                            local = rotationMatrix(info.mRotation) * osg::Matrixf::translate(xyz(info.mTranslation));
                        }
                        shapes(body.mShape.get(), local * osg::Matrixf::scale(units, units, units),
                            contactForLayer(body.mHavokFilter.mLayer), entry.collision, unsupported);
                    }
                }
                else
                    unsupported.insert(node.mCollision->mRecordName);
            }
            for (auto controller = node.mController; !controller.empty(); controller = controller->mNext)
                if (const auto* manager = dynamic_cast<const Nif::NiControllerManager*>(controller.getPtr()))
                    for (const auto& sequence : manager->mSequences)
                        if (!sequence.empty())
                        {
                            const auto name = Misc::StringUtils::lowerCase(sequence->mName);
                            const float duration = sequence->mStopTime - sequence->mStartTime;
                            if (!name.empty() && std::isfinite(duration) && duration > 0 && duration <= 30)
                                sequences.emplace(name, sequence.getPtr());
                        }
            if (const auto* group = dynamic_cast<const Nif::NiNode*>(&node))
                for (const auto& child : group->mChildren)
                    if (!child.empty())
                        self(self, child.get(), transform, &entry, legacy, active);
        };
        std::string rootName;
        for (std::size_t i = 0; i < file.numRoots(); ++i)
            if (const auto* root = dynamic_cast<const Nif::NiAVObject*>(file.getRoot(i)))
            {
                if (i == 0)
                    rootName = root->mName;
                visit(visit, *root, osg::Matrixf{}, nullptr, false, true);
            }

        for (const auto& type : unsupported)
            std::clog << "T5_NIF_COLLISION_UNSUPPORTED " << type << " in " << file.getFilename() << '\n';
        for (auto& entry : entries)
            for (auto& [kind, data] : entry.collision)
                removeInvalidGeometry(data);
        const auto solidHulls = [](const Node& node) -> const std::vector<Hull>& {
            static const std::vector<Hull> empty;
            const auto it = node.collision.find(Contact::Solid);
            return it == node.collision.end() ? empty : it->second.hulls;
        };
        // Articulation is a graph of collision bodies, not a list of wheel names.
        std::map<const Nif::bhkRigidBody*, const Node*> bodies;
        std::set<const Nif::bhkConstraint*> joints;
        for (const auto& entry : entries)
            if (!entry.source->mCollision.empty())
                if (const auto* c = dynamic_cast<const Nif::bhkCollisionObject*>(entry.source->mCollision.getPtr()); c && !c->mBody.empty())
                    if (const auto* body = dynamic_cast<const Nif::bhkRigidBody*>(c->mBody.getPtr()); body
                        && (actorSkeleton ? !entry.collision.empty() : !solidHulls(entry).empty()))
                    {
                        bodies.emplace(body, &entry);
                        for (const auto& joint : body->mConstraints)
                            if (!joint.empty())
                                if (const auto* constraint = dynamic_cast<const Nif::bhkConstraint*>(joint.getPtr()))
                                    joints.insert(constraint);
                    }
        std::erase_if(joints, [&](const auto* joint) {
            return !bodies.contains(dynamic_cast<const Nif::bhkRigidBody*>(joint->mInfo.mEntityA.getPtr()))
                || !bodies.contains(dynamic_cast<const Nif::bhkRigidBody*>(joint->mInfo.mEntityB.getPtr()));
        });
        const bool cart = !actorSkeleton && !joints.empty();
        std::set<const Node*> wheelNodes;
        if (cart)
            for (const auto* joint : joints)
                if (dynamic_cast<const Nif::bhkHingeConstraint*>(joint))
                    wheelNodes.insert(bodies.at(dynamic_cast<const Nif::bhkRigidBody*>(joint->mInfo.mEntityA.getPtr())));
        ModelMechanics result;
        if (actorSkeleton && !joints.empty())
        {
            std::set<const Node*> linked;
            for (const auto* joint : joints)
                for (const auto* body : {joint->mInfo.mEntityA.getPtr(), joint->mInfo.mEntityB.getPtr()})
                    linked.insert(bodies.at(static_cast<const Nif::bhkRigidBody*>(body)));
            std::ostringstream rag;
            rag << std::setprecision(9) << "{\"bones\":[";
            const char* comma = "";
            // XRGD visits the bodies in skeleton traversal order.
            for (const auto& entry : entries)
                if (linked.contains(&entry))
                {
                    const auto& info = static_cast<const Nif::bhkRigidBody&>(
                        static_cast<const Nif::bhkCollisionObject&>(entry.source->mCollision.get()).mBody.get()).mInfo;
                    rag << comma << '[' << std::quoted(entry.source->mName) << ',' << info.mMass << ','
                        << info.mLinearDamping << ',' << info.mAngularDamping << ','
                        << info.mFriction << ',' << info.mRestitution << ']';
                    comma = ",";
                }
            rag << "],\"joints\":[";
            comma = "";
            const auto vector = [&](const auto& v, float scale) {
                rag << '[' << v.x()*scale << ',' << v.y()*scale << ',' << v.z()*scale << ']';
            };
            const auto frame = [&](const auto& pivot, const auto& axis, const auto& normal) {
                rag << '['; vector(pivot, units); rag << ','; vector(axis, 1); rag << ','; vector(normal, 1); rag << ']';
            };
            for (const auto* joint : joints)
            {
                const auto names = [&] {
                    rag << comma << '['
                        << std::quoted(bodies.at(static_cast<const Nif::bhkRigidBody*>(joint->mInfo.mEntityA.getPtr()))->source->mName)
                        << ',' << std::quoted(bodies.at(static_cast<const Nif::bhkRigidBody*>(joint->mInfo.mEntityB.getPtr()))->source->mName) << ',';
                    comma = ",";
                };
                if (const auto* r = dynamic_cast<const Nif::bhkRagdollConstraint*>(joint))
                {
                    const auto& c = r->mConstraint;
                    names();
                    frame(c.mDataA.mPivot, c.mDataA.mTwist, c.mDataA.mPlane); rag << ',';
                    frame(c.mDataB.mPivot, c.mDataB.mTwist, c.mDataB.mPlane);
                    rag << ',' << c.mConeMaxAngle << ',' << c.mPlaneMinAngle << ',' << c.mPlaneMaxAngle
                        << ',' << c.mTwistMinAngle << ',' << c.mTwistMaxAngle << ']';
                }
                else if (const auto* h = dynamic_cast<const Nif::bhkLimitedHingeConstraint*>(joint))
                {
                    const auto& c = h->mConstraint;
                    names();
                    frame(c.mDataA.mPivot, c.mDataA.mAxis, c.mDataA.mPerpAxis1); rag << ',';
                    frame(c.mDataB.mPivot, c.mDataB.mAxis, c.mDataB.mPerpAxis1);
                    rag << ",0,0,0," << c.mMinAngle << ',' << c.mMaxAngle << ']';
                }
            }
            result.ragdoll = std::move(rag).str() + "]}";
        }
        std::ostringstream out;
        out << std::setprecision(9) << '{';
        if (cart)
        {
            out << "\"bodies\":[";
            bool firstBody = true;
            for (const auto& [body, node] : bodies)
            {
                const auto& info = body->mInfo;
                out << (firstBody ? "" : ",") << "{\"node\":" << std::quoted(node->source->mName) << ",\"rest\":";
                firstBody = false;
                result.animatedNodes.insert(node->source->mName);
                pose(out, node->transform.getTrans(), node->transform.getRotate(), node->transform.getScale().x());
                out << ",\"mass\":" << info.mMass << ",\"motion\":" << int(info.mMotionType)
                    << ",\"friction\":" << info.mFriction << ",\"restitution\":" << info.mRestitution
                    << ",\"linearDamping\":" << info.mLinearDamping << ",\"angularDamping\":" << info.mAngularDamping
                    << ",\"gravity\":" << info.mGravityFactor << ",\"center\":[" << info.mCenter.x() * units << ','
                    << info.mCenter.y() * units << ',' << info.mCenter.z() * units << "],\"inertia\":[";
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        out << ((i || j) ? "," : "") << info.mInertiaTensor.mValues[i][j];
                out << "],\"hulls\":";
                hulls(out, solidHulls(*node));
                out << '}';
            }
            out << "],\"joints\":[";
            bool firstJoint = true;
            const auto frame = [&](const auto& data) {
                const auto vector = [&](const auto& v, float scale) { out << '[' << v.x()*scale << ',' << v.y()*scale << ',' << v.z()*scale << ']'; };
                out << "{\"pivot\":"; vector(data.mPivot, units);
                out << ",\"axis\":"; vector(data.mAxis, 1);
                out << ",\"perpendicular\":"; vector(data.mPerpAxis1, 1); out << '}';
            };
            for (const auto* joint : joints)
            {
                out << (firstJoint ? "" : ",") << "{\"type\":" << std::quoted(joint->mRecordName)
                    << ",\"a\":" << std::quoted(bodies.at(dynamic_cast<const Nif::bhkRigidBody*>(joint->mInfo.mEntityA.getPtr()))->source->mName)
                    << ",\"b\":" << std::quoted(bodies.at(dynamic_cast<const Nif::bhkRigidBody*>(joint->mInfo.mEntityB.getPtr()))->source->mName);
                firstJoint = false;
                if (const auto* h = dynamic_cast<const Nif::bhkHingeConstraint*>(joint))
                { out << ",\"frameA\":"; frame(h->mConstraint.mDataA); out << ",\"frameB\":"; frame(h->mConstraint.mDataB); }
                else if (const auto* h = dynamic_cast<const Nif::bhkLimitedHingeConstraint*>(joint))
                {
                    out << ",\"frameA\":"; frame(h->mConstraint.mDataA); out << ",\"frameB\":"; frame(h->mConstraint.mDataB);
                    out << ",\"min\":" << h->mConstraint.mMinAngle << ",\"max\":" << h->mConstraint.mMaxAngle;
                }
                out << '}';
            }
            out << "],\"root\":" << std::quoted(rootName) << ",\"wheels\":[";
            bool first = true;
            for (const auto* wheelNode : wheelNodes)
            {
                if (!first)
                    out << ',';
                first = false;
                const auto& wheel = *wheelNode;
                out << "{\"node\":" << std::quoted(wheel.source->mName) << ",\"rest\":";
                pose(out, wheel.transform.getTrans(), wheel.transform.getRotate(), wheel.transform.getScale().x());
                out << ",\"hulls\":";
                hulls(out, solidHulls(wheel));
                out << '}';
            }
            out << "],\"chassis\":";
            if (const auto root = nodes.find(rootName); root != nodes.end())
            {
                Collision chassis;
                chassis.hulls = solidHulls(*root->second);
                hulls(out, transformed(std::move(chassis), root->second->transform).hulls);
            }
            else
                out << "[]";
        }
        else if (!actorSkeleton && !sequences.empty())
        {
            out << "\"sequences\":{";
            bool first = true;
            for (const auto& [name, sequence] : sequences)
            {
                if (!first)
                    out << ',';
                first = false;
                const float duration = sequence->mStopTime - sequence->mStartTime;
                out << std::quoted(name) << ":{\"seconds\":" << duration << ",\"tracks\":[";
                bool firstTrack = true;
                for (const auto& block : sequence->mControlledBlocks)
                {
                    const auto target = nodes.find(block.mNodeName);
                    if (target == nodes.end() || block.mInterpolator.empty())
                        continue;
                    const auto* transform = dynamic_cast<const Nif::NiTransformInterpolator*>(block.mInterpolator.getPtr());
                    if (!transform || transform->mData.empty())
                        continue;
                    const auto& keys = transform->mData.get();
                    const auto restLocal = target->second->transform * osg::Matrixf::inverse(target->second->parent);
                    NifOsg::ValueInterpolator<Nif::Vector3KeyMap> position(keys.mTranslations, restLocal.getTrans());
                    NifOsg::ValueInterpolator<Nif::QuaternionKeyMap> rotation(keys.mRotations, restLocal.getRotate());
                    NifOsg::ValueInterpolator<Nif::FloatKeyMap> xRotation(keys.mXRotations), yRotation(keys.mYRotations),
                        zRotation(keys.mZRotations);
                    NifOsg::ValueInterpolator<Nif::FloatKeyMap> scale(keys.mScales, static_cast<float>(restLocal.getScale().x()));
                    constexpr std::array<std::array<unsigned, 3>, 9> order{{
                        {0, 1, 2}, {0, 2, 1}, {1, 2, 0}, {1, 0, 2}, {2, 0, 1}, {2, 1, 0}, {0, 1, 0}, {1, 2, 1}, {2, 0, 2}
                    }};
                    const auto axis = std::min<std::size_t>(static_cast<unsigned>(keys.mAxisOrder), order.size() - 1);
                    if (!firstTrack)
                        out << ',';
                    firstTrack = false;
                    result.animatedNodes.insert(target->first);
                    out << "{\"node\":" << std::quoted(target->first) << ",\"rest\":";
                    const auto& rest = target->second->transform;
                    pose(out, rest.getTrans(), rest.getRotate(), rest.getScale().x());
                    out << ",\"frames\":[";
                    const auto count = static_cast<unsigned>(std::ceil(duration * 30));
                    for (unsigned i = 0; i <= count; ++i)
                    {
                        if (i)
                            out << ',';
                        const float time = sequence->mStartTime + duration * i / count;
                        auto q = rotation.interpKey(time);
                        if (!xRotation.empty() || !yRotation.empty() || !zRotation.empty())
                        {
                            const std::array xyz{osg::Quat(xRotation.interpKey(time), osg::X_AXIS),
                                osg::Quat(yRotation.interpKey(time), osg::Y_AXIS), osg::Quat(zRotation.interpKey(time), osg::Z_AXIS)};
                            q = xyz[order[axis][0]] * xyz[order[axis][1]] * xyz[order[axis][2]];
                        }
                        const float uniformScale = scale.interpKey(time);
                        auto matrix = osg::Matrixf::scale(uniformScale, uniformScale, uniformScale)
                            * osg::Matrixf::rotate(q) * osg::Matrixf::translate(position.interpKey(time)) * target->second->parent;
                        pose(out, matrix.getTrans(), matrix.getRotate(), matrix.getScale().x());
                    }
                    out << "]}";
                }
                out << "]}";
            }
            out << '}';
        }
        out << '}';
        if (cart || (!actorSkeleton && !sequences.empty()))
            result.json = std::move(out).str();
        for (const auto& name : clipNodes)
            if (nodes.contains(name))
                result.animatedNodes.insert(name);
        std::ostringstream collision;
        collision << std::setprecision(9) << '[';
        bool comma = false;
        for (const auto& entry : entries)
        {
            const auto& name = entry.source->mName;
            std::string_view motion;
            for (const Node* current = &entry; current; current = current->parentNode)
                if (result.animatedNodes.contains(current->source->mName))
                { motion = current->source->mName; break; }
            const bool cartPart = cart && result.animatedNodes.contains(name);
            for (const auto& [kind, data] : entry.collision)
            {
                auto local = transformed(data, actorSkeleton ? osg::Matrixf{} : entry.transform);
                removeInvalidGeometry(local);
                if (local.empty())
                    continue;
                if (actorSkeleton)
                {
                    const auto body = transformed(local, entry.transform);
                    for (const auto& hull : body.hulls)
                        for (const auto& vertex : hull)
                            result.actorBounds.expandBy(osg::Vec3d(vertex));
                    for (const auto& vertex : body.mesh.vertices)
                        result.actorBounds.expandBy(osg::Vec3d(vertex));
                    for (const auto& round : body.round)
                    {
                        const osg::Vec3d a(round.a), b(round.b);
                        const osg::Vec3d ra(round.radiusA, round.radiusA, round.radiusA);
                        const osg::Vec3d rb(round.radiusB, round.radiusB, round.radiusB);
                        result.actorBounds.expandBy(a - ra);
                        result.actorBounds.expandBy(a + ra);
                        result.actorBounds.expandBy(b - rb);
                        result.actorBounds.expandBy(b + rb);
                    }
                }
                const bool dedicated = cartPart && kind == Contact::Solid;
                collision << (comma ? "," : "") << "{\"motion\":" << std::quoted(motion)
                    << ",\"contact\":" << static_cast<int>(actorSkeleton ? Contact::Pick : kind)
                    << ",\"cartHull\":" << dedicated;
                if (actorSkeleton)
                    collision << ",\"bone\":" << std::quoted(name);
                collision << ',';
                writeCollision(collision, local);
                collision << '}';
                comma = true;
            }
        }
        if (comma)
            result.collision = std::move(collision).str() + ']';
        return result;
    }
}
