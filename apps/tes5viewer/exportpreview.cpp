#include "loadprogress.hpp"
#include "exportpreview.hpp"
#include <ranges>
#include "cellscene.hpp"
#include "facemorphs.hpp"
#include "characterseam.hpp"
#include <iostream>

#include <components/esm/fourcc.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nif/data.hpp>
#include <components/nif/node.hpp>
#include <components/nif/property.hpp>
#include <components/nif/texture.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/vfs/manager.hpp>

#include <osg/Geometry>
#include <osg/NodeVisitor>
#include <osg/Texture2D>
#include <osg/Transform>
#include <osg/TriangleIndexFunctor>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace TES5Viewer
{
    namespace
    {
        using ESM::fourCC;

        std::uint32_t field(std::span<const std::byte> bytes, std::size_t offset, std::size_t size)
        {
            if (offset + size > bytes.size())
                throw std::runtime_error("Truncated TES5 LAND material record");
            std::uint32_t result = 0;
            for (std::size_t i = 0; i < size; ++i)
                result |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (i * 8);
            return result;
        }

        std::string readString(std::span<const std::byte> bytes)
        {
            return { reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::size_t>(std::ranges::find(bytes, std::byte{}) - bytes.begin()) };
        }

        struct Indices
        {
            std::vector<std::uint32_t> values;
            void operator()(unsigned a, unsigned b, unsigned c)
            {
                if (a != b && b != c && a != c)
                    values.insert(values.end(), { a, b, c });
            }
        };

        struct Shader
        {
            std::array<std::string, 9> maps;
            osg::Vec4f color{ 1, 1, 1, 1 };
            osg::Vec3f emission, specular{ 1, 1, 1 };
            osg::Vec2f uvScale{ 1, 1 }, uvOffset;
            float gloss = 30.f, specStrength = 1.f, cutoff = .5f;
            unsigned type = 0, flags = 0, flags2 = 0;
            bool twoSided = false, alphaTest = false, alphaBlend = false;
        };

        template <class Shape>
        void readShader(Shader& shader, const Shape& shape)
        {
            if (!shape.mShaderProperty.empty())
            {
                if (const auto* lighting
                    = dynamic_cast<const Nif::BSLightingShaderProperty*>(shape.mShaderProperty.getPtr()))
                {
                    shader.type = lighting->mType;
                    shader.flags = lighting->mShaderFlags1;
                    shader.flags2 = lighting->mShaderFlags2;
                    shader.twoSided = lighting->doubleSided();
                    shader.uvScale = lighting->mUVScale;
                    shader.uvOffset = lighting->mUVOffset;
                    shader.gloss = lighting->mGlossiness;
                    shader.specular = lighting->mSpecular;
                    shader.specStrength = lighting->specular() ? lighting->mSpecStrength : 0.f;
                    shader.emission = lighting->mEmissive * lighting->mEmissiveMult;
                    shader.color.a() = lighting->mAlpha;
                    if (shader.type == 5)
                        shader.color = osg::Vec4f(lighting->mSkinTintColor, shader.color.a());
                    if (shader.type == 6)
                        shader.color = osg::Vec4f(lighting->mHairTintColor, shader.color.a());
                    if (!lighting->mTextureSet.empty())
                        for (std::size_t i = 0;
                            i < std::min(shader.maps.size(), lighting->mTextureSet->mTextures.size()); ++i)
                            if (!lighting->mTextureSet->mTextures[i].empty())
                                shader.maps[i] = lighting->mTextureSet->mTextures[i];
                }
                else if (const auto* effect
                    = dynamic_cast<const Nif::BSEffectShaderProperty*>(shape.mShaderProperty.getPtr()))
                {
                    shader.type = 100;
                    shader.flags = effect->mShaderFlags1;
                    shader.flags2 = effect->mShaderFlags2;
                    shader.twoSided = effect->doubleSided();
                    shader.uvScale = effect->mUVScale;
                    shader.uvOffset = effect->mUVOffset;
                    shader.color = effect->mBaseColor;
                    shader.emission
                        = osg::Vec3f(shader.color.r(), shader.color.g(), shader.color.b()) * effect->mBaseColorScale;
                }
            }
            if (!shape.mAlphaProperty.empty())
            {
                const auto& alpha = *shape.mAlphaProperty.getPtr();
                shader.alphaTest = alpha.useAlphaTesting();
                shader.alphaBlend = alpha.useAlphaBlending();
                shader.cutoff = alpha.mThreshold / 255.f;
            }
        }

        struct LandLayer
        {
            std::uint32_t texture = 0;
            unsigned index = 0;
            std::array<float, 17 * 17> weights{};
        };

        constexpr std::size_t maxInfluences = 8;
        struct SkinVertex
        {
            std::array<std::uint16_t, maxInfluences> joints{};
            std::array<float, maxInfluences> weights{};
        };
        static_assert(sizeof(SkinVertex) == 48);

        struct Rig
        {
            struct Joint
            {
                std::string name;
                int parent;
                osg::Matrixf local;
            };
            std::vector<Joint> joints;
            std::map<std::string, std::uint16_t, std::less<>> indices;
            std::set<const Nif::NiAVObject*> visited;

            std::vector<osg::Matrixd> worldMatrices() const
            {
                std::vector<osg::Matrixd> result;
                for (const auto& joint : joints)
                    result.push_back(joint.parent<0 ? osg::Matrixd(joint.local) : osg::Matrixd(joint.local)*result.at(joint.parent));
                return result;
            }

            void add(const Nif::NiAVObject& node, int parent, const osg::Matrixf& inherited)
            {
                const auto* branch = dynamic_cast<const Nif::NiNode*>(&node);
                if (!branch || joints.size() >= std::numeric_limits<std::uint16_t>::max() || !visited.insert(&node).second)
                    return;
                auto local = node.mTransform.toMatrix() * inherited;
                const auto name = Misc::StringUtils::lowerCase(node.mName);
                if (!name.empty() && !indices.contains(name))
                {
                    const auto index = static_cast<std::uint16_t>(joints.size());
                    indices.emplace(name, index);
                    joints.push_back({ node.mName, parent, local });
                    parent = index;
                    local.makeIdentity();
                }
                for (const auto& child : branch->mChildren)
                    if (!child.empty())
                        add(*child.getPtr(), parent, local);
            }
        };

        std::vector<SkinVertex> readWeights(const Nif::NiSkinInstance& skin, const Rig& rig, std::size_t vertexCount)
        {
            std::vector<std::optional<std::uint16_t>> indices(skin.mBones.size());
            for (std::size_t i = 0; i < skin.mBones.size(); ++i)
                if (!skin.mBones[i].empty())
                    if (const auto it = rig.indices.find(Misc::StringUtils::lowerCase(skin.mBones[i]->mName)); it != rig.indices.end())
                        indices[i] = it->second;
            using Influence = std::pair<std::uint16_t, float>;
            std::vector<std::vector<Influence>> vertices(vertexCount);
            bool missingBone = false;
            const auto add = [&](auto& out, std::size_t bone, float weight) {
                if (!std::isfinite(weight) || weight <= 0)
                    return;
                if (bone >= indices.size() || !indices[bone])
                {
                    missingBone = true;
                    return;
                }
                if (auto it = std::ranges::find(out, *indices[bone], &Influence::first); it != out.end())
                    it->second += weight;
                else
                    out.emplace_back(*indices[bone], weight);
            };
            if (const auto* partitions = skin.getPartitions())
                for (const auto& part : partitions->mPartitions)
                {
                    if (part.mVertexMap.empty() || part.mBoneIndices.size() != part.mWeights.size()
                        || part.mWeights.size() % part.mVertexMap.size() != 0)
                        continue;
                    const auto count = part.mWeights.size() / part.mVertexMap.size();
                    for (std::size_t i = 0; i < part.mVertexMap.size(); ++i)
                    {
                        const auto vertex = part.mVertexMap[i];
                        if (vertex >= vertices.size() || !vertices[vertex].empty())
                            continue;
                        for (std::size_t j = 0; j < count; ++j)
                        {
                            const auto local = static_cast<unsigned char>(part.mBoneIndices[i * count + j]);
                            if (local < part.mBones.size())
                                add(vertices[vertex], part.mBones[local], part.mWeights[i * count + j]);
                        }
                    }
                }
            if (!skin.mData.empty())
            {
                const auto* data = skin.mData.getPtr();
                std::vector<bool> useBoneWeights(vertexCount);
                for (std::size_t i = 0; i < vertexCount; ++i)
                    useBoneWeights[i] = vertices[i].empty();
                for (std::size_t bone = 0; bone < data->mBones.size(); ++bone)
                    for (const auto& [vertex, weight] : data->mBones[bone].mWeights)
                        if (vertex < vertexCount && useBoneWeights[vertex])
                            add(vertices[vertex], bone, weight);
            }
            if (missingBone || std::ranges::none_of(vertices, [](const auto& weights) { return !weights.empty(); }))
                return {};
            std::vector<SkinVertex> out(vertexCount);
            for (std::size_t i = 0; i < vertices.size(); ++i)
            {
                auto& row = vertices[i];
                std::ranges::sort(row, std::greater{}, &Influence::second);
                const auto count = std::min(row.size(), maxInfluences);
                float total = 0;
                for (std::size_t j = 0; j < count; ++j)
                    total += row[j].second;
                if (total > 0)
                    for (std::size_t j = 0; j < count; ++j)
                    {
                        out[i].joints[j] = row[j].first;
                        out[i].weights[j] = row[j].second / total;
                    }
                else
                    out[i].weights[0] = 1;
            }
            return out;
        }

        void writeMatrix(std::ostream& out, const osg::Matrixd& value)
        {
            out << '[';
            for (unsigned row = 0; row < 4; ++row)
                for (unsigned col = 0; col < 4; ++col)
                    out << (row || col ? "," : "") << value(row, col);
            out << ']';
        }

        class ExportVisitor : public osg::NodeVisitor
        {
        public:
            ExportVisitor(const VFS::Manager& vfs, Resource::NifFileManager& nifFiles,
                const ReadWinningRecord& readRecord, const osg::Vec3d& origin,
                std::ostream& manifest, std::ostream& geometry, PreviewBundle& bundle)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mVfs(vfs)
                , mNifFiles(nifFiles)
                , mReadRecord(readRecord)
                , mOrigin(origin)
                , mManifest(manifest)
                , mGeometry(geometry)
                , mBundle(bundle)
            {
            }

            void apply(osg::Drawable& drawable) override
            {
                const auto* geometry = drawable.asGeometry();
                if (!geometry)
                    return;
                const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());
                if (!vertices || vertices->empty())
                    return;
                const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry->getNormalArray());
                if (mLand)
                {
                    writeTerrain(*vertices, normals);
                    return;
                }
                osg::TriangleIndexFunctor<Indices> triangles;
                geometry->accept(triangles);
                const auto* uv = dynamic_cast<const osg::Vec2Array*>(geometry->getTexCoordArray(0));
                const auto* colors = dynamic_cast<const osg::Vec4ubArray*>(geometry->getColorArray());
                const auto* floatColors = dynamic_cast<const osg::Vec4Array*>(geometry->getColorArray());
                const auto transform = osg::computeLocalToWorld(getNodePath());
                if (mReplaceLodCells)
                {
                    std::size_t kept = 0;
                    for (std::size_t i = 0; i + 2 < triangles.values.size(); i += 3)
                    {
                        const auto a = triangles.values[i], b = triangles.values[i + 1], c = triangles.values[i + 2];
                        if (a >= vertices->size() || b >= vertices->size() || c >= vertices->size())
                            continue;
                        const auto center = (osg::Vec3d((*vertices)[a]) + osg::Vec3d((*vertices)[b])
                            + osg::Vec3d((*vertices)[c])) / 3. * transform;
                        const auto x = static_cast<int>(std::floor(center.x() / LandTile::size));
                        const auto y = static_cast<int>(std::floor(center.y() / LandTile::size));
                        if (!mReplaceLodCells->contains({x, y}))
                            for (std::size_t j = 0; j < 3; ++j)
                                triangles.values[kept++] = triangles.values[i + j];
                    }
                    triangles.values.resize(kept);
                }
                if (triangles.values.empty())
                    return;
                Shader shader;
                std::string source;
                osg::Vec3f actorSkinTone;
                bool hasActorSkinTone = false;
                unsigned recordIndex = ~0u;
                for (const auto* node : getNodePath())
                {
                    hasActorSkinTone |= node->getUserValue("helgenSkinTone", actorSkinTone);
                    if (node->getUserValue("helgenSourceNif", source))
                        recordIndex = ~0u;
                    node->getUserValue("recordIndex", recordIndex);
                    const auto* state = node->getStateSet();
                    if (!state)
                        continue;
                    for (unsigned unit = 0; unit < state->getTextureAttributeList().size(); ++unit)
                    {
                        const auto* type = state->getTextureAttribute(unit, SceneUtil::TextureType::AttributeType);
                        const auto* texture = dynamic_cast<const osg::Texture2D*>(
                            state->getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
                        if (!type || !texture || !texture->getImage())
                            continue;
                        const auto name = type->getName();
                        if (name == "diffuseMap")
                            shader.maps[0] = texture->getImage()->getFileName();
                        else if (name == "normalMap")
                            shader.maps[1] = texture->getImage()->getFileName();
                        else if (name == "emissiveMap")
                            shader.maps[2] = texture->getImage()->getFileName();
                    }
                }
                const Nif::NiSkinInstance* skin = nullptr;
                Nif::NIFFilePtr file;
                if (!source.empty() && recordIndex != ~0u)
                {
                    file = mNifFiles.get(VFS::Path::Normalized(source));
                    if (recordIndex < file->mRecords.size())
                    {
                        if (mPreview->directShapesOnly)
                            if (const auto* sourceShape = dynamic_cast<const Nif::NiAVObject*>(file->mRecords[recordIndex].get());
                                sourceShape && std::ranges::none_of(sourceShape->mParents,
                                    [](const auto* parent) { return parent->mParents.empty(); }))
                                return;
                        if (const auto* shape = dynamic_cast<const Nif::BSTriShape*>(file->mRecords[recordIndex].get()))
                        {
                            readShader(shader, *shape);
                            if (!shape->mSkin.empty())
                                skin = dynamic_cast<const Nif::NiSkinInstance*>(shape->mSkin.getPtr());
                        }
                        else if (const auto* geometryShape = dynamic_cast<const Nif::NiGeometry*>(file->mRecords[recordIndex].get()))
                        {
                            readShader(shader, *geometryShape);
                            if (!geometryShape->mSkin.empty())
                                skin = geometryShape->mSkin.getPtr();
                        }
                    }
                }
                if (shader.type==5)
                    if (const auto found=mPreview->skinTextures.find(source); found!=mPreview->skinTextures.end())
                    {
                        // SkinTint uses diffuse, model-space normal, subsurface
                        // and specular maps from ARMA's sex-specific TXST.
                        const auto& maps=found->second;
                        if (!maps[0].empty()) shader.maps[0]=maps[0];
                        if (!maps[1].empty()) shader.maps[1]=maps[1];
                        if (!maps[3].empty()) shader.maps[3]=maps[3];
                        if (!maps[7].empty()) shader.maps[2]=shader.maps[7]=maps[7];
                    }
                std::vector<SkinVertex> weights;
                std::string rigPath;
                if (mPreview->preserveSkin && skin)
                {
                    rigPath = !mPreview->skeleton.empty() && mVfs.exists(VFS::Path::Normalized(mPreview->skeleton))
                        ? mPreview->skeleton : source;
                    weights = readWeights(*skin, getRig(rigPath), vertices->size());
                    if (weights.empty() && rigPath != source)
                    {
                        rigPath = source;
                        weights = readWeights(*skin, getRig(rigPath), vertices->size());
                    }
                }
                auto meshTransform = transform;
                auto meshOrigin = mOrigin;
                beginSection(vertices->size(), triangles.values.size());
                if (!weights.empty())
                {
                    const osg::NodePath objectPath{getNodePath().front()};
                    const auto placement = osg::computeLocalToWorld(objectPath);
                    auto shiftedPlacement = placement;
                    shiftedPlacement.setTrans(placement.getTrans() - mOrigin);
                    meshTransform = transform * osg::Matrixd::inverse(placement);
                    meshOrigin.set(0, 0, 0);
                    mManifest << ",\"rig\":" << std::quoted(rigPath) << ",\"placement\":";
                    writeMatrix(mManifest, shiftedPlacement);
                    mManifest << ",\"skinOffset\":" << static_cast<std::uint64_t>(mSkin.tellp());
                }
                const auto inverse = osg::Matrixd::inverse(meshTransform);
                const bool character = mObject.starts_with("T54UEPlayer_") && !weights.empty();
                std::vector<osg::Matrixd> bindTransforms;
                if (character && skin && !skin->mData.empty())
                {
                    const auto& rig=getRig(rigPath); const auto globals=rig.worldMatrices();
                    std::vector<std::optional<osg::Matrixd>> bindings(rig.joints.size());
                    for (std::size_t i=0;i<skin->mBones.size() && i<skin->mData->mBones.size();++i)
                        if (!skin->mBones[i].empty())
                            if (auto found=rig.indices.find(Misc::StringUtils::lowerCase(skin->mBones[i]->mName));found!=rig.indices.end())
                                bindings[found->second]=osg::Matrixd(skin->mData->mBones[i].mTransform.toMatrix())*globals[found->second];
                    for (const auto& vertex : weights)
                    {
                        osg::Matrixd matrix; std::fill_n(matrix.ptr(),16,0.);
                        bool complete=true;
                        for (std::size_t j=0;j<maxInfluences;++j) if (vertex.weights[j]>0)
                        {
                            if (!bindings[vertex.joints[j]]) { complete=false; break; }
                            for (unsigned n=0;n<16;++n) matrix.ptr()[n]+=bindings[vertex.joints[j]]->ptr()[n]*vertex.weights[j];
                        }
                        bindTransforms.push_back(complete ? matrix : meshTransform);
                    }
                }
                std::vector<osg::Vec3f> bindPositions, bindNormals;
                CharacterSeamResult seam;
                if (character)
                {
                    if (mSeamObject!=mObject) { mSeamObject=mObject; mCharacterNeck={}; }
                    for (std::size_t i=0;i<vertices->size();++i)
                    {
                        const auto& matrix=bindTransforms.empty() ? meshTransform : bindTransforms[i];
                        bindPositions.emplace_back(osg::Vec3d((*vertices)[i])*matrix-meshOrigin);
                        osg::Vec3f normal(0,0,1);
                        if (normals && i<normals->size()) normal=osg::Matrixd::transform3x3(osg::Matrixd::inverse(matrix),osg::Vec3d((*normals)[i]));
                        normal.normalize(); bindNormals.push_back(normal);
                    }
                    if (shader.type==5) mCharacterNeck.consider(bindPositions,bindNormals,weights,characterRims(bindPositions,triangles.values));
                    if (shader.type==4) seam=fitCharacterNeck(bindPositions,bindNormals,weights,triangles.values,mCharacterNeck);
                }
                if (!weights.empty()) mSkin.write(reinterpret_cast<const char*>(weights.data()),weights.size()*sizeof(SkinVertex));
                mManifest << ",\"shape\":" << std::quoted(geometry->getName()) << ",\"source\":" << std::quoted(source)
                          << ",\"material\":{\"shader\":" << shader.type << ",\"flags\":" << shader.flags
                          << ",\"flags2\":" << shader.flags2
                          << ",\"twoSided\":" << shader.twoSided << ",\"alphaTest\":" << shader.alphaTest
                          << ",\"alphaBlend\":" << shader.alphaBlend << ",\"cutoff\":" << shader.cutoff
                          << ",\"vertexColor\":" << (colors != nullptr || floatColors != nullptr) << ",\"gloss\":" << shader.gloss
                          << ",\"specStrength\":" << shader.specStrength << ",\"color\":[" << shader.color.r() << ','
                          << shader.color.g() << ',' << shader.color.b() << ',' << shader.color.a()
                          << "],\"emission\":";
                writeVector(shader.emission);
                mManifest << ",\"specular\":";
                writeVector(shader.specular);
                mManifest << ",\"maps\":[";
                for (std::size_t i = 0; i < shader.maps.size(); ++i)
                    mManifest << (i ? "," : "") << std::quoted(copyTexture(shader.maps[i]));
                mManifest << ']';
                mManifest << ",\"sourceMaps\":[";
                for (std::size_t i=0;i<shader.maps.size();++i)
                    mManifest << (i ? "," : "") << std::quoted(shader.maps[i]);
                mManifest << ']';
                // Generated heads have no baked FaceTint DDS. Apply the same
                // actor skin transform as the body, hands and feet. Authored
                // FaceGen already contains its tint and must not be tinted twice.
                if ((shader.type == 5 || shader.type == 4)
                    && hasActorSkinTone && source.find("/facegendata/") == std::string::npos)
                {
                    mManifest << ",\"skinTone\":";
                    writeVector(actorSkinTone);
                }
                if ((shader.flags & 0x1000) != 0)
                {
                    mManifest << ",\"basis\":[";
                    for (unsigned axis = 0; axis < 3; ++axis)
                    {
                        osg::Vec3d v;
                        v[axis] = 1.;
                        v = osg::Matrixd::transform3x3(v, transform);
                        v.normalize();
                        mManifest << (axis ? "," : "") << '[' << v.x() << ',' << -v.y() << ',' << v.z() << ']';
                    }
                    mManifest << ']';
                }
                mManifest << '}';
                if (character && shader.type==4)
                    mManifest << ",\"neckSeam\":{\"vertices\":" << seam.vertices << ",\"before\":" << seam.before << ",\"after\":" << seam.after << '}';
                if (!weights.empty() && mPreview)
                {
                    const auto* shape = file && recordIndex < file->mRecords.size()
                        ? dynamic_cast<const Nif::NiAVObject*>(file->mRecords[recordIndex].get()) : nullptr;
                    std::vector<std::pair<std::string, bool>> paths;
                    if (shape)
                    {
                        auto it = mPreview->expressionMorphs.find(shape->mName);
                        if (it == mPreview->expressionMorphs.end()) it = mPreview->expressionMorphs.find(source);
                        if (it != mPreview->expressionMorphs.end())
                            paths.emplace_back(it->second, false);
                        auto creationIt = mPreview->creationMorphs.find(shape->mName);
                        if (creationIt == mPreview->creationMorphs.end()) creationIt = mPreview->creationMorphs.find(source);
                        if (creationIt != mPreview->creationMorphs.end())
                            for (const auto& path : creationIt->second) paths.emplace_back(path, true);
                    }
                    if (!paths.empty())
                    {
                        mManifest << ",\"expressions\":[";
                        bool comma = false;
                        std::set<std::string> emitted;
                        for (const auto& [path, creation] : paths)
                        try
                        {
                            const auto stream = mVfs.get(VFS::Path::Normalized(path));
                            const std::string bytes{std::istreambuf_iterator<char>(*stream), {}};
                            for (const auto& expression : readExpressionMorphs(bytes, vertices->size(), triangles.values))
                            {
                                if (!emitted.insert((creation ? "CharGen:" : "") + expression.name).second) continue;
                                mManifest << (comma ? "," : "") << '[' << std::quoted((creation ? "CharGen:" : "") + expression.name) << ','
                                    << static_cast<std::uint64_t>(mMorph.tellp()) << ']';
                                for (std::size_t i=0;i<expression.deltas.size();++i)
                                {
                                    osg::Vec3f transformed(osg::Matrixd::transform3x3(osg::Vec3d(expression.deltas[i]),bindTransforms.empty() ? meshTransform : bindTransforms[i]));
                                    if (creation && i<seam.morphMask.size()) transformed*=seam.morphMask[i];
                                    mMorph.write(reinterpret_cast<const char*>(transformed.ptr()), sizeof(float)*3);
                                }
                                comma = true;
                            }
                        }
                        catch (const std::exception& error) { std::cerr << "T5_FACE_MORPH " << path << ": " << error.what() << '\n'; }
                        mManifest << ']';
                    }
                }
                mManifest << '}';
                for (std::size_t i = 0; i < vertices->size(); ++i)
                {
                    const auto p = character ? osg::Vec3d(bindPositions[i]) : osg::Vec3d((*vertices)[i]) * meshTransform - meshOrigin;
                    auto n = normals && i < normals->size()
                        ? osg::Matrixd::transform3x3(inverse, osg::Vec3d((*normals)[i]))
                        : osg::Vec3d(0, 0, 1);
                    if (character) n=bindNormals[i];
                    if (n.normalize() == 0.)
                        n.set(0, 0, 1);
                    const auto tex = uv && i < uv->size() ? (*uv)[i] : osg::Vec2f();
                    auto tint = colors && i < colors->size() ? (*colors)[i] : osg::Vec4ub(255, 255, 255, 255);
                    if (floatColors && i < floatColors->size())
                        for (unsigned channel = 0; channel < 4; ++channel)
                            tint[channel] = static_cast<unsigned char>(std::clamp((*floatColors)[i][channel], 0.f, 1.f) * 255.f);
                    writeVertex(p, n,
                        osg::Vec2f(tex.x() * shader.uvScale.x() + shader.uvOffset.x(),
                            tex.y() * shader.uvScale.y() + shader.uvOffset.y()),
                        tint);
                }
                writeIndices(triangles.values);
            }

            std::string mObject, mGroup;
            const LandTile* mLand = nullptr;
            const std::set<std::pair<int, int>>* mReplaceLodCells = nullptr;
            const PreviewObject* mPreview = nullptr;

            void finish()
            {
                if (mSkin.tellp() == std::streampos(0))
                    return;
                mManifest << ",\"rigs\":{";
                bool comma = false;
                for (const auto& [name, rig] : mRigs)
                {
                    mManifest << (comma ? "," : "") << std::quoted(name) << ":[";
                    bool jointComma = false;
                    for (const auto& joint : rig.joints)
                    {
                        mManifest << (jointComma ? "," : "") << '[' << std::quoted(joint.name) << ',' << joint.parent << ',';
                        writeMatrix(mManifest, joint.local);
                        mManifest << ']';
                        jointComma = true;
                    }
                    mManifest << ']';
                    comma = true;
                }
                mManifest << '}';
                mBundle.emplace("skins.bin", std::move(mSkin).str());
                mBundle.emplace("morphs.bin", std::move(mMorph).str());
            }

        private:
            const Rig& getRig(const std::string& path)
            {
                auto [it, inserted] = mRigs.try_emplace(path);
                if (inserted)
                {
                    auto& rig = it->second;
                    rig.joints.push_back({ "T5Root", -1, {} });
                    const auto file = mNifFiles.get(VFS::Path::Normalized(path));
                    const Nif::FileView view(*file);
                    for (std::size_t i = 0; i < view.numRoots(); ++i)
                        if (const auto* root = dynamic_cast<const Nif::NiAVObject*>(view.getRoot(i)))
                            rig.add(*root, 0, {});
                }
                return it->second;
            }

            std::map<std::string, Rig, std::less<>> mRigs;
            std::string mSeamObject;
            CharacterNeck<SkinVertex> mCharacterNeck;
            std::ostringstream mSkin, mMorph;
            void beginSection(std::size_t vertices, std::size_t indices)
            {
                if (mComma)
                    mManifest << ",\n";
                mComma = true;
                mManifest << "{\"object\":" << std::quoted(mObject) << ",\"group\":" << std::quoted(mGroup)
                          << ",\"offset\":" << static_cast<std::uint64_t>(mGeometry.tellp())
                          << ",\"vertices\":" << vertices << ",\"indices\":" << indices;
                if (mPreview->reference)
                    mManifest << ",\"reference\":" << mPreview->reference;
                if (!mPreview->animationProject.empty())
                    mManifest << ",\"animationProject\":" << std::quoted(mPreview->animationProject);
                if (!mPreview->animatedNodes.empty())
                    for (const auto* node : getNodePath() | std::views::reverse)
                        if (mPreview->animatedNodes.contains(node->getName()))
                        {
                            mManifest << ",\"motionNode\":" << std::quoted(node->getName());
                            break;
                        }
            }

            void writeVector(const osg::Vec3f& v) { mManifest << '[' << v.x() << ',' << v.y() << ',' << v.z() << ']'; }

            void writeVertex(const osg::Vec3d& p, const osg::Vec3d& n, const osg::Vec2f& uv, const osg::Vec4ub& tint)
            {
                const std::array<float, 12> row{ float(p.x()), float(p.y()), float(p.z()), float(n.x()), float(n.y()),
                    float(n.z()), uv.x(), uv.y(), tint.r() / 255.f, tint.g() / 255.f, tint.b() / 255.f,
                    tint.a() / 255.f };
                mGeometry.write(reinterpret_cast<const char*>(row.data()), sizeof(row));
            }

            void writeIndices(const std::vector<std::uint32_t>& indices)
            {
                mGeometry.write(reinterpret_cast<const char*>(indices.data()), indices.size() * sizeof(std::uint32_t));
            }

            std::array<std::string, 2> landMaps(std::uint32_t id)
            {
                if (!id)
                    return { copyTexture("textures/landscape/fieldgrass01.dds"),
                        copyTexture("textures/landscape/fieldgrass01_n.dds") };
                if (auto found = mLandMaps.find(id); found != mLandMaps.end())
                    return found->second;
                const auto ltex = mReadRecord(id);
                std::uint32_t txst = 0;
                visitSubrecords(ltex, [&](auto type, auto bytes) {
                    if (type == fourCC("TNAM"))
                        txst = ltex.mapFormId(field(bytes, 0, 4));
                });
                std::array<std::string, 2> result;
                if (txst)
                    visitSubrecords(mReadRecord(txst), [&](auto type, auto bytes) {
                        if (type == fourCC("TX00") || type == fourCC("TX01"))
                            result[type == fourCC("TX01")] = copyTexture(readString(bytes));
                    });
                return mLandMaps.emplace(id, result).first->second;
            }

            void writeTerrain(const osg::Vec3Array& positions, const osg::Vec3Array* normals)
            {
                std::array<std::uint32_t, 4> bases{};
                std::array<std::vector<LandLayer>, 4> layers;
                LandLayer* current = nullptr;
                const auto land = mReadRecord(mLand->landId);
                visitSubrecords(land, [&](auto type, auto bytes) {
                    if (type == fourCC("BTXT") || type == fourCC("ATXT"))
                    {
                        const auto quadrant = field(bytes, 4, 1);
                        const auto texture = land.mapFormId(field(bytes, 0, 4));
                        if (quadrant >= bases.size())
                            throw std::runtime_error("Invalid LAND texture quadrant");
                        if (type == fourCC("BTXT"))
                        {
                            bases[quadrant] = texture;
                            current = nullptr;
                        }
                        else
                        {
                            layers[quadrant].push_back({ texture, field(bytes, 6, 2) });
                            current = &layers[quadrant].back();
                        }
                    }
                    else if (type == fourCC("VTXT") && current)
                    {
                        for (std::size_t offset = 0; offset + 8 <= bytes.size(); offset += 8)
                        {
                            const auto index = field(bytes, offset, 2);
                            const float weight = std::bit_cast<float>(field(bytes, offset + 4, 4));
                            if (index < current->weights.size() && std::isfinite(weight))
                                current->weights[index] = std::clamp(weight, 0.f, 1.f);
                        }
                    }
                });
                constexpr unsigned width = 17, stride = LandTile::side;
                for (unsigned quadrant = 0; quadrant < 4; ++quadrant)
                {
                    std::ranges::stable_sort(layers[quadrant], {}, &LandLayer::index);
                    beginSection(width * width, 16 * 16 * 6);
                    auto base = landMaps(bases[quadrant]);
                    mManifest << ",\"terrain\":{\"base\":[" << std::quoted(base[0]) << ',' << std::quoted(base[1])
                              << "],\"layers\":[";
                    bool comma = false;
                    for (const auto& layer : layers[quadrant])
                    {
                        const auto maps = landMaps(layer.texture);
                        if (maps[0].empty() || std::ranges::none_of(layer.weights, [](float w) { return w > 0.f; }))
                            continue;
                        mManifest << (comma ? "," : "") << "{\"maps\":[" << std::quoted(maps[0]) << ','
                                  << std::quoted(maps[1]) << "],\"weights\":[";
                        comma = true;
                        for (std::size_t i = 0; i < layer.weights.size(); ++i)
                            mManifest << (i ? "," : "") << layer.weights[i];
                        mManifest << "]}";
                    }
                    mManifest << "]}}";
                    std::vector<std::uint32_t> indices;
                    const unsigned startX = (quadrant & 1) * 16, startY = (quadrant >> 1) * 16;
                    for (unsigned y = 0; y < width; ++y)
                        for (unsigned x = 0; x < width; ++x)
                        {
                            const auto index = (startY + y) * stride + startX + x;
                            const auto world = osg::Vec3d(positions[index]);
                            const auto normal = normals ? osg::Vec3d((*normals)[index]) : osg::Vec3d(0, 0, 1);
                            const auto& tint = mLand->colors;
                            const auto rgb = mLand->hasColors
                                ? osg::Vec4ub(tint[index * 3], tint[index * 3 + 1], tint[index * 3 + 2], 255)
                                : osg::Vec4ub(255, 255, 255, 255);
                            writeVertex(world - mOrigin, normal,
                                osg::Vec2f(float(world.x() / 512.), float(-world.y() / 512.)), rgb);
                            if (x < 16 && y < 16)
                            {
                                const auto i = y * width + x;
                                indices.insert(indices.end(), { i, i + 1, i + width + 1, i, i + width + 1, i + width });
                            }
                        }
                    writeIndices(indices);
                }
            }

            std::string copyTexture(const std::string& texture)
            {
                if (texture.empty())
                    return {};
                const auto path = Misc::ResourceHelpers::correctTexturePath(VFS::Path::Normalized(texture), mVfs);
                if (!mVfs.exists(path))
                    return {};
                const std::string key(path.value());
                if (const auto found = mTextures.find(key); found != mTextures.end())
                    return found->second;
                const auto extension = std::filesystem::path(key).extension().string();
                const auto name = "Texture_" + std::to_string(mTextures.size()) + extension;
                auto input = mVfs.get(path);
                std::ostringstream output;
                output << input->rdbuf();
                if (!output)
                    throw std::runtime_error("Failed to read texture: " + key);
                const auto bytes = static_cast<std::uint64_t>(output.tellp());
                mBundle.emplace(name, std::move(output).str());
                mBundle.emplace("source:" + name, key);
                loadTexture(key, bytes);
                return mTextures.emplace(key, name).first->second;
            }

            const VFS::Manager& mVfs;
            Resource::NifFileManager& mNifFiles;
            const ReadWinningRecord& mReadRecord;
            const osg::Vec3d& mOrigin;
            std::ostream& mManifest;
            std::ostream& mGeometry;
            PreviewBundle& mBundle;
            std::map<std::string, std::string> mTextures;
            std::map<std::uint32_t, std::array<std::string, 2>> mLandMaps;
            bool mComma = false;
        };
        void writePreview(ExportVisitor& visitor, std::ostream& manifest, const osg::Vec3d& origin,
            const std::vector<PreviewObject>& objects)
        {
            manifest << "{\"version\":2,\"origin\":[" << origin.x() << ',' << origin.y() << ',' << origin.z()
                     << "],\"sections\":[\n";
            loadPhase("Converting models and textures", objects.size());
            for (const auto& object : objects)
            {
                loadItem(object.name);
                visitor.mObject = object.name;
                visitor.mGroup = object.group;
                visitor.mLand = object.land;
                visitor.mReplaceLodCells = object.replaceLodCells;
                visitor.mPreview = &object;
                object.node->accept(visitor);
                loadCompleted();
            }
            manifest << "\n]";
            visitor.finish();
            manifest << "}\n";
        }
    }

    void exportPreview(PreviewBundle& bundle, const VFS::Manager& vfs, Resource::NifFileManager& nifFiles,
        const ReadWinningRecord& readRecord, const osg::Vec3d& origin, const std::vector<PreviewObject>& objects)
    {
        std::ostringstream manifest, geometry;
        ExportVisitor visitor(vfs, nifFiles, readRecord, origin, manifest, geometry, bundle);
        writePreview(visitor, manifest, origin, objects);
        bundle.emplace("meshes.json", std::move(manifest).str());
        bundle.emplace("meshes.bin", std::move(geometry).str());
    }
}
