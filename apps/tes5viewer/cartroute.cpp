#include "cartroute.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace TES5Viewer
{
    namespace
    {
        double segmentLength(const ESM::Position& start, const ESM::Position& end)
        {
            return std::hypot(static_cast<double>(end.pos[0]) - start.pos[0],
                static_cast<double>(end.pos[1]) - start.pos[1], static_cast<double>(end.pos[2]) - start.pos[2]);
        }
    }

    double cartRouteLength(const CartRoute& route)
    {
        double length = 0.;
        for (std::size_t i = 1; i < route.points.size(); ++i)
            length += segmentLength(route.points[i - 1].position, route.points[i].position);
        return length;
    }

    void joinCartRoute(CartRoute& next, const CartRoute& previous, double distance)
    {
        const auto current = sampleCartRoute(previous, distance);
        const auto destination = next.points.front().position.asVec3();
        const auto origin = current.asVec3();
        std::vector<CartRoutePoint> remaining{ { 0, current } };
        double traversed = 0.;
        for (std::size_t i = 1; i < previous.points.size(); ++i)
        {
            traversed += segmentLength(previous.points[i - 1].position, previous.points[i].position);
            if (traversed > distance)
                remaining.push_back(previous.points[i]);
        }
        // Follow the old road up to the point closest to the next patrol start.
        std::size_t keep = 1;
        double best = (destination - origin).length2();
        for (std::size_t i = 1; i < remaining.size(); ++i)
        {
            const auto start = remaining[i - 1].position.asVec3();
            const auto segment = remaining[i].position.asVec3() - start;
            if (segment.length2() == 0)
                continue;
            const auto fraction = std::clamp(((destination - start) * segment) / segment.length2(), 0.f, 1.f);
            const double gap = (destination - (start + segment * fraction)).length2();
            if (gap < best)
            {
                best = gap;
                keep = i + (fraction >= 1.f ? 1 : 0);
            }
        }
        remaining.resize(keep);
        next.points.insert(next.points.begin(), remaining.begin(), remaining.end());
    }

    ESM::Position sampleCartRoute(const CartRoute& route, double distance)
    {
        if (!std::isfinite(distance) || route.points.empty())
            throw std::runtime_error("Cannot sample MQ101 patrol at the requested distance");
        if (distance <= 0)
            return route.points.front().position;
        for (std::size_t i = 1; i < route.points.size(); ++i)
        {
            const auto& start = route.points[i - 1].position;
            const auto& end = route.points[i].position;
            const auto length = segmentLength(start, end);
            if (length > 0 && distance < length)
            {
                ESM::Position result;
                const auto fraction = static_cast<float>(distance / length);
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    result.pos[axis] = std::lerp(start.pos[axis], end.pos[axis], fraction);
                    const auto delta = std::remainder(end.rot[axis] - start.rot[axis], 2 * std::numbers::pi_v<float>);
                    result.rot[axis] = start.rot[axis] + fraction * delta;
                }
                // While travelling, facing must agree with the path derivative.
                // Marker rotations describe arrival poses; interpolating them on a
                // straight positional segment makes an attached vehicle slide sideways.
                const float dx = end.pos[0] - start.pos[0], dy = end.pos[1] - start.pos[1];
                if (dx != 0 || dy != 0)
                    result.rot[2] = std::atan2(dx, dy);
                return result;
            }
            distance -= length;
        }
        return route.points.back().position;
    }

    float advanceRouteHeading(float current, float target, double radiansPerSecond, double seconds)
    {
        if (!std::isfinite(current) || !std::isfinite(target) || !std::isfinite(radiansPerSecond)
            || !std::isfinite(seconds) || radiansPerSecond < 0 || seconds < 0)
            throw std::runtime_error("Invalid actor turn rate or time step");
        const double difference = std::remainder(double(target) - current, 2 * std::numbers::pi);
        const double maximum = radiansPerSecond * seconds;
        return static_cast<float>(current + std::clamp(difference, -maximum, maximum));
    }
}
