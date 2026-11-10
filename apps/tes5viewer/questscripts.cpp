#include "questscripts.hpp"
#include "checkpointtypes.hpp"

#include "../papyrustool/errorlog.hpp"
#include "cellscene.hpp"

#include <components/esm/fourcc.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/vfs/manager.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace TES5Viewer
{
    namespace
    {
        using namespace Papyrus;
        using ESM::fourCC;
        constexpr unsigned optionalAlias = 0x0002;
        constexpr unsigned reuseAlias = 0x0008;
        constexpr unsigned allowDeadAlias = 0x0010;
        constexpr unsigned loadedAreaAlias = 0x0020;
        constexpr unsigned allowDisabledAlias = 0x0080;
        constexpr unsigned allowReservedAlias = 0x0200;
        constexpr unsigned closestAlias = 0x2000;
        constexpr unsigned initiallyDisabledAlias = 0x8000;
        constexpr std::uint32_t playerReference = 0x14;

        std::uint32_t number(std::span<const std::byte> bytes, std::size_t offset = 0, std::size_t size = 4)
        {
            if (offset > bytes.size() || size > bytes.size() - offset)
                throw std::runtime_error("Truncated QUST field");
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < size; ++i)
                value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
            return value;
        }

        bool aliasTag(std::uint32_t tag) { return tag == fourCC("ALST") || tag == fourCC("ALLS"); }
        AliasKind aliasKind(std::uint32_t tag) { return tag == fourCC("ALST") ? AliasKind::Reference : AliasKind::Location; }

        template <class T>
        T argument(const std::vector<Value>& values, std::size_t index)
        {
            if (index < values.size())
                if (const auto* value = std::get_if<T>(&values[index]))
                    return *value;
            throw std::runtime_error("Invalid Papyrus quest native argument " + std::to_string(index + 1));
        }

        void arity(const std::vector<Value>& values, std::size_t min, std::size_t max)
        {
            if (values.size() < min || values.size() > max)
                throw std::runtime_error("Invalid Papyrus quest native argument count");
        }

        Value formValue(std::uint32_t id) { return id ? Value(FormReference{id}) : Value{}; }

        struct Alias
        {
            AliasKind kind;
            std::uint16_t flags = 0;
            std::uint32_t fixed = 0, actorBase = 0, externalQuest = 0;
            std::uint32_t locationType = 0, createdBase = 0;
            std::optional<std::uint32_t> externalAlias, locationAlias, anchorAlias, forceAlias, linkedFrom;
            bool createIn = false, fromEvent = false;
            std::string name;
            std::vector<std::span<const std::byte>> conditions;
            std::vector<std::uint32_t> packages;
            std::optional<std::uint32_t> fill;
        };

        struct Entry
        {
            unsigned flags;
            std::vector<std::span<const std::byte>> conditions;
        };

        struct Stage
        {
            unsigned flags;
            std::vector<Entry> entries;
        };

        struct Objective
        {
            bool displayed = false, completed = false, failed = false;
            void checkpoint(Checkpoint& ar) { ar(displayed, completed, failed); }
        };

        struct ObjectiveTarget
        {
            std::uint32_t alias;
            std::vector<std::span<const std::byte>> conditions;
        };

        struct Instance
        {
            std::string name;
            ScriptInstance vm;
            std::optional<std::uint32_t> aliasId;
            std::set<std::pair<std::uint32_t, std::string>> animationEvents;
            bool initialized = false, failed = false;
            bool cellLoaded = false;
        };

        struct Quest
        {
            RecordPayload record;
            std::string editorId;
            QuestVmad vmad;
            std::map<std::uint16_t, Stage> stages;
            std::map<std::uint16_t, Objective> objectives;
            std::map<std::uint16_t, std::vector<ObjectiveTarget>> objectiveTargets;
            std::map<std::uint32_t, Alias> aliases;
            std::vector<std::uint32_t> aliasOrder;
            std::map<std::string, std::shared_ptr<Instance>> instances;
            std::vector<std::shared_ptr<Instance>> aliasInstances;
            std::vector<std::uint32_t> created;
            std::set<std::uint16_t> done, pending;
            std::string startFailure;
            std::uint16_t flags = 0, current = 0;
            unsigned priority = 0;
            std::uint64_t generation = 0;
            bool running = false, completed = false, failed = false;

            explicit Quest(RecordPayload payload) : record(std::move(payload))
            {
                Stage* stage = nullptr;
                Entry* entry = nullptr;
                Alias* alias = nullptr;
                std::vector<ObjectiveTarget>* targets = nullptr;
                visitSubrecords(record, [&](std::uint32_t tag, std::span<const std::byte> bytes) {
                    if (tag == fourCC("EDID") && !bytes.empty() && bytes.back() == std::byte{0})
                        editorId.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1);
                    else if (tag == fourCC("VMAD"))
                        vmad = readQuestVmad(bytes);
                    else if (tag == fourCC("DNAM"))
                    {
                        flags = static_cast<std::uint16_t>(number(bytes, 0, 2));
                        priority = number(bytes, 2, 1);
                    }
                    else if (tag == fourCC("INDX"))
                    {
                        const auto id = static_cast<std::uint16_t>(number(bytes, 0, 2));
                        auto [it, inserted] = stages.try_emplace(id, Stage{number(bytes, 2, 1), {}});
                        if (!inserted)
                            throw std::runtime_error("Duplicate QUST stage");
                        stage = &it->second;
                        entry = nullptr;
                    }
                    else if (tag == fourCC("QSDT") && stage)
                        entry = &stage->entries.emplace_back(Entry{number(bytes, 0, 1), {}});
                    else if (tag == fourCC("CTDA") && entry)
                        entry->conditions.push_back(bytes);
                    else if (tag == fourCC("QSTA") && targets)
                        targets->push_back({number(bytes), {}});
                    else if (tag == fourCC("CTDA") && targets && !targets->empty())
                        targets->back().conditions.push_back(bytes);
                    else if (tag == fourCC("QOBJ") || tag == fourCC("ANAM") || aliasTag(tag))
                    {
                        stage = nullptr;
                        entry = nullptr;
                        targets = nullptr;
                        if (tag == fourCC("QOBJ"))
                        {
                            const auto id = static_cast<std::uint16_t>(number(bytes, 0, 2));
                            objectives.try_emplace(id);
                            targets = &objectiveTargets[id];
                        }
                        if (aliasTag(tag))
                        {
                            const auto id = number(bytes);
                            auto [it, inserted] = aliases.try_emplace(id, Alias{aliasKind(tag)});
                            if (!inserted)
                                throw std::runtime_error("Duplicate QUST alias");
                            alias = &it->second;
                            aliasOrder.push_back(id);
                        }
                    }
                    else if (tag == fourCC("ALED"))
                        alias = nullptr;
                    else if (alias)
                    {
                        if (tag == fourCC("ALID") && !bytes.empty() && bytes.back() == std::byte{0})
                            alias->name.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1);
                        else if (tag == fourCC("FNAM"))
                            alias->flags = static_cast<std::uint16_t>(number(bytes, 0, 2));
                        else if (tag == fourCC("ALFR") && alias->kind == AliasKind::Reference)
                            alias->fixed = record.mapFormId(number(bytes));
                        else if (tag == fourCC("ALFL") && alias->kind == AliasKind::Location)
                            alias->fixed = record.mapFormId(number(bytes));
                        else if (tag == fourCC("ALUA") && alias->kind == AliasKind::Reference)
                            alias->actorBase = record.mapFormId(number(bytes));
                        else if (tag == fourCC("ALEQ"))
                            alias->externalQuest = record.mapFormId(number(bytes));
                        else if (tag == fourCC("ALEA"))
                            alias->externalAlias = number(bytes);
                        else if (tag == fourCC("ALFA"))
                            alias->locationAlias = number(bytes);
                        else if (tag == fourCC("ALRT"))
                            alias->locationType = record.mapFormId(number(bytes));
                        else if (tag == fourCC("ALCO"))
                            alias->createdBase = record.mapFormId(number(bytes));
                        else if (tag == fourCC("ALCA"))
                        {
                            alias->anchorAlias = number(bytes, 0, 2);
                            alias->createIn = (number(bytes, 2, 2) & 0x8000) != 0;
                        }
                        else if (tag == fourCC("ALFI"))
                            alias->forceAlias = number(bytes);
                        else if (tag == fourCC("ALNA"))
                            alias->linkedFrom = number(bytes);
                        else if (tag == fourCC("ALFE"))
                            alias->fromEvent = true;
                        else if (tag == fourCC("CTDA"))
                            alias->conditions.push_back(bytes);
                        else if (tag == fourCC("ALPC"))
                            alias->packages.push_back(record.mapFormId(number(bytes)));
                    }
                });
            }
        };

        struct ReferenceFields
        {
            std::uint32_t base = 0, cell = 0, location = 0, linked = 0;
            std::optional<std::array<float, 3>> position;
            bool disabled = false, actor = false, dead = false;
        };

        std::multimap<std::uint32_t, std::uint32_t> readLocationReferences(const RecordPayload& payload)
        {
            std::multimap<std::uint32_t, std::uint32_t> entries;
            visitSubrecords(payload, [&](auto tag, auto bytes) {
                if (tag != fourCC("ACSR") && tag != fourCC("LCSR"))
                    return;
                if (bytes.size() % 16)
                    throw std::runtime_error("Invalid LCTN static reference list");
                for (std::size_t offset = 0; offset < bytes.size(); offset += 16)
                    entries.emplace(payload.mapFormId(number(bytes, offset)), payload.mapFormId(number(bytes, offset + 4)));
            });
            return entries;
        }
    }

    Papyrus::ResolveQuestAlias questAliasResolver(const ReadWinningRecord& read)
    {
        return [read, cache = std::map<std::uint32_t, std::map<std::uint32_t, AliasKind>>{}]
            (std::uint32_t quest, std::uint32_t alias) mutable -> std::optional<AliasKind> {
            if (!cache.contains(quest))
            {
                const auto record = read(quest);
                if (record.metadata.recordType != fourCC("QUST"))
                    throw std::runtime_error("VMAD alias owner must resolve to a QUST record");
                std::map<std::uint32_t, AliasKind> aliases;
                visitSubrecords(record, [&](auto tag, auto bytes) {
                    if (aliasTag(tag))
                        aliases.try_emplace(number(bytes), aliasKind(tag));
                });
                cache.emplace(quest, std::move(aliases));
            }
            const auto& aliases = cache.at(quest);
            const auto found = aliases.find(alias);
            return found == aliases.end() ? std::nullopt : std::optional(found->second);
        };
    }

    void collectQuestTargets(const ReadWinningRecord& read, const std::map<std::uint32_t, std::uint32_t>& actors,
        std::set<std::uint32_t>& targets, const ScriptDiagnostic& diagnostic)
    {
        std::set<std::uint32_t> inspected;
        std::map<std::uint32_t, std::multimap<std::uint32_t, std::uint32_t>> locationRefs;
        std::deque<std::uint32_t> forms(targets.begin(), targets.end());
        while (!forms.empty() && inspected.size() < 4096)
        {
            const auto id = forms.front();
            forms.pop_front();
            if (!id || id == 0x14 || !inspected.insert(id).second)
                continue;
            try
            {
                auto record = read(id);
                if (record.metadata.recordType != fourCC("QUST"))
                    continue;
                const Quest quest(std::move(record));
                const auto add = [&](std::uint32_t form) {
                    if (form && targets.insert(form).second)
                        forms.push_back(form);
                };
                const auto properties = [&](const auto& table) {
                    for (const auto& script : table)
                        for (const auto& property : script.properties)
                            if (const auto* value = std::get_if<VmadObject>(&property.value))
                                add(quest.record.mapFormId(value->rawObjectId));
                };
                for (const auto& [aliasId, alias] : quest.aliases)
                {
                    add(alias.fixed);
                    add(alias.externalQuest);
                    if (alias.actorBase)
                    {
                        for (const auto& [ref, candidate] : actors)
                            if (candidate == alias.actorBase)
                                targets.insert(ref);
                    }
                    if (alias.locationAlias && alias.locationType)
                    {
                        const auto location = quest.aliases.find(*alias.locationAlias);
                        if (location == quest.aliases.end() || !location->second.fixed)
                            continue;
                        const auto form = location->second.fixed;
                        if (!locationRefs.contains(form))
                            locationRefs.emplace(form, readLocationReferences(read(form)));
                        const auto [first, last] = locationRefs.at(form).equal_range(alias.locationType);
                        for (auto it = first; it != last; ++it)
                            targets.insert(it->second);
                    }
                }
                properties(quest.vmad.common.scripts);
                for (const auto& alias : quest.vmad.aliases)
                    properties(alias.scripts);
            }
            catch (const std::exception& error)
            {
                diagnostic("QUST dependency " + std::to_string(id), error.what());
            }
        }
    }

    struct QuestScripts::Impl
    {
        ReadWinningRecord read;
        std::shared_ptr<const VFS::Manager> vfs;
        HostCallbacks base;
        ErrorLog& log;
        ResolveQuestAlias aliasResolver;
        std::shared_ptr<const WinningRecordIndex> index;
        std::map<std::uint32_t, Quest> quests;
        std::map<std::uint32_t, std::set<std::uint32_t>> actorsByBase;
        std::set<std::uint32_t> loaded;
        std::map<std::uint32_t, ReferenceFields> references;
        std::map<std::uint32_t, std::multimap<std::uint32_t, std::uint32_t>> locationRefs;
        std::map<std::uint32_t, std::uint32_t> cellLocations;
        std::map<std::uint32_t, std::uint32_t> currentLocations;
        struct CalendarUpdate
        {
            std::uint64_t generation = 0;
            double due = 0;
            void checkpoint(Checkpoint& ar) { ar(generation, due); }
        };
        std::map<std::pair<std::uint32_t, std::optional<std::uint32_t>>, CalendarUpdate> calendarUpdates;
        struct RepeatUpdate
        {
            double interval = 0, due = 0;
            std::uint64_t generation = 0;
            void checkpoint(Checkpoint& ar) { ar(interval, due, generation); }
        };
        std::map<std::uint32_t, RepeatUpdate> repeatUpdates;
        std::map<std::uint32_t, QuestCreatedReference> created;
        std::map<std::uint32_t, std::uint32_t> inventoryReferences;
        std::uint32_t nextCreated = 1;
        std::map<std::uint32_t, float> globals;
        std::map<std::string, std::shared_ptr<const PexFile>> pex;
        std::vector<SceneScriptEvent> events;
        std::set<std::uint32_t> changedPackageQuests;
        double time = 0;

        struct Task
        {
            std::uint32_t quest;
            std::uint64_t generation;
            std::optional<std::uint16_t> stage;
            std::shared_ptr<bool> finished;
            std::shared_ptr<Value> returned;
            std::vector<Value> arguments;
            std::vector<std::pair<std::shared_ptr<Instance>, std::string>> calls;
            std::size_t next = 0;
            double due = 0;
            std::optional<std::uint64_t> continuation;
            std::shared_ptr<const bool> nativeWait;
            bool prepared = false;
            bool failed = false;
            bool singleUpdate = false;
            std::optional<std::uint32_t> updateAlias;
        };
        std::deque<Task> tasks;
        std::optional<std::uint32_t> initializingQuest;

        Impl(ReadWinningRecord readIn, std::shared_ptr<const VFS::Manager> vfsIn, HostCallbacks baseIn, ErrorLog& logIn)
            : read(std::move(readIn)), vfs(std::move(vfsIn)), base(std::move(baseIn)), log(logIn),
              aliasResolver(questAliasResolver(read))
        {
        }

        void checkpoint(Checkpoint& ar)
        {
            ar(actorsByBase, loaded, created, nextCreated, globals, events, changedPackageQuests, time, initializingQuest);
            const auto instance = [&](std::shared_ptr<Instance>& pointer, std::uint32_t owner) {
                ar.shared(pointer, [&](auto current) {
                    std::string name = ar.loading ? "" : current->name;
                    std::optional<std::uint32_t> alias = ar.loading ? std::nullopt : current->aliasId;
                    ar(name, alias);
                    if (ar.loading)
                    {
                        auto& q = quest(owner); ScriptBinding binding; binding.name = name;
                        std::optional<AliasReference> identity;
                        if (alias) identity = AliasReference{owner, *alias, q.aliases.at(*alias).kind};
                        current = makeInstance(q, binding, identity);
                    }
                    return current;
                }, [&](auto& current) { ar(current.vm, current.animationEvents, current.initialized, current.failed, current.cellLoaded); });
            };
            auto it = quests.begin();
            if (ar.loading) quests.clear();
            ar.count(quests.size(), [&](auto) {
                std::uint32_t id = ar.loading ? 0 : it->first; ar(id);
                auto& q = ar.loading ? quest(id) : it++->second;
                ar(q.objectives, q.created, q.done, q.pending, q.startFailure, q.flags, q.current,
                    q.generation, q.running, q.completed, q.failed);
                // Alias definitions remain in QUST; only their current fills are overlaid.
                auto alias = q.aliases.begin();
                ar.count(q.aliases.size(), [&](auto) {
                    std::uint32_t key = ar.loading ? 0 : alias->first; ar(key);
                    ar(q.aliases.at(key).fill); if (!ar.loading) ++alias;
                });
                auto script = q.instances.begin();
                ar.count(q.instances.size(), [&](auto) {
                    std::string key = ar.loading ? "" : script->first; ar(key);
                    auto& ptr = ar.loading ? q.instances[key] : script++->second; instance(ptr, id);
                });
                ar.count(q.aliasInstances.size(), [&](auto i) {
                    if (ar.loading) q.aliasInstances.push_back({}); instance(q.aliasInstances[i], id);
                });
            });
            if (ar.loading) tasks.clear();
            ar.count(tasks.size(), [&](auto i) {
                if (ar.loading) tasks.push_back({}); auto& task = tasks[i];
                ar(task.quest, task.generation, task.stage, task.finished, task.returned, task.arguments,
                    task.next, task.due, task.continuation, task.nativeWait, task.prepared, task.failed,
                    task.singleUpdate, task.updateAlias);
                ar.count(task.calls.size(), [&](auto j) {
                    if (ar.loading) task.calls.push_back({}); auto& call = task.calls[j];
                    instance(call.first, task.quest); ar(call.second);
                });
            });
        }

        Quest& quest(std::uint32_t id)
        {
            if (const auto found = quests.find(id); found != quests.end())
                return found->second;
            auto record = read(id);
            if (record.metadata.recordType != fourCC("QUST"))
                throw std::runtime_error("Quest method requires a QUST receiver");
            return quests.try_emplace(id, std::move(record)).first->second;
        }

        std::uint32_t formType(std::uint32_t id)
        {
            if (id == playerReference)
                return fourCC("ACHR");
            if (const auto found = created.find(id); found != created.end())
                return found->second.actor ? fourCC("ACHR") : fourCC("REFR");
            return read(id).metadata.recordType;
        }

        bool validAliasForm(AliasKind kind, std::uint32_t form)
        {
            if (!form)
                return false;
            const auto type = formType(form);
            return kind == AliasKind::Location ? type == fourCC("LCTN") : type == fourCC("REFR") || type == fourCC("ACHR");
        }

        static std::uint32_t aliasValue(const Quest& q, std::uint32_t id)
        {
            if (const auto found = q.aliases.find(id); found != q.aliases.end())
                return found->second.fill.value_or(0);
            return 0;
        }

        const ReferenceFields& reference(std::uint32_t id)
        {
            if (const auto found = references.find(id); found != references.end())
                return found->second;
            ReferenceFields fields;
            if (id == playerReference)
            {
                fields.base = 7;
                fields.actor = true;
            }
            else if (const auto found = created.find(id); found != created.end())
            {
                fields = reference(found->second.anchor);
                fields.actor = found->second.actor;
                fields.base = found->second.base;
                fields.disabled = found->second.initiallyDisabled;
                fields.dead = false;
            }
            else
            {
                const auto payload = read(id);
                fields.actor = payload.metadata.recordType == fourCC("ACHR");
                fields.disabled = (payload.flags & 0x800) != 0;
                fields.dead = fields.actor && (payload.flags & 0x200) != 0;
                if (index)
                    if (const auto indexed = index->references.find(id); indexed != index->references.end())
                        fields.cell = indexed->second.cell;
                visitSubrecords(payload, [&](auto tag, auto bytes) {
                    if (tag == fourCC("NAME"))
                        fields.base = payload.mapFormId(number(bytes));
                    else if (tag == fourCC("XLCN"))
                        fields.location = payload.mapFormId(number(bytes));
                    else if (tag == fourCC("XLKR") && bytes.size() >= 4 && !fields.linked)
                    {
                        if (bytes.size() == 4)
                            fields.linked = payload.mapFormId(number(bytes));
                        else if (number(bytes) == 0)
                            fields.linked = payload.mapFormId(number(bytes, 4));
                    }
                    else if (tag == fourCC("DATA") && bytes.size() == 24)
                        fields.position = std::array<float, 3>{ std::bit_cast<float>(number(bytes)),
                            std::bit_cast<float>(number(bytes, 4)), std::bit_cast<float>(number(bytes, 8)) };
                });
            }
            return references.emplace(id, std::move(fields)).first->second;
        }

        std::uint32_t locationOf(std::uint32_t form)
        {
            if (!form || formType(form) == fourCC("LCTN"))
                return form;
            const auto& fields = reference(form);
            if (fields.location)
                return fields.location;
            const auto editorCell = base.getEditorCell ? base.getEditorCell(form) : 0;
            const auto cellId = editorCell ? editorCell : fields.cell;
            return cellLocation(cellId);
        }

        std::uint32_t cellLocation(std::uint32_t cellId)
        {
            if (!cellId)
                return 0;
            if (const auto found = cellLocations.find(cellId); found != cellLocations.end())
                return found->second;
            const auto cell = read(cellId);
            std::uint32_t location = 0;
            visitSubrecords(cell, [&](auto tag, auto bytes) {
                if (tag == fourCC("XLCN"))
                    location = cell.mapFormId(number(bytes));
            });
            return cellLocations.emplace(cellId, location).first->second;
        }

        std::uint32_t currentLocation(std::uint32_t form)
        {
            const auto cell = base.getCurrentCell ? base.getCurrentCell(form) : 0;
            return cell ? cellLocation(cell) : locationOf(form);
        }

        bool locationContains(std::uint32_t location, std::uint32_t current)
        {
            if (!location)
                return false;
            std::set<std::uint32_t> visited;
            while (current && visited.insert(current).second)
            {
                if (current == location)
                    return true;
                const auto record = read(current);
                current = 0;
                visitSubrecords(record, [&](auto tag, auto bytes) {
                    if (tag == fourCC("PNAM"))
                        current = record.mapFormId(number(bytes));
                });
            }
            return false;
        }

        bool inLocation(std::uint32_t form, std::uint32_t location)
        {
            return form && locationContains(location, currentLocation(form));
        }

        void updateLocations()
        {
            if (!base.getCurrentCell)
                return;
            std::set<std::uint32_t> observed{playerReference};
            for (auto& [id, q] : quests)
                if (q.running)
                    for (const auto& instance : q.aliasInstances)
                        if (instance->aliasId && instance->vm.hasFunction("OnLocationChange"))
                            if (const auto ref = aliasValue(q, *instance->aliasId))
                                observed.insert(ref);
            std::erase_if(currentLocations, [&](const auto& entry) { return !observed.contains(entry.first); });
            for (const auto ref : observed)
                if (const auto cell = base.getCurrentCell(ref))
                {
                    const auto next = cellLocation(cell);
                    const auto [prior, added] = currentLocations.try_emplace(ref, next);
                    if (!added && prior->second != next)
                    {
                        const auto old = std::exchange(prior->second, next);
                        if (ref == playerReference)
                            events.push_back({ref, "CELL", "LocationChange",
                                {{std::format("T5_LOCATION_CHANGE cell={:08X} old={:08X} new={:08X}", cell, old, next), 0}},
                                time, {}, 0});
                        actorEvent(ref, "OnLocationChange", {formValue(old), formValue(next)});
                    }
                }
        }

        bool hasKeyword(std::uint32_t form, std::uint32_t keyword)
        {
            if (!form || !keyword)
                return false;
            const auto type = formType(form);
            if (type == fourCC("ACHR") || type == fourCC("REFR"))
                form = reference(form).base;
            if (!form)
                return false;
            const auto payload = read(form);
            bool found = false;
            visitSubrecords(payload, [&](auto tag, auto bytes) {
                if (tag == fourCC("KWDA"))
                    for (std::size_t offset = 0; offset + 4 <= bytes.size(); offset += 4)
                        found |= payload.mapFormId(number(bytes, offset)) == keyword;
            });
            return found;
        }

        const std::multimap<std::uint32_t, std::uint32_t>& locationReferences(std::uint32_t location)
        {
            if (index)
                if (const auto refs = index->locationReferences.find(location); refs != index->locationReferences.end())
                    return refs->second;
            if (const auto found = locationRefs.find(location); found != locationRefs.end())
                return found->second;
            return locationRefs.emplace(location, readLocationReferences(read(location))).first->second;
        }

        bool disabled(std::uint32_t form)
        {
            if (form == playerReference)
                return false;
            if (base.isDisabled)
                return base.isDisabled(form);
            return reference(form).disabled;
        }

        bool candidateAllowed(const Quest& q, const Alias& alias, std::uint32_t form, bool match)
        {
            if (!form)
                return false;
            if (match && (alias.flags & loadedAreaAlias) && !loaded.contains(form))
                return false;
            if (match && alias.kind == AliasKind::Reference && !(alias.flags & allowDisabledAlias) && disabled(form))
                return false;
            if (match && alias.kind == AliasKind::Reference && !(alias.flags & allowDeadAlias) && reference(form).dead)
                return false;
            if (!testConditions(&q, q.record, alias.conditions, form))
                return false;
            const auto id = q.record.metadata.formId;
            for (const auto& [otherId, other] : q.aliases)
                if (&other != &alias && other.fill == form && !(alias.flags & reuseAlias))
                    return false;
            if (!(alias.flags & allowReservedAlias))
                for (const auto& [questId, other] : quests)
                    if (questId != id && other.running)
                        for (const auto& [otherId, otherAlias] : other.aliases)
                            if ((otherAlias.flags & 1) && otherAlias.fill == form)
                                return false;
            return true;
        }

        std::uint32_t createReference(Quest& q, const Alias& alias)
        {
            const auto anchor = alias.anchorAlias ? aliasValue(q, *alias.anchorAlias) : 0;
            if (!anchor)
                return 0;
            const auto id = placeReference(alias.createdBase, anchor, (alias.flags & initiallyDisabledAlias) != 0);
            if (alias.createIn)
                inventoryReferences.emplace(id, anchor);
            q.created.push_back(id);
            return id;
        }

        std::uint32_t placeReference(std::uint32_t baseId, std::uint32_t anchor, bool disabled)
        {
            const auto type = formType(baseId);
            if (type == fourCC("LVLN"))
                throw std::runtime_error("Leveled actor creation requires leveled list selection");
            if (nextCreated >= 0xffffff)
                throw std::runtime_error("Runtime reference FormID space exhausted");
            const auto id = 0xff000000 | nextCreated++;
            created.emplace(id, QuestCreatedReference{baseId, anchor, type == fourCC("NPC_"), disabled});
            return id;
        }

        std::uint32_t resolveFill(Quest& q, Alias& alias)
        {
            if (alias.fixed)
                return validAliasForm(alias.kind, alias.fixed) && testConditions(&q, q.record, alias.conditions, alias.fixed)
                    ? alias.fixed : 0;
            if (alias.actorBase)
            {
                if (alias.actorBase == 7)
                    return playerReference;
                if (index)
                    if (const auto found = index->actorsByBase.find(alias.actorBase);
                        found != index->actorsByBase.end() && found->second.size() == 1)
                        return candidateAllowed(q, alias, found->second.front(), false) ? found->second.front() : 0;
                if (const auto found = actorsByBase.find(alias.actorBase);
                    found != actorsByBase.end() && found->second.size() == 1)
                    return candidateAllowed(q, alias, *found->second.begin(), false) ? *found->second.begin() : 0;
                return 0;
            }
            if (alias.externalQuest && alias.externalAlias)
            {
                auto& other = quest(alias.externalQuest);
                if (const auto it = other.aliases.find(*alias.externalAlias); other.running && it != other.aliases.end()
                    && it->second.kind == alias.kind)
                    return it->second.fill.value_or(0);
                return 0;
            }
            if (alias.locationAlias && alias.locationType)
            {
                const auto location = aliasValue(q, *alias.locationAlias);
                if (!location)
                    return 0;
                const auto [first, last] = locationReferences(location).equal_range(alias.locationType);
                for (auto it = first; it != last; ++it)
                    if (validAliasForm(alias.kind, it->second) && candidateAllowed(q, alias, it->second, false))
                        return it->second;
                return 0;
            }
            if (alias.createdBase)
                return createReference(q, alias);
            if (alias.linkedFrom)
            {
                const auto owner = aliasValue(q, *alias.linkedFrom);
                const auto linked = owner ? reference(owner).linked : 0;
                return linked && validAliasForm(alias.kind, linked) && candidateAllowed(q, alias, linked, true) ? linked : 0;
            }
            if (alias.fromEvent || alias.conditions.empty())
                return 0;

            std::uint32_t match = 0;
            float bestDistance = std::numeric_limits<float>::infinity();
            const auto consider = [&](std::uint32_t id) {
                if (!candidateAllowed(q, alias, id, true))
                    return;
                if ((alias.flags & closestAlias) && alias.kind == AliasKind::Reference)
                {
                    const auto& a = reference(id).position;
                    const auto& b = reference(playerReference).position;
                    if (a && b)
                    {
                        const auto distance = std::hypot((*a)[0] - (*b)[0], (*a)[1] - (*b)[1], (*a)[2] - (*b)[2]);
                        if (distance < bestDistance || (distance == bestDistance && id < match))
                        {
                            match = id;
                            bestDistance = distance;
                        }
                        return;
                    }
                }
                if (!match || (bestDistance == std::numeric_limits<float>::infinity() && id < match))
                    match = id;
            };
            if (alias.kind == AliasKind::Location && index)
                for (auto id : index->locations)
                    consider(id);
            else if (alias.kind == AliasKind::Reference)
            {
                if (!(alias.flags & loadedAreaAlias) && index)
                    for (const auto& [id, info] : index->references)
                        if (info.flags & 0x400)
                            consider(id);
                for (auto id : loaded)
                    consider(id);
            }
            return match;
        }

        float getGlobal(std::uint32_t id)
        {
            if (const auto it = globals.find(id); it != globals.end())
                return it->second;
            const auto record = read(id);
            if (record.metadata.recordType != fourCC("GLOB"))
                throw std::runtime_error("Expected a GLOB record");
            std::optional<float> value;
            visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == fourCC("FLTV"))
                    value = std::bit_cast<float>(number(bytes));
            });
            if (!value || !std::isfinite(*value))
                throw std::runtime_error("Invalid GLOB value");
            return globals.emplace(id, *value).first->second;
        }

        void setGlobal(std::uint32_t id, const Value& value)
        {
            const auto* integer = std::get_if<std::int32_t>(&value);
            const auto amount = integer ? static_cast<float>(*integer) : std::get<float>(value);
            if (!std::isfinite(amount))
                throw std::runtime_error("GlobalVariable requires a finite value");
            globals.insert_or_assign(id, amount);
        }

        std::map<std::pair<std::uint32_t, std::uint32_t>, std::set<std::uint32_t>> inventoryFilters;

        static bool hasScript(const Quest& q, std::string_view type)
        {
            return std::ranges::any_of(q.vmad.common.scripts, [&](const auto& script) {
                return script.flags < 2 && Misc::StringUtils::ciEqual(script.name, type);
            });
        }

        HostCallbacks host()
        {
            auto result = base;
            result.isFormType = [this](auto id, auto type) {
                if (base.isFormType && base.isFormType(id, type))
                    return true;
                if (const auto spawned = created.find(id); spawned != created.end())
                    return Misc::StringUtils::ciEqual(type, "Form")
                        || Misc::StringUtils::ciEqual(type, "ObjectReference")
                        || (spawned->second.actor && Misc::StringUtils::ciEqual(type, "Actor"));
                if (base.isFormType && base.isFormType(id, "Quest"))
                    return hasScript(quest(id), type);
                if (const auto q = quests.find(id); q != quests.end())
                    return q->second.instances.contains(Misc::StringUtils::lowerCase(type));
                return false;
            };
            result.getQuestStage = [this](auto id) { return quest(id).current; };
            result.isQuestStageDone = [this](auto id, auto stage) {
                return stage >= 0 && stage <= 65535 && quest(id).done.contains(static_cast<std::uint16_t>(stage));
            };
            result.setQuestStageLatent = [this](auto id, auto stage) { return setStage(id, stage); };
            result.setGlobalValue = [this](auto id, float value) {
                getGlobal(id);
                if (!std::isfinite(value))
                    throw std::runtime_error("Global value must be finite");
                globals[id] = value;
                if(index)
                {
                    const auto hour=index->globalsByName.find("gamehour"), days=index->globalsByName.find("gamedayspassed");
                    if(hour!=index->globalsByName.end() && hour->second==id && days!=index->globalsByName.end())
                        globals[days->second]=std::floor(getGlobal(days->second))+value/24.f;
                }
            };
            result.callFormMethod = [this](auto id, auto name, const auto& args) { return formMethod(id, name, args); };
            result.callAliasMethod = [this](const auto& alias, auto name, const auto& args) { return aliasMethod(alias, name, args); };
            result.callFormScriptMethod = [this](std::uint32_t id, std::string_view type, std::string_view method,
                const std::vector<Value>& args) -> std::optional<HostCallbacks::ScriptCall> {
                if (!quests.contains(id) && index && std::ranges::find(index->startGameQuests, id) != index->startGameQuests.end())
                    quest(id);
                if (const auto q = quests.find(id); q != quests.end() && !q->second.running
                    && !q->second.generation && (q->second.flags & 1) && q->second.startFailure.empty()
                    && hasScript(q->second, type))
                    start(id, q->second);
                if (const auto q = quests.find(id); q != quests.end() && q->second.running)
                    if (const auto script = q->second.instances.find(Misc::StringUtils::lowerCase(type));
                        script != q->second.instances.end() && script->second->vm.hasFunction(method))
                    {
                        auto finished = std::make_shared<bool>(false);
                        auto returned = std::make_shared<Value>();
                        Task task{id, q->second.generation, {}, finished, returned, args};
                        task.calls.emplace_back(script->second, std::string(method));
                        tasks.push_back(std::move(task));
                        return HostCallbacks::ScriptCall{{}, std::move(finished), std::move(returned)};
                    }
                return base.callFormScriptMethod ? base.callFormScriptMethod(id, type, method, args) : std::nullopt;
            };
            result.getFormProperty = [this](std::uint32_t id, std::string_view name) -> std::optional<Value> {
                if (Misc::StringUtils::ciEqual(name, "Value") && formType(id) == fourCC("GLOB"))
                    return Value(getGlobal(id));
                return readProperty(id, name);
            };
            result.setFormProperty = [this](std::uint32_t id, std::string_view name, const Value& value) {
                if (Misc::StringUtils::ciEqual(name, "Value") && formType(id) == fourCC("GLOB"))
                {
                    setGlobal(id, value);
                    return true;
                }
                // Form properties exist independently of the quest's alias
                // filling and stage execution.
                if (formType(id) == fourCC("QUST") && !quest(id).running)
                    bindRootScripts(quest(id));
                if (const auto q = quests.find(id); q != quests.end())
                    for (const auto& [script, instance] : q->second.instances)
                        if (!instance->failed && instance->vm.hasAutoProperty(name))
                        {
                            instance->vm.writeAutoProperty(name, value);
                            return true;
                        }
                return base.setFormProperty && base.setFormProperty(id, name, value);
            };
            result.callStatic = [this](std::string_view script, std::string_view name, const auto& args) -> std::optional<Value> {
                if (Misc::StringUtils::ciEqual(script, "Game") && Misc::StringUtils::ciEqual(name, "GetForm"))
                {
                    arity(args, 1, 1);
                    const auto id = static_cast<std::uint32_t>(argument<std::int32_t>(args, 0));
                    try { return formValue(id && formType(id) ? id : 0); }
                    catch (const std::exception&) { return Value{}; }
                }
                return base.callStatic ? base.callStatic(script, name, args) : std::nullopt;
            };
            return result;
        }

        std::optional<Value> aliasMethod(const AliasReference& handle, std::string_view name, const std::vector<Value>& args)
        {
            auto& q = quest(handle.questFormId);
            auto& alias = q.aliases.at(handle.aliasId);
            if (alias.kind != handle.kind)
                throw std::runtime_error("Alias type mismatch");
            const auto method = Misc::StringUtils::lowerCase(name);
            if (method == "registerforsingleupdategametime" || method == "unregisterforupdategametime")
                return calendarMethod(handle.questFormId, handle.aliasId, method, args);
            if (method == "registerforsingleupdate")
            {
                arity(args, 1, 1);
                registerUpdate(handle.questFormId, handle.aliasId, argument<float>(args, 0));
                return Value{};
            }
            if (alias.kind == AliasKind::Reference && (method == "addinventoryeventfilter"
                || method == "removeinventoryeventfilter" || method == "removeallinventoryeventfilters"))
            {
                const auto key = std::pair(handle.questFormId, handle.aliasId);
                if (method == "removeallinventoryeventfilters")
                {
                    arity(args, 0, 0);
                    inventoryFilters.erase(key);
                }
                else
                {
                    arity(args, 1, 1);
                    const auto filter = argument<FormReference>(args, 0).formId;
                    auto& filters = inventoryFilters[key];
                    if (method == "addinventoryeventfilter")
                        filters.insert(filter);
                    else
                        filters.erase(filter);
                    if (filters.empty())
                        inventoryFilters.erase(key);
                }
                return Value{};
            }
            if (method == "registerforanimationevent" || method == "unregisterforanimationevent")
            {
                arity(args, 2, 2);
                bool registered = false;
                for (const auto& instance : q.aliasInstances)
                    if (instance->aliasId == handle.aliasId && instance->vm.hasFunction("OnAnimationEvent"))
                        if (const auto result = registerAnimation(handle.questFormId, instance->name, handle, method, args))
                            if (const auto* accepted = std::get_if<bool>(&*result))
                                registered |= *accepted;
                return method == "registerforanimationevent" ? Value(registered) : Value{};
            }
            if (method == "forcerefto" || method == "forcelocationto")
            {
                arity(args, 1, 1);
                const auto form = argument<FormReference>(args, 0).formId;
                if ((method == "forcerefto") != (alias.kind == AliasKind::Reference) || !validAliasForm(alias.kind, form))
                    throw std::runtime_error("Alias fill form type mismatch");
                alias.fill = form;
                return Value{};
            }
            if (method == "getowningquest")
            {
                arity(args, 0, 0);
                return formValue(handle.questFormId);
            }
            if (method == "clear")
            {
                arity(args, 0, 0);
                if (alias.flags & optionalAlias)
                    alias.fill = 0;
                return Value{};
            }
            const auto form = alias.fill.value_or(0);
            if (((method == "getreference" || method == "getref") && alias.kind == AliasKind::Reference)
                || (method == "getlocation" && alias.kind == AliasKind::Location))
            {
                arity(args, 0, 0);
                return formValue(form);
            }
            if ((method == "getactorreference" || method == "getactorref") && alias.kind == AliasKind::Reference)
            {
                arity(args, 0, 0);
                return formValue(form && formType(form) == fourCC("ACHR") ? form : 0);
            }
            if (method == "trytoenable" || method == "trytodisable"
                || method == "trytoenablenowait" || method == "trytodisablenowait")
            {
                arity(args, 0, 0);
                if (form && base.setDisabled)
                    base.setDisabled(form, method.starts_with("trytodisable"), false);
                return Value(form && bool(base.setDisabled));
            }
            if (method == "trytomoveto" || method == "trytoevaluatepackage")
            {
                arity(args, method == "trytomoveto" ? 1 : 0, method == "trytomoveto" ? 1 : 0);
                if (!form)
                    return Value(false);
                const auto native = method == "trytomoveto" ? "MoveTo" : "EvaluatePackage";
                if (base.callFormMethod && base.callFormMethod(form, native, args))
                    return Value(true);
                return std::nullopt;
            }
            return base.callAliasMethod ? base.callAliasMethod(handle, name, args) : std::nullopt;
        }

        std::optional<Value> formMethod(std::uint32_t id, std::string_view name, const std::vector<Value>& args)
        {
            const auto method = Misc::StringUtils::lowerCase(name);
            if ((method == "registerforsingleupdategametime" || method == "unregisterforupdategametime")
                && formType(id) == fourCC("QUST"))
                return calendarMethod(id, {}, method, args);
            if ((method == "registerforupdate" || method == "unregisterforupdate") && formType(id) == fourCC("QUST"))
            {
                if (method == "registerforupdate")
                {
                    arity(args, 1, 1);
                    const auto interval = static_cast<double>(argument<float>(args, 0));
                    if (!std::isfinite(interval) || interval < 0 || interval > MaxScheduledDelaySeconds)
                        throw std::runtime_error("Invalid quest repeat update delay");
                    const auto delay = std::max(interval, 1. / 60.);
                    repeatUpdates[id] = {delay, time + delay, quest(id).generation};
                    return Value{};
                }
                arity(args, 0, 0);
                repeatUpdates.erase(id);
                std::erase_if(tasks, [&](const auto& task) {
                    return task.quest == id && task.singleUpdate && !task.updateAlias
                        && !task.continuation && !task.next;
                });
                return Value{};
            }
            if (method == "getcurrentlocation" || method == "isinlocation")
            {
                arity(args, method == "isinlocation", method == "isinlocation");
                if (method == "getcurrentlocation")
                    return formValue(currentLocation(id));
                const auto* location = std::get_if<FormReference>(&args.front());
                return Value(location && inLocation(id, location->formId));
            }
            if (method == "ischild" && formType(id) == fourCC("LCTN"))
            {
                arity(args, 1, 1);
                const auto child = std::holds_alternative<std::monostate>(args.front()) ? 0
                    : argument<FormReference>(args, 0).formId;
                return Value(child && child != id && locationContains(id, child));
            }
            if (method == "getcurrentstageid")
            {
                arity(args, 0, 0);
                return Value(static_cast<std::int32_t>(quest(id).current));
            }
            if (method == "geteditorlocation")
            {
                arity(args, 0, 0);
                return formValue(locationOf(id));
            }
            if ((method == "getvalue" || method == "getvalueint" || method == "setvalue" || method == "setvalueint")
                && formType(id) == fourCC("GLOB"))
            {
                if (method.starts_with("set"))
                {
                    arity(args, 1, 1);
                    setGlobal(id, args.front());
                    return Value{};
                }
                arity(args, 0, 0);
                const auto value = getGlobal(id);
                return method == "getvalueint" ? Value(static_cast<std::int32_t>(std::clamp<double>(value,
                    std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()))) : Value(value);
            }
            if (method == "setobjectivedisplayed" || method == "setobjectivecompleted" || method == "setobjectivefailed"
                || method == "isobjectivedisplayed" || method == "isobjectivecompleted" || method == "isobjectivefailed")
                return objectiveMethod(quest(id), method, args);
            if (method == "getalias" || method == "start" || method == "stop" || method == "isrunning"
                || method == "iscompleted" || method == "completequest" || method == "isstopped")
            {
                if ((method == "start" || method == "stop") && formType(id) == fourCC("SCEN"))
                    return base.callFormMethod ? base.callFormMethod(id, name, args) : std::nullopt;
                auto& q = quest(id);
                if (method == "getalias")
                {
                    arity(args, 1, 1);
                    const auto alias = static_cast<std::uint32_t>(argument<std::int32_t>(args, 0));
                    const auto it = q.aliases.find(alias);
                    return it == q.aliases.end() ? Value{} : Value(AliasReference{id, alias, it->second.kind});
                }
                arity(args, 0, 0);
                if (method == "start")
                    return Value(start(id, q));
                if (method == "isrunning")
                    return Value(q.running);
                if (method == "isstopped")
                    return Value(!q.running);
                if (method == "iscompleted")
                    return Value(q.completed);
                if (method == "completequest")
                    q.completed = true;
                else
                    stop(q);
                return Value{};
            }
            return base.callFormMethod ? base.callFormMethod(id, name, args) : std::nullopt;
        }

        static Value objectiveMethod(Quest& q, std::string_view method, const std::vector<Value>& args)
        {
            const bool isSetter = method.starts_with("set");
            const bool display = method.ends_with("displayed");
            arity(args, 1, isSetter ? (display ? 3 : 2) : 1);
            const auto index = argument<std::int32_t>(args, 0);
            const auto it = index >= 0 && index <= 65535
                ? q.objectives.find(static_cast<std::uint16_t>(index)) : q.objectives.end();
            if (it == q.objectives.end())
                return isSetter ? Value{} : Value(false);
            auto& objective = it->second;
            auto& state = display ? objective.displayed : method.ends_with("completed") ? objective.completed : objective.failed;
            if (!isSetter)
                return state;
            if (args.size() == 3)
                argument<bool>(args, 2); // Force refreshes the notification in the journal UI.
            state = args.size() == 1 || argument<bool>(args, 1);
            return Value{};
        }

        std::optional<Value> readProperty(std::uint32_t id, std::string_view name) const
        {
            if (const auto q = quests.find(id); q != quests.end())
                for (const auto& [script, instance] : q->second.instances)
                    if (!instance->failed && instance->vm.hasAutoProperty(name))
                        return instance->vm.readAutoProperty(name);
            return base.getFormProperty ? base.getFormProperty(id, name) : std::nullopt;
        }

        std::shared_ptr<Instance> makeInstance(Quest& q, const ScriptBinding& binding, std::optional<AliasReference> alias)
        {
            const auto name = Misc::StringUtils::lowerCase(binding.name);
            if (name.empty() || !std::ranges::all_of(name, [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                }))
                throw std::runtime_error("Invalid quest PEX name");
            auto& file = pex[name];
            if (!file)
                file = std::make_shared<PexFile>(readPex(*vfs->get(VFS::Path::Normalized("scripts/" + name + ".pex"))));
            const auto entry = std::ranges::find_if(file->scripts,
                [&](const auto& script) { return Misc::StringUtils::ciEqual(script.name, name); });
            if (entry == file->scripts.end())
                throw std::runtime_error("Quest PEX class does not match VMAD");
            auto callbacks = host();
            callbacks.selfAlias = alias;
            const auto owner = q.record.metadata.formId;
            auto formCall = callbacks.callFormMethod;
            callbacks.callFormMethod = [this, call = std::move(formCall), owner, name, alias](
                                           std::uint32_t target, std::string_view method, const std::vector<Value>& args)
                -> std::optional<Value> {
                if (target == owner && !alias)
                    if (auto result = registerAnimation(owner, name, alias, method, args))
                        return result;
                return call(target, method, args);
            };
            callbacks.registerSingleUpdate = [this, owner](std::uint32_t target, double seconds) {
                if (target == owner || (target && formType(target) == fourCC("QUST")))
                    registerUpdate(target, {}, seconds);
                else if (base.registerSingleUpdate)
                {
                    base.registerSingleUpdate(target, seconds);
                }
            };
            return std::make_shared<Instance>(Instance{entry->name,
                ScriptInstance(std::shared_ptr<const Script>(file, &*entry), q.record.metadata.formId,
                    resolveProperties(binding, aliasResolver), std::move(callbacks)),
                alias ? std::optional(alias->aliasId) : std::nullopt});
        }

        std::shared_ptr<Instance> findInstance(
            const Quest& q, const std::string& name, std::optional<AliasReference> alias) const
        {
            if (alias)
            {
                const auto it = std::ranges::find_if(q.aliasInstances, [&](const auto& item) {
                    return item->aliasId == alias->aliasId && Misc::StringUtils::ciEqual(item->name, name);
                });
                if (it != q.aliasInstances.end())
                    return *it;
            }
            else if (const auto it = q.instances.find(name); it != q.instances.end())
                return it->second;
            return {};
        }

        void registerUpdate(std::uint32_t id, std::optional<std::uint32_t> alias, double seconds)
        {
            if (!std::isfinite(seconds) || seconds < 0 || seconds > MaxScheduledDelaySeconds)
                throw std::runtime_error("Invalid quest single update delay");
            auto& q = quest(id);
            std::erase_if(tasks, [&](const auto& task) {
                return task.quest == id && task.singleUpdate && task.updateAlias == alias
                    && !task.continuation && !task.next;
            });
            Task task{id, q.generation};
            if (alias)
            {
                for (const auto& instance : q.aliasInstances)
                    if (instance->aliasId == alias)
                        task.calls.emplace_back(instance, "OnUpdate");
            }
            else
            {
                for (const auto& [name, instance] : q.instances)
                    task.calls.emplace_back(instance, "OnUpdate");
            }
            task.singleUpdate = true;
            task.updateAlias = alias;
            task.due = time + seconds;
            tasks.push_back(std::move(task));
        }

        double gameDays()
        {
            if (index)
                if (const auto days = index->globalsByName.find("gamedayspassed"); days != index->globalsByName.end())
                    return getGlobal(days->second);
            return 0;
        }

        Value calendarMethod(std::uint32_t id, std::optional<std::uint32_t> alias, std::string_view method,
            const std::vector<Value>& args)
        {
            const bool add = method == "registerforsingleupdategametime";
            arity(args, add, add);
            const auto key = std::pair(id, alias);
            if (!add)
                calendarUpdates.erase(key);
            else
            {
                const auto hours = argument<float>(args, 0);
                if (!std::isfinite(hours) || hours < 0)
                    throw std::runtime_error("Invalid quest game-time update delay");
                calendarUpdates.insert_or_assign(key, CalendarUpdate{quest(id).generation, gameDays() + hours / 24.});
            }
            return {};
        }

        void dispatchCalendarUpdates()
        {
            if (calendarUpdates.empty())
                return;
            const auto days = gameDays();
            for (auto it = calendarUpdates.begin(); it != calendarUpdates.end();)
            {
                const auto [id, alias] = it->first;
                const auto& q = quest(id);
                if (q.running && it->second.generation == q.generation)
                {
                    if (it->second.due > days)
                    {
                        ++it;
                        continue;
                    }
                    Task task{id, q.generation};
                    if (alias)
                    {
                        for (const auto& instance : q.aliasInstances)
                            if (instance->aliasId == alias)
                                task.calls.emplace_back(instance, "OnUpdateGameTime");
                    }
                    else
                        for (const auto& [name, instance] : q.instances)
                            task.calls.emplace_back(instance, "OnUpdateGameTime");
                    tasks.push_back(std::move(task));
                }
                it = calendarUpdates.erase(it);
            }
        }

        std::optional<Value> registerAnimation(std::uint32_t id, const std::string& name,
            std::optional<AliasReference> alias, std::string_view method, const std::vector<Value>& args)
        {
            const bool registerEvent = Misc::StringUtils::ciEqual(method, "RegisterForAnimationEvent");
            if (!registerEvent && !Misc::StringUtils::ciEqual(method, "UnregisterForAnimationEvent"))
                return std::nullopt;
            arity(args, 2, 2);
            const auto* sender = std::get_if<FormReference>(&args[0]);
            const auto event = argument<std::string>(args, 1);
            const auto instance = findInstance(quest(id), name, alias);
            if (!sender || !sender->formId || event.empty() || !instance)
                return registerEvent ? Value(false) : Value{};
            if (registerEvent)
            {
                if (!validAliasForm(AliasKind::Reference, sender->formId))
                    return Value(false);
                instance->animationEvents.emplace(sender->formId, event);
                return Value(true);
            }
            instance->animationEvents.erase({sender->formId, event});
            return Value{};
        }

        void animationEvent(std::uint32_t sender, std::string_view event)
        {
            const auto key = std::pair(sender, std::string(event));
            for (auto& [id, q] : quests)
                if (q.running)
                {
                    const auto dispatch = [&](const std::shared_ptr<Instance>& instance) {
                        if (!instance->animationEvents.contains(key))
                            return;
                        Task task{id, q.generation};
                        task.arguments = {FormReference{sender}, key.second};
                        task.calls.emplace_back(instance, "OnAnimationEvent");
                        tasks.push_back(std::move(task));
                    };
                    for (const auto& [name, instance] : q.instances)
                        dispatch(instance);
                    for (const auto& instance : q.aliasInstances)
                        dispatch(instance);
                }
        }

        void actorEvent(std::uint32_t actor, const char* event, const std::vector<Value>& arguments)
        {
            for (auto& [id, q] : quests)
                if (q.running)
                    for (const auto& instance : q.aliasInstances)
                        if (instance->aliasId && aliasValue(q, *instance->aliasId) == actor)
                        {
                            Task task{id, q.generation};
                            task.arguments = arguments;
                            task.calls.emplace_back(instance, event);
                            tasks.push_back(std::move(task));
                        }
        }

        std::vector<std::uint32_t> activeObjectiveTargets()
        {
            std::set<std::uint32_t> unique;
            for (auto& [id, q] : quests)
                if (q.running)
                    for (const auto& [index, objective] : q.objectives)
                    {
                        if (!objective.displayed || objective.completed || objective.failed)
                            continue;
                        const auto targets = q.objectiveTargets.find(index);
                        if (targets == q.objectiveTargets.end())
                            continue;
                        for (const auto& target : targets->second)
                        {
                            const auto ref = aliasValue(q, target.alias);
                            if (ref && validAliasForm(AliasKind::Reference, ref) && !disabled(ref)
                                && testConditions(&q, q.record, target.conditions, playerReference))
                                unique.insert(ref);
                        }
                    }
            return {unique.begin(), unique.end()};
        }

        void inventoryChanged(std::uint32_t referenceId, std::uint32_t item, std::int32_t count,
            std::uint32_t other, bool added)
        {
            const auto event = added ? "OnItemAdded" : "OnItemRemoved";
            for (auto& [id, q] : quests)
                if (q.running)
                    for (const auto& instance : q.aliasInstances)
                    {
                        if (!instance->aliasId || aliasValue(q, *instance->aliasId) != referenceId || !instance->vm.hasFunction(event))
                            continue;
                        const auto filters = inventoryFilters.find({id, *instance->aliasId});
                        if (filters != inventoryFilters.end() && !std::ranges::any_of(filters->second, [&](auto filter) {
                            if (filter == item) return true;
                            if (formType(filter) != fourCC("FLST")) return false;
                            const auto list = read(filter);
                            bool match = false;
                            visitSubrecords(list, [&](auto tag, auto bytes) {
                                if (tag == fourCC("LNAM") && list.mapFormId(number(bytes)) == item) match = true;
                            });
                            return match;
                        }))
                            continue;
                        Task task{id, q.generation};
                        task.arguments = {formValue(item), count, Value{}, formValue(other)};
                        task.calls.emplace_back(instance, event);
                        tasks.push_back(std::move(task));
                    }
        }

        std::shared_ptr<Instance> bindScript(Quest& q, const VmadScript& record, std::optional<AliasReference> alias)
        {
            try
            {
                RecordAttachment source;
                source.resolveFormId = q.record.mapFormId;
                ScriptBindings bindings;
                applyVmadTable(bindings, {record}, source, {}, true);
                if (!bindings.empty())
                    return makeInstance(q, bindings.begin()->second, alias);
            }
            catch (const std::exception& error)
            {
                log.write("QUST " + std::to_string(q.record.metadata.formId) + " " + record.name, error.what());
            }
            return {};
        }

        void bindRootScripts(Quest& q)
        {
            for (const auto& record : q.vmad.common.scripts)
            {
                const auto name = Misc::StringUtils::lowerCase(record.name);
                if (!q.instances.contains(name))
                    if (auto instance = bindScript(q, record, {}))
                        q.instances.emplace(name, std::move(instance));
            }
        }

        bool start(std::uint32_t id, Quest& q, std::optional<std::uint16_t> requested = {}, bool diagnostic = true)
        {
            if (q.running)
                return true;
            if ((q.flags & 0x100) && (q.completed || q.failed))
                return false;
            q.startFailure.clear();
            const auto createdCount = q.created.size();
            std::map<std::uint32_t, std::string> errors;
            bool changed = true;
            for (std::size_t pass = 0; pass < q.aliasOrder.size() && changed; ++pass)
            {
                changed = false;
                for (auto aliasId : q.aliasOrder)
                {
                    auto& alias = q.aliases.at(aliasId);
                    if (alias.fill)
                        continue;
                    try
                    {
                        if (const auto form = resolveFill(q, alias))
                        {
                            alias.fill = form;
                            changed = true;
                            if (alias.forceAlias)
                                if (auto found = q.aliases.find(*alias.forceAlias);
                                    found != q.aliases.end() && validAliasForm(found->second.kind, form))
                                    found->second.fill = form;
                        }
                    }
                    catch (const std::exception& error)
                    {
                        errors.insert_or_assign(aliasId, error.what());
                    }
                }
            }
            for (auto aliasId : q.aliasOrder)
            {
                const auto& alias = q.aliases.at(aliasId);
                if (!alias.fill && !(alias.flags & optionalAlias))
                {
                    for (auto& [otherId, other] : q.aliases)
                        other.fill.reset();
                    while (q.created.size() > createdCount)
                    {
                        references.erase(q.created.back());
                        inventoryReferences.erase(q.created.back());
                        created.erase(q.created.back());
                        q.created.pop_back();
                    }
                    q.startFailure = "Cannot fill required alias " + alias.name + " (" + std::to_string(aliasId) + ")";
                    if (const auto error = errors.find(aliasId); error != errors.end())
                        q.startFailure += ": " + error->second;
                    if (diagnostic)
                        log.write("QUST " + q.editorId + " " + std::to_string(id), q.startFailure);
                    return false;
                }
            }
            q.running = true;
            const auto initialize = [&](const std::shared_ptr<Instance>& instance) {
                Task init{id, q.generation};
                init.calls.emplace_back(instance, std::string{});
                tasks.push_back(std::move(init));
            };
            bindRootScripts(q);
            for (const auto& record : q.vmad.common.scripts)
                if (const auto instance = q.instances.find(Misc::StringUtils::lowerCase(record.name));
                    instance != q.instances.end() && !instance->second->initialized)
                    initialize(instance->second);
            for (const auto& alias : q.vmad.aliases)
                if (alias.owner.alias >= 0 && q.record.mapFormId(alias.owner.rawObjectId) == id)
                    if (const auto found = q.aliases.find(alias.owner.alias); found != q.aliases.end())
                        for (const auto& record : alias.scripts)
                            if (auto instance = bindScript(q, record,
                                    AliasReference{id, static_cast<std::uint32_t>(alias.owner.alias), found->second.kind}))
                            {
                                initialize(instance);
                                q.aliasInstances.push_back(std::move(instance));
                            }
            for (const auto& [stage, data] : q.stages)
                if ((data.flags & 2) && requested != stage)
                    enqueue(id, q, stage);
            const auto count = std::ranges::count_if(q.aliases, [](const auto& entry) { return entry.second.fill.has_value(); });
            events.push_back({id, q.editorId, "QuestStarted",
                {{"T5_QUEST_STARTED " + q.editorId + " filled=" + std::to_string(count), 0}}, time, {}, 0});
            return true;
        }

        void startGame(std::optional<std::uint32_t> onlyQuest)
        {
            if (!index)
                return;
            std::vector<std::uint32_t> queued;
            std::set<std::uint32_t> seen;
            const auto add = [&](std::uint32_t id) {
                if ((!onlyQuest || id == *onlyQuest) && seen.insert(id).second)
                    queued.push_back(id);
            };
            if (onlyQuest)
                add(*onlyQuest);
            else
            {
                for (const auto& plugin : index->plugins)
                {
                    try
                    {
                        const VFS::Path::Normalized path(plugin.seqName);
                        if (!vfs->exists(path))
                            continue;
                        auto stream = vfs->get(path);
                        std::array<std::byte, 4> bytes;
                        std::size_t count = 0;
                        while (stream->read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
                        {
                            if (++count > 65536)
                                throw std::runtime_error("SEQ exceeds 65536 entries");
                            const auto id = plugin.mapFormId(number(bytes));
                            if (id)
                                add(id);
                        }
                        if (stream->gcount() != 0 || !stream->eof())
                            throw std::runtime_error("Truncated SEQ FormID");
                    }
                    catch (const std::exception& error)
                    {
                        log.write(plugin.seqName, error.what());
                    }
                }
                for (auto id : index->startGameQuests)
                    add(id);
            }

            // Retry dependent aliases after later quests have started. Failed rows
            // get one diagnostic, after their dependencies have had a chance to fill.
            while (!queued.empty())
            {
                const auto previous = queued.size();
                std::erase_if(queued, [&](auto id) {
                    try
                    {
                        auto& q = quest(id);
                        return (!onlyQuest && !(q.flags & 1)) || start(id, q, {}, false);
                    }
                    catch (const std::exception& error)
                    {
                        log.write("QUST startup " + std::to_string(id), error.what());
                        return true;
                    }
                });
                if (previous == queued.size())
                    break;
            }
            for (auto id : queued)
            {
                const auto& q = quest(id);
                if (!q.startFailure.empty())
                    log.write("QUST startup " + q.editorId + " " + std::to_string(id), q.startFailure);
            }
        }

        void cellLoaded()
        {
            for (auto& [id, q] : quests)
                if (q.running)
                    for (const auto& instance : q.aliasInstances)
                    {
                        const auto form = aliasValue(q, instance->aliasId.value());
                        bool present = form == playerReference || loaded.contains(form);
                        if (const auto found = created.find(form); found != created.end())
                            present |= loaded.contains(found->second.anchor);
                        if (present && !instance->cellLoaded)
                        {
                            Task task{id, q.generation};
                            task.calls.emplace_back(instance, "OnCellLoad");
                            tasks.push_back(std::move(task));
                        }
                        instance->cellLoaded = present;
                    }
        }

        void stop(Quest& q)
        {
            q.running = false;
            ++q.generation;
            q.pending.clear();
            q.instances.clear();
            q.aliasInstances.clear();
            repeatUpdates.erase(q.record.metadata.formId);
            std::erase_if(inventoryFilters, [&](const auto& filter) {
                return filter.first.first == q.record.metadata.formId;
            });
            for (auto& [id, alias] : q.aliases)
                alias.fill.reset();
        }

        HostCallbacks::StageRequest enqueue(std::uint32_t id, Quest& q, std::uint16_t stage)
        {
            if ((!(q.flags & 8) && q.done.contains(stage)) || !q.pending.insert(stage).second)
                return {};
            auto finished = std::make_shared<bool>(false);
            tasks.push_back(Task{id, q.generation, stage, finished});
            return {true, initializingQuest == id ? nullptr : finished};
        }

        HostCallbacks::StageRequest setStage(std::uint32_t id, std::int32_t stage)
        {
            auto& q = quest(id);
            if (stage < 0 || stage > 65535)
                return {};
            const auto stageId = static_cast<std::uint16_t>(stage);
            if (!q.stages.contains(stageId) || !start(id, q, stageId))
                return {};
            return enqueue(id, q, stageId);
        }

        bool testConditions(const Quest* q, const RecordPayload& source,
            std::span<const std::span<const std::byte>> conditions, std::uint32_t subject, std::uint32_t listener = 0)
        {
            bool any = false;
            for (const auto bytes : conditions)
            {
                const auto flags = number(bytes, 0, 1);
                if (any)
                {
                    any = (flags & 1) != 0;
                    continue;
                }
                if (bytes.size() != 32 || (flags & 0x18) != 0)
                    throw std::runtime_error("Unsupported CTDA flags");
                const auto expected = flags & 4 ? getGlobal(source.mapFormId(number(bytes, 4)))
                                               : std::bit_cast<float>(number(bytes, 4));
                const auto fn = number(bytes, 8, 2);
                const auto raw = number(bytes, 12);
                const auto alias = [&](std::uint32_t id) {
                    if (!q)
                        throw std::runtime_error("CTDA alias requires an owning quest");
                    return aliasValue(*q, id);
                };
                const auto parameter = [&] { return (flags & 2) ? alias(raw) : source.mapFormId(raw); };
                std::uint32_t receiver = subject;
                switch (number(bytes, 20))
                {
                    case 0: break;
                    case 1:
                        if (!listener) throw std::runtime_error("CTDA has no target");
                        receiver = listener;
                        break;
                    case 2: receiver = source.mapFormId(number(bytes, 24)); break;
                    case 4: receiver = subject ? reference(subject).linked : 0; break;
                    case 5: receiver = alias(number(bytes, 28)); break;
                    default: throw std::runtime_error("Unsupported CTDA Run On " + std::to_string(number(bytes, 20)));
                }
                float actual;
                switch (fn)
                {
                    case 1: {
                        const auto target = parameter();
                        if (receiver && target && base.callFormMethod)
                            if (const auto value = base.callFormMethod(receiver, "GetDistance", {formValue(target)}))
                            {
                                actual = std::get<float>(*value);
                                break;
                            }
                        const auto& from = receiver ? reference(receiver).position : std::nullopt;
                        const auto& to = target ? reference(target).position : std::nullopt;
                        actual = from && to
                            ? std::hypot((*from)[0] - (*to)[0], (*from)[1] - (*to)[1], (*from)[2] - (*to)[2])
                            : std::numeric_limits<float>::infinity();
                        break;
                    }
                    case 14: {
                        if (!base.getActorValue)
                            throw std::runtime_error("CTDA GetActorValue needs an actor value host");
                        const auto value = base.getActorValue(receiver, raw);
                        if (!value)
                            throw std::runtime_error("Unsupported CTDA actor value " + std::to_string(raw));
                        actual = *value;
                        break;
                    }
                    case 35: actual = receiver && disabled(receiver); break;
                    case 46: {
                        const auto value = receiver && base.callFormMethod
                            ? base.callFormMethod(receiver, "IsDead", {}) : std::nullopt;
                        actual = value ? std::get<bool>(*value) : receiver && reference(receiver).dead;
                        break;
                    }
                    case 47: {
                        const auto count = receiver && base.callFormMethod
                            ? base.callFormMethod(receiver, "GetItemCount", {formValue(parameter())}) : std::nullopt;
                        if (!count) throw std::runtime_error("CTDA GetItemCount needs an inventory host");
                        actual = std::get<std::int32_t>(*count);
                        break;
                    }
                    case 56: actual = quest(parameter()).running; break;
                    case 58: actual = quest(parameter()).current; break;
                    case 59: {
                        const auto stage = number(bytes, 16);
                        actual = stage <= 65535 && quest(parameter()).done.contains(static_cast<std::uint16_t>(stage));
                        break;
                    }
                    case 67: {
                        const auto value = receiver && base.callFormMethod
                            ? base.callFormMethod(receiver, "GetParentCell", {}) : std::nullopt;
                        const auto cell = value && std::holds_alternative<FormReference>(*value)
                            ? std::get<FormReference>(*value).formId : receiver ? reference(receiver).cell : 0;
                        actual = cell && cell == parameter();
                        break;
                    }
                    case 68:
                    case 69:
                    case 70:
                    case 71:
                    case 73:
                    case 426: {
                        const auto wanted = fn == 70 ? raw : parameter();
                        std::set<std::uint32_t> voiceTypes;
                        if (fn == 426 && wanted)
                        {
                            const auto voices = read(wanted);
                            if (voices.metadata.recordType == fourCC("FLST"))
                                visitSubrecords(voices, [&](auto tag, auto data) {
                                    if (tag == fourCC("LNAM")) voiceTypes.insert(voices.mapFormId(number(data)));
                                });
                        }
                        const auto field = fn == 68 ? fourCC("CNAM") : fn == 69 ? fourCC("RNAM")
                            : fn == 70 ? fourCC("ACBS") : fn == 426 ? fourCC("VTCK") : fourCC("SNAM");
                        const bool faction = fn == 71 || fn == 73;
                        if (faction && receiver && base.callFormMethod)
                            if (const auto result = base.callFormMethod(receiver, "GetFactionRank", {formValue(wanted)}))
                            {
                                const auto rank = std::get<std::int32_t>(*result);
                                actual = fn == 73 ? static_cast<float>(rank) : rank >= 0;
                                break;
                            }
                        int rank = -1;
                        actual = false;
                        auto actor = receiver ? reference(receiver).base : 0;
                        for (unsigned depth = 0; actor && depth < 16; ++depth)
                        {
                            const auto npc = read(actor);
                            if (npc.metadata.recordType != fourCC("NPC_")) break;
                            std::uint32_t parent = 0, templateFlags = 0;
                            bool matches = false;
                            rank = -1;
                            visitSubrecords(npc, [&](auto tag, auto data) {
                                if (tag == fourCC("TPLT")) parent = npc.mapFormId(number(data));
                                if (tag == fourCC("ACBS") && data.size() >= 20) templateFlags = number(data, 18, 2);
                                if (tag != field) return;
                                if (fn == 70) matches = (number(data) & 1u) == wanted;
                                else if (const auto form = npc.mapFormId(number(data));
                                    form == wanted || voiceTypes.contains(form))
                                {
                                    matches = true;
                                    if (faction && data.size() >= 5)
                                        rank = std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(number(data, 4, 1)));
                                }
                            });
                            if (parent && (templateFlags & (faction ? 4u : fn == 68 ? 2u : 1u)))
                            {
                                actor = parent;
                                continue;
                            }
                            actual = faction ? (fn == 73 ? static_cast<float>(rank) : rank >= 0) : matches;
                            break;
                        }
                        break;
                    }
                    case 72: actual = receiver && reference(receiver).base == parameter(); break;
                    case 74: actual = getGlobal(parameter()); break;
                    case 130:
                    case 131: {
                        // These conditions always query the player, regardless
                        // of the scene action's speaking actor / Run On subject.
                        // Read each time: RaceMenu replaces this record in place.
                        const auto player=read(7);
                        actual=false;
                        visitSubrecords(player,[&](auto tag,auto data) {
                            if (fn==130 && tag==fourCC("RNAM"))
                                actual=player.mapFormId(number(data))==parameter();
                            if (fn==131 && tag==fourCC("ACBS"))
                                actual=(number(data)&1u)==raw;
                        });
                        break;
                    }
                    case 136: actual = receiver && receiver == parameter(); break;
                    case 403: {
                        const auto rank = receiver && base.callFormMethod
                            ? base.callFormMethod(receiver, "GetRelationshipRank", {formValue(parameter())}) : std::nullopt;
                        actual = rank ? std::get<std::int32_t>(*rank) : 0;
                        break;
                    }
                    case 182: {
                        const auto value = receiver && base.callFormMethod
                            ? base.callFormMethod(receiver, "IsEquipped", { formValue(parameter()) }) : std::nullopt;
                        if (!value)
                            throw std::runtime_error("CTDA GetEquipped requires an inventory host");
                        actual = std::get<bool>(*value);
                        break;
                    }
                    case 359:
                    case 565: actual = receiver && inLocation(receiver, parameter()); break;
                    case 360: actual = receiver && inLocation(receiver, alias(raw)); break;
                    case 372: {
                        const auto list = read(parameter());
                        actual = false;
                        const auto refBase = receiver ? reference(receiver).base : 0;
                        visitSubrecords(list, [&](auto tag, auto data) {
                            if (tag == fourCC("LNAM"))
                            {
                                const auto id = list.mapFormId(number(data));
                                actual = actual || id == receiver || (refBase && id == refBase);
                            }
                        });
                        break;
                    }
                    case 543: actual = quest(parameter()).completed; break;
                    case 550: {
                        if (!base.callFormMethod)
                            throw std::runtime_error("CTDA IsSceneActionComplete needs a scene host");
                        const auto value = base.callFormMethod(parameter(), "IsActionComplete",
                            {std::bit_cast<std::int32_t>(number(bytes, 16))});
                        if (!value)
                            throw std::runtime_error("CTDA IsSceneActionComplete needs a scene host");
                        actual = std::get<bool>(*value);
                        break;
                    }
                    case 555: {
                        const auto value = receiver && base.callFormMethod
                            ? base.callFormMethod(receiver, "Is3DLoaded", {}) : std::nullopt;
                        actual = value ? std::get<bool>(*value) : receiver == playerReference || loaded.contains(receiver);
                        break;
                    }
                    case 560: actual = hasKeyword(receiver, parameter()); break;
                    case 255: {
                        const auto value = receiver && base.callFormMethod
                            ? base.callFormMethod(receiver, "GetOffersServicesNow", {}) : std::nullopt;
                        actual = value && std::get<bool>(*value);
                        break;
                    }
                    case 289: {
                        const auto value = receiver && base.callFormMethod
                            ? base.callFormMethod(receiver, "IsInCombat", {}) : std::nullopt;
                        actual = value && std::get<bool>(*value);
                        break;
                    }
                    case 707: {
                        const auto target = receiver && base.callFormMethod
                            ? base.callFormMethod(receiver, "GetCombatTarget", {}) : std::nullopt;
                        actual = target && std::holds_alternative<FormReference>(*target)
                            && hasKeyword(std::get<FormReference>(*target).formId, parameter());
                        break;
                    }
                    case 562: actual = hasKeyword(locationOf(receiver), parameter()); break;
                    case 566: actual = receiver && receiver == alias(raw); break;
                    case 700:
                        // This host does not put actors into Skyrim's commanded state.
                        actual = false;
                        break;
                    case 629:
                    case 630: {
                        std::string_view name;
                        bool selected = false;
                        visitSubrecords(source, [&](auto tag, auto data) {
                            if (tag == fourCC("CTDA"))
                                selected = data.data() == bytes.data();
                            else if (selected && tag == fourCC("CIS2") && !data.empty() && data.back() == std::byte{0})
                                name = {reinterpret_cast<const char*>(data.data()), data.size() - 1};
                        });
                        if (name.starts_with("::") && name.ends_with("_var"))
                            name = name.substr(2, name.size() - 6);
                        const auto value = name.empty() ? std::nullopt : readProperty(parameter(), name);
                        if (!value)
                            throw std::runtime_error("CTDA VM variable unavailable: " + std::string(name));
                        if (const auto* flag = std::get_if<bool>(&*value))
                            actual = *flag;
                        else if (const auto* integer = std::get_if<std::int32_t>(&*value))
                            actual = static_cast<float>(*integer);
                        else if (const auto* floating = std::get_if<float>(&*value))
                            actual = *floating;
                        else
                            throw std::runtime_error("CTDA VM variable requires a numeric or bool value");
                        break;
                    }
                    default: throw std::runtime_error("Unsupported CTDA function " + std::to_string(fn));
                }
                bool pass;
                switch (flags >> 5)
                {
                    case 0: pass = actual == expected; break;
                    case 1: pass = actual != expected; break;
                    case 2: pass = actual > expected; break;
                    case 3: pass = actual >= expected; break;
                    case 4: pass = actual < expected; break;
                    case 5: pass = actual <= expected; break;
                    default: throw std::runtime_error("Unsupported quest stage comparison");
                }
                any |= pass;
                if (!(flags & 1))
                {
                    if (!any)
                        return false;
                    any = false;
                }
            }
            return conditions.empty() || !(number(conditions.back(), 0, 1) & 1) || any;
        }

        bool advance(Task& task)
        {
            auto& q = quest(task.quest);
            if (!q.running || q.generation != task.generation)
                return true;
            if (task.stage && !task.prepared)
            {
                task.prepared = true;
                const auto& stage = q.stages.at(*task.stage);
                std::vector<bool> active;
                for (const auto& entry : stage.entries)
                {
                    try
                    {
                        active.push_back(testConditions(&q, q.record, entry.conditions, 0));
                        if (active.back())
                        {
                            q.completed |= (entry.flags & 1) != 0;
                            q.failed |= (entry.flags & 2) != 0;
                        }
                    }
                    catch (const std::exception& error)
                    {
                        log.write("QUST " + std::to_string(task.quest) + " CTDA", error.what());
                        active.push_back(false);
                    }
                }
                q.current = std::max(q.current, *task.stage);
                changedPackageQuests.insert(task.quest);
                events.push_back({task.quest, q.editorId, "QuestStage",
                    {{"T5_QUEST_STAGE " + q.editorId + " stage=" + std::to_string(*task.stage), 0}}, time, {}, 0});
                for (const auto& fragment : q.vmad.fragments)
                    if (fragment.stage == *task.stage)
                    {
                        if (fragment.logIndex >= active.size())
                            throw std::runtime_error("Quest fragment log index is outside QUST stage");
                        if (!active[fragment.logIndex])
                            continue;
                        const auto name = Misc::StringUtils::lowerCase(fragment.script);
                        const auto script = q.instances.find(name);
                        if (script == q.instances.end())
                            throw std::runtime_error("Missing quest fragment PEX " + fragment.script);
                        task.calls.emplace_back(script->second, fragment.function);
                    }
            }
            while (task.next < task.calls.size())
            {
                auto& [instance, function] = task.calls[task.next];
                if (instance->failed)
                {
                    task.failed = true;
                    ++task.next;
                    continue;
                }
                if ((!function.empty() && !instance->initialized) || task.due > time || (task.nativeWait && !*task.nativeWait))
                    return false;
                if (function.empty())
                    initializingQuest = task.quest;
                StepResult result;
                try
                {
                    result = task.continuation ? instance->vm.resume(*task.continuation)
                        : function.empty() ? instance->vm.startInitialization()
                                           : instance->vm.startEvent(function, task.arguments);
                }
                catch (...)
                {
                    initializingQuest.reset();
                    instance->failed = true;
                    throw;
                }
                initializingQuest.reset();
                const auto event = function.empty() ? "OnInit" : function;
                if (result.waitSeconds || (result.completed && result.completed->eventExecuted))
                    events.push_back({task.quest, instance->name, event,
                        result.completed ? std::move(result.completed->traces) : std::move(result.waitTraces), time,
                        result.waitSeconds, 0});
                else if (task.stage)
                    throw std::runtime_error("Quest PEX fragment function missing: " + function);
                if (result.waitSeconds)
                {
                    task.due = time + *result.waitSeconds;
                    task.continuation = result.continuation;
                    task.nativeWait = std::move(result.nativeWait);
                    return false;
                }
                if (function.empty())
                    instance->initialized = true;
                if (task.returned && result.completed)
                    *task.returned = std::move(result.completed->returned);
                task.continuation.reset();
                task.nativeWait.reset();
                ++task.next;
            }
            if (task.stage && !task.failed && q.generation == task.generation)
            {
                q.done.insert(*task.stage);
                changedPackageQuests.insert(task.quest);
                if (q.stages.at(*task.stage).flags & 4)
                    stop(q);
            }
            return true;
        }

        void tick(double now)
        {
            if(index && now>time)
            {
                const auto scale=index->globalsByName.find("timescale"), hour=index->globalsByName.find("gamehour"), days=index->globalsByName.find("gamedayspassed");
                if(scale!=index->globalsByName.end() && hour!=index->globalsByName.end())
                {
                    const double seconds=(now-time)*std::max(0.f,getGlobal(scale->second));
                    globals[hour->second]=static_cast<float>(std::fmod(getGlobal(hour->second)+seconds/3600.,24.));
                    if(days!=index->globalsByName.end()) globals[days->second]=getGlobal(days->second)+static_cast<float>(seconds/86400.);
                }
            }
            time = now;
            for (auto& [id, update] : repeatUpdates)
                if (time >= update.due && quest(id).running && quest(id).generation == update.generation)
                {
                    registerUpdate(id, {}, 0);
                    update.due = time + update.interval;
                }
            dispatchCalendarUpdates();
            const auto count = std::min<std::size_t>(tasks.size(), 128);
            for (std::size_t i = 0; i < count && !tasks.empty(); ++i)
            {
                auto task = std::move(tasks.front());
                tasks.pop_front();
                try
                {
                    if (!advance(task))
                    {
                        tasks.push_back(std::move(task));
                        continue;
                    }
                }
                catch (const std::exception& error)
                {
                    const auto detail = task.next < task.calls.size() ? " " + task.calls[task.next].second : "";
                    log.write("QUST " + std::to_string(task.quest)
                        + (task.stage ? " stage=" + std::to_string(*task.stage) : " OnInit") + detail, error.what());
                }
                if (task.stage && quest(task.quest).generation == task.generation)
                    quest(task.quest).pending.erase(*task.stage);
                if (task.finished)
                    *task.finished = true;
            }
        }
    };

    QuestScripts::QuestScripts(ReadWinningRecord read, std::shared_ptr<const VFS::Manager> vfs,
        Papyrus::HostCallbacks base, Papyrus::ErrorLog& log)
        : mImpl(std::make_unique<Impl>(std::move(read), std::move(vfs), std::move(base), log))
    {
    }
    QuestScripts::~QuestScripts() = default;
    Papyrus::HostCallbacks QuestScripts::host() const { return mImpl->host(); }
    void QuestScripts::setRecordIndex(std::shared_ptr<const WinningRecordIndex> index) { mImpl->index = std::move(index); }
    void QuestScripts::addActors(const std::map<std::uint32_t, std::uint32_t>& actors)
    {
        for (const auto& [id, base] : actors)
            mImpl->actorsByBase[base].insert(id);
    }
    void QuestScripts::setLoadedReferences(std::span<const std::uint32_t> references)
    {
        mImpl->loaded = {references.begin(), references.end()};
    }
    void QuestScripts::startGame(std::optional<std::uint32_t> onlyQuest) { mImpl->startGame(onlyQuest); }
    void QuestScripts::startDialogueQuest(std::uint32_t id)
    {
        auto& quest = mImpl->quest(id);
        if ((quest.flags & 1) && !quest.generation && !quest.running)
            mImpl->start(id, quest, {}, false);
    }
    void QuestScripts::cellLoaded() { mImpl->cellLoaded(); }
    void QuestScripts::updateLocations() { mImpl->updateLocations(); }
    void QuestScripts::animationEvent(std::uint32_t sender, std::string_view event)
    {
        mImpl->animationEvent(sender, event);
    }
    void QuestScripts::actorDeath(std::uint32_t actor, std::uint32_t killer)
    {
        mImpl->actorEvent(actor, "OnDeath", {formValue(killer)});
    }
    void QuestScripts::actorHit(std::uint32_t actor, std::uint32_t aggressor, std::uint32_t source, std::uint32_t projectile)
    {
        mImpl->actorEvent(actor, "OnHit", {formValue(aggressor), formValue(source), formValue(projectile),
            false, false, false, false});
    }
    void QuestScripts::inventoryChanged(std::uint32_t reference, std::uint32_t item, std::int32_t count,
        std::uint32_t other, bool added)
    {
        mImpl->inventoryChanged(reference, item, count, other, added);
    }
    void QuestScripts::equipmentChanged(std::uint32_t actor, std::uint32_t item, bool equipped)
    {
        mImpl->actorEvent(actor, equipped ? "OnObjectEquipped" : "OnObjectUnequipped",
            {formValue(item), Value{}});
    }
    std::uint32_t QuestScripts::placeReference(std::uint32_t base, std::uint32_t anchor, bool disabled)
    {
        return mImpl->placeReference(base, anchor, disabled);
    }
    bool QuestScripts::deleteReference(std::uint32_t id)
    {
        for (const auto& [quest, state] : mImpl->quests)
            if (std::ranges::find(state.created, id) != state.created.end())
                return false;
        mImpl->references.erase(id);
        return mImpl->created.erase(id) != 0;
    }
    void QuestScripts::checkpointInventoryFilters(Papyrus::Checkpoint& ar) { ar(mImpl->inventoryFilters); }
    void QuestScripts::checkpointLocationAndTimers(Papyrus::Checkpoint& ar)
    { ar(mImpl->currentLocations, mImpl->calendarUpdates); }
    void QuestScripts::checkpointInventoryReferences(Papyrus::Checkpoint& ar)
    { ar(mImpl->inventoryReferences); }
    void QuestScripts::checkpointRepeatUpdates(Papyrus::Checkpoint& ar)
    { ar(mImpl->repeatUpdates); }

    const std::map<std::uint32_t, std::uint32_t>& QuestScripts::inventoryReferences() const
    { return mImpl->inventoryReferences; }

    void QuestScripts::transferInventoryReferences(std::uint32_t base, std::uint32_t previous,
        std::uint32_t next, std::int32_t count)
    {
        if (previous == next)
            return;
        for (auto& [item, owner] : mImpl->inventoryReferences)
            if (count > 0 && owner == previous && mImpl->created.at(item).base == base)
            {
                owner = next;
                --count;
                mImpl->actorEvent(item, "OnContainerChanged", {formValue(next), formValue(previous)});
            }
    }
    bool QuestScripts::isRunning(std::uint32_t id) const
    {
        const auto found = mImpl->quests.find(id);
        return found != mImpl->quests.end() && found->second.running;
    }
    std::uint32_t QuestScripts::referenceAlias(std::uint32_t id, std::uint32_t alias) const
    {
        const auto found = mImpl->quests.find(id);
        if (found == mImpl->quests.end() || !found->second.running)
            return 0;
        const auto entry = found->second.aliases.find(alias);
        return entry != found->second.aliases.end() && entry->second.kind == AliasKind::Reference
            ? entry->second.fill.value_or(0) : 0;
    }
    bool QuestScripts::isOptionalAlias(std::uint32_t id, std::uint32_t alias) const
    {
        const auto found = mImpl->quests.find(id);
        if (found == mImpl->quests.end())
            return false;
        const auto entry = found->second.aliases.find(alias);
        return entry != found->second.aliases.end() && (entry->second.flags & optionalAlias) != 0;
    }
    std::vector<std::uint32_t> QuestScripts::packagesForActor(std::uint32_t actor) const
    {
        std::vector<const Quest*> quests;
        for (const auto& [id, quest] : mImpl->quests)
            if (quest.running)
                quests.push_back(&quest);
        std::ranges::stable_sort(quests, std::greater<>{}, [](const Quest* q) { return q->priority; });
        std::vector<std::uint32_t> packages;
        for (const auto* quest : quests)
            for (const auto id : quest->aliasOrder)
            {
                const auto& alias = quest->aliases.at(id);
                if (alias.kind == AliasKind::Reference && alias.fill == actor)
                    packages.insert(packages.end(), alias.packages.begin(), alias.packages.end());
            }
        return packages;
    }
    std::uint32_t QuestScripts::packageQuestForActor(std::uint32_t actor, std::uint32_t pack) const
    {
        const Quest* match = nullptr;
        for (const auto& [id, quest] : mImpl->quests)
            if (quest.running && (!match || quest.priority > match->priority))
                for (const auto& [aliasId, alias] : quest.aliases)
                    if (alias.kind == AliasKind::Reference && alias.fill == actor
                        && std::ranges::find(alias.packages, pack) != alias.packages.end())
                    {
                        match = &quest;
                        break;
                    }
        return match ? match->record.metadata.formId : 0;
    }
    bool QuestScripts::testConditions(std::uint32_t quest, const RecordPayload& source,
        std::span<const std::span<const std::byte>> conditions, std::uint32_t subject, std::uint32_t target)
    {
        return conditions.empty() || mImpl->testConditions(quest ? &mImpl->quest(quest) : nullptr, source, conditions, subject, target);
    }
    const std::map<std::uint32_t, QuestCreatedReference>& QuestScripts::createdReferences() const
    {
        return mImpl->created;
    }
    void QuestScripts::tick(double time) { mImpl->tick(time); }
    std::vector<std::uint32_t> QuestScripts::objectiveTargets() { return mImpl->activeObjectiveTargets(); }
    float QuestScripts::globalValue(std::string_view name, float fallback) const
    {
        if(!mImpl->index)return fallback;
        const auto found=mImpl->index->globalsByName.find(name);
        return found==mImpl->index->globalsByName.end()?fallback:mImpl->getGlobal(found->second);
    }
    std::set<std::uint32_t> QuestScripts::takeChangedPackageQuests()
    {
        return std::exchange(mImpl->changedPackageQuests, {});
    }
    std::vector<SceneScriptEvent> QuestScripts::takeEvents() { return std::exchange(mImpl->events, {}); }
    void QuestScripts::checkpoint(Papyrus::Checkpoint& ar) { mImpl->checkpoint(ar); }

}
