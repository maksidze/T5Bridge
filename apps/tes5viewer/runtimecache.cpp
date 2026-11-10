#include "runtimecache.hpp"
#include "loadprogress.hpp"
#include <components/files/conversion.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace TES5Viewer
{
    namespace
    {
        constexpr std::uint64_t maxEntry = 4ull * 1024 * 1024 * 1024;
        constexpr std::uint64_t maxDisk = 8ull * 1024 * 1024 * 1024;
        constexpr std::string_view magic = "T5DERIVED0002";
        std::uint64_t hash(std::string_view data)
        {
            std::uint64_t value = 14695981039346656037ull;
            for (const unsigned char byte : data) { value ^= byte; value *= 1099511628211ull; }
            return value;
        }
        template<class T> void write(std::ostream& out, const T& value)
        { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); }
        template<class T> void read(std::istream& in, T& value)
        { in.read(reinterpret_cast<char*>(&value), sizeof(value)); }
#ifdef _WIN32
        struct DirectoryWatch
        {
            HANDLE handle = INVALID_HANDLE_VALUE;
            explicit DirectoryWatch(const std::filesystem::path& path)
                : handle(FindFirstChangeNotificationW(path.c_str(), TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE)) {}
            ~DirectoryWatch() { if (valid()) FindCloseChangeNotification(handle); }
            bool valid() const { return handle != INVALID_HANDLE_VALUE; }
            bool changed() const { return !valid() || WaitForSingleObject(handle, 0) != WAIT_TIMEOUT; }
            void rearm()
            {
                if (valid() && !FindNextChangeNotification(handle))
                { FindCloseChangeNotification(handle); handle = INVALID_HANDLE_VALUE; }
            }
        };
#endif
        void stamp(std::ostream& out, const std::filesystem::path& path)
        {
            out << std::quoted(Files::pathToUnicodeString(std::filesystem::weakly_canonical(path))) << ' '
                << std::filesystem::file_size(path) << ' '
                << std::filesystem::last_write_time(path).time_since_epoch().count() << '\n';
        }
    }
    std::string runtimeCacheName(std::string_view key)
    { std::ostringstream out; out << std::hex << std::setw(16) << std::setfill('0') << hash(key); return out.str() + ".t5cache"; }

    std::string runtimeSourceKey(const std::filesystem::path& data,
        const std::filesystem::path& resources, const std::vector<std::filesystem::path>& plugins)
    {
        std::ostringstream out; out << magic << ' ' << __DATE__ << ' ' << __TIME__ << '\n';
#ifdef _WIN32
        // Fingerprint the loaded converter, including changes outside this translation unit.
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&runtimeSourceKey), &module))
            throw std::runtime_error("Cannot identify derived-data converter");
        std::array<wchar_t, 32768> modulePath{};
        const auto length = GetModuleFileNameW(module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (!length || length >= modulePath.size()) throw std::runtime_error("Cannot locate derived-data converter");
        stamp(out, std::filesystem::path(modulePath.data()));
#endif
        for (const auto& plugin : plugins) stamp(out, plugin); // Order affects FormIDs and overrides.
        const auto dataRoot = std::filesystem::canonical(data), resourceRoot = std::filesystem::canonical(resources);
        struct Manifest
        {
            std::filesystem::path data, resources;
            std::string text;
#ifdef _WIN32
            std::unique_ptr<DirectoryWatch> dataWatch, resourceWatch;
#endif
        };
        static Manifest manifest;
        bool dirty = manifest.data != dataRoot || manifest.resources != resourceRoot || manifest.text.empty();
#ifdef _WIN32
        if (dirty)
        {
            manifest.dataWatch = std::make_unique<DirectoryWatch>(dataRoot);
            manifest.resourceWatch = std::make_unique<DirectoryWatch>(resourceRoot);
        }
        dirty = dirty || manifest.dataWatch->changed() || manifest.resourceWatch->changed();
        if (dirty) { manifest.dataWatch->rearm(); manifest.resourceWatch->rearm(); }
#else
        dirty = true;
#endif
        if (dirty)
        {
            loadPhase("Indexing resource revisions");
            std::vector<std::filesystem::path> assets;
            for (const auto& root : {dataRoot, resourceRoot})
                for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
                    if (entry.is_regular_file()) assets.push_back(entry.path());
            std::ranges::sort(assets);
            std::ostringstream revisions;
            for (const auto& path : assets) stamp(revisions, path);
            manifest.text = std::move(revisions).str();
            manifest.data = dataRoot; manifest.resources = resourceRoot;
        }
        out << manifest.text;
        return out.str();
    }

    bool readRuntimeCache(const std::filesystem::path& directory, std::string_view key, PreviewBundle& bundle) noexcept
    {
        if (directory.empty() || key.empty()) return false;
        try
        {
            const auto path = directory / runtimeCacheName(key);
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            in.exceptions(std::ios::badbit | std::ios::failbit);
            const auto size = std::filesystem::file_size(path);
            if (size > maxEntry || size < magic.size() + 12) return false;
            std::uint64_t remaining = size;
            const auto take = [&](std::uint64_t count) { if (count > remaining) throw std::runtime_error("Truncated derived cache"); remaining -= count; };
            std::string header(magic.size(), '\0'); take(header.size()); in.read(header.data(), header.size());
            if (header != magic) return false;
            std::uint64_t keySize = 0; take(8); read(in, keySize);
            if (keySize != key.size()) return false;
            std::string storedKey(keySize, '\0'); take(keySize); in.read(storedKey.data(), keySize);
            if (storedKey != key) return false;
            std::uint32_t count = 0; take(4); read(in, count);
            if (count > 100000) return false;
            PreviewBundle candidate;
            loadPhase("Reading converted resource cache", size); loadItem(Files::pathToUnicodeString(path.filename()));
            for (std::uint32_t i = 0; i < count; ++i)
            {
                std::uint32_t nameSize = 0; std::uint64_t bytes = 0, checksum = 0;
                take(20); read(in, nameSize); read(in, bytes); read(in, checksum);
                if (nameSize > 4096) return false;
                take(nameSize); take(bytes);
                std::string name(nameSize, '\0'), value(bytes, '\0');
                in.read(name.data(), name.size()); in.read(value.data(), value.size());
                if (hash(value) != checksum || !candidate.emplace(std::move(name), std::move(value)).second) return false;
                loadProgress.done = size - remaining; loadProgress.publish();
            }
            if (remaining || !candidate.contains("meshes.json") || !candidate.contains("location.json")
                || !candidate.contains("meshes.bin") || !candidate.contains("collision.json")) return false;
            bundle = std::move(candidate);
            std::error_code error;
            std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), error);
            return true;
        }
        catch (...) { return false; } // Derived data is optional; a bad entry triggers normal conversion.
    }

    void writeRuntimeCache(const std::filesystem::path& directory, std::string_view key, const PreviewBundle& bundle) noexcept
    {
        if (directory.empty() || key.empty()) return;
        std::filesystem::path temporary;
        try
        {
            std::uint64_t total = magic.size() + 12 + key.size();
            std::uint32_t count = 0;
            for (const auto& [name, bytes] : bundle)
                if (!bundle.contains("source:" + name)) { total += 20 + name.size() + bytes.size(); ++count; }
            if (total > maxEntry) return;
            std::filesystem::create_directories(directory);
            const auto path = directory / runtimeCacheName(key);
            temporary = path; temporary += ".tmp";
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.exceptions(std::ios::badbit | std::ios::failbit);
            out.write(magic.data(), magic.size()); write(out, std::uint64_t(key.size())); out.write(key.data(), key.size());
            write(out, count);
            for (const auto& [name, bytes] : bundle)
            {
                // DDS is already converted/compressed in the source BSA or loose file.
                // Store its VFS identity once instead of duplicating gigabytes per window.
                if (bundle.contains("source:" + name)) continue;
                write(out, std::uint32_t(name.size())); write(out, std::uint64_t(bytes.size())); write(out, hash(bytes));
                out.write(name.data(), name.size()); out.write(bytes.data(), bytes.size());
            }
            out.close();
            // All bridge loads are serialized. Readers see only complete files.
            std::error_code error; std::filesystem::remove(path, error); std::filesystem::rename(temporary, path);
            std::vector<std::filesystem::directory_entry> entries;
            std::uint64_t used = 0;
            for (const auto& entry : std::filesystem::directory_iterator(directory))
                if (entry.is_regular_file() && entry.path().extension() == ".t5cache")
                { entries.push_back(entry); used += entry.file_size(); }
            std::ranges::sort(entries, {}, [](const auto& entry) { return entry.last_write_time(); });
            for (const auto& entry : entries)
            {
                if (used <= maxDisk) break;
                const auto bytes = entry.file_size();
                if (entry.path() != path && std::filesystem::remove(entry.path(), error)) used -= bytes;
            }
        }
        catch (...) { std::error_code error; if (!temporary.empty()) std::filesystem::remove(temporary, error); }
    }
}
