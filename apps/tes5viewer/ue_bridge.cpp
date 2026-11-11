#include <random>
#include <components/misc/mathutil.hpp>
#include "runtimeexport.hpp"
#include "runtimecache.hpp"
#include "loadprogress.hpp"
#include "audiodecode.hpp"
#include "pluginlist.hpp"
#include "questscripts.hpp"
#include "worldscripts.hpp"
#include "checkpointtypes.hpp"
#include <format>
#include "charactercreation.hpp"
#include "actorappearance.hpp"
#include "actorinventory.hpp"
#include "archery.hpp"
#include "modelmechanics.hpp"
#include "presentationstate.hpp"
#include "weather.hpp"
#include "hkxmovement.hpp"
#include "../papyrustool/errorlog.hpp"

#include <components/esm/fourcc.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/constants.hpp>
#include <components/nif/controller.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/niffile.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/settings/settings.hpp>
#include <components/vfs/manager.hpp>

#include <osgDB/Registry>

#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <sstream>

#ifdef _WIN32
#define T5_BRIDGE_API extern "C" __declspec(dllexport)
#else
#define T5_BRIDGE_API extern "C" __attribute__((visibility("default")))
#endif

namespace
{
    constexpr std::uint32_t raceMenuPending = 1u << 6;
    struct LoadResult
    {
        TES5Viewer::PreviewBundle buffers;
        TES5Viewer::RuntimeScriptBatch scripts;
        std::shared_ptr<const std::vector<TES5Viewer::ExteriorCell>> worldCells;
        TES5Viewer::ReadWinningRecord read;
        std::shared_ptr<const TES5Viewer::WinningRecordIndex> index;
        std::shared_ptr<const VFS::Manager> baseAnimations;
        std::optional<TES5Viewer::WorldScriptPose> initialPlayer;
        std::uint32_t openingQuest = 0;
        std::uint64_t revision = 0;
        std::string error;
        osg::Vec3f cartHorseOffset{0.f, 200.f, 0.f};
    };

    std::mutex gLoadMutex;
    std::filesystem::path gCacheDirectory;

    struct ClearSettings
    {
        ~ClearSettings() { Settings::Manager::clear(); }
    };

    struct CatalogCache
    {
        std::vector<std::filesystem::path> paths;
        std::vector<std::filesystem::file_time_type> dates;
        std::vector<std::uintmax_t> sizes;
        TES5Viewer::LocationCatalog catalog;
        std::map<std::uint32_t, std::shared_ptr<const std::vector<TES5Viewer::ExteriorCell>>> worldCells;
        TES5Viewer::ReadWinningRecord read;
        std::shared_ptr<const TES5Viewer::WinningRecordIndex> index;
        std::uint64_t revision = 0;
        std::filesystem::path mountedData;
        std::filesystem::file_time_type mountedDate;
        std::shared_ptr<VFS::Manager> mountedAssets;
        std::shared_ptr<VFS::Manager> baseAnimations;
        std::string mountedKey;
        using SceneKey = std::tuple<std::uint32_t, int, int, int, bool>;
        std::map<SceneKey, TES5Viewer::CellScene> scenes;
        std::vector<SceneKey> sceneOrder;
    };
    CatalogCache gLocations;

    const TES5Viewer::LocationCatalog& locations(const std::vector<std::filesystem::path>& paths)
    {
        std::vector<std::filesystem::file_time_type> dates;
        std::vector<std::uintmax_t> sizes;
        for (const auto& path : paths)
        {
            dates.push_back(std::filesystem::last_write_time(path));
            sizes.push_back(std::filesystem::file_size(path));
        }
        if (paths != gLocations.paths || dates != gLocations.dates || sizes != gLocations.sizes)
        {
            std::size_t hash = 1;
            for (std::size_t i = 0; i < paths.size(); ++i)
            {
                hash = hash * 31 + std::filesystem::hash_value(paths[i]);
                hash = hash * 31 + static_cast<std::size_t>(dates[i].time_since_epoch().count());
            }
            CatalogCache next;
            next.paths = paths; next.dates = std::move(dates); next.sizes = std::move(sizes);
            next.catalog = TES5Viewer::loadLocationCatalog(paths); next.revision = hash ? hash : 1;
            gLocations = std::move(next);
        }
        return gLocations.catalog;
    }

    void indexLocations(const std::vector<std::filesystem::path>& paths)
    {
        if (!gLocations.read)
        {
            TES5Viewer::loadPhase("Indexing winning records", paths.size());
            auto index = std::make_shared<TES5Viewer::WinningRecordIndex>();
            gLocations.read = TES5Viewer::indexWinningRecords(paths, index.get());
            gLocations.index = std::move(index);
        }
    }

