# Strava Last Activity — Waveshare ESP32-S3-PhotoPainter

Affiche la dernière activité Strava sur un écran e-paper 7.3" ACeP 7 couleurs (800×480 px), monté sur la carte Waveshare ESP32-S3-PhotoPainter.

## Aperçu

L'appareil se connecte au Wi-Fi, interroge l'API Strava toutes les heures, et rafraîchit l'écran uniquement si une nouvelle activité est détectée. L'image est conservée sans alimentation grâce au mode hibernate de l'écran e-paper.

**Informations affichées :**
- Nom, type et date de l'activité
- Distance et durée
- Dénivelé positif
- Allure moyenne et allure max (course à pied) — ou vitesse moyenne et vitesse max (vélo et autres)
- Tracé GPS avec marqueurs départ (vert) / arrivée (rouge)
- Profil altimétrique (distance vs altitude)
- Date et heure du dernier check API (`MAJ: jj/mm HH:MM`)

## Matériel requis

| Composant | Référence |
|-----------|-----------|
| Microcontrôleur | Waveshare ESP32-S3-PhotoPainter |
| Écran | 7.3" ACeP e-paper 7 couleurs (800×480) |
| PMU | AXP2101 (intégré à la carte) |

## Bibliothèques Arduino

À installer via le Gestionnaire de bibliothèques :

| Bibliothèque | Auteur | Version |
|---|---|---|
| GxEPD2 | Jean-Marc Zingg | >= 1.6.2 |
| Adafruit GFX Library | Adafruit | >= 1.11.9 |
| XPowersLib | Lewis He | >= 0.2.0 |
| ArduinoJson | Benoit Blanchon | >= 7.0.0 |

## Réglages Arduino IDE

| Paramètre | Valeur |
|---|---|
| Board | ESP32S3 Dev Module |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Enabled |
| Flash Mode | QIO 80MHz |
| PSRAM | OPI PSRAM |
| Upload Speed | 921600 |
| Monitor Speed | 115200 |

## Configuration

Créer un fichier `secrets.h` à la racine du sketch (non versionné) :

```cpp
#pragma once

#define WIFI_SSID        "votre_ssid"
#define WIFI_PASSWORD    "votre_mot_de_passe"

// Strava API — https://www.strava.com/settings/api
#define STRAVA_CLIENT_ID     "votre_client_id"
#define STRAVA_CLIENT_SECRET "votre_client_secret"
#define STRAVA_REFRESH_TOKEN "votre_refresh_token"
```

### Obtenir les credentials Strava

1. Créer une application sur [strava.com/settings/api](https://www.strava.com/settings/api)
2. Récupérer `Client ID` et `Client Secret`
3. Obtenir un `refresh_token` avec le scope `activity:read` via OAuth 2.0

### Fuseau horaire

Le fichier principal définit `TZ_OFFSET_SEC 7200` (CEST, UTC+2 — France heure d'été). Changer à `3600` en hiver (CET, UTC+1).

## Structure du projet

```
Strava_E-paper/
├── Strava_E-paper.ino   — sketch principal
├── pins.h               — définitions des broches
├── secrets.h            — credentials Wi-Fi + Strava (non commité)
├── CLAUDE.md            — documentation technique du projet
└── README.md
```

## Fonctionnement

L'appareil utilise le **deep sleep** de l'ESP32 pour maximiser l'autonomie. `setup()` fait tout le travail ; `loop()` n'est jamais atteint.

1. **Réveil** (démarrage initial ou réveil timer 24h)
2. Initialisation PMU → Wi-Fi → NTP → API Strava
3. Comparaison de l'ID de l'activité avec le cache NVS :
   - **Nouvelle activité** → initialisation écran + redessin (~30-40 s)
   - **Même activité** → pas de redessin (image conservée sur l'écran)
   - **Wi-Fi indisponible + cache existant** → pas de redessin
   - **Wi-Fi indisponible + aucun cache** → écran d'erreur
4. Coupure des rails PMU, puis **deep sleep 24h**

**Autonomie estimée** : quelques mois sur batterie 2000 mAh (consommation en sleep ~200 µA).

## Layout de l'écran

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              Strava  -  Derniere activite          (bandeau bleu)           │
├──────────────────────────────────────┬──────────────────────────────────────┤
│  Nom de l'activite                   │                                      │
│  Type    Date                        │         Trace GPS                    │
│ ─────────────────────               │    (marqueur depart vert,             │
│ ┌────────────┐ ┌────────────┐        │     arrivee rouge)                   │
│ │  DISTANCE  │ │   DUREE    │        │                                      │
│ │  9.5 km    │ │  0:45:12   │        ├──────────────────────────────────────┤
│ └────────────┘ └────────────┘        │  ▁▂▃▄▅▄▃▂▁  Profil altitude (bleu) │
│ ┌────────┐ ┌────────┐ ┌────────┐    │                           MAJ: jj/mm │
│ │DENIVE..│ │ ALLURE │ │ALL.MAX │    └──────────────────────────────────────┘
│ │  85 m  │ │5'12"/km│ │4'45"/km│
│ └────────┘ └────────┘ └────────┘
└──────────────────────────────────────
```

## Brochage

| Signal | GPIO |
|---|---|
| PMU SDA | 47 |
| PMU SCL | 48 |
| EPD MOSI | 11 |
| EPD SCK | 10 |
| EPD DC | 8 |
| EPD CS | 9 |
| EPD RST | 12 |
| EPD BUSY | 13 |
| LED Verte | 42 |
| LED Rouge | 45 |
| Bouton KEY | 4 |

## Licence

MIT
