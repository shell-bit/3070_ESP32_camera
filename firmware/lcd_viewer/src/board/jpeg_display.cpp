#include "jpeg_display.h"
#include "board_port.h"
#include "../preview/mjpeg_parser.h"
#include <esp_heap_caps.h>
#include <jpeg_decoder.h>
namespace lcd {
namespace { uint16_t* pixels=nullptr;uint16_t* candidate=nullptr; }
bool jpegDisplayBegin() {
    pixels=static_cast<uint16_t*>(heap_caps_calloc(kWidth*kHeight,sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    candidate=static_cast<uint16_t*>(heap_caps_malloc(kWidth*kHeight*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    return pixels && candidate;
}
const uint16_t* jpegPixels() { return pixels; }
bool decodeJpeg(const uint8_t* jpeg,size_t length) {
    if(!pixels || !jpeg || length<4 || length>kMaxJpegBytes)return false;
    esp_jpeg_image_cfg_t config={};
    config.indata=const_cast<uint8_t*>(jpeg);config.indata_size=length;
    config.out_format=JPEG_IMAGE_FORMAT_RGB565;config.out_scale=JPEG_IMAGE_SCALE_0;
    config.flags.swap_color_bytes=0;
    esp_jpeg_image_output_t result={};
    if(esp_jpeg_get_image_info(&config,&result)!=ESP_OK || result.width!=kWidth || result.height!=kHeight || result.output_len!=kWidth*kHeight*2)return false;
    config.outbuf=reinterpret_cast<uint8_t*>(candidate);config.outbuf_size=kWidth*kHeight*2;
    if(esp_jpeg_decode(&config,&result)!=ESP_OK)return false;
    // A malformed JPEG cannot partially overwrite the last valid picture.
    auto* previous=pixels;pixels=candidate;candidate=previous;return true;
}
}