    std::optional<ESM::Position> authoredPose(const TES5Viewer::RecordPayload& record)
    {
        std::optional<ESM::Position> pose;
        TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
            if (tag == ESM::fourCC("DATA") && bytes.size() == sizeof(ESM::Position))
                std::memcpy(&pose.emplace(), bytes.data(), bytes.size());
        });
        return pose;
    }

    std::uint32_t playerStartMarker(const TES5Viewer::ReadWinningRecord& read, std::uint32_t quest)
    {
        const auto record = read(quest);
        if (record.metadata.recordType != ESM::fourCC("QUST"))
            return 0;
        bool playerStart = false;
        std::uint32_t marker = 0;
        TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
            if (tag == ESM::fourCC("ALID") && !bytes.empty())
            {
                const auto text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1);
                playerStart = Misc::StringUtils::ciEqual(text, "PlayerStartMarker");
            }
            else if (tag == ESM::fourCC("ALFR") && playerStart && bytes.size() == 4)
            {
                std::uint32_t local;
                std::memcpy(&local, bytes.data(), 4);
                marker = record.mapFormId(local);
            }
            else if (tag == ESM::fourCC("ALED"))
                playerStart = false;
        });
        return marker;
    }

    struct SpawnedReference
    {
        std::uint32_t id, base, anchor, flags, cell;
        ESM::Position position;
        float scale = 1.f;
        void checkpoint(Papyrus::Checkpoint& ar) { ar(id, base, anchor, flags, cell, position, scale); }
    };

    template <class Action>
    void* performLoad(Action&& action) noexcept
    {
        try
        {
            auto result = std::make_unique<LoadResult>();
            try
            {
                std::lock_guard lock(gLoadMutex);
                action(*result);
            }
            catch (const std::exception& error)
            {
                result->error = error.what();
            }
            return result.release();
        }
        catch (...)
        {
            return nullptr;
        }
    }

    void configureOsg(const char* directory)
    {
        auto& directories = osgDB::Registry::instance()->getLibraryFilePathList();
        if (std::ranges::find(directories, directory) == directories.end())
            directories.insert(directories.begin(), directory);
    }

    std::string jsonText(std::string_view value)
    {
        std::string text(1, '"');
        for (unsigned char byte : value)
        {
            if (byte < 32)
                text += ' ';
            else
            {
                if (byte == '"' || byte == '\\')
                    text += '\\';
                text += static_cast<char>(byte);
            }
        }
        text += '"';
        return text;
    }

    void* loadLocation(const char* data, const char* pluginList, const char* resources, const char* osgPlugins,
        std::uint32_t id, int x, int y, int radius, bool playable, std::uint64_t revision,
        std::span<const std::uint32_t> tracked, std::uint32_t newGame = 0,
        std::span<const SpawnedReference> spawned = {}) noexcept
    {
        return performLoad([&](LoadResult& result) {
            if (radius < 0 || radius > 3 || x < -32768 || x > 32767 || y < -32768 || y > 32767)
                throw std::runtime_error("Invalid T5 exterior grid or load radius");
            ClearSettings cleanup;
            TES5Viewer::RuntimeExportOptions options;
            options.data = Files::pathFromUnicodeString(data);
            // Use the source game's Cart settings, then profile overrides beside plugins.txt.
            const auto trim = [](std::string value) {
                const auto first = value.find_first_not_of(" \t\r\n");
                return first == std::string::npos ? std::string{}
                    : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
            };
            for (const auto& ini : {options.data.parent_path() / "Skyrim.ini",
                     options.data.parent_path() / "SkyrimCustom.ini",
                     Files::pathFromUnicodeString(pluginList).parent_path() / "T5.ini"})
            {
                std::ifstream input(ini);
                bool cart = false;
                for (std::string line; std::getline(input, line);)
                {
                    line = trim(line.substr(0, line.find(';')));
                    if (line.starts_with('['))
                        cart = Misc::StringUtils::ciEqual(line, "[Cart]");
                    if (!cart)
                        continue;
                    const auto eq = line.find('=');
                    if (eq == std::string::npos)
                        continue;
                    const auto key = trim(line.substr(0, eq));
                    constexpr std::array keys{"fHorseOffsetX", "fHorseOffsetY", "fHorseOffsetZ"};
                    for (unsigned axis = 0; axis < keys.size(); ++axis)
                        if (Misc::StringUtils::ciEqual(key, keys[axis]))
                        {
                            const float value = std::stof(trim(line.substr(eq + 1)));
                            if (!std::isfinite(value))
                                throw std::runtime_error("Non-finite [Cart] horse offset");
                            result.cartHorseOffset[axis] = value;
                        }
                }
            }
            options.resources = Files::pathFromUnicodeString(resources);
            const auto paths = TES5Viewer::loadPluginList(Files::pathFromUnicodeString(pluginList), options.data);
            const auto& catalog = locations(paths);
            const auto world = catalog.worlds.find(id);
            if (world == catalog.worlds.end()
                && std::ranges::find(catalog.interiors, id, &TES5Viewer::InteriorInfo::formId) == catalog.interiors.end())
                throw std::runtime_error("The selected WRLD or interior CELL is absent from the active plugin list");
            const CatalogCache::SceneKey sceneKey{id, x, y, radius, playable};
            auto cachedScene = gLocations.scenes.find(sceneKey);
            if (cachedScene == gLocations.scenes.end())
            {
                auto parsed = world == catalog.worlds.end() ? TES5Viewer::loadRuntimeInterior(paths, id, playable)
                    : TES5Viewer::loadRuntimeExterior(paths, catalog, id, x - radius, y - radius, x + radius, y + radius, playable);
                while (gLocations.scenes.size() >= 8)
                {
                    gLocations.scenes.erase(gLocations.sceneOrder.front());
                    gLocations.sceneOrder.erase(gLocations.sceneOrder.begin());
                }
                cachedScene = gLocations.scenes.emplace(sceneKey, std::move(parsed)).first;
            }
            std::erase(gLocations.sceneOrder, sceneKey); gLocations.sceneOrder.push_back(sceneKey);
            // Spawn/move overlays must never mutate the cached authored scene.
            auto scene = cachedScene->second;
            const auto* lod = world == catalog.worlds.end() ? nullptr
                : &catalog.inherited(world->second, TES5Viewer::WorldspaceInfo::useLod);
            indexLocations(paths);
            std::set<std::uint32_t> targets;
            if (playable)
            {
                if (world != catalog.worlds.end())
                {
                    auto& cells = gLocations.worldCells[id];
                    if (!cells)
                        cells = std::make_shared<const std::vector<TES5Viewer::ExteriorCell>>(world->second.cells);
                    result.worldCells = cells;
                }
                result.revision = gLocations.revision;
                result.read = gLocations.read;
                result.index = gLocations.index;
                result.openingQuest = newGame;
                if (revision == result.revision)
                    targets.insert(tracked.begin(), tracked.end());
                if (newGame)
                {
                    targets.insert(newGame);
                    if (const auto marker = playerStartMarker(result.read, newGame); marker)
                        if (const auto placement = scene.exteriorReferences.find(marker); placement != scene.exteriorReferences.end())
                            result.initialPlayer = TES5Viewer::WorldScriptPose{ placement->second.cellId, placement->second.position };
                    std::map<std::uint32_t, std::uint32_t> actors;
                    for (const auto& [refId, ref] : scene.exteriorReferences)
                        if (ref.recordType == ESM::fourCC("ACHR"))
                            actors.emplace(refId, ref.baseId);
                    TES5Viewer::collectQuestTargets(result.read, actors, targets, [&](auto context, auto error) {
                        result.scripts.errors.emplace_back(context, error);
                    });
                }
                for (const auto& actor : revision == result.revision ? spawned : std::span<const SpawnedReference>{})
                {
                    if (result.index->explosions.contains(actor.base))
                        continue;
                    const auto placement = scene.exteriorReferences.find(actor.anchor);
                    const auto authored = scene.exteriorReferences.find(actor.id);
                    const auto inLocation = [&](std::uint32_t cell) {
                        return scene.cellId ? cell == scene.cellId
                            : std::ranges::find(scene.exteriorCells, cell, &TES5Viewer::ExteriorCell::cellId) != scene.exteriorCells.end();
                    };
                    const bool hasDestination = (actor.flags & 4) && actor.cell;
                    const auto sourceWorld = [&](std::uint32_t ref) {
                        const auto found = result.index->references.find(ref);
                        return found == result.index->references.end() ? 0u : found->second.world;
                    };
                    // Snapshot loads can finish after a moving actor has crossed a cell border.
                    // Keep references around the visible cells so companions don't blink out.
                    const bool nearExterior = !scene.cellId && hasDestination
                        && (sourceWorld(actor.anchor) == id || (!actor.anchor && sourceWorld(actor.id) == id))
                        && std::abs(std::floor(actor.position.pos[0] / TES5Viewer::LandTile::size) - x) <= radius + 1
                        && std::abs(std::floor(actor.position.pos[1] / TES5Viewer::LandTile::size) - y) <= radius + 1;
                    const bool movedHere = hasDestination && (inLocation(actor.cell) || nearExterior);
                    if (hasDestination && !movedHere)
                    {
                        if (authored != scene.exteriorReferences.end())
                        {
                            scene.exteriorReferences.erase(authored);
                            std::erase_if(scene.placements, [&](const auto& entry) { return entry.referenceId == actor.id; });
                        }
                        continue;
                    }
                    if (authored != scene.exteriorReferences.end())
                    {
                        targets.insert(actor.id);
                        continue;
                    }
                    if (!movedHere && placement == scene.exteriorReferences.end())
                        continue;
                    TES5Viewer::ExteriorReference ref;
                    if (movedHere)
                    {
                        ref.cellId = actor.cell;
                        ref.ownerCellId = actor.cell;
                        ref.position = actor.position;
                    }
                    else
                    {
                        ref.cellId = placement->second.cellId;
                        ref.ownerCellId = placement->second.ownerCellId;
                        ref.position = placement->second.position;
                    }
                    ref.referenceId = actor.id;
                    ref.baseId = actor.base;
                    ref.recordType = (actor.flags & 1) ? ESM::fourCC("ACHR") : ESM::fourCC("REFR");
                    ref.initiallyDisabled = (actor.flags & 2) != 0;
                    ref.enableParentId = 0;
                    ref.scale = actor.scale;
                    scene.exteriorReferences.emplace(actor.id, ref);
                    if (ref.recordType == ESM::fourCC("REFR"))
                        TES5Viewer::addRuntimeStaticPlacement(scene, ref, result.read);
                    targets.insert(actor.id);
                }
            }
            TES5Viewer::loadPhase("Validating resource cache");
            const auto sourceKey = TES5Viewer::runtimeSourceKey(options.data, options.resources, paths);
            const auto dataDate = std::filesystem::last_write_time(options.data);
            if (!gLocations.mountedAssets || gLocations.mountedKey != sourceKey)
            {
                const auto plan = TES5Viewer::buildAssetPlan(options.data, paths, {}, {}, false);
                gLocations.mountedAssets = TES5Viewer::mountRuntimeAssets(plan);
                TES5Viewer::AssetPlan base;
                for (const auto& archive : plan.archives)
                    if (Misc::StringUtils::ciEqual(Files::pathToUnicodeString(archive.path.filename()), "Skyrim - Animations.bsa"))
                        base.archives.push_back(archive);
                gLocations.baseAnimations = base.archives.empty() ? nullptr : TES5Viewer::mountRuntimeAssets(base);
                gLocations.mountedData = options.data;
                gLocations.mountedDate = dataDate;
                gLocations.mountedKey = sourceKey;
            }
            options.runtimeVfs = gLocations.mountedAssets;
            options.projectileModels = &gLocations.index->projectileModels;
            result.baseAnimations = gLocations.baseAnimations;
            configureOsg(osgPlugins);
            const std::vector<std::uint32_t> trackedIds(targets.begin(), targets.end());
            options.runtimeCacheDirectory = gCacheDirectory;
            if (!gCacheDirectory.empty())
            {
                Papyrus::Checkpoint key;
                auto ids = trackedIds;
                std::vector<SpawnedReference> overlay;
                if (revision == result.revision) overlay.assign(spawned.begin(), spawned.end());
                key(id, x, y, radius, playable, ids, overlay);
                options.runtimeCacheKey = sourceKey + key.bytes();
            }
            TES5Viewer::exportRuntimeLocation(scene, options, gLocations.read, lod, x, y, result.buffers,
                playable ? &result.scripts : nullptr, trackedIds);
            result.buffers.emplace("resource-revision", TES5Viewer::runtimeCacheName(sourceKey));
        });
    }

    struct PapyrusState
    {
        std::uint64_t revision;
        std::optional<TES5Viewer::CharacterChoice> character;
        std::optional<TES5Viewer::RecordPayload> characterRecordCache;
        std::optional<float> playerEyeHeight;
        std::optional<osg::BoundingBoxd> playerTriggerBounds;
        std::string characterName, characterMorphs = "{}";
        std::shared_ptr<const TES5Viewer::WinningRecordIndex> characterIndex;
        TES5Viewer::ReadWinningRecord originalRead;
        TES5Viewer::ReadWinningRecord read;
        std::shared_ptr<const VFS::Manager> vfs;
        std::shared_ptr<const VFS::Manager> baseAnimations;
        TES5Viewer::ScenePapyrusReport report;
        std::map<std::uint32_t, bool> overrides;
        std::map<std::uint32_t, std::pair<std::uint32_t, bool>> enableParents;
        std::set<std::uint32_t> tracked;
        TES5Viewer::QuestScripts quests;
        TES5Viewer::WorldScripts world;
        TES5Viewer::SceneScripts scripts;
        Papyrus::ErrorLog& log;
        bool startedGame = false;
        std::uint32_t controls = 0;
        std::uint32_t gameFlags = 1;
        float sittingRotation = 0.f;
        std::map<std::uint32_t, std::set<std::uint32_t>> outfits;
        std::map<std::uint32_t, SpawnedReference> relocated;
        std::map<std::uint32_t, std::pair<bool, double>> doorStates;
        std::map<std::uint32_t, std::pair<std::int32_t, bool>> locks;
        std::map<std::pair<std::uint32_t, std::uint32_t>, float> locationData;
        std::uint32_t randomState = 0x6A09E667;
        std::uint32_t nextMagicHandle = 0xffffffff;
        struct Cast
        {
            std::uint32_t spell = 0, source = 0, caster = 0, target = 0;
            void checkpoint(Papyrus::Checkpoint& ar) { ar(spell, source, caster, target); }
        };
        struct MagicFlight
        {
            std::uint32_t caster = 0, spell = 0, effect = 0, projectile = 0, duration = 0, area = 0, expectedTarget = 0;
            double expires = 0;
            bool cone = false;
            void checkpoint(Papyrus::Checkpoint& ar)
            { ar(caster, spell, effect, projectile, duration, area, expectedTarget, expires, cone); }
        };
        std::vector<Cast> spellQueue;
        std::map<std::uint32_t, MagicFlight> magicFlights;
        std::map<std::uint32_t, std::vector<TES5Viewer::PreparedSceneScript>> magicScripts;
        struct ObjectModel
        {
            std::string graph;
            std::string rig;
            std::map<std::string, float> sequences;
            std::map<std::string, TES5Viewer::HkxObjectAnimation> events;
        };
        struct ModelAnimation
        {
            std::string sequence;
            double started = 0;
            float duration = 0;
            std::vector<std::pair<float, std::string>> events;
            void checkpoint(Papyrus::Checkpoint& ar) { ar(sequence, started, duration, events); }
        };
        std::map<std::uint32_t, ObjectModel> objectModels;
        std::map<std::uint32_t, ModelAnimation> modelAnimations;
        struct ModelClip
        {
            std::string rig;
            float start = 0, speed = 1;
            void checkpoint(Papyrus::Checkpoint& ar) { ar(rig, start, speed); }
        };
        struct ModelAnimationWait
        {
            std::string event;
            std::shared_ptr<bool> finished = std::make_shared<bool>(false);
            std::shared_ptr<Papyrus::Value> result = std::make_shared<Papyrus::Value>(false);
            void checkpoint(Papyrus::Checkpoint& ar) { ar(event, finished, result); }
        };
        std::map<std::uint32_t, ModelClip> modelClips;
        std::map<std::uint32_t, std::string> modelSequenceLayers;
        std::map<std::uint32_t, ModelAnimationWait> modelAnimationWaits;
        std::set<std::uint32_t> noFavorAllowed;
        std::set<std::uint32_t> materializedQuestItems, learnedWords;
        struct DamageStage
        {
            unsigned percent = 0, modelStage = 0, flags = 0;
            std::string model;
        };
        struct Destructible
        {
            float health = 0;
            std::vector<DamageStage> stages;
        };
        struct ObjectDamage
        {
            float health = 0;
            std::uint32_t next = 0, stage = 0;
            bool noCollision = false;
            std::string model;
            void checkpoint(Papyrus::Checkpoint& ar) { ar(health, next, stage, noCollision, model); }
        };
        std::map<std::uint32_t, Destructible> destructibles;
        std::map<std::uint32_t, ObjectDamage> damagedObjects;
        struct ImageFade
        {
            double started = 0, seconds = 0;
            float from = 0, to = 0;
            void checkpoint(Papyrus::Checkpoint& ar) { ar(started, seconds, from, to); }
        };
        std::map<std::uint32_t, ImageFade> imageFades;
        std::uint32_t crossFadeImage = 0;
        struct DoorLink
        {
            std::uint32_t world = 0;
            TES5Viewer::WorldScriptPose destination{};
            osg::BoundingBoxd bounds;
            bool automatic = false;
        };
        std::map<std::uint32_t, DoorLink> doors;
        struct PendingTravel
        {
            std::uint32_t location = 0, door = 0;
            TES5Viewer::WorldScriptPose destination;
            void checkpoint(Papyrus::Checkpoint& ar) { ar(location, door, destination); }
        };
        std::optional<PendingTravel> pendingTravel;
        int (*lineOfSight)(void*, std::uint32_t, std::uint32_t) = nullptr;
        void* visibilityContext = nullptr;
        std::uint32_t weather = 0;
        bool weatherOverride = false;
        std::array<float, 12> weatherDay{};
        double time = 0;
        std::vector<std::string> presentation;
        TES5Viewer::PresentationState visuals;
        bool packagesNeedRecovery = false;
        TES5Viewer::ActorInventory inventory;
        std::set<std::uint32_t> blockedActivation, ignoreFriendlyHits;
        std::set<std::pair<std::uint32_t, std::uint32_t>> alliedFactions;
        std::uint32_t playerWeapon = 0;
        bool weaponDrawn = false;
        double nextPlayerAttack = 0;
        std::map<std::string, std::string> itemStrings;
        struct DialogueTopic
        {
            std::uint32_t quest = 0;
            bool root = false, blocking = false, hello = false;
            float priority = 50;
            std::string prompt;
        };
        struct DialogueInfo
        {
            TES5Viewer::RecordPayload record;
            std::vector<std::span<const std::byte>> conditions;
            std::uint32_t speaker = 0, shared = 0, flags = 0, resetHours = 0;
            std::vector<std::uint32_t> links;
            std::string prompt;
        };
        struct DialogueChoice
        {
            std::uint32_t id, info, quest;
            std::string prompt;
        };
        struct DialogueLine
        {
            std::string text;
            std::uint32_t number = 0;
            int emotion = 0;
            float strength = 0;
        };
        struct Conversation
        {
            std::uint32_t actor = 0, info = 0, quest = 0, completed = 0, vendor = 0;
            std::string name, json;
            std::string tradeMessage;
            std::vector<DialogueChoice> choices;
            std::vector<DialogueLine> lines;
            std::size_t line = 0;
            std::int32_t voice = 0;
            unsigned automaticSteps = 0;
            double due = 0;
        } dialogue;
        std::map<std::uint32_t, DialogueTopic> dialogueTopics;
        std::map<std::uint32_t, DialogueInfo> dialogueInfos;
        std::map<std::uint32_t, TES5Viewer::RecordPayload> dialogueQuests;
        std::map<std::pair<std::uint32_t, std::uint32_t>, double> spokenInfos;
        bool startMissingDialogueQuests = false;
        std::map<std::uint32_t, std::set<std::uint32_t>> animationDependencies;
        std::map<std::uint32_t, std::uint32_t> executionVictims;
        std::map<std::uint32_t, std::string> severedHeads;
        TES5Viewer::WorldWeather environment;

        PapyrusState(const LoadResult& result, Papyrus::ErrorLog& diagnostics, bool keepGoing)
            : revision(result.revision), characterIndex(result.index), originalRead(result.read),
              read([this](std::uint32_t id) {
                  if (id == 7 && character)
                  {
                      if (!characterRecordCache) characterRecordCache = TES5Viewer::characterGameplayRecord(originalRead,*character,characterName);
                      return *characterRecordCache;
                  }
                  return originalRead(id);
              }),
              vfs(result.scripts.vfs), baseAnimations(result.baseAnimations),
              quests(read, vfs, makeHost(keepGoing), diagnostics),
              world(read, vfs, quests, diagnostics), scripts(quests.host(), report, &diagnostics, true), log(diagnostics), environment(read,result.index)
        {
            quests.setRecordIndex(result.index);
            world.setRecordIndex(result.index);
            world.setCartHorseOffset(result.cartHorseOffset);
        }

        void checkpoint(Papyrus::Checkpoint& ar)
        {
            ar(report, overrides, enableParents, tracked, startedGame, controls, gameFlags, sittingRotation,
                outfits, relocated, doorStates, weather, weatherOverride, weatherDay, time, presentation);
            quests.checkpoint(ar); world.checkpoint(ar); scripts.checkpoint(ar, *vfs);
        }

        void checkpointPapyrusExtensions(Papyrus::Checkpoint& ar)
        {
            ar(locks, locationData, randomState, nextMagicHandle, spellQueue, magicFlights);
            quests.checkpointInventoryFilters(ar);
            world.checkpointCombat(ar);
            scripts.checkpointLifecycle(ar);
            if (!ar.loading || !ar.atEnd()) scripts.checkpointAnimationEvents(ar);
            if (!ar.loading || !ar.atEnd()) ar(modelAnimations);
            if (!ar.loading || !ar.atEnd()) ar(pendingTravel);
            if (!ar.loading || !ar.atEnd()) world.checkpointFlightPermissions(ar);
            if (!ar.loading || !ar.atEnd())
                ar(blockedActivation, ignoreFriendlyHits, alliedFactions, playerWeapon, weaponDrawn, nextPlayerAttack);
            if (!ar.loading || !ar.atEnd()) ar(severedHeads);
            if (!ar.loading || !ar.atEnd()) quests.checkpointLocationAndTimers(ar);
            if (!ar.loading || !ar.atEnd()) ar(modelClips, modelAnimationWaits, noFavorAllowed);
            if (!ar.loading || !ar.atEnd())
            {
                quests.checkpointInventoryReferences(ar);
                ar(materializedQuestItems, learnedWords);
            }
            if (!ar.loading || !ar.atEnd())
            {
                scripts.checkpointCalls(ar);
                ar(damagedObjects, imageFades, crossFadeImage);
                world.checkpointGhosts(ar);
                quests.checkpointRepeatUpdates(ar);
            }
            if (!ar.loading || !ar.atEnd()) world.checkpointSocialState(ar);
            if (!ar.loading || !ar.atEnd())
            {
                std::string tag = "T5 dialogue 1";
                ar(tag);
                if (tag != "T5 dialogue 1") throw std::runtime_error("Unknown dialogue snapshot extension");
                ar(spokenInfos);
            }
            if (!ar.loading || !ar.atEnd()) ar(modelSequenceLayers);
        }

        void attachDoors(const std::map<std::uint32_t, float>& references)
        {
            doors.clear();
            if (!characterIndex) return;
            for (const auto& [id, scale] : references)
            {
                const auto ref = read(id);
                std::uint32_t destination = 0, base = 0;
                DoorLink link;
                TES5Viewer::visitSubrecords(ref, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("NAME")) base = ref.mapFormId(word(bytes));
                    if (tag != ESM::fourCC("XTEL") || bytes.size() < 28) return;
                    destination = ref.mapFormId(word(bytes));
                    for (unsigned axis = 0; axis < 3; ++axis)
                    {
                        link.destination.position.pos[axis] = real(bytes, 4 + axis * 4);
                        link.destination.position.rot[axis] = real(bytes, 16 + axis * 4);
                    }
                });
                if (const auto target = characterIndex->references.find(destination);
                    target != characterIndex->references.end())
                {
                    link.destination.cell = target->second.cell;
                    link.world = target->second.world;
                }
                if (base)
                    TES5Viewer::visitSubrecords(read(base), [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("FNAM") && !bytes.empty())
                            link.automatic = (std::to_integer<unsigned>(bytes.front()) & 2) != 0;
                        if (tag == ESM::fourCC("OBND") && bytes.size() == 12)
                        {
                            std::array<std::int16_t, 6> bounds;
                            std::memcpy(bounds.data(), bytes.data(), bytes.size());
                            link.bounds.set(scale * bounds[0], scale * bounds[1], scale * bounds[2],
                                scale * bounds[3], scale * bounds[4], scale * bounds[5]);
                        }
                    });
                doors.emplace(id, link);
            }
        }

        void attachPlayerBounds()
        {
            try
            {
                if (!playerTriggerBounds)
                {
                    const auto appearance = TES5Viewer::loadActorAppearance(read, *vfs, {}, 7, {});
                    const VFS::Path::Normalized path(appearance.skeleton);
                    Nif::NIFFile file(path);
                    Nif::Reader(file, nullptr).parse(vfs->get(path));
                    const auto bounds = TES5Viewer::readModelMechanics(file, true).actorBounds;
                    if (bounds.valid())
                        playerTriggerBounds.emplace(bounds._min * appearance.height, bounds._max * appearance.height);
                }
                if (playerTriggerBounds) world.setTriggerActorBounds(0x14, *playerTriggerBounds);
            }
            catch (const std::exception& error) { log.write("Player trigger bounds", error.what()); }
        }

        void queueTravel(std::uint32_t id, const DoorLink& link)
        {
            if (pendingTravel || !link.destination.cell) return;
            const auto location = link.world ? link.world : link.destination.cell;
            pendingTravel = PendingTravel{location, id, link.destination};
        }

        void automaticDoors()
        {
            const auto player = world.placement(0x14);
            if (!player || pendingTravel || (controls & 1) || (gameFlags & (4 | 64 | 128))) return;
            std::uint32_t nearest = 0;
            double distance = std::numeric_limits<double>::infinity();
            const osg::Vec3d standingHeight(0, 0, playerEyeHeight.value_or(124.343f * 1.03f) * .7f);
            for (const auto& [id, door] : doors)
            {
                if (!door.automatic || !door.destination.cell || !door.bounds.valid() || isDisabled(id)) continue;
                const auto pose = world.placement(id);
                if (!pose || !world.sameSpace(0x14, player->cell, id, pose->cell)) continue;
                const auto offset = player->position.asVec3() - pose->position.asVec3();
                const auto inverse = Misc::Convert::makeOsgQuat(pose->position).inverse();
                const auto local = inverse * osg::Vec3d(offset);
                const TES5Viewer::WorldScriptTriggerBox volume{id, osg::Matrixd::translate(-door.bounds.center()),
                    (door.bounds._max - door.bounds._min) * .5};
                if (volume.intersects(local, local + inverse * standingHeight, {}) && offset.length2() < distance)
                {
                    nearest = id;
                    distance = offset.length2();
                }
            }
            if (nearest && !lockState(nearest).second) queueTravel(nearest, doors.at(nearest));
        }

        bool activateReference(std::uint32_t id, std::uint32_t activator, bool immediate = true, bool defaultOnly = false)
        {
            if (isDisabled(id)) return false;
            if (!defaultOnly && blockedActivation.contains(id))
            {
                if (scripts.hasScripts(id)) scripts.activate(id, activator, immediate);
                return immediate;
            }
            const auto door = doors.find(id);
            const bool chest = !world.actorBase(id) && referenceBase(id) && formType(referenceBase(id)) == ESM::fourCC("CONT");
            if ((door != doors.end() || chest) && lockState(id).second && !unlockWithKey(id, activator))
            {
                if (activator == 0x14)
                    presentation.push_back("[\"help\",\"Заперто\",2]");
                return true;
            }
            const bool scripted = !defaultOnly && scripts.hasScripts(id);
            if (scripted) scripts.activate(id, activator, immediate);
            if (door != doors.end())
            {
                if (door->second.destination.cell)
                    queueTravel(id, door->second);
                else
                {
                    auto& state = doorStates[id];
                    state = {!state.first, time};
                }
                return true;
            }
            if (activator == 0x14 && canUseInventory())
            {
                if ((chest || world.isDead(id)) && canLoot(id))
                {
                    if (chest) playModelAnimation(id, "Open", false);
                    presentation.push_back(std::format("[\"inventory\",{}]", id));
                    return true;
                }
                if (!world.actorBase(id))
                    if (const auto item = referenceBase(id); item && TES5Viewer::isInventoryItem(formType(item)))
                    {
                        TES5Viewer::ActorInventory::add(inventory.inventory(0x14, world.actorBase(0x14), read), item, 1);
                        overrides[id] = true;
                        tracked.insert(id);
                        quests.inventoryChanged(0x14, item, 1, id, true);
                        return true;
                    }
            }
            if (scripted) playModelAnimation(id, "Open", true);
            if (!defaultOnly && immediate && activator == 0x14 && startDialogue(id))
                return true;
            return immediate && scripted;
        }

        bool canUseInventory() const
        {
            return !(controls & (1u << 5)) && !(gameFlags & (4u | 64u | 128u)) && !world.isDead(0x14);
        }

        std::uint32_t inventoryBase(std::uint32_t id)
        {
            if (const auto actor = world.actorBase(id)) return actor;
            const auto base = referenceBase(id);
            return base && formType(base) == ESM::fourCC("CONT") ? base : 0;
        }

        bool unlockWithKey(std::uint32_t id, std::uint32_t activator)
        {
            const auto ref = read(id);
            std::uint32_t key = 0;
            TES5Viewer::visitSubrecords(ref, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("XLOC") && bytes.size() >= 8) key = ref.mapFormId(word(bytes, 4));
            });
            const auto base = world.actorBase(activator);
            if (!key || !base) return false;
            const auto& items = inventory.inventory(activator, base, read);
            const auto found = items.find(key);
            if (found == items.end() || found->second <= 0) return false;
            lockState(id).second = false;
            return true;
        }

        bool canLoot(std::uint32_t owner)
        {
            if (!canUseInventory() || owner == 0x14 || isDisabled(owner) || blockedActivation.contains(owner)) return false;
            const auto player = world.placement(0x14), target = world.placement(owner);
            if (!player || !target || !world.sameSpace(0x14, player->cell, owner, target->cell)
                || (player->position.asVec3() - target->position.asVec3()).length2() > 330.f * 330.f)
                return false;
            if (world.actorBase(owner)) return world.isDead(owner);
            return inventoryBase(owner) && !lockState(owner).second;
        }

        std::string localizedText(const TES5Viewer::RecordPayload& record, std::span<const std::byte> bytes,
            bool response = false)
        {
            if (bytes.empty()) return {};
            if (!record.localized || bytes.size() != 4) return TES5Viewer::characterString(bytes);
            const auto key = word(bytes);
            if (!key) return {};
            const auto stem = std::filesystem::path(record.metadata.sourcePlugin).stem().string();
            for (const auto language : {"russian", "english"})
                for (const auto suffix : {"strings", "dlstrings", "ilstrings"})
                {
                    if (!response && std::string_view(suffix) != "strings") break;
                    const auto file = "strings/" + stem + "_" + language + "." + suffix;
                    const VFS::Path::Normalized path(file);
                    auto [cached, added] = itemStrings.try_emplace(file);
                    if (added && vfs->exists(path))
                    {
                        const auto input = vfs->get(path);
                        cached->second.assign(std::istreambuf_iterator<char>(*input), {});
                    }
                    const auto& data = cached->second;
                    if (data.size() < 8) continue;
                    const auto buffer = std::as_bytes(std::span(data.data(), data.size()));
                    const auto count = word(buffer);
                    if (count > (data.size() - 8) / 8) continue;
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        if (word(buffer, 8 + 8 * i) != key) continue;
                        const auto offset = 8ull + 8ull * count + word(buffer, 12 + 8 * i);
                        if (offset >= data.size()) break;
                        if (std::string_view(suffix) == "strings")
                        {
                            const auto end = data.find(char(0), offset);
                            if (end != std::string::npos) return data.substr(offset, end - offset);
                        }
                        else if (data.size() - offset >= 4)
                        {
                            const auto length = word(buffer, offset);
                            if (length && length <= data.size() - offset - 4)
                                return TES5Viewer::characterString(buffer.subspan(offset + 4, length));
                        }
                        break;
                    }
                }
            return {};
        }

        std::string itemName(const TES5Viewer::RecordPayload& record)
        {
            std::string full, editor;
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("EDID")) editor = TES5Viewer::characterString(bytes);
                if (tag == ESM::fourCC("FULL")) full = localizedText(record, bytes);
            });
            return full.empty() ? editor : full;
        }

        bool canTalk(std::uint32_t actor)
        {
            if (!actor || actor == 0x14 || (controls & (1u << 6)) || (gameFlags & (4u | 64u | 128u))
                || isDisabled(actor) || blockedActivation.contains(actor) || world.isDead(actor) || world.isDead(0x14)
                || world.isInCombat(actor) || world.isInCombat(0x14) || pendingTravel)
                return false;
            const auto player = world.placement(0x14), target = world.placement(actor);
            return player && target && world.actorBase(actor) && world.sameSpace(0x14, player->cell, actor, target->cell)
                && (player->position.asVec3() - target->position.asVec3()).length2() <= 350.f * 350.f;
        }

        const DialogueTopic& dialogueTopic(std::uint32_t id)
        {
            if (const auto found = dialogueTopics.find(id); found != dialogueTopics.end()) return found->second;
            DialogueTopic topic;
            const auto record = read(id);
            std::uint32_t branch = 0;
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("FULL")) topic.prompt = localizedText(record, bytes);
                if (bytes.size() != 4) return;
                if (tag == ESM::fourCC("QNAM")) topic.quest = record.mapFormId(word(bytes));
                if (tag == ESM::fourCC("BNAM")) branch = record.mapFormId(word(bytes));
                if (tag == ESM::fourCC("PNAM"))
                {
                    const auto priority = std::bit_cast<float>(word(bytes));
                    if (std::isfinite(priority)) topic.priority = priority;
                }
                if (tag == ESM::fourCC("DATA")) topic.hello = ((word(bytes) >> 8) & 0xff) == 7;
            });
            if (branch)
            {
                const auto source = read(branch);
                std::uint32_t first = 0, flags = 0, category = 0;
                TES5Viewer::visitSubrecords(source, [&](auto tag, auto bytes) {
                    if (bytes.size() != 4) return;
                    if (tag == ESM::fourCC("SNAM")) first = source.mapFormId(word(bytes));
                    if (tag == ESM::fourCC("DNAM")) flags = word(bytes);
                    if (tag == ESM::fourCC("TNAM")) category = word(bytes);
                    if (tag == ESM::fourCC("QNAM") && !topic.quest) topic.quest = source.mapFormId(word(bytes));
                });
                topic.root = !category && first == id && (flags & 3);
                topic.blocking = topic.root && (flags & 2);
            }
            else topic.root = !topic.hello;
            return dialogueTopics.emplace(id, std::move(topic)).first->second;
        }

        const DialogueInfo& dialogueInfo(std::uint32_t id)
        {
            if (const auto found = dialogueInfos.find(id); found != dialogueInfos.end()) return found->second;
            DialogueInfo info;
            info.record = read(id);
            if (info.record.metadata.recordType != ESM::fourCC("INFO"))
                throw std::runtime_error("Expected INFO response");
            TES5Viewer::visitSubrecords(info.record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("CTDA")) info.conditions.push_back(bytes);
                if (tag == ESM::fourCC("RNAM")) info.prompt = localizedText(info.record, bytes);
                if (tag == ESM::fourCC("ENAM") && bytes.size() == 4)
                {
                    info.flags = word(bytes) & 0xffff;
                    info.resetHours = word(bytes) >> 16;
                }
                if (bytes.size() != 4) return;
                if (tag == ESM::fourCC("ANAM")) info.speaker = info.record.mapFormId(word(bytes));
                if (tag == ESM::fourCC("DNAM")) info.shared = info.record.mapFormId(word(bytes));
                if (tag == ESM::fourCC("TCLT"))
                    if (const auto link = info.record.mapFormId(word(bytes))) info.links.push_back(link);
            });
            return dialogueInfos.emplace(id, std::move(info)).first->second;
        }

        bool dialogueQuestAllows(std::uint32_t quest, std::uint32_t actor)
        {
            if (!quest) return true;
            if (!quests.isRunning(quest)) return false;
            auto found = dialogueQuests.find(quest);
            if (found == dialogueQuests.end()) found = dialogueQuests.emplace(quest, read(quest)).first;
            const auto& record = found->second;
            std::vector<std::span<const std::byte>> conditions;
            bool header = true;
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("NEXT") || tag == ESM::fourCC("INDX") || tag == ESM::fourCC("ANAM"))
                    header = false;
                if (header && tag == ESM::fourCC("CTDA")) conditions.push_back(bytes);
            });
            return quests.testConditions(quest, record, conditions, actor, 0x14);
        }

        std::optional<DialogueChoice> dialogueChoice(std::uint32_t id)
        {
            if (!characterIndex) return {};
            const auto direct = characterIndex->topicByInfo.find(id);
            const auto topicId = direct == characterIndex->topicByInfo.end() ? id : direct->second;
            const auto entries = characterIndex->infosByTopic.find(topicId);
            if (entries == characterIndex->infosByTopic.end()
                || !std::ranges::binary_search(characterIndex->playerDialogueTopics, topicId))
                return {};
            try
            {
                const auto& topic = dialogueTopic(topicId);
                if (!dialogueQuestAllows(topic.quest, dialogue.actor)) return {};
                const auto base = world.actorBase(dialogue.actor);
                for (const auto candidate : entries->second)
                {
                    if (direct != characterIndex->topicByInfo.end() && candidate != id) continue;
                    const auto& info = dialogueInfo(candidate);
                    if ((info.speaker && info.speaker != base) || (info.flags & (0x10 | 0x80))) continue;
                    if (const auto used = spokenInfos.find({dialogue.actor, candidate}); used != spokenInfos.end())
                    {
                        if ((info.flags & 4) || (info.resetHours
                            && quests.globalValue("gamedayspassed", 0.f) - used->second < info.resetHours / (2730.0 * 24.0)))
                            continue;
                    }
                    try
                    {
                        if (quests.testConditions(topic.quest, info.record, info.conditions, dialogue.actor, 0x14))
                            return DialogueChoice{id, candidate, topic.quest, info.prompt.empty() ? topic.prompt : info.prompt};
                    }
                    catch (const std::runtime_error&)
                    {
                        // An unsupported CTDA excludes this INFO while other responses remain eligible.
                    }
                }
            }
            catch (const std::runtime_error&)
            {
                // A missing/deleted record or an unsupported quest condition disables the topic.
            }
            return {};
        }

        static constexpr std::uint32_t tradePlayer = 0x14, tradeGold = 0xf;

        struct Vendor
        {
            std::uint32_t chest = 0;
            unsigned startHour = 0, endHour = 24;
            bool inverted = false, stolenOnly = false;
            std::set<std::uint32_t> filter;
        };
        std::map<std::uint32_t, Vendor> vendors;

        const Vendor& vendor(std::uint32_t faction)
        {
            if (const auto found = vendors.find(faction); found != vendors.end()) return found->second;
            Vendor result;
            const auto record = read(faction);
            std::uint32_t flags = 0, list = 0;
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("DATA") && bytes.size() >= 4) flags = word(bytes);
                if (tag == ESM::fourCC("VENC")) result.chest = record.mapFormId(word(bytes));
                if (tag == ESM::fourCC("VEND")) list = record.mapFormId(word(bytes));
                if (tag == ESM::fourCC("VENV") && bytes.size() >= 10)
                {
                    result.startHour = word(bytes) & 0xffff;
                    result.endHour = word(bytes) >> 16;
                    result.stolenOnly = bytes[8] != std::byte{};
                    result.inverted = bytes[9] != std::byte{};
                }
            });
            if (record.metadata.recordType != ESM::fourCC("FACT") || !(flags & 0x4000)
                || !result.chest || formType(inventoryBase(result.chest)) != ESM::fourCC("CONT"))
                result.chest = 0;
            if (result.chest && list)
            {
                const auto filter = read(list);
                TES5Viewer::visitSubrecords(filter, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("LNAM")) result.filter.insert(filter.mapFormId(word(bytes)));
                });
            }
            return vendors.emplace(faction, std::move(result)).first->second;
        }

        std::uint32_t vendorFaction(std::uint32_t actor)
        {
            const auto hour = quests.globalValue("gamehour", 12.f);
            auto base = world.actorBase(actor);
            for (int depth = 0; base && depth < 16; ++depth)
            {
                const auto npc = read(base);
                std::uint32_t parent = 0, faction = 0, inherited = 0;
                TES5Viewer::visitSubrecords(npc, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("TPLT")) parent = npc.mapFormId(word(bytes));
                    if (tag == ESM::fourCC("ACBS") && bytes.size() >= 20) inherited = word(bytes, 16) >> 16;
                });
                if (parent && (inherited & 4))
                {
                    base = parent;
                    continue;
                }
                TES5Viewer::visitSubrecords(npc, [&](auto tag, auto bytes) {
                    if (faction || tag != ESM::fourCC("SNAM") || bytes.size() < 5
                        || std::to_integer<unsigned>(bytes[4]) >= 128) return;
                    const auto id = npc.mapFormId(word(bytes));
                    if (!id) return;
                    const auto& service = vendor(id);
                    if (service.chest && (service.startHour < service.endHour
                            ? hour >= service.startHour && hour < service.endHour
                            : hour >= service.startHour || hour < service.endHour))
                        faction = id;
                });
                return faction;
            }
            return 0;
        }

        using TradeStock = std::map<std::uint32_t, std::int32_t>;

        void expandVendorList(TradeStock& stock, std::uint32_t id, int count, std::minstd_rand& rng,
            unsigned& budget, unsigned depth = 0)
        {
            if (!id || count <= 0 || depth == 16 || !budget) return;
            --budget;
            const auto record = read(id);
            if (record.metadata.recordType != ESM::fourCC("LVLI"))
            {
                if (TES5Viewer::isInventoryItem(record.metadata.recordType))
                    TES5Viewer::ActorInventory::add(stock, id, count);
                return;
            }
            struct Entry { std::uint32_t item; unsigned count; };
            std::vector<Entry> entries;
            unsigned flags = 0, chance = 0;
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                if (bytes.empty()) return;
                if (tag == ESM::fourCC("LVLF")) flags = std::to_integer<unsigned>(bytes[0]);
                if (tag == ESM::fourCC("LVLD")) chance = std::to_integer<unsigned>(bytes[0]);
                if (tag == ESM::fourCC("LVLO") && bytes.size() >= 12 && (word(bytes) & 0xffff) <= 1)
                    entries.push_back({record.mapFormId(word(bytes, 4)), word(bytes, 8) & 0xffff});
                if (tag == ESM::fourCC("LVLG") && word(bytes))
                    visitSubrecords(read(record.mapFormId(word(bytes))), [&](auto globalTag, auto data) {
                        if (globalTag == ESM::fourCC("FLTV") && data.size() == 4)
                        {
                            const auto value = std::bit_cast<float>(word(data));
                            if (std::isfinite(value)) chance = static_cast<unsigned>(std::clamp(value, 0.f, 100.f));
                        }
                    });
            });
            if (entries.empty()) return;
            const bool each = (flags & 2) && !(flags & 4);
            const auto add = [&](const Entry& entry) {
                const auto total = std::int64_t(entry.count) * (each ? 1 : count);
                if (total <= std::numeric_limits<int>::max())
                    expandVendorList(stock, entry.item, static_cast<int>(total), rng, budget, depth + 1);
            };
            for (int i = 0; i < (each ? count : 1) && budget; ++i)
            {
                --budget;
                if (std::uniform_int_distribution<unsigned>(0, 99)(rng) < chance) continue;
                if (flags & 4)
                    for (const auto& entry : entries) add(entry);
                else
                    add(entries[std::uniform_int_distribution<std::size_t>(0, entries.size() - 1)(rng)]);
            }
        }

        TradeStock& barterStock(const Vendor& service)
        {
            return inventory.inventory(service.chest, inventoryBase(service.chest), read);
        }

        void openBarter(std::uint32_t actor)
        {
            if (actor != dialogue.actor || dialogue.vendor || !canTalk(actor) || !canUseInventory()) return;
            const auto faction = vendorFaction(actor);
            if (!faction) return;
            auto& stock = barterStock(vendor(faction));
            std::vector<std::pair<std::uint32_t, int>> lists;
            for (const auto& [id, count] : stock)
                if (count > 0 && formType(id) == ESM::fourCC("LVLI")) lists.emplace_back(id, count);
            if (!lists.empty())
            {
                auto expanded = stock;
                std::minstd_rand rng(faction);
                unsigned budget = 4096;
                for (const auto [id, count] : lists)
                {
                    expandVendorList(expanded, id, count, rng, budget);
                    expanded[id] = 0;
                }
                stock = std::move(expanded);
            }
            syncQuestItems();
            dialogue.vendor = faction;
            refreshDialogueJson();
        }

        static int tradeCount(const TradeStock& stock, std::uint32_t item)
        {
            const auto it = stock.find(item);
            return it == stock.end() ? 0 : it->second;
        }

        int tradePrice(const TES5Viewer::RecordPayload& item, bool buy)
        {
            const auto type = item.metadata.recordType;
            if (!TES5Viewer::isInventoryItem(type) || type == ESM::fourCC("KEYM")
                || item.metadata.formId == tradeGold || (item.flags & 4)) return 0;
            const auto field = type == ESM::fourCC("ALCH") ? ESM::fourCC("ENIT") : ESM::fourCC("DATA");
            const std::size_t offset = type == ESM::fourCC("AMMO") ? 12 : type == ESM::fourCC("BOOK") ? 8 : 0;
            std::uint32_t value = 0;
            TES5Viewer::visitSubrecords(item, [&](auto tag, auto data) {
                if (tag == field && data.size() >= offset + 4) value = word(data, offset);
            });
            if (!value || value > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 2)) return 0;
            return buy ? static_cast<int>(value * 2) : std::max(1, static_cast<int>(value / 2));
        }

        bool vendorBuys(const Vendor& service, const TES5Viewer::RecordPayload& item)
        {
            if (service.stolenOnly) return false;
            if (service.filter.empty()) return true;
            bool matches = service.filter.contains(item.metadata.formId);
            TES5Viewer::visitSubrecords(item, [&](auto tag, auto data) {
                if (tag == ESM::fourCC("KWDA"))
                    for (std::size_t offset = 0; offset + 4 <= data.size(); offset += 4)
                        matches |= service.filter.contains(item.mapFormId(word(data, offset)));
            });
            return matches != service.inverted;
        }

        int sellableCount(const TradeStock& player, std::uint32_t item)
        {
            auto count = tradeCount(player, item);
            if (item == playerWeapon || (outfits.contains(tradePlayer) && outfits.at(tradePlayer).contains(item)))
                --count;
            for (const auto& [id, owner] : quests.inventoryReferences())
                if (owner == tradePlayer && quests.createdReferences().at(id).base == item) --count;
            return std::max(0, count);
        }

        std::string barterJson()
        {
            const auto& service = vendor(dialogue.vendor);
            const auto& stock = barterStock(service);
            const auto& player = inventory.inventory(tradePlayer, world.actorBase(tradePlayer), read);
            const auto rows = [&](const TradeStock& items, bool buy) {
                std::string result = "[";
                for (const auto& [id, quantity] : items)
                {
                    const auto count = buy ? quantity : sellableCount(items, id);
                    if (count <= 0) continue;
                    const auto item = read(id);
                    const auto price = tradePrice(item, buy);
                    if (!price || (!buy && !vendorBuys(service, item))) continue;
                    if (result.size() > 1) result += ',';
                    result += std::format("[{},{},{},{}]", id, jsonText(itemName(item)), count, price);
                }
                return result + ']';
            };
            return std::format("{{\"playerGold\":{},\"vendorGold\":{},\"buy\":{},\"sell\":{},\"message\":{}}}",
                tradeCount(player, tradeGold), tradeCount(stock, tradeGold), rows(stock, true), rows(player, false),
                jsonText(dialogue.tradeMessage));
        }

        void tradeItem(std::uint32_t id, bool buy)
        {
            if (!dialogue.vendor || !canTalk(dialogue.actor) || !canUseInventory() || !id || id == tradeGold) return;
            const auto& service = vendor(dialogue.vendor);
            auto& stock = barterStock(service);
            auto& player = inventory.inventory(tradePlayer, world.actorBase(tradePlayer), read);
            auto& from = buy ? stock : player;
            auto& to = buy ? player : stock;
            if ((buy ? tradeCount(stock, id) : sellableCount(player, id)) <= 0)
            {
                refreshDialogueJson();
                return;
            }
            const auto item = read(id);
            const auto price = tradePrice(item, buy);
            if (!price || (!buy && !vendorBuys(service, item))) return;
            if (tradeCount(to, tradeGold) < price)
                dialogue.tradeMessage = buy ? "У вас недостаточно золота." : "У торговца недостаточно золота.";
            else if (tradeCount(to, id) < std::numeric_limits<int>::max()
                && tradeCount(from, tradeGold) <= std::numeric_limits<int>::max() - price)
            {
                // Validate both legs before committing the item and gold together.
                auto& sourceCount = from.at(id);
                auto& buyerGold = to.at(tradeGold);
                auto& destinationCount = to[id];
                auto& sellerGold = from[tradeGold];
                --sourceCount;
                ++destinationCount;
                buyerGold -= price;
                sellerGold += price;
                dialogue.tradeMessage = buy ? "Покупка совершена." : "Продажа совершена.";
                const auto source = buy ? service.chest : tradePlayer;
                const auto destination = buy ? tradePlayer : service.chest;
                quests.inventoryChanged(source, id, 1, destination, false);
                quests.inventoryChanged(destination, id, 1, source, true);
                quests.inventoryChanged(destination, tradeGold, price, source, false);
                quests.inventoryChanged(source, tradeGold, price, destination, true);
            }
            refreshDialogueJson();
        }

        void leaveBarter()
        {
            if (!std::exchange(dialogue.vendor, 0u)) return;
            dialogue.tradeMessage.clear();
            if (dialogue.completed) continueDialogue();
            else refreshDialogueJson();
        }

        void refreshDialogueJson()
        {
            auto& state = dialogue;
            if (state.vendor)
            {
                state.json = std::format("{{\"actor\":{},\"name\":{},\"trade\":{}}}",
                    state.actor, jsonText(state.name), barterJson());
                return;
            }
            state.json = std::format("{{\"actor\":{},\"name\":{},\"line\":{},\"speaking\":{},\"choices\":[",
                state.actor, jsonText(state.name), jsonText(state.lines.empty() ? "" : state.lines[state.line].text),
                state.info ? "true" : "false");
            bool first = true;
            if (!state.info)
                for (const auto& choice : state.choices)
                {
                    if (!std::exchange(first, false)) state.json += ',';
                    state.json += std::format("[{},{}]", choice.id, jsonText(choice.prompt));
                }
            state.json += "]}";
        }

        void playDialogueLine(const TES5Viewer::RecordPayload& recording)
        {
            const auto& line = dialogue.lines[dialogue.line];
            const auto [voice, duration] = world.playTopicLine(recording, dialogue.actor, line.number, line.emotion, line.strength);
            dialogue.voice = voice;
            dialogue.due = time + (duration > 0 ? duration + .15 : std::clamp(1.0 + line.text.size() / 24.0, 1.4, 8.0));
            refreshDialogueJson();
        }

        const DialogueInfo& dialogueRecording(std::uint32_t id)
        {
            for (unsigned depth = 0; depth < 16; ++depth)
            {
                const auto& info = dialogueInfo(id);
                if (!info.shared) return info;
                id = info.shared;
            }
            throw std::runtime_error("Circular or too deep shared INFO chain");
        }

        void speak(const DialogueChoice& choice)
        {
            const auto& info = dialogueInfo(choice.info);
            const auto& recording = dialogueRecording(choice.info).record;
            std::vector<DialogueLine> lines;
            TES5Viewer::visitSubrecords(recording, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("TRDT") && bytes.size() >= 13)
                    lines.push_back({{}, std::to_integer<unsigned>(bytes[12]), static_cast<int>(word(bytes)),
                        std::clamp(word(bytes, 4) / 100.f, 0.f, 1.f)});
                if (tag == ESM::fourCC("NAM1"))
                {
                    if (lines.empty()) lines.emplace_back();
                    lines.back().text = localizedText(recording, bytes, true);
                }
            });
            if (info.flags & 4 || info.resetHours)
                spokenInfos[{dialogue.actor, choice.info}] = quests.globalValue("gamedayspassed", 0.f);
            dialogue.info = choice.info;
            dialogue.quest = choice.quest;
            dialogue.lines = std::move(lines);
            dialogue.line = 0;
            dialogue.due = time;
            world.topicFragment(info.record, dialogue.quest, dialogue.actor, true);
            if (!dialogue.lines.empty()) playDialogueLine(recording);
            else refreshDialogueJson();
        }

        void closeDialogue()
        {
            world.stopTopicLine(dialogue.voice);
            if (dialogue.info)
                world.topicFragment(dialogueInfo(dialogue.info).record, dialogue.quest, dialogue.actor, false);
            dialogue = {};
        }

        std::optional<DialogueChoice> rootDialogueChoices()
        {
            dialogue.choices.clear();
            if (std::exchange(startMissingDialogueQuests, false))
            {
                // Earlier MQ101-only launch profiles never started ambient dialogue quests.
                std::set<std::uint32_t> owners;
                for (const auto id : characterIndex->playerDialogueTopics)
                    try
                    {
                        const auto& topic = dialogueTopic(id);
                        if (topic.root || topic.hello) owners.insert(topic.quest);
                    }
                    catch (const std::runtime_error&) {}
                for (const auto quest : characterIndex->startGameQuests)
                    if (owners.contains(quest))
                        try { quests.startDialogueQuest(quest); }
                        catch (const std::runtime_error& error) { log.write("Dialogue quest startup", error.what()); }
            }
            std::vector<std::pair<float, DialogueChoice>> options;
            std::optional<DialogueChoice> greeting;
            float greetingPriority = -std::numeric_limits<float>::infinity();
            for (const auto id : characterIndex->playerDialogueTopics)
            {
                try
                {
                    const auto& topic = dialogueTopic(id);
                    if (!topic.root && !topic.hello) continue;
                    auto choice = dialogueChoice(id);
                    if (!choice) continue;
                    if (topic.blocking || topic.hello)
                    {
                        const auto priority = topic.priority + (topic.blocking ? 1000.f : 0.f);
                        if (priority > greetingPriority)
                        {
                            greetingPriority = priority;
                            greeting = std::move(choice);
                        }
                    }
                    else if (!choice->prompt.empty()) options.emplace_back(topic.priority, std::move(*choice));
                }
                catch (const std::runtime_error&) {}
            }
            std::ranges::stable_sort(options, std::greater<>{}, &std::pair<float, DialogueChoice>::first);
            for (auto& [priority, choice] : options)
                dialogue.choices.push_back(std::move(choice));
            return greeting;
        }

        bool startDialogue(std::uint32_t actor)
        {
            if (!characterIndex || dialogue.actor || !canTalk(actor)) return false;
            dialogue.actor = actor;
            try
            {
                const auto greeting = rootDialogueChoices();
                if (!greeting && dialogue.choices.empty())
                {
                    dialogue = {};
                    return false;
                }
                dialogue.name = itemName(read(world.actorBase(actor)));
                if (greeting) speak(*greeting);
                else refreshDialogueJson();
                presentation.push_back(std::format("[\"dialogue\",{}]", actor));
                return true;
            }
            catch (const std::exception& error)
            {
                closeDialogue();
                log.write("Player dialogue", error.what());
                return false;
            }
        }

        void chooseDialogue(std::uint32_t id)
        {
            if (!dialogue.actor || dialogue.vendor || dialogue.info || !canTalk(dialogue.actor)
                || std::ranges::find(dialogue.choices, id, &DialogueChoice::id) == dialogue.choices.end())
                return;
            if (const auto choice = dialogueChoice(id))
            {
                dialogue.automaticSteps = 0;
                speak(*choice);
            }
            else
            {
                std::erase_if(dialogue.choices, [id](const auto& choice) { return choice.id == id; });
                if (dialogue.choices.empty()) closeDialogue();
                else refreshDialogueJson();
            }
        }

        void tickDialogue()
        {
            if (!dialogue.actor) return;
            if (!canTalk(dialogue.actor))
            {
                closeDialogue();
                return;
            }
            if (!dialogue.info || time < dialogue.due) return;
            world.stopTopicLine(std::exchange(dialogue.voice, 0));
            if (dialogue.line + 1 < dialogue.lines.size())
            {
                ++dialogue.line;
                playDialogueLine(dialogueRecording(dialogue.info).record);
                return;
            }
            const auto& info = dialogueInfo(dialogue.info);
            world.topicFragment(info.record, dialogue.quest, dialogue.actor, false);
            dialogue.info = 0;
            dialogue.lines.clear();
            dialogue.choices.clear();
            // Let the End fragment apply its quest changes before evaluating linked topics.
            dialogue.due = time;
            dialogue.completed = info.record.metadata.formId;
        }

        void continueDialogue()
        {
            if (!dialogue.actor || dialogue.vendor || !dialogue.completed) return;
            const auto& info = dialogueInfo(std::exchange(dialogue.completed, 0));
            if (!(info.flags & 1))
            {
                for (const auto id : info.links)
                    if (const auto choice = dialogueChoice(id))
                        dialogue.choices.push_back(*choice);
                if (!dialogue.choices.empty()
                    && ((info.flags & 0x40) || dialogue.choices.front().prompt.empty())
                    && ++dialogue.automaticSteps <= 32)
                {
                    const auto next = dialogue.choices.front();
                    speak(next);
                    return;
                }
                std::erase_if(dialogue.choices, [](const auto& choice) { return choice.prompt.empty(); });
                if (dialogue.choices.empty()) rootDialogueChoices();
                if (!dialogue.choices.empty())
                {
                    refreshDialogueJson();
                    return;
                }
            }
            closeDialogue();
        }

        std::string inventoryItemDetails(const TES5Viewer::RecordPayload& record)
        {
            const auto type = record.metadata.recordType;
            const bool alchemy = type == ESM::fourCC("ALCH") || type == ESM::fourCC("INGR");
            std::optional<std::uint32_t> value, damage;
            std::optional<float> weight;
            bool food = false;
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto data) {
                if (tag == ESM::fourCC("ENIT") && alchemy && data.size() >= 4)
                {
                    value = word(data);
                    food = type == ESM::fourCC("ALCH") && data.size() >= 8 && (word(data, 4) & 2) != 0;
                }
                if (tag != ESM::fourCC("DATA")) return;
                const auto readWeight = [&](std::size_t offset) {
                    if (data.size() < offset + 4) return;
                    const float candidate = std::bit_cast<float>(word(data, offset));
                    if (std::isfinite(candidate) && candidate >= 0 && candidate <= 10000) weight = candidate;
                };
                if (alchemy)
                    readWeight(0);
                else if (type == ESM::fourCC("AMMO"))
                {
                    if (data.size() >= 16) value = word(data, 12);
                    readWeight(16);
                }
                else if (type == ESM::fourCC("BOOK") || type == ESM::fourCC("SCRL") || type == ESM::fourCC("WEAP")
                    || type == ESM::fourCC("ARMO") || type == ESM::fourCC("MISC") || type == ESM::fourCC("KEYM")
                    || type == ESM::fourCC("SLGM"))
                {
                    const std::size_t offset = type == ESM::fourCC("BOOK") ? 8 : 0;
                    if (data.size() >= offset + 4) value = word(data, offset);
                    readWeight(offset + 4);
                    if (type == ESM::fourCC("WEAP") && data.size() >= 10)
                        damage = std::to_integer<std::uint32_t>(data[8]) | (std::to_integer<std::uint32_t>(data[9]) << 8);
                }
            });
            return std::format("{{\"food\":{},\"weight\":{},\"value\":{},\"damage\":{}}}", food,
                weight ? std::format("{}", *weight) : "null", value ? std::to_string(*value) : "null",
                damage ? std::to_string(*damage) : "null");
        }

        std::string playerInventoryJson(std::uint32_t owner, bool details = false)
        {
            if (owner == 0x14 ? !canUseInventory() : !canLoot(owner)) return {};
            const auto base = inventoryBase(owner);
            if (!base) return {};
            auto result = "[" + jsonText(itemName(read(base)));
            for (const auto& [item, count] : inventory.inventory(owner, base, read))
            {
                if (count <= 0) continue;
                const auto record = read(item);
                const auto type = record.metadata.recordType;
                if (!TES5Viewer::isInventoryItem(type)) continue;
                const bool equipped = owner == 0x14 && (playerWeapon == item
                    || (outfits.contains(owner) && outfits.at(owner).contains(item)));
                result += std::format(",[{},{},{},{},{}", item, count, jsonText(itemName(record)), type, equipped);
                if (details) result += ',' + inventoryItemDetails(record);
                result += ']';
            }
            return result + ']';
        }

        void syncQuestItems()
        {
            for (const auto& [id, owner] : quests.inventoryReferences())
                if (owner && !materializedQuestItems.contains(id))
                {
                    const auto item = quests.createdReferences().at(id).base;
                    if (!TES5Viewer::isInventoryItem(formType(item)))
                        continue;
                    TES5Viewer::ActorInventory::add(inventory.inventory(owner, inventoryBase(owner), read), item, 1);
                    materializedQuestItems.insert(id);
                }
        }

        bool takeItem(std::uint32_t owner, std::uint32_t item, std::int32_t count)
        {
            if (!canLoot(owner) || !TES5Viewer::isInventoryItem(formType(item))) return false;
            syncQuestItems();
            auto& from = inventory.inventory(owner, inventoryBase(owner), read);
            auto& to = inventory.inventory(0x14, world.actorBase(0x14), read);
            const auto moved = TES5Viewer::ActorInventory::transfer(from, to, item, count);
            if (!moved) return false;
            quests.inventoryChanged(owner, item, moved, 0x14, false);
            quests.inventoryChanged(0x14, item, moved, owner, true);
            quests.transferInventoryReferences(item, owner, 0x14, moved);
            return true;
        }

        bool equipPlayer(std::uint32_t item, bool toggle)
        {
            const auto type = formType(item);
            if (type == ESM::fourCC("WEAP"))
            {
                const auto replacement = toggle && playerWeapon == item ? 0 : item;
                if (replacement != playerWeapon)
                {
                    if (playerWeapon) quests.equipmentChanged(0x14, playerWeapon, false);
                    playerWeapon = replacement;
                    if (playerWeapon) quests.equipmentChanged(0x14, playerWeapon, true);
                }
                return true;
            }
            if (type != ESM::fourCC("ARMO")) return false;
            auto& equipped = outfits[0x14];
            if (equipped.contains(item))
            {
                if (toggle)
                {
                    equipped.erase(item);
                    quests.equipmentChanged(0x14, item, false);
                }
                return true;
            }
            const auto slots = [&](std::uint32_t id) {
                std::uint32_t mask = 0;
                TES5Viewer::visitSubrecords(read(id), [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("BOD2") || tag == ESM::fourCC("BODT")) mask = word(bytes);
                });
                return mask;
            };
            const auto mask = slots(item);
            std::erase_if(equipped, [&](auto id) {
                if (!(slots(id) & mask)) return false;
                quests.equipmentChanged(0x14, id, false);
                return true;
            });
            equipped.insert(item);
            quests.equipmentChanged(0x14, item, true);
            return true;
        }

        bool useItem(std::uint32_t item)
        {
            if (!canUseInventory()) return false;
            auto& contents = inventory.inventory(0x14, world.actorBase(0x14), read);
            const auto found = contents.find(item);
            if (found == contents.end() || found->second <= 0) return false;
            if (equipPlayer(item, true)) return true;
            const auto record = read(item);
            if (record.metadata.recordType != ESM::fourCC("ALCH") && record.metadata.recordType != ESM::fourCC("INGR"))
                return false;
            bool consumed = false;
            std::uint32_t effect = 0;
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("EFID")) effect = record.mapFormId(word(bytes));
                if (tag != ESM::fourCC("EFIT") || !effect || bytes.size() < 12 || word(bytes, 8)) return;
                const auto magnitude = real(bytes, 0);
                TES5Viewer::visitSubrecords(read(effect), [&](auto type, auto data) {
                    if (type == ESM::fourCC("DATA") && data.size() >= 72 && word(data, 64) == 0
                        && !(word(data) & 4))
                        consumed |= world.restoreActorValue(0x14, word(data, 68), magnitude);
                });
            });
            if (!consumed) return false;
            --found->second;
            quests.inventoryChanged(0x14, item, 1, 0, false);
            return true;
        }

        struct WeaponStats
        {
            float damage = 4, reach = 96, speed = 1;
            unsigned type = 0;
            std::uint32_t attackSound = 0;
        };

        WeaponStats weaponStats()
        {
            WeaponStats stats;
            if (playerWeapon)
            {
                float distance = 141.f;
                if (characterIndex)
                    if (const auto setting = characterIndex->floatSettings.find("fcombatdistance");
                        setting != characterIndex->floatSettings.end() && setting->second > 0)
                        distance = setting->second;
                const auto record = read(playerWeapon);
                stats.reach = distance;
                TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("DATA") && bytes.size() >= 10)
                    {
                        std::uint16_t damage;
                        std::memcpy(&damage, bytes.data() + 8, 2);
                        stats.damage = damage;
                    }
                    if (tag == ESM::fourCC("DNAM") && bytes.size() >= 12)
                    {
                        stats.type = std::to_integer<unsigned>(bytes[0]);
                        stats.speed = real(bytes, 4);
                        stats.reach = distance * real(bytes, 8);
                    }
                    if (tag == ESM::fourCC("SNAM")) stats.attackSound = record.mapFormId(word(bytes));
                });
            }
            else
            {
                const auto npc = read(world.actorBase(0x14));
                TES5Viewer::visitSubrecords(npc, [&](auto tag, auto bytes) {
                    if (tag != ESM::fourCC("RNAM")) return;
                    TES5Viewer::visitSubrecords(read(npc.mapFormId(word(bytes))), [&](auto type, auto data) {
                        if (type != ESM::fourCC("DATA") || data.size() < 104) return;
                        stats.damage = real(data, 96);
                        stats.reach = real(data, 100);
                    });
                });
            }
            if (!std::isfinite(stats.reach) || stats.reach <= 0) stats.reach = 96;
            if (!std::isfinite(stats.speed) || stats.speed <= 0) stats.speed = 1;
            return stats;
        }

        bool canFight() const
        {
            return !(controls & (1u << 1)) && !(gameFlags & (4u | 64u | 128u)) && !world.isDead(0x14);
        }

        bool playerBow(const float* aim)
        {
            if (!aim || !weaponDrawn || !playerWeapon || !canFight() || time < nextPlayerAttack) return false;
            const auto stats = weaponStats();
            if (stats.type != 7 && stats.type != 9) return false;
            for (unsigned i = 0; i < 6; ++i) if (!std::isfinite(aim[i])) return false;
            const osg::Vec3f start(aim[0], aim[1], aim[2]);
            auto direction = osg::Vec3f(aim[3], aim[4], aim[5]);
            if (!direction.normalize()) return false;
            auto& items = inventory.inventory(0x14, world.actorBase(0x14), read);
            for (auto& [item, count] : items)
            {
                if (count <= 0 || formType(item) != ESM::fourCC("AMMO")) continue;
                const auto loadout = TES5Viewer::readArrowLoadout(read, 0, playerWeapon, item);
                if (!loadout) continue;
                const auto shot = world.registerPlayerArrow(*loadout);
                --count;
                quests.inventoryChanged(0x14, item, 1, 0, false);
                nextPlayerAttack = time + 1. / std::clamp(loadout->drawSpeed, .1f, 5.f);
                const auto end = start + direction;
                presentation.push_back(std::format(
                    "[\"spellProjectile\",{},{},0,0,{},{},{},{},{},{},{},{},{},{},{},{}]",
                    shot, 0x14, start.x(), start.y(), start.z(), end.x(), end.y(), end.z(),
                    loadout->speed, loadout->gravity, loadout->range, loadout->radius,
                    loadout->lifetime > 0 ? loadout->lifetime : loadout->range / loadout->speed, jsonText(loadout->projectileModel)));
                return true;
            }
            presentation.push_back("[\"help\",\"Нет подходящих боеприпасов\",2]");
            return false;
        }

        bool friendlyFactions(std::uint32_t actor)
        {
            const auto factions = [&](std::uint32_t ref) {
                std::vector<std::uint32_t> result;
                if (const auto base = world.actorBase(ref))
                {
                    const auto record = read(base);
                    TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("SNAM") && bytes.size() >= 5 && std::to_integer<unsigned>(bytes[4]) < 128)
                            result.push_back(record.mapFormId(word(bytes)));
                    });
                }
                return result;
            };
            const auto other = factions(actor);
            for (const auto player : factions(0x14))
                for (const auto faction : other)
                    if (player == faction || alliedFactions.contains({faction, player})) return true;
            return false;
        }

        bool playerAttack(std::uint32_t target)
        {
            if (!weaponDrawn || !canFight() || time < nextPlayerAttack) return false;
            const auto stats = weaponStats();
            if (stats.type == 7 || stats.type == 9) return false;
            nextPlayerAttack = time + 1. / std::clamp(stats.speed, .1f, 5.f);
            if (stats.attackSound)
                world.callFormMethod(stats.attackSound, "Play", {formValue(0x14)});
            if (target && !isDisabled(target) && !(ignoreFriendlyHits.contains(target) && friendlyFactions(target)))
            {
                const auto player = world.placement(0x14), other = world.placement(target);
                if (player && other && world.sameSpace(0x14, player->cell, target, other->cell)
                    && (player->position.asVec3() - other->position.asVec3()).length() <= stats.reach)
                {
                    if (!world.strike(target, 0x14, playerWeapon, stats.damage))
                        damageObject(target, stats.damage);
                }
            }
            return true;
        }

        std::uint32_t referenceBase(std::uint32_t id) const
        {
            if (const auto found = quests.createdReferences().find(id); found != quests.createdReferences().end())
                return found->second.base;
            const auto record = read(id);
            if (record.metadata.recordType != ESM::fourCC("REFR"))
                return 0;
            std::uint32_t base = 0;
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("NAME")) base = record.mapFormId(word(bytes));
            });
            return base;
        }

        const Destructible& destruction(std::uint32_t base)
        {
            if (const auto found = destructibles.find(base); found != destructibles.end())
                return found->second;
            Destructible definition;
            if (base)
                TES5Viewer::visitSubrecords(read(base), [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("DEST") && bytes.size() >= 8)
                        definition.health = static_cast<float>(std::bit_cast<std::int32_t>(word(bytes)));
                    if (tag == ESM::fourCC("DSTD") && bytes.size() >= 20)
                        definition.stages.push_back({std::to_integer<unsigned>(bytes[0]),
                            std::to_integer<unsigned>(bytes[2]), std::to_integer<unsigned>(bytes[3])});
                    if (tag == ESM::fourCC("DMDL") && !definition.stages.empty())
                        definition.stages.back().model = TES5Viewer::modelAssetPath(TES5Viewer::characterString(bytes));
                });
            return destructibles.emplace(base, std::move(definition)).first->second;
        }

        bool damageObject(std::uint32_t id, float amount)
        {
            if (!std::isfinite(amount) || amount <= 0 || isDisabled(id)) return false;
            const auto& definition = destruction(referenceBase(id));
            if (!(definition.health > 0) || definition.stages.empty()) return false;
            auto& state = damagedObjects.try_emplace(id, ObjectDamage{definition.health}).first->second;
            if (state.health <= 0) return false;
            if (state.next && (definition.stages[state.next - 1].flags & 8)) return false;
            state.health = std::max(0.f, state.health - amount);
            while (state.next < definition.stages.size())
            {
                const auto& stage = definition.stages[state.next];
                const auto threshold = definition.health * (stage.percent / 100.f);
                if (state.health > threshold) break;
                ++state.next;
                const auto previous = std::exchange(state.stage, stage.modelStage);
                state.model = stage.model;
                state.noCollision = (stage.flags & 6) || !stage.percent;
                if (stage.flags & 6) overrides[id] = true;
                tracked.insert(id);
                if (previous != state.stage)
                    scripts.destructionStage(id, previous, state.stage);
                if (stage.flags & 1)
                {
                    state.health = threshold;
                    break;
                }
            }
            return true;
        }

        void fadeImage(std::uint32_t id, float strength, double seconds)
        {
            if (!id) return;
            if (!(seconds > 0) || !std::isfinite(seconds))
            {
                imageFades.erase(id);
                if (strength == 0) visuals.effects.erase(id);
                else visuals.effects[id] = {time, strength};
                return;
            }
            auto& effect = visuals.effects.try_emplace(id, TES5Viewer::PresentationState::Effect{time, 0}).first->second;
            imageFades[id] = {time, seconds, effect.strength, strength};
        }

        void tickImageFades()
        {
            for (auto it = imageFades.begin(); it != imageFades.end();)
            {
                const auto& fade = it->second;
                const auto alpha = std::clamp((time - fade.started) / fade.seconds, 0., 1.);
                if (const auto effect = visuals.effects.find(it->first); effect != visuals.effects.end())
                {
                    effect->second.strength = std::lerp(fade.from, fade.to, static_cast<float>(alpha));
                    if (alpha >= 1 && fade.to == 0) visuals.effects.erase(effect);
                }
                if (alpha >= 1) it = imageFades.erase(it);
                else ++it;
            }
        }

        bool playModelAnimation(std::uint32_t id, std::string_view event, bool furnitureOnly = false, bool preview = false)
        {
            const auto baseId = referenceBase(id);
            if (!baseId || event.empty()) return false;
            const auto base = read(baseId);
            if (furnitureOnly && base.metadata.recordType != ESM::fourCC("FURN")) return false;
            auto model = objectModels.find(baseId);
            const auto readHkx = [this](const std::string& path) {
                const auto stream = vfs->get(VFS::Path::Normalized(path));
                return std::string(std::istreambuf_iterator<char>(*stream), std::istreambuf_iterator<char>{});
            };
            if (model == objectModels.end())
            {
                ObjectModel definition;
                std::string path;
                TES5Viewer::visitSubrecords(base, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("MODL") && !bytes.empty())
                        path = TES5Viewer::modelAssetPath(TES5Viewer::characterString(bytes));
                });
                if (!path.empty() && vfs->exists(VFS::Path::Normalized(path)))
                {
                    Nif::NIFFile file{VFS::Path::Normalized(path)};
                    Nif::Reader(file, nullptr).parse(vfs->get(VFS::Path::Normalized(path)));
                    for (const auto& block : file.mRecords)
                        if (const auto* sequence = dynamic_cast<const Nif::NiControllerSequence*>(block.get()))
                        {
                            const auto duration = sequence->mStopTime - sequence->mStartTime;
                            if (std::isfinite(duration) && duration > 0 && duration <= 30)
                                definition.sequences.emplace(Misc::StringUtils::lowerCase(sequence->mName), duration);
                        }
                        else if (const auto* graph = dynamic_cast<const Nif::BSBehaviorGraphExtraData*>(block.get()))
                            if (!graph->mFile.empty())
                                definition.graph = TES5Viewer::modelAssetPath(graph->mFile);
                }
                if (!definition.graph.empty() && vfs->exists(VFS::Path::Normalized(definition.graph)))
                {
                    const auto project = definition.graph;
                    definition.graph = TES5Viewer::hkxBehaviorPath(project, readHkx);
                    definition.rig = TES5Viewer::hkxObjectRig(project, readHkx);
                }
                else
                    definition.graph.clear();
                model = objectModels.emplace(baseId, std::move(definition)).first;
            }
            const auto key = Misc::StringUtils::lowerCase(event);
            auto& events = model->second.events;
            auto selection = events.find(key);
            if (selection == events.end())
            {
                TES5Viewer::HkxObjectAnimation resolved;
                if (!model->second.graph.empty() && vfs->exists(VFS::Path::Normalized(model->second.graph)))
                    resolved = TES5Viewer::hkxObjectAnimation(readHkx(model->second.graph), event);
                if (resolved.sequence.empty()) resolved.sequence = key;
                selection = events.emplace(key, std::move(resolved)).first;
            }
            const auto& resolved = selection->second;
            const auto sequence = model->second.sequences.find(resolved.sequence);
            auto clipPath = std::string{};
            const float sequenceDuration = sequence == model->second.sequences.end() ? 0 : sequence->second;
            float duration = sequenceDuration;
            float begin = 0, end = duration, start = 0, speed = 1;
            if (!resolved.clip.empty() && !model->second.rig.empty())
            {
                clipPath = (std::filesystem::path(model->second.graph).parent_path().parent_path() / resolved.clip)
                    .lexically_normal().generic_string();
                if (resolved.clip.starts_with("meshes/")) clipPath = resolved.clip;
                if (vfs->exists(VFS::Path::Normalized(clipPath)))
                {
                    begin = resolved.cropStart;
                    end = TES5Viewer::hkxClipDuration(readHkx(clipPath)) - resolved.cropEnd;
                    start = begin + resolved.start;
                    speed = resolved.speed;
                    if (!std::isfinite(start) || !std::isfinite(speed) || !(speed > 0) || start < begin || start > end)
                        return false;
                    duration = std::max(sequenceDuration, (end - start) / speed);
                }
                else
                    clipPath.clear();
            }
            if (!std::isfinite(duration) || duration <= 0) return false;
            if (preview) return true;
            if (furnitureOnly && modelAnimations.contains(id)) return true;
            std::vector<std::pair<float, std::string>> notifications;
            const auto addEvents = [&](const auto& entries, float origin) {
                for (const auto& [at, name] : entries)
                    if (const auto keyTime = at + origin; keyTime >= start && keyTime <= end)
                        notifications.emplace_back((keyTime - start) / speed, name);
            };
            addEvents(resolved.events, begin);
            addEvents(resolved.endRelativeEvents, end);
            std::ranges::sort(notifications);
            notifications.erase(std::unique(notifications.begin(), notifications.end()), notifications.end());
            finishModelWait(id);
            modelSequenceLayers.erase(id);
            if (clipPath.empty())
                modelClips.erase(id);
            else
            {
                modelClips.insert_or_assign(id, ModelClip{model->second.rig, start, speed});
                if (sequence != model->second.sequences.end())
                    modelSequenceLayers.insert_or_assign(id, sequence->first);
            }
            modelAnimations.insert_or_assign(id, ModelAnimation{
                clipPath.empty() ? sequence->first : clipPath, time, duration, std::move(notifications)});
            tracked.insert(id);
            return true;
        }

        void finishModelWait(std::uint32_t id, std::string_view event = {})
        {
            if (const auto it = modelAnimationWaits.find(id); it != modelAnimationWaits.end()
                && (event.empty() || Misc::StringUtils::ciEqual(it->second.event, event)))
            {
                *it->second.result = !event.empty();
                *it->second.finished = true;
                modelAnimationWaits.erase(it);
            }
        }

        void updateModelAnimations()
        {
            for (auto& [id, animation] : modelAnimations)
            {
                const auto elapsed = time - animation.started;
                if (isDisabled(id))
                {
                    finishModelWait(id);
                    continue;
                }
                auto event = animation.events.begin();
                while (event != animation.events.end() && event->first <= elapsed)
                {
                    finishModelWait(id, event->second);
                    quests.animationEvent(id, event->second);
                    scripts.animationEvent(id, event->second);
                    ++event;
                }
                animation.events.erase(animation.events.begin(), event);
                if (elapsed >= animation.duration)
                    finishModelWait(id);
            }
        }

        static std::uint32_t word(std::span<const std::byte> bytes, std::size_t offset = 0)
        {
            if (offset > bytes.size() || bytes.size() - offset < 4)
                throw std::runtime_error("Truncated record data");
            return TES5Viewer::ActorInventory::word(bytes.subspan(offset, 4));
        }

        static float real(std::span<const std::byte> bytes, std::size_t offset)
        {
            return std::bit_cast<float>(word(bytes, offset));
        }

        static float numeric(const Papyrus::Value& value)
        {
            if (const auto* number = std::get_if<float>(&value)) return *number;
            return static_cast<float>(std::get<std::int32_t>(value));
        }

        static Papyrus::Value formValue(std::uint32_t id)
        {
            return id ? Papyrus::Value{Papyrus::FormReference{id}} : Papyrus::Value{};
        }

        std::pair<std::int32_t, bool>& lockState(std::uint32_t id)
        {
            if (const auto it = locks.find(id); it != locks.end()) return it->second;
            auto& state = locks[id];
            TES5Viewer::visitSubrecords(read(id), [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("XLOC") && !bytes.empty())
                    state = {std::to_integer<std::uint8_t>(bytes[0]), true};
            });
            return state;
        }

        std::uint32_t magicHandle()
        {
            if (nextMagicHandle <= 0xff800000)
                throw std::runtime_error("Magic handle space exhausted");
            return nextMagicHandle--;
        }

        std::vector<std::uint32_t> areaActors(std::uint32_t center, std::uint32_t feet)
        {
            std::vector<std::uint32_t> actors;
            if (world.actorBase(center)) actors.push_back(center);
            const auto pose = world.placement(center);
            if (!feet || !pose) return actors;
            const float radius = static_cast<float>(feet) * .3048f * Constants::UnitsPerMeter;
            for (const auto actor : world.loadedActors())
                if (actor != center && !isDisabled(actor))
                    if (const auto target = world.placement(actor); target && world.sameSpace(center, pose->cell, actor, target->cell)
                        && (target->position.asVec3() - pose->position.asVec3()).length2() <= radius * radius)
                        actors.push_back(actor);
            return actors;
        }

        void applyMagic(const MagicFlight& magic, std::uint32_t target)
        {
            if (!target || !world.actorBase(target) || isDisabled(target)) return;
            const auto effect = read(magic.effect);
            std::vector<std::span<const std::byte>> conditions;
            TES5Viewer::visitSubrecords(effect, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("CTDA")) conditions.push_back(bytes);
            });
            if (!quests.testConditions(0, effect, conditions, target)) return;
            const auto [cache, added] = magicScripts.try_emplace(magic.effect);
            if (added)
            {
                TES5Viewer::RecordAttachment attachment;
                attachment.formId = magic.effect;
                attachment.recordType = effect.metadata.recordType;
                attachment.sourcePlugin = effect.metadata.sourcePlugin;
                attachment.resolveFormId = effect.mapFormId;
                TES5Viewer::visitSubrecords(effect, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("VMAD")) attachment.vmad.assign(bytes.begin(), bytes.end());
                });
                if (!attachment.vmad.empty())
                {
                    Papyrus::ScriptBindings bindings;
                    Papyrus::applyVmad(bindings, attachment);
                    for (const auto& [name, binding] : bindings)
                        cache->second.push_back({0, TES5Viewer::checkpointScript(*vfs, name), attachment.sourcePlugin,
                            Papyrus::resolveProperties(binding, TES5Viewer::questAliasResolver(read))});
                }
            }
            auto prepared = cache->second;
            if (!prepared.empty())
            {
                const auto handle = magicHandle();
                for (auto& entry : prepared) entry.owner = handle;
                scripts.addEffect(handle, target, magic.caster, time + magic.duration, prepared);
            }
            if (magic.projectile) quests.actorHit(target, magic.caster, magic.spell, magic.projectile);
        }

        void magicImpact(std::uint32_t shot, std::uint32_t target)
        {
            const auto entry = magicFlights.extract(shot);
            if (entry.empty()) return;
            const auto& magic = entry.mapped();
            if (!target || (magic.expectedTarget && target != magic.expectedTarget)) return;
            for (const auto actor : areaActors(target, magic.cone ? 0 : magic.area))
                applyMagic(magic, actor);
        }

        void launchMagic(MagicFlight magic, const Cast& cast)
        {
            const auto source = world.placement(cast.source);
            const auto target = world.placement(cast.target);
            if (!source || !target || !world.sameSpace(cast.source, source->cell, cast.target, target->cell)) return;
            const auto projectile = read(magic.projectile);
            std::string model;
            std::uint32_t type = 0;
            float speed = 0, range = 0, gravity = 0, radius = 0, lifetime = 0, spread = 0;
            TES5Viewer::visitSubrecords(projectile, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("MODL")) model = TES5Viewer::characterString(bytes);
                if (tag != ESM::fourCC("DATA") || bytes.size() < 80) return;
                type = word(bytes) >> 16;
                gravity = real(bytes, 4);
                speed = real(bytes, 8);
                range = real(bytes, 12);
                spread = real(bytes, 68);
                radius = real(bytes, 72);
                lifetime = real(bytes, 76);
            });
            if ((type != 1 && type != 2 && type != 0x10) || !(speed > 0 && range > 0))
                throw std::runtime_error("Unsupported magic projectile motion");
            if (lifetime <= 0) lifetime = range / speed;
            magic.cone = type == 0x10;
            magic.expires = time + lifetime + 1;
            const auto start = source->position.asVec3();
            const auto aim = target->position.asVec3();
            const auto emit = [&](std::uint32_t actor, const osg::Vec3f& end, bool showModel) {
                const auto id = magicHandle();
                magic.expectedTarget = magic.cone ? actor : 0;
                magicFlights.emplace(id, magic);
                presentation.push_back(std::format(
                    "[\"spellProjectile\",{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}]",
                    id, cast.caster, cast.source == cast.caster ? cast.source : 0, actor,
                    start.x(), start.y(), start.z(), end.x(), end.y(), end.z(), speed, gravity, range, radius, lifetime,
                    jsonText(showModel ? model : std::string{})));
            };
            if (!magic.cone)
            {
                emit(world.actorBase(cast.target) ? cast.target : 0, aim, true);
                return;
            }
            auto direction = aim - start;
            if (direction.normalize() == 0) return;
            const auto minDot = std::cos(std::clamp(spread, 0.f, 180.f) * std::numbers::pi_v<float> / 180.f);
            if (!world.actorBase(cast.target))
                emit(cast.target, aim, true);
            for (const auto actor : world.loadedActors())
            {
                if (actor == cast.caster || isDisabled(actor)) continue;
                const auto pose = world.placement(actor);
                if (!pose || !world.sameSpace(cast.source, source->cell, actor, pose->cell)) continue;
                auto offset = pose->position.asVec3() - start;
                const auto distance = offset.normalize();
                if (distance > 0 && distance <= range && direction * offset >= minDot)
                    emit(actor, pose->position.asVec3(), actor == cast.target);
            }
        }

        void tickMagic()
        {
            std::erase_if(magicFlights, [&](const auto& entry) { return entry.second.expires < time; });
            for (const auto& [spell, actor, target] : world.takeSpellCasts())
                spellQueue.push_back({spell, actor, actor, target});
            for (const auto& cast : std::exchange(spellQueue, {}))
                try
                {
                    world.playCastSounds(cast.spell, cast.caster);
                    const auto spell = read(cast.spell);
                    std::uint32_t delivery = 0;
                    std::vector<MagicFlight> effects;
                    TES5Viewer::visitSubrecords(spell, [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("SPIT")) delivery = word(bytes, 20);
                        if (tag == ESM::fourCC("EFID"))
                            effects.push_back({cast.caster, cast.spell, spell.mapFormId(word(bytes))});
                        if (tag == ESM::fourCC("EFIT") && !effects.empty())
                        {
                            effects.back().area = word(bytes, 4);
                            effects.back().duration = word(bytes, 8);
                        }
                    });
                    for (auto& magic : effects)
                    {
                        if (delivery == 2)
                        {
                            const auto effect = read(magic.effect);
                            TES5Viewer::visitSubrecords(effect, [&](auto tag, auto bytes) {
                                if (tag == ESM::fourCC("DATA")) magic.projectile = effect.mapFormId(word(bytes, 72));
                            });
                            if (magic.projectile)
                            {
                                launchMagic(magic, cast);
                                continue;
                            }
                        }
                        for (const auto actor : areaActors(delivery == 0 ? cast.caster : cast.target, magic.area))
                            applyMagic(magic, actor);
                    }
                }
                catch (const std::exception& error)
                {
                    log.write("Spell.Cast " + std::to_string(cast.spell), error.what());
                }
        }

        std::string objectiveMarkers()
        {
            std::string result = "[\"objectives\"";
            const auto player = world.placement(0x14);
            if (player)
                for (const auto id : quests.objectiveTargets())
                    if (const auto pose = world.placement(id); pose && world.sameSpace(0x14, player->cell, id, pose->cell))
                        result += std::format(",[{},{},{},{}]", id, pose->position.pos[0], pose->position.pos[1], pose->position.pos[2]);
            result += ']';
            return result;
        }

        std::uint32_t formType(std::uint32_t id) const
        {
            if (id == 0x14u)
                return ESM::fourCC("ACHR");
            if (const auto& created = quests.createdReferences(); created.contains(id))
                return created.at(id).actor ? ESM::fourCC("ACHR") : ESM::fourCC("REFR");
            return read(id).metadata.recordType;
        }

        bool isDisabled(std::uint32_t id, unsigned depth = 0) const
        {
            if (depth >= 32)
                throw std::runtime_error("Cyclic Papyrus enable parent chain");
            if (const auto change = overrides.find(id); change != overrides.end())
                return change->second;
            if (id == 0x14u)
                return false;
            if (const auto parent = enableParents.find(id); parent != enableParents.end())
                return isDisabled(parent->second.first, depth + 1) != parent->second.second;
            if (const auto known = report.visibility.find(id); known != report.visibility.end())
                return !known->second;
            if (const auto& created = quests.createdReferences(); created.contains(id))
                return created.at(id).initiallyDisabled;
            const auto record = read(id);
            const auto type = record.metadata.recordType;
            if (type != ESM::fourCC("REFR") && type != ESM::fourCC("ACHR"))
                throw std::runtime_error("Papyrus operation requires a placed reference");
            return (record.flags & 0x800) != 0;
        }

        void setWeather(std::uint32_t id, bool overrideWeather)
        {
            if (weatherOverride && !overrideWeather)
                return;
            const auto record = read(id);
            std::array<float, 12> day{};
            bool haveColors = false;
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("NAM0") && bytes.size() >= 5 * 16)
                {
                    const std::array<std::size_t, 3> slots{4, 3, 1}; // Day sunlight, ambient and near fog.
                    for (std::size_t i = 0; i < slots.size(); ++i)
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            day[i * 3 + channel] = std::to_integer<unsigned>(bytes[slots[i] * 16 + 4 + channel]) / 255.f;
                    haveColors = true;
                }
                else if (tag == ESM::fourCC("FNAM") && bytes.size() >= 8 * sizeof(float))
                {
                    std::memcpy(&day[9], bytes.data(), 2 * sizeof(float));
                    std::memcpy(&day[11], bytes.data() + 6 * sizeof(float), sizeof(float));
                }
            });
            if (!haveColors || !std::ranges::all_of(day, [](float value) { return std::isfinite(value); }))
                throw std::runtime_error("Weather.ForceActive requires valid WTHR colors and fog");
            weather = id;
            weatherOverride = overrideWeather;
            weatherDay = day;
            environment.force(id,overrideWeather);
        }

        bool isDoor(std::uint32_t id) const
        {
            if (formType(id) != ESM::fourCC("REFR"))
                return false;
            const auto reference = read(id);
            std::uint32_t base = 0;
            TES5Viewer::visitSubrecords(reference, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("NAME") && bytes.size() == 4)
                {
                    std::uint32_t local;
                    std::memcpy(&local, bytes.data(), 4);
                    base = reference.mapFormId(local);
                }
            });
            return base && formType(base) == ESM::fourCC("DOOR");
        }

        void updateRelocations()
        {
            for (const auto& [id, ref] : world.movingReferences())
                if (!quests.createdReferences().contains(id) && !relocated.contains(id))
                    relocated.emplace(id, SpawnedReference{ id, ref.base, ref.anchor,
                        (ref.recordType == ESM::fourCC("ACHR") ? 1u : 0u) | (isDisabled(id) ? 2u : 0u),
                        0, {}, ref.scale });
        }

        std::string messageText(const TES5Viewer::RecordPayload& record)
        {
            std::string text;
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag != ESM::fourCC("DESC")) return;
                if (bytes.size() != 4 || (bytes.back() == std::byte{0} && std::all_of(bytes.begin(), bytes.end()-1,
                    [](auto b) { const auto c = std::to_integer<unsigned>(b); return c >= 32 && c <= 126; })))
                { text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size()); if (!text.empty() && !text.back()) text.pop_back(); return; }
                std::uint32_t id; std::memcpy(&id, bytes.data(), 4);
                const auto stem = std::filesystem::path(record.metadata.sourcePlugin).stem().string();
                for (const auto language : {"english", "russian"})
                {
                    const VFS::Path::Normalized path("strings/" + stem + "_" + language + ".dlstrings");
                    if (!vfs->exists(path)) continue;
                    const auto stream = vfs->get(path);
                    const std::string data{std::istreambuf_iterator<char>(*stream), {}};
                    const auto word = [&](std::size_t at) {
                        if (at + 4 > data.size()) throw std::runtime_error("Truncated message string table");
                        std::uint32_t value; std::memcpy(&value, data.data() + at, 4); return value;
                    };
                    const auto count = word(0);
                    if (count > (data.size() - 8) / 8) throw std::runtime_error("Invalid message string directory");
                    for (std::size_t i = 0; i < count; ++i)
                        if (word(8 + i * 8) == id)
                        {
                            const std::size_t at = 8ull + count * 8ull + word(12 + i * 8);
                            const auto length = word(at);
                            if (!length || at + 4ull + length > data.size()) throw std::runtime_error("Invalid message string length");
                            text.assign(data.data() + at + 4, length - 1); return;
                        }
                }
                throw std::runtime_error("Localized MESG text was not found");
            });
            return text;
        }

        std::optional<Papyrus::Value> callGame(std::string_view name, const std::vector<Papyrus::Value>& args)
        {
            const auto method = Misc::StringUtils::lowerCase(name);
            const auto boolean = [&](std::size_t i, bool fallback) {
                return i < args.size() ? std::get<bool>(args[i]) : fallback;
            };
            const auto setFlag = [&](std::uint32_t flag, bool enabled) {
                gameFlags = enabled ? gameFlags | flag : gameFlags & ~flag;
            };
            if (method == "teachword" && args.size() == 1)
            {
                if (const auto* word = std::get_if<Papyrus::FormReference>(&args.front());
                    word && word->formId && formType(word->formId) == ESM::fourCC("WOOP")
                    && learnedWords.insert(word->formId).second)
                {
                    const auto record = read(word->formId);
                    std::string text;
                    TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("FULL")) text = localizedText(record, bytes);
                    });
                    presentation.push_back("[\"help\"," + jsonText("Изучено слово силы: " + text) + ",5]");
                }
                return Papyrus::Value{};
            }
            if (method == "usinggamepad" && args.empty())
                return Papyrus::Value{visuals.gamepad};
            if (method == "showfirstpersongeometry" && args.size() <= 1)
                setFlag(1, boolean(0, true));
            else if (method == "sethudcartmode" && args.size() <= 1)
                setFlag(2, boolean(0, true));
            else if (method == "setplayeraidriven" && args.size() <= 1)
                setFlag(4, boolean(0, true));
            else if (method == "setsittingrotation" && args.size() == 1)
            {
                const auto* integer = std::get_if<std::int32_t>(&args[0]);
                const float degrees = integer ? static_cast<float>(*integer) : std::get<float>(args[0]);
                if (!std::isfinite(degrees))
                    throw std::runtime_error("Game.SetSittingRotation requires finite degrees");
                sittingRotation = degrees;
            }
            else if (method == "setinchargen" && args.size() == 3)
            {
                setFlag(8, boolean(0, false));
                setFlag(16, boolean(1, false));
                setFlag(32, boolean(2, false));
            }
            else if ((method == "requestsave" || method == "requestautosave") && args.empty())
                presentation.push_back("[" + jsonText(method) + "]");
            else if (method == "precachechargen" && args.empty())
            {
                // UE builds the character preview on demand from the mounted VFS.
            }
            else if (method == "showracemenu" && args.empty())
            {
                setFlag(raceMenuPending, true);
            }
            else if ((method == "showtitlesequencemenu" || method == "hidetitlesequencemenu") && args.empty())
                presentation.push_back("[" + jsonText(method) + "]");
            else if (method == "starttitlesequence" && args.size() == 1)
                presentation.push_back("[" + jsonText(method) + "," + jsonText(std::get<std::string>(args[0])) + "]");
            else if (method == "fadeoutgame" && args.size() == 4)
            {
                const auto seconds = [&](std::size_t i) {
                    if (const auto* whole = std::get_if<std::int32_t>(&args[i]))
                        return static_cast<float>(*whole);
                    return std::get<float>(args[i]);
                };
                const float delay = seconds(2), duration = seconds(3);
                if (!std::isfinite(delay) || !std::isfinite(duration) || delay < 0 || duration < 0)
                    throw std::runtime_error("Game.FadeOutGame requires finite nonnegative times");
                std::ostringstream command;
                command << "[\"fadeoutgame\"," << boolean(0, false) << ',' << boolean(1, true)
                    << ',' << delay << ',' << duration << ']';
                presentation.push_back(std::move(command).str());
            }
            else if (method == "disableplayercontrols" || method == "enableplayercontrols")
            {
                const bool enable = method == "enableplayercontrols";
                constexpr std::uint32_t defaults = 0b01100011;
                if (args.size() > 9 || (args.size() == 9 && std::get<std::int32_t>(args[8]) != 0))
                    return std::nullopt;
                for (unsigned i = 0; i < 8; ++i)
                    if (boolean(i, enable || (defaults & (1 << i))))
                        controls = enable ? controls & ~(1 << i) : controls | (1 << i);
            }
            else
                return std::nullopt;
            return Papyrus::Value{};
        }

        Papyrus::HostCallbacks makeHost(bool keepGoing)
        {
            Papyrus::HostCallbacks host;
            host.isDisabled = [this](std::uint32_t id) { return isDisabled(id); };
            host.setDisabled = [this](std::uint32_t id, bool disabled, bool) {
                overrides.insert_or_assign(id, disabled);
                tracked.insert(id);
            };
            host.getPlayer = [] { return 0x14u; };
            host.getActorValue = [this](auto actor, auto value) { return world.getActorValue(actor, value); };
            host.isFormType = [this](std::uint32_t id, std::string_view type) {
                const auto normalized = Misc::StringUtils::lowerCase(type);
                try
                {
                    const auto tag = formType(id);
                    if (normalized == "form")
                        return true;
                    if (normalized == "actor")
                        return tag == ESM::fourCC("ACHR");
                    if (normalized == "objectreference")
                        return tag == ESM::fourCC("ACHR") || tag == ESM::fourCC("REFR");
                    return (normalized == "quest" && tag == ESM::fourCC("QUST"))
                        || (normalized == "cell" && tag == ESM::fourCC("CELL"))
                        || (normalized == "location" && tag == ESM::fourCC("LCTN"))
                        || (normalized == "globalvariable" && tag == ESM::fourCC("GLOB"))
                        || (normalized == "scene" && tag == ESM::fourCC("SCEN"))
                        || (normalized == "package" && tag == ESM::fourCC("PACK"))
                        || (normalized == "idle" && tag == ESM::fourCC("IDLE"))
                        || (normalized == "musictype" && tag == ESM::fourCC("MUSC"))
                        || (normalized == "actorbase" && tag == ESM::fourCC("NPC_"))
                        || (normalized == "race" && tag == ESM::fourCC("RACE"))
                        || (normalized == "keyword" && tag == ESM::fourCC("KYWD"))
                        || (normalized == "weather" && tag == ESM::fourCC("WTHR"))
                        || (normalized == "spell" && tag == ESM::fourCC("SPEL"))
                        || (normalized == "imagespacemodifier" && tag == ESM::fourCC("IMAD"))
                        || (normalized == "armor" && tag == ESM::fourCC("ARMO"));
                }
                catch (const std::exception&)
                {
                    return false;
                }
            };
            host.getOwningQuest = [this](auto id) { return world.owningQuest(id); };
            host.getEditorCell = [this](auto id) { return world.editorCell(id); };
            host.getCurrentCell = [this](auto id) {
                const auto pose = world.placement(id);
                return pose ? pose->cell : 0;
            };
            host.callLatentFormMethod = [this](auto id, std::string_view method, const auto& args)
                -> std::optional<Papyrus::HostCallbacks::ScriptCall> {
                if (!Misc::StringUtils::ciEqual(method, "PlayAnimationAndWait")) return std::nullopt;
                if (args.size() != 2) throw std::runtime_error("PlayAnimationAndWait requires two event names");
                const auto& completion = std::get<std::string>(args[1]);
                if (completion.empty() || !playModelAnimation(id, std::get<std::string>(args[0])))
                    return Papyrus::HostCallbacks::ScriptCall{false};
                const auto& wait = modelAnimationWaits.emplace(id, ModelAnimationWait{completion}).first->second;
                return Papyrus::HostCallbacks::ScriptCall{false, wait.finished, wait.result};
            };
            host.getFormProperty = [this](auto id, auto name) { return world.getFormProperty(id, name); };
            host.callFormMethod = [this](std::uint32_t id, std::string_view name, const std::vector<Papyrus::Value>& args)
                -> std::optional<Papyrus::Value> {
                const auto method = Misc::StringUtils::lowerCase(name);
                const auto form = [&](std::size_t i) -> std::uint32_t {
                    if (i >= args.size() || std::holds_alternative<std::monostate>(args[i])) return 0;
                    return std::get<Papyrus::FormReference>(args[i]).formId;
                };
                if (method == "activate" && !args.empty() && args.size() <= 2)
                    return Papyrus::Value(activateReference(id, form(0), false,
                        args.size() == 2 && std::get<bool>(args[1])));
                if (method == "damageobject" && args.size() == 1)
                {
                    damageObject(id, numeric(args.front()));
                    return Papyrus::Value{};
                }
                if (method == "getcurrentdestructionstage" && args.empty())
                {
                    const auto it = damagedObjects.find(id);
                    return Papyrus::Value(static_cast<std::int32_t>(it == damagedObjects.end() ? 0 : it->second.stage));
                }
                if (method == "isindialoguewithplayer" && args.empty())
                    return Papyrus::Value(dialogue.actor == id);
                if (method == "getoffersservicesnow" && args.empty())
                    return Papyrus::Value(vendorFaction(id) != 0);
                if (method == "showbartermenu" && args.empty())
                {
                    openBarter(id);
                    return Papyrus::Value{};
                }
                if (method == "haslos" && args.size() == 1 && lineOfSight)
                {
                    const auto target = form(0);
                    const auto sourcePose = world.placement(id);
                    const auto targetPose = target ? world.placement(target) : std::nullopt;
                    return Papyrus::Value(sourcePose && targetPose && !isDisabled(id) && !isDisabled(target)
                        && world.sameSpace(id, sourcePose->cell, target, targetPose->cell)
                        && lineOfSight(visibilityContext, id, target) != 0);
                }
                if ((method == "playanimation" || method == "playfurnitureanimation" || method == "hasfurnitureanimation")
                    && args.size() == 1)
                    return Papyrus::Value(playModelAnimation(id, std::get<std::string>(args.front()),
                        method != "playanimation", method == "hasfurnitureanimation"));
                if (method == "setnofavorallowed" && args.size() <= 1)
                {
                    if (args.empty() || std::get<bool>(args.front())) noFavorAllowed.insert(id);
                    else noFavorAllowed.erase(id);
                    return Papyrus::Value{};
                }
                if (method == "getopenstate" && args.empty())
                {
                    if (!isDoor(id)) return Papyrus::Value(std::int32_t(0));
                    const auto it = doorStates.find(id);
                    return Papyrus::Value(std::int32_t(it != doorStates.end() && it->second.first ? 1 : 3));
                }
                if (method == "getlength" && args.empty())
                {
                    const auto base = referenceBase(id);
                    if (!base) return std::nullopt;
                    float length = 0, scale = 1;
                    TES5Viewer::visitSubrecords(read(base), [&](auto tag, auto bytes) {
                        if (tag != ESM::fourCC("OBND") || bytes.size() < 12) return;
                        std::int16_t low, high;
                        std::memcpy(&low, bytes.data() + 2, 2);
                        std::memcpy(&high, bytes.data() + 8, 2);
                        length = static_cast<float>(std::max(0, high - low));
                    });
                    TES5Viewer::visitSubrecords(read(id), [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("XSCL")) scale = real(bytes, 0);
                    });
                    return Papyrus::Value(length * scale);
                }
                if (method == "knockareaeffect" && args.size() == 2)
                {
                    const auto pose = world.placement(id);
                    const auto strength = numeric(args[0]), radius = numeric(args[1]);
                    if (pose && std::isfinite(strength) && std::isfinite(radius) && radius > 0)
                        presentation.push_back(std::format("[\"radialImpulse\",{},{},{},{},{}]",
                            pose->position.pos[0], pose->position.pos[1], pose->position.pos[2], radius, strength));
                    return Papyrus::Value{};
                }
                if (method == "getcasteractor" || method == "getcaster" || method == "gettargetactor" || method == "gettarget")
                    if (args.empty())
                        if (const auto actor = scripts.effectActor(id, method.starts_with("getcaster")))
                            return formValue(actor);
                if ((method == "getsize" || method == "getat") && formType(id) == ESM::fourCC("FLST"))
                {
                    if (args.size() != (method == "getat" ? 1u : 0u)) return std::nullopt;
                    const auto index = args.empty() ? -1 : std::get<std::int32_t>(args.front());
                    const auto list = read(id);
                    std::int32_t size = 0;
                    std::uint32_t formId = 0;
                    TES5Viewer::visitSubrecords(list, [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("LNAM") && size++ == index)
                            formId = list.mapFormId(word(bytes));
                    });
                    return method == "getsize" ? Papyrus::Value(size) : formValue(formId);
                }
                if (method == "getclassification" && args.empty() && formType(id) == ESM::fourCC("WTHR"))
                {
                    std::int32_t classification = -1;
                    TES5Viewer::visitSubrecords(read(id), [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("DATA") && bytes.size() > 11)
                        {
                            const auto flags = std::to_integer<unsigned>(bytes[11]) & 15;
                            if (flags) classification = std::countr_zero(flags);
                        }
                    });
                    return Papyrus::Value(classification);
                }
                if ((method == "setkeyworddata" || method == "getkeyworddata") && formType(id) == ESM::fourCC("LCTN"))
                {
                    const bool write = method == "setkeyworddata";
                    if (args.size() != (write ? 2u : 1u) || !form(0) || formType(form(0)) != ESM::fourCC("KYWD"))
                        return std::nullopt;
                    const auto key = std::pair(id, form(0));
                    if (!write)
                    {
                        const auto it = locationData.find(key);
                        return Papyrus::Value(it == locationData.end() ? 0.f : it->second);
                    }
                    const auto value = numeric(args[1]);
                    if (!std::isfinite(value)) throw std::runtime_error("Location keyword data must be finite");
                    locationData.insert_or_assign(key, value);
                    return Papyrus::Value{};
                }
                if ((method == "cast" || method == "remotecast") && formType(id) == ESM::fourCC("SPEL"))
                {
                    const bool remote = method == "remotecast";
                    if (args.empty() || args.size() > (remote ? 3u : 2u) || (remote && args.size() < 2))
                        return std::nullopt;
                    const auto source = form(0);
                    const auto caster = remote ? form(1) : source;
                    const auto target = form(remote ? 2 : 1);
                    if (source && caster) spellQueue.push_back({id, source, caster, target ? target : source});
                    return Papyrus::Value{};
                }
                if (method == "placeatme")
                {
                    if (args.empty() || args.size() > 4) return std::nullopt;
                    const auto base = form(0);
                    const auto count = args.size() > 1 ? std::get<std::int32_t>(args[1]) : 1;
                    if (!base || count <= 0 || !world.placement(id)) return Papyrus::Value{};
                    if (count > 128) throw std::runtime_error("PlaceAtMe count exceeds runtime limit");
                    const bool disabled = args.size() > 3 && std::get<bool>(args[3]);
                    std::uint32_t first = 0;
                    for (std::int32_t n = 0; n < count; ++n)
                    {
                        const auto placed = quests.placeReference(base, id, disabled);
                        world.callFormMethod(placed, "MoveTo", {formValue(id)});
                        if (!first) first = placed;
                    }
                    return formValue(first);
                }
                if (method == "delete" && args.empty() && quests.createdReferences().contains(id))
                {
                    if (quests.deleteReference(id))
                    {
                        overrides.erase(id);
                        tracked.erase(id);
                        relocated.erase(id);
                        world.deletePlacement(id);
                    }
                    return Papyrus::Value{};
                }
                if (method == "applycrossfade" && args.size() <= 1 && formType(id) == ESM::fourCC("IMAD"))
                {
                    const auto seconds = args.empty() ? 1.f : numeric(args.front());
                    if (id != crossFadeImage)
                    {
                        fadeImage(std::exchange(crossFadeImage, id), 0, seconds);
                        fadeImage(id, 1, seconds);
                    }
                    return Papyrus::Value{};
                }
                if ((method == "apply" || method == "remove" || method == "popto") && formType(id) == ESM::fourCC("IMAD"))
                {
                    const bool pop = method == "popto";
                    if (args.size() > (pop ? 2u : method == "apply" ? 1u : 0u)
                        || (pop && (args.empty() || !form(0) || formType(form(0)) != ESM::fourCC("IMAD"))))
                        return std::nullopt;
                    const auto strengthIndex = pop ? 1u : 0u;
                    const float strength = args.size() <= strengthIndex ? 1.f : numeric(args[strengthIndex]);
                    if (!std::isfinite(strength)) throw std::runtime_error("ImageSpaceModifier strength must be finite");
                    if (method == "remove" || pop) visuals.effects.erase(id);
                    if (method != "remove")
                    {
                        visuals.effects[pop ? form(0) : id] = {time, strength};
                    }
                    return Papyrus::Value{};
                }
                if (method == "showashelpmessage" && formType(id) == ESM::fourCC("MESG"))
                {
                    if (args.size() != 4) return std::nullopt;
                    const auto& event = std::get<std::string>(args[0]);
                    const auto numeric = [&](std::size_t i) { return std::holds_alternative<float>(args[i])
                        ? std::get<float>(args[i]) : float(std::get<std::int32_t>(args[i])); };
                    const auto duration = numeric(1), interval = numeric(2), limit = numeric(3);
                    if (!std::isfinite(duration) || !std::isfinite(interval) || !std::isfinite(limit))
                        throw std::runtime_error("Message.ShowAsHelpMessage requires finite times and count");
                    auto [it, first] = visuals.help.try_emplace(event, -1e30, 0);
                    if ((limit <= 0 || it->second.second < limit) && time - it->second.first >= interval)
                    {
                        presentation.push_back("[\"help\"," + jsonText(messageText(read(id))) + "," + std::to_string(std::max(0.f, duration)) + "]");
                        it->second = {time, it->second.second + 1};
                    }
                    return Papyrus::Value{};
                }
                if (method == "show" && args.size() <= 9 && formType(id) == ESM::fourCC("MESG"))
                {
                    const auto message = read(id);
                    std::uint32_t flags = 0, duration = 2;
                    TES5Viewer::visitSubrecords(message, [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("DNAM")) flags = word(bytes);
                        if (tag == ESM::fourCC("TNAM")) duration = word(bytes);
                    });
                    if (flags & 1) return std::nullopt;
                    const auto text = messageText(message);
                    if (text.find('%') != std::string::npos) return std::nullopt;
                    presentation.push_back(std::format("[\"help\",{},{}]", jsonText(text), duration));
                    return Papyrus::Value(std::int32_t(-1));
                }
                if (method == "adddependentanimatedobjectreference" || method == "removedependentanimatedobjectreference")
                {
                    if (args.size() != 1) throw std::runtime_error("Invalid animation dependency arguments");
                    const auto target = form(0);
                    if (!target || id == target || !world.actorBase(id) || !world.actorBase(target)) return Papyrus::Value{false};
                    if (method == "adddependentanimatedobjectreference") animationDependencies[id].insert(target);
                    else animationDependencies[id].erase(target);
                    return Papyrus::Value{true};
                }
                if (method == "setrestrained")
                {
                    if (args.size() > 1 || !world.actorBase(id)) return std::nullopt;
                    const bool restrained = args.empty() || std::get<bool>(args[0]);
                    if (id == 0x14) gameFlags = restrained ? gameFlags | 128u : gameFlags & ~128u;
                    return Papyrus::Value{};
                }
                if (method == "blockactivation" && args.size() <= 1)
                {
                    if (args.empty() || std::get<bool>(args[0])) blockedActivation.insert(id);
                    else blockedActivation.erase(id);
                    return Papyrus::Value{};
                }
                if (method == "isactivationblocked" && args.empty())
                    return Papyrus::Value(blockedActivation.contains(id));
                if (method == "isenabled" && args.empty()) return Papyrus::Value(!isDisabled(id));
                if (method == "isequipped" && args.size() == 1)
                {
                    const auto item = form(0);
                    return Papyrus::Value(item && ((id == 0x14 && playerWeapon == item)
                        || (outfits.contains(id) && outfits.at(id).contains(item))));
                }
                if (id == 0x14 && method == "getequippeditemtype" && args.size() <= 1)
                {
                    const auto hand = args.empty() ? 0 : std::get<std::int32_t>(args[0]);
                    if (hand < 0 || hand > 1) return Papyrus::Value(std::int32_t{-1});
                    const auto type = weaponStats().type;
                    // The player has one equipment slot. Two-handed weapons occupy both hands.
                    if (hand == 1 && type != 5 && type != 6 && type != 7 && type != 9)
                        return Papyrus::Value(std::int32_t{0});
                    return Papyrus::Value(static_cast<std::int32_t>(type == 9 ? 12 : type));
                }
                if (method == "ignorefriendlyhits" && args.size() <= 1 && world.actorBase(id))
                {
                    if (args.empty() || std::get<bool>(args[0])) ignoreFriendlyHits.insert(id);
                    else ignoreFriendlyHits.erase(id);
                    return Papyrus::Value{};
                }
                if (method == "setnotshowonstealthmeter" && args.size() == 1 && world.actorBase(id))
                    return Papyrus::Value{}; // The host has no stealth meter.
                if (method == "setally" && !args.empty() && args.size() <= 3 && formType(id) == ESM::fourCC("FACT")
                    && form(0) && formType(form(0)) == ESM::fourCC("FACT"))
                {
                    // The optional booleans choose Friend versus Ally, both friendly for hits.
                    alliedFactions.emplace(id, form(0));
                    alliedFactions.emplace(form(0), id);
                    return Papyrus::Value{};
                }
                if (method == "setoutfit" || method == "unequipitem")
                {
                    if (args.empty() || args.size() > (method == "setoutfit" ? 2u : 3u) || !world.actorBase(id)) return std::nullopt;
                    const auto item = form(0);
                    if (!item) return Papyrus::Value{};
                    if (method == "unequipitem")
                    {
                        outfits[id].erase(item);
                        if (id == 0x14 && playerWeapon == item) playerWeapon = 0;
                    }
                    else
                    {
                        const auto outfit = read(item);
                        if (outfit.metadata.recordType != ESM::fourCC("OTFT")) return std::nullopt;
                        auto& equipped = outfits[id]; equipped.clear();
                        TES5Viewer::visitSubrecords(outfit, [&](auto tag, auto bytes) {
                            if (tag == ESM::fourCC("INAM")) equipped.insert(outfit.mapFormId(TES5Viewer::ActorInventory::word(bytes)));
                        });
                    }
                    return Papyrus::Value{};
                }
                if (method == "playidle")
                {
                    executionVictims.erase(id);
                    if (args.size() == 1 && form(0) && animationDependencies.contains(id) && !animationDependencies.at(id).empty())
                    {
                        std::string event;
                        TES5Viewer::visitSubrecords(read(form(0)), [&](auto tag, auto bytes) {
                            if (tag == ESM::fourCC("ENAM")) event = TES5Viewer::characterString(bytes);
                        });
                        if (Misc::StringUtils::ciEqual(event, "IdleExecutionerChop_Player"))
                        {
                            world.playPlayerExecution(id, true);
                            return Papyrus::Value{true};
                        }
                        if (Misc::StringUtils::ciEqual(event, "IdleExecutionerChop"))
                        {
                            const auto& linked = animationDependencies.at(id);
                            const std::vector<std::uint32_t> dependents(linked.begin(), linked.end());
                            executionVictims[id] = world.playExecution(id, dependents);
                            return Papyrus::Value{true};
                        }
                    }
                }
                if (method == "playidle" && args.size() == 1 && form(0))
                {
                    std::string event;
                    TES5Viewer::visitSubrecords(read(form(0)), [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("ENAM")) event = TES5Viewer::characterString(bytes);
                    });
                    if (Misc::StringUtils::ciEqual(event, "IdleExecutionerChop_Player"))
                    {
                        world.playPlayerExecution(id, id == 0x14);
                        return Papyrus::Value{true};
                    }
                }
                if (method == "addspell" || method == "removespell" || method == "hasspell")
                {
                    if (args.empty() || args.size() > (method == "addspell" ? 2u : 1u))
                        throw std::runtime_error("Invalid spell native arguments");
                    const auto actor = world.actorBase(id), spell = form(0);
                    if (!actor || !spell) return Papyrus::Value{false};
                    if (formType(spell) != ESM::fourCC("SPEL")) return Papyrus::Value{false};
                    auto& book = inventory.spellbook(id, actor, read);
                    if (method == "addspell") return Papyrus::Value{book.insert(spell).second};
                    if (method == "removespell") return Papyrus::Value{book.erase(spell) != 0};
                    return Papyrus::Value{book.contains(spell)};
                }
                if (method == "additem" || method == "removeitem" || method == "getitemcount")
                {
                    if (args.empty() || args.size() > (method == "getitemcount" ? 1u : method == "additem" ? 3u : 4u))
                        throw std::runtime_error("Invalid inventory native arguments");
                    auto base = world.actorBase(id);
                    if (!base && formType(id) == ESM::fourCC("REFR"))
                    {
                        const auto ref = read(id);
                        TES5Viewer::visitSubrecords(ref, [&](auto tag, auto bytes) {
                            if (tag == ESM::fourCC("NAME")) base = ref.mapFormId(TES5Viewer::ActorInventory::word(bytes));
                        });
                    }
                    if (!base) return std::nullopt;
                    const auto item = form(0);
                    syncQuestItems();
                    auto& contents = inventory.inventory(id, base, read);
                    if (method == "getitemcount")
                        return Papyrus::Value{item && contents.contains(item) ? contents.at(item) : 0};
                    const auto count = args.size() > 1 ? std::get<std::int32_t>(args[1]) : 1;
                    if (!item || count <= 0) return Papyrus::Value{};
                    if (method == "additem")
                    {
                        TES5Viewer::ActorInventory::add(contents, item, count);
                        quests.inventoryChanged(id, item, count, 0, true);
                    }
                    else
                    {
                        const auto removed = std::min(count, contents[item]);
                        if (const auto target = form(3); target && target != id && removed)
                        {
                            auto targetBase = world.actorBase(target);
                            if (!targetBase)
                            {
                                const auto ref = read(target);
                                TES5Viewer::visitSubrecords(ref, [&](auto tag, auto bytes) {
                                    if (tag == ESM::fourCC("NAME")) targetBase = ref.mapFormId(TES5Viewer::ActorInventory::word(bytes));
                                });
                            }
                            if (!targetBase) throw std::runtime_error("RemoveItem destination is not a container");
                            TES5Viewer::ActorInventory::add(inventory.inventory(target, targetBase, read), item, removed);
                            quests.inventoryChanged(target, item, removed, id, true);
                        }
                        if (form(3) != id && removed)
                        {
                            contents[item] -= removed;
                            quests.inventoryChanged(id, item, removed, form(3), false);
                            quests.transferInventoryReferences(item, id, form(3), removed);
                        }
                        if (!contents[item])
                        {
                            outfits[id].erase(item);
                            if (id == 0x14 && playerWeapon == item) playerWeapon = 0;
                        }
                    }
                    return Papyrus::Value{};
                }
                if (character && (id == 7 || id == 0x14) && args.empty())
                {
                    if (Misc::StringUtils::ciEqual(name, "GetRace")) return Papyrus::FormReference{character->race};
                    if (Misc::StringUtils::ciEqual(name, "GetSex")) return std::int32_t(character->female);
                    if (Misc::StringUtils::ciEqual(name, "GetName") || Misc::StringUtils::ciEqual(name, "GetDisplayName")) return characterName;
                }
                if (Misc::StringUtils::ciEqual(name, "ForceActive") && args.size() <= 1 && formType(id) == ESM::fourCC("WTHR"))
                {
                    setWeather(id, !args.empty() && std::get<bool>(args.front()));
                    return Papyrus::Value{};
                }
                if (Misc::StringUtils::ciEqual(name, "SetOpen") && args.size() <= 1 && isDoor(id))
                {
                    const bool open = args.empty() || std::get<bool>(args.front());
                    auto [door, added] = doorStates.try_emplace(id, open, open ? time : time - 30.);
                    if (!added && door->second.first != open)
                        door->second = {open, time};
                    tracked.insert(id);
                    if (open) lockState(id) = {};
                    return Papyrus::Value{};
                }
                if ((method == "setlocklevel" || method == "getlocklevel" || method == "lock" || method == "islocked")
                    && formType(id) == ESM::fourCC("REFR"))
                {
                    auto& [level, locked] = lockState(id);
                    if (method == "getlocklevel" && args.empty()) return Papyrus::Value(level);
                    if (method == "islocked" && args.empty()) return Papyrus::Value(locked);
                    if (method == "lock" && args.size() <= 2)
                    {
                        locked = args.empty() || std::get<bool>(args[0]);
                        return Papyrus::Value{};
                    }
                    if (method == "setlocklevel" && args.size() == 1)
                    {
                        level = std::clamp(std::get<std::int32_t>(args[0]), 0, 255);
                        return Papyrus::Value{};
                    }
                    return std::nullopt;
                }
                if (Misc::StringUtils::ciEqual(name, "RemoveAllItems") && args.size() == 3
                    && std::holds_alternative<std::monostate>(args[0]) && !std::get<bool>(args[1])
                    && !std::get<bool>(args[2]) && formType(id) == ESM::fourCC("ACHR"))
                {
                    auto& contents = inventory.inventory(id, world.actorBase(id), read);
                    for (const auto& [item, count] : contents)
                        if (count > 0) quests.inventoryChanged(id, item, count, 0, false);
                    contents.clear();
                    outfits[id].clear();
                    if (id == 0x14) playerWeapon = 0;
                    return Papyrus::Value{};
                }
                if (Misc::StringUtils::ciEqual(name, "EquipItem") && args.size() == 3
                    && formType(id) == ESM::fourCC("ACHR"))
                {
                    const auto item = std::get<Papyrus::FormReference>(args[0]).formId;
                    if (id == 0x14 && equipPlayer(item, false))
                    {
                        auto& contents = inventory.inventory(id, world.actorBase(id), read);
                        if (contents[item] <= 0) contents[item] = 1;
                        return Papyrus::Value{};
                    }
                    if (formType(item) != ESM::fourCC("ARMO") || std::get<bool>(args[1]))
                        return std::nullopt;
                    outfits[id].insert(item);
                    return Papyrus::Value{};
                }
                return world.callFormMethod(id, name, args);
            };
            host.callStatic = [this](std::string_view type, std::string_view name, const auto& args) {
                if (Misc::StringUtils::ciEqual(type, "ImageSpaceModifier")
                    && Misc::StringUtils::ciEqual(name, "RemoveCrossFade") && args.size() <= 1)
                {
                    fadeImage(std::exchange(crossFadeImage, 0), 0, args.empty() ? 1.f : numeric(args.front()));
                    return std::optional<Papyrus::Value>(Papyrus::Value{});
                }
                if (Misc::StringUtils::ciEqual(type, "Utility") && Misc::StringUtils::ciEqual(name, "RandomFloat")
                    && args.size() <= 2)
                {
                    const auto min = args.empty() ? 0.f : numeric(args[0]);
                    const auto max = args.size() < 2 ? 1.f : numeric(args[1]);
                    if (!std::isfinite(min) || !std::isfinite(max))
                        throw std::runtime_error("Utility.RandomFloat bounds must be finite");
                    randomState ^= randomState << 13;
                    randomState ^= randomState >> 17;
                    randomState ^= randomState << 5;
                    return std::optional<Papyrus::Value>(std::lerp(min, max, static_cast<float>(randomState >> 8) / 16777216.f));
                }
                if (Misc::StringUtils::ciEqual(type, "Utility") && Misc::StringUtils::ciEqual(name, "RandomInt")
                    && args.size() <= 2)
                {
                    auto low = args.empty() ? 0 : std::get<std::int32_t>(args[0]);
                    auto high = args.size() < 2 ? 100 : std::get<std::int32_t>(args[1]);
                    if (low > high) std::swap(low, high);
                    randomState ^= randomState << 13;
                    randomState ^= randomState >> 17;
                    randomState ^= randomState << 5;
                    const auto span = static_cast<std::uint64_t>(std::int64_t(high) - low) + 1;
                    return std::optional<Papyrus::Value>(static_cast<std::int32_t>(std::int64_t(low) + randomState % span));
                }
                if (Misc::StringUtils::ciEqual(type, "Game"))
                    return callGame(name, args);
                if (Misc::StringUtils::ciEqual(type, "Sound"))
                    return world.callSoundStatic(name, args);
                if (Misc::StringUtils::ciEqual(type, "Weather") && args.empty())
                {
                    if (Misc::StringUtils::ciEqual(name, "GetCurrentWeather"))
                        return std::optional<Papyrus::Value>(weather ? Papyrus::Value(Papyrus::FormReference{ weather }) : Papyrus::Value{});
                    if (Misc::StringUtils::ciEqual(name, "ReleaseOverride"))
                    {
                        weatherOverride = false;
                        environment.overrideId = 0;
                        return std::optional<Papyrus::Value>(Papyrus::Value{});
                    }
                }
                return std::optional<Papyrus::Value>{};
            };
            if (keepGoing)
                host.reportMissingNative = [this](std::string_view message) { log.write("Missing native", message); };
            return host;
        }
    };

    struct PapyrusSession
    {
        Papyrus::ErrorLog log;
        std::unique_ptr<PapyrusState> state;
        std::string messages;
        std::string errors;
        std::string animations;
        std::string audio;
        std::string presentation;
        std::string lastObjectives;
        std::string inventoryJson;
        std::uint32_t selectedQuest = 0;
        bool newGame = false;
        bool continueMissing = false;
        std::vector<std::pair<std::uint32_t, std::string>> animationEvents;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> impacts;

        PapyrusSession()
        {
            log.observe = [this](std::string_view context, std::string_view message) {
                const auto append = [&](std::string_view value) {
                    for (char c : value)
                        errors += c == '\r' || c == '\n' || c == '\t' ? ' ' : c;
                };
                append(context);
                errors += " | ";
                append(message);
                errors += '\n';
            };
        }

        template <class Action>
        bool perform(Action&& action) noexcept
        {
            try
            {
                messages.clear();
                errors.clear();
                animations.clear();
                audio.clear();
                presentation.clear();
                std::ostringstream out;
                action(out);
                const auto write = [&](const auto& events) {
                    for (const auto& event : events)
                    {
                        out << "T5_PAPYRUS_EVENT owner=" << std::hex << event.owner << std::dec
                            << " script=" << event.script << " event=" << event.event << " time=" << event.time << '\n';
                        for (const auto& trace : event.traces)
                            out << trace.message << '\n';
                    }
                };
                write(state->quests.takeEvents());
                write(state->world.takeEvents());
                write(state->scripts.takeEvents());
                auto visuals = std::exchange(state->presentation, {});
                auto objectives = state->objectiveMarkers();
                if (objectives != lastObjectives)
                {
                    out << "T5_QUEST_MARKERS " << objectives << '\n';
                    lastObjectives = objectives;
                }
                visuals.push_back(std::move(objectives));
                if (const auto& travel = state->pendingTravel)
                {
                    const auto& position = travel->destination.position;
                    visuals.push_back(std::format("[\"doorTravel\",{},{},{},{}]", travel->door, travel->location,
                        static_cast<int>(std::floor(position.pos[0] / TES5Viewer::LandTile::size)),
                        static_cast<int>(std::floor(position.pos[1] / TES5Viewer::LandTile::size))));
                }
                for (const auto& [id, animation] : state->modelAnimations)
                {
                    const auto elapsed = std::clamp(state->time - animation.started, 0., double(animation.duration));
                    if (const auto clip = state->modelClips.find(id); clip != state->modelClips.end())
                    {
                        const auto layer = state->modelSequenceLayers.find(id);
                        visuals.push_back(std::format("[\"modelHkx\",{},{},{},{},{},{}]", id, jsonText(clip->second.rig),
                            jsonText(animation.sequence), clip->second.start + elapsed * clip->second.speed,
                            jsonText(layer == state->modelSequenceLayers.end() ? std::string_view{} : std::string_view(layer->second)),
                            elapsed));
                    }
                    else
                        visuals.push_back(std::format("[\"modelAnimation\",{},{},{}]", id,
                            jsonText(animation.sequence), elapsed));
                }
                for (const auto& [id, damage] : state->damagedObjects)
                    if (damage.next)
                        visuals.push_back(std::format("[\"destruction\",{},{},{},{}]", id, damage.stage,
                            damage.noCollision, !damage.model.empty()));
                visuals.push_back(state->visuals.sample(state->read, state->time));
                for (const auto& arrow : state->world.takeArrows())
                {
                    std::ostringstream serialized;
                    serialized << "[\"arrow\"," << arrow.command << ',' << arrow.actor << ',' << arrow.target << ','
                        << arrow.shot << ',' << jsonText(arrow.model) << ',' << arrow.speed << ',' << arrow.gravity << ','
                        << arrow.range << ',' << arrow.radius << ',' << arrow.lifetime << ',' << arrow.alwaysHit << ','
                        << arrow.holdWhenBlocked << ']';
                    visuals.push_back(std::move(serialized).str());
                }
                for (const auto& face : state->world.takeFaces())
                {
                    std::ostringstream serialized;
                    serialized << "[\"face\"," << face.kind << ',' << face.actor << ',' << face.target << ',' << face.value << ']';
                    visuals.push_back(std::move(serialized).str());
                }
                for (const auto& fall : state->world.takeFalls())
                    visuals.push_back(std::format("[\"actorFall\",{},{},{},{},{}]", fall.actor,
                        fall.velocity.x(), fall.velocity.y(), fall.velocity.z(), fall.delta));
                for (const auto& firing : state->world.takeFiringPositions())
                    visuals.push_back(std::format("[\"firingPosition\",{},{},{},{},{}]", firing.actor,
                        firing.target, firing.marker, firing.range, firing.radius));
                if (!visuals.empty())
                {
                    presentation = '[';
                    for (std::size_t i = 0; i < visuals.size(); ++i)
                        presentation += (i ? "," : "") + visuals[i];
                    presentation += ']';
                }
                const auto commands = state->world.takeAnimations();
                if (!commands.empty())
                {
                    std::ostringstream serialized;
                    serialized << '[';
                    for (std::size_t i = 0; i < commands.size(); ++i)
                    {
                        const auto& command = commands[i];
                        if (i)
                            serialized << ',';
                        serialized << '[' << command.reference << ',' << command.idle << ','
                            << jsonText(command.behavior) << ',' << jsonText(command.event) << ',' << jsonText(command.clip)
                            << ',' << command.speed << ']';
                    }
                    serialized << ']';
                    animations = std::move(serialized).str();
                }
                const auto sounds = state->world.takeAudio();
                if (!sounds.empty())
                {
                    std::ostringstream serialized;
                    serialized << '[';
                    for (std::size_t i = 0; i < sounds.size(); ++i)
                    {
                        const auto& sound = sounds[i];
                        if (i)
                            serialized << ',';
                        serialized << '[' << sound.command << ',' << sound.instance << ',' << sound.source << ','
                            << jsonText(sound.path) << ',' << sound.delay << ',' << sound.volume << ',' << sound.pitch
                            << ',' << sound.minDistance << ',' << sound.maxDistance << ','
                            << (sound.spatial ? "true" : "false") << ',' << (sound.loop ? "true" : "false")
                            << ',' << sound.fade << ',' << sound.loopBegin << ',' << sound.loopEnd << ',' << sound.loopCount
                            << ',' << (sound.music ? "true" : "false") << ',' << jsonText(sound.finale) << ",[";
                        for (std::size_t cue = 0; cue < sound.cues.size(); ++cue)
                        {
                            if (cue)
                                serialized << ',';
                            serialized << sound.cues[cue];
                        }
                        serialized << "]," << sound.emotion << "," << sound.emotionStrength << "]";
                    }
                    serialized << ']';
                    audio = std::move(serialized).str();
                }
                messages = std::move(out).str();
                return true;
            }
            catch (const std::exception& error)
            {
                log.write("T5 Papyrus", error.what());
                return false;
            }
        }
    };
}

