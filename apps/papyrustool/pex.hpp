#ifndef OPENMW_APPS_PAPYRUSTOOL_PEX_HPP
#define OPENMW_APPS_PAPYRUSTOOL_PEX_HPP

#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Papyrus
{
    struct Identifier
    {
        std::string name;
        bool operator==(const Identifier&) const = default;
    };
    // Runtime-only canonical game FormID. This is never a tagged PEX literal.
    struct FormReference
    {
        std::uint32_t formId = 0;
        bool operator==(const FormReference&) const = default;
    };
    enum class AliasKind
    {
        Reference,
        Location
    };
    // Runtime-only quest alias identity, independent of its current fill.
    struct AliasReference
    {
        std::uint32_t questFormId = 0;
        std::uint32_t aliasId = 0;
        AliasKind kind = AliasKind::Reference;
        bool operator==(const AliasReference&) const = default;
    };
    using Value = std::variant<std::monostate, Identifier, std::string, std::int32_t, float, bool, FormReference,
        AliasReference>;

    enum class Opcode : std::uint8_t
    {
        Nop,
        IAdd,
        FAdd,
        ISub,
        FSub,
        IMul,
        FMul,
        IDiv,
        FDiv,
        IMod,
        Not,
        INeg,
        FNeg,
        Assign,
        Cast,
        CmpEq,
        CmpLt,
        CmpLe,
        CmpGt,
        CmpGe,
        Jump,
        JumpTrue,
        JumpFalse,
        CallMethod,
        CallParent,
        CallStatic,
        Return,
        StrCat,
        PropGet,
        PropSet,
        ArrayCreate,
        ArrayLength,
        ArrayGet,
        ArraySet,
        ArrayFind,
        ArrayRFind
    };

    struct Instruction
    {
        Opcode opcode;
        std::vector<Value> operands;
    };

    struct Slot
    {
        std::string name;
        std::string type;
    };

    struct Variable
    {
        Slot slot;
        std::uint32_t userFlags = 0;
        Value initial;
    };

    struct Function
    {
        std::string name;
        std::string returnType;
        std::string documentation;
        std::uint32_t userFlags = 0;
        std::uint8_t flags = 0;
        std::vector<Slot> parameters;
        std::vector<Slot> locals;
        std::vector<Instruction> instructions;
    };

    struct Property
    {
        std::string name;
        std::string type;
        std::string documentation;
        std::uint32_t userFlags = 0;
        std::uint8_t flags = 0;
        std::string autoVariable;
        std::optional<Function> getter;
        std::optional<Function> setter;
    };

    struct State
    {
        std::string name;
        std::vector<Function> functions;
    };

    struct Script
    {
        std::string name;
        std::string parent;
        std::string documentation;
        std::uint32_t userFlags = 0;
        std::string autoState;
        std::vector<Variable> variables;
        std::vector<Property> properties;
        std::vector<State> states;
    };

    struct DebugFunction
    {
        std::string script;
        std::string state;
        std::string name;
        std::uint8_t kind = 0;
        std::vector<std::uint16_t> lines;
    };

    struct UserFlag
    {
        std::string name;
        std::uint8_t bit = 0;
    };

    struct PexFile
    {
        std::uint8_t major = 0;
        std::uint8_t minor = 0;
        std::uint16_t game = 0;
        std::uint64_t compilationTime = 0;
        std::string source;
        std::string user;
        std::string machine;
        std::vector<std::string> strings;
        std::optional<std::uint64_t> debugTime;
        std::vector<DebugFunction> debugFunctions;
        std::vector<UserFlag> userFlags;
        std::vector<Script> scripts;
    };

    // Call operands include the fixed prefix (including the count), then the arguments.
    PexFile readPex(std::istream& input);
}

#endif
