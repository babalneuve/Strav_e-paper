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
#include <esp_sleep.h>

#include "pins.h"
#include "secrets.h"

XPowersAXP2101 PMU;
Preferences       prefs;

GxEPD2_7C<GxEPD2_730c_GDEP073E01, GxEPD2_730c_GDEP073E01::HEIGHT> display(
    GxEPD2_730c_GDEP073E01(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

#define SLEEP_DURATION_US  (24ULL * 3600ULL * 1000000ULL)   // 24h en microsecondes
#define TZ_OFFSET_SEC      7200    // CEST (UTC+2, France heure d'ete)
#define MAX_STREAM_PTS     200

struct Activity {
    int64_t id            = 0;
    String  name;
    String  type;
    String  date;
    String  polyline;
    float   dist_km       = 0;
    int     moving_secs   = 0;
    float   elevation_m   = 0;
    float   avg_speed_kph = 0;
    float   max_speed_kph = 0;
    // Stream altitude (non cache NVS — trop volumineux)
    float   alt_pts[MAX_STREAM_PTS];
    float   dist_pts[MAX_STREAM_PTS];
    int     stream_count  = 0;
};

struct LatLng { float lat, lng; };
struct Pt     { int16_t x, y; };

// ── Prototypes ────────────────────────────────────────────────────────────────
static void   pmu_init();
static void   pmu_disable_rails();
static void   epd_wait_busy();
static void   epd_hardware_reset();
static void   epd_deep_init();
static void   display_init();
static bool   wifi_connect();
static void   wifi_disconnect();
static String ntp_get_time_str();
static String strava_get_token();
static bool   strava_fetch_last(const String& token, Activity& act);
static bool   strava_fetch_streams(const String& token, Activity& act);
static void   save_activity_cache(const Activity& act);
static void   save_last_check();
static bool   load_activity_cache(Activity& act);
static void   draw_activity(const Activity& act, const String& gpsError = "");
static void   draw_error(const char* line1, const char* line2 = "");
static void   draw_stat_box(int x, int y, int w, int h, const char* label, const String& value);
static void   print_centered(const String& text, int bx, int by, int bw, int bh);
static int    decode_polyline(const String& encoded, LatLng* pts, int maxPts);
static void   prepare_gps_pixels(const Activity& act, Pt* pixels, int& count,
                                  int ax, int ay, int aw, int ah);
static float  interp_altitude(const Activity& act, float dist_m);
static String fmt_dist(float km);
static String fmt_time(int secs);
static String fmt_elev(float m);
static String fmt_pace(float kph);
static String fmt_speed(float kph);
static String parse_date(const String& iso);
static String translate_type(const String& type);
static void   led_blink(uint8_t pin, int times, int period_ms);
static void   strava_check(int64_t& cachedId, bool& hasCache);
static void   go_to_sleep();

static String lastCheckStr = "";

// ─────────────────────────────────────────────────────────────────────────────
// Chaque réveil exécute setup() depuis le début — deep_sleep_start() à la fin.
// loop() n'est jamais atteint en fonctionnement normal.
// ─────────────────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < 3000);
    delay(100);

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool fromSleep  = (cause == ESP_SLEEP_WAKEUP_TIMER);
    bool fromButton = (cause == ESP_SLEEP_WAKEUP_EXT1);
    Serial.println(fromButton ? "\n=== Réveil bouton KEY — Strava check ===" :
                   fromSleep  ? "\n=== Réveil deep sleep — Strava check ===" :
                                "\n=== Démarrage initial — Strava Last Activity ===");

    // ── LEDs ──────────────────────────────────────────────────────────────────
    pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
    pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   HIGH);
    led_blink(LED_GREEN, fromButton ? 2 : (fromSleep ? 1 : 3), 150);

    // ── Broches EPD — états initiaux avant tout init SPI ─────────────────────
    pinMode(EPD_RST,  OUTPUT); digitalWrite(EPD_RST, HIGH);
    pinMode(EPD_DC,   OUTPUT); digitalWrite(EPD_DC,  LOW);
    pinMode(EPD_CS,   OUTPUT); digitalWrite(EPD_CS,  HIGH);
    pinMode(EPD_BUSY, INPUT);

    // ── PMU — alimente l'écran via ALDO1-4, doit précéder tout init SPI ──────
    pmu_init();
    delay(2000);

    // ── Lecture de l'état en cache ────────────────────────────────────────────
    prefs.begin("strava_cache", true);
    int64_t cachedId = prefs.getLong64("id", 0);
    bool    hasCache = prefs.isKey("name");
    lastCheckStr     = prefs.getString("lastcheck", "");
    prefs.end();
    Serial.print("  Cache : "); Serial.print(hasCache ? "OK" : "vide");
    Serial.print("  ID: "); Serial.println((long long)cachedId);

    if (fromButton) {
        // Attendre relachement du bouton de réveil avant de démarrer la boucle
        pinMode(BTN_KEY, INPUT_PULLUP);
        while (digitalRead(BTN_KEY) == LOW) delay(10);
        delay(200);
        // Boucle — tourne jusqu'au prochain appui bouton
        do {
            strava_check(cachedId, hasCache);
        } while (digitalRead(BTN_KEY) == HIGH);
        delay(50);
    } else {
        strava_check(cachedId, hasCache);
    }

    go_to_sleep();
}