T5_BRIDGE_API void T5DataSetCacheDirectory(const char* directory) noexcept
{
    try { std::lock_guard lock(gLoadMutex); gCacheDirectory = directory ? Files::pathFromUnicodeString(directory) : std::filesystem::path{}; }
    catch (...) {}
}

T5_BRIDGE_API void T5DataSetLoadProgress(TES5Viewer::LoadProgressCallback callback, void* context) noexcept
{
    TES5Viewer::loadProgress = {};
    TES5Viewer::loadProgress.callback = callback;
    TES5Viewer::loadProgress.context = context;
    TES5Viewer::loadPhase("Opening game files");
}

T5_BRIDGE_API std::uint32_t T5DataBridgeVersion() noexcept { return 5; }

T5_BRIDGE_API void* T5DataLoadHelgen(
    const char* data, const char* pluginList, const char* resources, const char* osgPlugins) noexcept
{
    return performLoad([&](LoadResult& result) {
            ClearSettings cleanup;
            TES5Viewer::RuntimeExportOptions options;
            options.data = Files::pathFromUnicodeString(data);
            options.resources = Files::pathFromUnicodeString(resources);
            const auto plugins = TES5Viewer::loadPluginList(Files::pathFromUnicodeString(pluginList), options.data);
            options.assetPlan = TES5Viewer::buildAssetPlan(options.data, plugins, {}, {}, false);
            configureOsg(osgPlugins);
            const auto& catalog = locations(plugins);
            const auto scene = TES5Viewer::loadRuntimeExterior(plugins, catalog, 0x3c, 3, -25, 6, -18, false);
            indexLocations(plugins);
            TES5Viewer::exportRuntimeLocation(scene, options, gLocations.read, nullptr, 4, -20, result.buffers);
            if (!result.buffers.contains("meshes.json"))
                result.error = "The selected T5 files produced no Helgen scene";
    });
}

