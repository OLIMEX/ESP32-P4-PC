# ESP32-P4 HDMI "Stable Video" (no full-framebuffer)

Goal: Get **stable HDMI output** on ESP32-P4 with LT8912B-style MIPI-DSI->HDMI bridge, without external PSRAM.
We avoid full-frame buffers and rely on LVGL + partial draw buffers (like Espressif demos).

## 0) Fetch BSP
```bash
cd components
chmod +x bootstrap.sh
./bootstrap.sh
```

## 1) Build (recommended)
```bash
. $IDF_PATH/export.sh
cd ..

rm -rf build sdkconfig sdkconfig.old managed_components dependencies.lock
idf.py set-target esp32p4
idf.py reconfigure
idf.py menuconfig
```

### Menuconfig settings (critical)
`Board Support Package (ESP32-P4) -> Display -> HDMI (LT8912B)`:

- Resolution: **800x600@60Hz** (or start with 640x480@60Hz for stability testing)
- Color format: **RGB888** (HDMI)
- Frame buffer mode: **Partial / Line buffer / Strip buffer**
- Buffer height: **16** (try 24/32 if stable)
- Double buffer: **OFF**

Then:
```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Notes
- Seeing "MIPI" in logs is normal: the SoC uses MIPI-DSI/DPI internally to feed the HDMI bridge.
- If you hit `no memory for frame buffer`, you are still in "full frame buffer" mode or your buffer height is too big.
