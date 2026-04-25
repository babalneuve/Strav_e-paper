#pragma once

// ── PMU AXP2101 (I2C) ────────────────────────────────────────────────────────
#define PMU_SDA    47
#define PMU_SCL    48

// ── E-Paper 7.3" ACeP (SPI) ──────────────────────────────────────────────────
#define EPD_MOSI   11
#define EPD_MISO   -1  // non connecté sur cet afficheur
#define EPD_SCK    10
#define EPD_DC      8
#define EPD_CS      9
#define EPD_RST    12
#define EPD_BUSY   13

// ── LEDs (active bas : LOW = allumé) ─────────────────────────────────────────
#define LED_GREEN  42
#define LED_RED    45

// ── Bouton KEY ────────────────────────────────────────────────────────────────
#define BTN_KEY     4
