#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#define HIGH 1
#define LOW 0
extern bool fakePsram;
inline bool psramFound(){return fakePsram;}
