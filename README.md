# Strava Last Activity — Waveshare ESP32-S3-PhotoPainter

Affiche la dernière activité Strava sur un écran e-paper 7.3" ACeP 7 couleurs (800×480 px), monté sur la carte Waveshare ESP32-S3-PhotoPainter.

## Aperçu

L'appareil se connecte au Wi-Fi, interroge l'API Strava toutes les heures, et rafraîchit l'écran uniquement si une nouvelle activité est détectée. L'image est conservée sans alimentation grâce au mode hibernate de l'écran e-paper.

**Informations affichées :**
- Nom et type de l'activité (Course, Vélo, Natation…)
- Date
- Distance, durée, dénivelé positif, allure ou vitesse
- Tracé GPS (Google Encoded Polyline) avec marqueurs départ/arrivée

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

## Structure du projet

```
Strava_E-paper/
├── Strava_E-paper.ino   — sketch principal
├── pins.h               — définitions des broches
├── secrets.h            — credentials Wi-Fi + Strava (non commité)
└── README.md
```

## Fonctionnement

1. **Démarrage** : initialisation PMU → écran → Wi-Fi → API Strava → dessin
2. **Boucle** : vérification toutes les 60 minutes
   - Nouvelle activité détectée → rafraîchissement de l'écran (~30-40 s)
   - Aucun changement → attente
3. **Fallback cache** : si Wi-Fi ou API indisponible, affichage des dernières données sauvegardées en flash (NVS)

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
