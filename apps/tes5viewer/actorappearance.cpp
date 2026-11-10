#include "actorappearance.hpp"

#include "assetresources.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <components/esm/fourcc.hpp>
#include <components/vfs/manager.hpp>

namespace TES5Viewer
{
    namespace
    {
        using ESM::fourCC;

        std::uint32_t number(std::span<const std::byte> bytes, std::size_t offset = 0, std::size_t size = 4)
        {
            if (offset + size > bytes.size())
                throw std::runtime_error("Truncated TES5 actor appearance field");
            std::uint32_t result = 0;
            for (std::size_t i = 0; i < size; ++i)
                result |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (i * 8);
            return result;
        }

        std::string string(std::span<const std::byte> bytes)
        {
            const auto end = std::ranges::find(bytes, std::byte{});
            return { reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(end - bytes.begin()) };
        }

        struct Npc
        {
            std::uint32_t race = 0, outfit = 0, skin = 0, templateId = 0, flags = 0, templateFlags = 0, headOwner = 0;
            std::string editorId, sourcePlugin;
            std::vector<std::uint32_t> headParts;
            float height = 1.f, weight = 50.f;
            std::optional<osg::Vec3f> skinTone;
        };

        Npc readNpc(const ReadWinningRecord& read, std::uint32_t id, std::set<std::uint32_t>& parents)
        {
            if (id == 0 || parents.size() >= 32 || !parents.insert(id).second)
                return {};
            const auto record = read(id);
            if (record.metadata.recordType == fourCC("LVLN"))
            {
                // Preview a concrete base deterministically at the opening's player level (1).
                std::vector<std::pair<std::uint32_t, std::uint32_t>> choices;
                visitSubrecords(record, [&](std::uint32_t type, std::span<const std::byte> bytes) {
                    if (type == fourCC("LVLO") && bytes.size() >= 12)
                        choices.emplace_back(number(bytes, 0, 2), record.mapFormId(number(bytes, 4)));
                });
                std::ranges::stable_sort(choices, {}, &std::pair<std::uint32_t, std::uint32_t>::first);
                for (const auto& [level, candidate] : choices)
                {
                    auto branch = parents;
                    auto npc = readNpc(read, candidate, branch);
                    if (npc.race != 0)
                        return npc;
                }
                return {};
            }
            if (record.metadata.recordType != fourCC("NPC_"))
                return {};

            Npc npc;
            npc.headOwner = id;
            npc.sourcePlugin = record.metadata.sourcePlugin;
            visitSubrecords(record, [&](std::uint32_t type, std::span<const std::byte> bytes) {
                switch (type)
                {
                    case fourCC("EDID"):
                        npc.editorId = string(bytes);
                        break;
                    case fourCC("ACBS"):
                        npc.flags = number(bytes);
                        npc.templateFlags = number(bytes, 18, 2);
                        break;
                    case fourCC("RNAM"):
                        npc.race = record.mapFormId(number(bytes));
                        break;
                    case fourCC("DOFT"):
                        npc.outfit = record.mapFormId(number(bytes));
                        break;
                    case fourCC("WNAM"):
                        npc.skin = record.mapFormId(number(bytes));
                        break;
                    case fourCC("TPLT"):
                        npc.templateId = record.mapFormId(number(bytes));
                        break;
                    case fourCC("PNAM"):
                        for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4)
                            npc.headParts.push_back(record.mapFormId(number(bytes, offset)));
                        break;
                    case fourCC("NAM6"):
                        npc.height = std::bit_cast<float>(number(bytes));
                        break;
                    case fourCC("NAM7"):
                        npc.weight = std::bit_cast<float>(number(bytes));
                        break;
                    case fourCC("QNAM"):
                        if (bytes.size() == 12)
                        {
                            const osg::Vec3f color(std::bit_cast<float>(number(bytes, 0)),
                                std::bit_cast<float>(number(bytes, 4)), std::bit_cast<float>(number(bytes, 8)));
                            if (std::isfinite(color.x()) && std::isfinite(color.y()) && std::isfinite(color.z()))
                                npc.skinTone = color;
                        }
                        break;
                }
            });
            if (npc.templateId && (npc.templateFlags & 0x101))
            {
                auto base = readNpc(read, npc.templateId, parents);
                if ((npc.templateFlags & 1) && base.race != 0)
                {
                    npc.flags = base.flags;
                    npc.race = base.race;
                    npc.height = base.height;
                    npc.weight = base.weight;
                    npc.skinTone = base.skinTone;
                    npc.headParts = std::move(base.headParts);
                    npc.headOwner = base.headOwner;
                    npc.sourcePlugin = std::move(base.sourcePlugin);
                }
                if (npc.templateFlags & 0x100)
                    npc.outfit = base.outfit;
            }
            return npc;
        }

