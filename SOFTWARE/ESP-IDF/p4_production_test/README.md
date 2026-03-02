========================= SETUP ===============

The project was tested with ESP-IDF v5.5 (if you already have it just skip to point 5.)

Settup up ESP-IDF 5.5:

1. Download ESP-IDF v5.5:

git clone https://github.com/espressif/esp-idf -b "release/v5.5"

2. Go to esp-idf directory:

cd esp-idf

3. install esp-idf:

./install.sh

4.  Every time you start-up terminal you must go to esp-idf and source the export.sh script:

. ./export.sh

Compile the project:

5. Go to the project and compile

cd ../p4_production_test

6. Compile:

idf.py build

============= EXAMPLE ==========

The Example tests HDMI, SD CARD, USB FLASH, AUDIO.
After the board boots, you will see red, green and blue rectangles
flashing on the screen, and then the screen
will ask to either short-press or long press the BOOT1 button.

If you long press BOOT1, the test starts again.
If you shor press BOOT1, the test continues SD Card and USB FLASH drive.
The SD card and usb flash must be formated in FAT32.

You can long-press the test to repeat or short-press it to continue

Then the audio test starts. It will produce the sound captured from the mic to the headphones.
You can long press to end the test or short press to restart

Then the LAN test starts. It initializes PHY module, then takes IP from DHCP 
and then pings 8.8.8.8 to test conectivity.




