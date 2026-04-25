# Projet : Waveshare ESP32-S3-PhotoPainter

Afficheur e-paper 7.3" ACeP 7 couleurs, 800x480 px. Framework Arduino IDE.

## Hardware

**MCU** : ESP32-S3 @ 240 MHz, OPI PSRAM 8 MB

**PMU** : AXP2101 (I2C addr 0x34)
- SDA = GPIO 47, SCL = GPIO 48
- ALDO3 alimente l'écran, ALDO4 les périphériques
- **Doit être initialisé en premier** — sans `enableALDO3/4()`, l'écran n'a aucune alimentation

**E-Paper** : 7.3" ACeP, SPI
- MOSI=11, MISO=-1 (non connecté), SCK=10, DC=8, CS=9, RST=12, BUSY=13
- BUSY = LOW pendant le refresh, HIGH quand prêt
- Refresh complet : ~30-40 secondes

**LEDs** : active bas (LOW = allumé)
- Vert = GPIO 42, Rouge = GPIO 45

**Bouton KEY** : GPIO 4

## Bibliothèques Arduino

| Bibliothèque       | Auteur           | Version  |
|--------------------|------------------|----------|
| GxEPD2             | Jean-Marc Zingg  | >= 1.6.2 |
| Adafruit GFX       | Adafruit         | >= 1.11.9|
| XPowersLib         | Lewis He         | >= 0.2.0 |
| ArduinoJson        | Benoit Blanchon  | >= 7.0.0 |

## Réglages Arduino IDE

| Paramètre          | Valeur                    |
|--------------------|---------------------------|
| Board              | ESP32S3 Dev Module        |
| USB Mode           | Hardware CDC and JTAG     |
| USB CDC On Boot    | Enabled                   |
| Flash Mode         | QIO 80MHz                 |
| PSRAM              | OPI PSRAM                 |
| Upload Speed       | 921600                    |
| Monitor Speed      | 115200                    |

## Séquence d'initialisation obligatoire

```cpp
// 1. PMU en premier — alimente l'écran
Wire.begin(47, 48);
PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, 47, 48);
PMU.setALDO3Voltage(3300); PMU.enableALDO3();
PMU.setALDO4Voltage(3300); PMU.enableALDO4();
delay(2000);  // stabilisation des rails

// 2. États initiaux des broches EPD
pinMode(EPD_RST, OUTPUT); digitalWrite(EPD_RST, HIGH);
pinMode(EPD_DC,  OUTPUT); digitalWrite(EPD_DC,  LOW);
pinMode(EPD_CS,  OUTPUT); digitalWrite(EPD_CS,  HIGH);
pinMode(EPD_BUSY, INPUT);

// 3. SPI + reset matériel + init display
SPI.begin(10, -1, 11, 9);
epd_deep_init();  // RST HIGH->LOW->HIGH + attente BUSY HIGH
display.init(115200, true, 2, false);  // sans paramètre SPISettings
display.setRotation(2);  // 180° — orientation physique de la carte
delay(200);
```

## Driver GxEPD2

```cpp
#include <GxEPD2_7C.h>  // inclut GxEPD2_730c_GDEP073E01

GxEPD2_7C<GxEPD2_730c_GDEP073E01, GxEPD2_730c_GDEP073E01::HEIGHT> display(
    GxEPD2_730c_GDEP073E01(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
```

**Ne pas utiliser** `GxEPD2_730c_ACeP_730` — mauvais driver pour ce hardware.

## Rendu e-paper

```cpp
display.setFullWindow();
display.firstPage();
do {
    display.fillScreen(GxEPD_WHITE);
    // ... dessin ...
} while (display.nextPage());
display.hibernate();  // conserve l'image, coupe le SPI
```

Couleurs disponibles : `GxEPD_BLACK`, `GxEPD_WHITE`, `GxEPD_RED`,
`GxEPD_GREEN`, `GxEPD_BLUE`, `GxEPD_YELLOW`.

## Pièges connus

- **PMU absent = écran sans alimentation** : ne pas appeler `SPI.begin` avant `PMU.enableALDO3/4()`
- **BUSY polarity** : attendre `while (digitalRead(BUSY) == LOW)` — LOW = occupé
- **`display.init()` sans SPISettings** : passer les 4 arguments simples, pas l'objet `SPISettings`
- **LEDs active bas** : `LOW` = allumé, `HIGH` = éteint
- **Fonts ASCII uniquement** : les caractères accentués (é, è, ù) ne s'affichent pas avec FreeMonoBold

## Structure du projet

```
Hello_World/
├── Hello_World.ino   — sketch principal
├── pins.h            — définitions des broches
└── secrets.h         — credentials WiFi + Strava (ne pas committer)
```

## Git

Dépôt GitHub : https://github.com/babalneuve/Strav_e-paper.git

**Après chaque commit impliquant au moins un fichier versionné, faire un push automatiquement.**