void loop()
{
    // Ne devrait jamais etre atteint — deep sleep dans setup()
    delay(10000);
}

// ─────────────────────────────────────────────────────────────────────────────
static void strava_check(int64_t& cachedId, bool& hasCache)
{
    Activity act;
    bool     needDraw = false;
    String   gpsError = "";

    if (!wifi_connect()) {
        if (!hasCache) {
            gpsError = "WiFi impossible";
            needDraw = true;
        } else {
            Serial.println("WiFi KO — cache disponible, pas de redessin");
        }
    } else {
        String t = ntp_get_time_str();
        if (!t.isEmpty()) { lastCheckStr = t; save_last_check(); }

        String token = strava_get_token();
        if (token.isEmpty()) {
            if (!hasCache) { gpsError = "Token Strava invalide"; needDraw = true; }
        } else {
            if (!strava_fetch_last(token, act)) {
                if (!hasCache) { gpsError = "Erreur API Strava"; needDraw = true; }
            } else if (act.id != cachedId) {
                Serial.print("Nouvelle activite ! ID: "); Serial.println((long long)act.id);
                strava_fetch_streams(token, act);
                save_activity_cache(act);
                cachedId = act.id;
                hasCache = true;
                needDraw = true;
            } else {
                Serial.println("Meme activite (ID identique) — pas de redessin");
            }
        }
        wifi_disconnect();
    }

    if (needDraw) {
        Serial.println("Init afficheur...");
        display_init();
        led_blink(LED_GREEN, 2, 100);

        if (!gpsError.isEmpty()) {
            Activity cached;
            if (!load_activity_cache(cached)) {
                draw_error(gpsError.c_str(), "Aucune donnee en cache");
            } else {
                Serial.println("Affichage cache avec erreur reseau...");
                draw_activity(cached, gpsError);
            }
        } else {
            Serial.println("Dessin en cours (~30-40s)...");
            draw_activity(act, "");
        }
        Serial.println("[OK] Dessin termine — image conservee sans alimentation");
        led_blink(LED_RED, 2, 200);
    }
}

static void go_to_sleep()
{
    // Coupe les rails PMU : l'écran hiberné conserve son image sans alimentation
    pmu_disable_rails();
    SPI.end();

    Serial.println("Deep sleep 24h (réveil possible via bouton KEY)...");
    Serial.flush();
    delay(100);

    // Réveil par bouton KEY (GPIO 4, actif bas) ou par timer 24h
    pinMode(BTN_KEY, INPUT_PULLUP);
    esp_sleep_enable_ext1_wakeup(1ULL << BTN_KEY, ESP_EXT1_WAKEUP_ALL_LOW);
    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
    esp_deep_sleep_start();
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

    PMU.setALDO1Voltage(3300); PMU.enableALDO1();
    PMU.setALDO2Voltage(3300); PMU.enableALDO2();
    PMU.setALDO3Voltage(3300); PMU.enableALDO3();
    PMU.setALDO4Voltage(3300); PMU.enableALDO4();
    PMU.clearIrqStatus();
    Serial.println("  ALDO1-4 actives a 3.3V");
}

static void pmu_disable_rails()
{
    // Coupe les rails avant deep sleep pour minimiser la consommation
    // L'écran retient son image après hibernate même sans alimentation
    PMU.disableALDO1();
    PMU.disableALDO2();
    PMU.disableALDO3();
    PMU.disableALDO4();
    Serial.println("  ALDO1-4 desactives");
}

