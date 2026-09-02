#pragma once

namespace Halcyon::Examples
{

struct ExampleDefinition
{
    const char* title = "Halcyon Example";
    const char* startupTexturePath = nullptr;
    const char* startupMeshPath = nullptr;
};

int run(const ExampleDefinition& definition, int argc, char** argv);

} // namespace Halcyon::Examples
