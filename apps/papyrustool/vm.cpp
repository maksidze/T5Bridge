#include "vm.hpp"
#include "checkpoint.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Papyrus
{
    void checkpoint(Checkpoint& ar, Identifier& value) { ar(value.name); }
    void checkpoint(Checkpoint& ar, FormReference& value) { ar(value.formId); }
    void checkpoint(Checkpoint& ar, AliasReference& value) { ar(value.questFormId, value.aliasId, value.kind); }
    void checkpoint(Checkpoint& ar, Trace& value) { ar(value.message, value.severity); }
    void checkpoint(Checkpoint& ar, RunResult& value)
    { ar(value.properties, value.traces, value.returned, value.instructions, value.ownerFormId, value.eventExecuted, value.currentState); }

    namespace
    {
        constexpr std::size_t maxCallDepth = 64;
        constexpr std::size_t maxStringBytes = 1024 * 1024;
        constexpr std::size_t maxTraceBytes = 8 * maxStringBytes;
        constexpr std::size_t maxTraces = 10000;
        constexpr std::size_t maxStringWork = 64 * maxStringBytes;

        std::string key(std::string_view value)
        {
            std::string result(value);
            for (auto& c : result)
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            return result;
        }

        bool isAliasType(std::string_view normalized)
        {
            return normalized == "alias" || normalized == "referencealias" || normalized == "locationalias";
        }

        bool matchesAliasType(const AliasReference& alias, std::string_view normalized)
        {
            return alias.questFormId != 0
                && (normalized == "alias" || (alias.kind == AliasKind::Reference && normalized == "referencealias")
                    || (alias.kind == AliasKind::Location && normalized == "locationalias"));
        }

        template <class T>
        const T& require(const Value& value, const char* description)
        {
            if (const auto* result = std::get_if<T>(&value))
                return *result;
            throw std::runtime_error(std::string("expected ") + description);
        }

        std::int32_t checkedInt(std::int64_t value)
        {
            if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
                throw std::runtime_error("integer overflow is unsupported");
            return static_cast<std::int32_t>(value);
        }

        float checkedFloat(float value)
        {
            if (!std::isfinite(value))
                throw std::runtime_error("non-finite floating-point value is unsupported");
            return value;
        }

        void checkString(const std::string& value)
        {
            if (value.size() > maxStringBytes)
                throw std::runtime_error("string exceeds the VM byte limit");
        }

        bool truth(const Value& value)
        {
            if (const auto* number = std::get_if<std::int32_t>(&value))
                return *number != 0;
            if (const auto* number = std::get_if<float>(&value))
                return checkedFloat(*number) != 0;
            if (const auto* boolean = std::get_if<bool>(&value))
                return *boolean;
            if (const auto* string = std::get_if<std::string>(&value))
                return !string->empty();
            if (const auto* reference = std::get_if<FormReference>(&value))
                return reference->formId != 0;
            if (const auto* alias = std::get_if<AliasReference>(&value))
                return alias->questFormId != 0;
            return std::holds_alternative<Identifier>(value);
        }

        std::string stringValue(const Value& value)
        {
            if (const auto* string = std::get_if<std::string>(&value))
                return *string;
            if (const auto* number = std::get_if<std::int32_t>(&value))
                return std::to_string(*number);
            if (const auto* number = std::get_if<float>(&value))
            {
                std::ostringstream stream;
                stream.imbue(std::locale::classic());
                stream << std::fixed << std::setprecision(6) << checkedFloat(*number);
                return stream.str();
            }
            if (const auto* boolean = std::get_if<bool>(&value))
                return *boolean ? "True" : "False";
            if (std::holds_alternative<std::monostate>(value))
                return "None";
            if (const auto* reference = std::get_if<FormReference>(&value))
            {
                if (reference->formId == 0)
                    return "None";
                std::ostringstream stream;
                stream << "[0x" << std::hex << std::setw(8) << std::setfill('0') << reference->formId << ']';
                return stream.str();
            }
            throw std::runtime_error("object-to-string casts are unsupported");
        }

        Value defaultValue(std::string_view type)
        {
            const auto normalized = key(type);
            if (normalized == "int")
                return std::int32_t{ 0 };
            if (normalized == "float")
                return 0.f;
            if (normalized == "bool")
                return false;
            if (normalized == "string")
                return std::string{};
            return std::monostate{};
        }

        struct Binding
        {
            std::string type;
            Value value;
            void checkpoint(Checkpoint& ar) { ar(type, value); }
        };
        using Bindings = std::map<std::string, Binding>;

        double delaySeconds(const Value& value)
        {
            const double seconds = std::holds_alternative<std::int32_t>(value)
                ? static_cast<double>(std::get<std::int32_t>(value))
                : static_cast<double>(require<float>(value, "a numeric delay in seconds"));
            if (!std::isfinite(seconds) || seconds < 0 || seconds > MaxScheduledDelaySeconds)
                throw std::runtime_error("delay must be finite and between 0 and 86400 seconds");
            return seconds;
        }

        class Interpreter
        {
            const Script& mScript;
            std::size_t mLimit;
            Bindings mFields;
            RunResult mResult;
            std::optional<std::uint32_t> mOwner;
            HostCallbacks mHost;
            std::shared_ptr<const bool> mNativeWait;
            std::string mState;
            std::size_t mTraceBytes = 0;
            std::size_t mTraceCount = 0;
            std::size_t mStringWork = 0;
            struct Frame
            {
                const Function* function;
                Bindings locals;
                std::size_t pc = 0;
                std::optional<Value> returnDestination;
            };
            std::vector<Frame> mFrames;
            bool mActive = false;
            struct DeferredResult
            {
                Value destination;
                std::shared_ptr<const Value> value;
                void checkpoint(Checkpoint& ar) { ar(destination, value); }
            };
            struct Continuation
            {
                std::vector<Frame> frames;
                RunResult result;
                std::size_t limit, traceBytes, traceCount, stringWork;
                std::optional<DeferredResult> deferred;
            };
            std::map<std::uint64_t, Continuation> mSuspended;
            std::uint64_t mNextContinuation = 1;

            StepResult suspend(double seconds, std::shared_ptr<const bool> nativeWait = {},
                std::optional<DeferredResult> deferred = {})
            {
                if (mSuspended.size() >= 16 || mNextContinuation == 0)
                    throw std::runtime_error("suspended continuation limit exceeded");
                const auto id = mNextContinuation++;
                auto traces = std::move(mResult.traces);
                mResult.traces.clear();
                mSuspended.emplace(id,
                    Continuation{
                        std::move(mFrames), std::move(mResult), mLimit, mTraceBytes, mTraceCount, mStringWork,
                        std::move(deferred) });
                mFrames.clear();
                mActive = false;
                return { std::nullopt, seconds, id, std::move(traces), std::move(nativeWait) };
            }

            void chargeString(std::size_t bytes)
            {
                if (bytes > maxStringWork - mStringWork)
                    throw std::runtime_error("cumulative string work limit exceeded");
                mStringWork += bytes;
            }

            void chargeValue(const Value& value)
            {
                if (const auto* string = std::get_if<std::string>(&value))
                {
                    checkString(*string);
                    chargeString(string->size());
                }
            }

            Value assignType(const Value& value, std::string_view type)
            {
                const auto normalized = key(type);
                if (normalized == "int")
                    return require<std::int32_t>(value, "an integer");
                if (normalized == "float")
                {
                    if (const auto* integer = std::get_if<std::int32_t>(&value))
                        return static_cast<float>(*integer);
                    return checkedFloat(require<float>(value, "a float"));
                }
                if (normalized == "bool")
                    return require<bool>(value, "a bool");
                if (normalized == "string")
                {
                    const auto& string = require<std::string>(value, "a string");
                    checkString(string);
                    chargeString(string.size());
                    return string;
                }
                if (std::holds_alternative<std::monostate>(value))
                    return value;
                if (std::holds_alternative<FormReference>(value) && normalized != "none" && !normalized.empty()
                    && !normalized.ends_with("[]") && !isAliasType(normalized))
                    return value;
                if (const auto* alias = std::get_if<AliasReference>(&value);
                    alias && matchesAliasType(*alias, normalized))
                    return value;
                if (normalized == key(mScript.name) && std::holds_alternative<Identifier>(value)
                    && key(std::get<Identifier>(value).name) == "self")
                    return value;
                throw std::runtime_error("unsupported assignment to type '" + std::string(type) + "'");
            }

            Value cast(const Value& value, std::string_view type)
            {
                const auto normalized = key(type);
                if (normalized == "string")
                {
                    if (const auto* identifier = std::get_if<Identifier>(&value);
                        identifier && key(identifier->name) == "self" && mOwner)
                    {
                        chargeString(64);
                        return stringValue(FormReference{ *mOwner });
                    }
                    if (const auto* string = std::get_if<std::string>(&value))
                        chargeString(string->size());
                    else
                        chargeString(64); // Upper bound for finite primitive formatting.
                    return stringValue(value);
                }
                if (normalized == "bool")
                    return truth(value);
                if (normalized == "int" || normalized == "float")
                {
                    double number = 0;
                    if (const auto* integer = std::get_if<std::int32_t>(&value))
                        number = *integer;
                    else if (const auto* floating = std::get_if<float>(&value))
                        number = checkedFloat(*floating);
                    else if (const auto* boolean = std::get_if<bool>(&value))
                        number = *boolean ? 1 : 0;
                    else if (const auto* string = std::get_if<std::string>(&value))
                    {
                        chargeString(string->size());
                        std::istringstream stream(*string);
                        stream.imbue(std::locale::classic());
                        stream >> number;
                        if (!stream || !(stream >> std::ws).eof() || !std::isfinite(number))
                            throw std::runtime_error("unsupported numeric string cast");
                    }
                    else if (!std::holds_alternative<std::monostate>(value))
                        throw std::runtime_error("object-to-number casts are unsupported");
                    if (normalized == "float")
                        return checkedFloat(static_cast<float>(number));
                    number = std::trunc(number);
                    if (number < std::numeric_limits<std::int32_t>::min()
                        || number > std::numeric_limits<std::int32_t>::max())
                        throw std::runtime_error("integer cast exceeds the 32-bit range");
                    return static_cast<std::int32_t>(number);
                }
                if (const auto* alias = std::get_if<AliasReference>(&value))
                    return matchesAliasType(*alias, normalized) ? value : Value{};
                if (std::holds_alternative<FormReference>(value)
                    || (std::holds_alternative<Identifier>(value) && normalized != key(mScript.name)))
                {
                    if (normalized.empty() || normalized == "none" || normalized.ends_with("[]"))
                        throw std::runtime_error("unsupported object cast target '" + std::string(type) + "'");
                    const auto id = objectId(value);
                    if (id == 0)
                        return std::monostate{};
                    if (!mHost.isFormType)
                        throw std::runtime_error("FormID object casts require game type information");
                    if (isAliasType(normalized) || !mHost.isFormType(id, type))
                        return std::monostate{};
                    return FormReference{ id };
                }
                return assignType(value, type);
            }

            std::uint32_t objectId(const Value& value) const
            {
                if (const auto* reference = std::get_if<FormReference>(&value))
                    return reference->formId;
                if (const auto* identifier = std::get_if<Identifier>(&value);
                    identifier && key(identifier->name) == "self" && mOwner)
                    return *mOwner;
                throw std::runtime_error("native or object cast requires a bound form handle");
            }

            void setState(const Value& value)
            {
                const auto& requested = require<std::string>(value, "a state name string");
                checkString(requested);
                chargeString(requested.size());
                const State* selected = nullptr;
                for (const auto& state : mScript.states)
                    if (key(state.name) == key(requested))
                    {
                        if (selected)
                            throw std::runtime_error("ambiguous state '" + requested + "'");
                        selected = &state;
                    }
                if (!selected && !requested.empty())
                    throw std::runtime_error("state does not exist in this script: '" + requested + "'");
                mState = selected ? selected->name : std::string{};
                if (const auto field = mFields.find("::state"); field != mFields.end())
                    field->second.value = mState;
            }

            Binding& binding(Bindings& locals, const std::string& name)
            {
                const auto normalized = key(name);
                if (const auto found = locals.find(normalized); found != locals.end())
                    return found->second;
                if (const auto found = mFields.find(normalized); found != mFields.end())
                    return found->second;
                throw std::runtime_error("unknown variable '" + name + "'");
            }

            Value evaluate(const Value& operand, Bindings& locals)
            {
                if (const auto* identifier = std::get_if<Identifier>(&operand))
                {
                    const auto normalized = key(identifier->name);
                    if (normalized == "self")
                        return Identifier{ "self" };
                    if (normalized == "::nonevar")
                        return std::monostate{};
                    if (normalized == "::state")
                    {
                        chargeString(mState.size());
                        return mState;
                    }
                    const auto& value = binding(locals, identifier->name).value;
                    chargeValue(value);
                    return value;
                }
                if (const auto* number = std::get_if<float>(&operand))
                    checkedFloat(*number);
                chargeValue(operand);
                return operand;
            }

            void store(const Value& operand, const Value& value, Bindings& locals)
            {
                const auto& name = require<Identifier>(operand, "a destination identifier").name;
                if (key(name) == "::nonevar")
                    return;
                if (key(name) == "::state")
                {
                    setState(value);
                    return;
                }
                if (key(name) == "self")
                    throw std::runtime_error("assignment to self is unsupported");
                auto& target = binding(locals, name);
                target.value = assignType(value, target.type);
            }

            std::string diagnostic(std::string_view function, std::size_t pc, std::string_view message) const
            {
                std::ostringstream detail;
                detail << mScript.name.substr(0, 128) << '.' << function.substr(0, 128)
                       << " at instruction " << pc << ": " << message.substr(0, 2048)
                       << " [state=" << std::quoted(mState.substr(0, 128));
                if (mOwner)
                    detail << " owner=0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << *mOwner;
                detail << " stack=";
                for (std::size_t i = 0; i < mFrames.size(); ++i)
                {
                    if (i != 0)
                        detail << " -> ";
                    detail << mFrames[i].function->name.substr(0, 64);
                }
                detail << ']';
                return detail.str();
            }

            const Function* lookupFunction(std::string_view name) const
            {
                const Function* active = nullptr;
                const Function* fallback = nullptr;
                bool foundState = mState.empty();
                for (const auto& state : mScript.states)
                {
                    const bool selected = key(state.name) == key(mState);
                    foundState = foundState || selected;
                    if (!selected && !state.name.empty())
                        continue;
                    for (const auto& function : state.functions)
                        if (key(function.name) == key(name))
                        {
                            auto& destination = selected ? active : fallback;
                            if (destination)
                                throw std::runtime_error("ambiguous function '" + std::string(name) + "'");
                            destination = &function;
                        }
                }
                if (!foundState)
                    throw std::runtime_error("current state does not exist: '" + mState + "'");
                if (active)
                    return active;
                if (fallback)
                    return fallback;
                return nullptr;
            }

            const Function& findFunction(std::string_view name) const
            {
                if (const auto* function = lookupFunction(name))
                    return *function;
                throw std::runtime_error("function not found in this instance: '" + std::string(name)
                    + "'; inherited execution is unsupported");
            }

            static bool worldNative(std::string_view name)
            {
                const auto normalized = key(name);
                return normalized == "enable" || normalized == "disable" || normalized == "isdisabled"
                    || normalized == "enablenowait" || normalized == "disablenowait"
                    || normalized == "registerforsingleupdate" || normalized == "getstage" || normalized == "setstage"
                    || normalized == "getstagedone" || normalized == "getcurrentstageid"
                    || normalized == "setcurrentstageid" || normalized == "isstagedone"
                    || normalized == "getowningquest" || normalized == "setvalue";
            }

            Value invokeWorldNative(std::string_view name, const Value& target, const std::vector<Value>& arguments)
            {
                const auto id = objectId(target);
                if (id == 0)
                    throw std::runtime_error("native requires a non-null bound form handle");
                const auto normalized = key(name);
                if (normalized == "getowningquest")
                {
                    if (!arguments.empty())
                        throw std::runtime_error("Alias.GetOwningQuest takes no arguments");
                    if (!mHost.getOwningQuest)
                        throw std::runtime_error("Alias.GetOwningQuest host callback is unavailable");
                    const auto quest = mHost.getOwningQuest(id);
                    if (quest == 0)
                        return std::monostate{};
                    return FormReference{ quest };
                }
                if (normalized == "setvalue")
                {
                    if (arguments.size() != 1)
                        throw std::runtime_error("GlobalVariable.SetValue requires one numeric argument");
                    if (!mHost.setGlobalValue)
                        throw std::runtime_error("GlobalVariable.SetValue host callback is unavailable");
                    const auto value = assignType(arguments.front(), "Float");
                    mHost.setGlobalValue(id, std::get<float>(value));
                    return std::monostate{};
                }
                if (normalized == "getstage" || normalized == "getcurrentstageid")
                {
                    if (!arguments.empty())
                        throw std::runtime_error("Quest.GetStage takes no arguments");
                    if (!mHost.getQuestStage)
                        throw std::runtime_error("Quest.GetStage host callback is unavailable");
                    return mHost.getQuestStage(id);
                }
                const bool setStage = normalized == "setstage" || normalized == "setcurrentstageid";
                if (setStage || normalized == "getstagedone" || normalized == "isstagedone")
                {
                    if (arguments.size() != 1)
                        throw std::runtime_error("Quest.SetStage/GetStageDone requires one integer stage");
                    const auto stage = require<std::int32_t>(arguments[0], "an integer quest stage");
                    if (setStage)
                    {
                        if (mHost.setQuestStageLatent)
                        {
                            auto result = mHost.setQuestStageLatent(id, stage);
                            mNativeWait = std::move(result.finished);
                            return result.accepted;
                        }
                        if (!mHost.setQuestStage)
                            throw std::runtime_error("Quest.SetStage host callback is unavailable");
                        return mHost.setQuestStage(id, stage);
                    }
                    if (!mHost.isQuestStageDone)
                        throw std::runtime_error("Quest.GetStageDone host callback is unavailable");
                    return mHost.isQuestStageDone(id, stage);
                }
                if (normalized == "registerforsingleupdate")
                {
                    if (arguments.size() != 1)
                        throw std::runtime_error("RegisterForSingleUpdate requires one delay argument");
                    const auto seconds = delaySeconds(arguments[0]);
                    if (!mHost.registerSingleUpdate)
                        throw std::runtime_error("RegisterForSingleUpdate host callback is unavailable");
                    mHost.registerSingleUpdate(id, seconds);
                    return std::monostate{};
                }
                if (normalized == "isdisabled")
                {
                    if (!arguments.empty())
                        throw std::runtime_error("ObjectReference.IsDisabled takes no arguments");
                    if (!mHost.isDisabled)
                        throw std::runtime_error("ObjectReference.IsDisabled host callback is unavailable");
                    return mHost.isDisabled(id);
                }
                if (arguments.size() > 1)
                    throw std::runtime_error("ObjectReference.Enable/Disable takes an optional bool fade argument");
                const bool fade = arguments.empty() ? false : require<bool>(arguments[0], "a bool fade argument");
                if (!mHost.setDisabled)
                    throw std::runtime_error("ObjectReference.Enable/Disable host callback is unavailable");
                mHost.setDisabled(id, normalized == "disable" || normalized == "disablenowait", fade);
                return std::monostate{};
            }

            const Property& property(std::string_view name) const
            {
                const auto normalized = key(name);
                const Property* result = nullptr;
                for (const auto& property : mScript.properties)
                    if (key(property.name) == normalized)
                    {
                        if (result)
                            throw std::runtime_error("ambiguous property '" + normalized + "'");
                        result = &property;
                    }
                if (!result)
                    throw std::runtime_error("unknown property '" + normalized + "'");
                return *result;
            }

            Binding& propertyBinding(const Property& property)
            {
                if ((property.flags & 4) == 0)
                    throw std::runtime_error("Property has no auto backing field: '" + property.name + "'");
                const auto found = mFields.find(key(property.autoVariable));
                if (found == mFields.end() || key(found->second.type) != key(property.type))
                    throw std::runtime_error("invalid auto-property backing field: '" + property.name + "'");
                return found->second;
            }

            static bool isSelf(const Value& value)
            {
                const auto* identifier = std::get_if<Identifier>(&value);
                return identifier && key(identifier->name) == "self";
            }

            static void requireSelf(const Value& value)
            {
                if (!isSelf(value))
                    throw std::runtime_error("only self object calls/properties are supported");
            }

            int compare(const Value& left, const Value& right)
            {
                const auto numeric = [](const Value& value) -> std::optional<double> {
                    if (const auto* number = std::get_if<std::int32_t>(&value))
                        return *number;
                    if (const auto* number = std::get_if<float>(&value))
                        return checkedFloat(*number);
                    return std::nullopt;
                };
                if (const auto a = numeric(left))
                {
                    const auto b = numeric(right);
                    if (!b)
                        throw std::runtime_error("comparison requires matching primitive types");
                    return *a < *b ? -1 : *a > *b ? 1 : 0;
                }
                if (const auto* a = std::get_if<std::string>(&left))
                {
                    const auto& rightString = require<std::string>(right, "a string comparison operand");
                    chargeString(a->size() + rightString.size());
                    const auto b = key(rightString);
                    const auto normalized = key(*a);
                    return normalized < b ? -1 : normalized > b ? 1 : 0;
                }
                if (const auto* a = std::get_if<bool>(&left))
                {
                    const auto b = require<bool>(right, "a bool comparison operand");
                    return *a == b ? 0 : *a ? 1 : -1;
                }
                throw std::runtime_error("ordered object/None comparisons are unsupported");
            }

            bool equalObjects(const Value& left, const Value& right) const
            {
                const auto equalReference = [&](const FormReference& reference, const Value& other) {
                    if (const auto* target = std::get_if<FormReference>(&other))
                        return reference.formId == target->formId;
                    if (std::holds_alternative<std::monostate>(other))
                        return reference.formId == 0;
                    if (const auto* identifier = std::get_if<Identifier>(&other))
                        return key(identifier->name) == "self" && mOwner && reference.formId == *mOwner;
                    return false;
                };
                if (const auto* reference = std::get_if<FormReference>(&left))
                    return equalReference(*reference, right);
                if (const auto* reference = std::get_if<FormReference>(&right))
                    return equalReference(*reference, left);
                return left == right;
            }

            void pushFrame(const Function& function, const std::vector<Value>& arguments,
                std::optional<Value> destination = std::nullopt)
            {
                if (mFrames.size() >= maxCallDepth)
                    throw std::runtime_error("call depth limit exceeded");
                if ((function.flags & 2) != 0)
                    throw std::runtime_error("native instance functions are unsupported");
                if ((function.flags & 1) != 0)
                    throw std::runtime_error("global script function execution is unsupported");
                if (arguments.size() != function.parameters.size())
                    throw std::runtime_error("function argument count mismatch");
                Frame frame{ &function, {}, 0, std::move(destination) };
                const auto add = [&](const Slot& slot, const Value& value) {
                    const auto name = key(slot.name);
                    if (name == "self" || name == "::state"
                        || !frame.locals.emplace(name, Binding{ slot.type, assignType(value, slot.type) }).second)
                        throw std::runtime_error("duplicate or reserved local/parameter '" + slot.name + "'");
                };
                for (std::size_t i = 0; i < arguments.size(); ++i)
                    add(function.parameters[i], arguments[i]);
                for (const auto& slot : function.locals)
                    add(slot, defaultValue(slot.type));
                mFrames.push_back(std::move(frame));
            }

            void returnFrame(Value value)
            {
                const auto destination = std::move(mFrames.back().returnDestination);
                mFrames.pop_back();
                if (mFrames.empty())
                    mResult.returned = std::move(value);
                else
                {
                    if (!destination)
                        throw std::runtime_error("nested call has no return destination");
                    store(*destination, value, mFrames.back().locals);
                }
            }

            StepResult drive()
            {
                while (!mFrames.empty())
                {
                    auto& frame = mFrames.back();
                    const auto& function = *frame.function;
                    auto& pc = frame.pc;
                    auto& locals = frame.locals;
                    // Copy diagnostic context before a nested push/pop can invalidate frame references.
                    const auto diagnosticPc = pc;
                    try
                    {
                        if (pc == function.instructions.size())
                        {
                            if (key(function.returnType) != "none")
                                throw std::runtime_error("non-void function reached its end without returning a value");
                            returnFrame(std::monostate{});
                            continue;
                        }
                        if (mResult.instructions >= mLimit)
                            throw std::runtime_error("instruction limit exceeded");
                        ++mResult.instructions;
                        const auto& instruction = function.instructions[pc];
                        const auto& args = instruction.operands;
                        const auto arity = [&](std::size_t count) {
                            if (args.size() != count)
                                throw std::runtime_error("instruction operand count mismatch");
                        };
                        const auto eval = [&](std::size_t index) { return evaluate(args.at(index), locals); };
                        const auto integer = [&](std::size_t index) {
                            return require<std::int32_t>(eval(index), "an integer operand");
                        };
                        const auto floating = [&](std::size_t index) {
                            return checkedFloat(require<float>(eval(index), "a float operand"));
                        };
                        const auto put = [&](Value value) { store(args.at(0), std::move(value), locals); };
                        const auto jump = [&](std::size_t index) {
                            const auto offset = require<std::int32_t>(args.at(index), "a literal jump offset");
                            const auto target = static_cast<std::int64_t>(pc) + offset;
                            if (target < 0 || static_cast<std::uint64_t>(target) > function.instructions.size())
                                throw std::runtime_error("jump target is outside the function");
                            pc = static_cast<std::size_t>(target);
                        };
                        switch (instruction.opcode)
                        {
                            case Opcode::Nop:
                                arity(0);
                                break;
                            case Opcode::IAdd:
                            case Opcode::ISub:
                            case Opcode::IMul:
                            case Opcode::IDiv:
                            case Opcode::IMod:
                            {
                                arity(3);
                                const std::int64_t a = integer(1), b = integer(2);
                                if ((instruction.opcode == Opcode::IDiv || instruction.opcode == Opcode::IMod)
                                    && b == 0)
                                    throw std::runtime_error("integer division by zero");
                                const auto value = instruction.opcode == Opcode::IAdd ? a + b
                                    : instruction.opcode == Opcode::ISub              ? a - b
                                    : instruction.opcode == Opcode::IMul              ? a * b
                                    : instruction.opcode == Opcode::IDiv              ? a / b
                                                                                      : a % b;
                                put(checkedInt(value));
                                break;
                            }
                            case Opcode::FAdd:
                            case Opcode::FSub:
                            case Opcode::FMul:
                            case Opcode::FDiv:
                            {
                                arity(3);
                                const auto a = floating(1), b = floating(2);
                                if (instruction.opcode == Opcode::FDiv && b == 0)
                                    throw std::runtime_error("float division by zero");
                                put(checkedFloat(instruction.opcode == Opcode::FAdd ? a + b
                                        : instruction.opcode == Opcode::FSub        ? a - b
                                        : instruction.opcode == Opcode::FMul        ? a * b
                                                                                    : a / b));
                                break;
                            }
                            case Opcode::INeg:
                                arity(2);
                                put(checkedInt(-static_cast<std::int64_t>(integer(1))));
                                break;
                            case Opcode::FNeg:
                                arity(2);
                                put(checkedFloat(-floating(1)));
                                break;
                            case Opcode::Not:
                                arity(2);
                                put(!truth(eval(1)));
                                break;
                            case Opcode::Assign:
                                arity(2);
                                put(eval(1));
                                break;
                            case Opcode::Cast:
                            {
                                arity(2);
                                const auto& name = require<Identifier>(args[0], "a cast destination").name;
                                put(cast(eval(1), binding(locals, name).type));
                                break;
                            }
                            case Opcode::CmpEq:
                            case Opcode::CmpLt:
                            case Opcode::CmpLe:
                            case Opcode::CmpGt:
                            case Opcode::CmpGe:
                            {
                                arity(3);
                                const auto a = eval(1), b = eval(2);
                                const bool object = std::holds_alternative<std::monostate>(a)
                                    || std::holds_alternative<std::monostate>(b)
                                    || std::holds_alternative<Identifier>(a) || std::holds_alternative<Identifier>(b)
                                    || std::holds_alternative<FormReference>(a)
                                    || std::holds_alternative<FormReference>(b)
                                    || std::holds_alternative<AliasReference>(a)
                                    || std::holds_alternative<AliasReference>(b);
                                if (object && instruction.opcode == Opcode::CmpEq)
                                    put(equalObjects(a, b));
                                else
                                {
                                    const auto result = compare(a, b);
                                    put(instruction.opcode == Opcode::CmpEq       ? result == 0
                                            : instruction.opcode == Opcode::CmpLt ? result < 0
                                            : instruction.opcode == Opcode::CmpLe ? result <= 0
                                            : instruction.opcode == Opcode::CmpGt ? result > 0
                                                                                  : result >= 0);
                                }
                                break;
                            }
                            case Opcode::Jump:
                                arity(1);
                                jump(0);
                                continue;
                            case Opcode::JumpTrue:
                            case Opcode::JumpFalse:
                                arity(2);
                                if (truth(eval(0)) == (instruction.opcode == Opcode::JumpTrue))
                                {
                                    jump(1);
                                    continue;
                                }
                                break;
                            case Opcode::CallMethod:
                            case Opcode::CallStatic:
                            {
                                if (args.size() < 4)
                                    throw std::runtime_error("call instruction lacks operands");
                                const auto count = require<std::int32_t>(args[3], "a literal argument count");
                                if (count < 0 || static_cast<std::size_t>(count) != args.size() - 4)
                                    throw std::runtime_error("call instruction argument count mismatch");
                                std::vector<Value> parameters;
                                for (std::size_t i = 4; i < args.size(); ++i)
                                    parameters.push_back(eval(i));
                                Value returned;
                                const auto defaultResult = [&]() -> Value {
                                    const auto& dest = require<Identifier>(args[2], "a destination identifier").name;
                                    return key(dest) == "::nonevar" ? Value{} : defaultValue(binding(locals, dest).type);
                                };
                                const auto missing = [&](std::string message) -> Value {
                                    if (!mHost.reportMissingNative)
                                        throw std::runtime_error(std::move(message));
                                    mHost.reportMissingNative(diagnostic(function.name, diagnosticPc, message));
                                    return defaultResult();
                                };
                                if (instruction.opcode == Opcode::CallMethod)
                                {
                                    const auto target = eval(1);
                                    const auto& name = require<Identifier>(args[0], "a function identifier").name;
                                    const auto* targetForm = std::get_if<FormReference>(&target);
                                    if (std::holds_alternative<std::monostate>(target) || (targetForm && !targetForm->formId))
                                    {
                                        // A None receiver aborts the call, leaving the containing function running.
                                        store(args[2], defaultResult(), locals);
                                        break;
                                    }
                                    const bool self = isSelf(target);
                                    const auto* callee = self ? lookupFunction(name) : nullptr;
                                    if (!callee && mHost.callLatentFormMethod)
                                    {
                                        const auto* reference = std::get_if<FormReference>(&target);
                                        const auto id = reference ? reference->formId
                                            : self && !mHost.selfAlias ? mOwner.value_or(0) : 0;
                                        if (id)
                                            if (auto call = mHost.callLatentFormMethod(id, name, parameters))
                                            {
                                                if (call->finished && !*call->finished)
                                                {
                                                    ++pc;
                                                    return suspend(0, std::move(call->finished),
                                                        DeferredResult{args[2], std::move(call->deferredReturn)});
                                                }
                                                store(args[2], std::move(call->returned), locals);
                                                break;
                                            }
                                    }
                                    if (const auto* reference = std::get_if<FormReference>(&target);
                                        reference && reference->formId && mHost.callFormScriptMethod)
                                    {
                                        const auto* operand = std::get_if<Identifier>(&args[1]);
                                        const auto type = operand ? std::string_view(binding(locals, operand->name).type)
                                                                  : std::string_view{};
                                        if (auto call = mHost.callFormScriptMethod(reference->formId, type, name, parameters))
                                        {
                                            if (call->finished && !*call->finished)
                                            {
                                                ++pc;
                                                return suspend(0, std::move(call->finished),
                                                    DeferredResult{ args[2], std::move(call->deferredReturn) });
                                            }
                                            store(args[2], call->returned, locals);
                                            break;
                                        }
                                    }
                                    if (callee && (callee->flags & 2) == 0)
                                    {
                                        const auto destination = args[2];
                                        ++pc;
                                        pushFrame(*callee, parameters, destination);
                                        continue;
                                    }
                                    else if (self && !callee
                                        && (key(name) == "onendstate" || key(name) == "onbeginstate"))
                                    {
                                        if (!parameters.empty())
                                            throw std::runtime_error("state lifecycle events take no arguments");
                                        returned = std::monostate{};
                                    }
                                    else if (const auto* alias = self && mHost.selfAlias ? &*mHost.selfAlias
                                                                                      : std::get_if<AliasReference>(&target))
                                    {
                                        const auto result = mHost.callAliasMethod
                                            ? mHost.callAliasMethod(*alias, name, parameters)
                                            : std::nullopt;
                                        returned = result ? *result : missing("unsupported alias method: " + name
                                            + " on alias " + std::to_string(alias->aliasId));
                                    }
                                    else if (worldNative(name))
                                        returned = invokeWorldNative(name, target, parameters);
                                    else
                                    {
                                        if (const auto* reference = std::get_if<FormReference>(&target))
                                        {
                                            const auto result = mHost.callFormMethod && reference->formId != 0
                                                ? mHost.callFormMethod(reference->formId, name, parameters)
                                                : std::nullopt;
                                            returned = result ? *result
                                                              : missing("unsupported form method: " + name + " on " + stringValue(target));
                                        }
                                        else
                                        {
                                            if (self && !callee && mOwner && mHost.callFormMethod)
                                                if (auto result = mHost.callFormMethod(*mOwner, name, parameters))
                                                {
                                                    store(args[2], *result, locals);
                                                    ++pc;
                                                    continue;
                                                }
                                            requireSelf(target);
                                            if (callee && (callee->flags & 2))
                                            {
                                                store(args[2], missing("unsupported native method: " + name), locals);
                                                break;
                                            }
                                            const auto destination = args[2];
                                            ++pc;
                                            pushFrame(findFunction(name), parameters, destination);
                                            continue;
                                        }
                                    }
                                }
                                else
                                {
                                    const auto& script = require<Identifier>(args[0], "a script identifier").name;
                                    const auto& name = require<Identifier>(args[1], "a function identifier").name;
                                    if (key(script) == "game" && key(name) == "getplayer")
                                    {
                                        if (!parameters.empty())
                                            throw std::runtime_error("Game.GetPlayer takes no arguments");
                                        if (!mHost.getPlayer)
                                            throw std::runtime_error("Game.GetPlayer host callback is unavailable");
                                        const auto player = mHost.getPlayer();
                                        if (player == 0)
                                            throw std::runtime_error("Game.GetPlayer host returned a null player");
                                        store(args[2], FormReference{ player }, locals);
                                        ++pc;
                                        continue;
                                    }
                                    if (key(script) == "utility" && key(name) == "wait")
                                    {
                                        if (parameters.size() != 1)
                                            throw std::runtime_error("Utility.Wait requires one delay argument");
                                        const auto seconds = delaySeconds(parameters[0]);
                                        store(args[2], std::monostate{}, locals);
                                        ++pc;
                                        if (seconds == 0)
                                            continue;
                                        return suspend(seconds);
                                    }
                                    if (key(script) == "debug" && key(name) == "trace")
                                    {
                                        if (parameters.empty() || parameters.size() > 2)
                                            throw std::runtime_error(
                                                "Debug.Trace requires a string and optional integer severity");
                                        const auto& message
                                            = require<std::string>(parameters[0], "a Debug.Trace string");
                                        const auto severity = parameters.size() == 2
                                            ? require<std::int32_t>(parameters[1], "an integer trace severity")
                                            : 0;
                                        if (mTraceCount >= maxTraces || message.size() > maxTraceBytes - mTraceBytes)
                                            throw std::runtime_error("trace output limit exceeded");
                                        mTraceBytes += message.size();
                                        ++mTraceCount;
                                        chargeString(message.size());
                                        mResult.traces.push_back({ message, severity });
                                    }
                                    else
                                    {
                                        const auto result = mHost.callStatic
                                            ? mHost.callStatic(script, name, parameters)
                                            : std::nullopt;
                                        returned = result ? *result
                                                          : missing("unsupported static/native call: " + script + "." + name);
                                    }
                                }
                                store(args[2], std::move(returned), locals);
                                if (auto wait = std::exchange(mNativeWait, {}); wait && !*wait)
                                {
                                    ++pc;
                                    return suspend(0, std::move(wait));
                                }
                                break;
                            }
                            case Opcode::Return:
                                arity(1);
                                returnFrame(assignType(eval(0), function.returnType));
                                continue;
                            case Opcode::StrCat:
                            {
                                arity(3);
                                const auto aValue = eval(1), bValue = eval(2);
                                const auto& a = require<std::string>(aValue, "a string concatenation operand");
                                const auto& b = require<std::string>(bValue, "a string concatenation operand");
                                if (b.size() > maxStringBytes - a.size())
                                    throw std::runtime_error("string concatenation exceeds the VM byte limit");
                                chargeString(a.size() + b.size());
                                put(a + b);
                                break;
                            }
                            case Opcode::PropGet:
                            case Opcode::PropSet:
                            {
                                arity(3);
                                const auto target = eval(1);
                                const auto& name = require<Identifier>(args[0], "a property identifier").name;
                                if (isSelf(target))
                                {
                                    const auto& prop = property(name);
                                    if (!(prop.flags & 4))
                                    {
                                        const bool read = instruction.opcode == Opcode::PropGet;
                                        const auto& accessor = read ? prop.getter : prop.setter;
                                        if (!accessor)
                                            throw std::runtime_error("Missing property accessor: '" + name + "'");
                                        auto arguments = read ? std::vector<Value>{} : std::vector<Value>{eval(2)};
                                        ++pc;
                                        pushFrame(*accessor, arguments, read ? args[2] : Value{Identifier{"::nonevar"}});
                                        continue;
                                    }
                                    if (instruction.opcode == Opcode::PropGet)
                                        store(args[2], readAutoProperty(name), locals);
                                    else
                                        writeAutoProperty(name, eval(2));
                                }
                                else
                                {
                                    const auto id = objectId(target);
                                    if (id == 0)
                                        throw std::runtime_error("property access requires a non-null form handle");
                                    if (instruction.opcode == Opcode::PropGet)
                                    {
                                        const auto value
                                            = mHost.getFormProperty ? mHost.getFormProperty(id, name) : std::nullopt;
                                        if (!value)
                                            throw std::runtime_error("unsupported form property read: " + name);
                                        store(args[2], *value, locals);
                                    }
                                    else if (!mHost.setFormProperty || !mHost.setFormProperty(id, name, eval(2)))
                                        throw std::runtime_error("unsupported form property write: " + name);
                                }
                                break;
                            }
                            default:
                                throw std::runtime_error("unsupported opcode "
                                    + std::to_string(static_cast<unsigned int>(instruction.opcode)));
                        }
                        ++pc;
                    }
                    catch (const std::exception& error)
                    {
                        throw std::runtime_error(diagnostic(function.name, diagnosticPc, error.what()));
                    }
                }
                mActive = false;
                return { finishResult(), std::nullopt, std::nullopt, {} };
            }

        public:
            void checkpoint(Checkpoint& ar)
            {
                if (mActive) throw std::runtime_error("Cannot save an executing Papyrus stack");
                // Validate the original bytecode before rebinding frame function indices.
                std::vector<const Function*> functions;
                Checkpoint signature;
                auto scriptName = mScript.name; auto parent = mScript.parent; auto stateName = mScript.autoState;
                signature(scriptName, parent, stateName);
                for (const auto& variable : mScript.variables)
                { auto name = variable.slot.name; auto type = variable.slot.type; auto value = variable.initial; signature(name, type, value); }
                for (const auto& property : mScript.properties)
                { auto name = property.name; auto type = property.type; auto field = property.autoVariable; signature(name, type, field); }
                for (const auto& state : mScript.states)
                    for (const auto& function : state.functions) functions.push_back(&function);
                for (const auto& property : mScript.properties)
                { if (property.getter) functions.push_back(&*property.getter); if (property.setter) functions.push_back(&*property.setter); }
                for (const auto* function : functions)
                {
                    auto name = function->name; auto result = function->returnType; auto flags = function->flags; signature(name, result, flags);
                    for (const auto& slot : function->parameters) { auto name = slot.name; auto type = slot.type; signature(name, type); }
                    for (const auto& slot : function->locals) { auto name = slot.name; auto type = slot.type; signature(name, type); }
                    for (const auto& instruction : function->instructions)
                    { auto op = instruction.opcode; auto operands = instruction.operands; signature(op, operands); }
                }
                std::uint64_t fingerprint = 14695981039346656037ull;
                for (unsigned char byte : signature.bytes()) { fingerprint ^= byte; fingerprint *= 1099511628211ull; }
                auto expected = fingerprint; ar(expected);
                if (expected != fingerprint) throw std::runtime_error("Saved Papyrus bytecode differs from mounted PEX");
                const auto owner = mOwner;
                ar(mLimit, mFields, mResult, mOwner, mNativeWait, mState, mTraceBytes, mTraceCount, mStringWork, mNextContinuation);
                if (mOwner != owner) throw std::runtime_error("Snapshot VM owner mismatch");
                const auto frames = [&](std::vector<Frame>& list) {
                    if (ar.loading) list.clear();
                    ar.count(list.size(), [&](auto i) {
                        std::uint32_t function = 0;
                        if (!ar.loading)
                        {
                            auto it = std::find(functions.begin(), functions.end(), list[i].function);
                            if (it == functions.end()) throw std::runtime_error("Unknown saved Papyrus function");
                            function = static_cast<std::uint32_t>(it - functions.begin());
                        }
                        ar(function);
                        if (function >= functions.size()) throw std::runtime_error("Invalid Papyrus frame function");
                        if (ar.loading) list.push_back({functions[function], {}});
                        auto& frame = list[i]; ar(frame.locals, frame.pc, frame.returnDestination);
                        if (frame.pc > frame.function->instructions.size()) throw std::runtime_error("Invalid Papyrus instruction pointer");
                    });
                };
                frames(mFrames);
                auto it = mSuspended.begin();
                if (ar.loading) mSuspended.clear();
                ar.count(mSuspended.size(), [&](auto) {
                    std::uint64_t id = ar.loading ? 0 : it->first; ar(id);
                    if (ar.loading) mSuspended.emplace(id, Continuation{});
                    auto& continuation = ar.loading ? mSuspended.at(id) : it++->second;
                    frames(continuation.frames);
                    ar(continuation.result, continuation.limit, continuation.traceBytes, continuation.traceCount,
                        continuation.stringWork, continuation.deferred);
                });
                if (mSuspended.size() > 16) throw std::runtime_error("Papyrus continuation limit");
            }

            Interpreter(const Script& script, std::size_t limit, HostCallbacks host = {})
                : mScript(script)
                , mLimit(limit)
                , mHost(std::move(host))
                , mState(script.autoState)
            {
                if (limit == 0)
                    throw std::runtime_error("instruction limit must be positive");
                for (const auto& variable : script.variables)
                {
                    const auto name = key(variable.slot.name);
                    if (name == "self" || name == "::nonevar")
                        throw std::runtime_error("reserved field name '" + variable.slot.name + "'");
                    if (name == "::state" && key(variable.slot.type) != "string")
                        throw std::runtime_error("compiler state field must have string type");
                    if (std::holds_alternative<Identifier>(variable.initial))
                        throw std::runtime_error(
                            "identifier field initializers are unsupported: '" + variable.slot.name + "'");
                    auto value = std::holds_alternative<std::monostate>(variable.initial)
                        ? defaultValue(variable.slot.type)
                        : assignType(variable.initial, variable.slot.type);
                    if (!mFields.emplace(name, Binding{ variable.slot.type, std::move(value) }).second)
                        throw std::runtime_error("duplicate field '" + variable.slot.name + "'");
                }
                setState(mState);
            }

            void bind(std::uint32_t owner, const std::map<std::string, Value>& properties)
            {
                if (owner == 0)
                    throw std::runtime_error("bound script owner must be a non-null FormID");
                std::set<std::string> declarations;
                for (const auto& prop : mScript.properties)
                {
                    if (!declarations.insert(key(prop.name)).second)
                        throw std::runtime_error("duplicate property '" + prop.name + "'");
                    if ((prop.flags & 4) != 0)
                        propertyBinding(prop);
                }
                std::set<std::string> names;
                std::vector<std::pair<Binding*, Value>> pending;
                std::vector<std::pair<const Function*, Value>> setters;
                for (const auto& [name, value] : properties)
                {
                    if (!names.insert(key(name)).second)
                        throw std::runtime_error("duplicate bound property '" + name + "'");
                    const auto& prop = property(name);
                    const auto type = key(prop.type);
                    if (type.ends_with("[]"))
                        throw std::runtime_error("array property binding is unsupported: '" + name + "'");
                    const bool matches = type == "int" ? std::holds_alternative<std::int32_t>(value)
                        : type == "float"              ? std::holds_alternative<float>(value)
                        : type == "bool"               ? std::holds_alternative<bool>(value)
                        : type == "string"             ? std::holds_alternative<std::string>(value)
                        : type == "none" || type.empty()
                        ? false
                        : std::holds_alternative<FormReference>(value) || std::holds_alternative<AliasReference>(value)
                            || std::holds_alternative<std::monostate>(value);
                    if (!matches)
                        throw std::runtime_error(
                            "bound property type mismatch for '" + name + "': expected " + prop.type);
                    if (prop.flags & 4)
                        pending.emplace_back(&propertyBinding(prop), assignType(value, prop.type));
                    else if (prop.setter)
                        setters.emplace_back(&*prop.setter, assignType(value, prop.type));
                    else
                        throw std::runtime_error("Bound property has no setter: '" + name + "'");
                }
                // Validate the entire binding before changing any field or invoking an event.
                for (auto& [target, value] : pending)
                    target->value = std::move(value);
                mOwner = owner;
                mResult.ownerFormId = owner;
                for (const auto& [setter, value] : setters)
                {
                    mActive = true;
                    pushFrame(*setter, {value});
                    if (!drive().completed)
                        throw std::runtime_error("VMAD property setter cannot suspend during binding");
                    mResult = {};
                }
            }

            bool active() const { return mActive; }
            const std::string& currentState() const { return mState; }

            void abortSegment()
            {
                mActive = false;
                mFrames.clear();
            }

            bool hasFunction(std::string_view name) const
            {
                const auto* function = lookupFunction(name);
                return function && !(function->flags & 2);
            }

            bool hasAutoProperty(std::string_view name) const
            {
                return std::ranges::any_of(mScript.properties,
                    [&](const auto& prop) { return (prop.flags & 4) && key(prop.name) == key(name); });
            }

            Value readAutoProperty(std::string_view name)
            {
                const auto& prop = property(name);
                if ((prop.flags & 1) == 0)
                    throw std::runtime_error("property is not readable: '" + prop.name + "'");
                return propertyBinding(prop).value;
            }

            void writeAutoProperty(std::string_view name, const Value& value)
            {
                const auto& prop = property(name);
                if ((prop.flags & 2) == 0)
                    throw std::runtime_error("property is not writable: '" + prop.name + "'");
                propertyBinding(prop).value = assignType(value, prop.type);
            }

            StepResult start(std::string_view event, const std::vector<Value>& arguments = {},
                std::optional<std::size_t> instructionLimit = std::nullopt, bool allowMissing = false)
            {
                if (mActive)
                    throw std::runtime_error("an event segment is already executing");
                if (instructionLimit)
                    mLimit = *instructionLimit;
                if (mLimit == 0)
                    throw std::runtime_error("instruction limit must be positive");
                mResult = {};
                mResult.ownerFormId = mOwner;
                mTraceBytes = 0;
                mTraceCount = 0;
                mStringWork = 0;
                const auto* function = lookupFunction(event);
                if (!function && !allowMissing)
                    findFunction(event); // Preserve the standalone missing-event diagnostic.
                mResult.eventExecuted = function != nullptr;
                mActive = true;
                if (function)
                    pushFrame(*function, arguments);
                return drive();
            }

            bool suspended(std::uint64_t id) const { return mSuspended.contains(id); }

            std::uint64_t soleContinuation() const
            {
                if (mSuspended.size() != 1)
                    throw std::runtime_error("resume requires exactly one suspended continuation or an explicit ID");
                return mSuspended.begin()->first;
            }

            StepResult resume(std::uint64_t id)
            {
                if (mActive)
                    throw std::runtime_error("an event segment is already executing");
                const auto found = mSuspended.find(id);
                if (found == mSuspended.end())
                    throw std::runtime_error("unknown or completed continuation");
                auto state = std::move(found->second);
                mSuspended.erase(found);
                mFrames = std::move(state.frames);
                mResult = std::move(state.result);
                mLimit = state.limit;
                mTraceBytes = state.traceBytes;
                mTraceCount = state.traceCount;
                mStringWork = state.stringWork;
                if (state.deferred)
                    store(state.deferred->destination, state.deferred->value ? *state.deferred->value : Value{},
                        mFrames.back().locals);
                mActive = true;
                return drive();
            }

            RunResult finishResult()
            {
                mResult.currentState = mState;
                std::set<std::string> names;
                for (const auto& prop : mScript.properties)
                {
                    if (!names.insert(key(prop.name)).second)
                        throw std::runtime_error("duplicate property '" + prop.name + "'");
                    if ((prop.flags & 4) != 0)
                    {
                        const auto& value = propertyBinding(prop).value;
                        chargeValue(value);
                        mResult.properties.emplace(prop.name, value);
                    }
                }
                return std::move(mResult);
            }

            RunResult run(std::string_view event, const std::vector<Value>& arguments = {},
                std::optional<std::size_t> instructionLimit = std::nullopt, bool allowMissing = false)
            {
                auto step = start(event, arguments, instructionLimit, allowMissing);
                if (!step.completed)
                    throw std::runtime_error("latent Utility.Wait requires the resumable event API");
                return std::move(*step.completed);
            }
        };
    }

    struct ScriptInstance::Impl
    {
        std::shared_ptr<const Script> script;
        Interpreter interpreter;
        bool initialized = false;
        bool initializationComplete = false;
        std::optional<std::uint64_t> initializationContinuation;
        bool failed = false;

        static const Script& checkedScript(const std::shared_ptr<const Script>& script)
        {
            if (!script)
                throw std::runtime_error("persistent script instance requires a script");
            return *script;
        }

        Impl(std::shared_ptr<const Script> source, std::uint32_t owner, const std::map<std::string, Value>& properties,
            HostCallbacks host)
            : script(std::move(source))
            , interpreter(checkedScript(script), 100000, std::move(host))
        {
            interpreter.bind(owner, properties);
        }

        StepResult execute(std::string_view event, const std::vector<Value>& arguments, std::size_t instructionLimit,
            bool allowMissing)
        {
            try
            {
                return interpreter.start(event, arguments, instructionLimit, allowMissing);
            }
            catch (const std::exception& error)
            {
                failed = true;
                interpreter.abortSegment();
                throw std::runtime_error(
                    "script '" + script->name.substr(0, 128) + "': " + std::string(error.what()).substr(0, 4096));
            }
        }

        StepResult resume(std::uint64_t continuation)
        {
            try
            {
                auto step = interpreter.resume(continuation);
                if (initializationContinuation == continuation)
                {
                    initializationComplete = step.completed.has_value();
                    initializationContinuation = step.continuation;
                }
                return step;
            }
            catch (const std::exception& error)
            {
                failed = true;
                interpreter.abortSegment();
                throw std::runtime_error(
                    "script '" + script->name.substr(0, 128) + "': " + std::string(error.what()).substr(0, 4096));
            }
        }

        RunResult synchronous(StepResult step)
        {
            if (!step.completed)
            {
                failed = true;
                throw std::runtime_error("latent Utility.Wait requires the resumable event API");
            }
            return std::move(*step.completed);
        }
    };

    ScriptInstance::ScriptInstance(std::shared_ptr<const Script> script, std::uint32_t owner,
        const std::map<std::string, Value>& properties, HostCallbacks host)
        : mImpl(std::make_unique<Impl>(std::move(script), owner, properties, std::move(host)))
    {
    }

    void ScriptInstance::checkpoint(Checkpoint& ar)
    {
        ar(mImpl->initialized, mImpl->initializationComplete, mImpl->initializationContinuation, mImpl->failed);
        mImpl->interpreter.checkpoint(ar);
    }

    ScriptInstance::~ScriptInstance() = default;
    ScriptInstance::ScriptInstance(ScriptInstance&&) noexcept = default;
    ScriptInstance& ScriptInstance::operator=(ScriptInstance&&) noexcept = default;

    RunResult ScriptInstance::initialize(std::size_t instructionLimit)
    {
        auto step = startInitialization(instructionLimit);
        return mImpl->synchronous(std::move(step));
    }

    StepResult ScriptInstance::startInitialization(std::size_t instructionLimit)
    {
        if (!mImpl)
            throw std::runtime_error("script instance has been moved from");
        if (mImpl->failed)
            throw std::runtime_error("script instance is terminal after a previous failure");
        if (mImpl->initialized)
            throw std::runtime_error("script instance is already initialized");
        mImpl->initialized = true;
        auto step = mImpl->execute("OnInit", {}, instructionLimit, true);
        mImpl->initializationComplete = step.completed.has_value();
        mImpl->initializationContinuation = step.continuation;
        return step;
    }

    RunResult ScriptInstance::dispatch(
        std::string_view event, const std::vector<Value>& arguments, std::size_t instructionLimit)
    {
        auto step = startEvent(event, arguments, instructionLimit);
        return mImpl->synchronous(std::move(step));
    }

    StepResult ScriptInstance::startEvent(
        std::string_view event, const std::vector<Value>& arguments, std::size_t instructionLimit)
    {
        if (!mImpl)
            throw std::runtime_error("script instance has been moved from");
        if (mImpl->failed)
            throw std::runtime_error("script instance is terminal after a previous failure");
        if (!mImpl->initializationComplete)
            throw std::runtime_error("script instance must finish initialization before dispatch");
        if (mImpl->interpreter.active())
            throw std::runtime_error("an event segment is already executing");
        if (key(event) == "oninit")
            throw std::runtime_error("OnInit is dispatched only by initialize");
        return mImpl->execute(event, arguments, instructionLimit, true);
    }

    StepResult ScriptInstance::resume(std::uint64_t continuation)
    {
        if (!mImpl)
            throw std::runtime_error("script instance has been moved from");
        if (mImpl->failed)
            throw std::runtime_error("script instance is terminal after a previous failure");
        if (mImpl->interpreter.active() || !mImpl->interpreter.suspended(continuation))
            throw std::runtime_error("continuation is not suspended or an event segment is already executing");
        return mImpl->resume(continuation);
    }

    StepResult ScriptInstance::resume()
    {
        if (!mImpl)
            throw std::runtime_error("script instance has been moved from");
        if (mImpl->failed)
            throw std::runtime_error("script instance is terminal after a previous failure");
        return resume(mImpl->interpreter.soleContinuation());
    }

    Value ScriptInstance::readAutoProperty(std::string_view name) const
    {
        if (!mImpl || mImpl->failed || mImpl->interpreter.active())
            throw std::runtime_error("auto-property access requires an idle script instance");
        return mImpl->interpreter.readAutoProperty(name);
    }

    void ScriptInstance::writeAutoProperty(std::string_view name, const Value& value)
    {
        if (!mImpl || mImpl->failed || mImpl->interpreter.active())
            throw std::runtime_error("auto-property access requires an idle script instance");
        mImpl->interpreter.writeAutoProperty(name, value);
    }

    bool ScriptInstance::hasFunction(std::string_view name) const
    {
        return mImpl && !mImpl->failed && mImpl->interpreter.hasFunction(name);
    }

    std::string ScriptInstance::currentState() const { return mImpl->interpreter.currentState(); }

    bool ScriptInstance::hasAutoProperty(std::string_view name) const
    {
        return mImpl && !mImpl->failed && mImpl->interpreter.hasAutoProperty(name);
    }

    RunResult runEvent(const Script& script, std::string_view event, std::size_t instructionLimit)
    {
        try
        {
            return Interpreter(script, instructionLimit).run(event);
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error(
                "script '" + script.name.substr(0, 128) + "': " + std::string(error.what()).substr(0, 4096));
        }
    }

    RunResult runBoundEvent(const Script& script, std::string_view event, std::size_t instructionLimit,
        std::uint32_t owner, const std::map<std::string, Value>& properties)
    {
        try
        {
            Interpreter interpreter(script, instructionLimit);
            interpreter.bind(owner, properties);
            return interpreter.run(event);
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error(
                "script '" + script.name.substr(0, 128) + "': " + std::string(error.what()).substr(0, 4096));
        }
    }
}
