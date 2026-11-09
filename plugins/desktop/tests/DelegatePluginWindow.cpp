#include "DelegateTestWindow.hpp"

std::unique_ptr<DesktopWindow> CreateDesktopWindow()
{
    return std::make_unique<DelegateTestWindow>();
}
