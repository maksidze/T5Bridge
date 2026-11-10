#include "pex.hpp"

#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Papyrus
{
    namespace
    {
        // Independent limits also bound expansion from tiny encoded operands to C++ objects.
        constexpr std::size_t MaxFileSize = 64 * 1024 * 1024;
        constexpr std::size_t MaxElements = 1024 * 1024;
        constexpr std::size_t MaxExpandedStringBytes = 64 * 1024 * 1024;

        class Reader
        {
        public:
            explicit Reader(std::vector<unsigned char> bytes)
                : mBytes(std::move(bytes))
                , mLimit(mBytes.size())
            {
            }

            PexFile read()
            {
                if (u32() != 0xFA57C0DE)
                    fail("expected big-endian Skyrim PEX magic FA57C0DE", 0);
                mFile.major = u8();
                mFile.minor = u8();
                mFile.game = u16();
                if (mFile.major != 3 || (mFile.minor != 1 && mFile.minor != 2) || mFile.game != 1)
                    fail("unsupported PEX version/game (expected Skyrim 3.1 or 3.2, game 1)", 4);
                mFile.compilationTime = u64();
                mFile.source = string();
                mFile.user = string();
                mFile.machine = string();
                const auto strings = count(2, "string table");
                mFile.strings.reserve(strings);
                for (std::size_t i = 0; i < strings; ++i)
                    mFile.strings.push_back(string());

                mContext = "debug information";
                const auto debug = u8();
                if (debug > 1)
                    fail("invalid debug-presence flag", mPos - 1);
                if (debug)
                {
                    mFile.debugTime = u64();
                    const auto functions = count(9, "debug functions");
                    mFile.debugFunctions.reserve(functions);
                    for (std::size_t i = 0; i < functions; ++i)
                    {
                        DebugFunction function;
                        function.script = reference();
                        function.state = reference();
                        function.name = reference();
                        function.kind = u8();
                        if (function.kind > 2)
                            fail("unknown debug function kind", mPos - 1);
                        const auto lines = count(2, "debug lines");
                        function.lines.reserve(lines);
                        for (std::size_t j = 0; j < lines; ++j)
                            function.lines.push_back(u16());
                        mFile.debugFunctions.push_back(std::move(function));
                    }
                }

                mContext = "user flags";
                const auto flags = count(3, "user flags");
                mFile.userFlags.reserve(flags);
                for (std::size_t i = 0; i < flags; ++i)
                {
                    UserFlag flag;
                    flag.name = reference();
                    flag.bit = u8();
                    if (flag.bit >= 32)
                        fail("user flag bit exceeds 32-bit flag storage", mPos - 1);
                    mFile.userFlags.push_back(std::move(flag));
                }

                mContext = "objects";
                const auto objects = count(22, "objects");
                mFile.scripts.reserve(objects);
                for (std::size_t i = 0; i < objects; ++i)
                    mFile.scripts.push_back(script());
                mContext = "file";
                if (mPos != mLimit)
                    fail("trailing bytes after object table");
                return std::move(mFile);
            }

        private:
            class Context
            {
            public:
                Context(Reader& reader, std::string text)
                    : mReader(reader)
                    , mOld(reader.mContext)
                {
                    reader.mContext += "/" + text;
                }
                ~Context() { mReader.mContext = std::move(mOld); }

            private:
                Reader& mReader;
                std::string mOld;
            };

            [[noreturn]] void fail(const std::string& reason, std::size_t offset) const
            {
                std::ostringstream message;
                message << "PEX " << mContext << " at byte " << offset << " (0x" << std::hex << offset
                        << "): " << reason;
                throw std::runtime_error(message.str());
            }

            [[noreturn]] void fail(const std::string& reason) const { fail(reason, mPos); }

            void need(std::size_t bytes) const
            {
                if (bytes > mLimit - mPos)
                    fail("truncated data or object boundary exceeded (need " + std::to_string(bytes) + " bytes)");
            }

            void elements(std::size_t amount)
            {
                if (amount > MaxElements - mElements)
                    fail("decoded element limit exceeded (1048576)");
                mElements += amount;
            }

            std::size_t count(std::size_t minimumBytes, const char* what)
            {
                const std::size_t result = u16();
                if (result > (mLimit - mPos) / minimumBytes)
                    fail(std::string(what) + " count exceeds remaining bytes", mPos - 2);
                elements(result);
                return result;
            }

            std::uint8_t u8()
            {
                need(1);
                return mBytes[mPos++];
            }

            std::uint16_t u16()
            {
                need(2);
                const auto value = static_cast<std::uint16_t>((mBytes[mPos] << 8) | mBytes[mPos + 1]);
                mPos += 2;
                return value;
            }

            std::uint32_t u32()
            {
                need(4);
                std::uint32_t value = 0;
                for (unsigned i = 0; i < 4; ++i)
                    value = (value << 8) | mBytes[mPos++];
                return value;
            }

            std::uint64_t u64()
            {
                const std::uint64_t high = u32();
                return (high << 32) | u32();
            }

            std::string string()
            {
                const auto length = u16();
                need(length);
                std::string result(reinterpret_cast<const char*>(mBytes.data() + mPos), length);
                mPos += length;
                return result;
            }

            std::string reference()
            {
                const auto index = u16();
                if (index >= mFile.strings.size())
                    fail("string-table index " + std::to_string(index) + " is out of range", mPos - 2);
                if (mFile.strings[index].size() > MaxExpandedStringBytes - mExpandedStringBytes)
                    fail("expanded string references exceed 64 MiB limit", mPos - 2);
                mExpandedStringBytes += mFile.strings[index].size();
                return mFile.strings[index];
            }

            Value value()
            {
                elements(1);
                const auto type = u8();
                switch (type)
                {
                    case 0:
                        return std::monostate{};
                    case 1:
                        return Identifier{ reference() };
                    case 2:
                        return reference();
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
                        fail("unknown value type " + std::to_string(type), mPos - 1);
                }
            }

            Instruction instruction()
            {
                // Skyrim instruction operands are all tagged Values, including the variable call count.
                constexpr std::array<unsigned char, 36> arities = { 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3,
                    3, 3, 3, 1, 2, 2, 3, 2, 3, 1, 3, 3, 3, 2, 2, 3, 3, 4, 4 };
                const auto opcode = u8();
                if (opcode >= arities.size())
                    fail("unknown Skyrim opcode " + std::to_string(opcode), mPos - 1);
                Instruction result;
                result.opcode = static_cast<Opcode>(opcode);
                need(arities[opcode]);
                result.operands.reserve(arities[opcode]);
                for (unsigned i = 0; i < arities[opcode]; ++i)
                    result.operands.push_back(value());
                if (opcode >= 23 && opcode <= 25)
                {
                    const auto countOffset = mPos;
                    auto argumentCount = value();
                    const auto* number = std::get_if<std::int32_t>(&argumentCount);
                    if (!number || *number < 0)
                        fail("call argument count must be a nonnegative integer Value", countOffset);
                    const auto arguments = static_cast<std::size_t>(*number);
                    need(arguments);
                    if (arguments > MaxElements - mElements)
                        fail("call argument count exceeds decoded element limit", countOffset);
                    result.operands.reserve(result.operands.size() + 1 + arguments);
                    result.operands.push_back(std::move(argumentCount));
                    for (std::size_t i = 0; i < arguments; ++i)
                        result.operands.push_back(value());
                }
                return result;
            }

            Slot slot() { return Slot{ reference(), reference() }; }

            Function function(std::string name)
            {
                Context context(*this, "function " + name);
                Function result;
                result.name = std::move(name);
                result.returnType = reference();
                result.documentation = reference();
                result.userFlags = u32();
                result.flags = u8();
                if (result.flags & ~3u)
                    fail("unknown function flags", mPos - 1);
                const auto parameters = count(4, "parameters");
                result.parameters.reserve(parameters);
                for (std::size_t i = 0; i < parameters; ++i)
                    result.parameters.push_back(slot());
                const auto locals = count(4, "locals");
                result.locals.reserve(locals);
                for (std::size_t i = 0; i < locals; ++i)
                    result.locals.push_back(slot());
                const auto instructions = count(1, "instructions");
                result.instructions.reserve(instructions);
                for (std::size_t i = 0; i < instructions; ++i)
                {
                    Context instructionContext(*this, "instruction " + std::to_string(i));
                    result.instructions.push_back(instruction());
                }
                return result;
            }

            Property property()
            {
                Property result;
                result.name = reference();
                Context context(*this, "property " + result.name);
                result.type = reference();
                result.documentation = reference();
                result.userFlags = u32();
                result.flags = u8();
                if (result.flags & ~7u)
                    fail("unknown property flags", mPos - 1);
                if (result.flags & 4)
                    result.autoVariable = reference();
                else
                {
                    if (result.flags & 1)
                        result.getter = function("get");
                    if (result.flags & 2)
                        result.setter = function("set");
                }
                return result;
            }

            Script script()
            {
                Script result;
                result.name = reference();
                Context context(*this, "object " + result.name);
                const auto sizeOffset = mPos;
                const auto size = u32();
                if (size < 20 || size > mLimit - sizeOffset)
                    fail("object size is too small or exceeds enclosing file", sizeOffset);
                const auto outerLimit = mLimit;
                mLimit = sizeOffset + size; // Includes the size field, excludes the preceding name reference.
                result.parent = reference();
                result.documentation = reference();
                result.userFlags = u32();
                result.autoState = reference();
                const auto variables = count(9, "variables");
                result.variables.reserve(variables);
                for (std::size_t i = 0; i < variables; ++i)
                {
                    Variable variable;
                    variable.slot = slot();
                    Context variableContext(*this, "variable " + variable.slot.name);
                    variable.userFlags = u32();
                    variable.initial = value();
                    result.variables.push_back(std::move(variable));
                }
                const auto properties = count(11, "properties");
                result.properties.reserve(properties);
                for (std::size_t i = 0; i < properties; ++i)
                    result.properties.push_back(property());
                const auto states = count(4, "states");
                result.states.reserve(states);
                for (std::size_t i = 0; i < states; ++i)
                {
                    State state;
                    state.name = reference();
                    Context stateContext(*this, "state " + state.name);
                    const auto functions = count(17, "functions");
                    state.functions.reserve(functions);
                    for (std::size_t j = 0; j < functions; ++j)
                    {
                        auto name = reference();
                        state.functions.push_back(function(std::move(name)));
                    }
                    result.states.push_back(std::move(state));
                }
                if (mPos != mLimit)
                    fail("object size does not match parsed object body");
                mLimit = outerLimit;
                return result;
            }

            std::vector<unsigned char> mBytes;
            std::size_t mPos = 0;
            std::size_t mLimit;
            std::size_t mElements = 0;
            std::size_t mExpandedStringBytes = 0;
            std::string mContext = "header";
            PexFile mFile;
        };
    }

    PexFile readPex(std::istream& input)
    {
        std::vector<unsigned char> bytes;
        std::array<char, 8192> chunk;
        while (input)
        {
            input.read(chunk.data(), chunk.size());
            const auto count = static_cast<std::size_t>(input.gcount());
            if (count > MaxFileSize - bytes.size())
                throw std::runtime_error(
                    "PEX input at byte " + std::to_string(bytes.size()) + ": exceeds 64 MiB limit");
            bytes.insert(bytes.end(), chunk.data(), chunk.data() + count);
        }
        if (!input.eof() || input.bad())
            throw std::runtime_error("PEX input at byte " + std::to_string(bytes.size()) + ": stream read failed");
        return Reader(std::move(bytes)).read();
    }
}
