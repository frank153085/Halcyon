#include "ExampleRunner.h"

int main(int argc, char** argv)
{
    Halcyon::Examples::ExampleDefinition definition{};
    definition.title = "Halcyon Example 01 - Triangle";
    return Halcyon::Examples::run(definition, argc, argv);
}
