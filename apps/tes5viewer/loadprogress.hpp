#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace TES5Viewer
{
    using LoadProgressCallback = void (*)(void*, const char*, const char*, std::uint64_t,
        std::uint64_t, std::uint64_t, std::uint64_t);
    struct LoadProgressState
    {
        LoadProgressCallback callback = nullptr;
        void* context = nullptr;
        std::string phase, item;
        std::uint64_t done = 0, total = 0, textures = 0, bytes = 0;
        void publish() const
        { if (callback) callback(context, phase.c_str(), item.c_str(), done, total, textures, bytes); }
    };
    inline thread_local LoadProgressState loadProgress;
    inline void loadPhase(std::string_view phase, std::uint64_t total = 0)
    { loadProgress.phase = phase; loadProgress.done = 0; loadProgress.total = total; loadProgress.item.clear(); loadProgress.publish(); }
    inline void loadItem(std::string_view item)
    { loadProgress.item = item; loadProgress.publish(); }
    inline void loadCompleted()
    { ++loadProgress.done; loadProgress.publish(); }
    inline void loadTexture(std::string_view path, std::uint64_t bytes)
    { ++loadProgress.textures; loadProgress.bytes += bytes; loadItem(path); }
}
