#include "ExampleRunner.h"

int main(int argc, char** argv)
{
    Halcyon::Examples::ExampleDefinition definition{};
    definition.title = "Halcyon Example 02 - Textured Model";
    definition.startupTexturePath = "assets/models/monkey/color.png";
    definition.startupMeshPath = "assets/models/monkey/monkey.obj";
    return Halcyon::Examples::run(definition, argc, argv);
}
