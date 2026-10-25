#include "QtWindowInput.hpp"
#include <cstdlib>
#include <iostream>

int main()
{
    const auto require = [](bool condition) { if (!condition) std::abort(); };
    require(QtModelInputRegion({}).isEmpty());
    require(QtModelInputRegion({{10, 20, 0, 10}, {0, 0, -1, 20}}).isEmpty());
    const QRegion region = QtModelInputRegion({{10, 20, 30, 40}, {70, 80, 10, 10}});
    require(region.contains(QPoint(10, 20)));
    require(region.contains(QPoint(39, 59)));
    require(!region.contains(QPoint(40, 60)));
    require(!region.contains(QPoint(50, 50)));
    require(region.contains(QPoint(75, 85)));
    require(QtWaylandModelKey(23) == 15); // 制表键 Tab。
    require(QtWaylandModelKey(9) == 1); // 退出键 Escape。
    require(QtWaylandModelKey(0) == 0);
    require(QtWaylandModelKey(7) == 0);
    std::cout << "Qt input region and evdev mapping passed\n";
}
