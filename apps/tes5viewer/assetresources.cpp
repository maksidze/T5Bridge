#include "assetresources.hpp"

#include <algorithm>
#include <istream>
#include <map>
#include <stdexcept>

#include <components/files/conversion.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/bsaarchive.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace TES5Viewer
{
    namespace
    {
        // Data can supply art independently of the active plugins. An inactive
        // plugin's archive must not replace the code of a winning quest record.
        class ArtArchive final : public VFS::Archive
        {
        public:
            explicit ArtArchive(std::unique_ptr<VFS::Archive> archive) : mArchive(std::move(archive)) {}

            void listResources(VFS::FileMap& out) override
            {
                VFS::FileMap files;
                mArchive->listResources(files);
                std::erase_if(files, [](const auto& item) { return script(item.first); });
                for (const auto& [path, file] : files)
                    out.insert_or_assign(path, file);
            }
            bool contains(VFS::Path::NormalizedView file) const override
            {
                return !script(file) && mArchive->contains(file);
            }
            std::string getDescription() const override { return mArchive->getDescription(); }

        private:
            static bool script(VFS::Path::NormalizedView path)
            {
                return path.value().starts_with("scripts/") || path.value().starts_with("seq/");
            }
            std::unique_ptr<VFS::Archive> mArchive;
        };

        std::string pathString(const std::filesystem::path& path)
        {
            return Files::pathToUnicodeString(path);
        }

        bool directoryLink(const std::filesystem::directory_entry& entry)
        {
            if (entry.is_symlink())
                return true;
#ifdef _WIN32
            // Junctions are reparse points but are not consistently reported as symlinks.
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
                throw std::runtime_error("Cannot inspect loose directory: " + pathString(entry.path()));
            return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
            return false;
#endif
        }

        void validateLooseRoot(const std::filesystem::path& root)
        {
            std::set<std::string> names;
            // Do not follow nested directory aliases. The caller has canonicalized the root itself.
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
            {
                if (entry.is_directory())
                {
                    if (directoryLink(entry))
                        throw std::runtime_error(
                            "Loose directory links and junctions are not supported: " + pathString(entry.path()));
                    continue;
                }
                if (!entry.is_regular_file())
                    throw std::runtime_error("Loose resource is not a regular file: " + pathString(entry.path()));
                const auto name = VFS::Path::normalizeFilename(pathString(entry.path().lexically_relative(root)));
                if (!names.insert(name).second)
                    throw std::runtime_error(
                        "Case-insensitive loose resource collision in " + pathString(root) + ": " + name);
            }
        }
    }

    std::vector<std::string> coreRenderArchives()
    {
        std::vector<std::string> result{ "Skyrim - Meshes0.bsa", "Skyrim - Meshes1.bsa" };
        for (unsigned int i = 0; i < 9; ++i)
            result.push_back("Skyrim - Textures" + std::to_string(i) + ".bsa");
        return result;
    }

    AssetPlan buildAssetPlan(const std::filesystem::path& data, const std::vector<std::filesystem::path>& contents,
        const std::vector<std::string>& archives, const std::vector<std::filesystem::path>& looseData,
        bool isolatedAssets)
    {
        if (!std::filesystem::is_directory(data))
            throw std::runtime_error("Data directory does not exist: " + pathString(data));
        const auto dataRoot = std::filesystem::canonical(data);
        std::map<std::string, std::vector<std::filesystem::path>> candidates;
        for (const auto& entry : std::filesystem::directory_iterator(dataRoot))
            candidates[VFS::Path::normalizeFilename(pathString(entry.path().filename()))].push_back(entry.path());

        AssetPlan result;
        result.dataResourcesIncluded = !isolatedAssets;
        const auto appendArchive = [&](const std::string& name, const std::string& kind, const std::string& plugin,
                                       bool required) {
            const auto filename = Files::pathFromUnicodeString(name);
            if (name.empty() || name.find_first_of("/\\") != std::string::npos || filename.has_parent_path()
                || filename.has_root_name() || filename.filename() != filename || name == "." || name == "..")
                throw std::runtime_error("Archive requires a filename within --data, not a path: " + name);
            const auto found = candidates.find(VFS::Path::normalizeFilename(name));
            if (found == candidates.end())
            {
                if (required)
                    throw std::runtime_error("Required " + kind + " archive was not found: " + name);
                return;
            }
            if (found->second.size() != 1)
                throw std::runtime_error("Case-insensitive archive filename is ambiguous: " + name);
            if (!std::filesystem::is_regular_file(found->second.front()))
                throw std::runtime_error("Archive is not a regular file: " + pathString(found->second.front()));
            const auto resolved = std::filesystem::canonical(found->second.front());
            // Repeated names, symlinks and hardlinks all keep the final requested priority.
            std::erase_if(result.archives,
                [&](const AssetArchive& previous) { return std::filesystem::equivalent(previous.path, resolved); });
            result.archives.push_back({ resolved, kind, plugin });
        };

        std::set<std::string> orderedArchives;
        for (const auto& archive : coreRenderArchives())
        {
            appendArchive(archive, "core", "", true);
            orderedArchives.insert(VFS::Path::normalizeFilename(archive));
        }
        std::set<std::string> activeOwners;
        for (const auto& content : contents)
        {
            const auto stem = pathString(content.stem());
            activeOwners.insert(VFS::Path::normalizeFilename(stem));
            orderedArchives.insert(VFS::Path::normalizeFilename(stem + ".bsa"));
            orderedArchives.insert(VFS::Path::normalizeFilename(stem + " - Textures.bsa"));
        }
        if (!isolatedAssets)
        {
            const auto ownedByInactivePlugin = [&](std::string_view archive) {
                auto owner = std::string(archive.substr(0, archive.size() - 4));
                for (;;)
                {
                    if (owner == "skyrim" || activeOwners.contains(owner))
                        return false;
                    if (candidates.contains(owner + ".esp") || candidates.contains(owner + ".esm")
                        || candidates.contains(owner + ".esl"))
                        return true;
                    const auto suffix = owner.rfind(" - ");
                    if (suffix == std::string::npos)
                        return false;
                    owner.resize(suffix);
                }
            };
            // The map supplies deterministic case-insensitive filename order. Active-plugin
            // archives are handled below so their priority follows the explicit plugin list.
            for (const auto& [name, paths] : candidates)
                if (name.ends_with(".bsa") && !orderedArchives.contains(name)
                    && (paths.size() > 1 || std::filesystem::is_regular_file(paths.front())))
                    appendArchive(name, ownedByInactivePlugin(name) ? "inactive-plugin-art" : "data", "", true);
            result.looseData.push_back(dataRoot);
        }
        for (const auto& content : contents)
        {
            const auto plugin = pathString(content.filename());
            const auto stem = pathString(content.stem());
            appendArchive(stem + ".bsa", "associated", plugin, false);
            appendArchive(stem + " - Textures.bsa", "associated", plugin, false);
        }
        for (const auto& archive : archives)
            appendArchive(archive, "explicit", "", true);
        for (const auto& root : looseData)
        {
            if (!std::filesystem::is_directory(root))
                throw std::runtime_error("Loose data directory does not exist: " + pathString(root));
            const auto resolved = std::filesystem::canonical(root);
            std::erase_if(result.looseData,
                [&](const std::filesystem::path& previous) { return std::filesystem::equivalent(previous, resolved); });
            result.looseData.push_back(resolved);
        }
        for (const auto& root : result.looseData)
            validateLooseRoot(root);
        return result;
    }

    std::unique_ptr<VFS::Manager> mountAssetResources(
        const AssetPlan& plan, const ToUTF8::StatelessUtf8Encoder* encoder)
    {
        auto result = std::make_unique<VFS::Manager>();
        for (const auto& archive : plan.archives)
        {
            auto source = VFS::makeBsaArchive(archive.path, encoder);
            if (archive.kind == "inactive-plugin-art")
                source = std::make_unique<ArtArchive>(std::move(source));
            result->addArchive(std::move(source));
        }
        for (const auto& root : plan.looseData)
            result->addArchive(std::make_unique<VFS::FileSystemArchive>(root));
        result->buildIndex();
        return result;
    }

    std::shared_ptr<VFS::Manager> mountRuntimeAssets(const AssetPlan& plan)
    {
        auto encoder = std::make_shared<ToUTF8::StatelessUtf8Encoder>(ToUTF8::calculateEncoding("win1252"));
        auto mounted = mountAssetResources(plan, encoder.get());
        return {mounted.release(), [encoder = std::move(encoder)](VFS::Manager* manager) { delete manager; }};
    }

    std::string modelAssetPath(std::string_view model)
    {
        auto result = VFS::Path::normalizeFilename(model);
        if (!result.starts_with("meshes/"))
            result.insert(0, "meshes/");
        return result;
    }
}