// ── Init afficheur ───────────────────────────────────────────────────────────
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
    SPI.end();
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

static String ntp_get_time_str()
{
    configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("  NTP sync...");
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5000)) {
        Serial.println(" FAIL");
        return "";
    }
    char buf[20];
    strftime(buf, sizeof(buf), "%d/%m %H:%M", &timeinfo);
    Serial.print(" OK: "); Serial.println(buf);
    return String(buf);
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
        http.setTimeout(15000);

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

    JsonDocument filter;
    filter[0]["id"]                      = true;
    filter[0]["name"]                    = true;
    filter[0]["type"]                    = true;
    filter[0]["start_date_local"]        = true;
    filter[0]["distance"]                = true;
    filter[0]["moving_time"]             = true;
    filter[0]["total_elevation_gain"]    = true;
    filter[0]["average_speed"]           = true;
    filter[0]["max_speed"]               = true;
    filter[0]["map"]["summary_polyline"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString(),
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err || !doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
        Serial.print("  [FAIL] JSON: "); Serial.println(err.c_str());
        return false;
    }

    JsonObject a      = doc[0];
    act.id            = a["id"].as<int64_t>();
    act.name          = a["name"]                 | "Sans titre";
    act.type          = a["type"]                 | "?";
    act.dist_km       = ((float)(a["distance"]     | 0.0f)) / 1000.0f;
    act.moving_secs   = a["moving_time"]          | 0;
    act.elevation_m   = a["total_elevation_gain"] | 0.0f;
    act.avg_speed_kph = ((float)(a["average_speed"]| 0.0f)) * 3.6f;
    act.max_speed_kph = ((float)(a["max_speed"]    | 0.0f)) * 3.6f;
    act.date          = parse_date(a["start_date_local"] | "");
    act.polyline      = a["map"]["summary_polyline"] | "";

    Serial.println("  Nom     : " + act.name);
    Serial.println("  Type    : " + act.type);
    Serial.println("  Date    : " + act.date);
    Serial.print  ("  Distance: "); Serial.print(act.dist_km);       Serial.println(" km");
    Serial.print  ("  Temps   : "); Serial.print(act.moving_secs);   Serial.println(" s");
    Serial.print  ("  Denivele: "); Serial.print(act.elevation_m);   Serial.println(" m");
    Serial.print  ("  Vit.moy : "); Serial.print(act.avg_speed_kph); Serial.println(" km/h");
    Serial.print  ("  Vit.max : "); Serial.print(act.max_speed_kph); Serial.println(" km/h");
    Serial.print  ("  ID      : "); Serial.println((long long)act.id);
    Serial.print  ("  Polyline: "); Serial.print(act.polyline.length()); Serial.println(" chars");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
static bool strava_fetch_streams(const String& token, Activity& act)
{
    if (act.id == 0) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    char idBuf[24];
    snprintf(idBuf, sizeof(idBuf), "%lld", (long long)act.id);
    String url = "https://www.strava.com/api/v3/activities/";
    url += idBuf;
    url += "/streams?keys=altitude,distance&key_by_type=true&resolution=low";

    http.begin(client, url);
    http.addHeader("Authorization", "Bearer " + token);
    int code = http.GET();
    Serial.print("  HTTP streams: "); Serial.println(code);

    if (code != 200) { http.end(); return false; }

    JsonDocument filter;
    filter["altitude"]["data"][0] = true;
    filter["distance"]["data"][0] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString(),
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        Serial.print("  [FAIL] streams JSON: "); Serial.println(err.c_str());
        return false;
    }

    JsonArray altData  = doc["altitude"]["data"].as<JsonArray>();
    JsonArray distData = doc["distance"]["data"].as<JsonArray>();

    if (altData.isNull() || distData.isNull()) {
        Serial.println("  [FAIL] streams: donnees altitude/distance manquantes");
        return false;
    }

    int n = (int)altData.size();
    if ((int)distData.size() < n) n = (int)distData.size();
    if (n > MAX_STREAM_PTS) n = MAX_STREAM_PTS;

    for (int i = 0; i < n; i++) {
        act.alt_pts[i]  = altData[i].as<float>();
        act.dist_pts[i] = distData[i].as<float>();
    }
    act.stream_count = n;

    Serial.print("  [OK] Streams: "); Serial.print(n); Serial.println(" pts altitude");
    return true;
}

