// Zielboard: Waveshare ESP32-S3-Touch-LCD-4.3 (800x480, RGB)
#pragma once

constexpr const char* BOARD_NAME = "Waveshare ESP32-S3-Touch-LCD-4.3";
// Ordner fuers Online-Update im Repo
constexpr const char* FW_DIR = "esp32/firmware/ws43/";
constexpr int PIN_SDA = 8, PIN_SCL = 9;
constexpr int PIN_BOOT = -1;  // GPIO 0 ist eine Display-Leitung (G3)
constexpr bool HAS_DIMMING = false;      // Hintergrundlicht nur an/aus (CH422G)
constexpr int SCREEN_W = 800, SCREEN_H = 480;  // quer

// Oeffentliche Ausgabe (Standard): Logs bleiben auf der SD-Karte, nichts geht
// nach GitHub, Updates kommen aus dem oeffentlichen Repo. Marcs eigene Anzeige
// baut mit -DLOG_UPLOAD=1: Upload der Logs und Updates aus dem privaten Repo.
#ifndef LOG_UPLOAD
#define LOG_UPLOAD 0
#endif
#if LOG_UPLOAD
#define UPDATE_REPO_DEFAULT "Darshumar/Ioniq6_obd"
#else
#define UPDATE_REPO_DEFAULT "Darshumar/ioniq6-display"
#endif
