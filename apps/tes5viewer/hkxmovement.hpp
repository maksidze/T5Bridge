#ifndef TES5VIEWER_HKXMOVEMENT_HPP
#define TES5VIEWER_HKXMOVEMENT_HPP
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace TES5Viewer
{
    // Resolve the movement-state tags which own the normal forward walk/run
    // clips. Names come from iState_* constants, never from debug node names.
    // Empty/ambiguous results leave the caller's RACE/DOBJ fallback intact.
    std::array<std::string, 2> hkxLocomotionTypes(const std::string& project,
        const std::function<std::string(const std::string&)>& read);
    std::string hkxBehaviorPath(const std::string& project,
        const std::function<std::string(const std::string&)>& read);
    std::string hkxObjectRig(const std::string& project,
        const std::function<std::string(const std::string&)>& read);
    std::vector<std::string> hkxObjectBones(std::string bytes);
    struct HkxObjectAnimation
    {
        std::string sequence;
        std::string clip;
        float speed = 1, cropStart = 0, cropEnd = 0, start = 0;
        std::vector<std::pair<float, std::string>> events;
        std::vector<std::pair<float, std::string>> endRelativeEvents;
    };
    HkxObjectAnimation hkxObjectAnimation(std::string bytes, std::string_view event);
    float hkxClipDuration(std::string bytes);
}
#endif