T5_BRIDGE_API void* T5DataCatalog(const char* data, const char* pluginList) noexcept
{
    return performLoad([&](LoadResult& result) {
        const auto paths = TES5Viewer::loadPluginList(Files::pathFromUnicodeString(pluginList), Files::pathFromUnicodeString(data));
        const auto& catalog = locations(paths);
        std::ostringstream json;
        json << "{\"worlds\":[";
        bool comma = false;
        for (const auto& [id, world] : catalog.worlds)
        {
            const auto& map = catalog.inherited(world, TES5Viewer::WorldspaceInfo::useMap);
            const auto& lod = catalog.inherited(world, TES5Viewer::WorldspaceInfo::useLod);
            const auto& land = catalog.inherited(world, TES5Viewer::WorldspaceInfo::useLand);
            json << (comma ? "," : "") << "{\"id\":" << id << ",\"editorId\":" << jsonText(world.editorId)
                 << ",\"plugin\":" << jsonText(world.sourcePlugin) << ",\"parent\":" << world.parent
                 << ",\"parentFlags\":" << world.parentFlags << ",\"mapWorld\":" << map.formId
                 << ",\"lodWorld\":" << lod.formId << ",\"landWorld\":" << land.formId
                 << ",\"mapImage\":" << jsonText(map.mapImage);
            if (map.mapBounds)
            {
                const auto& b = *map.mapBounds;
                json << ",\"mapBounds\":[" << b[0] << ',' << b[1] << ',' << b[2] << ',' << b[3] << ']';
            }
            if (world.mapOffset)
            {
                const auto& o = *world.mapOffset;
                json << ",\"mapOffset\":[" << o[0] << ',' << o[1] << ',' << o[2] << ',' << o[3] << ']';
            }
            if (world.center)
                json << ",\"center\":[" << world.center->first << ',' << world.center->second << ']';
            json << ",\"cells\":[";
            bool cellComma = false;
            for (const auto& cell : world.cells)
            {
                json << (cellComma ? "," : "") << '[' << cell.x << ',' << cell.y << ',' << cell.cellId << ','
                     << jsonText(cell.editorId) << ']';
                cellComma = true;
            }
            json << "]}";
            comma = true;
        }
        json << "],\"interiors\":[";
        comma = false;
        for (const auto& cell : catalog.interiors)
        {
            json << (comma ? "," : "") << "{\"id\":" << cell.formId << ",\"editorId\":" << jsonText(cell.editorId)
                 << ",\"plugin\":" << jsonText(cell.sourcePlugin) << '}';
            comma = true;
        }
        json << "]}";
        result.buffers.emplace("catalog.json", std::move(json).str());
    });
}