        struct ArmorPart
        {
            std::string model;
            std::uint32_t slots = 0;
            bool weightSlider = false;
            std::array<std::string,8> skinTextures;
        };

        void resolveOutfitItem(
            const ReadWinningRecord& read, std::uint32_t item, std::vector<std::uint32_t>& armor, std::size_t depth = 0)
        {
            if (!item || depth >= 16)
                return;
            const auto record = read(item);
            if (record.metadata.recordType == fourCC("ARMO"))
                armor.push_back(item);
            if (record.metadata.recordType != fourCC("LVLI"))
                return;
            std::uint32_t flags = 0;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;
            visitSubrecords(record, [&](std::uint32_t type, std::span<const std::byte> bytes) {
                if (type == fourCC("LVLF"))
                    flags = number(bytes, 0, 1);
                else if (type == fourCC("LVLO") && bytes.size() >= 12)
                    entries.emplace_back(number(bytes, 0, 2), record.mapFormId(number(bytes, 4)));
            });
            std::ranges::stable_sort(entries, {}, &std::pair<std::uint32_t, std::uint32_t>::first);
            if (entries.empty())
                return;
            for (const auto& [level, candidate] : entries)
            {
                if (level > std::max(1u, entries.front().first))
                    break;
                resolveOutfitItem(read, candidate, armor, depth + 1);
                if ((flags & 4) == 0) // Use All
                    break;
            }
        }

        bool addModel(std::vector<std::string>& models, const VFS::Manager& vfs, const std::string& model)
        {
            if (model.empty())
                return false;
            auto path = modelAssetPath(model);
            if (!vfs.exists(VFS::Path::Normalized(path)))
                return false;
            if (std::ranges::find(models, path) == models.end())
                models.push_back(std::move(path));
            return true;
        }

        std::vector<ArmorPart> armorParts(const ReadWinningRecord& read, std::uint32_t armor, std::uint32_t race,
            std::uint32_t armorRace, bool female, std::uint32_t& equippedSlots, bool firstPerson)
        {
            std::vector<std::uint32_t> addons;
            std::set<std::uint32_t> visited;
            while (armor && visited.size() < 32 && visited.insert(armor).second && addons.empty())
            {
                const auto record = read(armor);
                if (record.metadata.recordType != fourCC("ARMO"))
                    break;
                armor = 0;
                visitSubrecords(record, [&](std::uint32_t type, std::span<const std::byte> bytes) {
                    if (type == fourCC("MODL") && bytes.size() == 4)
                        addons.push_back(record.mapFormId(number(bytes)));
                    if (type == fourCC("TNAM"))
                        armor = record.mapFormId(number(bytes));
                    if (type == fourCC("BOD2") || type == fourCC("BODT"))
                        equippedSlots = number(bytes);
                });
            }
            std::vector<ArmorPart> result;
            for (const auto addon : addons)
            {
                if (!addon)
                    continue;
                const auto record = read(addon);
                if (record.metadata.recordType != fourCC("ARMA"))
                    continue;
                std::string maleModel, femaleModel;
                std::uint32_t slots = 0, textureSet = 0;
                bool allowed = false, hasRace = false, weightSlider = false;
                visitSubrecords(record, [&](std::uint32_t type, std::span<const std::byte> bytes) {
                    if (type == fourCC(firstPerson ? "MOD4" : "MOD2"))
                        maleModel = string(bytes);
                    else if (type == fourCC(firstPerson ? "MOD5" : "MOD3"))
                        femaleModel = string(bytes);
                    else if (type == fourCC("BOD2") || type == fourCC("BODT"))
                        slots = number(bytes);
                    else if (type == fourCC(female ? "NAM1" : "NAM0"))
                        textureSet = record.mapFormId(number(bytes));
                    else if (type == fourCC("RNAM") || type == fourCC("MODL"))
                    {
                        const auto candidate = record.mapFormId(number(bytes));
                        hasRace |= candidate != 0;
                        allowed |= candidate != 0 && (candidate == race || candidate == armorRace);
                    }
                    else if (type == fourCC("DNAM") && bytes.size() >= 4)
                        weightSlider = (number(bytes, female ? 3 : 2, 1) & 2) != 0;
                });
                auto model = female && !femaleModel.empty() ? std::move(femaleModel) : std::move(maleModel);
                if ((!hasRace || allowed) && !model.empty())
                {
                    ArmorPart part{std::move(model),slots,weightSlider,{}};
                    if (textureSet)
                    {
                        const auto textures=read(textureSet);
                        if (textures.metadata.recordType==fourCC("TXST"))
                            visitSubrecords(textures,[&](auto tag,auto bytes) {
                                for (unsigned i=0;i<part.skinTextures.size();++i)
                                {
                                    const char field[]={'T','X','0',char('0'+i),'\0'};
                                    if (tag==fourCC(field)) part.skinTextures[i]=string(bytes);
                                }
                            });
                    }
                    result.push_back(std::move(part));
                }
            }
            return result;
        }

