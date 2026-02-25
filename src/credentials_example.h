#ifndef CREDENTIALS_H
#define CREDENTIALS_H

// Credenciales WiFi
// Copia este archivo como "credentials.h" y pon tus credenciales reales
const char* WIFI_SSID = "TU_RED_WIFI";
const char* WIFI_PASS = "TU_PASSWORD_WIFI";
const char* OTA_PASS = "TU_PASSWORD_OTA";
const char* MQTT_HOST = "test.mosquitto.org";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASS = "";
const bool MQTT_USE_IP = false;
const uint8_t MQTT_HOST_IP[4] = {0, 0, 0, 0};

#endif