T5_BRIDGE_API void* T5DataLoadLocation(const char* data, const char* pluginList, const char* resources,
    const char* osgPlugins, std::uint32_t id, int x, int y, int radius) noexcept
{
    return loadLocation(data, pluginList, resources, osgPlugins, id, x, y, radius, false, 0, {});
}

T5_BRIDGE_API void* T5DataLoadPlayableLocation(const char* data, const char* pluginList, const char* resources,
    const char* osgPlugins, std::uint32_t id, int x, int y, int radius, std::uint64_t revision,
    const std::uint32_t* tracked, std::uint32_t count) noexcept
{
    return loadLocation(data, pluginList, resources, osgPlugins, id, x, y, radius, true, revision,
        tracked ? std::span(tracked, count) : std::span<const std::uint32_t>{});
}

T5_BRIDGE_API void* T5DataLoadGameLocation(const char* data, const char* pluginList, const char* resources,
    const char* osgPlugins, std::uint32_t id, int x, int y, int radius, std::uint64_t revision,
    const std::uint32_t* tracked, std::uint32_t count, std::uint32_t openingQuest,
    const SpawnedReference* spawned, std::uint32_t spawnedCount) noexcept
{
    return loadLocation(data, pluginList, resources, osgPlugins, id, x, y, radius, true, revision,
        tracked ? std::span(tracked, count) : std::span<const std::uint32_t>{}, openingQuest,
        spawned ? std::span(spawned, spawnedCount) : std::span<const SpawnedReference>{});
}

