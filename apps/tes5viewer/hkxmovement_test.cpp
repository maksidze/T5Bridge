#include "hkxmovement.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace
{
    void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    struct Fixture
    {
        std::string data, classes;
        std::vector<std::pair<unsigned, unsigned>> pointers, objects;
        unsigned reserve(std::size_t n) { const auto p = data.size(); data.resize(p + n); return static_cast<unsigned>(p); }
        void word(unsigned p, unsigned value) { std::memcpy(data.data() + p, &value, 4); }
        unsigned text(const std::string& value) { const auto p = reserve(value.size() + 1); data.replace(p, value.size(), value); return p; }
        void pointer(unsigned field, unsigned to) { pointers.emplace_back(field, to); }
        unsigned object(const std::string& type, unsigned n = 192)
        {
            const auto p = reserve(n); objects.emplace_back(p, static_cast<unsigned>(classes.size())); classes += type; classes += '\0'; return p;
        }
        void string(unsigned field, const std::string& value) { pointer(field, text(value)); }
        void array(unsigned field, const std::vector<unsigned>& entries, bool refs = true)
        {
            const auto at = reserve(entries.size() * (refs ? 8 : 4));
            pointer(field, at); word(field + 8, static_cast<unsigned>(entries.size()));
            for (unsigned i = 0; i < entries.size(); ++i)
                if (refs) pointer(at + i * 8, entries[i]); else word(at + i * 4, entries[i]);
        }
        std::string pack()
        {
            std::string out(208 + classes.size(), '\0'); out.replace(208, classes.size(), classes);
            const auto dataAt = static_cast<unsigned>(out.size()), local = static_cast<unsigned>(data.size());
            const auto put = [&](unsigned value) { out.append(reinterpret_cast<const char*>(&value), 4); };
            out += data;
            for (auto [from, to] : pointers) { put(from); put(to); }
            const auto global = static_cast<unsigned>(out.size()) - dataAt, virt = global;
            for (auto [from, name] : objects) { put(from); put(0); put(name); }
            const auto end = static_cast<unsigned>(out.size()) - dataAt;
            const auto at = [&](unsigned p, unsigned value) { std::memcpy(out.data() + p, &value, 4); };
            at(0, 0x57e0e057); at(4, 0x10c0c010); at(12, 8); at(16, 0x108); at(20, 3);
            out.replace(40, 14, "hk_2010.2.0-r1");
            out.replace(64, 14, "__classnames__"); at(84, 208);
            for (unsigned p = 88; p <= 108; p += 4) at(p, static_cast<unsigned>(classes.size()));
            out.replace(112, 9, "__types__"); at(132, dataAt);
            out.replace(160, 8, "__data__"); at(180, dataAt);
            at(184, local); at(188, global); at(192, virt); at(196, end); at(200, end); at(204, end);
            return out;
        }
    };
    std::map<std::string, std::string> makeFiles(bool ambiguous = false, bool rider = false)
    {
        Fixture project;
        auto ps = project.object("hkbProjectStringData");
        project.array(ps + 48, {project.text("Characters\\Strange.hkx")});
        Fixture character; auto cs = character.object("hkbCharacterStringData", 208);
        character.string(cs + 184, "Behaviors\\Main.hkx");
        Fixture graph;
        auto root = graph.object("hkbBehaviorGraph"), machine = graph.object("hkbStateMachine");
        graph.pointer(root + 128, machine);
        auto names = graph.object("hkbBehaviorGraphStringData"), values = graph.object("hkbVariableValueSet");
        graph.array(names + 48, {graph.text("iState_AlphaMove"), graph.text("iState_OtherMove")});
        graph.array(values + 16, {41, 93}, false);
        std::vector<unsigned> states;
        for (unsigned i = 0; i < (ambiguous ? 3u : 2u); ++i)
        {
            const auto state = graph.object("hkbStateMachineStateInfo"), tag = graph.object("BSiStateTaggingGenerator");
            const auto clip = graph.object("hkbClipGenerator");
            graph.pointer(state + 88, tag); graph.pointer(tag + 80, clip); graph.word(tag + 88, i == 2 ? 93 : 41);
            graph.string(tag + 56, "MisleadingDebugName");
            graph.string(clip + 72, i == 1 ? "Animations\\RunForward.hkx"
                : i == 2 && rider ? "Animations\\Horse_Rider\\WalkForward.hkx" : "Animations\\WalkForward.hkx");
            states.push_back(state);
        }
        graph.array(machine + 144, states);
        return {{"meshes/actors/test/project.hkx", project.pack()},
            {"meshes/actors/test/characters/strange.hkx", character.pack()},
            {"meshes/actors/test/behaviors/main.hkx", graph.pack()}};
    }
}
int main(int argc, char** argv)
{
    try
    {
        auto files = makeFiles();
        const auto read = [&](const std::string& name) { return files.at(name); };
        auto result = TES5Viewer::hkxLocomotionTypes("meshes\\actors\\test\\Project.hkx", read);
        require(result[0] == "alphamove" && result[1] == "alphamove", "Authored movement constants were not resolved");
        files = makeFiles(true); result = TES5Viewer::hkxLocomotionTypes("meshes/actors/test/project.hkx", read);
        require(result[0].empty() && result[1] == "alphamove", "Ambiguous walk must not select an arbitrary movement type");
        files = makeFiles(true, true); result = TES5Viewer::hkxLocomotionTypes("meshes/actors/test/project.hkx", read);
        require(result[0] == "alphamove" && result[1] == "alphamove", "Horse rider clips changed the dismounted actor's movement type");
        auto& graph = files.at("meshes/actors/test/behaviors/main.hkx");
        graph.resize(32); bool rejected = false;
        try { TES5Viewer::hkxLocomotionTypes("meshes/actors/test/project.hkx", read); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "Truncated graph accepted");
        std::cout << "PASS movement constants, path resolution, debug-name independence, ambiguity, truncated HKX\n";
        if (argc == 3)
        {
            const auto types = TES5Viewer::hkxLocomotionTypes(argv[2], [&](const std::string& name) {
                std::ifstream stream(std::filesystem::path(argv[1]) / name, std::ios::binary);
                if (!stream) throw std::runtime_error("Missing asset " + name);
                return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{});
            });
            std::cout << "ASSET walk=" << types[0] << " run=" << types[1] << '\n';
            require(!types[0].empty() && !types[1].empty(), "Original asset did not resolve forward locomotion");
        }
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
