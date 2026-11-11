# Dependency and hardware source pins

- Board: [Waveshare ESP32-S3-Touch-LCD-2.8 V2](https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8).
- [Official V2 example archive](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8-V2-Demo.zip), checked 2026-11-11. SHA256: `b09e8cdfb47212ea94a06ba02a11568dfb70dcc1ea040142067d08e374b29737`.
- `board_port.cpp` adapts the pin map, ST7789 initialization and CST3530 32-bit register/coordinate protocol from `Arduino/examples/LVGL_Arduino/Display_ST7789.*`, `Touch_CST3530.*` and `PWR_Key.h` in that archive. It uses only CST3530, one touch point, landscape coordinates and synchronous SPI.
- The generic archive linked from the Arduino tutorial is V1. Use the separate **V2** link on the [Resources page](https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8/Resources-And-Documents).
- LVGL **8.3.10**, MIT license, from `Arduino/libraries/lvgl` in the pinned archive. The dependency preparation script retains upstream license files. `LV_COLOR_DEPTH=16`, `LV_COLOR_16_SWAP=0`.
- Arduino-ESP32 **3.3.12**, local bundled ESP-IDF **5.5.5** (`b774170ff46`), Xtensa toolchain package **2601**. Core and SDK retain their upstream licenses; no core modifications are required.
- JPEG decoding uses the core's bundled `espressif__esp_jpeg` component, Apache-2.0; no TJpg_Decoder or ArduinoJson installation is needed. JSON uses bundled cJSON.
- [ST7789T3 datasheet](https://files.waveshare.com/wiki/common/ST7789T3_SPEC_V1.0.pdf), RAMCTRL §9.2.1 (page 258): `0xB0: 00 E8` selects little-endian pixel input, matching native LVGL RGB565. [Espressif decoder source](https://github.com/espressif/idf-extra-components/blob/master/esp_jpeg/jpeg_decoder.c) implements native RGB565 with `swap_color_bytes=0`.
- [Official schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8.pdf): BOOT is GPIO0; battery key/control are GPIO6/GPIO7; RESET is separate.

Hardware source inspection and successful compilation do not establish touch calibration, color correctness or timing on a physical board.

## Local 28 px display font

`monitor_font_28.c` copies the generated glyph and kerning tables from
`src/font/lv_font_montserrat_28.c` in the pinned LVGL 8.3.10 archive above
(source SHA256: `c963a5d3f55ed415b2d991f8ea4766797f98692f9253db3d079caf34bc1a661e`).
Only the include path, enable guard and exported symbol differ. This lets the
sketch use the font without changing the board archive's LVGL configuration.
LVGL's generator used Montserrat Medium and Font Awesome 5 glyphs; both source
fonts are in the archive's `scripts/built_in_font` directory.

The generated C source is covered by LVGL's MIT license:

> Copyright (c) 2021 LVGL Kft
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the “Software”), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is furnished
> to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

The glyphs are covered by the SIL Open Font License:

> Copyright 2011 The Montserrat Project Authors
> (https://github.com/JulietaUla/Montserrat).
>
> Copyright (c) Font Awesome. Reserved Font Name: Font Awesome.
>
> SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
>
> PREAMBLE
>
> The goals of the Open Font License (OFL) are to stimulate worldwide
> development of collaborative font projects, to support the font creation
> efforts of academic and linguistic communities, and to provide a free and
> open framework in which fonts may be shared and improved in partnership
> with others.
>
> The OFL allows the licensed fonts to be used, studied, modified and
> redistributed freely as long as they are not sold by themselves. The
> fonts, including any derivative works, can be bundled, embedded,
> redistributed and/or sold with any software provided that any reserved
> names are not used by derivative works. The fonts and derivatives,
> however, cannot be released under any other type of license. The
> requirement for fonts to remain under this license does not apply
> to any document created using the fonts or their derivatives.
>
> DEFINITIONS
>
> "Font Software" refers to the set of files released by the Copyright
> Holder(s) under this license and clearly marked as such. This may
> include source files, build scripts and documentation.
>
> "Reserved Font Name" refers to any names specified as such after the
> copyright statement(s).
>
> "Original Version" refers to the collection of Font Software components
> as distributed by the Copyright Holder(s).
>
> "Modified Version" refers to any derivative made by adding to, deleting,
> or substituting — in part or in whole — any of the components of the
> Original Version, by changing formats or by porting the Font Software to
> a new environment.
>
> "Author" refers to any designer, engineer, programmer, technical
> writer or other person who contributed to the Font Software.
>
> PERMISSION & CONDITIONS
>
> Permission is hereby granted, free of charge, to any person obtaining a
> copy of the Font Software, to use, study, copy, merge, embed, modify,
> redistribute, and sell modified and unmodified copies of the Font
> Software, subject to the following conditions:
>
> 1) Neither the Font Software nor any of its individual components,
> in Original or Modified Versions, may be sold by itself.
>
> 2) Original or Modified Versions of the Font Software may be bundled,
> redistributed and/or sold with any software, provided that each copy
> contains the above copyright notice and this license. These can be
> included either as stand-alone text files, human-readable headers or
> in the appropriate machine-readable metadata fields within text or
> binary files as long as those fields can be easily viewed by the user.
>
> 3) No Modified Version of the Font Software may use the Reserved Font
> Name(s) unless explicit written permission is granted by the corresponding
> Copyright Holder. This restriction only applies to the primary font name as
> presented to the users.
>
> 4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
> Software shall not be used to promote, endorse or advertise any
> Modified Version, except to acknowledge the contribution(s) of the
> Copyright Holder(s) and the Author(s) or with their explicit written
> permission.
>
> 5) The Font Software, modified or unmodified, in part or in whole,
> must be distributed entirely under this license, and must not be
> distributed under any other license. The requirement for fonts to remain
> under this license does not apply to any document created using the
> Font Software.
>
> TERMINATION
>
> This license becomes null and void if any of the above conditions are
> not met.
>
> DISCLAIMER
>
> THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
> EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
> MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
> OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
> COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
> INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
> DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
> FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
> OTHER DEALINGS IN THE FONT SOFTWARE.
