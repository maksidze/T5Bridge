#include "pluginlist.hpp"

#include <components/files/conversion.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace TES5Viewer
{
    namespace
    {
        std::string trim(std::string value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return {};
            return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
        }
    }

    std::vector<std::filesystem::path> loadPluginList(
        const std::filesystem::path& list, const std::filesystem::path& data)
    {
        if (std::filesystem::file_size(list) > 4 * 1024 * 1024)
            throw std::runtime_error("Plugin list exceeds the 4 MiB limit");
        std::ifstream input(list, std::ios::binary);
        if (!input)
            throw std::runtime_error("Cannot open plugin list: " + Files::pathToUnicodeString(list));
        std::vector<std::string> lines;
        bool starred = Misc::StringUtils::ciEqual(Files::pathToUnicodeString(list.filename()), "plugins.txt");
        std::string line;
        bool firstLine = true;
        while (std::getline(input, line))
        {
            if (firstLine && line.starts_with("\xef\xbb\xbf"))
                line.erase(0, 3);
            firstLine = false;
            if (line.find('\0') != std::string::npos)
                throw std::runtime_error("Plugin list must be UTF-8 text, not UTF-16 or binary data");
            line = trim(std::move(line));
            if (line.empty() || line.front() == '#')
                continue;
            starred = starred || line.front() == '*';
            lines.push_back(std::move(line));
        }
        if (input.bad())
            throw std::runtime_error("Failed reading plugin list");

        std::map<std::string, std::filesystem::path> available;
        for (const auto& entry : std::filesystem::directory_iterator(data))
        {
            if (!entry.is_regular_file())
                continue;
            const auto extension = Misc::StringUtils::lowerCase(Files::pathToUnicodeString(entry.path().extension()));
            if (extension != ".esm" && extension != ".esp" && extension != ".esl")
                continue;
            const auto key = Misc::StringUtils::lowerCase(Files::pathToUnicodeString(entry.path().filename()));
            if (!available.emplace(key, entry.path()).second)
                throw std::runtime_error("Ambiguous plugin filenames differing only by case: " + key);
        }

        std::vector<std::string> active;
        std::set<std::string> seen;
        for (auto name : lines)
        {
            const bool marked = name.front() == '*';
            if (marked)
                name = trim(name.substr(1));
            const auto filename = Files::pathFromUnicodeString(name);
            const auto extension = Misc::StringUtils::lowerCase(Files::pathToUnicodeString(filename.extension()));
            if (name.empty() || name.front() == '+' || name.front() == '-' || filename.has_parent_path()
                || name.find_first_of("/\\:") != std::string::npos
                || (extension != ".esm" && extension != ".esp" && extension != ".esl"))
                throw std::runtime_error("Expected plugin filenames (.esm/.esp/.esl), not a mod-folder list: " + name);
            if (starred && !marked)
                continue;
            const auto key = Misc::StringUtils::lowerCase(name);
            if (!seen.insert(key).second)
                throw std::runtime_error("Duplicate active plugin in list: " + name);
            if (!available.contains(key))
                throw std::runtime_error("Plugin listed but absent from Data: " + name);
            active.push_back(key);
        }
        if (active.empty())
            throw std::runtime_error("Plugin list contains no active plugins");

        constexpr std::array core{ "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm" };
        std::vector<std::filesystem::path> result;
        for (const auto* name : core)
            if (const auto found = available.find(name); found != available.end())
                result.push_back(found->second);
        for (const auto& name : active)
            if (std::find(core.begin(), core.end(), name) == core.end())
                result.push_back(available.at(name));
        return result;
    }
}
