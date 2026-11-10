#pragma once

#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <typeindex>
#include <variant>
#include <vector>

namespace Papyrus
{
    struct Identifier; struct FormReference; struct AliasReference; struct Trace; struct RunResult;
    class Checkpoint;
    void checkpoint(Checkpoint&, Identifier&);
    void checkpoint(Checkpoint&, FormReference&);
    void checkpoint(Checkpoint&, AliasReference&);
    void checkpoint(Checkpoint&, Trace&);
    void checkpoint(Checkpoint&, RunResult&);
    // Explicit, versioned state archive. No executable code, addresses or host callbacks
    // are persisted. Shared wait tokens keep their identity across VM and scheduler state.
    class Checkpoint
    {
        struct Shared { std::shared_ptr<void> pointer; std::type_index type{typeid(void)}; };
        std::map<const void*, std::uint32_t> saved;
        std::vector<Shared> restored;
        std::string output;
        std::string_view input;
        template<std::size_t I = 0, class... T> void variant(std::variant<T...>& value, std::uint32_t index)
        {
            if constexpr (I < sizeof...(T))
            {
                if (index == I) { if (loading) value.template emplace<I>(); (*this)(std::get<I>(value)); }
                else variant<I + 1>(value, index);
            }
            else throw std::runtime_error("Invalid checkpoint variant");
        }
    public:
        static constexpr std::size_t maxBytes = 256 * 1024 * 1024;
        const bool loading;
        Checkpoint() : loading(false) {}
        explicit Checkpoint(std::string_view bytes) : input(bytes), loading(true)
        { if (bytes.size() > maxBytes) throw std::runtime_error("Checkpoint size limit"); }
        const std::string& bytes() const { return output; }
        bool atEnd() const { return input.empty(); }
        void raw(void* pointer, std::size_t count)
        {
            if (loading)
            {
                if (count > input.size()) throw std::runtime_error("Truncated checkpoint");
                std::memcpy(pointer, input.data(), count); input.remove_prefix(count);
            }
            else
            {
                if (count > maxBytes - output.size()) throw std::runtime_error("Checkpoint size limit");
                output.append(static_cast<const char*>(pointer), count);
            }
        }
        template<class T> requires (std::is_arithmetic_v<T> || std::is_enum_v<T>)
        void item(T& value) { raw(&value, sizeof(T)); }
        void item(std::monostate&) {}
        void item(std::string& value)
        {
            std::uint32_t count = std::uint32_t(value.size()); (*this)(count);
            if (count > maxBytes || (loading && count > input.size())) throw std::runtime_error("Checkpoint string limit");
            if (loading) value.resize(count);
            raw(value.data(), count);
        }
        template<class... T> void operator()(T&... values) { (item(values), ...); }
        template<class T> requires requires(T& value, Checkpoint& ar) { value.checkpoint(ar); }
        void item(T& value) { value.checkpoint(*this); }
        template<class T> requires requires(T& value, Checkpoint& ar) { checkpoint(ar, value); }
        void item(T& value) { checkpoint(*this, value); }
        template<class... T> void item(std::variant<T...>& value)
        { auto index = std::uint32_t(value.index()); (*this)(index); variant(value, index); }
        template<class T> void item(std::optional<T>& value)
        {
            bool present = bool(value); (*this)(present);
            if (loading) { if (present) value.emplace(); else value.reset(); }
            if (present) (*this)(*value);
        }
        template<class T, std::size_t N> void item(std::array<T, N>& values) { for (auto& value : values) (*this)(value); }
        template<class T, std::size_t N> void item(T (&values)[N]) { for (auto& value : values) (*this)(value); }
        template<class A, class B> void item(std::pair<A, B>& value) { (*this)(value.first, value.second); }
        template<class... T> void item(std::tuple<T...>& value) { std::apply([&](auto&... fields) { (*this)(fields...); }, value); }
        template<class F> void count(std::size_t size, F serialize)
        {
            auto count = std::uint32_t(size); (*this)(count);
            if (count > 1000000) throw std::runtime_error("Checkpoint collection limit");
            for (std::uint32_t i = 0; i < count; ++i) serialize(i);
        }
        template<class C> void sequence(C& values)
        {
            auto count = std::uint32_t(values.size()); (*this)(count);
            if (count > 1000000) throw std::runtime_error("Checkpoint collection limit");
            if (loading) { values.clear(); values.resize(count); }
            for (auto& value : values) (*this)(value);
        }
        template<class T> void item(std::vector<T>& values) { sequence(values); }
        template<class T> void item(std::deque<T>& values) { sequence(values); }
        template<class K, class V, class C> void item(std::map<K, V, C>& values)
        {
            auto it = values.begin();
            if (loading) values.clear();
            count(values.size(), [&](auto) {
                K key{}; V value{};
                if (loading) { (*this)(key, value); if (!values.emplace(std::move(key), std::move(value)).second) throw std::runtime_error("Duplicate checkpoint key"); }
                else { key = it->first; (*this)(key, it->second); ++it; }
            });
        }
        template<class K, class C> void item(std::set<K, C>& values)
        {
            auto it = values.begin(); if (loading) values.clear();
            count(values.size(), [&](auto) { K key{}; if (!loading) key = *it++; (*this)(key); if (loading) values.insert(std::move(key)); });
        }
        template<class T, class Factory, class Serialize> void shared(std::shared_ptr<T>& value, Factory factory, Serialize serialize)
        {
            using U = std::remove_const_t<T>;
            std::uint32_t id = 0; bool fresh = false;
            if (!loading && value)
            {
                auto [it, inserted] = saved.emplace(value.get(), std::uint32_t(saved.size() + 1));
                id = it->second; fresh = inserted;
            }
            (*this)(id);
            if (!id) { if (loading) value.reset(); return; }
            if (id > 1000000) throw std::runtime_error("Checkpoint shared-object limit");
            if (loading) fresh = id == restored.size() + 1;
            if (fresh)
            {
                auto pointer = factory(value);
                if (loading) restored.push_back({std::const_pointer_cast<U>(pointer), typeid(U)});
                serialize(*std::const_pointer_cast<U>(pointer));
                value = std::move(pointer);
            }
            else if (loading)
            {
                if (id > restored.size() || restored[id - 1].type != typeid(U)) throw std::runtime_error("Invalid checkpoint object link");
                value = std::static_pointer_cast<U>(restored[id - 1].pointer);
            }
        }
        template<class T> void item(std::shared_ptr<T>& value)
        {
            shared(value, [&](auto pointer) { if (loading) pointer = std::make_shared<std::remove_const_t<T>>(); return pointer; },
                [&](auto& target) { (*this)(target); });
        }
        template<class Random> void random(Random& engine)
        {
            std::string text;
            if (!loading) { std::ostringstream stream; stream << engine; text = stream.str(); }
            (*this)(text);
            if (loading) { std::istringstream stream(text); if (!(stream >> engine)) throw std::runtime_error("Invalid saved random state"); }
        }
    };
}