// ── Cache NVS (Preferences) ──────────────────────────────────────────────────
static void save_activity_cache(const Activity& act)
{
    prefs.begin("strava_cache", false);
    prefs.putLong64("id",       act.id);
    prefs.putString("name",     act.name);
    prefs.putString("type",     act.type);
    prefs.putString("date",     act.date);
    prefs.putFloat ("dist",     act.dist_km);
    prefs.putInt   ("secs",     act.moving_secs);
    prefs.putFloat ("elev",     act.elevation_m);
    prefs.putFloat ("speed",    act.avg_speed_kph);
    prefs.putFloat ("maxspeed", act.max_speed_kph);
    prefs.end();
    Serial.println("[OK] Activite mise en cache (NVS)");
}

static void save_last_check()
{
    prefs.begin("strava_cache", false);
    prefs.putString("lastcheck", lastCheckStr);
    prefs.end();
}

static bool load_activity_cache(Activity& act)
{
    prefs.begin("strava_cache", true);
    bool ok = prefs.isKey("name");
    if (ok) {
        act.id            = prefs.getLong64("id",       0);
        act.name          = prefs.getString("name",     "");
        act.type          = prefs.getString("type",     "");
        act.date          = prefs.getString("date",     "");
        act.dist_km       = prefs.getFloat ("dist",     0);
        act.moving_secs   = prefs.getInt   ("secs",     0);
        act.elevation_m   = prefs.getFloat ("elev",     0);
        act.avg_speed_kph = prefs.getFloat ("speed",    0);
        act.max_speed_kph = prefs.getFloat ("maxspeed", 0);
        act.polyline      = "";
        act.stream_count  = 0;
    }
    prefs.end();
    if (ok) Serial.println("[OK] Cache charge : " + act.name);
    return ok && act.name.length() > 0;
}

