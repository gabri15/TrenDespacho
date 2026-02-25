#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include "esp_camera.h"
#include "credentials.h"

IPAddress localIP(192, 168, 0, 100);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns1(8, 8, 8, 8);
IPAddress dns2(1, 1, 1, 1);

WebServer server(80);
volatile bool otaInProgress = false;
WiFiClient* activeStreamClient = nullptr;

// Pines ESP32-CAM (AI Thinker)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

void handleRoot() {
  server.send(200, "text/plain", "ESP32-CAM OK. Usa /capture o /stream.");
}

void handleCapture() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(503, "text/plain", "No se pudo capturar imagen");
    return;
  }

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send_P(200, "image/jpeg", reinterpret_cast<const char*>(fb->buf), fb->len);
  esp_camera_fb_return(fb);
}

void handleStream() {
  if (otaInProgress) {
    server.send(503, "text/plain", "OTA en progreso. Stream pausado.");
    return;
  }

  WiFiClient client = server.client();
  activeStreamClient = &client;
  const uint32_t streamSessionMs = 8000;
  const uint32_t streamStart = millis();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Cache-Control: no-store, no-cache, must-revalidate, max-age=0");
  client.println("Pragma: no-cache");
  client.println("Expires: 0");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: close");
  client.println();

  while (client.connected() && !otaInProgress) {
    if (millis() - streamStart > streamSessionMs) {
      break;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      ArduinoOTA.handle();
      delay(2);
      yield();
      continue;
    }

    client.println("--frame");
    client.println("Content-Type: image/jpeg");
    client.printf("Content-Length: %u\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);
    client.println();

    esp_camera_fb_return(fb);
    ArduinoOTA.handle();
    delay(1);
    yield();
  }

  client.stop();
  activeStreamClient = nullptr;
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 14;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 16;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Error inicializando camara: 0x%x\n", err);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_vflip(sensor, 1);
    sensor->set_framesize(sensor, FRAMESIZE_QVGA);
  }

  return true;
}

void initWiFi() {
  WiFi.mode(WIFI_STA);

  if (!WiFi.config(localIP, gateway, subnet, dns1, dns2)) {
    Serial.println("No se pudo configurar IP fija");
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();
  Serial.print("WiFi conectada. IP: ");
  Serial.println(WiFi.localIP());
}

void initOTA() {
  ArduinoOTA.setHostname("esp32-cam-tren");
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    
    if (activeStreamClient != nullptr && activeStreamClient->connected()) {
      activeStreamClient->stop();
    }
    activeStreamClient = nullptr;
    
    server.stop();
    delay(100);
    Serial.println("OTA iniciado");
  });

  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    server.begin();
    Serial.println("\nOTA completado");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    server.begin();
    Serial.printf("Error OTA [%u]\n", error);
  });

  ArduinoOTA.begin();
  Serial.println("OTA listo");
}

void initHttpServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/stream", HTTP_GET, handleStream);
  server.begin();
  Serial.println("Servidor HTTP listo");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!initCamera()) {
    Serial.println("Fallo al iniciar camara. Reinicia dispositivo.");
    while (true) {
      delay(1000);
    }
  }

  initWiFi();
  initOTA();
  initHttpServer();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
}