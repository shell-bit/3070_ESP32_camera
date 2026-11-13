#pragma once
#include <cstdint>
#include <cstddef>
enum framesize_t { FRAMESIZE_FHD, FRAMESIZE_VGA, FRAMESIZE_QVGA };
enum pixformat_t { PIXFORMAT_JPEG };
struct camera_config_t { framesize_t frame_size; };
struct camera_fb_t { uint8_t*buf;size_t len;uint16_t width,height;pixformat_t format; };
struct sensor_t {struct {int PID;} id;int(*set_reg)(sensor_t*,int,int,int);int(*set_framesize)(sensor_t*,framesize_t);int(*set_quality)(sensor_t*,int);int(*set_vflip)(sensor_t*,int);};
constexpr int OV5640_PID=0x5640,ESP_OK=0;
int esp_camera_init(camera_config_t*);
int esp_camera_deinit();
sensor_t*esp_camera_sensor_get();
camera_fb_t*esp_camera_fb_get();
void esp_camera_fb_return(camera_fb_t*);