// ── Dessin ───────────────────────────────────────────────────────────────────
//
// Layout 800x480 :
//   y=  0- 74 : bandeau bleu header
//   x=  0-389 : panneau gauche — 5 boites stats
//   x=390-391 : séparateur vertical
//   x=392-799 : panneau droit — tracé GPS + profil altitude
//
//   Panneau gauche :
//     y= 82-126 : nom de l'activite
//     y=132-159 : type + date
//     y=163     : separateur horizontal
//     y=168-312 : rang 1 — DISTANCE (x=10,w=179) | DUREE (x=199,w=179), h=145
//     y=323-467 : rang 2 — DENIVELE+ (x=10) | ALLURE/VIT (x=136) | MAX (x=262), w=116 h=145
//
//   Panneau droit :
//     y= 78-292 : tracé GPS (h=215)
//     y=297-298 : séparateur horizontal
//     y=302-426 : profil altitude (chartX=423, chartY=304, chartW=366, chartH=121)
//     y=447     : date/heure dernier check API (bas droite)
//
static void draw_activity(const Activity& act, const String& gpsError)
{
    bool is_foot = (act.type == "Run" || act.type == "Walk" || act.type == "Hike");

    const char* label4 = is_foot ? "ALLURE"  : "VITESSE";
    const char* label5 = is_foot ? "ALL.MAX" : "VIT.MAX";
    String val4 = is_foot ? fmt_pace(act.avg_speed_kph)  : fmt_speed(act.avg_speed_kph);
    String val5 = is_foot ? fmt_pace(act.max_speed_kph)  : fmt_speed(act.max_speed_kph);

    // ── Pre-calcul pixels GPS (zone : ax=400 ay=78 aw=390 ah=215) ─────────────
    static Pt gpsPixels[400];
    int gpsPtCount = 0;
    prepare_gps_pixels(act, gpsPixels, gpsPtCount, 400, 78, 390, 215);
    Serial.print("  GPS points decoded: "); Serial.println(gpsPtCount);

    // ── Pre-calcul profil altitude ────────────────────────────────────────────
    const int altAX = 395, altAY = 302, altAW = 398, altAH = 125;
    const int altML = 28, altMT = 2, altMR = 4, altMB = 2;
    const int chartX = altAX + altML;
    const int chartY = altAY + altMT;
    const int chartW = altAW - altML - altMR;
    const int chartH = altAH - altMT - altMB;

    static int16_t altPyBuf[400];
    int   altBufW = 0;
    float altMin  = 0, altMax = 0;
    bool  hasAlt  = (act.stream_count >= 2);

    if (hasAlt) {
        float maxDist = act.dist_pts[act.stream_count - 1];
        altMin = act.alt_pts[0]; altMax = act.alt_pts[0];
        for (int i = 1; i < act.stream_count; i++) {
            if (act.alt_pts[i] < altMin) altMin = act.alt_pts[i];
            if (act.alt_pts[i] > altMax) altMax = act.alt_pts[i];
        }
        float dAlt = altMax - altMin;
        if (dAlt   < 1.0f)  dAlt   = 1.0f;
        if (maxDist < 1.0f) maxDist = 1.0f;

        altBufW = (chartW < 400) ? chartW : 400;
        for (int px = 0; px < altBufW; px++) {
            float d   = (float)px / (float)(altBufW - 1) * maxDist;
            float alt = interp_altitude(act, d);
            int   py  = (int)(((alt - altMin) / dAlt) * (float)(chartH - 1));
            if (py < 0) py = 0;
            if (py >= chartH) py = chartH - 1;
            altPyBuf[px] = (int16_t)py;
        }
    }

    // ── Boucle de rendu GxEPD2 ───────────────────────────────────────────────
    display.setFullWindow();
    display.firstPage();
    int page = 0;

    do {
        Serial.print("  Page "); Serial.println(++page);
        display.fillScreen(GxEPD_WHITE);

        // ── Bandeau header bleu ────────────────────────────────────────────────
        display.fillRect(0, 0, 800, 75, GxEPD_BLUE);
        display.setFont(&FreeMonoBold12pt7b);
        display.setTextColor(GxEPD_WHITE);
        print_centered("Strava  -  Derniere activite", 0, 8, 800, 67);

        // ── Séparateur vertical ────────────────────────────────────────────────
        display.drawFastVLine(390, 75, 405, GxEPD_BLACK);
        display.drawFastVLine(391, 75, 405, GxEPD_BLACK);

        // ── Panneau gauche ─────────────────────────────────────────────────────
        String name = act.name;
        if (name.length() > 24) name = name.substring(0, 22) + "..";
        display.setFont(&FreeMonoBold12pt7b);
        display.setTextColor(GxEPD_BLACK);
        print_centered(name, 0, 82, 387, 45);

        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLUE);
        print_centered(translate_type(act.type) + "  " + act.date, 0, 132, 387, 28);

        display.drawFastHLine(10, 163, 370, GxEPD_BLACK);

        // Rang 1 : 2 boites (bw=179, bh=145, y=168)
        const int   bx1[]  = { 10, 199 };
        const char* lbl1[] = { "DISTANCE", "DUREE" };
        String      val1[] = { fmt_dist(act.dist_km), fmt_time(act.moving_secs) };
        for (int i = 0; i < 2; i++)
            draw_stat_box(bx1[i], 168, 179, 145, lbl1[i], val1[i]);

        // Rang 2 : 3 boites (bw=116, bh=145, y=323)
        const int   bx2[]  = { 10, 136, 262 };
        const char* lbl2[] = { "DENIVELE+", label4, label5 };
        String      val2[] = { fmt_elev(act.elevation_m), val4, val5 };
        for (int i = 0; i < 3; i++)
            draw_stat_box(bx2[i], 323, 116, 145, lbl2[i], val2[i]);

        // ── Panneau droit : tracé GPS ──────────────────────────────────────────
        if (!gpsError.isEmpty()) {
            display.setFont(&FreeMonoBold9pt7b);
            display.setTextColor(GxEPD_RED);
            print_centered(gpsError, 393, 78, 407, 110);
        } else if (gpsPtCount >= 2) {
            for (int i = 1; i < gpsPtCount; i++) {
                display.drawLine(gpsPixels[i-1].x,   gpsPixels[i-1].y,
                                 gpsPixels[i].x,     gpsPixels[i].y,   GxEPD_BLACK);
                display.drawLine(gpsPixels[i-1].x+1, gpsPixels[i-1].y,
                                 gpsPixels[i].x+1,   gpsPixels[i].y,   GxEPD_BLACK);
            }
            display.fillCircle(gpsPixels[0].x,            gpsPixels[0].y,            5, GxEPD_GREEN);
            display.fillCircle(gpsPixels[gpsPtCount-1].x, gpsPixels[gpsPtCount-1].y, 5, GxEPD_RED);
        } else {
            display.setFont(&FreeMonoBold9pt7b);
            display.setTextColor(GxEPD_BLACK);
            print_centered("Pas de trace GPS", 393, 78, 407, 215);
        }

        // ── Séparateur GPS / altitude ──────────────────────────────────────────
        display.drawFastHLine(393, 297, 404, GxEPD_BLACK);
        display.drawFastHLine(393, 298, 404, GxEPD_BLACK);

        // ── Panneau droit : profil altitude ───────────────────────────────────
        if (hasAlt) {
            for (int px = 0; px < altBufW; px++) {
                int screenY = chartY + (chartH - 1 - altPyBuf[px]);
                display.drawFastVLine(chartX + px, screenY,
                                      chartY + chartH - screenY, GxEPD_BLUE);
                if (px > 0) {
                    display.drawLine(
                        chartX + px - 1, chartY + (chartH - 1 - altPyBuf[px-1]),
                        chartX + px,     chartY + (chartH - 1 - altPyBuf[px]),
                        GxEPD_BLACK);
                }
            }
            display.drawFastVLine(chartX, chartY, chartH, GxEPD_BLACK);
            display.drawFastHLine(chartX, chartY + chartH - 1, chartW, GxEPD_BLACK);
            display.setFont(NULL);
            display.setTextColor(GxEPD_BLACK);
            char buf[10];
            snprintf(buf, sizeof(buf), "%dm", (int)(altMax + 0.5f));
            display.setCursor(altAX + 2, chartY + 2);
            display.print(buf);
            snprintf(buf, sizeof(buf), "%dm", (int)(altMin + 0.5f));
            display.setCursor(altAX + 2, chartY + chartH - 10);
            display.print(buf);
        } else {
            display.setFont(&FreeMonoBold9pt7b);
            display.setTextColor(GxEPD_BLACK);
            print_centered("Pas de profil altitude", altAX, altAY, altAW, altAH);
        }

        // ── Date/heure du dernier check API (bas droite) ───────────────────────
        if (!lastCheckStr.isEmpty()) {
            display.setFont(&FreeMonoBold9pt7b);
            display.setTextColor(GxEPD_BLACK);
            String label = "MAJ: " + lastCheckStr;
            int16_t x1, y1; uint16_t tw, th;
            display.getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);
            display.setCursor(790 - (int)tw - x1, 447 - y1);
            display.print(label);
        }

        // ── Cadre décoratif ────────────────────────────────────────────────────
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

