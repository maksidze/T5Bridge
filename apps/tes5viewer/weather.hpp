#pragma once
#include "recordattachment.hpp"
#include "../papyrustool/checkpoint.hpp"
#include <array>

namespace TES5Viewer
{
    class WorldWeather
    {
        struct Region { std::uint32_t id, world; std::vector<std::vector<std::array<float,2>>> polygons; unsigned priority = 0; };
        ReadWinningRecord read;
        std::shared_ptr<const WinningRecordIndex> index;
        std::vector<Region> regions;
        bool regionsLoaded = false;
        std::vector<std::uint32_t> signature;
        std::vector<std::uint32_t> containing(std::uint32_t world, double x, double y);
        std::array<float,21> sampleOne(std::uint32_t id, float hour) const;
    public:
        std::uint32_t current = 0, previous = 0, overrideId = 0, world = 0, climate = 0;
        float progress = 1;
        bool initialized = false;
        WorldWeather(ReadWinningRecord reader, std::shared_ptr<const WinningRecordIndex> records)
            : read(std::move(reader)), index(std::move(records)) {}
        void force(std::uint32_t id, bool overrideWeather);
        void begin(std::uint32_t worldId, std::uint32_t initial, double x, double y);
        void tick(double x, double y, double gameHours);
        std::array<float,21> sample(float hour) const;
        void checkpoint(Papyrus::Checkpoint& ar) { ar(current,previous,overrideId,world,climate,progress,initialized,signature); }
    };
}
