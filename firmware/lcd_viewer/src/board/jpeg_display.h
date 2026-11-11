#pragma once
#include <cstdint>
#include <cstddef>
namespace lcd {
bool jpegDisplayBegin();
bool decodeJpeg(const uint8_t* jpeg,size_t length);
const uint16_t* jpegPixels();
}
