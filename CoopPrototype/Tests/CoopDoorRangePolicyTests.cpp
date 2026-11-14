#include "../Src/CoopDoorRangePolicy.h"

#include <cassert>
#include <limits>

struct Point { float x, y, z; };

int main()
{
    using namespace CoopDoorRangePolicy;
    const Point box{4, 6, 3};
    assert(Contains(box, Point{0, 0, 0}));
    assert(Contains(box, Point{2, -3, 3}));
    assert(Contains(box, Point{0, 2.44946f, -0.000030517578125f}));
    assert(Contains(box, Point{2.0001f, -3.0001f, 3.0001f}));
    assert(!Contains(box, Point{2.01f, 0, 1}));
    assert(!Contains(box, Point{0, 3.01f, 1}));
    assert(!Contains(box, Point{0, 0, -0.01f}));
    assert(!Contains(box, Point{0, 0, 3.01f}));
    assert(!Contains(Point{0, 6, 3}, Point{0, 0, 1}));
    assert(!Contains(box, Point{std::numeric_limits<float>::quiet_NaN(), 0, 1}));
    assert(!Contains(Point{4, std::numeric_limits<float>::infinity(), 3}, Point{0, 0, 1}));

    assert(AcceptNetworkEdge(true, true));
    assert(!AcceptNetworkEdge(false, true));
    assert(!AcceptNetworkEdge(true, false));
    assert(AcceptNetworkEdge(false, false));
    assert(AcceptNetworkEdge(true, false, true));
    assert(AcceptNetworkEdge(true, true, true));
    assert(!AcceptNetworkEdge(false, true, true));
    assert(AcceptNetworkEdge(false, false, true));

    // Neither repeated opens nor arbitrary elapsed frames can create occupancy.
    for (int frame = 0; frame != 100000; ++frame)
    {
        assert(!AcceptNetworkEdge(true, false));
        assert(!AcceptNetworkEdge(false, true));
    }
    const Point outside{10, 0, 1}, inside{0, 0, 1};
    assert(!AcceptNetworkEdge(false, Contains(box, inside) || Contains(box, outside)));
    assert(AcceptNetworkEdge(false, Contains(box, outside) || Contains(box, outside)));
}
