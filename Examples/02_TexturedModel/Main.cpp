#include "ExampleRunner.h"

int main(int argc, char** argv)
{
    return Halcyon::Examples::run({"Halcyon Example 02 - Textured Model",
                                      "assets/models/monkey/color.png",
                                      "assets/models/monkey/monkey.obj"},
        argc,
        argv);
}
