#include "processor/model/ResourcePath.hpp"

#include <iostream>

int main()
{
    if (ResolveResourcePath("/app/", "shader.glsl") != "/app/shader.glsl" ||
        ResolveResourcePath("/app/", "/models/a.model3.json") != "/models/a.model3.json")
        return 1;

    std::cout << "explicit component inputs and paths passed\n";
}
