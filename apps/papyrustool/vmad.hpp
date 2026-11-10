#ifndef OPENMW_APPS_PAPYRUSTOOL_VMAD_HPP
#define OPENMW_APPS_PAPYRUSTOOL_VMAD_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Papyrus
{
    struct VmadObject
    {
        std::uint32_t rawObjectId = 0;
        std::int16_t alias = -1;
        std::uint16_t unused = 0;
        bool operator==(const VmadObject&) const = default;
    };

    using VmadScalar = std::variant<std::monostate, VmadObject, std::string, std::int32_t, float, bool>;

    struct VmadProperty
    {
        std::string name;
        std::uint8_t type = 0;
        // Version <4 has no encoded status; its effective default is Edited (1).
        std::uint8_t flags = 1;
        VmadScalar value;
        // Types 11..15 use elements; value remains monostate (including empty arrays).
        std::vector<VmadScalar> elements;
        bool operator==(const VmadProperty&) const = default;
    };

    struct VmadScript
    {
        std::string name;
        // Version <4 has no encoded status; its effective default is Local (0).
        std::uint8_t flags = 0;
        std::vector<VmadProperty> properties;
    };

    struct Vmad
    {
        std::uint16_t version = 0;
        std::uint16_t objectFormat = 0;
        std::vector<VmadScript> scripts;
    };

    struct QuestVmadFragment
    {
        std::uint16_t stage;
        std::uint32_t logIndex;
        std::string script;
        std::string function;
    };

    struct QuestVmadAlias
    {
        VmadObject owner;
        std::vector<VmadScript> scripts;
    };

    struct QuestVmad
    {
        Vmad common;
        std::string fragmentFile;
        std::vector<QuestVmadFragment> fragments;
        std::vector<QuestVmadAlias> aliases;
    };

    struct SceneVmadPhaseFragment
    {
        std::uint8_t flags;
        std::uint8_t phase;
        std::string script;
        std::string function;
        bool legacyAliasParameter = false;
    };

    struct SceneVmad
    {
        Vmad common;
        std::string fragmentFile;
        std::vector<SceneVmadPhaseFragment> phases;
        bool hasSceneStart = false;
        std::string onBegin;
        std::string onEnd;
    };

    struct PackageVmad
    {
        Vmad common;
        std::string fragmentFile;
        std::string onEnd;
        std::string onBegin;
        std::string onChange;
    };

    struct InfoVmad
    {
        Vmad common;
        std::string fragmentFile;
        std::string onBegin;
        std::string onEnd;
    };

    // Common Skyrim script table only. Fragment/alias tails are rejected, not silently ignored.
    // Raw flags and object IDs are preserved: removal filtering and load-order resolution belong to the caller.
    Vmad readVmad(std::span<const std::byte> bytes);
    QuestVmad readQuestVmad(std::span<const std::byte> bytes);
    SceneVmad readSceneVmad(std::span<const std::byte> bytes);
    PackageVmad readPackageVmad(std::span<const std::byte> bytes);
    InfoVmad readInfoVmad(std::span<const std::byte> bytes);
}

#endif