T5_BRIDGE_API void* T5DataNewGameStart(const char* data, const char* pluginList, std::uint32_t quest) noexcept
{
    return performLoad([&](LoadResult& result) {
        const auto paths = TES5Viewer::loadPluginList(Files::pathFromUnicodeString(pluginList), Files::pathFromUnicodeString(data));
        const auto& catalog = locations(paths);
        indexLocations(paths);
        const auto marker = playerStartMarker(gLocations.read, quest);
        if (!marker)
            throw std::runtime_error("The starting quest has no forced PlayerStartMarker alias");
        const auto reference = gLocations.index->references.find(marker);
        const auto pose = authoredPose(gLocations.read(marker));
        if (!pose || reference == gLocations.index->references.end())
            throw std::runtime_error("Cannot resolve the starting reference in active T5 plugins");
        const auto world = reference->second.world;
        const auto location = world ? world : reference->second.cell;
        if (!location || (world && !catalog.worlds.contains(world)))
            throw std::runtime_error("Starting reference has no active location");
        const auto x = world ? static_cast<int>(std::floor(pose->pos[0] / TES5Viewer::LandTile::size)) : 0;
        const auto y = world ? static_cast<int>(std::floor(pose->pos[1] / TES5Viewer::LandTile::size)) : 0;
        result.buffers.emplace("start.json", "{\"id\":" + std::to_string(location)
            + ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + '}');
    });
}

