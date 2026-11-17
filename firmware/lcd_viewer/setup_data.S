#ifdef LCD_CAMERA_JSON_FILE
#error "LCD_CAMERA_JSON_FILE is no longer supported; use LCD_SETUP_JSON_FILE with full setup.json"
#endif

    .section .rodata.lcd_setup,"a",@progbits
    .global lcd_setup_json
    .type lcd_setup_json,@object
lcd_setup_json:
#ifdef LCD_SETUP_JSON_FILE
    .incbin LCD_SETUP_JSON_FILE
#endif
.Lsetup_end:
    .byte 0
    .size lcd_setup_json, . - lcd_setup_json

    .balign 4
    .global lcd_setup_size
    .type lcd_setup_size,@object
lcd_setup_size:
    .4byte .Lsetup_end - lcd_setup_json
    .size lcd_setup_size, . - lcd_setup_size

#ifdef LCD_SETUP_JSON_FILE
    // Register the JSON with GCC dependency tracking for incremental IDE builds.
    .if 0
#include LCD_SETUP_JSON_FILE
    .endif
#endif