        bool addArmorPart(ActorAppearance& result, const VFS::Manager& vfs, const ArmorPart& part, float weight)
        {
            // Bind the override to the actually selected low/high weight model.
            const auto add=[&](const std::string& model) {
                if (!addModel(result.models,vfs,model)) return false;
                result.skinTextures[modelAssetPath(model)]=part.skinTextures;
                return true;
            };
            auto high = modelAssetPath(part.model);
            if (!part.weightSlider || !high.ends_with("_1.nif"))
                return add(high);
            auto low = high;
            low[low.size() - 5] = '0';
            const float blend = std::isfinite(weight) ? std::clamp(weight / 100.f, 0.f, 1.f) : .5f;
            if (!vfs.exists(VFS::Path::Normalized(low)))
                return add(high);
            if (!vfs.exists(VFS::Path::Normalized(high)))
                return add(low);
            const bool preferLow = blend < .5f;
            const auto& selected = preferLow ? low : high;
            if (!add(selected))
                return false;
            if (blend > 0.f && blend < 1.f)
                result.weightBlends.emplace(selected,
                    ActorAppearance::WeightBlend{ preferLow ? high : low, preferLow ? blend : 1.f - blend });
            return true;
        }

        bool addFaceGen(std::vector<std::string>& models, const VFS::Manager& vfs,
            std::span<const PluginLoadInfo> plugins, const Npc& npc)
        {
            constexpr std::string_view root = "actors/character/facegendata/facegeom/";
            const bool light = (npc.headOwner >> 24) == 0xFE;
            const auto mask = light ? 0xFFFu : 0xFFFFFFu;
            const auto owner = std::ranges::find(plugins, npc.headOwner & ~mask, &PluginLoadInfo::canonicalPrefix);
            std::ostringstream file;
            file << '/' << std::hex << std::setfill('0') << std::setw(8) << (npc.headOwner & mask) << ".nif";
            if (addModel(models, vfs, std::string(root) + npc.sourcePlugin + file.str()))
                return true;
            return owner != plugins.end() && addModel(models, vfs, std::string(root) + owner->filename + file.str());
        }