T5_BRIDGE_API void* T5PapyrusCreate(const char* log) noexcept
{
    try
    {
        auto session = std::make_unique<PapyrusSession>();
        session->log.open(Files::pathFromUnicodeString(log));
        return session.release();
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

T5_BRIDGE_API void T5PapyrusDestroy(void* handle) noexcept { delete static_cast<PapyrusSession*>(handle); }

T5_BRIDGE_API void T5PapyrusConfigure(void* handle, int newGame, std::uint32_t quest, int keepGoing) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    session.newGame = newGame != 0;
    session.selectedQuest = quest;
    session.continueMissing = keepGoing != 0;
}

T5_BRIDGE_API int T5PapyrusAttach(void* handle, const void* load) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    const auto& result = *static_cast<const LoadResult*>(load);
    if (!result.revision || !result.error.empty())
        return 0;
    return session.perform([&](std::ostream& out) {
        if (!session.state || session.state->revision != result.revision)
            session.state = std::make_unique<PapyrusState>(result, session.log, session.continueMissing);
        auto& state = *session.state;
        auto placements = result.scripts.placements;
        if (result.initialPlayer && !state.startedGame)
            placements.emplace(0x14u, *result.initialPlayer);
        state.world.setExteriorCells(result.scripts.worldspace,
            result.worldCells ? *result.worldCells : result.scripts.exteriorCells);
        state.world.addPlacements(placements);
        state.attachDoors(result.scripts.doors);
        if (state.pendingTravel && state.pendingTravel->location
            == (result.scripts.worldspace ? result.scripts.worldspace : result.scripts.cell))
        {
            state.world.teleportPlayer(state.pendingTravel->destination, result.scripts.worldspace);
            out << "T5_DOOR_ARRIVAL door=" << std::hex << state.pendingTravel->door
                << " cell=" << state.pendingTravel->destination.cell << std::dec << '\n';
            state.pendingTravel.reset();
        }
        state.world.setTriggers(result.scripts.scripts, result.scripts.actors, result.scripts.actorBounds);
        state.attachPlayerBounds();
        std::vector<std::uint32_t> loaded;
        loaded.reserve(placements.size());
        for (const auto& [id, pose] : placements)
            loaded.push_back(id);
        state.quests.setLoadedReferences(loaded);
        state.quests.addActors(result.scripts.actors);
        if (session.selectedQuest)
        {
            if (const auto scene = result.index->scenesByQuest.find(session.selectedQuest);
                scene != result.index->scenesByQuest.end())
                state.world.addScenes(scene->second);
        }
        else
            for (const auto& [quest, scenes] : result.index->scenesByQuest)
                state.world.addScenes(scenes);
        for (const auto& [id, disabled] : result.scripts.initialDisabled)
            state.report.visibility.insert_or_assign(id, !disabled);
        state.enableParents.insert(result.scripts.enableParents.begin(), result.scripts.enableParents.end());
        state.tracked.insert(result.scripts.targets.begin(), result.scripts.targets.end());
        for (const auto& [context, error] : result.scripts.errors)
            session.log.write(context, error);
        const auto oldCount = state.report.scripts.size();
        state.scripts.install(result.scripts.scripts);
        for (std::size_t i = oldCount; i < state.report.scripts.size(); ++i)
        {
            const auto& script = state.report.scripts[i];
            out << "T5_PAPYRUS_BIND owner=" << std::hex << script.owner << std::dec
                << " script=" << script.name << " source=" << script.source << '\n';
        }
        if (session.newGame && !state.startedGame)
        {
            state.startedGame = true;
            if(result.index)
                if(const auto days=result.index->globalsByName.find("gamedayspassed");days!=result.index->globalsByName.end())
                    state.quests.host().setGlobalValue(days->second,0);
            out << "T5_NEW_GAME quest=" << std::hex << session.selectedQuest << std::dec << '\n';
            if (session.selectedQuest)
            {
                state.quests.startGame(session.selectedQuest);
                state.startMissingDialogueQuests = session.selectedQuest == 0x3372b;
            }
            else
            {
                if (result.openingQuest)
                    state.quests.startGame(result.openingQuest);
                state.quests.startGame();
            }
        }
        state.quests.cellLoaded();
        state.quests.updateLocations();
    });
}

T5_BRIDGE_API std::uint64_t T5PapyrusRevision(const void* handle) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    return state ? state->revision : 0;
}

T5_BRIDGE_API std::uint32_t T5PapyrusTracked(const void* handle, std::uint32_t* data, std::uint32_t capacity) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    if (!state)
        return 0;
    if (data)
        std::copy_n(state->tracked.begin(), std::min<std::size_t>(capacity, state->tracked.size()), data);
    return static_cast<std::uint32_t>(state->tracked.size());
}

T5_BRIDGE_API std::uint32_t T5PapyrusSpawned(const void* handle, SpawnedReference* data, std::uint32_t capacity) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    if (!state)
        return 0;
    const auto& references = state->quests.createdReferences();
    const auto& moves = state->world.transforms();
    std::uint32_t count = 0;
    const auto store = [&](SpawnedReference value) {
        // PlaceAtMe can also emit an EXPL effect. It has no persistent scene
        // placement and must not invalidate the cell's geometry.
        if (state->characterIndex && state->characterIndex->explosions.contains(value.base))
            return;
        if (data && count < capacity)
        {
            if (const auto found = moves.find(value.id); found != moves.end())
            {
                value.flags |= 4;
                value.cell = found->second.cell;
                value.position = found->second.position;
            }
            data[count] = value;
        }
        ++count;
    };
    for (const auto& [id, ref] : references)
        if (!state->quests.inventoryReferences().contains(id))
            store({ id, ref.base, ref.anchor, (ref.actor ? 1u : 0u) | (ref.initiallyDisabled ? 2u : 0u) });
    for (const auto& [id, ref] : state->relocated)
        store(ref);
    return count;
}

T5_BRIDGE_API int T5PapyrusTransform(const void* handle, std::uint32_t id, float* data, std::uint32_t* parent) noexcept
{
    try
    {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        if (!state || !data)
            return 0;
        const auto current = state->world.transforms().find(id);
        const auto base = state->world.authoredPlacement(id);
        if (!base || (id == 0x14u && current == state->world.transforms().end() && !state->startedGame))
            return 0;
        if (parent)
            *parent = state->world.vehicle(id);
        const auto pack = [](const ESM::Position& pose, float* out) {
            out[0] = pose.pos[0];
            out[1] = pose.pos[1];
            out[2] = pose.pos[2];
            const auto q = Misc::Convert::makeOsgQuat(pose);
            out[3] = static_cast<float>(q.x());
            out[4] = static_cast<float>(q.y());
            out[5] = static_cast<float>(q.z());
            out[6] = static_cast<float>(q.w());
        };
        pack(base->position, data);
        pack(current == state->world.transforms().end() ? base->position : current->second.position, data + 7);
        return 1;
    }
    catch (const std::exception&)
    {
        return 0;
    }
}

T5_BRIDGE_API void T5PapyrusGameState(const void* handle, std::uint32_t* data, float* sitting) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    if (state && data)
    {
        data[0] = state->controls;
        data[1] = state->gameFlags;
        if (sitting)
            *sitting = state->sittingRotation;
    }
}

T5_BRIDGE_API int T5PapyrusIsDisabled(const void* handle, std::uint32_t id) noexcept
{
    try
    {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        return state ? static_cast<int>(state->isDisabled(id)) : -1;
    }
    catch (const std::exception&)
    {
        return -1;
    }
}

T5_BRIDGE_API int T5PapyrusHasScripts(const void* handle, std::uint32_t id) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    return state && state->scripts.hasScripts(id);
}

T5_BRIDGE_API int T5PapyrusWasKilled(const void* handle, std::uint32_t id) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    return state && state->world.wasKilled(id);
}

T5_BRIDGE_API const char* T5PapyrusSeveredHead(const void* handle, std::uint32_t id) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    if (!state) return nullptr;
    const auto it = state->severedHeads.find(id);
    return it == state->severedHeads.end() ? nullptr : it->second.c_str();
}

T5_BRIDGE_API void* T5PapyrusAnimationObject(const void* handle, const char* editorId) noexcept
{
    return performLoad([&](LoadResult& result) {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        if (!state || !state->characterIndex || !state->vfs || !editorId)
            throw std::runtime_error("Animation object has no mounted resources");
        const auto found = state->characterIndex->animationObjects.find(Misc::StringUtils::lowerCase(std::string_view(editorId)));
        if (found == state->characterIndex->animationObjects.end())
            throw std::runtime_error("Unknown ANIO");
        std::string model, unload;
        TES5Viewer::visitSubrecords(state->read(found->second), [&](auto tag, auto bytes) {
            if (tag == ESM::fourCC("MODL")) model = TES5Viewer::modelAssetPath(TES5Viewer::characterString(bytes));
            if (tag == ESM::fourCC("BNAM")) unload = TES5Viewer::characterString(bytes);
        });
        if (model.empty()) throw std::runtime_error("ANIO has no model");
        const auto socket = TES5Viewer::exportAnimationObject(*state->vfs, state->read, model, result.buffers);
        if (socket.empty()) throw std::runtime_error("ANIO model has no attachment node");
        result.buffers.emplace("prop.json", std::format("{{\"socket\":{},\"unload\":{}}}", jsonText(socket), jsonText(unload)));
    });
}

T5_BRIDGE_API void* T5PapyrusDestructionModel(const void* handle, std::uint32_t id) noexcept
{
    return performLoad([&](LoadResult& result) {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        if (!state || !state->vfs) throw std::runtime_error("Destruction model has no mounted resources");
        const auto damage = state->damagedObjects.find(id);
        if (damage == state->damagedObjects.end() || damage->second.model.empty())
            throw std::runtime_error("Destruction stage has no replacement model");
        TES5Viewer::exportAnimationObject(*state->vfs, state->read, damage->second.model, result.buffers, false);
        result.buffers.emplace("prop.json", "{\"socket\":\"\",\"unload\":\"\"}");
    });
}

T5_BRIDGE_API int T5PapyrusActivate(void* handle, std::uint32_t id, std::uint32_t activator) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    bool activated = false;
    return session.state && !(session.state->gameFlags & raceMenuPending)
        && (activator != 0x14 || !(session.state->controls & (1u << 6)))
        && session.perform([&](std::ostream&) {
            activated = session.state->activateReference(id, activator);
        }) && activated;
}

enum DialogueCommand { DialogueRead, DialogueChoose, DialogueClose, DialogueBuy, DialogueSell, DialogueLeaveBarter };

T5_BRIDGE_API const char* T5PapyrusDialogue(void* handle, int action, std::uint32_t choice) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    if (!session.state) return nullptr;
    auto& state = *session.state;
    if (action >= DialogueChoose && action <= DialogueLeaveBarter)
        session.perform([&](std::ostream&) {
            switch (action)
            {
                case DialogueChoose: state.chooseDialogue(choice); break;
                case DialogueClose: state.closeDialogue(); break;
                case DialogueBuy: state.tradeItem(choice, true); break;
                case DialogueSell: state.tradeItem(choice, false); break;
                case DialogueLeaveBarter: state.leaveBarter(); break;
            }
        });
    return state.dialogue.actor ? state.dialogue.json.c_str() : nullptr;
}

T5_BRIDGE_API const char* T5PapyrusInventory(void* handle, std::uint32_t owner) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    if (!session.state) return nullptr;
    try
    {
        session.state->syncQuestItems();
        session.inventoryJson = session.state->playerInventoryJson(owner);
        return session.inventoryJson.empty() ? nullptr : session.inventoryJson.c_str();
    }
    catch (const std::exception& error) { session.log.write("Inventory", error.what()); return nullptr; }
}

T5_BRIDGE_API const char* T5PapyrusInventoryPresentation(void* handle, std::uint32_t owner) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    if (!session.state) return nullptr;
    try
    {
        session.state->syncQuestItems();
        session.inventoryJson = session.state->playerInventoryJson(owner, true);
        return session.inventoryJson.empty() ? nullptr : session.inventoryJson.c_str();
    }
    catch (const std::exception& error) { session.log.write("Inventory presentation", error.what()); return nullptr; }
}

T5_BRIDGE_API void* T5PapyrusInventoryModel(const void* handle, std::uint32_t id) noexcept
{
    return performLoad([&](LoadResult& result) {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        if (!state || !state->vfs) throw std::runtime_error("Inventory model has no mounted resources");
        const auto record = state->read(id);
        const auto type = record.metadata.recordType;
        if (!TES5Viewer::isInventoryItem(type)) throw std::runtime_error("Form is not an inventory item");
        std::string model, fallback;
        TES5Viewer::visitSubrecords(record, [&](auto tag, auto data) {
            if (tag == ESM::fourCC("MODL")) model = TES5Viewer::characterString(data);
            if (type == ESM::fourCC("ARMO") && tag == ESM::fourCC("MOD2")) fallback = TES5Viewer::characterString(data);
        });
        if (model.empty()) model = std::move(fallback);
        model = TES5Viewer::modelAssetPath(model);
        if (model.empty()) throw std::runtime_error("Inventory item has no world model");
        TES5Viewer::exportAnimationObject(*state->vfs, state->read, model, result.buffers, false);
        result.buffers.emplace("prop.json", "{\"socket\":\"\",\"unload\":\"\"}");
    });
}

T5_BRIDGE_API int T5PapyrusResourcePools(const void* handle, float* ratios) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    if (!state || !ratios) return 0;
    try
    {
        for (std::uint32_t index = 0; index < 3; ++index)
        {
            const auto pool = state->world.resourcePool(0x14, 24 + index);
            ratios[index] = pool ? std::clamp(pool->first / pool->second, 0.f, 1.f) : -1.f;
        }
        return 1;
    }
    catch (const std::exception&) { return 0; }
}

T5_BRIDGE_API int T5PapyrusInventoryAction(void* handle, std::uint32_t owner, std::uint32_t item,
    std::int32_t count) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    bool accepted = false;
    return session.state && session.perform([&](std::ostream&) {
        accepted = owner == 0x14 ? session.state->useItem(item) : session.state->takeItem(owner, item, count);
    }) && accepted;
}

T5_BRIDGE_API int T5PapyrusPlayerWeapon(void* handle, int action, std::uint32_t target, float* reach) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    if (!session.state) return 0;
    auto& state = *session.state;
    try
    {
        if (!state.canFight()) return 0;
        if (action == 0)
        {
            const auto stats = state.weaponStats();
            if (reach) *reach = stats.type == 7 || stats.type == 9 ? -1.f : stats.reach;
            return state.weaponDrawn ? 1 : 0;
        }
        if (action == 1)
        {
            state.weaponDrawn = !state.weaponDrawn;
            return state.weaponDrawn ? 1 : 2;
        }
        return action == 2 && state.playerAttack(target) ? 1 : 0;
    }
    catch (const std::exception& error) { session.log.write("Player weapon", error.what()); return 0; }
}

T5_BRIDGE_API int T5PapyrusPlayerBow(void* handle, const float* aim) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    bool fired = false;
    return session.state && session.perform([&](std::ostream&) {
        fired = session.state->playerBow(aim);
    }) && fired;
}

T5_BRIDGE_API std::uint64_t T5PapyrusViewKey(const void* handle, int* drawn) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    if (!state) return 0;
    if (drawn) *drawn = state->weaponDrawn;
    std::uint64_t key = 14695981039346656037ull;
    const auto add = [&](std::uint32_t field) { key = (key ^ field) * 1099511628211ull; };
    add(state->playerWeapon);
    if (const auto found = state->outfits.find(0x14); found != state->outfits.end())
        for (auto form : found->second) add(form);
    if (state->character)
    {
        for (auto value : std::as_bytes(std::span(&*state->character, 1)))
            add(std::to_integer<unsigned char>(value));
    }
    return key ? key : 1;
}

T5_BRIDGE_API void* T5PapyrusPlayerView(const void* handle) noexcept
{
    return performLoad([&](LoadResult& result) {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        if (!state || !state->vfs) throw std::runtime_error("Player view has no mounted resources");
        std::string weapon;
        std::uint32_t firstPerson = 0;
        const auto findModel = [&](const auto& record, bool weaponRecord) {
            TES5Viewer::visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("MODL")) weapon = TES5Viewer::modelAssetPath(TES5Viewer::characterString(bytes));
                if (weaponRecord && tag == ESM::fourCC("WNAM")) firstPerson = record.mapFormId(TES5Viewer::characterNumber(bytes));
            });
        };
        if (state->playerWeapon) findModel(state->read(state->playerWeapon), true);
        if (firstPerson)
        {
            const auto fallback = weapon;
            findModel(state->read(firstPerson), false);
            if (weapon.empty() || !state->vfs->exists(VFS::Path::Normalized(weapon))) weapon = fallback;
        }
        const auto outfit = state->outfits.find(0x14);
        const std::set<std::uint32_t> empty;
        TES5Viewer::exportPlayerView(*state->vfs, state->read, state->world.actorBase(0x14),
            outfit != state->outfits.end() ? outfit->second : empty, weapon, result.buffers);
        result.buffers.emplace("view.json", std::format("{{\"type\":{}}}", state->weaponStats().type));
    });
}

T5_BRIDGE_API void* T5PapyrusPlayerBody(const void* handle) noexcept
{
    return performLoad([&](LoadResult& result) {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        if (!state || !state->vfs) throw std::runtime_error("Player body has no mounted resources");
        const auto outfit = state->outfits.find(0x14);
        const std::set<std::uint32_t> empty;
        TES5Viewer::exportPlayerView(*state->vfs, state->read, state->world.actorBase(0x14),
            outfit != state->outfits.end() ? outfit->second : empty, {}, result.buffers, false);
        if (state->character) result.buffers.emplace("morphs.json", state->characterMorphs);
    });
}

