#ifndef OPENMW_APPS_PAPYRUSTOOL_VMADBINDING_H
#define OPENMW_APPS_PAPYRUSTOOL_VMADBINDING_H

#include "pex.hpp"
#include "vmad.hpp"

#include <map>
#include <functional>
#include <string_view>

namespace TES5Viewer
{
    struct RecordAttachment;
}

namespace Papyrus
{
    struct BoundProperty
    {
        VmadProperty property;
        const TES5Viewer::RecordAttachment* source;
    };

    struct ScriptBinding
    {
        std::string name;
        std::map<std::string, BoundProperty> properties;
    };

    using ScriptBindings = std::map<std::string, ScriptBinding>;

    // Apply base records first. Inherited entries overlay individual properties.
    // The attachment records must outlive resolution of the bindings.
    void applyVmad(
        ScriptBindings& bindings, const TES5Viewer::RecordAttachment& record, std::string_view onlyScript = {},
        bool standalone = false);
    void applyVmadTable(ScriptBindings& bindings, std::vector<VmadScript> scripts,
        const TES5Viewer::RecordAttachment& record, std::string_view onlyScript = {}, bool standalone = false);
    using ResolveQuestAlias = std::function<std::optional<AliasKind>(std::uint32_t quest, std::uint32_t alias)>;
    std::map<std::string, Value> resolveProperties(const ScriptBinding& binding, const ResolveQuestAlias& aliases = {});
}

#endif
