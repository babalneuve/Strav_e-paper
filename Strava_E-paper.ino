/*
 * Strava Last Activity — Waveshare ESP32-S3-PhotoPainter
 * Afficheur 7.3" ACeP 7 couleurs, 800x480 px
 *
 * ── Réglages Arduino IDE ──────────────────────────────────────────────────────
 *  Board            : ESP32S3 Dev Module
 *  USB Mode         : Hardware CDC and JTAG
 *  USB CDC On Boot  : Enabled
 *  Flash Mode       : QIO 80MHz
 *  PSRAM            : OPI PSRAM
 *  Upload Speed     : 921600
 *
 * ── Bibliothèques à installer (Gestionnaire de bibliothèques) ─────────────────
 *  GxEPD2            par Jean-Marc Zingg   (>= 1.6.2)
 *  Adafruit GFX Library                    (>= 1.11.9)
 *  XPowersLib        par Lewis He          (>= 0.2.0)
 *  ArduinoJson       par Benoit Blanchon   (>= 7.0.0)
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <XPowersLib.h>
#include <Preferences.h>
#include <GxEPD2_7C.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>

#include "pins.h"
#include "secrets.h"

XPowersAXP2101 PMU;
Preferences       prefs;

GxEPD2_7C<GxEPD2_730c_GDEP073E01, GxEPD2_730c_GDEP073E01::HEIGHT> display(
    GxEPD2_730c_GDEP073E01(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// Intervalle de vérification d'une nouvelle activité Strava
#define REFRESH_INTERVAL_MIN  60
#define REFRESH_INTERVAL_MS   (REFRESH_INTERVAL_MIN * 60UL * 1000UL)

struct Activity {
    int64_t id           = 0;   // ID unique Strava — sert à détecter une nouvelle activité
    String  name;
    String  type;
    String  date;
    String  polyline;           // Google Encoded Polyline (summary_polyline Strava)
    float   dist_km      = 0;
    int     moving_secs  = 0;
    float   elevation_m  = 0;
    float   avg_speed_kph= 0;
};

struct LatLng { float lat, lng; };
struct Pt     { int16_t x, y; };

// ── Prototypes ────────────────────────────────────────────────────────────────
static void   pmu_init();
static void   epd_wait_busy();
static void   epd_hardware_reset();
static void   epd_deep_init();
static void   display_init();
static bool   wifi_connect();
static void   wifi_disconnect();
static String strava_get_token();
static bool   strava_fetch_last(const String& token, Activity& act);
static void   save_activity_cache(const Activity& act);
static bool   load_activity_cache(Activity& act);
static void   check_and_refresh();
static void   draw_activity(const Activity& act, const String& gpsError = "");
static void   draw_error(const char* line1, const char* line2 = "");
static void   draw_stat_box(int x, int y, int w, int h, uint16_t color, const char* label, const String& value);
static void   print_centered(const String& text, int bx, int by, int bw, int bh);
static int    decode_polyline(const String& encoded, LatLng* pts, int maxPts);
static void   prepare_gps_pixels(const Activity& act, Pt* pixels, int& count,
                                  int ax, int ay, int aw, int ah);
static String fmt_dist(float km);
static String fmt_time(int secs);
static String fmt_elev(float m);
static String fmt_pace(float kph);
static String fmt_speed(float kph);
static String parse_date(const String& iso);
static String translate_type(const String& type);
static void   led_blink(uint8_t pin, int times, int period_ms);  // active bas : LOW = allume

static unsigned long lastRefreshMs = 0;

// ─────────────────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < 3000);
    delay(100);
    Serial.println("\n=== Strava Last Activity — ESP32-S3-PhotoPainter ===");

    // ── LEDs ──────────────────────────────────────────────────────────────────
    pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
    pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   HIGH);
    led_blink(LED_GREEN, 3, 150);

    // ── Broches EPD — états initiaux avant tout init SPI ─────────────────────
    pinMode(EPD_RST,  OUTPUT); digitalWrite(EPD_RST, HIGH);
    pinMode(EPD_DC,   OUTPUT); digitalWrite(EPD_DC,  LOW);
    pinMode(EPD_CS,   OUTPUT); digitalWrite(EPD_CS,  HIGH);
    pinMode(EPD_BUSY, INPUT);
    Serial.println("[OK] Broches EPD configurees");

    // ── PMU — alimente l'écran via ALDO3/ALDO4, doit précéder tout init SPI ──
    Serial.println("Init PMU...");
    pmu_init();
    Serial.println("Stabilisation 2s...");
    delay(2000);

    // ── Afficheur ─────────────────────────────────────────────────────────────
    Serial.println("Init afficheur...");
    display_init();
    Serial.println("[OK] Afficheur pret");

    // ── WiFi + Strava ─────────────────────────────────────────────────────────
    led_blink(LED_GREEN, 2, 100);
    Activity act;
    String   gpsError = "";

    if (!wifi_connect()) {
        gpsError = "WiFi impossible";
    } else {
        Serial.println("Token Strava...");
        String token = strava_get_token();

        if (token.isEmpty()) {
            gpsError = "Token Strava invalide";
        } else {
            Serial.println("Recuperation derniere activite...");
            if (!strava_fetch_last(token, act)) {
                gpsError = "Erreur API Strava";
            } else {
                save_activity_cache(act);  // succes : mise en cache
            }
        }
        wifi_disconnect();
    }

    // Si erreur : tenter de charger les donnees en cache
    if (!gpsError.isEmpty()) {
        Serial.println("[WARN] " + gpsError + " — tentative cache...");
        if (!load_activity_cache(act)) {
            // Aucune donnee en cache : ecran d'erreur complet
            draw_error(gpsError.c_str(), "Aucune donnee en cache");
            return;
        }
        Serial.println("[OK] Donnees en cache chargees");
    }

    Serial.println("Dessin en cours (~30-40s)...");
    draw_activity(act, gpsError);
    Serial.println("[OK] Dessin termine — image conservee sans alimentation");
    led_blink(LED_RED, 2, 200);

    lastRefreshMs = millis();  // évite un refresh immédiat au premier tour de loop()
}

void loop()
{
    if ((millis() - lastRefreshMs) >= REFRESH_INTERVAL_MS) {
        lastRefreshMs = millis();
        check_and_refresh();
    }
    led_blink(LED_GREEN, 1, 200);
    delay(3000);
}

// ─────────────────────────────────────────────────────────────────────────────
static void pmu_init()
{
    Wire.begin(PMU_SDA, PMU_SCL);
    delay(50);

    if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL)) {
        Serial.println("[ERREUR] PMU non detecte sur I2C !");
        return;
    }
    Serial.println("  PMU detecte (AXP2101)");

    PMU.disableSleep();
    PMU.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_2000MA);

    // ALDO3 alimente l'ecran, ALDO4 les peripheriques
    PMU.setALDO1Voltage(3300); PMU.enableALDO1();
    PMU.setALDO2Voltage(3300); PMU.enableALDO2();
    PMU.setALDO3Voltage(3300); PMU.enableALDO3();
    PMU.setALDO4Voltage(3300); PMU.enableALDO4();
    PMU.clearIrqStatus();
    Serial.println("  ALDO1-4 actives a 3.3V");
}

// ── Init afficheur (séquence identique à l'application IBIS) ─────────────────
static void epd_wait_busy()
{
    Serial.print("  BUSY");
    unsigned long start = millis();
    while (digitalRead(EPD_BUSY) == LOW) {
        delay(10);
        if (millis() - start > 60000) { Serial.println(" TIMEOUT!"); return; }
    }
    Serial.print(" OK ("); Serial.print(millis() - start); Serial.println("ms)");
}

static void epd_hardware_reset()
{
    Serial.println("  Reset materiel...");
    digitalWrite(EPD_RST, HIGH); delay(50);
    digitalWrite(EPD_RST, LOW);  delay(20);
    digitalWrite(EPD_RST, HIGH); delay(50);
    epd_wait_busy();
}

static void epd_deep_init()
{
    epd_hardware_reset();
    delay(80);
    epd_wait_busy();
}

static void display_init()
{
    SPI.end();   // sans effet si déjà arrêté ; évite les conflits lors d'un second appel
    delay(100);
    SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);
    epd_deep_init();
    display.init(115200, true, 2, false);
    display.setRotation(2);
    delay(200);
}

// ─────────────────────────────────────────────────────────────────────────────
static bool wifi_connect()
{
    Serial.print("WiFi: connexion a "); Serial.println(WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500); Serial.print("."); attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        Serial.print("[OK] WiFi connecte — IP: "); Serial.println(WiFi.localIP());
        return true;
    }
    Serial.println("[FAIL] WiFi echec");
    return false;
}

static void wifi_disconnect()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi deconnecte");
}

// ─────────────────────────────────────────────────────────────────────────────
static String strava_get_token()
{
    String url = "https://www.strava.com/oauth/token";
    url += "?client_id="     STRAVA_CLIENT_ID;
    url += "&client_secret=" STRAVA_CLIENT_SECRET;
    url += "&refresh_token=" STRAVA_REFRESH_TOKEN;
    url += "&grant_type=refresh_token";

    for (int attempt = 1; attempt <= 2; attempt++) {
        Serial.print("  Token tentative "); Serial.println(attempt);

        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setTimeout(15000);  // 15s max — SSL sur ESP32 peut etre lent

        http.begin(client, url);
        int code = http.POST("");
        Serial.print("  HTTP: "); Serial.println(code);

        if (code == 200) {
            JsonDocument doc;
            deserializeJson(doc, http.getString());
            http.end();
            String token = doc["access_token"] | "";
            if (!token.isEmpty()) { Serial.println("  [OK] Token obtenu"); return token; }
            Serial.println("  [FAIL] access_token absent de la reponse");
        } else {
            // Logge la reponse d'erreur pour diagnostic
            String body = http.getString();
            http.end();
            Serial.print("  [FAIL] Reponse Strava: "); Serial.println(body);
        }

        if (attempt < 2) { Serial.println("  Retry dans 3s..."); delay(3000); }
    }
    return "";
}

// ─────────────────────────────────────────────────────────────────────────────
static bool strava_fetch_last(const String& token, Activity& act)
{
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    http.begin(client, "https://www.strava.com/api/v3/athlete/activities?per_page=1");
    http.addHeader("Authorization", "Bearer " + token);
    int code = http.GET();
    Serial.print("  HTTP activites: "); Serial.println(code);

    if (code != 200) { http.end(); return false; }

    // Filtre JSON : on ne garde que les champs utiles pour economiser la RAM
    JsonDocument filter;
    filter[0]["id"]                      = true;
    filter[0]["name"]                    = true;
    filter[0]["type"]                    = true;
    filter[0]["start_date_local"]        = true;
    filter[0]["distance"]                = true;
    filter[0]["moving_time"]             = true;
    filter[0]["total_elevation_gain"]    = true;
    filter[0]["average_speed"]           = true;
    filter[0]["map"]["summary_polyline"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString(),
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err || !doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
        Serial.print("  [FAIL] JSON: "); Serial.println(err.c_str());
        return false;
    }

    JsonObject a     = doc[0];
    act.id           = a["id"].as<int64_t>();
    act.name         = a["name"]                 | "Sans titre";
    act.type         = a["type"]                 | "?";
    act.dist_km      = ((float)(a["distance"]     | 0.0f)) / 1000.0f;
    act.moving_secs  = a["moving_time"]          | 0;
    act.elevation_m  = a["total_elevation_gain"] | 0.0f;
    act.avg_speed_kph= ((float)(a["average_speed"]| 0.0f)) * 3.6f;
    act.date         = parse_date(a["start_date_local"] | "");
    act.polyline     = a["map"]["summary_polyline"] | "";

    Serial.println("  Nom     : " + act.name);
    Serial.println("  Type    : " + act.type);
    Serial.println("  Date    : " + act.date);
    Serial.print  ("  Distance: "); Serial.print(act.dist_km);       Serial.println(" km");
    Serial.print  ("  Temps   : "); Serial.print(act.moving_secs);   Serial.println(" s");
    Serial.print  ("  Denivele: "); Serial.print(act.elevation_m);   Serial.println(" m");
    Serial.print  ("  Vitesse : "); Serial.print(act.avg_speed_kph); Serial.println(" km/h");
    Serial.print  ("  ID      : "); Serial.println((long long)act.id);
    Serial.print  ("  Polyline: "); Serial.print(act.polyline.length()); Serial.println(" chars");
    return true;
}

// ── Cache NVS (Preferences) ──────────────────────────────────────────────────
// Persiste les stats de la derniere activite reussie dans la flash.
// La polyline n'est pas cachee (trop grande, tracé non critique en mode erreur).

static void save_activity_cache(const Activity& act)
{
    prefs.begin("strava_cache", false);
    prefs.putLong64("id",    act.id);
    prefs.putString("name",  act.name);
    prefs.putString("type",  act.type);
    prefs.putString("date",  act.date);
    prefs.putFloat ("dist",  act.dist_km);
    prefs.putInt   ("secs",  act.moving_secs);
    prefs.putFloat ("elev",  act.elevation_m);
    prefs.putFloat ("speed", act.avg_speed_kph);
    prefs.end();
    Serial.println("[OK] Activite mise en cache (NVS)");
}

static bool load_activity_cache(Activity& act)
{
    prefs.begin("strava_cache", true);
    bool ok = prefs.isKey("name");
    if (ok) {
        act.id            = prefs.getLong64("id",    0);
        act.name          = prefs.getString("name",  "");
        act.type          = prefs.getString("type",  "");
        act.date          = prefs.getString("date",  "");
        act.dist_km       = prefs.getFloat ("dist",  0);
        act.moving_secs   = prefs.getInt   ("secs",  0);
        act.elevation_m   = prefs.getFloat ("elev",  0);
        act.avg_speed_kph = prefs.getFloat ("speed", 0);
        act.polyline      = "";  // pas de trace en mode cache
    }
    prefs.end();
    if (ok) Serial.println("[OK] Cache charge : " + act.name);
    return ok && act.name.length() > 0;
}

// ── Refresh automatique ───────────────────────────────────────────────────────
// Appelée toutes les REFRESH_INTERVAL_MS depuis loop(). Interroge l'API Strava,
// compare l'ID de l'activité reçue avec l'ID en cache ; redessin uniquement si
// une nouvelle activité est détectée.
static void check_and_refresh()
{
    Serial.println("=== Verification nouvelle activite ===");
    led_blink(LED_GREEN, 2, 100);

    // Lire l'ID actuellement en cache pour comparer après l'appel API
    prefs.begin("strava_cache", true);
    int64_t cachedId = prefs.getLong64("id", 0);
    prefs.end();
    Serial.print("  ID en cache : "); Serial.println((long long)cachedId);

    if (!wifi_connect()) {
        Serial.println("[WARN] WiFi indisponible — refresh ignore");
        return;
    }

    String token = strava_get_token();
    if (token.isEmpty()) {
        Serial.println("[WARN] Token invalide — refresh ignore");
        wifi_disconnect();
        return;
    }

    Activity act;
    if (!strava_fetch_last(token, act)) {
        Serial.println("[WARN] API Strava KO — refresh ignore");
        wifi_disconnect();
        return;
    }
    wifi_disconnect();

    if (act.id == 0 || act.id == cachedId) {
        Serial.println("Pas de nouvelle activite (ID identique)");
        return;
    }

    Serial.print("Nouvelle activite ! ID: "); Serial.println((long long)act.id);
    save_activity_cache(act);

    display_init();  // réveille l'écran sorti de hibernate
    Serial.println("Redessin en cours (~30-40s)...");
    draw_activity(act, "");
    Serial.println("[OK] Refresh termine");
    led_blink(LED_RED, 3, 200);
}

// ── Dessin ───────────────────────────────────────────────────────────────────
//
// Layout 800x480 :
//   y=  0- 75 : bandeau orange
//   x=  0-385 : panneau gauche — stats
//   x=390      : separateur vertical
//   x=393-800 : panneau droit  — trace GPS

//
//   Panneau gauche :
//     y= 85-127 : nom de l'activite
//     y=132-157 : type + date
//     y=163      : separateur horizontal
//     y=168-295 : boites stats rang 1 (DISTANCE | DUREE)
//     y=306-433 : boites stats rang 2 (DENIVELE | ALLURE/VITESSE)
//
static void draw_activity(const Activity& act, const String& gpsError)
{
    // 4e stat : allure (min/km) pour les sports "pied", vitesse sinon
    bool is_foot = (act.type == "Run" || act.type == "Walk" || act.type == "Hike");
    const char* label4 = is_foot ? "ALLURE" : "VITESSE";
    String val4 = is_foot ? fmt_pace(act.avg_speed_kph) : fmt_speed(act.avg_speed_kph);

    // Pre-calcul des pixels GPS en dehors de la boucle de rendu
    // Zone GPS : x=400, y=82, w=390, h=350
    static Pt gpsPixels[400];
    int gpsPtCount = 0;
    prepare_gps_pixels(act, gpsPixels, gpsPtCount, 400, 82, 390, 350);
    Serial.print("  GPS points decoded: "); Serial.println(gpsPtCount);

    display.setFullWindow();
    display.firstPage();
    int page = 0;

    do {
        Serial.print("  Page "); Serial.println(++page);
        display.fillScreen(GxEPD_WHITE);

        // ── Bandeau header orange ──────────────────────────────────────────────
        display.fillRect(0, 0, 800, 75, GxEPD_RED);
        display.setFont(&FreeMonoBold12pt7b);
        display.setTextColor(GxEPD_WHITE);
        print_centered("Strava  -  Derniere activite", 0, 8, 800, 67);

        // ── Separateur vertical ────────────────────────────────────────────────
        display.drawFastVLine(390, 75, 365, GxEPD_BLACK);
        display.drawFastVLine(391, 75, 365, GxEPD_BLACK);

        // ── Panneau gauche : stats ─────────────────────────────────────────────
        String name = act.name;
        if (name.length() > 24) name = name.substring(0, 22) + "..";
        display.setFont(&FreeMonoBold12pt7b);
        display.setTextColor(GxEPD_BLACK);
        print_centered(name, 0, 82, 387, 45);

        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLUE);
        print_centered(translate_type(act.type) + "  " + act.date, 0, 132, 387, 28);

        display.drawFastHLine(10, 163, 370, GxEPD_BLACK);

        // 2x2 boites : bw=177, bh=127, margin=10, gap h=11 v=10
        const int bx[] = { 10, 198,  10, 198 };
        const int by[] = { 168, 168, 305, 305 };
        const uint16_t colors[] = { GxEPD_RED, GxEPD_BLUE, GxEPD_GREEN, GxEPD_BLACK };
        const char*  labels[]   = { "DISTANCE", "DUREE", "DENIVELE+", label4 };
        String       values[]   = { fmt_dist(act.dist_km), fmt_time(act.moving_secs),
                                    fmt_elev(act.elevation_m), val4 };
        for (int i = 0; i < 4; i++) {
            draw_stat_box(bx[i], by[i], 177, 127, colors[i], labels[i], values[i]);
        }

        // ── Panneau droit : tracé GPS ──────────────────────────────────────────
        if (!gpsError.isEmpty()) {
            // Erreur reseau/API : afficher le message a la place du trace
            display.setFont(&FreeMonoBold9pt7b);
            display.setTextColor(GxEPD_RED);
            print_centered(gpsError, 393, 82, 407, 355);
        } else if (gpsPtCount >= 2) {
            // Trace (2px d'epaisseur)
            for (int i = 1; i < gpsPtCount; i++) {
                display.drawLine(gpsPixels[i-1].x,   gpsPixels[i-1].y,
                                 gpsPixels[i].x,     gpsPixels[i].y,   GxEPD_BLACK);
                display.drawLine(gpsPixels[i-1].x+1, gpsPixels[i-1].y,
                                 gpsPixels[i].x+1,   gpsPixels[i].y,   GxEPD_BLACK);
            }
            // Marqueur depart (vert) et arrivee (rouge)
            display.fillCircle(gpsPixels[0].x,             gpsPixels[0].y,             5, GxEPD_GREEN);
            display.fillCircle(gpsPixels[gpsPtCount-1].x,  gpsPixels[gpsPtCount-1].y,  5, GxEPD_RED);
        } else {
            display.setFont(&FreeMonoBold9pt7b);
            display.setTextColor(GxEPD_BLACK);
            print_centered("Pas de trace GPS", 393, 82, 407, 355);
        }

        // ── Cadre decoratif ────────────────────────────────────────────────────
        display.drawRect(3, 3, 794, 474, GxEPD_BLACK);
        display.drawRect(6, 6, 788, 468, GxEPD_ORANGE);


    } while (display.nextPage());

    display.hibernate();
}

static void draw_error(const char* line1, const char* line2)
{
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.fillRect(0, 0, 800, 75, GxEPD_RED);
        display.setFont(&FreeMonoBold12pt7b);
        display.setTextColor(GxEPD_WHITE);
        print_centered("Erreur", 0, 8, 800, 67);

        display.setFont(&FreeMonoBold12pt7b);
        display.setTextColor(GxEPD_BLACK);
        print_centered(line1, 40, 100, 720, 60);

        if (line2 && line2[0]) {
            display.setFont(&FreeMonoBold9pt7b);
            display.setTextColor(GxEPD_BLUE);
            print_centered(line2, 40, 170, 720, 40);
        }
    } while (display.nextPage());
    display.hibernate();
}

// Boite stat : bandeau coloré (label) + zone blanche (valeur centrée)
static void draw_stat_box(int x, int y, int w, int h, uint16_t color,
                          const char* label, const String& value)
{
    const int lh = 35;  // hauteur du bandeau label

    display.drawRect(x, y, w, h, GxEPD_BLACK);

    display.fillRect(x + 1, y + 1, w - 2, lh - 1, color);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    print_centered(String(label), x, y + 2, w, lh - 4);

    display.setFont(&FreeMonoBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
    print_centered(value, x, y + lh, w, h - lh);
}

// Centre un texte horizontalement et verticalement dans un rectangle
static void print_centered(const String& text, int bx, int by, int bw, int bh)
{
    int16_t x1, y1; uint16_t tw, th;
    display.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
    display.setCursor(bx + ((int)bw - (int)tw) / 2 - x1,
                      by + ((int)bh - (int)th) / 2 - y1);
    display.print(text);
}

// ── GPS ───────────────────────────────────────────────────────────────────────

// Décode une Google Encoded Polyline en tableau de coordonnées lat/lng
static int decode_polyline(const String& enc, LatLng* pts, int maxPts)
{
    int idx = 0, len = enc.length(), count = 0;
    int32_t lat = 0, lng = 0;
    while (idx < len && count < maxPts) {
        int32_t result = 0, shift = 0, b;
        do { b = enc[idx++] - 63; result |= (b & 0x1F) << shift; shift += 5; } while (b >= 0x20 && idx < len);
        lat += (result & 1) ? ~(result >> 1) : (result >> 1);
        result = 0; shift = 0;
        do { b = enc[idx++] - 63; result |= (b & 0x1F) << shift; shift += 5; } while (b >= 0x20 && idx < len);
        lng += (result & 1) ? ~(result >> 1) : (result >> 1);
        pts[count++] = { lat / 1e5f, lng / 1e5f };
    }
    return count;
}

// Convertit la polyline en coordonnées pixel, centrées dans la zone (ax,ay,aw,ah)
static void prepare_gps_pixels(const Activity& act, Pt* pixels, int& count,
                                int ax, int ay, int aw, int ah)
{
    count = 0;
    if (act.polyline.isEmpty()) return;

    static LatLng pts[400];
    int n = decode_polyline(act.polyline, pts, 400);
    if (n < 2) return;

    // Bounding box géographique
    float minLat = pts[0].lat, maxLat = pts[0].lat;
    float minLng = pts[0].lng, maxLng = pts[0].lng;
    for (int i = 1; i < n; i++) {
        if (pts[i].lat < minLat) minLat = pts[i].lat;
        if (pts[i].lat > maxLat) maxLat = pts[i].lat;
        if (pts[i].lng < minLng) minLng = pts[i].lng;
        if (pts[i].lng > maxLng) maxLng = pts[i].lng;
    }

    float dLat = maxLat - minLat;
    float dLng = maxLng - minLng;
    if (dLat < 1e-6f) dLat = 1e-6f;
    if (dLng < 1e-6f) dLng = 1e-6f;

    // Echelle uniforme avec marge de 15px, rapport d'aspect conservé
    const int margin = 15;
    float scaleX = (float)(aw - 2 * margin) / dLng;
    float scaleY = (float)(ah - 2 * margin) / dLat;
    float scale  = scaleX < scaleY ? scaleX : scaleY;

    int drawW = (int)(dLng * scale);
    int drawH = (int)(dLat * scale);
    int offX  = ax + margin + (aw - 2 * margin - drawW) / 2;
    int offY  = ay + margin + (ah - 2 * margin - drawH) / 2;

    for (int i = 0; i < n; i++) {
        pixels[i].x = (int16_t)(offX + (pts[i].lng - minLng) * scale);
        pixels[i].y = (int16_t)(offY + (maxLat - pts[i].lat) * scale);  // Y inversé
    }
    count = n;
}

// ── Formatage ────────────────────────────────────────────────────────────────
static String fmt_dist(float km)
{
    char buf[16]; snprintf(buf, sizeof(buf), "%.1f km", km); return buf;
}

static String fmt_time(int secs)
{
    int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    char buf[16]; snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s); return buf;
}

static String fmt_elev(float m)
{
    char buf[16]; snprintf(buf, sizeof(buf), "%d m", (int)(m + 0.5f)); return buf;
}

static String fmt_pace(float kph)
{
    if (kph <= 0) return "--'--\"/km";
    float pace = 60.0f / kph;
    int pm = (int)pace, ps = (int)((pace - pm) * 60);
    char buf[16]; snprintf(buf, sizeof(buf), "%d'%02d\"/km", pm, ps); return buf;
}

static String fmt_speed(float kph)
{
    char buf[16]; snprintf(buf, sizeof(buf), "%.1f km/h", kph); return buf;
}

static String parse_date(const String& iso)
{
    if (iso.length() < 10) return iso;
    int day   = iso.substring(8, 10).toInt();
    int month = iso.substring(5,  7).toInt();
    int year  = iso.substring(0,  4).toInt();
    const char* m[] = {"Jan","Fev","Mar","Avr","Mai","Jun",
                        "Jul","Aou","Sep","Oct","Nov","Dec"};
    char buf[20];
    if (month >= 1 && month <= 12)
        snprintf(buf, sizeof(buf), "%d %s %d", day, m[month - 1], year);
    else
        snprintf(buf, sizeof(buf), "%.10s", iso.c_str());
    return buf;
}

static String translate_type(const String& type)
{
    if (type == "Run")            return "Course";
    if (type == "Ride")           return "Velo";
    if (type == "VirtualRide")    return "Velo virtuel";
    if (type == "Swim")           return "Natation";
    if (type == "Walk")           return "Marche";
    if (type == "Hike")           return "Randonnee";
    if (type == "WeightTraining") return "Musculation";
    if (type == "Workout")        return "Entrainement";
    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
static void led_blink(uint8_t pin, int times, int period_ms)
{
    for (int i = 0; i < times; i++) {
        digitalWrite(pin, LOW);
        delay(period_ms);
        digitalWrite(pin, HIGH);
        delay(period_ms);
    }
}
