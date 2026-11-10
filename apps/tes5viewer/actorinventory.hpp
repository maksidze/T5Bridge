#pragma once

#include "recordattachment.hpp"
#include "../papyrustool/checkpoint.hpp"
#include <components/esm/fourcc.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace TES5Viewer
{
    inline bool isInventoryItem(std::uint32_t type)
    {
        using ESM::fourCC;
        return type == fourCC("WEAP") || type == fourCC("ARMO") || type == fourCC("ALCH")
            || type == fourCC("AMMO") || type == fourCC("BOOK") || type == fourCC("MISC")
            || type == fourCC("KEYM") || type == fourCC("INGR") || type == fourCC("SLGM") || type == fourCC("SCRL");
    }

    // Absolute overrides are initialized lazily from the winning NPC/container.
    // Keeping zero entries is essential: a removed authored item must stay removed.
    struct ActorInventory
    {
        std::map<std::uint32_t, std::map<std::uint32_t, std::int32_t>> items;
        std::map<std::uint32_t, std::set<std::uint32_t>> spells;

        void checkpoint(Papyrus::Checkpoint& ar) { ar(items, spells); }

        static std::uint32_t word(std::span<const std::byte> bytes, std::size_t offset = 0)
        {
            if (offset + 4 > bytes.size()) throw std::runtime_error("Truncated inventory field");
            std::uint32_t value; std::memcpy(&value, bytes.data() + offset, 4); return value;
        }

        auto& inventory(std::uint32_t owner, std::uint32_t base, const ReadWinningRecord& read)
        {
            if (auto it = items.find(owner); it != items.end()) return it->second;
            std::map<std::uint32_t, std::int32_t> initial;
            std::set<std::uint32_t> visited;
            std::uint32_t outfit = 0;
            for (auto id = base; id && visited.insert(id).second;)
            {
                auto record = read(id);
                std::uint32_t parent = 0, inheritance = 0;
                visitSubrecords(record, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("TPLT")) parent = record.mapFormId(word(bytes));
                    if (tag == ESM::fourCC("ACBS") && bytes.size() >= 20) inheritance = word(bytes, 16) >> 16;
                    if (tag == ESM::fourCC("DOFT") && !outfit) outfit = record.mapFormId(word(bytes));
                });
                if ((inheritance & 0x100) && parent) { id = parent; continue; }
                visitSubrecords(record, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("CNTO"))
                    {
                        const auto item = record.mapFormId(word(bytes));
                        const auto count = static_cast<std::int32_t>(word(bytes, 4));
                        add(initial, item, count);
                    }
                });
                break;
            }
            if (outfit)
            {
                const auto record = read(outfit);
                if (record.metadata.recordType == ESM::fourCC("OTFT"))
                    visitSubrecords(record, [&](auto tag, auto bytes) {
                        if (tag == ESM::fourCC("INAM"))
                            for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4)
                            {
                                const auto item = record.mapFormId(word(bytes, offset));
                                if (item) initial.try_emplace(item, 1);
                            }
                    });
            }
            return items.emplace(owner, std::move(initial)).first->second;
        }

        auto& spellbook(std::uint32_t actor, std::uint32_t base, const ReadWinningRecord& read)
        {
            if (auto it = spells.find(actor); it != spells.end()) return it->second;
            std::set<std::uint32_t> initial, visited;
            for (auto id = base; id && visited.insert(id).second;)
            {
                auto record = read(id);
                std::uint32_t parent = 0, inheritance = 0;
                visitSubrecords(record, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("TPLT")) parent = record.mapFormId(word(bytes));
                    if (tag == ESM::fourCC("ACBS") && bytes.size() >= 20) inheritance = word(bytes, 16) >> 16;
                });
                if ((inheritance & 0x08) && parent) { id = parent; continue; }
                visitSubrecords(record, [&](auto tag, auto bytes) {
                    if (tag == ESM::fourCC("SPLO")) initial.insert(record.mapFormId(word(bytes)));
                });
                break;
            }
            initial.erase(0);
            return spells.emplace(actor, std::move(initial)).first->second;
        }

        static void add(std::map<std::uint32_t, std::int32_t>& inventory, std::uint32_t item, std::int32_t count)
        {
            if (count <= 0 || !item) return;
            auto& total = inventory[item];
            if (count > std::numeric_limits<std::int32_t>::max() - total)
                throw std::runtime_error("Inventory count overflow");
            total += count;
        }

        static std::int32_t transfer(std::map<std::uint32_t, std::int32_t>& from,
            std::map<std::uint32_t, std::int32_t>& to, std::uint32_t item, std::int32_t count)
        {
            if (&from == &to || count <= 0) return 0;
            const auto entry = from.find(item);
            if (entry == from.end()) return 0;
            count = std::min(count, entry->second);
            add(to, item, count);
            entry->second -= count;
            return count;
        }
    };
}
