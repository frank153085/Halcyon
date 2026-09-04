#include "ExampleRunner.h"

int main(int argc, char** argv)
{
    Halcyon::Examples::ExampleDefinition definition{};
    definition.title = "Halcyon Example 02 - Textured Model";
    definition.scene.name = "textured-monkey";
    definition.scene.assets.push_back(
        {"monkey", std::filesystem::path{"models/monkey/monkey.gltf"}});
    definition.scene.instances.push_back({"main", "monkey"});
    definition.animatedInstanceName = "main";
    return Halcyon::Examples::run(definition, argc, argv);
}
