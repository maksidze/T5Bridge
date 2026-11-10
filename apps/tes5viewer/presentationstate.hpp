#ifndef TES5_PRESENTATIONSTATE_HPP
#define TES5_PRESENTATIONSTATE_HPP

#include "recordattachment.hpp"
#include "../papyrustool/checkpoint.hpp"
#include <components/vfs/manager.hpp>
#include <components/esm/fourcc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>

namespace TES5Viewer
{
    // Native state uses the script clock. Reloading a checkpoint must not replay Apply.
    struct PresentationState
    {
        struct Effect
        {
            double started = 0;
            float strength = 1;
            void checkpoint(Papyrus::Checkpoint& ar) { ar(started, strength); }
        };
        std::map<std::uint32_t, Effect> effects;
        std::map<std::string, std::pair<double, std::uint32_t>> help;
        bool gamepad = false;
        void checkpoint(Papyrus::Checkpoint& ar) { ar(effects, help); }

        static float real(std::span<const std::byte> b, std::size_t at)
        {
            float value = 0;
            if (at + 4 > b.size()) throw std::runtime_error("Truncated presentation data");
            std::memcpy(&value, b.data() + at, 4);
            if (!std::isfinite(value)) throw std::runtime_error("Nonfinite presentation value");
            return value;
        }
        static float duration(const RecordPayload& record)
        {
            float value = 0;
            visitSubrecords(record, [&](auto tag, auto bytes) {
                if (tag == ESM::fourCC("DNAM") && bytes.size() >= 8)
                {
                    std::uint32_t animated; std::memcpy(&animated, bytes.data(), 4);
                    if (animated) value = real(bytes, 4);
                }
            });
            return std::max(0.f, value);
        }
        static float curve(std::span<const std::byte> bytes, float t, float fallback)
        {
            if (bytes.empty()) return fallback;
            if (bytes.size() % 8) throw std::runtime_error("Invalid IMAD interpolator");
            float previousT = real(bytes, 0), previous = real(bytes, 4);
            for (std::size_t i = 8; i < bytes.size(); i += 8)
            {
                const float nextT = real(bytes, i), next = real(bytes, i + 4);
                if (nextT < previousT) throw std::runtime_error("Unordered IMAD interpolator");
                if (t <= nextT)
                    return previous + (next - previous) * std::clamp((t - previousT) / std::max(1e-6f, nextT - previousT), 0.f, 1.f);
                previousT = nextT; previous = next;
            }
            return previous;
        }
        // [saturation, brightness, contrast, bloom mult/add, blur, radial, motion blur]
        std::string sample(const ReadWinningRecord& read, double now)
        {
            std::array<float, 8> result{1,1,1,1,0,0,0,0};
            for (auto it = effects.begin(); it != effects.end();)
            {
                const auto record = read(it->first);
                const float length = duration(record), elapsed = static_cast<float>(std::max(0., now - it->second.started));
                if (length > 0 && elapsed >= length) { it = effects.erase(it); continue; }
                const float t = length > 0 ? elapsed / length : 0;
                std::array<float, 8> mult{1,1,1,1,1,1,1,1}, add{};
                visitSubrecords(record, [&](auto tag, auto bytes) {
                    for (unsigned i = 0; i < 3; ++i)
                    {
                        if (tag == (ESM::fourCC("\x11IAD") + i)) mult[i] = curve(bytes, t, 1);
                        if (tag == (ESM::fourCC("QIAD") + i)) add[i] = curve(bytes, t, 0);
                    }
                    if (tag == ESM::fourCC("\x03IAD")) mult[3] = curve(bytes, t, 1);
                    if (tag == ESM::fourCC("CIAD")) add[4] = curve(bytes, t, 0);
                    if (tag == ESM::fourCC("BNAM")) add[5] = curve(bytes, t, 0);
                    if (tag == ESM::fourCC("RNAM")) add[6] = curve(bytes, t, 0);
                    if (tag == ESM::fourCC("NAM4")) add[7] = curve(bytes, t, 0);
                });
                const float strength = it->second.strength;
                for (unsigned i = 0; i < result.size(); ++i)
                    result[i] = result[i] * (1 + (mult[i] - 1) * strength) + add[i] * strength;
                ++it;
            }
            std::ostringstream out; out << "[\"imagespace\"";
            for (auto v : result) out << ',' << v;
            out << ',' << !effects.empty() << ']'; return out.str();
        }
    };
}
#endif
