#include "loadprogress.hpp"
#include "cellscene.hpp"
#include "actorinventory.hpp"
#include "recordattachment.hpp"

#include <components/esm/common.hpp>
#include <components/esm/esmterrain.hpp>
#include <components/esm4/grouptype.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadland.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadstat.hpp>
#include <components/esm4/reader.hpp>
#include <components/esm4/readerutils.hpp>
#include <components/files/conversion.hpp>
#include <components/files/openfile.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/vfs/manager.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <limits>
#include <streambuf>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace TES5Viewer
{
    namespace
    {
        constexpr std::uint32_t deleted = 0x20;
        constexpr std::uint32_t initiallyDisabled = 0x800;
        constexpr std::uint32_t partialCell = 0x4000;
        constexpr std::uint32_t lightPlugin = 0x200;

        std::string hexId(std::uint32_t id)
        {
            std::ostringstream out;
            out << "0x" << std::hex << std::setfill('0') << std::setw(8) << id;
            return out.str();
        }

        std::uint32_t readLittleU32(std::span<const std::byte> bytes)
        {
            return std::to_integer<std::uint32_t>(bytes[0]) | (std::to_integer<std::uint32_t>(bytes[1]) << 8)
                | (std::to_integer<std::uint32_t>(bytes[2]) << 16) | (std::to_integer<std::uint32_t>(bytes[3]) << 24);
        }

        // Independent seek cursors share immutable plugin bytes. A profile keeps its
        // plugins resident; changed size/mtime replaces the snapshot transactionally.
        struct PluginBytes { std::filesystem::file_time_type date; std::uintmax_t size; std::shared_ptr<std::string> bytes; };
        std::mutex pluginBytesMutex;
        std::map<std::filesystem::path, PluginBytes> pluginBytes;
        class PluginStream final : public std::istream
        {
            class Buffer final : public std::streambuf
            {
            public:
                explicit Buffer(std::string& bytes) { setg(bytes.data(), bytes.data(), bytes.data() + bytes.size()); }
                pos_type seekoff(off_type offset, std::ios_base::seekdir dir, std::ios_base::openmode mode) override
                {
                    if (!(mode & std::ios_base::in)) return pos_type(off_type(-1));
                    const auto base = dir == std::ios_base::beg ? 0 : dir == std::ios_base::cur ? gptr() - eback() : egptr() - eback();
                    const auto length = egptr() - eback();
                    if (offset < -base || offset > length - base) return pos_type(off_type(-1));
                    setg(eback(), eback() + base + offset, egptr());
                    return pos_type(base + offset);
                }
                pos_type seekpos(pos_type position, std::ios_base::openmode mode) override
                { return seekoff(off_type(position), std::ios_base::beg, mode); }
            };
            std::shared_ptr<std::string> bytes;
            Buffer buffer;
        public:
            explicit PluginStream(std::shared_ptr<std::string> source)
                : std::istream(nullptr), bytes(std::move(source)), buffer(*bytes) { rdbuf(&buffer); }
        };
        std::unique_ptr<std::istream> openPlugin(const std::filesystem::path& path)
        {
            const auto key = std::filesystem::canonical(path);
            const auto date = std::filesystem::last_write_time(key);
            const auto size = std::filesystem::file_size(key);
            std::lock_guard lock(pluginBytesMutex);
            auto found = pluginBytes.find(key);
            if (found == pluginBytes.end() || found->second.date != date || found->second.size != size)
            {
                loadPhase("Reading plugin bytes", size); loadItem(Files::pathToUnicodeString(path.filename()));
                auto input = Files::openBinaryInputFileStream(key);
                auto bytes = std::make_shared<std::string>();
                if (size > bytes->max_size() || size > std::uintmax_t(std::numeric_limits<std::streamsize>::max()))
                    throw std::runtime_error("Plugin is too large for memory cache");
                bytes->resize(static_cast<std::size_t>(size));
                input->read(bytes->data(), static_cast<std::streamsize>(size));
                if (!*input || std::filesystem::last_write_time(key) != date || std::filesystem::file_size(key) != size)
                    throw std::runtime_error("Plugin changed or could not be read: " + Files::pathToUnicodeString(path));
                found = pluginBytes.insert_or_assign(key, PluginBytes{date, size, std::move(bytes)}).first;
                loadProgress.done = size; loadProgress.publish();
            }
            return std::make_unique<PluginStream>(found->second.bytes);
        }

        struct Slot
        {
            bool light = false;
            std::uint32_t number = 0;
            std::uint32_t prefix = 0;
        };

        struct Plugin
        {
            std::filesystem::path path;
            std::string name;
            std::size_t index;
            std::uint32_t version;
            std::vector<std::string> masters;
            std::vector<Slot> masterSlots;
            Slot slot;
            bool lightByExtension = false;
            bool lightByFlag = false;
            bool localized = false;
        };

        void validateHeader(const ESM4::Reader& reader, const std::string& name)
        {
            const auto version = reader.esmVersion();
            const auto formVersion = reader.headerFormVersion();
            if (reader.hdr().record.typeId != ESM4::REC_TES4 || reader.hdr().record.id != 0
                || (version != ESM::VER_094 && version != ESM::VER_170 && version != ESM::VER_171)
                || (formVersion != 43 && formVersion != 44) || reader.gameFormat() != ESM::GameFormat::TES5)
                throw std::runtime_error(name + ": requires a Skyrim TES4 header (form version 43 or 44)");
        }

        std::vector<Plugin> loadOrder(const std::vector<std::filesystem::path>& paths)
        {
            if (paths.empty() || paths.size() > 254 + 4096)
                throw std::runtime_error("Specify ordered plugins within the limits of 254 full and 4096 light slots");
            std::map<std::string, std::size_t> positions;
            std::set<std::filesystem::path> resolvedPaths;
            std::vector<std::filesystem::path> hardlinkedPaths;
            for (std::size_t i = 0; i < paths.size(); ++i)
            {
                const auto name = Files::pathToUnicodeString(paths[i].filename());
                if (!positions.emplace(Misc::StringUtils::lowerCase(name), i).second)
                    throw std::runtime_error("Duplicate plugin filename in load order: " + name);
                const auto resolved = std::filesystem::canonical(paths[i]);
                if (!resolvedPaths.insert(resolved).second)
                    throw std::runtime_error("Duplicate plugin file in load order: " + name);
                // Thousands of light files must not require millions of filesystem
                // equivalence queries. Only multiply linked files need this comparison.
                if (std::filesystem::hard_link_count(resolved) > 1)
                {
                    for (const auto& previous : hardlinkedPaths)
                        if (std::filesystem::equivalent(resolved, previous))
                            throw std::runtime_error("Duplicate plugin file in load order: " + name);
                    hardlinkedPaths.push_back(resolved);
                }
            }
            {
                std::lock_guard lock(pluginBytesMutex);
                std::erase_if(pluginBytes, [&](const auto& entry) { return !resolvedPaths.contains(entry.first); });
            }
            std::vector<Plugin> result;
            std::uint32_t fullCount = 0;
            std::uint32_t lightCount = 0;
            for (std::size_t i = 0; i < paths.size(); ++i)
            {
                Plugin plugin{ paths[i], Files::pathToUnicodeString(paths[i].filename()), i, 0, {}, {}, {} };
                VFS::Manager emptyVfs;
                ESM4::ReaderDiagnostics diagnostics;
                auto stream = openPlugin(paths[i]);
                if (!*stream)
                    throw std::runtime_error("Cannot open plugin: " + plugin.name);
                ESM4::Reader reader(std::move(stream), paths[i], &emptyVfs, nullptr, true, &diagnostics);
                validateHeader(reader, plugin.name);
                plugin.version = reader.esmVersion();
                // xEdit Scan.AssignSlot considers both the TES4 flag and .esl extension.
                // Slot numbering is independent of input order, which remains override priority.
                plugin.lightByExtension = Misc::StringUtils::ciEndsWith(plugin.name, ".esl");
                plugin.lightByFlag = (reader.hdr().record.flags & lightPlugin) != 0;
                plugin.localized = (reader.hdr().record.flags & 0x80) != 0;
                plugin.slot.light = plugin.lightByExtension || plugin.lightByFlag;
                if (plugin.slot.light)
                {
                    if (lightCount >= 4096)
                        throw std::runtime_error(plugin.name + ": load order exceeds 4096 light plugin slots");
                    plugin.slot.number = lightCount++;
                    plugin.slot.prefix = 0xfe000000 | (plugin.slot.number << 12);
                }
                else
                {
                    if (fullCount >= 254)
                        throw std::runtime_error(plugin.name + ": load order exceeds 254 full plugin slots");
                    plugin.slot.number = fullCount++;
                    plugin.slot.prefix = plugin.slot.number << 24;
                }
                // xEdit AddMasters caps Skyrim SE files at 0xFD masters. This keeps
                // every on-disk local index below the reserved FE/FF prefixes.
                if (reader.getGameFiles().size() > 253)
                    throw std::runtime_error(plugin.name + ": more than 253 masters are not supported");
                std::set<std::string> uniqueMasters;
                for (const auto& master : reader.getGameFiles())
                {
                    const auto key = Misc::StringUtils::lowerCase(master.name);
                    if (!uniqueMasters.insert(key).second)
                        throw std::runtime_error(plugin.name + ": duplicate master: " + master.name);
                    const auto found = positions.find(key);
                    if (found == positions.end())
                        throw std::runtime_error(plugin.name + ": missing master: " + master.name);
                    if (found->second == i)
                        throw std::runtime_error(plugin.name + ": plugin declares itself as master");
                    if (found->second > i)
                        throw std::runtime_error(
                            plugin.name + ": master is later in explicit load order: " + master.name);
                    plugin.masters.push_back(master.name);
                    plugin.masterSlots.push_back(result[found->second].slot);
                }
                result.push_back(std::move(plugin));
            }
            return result;
        }

        std::uint32_t canonicalId(std::uint32_t raw, const Plugin& plugin, bool allowNull = false)
        {
            if (raw == 0)
            {
                if (allowNull)
                    return 0;
                throw std::runtime_error(plugin.name + ": null record/group FormID");
            }
            const std::uint32_t localIndex = raw >> 24;
            if (localIndex >= 0xfe || localIndex > plugin.masterSlots.size())
                throw std::runtime_error(plugin.name + ": invalid local FormID index in " + hexId(raw));
            const std::uint32_t object = raw & 0xffffff;
            // xEdit FileFormIDtoLoadOrderFormID: raw hardcoded IDs stay in the game
            // master. SSE 1.71 with masters also permits low IDs in nonzero slots.
            if (raw < 0x800 || (object < 0x800 && !(plugin.version == ESM::VER_171 && !plugin.masters.empty())))
                return object;
            const auto& target = localIndex < plugin.masterSlots.size() ? plugin.masterSlots[localIndex] : plugin.slot;
            if (target.light && object > 0xfff)
                throw std::runtime_error(plugin.name + ": light-plugin object exceeds 0xFFF in " + hexId(raw));
            return target.prefix | object;
        }

        bool isReference(std::uint32_t type)
        {
            return type == ESM4::REC_REFR || type == ESM4::REC_ACHR;
        }

        // GMST is identified by EditorID and can carry nonstandard hashed FormIDs.
        // It cannot supply a placement base and must not collide in the scene index.
        bool hasSceneFormId(std::uint32_t type)
        {
            return type != ESM4::REC_GMST;
        }

        struct Input
        {
            const Plugin& plugin;
            VFS::Manager emptyVfs;
            ESM4::ReaderDiagnostics diagnostics;
            std::unique_ptr<ESM4::Reader> reader;
            // Keep canonical owners from RAW group labels, before ReaderUtils remaps
            // them using its different hardcoded-ID policy. Ends are physical offsets.
            std::vector<std::pair<std::uint64_t, std::uint32_t>> cellGroups;
            std::vector<std::pair<std::uint64_t, std::uint32_t>> worldGroups;
            std::vector<std::pair<std::uint64_t, std::uint32_t>> topicGroups;

            explicit Input(const Plugin& source)
                : plugin(source)
            {
                auto stream = openPlugin(plugin.path);
                if (!*stream)
                    throw std::runtime_error("Cannot open plugin: " + plugin.name);
                reader = std::make_unique<ESM4::Reader>(
                    std::move(stream), plugin.path, &emptyVfs, nullptr, true, &diagnostics);
                validateHeader(*reader, plugin.name);
                if (reader->esmVersion() != plugin.version
                    || ((reader->hdr().record.flags & lightPlugin) != 0) != plugin.lightByFlag)
                    throw std::runtime_error(plugin.name + ": header version or light flag changed during loading");
                if (reader->getGameFiles().size() != plugin.masters.size())
                    throw std::runtime_error(plugin.name + ": master list changed during loading");
                std::map<std::string, int> indices;
                for (std::size_t i = 0; i < plugin.masters.size(); ++i)
                {
                    if (!Misc::StringUtils::ciEqual(reader->getGameFiles()[i].name, plugin.masters[i]))
                        throw std::runtime_error(plugin.name + ": master list changed during loading");
                    indices.emplace(Misc::StringUtils::lowerCase(plugin.masters[i]), static_cast<int>(i));
                }
                // Native ESM4 FormId32 helpers cannot represent mixed full/light slots.
                // Keep their discarded IDs in the local namespace: consumed record IDs,
                // group owners and NAME fields are mapped from raw bytes by canonicalId.
                reader->setModIndex(static_cast<std::uint32_t>(plugin.masters.size()));
                reader->updateModIndices(indices);
            }

            void trimGroups()
            {
                for (auto* groups : { &cellGroups, &worldGroups, &topicGroups })
                    while (!groups->empty() && diagnostics.mRecordOffset
                        && *diagnostics.mRecordOffset >= groups->back().first)
                        groups->pop_back();
            }

            std::optional<std::uint32_t> owner() const
            {
                if (cellGroups.empty())
                    return std::nullopt;
                return cellGroups.back().second;
            }

            std::optional<std::uint32_t> worldspace() const
            {
                if (worldGroups.empty())
                    return std::nullopt;
                return worldGroups.back().second;
            }

            std::optional<std::uint32_t> topic() const
            {
                if (topicGroups.empty())
                    return std::nullopt;
                return topicGroups.back().second;
            }

            template <class Visitor>
            void readAll(Visitor&& visitor)
            {
                loadPhase("Scanning resident plugin records", std::filesystem::file_size(plugin.path));
                loadItem(plugin.name);
                std::uint64_t reported = 0;
                ESM4::ReaderUtils::readAll(
                    *reader,
                    [&](ESM4::Reader& r) {
                        trimGroups();
                        if (diagnostics.mRecordOffset && *diagnostics.mRecordOffset >= reported + 1048576)
                        {
                            reported = *diagnostics.mRecordOffset;
                            loadProgress.done = reported;
                            loadProgress.publish();
                        }
                        return visitor(r);
                    },
                    [&](ESM4::Reader& r) {
                        trimGroups();
                        const auto& group = r.hdr().group;
                        if (group.type < ESM4::Grp_RecordType || group.type > ESM4::Grp_CellVisibleDistChild)
                            throw std::runtime_error(plugin.name + ": unsupported group type");
                        if (r.stackSize() >= 64)
                            throw std::runtime_error(plugin.name + ": group nesting exceeds viewer limit");
                        if (group.type == ESM4::Grp_WorldChild || group.type >= ESM4::Grp_CellChild)
                        {
                            const auto id = canonicalId(group.label.value, plugin);
                            if (group.type == ESM4::Grp_WorldChild)
                                worldGroups.emplace_back(*diagnostics.mRecordOffset + group.groupSize, id);
                            else if (group.type == ESM4::Grp_CellChild)
                                cellGroups.emplace_back(*diagnostics.mRecordOffset + group.groupSize, id);
                            else if (group.type == ESM4::Grp_TopicChild)
                                topicGroups.emplace_back(*diagnostics.mRecordOffset + group.groupSize, id);
                            else if (group.type >= ESM4::Grp_CellPersistentChild)
                            {
                                if (!owner())
                                    throw std::runtime_error(
                                        plugin.name + ": cell subgroup has no parent CELL child group");
                                if (owner() != id)
                                    throw std::runtime_error(
                                        plugin.name + ": child group label disagrees with CELL owner");
                            }
                        }
                    });
                loadProgress.done = loadProgress.total;
                loadProgress.publish();
            }

            std::uint64_t seen(std::uint32_t type, std::uint32_t field) const
            {
                const auto found = diagnostics.mSubrecords.find({ type, field });
                return found == diagnostics.mSubrecords.end() ? 0 : found->second.mHeadersSeen;
            }
        };

        struct IndexedRecord
        {
            std::uint32_t type;
            std::uint32_t flags;
            std::size_t source;
            std::optional<std::uint32_t> owner;
            std::size_t overrides = 0;
            std::uint64_t offset = 0;
        };

        struct Cell
        {
            std::string editorId;
            std::uint16_t flags = 0;
            bool hasFlags = false;
        };

        struct ReferenceFields
        {
            std::uint32_t base = 0;
            bool hasBase = false;
            bool hasPosition = false;
            bool hasEnableParent = false;
            std::uint32_t exteriorCell = 0;
            std::uint32_t enableParent = 0;
            bool oppositeOfParent = false;
        };

        struct Reference
        {
            ESM::Position position;
            float scale = 1.f;
        };

        template <class Record>
        Reference readReference(Input& input)
        {
            auto& reader = *input.reader;
            reader.getRecordData();
            Record record{};
            record.load(reader);
            if (!std::isfinite(record.mScale))
                throw std::runtime_error(input.plugin.name + ": non-finite reference scale");
            for (std::size_t i = 0; i < 3; ++i)
                if (!std::isfinite(record.mPos.pos[i]) || !std::isfinite(record.mPos.rot[i]))
                    throw std::runtime_error(input.plugin.name + ": non-finite reference position or rotation");
            return { record.mPos, record.mScale };
        }
        std::vector<std::byte> readPayload(Input& input)
        {
            const auto& plugin = input.plugin;
            auto& reader = *input.reader;
            const auto& header = reader.hdr().record;
            constexpr std::uint32_t maxPayload = 64 * 1024 * 1024;
            if (header.dataSize > maxPayload)
                throw std::runtime_error(plugin.name + ": attachment record exceeds the 64 MiB payload limit");
            if ((header.flags & ESM4::Rec_Compressed) != 0)
            {
                if (header.dataSize < 4)
                    throw std::runtime_error(plugin.name + ": compressed attachment record lacks its size");
                // Peek through an independent stream: restoring ReaderContext here
                // would count the current header twice in its parent group.
                auto physical = openPlugin(plugin.path);
                physical->seekg(
                    static_cast<std::streamoff>(*input.diagnostics.mRecordOffset + sizeof(ESM4::RecordHeader)));
                std::array<unsigned char, 4> size{};
                physical->read(reinterpret_cast<char*>(size.data()), size.size());
                if (physical->gcount() != 4)
                    throw std::runtime_error(plugin.name + ": truncated compressed attachment size");
                const auto unpacked = std::uint32_t(size[0]) | (std::uint32_t(size[1]) << 8)
                    | (std::uint32_t(size[2]) << 16) | (std::uint32_t(size[3]) << 24);
                if (unpacked > maxPayload)
                    throw std::runtime_error(plugin.name + ": unpacked attachment exceeds the 64 MiB payload limit");
            }
            reader.getRecordData();
            const auto payloadSize = reader.hdr().record.dataSize;
            std::vector<std::byte> payload(payloadSize);
            if (payloadSize != 0)
                reader.get(payload.data(), payload.size());
            reader.updateRecordRead(payloadSize);
            return payload;
        }

        template <class Visitor>
        void visitSubrecords(std::span<const std::byte> payload, const std::string& plugin, Visitor&& visitor)
        {
            // Reader::getSubRecordHeader skips XXXX payloads. Scan just this bounded
            // record envelope so an extended VMAD is preserved, not silently lost.
            const auto u16 = [&](std::size_t offset) {
                return std::to_integer<std::uint32_t>(payload[offset])
                    | (std::to_integer<std::uint32_t>(payload[offset + 1]) << 8);
            };
            const auto u32 = [&](std::size_t offset) { return u16(offset) | (u16(offset + 2) << 16); };
            std::size_t offset = 0;
            std::optional<std::uint32_t> extended;
            while (offset < payload.size())
            {
                if (payload.size() - offset < 6)
                    throw std::runtime_error(plugin + ": truncated attachment subrecord header");
                const auto type = u32(offset);
                const auto shortSize = u16(offset + 4);
                offset += 6;
                if (type == ESM::fourCC("XXXX"))
                {
                    if (extended || shortSize != 4 || payload.size() - offset < 4)
                        throw std::runtime_error(plugin + ": invalid attachment XXXX subrecord");
                    extended = u32(offset);
                    offset += 4;
                    continue;
                }
                const auto size = extended.value_or(shortSize);
                extended.reset();
                if (size > payload.size() - offset)
                    throw std::runtime_error(plugin + ": attachment subrecord extends beyond its record");
                visitor(type, payload.subspan(offset, size));
                offset += size;
            }
            if (extended)
                throw std::runtime_error(plugin + ": attachment XXXX has no following subrecord");
        }

        struct ExteriorRegion
        {
            std::uint32_t worldspace;
            int minX, minY, maxX, maxY;
            std::uint32_t landParent = 0;

            bool contains(double x, double y) const { return x >= minX && x <= maxX && y >= minY && y <= maxY; }
        };

        struct ExteriorCellFields
        {
            std::string editorId;
            std::optional<std::pair<int, int>> grid;
            std::uint8_t hiddenQuadrants = 0;
        };

        ExteriorCellFields readExteriorCell(Input& input)
        {
            ExteriorCellFields fields;
            const auto payload = readPayload(input);
            visitSubrecords(payload, input.plugin.name, [&](std::uint32_t type, std::span<const std::byte> value) {
                if (type == ESM::fourCC("EDID"))
                {
                    if (value.empty() || value.back() != std::byte{ 0 })
                        throw std::runtime_error(input.plugin.name + ": unterminated exterior CELL EDID");
                    fields.editorId.assign(reinterpret_cast<const char*>(value.data()), value.size() - 1);
                }
                else if (type == ESM::fourCC("XCLC"))
                {
                    if (fields.grid || (value.size() != 8 && value.size() != 12))
                        throw std::runtime_error(input.plugin.name + ": invalid or duplicate exterior CELL XCLC");
                    std::array<std::int32_t, 2> coordinates;
                    std::memcpy(coordinates.data(), value.data(), sizeof(coordinates));
                    fields.grid = { coordinates[0], coordinates[1] };
                    if (value.size() == 12)
                        fields.hiddenQuadrants = std::to_integer<std::uint8_t>(value[8]) & 0xf;
                }
            });
            return fields;
        }

        std::optional<LandTile> readLandTile(
            Input& input, const ExteriorCell& cell, std::uint32_t id, std::uint8_t hiddenQuadrants)
        {
            static_assert(LandTile::side == ESM4::Land::sVertsPerSide && LandTile::size == ESM4::Land::sRealSize);
            ESM4::Land land{};
            const auto payload = readPayload(input);
            visitSubrecords(payload, input.plugin.name, [&](std::uint32_t type, std::span<const std::byte> value) {
                if (type == ESM::fourCC("VHGT"))
                {
                    if ((land.mDataTypes & ESM4::Land::LAND_VHGT) || value.size() != sizeof(land.mHeightMap))
                        throw std::runtime_error(input.plugin.name + ": invalid or duplicate LAND VHGT");
                    std::memcpy(&land.mHeightMap, value.data(), value.size());
                    land.mDataTypes |= ESM4::Land::LAND_VHGT;
                }
                else if (type == ESM::fourCC("VCLR"))
                {
                    if ((land.mDataTypes & ESM4::Land::LAND_VCLR) || value.size() != sizeof(land.mVertColr))
                        throw std::runtime_error(input.plugin.name + ": invalid or duplicate LAND VCLR");
                    std::memcpy(land.mVertColr, value.data(), value.size());
                    land.mDataTypes |= ESM4::Land::LAND_VCLR;
                }
            });
            if ((land.mDataTypes & ESM4::Land::LAND_VHGT) == 0)
                return std::nullopt;

            const ESM::LandData data(land, land.mDataTypes);
            if (!std::ranges::all_of(data.getHeights(), [](float height) { return std::isfinite(height); }))
                throw std::runtime_error(input.plugin.name + ": non-finite LAND heights");
            LandTile tile;
            tile.cellId = cell.cellId;
            tile.landId = id;
            tile.x = cell.x;
            tile.y = cell.y;
            tile.hiddenQuadrants = hiddenQuadrants;
            tile.sourcePlugin = input.plugin.name;
            std::ranges::copy(data.getHeights(), tile.heights.begin());
            tile.hasColors = (land.mDataTypes & ESM4::Land::LAND_VCLR) != 0;
            if (tile.hasColors)
                std::ranges::copy(data.getColors(), tile.colors.begin());
            return tile;
        }

        std::optional<std::vector<std::byte>> extractVmad(Input& input)
        {
            const auto payload = readPayload(input);
            std::optional<std::vector<std::byte>> result;
            visitSubrecords(payload, input.plugin.name, [&](std::uint32_t type, std::span<const std::byte> value) {
                if (type == ESM::fourCC("VMAD"))
                {
                    if (result)
                        throw std::runtime_error(input.plugin.name + ": duplicate VMAD in attachment owner");
                    result.emplace(value.begin(), value.end());
                }
            });
            return result;
        }

        struct AttachmentRecord
        {
            std::uint32_t type;
            std::uint32_t flags;
            std::size_t source;
            std::uint64_t offset;
        };

        struct AttachmentContext
        {
            std::vector<Plugin> plugins;
            std::unordered_map<std::uint32_t, AttachmentRecord> records;
            mutable std::mutex payloadMutex;
            mutable std::unordered_map<std::uint32_t, std::vector<std::byte>> payloads;
        };

        const AttachmentRecord& liveRecord(const AttachmentContext& context, std::uint32_t id)
        {
            const auto found = context.records.find(id);
            if (id == 0 || found == context.records.end())
                throw std::runtime_error("Missing canonical record " + hexId(id));
            if ((found->second.flags & deleted) != 0)
                throw std::runtime_error("Deleted canonical record " + hexId(id));
            return found->second;
        }

        std::vector<std::byte> indexedPayload(const AttachmentContext& context, std::uint32_t id)
        {
            std::lock_guard lock(context.payloadMutex);
            if (const auto cached = context.payloads.find(id); cached != context.payloads.end()) return cached->second;
            const auto& record = liveRecord(context, id);
            const auto& plugin = context.plugins.at(record.source);
            Input input(plugin);
            // The initial traversal already validated parent-group boundaries. This
            // fresh reader seeks directly to the selected winning record, with no
            // inherited group counters to count that header twice.
            auto saved = input.reader->getContext();
            saved.filePos = static_cast<std::streamoff>(record.offset);
            saved.fileRead = record.offset;
            saved.recordRead = 0;
            saved.groupStack.clear();
            if (!input.reader->restoreContext(saved))
                throw std::runtime_error(plugin.name + ": cannot restore selected record " + hexId(id));
            const auto& header = input.reader->hdr().record;
            if (header.typeId != record.type || header.flags != record.flags || canonicalId(header.id, plugin) != id)
                throw std::runtime_error(plugin.name + ": selected record changed during loading");
            auto payload = readPayload(input);
            context.payloads.emplace(id, payload);
            return payload;
        }

        QuestStages questStages(const AttachmentContext& context, std::uint32_t id)
        {
            const auto& record = liveRecord(context, id);
            if (record.type != ESM4::REC_QUST)
                throw std::runtime_error("Quest metadata requires a QUST record: " + hexId(id));
            const auto& plugin = context.plugins.at(record.source);
            const auto payload = indexedPayload(context, id);
            QuestStages result;
            result.formId = id;
            result.sourcePlugin = plugin.name;
            bool hasEditorId = false;
            std::set<std::uint16_t> stages;
            visitSubrecords(payload, plugin.name, [&](std::uint32_t type, std::span<const std::byte> value) {
                if (type == ESM::fourCC("EDID"))
                {
                    if (hasEditorId || value.empty() || value.back() != std::byte{ 0 })
                        throw std::runtime_error(plugin.name + ": invalid or duplicate QUST EDID");
                    hasEditorId = true;
                    result.editorId.assign(reinterpret_cast<const char*>(value.data()), value.size() - 1);
                    if (result.editorId.find('\0') != std::string::npos)
                        throw std::runtime_error(plugin.name + ": embedded null in QUST EDID");
                }
                else if (type == ESM::fourCC("INDX"))
                {
                    // TES5 INDX is uint16 stage, uint8 flags, uint8 unused. The
                    // unused byte is not necessarily zero in the original masters.
                    if (value.size() != 4)
                        throw std::runtime_error(plugin.name + ": QUST INDX must contain four bytes");
                    const auto stage
                        = std::to_integer<std::uint16_t>(value[0]) | (std::to_integer<std::uint16_t>(value[1]) << 8);
                    stages.insert(static_cast<std::uint16_t>(stage));
                }
            });
            if (!hasEditorId || result.editorId.empty())
                throw std::runtime_error(plugin.name + ": QUST EDID is missing or empty");
            result.stages.assign(stages.begin(), stages.end());
            return result;
        }

        std::function<std::uint32_t(std::uint32_t)> makeFormResolver(
            const Plugin& plugin, const std::shared_ptr<AttachmentContext>& context)
        {
            auto source = std::make_shared<Plugin>(plugin);
            return [source, context](std::uint32_t raw) {
                const auto& plugin = *source;
                const auto resolved = canonicalId(raw, plugin, true);
                if (raw == 0)
                    return std::uint32_t{ 0 };
                const auto found = context->records.find(resolved);
                if (found == context->records.end())
                    throw std::runtime_error(
                        plugin.name + ": VMAD object " + hexId(raw) + " resolves to missing record " + hexId(resolved));
                if ((found->second.flags & deleted) != 0)
                    throw std::runtime_error(
                        plugin.name + ": VMAD object " + hexId(raw) + " resolves to deleted record " + hexId(resolved));
                return resolved;
            };
        }

        void addMetadata(RecordAttachment& attachment, const std::shared_ptr<AttachmentContext>& context)
        {
            attachment.describeForm = [context](std::uint32_t id) {
                const auto& record = liveRecord(*context, id);
                return FormMetadata{ id, record.type, context->plugins.at(record.source).name };
            };
            attachment.readQuestStages = [context](std::uint32_t id) { return questStages(*context, id); };
        }

    }

    const WorldspaceInfo& LocationCatalog::inherited(const WorldspaceInfo& world, unsigned mask) const
    {
        const WorldspaceInfo* current = &world;
        std::set<std::uint32_t> visited;
        while (current->parent && (current->parentFlags & mask) != 0)
        {
            if (!visited.insert(current->formId).second)
                return world;
            const auto parent = worlds.find(current->parent);
            if (parent == worlds.end())
                break;
            current = &parent->second;
        }
        return *current;
    }

    LocationCatalog loadLocationCatalog(const std::vector<std::filesystem::path>& paths)
    {
        const auto plugins = loadOrder(paths);
        struct CatalogCell
        {
            ExteriorCell cell;
            std::uint32_t world = 0;
            bool interior = false;
        };
        std::map<std::uint32_t, CatalogCell> cells;
        LocationCatalog catalog;
        for (const auto& plugin : plugins)
        {
            Input input(plugin);
            input.readAll([&](ESM4::Reader& reader) {
                const auto& header = reader.hdr().record;
                if (header.typeId != ESM4::REC_WRLD && header.typeId != ESM4::REC_CELL)
                    return false;
                const auto id = canonicalId(header.id, plugin);
                const bool world = header.typeId == ESM4::REC_WRLD;
                if (!world && (header.flags & partialCell) != 0)
                    return false;
                if (world)
                    catalog.worlds.erase(id);
                else
                    cells.erase(id);
                if ((header.flags & deleted) != 0)
                    return false;

                const auto payload = readPayload(input);
                const auto text = [](std::span<const std::byte> value) {
                    const auto end = std::ranges::find(value, std::byte{});
                    return std::string(reinterpret_cast<const char*>(value.data()), end - value.begin());
                };
                if (world)
                {
                    WorldspaceInfo result;
                    result.formId = id;
                    result.sourcePlugin = plugin.name;
                    visitSubrecords(payload, plugin.name, [&](auto tag, std::span<const std::byte> value) {
                        if (tag == ESM::fourCC("EDID"))
                            result.editorId = text(value);
                        else if (tag == ESM::fourCC("ICON"))
                            result.mapImage = text(value);
                        else if (tag == ESM::fourCC("WNAM") && value.size() == 4)
                        {
                            std::uint32_t raw;
                            std::memcpy(&raw, value.data(), sizeof(raw));
                            result.parent = canonicalId(raw, plugin, true);
                        }
                        else if (tag == ESM::fourCC("PNAM") && !value.empty())
                            result.parentFlags = std::to_integer<unsigned>(value[0]);
                        else if (tag == ESM::fourCC("DATA") && !value.empty())
                            result.flags = std::to_integer<unsigned>(value[0]);
                        else if (tag == ESM::fourCC("WCTR") && value.size() == 4)
                        {
                            std::array<std::int16_t, 2> center;
                            std::memcpy(center.data(), value.data(), sizeof(center));
                            result.center = std::pair<int, int>{center[0], center[1]};
                        }
                        else if (tag == ESM::fourCC("MNAM") && value.size() >= 16)
                        {
                            std::array<std::int16_t, 4> bounds;
                            std::memcpy(bounds.data(), value.data() + 8, sizeof(bounds));
                            result.mapBounds = std::array<int, 4>{bounds[0], bounds[1], bounds[2], bounds[3]};
                        }
                        else if (tag == ESM::fourCC("ONAM") && value.size() == 16)
                        {
                            std::array<float, 4> offset;
                            std::memcpy(offset.data(), value.data(), sizeof(offset));
                            if (std::ranges::all_of(offset, [](float v) { return std::isfinite(v); }))
                                result.mapOffset = offset;
                        }
                    });
                    catalog.worlds.emplace(id, std::move(result));
                }
                else
                {
                    CatalogCell result;
                    result.cell.cellId = id;
                    result.cell.sourcePlugin = plugin.name;
                    bool grid = false;
                    visitSubrecords(payload, plugin.name, [&](auto tag, std::span<const std::byte> value) {
                        if (tag == ESM::fourCC("EDID"))
                            result.cell.editorId = text(value);
                        else if (tag == ESM::fourCC("DATA") && !value.empty())
                            result.interior = (std::to_integer<unsigned>(value[0]) & ESM4::CELL_Interior) != 0;
                        else if (tag == ESM::fourCC("XCLC") && value.size() >= 8)
                        {
                            std::array<std::int32_t, 2> xy;
                            std::memcpy(xy.data(), value.data(), sizeof(xy));
                            result.cell.x = xy[0];
                            result.cell.y = xy[1];
                            grid = true;
                        }
                    });
                    if (result.interior)
                        cells.emplace(id, std::move(result));
                    else if (grid && (header.flags & ESM4::Rec_Persistent) == 0 && input.worldspace())
                    {
                        result.world = *input.worldspace();
                        cells.emplace(id, std::move(result));
                    }
                }
                return true;
            });
        }
        for (auto& [id, cell] : cells)
        {
            if (cell.interior)
                catalog.interiors.push_back({id, std::move(cell.cell.editorId), std::move(cell.cell.sourcePlugin)});
            else if (auto world = catalog.worlds.find(cell.world); world != catalog.worlds.end())
                world->second.cells.push_back(std::move(cell.cell));
        }
        return catalog;
    }

    static CellScene loadScene(const std::vector<std::filesystem::path>& paths, const std::string& editorId,
        bool withPapyrus, const ExteriorRegion* exterior, std::uint32_t interiorId = 0, bool runtime = false)
    {
        const auto plugins = loadOrder(paths);
        CellScene scene;
        for (const auto& plugin : plugins)
        {
            scene.loadOrder.push_back(plugin.name);
            PluginLoadInfo info;
            info.filename = plugin.name;
            info.kind = plugin.slot.light ? "light" : "full";
            info.inputOrder = plugin.index;
            if (plugin.slot.light)
                info.lightSlot = plugin.slot.number;
            else
                info.fullSlot = plugin.slot.number;
            info.canonicalPrefix = plugin.slot.prefix;
            info.headerVersion = std::bit_cast<float>(plugin.version);
            info.masters = plugin.masters;
            info.lightByExtension = plugin.lightByExtension;
            info.lightByFlag = plugin.lightByFlag;
            scene.pluginLoadOrder.push_back(std::move(info));
        }
        auto& stats = scene.overrideStatistics;
        for (const auto* name : { "records_read", "records_overridden", "effective_records", "cell_records_overridden",
                 "reference_records_overridden", "references_moved_between_cells", "selected_reference_overrides",
                 "selected_base_overrides", "resolved_bases", "partial_cell_records_skipped",
                 "non_formid_records_skipped" })
            stats[name] = 0;

        std::unordered_map<std::uint32_t, IndexedRecord> winners;
        // Identity-only precursor for a nameless deleted CELL tombstone. No DATA,
        // flags or placement fields are inherited from this descriptor.
        std::unordered_map<std::uint32_t, std::size_t> cellIdentitySources;
        for (const auto& plugin : plugins)
        {
            Input input(plugin);
            std::unordered_set<std::uint32_t> inFile;
            input.readAll([&](ESM4::Reader& reader) {
                const auto& header = reader.hdr().record;
                ++stats["records_read"];
                if (!hasSceneFormId(header.typeId))
                {
                    ++stats["non_formid_records_skipped"];
                    return false;
                }
                const auto id = canonicalId(header.id, plugin);
                if (!inFile.insert(id).second)
                    throw std::runtime_error(plugin.name + ": duplicate canonical record " + hexId(id));
                if (header.typeId == ESM4::REC_CELL && (header.flags & partialCell) != 0)
                {
                    ++stats["partial_cell_records_skipped"];
                    return false;
                }
                IndexedRecord record{ header.typeId, header.flags, plugin.index, input.owner(), 0,
                    *input.diagnostics.mRecordOffset };
                const auto previous = winners.find(id);
                if (previous != winners.end())
                {
                    record.overrides = previous->second.overrides + 1;
                    ++stats["records_overridden"];
                    if (previous->second.type == ESM4::REC_CELL || header.typeId == ESM4::REC_CELL)
                        ++stats["cell_records_overridden"];
                    if (isReference(previous->second.type) || isReference(header.typeId))
                        ++stats["reference_records_overridden"];
                    if (isReference(previous->second.type) && isReference(header.typeId)
                        && previous->second.owner != record.owner)
                        ++stats["references_moved_between_cells"];
                }
                if (header.typeId != ESM4::REC_CELL)
                    cellIdentitySources.erase(id);
                else if ((header.flags & deleted) == 0)
                    cellIdentitySources[id] = plugin.index;
                winners.insert_or_assign(id, record);
                return false;
            });
        }
        stats["effective_records"] = winners.size();

        std::map<std::uint32_t, Cell> cells;
        std::map<std::uint32_t, std::string> deletedCellNames;
        std::map<std::pair<int, int>, std::uint32_t> exteriorGrids;
        std::map<std::uint32_t, std::size_t> exteriorCellIndices;
        std::map<std::uint32_t, ExteriorCell> inheritedCells;
        std::map<std::uint32_t, std::uint8_t> hiddenLand;
        std::set<std::uint32_t> persistentCells;
        if (exterior)
        {
            const auto found = winners.find(exterior->worldspace);
            if (found == winners.end() || found->second.type != ESM4::REC_WRLD || (found->second.flags & deleted) != 0)
                throw std::runtime_error("Exterior worldspace not found: " + hexId(exterior->worldspace));
            scene.worldspaceId = exterior->worldspace;
            scene.cellSourcePlugin = plugins[found->second.source].name;
        }
        for (const auto& plugin : plugins)
        {
            Input input(plugin);
            input.readAll([&](ESM4::Reader& reader) {
                const auto& header = reader.hdr().record;
                if (exterior && header.typeId == ESM4::REC_WRLD
                    && canonicalId(header.id, plugin) == exterior->worldspace
                    && winners.at(exterior->worldspace).source == plugin.index)
                {
                    const auto payload = readPayload(input);
                    visitSubrecords(
                        payload, input.plugin.name, [&](std::uint32_t type, std::span<const std::byte> value) {
                            if (type == ESM::fourCC("EDID") && !value.empty() && value.back() == std::byte{ 0 })
                                scene.editorId.assign(reinterpret_cast<const char*>(value.data()), value.size() - 1);
                        });
                    return true;
                }
                if (header.typeId != ESM4::REC_CELL || (header.flags & partialCell) != 0)
                    return false;
                const auto id = canonicalId(header.id, plugin);
                const auto found = winners.find(id);
                if (found == winners.end() || found->second.type != ESM4::REC_CELL)
                    return false;
                const bool winner = found->second.source == plugin.index;
                if (exterior)
                {
                    if (!winner || (header.flags & deleted) != 0)
                        return false;
                    const auto world = input.worldspace();
                    const bool own = world == exterior->worldspace;
                    const bool inherited = exterior->landParent && world == exterior->landParent;
                    if (!own && !inherited)
                        return false;
                    // Skyrim's world-persistent CELL can carry a placeholder XCLC
                    // at (0,0). Its children belong to the grids of their transforms.
                    if ((header.flags & ESM4::Rec_Persistent) != 0)
                    {
                        if (own)
                            persistentCells.insert(id);
                        return false;
                    }
                    auto cell = readExteriorCell(input);
                    if (!cell.grid)
                        return true;
                    const auto [x, y] = *cell.grid;
                    if (!exterior->contains(x, y))
                        return true;
                    if (inherited)
                    {
                        inheritedCells.emplace(id, ExteriorCell{id, x, y, std::move(cell.editorId), plugin.name});
                        hiddenLand.emplace(id, cell.hiddenQuadrants);
                        return true;
                    }
                    if (!exteriorGrids.emplace(*cell.grid, id).second)
                        throw std::runtime_error("Duplicate effective exterior CELL grid in " + scene.editorId);
                    exteriorCellIndices.emplace(id, scene.exteriorCells.size());
                    hiddenLand.emplace(id, cell.hiddenQuadrants);
                    scene.exteriorCells.push_back({ id, x, y, std::move(cell.editorId), plugin.name });
                    return true;
                }
                if (interiorId && id != interiorId)
                    return false;
                const auto identity = cellIdentitySources.find(id);
                const bool identityOnly = (found->second.flags & deleted) != 0 && identity != cellIdentitySources.end()
                    && identity->second == plugin.index;
                if (!winner && !identityOnly)
                    return false;
                const auto beforeData = input.seen(ESM4::REC_CELL, ESM::fourCC("DATA"));
                const auto beforeEdid = input.seen(ESM4::REC_CELL, ESM::fourCC("EDID"));
                reader.getRecordData();
                ESM4::Cell record{};
                record.load(reader);
                const auto dataCount = input.seen(ESM4::REC_CELL, ESM::fourCC("DATA")) - beforeData;
                if (dataCount > 1 || input.seen(ESM4::REC_CELL, ESM::fourCC("EDID")) - beforeEdid > 1)
                    throw std::runtime_error(plugin.name + ": duplicate CELL identity/flags field");
                if (identityOnly)
                    deletedCellNames[id] = record.mEditorId;
                if (winner)
                    cells[id] = { record.mEditorId, record.mCellFlags, dataCount == 1 };
                return true;
            });
        }
        std::size_t matches = 0;
        bool cellDeleted = false;
        for (const auto& [id, cell] : cells)
        {
            const auto& winner = winners.at(id);
            const bool isDeleted = (winner.flags & deleted) != 0;
            std::string name = cell.editorId;
            if (isDeleted && name.empty())
                name = deletedCellNames[id];
            if (interiorId ? id != interiorId : !Misc::StringUtils::ciEqual(name, editorId))
                continue;
            if (!isDeleted && (!cell.hasFlags || (cell.flags & ESM4::CELL_Interior) == 0))
                throw std::runtime_error("Selected effective CELL must have interior DATA flags");
            ++matches;
            scene.cellId = id;
            scene.editorId = std::move(name);
            scene.cellSourcePlugin = plugins[winner.source].name;
            cellDeleted = isDeleted;
        }
        if (!exterior && matches != 1)
            throw std::runtime_error("Expected one effective interior CELL for EditorID '" + editorId + "'; found "
                + std::to_string(matches));
        if (exterior && scene.exteriorCells.empty() && !runtime)
            throw std::runtime_error("No effective exterior CELL in selected region of " + scene.editorId);

        std::map<std::uint32_t, ReferenceFields> fields;
        for (const auto& [id, record] : winners)
            if (isReference(record.type) && record.owner
                && (exterior ? exteriorCellIndices.contains(*record.owner) || persistentCells.contains(*record.owner)
                             : *record.owner == scene.cellId))
            {
                fields.emplace(id, ReferenceFields{});
            }

        // Validate only winning selected references, before native FormID helpers can
        // silently fold invalid slots or hardcoded IDs to a different master.
        std::set<std::uint32_t> cellsWithLand;
        std::vector<LandTile> inheritedLand;
        for (const auto& plugin : plugins)
        {
            Input input(plugin);
            input.readAll([&](ESM4::Reader& reader) {
                const auto& record = reader.hdr().record;
                if (exterior && record.typeId == ESM4::REC_LAND && input.owner()
                    && (exteriorCellIndices.contains(*input.owner()) || inheritedCells.contains(*input.owner())))
                {
                    const auto id = canonicalId(record.id, plugin);
                    const auto& winner = winners.at(id);
                    if (winner.source != plugin.index || winner.type != ESM4::REC_LAND || (winner.flags & deleted) != 0)
                        return false;
                    const auto cell = *input.owner();
                    if (!cellsWithLand.insert(cell).second)
                        throw std::runtime_error("Duplicate effective LAND for exterior CELL " + hexId(cell));
                    const auto inherited = inheritedCells.find(cell);
                    const auto& descriptor = inherited == inheritedCells.end()
                        ? scene.exteriorCells.at(exteriorCellIndices.at(cell)) : inherited->second;
                    if (auto tile = readLandTile(input, descriptor, id, hiddenLand.at(cell)))
                        (inherited == inheritedCells.end() ? scene.landTiles : inheritedLand).push_back(std::move(*tile));
                    return true;
                }
                if (!isReference(record.typeId))
                    return false;
                const auto id = canonicalId(record.id, plugin);
                const auto selected = fields.find(id);
                if (selected == fields.end() || winners.at(id).source != plugin.index)
                    return false;
                auto& value = selected->second;
                if (exterior && exteriorCellIndices.contains(*winners.at(id).owner))
                    value.exteriorCell = *winners.at(id).owner;
                constexpr std::array tags{ ESM::fourCC("NAME"), ESM::fourCC("DATA"), ESM::fourCC("XSCL"),
                    ESM::fourCC("XESP") };
                std::array<std::uint64_t, 4> before;
                for (std::size_t i = 0; i < tags.size(); ++i)
                    before[i] = input.seen(reader.hdr().record.typeId, tags[i]);
                std::set<std::uint32_t> handled;
                reader.getRecordData();
                while (reader.getSubRecordHeader())
                {
                    const auto& header = reader.subRecordHeader();
                    std::uint16_t expected = 0;
                    if (header.typeId == ESM::fourCC("NAME") || header.typeId == ESM::fourCC("XSCL"))
                        expected = 4;
                    else if (header.typeId == ESM::fourCC("DATA"))
                        expected = 24;
                    else if (header.typeId == ESM::fourCC("XESP"))
                        expected = 8;
                    if (expected != 0 && (header.dataSize != expected || !handled.insert(header.typeId).second))
                        throw std::runtime_error(plugin.name + ": invalid/duplicate placement field in " + hexId(id));
                    if (header.typeId == ESM::fourCC("NAME"))
                    {
                        std::uint32_t rawBase;
                        reader.get(rawBase);
                        value.base = canonicalId(rawBase, plugin, true);
                        value.hasBase = rawBase != 0;
                    }
                    else if (header.typeId == ESM::fourCC("DATA"))
                    {
                        value.hasPosition = true;
                        if (exterior && value.exteriorCell == 0)
                        {
                            ESM::Position position;
                            reader.get(position);
                            const double x = std::floor(static_cast<double>(position.pos[0]) / LandTile::size);
                            const double y = std::floor(static_cast<double>(position.pos[1]) / LandTile::size);
                            if (exterior->contains(x, y))
                            {
                                const auto cell = exteriorGrids.find({ static_cast<int>(x), static_cast<int>(y) });
                                if (cell != exteriorGrids.end())
                                    value.exteriorCell = cell->second;
                            }
                        }
                        else
                            reader.skipSubRecordData();
                    }
                    else if (header.typeId == ESM::fourCC("XESP") && (exterior || runtime))
                    {
                        std::uint32_t rawParent, parentFlags;
                        reader.get(rawParent);
                        reader.get(parentFlags);
                        value.hasEnableParent = true;
                        value.enableParent = canonicalId(rawParent, plugin, true);
                        value.oppositeOfParent = (parentFlags & ESM4::EnableParent::Flag_Inversed) != 0;
                    }
                    else
                    {
                        if (header.typeId == ESM::fourCC("XESP"))
                            value.hasEnableParent = true;
                        reader.skipSubRecordData();
                    }
                }
                for (std::size_t i = 0; i < tags.size(); ++i)
                    if (input.seen(reader.hdr().record.typeId, tags[i]) - before[i] != handled.count(tags[i]))
                        throw std::runtime_error(plugin.name + ": skipped extended placement field in " + hexId(id));
                return true;
            });
        }
        if (!inheritedLand.empty())
        {
            std::set<std::pair<int, int>> ownLand;
            for (const auto& tile : scene.landTiles)
                ownLand.emplace(tile.x, tile.y);
            for (auto& tile : inheritedLand)
                if (ownLand.emplace(tile.x, tile.y).second)
                    scene.landTiles.push_back(std::move(tile));
        }
        if (exterior)
            std::erase_if(fields, [](const auto& entry) { return entry.second.exteriorCell == 0; });
        for (const auto& [id, value] : fields)
            stats["selected_reference_overrides"] += winners.at(id).overrides;
        scene.referenceCount = fields.size();

        std::map<std::uint32_t, Reference> references;
        for (const auto& plugin : plugins)
        {
            Input input(plugin);
            input.readAll([&](ESM4::Reader& reader) {
                const auto type = reader.hdr().record.typeId;
                if (!isReference(type))
                    return false;
                const auto id = canonicalId(reader.hdr().record.id, plugin);
                if (!fields.contains(id) || winners.at(id).source != plugin.index)
                    return false;
                reader.setCurrCell(ESM::FormId::fromUint32(exterior ? fields.at(id).exteriorCell : scene.cellId));
                references[id] = type == ESM4::REC_REFR ? readReference<ESM4::Reference>(input)
                                                        : readReference<ESM4::ActorCharacter>(input);
                return true;
            });
        }

        std::set<std::uint32_t> needed;
        for (const auto& [id, ref] : fields)
            if (ref.hasBase)
                needed.insert(ref.base);
        std::map<std::uint32_t, std::string> models;
        for (const auto id : needed)
            if (const auto found = winners.find(id); found != winners.end())
            {
                ++stats["resolved_bases"];
                stats["selected_base_overrides"] += found->second.overrides;
            }
        for (const auto& plugin : plugins)
        {
            Input input(plugin);
            input.readAll([&](ESM4::Reader& reader) {
                if (!hasSceneFormId(reader.hdr().record.typeId))
                    return false;
                const auto id = canonicalId(reader.hdr().record.id, plugin);
                const auto winner = winners.find(id);
                if (!needed.contains(id) || winner == winners.end() || winner->second.source != plugin.index)
                    return false;
                reader.getRecordData();
                std::string model;
                if (reader.hdr().record.typeId == ESM4::REC_STAT)
                {
                    const auto before = input.seen(ESM4::REC_STAT, ESM::fourCC("MODL"));
                    ESM4::Static record{};
                    record.load(reader);
                    if (input.seen(ESM4::REC_STAT, ESM::fourCC("MODL")) - before > 1)
                        throw std::runtime_error(plugin.name + ": duplicate model field in " + hexId(id));
                    model = record.mModel.getOriginal();
                }
                else
                {
                    // Outside Papyrus-enabled ACTI rendering, non-STAT models only
                    // serve the missing_model diagnostic. ARMO MODL is an ARMA array; its
                    // world-model paths are MOD2/MOD4. Never interpret those IDs as strings.
                    const bool armor = reader.hdr().record.typeId == ESM4::REC_ARMO;
                    while (reader.getSubRecordHeader())
                    {
                        const auto tag = reader.subRecordHeader().typeId;
                        const bool modelField = armor ? tag == ESM::fourCC("MOD2") || tag == ESM::fourCC("MOD4")
                                                      : tag == ESM::fourCC("MODL");
                        if (modelField)
                        {
                            std::string candidate;
                            reader.getZString(candidate);
                            if (model.empty())
                                model = std::move(candidate);
                        }
                        else
                            reader.skipSubRecordData();
                    }
                }
                models.emplace(id, std::move(model));
                return true;
            });
        }

        std::map<std::uint32_t, std::optional<bool>> disabledCache;
        std::set<std::uint32_t> visitingEnableParents;
        const auto resolveDisabled = [&](const auto& recurse, std::uint32_t id) -> std::optional<bool> {
            if (const auto cached = disabledCache.find(id); cached != disabledCache.end())
                return cached->second;
            const auto ref = fields.find(id);
            if (ref == fields.end() || (winners.at(id).flags & deleted) != 0 || visitingEnableParents.size() >= 64
                || !visitingEnableParents.insert(id).second)
                return std::nullopt;
            std::optional<bool> disabledValue = (winners.at(id).flags & initiallyDisabled) != 0;
            if (ref->second.hasEnableParent)
            {
                const auto parent = recurse(recurse, ref->second.enableParent);
                if (parent)
                    disabledValue = *parent != ref->second.oppositeOfParent;
                else
                    disabledValue = std::nullopt;
            }
            visitingEnableParents.erase(id);
            disabledCache.emplace(id, disabledValue);
            return disabledValue;
        };
        for (const auto& [id, ref] : fields)
        {
            const auto& winner = winners.at(id);
            const auto& transform = references.at(id);
            const bool disabledByFlag = (winner.flags & initiallyDisabled) != 0;
            if ((exterior || runtime) && ref.hasPosition && (winner.flags & deleted) == 0)
                scene.exteriorReferences.emplace(id,
                    ExteriorReference{ id, ref.base, exterior ? ref.exteriorCell : scene.cellId, *winner.owner, winner.type, transform.position,
                        transform.scale, disabledByFlag, ref.enableParent, ref.oppositeOfParent,
                        winner.type == ESM4::REC_ACHR && (winner.flags & 0x200) != 0 });
            const auto disabled = exterior || runtime ? resolveDisabled(resolveDisabled, id) : std::optional<bool>(disabledByFlag);
            bool excluded = false;
            const auto exclude = [&](bool condition, const char* reason) {
                if (condition)
                {
                    ++scene.exclusions[reason];
                    excluded = true;
                }
            };
            exclude(cellDeleted, "cell_deleted");
            exclude((winner.flags & deleted) != 0, "reference_deleted");
            exclude(!exterior && !withPapyrus && disabledByFlag, "initially_disabled");
            exclude(exterior || runtime ? !disabled.has_value() && ref.hasEnableParent : ref.hasEnableParent,
                "enable_parent_not_evaluated");
            exclude(winner.type == ESM4::REC_ACHR, "actor_reference");
            exclude(!ref.hasPosition, "missing_transform");
            exclude(transform.scale <= 0, "nonpositive_scale");
            const auto base = ref.hasBase ? winners.find(ref.base) : winners.end();
            if (base == winners.end())
                exclude(true, "unresolved_base");
            else
            {
                const auto type = base->second.type;
                const bool exteriorModel = (exterior || runtime)
                    && (type == ESM4::REC_ACTI || type == ESM4::REC_MSTT || type == ESM4::REC_DOOR
                        || type == ESM4::REC_FURN || type == ESM4::REC_TREE || type == ESM4::REC_FLOR
                        || (runtime && (type == ESM4::REC_CONT || type == ESM4::REC_LIGH || isInventoryItem(type))));
                exclude(type != ESM4::REC_STAT && !(withPapyrus && type == ESM4::REC_ACTI) && !exteriorModel,
                    exterior ? "base_type_outside_exterior_scope" : "base_type_outside_static_STAT_scope");
                exclude((base->second.flags & deleted) != 0, "base_deleted");
                exclude(models.at(ref.base).empty(), "missing_model");
            }
            if (!excluded)
            {
                const auto type = base->second.type;
                scene.placements.push_back({ id, ref.base, models.at(ref.base), transform.position, transform.scale,
                    plugins[winner.source].name, plugins[base->second.source].name, disabled.value_or(false),
                    type == ESM4::REC_CONT || isInventoryItem(type) });
            }
        }
        scene.staticCount = scene.placements.size();
        if (withPapyrus)
        {
            // Reuse the effective record index. A source ACTI is extracted once and
            // then attached to each placed REFR; direct REFR VMAD remains separate.
            std::map<std::uint32_t, std::vector<std::uint32_t>> attachmentOwners;
            if (runtime)
            {
                for (const auto& [id, ref] : scene.exteriorReferences)
                {
                    attachmentOwners[id].push_back(id);
                    if (const auto base = winners.find(ref.baseId); base != winners.end() && (base->second.flags & deleted) == 0)
                        attachmentOwners[ref.baseId].push_back(id);
                }
            }
            else for (const auto& placement : scene.placements)
            {
                attachmentOwners[placement.referenceId].push_back(placement.referenceId);
                if (winners.at(placement.baseId).type == ESM4::REC_ACTI)
                    attachmentOwners[placement.baseId].push_back(placement.referenceId);
            }
            if (attachmentOwners.empty())
                return scene;
            auto context = std::make_shared<AttachmentContext>();
            context->plugins = plugins;
            context->records.reserve(winners.size());
            for (const auto& [id, winner] : winners)
                context->records.emplace(
                    id, AttachmentRecord{ winner.type, winner.flags, winner.source, winner.offset });
            std::size_t attachmentBytes = 0;
            constexpr std::size_t maxAttachmentBytes = 64 * 1024 * 1024;
            constexpr std::size_t maxAttachments = 65536;
            for (const auto& plugin : plugins)
            {
                std::set<std::uint32_t> selected;
                for (const auto& [id, owners] : attachmentOwners)
                    if (winners.at(id).source == plugin.index)
                        selected.insert(id);
                if (selected.empty())
                    continue;
                Input input(plugin);
                const auto resolver = makeFormResolver(plugin, context);
                input.readAll([&](ESM4::Reader& reader) {
                    const auto& header = reader.hdr().record;
                    if (!hasSceneFormId(header.typeId))
                        return false;
                    const auto id = canonicalId(header.id, plugin);
                    if (!selected.erase(id))
                        return false;
                    const auto& winner = winners.at(id);
                    if (header.typeId != winner.type || header.flags != winner.flags)
                        throw std::runtime_error(plugin.name + ": attachment record changed during loading");
                    if (auto vmad = extractVmad(input))
                    {
                        for (const auto owner : attachmentOwners.at(id))
                        {
                            if (scene.scriptAttachments.size() >= maxAttachments
                                || vmad->size() > maxAttachmentBytes - attachmentBytes)
                                throw std::runtime_error("Scene script attachments exceed count or 64 MiB byte limit");
                            attachmentBytes += vmad->size();
                            RecordAttachment attachment;
                            attachment.formId = id;
                            attachment.recordType = winner.type;
                            attachment.sourcePlugin = plugin.name;
                            attachment.vmad = *vmad;
                            attachment.resolveFormId = resolver;
                            attachment.ownerReferenceId = owner;
                            addMetadata(attachment, context);
                            scene.scriptAttachments.push_back(std::move(attachment));
                        }
                    }
                    return true;
                });
                if (!selected.empty())
                    throw std::runtime_error(plugin.name + ": scene attachment record disappeared during loading");
            }
        }
        return scene;
    }

    CellScene loadStaticCell(
        const std::vector<std::filesystem::path>& paths, const std::string& editorId, bool withPapyrus)
    {
        return loadScene(paths, editorId, withPapyrus, nullptr);
    }

    CellScene loadStaticCell(const std::filesystem::path& plugin, const std::string& editorId)
    {
        return loadStaticCell(std::vector<std::filesystem::path>{ plugin }, editorId);
    }

    CellScene loadRuntimeInterior(const std::vector<std::filesystem::path>& plugins, std::uint32_t formId, bool withPapyrus)
    {
        return loadScene(plugins, {}, withPapyrus, nullptr, formId, true);
    }

    CellScene loadExteriorScene(const std::vector<std::filesystem::path>& plugins, std::uint32_t worldspaceId,
        int minX, int minY, int maxX, int maxY)
    {
        const auto width = static_cast<std::int64_t>(maxX) - minX + 1;
        const auto height = static_cast<std::int64_t>(maxY) - minY + 1;
        if (worldspaceId == 0 || width <= 0 || height <= 0 || width > 256 || height > 256 || width * height > 256)
            throw std::runtime_error("Exterior region must name a worldspace and span between 1 and 256 cells");
        const ExteriorRegion exterior{ worldspaceId, minX, minY, maxX, maxY };
        return loadScene(plugins, {}, false, &exterior);
    }

    CellScene loadRuntimeExterior(const std::vector<std::filesystem::path>& plugins, const LocationCatalog& catalog,
        std::uint32_t worldspaceId, int minX, int minY, int maxX, int maxY, bool withPapyrus)
    {
        const auto& world = catalog.worlds.at(worldspaceId);
        const auto parent = catalog.inherited(world, WorldspaceInfo::useLand).formId;
        const ExteriorRegion region{worldspaceId, minX, minY, maxX, maxY, parent == worldspaceId ? 0 : parent};
        return loadScene(plugins, {}, withPapyrus, &region, 0, true);
    }

    namespace
    {
        void indexFloatSetting(Input& input, WinningRecordIndex& index)
        {
            const auto flags = input.reader->hdr().record.flags;
            std::string key;
            std::optional<float> value;
            const auto payload = readPayload(input);
            visitSubrecords(payload, input.plugin.name, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("EDID") && bytes.size() > 1 && bytes.back() == std::byte{0})
                    key = Misc::StringUtils::lowerCase(std::string_view(
                        reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1));
                else if (tag == ESM::fourCC("DATA") && bytes.size() == 4)
                    value = std::bit_cast<float>(readLittleU32(bytes));
            });
            if (!key.starts_with('f'))
                return;
            if (flags & deleted)
                index.floatSettings.erase(key);
            else if (value && std::isfinite(*value))
                index.floatSettings.insert_or_assign(std::move(key), *value);
        }

        std::shared_ptr<AttachmentContext> indexAttachments(
            const std::vector<std::filesystem::path>& paths, WinningRecordIndex* index = nullptr)
        {
            auto context = std::make_shared<AttachmentContext>();
            context->plugins = loadOrder(paths);
            std::map<std::uint32_t, std::uint32_t> actorBases, sceneOwners;
            std::set<std::uint32_t> dialogueTopics;
            std::map<std::uint32_t, std::pair<std::pair<std::uint32_t, std::uint32_t>, std::int32_t>> relationships;
            // LCTN overrides can supply ACSR while retaining the master's LCSR.
            std::map<std::pair<std::uint32_t, std::uint32_t>,
                std::vector<std::pair<std::uint32_t, std::uint32_t>>> staticLocationRefs;
            std::map<std::uint32_t, std::uint32_t> navigationOwners;
            std::unordered_map<std::uint32_t, std::uint32_t> infoTopics;
            std::vector<std::uint32_t> infoOrder;
            std::map<std::uint32_t, unsigned> startQuests;
            std::map<std::uint32_t, std::string> soundNames;
            std::map<std::uint32_t, std::string> animationObjectNames;
            std::map<std::uint32_t, std::string> movementNames;
            std::map<std::uint32_t, std::string> globalNames;
            for (const auto& plugin : context->plugins)
            {
                if (index)
                    index->plugins.push_back({ "seq/" + Files::pathToUnicodeString(plugin.path.stem()) + ".seq",
                        [context, source = plugin.index](std::uint32_t raw) {
                            return canonicalId(raw, context->plugins.at(source), true);
                        } });
                Input input(plugin);
                std::unordered_set<std::uint32_t> inFile;
                input.readAll([&](ESM4::Reader& reader) {
                    const auto& header = reader.hdr().record;
                    if (index && header.typeId == ESM4::REC_GMST)
                    {
                        indexFloatSetting(input, *index);
                        return true;
                    }
                    if (!hasSceneFormId(header.typeId))
                        return false;
                    const auto id = canonicalId(header.id, plugin);
                    if (!inFile.insert(id).second)
                        throw std::runtime_error(plugin.name + ": duplicate canonical record " + hexId(id));
                    if (header.typeId == ESM4::REC_CELL && (header.flags & partialCell) != 0)
                        return false;
                    context->records.insert_or_assign(id,
                        AttachmentRecord{ header.typeId, header.flags, plugin.index, *input.diagnostics.mRecordOffset });
                    if (!index)
                        return false;
                    if (header.typeId == ESM::fourCC("EXPL"))
                    {
                        if (header.flags & deleted)
                            index->explosions.erase(id);
                        else
                            index->explosions.insert(id);
                        return false;
                    }
                    if (header.typeId == ESM4::REC_PROJ)
                    {
                        index->projectileModels.erase(id);
                        if (header.flags & deleted)
                            return false;
                        const auto payload = readPayload(input);
                        visitSubrecords(payload, plugin.name, [&](auto tag, auto bytes) {
                            if (tag == ESM::fourCC("MODL") && !bytes.empty() && bytes.back() == std::byte{})
                                index->projectileModels.emplace(id,
                                    std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1));
                        });
                        return true;
                    }
                    if (header.typeId == ESM::fourCC("ANIO"))
                    {
                        if (const auto previous = animationObjectNames.extract(id); !previous.empty())
                            if (const auto entry = index->animationObjects.find(previous.mapped());
                                entry != index->animationObjects.end() && entry->second == id)
                                index->animationObjects.erase(entry);
                        if (header.flags & deleted) return false;
                        const auto payload = readPayload(input);
                        visitSubrecords(payload, plugin.name, [&](auto tag, auto bytes) {
                            if (tag == ESM::fourCC("EDID") && !bytes.empty() && bytes.back() == std::byte{})
                            {
                                auto name = Misc::StringUtils::lowerCase(
                                    std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1));
                                index->animationObjects.insert_or_assign(name, id);
                                animationObjectNames.insert_or_assign(id, std::move(name));
                            }
                        });
                        return true;
                    }
                    if (header.typeId == ESM::fourCC("SNDR") || header.typeId == ESM::fourCC("SOUN"))
                    {
                        if (const auto previous = soundNames.extract(id); !previous.empty())
                            if (const auto entry = index->soundsByEditorId.find(previous.mapped());
                                entry != index->soundsByEditorId.end() && entry->second == id)
                                index->soundsByEditorId.erase(entry);
                        if (header.flags & deleted)
                            return false;
                        const auto payload = readPayload(input);
                        visitSubrecords(payload, plugin.name, [&](auto tag, auto bytes) {
                            if (tag == ESM::fourCC("EDID") && !bytes.empty() && bytes.back() == std::byte{})
                            {
                                const std::string_view name(reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1);
                                auto normalized = Misc::StringUtils::lowerCase(name);
                                index->soundsByEditorId.insert_or_assign(normalized, id);
                                soundNames.insert_or_assign(id, std::move(normalized));
                            }
                        });
                        return true;
                    }
                    if (header.typeId == ESM4::REC_GLOB)
                    {
                        if (const auto previous=globalNames.extract(id); !previous.empty())
                            index->globalsByName.erase(previous.mapped());
                        if(header.flags & deleted)return false;
                        const auto payload=readPayload(input);
                        visitSubrecords(payload,plugin.name,[&](auto tag,auto bytes){
                            if(tag==ESM::fourCC("EDID") && !bytes.empty() && bytes.back()==std::byte{})
                            {
                                auto name=Misc::StringUtils::lowerCase(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()-1));
                                index->globalsByName.insert_or_assign(name,id);globalNames.insert_or_assign(id,std::move(name));
                            }
                        });
                        return true;
                    }
                    if (header.typeId == ESM4::REC_MOVT)
                    {
                        if (const auto previous = movementNames.extract(id); !previous.empty())
                            if (const auto entry = index->movementTypesByName.find(previous.mapped());
                                entry != index->movementTypesByName.end() && entry->second == id)
                                index->movementTypesByName.erase(entry);
                        if (header.flags & deleted) return false;
                        const auto payload = readPayload(input);
                        visitSubrecords(payload, plugin.name, [&](auto tag, auto bytes) {
                            if (tag == ESM::fourCC("MNAM") && !bytes.empty() && bytes.back() == std::byte{})
                            {
                                const auto name = Misc::StringUtils::lowerCase(std::string_view(
                                    reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1));
                                index->movementTypesByName.insert_or_assign(name, id);
                                movementNames.insert_or_assign(id, name);
                            }
                        });
                        return true;
                    }
                    if (header.typeId == ESM4::REC_DOBJ)
                    {
                        if (header.flags & deleted)
                        {
                            index->defaultObjects.clear();
                            return false;
                        }
                        const auto payload = readPayload(input);
                        visitSubrecords(payload, plugin.name, [&](auto tag, auto bytes) {
                            if (tag != ESM::fourCC("DNAM"))
                                return;
                            if (bytes.size() % 8)
                                throw std::runtime_error(plugin.name + ": invalid DOBJ default object array");
                            for (std::size_t offset = 0; offset < bytes.size(); offset += 8)
                            {
                                const auto key = readLittleU32(bytes.subspan(offset, 4));
                                if (key)
                                    index->defaultObjects.insert_or_assign(
                                        key, canonicalId(readLittleU32(bytes.subspan(offset + 4, 4)), plugin, true));
                            }
                        });
                        return true;
                    }
                    if (header.typeId == ESM4::REC_LCTN)
                    {
                        if (header.flags & deleted)
                        {
                            staticLocationRefs.erase({id, ESM::fourCC("ACSR")});
                            staticLocationRefs.erase({id, ESM::fourCC("LCSR")});
                            return false;
                        }
                        const auto payload = readPayload(input);
                        visitSubrecords(payload, plugin.name, [&](auto tag, auto bytes) {
                            if (tag != ESM::fourCC("ACSR") && tag != ESM::fourCC("LCSR")) return;
                            if (bytes.size() % 16)
                                throw std::runtime_error(plugin.name + ": invalid LCTN reference array");
                            auto& refs = staticLocationRefs[{id, tag}];
                            refs.clear();
                            for (std::size_t offset = 0; offset < bytes.size(); offset += 16)
                                refs.emplace_back(canonicalId(readLittleU32(bytes.subspan(offset, 4)), plugin, true),
                                    canonicalId(readLittleU32(bytes.subspan(offset + 4, 4)), plugin, true));
                        });
                        return true;
                    }
                    if (header.typeId == ESM::fourCC("RELA"))
                    {
                        relationships.erase(id);
                        if (header.flags & deleted) return false;
                        const auto payload = readPayload(input);
                        visitSubrecords(payload, plugin.name, [&](auto tag, auto bytes) {
                            if (tag != ESM::fourCC("DATA") || bytes.size() < 12) return;
                            const auto a = canonicalId(readLittleU32(bytes.first(4)), plugin, true);
                            const auto b = canonicalId(readLittleU32(bytes.subspan(4, 4)), plugin, true);
                            const auto rank = readLittleU32(bytes.subspan(8, 4)) & 0xffff;
                            if (a && b && rank <= 8)
                                relationships[id] = {{std::min(a, b), std::max(a, b)}, 4 - static_cast<std::int32_t>(rank)};
                        });
                        return true;
                    }
                    if (isReference(header.typeId))
                    {
                        index->references.erase(id);
                        if (!(header.flags & deleted))
                            index->references.emplace(id,
                                WinningRecordIndex::Reference{
                                    input.owner().value_or(0), input.worldspace().value_or(0), header.flags });
                    }
                    if (header.typeId == ESM4::REC_NAVM)
                    {
                        navigationOwners.erase(id);
                        if (!(header.flags & deleted))
                            if (const auto owner = input.owner())
                                navigationOwners.emplace(id, *owner);
                    }
                    if (header.typeId == ESM4::REC_ACHR)
                        actorBases.erase(id);
                    if (header.typeId == ESM4::REC_SCEN)
                        sceneOwners.erase(id);
                    if (header.typeId == ESM4::REC_DIAL)
                        dialogueTopics.erase(id);
                    if (header.typeId == ESM4::REC_QUST)
                        startQuests.erase(id);
                    if (header.typeId == ESM4::REC_INFO)
                    {
                        const auto topic = input.topic().value_or(0);
                        if (infoTopics.try_emplace(id, topic).second)
                            infoOrder.push_back(id);
                        else
                            infoTopics.at(id) = topic;
                    }
                    if ((header.flags & deleted) || (header.typeId != ESM4::REC_QUST
                        && header.typeId != ESM4::REC_ACHR && header.typeId != ESM4::REC_SCEN && header.typeId != ESM4::REC_DIAL
                        && (header.typeId != ESM4::REC_INFO || input.topic())))
                        return false;

                    const auto type = header.typeId;
                    const auto payload = readPayload(input);
                    visitSubrecords(payload, plugin.name, [&](auto tag, auto bytes) {
                        if (type == ESM4::REC_DIAL && tag == ESM::fourCC("DATA") && bytes.size() == 4)
                        {
                            const auto category = std::to_integer<unsigned>(bytes[1]);
                            if (category == 0 || (category == 7 && (readLittleU32(bytes) >> 16) == 79))
                                dialogueTopics.insert(id);
                        }
                        if (type == ESM4::REC_QUST && tag == ESM::fourCC("DNAM") && bytes.size() >= 3
                            && (std::to_integer<unsigned>(bytes[0]) & 1))
                            startQuests[id] = std::to_integer<unsigned>(bytes[2]);
                        if (bytes.size() != 4)
                            return;
                        const auto raw = readLittleU32(bytes);
                        if (type == ESM4::REC_ACHR && tag == ESM::fourCC("NAME"))
                            actorBases[id] = canonicalId(raw, plugin, true);
                        else if (type == ESM4::REC_SCEN && tag == ESM::fourCC("PNAM"))
                            sceneOwners[id] = canonicalId(raw, plugin, true);
                        else if (type == ESM4::REC_INFO && tag == ESM::fourCC("TPIC"))
                            infoTopics.at(id) = canonicalId(raw, plugin, true);
                    });
                    return true;
                });
            }
            if (index)
            {
                for (const auto& [id, relation] : relationships)
                    index->relationshipRanks.insert_or_assign(relation.first, relation.second);
                for (const auto& [key, refs] : staticLocationRefs)
                    for (const auto [type, reference] : refs)
                        if (const auto entry = context->records.find(reference); type && entry != context->records.end()
                            && !(entry->second.flags & deleted))
                            index->locationReferences[key.first].emplace(type, reference);
                for (const auto& [id, cell] : navigationOwners)
                    index->navigationByCell[cell].push_back(id);
                for (const auto& [id, base] : actorBases)
                    if (base)
                        index->actorsByBase[base].push_back(id);
                for (const auto& [id, quest] : sceneOwners)
                    if (quest)
                        index->scenesByQuest[quest].push_back(id);
                for (const auto id : infoOrder)
                    if (const auto topic = infoTopics.at(id); topic && !(context->records.at(id).flags & deleted))
                    {
                        index->infosByTopic[topic].push_back(id);
                        if (dialogueTopics.contains(topic)) index->topicByInfo.emplace(id, topic);
                    }
                index->playerDialogueTopics.assign(dialogueTopics.begin(), dialogueTopics.end());
                for (const auto& [id, record] : context->records)
                    if(record.type==ESM4::REC_REGN && !(record.flags & deleted))index->weatherRegions.push_back(id);
                std::ranges::sort(index->weatherRegions);
                for (const auto& [id, record] : context->records)
                    if (record.type == ESM4::REC_LCTN && !(record.flags & deleted))
                        index->locations.push_back(id);
                std::ranges::sort(index->locations);
                for (const auto& [id, record] : context->records)
                    if (!(record.flags & deleted))
                    {
                        if (record.type == ESM::fourCC("RACE")) index->characterRaces.push_back(id);
                        if (record.type == ESM::fourCC("HDPT")) index->characterHeadParts.push_back(id);
                    }
                std::ranges::sort(index->characterRaces);
                std::ranges::sort(index->characterHeadParts);
                for (const auto& [id, priority] : startQuests)
                    index->startGameQuests.push_back(id);
                std::ranges::stable_sort(index->startGameQuests, std::greater<>{}, [&](auto id) {
                    return startQuests.at(id);
                });
            }
            return context;
        }
    }

    ReadWinningRecord indexWinningRecords(const std::vector<std::filesystem::path>& paths, WinningRecordIndex* index)
    {
        if (index)
            *index = {};
        return [context = indexAttachments(paths, index)](std::uint32_t id) {
            const auto& record = liveRecord(*context, id);
            const auto& plugin = context->plugins.at(record.source);
            return RecordPayload{ { id, record.type, plugin.name }, indexedPayload(*context, id),
                [context, source = record.source](std::uint32_t raw) {
                    return canonicalId(raw, context->plugins.at(source), true);
                }, record.flags, plugin.localized };
        };
    }

    void visitSubrecords(const RecordPayload& record,
        const std::function<void(std::uint32_t, std::span<const std::byte>)>& visitor)
    {
        visitSubrecords(record.bytes, record.metadata.sourcePlugin, visitor);
    }

    RecordAttachment readRecordAttachment(
        const std::vector<std::filesystem::path>& paths, std::uint32_t canonicalFormId, bool includeBase)
    {
        if (canonicalFormId == 0)
            throw std::runtime_error("Attachment owner cannot be null");
        const auto context = indexAttachments(paths);
        const auto& owner = liveRecord(*context, canonicalFormId);
        if (!isReference(owner.type) && owner.type != ESM4::REC_ACTI)
            throw std::runtime_error("Direct VMAD attachment inspection supports only REFR, ACHR and ACTI records");

        const auto attachmentFor = [&](std::uint32_t id, std::optional<std::uint32_t>* base) {
            const auto& record = liveRecord(*context, id);
            const auto& plugin = context->plugins.at(record.source);
            RecordAttachment result;
            result.formId = id;
            result.recordType = record.type;
            result.sourcePlugin = plugin.name;
            result.resolveFormId = makeFormResolver(plugin, context);
            addMetadata(result, context);
            const auto payload = indexedPayload(*context, id);
            bool foundVmad = false;
            bool foundName = false;
            visitSubrecords(payload, plugin.name, [&](std::uint32_t type, std::span<const std::byte> value) {
                if (type == ESM::fourCC("VMAD"))
                {
                    if (foundVmad)
                        throw std::runtime_error(plugin.name + ": duplicate VMAD in attachment owner");
                    foundVmad = true;
                    if (value.empty())
                        throw std::runtime_error(plugin.name + ": empty VMAD in attachment owner");
                    result.vmad.assign(value.begin(), value.end());
                }
                else if (base && type == ESM::fourCC("NAME"))
                {
                    if (foundName || value.size() != 4)
                        throw std::runtime_error(plugin.name + ": invalid or duplicate attachment NAME");
                    foundName = true;
                    const auto raw = std::to_integer<std::uint32_t>(value[0])
                        | (std::to_integer<std::uint32_t>(value[1]) << 8)
                        | (std::to_integer<std::uint32_t>(value[2]) << 16)
                        | (std::to_integer<std::uint32_t>(value[3]) << 24);
                    *base = result.resolveFormId(raw);
                }
            });
            return result;
        };

        std::optional<std::uint32_t> base;
        auto result = attachmentFor(canonicalFormId, includeBase && owner.type == ESM4::REC_REFR ? &base : nullptr);
        if (includeBase && owner.type == ESM4::REC_REFR)
        {
            result.ownerReferenceId = canonicalFormId;
            if (base && *base != 0)
            {
                const auto& baseRecord = liveRecord(*context, *base);
                if (baseRecord.type == ESM4::REC_ACTI)
                {
                    auto attached = attachmentFor(*base, nullptr);
                    if (!attached.vmad.empty())
                    {
                        attached.ownerReferenceId = canonicalFormId;
                        result.baseAttachment = std::make_shared<RecordAttachment>(std::move(attached));
                    }
                }
            }
        }
        if (result.vmad.empty() && !result.baseAttachment)
            throw std::runtime_error(result.sourcePlugin + ": winning attachment owner has no direct VMAD"
                + (includeBase ? " or supported ACTI base VMAD" : ""));
        return result;
    }
}