static void draw_stat_box(int x, int y, int w, int h,
                          const char* label, const String& value)
{
    const int lh = 30;

    display.drawRect(x, y, w, h, GxEPD_BLACK);
    display.fillRect(x + 1, y + 1, w - 2, lh - 1, GxEPD_BLUE);

    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    print_centered(String(label), x, y + 2, w, lh - 4);

    display.setFont(w >= 150 ? &FreeMonoBold12pt7b : &FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    print_centered(value, x, y + lh, w, h - lh);
}

static void print_centered(const String& text, int bx, int by, int bw, int bh)
{
    int16_t x1, y1; uint16_t tw, th;
    display.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
    display.setCursor(bx + ((int)bw - (int)tw) / 2 - x1,
                      by + ((int)bh - (int)th) / 2 - y1);
    display.print(text);
}

// ── GPS ───────────────────────────────────────────────────────────────────────
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

static void prepare_gps_pixels(const Activity& act, Pt* pixels, int& count,
                                int ax, int ay, int aw, int ah)
{
    count = 0;
    if (act.polyline.isEmpty()) return;

    static LatLng pts[400];
    int n = decode_polyline(act.polyline, pts, 400);
    if (n < 2) return;

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
        pixels[i].y = (int16_t)(offY + (maxLat - pts[i].lat) * scale);
    }
    count = n;
}

static float interp_altitude(const Activity& act, float dist_m)
{
    if (act.stream_count < 2) return 0;
    if (dist_m <= act.dist_pts[0]) return act.alt_pts[0];
    if (dist_m >= act.dist_pts[act.stream_count - 1]) return act.alt_pts[act.stream_count - 1];
    for (int i = 1; i < act.stream_count; i++) {
        if (act.dist_pts[i] >= dist_m) {
            float t = (dist_m - act.dist_pts[i-1]) / (act.dist_pts[i] - act.dist_pts[i-1]);
            return act.alt_pts[i-1] + t * (act.alt_pts[i] - act.alt_pts[i-1]);
        }
    }
    return act.alt_pts[act.stream_count - 1];
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
