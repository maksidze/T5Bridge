#include "hkxmovement.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace TES5Viewer
{
    namespace
    {
        std::string normalized(std::string value)
        {
            for (auto& c : value) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return value;
        }
        std::string resolve(const std::string& root, std::string name)
        {
            name = normalized(std::move(name));
            if (name.empty()) return {};
            if (name.starts_with("meshes/")) return name;
            if (name.starts_with("actors/")) return "meshes/" + name;
            return (std::filesystem::path(root) / name).lexically_normal().generic_string();
        }
        class Pack
        {
            struct Section { std::uint32_t data, local, global, virt, exports, end; };
            std::string bytes;
            std::map<std::uint32_t, std::uint32_t> pointers;
            std::uint32_t dataStart = 0, dataEnd = 0;
            void has(std::uint64_t offset, std::uint64_t count) const
            {
                if (offset > bytes.size() || count > bytes.size() - offset)
                    throw std::runtime_error("Truncated movement HKX packfile");
            }
            std::string text(std::uint32_t offset, std::uint32_t limit) const
            {
                has(offset, limit);
                const auto end = bytes.find('\0', offset);
                if (end == std::string::npos || end - offset >= limit)
                    throw std::runtime_error("Invalid movement HKX string");
                return bytes.substr(offset, end - offset);
            }
        public:
            std::map<std::uint32_t, std::string> objects;
            explicit Pack(std::string input) : bytes(std::move(input))
            {
                if (bytes.size() > 64 * 1024 * 1024) throw std::runtime_error("Movement HKX exceeds 64 MiB");
                if (word(0) != 0x57e0e057 || word(4) != 0x10c0c010 || word(12) != 8
                    || (word(16) & 0xffff) != 0x108 || text(40, 16) != "hk_2010.2.0-r1")
                    throw std::runtime_error("Unsupported movement HKX layout");
                const auto n = word(20);
                if (n < 3 || n > 16) throw std::runtime_error("Invalid movement HKX sections");
                std::vector<Section> sections;
                std::size_t dataIndex = n;
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    const auto h = 64 + i * 48;
                    const auto start = word(h + 20);
                    const auto l = word(h + 24), g = word(h + 28), v = word(h + 32), e = word(h + 36), end = word(h + 44);
                    if (l > g || g > v || v > e || e > end) throw std::runtime_error("Invalid movement HKX bounds");
                    has(start, end);
                    sections.push_back({start, start + l, start + g, start + v, start + e, start + end});
                    if (text(h, 16) == "__data__") dataIndex = i;
                }
                if (dataIndex == n) throw std::runtime_error("Missing movement HKX data");
                const auto& d = sections[dataIndex]; dataStart = d.data; dataEnd = d.local;
                const auto fix = [&](std::uint32_t from, std::uint32_t target, std::uint32_t to) {
                    if (target >= n || from > dataEnd - dataStart || dataEnd - dataStart - from < 8
                        || to >= sections[target].local - sections[target].data)
                        throw std::runtime_error("Invalid movement HKX pointer");
                    pointers.insert_or_assign(d.data + from, sections[target].data + to);
                };
                for (auto p = d.local; p + 8 <= d.global && word(p) != UINT32_MAX; p += 8) fix(word(p), static_cast<std::uint32_t>(dataIndex), word(p + 4));
                for (auto p = d.global; p + 12 <= d.virt && word(p) != UINT32_MAX; p += 12) fix(word(p), word(p + 4), word(p + 8));
                for (auto p = d.virt; p + 12 <= d.exports && word(p) != UINT32_MAX; p += 12)
                {
                    const auto from = word(p), target = word(p + 4), name = word(p + 8);
                    if (from >= dataEnd - dataStart || target >= n || name >= sections[target].local - sections[target].data)
                        throw std::runtime_error("Invalid movement HKX object");
                    objects.emplace(d.data + from, text(sections[target].data + name,
                        std::min(128u, sections[target].local - sections[target].data - name)));
                }
            }
            std::uint32_t word(std::uint32_t offset) const
            {
                has(offset, 4); std::uint32_t result; std::memcpy(&result, bytes.data() + offset, 4); return result;
            }
            std::uint32_t ref(std::uint32_t offset) const
            { const auto found = pointers.find(offset); return found == pointers.end() ? 0 : found->second; }
            std::string string(std::uint32_t offset) const
            { const auto p = ref(offset); return p >= dataStart && p < dataEnd ? text(p, std::min(4096u, dataEnd - p)) : std::string{}; }
            std::uint32_t find(std::string_view type) const
            { for (const auto& [p, t] : objects) if (t == type) return p; return 0; }
            std::vector<std::uint32_t> array(std::uint32_t field, std::uint32_t stride = 8, bool pointer = true) const
            {
                const auto count = word(field + 8), at = ref(field);
                if (!count) return {};
                if (count > 100000 || at < dataStart || at > dataEnd || std::uint64_t(count) * stride > dataEnd - at)
                    throw std::runtime_error("Invalid movement HKX array");
                std::vector<std::uint32_t> result;
                for (std::uint32_t i = 0; i < count; ++i) result.push_back(pointer ? ref(at + i * stride) : word(at + i * stride));
                return result;
            }
        };

        class Search
        {
            const std::function<std::string(const std::string&)>& read;
            std::map<std::string, std::unique_ptr<Pack>> files;
            std::set<std::tuple<std::string, std::uint32_t, std::string>> visited;
            std::array<std::set<std::string>, 2> candidates;
            Pack& pack(const std::string& path)
            {
                if (files.size() > 64) throw std::runtime_error("Movement HKX graph limit");
                auto& file = files[path];
                if (!file) file = std::make_unique<Pack>(read(path));
                return *file;
            }
            std::string stateName(const Pack& file, std::uint32_t state) const
            {
                const auto names = file.find("hkbBehaviorGraphStringData"), values = file.find("hkbVariableValueSet");
                if (!names || !values) return {};
                const auto namesAt = file.ref(names + 48);
                const auto words = file.array(values + 16, 4, false);
                const auto count = file.word(names + 56);
                if (count != words.size()) throw std::runtime_error("Movement HKX variable count mismatch");
                std::set<std::string> matches;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const auto name = file.string(namesAt + i * 8);
                    if (words[i] == state && name.starts_with("iState_")) matches.insert(normalized(name.substr(7)));
                }
                return matches.size() == 1 ? *matches.begin() : std::string{};
            }
            void graph(const std::string& path, const std::string& inherited, unsigned depth)
            {
                auto& file = pack(path); const auto root = file.find("hkbBehaviorGraph");
                if (root) node(path, file.ref(root + 128), inherited, depth + 1);
            }
            void node(const std::string& path, std::uint32_t at, std::string state, unsigned depth)
            {
                if (!at || depth > 256 || visited.size() > 100000) return;
                if (!visited.emplace(path, at, state).second) return;
                auto& file = pack(path);
                const auto found = file.objects.find(at);
                if (found == file.objects.end()) return;
                const auto& type = found->second;
                if (type == "BSiStateTaggingGenerator")
                { state = stateName(file, file.word(at + 88)); node(path, file.ref(at + 80), state, depth + 1); }
                else if (type == "hkbClipGenerator")
                {
                    const auto clipPath = normalized(file.string(at + 72));
                    // Human graphs also contain the horse rider's walk/run clips.
                    // A mounted actor's route mover is the mount; these clips must
                    // never choose HorseDefault for the dismounted human actor.
                    if (clipPath.find("/horse_rider/") != std::string::npos) return;
                    const auto clip = std::filesystem::path(clipPath).stem().string();
                    if (!state.empty())
                        for (std::size_t i = 0; i < 2; ++i)
                            if (clip == (i == 0 ? "walkforward" : "runforward") || clip == (i == 0 ? "mt_walkforward" : "mt_runforward"))
                                candidates[i].insert(state);
                }
                else if (type == "hkbBehaviorReferenceGenerator")
                    graph(resolve(std::filesystem::path(path).parent_path().parent_path().generic_string(), file.string(at + 72)), state, depth + 1);
                else if (type == "hkbModifierGenerator" || type == "BSOffsetAnimationGenerator")
                    node(path, file.ref(at + 80), state, depth + 1);
                else if (type == "hkbStateMachine")
                    for (const auto child : file.array(at + 144)) node(path, file.ref(child + 88), state, depth + 1);
                else if (type == "hkbBlenderGenerator")
                    for (const auto child : file.array(at + 96)) node(path, file.ref(child + 48), state, depth + 1);
                else if (type == "hkbManualSelectorGenerator")
                    for (const auto child : file.array(at + 72)) node(path, child, state, depth + 1);
            }
        public:
            explicit Search(const std::function<std::string(const std::string&)>& source) : read(source) {}
            std::string characterResource(const std::string& project, std::uint32_t offset)
            {
                auto& p = pack(project); const auto ps = p.find("hkbProjectStringData");
                if (!ps || p.word(ps + 56) != 1) return {};
                const auto character = resolve(std::filesystem::path(project).parent_path().generic_string(), p.string(p.ref(ps + 48)));
                auto& c = pack(character); const auto cs = c.find("hkbCharacterStringData");
                if (!cs) return {};
                return resolve(std::filesystem::path(character).parent_path().parent_path().generic_string(), c.string(cs + offset));
            }
            std::string behavior(const std::string& project) { return characterResource(project, 184); }
            std::array<std::string, 2> run(const std::string& project)
            {
                const auto path = behavior(project);
                if (path.empty()) return {};
                graph(path, {}, 0);
                std::array<std::string, 2> result;
                for (std::size_t i = 0; i < 2; ++i) if (candidates[i].size() == 1) result[i] = *candidates[i].begin();
                return result;
            }
        };
    }
    std::array<std::string, 2> hkxLocomotionTypes(const std::string& project,
        const std::function<std::string(const std::string&)>& read)
    { return Search(read).run(normalized(project)); }
    std::string hkxBehaviorPath(const std::string& project, const std::function<std::string(const std::string&)>& read)
    { return Search(read).behavior(normalized(project)); }
    std::string hkxObjectRig(const std::string& project, const std::function<std::string(const std::string&)>& read)
    { return Search(read).characterResource(normalized(project), 168); }
    std::vector<std::string> hkxObjectBones(std::string bytes)
    {
        const Pack rig(std::move(bytes));
        std::vector<std::string> result;
        if (const auto skeleton = rig.find("hkaSkeleton"))
        {
            const auto bones = rig.array(skeleton + 40, 16);
            for (std::uint32_t i = 0; i < bones.size(); ++i)
                if (bones[i])
                    result.push_back(rig.string(rig.ref(skeleton + 40) + 16 * i));
        }
        return result;
    }
    float hkxClipDuration(std::string bytes)
    {
        const Pack animation(std::move(bytes));
        const auto clip = animation.find("hkaSplineCompressedAnimation");
        return clip ? std::bit_cast<float>(animation.word(clip + 20)) : 0;
    }

    HkxObjectAnimation hkxObjectAnimation(std::string bytes, std::string_view event)
    {
        const Pack graph(std::move(bytes));
        const auto root = graph.find("hkbBehaviorGraph");
        const auto data = root ? graph.ref(root + 136) : 0;
        const auto names = data ? graph.ref(data + 120) : 0;
        if (!names) return {};
        const auto eventTable = graph.array(names + 16);
        std::vector<std::string> events;
        for (std::size_t i = 0; i < eventTable.size(); ++i)
            events.push_back(graph.string(graph.ref(names + 16) + 8 * i));
        const auto key = normalized(std::string(event));
        const auto chosen = std::ranges::find_if(events, [&](const auto& value) { return normalized(value) == key; });
        if (chosen == events.end()) return {};
        const auto eventId = static_cast<std::uint32_t>(chosen - events.begin());
        HkxObjectAnimation result;
        std::set<std::uint32_t> visited;
        const auto collect = [&](auto&& self, std::uint32_t generator) -> void {
            const auto object = graph.objects.find(generator);
            if (object == graph.objects.end() || !visited.insert(generator).second)
                return;
            if (object->second == "hkbBlenderGenerator")
            {
                for (const auto child : graph.array(generator + 96))
                    if (child)
                        self(self, graph.ref(child + 48));
                return;
            }
            if (object->second == "BGSGamebryoSequenceGenerator")
            {
                const auto sequence = normalized(graph.string(generator + 72));
                if (!result.sequence.empty() && result.sequence != sequence)
                    throw std::runtime_error("Ambiguous object animation sequence");
                result.sequence = sequence;
            }
            if (object->second != "hkbClipGenerator")
                return;
            const auto clip = normalized(graph.string(generator + 72));
            if (!result.clip.empty() && result.clip != clip)
                throw std::runtime_error("Ambiguous object animation clip");
            result.clip = clip;
            result.cropStart = std::bit_cast<float>(graph.word(generator + 88));
            result.cropEnd = std::bit_cast<float>(graph.word(generator + 92));
            result.start = std::bit_cast<float>(graph.word(generator + 96));
            result.speed = std::bit_cast<float>(graph.word(generator + 100));
            if (const auto triggers = graph.ref(generator + 80))
            {
                const auto start = graph.ref(triggers + 16);
                const auto size = graph.word(triggers + 24);
                if (!start || size > 8192)
                    return;
                for (std::uint32_t j = 0; j < size; ++j)
                {
                    const auto entry = start + j * 32;
                    const auto id = graph.word(entry + 8);
                    const auto time = std::bit_cast<float>(graph.word(entry));
                    if (id < events.size() && std::isfinite(time))
                    {
                        auto& list = graph.word(entry + 24) & 0xff ? result.endRelativeEvents : result.events;
                        list.emplace_back(time, events[id]);
                    }
                }
            }
        };
        for (const auto& [machine, type] : graph.objects)
        {
            if (type != "hkbStateMachine") continue;
            std::map<std::uint32_t, std::uint32_t> states;
            std::set<std::uint32_t> lists;
            if (const auto wildcards = graph.ref(machine + 160)) lists.insert(wildcards);
            for (const auto state : graph.array(machine + 144))
            {
                if (!state) continue;
                states.emplace(graph.word(state + 104), state);
                if (const auto transitions = graph.ref(state + 80)) lists.insert(transitions);
            }
            for (const auto list : lists)
            {
                const auto rows = graph.ref(list + 16);
                const auto count = graph.word(list + 24);
                if (!rows || count > 8192) continue;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const auto row = rows + 72 * i;
                    if (graph.word(row + 48) != eventId || (graph.word(row + 64) >> 16 & 0x23)
                        || graph.ref(row + 40)) continue;
                    const auto state = states.find(graph.word(row + 52));
                    if (state == states.end() || (graph.word(state->second + 112) & 0xff) == 0) continue;
                    collect(collect, graph.ref(state->second + 88));
                }
            }
        }
        return result;
    }
}
