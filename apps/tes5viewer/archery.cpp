#include "archery.hpp"

#include "assetresources.hpp"

#include <components/esm/fourcc.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace TES5Viewer
{
    namespace
    {
        using ESM::fourCC;
        using Bytes = std::span<const std::byte>;

        template <class T> T field(Bytes data, std::size_t offset)
        {
            T value{};
            if (offset <= data.size() && data.size() - offset >= sizeof(T))
                std::memcpy(&value, data.data() + offset, sizeof(T));
            return value;
        }

        std::string model(Bytes data)
        {
            if (data.empty() || data.front() == std::byte{})
                return {};
            const auto end = std::ranges::find(data, std::byte{});
            return modelAssetPath(std::string_view(reinterpret_cast<const char*>(data.data()), end - data.begin()));
        }
    }

    std::optional<ArrowLoadout> readArrowLoadout(const ReadWinningRecord& read, std::uint32_t actorBase,
        std::uint32_t requestedBow, std::uint32_t requestedAmmo)
    {
        ArrowLoadout loadout;
        float ammoDamage = 0;
        bool crossbow = false;
        const auto readBow = [&](const auto& bow) {
            if (bow.metadata.recordType != fourCC("WEAP"))
                return false;
            bool archery = false, boltWeapon = false;
            float damage = 0, speed = 1;
            std::uint32_t sound = 0;
            std::string path;
            visitSubrecords(bow, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("DNAM"))
                {
                    const auto type = field<std::uint8_t>(bytes, 0);
                    boltWeapon = type == 9;
                    archery = type == 7 || (requestedAmmo && boltWeapon);
                    speed = field<float>(bytes, 4);
                }
                else if (tag == fourCC("DATA"))
                    damage = field<std::uint16_t>(bytes, 8);
                else if (tag == fourCC("MODL"))
                    path = model(bytes);
                else if (tag == fourCC("SNAM"))
                    sound = bow.mapFormId(field<std::uint32_t>(bytes, 0));
            });
            if (!archery)
                return false;
            crossbow = boltWeapon;
            loadout.bow = bow.metadata.formId;
            loadout.releaseSound = sound;
            loadout.damage = damage;
            loadout.drawSpeed = std::isfinite(speed) && speed > 0 ? speed : 1.f;
            loadout.bowModel = std::move(path);
            return true;
        };
        if (requestedBow && !readBow(read(requestedBow)))
            return std::nullopt;
        const auto readAmmo = [&](const RecordPayload& item, std::uint32_t count) {
            if (item.metadata.recordType != fourCC("AMMO")) return;
            std::uint32_t projectile = 0;
            std::string path;
            float damage = 0;
            visitSubrecords(item, [&](auto type, Bytes data) {
                if (type == fourCC("DATA") && ((field<std::uint32_t>(data, 4) & 4) != 0) != crossbow)
                {
                    projectile = item.mapFormId(field<std::uint32_t>(data, 0));
                    damage = field<float>(data, 8);
                }
                else if (type == fourCC("MODL")) path = model(data);
            });
            if (!projectile || !std::isfinite(damage)) return;
            loadout.ammo = item.metadata.formId;
            loadout.projectile = projectile;
            loadout.count = count;
            loadout.ammoModel = std::move(path);
            ammoDamage = damage;
        };
        if (requestedAmmo) readAmmo(read(requestedAmmo), 1);
        std::set<std::uint32_t> visited;
        for (auto base = actorBase; base && visited.insert(base).second && visited.size() <= 16;)
        {
            auto parent = 0u;
            bool useParentInventory = false;
            const auto npc = read(base);
            if (npc.metadata.recordType != fourCC("NPC_"))
                break;
            visitSubrecords(npc, [&](auto tag, Bytes bytes) {
                if (tag == fourCC("ACBS"))
                    useParentInventory = (field<std::uint16_t>(bytes, 18) & 0x0100) != 0;
                else if (tag == fourCC("TPLT"))
                    parent = npc.mapFormId(field<std::uint32_t>(bytes, 0));
            });
            if (useParentInventory && parent)
            {
                base = parent;
                continue;
            }
            visitSubrecords(npc, [&](auto tag, Bytes bytes) {
                if (tag != fourCC("CNTO") || (loadout.bow && loadout.ammo))
                    return;
                const auto id = npc.mapFormId(field<std::uint32_t>(bytes, 0));
                const auto count = field<std::int32_t>(bytes, 4);
                if (!id || count <= 0)
                    return;
                const auto item = read(id);
                if (!loadout.bow && readBow(item))
                    return;
                if (!loadout.ammo) readAmmo(item, static_cast<std::uint32_t>(count));
            });
            if (loadout.bow && loadout.ammo)
                break;
            break;
        }
        if (!loadout.bow || !loadout.projectile)
            return std::nullopt;
        const auto proj = read(loadout.projectile);
        if (proj.metadata.recordType != fourCC("PROJ"))
            return std::nullopt;
        visitSubrecords(proj, [&](auto tag, Bytes bytes) {
            if (tag == fourCC("DATA") && (field<std::uint16_t>(bytes, 2) & 0x40))
            {
                loadout.gravity = field<float>(bytes, 4);
                loadout.speed = field<float>(bytes, 8);
                loadout.range = field<float>(bytes, 12);
                loadout.radius = field<float>(bytes, 72);
                loadout.lifetime = field<float>(bytes, 76);
            }
            else if (tag == fourCC("MODL"))
                loadout.projectileModel = model(bytes);
        });
        if (!std::isfinite(loadout.speed) || loadout.speed <= 0 || !std::isfinite(loadout.gravity)
            || !std::isfinite(loadout.range) || loadout.range <= 0 || !std::isfinite(loadout.radius)
            || !std::isfinite(loadout.lifetime))
            return std::nullopt;
        loadout.damage += ammoDamage;
        return loadout;
    }
}
