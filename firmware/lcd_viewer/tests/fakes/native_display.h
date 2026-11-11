#pragma once

#include <lvgl.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        std::exit(1); \
    } \
} while (false)

namespace native {

constexpr int width = 320;
constexpr int height = 240;
using Pixels = std::array<uint16_t, width * height>;

class Display {
public:
    Display() {
        static_assert(LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0, "Native fixture uses unswapped RGB565");
        lv_init();
        lv_disp_draw_buf_init(&drawBuffer_, drawPixels_.data(), nullptr, drawPixels_.size());
        lv_disp_drv_init(&driver_);
        driver_.hor_res = width;
        driver_.ver_res = height;
        driver_.draw_buf = &drawBuffer_;
        driver_.user_data = this;
        driver_.flush_cb = flush;
        display_ = lv_disp_drv_register(&driver_);
        CHECK(display_ != nullptr);

        lv_indev_drv_init(&pointerDriver_);
        pointerDriver_.type = LV_INDEV_TYPE_POINTER;
        pointerDriver_.disp = display_;
        pointerDriver_.user_data = this;
        pointerDriver_.read_cb = readPointer;
        CHECK(lv_indev_drv_register(&pointerDriver_) != nullptr);
    }

    void render() {
        lv_obj_update_layout(lv_scr_act());
        lv_refr_now(display_);
    }

    void tap(lv_coord_t x, lv_coord_t y) {
        pointer_.point = {x, y};
        pointer_.state = LV_INDEV_STATE_PRESSED;
        lv_tick_inc(40);
        lv_timer_handler();
        pointer_.state = LV_INDEV_STATE_RELEASED;
        lv_tick_inc(40);
        lv_timer_handler();
        render();
    }

    void tap(lv_obj_t* target) {
        CHECK(target != nullptr);
        lv_area_t bounds;
        lv_obj_get_coords(target, &bounds);
        tap((bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2);
    }

    const Pixels& pixels() const { return pixels_; }

private:
    static void flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* colors) {
        auto& self = *static_cast<Display*>(driver->user_data);
        CHECK(area->x1 >= 0 && area->x2 < width && area->y1 >= 0 && area->y2 < height);
        for (int y = area->y1; y <= area->y2; ++y) {
            for (int x = area->x1; x <= area->x2; ++x) {
                self.pixels_[y * width + x] = (colors++)->full;
            }
        }
        lv_disp_flush_ready(driver);
    }

    static void readPointer(lv_indev_drv_t* driver, lv_indev_data_t* data) {
        *data = static_cast<Display*>(driver->user_data)->pointer_;
    }

    Pixels pixels_{};
    std::array<lv_color_t, width * 12> drawPixels_{};
    lv_disp_draw_buf_t drawBuffer_{};
    lv_disp_drv_t driver_{};
    lv_disp_t* display_ = nullptr;
    lv_indev_drv_t pointerDriver_{};
    lv_indev_data_t pointer_{};
};

inline void visibleObjects(lv_obj_t* object, std::vector<lv_obj_t*>& result) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    result.push_back(object);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i) {
        visibleObjects(lv_obj_get_child(object, i), result);
    }
}

inline std::vector<lv_obj_t*> visibleObjects() {
    std::vector<lv_obj_t*> result;
    visibleObjects(lv_scr_act(), result);
    return result;
}

inline lv_obj_t* findLabel(const char* text, bool exact = true) {
    for (auto* object : visibleObjects()) {
        if (!lv_obj_check_type(object, &lv_label_class)) continue;
        const char* label = lv_label_get_text(object);
        if (exact ? std::strcmp(label, text) == 0 : std::strstr(label, text) != nullptr) return object;
    }
    return nullptr;
}

inline lv_obj_t* findButton(const char* caption) {
    auto* button = findLabel(caption);
    while (button && !lv_obj_has_flag(button, LV_OBJ_FLAG_CLICKABLE)) {
        button = lv_obj_get_parent(button);
    }
    if (!button || button == lv_scr_act()) return nullptr;
    return button;
}

inline lv_obj_t* frameImage() {
    for (auto* object : visibleObjects()) {
        if (lv_obj_check_type(object, &lv_img_class) &&
            lv_obj_get_width(object) == width && lv_obj_get_height(object) == height) {
            return object;
        }
    }
    return nullptr;
}

// Every visible row and column has a distinct pattern to expose scaling or misalignment.
inline Pixels cameraFixture() {
    Pixels pixels{};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int red = (x * 31 / (width - 1) + y / 24) % 32;
            const int green = (y * 63 / (height - 1) + x / 24) % 64;
            const int blue = (x / 12 + y / 12) % 32;
            pixels[y * width + x] = static_cast<uint16_t>((red << 11) | (green << 5) | blue);
        }
    }
    return pixels;
}

}
