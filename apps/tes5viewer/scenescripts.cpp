#include "scenescripts.hpp"
#include "checkpointtypes.hpp"

#include "../papyrustool/errorlog.hpp"
#include "../papyrustool/vmadbinding.hpp"
#include "cellscene.hpp"

#include <components/misc/strings/algorithm.hpp>
#include <components/vfs/manager.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace TES5Viewer
{
    namespace
    {
        using OwnerBindings = std::map<std::uint32_t, Papyrus::ScriptBindings>;

        std::shared_ptr<const Papyrus::Script> readReferenceScript(const VFS::Manager& vfs, std::string name)
        {
            std::set<std::string> visited;
            const auto read = [&](auto&& self, std::string className) -> std::shared_ptr<const Papyrus::Script> {
                className = Misc::StringUtils::lowerCase(className);
                if (visited.size() >= 16 || !visited.insert(className).second)
                    throw std::runtime_error("Invalid PEX inheritance chain");
                auto child = checkpointScript(vfs, className);
                const auto parentName = Misc::StringUtils::lowerCase(child->parent);
                // Engine base classes are implemented by the VM host.
                if (parentName.empty() || parentName == "objectreference" || parentName == "actor"
                    || parentName == "form" || parentName == "activemagiceffect" || parentName == "alias"
                    || parentName == "referencealias" || parentName == "quest")
                    return child;
                if (parentName.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos
                    || !vfs.exists(VFS::Path::Normalized("scripts/" + parentName + ".pex")))
                    return child;
                const auto parent = self(self, parentName);
                auto definition = std::make_shared<Papyrus::Script>(*child);
                for (const auto& property : parent->properties)
                    if (std::ranges::none_of(definition->properties, [&](const auto& own) {
                        return Misc::StringUtils::ciEqual(own.name, property.name);
                    }))
                        definition->properties.push_back(property);
                for (const auto& variable : parent->variables)
                    if (std::ranges::none_of(definition->variables, [&](const auto& own) {
                        return Misc::StringUtils::ciEqual(own.slot.name, variable.slot.name);
                    }))
                        definition->variables.push_back(variable);
                for (const auto& state : parent->states)
                {
                    const auto own = std::ranges::find_if(definition->states, [&](const auto& candidate) {
                        return Misc::StringUtils::ciEqual(candidate.name, state.name);
                    });
                    if (own == definition->states.end())
                        definition->states.push_back(state);
                    else
                        for (const auto& function : state.functions)
                            if (std::ranges::none_of(own->functions, [&](const auto& candidate) {
                                return Misc::StringUtils::ciEqual(candidate.name, function.name);
                            }))
                                own->functions.push_back(function);
                }
                if (definition->autoState.empty())
                    definition->autoState = parent->autoState;
                return definition;
            };
            return read(read, std::move(name));
        }

        OwnerBindings collectBindings(const CellScene& scene, const ScriptDiagnostic& diagnostic)
        {
            OwnerBindings bindings;
            // Base attachments are installed first. A direct attachment can replace
            // or remove the script on this particular reference.
            for (const bool direct : { false, true })
            {
                for (const auto& record : scene.scriptAttachments)
                {
                    if ((record.formId == record.ownerReferenceId) != direct)
                        continue;
                    try
                    {
                        Papyrus::applyVmad(bindings[record.ownerReferenceId], record, {}, direct);
                    }
                    catch (const std::exception& error)
                    {
                        if (!diagnostic)
                            throw;
                        diagnostic("VMAD owner=" + std::to_string(record.ownerReferenceId) + " " + record.sourcePlugin, error.what());
                    }
                }
            }
            return bindings;
        }

    }

    std::vector<PreparedSceneScript> prepareSceneScripts(
        const CellScene& scene, const VFS::Manager& vfs, const ScriptDiagnostic& diagnostic,
        const std::map<std::uint32_t, bool>* visible, const Papyrus::ResolveQuestAlias& aliases)
    {
        std::vector<PreparedSceneScript> prepared;
        std::map<std::string, std::shared_ptr<const Papyrus::Script>> files;
        for (const auto& [owner, scripts] : collectBindings(scene, diagnostic))
        {
            if (visible && !visible->contains(owner))
                continue;
            for (const auto& [name, binding] : scripts)
            try
            {
                if (!std::all_of(name.begin(), name.end(),
                        [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
                    throw std::runtime_error("Invalid Skyrim script name '" + name + "'");
                const VFS::Path::Normalized path("scripts/" + name + ".pex");
                auto& script = files[name];
                if (!script)
                    script = readReferenceScript(vfs, name);
                prepared.push_back({ owner, script, vfs.getArchive(path),
                    Papyrus::resolveProperties(binding, aliases) });
            }
            catch (const std::exception& error)
            {
                if (!diagnostic)
                    throw;
                diagnostic(name + " owner=" + std::to_string(owner), error.what());
            }
        }
        return prepared;
    }

    SceneScripts::SceneScripts(const CellScene& scene, const VFS::Manager& vfs, Papyrus::HostCallbacks host,
        ScenePapyrusReport& report, Papyrus::ErrorLog* log)
        : SceneScripts(std::move(host), report, log, false)
    {
        install(prepareSceneScripts(scene, vfs, {}, &report.visibility));
    }

    SceneScripts::SceneScripts(Papyrus::HostCallbacks host, ScenePapyrusReport& report, Papyrus::ErrorLog* log,
        bool tolerateErrors)
        : mHost(std::move(host))
        , mStaged(mHost)
        , mReport(report)
        , mLog(log)
        , mTolerateErrors(tolerateErrors)
    {
        mStaged.setDisabled = [this](std::uint32_t form, bool disabled, bool fade) {
            if (fade)
                throw std::runtime_error("Papyrus fade effects are unsupported");
            mHost.isDisabled(form);
            mPendingDisabled.insert_or_assign(form, disabled);
        };
        mStaged.isDisabled = [this](std::uint32_t form) {
            const auto pending = mPendingDisabled.find(form);
            return pending != mPendingDisabled.end() ? pending->second : mHost.isDisabled(form);
        };
        mStaged.registerSingleUpdate = [this](std::uint32_t form, double delay) {
            if (!mInstances.contains(form))
                throw std::runtime_error("Papyrus update requires a scripted reference");
            if (!std::isfinite(delay) || delay < 0 || delay > Papyrus::MaxScheduledDelaySeconds)
                throw std::runtime_error("Papyrus update delay is out of range");
            mPendingUpdates.insert_or_assign(form, mTime + delay);
        };
    }

    void SceneScripts::install(const std::vector<PreparedSceneScript>& scripts)
    {
        mAttached.clear();
        for (const auto& entry : scripts)
            if (mReport.visibility.contains(entry.owner))
                mAttached.insert(entry.owner);
        installInstances(scripts);
    }

    void SceneScripts::addEffect(std::uint32_t owner, std::uint32_t target, std::uint32_t caster, double expires,
        const std::vector<PreparedSceneScript>& scripts)
    {
        if (scripts.empty())
            return;
        mEffects.emplace(owner, Effect{target, caster, expires});
        installInstances(scripts);
        enqueue(mInstances.at(owner), {Event::EffectStart});
    }

    std::uint32_t SceneScripts::effectActor(std::uint32_t owner, bool caster) const
    {
        const auto it = mEffects.find(owner);
        return it == mEffects.end() ? 0 : caster ? it->second.caster : it->second.target;
    }

    Papyrus::HostCallbacks SceneScripts::instanceHost(std::uint32_t owner, const std::string& script)
    {
        auto callbacks = mStaged;
        callbacks.isFormType = [this](auto id, std::string_view type) {
            if (const auto it = mInstances.find(id); it != mInstances.end())
                if (std::ranges::any_of(it->second, [&](const auto& instance) {
                    return !instance.failed && Misc::StringUtils::ciEqual(instance.name, type);
                }))
                    return true;
            return mStaged.isFormType && mStaged.isFormType(id, type);
        };
        callbacks.callFormScriptMethod = [this](auto id, std::string_view type, std::string_view method,
            const auto& args) -> std::optional<Papyrus::HostCallbacks::ScriptCall> {
            if (const auto it = mInstances.find(id); it != mInstances.end())
                for (const auto& instance : it->second)
                    if (!instance.failed && Misc::StringUtils::ciEqual(instance.name, type))
                    {
                        if (args.empty() && Misc::StringUtils::ciEqual(method, "GetState"))
                            return Papyrus::HostCallbacks::ScriptCall{instance.vm.currentState()};
                        if (instance.vm.hasFunction(method))
                        {
                            if (mCalls.size() >= 4096)
                                throw std::runtime_error("Scene Papyrus call queue limit exceeded");
                            auto finished = std::make_shared<bool>(false);
                            auto returned = std::make_shared<Papyrus::Value>();
                            mCalls.push_back({id, instance.name, std::string(method), args, finished, returned, mTime});
                            return Papyrus::HostCallbacks::ScriptCall{{}, std::move(finished), std::move(returned)};
                        }
                    }
            return mStaged.callFormScriptMethod ? mStaged.callFormScriptMethod(id, type, method, args) : std::nullopt;
        };
        callbacks.getFormProperty = [this](auto id, std::string_view name) -> std::optional<Papyrus::Value> {
            if (const auto it = mInstances.find(id); it != mInstances.end())
                for (const auto& instance : it->second)
                    if (!instance.failed && instance.vm.hasAutoProperty(name))
                        return instance.vm.readAutoProperty(name);
            return mStaged.getFormProperty ? mStaged.getFormProperty(id, name) : std::nullopt;
        };
        callbacks.setFormProperty = [this](auto id, std::string_view name, const auto& value) {
            if (const auto it = mInstances.find(id); it != mInstances.end())
                for (auto& instance : it->second)
                    if (!instance.failed && instance.vm.hasAutoProperty(name))
                    {
                        instance.vm.writeAutoProperty(name, value);
                        return true;
                    }
            return mStaged.setFormProperty && mStaged.setFormProperty(id, name, value);
        };
        callbacks.callFormMethod = [this, owner, script](auto receiver, std::string_view method, const auto& args)
            -> std::optional<Papyrus::Value> {
            const bool add = Misc::StringUtils::ciEqual(method, "RegisterForAnimationEvent");
            if (receiver == owner && (add || Misc::StringUtils::ciEqual(method, "UnregisterForAnimationEvent")))
            {
                if (args.size() != 2)
                    throw std::runtime_error("Animation registration requires a sender and event name");
                const auto* sender = std::get_if<Papyrus::FormReference>(&args[0]);
                const auto& event = std::get<std::string>(args[1]);
                if (!sender || !sender->formId || event.empty() || !mHost.isFormType(sender->formId, "ObjectReference"))
                    return add ? Papyrus::Value(false) : Papyrus::Value{};
                const AnimationKey key{sender->formId, event}, subscriber{owner, script};
                if (add)
                    mAnimationEvents[key].insert(subscriber);
                else if (const auto it = mAnimationEvents.find(key); it != mAnimationEvents.end())
                {
                    it->second.erase(subscriber);
                    if (it->second.empty())
                        mAnimationEvents.erase(it);
                }
                return add ? Papyrus::Value(true) : Papyrus::Value{};
            }
            return mStaged.callFormMethod ? mStaged.callFormMethod(receiver, method, args) : std::nullopt;
        };
        return callbacks;
    }

    void SceneScripts::animationEvent(std::uint32_t sender, std::string_view event)
    {
        const auto subscriptions = mAnimationEvents.find({sender, std::string(event)});
        if (subscriptions == mAnimationEvents.end())
            return;
        for (const auto& [owner, script] : subscriptions->second)
            if (const auto instances = mInstances.find(owner); instances != mInstances.end())
                for (auto& instance : instances->second)
                    if (instance.name == script && !instance.failed && instance.pending.size() < 128)
                        instance.pending.push_back({Event::Animation, sender, std::string(event)});
    }

    void SceneScripts::installInstances(const std::vector<PreparedSceneScript>& scripts)
    {
        std::vector<std::pair<std::uint32_t, std::size_t>> added;
        for (const auto& entry : scripts)
        {
            if (!mAttached.contains(entry.owner) && !mEffects.contains(entry.owner))
                continue;
            auto& instances = mInstances[entry.owner];
            if (std::ranges::any_of(instances,
                    [&](const Instance& instance) { return Misc::StringUtils::ciEqual(instance.name, entry.script->name); }))
                continue;
            try
            {
                if (mReport.scripts.size() >= (mTolerateErrors ? 65536 : 256))
                    throw std::runtime_error("Scene Papyrus instance limit exceeded");
                instances.emplace_back(Instance{ entry.script->name,
                    Papyrus::ScriptInstance(entry.script, entry.owner, entry.properties,
                        instanceHost(entry.owner, entry.script->name)) });
                if (!mEffects.contains(entry.owner))
                    mReport.scripts.push_back({ entry.owner, entry.script->name, entry.source });
                added.emplace_back(entry.owner, instances.size() - 1);
            }
            catch (const std::exception& error)
            {
                if (!mTolerateErrors)
                    throw;
                if (mLog)
                    mLog->write(entry.script->name + " owner=" + std::to_string(entry.owner), error.what());
            }
        }
        // Install all instances before running any initializers.
        for (const auto& [owner, index] : added)
            step(owner, mInstances.at(owner)[index], Event::Init, mNextInvocation++);
    }

    bool SceneScripts::hasScripts(std::uint32_t owner) const
    {
        const auto it = mInstances.find(owner);
        return it != mInstances.end() && std::ranges::any_of(it->second, [](const Instance& entry) { return !entry.failed; });
    }

    void SceneScripts::step(std::uint32_t owner, Instance& instance, Event kind, std::uint64_t invocation,
        std::optional<std::uint64_t> continuation, std::uint32_t activator, std::string_view animation,
        std::int32_t nextStage)
    {
        Papyrus::StepResult result;
        std::string event;
        switch (kind)
        {
            case Event::Init:
                event = "OnInit";
                break;
            case Event::Activate:
                event = "OnActivate";
                break;
            case Event::Update:
                event = "OnUpdate";
                break;
            case Event::TriggerEnter:
                event = "OnTriggerEnter";
                break;
            case Event::TriggerLeave:
                event = "OnTriggerLeave";
                break;
            case Event::Load:
                event = "OnLoad";
                break;
            case Event::CellLoad:
                event = "OnCellLoad";
                break;
            case Event::DestructionStage:
                event = "OnDestructionStageChanged";
                break;
            case Event::Unload:
                event = "OnUnload";
                break;
            case Event::Animation:
                event = "OnAnimationEvent";
                break;
            case Event::EffectStart:
            case Event::EffectFinish:
                event = kind == Event::EffectStart ? "OnEffectStart" : "OnEffectFinish";
                break;
        }
        try
        {
            if (continuation)
                result = instance.vm.resume(*continuation);
            else if (kind == Event::Init)
                result = instance.vm.startInitialization();
            else if (kind == Event::Activate || kind == Event::TriggerEnter || kind == Event::TriggerLeave)
                result = instance.vm.startEvent(event,
                    { activator ? Papyrus::Value(Papyrus::FormReference{activator}) : Papyrus::Value(std::monostate{}) });
            else if (kind == Event::EffectStart || kind == Event::EffectFinish)
            {
                const auto& effect = mEffects.at(owner);
                result = instance.vm.startEvent(event, {Papyrus::FormReference{effect.target}, Papyrus::FormReference{effect.caster}});
            }
            else if (kind == Event::Animation)
                result = instance.vm.startEvent(event, {Papyrus::FormReference{activator}, std::string(animation)});
            else if (kind == Event::DestructionStage)
                result = instance.vm.startEvent(event, {static_cast<std::int32_t>(activator), nextStage});
            else
                result = instance.vm.startEvent(event);
        }
        catch (const std::exception& error)
        {
            mPendingDisabled.clear();
            mPendingUpdates.clear();
            if (mLog)
            {
                std::ostringstream context;
                context << (mTolerateErrors ? "T5 Papyrus " : "tes5viewer ") << instance.name << '.' << event
                        << " owner=0x" << std::hex << std::uppercase
                        << std::setw(8) << std::setfill('0') << owner << std::dec << " invocation=" << invocation
                        << " scene_seconds=" << mTime;
                if (continuation)
                    context << " continuation=" << *continuation;
                mLog->write(context.str(), error.what());
            }
            if (!mTolerateErrors)
                throw;
            instance.failed = true;
            instance.pending.clear();
            instance.waiting.clear();
            return;
        }

        if (result.waitSeconds)
            instance.waiting.push_back({ kind, mTime + *result.waitSeconds, result.continuation.value(), invocation,
                std::move(result.nativeWait) });
        else if (kind == Event::Init)
            instance.initializing = false;

        if (result.waitSeconds || (result.completed && result.completed->eventExecuted))
        {
            auto traces = result.completed ? std::move(result.completed->traces) : std::move(result.waitTraces);
            constexpr std::size_t maxTraceBytes = 8 * 1024 * 1024;
            for (const auto& trace : traces)
            {
                if (trace.message.size() > maxTraceBytes - mTraceBytes)
                    throw std::runtime_error("Scene Papyrus trace limit exceeded");
                mTraceBytes += trace.message.size();
            }
            if (mReport.events.size() >= 4096)
                throw std::runtime_error("Scene Papyrus event limit exceeded");
            mReport.events.push_back(
                { owner, instance.name, std::move(event), std::move(traces), mTime, result.waitSeconds, invocation });
        }
        commitNativeChanges();
    }

    void SceneScripts::commitNativeChanges()
    {
        for (const auto& [form, disabled] : mPendingDisabled)
            mHost.setDisabled(form, disabled, false);
        mPendingDisabled.clear();
        for (const auto& [form, due] : mPendingUpdates)
            mUpdates.insert_or_assign(form, due);
        mPendingUpdates.clear();
    }

    void SceneScripts::enqueue(std::vector<Instance>& instances, Pending event)
    {
        for (const auto& instance : instances)
            if (instance.pending.size() >= 128)
                throw std::runtime_error("Papyrus pending event limit exceeded");
        for (auto& instance : instances)
            if (!instance.failed)
                instance.pending.push_back(event);
    }

    void SceneScripts::startNext(std::uint32_t owner, Instance& instance)
    {
        if (instance.failed || instance.initializing || instance.pending.empty() || instance.waiting.size() >= 16)
            return;
        const auto event = instance.pending.front();
        instance.pending.pop_front();
        step(owner, instance, event.event, mNextInvocation++, std::nullopt, event.activator, event.animation,
            event.nextStage);
    }

    void SceneScripts::activate(std::uint32_t owner, std::uint32_t activator, bool immediate)
    {
        const auto it = mInstances.find(owner);
        if (it == mInstances.end() || mHost.isDisabled(owner))
            throw std::runtime_error("Activation requires an enabled scripted reference in the selected cell");
        updateLoaded(owner, it->second);
        enqueue(it->second, {Event::Activate, activator});
        if (immediate)
            for (auto& instance : it->second)
                startNext(owner, instance);
    }

    void SceneScripts::trigger(std::uint32_t owner, std::uint32_t actor, bool entering)
    {
        const auto it = mInstances.find(owner);
        if (it != mInstances.end() && !mHost.isDisabled(owner))
        {
            updateLoaded(owner, it->second);
            enqueue(it->second, {entering ? Event::TriggerEnter : Event::TriggerLeave, actor});
        }
    }

    void SceneScripts::destructionStage(std::uint32_t owner, std::int32_t previous, std::int32_t next)
    {
        if (const auto instances = mInstances.find(owner); instances != mInstances.end())
            for (auto& instance : instances->second)
                if (!instance.failed && instance.vm.hasFunction("OnDestructionStageChanged") && instance.pending.size() < 128)
                    instance.pending.push_back({Event::DestructionStage, static_cast<std::uint32_t>(previous), {}, next});
    }

    void SceneScripts::tickCalls()
    {
        const auto count = std::min<std::size_t>(mCalls.size(), 128);
        for (std::size_t i = 0; i < count; ++i)
        {
            auto call = std::move(mCalls.front());
            mCalls.pop_front();
            if (const auto instances = mInstances.find(call.owner); instances != mInstances.end())
                if (const auto instance = std::ranges::find(instances->second, call.script, &Instance::name);
                    instance != instances->second.end() && !instance->failed)
                {
                    if (instance->initializing || mTime < call.due || (call.nativeWait && !*call.nativeWait))
                    {
                        mCalls.push_back(std::move(call));
                        continue;
                    }
                    try
                    {
                        auto result = call.continuation ? instance->vm.resume(*call.continuation)
                            : instance->vm.startEvent(call.method, call.arguments);
                        commitNativeChanges();
                        if (result.continuation)
                        {
                            call.continuation = result.continuation;
                            call.due = mTime + result.waitSeconds.value_or(0);
                            call.nativeWait = std::move(result.nativeWait);
                            mCalls.push_back(std::move(call));
                            continue;
                        }
                        if (result.completed)
                            *call.returned = std::move(result.completed->returned);
                    }
                    catch (const std::exception& error)
                    {
                        mPendingDisabled.clear();
                        mPendingUpdates.clear();
                        instance->failed = true;
                        if (mLog) mLog->write(call.script + "." + call.method, error.what());
                    }
                }
            *call.finished = true;
        }
    }

    void SceneScripts::updateLoaded(std::uint32_t owner, std::vector<Instance>& instances)
    {
        if (!mAttached.contains(owner) || mHost.isDisabled(owner))
        {
            if (mLoaded.erase(owner))
                for (auto& instance : instances)
                    if (!instance.failed && instance.vm.hasFunction("OnUnload"))
                        instance.pending.push_back({Event::Unload});
            return;
        }
        if (mLoaded.insert(owner).second)
            for (auto& instance : instances)
                if (!instance.failed)
                {
                    if (instance.vm.hasFunction("OnLoad"))
                        instance.pending.push_back({Event::Load});
                    if (instance.vm.hasFunction("OnCellLoad"))
                        instance.pending.push_back({Event::CellLoad});
                }
    }

    void SceneScripts::tick(double time)
    {
        if (!std::isfinite(time) || time < mTime)
            throw std::runtime_error("Papyrus scene clock must be finite and monotonic");
        mTime = time;
        tickCalls();
        // Timers added while executing an event become eligible on the next tick.
        for (auto it = mUpdates.begin(); it != mUpdates.end();)
        {
            if (it->second > mTime)
            {
                ++it;
                continue;
            }
            enqueue(mInstances.at(it->first), {Event::Update});
            it = mUpdates.erase(it);
        }
        for (auto& [owner, instances] : mInstances)
        {
            updateLoaded(owner, instances);
            for (auto& instance : instances)
            {
                const auto parked = instance.waiting.size();
                for (std::size_t i = 0; i < parked && !instance.failed; ++i)
                {
                    const auto waiting = instance.waiting.front();
                    instance.waiting.pop_front();
                    if (waiting.due <= mTime && (!waiting.nativeWait || *waiting.nativeWait))
                        step(owner, instance, waiting.event, waiting.invocation, waiting.continuation);
                    else
                        instance.waiting.push_back(waiting);
                }
                startNext(owner, instance);
            }
        }
        for (auto it = mEffects.begin(); it != mEffects.end();)
        {
            const auto owner = it->first;
            auto& effect = it->second;
            auto& instances = mInstances.at(owner);
            if (!effect.finishing && effect.expires <= mTime)
            {
                effect.finishing = true;
                mUpdates.erase(owner);
                enqueue(instances, {Event::EffectFinish});
            }
            if (effect.finishing && std::ranges::none_of(instances,
                [](const auto& instance) { return !instance.failed && (!instance.pending.empty() || !instance.waiting.empty()); }))
            {
                mUpdates.erase(owner);
                mInstances.erase(owner);
                it = mEffects.erase(it);
            }
            else
                ++it;
        }
    }

    std::vector<SceneScriptEvent> SceneScripts::takeEvents()
    {
        mTraceBytes = 0;
        return std::exchange(mReport.events, {});
    }
    void SceneScripts::Waiting::checkpoint(Papyrus::Checkpoint& ar) { ar(event, due, continuation, invocation, nativeWait); }
    void SceneScripts::Pending::checkpoint(Papyrus::Checkpoint& ar)
    {
        ar(event, activator);
        if (event == Event::Animation)
            ar(animation);
        if (event == Event::DestructionStage)
            ar(nextStage);
    }
    void SceneScripts::Call::checkpoint(Papyrus::Checkpoint& ar)
    { ar(owner, script, method, arguments, finished, returned, due, continuation, nativeWait); }
    void SceneScripts::Effect::checkpoint(Papyrus::Checkpoint& ar) { ar(target, caster, expires, finishing); }
    void SceneScripts::checkpointLifecycle(Papyrus::Checkpoint& ar) { ar(mAttached, mLoaded, mEffects); }
    void SceneScripts::checkpointAnimationEvents(Papyrus::Checkpoint& ar) { ar(mAnimationEvents); }
    void SceneScripts::checkpointCalls(Papyrus::Checkpoint& ar) { ar(mCalls); }
    void SceneScripts::checkpoint(Papyrus::Checkpoint& ar, const VFS::Manager& vfs)
    {
        ar(mPendingDisabled, mUpdates, mPendingUpdates, mTime, mNextInvocation, mTraceBytes);
        auto it = mInstances.begin();
        if (ar.loading) mInstances.clear();
        ar.count(mInstances.size(), [&](auto) {
            std::uint32_t owner = ar.loading ? 0 : it->first; ar(owner);
            auto& instances = ar.loading ? mInstances[owner] : it++->second;
            ar.count(instances.size(), [&](auto i) {
                std::string name = ar.loading ? "" : instances[i].name; ar(name);
                if (ar.loading) instances.push_back({name,
                    Papyrus::ScriptInstance(readReferenceScript(vfs, name), owner, {}, instanceHost(owner, name))});
                auto& instance = instances[i];
                ar(instance.vm, instance.initializing, instance.failed, instance.waiting, instance.pending);
            });
        });
    }

}
