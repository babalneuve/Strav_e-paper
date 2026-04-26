# Projet : Waveshare ESP32-S3-PhotoPainter

Afficheur e-paper 7.3" ACeP 7 couleurs, 800x480 px. Framework Arduino IDE.

## Hardware

**MCU** : ESP32-S3 @ 240 MHz, OPI PSRAM 8 MB

**PMU** : AXP2101 (I2C addr 0x34)
- SDA = GPIO 47, SCL = GPIO 48
- ALDO1-4 tous activés à 3.3V (ALDO3 alimente l'écran, ALDO4 les périphériques)
- **Doit être initialisé en premier** — sans `enableALDO3/4()`, l'écran n'a aucune alimentation

**E-Paper** : 7.3" ACeP, SPI
- MOSI=11, MISO=-1 (non connecté), SCK=10, DC=8, CS=9, RST=12, BUSY=13
- BUSY = LOW pendant le refresh, HIGH quand prêt
- Refresh complet : ~30-40 secondes

**LEDs** : active bas (LOW = allumé)
- Vert = GPIO 42, Rouge = GPIO 45

**Bouton** : `BTN_KEY` = GPIO 4

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
PMU.setALDO1Voltage(3300); PMU.enableALDO1();
PMU.setALDO2Voltage(3300); PMU.enableALDO2();
PMU.setALDO3Voltage(3300); PMU.enableALDO3();
PMU.setALDO4Voltage(3300); PMU.enableALDO4();
delay(2000);  // stabilisation des rails

// 2. États initiaux des broches EPD
pinMode(EPD_RST, OUTPUT); digitalWrite(EPD_RST, HIGH);
pinMode(EPD_DC,  OUTPUT); digitalWrite(EPD_DC,  LOW);
pinMode(EPD_CS,  OUTPUT); digitalWrite(EPD_CS,  HIGH);
pinMode(EPD_BUSY, INPUT);

// 3. SPI + reset matériel + init display (encapsulé dans display_init())
SPI.end();   // évite les conflits lors d'un re-init
delay(100);
SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);
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
`GxEPD_GREEN`, `GxEPD_BLUE`, `GxEPD_YELLOW`, `GxEPD_ORANGE`.

## Cache NVS (Preferences)

Les stats de la dernière activité réussie sont sauvegardées en flash via `Preferences` (namespace `strava_cache`). Chargées automatiquement si le Wi-Fi ou l'API Strava est indisponible.

- La polyline GPS et le stream altitude **ne sont pas** mis en cache — tracé et profil absents en mode fallback
- La détection de nouvelle activité se fait par comparaison de l'`id` (int64_t) Strava
- Le timestamp du dernier check API (`lastcheck`) est aussi sauvegardé séparément via `save_last_check()`

## Refresh automatique

`check_and_refresh()` est appelée depuis `loop()` toutes les **60 minutes** (`REFRESH_INTERVAL_MIN`). Elle compare l'ID de l'activité reçue avec l'ID en cache ; `display_init()` est rappelée pour réveiller l'écran sorti de `hibernate()`.

## Synchronisation NTP

`ntp_get_time_str()` est appelée après chaque connexion WiFi réussie. Elle configure l'heure via `configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org")` (CEST, UTC+2). Le timestamp est affiché en bas à droite de l'écran sous la forme `MAJ: jj/mm HH:MM`.

- `TZ_OFFSET_SEC = 7200` — à ajuster en hiver (3600 pour CET/UTC+1)

## Stats affichées et layout

**5 boîtes stats (panneau gauche) :**
| Rang | Boîte | Label course | Label vélo |
|------|-------|-------------|------------|
| 1 | Large (w=179) | DISTANCE | DISTANCE |
| 1 | Large (w=179) | DUREE | DUREE |
| 2 | Étroite (w=116) | DENIVELE+ | DENIVELE+ |
| 2 | Étroite (w=116) | ALLURE | VITESSE |
| 2 | Étroite (w=116) | ALL.MAX | VIT.MAX |

Sport "à pied" = Run, Walk, Hike → allure (min/km). Autre → vitesse (km/h).

**Panneau droit :**
- `y=78–292` : tracé GPS (h=215) avec marqueurs départ vert / arrivée rouge
- `y=302–426` : profil altitude rempli en bleu (stream Strava `resolution=low`)
- `y=447` : timestamp `MAJ: jj/mm HH:MM` aligné à droite

**Stream altitude :** appel séparé `GET /activities/{id}/streams?keys=altitude,distance&key_by_type=true&resolution=low` après `strava_fetch_last`. Maximum `MAX_STREAM_PTS=200` points.

## Pièges connus

- **PMU absent = écran sans alimentation** : ne pas appeler `SPI.begin` avant `PMU.enableALDO1-4()`
- **BUSY polarity** : attendre `while (digitalRead(BUSY) == LOW)` — LOW = occupé
- **`display.init()` sans SPISettings** : passer les 4 arguments simples, pas l'objet `SPISettings`
- **`SPI.end()` avant `SPI.begin()`** : obligatoire dans `display_init()` pour éviter les conflits sur re-init après `hibernate()`
- **LEDs active bas** : `LOW` = allumé, `HIGH` = éteint
- **Fonts ASCII uniquement** : les caractères accentués (é, è, ù) ne s'affichent pas avec FreeMonoBold

## Structure du projet

```
Strava_E-paper/
├── Strava_E-paper.ino  — sketch principal
├── pins.h              — définitions des broches
├── secrets.h           — credentials WiFi + Strava (ne pas committer)
└── README.md
```

## Git

Dépôt GitHub : https://github.com/babalneuve/Strav_e-paper.git

**Après chaque commit impliquant au moins un fichier versionné, faire un push automatiquement.**
