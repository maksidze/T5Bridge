#ifndef T5_FACEMORPHS_HPP
#define T5_FACEMORPHS_HPP

#include <osg/Vec3f>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace TES5Viewer
{
    struct ExpressionMorph { std::string name; std::vector<osg::Vec3f> deltas; };

    // FRTRI003: https://facegen.com/dl/sdk/doc/manual/fileformats.html
    // Vertex ordering must agree with the head part, never fit deltas by proximity.
    inline std::vector<ExpressionMorph> readExpressionMorphs(std::string_view bytes,
        std::size_t vertices, std::span<const unsigned> indices)
    {
        std::size_t pos = 0;
        const auto take = [&](std::size_t size) {
            if (size > bytes.size() - pos) throw std::runtime_error("Truncated expression TRI");
            const auto result = bytes.substr(pos, size); pos += size; return result;
        };
        const auto number = [&]<class T>() { T v; const auto data = take(sizeof(T)); std::memcpy(&v, data.data(), sizeof(T)); return v; };
        if (take(8) != "FRTRI003") throw std::runtime_error("Unsupported expression TRI signature");
        std::array<std::uint32_t, 14> h{};
        for (auto& v : h) v = number.operator()<std::uint32_t>();
        if (h[0] != vertices || h[1] * std::uint64_t(3) != indices.size() || h[2] || h[3] || h[4]
            || h[7] > 1024 || h[9] > 1000000 || vertices > 1000000)
            throw std::runtime_error("Expression TRI does not match head topology");
        take((std::uint64_t(h[0]) + h[9]) * 12);
        using Triangle = std::array<unsigned, 3>;
        std::vector<Triangle> source, target;
        for (std::size_t i = 0; i < indices.size(); i += 3)
        {
            Triangle a{}, b{indices[i], indices[i+1], indices[i+2]};
            for (auto& v : a) v = number.operator()<std::uint32_t>();
            std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
            source.push_back(a); target.push_back(b);
        }
        std::sort(source.begin(), source.end()); std::sort(target.begin(), target.end());
        if (source != target) throw std::runtime_error("Expression TRI vertex indexing differs from head");
        take(std::uint64_t(h[5]) * 8);
        if (h[5]) take(std::uint64_t(h[1]) * 12);
        std::vector<ExpressionMorph> result;
        for (unsigned m = 0; m < h[7]; ++m)
        {
            const auto size = number.operator()<std::uint32_t>();
            if (!size || size > 1024) throw std::runtime_error("Invalid expression TRI name");
            auto name = take(size);
            if (name.back() == '\0') name.remove_suffix(1);
            const auto scale = number.operator()<float>();
            if (!std::isfinite(scale)) throw std::runtime_error("Invalid expression TRI scale");
            auto& morph = result.emplace_back(std::string(name), std::vector<osg::Vec3f>(vertices));
            for (auto& delta : morph.deltas)
                for (unsigned c = 0; c < 3; ++c)
                {
                    delta[c] = number.operator()<std::int16_t>() * scale;
                    if (!std::isfinite(delta[c])) throw std::runtime_error("Non-finite expression TRI delta");
                }
        }
        return result;
    }
}
#endif
