#include <mist/defines.h>

#define main nukeFixtureMain
#include "../src/utils/util_nuke.cpp"
#undef main

int main(int argc, char **argv) {
  // The real nuke has positional arguments only; these fixtures synchronize on
  // its INFO barriers.
  Util::printDebugLevel = DLVL_DEVEL;
  return nukeFixtureMain(argc, argv);
}
