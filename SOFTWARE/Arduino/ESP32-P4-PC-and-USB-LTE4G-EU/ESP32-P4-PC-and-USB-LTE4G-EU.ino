#include <Arduino.h>

/*
  ESP32-P4-PC <-> USB-LTE4G-EU UART demo

  Goal:
    This sketch tests UART communication between an OLIMEX ESP32-P4-PC
    and an OLIMEX USB-LTE4G-EU / USB-LTE4G-EU-ANT modem board.

  Purpose:
    The ESP32-P4-PC sends basic AT commands to the LTE modem and prints
    the modem responses to the Arduino Serial Monitor. After startup, the
    sketch waits for manual AT command input from the Serial Monitor.

  Important hardware notes:
    The USB-LTE4G-EU modem UART works at 1.8 V logic level.
    The ESP32-P4-PC UEXT UART works at 3.3 V logic level.

    A level shifter is required between the two boards.
    OLIMEX UEXT-LVL-SHIFT can be used for this purpose.

  Connection:
    ESP32-P4-PC <-> UEXT-LVL-SHIFT:
      Connect these two boards with a UEXT cable.

    USB-LTE4G-EU:
      Power the USB-LTE4G-EU board from its own USB cable.

    USB-LTE4G-EU EXT1 header <-> UEXT-LVL-SHIFT UEXT_1.65V_to_5V5:
      Use 4 male-female jumper wires from the USB-LTE4G-EU EXT1 header
      to the low-voltage side of the UEXT-LVL-SHIFT board:

      USB-LTE4G-EU EXT1 pin 8 / VDD_EXT -> pin 1 of UEXT_1.65V_to_5V5
      USB-LTE4G-EU EXT1 pin 2 / GND     -> pin 2 of UEXT_1.65V_to_5V5
      USB-LTE4G-EU EXT1 pin 3 / TX      -> pin 4 of UEXT_1.65V_to_5V5
      USB-LTE4G-EU EXT1 pin 4 / RX      -> pin 3 of UEXT_1.65V_to_5V5

  Modem sleep note:
    The LTE module can enter sleep mode. This demo sends:

      AT+QSCLK=0

    during startup to disable modem sleep mode.
*/

static const int LTE_RX_PIN = 38;        // P4 RX <- LTE TXD via level shifter
static const int LTE_TX_PIN = 37;        // P4 TX -> LTE RXD via level shifter
static const uint32_t LTE_BAUD = 115200;

HardwareSerial LTE(1);

char inputLine[160];
size_t inputLen = 0;
uint32_t lastWaitingPrint = 0;

void sendCommand(const char *cmd, uint32_t waitMs = 1500) {
  Serial.println();
  Serial.print(">> ");
  Serial.println(cmd);

  while (LTE.available()) {
    LTE.read();
  }

  LTE.print(cmd);
  LTE.print("\r\n");

  bool gotResponse = false;
  uint32_t start = millis();

  while (millis() - start < waitMs) {
    while (LTE.available()) {
      Serial.write(LTE.read());
      gotResponse = true;
    }
    delay(1);
  }

  if (!gotResponse) {
    Serial.println("(no bytes received from LTE UART)");
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("=== ESP32-P4-PC LTE UART DEMO ===");
  Serial.println("Console: Serial over USB CDC, 115200 baud");
  Serial.println("Modem:   UART1, 115200 baud");
  Serial.println();

  Serial.println("Purpose:");
  Serial.println("  Test AT command communication with USB-LTE4G-EU over UART.");
  Serial.println("  A level shifter is required: modem UART is 1.8 V, P4 UEXT is 3.3 V.");
  Serial.println("  The USB-LTE4G-EU board must be powered from USB.");
  Serial.println();

  Serial.printf("Chip: %s rev %u\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("Flash: %lu MB\n", (unsigned long)(ESP.getFlashChipSize() / 1024 / 1024));
  Serial.printf("PSRAM: %lu MB\n", (unsigned long)(ESP.getPsramSize() / 1024 / 1024));
  Serial.println();

  LTE.begin(LTE_BAUD, SERIAL_8N1, LTE_RX_PIN, LTE_TX_PIN);
  delay(2000);

  Serial.println("Startup modem probe:");
  sendCommand("AT");
  sendCommand("AT");
  sendCommand("AT");
  sendCommand("AT+QSCLK=0");   // Disable modem sleep mode.
  sendCommand("ATI");
  sendCommand("AT+CPIN?");
  sendCommand("AT+CSQ");

  Serial.println();
  Serial.println("Ready. Type AT commands and press Enter.");
  Serial.println("Examples: AT, ATI, AT+CPIN?, AT+CSQ, AT+CEREG?, AT+COPS?");
  Serial.println();

  lastWaitingPrint = millis();
}

void loop() {
  while (LTE.available()) {
    Serial.write(LTE.read());
  }

  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      if (inputLen > 0) {
        inputLine[inputLen] = '\0';
        sendCommand(inputLine);
        inputLen = 0;
        lastWaitingPrint = millis();
      }
      continue;
    }

    if (inputLen < sizeof(inputLine) - 1) {
      inputLine[inputLen++] = c;
    } else {
      inputLen = 0;
      Serial.println("Input too long, cleared.");
    }
  }

  if (millis() - lastWaitingPrint >= 30000) {
    Serial.println("Waiting for AT command input...");
    Serial.println("Try: AT, ATI, AT+CPIN?, AT+CSQ, AT+CEREG?, AT+COPS?");
    lastWaitingPrint = millis();
  }
}
