#include "cartroute.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    TES5Viewer::CartRoutePoint point(unsigned id, float x, float y)
    {
        ESM::Position position{};
        position.pos[0] = x;
        position.pos[1] = y;
        return {id, position};
    }
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
}

int main()
{
    using namespace TES5Viewer;
    try
    {
        const CartRoute road{1, {point(10, 0, 0), point(11, 0, 10), point(12, 10, 10), point(13, 10, 20)}, 0};
        CartRoute branch{2, {point(20, 10, 15), point(21, 20, 15)}, 0};
        joinCartRoute(branch, road, 5);
        require(branch.points.size() == 5, "Patrol switch must retain both unvisited corners");
        require(branch.points[1].referenceId == 11 && branch.points[2].referenceId == 12,
            "Authored approach marker order changed");
        require(sampleCartRoute(branch, 0) == sampleCartRoute(road, 5), "Switch teleported the actor");
        require(sampleCartRoute(branch, 8).asVec3() == osg::Vec3f(3, 10, 0), "Route cut across the road corner");
        require(std::abs(cartRouteLength(branch) - 30) < .001, "Incorrect joined route length");

        CartRoute afterCorner{3, {point(20, 10, 15), point(21, 20, 15)}, 0};
        joinCartRoute(afterCorner, road, 17);
        require(afterCorner.points.size() == 4 && afterCorner.points[1].referenceId == 12,
            "Patrol switch revisits a passed marker");
        require(sampleCartRoute(afterCorner, 0).asVec3() == osg::Vec3f(7, 10, 0), "Joined current pose is wrong");
        require(sampleCartRoute(afterCorner, 100).asVec3() == osg::Vec3f(20, 15, 0), "Destination changed");

        CartRoute nextRoad{4, {point(30, 10, 25), point(31, 10, 30)}, 0};
        joinCartRoute(nextRoad, road, cartRouteLength(road));
        require(nextRoad.points.size() == 3, "Completed road must not replay old markers");
        require(sampleCartRoute(nextRoad, 0).asVec3() == osg::Vec3f(10, 20, 0), "Completed road join teleports");
        CartRoute straight{5, {point(40, 100, 100), point(41, -900, 600)}, 0};
        straight.points[0].position.rot[2] = 5.15f;
        straight.points[1].position.rot[2] = 4.78f;
        const auto moving = sampleCartRoute(straight, cartRouteLength(straight) / 2);
        const double dx = -1000, dy = 500;
        require(std::abs(std::sin(moving.rot[2]) * dy - std::cos(moving.rot[2]) * dx) < .001,
            "Travel heading is not tangent to the positional path");
        require(sampleCartRoute(straight, cartRouteLength(straight)).rot[2] == straight.points.back().position.rot[2],
            "Authored arrival rotation was discarded");
        require(std::abs(advanceRouteHeading(0, 1.5f, 2, .1) - .2) < .00001,
            "A route corner bypasses the authored angular speed");
        require(std::abs(advanceRouteHeading(0, .1f, 2, .1) - .1) < .00001,
            "Turning overshoots the destination heading");
        require(std::abs(advanceRouteHeading(3.1f, -3.1f, 1, .01) - 3.11) < .00001,
            "Turning does not take the shortest path across the angle wrap");
        float stepped = 0;
        for (int i = 0; i < 10; ++i) stepped = advanceRouteHeading(stepped, 1.5f, 2, .01);
        require(std::abs(stepped - advanceRouteHeading(0, 1.5f, 2, .1)) < .00001,
            "Actor turning depends on frame rate");
        std::cout << "PASS: continuous patrol joins, authored corners, no passed-marker replay\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
