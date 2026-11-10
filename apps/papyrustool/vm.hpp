#ifndef OPENMW_APPS_PAPYRUSTOOL_VM_HPP
#define OPENMW_APPS_PAPYRUSTOOL_VM_HPP

#include "pex.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string_view>

namespace Papyrus
{
    class Checkpoint;
    struct Trace
    {
        std::string message;
        std::int32_t severity = 0;
    };

    struct RunResult
    {
        std::map<std::string, Value> properties;
        std::vector<Trace> traces;
        Value returned;
        std::size_t instructions = 0;
        // Present only for a script explicitly bound to an owning game record.
        std::optional<std::uint32_t> ownerFormId;
        bool eventExecuted = true;
        std::string currentState;
    };

    struct HostCallbacks
    {
        // The host must reject unknown records and unsupported fade requests.
        std::function<void(std::uint32_t formId, bool disabled, bool fade)> setDisabled;
        std::function<bool(std::uint32_t formId)> isDisabled;
        std::function<void(std::uint32_t formId, double seconds)> registerSingleUpdate;
        // The host validates canonical IDs and record types; these callbacks do not
        // authorize arbitrary game natives or inherited script execution.
        std::function<std::uint32_t()> getPlayer;
        std::function<bool(std::uint32_t formId, std::string_view type)> isFormType;
        std::function<std::int32_t(std::uint32_t formId)> getQuestStage;
        std::function<std::optional<float>(std::uint32_t actor, std::uint32_t value)> getActorValue;
        std::function<bool(std::uint32_t formId, std::int32_t stage)> setQuestStage;
        struct StageRequest
        {
            bool accepted = false;
            std::shared_ptr<const bool> finished;
        };
        std::function<StageRequest(std::uint32_t formId, std::int32_t stage)> setQuestStageLatent;
        std::optional<AliasReference> selfAlias;
        std::function<bool(std::uint32_t formId, std::int32_t stage)> isQuestStageDone;
        std::function<void(std::uint32_t formId, float value)> setGlobalValue;
        std::function<std::uint32_t(std::uint32_t aliasReference)> getOwningQuest;
        std::function<std::uint32_t(std::uint32_t reference)> getEditorCell;
        std::function<std::uint32_t(std::uint32_t reference)> getCurrentCell;
        std::function<std::optional<Value>(
            std::uint32_t formId, std::string_view method, const std::vector<Value>& arguments)>
            callFormMethod;
        struct ScriptCall
        {
            Value returned;
            std::shared_ptr<const bool> finished;
            std::shared_ptr<const Value> deferredReturn;
        };
        std::function<std::optional<ScriptCall>(std::uint32_t formId, std::string_view method,
            const std::vector<Value>& arguments)> callLatentFormMethod;
        std::function<std::optional<ScriptCall>(std::uint32_t formId, std::string_view declaredType,
            std::string_view method, const std::vector<Value>& arguments)>
            callFormScriptMethod;
        // Return nullopt for an unsupported operation and an engaged None for a successful void call.
        std::function<std::optional<Value>(
            std::string_view script, std::string_view method, const std::vector<Value>& arguments)>
            callStatic;
        std::function<std::optional<Value>(
            const AliasReference& alias, std::string_view method, const std::vector<Value>& arguments)>
            callAliasMethod;
        std::function<std::optional<Value>(std::uint32_t formId, std::string_view name)> getFormProperty;
        std::function<bool(std::uint32_t formId, std::string_view name, const Value& value)> setFormProperty;
        // Optional diagnostic mode. A missing host method reports the stack and returns the VM type's default.
        std::function<void(std::string_view)> reportMissingNative;
    };

    inline constexpr double MaxScheduledDelaySeconds = 86400.0;

    struct StepResult
    {
        // Exactly one member is set: an event completed or yielded at Utility.Wait.
        std::optional<RunResult> completed;
        std::optional<double> waitSeconds;
        std::optional<std::uint64_t> continuation;
        // Drained at a yield; completion contains only traces emitted since the last yield.
        std::vector<Trace> waitTraces;
        std::shared_ptr<const bool> nativeWait;
    };

    // One persistent instance, retaining fields between bounded events. A failed
    // initialization or dispatch makes the instance terminal; side effects are not rolled back.
    class ScriptInstance
    {
        struct Impl;
        std::unique_ptr<Impl> mImpl;

    public:
        ScriptInstance(std::shared_ptr<const Script> script, std::uint32_t owner,
            const std::map<std::string, Value>& properties, HostCallbacks host = {});
        ~ScriptInstance();
        ScriptInstance(ScriptInstance&&) noexcept;
        ScriptInstance& operator=(ScriptInstance&&) noexcept;
        ScriptInstance(const ScriptInstance&) = delete;
        ScriptInstance& operator=(const ScriptInstance&) = delete;

        // Synchronous methods reject a positive Utility.Wait; use start/resume for latent events.
        // Missing OnInit is valid: returns a snapshot with eventExecuted=false.
        RunResult initialize(std::size_t instructionLimit = 100000);
        // Scene events without a handler are also no-ops with eventExecuted=false.
        RunResult dispatch(
            std::string_view event, const std::vector<Value>& arguments = {}, std::size_t instructionLimit = 100000);
        StepResult startInitialization(std::size_t instructionLimit = 100000);
        StepResult startEvent(
            std::string_view event, const std::vector<Value>& arguments = {}, std::size_t instructionLimit = 100000);
        // Regular events may interleave with suspended events. OnInit remains a barrier.
        // The host resumes a continuation after its wait deadline (up to 16 suspended stacks).
        StepResult resume(std::uint64_t continuation);
        // Convenience for callers with exactly one suspended continuation.
        StepResult resume();
        // Access another initialized instance's auto property through the host.
        Value readAutoProperty(std::string_view name) const;
        void writeAutoProperty(std::string_view name, const Value& value);
        void checkpoint(Checkpoint& archive);
        bool hasFunction(std::string_view name) const;
        std::string currentState() const;
        bool hasAutoProperty(std::string_view name) const;
    };

    RunResult runEvent(const Script& script, std::string_view event, std::size_t instructionLimit);
    // Only scalar auto properties can be initialized. The host authorizes access to other objects.
    RunResult runBoundEvent(const Script& script, std::string_view event, std::size_t instructionLimit,
        std::uint32_t owner, const std::map<std::string, Value>& properties);
}

#endif
