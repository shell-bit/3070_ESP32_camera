#include "board_port.h"
#include <SPI.h>
#include <Wire.h>
#include <esp_heap_caps.h>

// Pin map and panel initialization: Waveshare ESP32-S3-Touch-LCD-2.8 V2 demo.
// See THIRD_PARTY.md for exact archive hash. GPIO6/7 control battery power;
// neither battery key nor RESET is repurposed as a preview button.
namespace lcd {
namespace {
constexpr int kMosi=45,kClock=40,kCs=42,kDc=41,kReset=39,kBacklight=5;
constexpr int kTouchSda=1,kTouchScl=3,kTouchReset=2,kTouchInt=4,kBoot=0,kPowerHold=7;
constexpr uint8_t kTouchAddress=0x58;
SPIClass panel(FSPI);
lv_disp_draw_buf_t drawBuffer;
lv_color_t* drawPixels=nullptr;
bool touchAvailable=false;
uint32_t previousTick=0;
void command(uint8_t command,const uint8_t* bytes=nullptr,size_t count=0) {
    panel.beginTransaction(SPISettings(40000000,MSBFIRST,SPI_MODE0));
    digitalWrite(kCs,LOW);digitalWrite(kDc,LOW);panel.transfer(command);
    digitalWrite(kDc,HIGH);if(count)panel.writeBytes(bytes,count);
    digitalWrite(kCs,HIGH);panel.endTransaction();
}
void registerValue(uint8_t reg,std::initializer_list<uint8_t> data) { command(reg,data.begin(),data.size()); }
void flush(lv_disp_drv_t* driver,const lv_area_t* area,lv_color_t* pixels) {
    registerValue(0x2a,{uint8_t(area->x1>>8),uint8_t(area->x1),uint8_t(area->x2>>8),uint8_t(area->x2)});
    registerValue(0x2b,{uint8_t(area->y1>>8),uint8_t(area->y1),uint8_t(area->y2>>8),uint8_t(area->y2)});
    command(0x2c,reinterpret_cast<uint8_t*>(pixels),(area->x2-area->x1+1)*(area->y2-area->y1+1)*2);
    // Synchronous transfer: no queued DMA references survive flush_ready.
    lv_disp_flush_ready(driver);
}
bool touchRegister(uint32_t reg,uint8_t* bytes=nullptr,size_t n=0) {
    Wire1.beginTransmission(kTouchAddress);
    for(int shift=24;shift>=0;shift-=8)Wire1.write(uint8_t(reg>>shift));
    if(Wire1.endTransmission(n==0)!=0)return false;
    if(!n)return true;
    if(Wire1.requestFrom(kTouchAddress,n)!=n)return false;
    for(size_t i=0;i<n;++i)bytes[i]=uint8_t(Wire1.read());
    return true;
}
void readTouch(lv_indev_drv_t*,lv_indev_data_t* data) {
    data->state=LV_INDEV_STATE_REL;
    if(!touchAvailable)return;
    uint8_t bytes[9];
    const bool read=touchRegister(0xd0070000,bytes,sizeof bytes);
    touchRegister(0xd00002ab); // CST3530 ACK is a four-byte register write, no payload.
    if(!read || !(bytes[3]&0x0f) || (bytes[3]&0x0f)>5 || !(bytes[8]&0xf0))return;
    const uint16_t x=((bytes[7]&0x0f)<<8)|bytes[4];
    const uint16_t y=((bytes[7]&0xf0)<<4)|bytes[5];
    if(x>=240 || y>=320)return;
    data->point.x=y;data->point.y=239-x;data->state=LV_INDEV_STATE_PR;
}
}
bool boardBegin() {
    static_assert(LVGL_VERSION_MAJOR==8 && LVGL_VERSION_MINOR==3 && LVGL_VERSION_PATCH==10,"Use pinned LVGL 8.3.10");
    static_assert(LV_COLOR_DEPTH==16 && LV_COLOR_16_SWAP==0,"Panel expects native RGB565 with RAMCTRL E8");
    pinMode(kPowerHold,OUTPUT);digitalWrite(kPowerHold,HIGH);
    pinMode(kBoot,INPUT_PULLUP);
    pinMode(kCs,OUTPUT);pinMode(kDc,OUTPUT);pinMode(kReset,OUTPUT);
    digitalWrite(kCs,HIGH);digitalWrite(kReset,LOW);delay(50);digitalWrite(kReset,HIGH);delay(120);
    panel.begin(kClock,-1,kMosi,kCs);
    command(0x11);delay(120);
    registerValue(0x36,{0x60}); // MX + MV: landscape; matching CST3530 rotation above.
    registerValue(0x3a,{0x05});
    registerValue(0xb0,{0x00,0xe8}); // Official V2 RAMCTRL, little-endian RGB565.
    registerValue(0xb2,{0x0c,0x0c,0x00,0x33,0x33});registerValue(0xb7,{0x75});
    registerValue(0xbb,{0x1a});registerValue(0xc0,{0x2c});registerValue(0xc2,{0x01,0xff});
    registerValue(0xc3,{0x13});registerValue(0xc4,{0x20});registerValue(0xc6,{0x0f});
    registerValue(0xd0,{0xa4,0xa1});registerValue(0xd6,{0xa1});
    registerValue(0xe0,{0xd0,0x0d,0x14,0x0d,0x0d,0x09,0x38,0x44,0x4e,0x3a,0x17,0x18,0x2f,0x30});
    registerValue(0xe1,{0xd0,0x09,0x0f,0x08,0x07,0x14,0x37,0x44,0x4d,0x38,0x15,0x16,0x2c,0x2e});
    command(0x21);command(0x29);
    ledcAttach(kBacklight,20000,10);ledcWrite(kBacklight,0);
    Wire1.begin(kTouchSda,kTouchScl,400000);Wire1.setTimeOut(15);
    pinMode(kTouchInt,INPUT);pinMode(kTouchReset,OUTPUT);
    digitalWrite(kTouchReset,LOW);delay(100);digitalWrite(kTouchReset,HIGH);delay(500);
    Wire1.beginTransmission(kTouchAddress);touchAvailable=Wire1.endTransmission()==0;
    drawPixels=static_cast<lv_color_t*>(heap_caps_malloc(kWidth*20*sizeof(lv_color_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(!drawPixels)return false;
    lv_init();lv_disp_draw_buf_init(&drawBuffer,drawPixels,nullptr,kWidth*20);
    static lv_disp_drv_t display;lv_disp_drv_init(&display);
    display.hor_res=kWidth;display.ver_res=kHeight;display.flush_cb=flush;display.draw_buf=&drawBuffer;
    lv_disp_drv_register(&display);
    static lv_indev_drv_t input;lv_indev_drv_init(&input);input.type=LV_INDEV_TYPE_POINTER;input.read_cb=readTouch;lv_indev_drv_register(&input);
    previousTick=millis();return true;
}
void boardBacklight(bool on) { ledcWrite(kBacklight,on?600:0); }
bool boardTouchAvailable() { return touchAvailable; }
void boardUiTick(uint32_t now) { lv_tick_inc(now-previousTick);previousTick=now;lv_timer_handler(); }
bool boardButtonPressed(uint32_t now) {
    static bool raw=HIGH,stable=HIGH;static uint32_t changed=0;
    const bool current=digitalRead(kBoot);
    if(current!=raw) { raw=current;changed=now; }
    if(current!=stable && now-changed>=35) { stable=current;return stable==LOW; }
    return false;
}
}
