# Third-party: HX8394 MIPI-DSI panel driver

`esp_lcd_hx8394.c` and `esp_lcd_hx8394.h` come from Waveshare's `waveshare/esp_lcd_hx8394`
component on the [ESP Component Registry](https://components.espressif.com/components/waveshare/esp_lcd_hx8394),
which lists it as MIT. They stay under that license, reproduced below as upstream
ships it, with its placeholders unfilled.

Local changes, none to the panel path: the unused `i2c_bus.h` include is removed, and
`ESP_LCD_HX8394_VER_{MAJOR,MINOR,PATCH}` are defined at the top of the `.c`, because a
plain PlatformIO library build does not inject them.

```
MIT License

Copyright (c) [year] [fullname]

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
