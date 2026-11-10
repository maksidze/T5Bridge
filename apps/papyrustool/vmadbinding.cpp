#include "vmadbinding.hpp"

#include "../tes5viewer/recordattachment.hpp"

#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>

#include <set>
#include <stdexcept>
#include <type_traits>

namespace Papyrus
{
    void applyVmad(ScriptBindings& bindings, const TES5Viewer::RecordAttachment& record, std::string_view onlyScript,
        bool standalone)
    {
        if (record.vmad.empty())
            return;
        applyVmadTable(bindings, readVmad(record.vmad).scripts, record, onlyScript, standalone);
    }

    void applyVmadTable(ScriptBindings& bindings, std::vector<VmadScript> scripts,
        const TES5Viewer::RecordAttachment& record, std::string_view onlyScript, bool standalone)
    {
        std::map<std::string, const VmadScript*> names;
        for (auto& script : scripts)
        {
            if (script.name.empty() || (!onlyScript.empty() && !Misc::StringUtils::ciEqual(onlyScript, script.name)))
                continue;
            const auto name = Misc::StringUtils::lowerCase(script.name);
            const auto [entry, unique] = names.emplace(name, &script);
            if (!unique)
            {
                if (script.flags == entry->second->flags && script.properties == entry->second->properties)
                    continue;
                throw std::runtime_error("Duplicate VMAD script '" + script.name + "'");
            }
            if (script.flags == 2 || script.flags == 3)
            {
                bindings.erase(name);
                continue;
            }
            if (script.flags == 0 || (standalone && script.flags == 1 && !bindings.contains(name)))
                bindings.insert_or_assign(name, ScriptBinding{ script.name, {} });
            else if (script.flags != 1 || !bindings.contains(name))
                throw std::runtime_error("VMAD script '" + script.name + "' has unsupported flags or no base binding");

            auto& properties = bindings.at(name).properties;
            std::set<std::string> propertyNames;
            for (auto& property : script.properties)
            {
                const auto key = Misc::StringUtils::lowerCase(property.name);
                if (!propertyNames.insert(key).second)
                    throw std::runtime_error("Duplicate VMAD property '" + property.name + "'");
                if (property.flags == 3)
                    properties.erase(key);
                else
                    properties.insert_or_assign(key, BoundProperty{ property, &record });
            }
        }
    }

    std::map<std::string, Value> resolveProperties(const ScriptBinding& binding, const ResolveQuestAlias& aliases)
    {
        std::map<std::string, Value> result;
        for (const auto& [key, bound] : binding.properties)
        {
            const auto& property = bound.property;
            if (property.flags > 3 || property.type >= 11)
                throw std::runtime_error("Unsupported VMAD property '" + property.name + "'");
            auto value = std::visit(
                [&](const auto& scalar) -> Value {
                    using T = std::decay_t<decltype(scalar)>;
                    if constexpr (std::is_same_v<T, VmadObject>)
                    {
                        if (scalar.rawObjectId == 0)
                            return std::monostate{};
                        const auto id = bound.source->resolveFormId(scalar.rawObjectId);
                        if (scalar.alias == -1)
                            return FormReference{ id };
                        if (scalar.alias < 0 || !aliases)
                            throw std::runtime_error("Quest alias property '" + property.name + "' is unsupported");
                        const auto alias = static_cast<std::uint32_t>(scalar.alias);
                        if (const auto kind = aliases(id, alias))
                            return AliasReference{ id, alias, *kind };
                        return std::monostate{};
                    }
                    else
                        return scalar;
                },
                property.value);
            result.emplace(property.name, std::move(value));
        }
        return result;
    }
}
