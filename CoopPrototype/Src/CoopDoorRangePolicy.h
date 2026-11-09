#pragma once

#include <cmath>

namespace CoopDoorRangePolicy
{
// ArkDoor's authored box is centered on X/Y and starts at Z=0.
template<class Point>
bool Contains(const Point& dimensions, const Point& localPoint)
{
    // Native grounded roots can settle just below the authored floor plane.
    constexpr float boundaryEpsilon = 0.001f;
    return std::isfinite(dimensions.x) && std::isfinite(dimensions.y) &&
        std::isfinite(dimensions.z) && dimensions.x > 0 && dimensions.y > 0 && dimensions.z > 0 &&
        std::isfinite(localPoint.x) && std::isfinite(localPoint.y) && std::isfinite(localPoint.z) &&
        std::abs(localPoint.x) <= dimensions.x * 0.5f + boundaryEpsilon &&
        std::abs(localPoint.y) <= dimensions.y * 0.5f + boundaryEpsilon &&
        localPoint.z >= -boundaryEpsilon && localPoint.z <= dimensions.z + boundaryEpsilon;
}

inline bool AcceptNetworkEdge(bool opening, bool occupied, bool explicitOpen = false, bool senderInside = false)
{
    return opening ? occupied || explicitOpen || senderInside : !occupied;
}
}
