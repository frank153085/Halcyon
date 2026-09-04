#include "ExampleRunner.h"

int main(int argc, char** argv)
{
    Halcyon::Examples::ExampleDefinition definition{};
    definition.title = "Halcyon Example 01 - Triangle";
    definition.scene.name = "triangle";
    definition.scene.assets.push_back({"triangle", Halcyon::Examples::makeTriangleScene()});
    definition.scene.instances.push_back({"main", "triangle"});
    definition.animatedInstanceName = "main";
    return Halcyon::Examples::run(definition, argc, argv);
}
