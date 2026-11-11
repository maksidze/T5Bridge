#include "runtimeexport.hpp"

#include <format>

#include "actorappearance.hpp"
#include "actorpose.hpp"
#include "checkpointtypes.hpp"
#include "hkxmovement.hpp"
#include "loadprogress.hpp"
#include "modelmechanics.hpp"
#include "questscripts.hpp"
#include "runtimecache.hpp"

#include <components/esm/fourcc.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/node.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/vfs/manager.hpp>

#include <osg/ComputeBoundsVisitor>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osgUtil/SmoothingVisitor>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace TES5Viewer
{
    namespace
    {
        bool marker(std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return std::tolower(c); });
            path = path.substr(path.find_last_of('/') + 1);
            return path == "markerxheading.nif" || path == "markerx.nif" || path == "markercocheading.nif"
                || path == "marker_north.nif";
        }

        osg::ref_ptr<osg::Geode> makeLandscapeTile(const LandTile& tile)
        {
            constexpr unsigned size = LandTile::side;
            constexpr unsigned intervals = size - 1;
            constexpr float cellWidth = LandTile::size;
            osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            osg::ref_ptr<osg::DrawElementsUShort> triangles = new osg::DrawElementsUShort(GL_TRIANGLES);
            positions->reserve(size * size);
            colors->reserve(size * size);
            triangles->reserve(intervals * intervals * 6);
            for (unsigned y = 0; y < size; ++y)
                for (unsigned x = 0; x < size; ++x)
                {
                    const auto index = y * size + x;
                    positions->push_back(osg::Vec3(static_cast<float>(tile.x) * cellWidth + x * (cellWidth / intervals),
                        static_cast<float>(tile.y) * cellWidth + y * (cellWidth / intervals), tile.heights[index]));
                    osg::Vec4 color(.49f, .46f, .36f, 1.f);
                    if (tile.hasColors)
                        for (unsigned channel = 0; channel < 3; ++channel)
                            color[channel] *= tile.colors[3 * index + channel] / 255.f;
                    colors->push_back(color);
                    if (x < intervals && y < intervals)
                    {
                        const auto lowerLeft = static_cast<GLushort>(index);
                        const auto lowerRight = static_cast<GLushort>(index + 1);
                        const auto upperLeft = static_cast<GLushort>(index + size);
                        const auto upperRight = static_cast<GLushort>(index + size + 1);
                        for (const auto vertex :
                            { lowerLeft, lowerRight, upperRight, lowerLeft, upperRight, upperLeft })
                            triangles->push_back(vertex);
                    }
                }
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(positions);
            geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
            geometry->addPrimitiveSet(triangles);
            geometry->setUseDisplayList(false);
            geometry->setUseVertexBufferObjects(true);
            osgUtil::SmoothingVisitor::smooth(*geometry);
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->addDrawable(geometry);
            return geode;
        }

        class WeightShapes : public osg::NodeVisitor
        {
        public:
            WeightShapes()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Drawable& drawable) override
            {
                if (auto* geometry = drawable.asGeometry())
                    shapes.emplace(geometry->getName(), Shape{ geometry, osg::computeLocalToWorld(getNodePath()) });
            }

            struct Shape
            {
                osg::Geometry* geometry;
                osg::Matrixd transform;
            };
            std::multimap<std::string, Shape> shapes;
        };

        bool matchingTopology(const osg::Geometry& a, const osg::Geometry& b)
        {
            if (a.getNumPrimitiveSets() != b.getNumPrimitiveSets())
                return false;
            for (unsigned i = 0; i < a.getNumPrimitiveSets(); ++i)
            {
                const auto* left = a.getPrimitiveSet(i);
                const auto* right = b.getPrimitiveSet(i);
                if (left->getMode() != right->getMode() || left->getNumIndices() != right->getNumIndices())
                    return false;
                for (unsigned j = 0; j < left->getNumIndices(); ++j)
                    if (left->index(j) != right->index(j))
                        return false;
            }
            return true;
        }

        void blendWeightModels(osg::Node& selected, osg::Node& other, float fraction)
        {
            WeightShapes a, b;
            selected.accept(a);
            other.accept(b);
            for (const auto& [name, shape] : a.shapes)
            {
                const auto match = b.shapes.find(name);
                if (match == b.shapes.end() || b.shapes.count(name) != 1 || a.shapes.count(name) != 1)
                    continue;
                auto& base = *shape.geometry;
                const auto& target = *match->second.geometry;
                auto* vertices = dynamic_cast<osg::Vec3Array*>(base.getVertexArray());
                const auto* otherVertices = dynamic_cast<const osg::Vec3Array*>(target.getVertexArray());
                if (!vertices || !otherVertices || vertices->size() != otherVertices->size()
                    || !matchingTopology(base, target))
                    continue;
                const auto toBase = match->second.transform * osg::Matrixd::inverse(shape.transform);
                for (std::size_t i = 0; i < vertices->size(); ++i)
                {
                    const auto p = osg::Vec3d((*otherVertices)[i]) * toBase;
                    (*vertices)[i] = (*vertices)[i] * (1.f - fraction) + osg::Vec3f(p) * fraction;
                }
                auto* normals = dynamic_cast<osg::Vec3Array*>(base.getNormalArray());
                const auto* otherNormals = dynamic_cast<const osg::Vec3Array*>(target.getNormalArray());
                if (normals && otherNormals && normals->size() == otherNormals->size())
                {
                    const auto inverse = osg::Matrixd::inverse(toBase);
                    for (std::size_t i = 0; i < normals->size(); ++i)
                    {
                        auto n = osg::Matrixd::transform3x3(inverse, osg::Vec3d((*otherNormals)[i]));
                        n.normalize();
                        auto blended = (*normals)[i] * (1.f - fraction) + osg::Vec3f(n) * fraction;
                        blended.normalize();
                        (*normals)[i] = blended;
                    }
                    normals->dirty();
                }
                vertices->dirty();
                base.dirtyBound();
            }
        }
    }

    namespace
    {
        template <class T>
        T lightingValue(std::span<const std::byte> bytes, std::size_t offset, T fallback = {})
        {
            if (offset + sizeof(T) <= bytes.size())
            {
                T value;
                std::memcpy(&value, bytes.data() + offset, sizeof(T));
                if constexpr (std::is_floating_point_v<T>)
                    return std::isfinite(value) ? value : fallback;
                else
                    return value;
            }
            return fallback;
        }

        void writeColor(std::ostream& out, std::span<const std::byte> bytes, std::size_t offset)
        {
            out << '[' << unsigned(lightingValue<std::uint8_t>(bytes, offset)) << ','
                << unsigned(lightingValue<std::uint8_t>(bytes, offset + 1)) << ','
                << unsigned(lightingValue<std::uint8_t>(bytes, offset + 2)) << ']';
        }

        void writeLocationLighting(std::ostream& out, const CellScene& scene, const ReadWinningRecord& read, int x, int y)
        {
            const auto exteriorCell = std::ranges::find_if(scene.exteriorCells,
                [x, y](const ExteriorCell& cell) { return cell.x == x && cell.y == y; });
            const auto cellId = scene.cellId ? scene.cellId : exteriorCell == scene.exteriorCells.end() ? 0 : exteriorCell->cellId;
            const auto cell = cellId ? read(cellId) : RecordPayload{};
            const auto world = scene.worldspaceId ? read(scene.worldspaceId) : RecordPayload{};
            std::span<const std::byte> own, base, axes;
            std::uint32_t templateId = 0, imageId = 0;
            std::uint16_t cellFlags = 0;
            bool worldNoSky = false;
            if (world.metadata.formId)
                visitSubrecords(world, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("LTMP"))
                        templateId = world.mapFormId(lightingValue<std::uint32_t>(bytes, 0));
                    else if (tag == ESM::fourCC("DATA"))
                        worldNoSky = (lightingValue<std::uint8_t>(bytes, 0) & 0x20) != 0;
                });
            if (cell.metadata.formId)
                visitSubrecords(cell, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("XCLL"))
                        own = bytes;
                    else if (tag == ESM::fourCC("LTMP"))
                    {
                        if (const auto id = cell.mapFormId(lightingValue<std::uint32_t>(bytes, 0)))
                            templateId = id;
                    }
                    else if (tag == ESM::fourCC("XCIM"))
                        imageId = cell.mapFormId(lightingValue<std::uint32_t>(bytes, 0));
                    else if (tag == ESM::fourCC("DATA"))
                        cellFlags = lightingValue<std::uint16_t>(bytes, 0);
                });
            const auto lookup = [&](std::uint32_t id, std::uint32_t type) {
                if (id)
                    try
                    {
                        auto record = read(id);
                        if (record.metadata.recordType == type)
                            return record;
                    }
                    catch (const std::exception& error)
                    {
                        std::cerr << "T5 lighting record " << id << ": " << error.what() << '\n';
                    }
                return RecordPayload{};
            };
            const auto lighting = lookup(templateId, ESM::fourCC("LGTM"));
            if (lighting.metadata.formId)
                visitSubrecords(lighting, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("DATA"))
                        base = bytes;
                    else if (tag == ESM::fourCC("DALC"))
                        axes = bytes;
                });
            const auto image = lookup(imageId, ESM::fourCC("IMGS"));
            out << ",\"environment\":{\"cell\":" << cellId << ",\"template\":" << lighting.metadata.formId
                << ",\"imageSpace\":" << image.metadata.formId << ",\"cellFlags\":" << cellFlags
                << ",\"showSky\":" << (scene.cellId ? (cellFlags & 0x80) != 0 : !worldNoSky)
                << ",\"useSkyLighting\":" << (scene.cellId ? (cellFlags & 0x100) != 0 : !worldNoSky);
            if (!own.empty() || !base.empty())
            {
                const auto inherits = lightingValue<std::uint32_t>(own, 88);
                const auto source = [&](unsigned mask, std::size_t end) {
                    if (base.size() >= end && ((inherits & mask) != 0 || own.size() < end))
                        return base;
                    return own;
                };
                out << ",\"ambient\":";
                writeColor(out, source(0x001, 4), 0);
                auto directionColors = source(0x001, 72);
                std::size_t directionOffset = 40;
                if (directionColors.data() == base.data() && axes.size() >= 24)
                {
                    directionColors = axes;
                    directionOffset = 0;
                }
                out << ",\"ambientFaces\":[";
                for (unsigned face = 0; face < 6; ++face)
                {
                    if (face)
                        out << ',';
                    writeColor(out, directionColors, directionOffset + face * 4);
                }
                out << "],\"directional\":";
                writeColor(out, source(0x002, 8), 4);
                out << ",\"directionalRotation\":[" << lightingValue<std::int32_t>(source(0x020, 28), 20) << ','
                    << lightingValue<std::int32_t>(source(0x020, 28), 24) << ']'
                    << ",\"directionalFade\":" << lightingValue<float>(source(0x040, 32), 28, 1.f)
                    << ",\"fogNear\":" << lightingValue<float>(source(0x008, 16), 12)
                    << ",\"fogFar\":" << lightingValue<float>(source(0x010, 20), 16)
                    << ",\"fogClip\":" << lightingValue<float>(source(0x080, 36), 32)
                    << ",\"fogPower\":" << lightingValue<float>(source(0x100, 40), 36, 1.f)
                    << ",\"fogMax\":" << lightingValue<float>(source(0x200, 80), 76, 1.f)
                    << ",\"lightFadeBegin\":" << lightingValue<float>(source(0x400, 88), 80)
                    << ",\"lightFadeEnd\":" << lightingValue<float>(source(0x400, 88), 84)
                    << ",\"fogNearColor\":";
                writeColor(out, source(0x004, 12), 8);
                out << ",\"fogFarColor\":";
                writeColor(out, source(0x004, 76), 72);
            }
            if (image.metadata.formId)
                visitSubrecords(image, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("CNAM") && bytes.size() >= 12)
                        out << ",\"cinematic\":[" << lightingValue<float>(bytes, 0, 1.f) << ','
                            << lightingValue<float>(bytes, 4, 1.f) << ',' << lightingValue<float>(bytes, 8, 1.f) << ']';
                    else if (tag == ESM::fourCC("HNAM") && bytes.size() >= 16)
                        out << ",\"hdr\":[" << lightingValue<float>(bytes, 0) << ','
                            << lightingValue<float>(bytes, 8) << ',' << lightingValue<float>(bytes, 12) << ']';
                    else if (tag == ESM::fourCC("TNAM") && bytes.size() >= 16)
                        out << ",\"tint\":[" << lightingValue<float>(bytes, 0) << ',' << lightingValue<float>(bytes, 4)
                            << ',' << lightingValue<float>(bytes, 8) << ',' << lightingValue<float>(bytes, 12) << ']';
                });
            out << '}';
        }
    }

    void addRuntimeStaticPlacement(CellScene& scene, const ExteriorReference& reference, const ReadWinningRecord& read)
    {
        const auto base = read(reference.baseId);
        std::string model;
        visitSubrecords(base, [&](auto tag, auto bytes) {
            if (tag != ESM::fourCC("MODL") || bytes.empty())
                return;
            const auto length = std::ranges::find(bytes, std::byte{}) - bytes.begin();
            model.assign(reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(length));
        });
        if (model.empty())
            return;
        scene.placements.push_back({reference.referenceId, reference.baseId, std::move(model), reference.position,
            reference.scale, {}, base.metadata.sourcePlugin, reference.initiallyDisabled});
    }

    std::string exportAnimationObject(const VFS::Manager& vfs, const ReadWinningRecord& read,
        const std::string& model, PreviewBundle& bundle, bool requireSocket)
    {
        const ToUTF8::StatelessUtf8Encoder encoder(ToUTF8::calculateEncoding("win1252"));
        Resource::ResourceSystem resources(&vfs, 30., &encoder);
        auto nif = resources.getNifFileManager()->get(VFS::Path::Normalized(model));
        std::string socket;
        osg::Matrixf anchor;
        const auto find = [&](auto&& self, const Nif::NiAVObject& node, const osg::Matrixf& parent) -> void {
            const auto pose = node.mTransform.toMatrix() * parent;
            constexpr std::string_view prefix = "AnimObject";
            if (node.mName.starts_with(prefix) && node.mName.size() == prefix.size() + 1)
            {
                socket = node.mName;
                anchor = pose;
                return;
            }
            if (const auto* group = dynamic_cast<const Nif::NiNode*>(&node))
                for (const auto& child : group->mChildren)
                    if (!child.empty() && socket.empty()) self(self, child.get(), pose);
        };
        for (const auto* object : nif->mRoots)
            if (const auto* root = dynamic_cast<const Nif::NiAVObject*>(object); root && socket.empty())
            {
                for (const auto& extra : root->mExtraList)
                    if (!extra.empty())
                        if (const auto* parent = dynamic_cast<const Nif::NiStringExtraData*>(extra.getPtr());
                            parent && Misc::StringUtils::ciEqual(parent->mName, "Prn"))
                            socket = parent->mData;
                if (socket.empty()) find(find, *root, osg::Matrixf{});
            }
        if (socket.empty() && requireSocket) return {};
        auto node = NifOsg::Loader::load(*nif, resources.getImageManager(), resources.getBgsmFileManager());
        node->setUserValue("helgenSourceNif", model);
        auto root = osg::ref_ptr(new osg::MatrixTransform(osg::Matrixf::inverse(anchor)));
        root->addChild(node);
        exportPreview(bundle, vfs, *resources.getNifFileManager(), read, {}, {{"Prop", "AnimationObject", root.get()}});
        return socket;
    }

    void exportPlayerView(const VFS::Manager& vfs, const ReadWinningRecord& read, std::uint32_t player,
        const std::set<std::uint32_t>& equipped, const std::string& weaponModel, PreviewBundle& bundle, bool firstPerson)
    {
        const auto appearance = loadActorAppearance(read, vfs, {}, player, equipped, firstPerson);
        const ToUTF8::StatelessUtf8Encoder encoder(ToUTF8::calculateEncoding("win1252"));
        Resource::ResourceSystem resources(&vfs, 30., &encoder);
        const auto load = [&](const std::string& path) {
            auto nif = resources.getNifFileManager()->get(VFS::Path::Normalized(path));
            auto node = NifOsg::Loader::load(*nif, resources.getImageManager(), resources.getBgsmFileManager());
            node->setUserValue("helgenSourceNif", path);
            return node;
        };
        std::vector<osg::ref_ptr<osg::Node>> nodes;
        std::vector<PreviewObject> objects;
        auto body = osg::ref_ptr(new osg::Group);
        if (appearance.skinTone) body->setUserValue("helgenSkinTone", *appearance.skinTone);
        for (const auto& path : appearance.models)
        {
            auto node = load(path);
            if (const auto blend = appearance.weightBlends.find(path); blend != appearance.weightBlends.end())
                blendWeightModels(*node, *load(blend->second.otherModel), blend->second.fraction);
            body->addChild(node);
        }
        if (body->getNumChildren())
        {
            nodes.push_back(body);
            objects.push_back({firstPerson ? "Arms" : "T54UEPlayer_Body", "NPC", body.get(),
                nullptr, nullptr, true, appearance.skeleton});
            auto& object = objects.back();
            object.skinTextures = appearance.skinTextures;
            object.rebindSkin = true;
            if (!firstPerson)
            {
                object.animationProject = appearance.animationProject;
                object.creationMorphs = appearance.creationMorphs;
                bundle.emplace("body.json", std::format("{{\"scale\":{}}}", appearance.height));
            }
        }
        if (!weaponModel.empty() && vfs.exists(VFS::Path::Normalized(weaponModel)))
        {
            nodes.push_back(load(weaponModel));
            objects.push_back({"Weapon", "ViewWeapon", nodes.back().get()});
        }
        exportPreview(bundle, vfs, *resources.getNifFileManager(), read, {}, objects);
    }

    void exportRuntimeLocation(const CellScene& scene, const RuntimeExportOptions& options, const ReadWinningRecord& read,
        const WorldspaceInfo* lodWorld, int cellX, int cellY, PreviewBundle& bundle,
        RuntimeScriptBatch* scripts, std::span<const std::uint32_t> tracked)
    {
        Settings::SettingsFileParser parser;
        parser.loadSettingsFile(options.resources / "settings-default.cfg", Settings::Manager::mDefaultSettings);
        Settings::StaticValues::initDefaults();
        Settings::Manager::mUserSettings = Settings::Manager::mDefaultSettings;
        Settings::StaticValues::init();
        const auto encoder = std::make_shared<ToUTF8::StatelessUtf8Encoder>(ToUTF8::calculateEncoding("win1252"));
        const auto vfs = options.runtimeVfs ? options.runtimeVfs : mountRuntimeAssets(options.assetPlan);
        std::set<std::uint32_t> trackedReferences(tracked.begin(), tracked.end());
        if (scripts)
        {
            const auto diagnostic = [&](std::string_view context, std::string_view error) {
                scripts->errors.emplace_back(context, error);
            };
            scripts->vfs = vfs;
            scripts->cell = scene.cellId;
            scripts->worldspace = scene.worldspaceId;
            scripts->exteriorCells = scene.exteriorCells;
            std::map<std::uint32_t, bool> doorBases;
            for (const auto& [id, ref] : scene.exteriorReferences)
            {
                scripts->placements.emplace(id, WorldScriptPose{ ref.cellId, ref.position });
                if (ref.enableParentId)
                    scripts->enableParents.emplace(id, std::pair{ ref.enableParentId, ref.oppositeOfParent });
                if (ref.recordType == ESM::fourCC("ACHR") && ref.baseId)
                    scripts->actors.emplace(id, ref.baseId);
                else if (ref.baseId)
                {
                    const auto [base, added] = doorBases.try_emplace(ref.baseId);
                    if (added)
                        try { base->second = read(ref.baseId).metadata.recordType == ESM::fourCC("DOOR"); }
                        catch (const std::exception&) {}
                    if (base->second)
                    {
                        scripts->doors.emplace(id, ref.scale);
                        scripts->targets.insert(id);
                    }
                }
            }
            scripts->scripts = prepareSceneScripts(scene, *vfs, diagnostic, nullptr, questAliasResolver(read));
            for (const auto& entry : scripts->scripts)
            {
                scripts->targets.insert(entry.owner);
                for (const auto& [name, value] : entry.properties)
                    if (const auto* form = std::get_if<Papyrus::FormReference>(&value); form && form->formId)
                        scripts->targets.insert(form->formId);
                    else if (const auto* alias = std::get_if<Papyrus::AliasReference>(&value))
                        scripts->targets.insert(alias->questFormId);
            }
            collectQuestTargets(read, scripts->actors, scripts->targets, diagnostic);
            trackedReferences.insert(scripts->targets.begin(), scripts->targets.end());
            for (std::size_t previous = 0; previous != trackedReferences.size();)
            {
                previous = trackedReferences.size();
                for (const auto& [id, dependency] : scripts->enableParents)
                    if (trackedReferences.contains(dependency.first))
                        trackedReferences.insert(id);
            }
        }
        if (readRuntimeCache(options.runtimeCacheDirectory, options.runtimeCacheKey, bundle))
        {
            try
            {
                std::vector<std::pair<std::string, std::string>> textures;
                for (const auto& [name, source] : bundle)
                    if (name.starts_with("source:")) textures.emplace_back(name.substr(7), source);
                loadPhase("Reading cached texture sources", textures.size());
                for (const auto& [name, source] : textures)
                {
                    auto input = vfs->get(VFS::Path::Normalized(source));
                    std::ostringstream bytes; bytes << input->rdbuf();
                    if (!bytes) throw std::runtime_error("Cannot read cached texture source");
                    auto value = std::move(bytes).str();
                    loadTexture(source, value.size());
                    bundle.emplace(name, std::move(value)); bundle.erase("source:" + name);
                    loadCompleted();
                }
                if (scripts)
                {
                    auto disabled = scripts->initialDisabled;
                    auto bounds = scripts->actorBounds;
                    Papyrus::Checkpoint cache(bundle.at("runtime-metadata"));
                    cache(disabled, bounds);
                    if (!cache.atEnd()) throw std::runtime_error("Trailing derived metadata");
                    scripts->initialDisabled = std::move(disabled); scripts->actorBounds = std::move(bounds);
                }
                bundle.erase("runtime-metadata");
                return;
            }
            catch (...) { bundle.clear(); }
        }
        // Keep parsed NIFs/images and the preceding window's immutable static models
        // across neighboring loads. NPC weight blending still uses fresh instances.
        struct ResidentAssets
        {
            std::shared_ptr<VFS::Manager> vfs;
            std::shared_ptr<ToUTF8::StatelessUtf8Encoder> encoder;
            Resource::ResourceSystem resources;
            std::map<std::string, osg::ref_ptr<osg::Node>, std::less<>> models;
            std::map<std::string, ModelMechanics, std::less<>> mechanics;
            ResidentAssets(std::shared_ptr<VFS::Manager> files, std::shared_ptr<ToUTF8::StatelessUtf8Encoder> text)
                : vfs(std::move(files)), encoder(std::move(text)), resources(vfs.get(), 30., encoder.get())
            { resources.getNifFileManager()->setExpiryDelay(30.); }
        };
        static std::unique_ptr<ResidentAssets> resident;
        if (!resident || resident->vfs != vfs) resident = std::make_unique<ResidentAssets>(vfs, encoder);
        auto& resources = resident->resources;
        const auto cacheTime = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
        resources.updateCache(cacheTime());
        NifOsg::Loader::setShowMarkers(false);

        std::vector<osg::ref_ptr<osg::Node>> nodes;
        std::vector<PreviewObject> objects;
        std::set<std::pair<int, int>> landCells, exteriorCells;
        for (const auto& cell : scene.exteriorCells)
            exteriorCells.emplace(cell.x, cell.y);
        loadPhase("Building landscape tiles", scene.landTiles.size());
        for (const auto& tile : scene.landTiles)
        {
            loadItem(std::to_string(tile.cellId));
            nodes.push_back(makeLandscapeTile(tile));
            objects.push_back({std::to_string(tile.cellId), "Terrain", nodes.back().get(), &tile});
            landCells.emplace(tile.x, tile.y);
            loadCompleted();
        }
        std::map<std::string, osg::ref_ptr<osg::Node>, std::less<>> staticModels;
        std::map<std::string, ModelMechanics, std::less<>> modelMechanics;
        std::ostringstream mechanics;
        mechanics << '{';
        bool haveMechanics = false;
        std::set<std::string, std::less<>> collisionModels;
        std::ostringstream collisionInstances;
        collisionInstances << std::setprecision(9) << '[';
        bool haveCollision = false;
        std::set<std::string, std::less<>> failed;
        const auto model = [&](std::string_view path) {
            loadItem(path);
            const auto file = resources.getNifFileManager()->get(VFS::Path::Normalized(path));
            auto node = NifOsg::Loader::load(*file, resources.getImageManager(), resources.getBgsmFileManager());
            node->setUserValue("helgenSourceNif", std::string(path));
            return node;
        };
        const auto mechanicsFor = [&](const std::string& path, bool skeleton) -> const ModelMechanics& {
            const auto key = skeleton ? "rig:" + path : path;
            if (const auto found = modelMechanics.find(key); found != modelMechanics.end())
                return found->second;
            auto found = resident->mechanics.find(key);
            ModelMechanics data;
            if (found != resident->mechanics.end())
                data = found->second;
            else
            {
                const auto file = resources.getNifFileManager()->get(VFS::Path::Normalized(path));
                std::set<std::string> clipNodes;
                if (!skeleton)
                    for (const auto& record : file->mRecords)
                        if (const auto* graph = dynamic_cast<const Nif::BSBehaviorGraphExtraData*>(record.get());
                            graph && !graph->mFile.empty())
                        {
                            const auto readHkx = [&](const std::string& resource) {
                                const auto stream = vfs->get(VFS::Path::Normalized(resource));
                                return std::string(std::istreambuf_iterator<char>(*stream), {});
                            };
                            try
                            {
                                const auto rig = hkxObjectRig(modelAssetPath(graph->mFile), readHkx);
                                if (!rig.empty())
                                {
                                    const auto names = hkxObjectBones(readHkx(rig));
                                    clipNodes.insert(names.begin(), names.end());
                                }
                            }
                            catch (const std::exception&)
                            {
                                // The base model and collision are usable without its optional HKX project.
                            }
                        }
                data = readModelMechanics(*file, skeleton, clipNodes);
            }
            if (!data.collision.empty())
                collisionModels.insert(key);
            return modelMechanics.emplace(key, std::move(data)).first->second;
        };
        const auto collisionPlacement = [&](const std::string& modelPath, const osg::Matrix& matrix,
                                            std::uint32_t reference, bool skeleton) {
            const auto p = matrix.getTrans();
            const auto q = matrix.getRotate();
            collisionInstances << (haveCollision ? "," : "") << "{\"model\":" << std::quoted(modelPath)
                << ",\"ref\":" << reference << ",\"pose\":[" << p.x() << ',' << p.y() << ',' << p.z() << ','
                << q.x() << ',' << q.y() << ',' << q.z() << ',' << q.w() << ',' << matrix.getScale().x()
                << "],\"skeleton\":" << skeleton << '}';
            haveCollision = true;
        };
        const auto addStatic = [&](const std::string& path, std::string name, const char* group,
                                   const osg::Matrix& matrix, const std::set<std::pair<int, int>>* replaced = nullptr,
                                   std::uint32_t reference = 0, bool render = true) {
            if (failed.contains(path))
                return;
            try
            {
                const auto* data = replaced || std::string_view(group) == "Archery" ? nullptr : &mechanicsFor(path, false);
                if (data && !data->collision.empty())
                    collisionPlacement(path, matrix, reference, false);
                if (!render)
                    return;
                auto& cached = staticModels[path];
                if (!cached)
                    cached = resident->models.contains(path) ? resident->models.at(path) : model(path);
                auto node = osg::ref_ptr(new osg::MatrixTransform(matrix));
                node->addChild(cached);
                nodes.push_back(node);
                objects.push_back({std::move(name), group, node.get(), nullptr, replaced, replaced == nullptr, {}, reference});
                if (reference && data)
                    objects.back().animatedNodes = data->animatedNodes;
                if (reference && data && !data->json.empty())
                {
                    if (haveMechanics)
                        mechanics << ',';
                    haveMechanics = true;
                    mechanics << '"' << reference << "\":{\"scale\":" << matrix.getScale().x()
                              << ",\"model\":" << data->json << '}';
                }
            }
            catch (const std::exception& error)
            {
                failed.insert(path);
                std::cerr << path << ": " << error.what() << '\n';
            }
        };
        std::optional<ESM::Position> heading;
        loadPhase("Reading placed NIF models", scene.placements.size());
        for (const auto& placement : scene.placements)
        {
            struct Completion { ~Completion() { loadCompleted(); } } completion;
            const auto path = modelAssetPath(placement.model);
            if (path.ends_with("markercocheading.nif") && !heading && !placement.initiallyDisabled)
                heading = placement.position;
            const bool trackedPlacement = trackedReferences.contains(placement.referenceId) || (scripts && placement.lootable);
            if (placement.initiallyDisabled && !trackedPlacement)
                continue;
            const auto matrix = osg::Matrix::scale(placement.scale, placement.scale, placement.scale)
                * osg::Matrix::rotate(Misc::Convert::makeOsgQuat(placement.position))
                * osg::Matrix::translate(placement.position.asVec3());
            addStatic(path, std::to_string(placement.referenceId), path.starts_with("meshes/architecture/") ? "Architecture" : "Props",
                matrix, nullptr, trackedPlacement ? placement.referenceId : 0,
                !marker(path) && !path.starts_with("meshes/effects/"));
        }

        std::map<std::uint32_t, std::optional<bool>> visibility;
        const auto enabled = [&](const auto& self, std::uint32_t id) -> std::optional<bool> {
            if (const auto cached = visibility.find(id); cached != visibility.end())
                return cached->second;
            const auto reference = scene.exteriorReferences.find(id);
            if (reference == scene.exteriorReferences.end())
                return std::nullopt;
            auto [entry, inserted] = visibility.emplace(id, std::nullopt);
            const auto& value = reference->second;
            if (!value.enableParentId)
                entry->second = !value.initiallyDisabled;
            else if (const auto parent = self(self, value.enableParentId))
                entry->second = *parent != value.oppositeOfParent;
            return entry->second;
        };
        std::ostringstream lights;
        lights << '[';
        bool hasLights = false;
        std::ostringstream corpses;
        corpses << std::setprecision(9) << '{';
        const char* corpseComma = "";
        std::set<std::string> weaponModels;
        if (scripts && options.projectileModels)
            for (const auto& [id, model] : *options.projectileModels)
            {
                const auto path = modelAssetPath(model);
                if (path.empty() || !vfs->exists(VFS::Path::Normalized(path)) || !weaponModels.insert(path).second)
                    continue;
                addStatic(path, path, "Archery", osg::Matrix::identity());
            }
        loadPhase("Resolving actors and references", scene.exteriorReferences.size());
        for (const auto& [id, ref] : scene.exteriorReferences)
        {
            loadItem(std::to_string(id));
            struct Completion { ~Completion() { loadCompleted(); } } completion;
            const bool authoredDisabled = !enabled(enabled, id).value_or(false);
            const auto trackedId = trackedReferences.contains(id) ? id : 0;
            if (scripts)
                scripts->initialDisabled.emplace(id, authoredDisabled);
            if ((authoredDisabled && !trackedId) || !ref.baseId || ref.scale <= 0)
                continue;
            try
            {
                if (ref.recordType == ESM::fourCC("ACHR"))
                {
                    if (scripts && ref.initiallyDead)
                    {
                        const auto record = read(id);
                        corpses << corpseComma << '"' << id << "\":[";
                        corpseComma = ",";
                        visitSubrecords(record, [&](auto tag, auto bytes) {
                            constexpr std::size_t stride = 28;
                            if (tag != ESM::fourCC("XRGD") || bytes.empty() || bytes.size() % stride) return;
                            const char* comma = "";
                            for (std::size_t at = 0; at < bytes.size(); at += stride)
                            {
                                std::array<float, 6> pose;
                                std::memcpy(pose.data(), bytes.data() + at + 4, sizeof(pose));
                                if (!std::ranges::all_of(pose, [](float v) { return std::isfinite(v); })) break;
                                corpses << comma << '[' << pose[0] << ',' << pose[1] << ',' << pose[2] << ','
                                    << pose[3] << ',' << pose[4] << ',' << pose[5] << ']';
                                comma = ",";
                            }
                        });
                        corpses << ']';
                    }
                    try
                    {
                        if (const auto equipment = readArrowLoadout(read, ref.baseId))
                            for (const auto& path : {equipment->bowModel, equipment->ammoModel, equipment->projectileModel})
                                if (!path.empty() && weaponModels.insert(path).second)
                                {
                                    const auto before = objects.size();
                                    addStatic(path, path, "Archery", osg::Matrix::identity());
                                    if (objects.size() == before)
                                        continue;
                                    auto& asset = objects.back();
                                    asset.directShapesOnly = path != equipment->bowModel;
                                    if (asset.directShapesOnly)
                                        continue;
                                    const auto weaponNif = resources.getNifFileManager()->get(VFS::Path::Normalized(path));
                                    for (const auto& record : weaponNif->mRecords)
                                        if (const auto* graph = dynamic_cast<const Nif::BSBehaviorGraphExtraData*>(record.get()))
                                        {
                                            asset.animationProject = modelAssetPath(graph->mFile);
                                            break;
                                        }
                                }
                    }
                    catch (const std::exception& error)
                    {
                        std::cerr << "Archery assets " << ref.baseId << ": " << error.what() << '\n';
                    }
                    const auto appearance = loadActorAppearance(read, *vfs, scene.pluginLoadOrder, ref.baseId, {});
                    if (appearance.models.empty())
                        continue;
                    const double scale = ref.scale * appearance.height;
                    const auto matrix = osg::Matrix::scale(scale, scale, scale)
                        * osg::Matrix::rotate(Misc::Convert::makeOsgQuat(ref.position)) * osg::Matrix::translate(ref.position.asVec3());
                    auto node = osg::ref_ptr(new osg::MatrixTransform(matrix));
                    if (appearance.skinTone)
                        node->setUserValue("helgenSkinTone", *appearance.skinTone);
                    for (const auto& path : appearance.models)
                        try
                        {
                            auto mesh = model(path);
                            if (const auto blend = appearance.weightBlends.find(path); blend != appearance.weightBlends.end())
                            {
                                auto other = model(blend->second.otherModel);
                                blendWeightModels(*mesh, *other, blend->second.fraction);
                            }
                            node->addChild(mesh);
                        }
                        catch (const std::exception& error)
                        {
                            std::cerr << path << ": " << error.what() << '\n';
                        }
                    nodes.push_back(node);
                    objects.push_back({appearance.editorId + '_' + std::to_string(id), "NPC", node.get(),
                        nullptr, nullptr, true, appearance.skeleton, id});
                    objects.back().animationProject = appearance.animationProject;
                    objects.back().expressionMorphs = appearance.expressionMorphs;
                    objects.back().skinTextures = appearance.skinTextures;
                    if (appearance.editorId == "T54UEPlayer") objects.back().creationMorphs = appearance.creationMorphs;
                    if (vfs->exists(VFS::Path::Normalized(appearance.skeleton)))
                    {
                        const auto& skeleton = mechanicsFor(appearance.skeleton, true);
                        if (!skeleton.collision.empty())
                            collisionPlacement("rig:" + appearance.skeleton, matrix, id, true);
                        if (scripts && skeleton.actorBounds.valid())
                            scripts->actorBounds.emplace(id, osg::BoundingBoxd(
                                skeleton.actorBounds._min * scale, skeleton.actorBounds._max * scale));
                    }
                }
                else if (scene.cellId)
                {
                    const auto base = read(ref.baseId);
                    if (base.metadata.recordType != ESM::fourCC("LIGH"))
                        continue;
                    std::uint32_t radius = 0, flags = 0;
                    std::array<unsigned, 3> color{};
                    float fade = 1.f;
                    visitSubrecords(base, [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("DATA") && bytes.size() >= 16)
                        {
                            std::memcpy(&radius, bytes.data() + 4, 4);
                            std::memcpy(&flags, bytes.data() + 12, 4);
                            for (unsigned channel = 0; channel < color.size(); ++channel)
                                color[channel] = std::to_integer<unsigned>(bytes[8 + channel]);
                        }
                        else if (tag == ESM::fourCC("FNAM") && bytes.size() == 4)
                            std::memcpy(&fade, bytes.data(), 4);
                    });
                    if (radius && (flags & (0x04 | 0x20)) == 0 && std::isfinite(fade))
                    {
                        lights << (hasLights ? "," : "") << '[' << ref.position.pos[0] << ',' << ref.position.pos[1]
                               << ',' << ref.position.pos[2] << ',' << radius << ',' << color[0] << ',' << color[1]
                               << ',' << color[2] << ',' << fade << ',' << flags << ',' << trackedId << ']';
                        hasLights = true;
                    }
                }
            }
            catch (const std::exception& error)
            {
                std::cerr << id << ": " << error.what() << '\n';
            }
        }
        lights << ']';

        if (lodWorld && !lodWorld->editorId.empty())
        {
            const std::string name = VFS::Path::normalizeFilename(lodWorld->editorId);
            const std::string root = "meshes/terrain/" + name + '/';
            const int tileX = static_cast<int>(std::floor(cellX / 4.0)) * 4;
            const int tileY = static_cast<int>(std::floor(cellY / 4.0)) * 4;
            for (int y = tileY - 8; y <= tileY + 8; y += 4)
                for (int x = tileX - 8; x <= tileX + 8; x += 4)
                {
                    const auto tile = name + ".4." + std::to_string(x) + '.' + std::to_string(y);
                    const auto matrix = osg::Matrix::translate(double(x) * LandTile::size, double(y) * LandTile::size, 0.);
                    const auto terrain = root + tile + ".btr";
                    if (vfs->exists(VFS::Path::Normalized(terrain)))
                        addStatic(terrain, tile, "LOD", matrix, &landCells);
                    const auto object = root + "objects/" + tile + ".bto";
                    if (vfs->exists(VFS::Path::Normalized(object)))
                        addStatic(object, tile, "LOD", matrix, &exteriorCells);
                }
        }

        osg::Vec3d spawn;
        if (scene.worldspaceId)
        {
            double height = 0;
            for (const auto& tile : scene.landTiles)
                if (tile.x == cellX && tile.y == cellY)
                    height = tile.heights[tile.heights.size() / 2];
            spawn.set((cellX + .5) * LandTile::size, (cellY + .5) * LandTile::size, height + 200.);
        }
        else if (heading)
            spawn = osg::Vec3d(heading->asVec3()) + osg::Vec3d(0, 0, 92.);
        else if (!nodes.empty())
        {
            osg::ComputeBoundsVisitor bounds;
            for (const auto& node : nodes)
                node->accept(bounds);
            if (const auto& box = bounds.getBoundingBox(); box.valid())
                spawn = osg::Vec3d(box.center().x(), box.center().y(), box.zMin() + 128.);
        }
        std::ostringstream location;
        location << "{\"interior\":" << (scene.cellId != 0) << ",\"spawn\":[" << spawn.x() << ',' << spawn.y() << ','
                 << spawn.z() << "],\"yaw\":" << (heading ? -osg::RadiansToDegrees(heading->rot[2]) : 0.)
                 << ",\"lights\":" << std::move(lights).str();
        if (scripts)
        {
            location << ",\"loot\":[";
            const char* separator = "";
            for (const auto& placement : scene.placements)
                if (placement.lootable)
                {
                    location << separator << placement.referenceId;
                    separator = ",";
                }
            location << "],\"corpses\":" << std::move(corpses).str() << '}';
        }
        writeLocationLighting(location, scene, read, cellX, cellY);
        location << '}';
        bundle.emplace("location.json", std::move(location).str());
        mechanics << '}';
        bundle.emplace("mechanics.json", std::move(mechanics).str());
        collisionInstances << ']';
        std::ostringstream collision;
        collision << "{\"models\":{";
        bool modelComma = false;
        for (const auto& path : collisionModels)
        {
            collision << (modelComma ? "," : "") << std::quoted(path) << ':' << modelMechanics.at(path).collision;
            modelComma = true;
        }
        collision << "},\"ragdolls\":{";
        const char* ragComma = "";
        for (const auto& path : collisionModels)
            if (const auto& ragdoll = modelMechanics.at(path).ragdoll; !ragdoll.empty())
            {
                collision << ragComma << std::quoted(path) << ':' << ragdoll;
                ragComma = ",";
            }
        collision << "},\"instances\":" << std::move(collisionInstances).str() << '}';
        bundle.emplace("collision.json", std::move(collision).str());
        exportPreview(bundle, *vfs, *resources.getNifFileManager(), read, {}, objects);
        resident->models = std::move(staticModels);
        resident->mechanics = std::move(modelMechanics);
        resources.updateCache(cacheTime());
        if (!options.runtimeCacheKey.empty())
        {
            if (scripts)
            {
                Papyrus::Checkpoint cache;
                cache(scripts->initialDisabled, scripts->actorBounds);
                bundle.emplace("runtime-metadata", cache.bytes());
            }
            writeRuntimeCache(options.runtimeCacheDirectory, options.runtimeCacheKey, bundle);
            bundle.erase("runtime-metadata");
        }
        std::erase_if(bundle, [](const auto& entry) { return entry.first.starts_with("source:"); });
    }
}
