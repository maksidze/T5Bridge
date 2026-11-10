#include "vmad.hpp"

#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Papyrus
{
    namespace
    {
        constexpr std::size_t MaxBytes = 64 * 1024 * 1024;
        constexpr std::size_t MaxElements = 1024 * 1024;

        class VmadReader
        {
        public:
            explicit VmadReader(std::span<const std::byte> bytes)
                : mBytes(bytes)
            {
            }

            Vmad read()
            {
                auto result = common();
                if (mPos != mBytes.size())
                    fail("unsupported trailing fragment/alias data or extra bytes after common script table");
                return result;
            }

            QuestVmad readQuest()
            {
                QuestVmad quest;
                quest.common = common();
                if (mPos == mBytes.size())
                    return quest;
                mContext = "quest fragments";
                if (u8() != 2)
                    fail("unsupported quest fragment data version", mPos - 1);
                const auto fragments = u16();
                quest.fragmentFile = string();
                count(fragments, 13, "quest fragments");
                quest.fragments.reserve(fragments);
                for (std::size_t i = 0; i < fragments; ++i)
                {
                    const auto stage = u16();
                    u16(); // Reserved.
                    const auto logIndex = u32();
                    u8(); // Fragment marker.
                    quest.fragments.push_back({ stage, logIndex, string(), string() });
                }
                mContext = "quest aliases";
                const auto aliases = u16();
                count(aliases, 14, "quest aliases");
                quest.aliases.reserve(aliases);
                for (std::size_t i = 0; i < aliases; ++i)
                {
                    mContext = "quest alias[" + std::to_string(i) + "]";
                    mObjectFormat = quest.common.objectFormat;
                    auto owner = object();
                    readVersion();
                    quest.aliases.push_back({ owner, scriptTable() });
                }
                if (mPos != mBytes.size())
                    fail("unexpected bytes after quest alias table");
                return quest;
            }

            SceneVmad readScene()
            {
                SceneVmad scene;
                scene.common = common();
                if (mPos == mBytes.size())
                    return scene;
                mContext = "scene fragments";
                if (u8() != 2)
                    fail("unsupported scene fragment data version", mPos - 1);
                const auto flags = u8();
                if ((flags & ~3) != 0)
                    fail("unknown scene fragment flags", mPos - 1);
                scene.hasSceneStart = (flags & 1) != 0;
                scene.fragmentFile = string();
                for (const auto flag : { 1, 2 })
                    if ((flags & flag) != 0)
                    {
                        u8();
                        const auto script = string();
                        auto function = string();
                        if (script != scene.fragmentFile)
                            fail("scene fragment script does not match VMAD");
                        (flag == 1 ? scene.onBegin : scene.onEnd) = std::move(function);
                    }
                const auto countPhases = u16();
                count(countPhases, 10, "scene phase fragments");
                scene.phases.reserve(countPhases);
                for (std::size_t i = 0; i < countPhases; ++i)
                {
                    const auto phaseFlags = u8();
                    const auto phaseIndex = u8();
                    if ((phaseFlags & ~3) != 0 || (!phaseFlags && !phaseIndex))
                        fail("unknown scene phase flags", mPos - 2);
                    u32(); // Reserved by Skyrim.
                    // A zero flag encodes completion at the one-based phase boundary.
                    scene.phases.push_back({ phaseFlags ? phaseFlags : std::uint8_t{2},
                        phaseFlags ? phaseIndex : std::uint8_t(phaseIndex - 1), string(), string(), phaseFlags == 0 });
                }
                if (mPos != mBytes.size())
                    fail("unexpected bytes after scene phase table");
                return scene;
            }

            PackageVmad readPackage()
            {
                PackageVmad package;
                package.common = common();
                if (mPos == mBytes.size())
                    return package;
                mContext = "package fragments";
                if (u8() != 2)
                    fail("unsupported package fragment data version", mPos - 1);
                const auto flags = u8();
                if ((flags & ~7) != 0)
                    fail("unknown package fragment flags", mPos - 1);
                package.fragmentFile = string();
                for (const auto flag : { 1, 2, 4 })
                {
                    if ((flags & flag) == 0)
                        continue;
                    u8();
                    const auto script = string();
                    auto function = string();
                    if (script != package.fragmentFile)
                        fail("package fragment script does not match VMAD");
                    if (flag == 1)
                        package.onBegin = std::move(function);
                    else if (flag == 2)
                        package.onEnd = std::move(function);
                    else
                        package.onChange = std::move(function);
                }
                if (mPos != mBytes.size())
                    fail("unexpected bytes after package fragments");
                return package;
            }

            InfoVmad readInfo()
            {
                InfoVmad info;
                info.common = common();
                if (mPos == mBytes.size())
                    return info;
                mContext = "dialogue INFO fragments";
                if (u8() != 2)
                    fail("unsupported INFO fragment data version", mPos - 1);
                const auto flags = u8();
                if ((flags & ~3) != 0)
                    fail("unknown INFO fragment flags", mPos - 1);
                info.fragmentFile = string();
                for (const auto flag : { 1, 2 })
                    if (flags & flag)
                    {
                        u8();
                        if (string() != info.fragmentFile)
                            fail("INFO fragment script does not match VMAD");
                        (flag == 1 ? info.onBegin : info.onEnd) = string();
                    }
                if (mPos != mBytes.size())
                    fail("unexpected bytes after INFO fragments");
                return info;
            }

        private:
            void readVersion()
            {
                const auto offset = mPos;
                mVersion = u16();
                if (mVersion < 1 || mVersion > 5)
                    fail("unsupported Skyrim VMAD version (expected 1..5)", offset);
                mObjectFormat = u16();
                if (mObjectFormat != 1 && mObjectFormat != 2)
                    fail("unsupported object format (expected 1 or 2)", offset + 2);
            }

            Vmad common()
            {
                if (mBytes.size() > MaxBytes)
                    fail("input exceeds 64 MiB limit");
                readVersion();
                return { mVersion, mObjectFormat, scriptTable() };
            }

            std::vector<VmadScript> scriptTable()
            {
                const auto scripts = u16();
                count(scripts, mVersion >= 4 ? 5 : 4, "scripts");
                std::vector<VmadScript> result;
                result.reserve(scripts);
                for (std::size_t i = 0; i < scripts; ++i)
                {
                    mContext = "script[" + std::to_string(i) + "]";
                    VmadScript script;
                    script.name = string();
                    // An empty regular script slot still has its status/count fields.
                    // Do not apply the different FO4 anonymous-fragment layout here.
                    if (mVersion >= 4)
                        script.flags = u8();
                    const auto properties = u16();
                    count(properties, mVersion >= 4 ? 4 : 3, "properties");
                    script.properties.reserve(properties);
                    for (std::size_t j = 0; j < properties; ++j)
                    {
                        mContext = "script[" + std::to_string(i) + "]/property[" + std::to_string(j) + "]";
                        script.properties.push_back(property());
                    }
                    result.push_back(std::move(script));
                }
                mContext = "script table";
                return result;
            }

            [[noreturn]] void fail(const std::string& reason, std::size_t offset) const
            {
                std::ostringstream message;
                message << "VMAD " << mContext << " at byte " << offset << " (0x" << std::hex << offset
                        << "): " << reason;
                throw std::runtime_error(message.str());
            }

            [[noreturn]] void fail(const std::string& reason) const { fail(reason, mPos); }

            void need(std::size_t bytes) const
            {
                if (bytes > mBytes.size() - mPos)
                    fail("truncated data (need " + std::to_string(bytes) + " bytes)");
            }

            void count(std::size_t amount, std::size_t minimumSize, const char* kind)
            {
                if (amount > (mBytes.size() - mPos) / minimumSize)
                    fail(std::string(kind) + " count exceeds remaining bytes");
                if (amount > MaxElements - mElements)
                    fail(std::string(kind) + " count exceeds decoded element limit (1048576)");
                mElements += amount;
            }

            std::uint8_t u8()
            {
                need(1);
                return std::to_integer<std::uint8_t>(mBytes[mPos++]);
            }

            std::uint16_t u16()
            {
                need(2);
                const std::uint16_t low = u8();
                return low | (static_cast<std::uint16_t>(u8()) << 8);
            }

            std::uint32_t u32()
            {
                need(4);
                std::uint32_t result = 0;
                for (unsigned shift = 0; shift < 32; shift += 8)
                    result |= static_cast<std::uint32_t>(u8()) << shift;
                return result;
            }

            std::int16_t i16()
            {
                const auto bits = u16();
                std::int16_t result;
                std::memcpy(&result, &bits, sizeof(result));
                return result;
            }

            std::string string()
            {
                const auto length = u16();
                need(length);
                // Strings are inline length-prefixed bytes, not PEX string-table indices.
                std::string result(reinterpret_cast<const char*>(mBytes.data() + mPos), length);
                mPos += length;
                return result;
            }

            VmadObject object()
            {
                need(8);
                VmadObject result;
                // TES5Edit wbDefinitionsTES5.pas:4707, Object v1/v2 union.
                if (mObjectFormat == 1)
                {
                    result.rawObjectId = u32();
                    result.alias = i16();
                    result.unused = u16();
                }
                else
                {
                    result.unused = u16();
                    result.alias = i16();
                    result.rawObjectId = u32();
                }
                return result;
            }

            VmadScalar scalar(std::uint8_t type)
            {
                switch (type)
                {
                    case 0:
                        return std::monostate{};
                    case 1:
                        return object();
                    case 2:
                        return string();
                    case 3:
                    {
                        const auto bits = u32();
                        std::int32_t result;
                        std::memcpy(&result, &bits, sizeof(result));
                        return result;
                    }
                    case 4:
                    {
                        const auto bits = u32();
                        float result;
                        static_assert(sizeof(result) == sizeof(bits));
                        std::memcpy(&result, &bits, sizeof(result));
                        return result;
                    }
                    case 5:
                    {
                        const auto result = u8();
                        if (result > 1)
                            fail("boolean value must be zero or one", mPos - 1);
                        return result != 0;
                    }
                    default:
                        fail("unknown scalar type " + std::to_string(type));
                }
            }

            VmadProperty property()
            {
                VmadProperty result;
                result.name = string();
                const auto typeOffset = mPos;
                result.type = u8();
                if (result.type > 5 && (result.type < 11 || result.type > 15))
                    fail("unknown Skyrim property type " + std::to_string(result.type), typeOffset);
                if (mVersion >= 4)
                    result.flags = u8();
                if (result.type <= 5)
                    result.value = scalar(result.type);
                else
                {
                    if (mVersion < 5)
                        fail("array properties require VMAD version 5", typeOffset);
                    const auto elementType = static_cast<std::uint8_t>(result.type - 10);
                    constexpr std::array<std::size_t, 6> minimumBytes{ 0, 8, 2, 4, 4, 1 };
                    const auto size = u32();
                    count(size, minimumBytes[elementType], "array elements");
                    result.elements.reserve(size);
                    const auto propertyContext = mContext;
                    for (std::size_t i = 0; i < size; ++i)
                    {
                        mContext = propertyContext + "/element[" + std::to_string(i) + "]";
                        result.elements.push_back(scalar(elementType));
                    }
                    mContext = propertyContext;
                }
                return result;
            }

            std::span<const std::byte> mBytes;
            std::size_t mPos = 0;
            std::size_t mElements = 0;
            std::string mContext = "header";
            std::uint16_t mVersion = 0;
            std::uint16_t mObjectFormat = 0;
        };
    }

    Vmad readVmad(std::span<const std::byte> bytes)
    {
        return VmadReader(bytes).read();
    }

    QuestVmad readQuestVmad(std::span<const std::byte> bytes)
    {
        return VmadReader(bytes).readQuest();
    }

    SceneVmad readSceneVmad(std::span<const std::byte> bytes)
    {
        return VmadReader(bytes).readScene();
    }

    PackageVmad readPackageVmad(std::span<const std::byte> bytes)
    {
        return VmadReader(bytes).readPackage();
    }

    InfoVmad readInfoVmad(std::span<const std::byte> bytes)
    {
        return VmadReader(bytes).readInfo();
    }
}