T5_BRIDGE_API void T5PapyrusTick(void* handle, double delta) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    if (session.state)
        session.perform([&](std::ostream&) {
            auto& state = *session.state;
            if (state.gameFlags & raceMenuPending)
                return;
            for (const auto& [shot, target] : std::exchange(session.impacts, {}))
                if (shot >= 0xff800000)
                    state.magicImpact(shot, target);
                else
                    state.world.arrowImpact(shot, target);
            for (const auto& [sender, event] : std::exchange(session.animationEvents, {}))
            {
                if (Misc::StringUtils::ciEqual(event, "Decapitate") && !state.severedHeads.contains(sender))
                    if (const auto base = state.world.actorBase(sender))
                        if (auto bone = TES5Viewer::actorHeadBone(state.read, base); !bone.empty())
                            state.severedHeads.emplace(sender, std::move(bone));
                if (Misc::StringUtils::ciEqual(event, "T5ClipEnd") || Misc::StringUtils::ciEqual(event, "KillActor"))
                    if (const auto victim = state.executionVictims.extract(sender); !victim.empty())
                        state.world.callFormMethod(sender, "Kill", {Papyrus::FormReference{victim.mapped()}});
                state.world.animationEvent(sender, event);
                state.finishModelWait(sender, event);
                state.quests.animationEvent(sender, event);
                state.scripts.animationEvent(sender, event);
            }
            state.time += delta;
            state.tickImageFades();
            state.quests.tick(state.time);
            state.syncQuestItems();
            if (state.gameFlags & raceMenuPending)
                return;
            if (std::exchange(state.packagesNeedRecovery, false)) state.world.recoverPackages();
            state.tickDialogue();
            state.world.tick(state.time);
            state.quests.updateLocations();
            state.tickMagic();
            for (const auto& [furniture, actor] : state.world.takeFurnitureActivations())
                if (state.scripts.hasScripts(furniture)) state.scripts.activate(furniture, actor);
            state.updateRelocations();
            if(const auto player=state.world.placement(0x14))
                state.environment.tick(player->position.pos[0],player->position.pos[1],delta*state.quests.globalValue("timescale",20.f)/3600.);
            if(state.environment.current)
            {
                state.weather=state.environment.current;
                const auto sampled=state.environment.sample(state.quests.globalValue("gamehour",12.f));
                std::copy_n(sampled.begin(),12,state.weatherDay.begin());
            }
            for (const auto& event : state.world.takeTriggerEvents())
                state.scripts.trigger(event.trigger, event.actor, event.entering);
            state.updateModelAnimations();
            state.scripts.tick(state.time);
            state.automaticDoors();
            state.continueDialogue();
        });
}

T5_BRIDGE_API const char* T5PapyrusMessages(const void* handle) noexcept
{
    return static_cast<const PapyrusSession*>(handle)->messages.c_str();
}

T5_BRIDGE_API const char* T5PapyrusErrors(const void* handle) noexcept
{
    return static_cast<const PapyrusSession*>(handle)->errors.c_str();
}

T5_BRIDGE_API const char* T5PapyrusAnimations(const void* handle) noexcept
{
    return static_cast<const PapyrusSession*>(handle)->animations.c_str();
}

T5_BRIDGE_API const char* T5PapyrusPresentation(const void* handle) noexcept
{
    return static_cast<const PapyrusSession*>(handle)->presentation.c_str();
}

T5_BRIDGE_API void T5PapyrusSetGamepad(void* handle, int active) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    if (session.state) session.state->visuals.gamepad = active != 0;
}

T5_BRIDGE_API void T5PapyrusArrowImpact(void* handle, std::uint32_t shot, std::uint32_t target) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    try
    {
        if (session.state && session.impacts.size() < 1024)
            session.impacts.emplace_back(shot, target);
    }
    catch (const std::exception&)
    {
    }
}

T5_BRIDGE_API void T5PapyrusFallResult(void* handle, std::uint32_t actor,
    const float* position, const float* velocity, int grounded) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    try
    {
        if (session.state && position && velocity
            && std::ranges::all_of(std::span(position, 3), [](float v) { return std::isfinite(v); })
            && std::ranges::all_of(std::span(velocity, 3), [](float v) { return std::isfinite(v); }))
            session.state->world.fallResult(actor, {position[0], position[1], position[2]},
                {velocity[0], velocity[1], velocity[2]}, grounded != 0);
    }
    catch (const std::exception& error) { session.log.write("Actor fall", error.what()); }
}

T5_BRIDGE_API void T5PapyrusSetLineOfSight(void* handle,
    int (*callback)(void*, std::uint32_t, std::uint32_t), void* context) noexcept
{
    auto* session = static_cast<PapyrusSession*>(handle);
    if (session && session->state)
    {
        session->state->lineOfSight = callback;
        session->state->visibilityContext = context;
    }
}

T5_BRIDGE_API void T5PapyrusFiringPosition(void* handle, std::uint32_t actor, std::uint32_t target,
    const float* position) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    try
    {
        if (session.state && position
            && std::ranges::all_of(std::span(position, 3), [](float v) { return std::isfinite(v); }))
            session.state->world.firingPosition(actor, target, {position[0], position[1], position[2]});
    }
    catch (const std::exception& error) { session.log.write("Firing position", error.what()); }
}

T5_BRIDGE_API int T5PapyrusMotionType(const void* handle, std::uint32_t id) noexcept
{
    const auto& state=static_cast<const PapyrusSession*>(handle)->state;
    return state?state->world.motionType(id):0;
}

T5_BRIDGE_API float T5PapyrusVisualScale(const void* handle, std::uint32_t id) noexcept
{
    try
    {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        return state ? state->world.visualScale(id) : 1.f;
    }
    catch (const std::exception&) { return 1.f; }
}

T5_BRIDGE_API int T5PapyrusDoorState(const void* handle, std::uint32_t id, double* elapsed) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    if (!state)
        return -1;
    const auto door = state->doorStates.find(id);
    if (door == state->doorStates.end())
        return -1;
    if (elapsed)
        *elapsed = state->time - door->second.second;
    return static_cast<int>(door->second.first);
}

T5_BRIDGE_API std::uint32_t T5PapyrusWeather(const void* handle, float* data) noexcept
{
    const auto& state = static_cast<const PapyrusSession*>(handle)->state;
    if (!state || !state->weather)
        return 0;
    if (data)
        std::ranges::copy(state->weatherDay, data);
    return state->weather;
}

T5_BRIDGE_API std::uint32_t T5PapyrusEnvironment(const void* handle, float* data, std::uint32_t capacity) noexcept
{
    try
    {
        const auto& state=static_cast<const PapyrusSession*>(handle)->state;
        if(!state)return 0;
        const auto values=state->environment.sample(state->quests.globalValue("gamehour",12.f));
        if(data)std::copy_n(values.begin(),std::min<std::size_t>(capacity,values.size()),data);
        return state->environment.current;
    }
    catch(const std::exception&){return 0;}
}

T5_BRIDGE_API void T5PapyrusBeginEnvironment(void* handle, std::uint32_t worldId, std::uint32_t initial) noexcept
{
    auto& session=*static_cast<PapyrusSession*>(handle);
    if(!session.state || session.state->environment.initialized || !worldId)return;
    session.perform([&](std::ostream&){
        if(const auto pose=session.state->world.placement(0x14))
            session.state->environment.begin(worldId,session.newGame?initial:0,pose->position.pos[0],pose->position.pos[1]);
    });
}

T5_BRIDGE_API const char* T5PapyrusAudio(const void* handle) noexcept
{
    return static_cast<const PapyrusSession*>(handle)->audio.c_str();
}

T5_BRIDGE_API void T5PapyrusAudioFinished(void* handle, std::int32_t instance, int played) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    try
    {
        if (session.state)
            session.state->world.audioFinished(instance, played != 0);
    }
    catch (const std::exception& error)
    {
        session.log.write("Music playback", error.what());
    }
}

T5_BRIDGE_API void* T5DataDecodeAudio(const void* data, std::uint64_t size) noexcept
{
    try
    {
        auto result = std::make_unique<LoadResult>();
        try
        {
            const auto* bytes = static_cast<const std::byte*>(data);
            if (!bytes || size > std::numeric_limits<std::size_t>::max())
                throw std::runtime_error("Invalid T5 audio input");
            auto decoded = TES5Viewer::decodeAudio(std::span(bytes, static_cast<std::size_t>(size)));
            std::array<std::uint32_t, 2> info{
                static_cast<std::uint32_t>(decoded.sampleRate), static_cast<std::uint32_t>(decoded.channels)
            };
            result->buffers.emplace("audio.info", std::string(reinterpret_cast<const char*>(info.data()), sizeof(info)));
            result->buffers.emplace("audio.pcm", std::move(decoded.pcm));
            if (decoded.loop)
            {
                const std::array<std::uint32_t, 2> loop{ decoded.loop->first, decoded.loop->end };
                result->buffers.emplace("audio.loop", std::string(reinterpret_cast<const char*>(loop.data()), sizeof(loop)));
            }
        }
        catch (const std::exception& error)
        {
            result->error = error.what();
        }
        return result.release();
    }
    catch (...)
    {
        return nullptr;
    }
}

T5_BRIDGE_API void T5PapyrusAnimationPose(void* handle, std::uint32_t sender, const float* data, int active) noexcept
{
    try
    {
        auto& session = *static_cast<PapyrusSession*>(handle);
        if (!session.state || !data) return;
        for (int i = 0; i < 7; ++i) if (!std::isfinite(data[i])) return;
        ESM::Position pose{};
        osg::Quat q(data[3], data[4], data[5], data[6]);
        if (q.length2() < 0.001) return;
        q /= q.length();
        const auto angles = Misc::toEulerAnglesZYX(q);
        for (int i = 0; i < 3; ++i) { pose.pos[i] = data[i]; pose.rot[i] = angles[i]; }
        // Pose-channel flags: bit 0 active, bit 1 physics rather than animation.
        if (active & 2) session.state->world.physicsPose(sender, pose, (active & 1) != 0);
        else session.state->world.animationPose(sender, pose, active != 0);
    }
    catch (const std::exception&) {}
}

T5_BRIDGE_API void T5PapyrusAnimationEvent(void* handle, std::uint32_t sender, const char* event) noexcept
{
    try
    {
        auto& session = *static_cast<PapyrusSession*>(handle);
        if (session.state && event && *event && session.animationEvents.size() < 1024)
            session.animationEvents.emplace_back(sender, event);
    }
    catch (const std::exception&)
    {
    }
}

T5_BRIDGE_API void* T5PapyrusReadResource(const void* handle, const char* path) noexcept
{
    try
    {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        if (!state || !path || !*path)
            return nullptr;
        std::string_view asset(path);
        constexpr std::string_view basePrefix = "@base-animation/";
        const auto& resources = asset.starts_with(basePrefix) ? state->baseAnimations : state->vfs;
        if (!resources)
            return nullptr;
        if (asset.starts_with(basePrefix))
            asset.remove_prefix(basePrefix.size());
        const auto input = resources->find(VFS::Path::Normalized(asset));
        if (!input)
            return nullptr;
        auto result = std::make_unique<LoadResult>();
        result->buffers.emplace("resource",
            std::string(std::istreambuf_iterator<char>(*input), std::istreambuf_iterator<char>{}));
        return result.release();
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

T5_BRIDGE_API const char* T5DataGetError(const void* handle) noexcept
{
    return handle ? static_cast<const LoadResult*>(handle)->error.c_str() : "Cannot allocate T5 scene";
}

T5_BRIDGE_API const void* T5DataGetBuffer(
    const void* handle, const char* name, std::uint64_t* size) noexcept
{
    *size = 0;
    if (!handle)
        return nullptr;
    const auto& buffers = static_cast<const LoadResult*>(handle)->buffers;
    const auto entry = buffers.find(name);
    if (entry == buffers.end())
        return nullptr;
    *size = entry->second.size();
    return entry->second.data();
}

T5_BRIDGE_API void T5DataRelease(void* handle) noexcept { delete static_cast<LoadResult*>(handle); }

// Snapshot state overlays the mounted plugin/PEX records. No simulation is executed here.
T5_BRIDGE_API void* T5PapyrusSaveCheckpoint(void* handle) noexcept
{
    try
    {
        auto& session = *static_cast<PapyrusSession*>(handle);
        if (!session.state) return nullptr;
        Papyrus::Checkpoint ar;
        std::string magic = "T5 state snapshot 1";
        ar(magic, session.state->revision, session.newGame, session.selectedQuest, session.continueMissing);
        session.state->checkpoint(ar);
        ar(session.animationEvents, session.impacts);
        ar(session.state->character, session.state->characterName, session.state->characterMorphs);
        session.state->inventory.checkpoint(ar);
        ar(session.state->animationDependencies, session.state->executionVictims);
        session.state->visuals.checkpoint(ar);
        session.state->world.checkpointExtensions(ar);
        std::string environmentTag="T5 environment 1"; ar(environmentTag);session.state->environment.checkpoint(ar);
        std::string furnitureTag="T5 furniture 1"; ar(furnitureTag);session.state->world.checkpointFurniture(ar);
        std::string papyrusTag = "T5 Papyrus extensions 1";
        ar(papyrusTag); session.state->checkpointPapyrusExtensions(ar);
        auto result = std::make_unique<LoadResult>();
        result->buffers.emplace("checkpoint", ar.bytes());
        return result.release();
    }
    catch (const std::exception& error)
    { static_cast<PapyrusSession*>(handle)->log.write("Checkpoint save", error.what()); return nullptr; }
}

T5_BRIDGE_API int T5PapyrusRestoreCheckpoint(void* handle, const void* context, const void* bytes, std::uint64_t size) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    try
    {
        if (session.state || !bytes || size > Papyrus::Checkpoint::maxBytes) throw std::runtime_error("Invalid snapshot target or size");
        const auto& source = *static_cast<const LoadResult*>(context);
        Papyrus::Checkpoint ar{std::string_view(static_cast<const char*>(bytes), size)};
        std::string magic; std::uint64_t revision = 0;
        ar(magic, revision, session.newGame, session.selectedQuest, session.continueMissing);
        if (magic != "T5 state snapshot 1" || revision != source.revision || !source.scripts.vfs)
            throw std::runtime_error("Snapshot version or plugin revision mismatch");
        session.state = std::make_unique<PapyrusState>(source, session.log, session.continueMissing);
        session.state->checkpoint(ar);
        ar(session.animationEvents, session.impacts);
        if (!ar.atEnd()) ar(session.state->character, session.state->characterName, session.state->characterMorphs);
        if (!ar.atEnd()) { session.state->inventory.checkpoint(ar); ar(session.state->animationDependencies); }
        if (!ar.atEnd()) ar(session.state->executionVictims);
        if (!ar.atEnd()) session.state->visuals.checkpoint(ar);
        if (!ar.atEnd()) session.state->world.checkpointExtensions(ar);
        if(!ar.atEnd())
        {
            std::string tag;ar(tag);if(tag!="T5 environment 1")throw std::runtime_error("Unknown environment snapshot extension");
            session.state->environment.checkpoint(ar);
            if(!ar.atEnd())
            {
                std::string furnitureTag;ar(furnitureTag);
                if(furnitureTag!="T5 furniture 1")throw std::runtime_error("Unknown furniture snapshot extension");
                session.state->world.checkpointFurniture(ar);
                if (!ar.atEnd())
                {
                    std::string papyrusTag;
                    ar(papyrusTag);
                    if (papyrusTag != "T5 Papyrus extensions 1") throw std::runtime_error("Unknown Papyrus snapshot extension");
                    session.state->checkpointPapyrusExtensions(ar);
                }
            }
        }
        else
        {
            if(session.state->weather)session.state->environment.force(session.state->weather,session.state->weatherOverride);
            session.state->environment.initialized=true;
        }
        if (session.state->character) TES5Viewer::validateCharacter(session.state->originalRead, *session.state->character, session.state->characterName);
        if (!ar.atEnd()) throw std::runtime_error("Trailing snapshot data");
        // Geometry is built from the current saved placements (including relocated
        // references), whereas the snapshot's authored baselines belong to its last
        // cell attachment. Rebase to the geometry just built, as normal Attach does,
        // without executing cell-loaded events or initializers a second time.
        session.state->world.addPlacements(source.scripts.placements);
        session.state->attachDoors(source.scripts.doors);
        session.state->attachPlayerBounds();
        session.state->packagesNeedRecovery = true;
        session.state->startMissingDialogueQuests = session.selectedQuest == 0x3372b;
        return 1;
    }
    catch (const std::exception& error)
    { session.log.write("Checkpoint restore", error.what()); return 0; }
}


// Host-owned character creation. Skyrim/SKSE binaries are never loaded by this API.
T5_BRIDGE_API void* T5PapyrusCharacterCatalog(const void* handle) noexcept
{
    return performLoad([&](LoadResult& result) {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        if (!state || !state->characterIndex) throw std::runtime_error("Character creation requires an attached world");
        std::ostringstream json; json << "{\"races\":["; bool comma = false;
        for (auto id : state->characterIndex->characterRaces)
        {
            auto race = state->originalRead(id);
            if (!TES5Viewer::playableCharacterRace(race)) continue;
            std::string name; std::vector<std::pair<std::uint32_t, bool>> presets;
            TES5Viewer::visitSubrecords(race, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("EDID")) name = TES5Viewer::characterString(bytes);
                if (tag == ESM::fourCC("RPRM") || tag == ESM::fourCC("RPRF"))
                    presets.emplace_back(race.mapFormId(TES5Viewer::characterNumber(bytes)), tag == ESM::fourCC("RPRF"));
            });
            json << (comma ? "," : "") << "{\"id\":" << id << ",\"name\":" << jsonText(name) << ",\"presets\":[";
            bool pc = false;
            for (const auto& [preset, female] : presets)
            { json << (pc ? "," : "") << '[' << preset << ',' << female << ']'; pc = true; }
            json << "]}"; comma = true;
        }
        json << "],\"parts\":["; comma = false;
        for (auto id : state->characterIndex->characterHeadParts)
        {
            auto part = state->originalRead(id); std::string name, model; unsigned flags = 0, type = 0;
            TES5Viewer::visitSubrecords(part, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("EDID")) name = TES5Viewer::characterString(bytes);
                if (tag == ESM::fourCC("MODL")) model = TES5Viewer::modelAssetPath(TES5Viewer::characterString(bytes));
                if (tag == ESM::fourCC("DATA") && !bytes.empty()) flags = std::to_integer<unsigned>(bytes[0]);
                if (tag == ESM::fourCC("PNAM")) type = TES5Viewer::characterNumber(bytes);
            });
            if (!(flags & 1) || flags & 16 || part.flags & 4 || model.empty() || !state->vfs->exists(VFS::Path::Normalized(model))) continue;
            json << (comma ? "," : "") << "{\"id\":" << id << ",\"name\":" << jsonText(name) << ",\"type\":" << type << ",\"races\":[";
            bool rc = false;
            for (auto race : state->characterIndex->characterRaces)
                if (TES5Viewer::playableCharacterRace(state->originalRead(race)))
                    for (unsigned female = 0; female < 2; ++female)
                        if (TES5Viewer::validCharacterPart(state->originalRead, id, race, female))
                        { json << (rc ? "," : "") << '[' << race << ',' << female << ']'; rc = true; }
            json << "]}"; comma = true;
        }
        json << "]}";
        result.buffers.emplace("catalog.json", std::move(json).str());
        if (state->character)
        {
            result.buffers.emplace("choice.bin", std::string(reinterpret_cast<const char*>(&*state->character), sizeof(TES5Viewer::CharacterChoice)));
            result.buffers.emplace("name", state->characterName);
            result.buffers.emplace("morphs.json", state->characterMorphs);
        }
    });
}

T5_BRIDGE_API void* T5PapyrusCharacterPreview(const void* handle, const void* bytes, std::uint32_t size,
    const char* resources, const char* osgPlugins, const char* modRoot) noexcept
{
    return performLoad([&](LoadResult& result) {
        const auto& state = static_cast<const PapyrusSession*>(handle)->state;
        if (!state || !bytes || size != sizeof(TES5Viewer::CharacterChoice)) throw std::runtime_error("Invalid preview request");
        TES5Viewer::CharacterChoice choice; std::memcpy(&choice, bytes, size);
        TES5Viewer::validateCharacter(state->originalRead, choice, "Player");
        auto npc = TES5Viewer::characterRecord(state->originalRead, choice, "Player");
        auto read = [npc, base = state->originalRead](std::uint32_t id) { return id == 7 ? npc : base(id); };
        ClearSettings cleanup; configureOsg(osgPlugins);
        TES5Viewer::RuntimeExportOptions options; options.resources = Files::pathFromUnicodeString(resources);
        // Mount the provided RaceMenu assets as a separate overlay. Keep the save's plugin namespace unchanged.
        static std::shared_ptr<VFS::Manager> characterVfs;
        static std::string characterVfsKey;
        const std::string key = gLocations.mountedKey + (modRoot ? modRoot : "");
        if (!characterVfs || key != characterVfsKey)
        {
            auto plan = TES5Viewer::buildAssetPlan(gLocations.mountedData, gLocations.paths, {}, {}, false);
            if (modRoot && *modRoot)
            {
                auto root = Files::pathFromUnicodeString(modRoot);
                if (std::filesystem::is_regular_file(root / "RaceMenu.bsa")) plan.archives.push_back({root / "RaceMenu.bsa", "character creation", "RaceMenu"});
                if (std::filesystem::is_directory(root)) plan.looseData.push_back(root);
            }
            characterVfs = TES5Viewer::mountRuntimeAssets(plan); characterVfsKey = key;
        }
        options.runtimeVfs = characterVfs;
        TES5Viewer::CellScene scene;
        TES5Viewer::ExteriorReference ref; ref.referenceId = 0x14; ref.baseId = 7; ref.recordType = ESM::fourCC("ACHR");
        ref.position = {};
        // Orient the authored humanoid idle toward the studio portrait camera.
        ref.position.rot[2] = 3.14159265358979323846f;
        scene.exteriorReferences.emplace(0x14, ref);
        TES5Viewer::exportRuntimeLocation(scene, options, read, nullptr, 0, 0, result.buffers);
    });
}

T5_BRIDGE_API int T5PapyrusCommitCharacter(void* handle, const void* bytes, std::uint32_t size,
    const char* name, const char* morphs) noexcept
{
    auto& session = *static_cast<PapyrusSession*>(handle);
    if (!session.state || !bytes || size != sizeof(TES5Viewer::CharacterChoice) || !name || !*name || !morphs) return 0;
    return session.perform([&](std::ostream& out) {
        TES5Viewer::CharacterChoice choice; std::memcpy(&choice, bytes, size);
        TES5Viewer::validateCharacter(session.state->originalRead, choice, name);
        if (std::strlen(morphs) > 65536) throw std::runtime_error("Character morph limit");
        auto& state = *session.state;
        auto playerRecord = TES5Viewer::characterGameplayRecord(state.originalRead,choice,name);
        state.character = choice; state.characterName = name; state.characterMorphs = morphs;
        state.characterRecordCache = std::move(playerRecord);
        state.playerEyeHeight.reset();
        state.playerTriggerBounds.reset();
        state.attachPlayerBounds();
        state.gameFlags &= ~raceMenuPending;
        out << "T5_CHARACTER_COMMIT race=" << std::hex << choice.race << std::dec << " female=" << choice.female << '\n';
    });
}

// Standing eye level in source units. The shared humanoid rig's head pivot is
// at 120.343 units; the eyes sit four units above it. Apply the winning race's
// sex-specific height and the accepted character's scale, as the mesh loader does.
T5_BRIDGE_API float T5PapyrusPlayerEyeHeight(const void* handle) noexcept
{
    try
    {
        auto& state=static_cast<const PapyrusSession*>(handle)->state;
        if (!state) return 124.343f*1.03f;
        if (!state->playerEyeHeight)
        {
            const auto appearance=TES5Viewer::loadActorAppearance(state->read,*state->vfs,{},7,{});
            state->playerEyeHeight=124.343f*appearance.height;
        }
        return *state->playerEyeHeight;
    }
    catch (const std::exception&) { return 124.343f*1.03f; }
}