        void addHeadParts(std::vector<std::string>& models, const VFS::Manager& vfs, const ReadWinningRecord& read,
            std::uint32_t part, std::set<std::uint32_t>& visited)
        {
            if (part == 0 || !visited.insert(part).second)
                return;
            const auto record = read(part);
            if (record.metadata.recordType == fourCC("HDPT"))
                visitSubrecords(record, [&](std::uint32_t type, std::span<const std::byte> bytes) {
                    if (type == fourCC("MODL"))
                        addModel(models, vfs, string(bytes));
                    else if (type == fourCC("HNAM"))
                        addHeadParts(models, vfs, read, record.mapFormId(number(bytes)), visited);
                });
        }
    }

    ActorBodySize actorBodySize(const ReadWinningRecord& read, std::uint32_t id)
    {
        std::set<std::uint32_t> parents;
        const auto npc = readNpc(read, id, parents);
        ActorBodySize size{npc.height};
        if (npc.race)
            visitSubrecords(read(npc.race), [&](auto tag, auto bytes) {
                if (tag == fourCC("DATA") && bytes.size() >= 24)
                {
                    size.height *= std::bit_cast<float>(number(bytes, npc.flags & 1 ? 20 : 16));
                    if (bytes.size() >= 36) size.child = (number(bytes, 32) & 4) != 0;
                }
            });
        if (!std::isfinite(size.height) || size.height <= 0) size.height = 1.f;
        return size;
    }

    std::string actorHeadBone(const ReadWinningRecord& read, std::uint32_t npcId)
    {
        std::set<std::uint32_t> parents;
        const auto npc = readNpc(read, npcId, parents);
        if (!npc.race) return {};
        std::uint32_t data = 0;
        const auto race = read(npc.race);
        visitSubrecords(race, [&](auto tag, auto bytes) {
            if (tag == fourCC("GNAM")) data = race.mapFormId(number(bytes));
        });
        if (!data) return {};
        std::string bone, result;
        visitSubrecords(read(data), [&](auto tag, auto bytes) {
            if (tag == fourCC("BPNN")) bone = string(bytes);
            if (tag == fourCC("BPND") && bytes.size() >= 6 && number(bytes, 5, 1) == 1)
                result = bone;
        });
        return result;
    }

    ActorAppearance loadActorAppearance(const ReadWinningRecord& read, const VFS::Manager& vfs,
        std::span<const PluginLoadInfo> plugins, std::uint32_t npcId, const std::set<std::uint32_t>& equipped, bool firstPerson)
    {
        std::set<std::uint32_t> parents;
        const auto npc = readNpc(read, npcId, parents);
        if (npc.race == 0)
            return {};
        const auto race = read(npc.race);
        if (race.metadata.recordType != fourCC("RACE"))
            return {};
        const bool female = (npc.flags & 1) != 0;
        std::uint32_t skin = npc.skin, armorRace = 0;
        std::string skeleton;
        std::vector<std::string> animationProjects;
        bool skeletonIsFemale = false;
        float raceHeight = 1.f;
        visitSubrecords(race, [&](std::uint32_t type, std::span<const std::byte> bytes) {
            if (type == fourCC("WNAM") && skin == 0)
                skin = race.mapFormId(number(bytes));
            else if (type == fourCC("RNAM"))
                armorRace = race.mapFormId(number(bytes));
            else if (type == fourCC("MNAM") || type == fourCC("FNAM"))
                skeletonIsFemale = type == fourCC("FNAM");
            else if (type == fourCC("ANAM") && female == skeletonIsFemale && skeleton.empty())
                skeleton = modelAssetPath(string(bytes));
            else if (type == fourCC("MODL"))
            {
                auto path = modelAssetPath(string(bytes));
                if (path.ends_with(".hkx"))
                    animationProjects.push_back(std::move(path));
            }
            else if (type == fourCC("DATA") && bytes.size() >= 24)
                raceHeight = std::bit_cast<float>(number(bytes, female ? 20 : 16));
        });
        if (skeleton.empty())
            return {};

        ActorAppearance result;
        result.skeleton = firstPerson ? "meshes/actors/character/_1stperson/skeleton.nif" : std::move(skeleton);
        if (!animationProjects.empty())
            result.animationProject = animationProjects[female && animationProjects.size() > 1 ? 1 : 0];
        result.editorId = npc.editorId;
        result.skinTone = npc.skinTone;
        result.height
            = std::isfinite(npc.height * raceHeight) && npc.height * raceHeight > 0.f ? npc.height * raceHeight : 1.f;
        std::vector<std::uint32_t> outfit;
        for (const auto item : equipped)
            resolveOutfitItem(read, item, outfit);
        if (npc.outfit)
        {
            const auto record = read(npc.outfit);
            if (record.metadata.recordType == fourCC("OTFT"))
                visitSubrecords(record, [&](std::uint32_t type, std::span<const std::byte> bytes) {
                    if (type == fourCC("INAM"))
                        for (std::size_t i = 0; i < bytes.size(); i += 4)
                            resolveOutfitItem(read, record.mapFormId(number(bytes, i)), outfit);
                });
        }
        std::uint32_t covered = 0, usedSlots = 0;
        for (const auto item : outfit)
        {
            std::uint32_t slots = 0;
            const auto parts = armorParts(read, item, npc.race, armorRace, female, slots, firstPerson);
            if ((usedSlots & slots) != 0)
                continue;
            bool worn = false;
            for (const auto& part : parts)
                if (addArmorPart(result, vfs, part, npc.weight))
                {
                    covered |= part.slots;
                    worn = true;
                }
            if (worn)
                usedSlots |= slots;
        }
        std::uint32_t skinSlots = 0;
        for (const auto& part : armorParts(read, skin, npc.race, armorRace, female, skinSlots, firstPerson))
            if ((covered & part.slots) == 0)
                addArmorPart(result, vfs, part, npc.weight);
        if (firstPerson)
            return result;
        // FaceGen retains the HDPT editor identifiers on its shapes. Expression
        // TRI comes from the winning head-part records, including extra parts.
        std::set<std::uint32_t> morphParts;
        const auto collectMorphs = [&](auto&& self, std::uint32_t id) -> void {
            if (!id || !morphParts.insert(id).second)
                return;
            const auto record = read(id);
            if (record.metadata.recordType != fourCC("HDPT"))
                return;
            std::string name, path, model;
            std::vector<std::string> creation;
            std::uint32_t partType = ~0u;
            visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == fourCC("EDID")) name = string(bytes);
                else if (tag == fourCC("MODL")) model = modelAssetPath(string(bytes));
                else if (tag == fourCC("NAM0")) partType = number(bytes);
                else if (tag == fourCC("NAM1") && partType == 1) path = string(bytes);
                else if (tag == fourCC("NAM1") && (partType == 0 || partType == 2)) creation.push_back(modelAssetPath(string(bytes)));
                else if (tag == fourCC("HNAM")) self(self, record.mapFormId(number(bytes)));
            });
            if (!name.empty() && !path.empty())
                result.expressionMorphs.emplace(name, modelAssetPath(path));
            if (!name.empty() && !creation.empty()) result.creationMorphs.emplace(name, std::move(creation));
            if (npc.editorId == "T54UEPlayer" && !model.empty())
            {
                if (!path.empty()) result.expressionMorphs.emplace(model, modelAssetPath(path));
                if (result.creationMorphs.contains(name)) result.creationMorphs.emplace(model, result.creationMorphs.at(name));
            }
        };
        bool headData = false, headFemale = false;
        visitSubrecords(race, [&](auto tag, auto bytes) {
            if (tag == fourCC("NAM0")) headData = true;
            else if (tag == fourCC("MNAM") || tag == fourCC("FNAM")) headFemale = tag == fourCC("FNAM");
            else if (headData && headFemale == female && tag == fourCC("HEAD"))
                collectMorphs(collectMorphs, race.mapFormId(number(bytes)));
        });
        for (const auto part : npc.headParts)
            collectMorphs(collectMorphs, part);
        if (npc.editorId == "T54UEPlayer")
        {
            const VFS::Path::Normalized ini("meshes/actors/character/facegenmorphs/racemenu.esp/morphs.ini");
            if (vfs.exists(ini))
            {
                auto stream = vfs.get(ini); std::string line;
                const auto trim = [](std::string text) {
                    const auto first = text.find_first_not_of(" \t\r\n");
                    return first == std::string::npos ? std::string{} : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
                };
                while (std::getline(*stream, line))
                {
                    auto equal = line.find('=');
                    if (equal == std::string::npos || trim(line.substr(0,equal)) != "extension") continue;
                    std::vector<std::string> files; std::istringstream values(line.substr(equal + 1)); std::string value;
                    while (std::getline(values, value, ',')) files.push_back(trim(value));
                    if (files.size() < 2) continue;
                    const auto source = modelAssetPath(files.front());
                    for (auto& [shape, paths] : result.creationMorphs)
                        if (std::ranges::find(paths, source) != paths.end())
                            for (std::size_t i = 1; i < files.size(); ++i)
                            {
                                auto extra = modelAssetPath("actors/character/facegenmorphs/morphs/" + files[i]);
                                if (vfs.exists(VFS::Path::Normalized(extra)) && std::ranges::find(paths, extra) == paths.end()) paths.push_back(std::move(extra));
                            }
                }
            }
        }
        if (!addFaceGen(result.models, vfs, plugins, npc))
        {
            std::set<std::uint32_t> visited;
            for (const auto part : npc.headParts)
                addHeadParts(result.models, vfs, read, part, visited);
        }
        return result;
    }
}
