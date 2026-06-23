# 7x7 LED Matrix — ESP32-C6 + HT16K33
 
A compact 7x7 red SMD LED matrix controller built around the HT16K33 LED driver and ESP32-C6 SuperMini. Designed as a custom PCB with a single push button for interaction.
 
## Hardware
 
| Component | Details |
|---|---|
| Microcontroller | ESP32-C6 SuperMini |
| LED Driver | HT16K33 (I2C) |
| LEDs | 49× 0805 Red SMD |
| Button | 6x6mm tactile switch |
| I2C Pull-up Resistors | 2× 10kΩ (SDA, SCL) |
| Address Resistors | 3× 10kΩ (A0, A1, A2 → GND, I2C addr 0x70) |
| PCB Size | 42.67 × 48.41 mm, 2-layer |
 
## Schematic & PCB
 
Designed in EasyEDA. Gerber files are available in the `/gerber` folder.
 
- 2-layer PCB
- 0805 SMD LEDs arranged in a 7×7 matrix
- HT16K33 controls rows and columns via I2C
- ESP32-C6 SuperMini communicates with HT16K33 over I2C (SDA/SCL)
- Single tactile button connected to ESP32 GPIO
## Wiring
 
```
ESP32-C6  →  HT16K33
SDA       →  SDA
SCL       →  SCL
3.3V      →  VDD
GND       →  GND
```
