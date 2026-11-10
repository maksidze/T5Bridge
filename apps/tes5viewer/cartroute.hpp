#ifndef OPENMW_APPS_TES5VIEWER_CARTROUTE_HPP
#define OPENMW_APPS_TES5VIEWER_CARTROUTE_HPP

#include "recordattachment.hpp"

#include <components/esm/position.hpp>

namespace TES5Viewer
{
    struct CartRoutePoint
    {
        std::uint32_t referenceId;
        ESM::Position position;
    };

    struct CartRoute
    {
        std::uint32_t packageId;
        std::vector<CartRoutePoint> points;
        std::uint8_t preferredSpeed;
    };

    // Continue from the current transform without teleporting when the patrol changes.
    void joinCartRoute(CartRoute& next, const CartRoute& previous, double distance);

    double cartRouteLength(const CartRoute& route);

    // Kinematic interpolation of the linked markers. The caller supplies distance
    // in world units. The path holds its last transform when that distance is exhausted.
    ESM::Position sampleCartRoute(const CartRoute& route, double distance);

    // Advance an actor's yaw toward the path heading using its MOVT angular speed.
    float advanceRouteHeading(float current, float target, double radiansPerSecond, double seconds);
}

#endif
