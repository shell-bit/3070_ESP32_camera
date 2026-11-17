    .section .rodata.lcd_camera_setup,"a",@progbits
    .global lcd_camera_setup_json
    .type lcd_camera_setup_json,@object
lcd_camera_setup_json:
#ifdef LCD_CAMERA_JSON_FILE
    .incbin LCD_CAMERA_JSON_FILE
#endif
.Lcamera_setup_end:
    .byte 0
    .size lcd_camera_setup_json, . - lcd_camera_setup_json

    .balign 4
    .global lcd_camera_setup_size
    .type lcd_camera_setup_size,@object
lcd_camera_setup_size:
    .4byte .Lcamera_setup_end - lcd_camera_setup_json
    .size lcd_camera_setup_size, . - lcd_camera_setup_size

#ifdef LCD_CAMERA_JSON_FILE
    // Register the JSON with GCC dependency tracking for incremental IDE builds.
    .if 0
#include LCD_CAMERA_JSON_FILE
    .endif
#endif
