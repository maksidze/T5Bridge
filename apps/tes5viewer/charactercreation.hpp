#pragma once

#include "recordattachment.hpp"
#include "../papyrustool/checkpoint.hpp"
#include <components/esm/fourcc.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace TES5Viewer
{
    // Versioned C ABI: integers followed by floats; no STL ownership crosses the DLL.
    struct CharacterChoice
    {
        std::uint32_t race = 0x13746, female = 0, preset = 0;
        std::array<std::uint32_t, 7> parts{};
        float weight = 50, height = 1;
        std::array<float, 3> skin{.65f, .49f, .38f};
        void checkpoint(Papyrus::Checkpoint& ar) { ar(race, female, preset, parts, weight, height, skin); }
    };
    static_assert(sizeof(CharacterChoice) == 60);

    inline std::uint32_t characterNumber(std::span<const std::byte> bytes, std::size_t at = 0)
    {
        if (at + 4 > bytes.size()) throw std::runtime_error("Truncated character record");
        std::uint32_t n; std::memcpy(&n, bytes.data() + at, 4); return n;
    }
    inline std::string characterString(std::span<const std::byte> bytes)
    { return {reinterpret_cast<const char*>(bytes.data()), std::size_t(std::ranges::find(bytes, std::byte{}) - bytes.begin())}; }

    inline bool playableCharacterRace(const RecordPayload& record)
    {
        bool playable = false;
        if (record.metadata.recordType != ESM::fourCC("RACE")) return false;
        visitSubrecords(record, [&](auto tag, auto bytes) {
            if (tag == ESM::fourCC("DATA") && bytes.size() >= 36) playable = (characterNumber(bytes, 32) & 1) != 0;
        });
        return playable;
    }
    inline bool validCharacterPart(const ReadWinningRecord& read, std::uint32_t id, std::uint32_t race, bool female)
    {
        auto record = read(id);
        if (record.metadata.recordType != ESM::fourCC("HDPT") || (record.flags & 4)) return false;
        unsigned flags = 0; std::uint32_t races = 0;
        visitSubrecords(record, [&](auto tag, auto bytes) {
            if (tag == ESM::fourCC("DATA") && !bytes.empty()) flags = std::to_integer<unsigned>(bytes[0]);
            if (tag == ESM::fourCC("RNAM")) races = record.mapFormId(characterNumber(bytes));
        });
        if (!(flags & 1) || !(flags & (female ? 4 : 2)) || (flags & 16)) return false;
        if (!races) return true;
        bool allowed = false;
        auto list = read(races);
        visitSubrecords(list, [&](auto tag, auto bytes) {
            if (tag == ESM::fourCC("LNAM"))
                for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4)
                    allowed |= list.mapFormId(characterNumber(bytes, i)) == race;
        });
        return allowed;
    }
    inline void validateCharacter(const ReadWinningRecord& read, const CharacterChoice& choice, std::string_view name)
    {
        if (!playableCharacterRace(read(choice.race)) || choice.female > 1
            || !std::isfinite(choice.weight) || choice.weight < 0 || choice.weight > 100
            || !std::isfinite(choice.height) || choice.height < .8f || choice.height > 1.2f
            || !std::ranges::all_of(choice.skin, [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; })
            || name.size() > 128 || name.find_first_of("\r\n\t") != std::string_view::npos)
            throw std::runtime_error("Invalid character choice");
        if (choice.preset)
        {
            auto record = read(choice.race); bool allowed = false;
            visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC(choice.female ? "RPRF" : "RPRM"))
                    allowed |= record.mapFormId(characterNumber(bytes)) == choice.preset;
            });
            if (!allowed) throw std::runtime_error("Preset does not belong to the selected race and sex");
        }
        for (unsigned type = 0; type < choice.parts.size(); ++type)
            if (auto id = choice.parts[type])
            {
                if (!validCharacterPart(read, id, choice.race, choice.female)) throw std::runtime_error("Incompatible head part");
                bool same = false;
                visitSubrecords(read(id), [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("PNAM")) same = characterNumber(bytes) == type;
                });
                if (!same) throw std::runtime_error("Incorrect head part type");
            }
    }
    inline RecordPayload characterRecord(const ReadWinningRecord& read, const CharacterChoice& choice, std::string_view name)
    {
        RecordPayload npc; npc.metadata = {7, ESM::fourCC("NPC_"), "T54UE Character"};
        npc.mapFormId = [](std::uint32_t id) { return id; };
        const auto field = [&](const char* tag, const void* data, std::uint16_t size) {
            const auto append = [&](const void* p, std::size_t n) { const auto* b = static_cast<const std::byte*>(p); npc.bytes.insert(npc.bytes.end(), b, b + n); };
            append(tag, 4); append(&size, 2); append(data, size);
        };
        const char editor[] = "T54UEPlayer"; field("EDID", editor, sizeof(editor));
        std::string full(name); full += '\0'; field("FULL", full.data(), std::uint16_t(full.size()));
        std::array<std::uint32_t, 6> acbs{}; acbs[0] = choice.female; field("ACBS", acbs.data(), 24);
        field("RNAM", &choice.race, 4); field("NAM6", &choice.height, 4); field("NAM7", &choice.weight, 4);
        field("QNAM", choice.skin.data(), 12);
        std::map<unsigned, std::uint32_t> parts;
        const auto addPart = [&](std::uint32_t id) {
            auto part = read(id);
            visitSubrecords(part, [&](auto tag, auto bytes) { if (tag == ESM::fourCC("PNAM")) parts[characterNumber(bytes)] = id; });
        };
        auto race = read(choice.race); bool head = false, female = false;
        visitSubrecords(race, [&](auto tag, auto bytes) {
            if (tag == ESM::fourCC("NAM0")) head = true;
            if (tag == ESM::fourCC("MNAM") || tag == ESM::fourCC("FNAM")) female = tag == ESM::fourCC("FNAM");
            if (head && female == bool(choice.female) && tag == ESM::fourCC("HEAD")) addPart(race.mapFormId(characterNumber(bytes)));
        });
        if (choice.preset)
        {
            auto preset = read(choice.preset);
            visitSubrecords(preset, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("PNAM"))
                    for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4) addPart(preset.mapFormId(characterNumber(bytes, i)));
            });
        }
        for (unsigned type = 0; type < choice.parts.size(); ++type) if (choice.parts[type]) parts[type] = choice.parts[type];
        for (const auto& [type, id] : parts) field("PNAM", &id, 4);
        return npc;
    }

    // Keep the player's inventory, spells, factions and actor statistics. Only
    // appearance fields change; references remain in the original plugin's namespace.
    inline RecordPayload characterGameplayRecord(const ReadWinningRecord& read, const CharacterChoice& choice, std::string_view name)
    {
        const auto original = read(7);
        auto result = original; result.bytes.clear(); result.metadata.sourcePlugin = "T54UE Character";
        const auto field = [&](auto tag, std::span<const std::byte> bytes) {
            const auto size = std::uint16_t(bytes.size());
            const auto append = [&](const void* p, std::size_t n) {
                const auto* b = static_cast<const std::byte*>(p); result.bytes.insert(result.bytes.end(),b,b+n);
            };
            append(&tag,4); append(&size,2); append(bytes.data(),bytes.size());
        };
        const std::set<std::uint32_t> changed{ESM::fourCC("FULL"),ESM::fourCC("RNAM"),ESM::fourCC("PNAM"),
            ESM::fourCC("NAM6"),ESM::fourCC("NAM7"),ESM::fourCC("QNAM"),ESM::fourCC("WNAM"),ESM::fourCC("FTST"),
            ESM::fourCC("NAM9"),ESM::fourCC("NAMA"),ESM::fourCC("TINI"),ESM::fourCC("TINC"),ESM::fourCC("TINV"),ESM::fourCC("TIAS")};
        visitSubrecords(original,[&](auto tag,auto bytes) {
            if (changed.contains(tag)) return;
            if (tag == ESM::fourCC("ACBS"))
            {
                std::vector<std::byte> data(bytes.begin(),bytes.end());
                if (!data.empty()) data[0] = std::byte((std::to_integer<unsigned>(data[0]) & ~1u) | choice.female);
                field(tag,data);
            }
            else field(tag,bytes);
        });
        std::map<std::uint32_t,std::uint32_t> extendedIds;
        const auto rawId = [&](std::uint32_t canonical) {
            for (unsigned slot = 0; slot < 256; ++slot)
            {
                auto raw = (slot << 24) | (canonical & 0xFFFFFF);
                try { if (original.mapFormId(raw) == canonical) return raw; } catch (const std::exception&) {}
            }
            // A new mod's head part need not be a master of the original Player
            // record. Reserve an otherwise invalid raw namespace just for this overlay.
            for (int slot = 255; slot >= 0; --slot)
            {
                auto raw = (std::uint32_t(slot) << 24) | (canonical & 0xFFFFFF);
                if (auto found = extendedIds.find(raw); found != extendedIds.end())
                { if (found->second == canonical) return raw; continue; }
                try { original.mapFormId(raw); }
                catch (const std::exception&) { extendedIds.emplace(raw,canonical); return raw; }
            }
            throw std::runtime_error("No namespace available for the character resource");
        };
        visitSubrecords(characterRecord(read,choice,name),[&](auto tag,auto bytes) {
            if (!changed.contains(tag)) return;
            if (tag == ESM::fourCC("RNAM") || tag == ESM::fourCC("PNAM"))
            {
                auto raw = rawId(characterNumber(bytes));
                field(tag,std::as_bytes(std::span{&raw,1}));
            }
            else field(tag,bytes);
        });
        result.mapFormId = [base = original.mapFormId, ids = std::move(extendedIds)](std::uint32_t raw) {
            const auto found = ids.find(raw); return found == ids.end() ? base(raw) : found->second;
        };
        return result;
    }
}
