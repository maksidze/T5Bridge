#ifndef OPENMW_APPS_TES5VIEWER_AUDIODECODE_HPP
#define OPENMW_APPS_TES5VIEWER_AUDIODECODE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace TES5Viewer
{
    struct AudioLoop
    {
        std::uint32_t first, end;
    };

    struct DecodedAudio
    {
        int sampleRate;
        int channels;
        std::string pcm;
        std::optional<AudioLoop> loop;
    };

    DecodedAudio decodeAudio(std::span<const std::byte> data);
}

#endif
