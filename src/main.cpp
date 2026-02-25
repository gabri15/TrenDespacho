#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
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
#define LED_FLASH_GPIO     4

const char* MQTT_TOPIC_SET = "tren/esp32cam/led/set";
const char* MQTT_TOPIC_STATE = "tren/esp32cam/led/state";
const char* MQTT_TOPIC_GET = "tren/esp32cam/led/get";
const uint32_t MQTT_RECONNECT_MS = 5000;
const uint32_t MQTT_RESOLVE_MS = 10000;

framesize_t gFrameSize = FRAMESIZE_HQVGA;
int gJpegQuality = 18;

WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);
bool ledOn = false;
uint32_t lastMqttReconnect = 0;
uint32_t lastMqttResolve = 0;
IPAddress mqttResolvedIp;
bool mqttHostReady = false;

void ensureMqttConnected();

void handleRoot() {
  server.send(200, "text/plain", "ESP32-CAM OK. Usa /capture o /stream.");
}

void handleCapture() {
  if (activeStreamClient != nullptr && activeStreamClient->connected()) {
    activeStreamClient->stop();
    activeStreamClient = nullptr;
    delay(50);
  }

  camera_fb_t* fb = nullptr;
  for (int attempt = 0; attempt < 5 && fb == nullptr; attempt++) {
    fb = esp_camera_fb_get();
    if (!fb) {
      ArduinoOTA.handle();
      ensureMqttConnected();
      mqttClient.loop();
      delay(30);
    }
  }
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

void handleStopStream() {
  if (activeStreamClient != nullptr && activeStreamClient->connected()) {
    activeStreamClient->stop();
    activeStreamClient = nullptr;
    server.send(200, "text/plain", "Stream detenido");
    return;
  }

  server.send(204, "text/plain", "Sin stream activo");
}

void handleStream() {
  if (otaInProgress) {
    server.send(503, "text/plain", "OTA en progreso. Stream pausado.");
    return;
  }

  if (activeStreamClient != nullptr && activeStreamClient->connected()) {
    server.send(429, "text/plain", "Stream ocupado. Solo un cliente a la vez.");
    return;
  }

  WiFiClient client = server.client();
  activeStreamClient = &client;
  uint32_t lastFrameTime = millis();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Cache-Control: no-store, no-cache, must-revalidate, max-age=0");
  client.println("Pragma: no-cache");
  client.println("Expires: 0");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: keep-alive");
  client.println();

  while (client.connected() && !otaInProgress) {
    if (!client.connected()) {
      break;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      if (millis() - lastFrameTime > 1000) {
        break;
      }
      ArduinoOTA.handle();
      ensureMqttConnected();
      mqttClient.loop();
      yield();
      continue;
    }

    size_t written = 0;
    if (!client.connected()) {
      esp_camera_fb_return(fb);
      break;
    }

    client.println("--frame");
    client.println("Content-Type: image/jpeg");
    client.printf("Content-Length: %u\r\n\r\n", fb->len);
    written = client.write(fb->buf, fb->len);
    if (written == fb->len) {
      client.println();
    }

    esp_camera_fb_return(fb);
    
    if (written != fb->len) {
      break;
    }
    
    lastFrameTime = millis();
    ArduinoOTA.handle();
    ensureMqttConnected();
    mqttClient.loop();
    yield();
  }

  client.stop();
  activeStreamClient = nullptr;
}

void setLed(bool on) {
  ledOn = on;
  digitalWrite(LED_FLASH_GPIO, on ? HIGH : LOW);

  if (mqttClient.connected()) {
    mqttClient.publish(MQTT_TOPIC_STATE, on ? "ON" : "OFF", true);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC_GET) == 0) {
    if (mqttClient.connected()) {
      mqttClient.publish(MQTT_TOPIC_STATE, ledOn ? "ON" : "OFF", true);
    }
    return;
  }

  if (strcmp(topic, MQTT_TOPIC_SET) != 0) {
    return;
  }

  bool turnOn = false;
  if (length == 1 && (payload[0] == '0' || payload[0] == '1')) {
    turnOn = payload[0] == '1';
  } else {
    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; i++) {
      message += static_cast<char>(payload[i]);
    }
    message.trim();
    message.toUpperCase();

    if (message == "ON" || message == "TRUE") {
      turnOn = true;
    } else if (message == "OFF" || message == "FALSE") {
      turnOn = false;
    } else {
      return;
    }
  }

  setLed(turnOn);
}

void initLed() {
  pinMode(LED_FLASH_GPIO, OUTPUT);
  setLed(false);
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
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    gFrameSize = FRAMESIZE_HQVGA;  // 240x176 para mejor FPS
    gJpegQuality = 18;             // Mayor = peor calidad pero más rápido
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.frame_size = gFrameSize;
    config.jpeg_quality = gJpegQuality;
    config.fb_count = 2;
  } else {
    gFrameSize = FRAMESIZE_QQVGA;  // 160x120 sin PSRAM
    gJpegQuality = 20;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.frame_size = gFrameSize;
    config.jpeg_quality = gJpegQuality;
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
    sensor->set_framesize(sensor, gFrameSize);
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

void initMqtt() {
  mqttClient.setCallback(mqttCallback);
}

bool resolveMqttHost() {
  if (MQTT_USE_IP) {
    mqttResolvedIp = IPAddress(MQTT_HOST_IP[0], MQTT_HOST_IP[1], MQTT_HOST_IP[2], MQTT_HOST_IP[3]);
    mqttClient.setServer(mqttResolvedIp, MQTT_PORT);
    mqttHostReady = true;
    return true;
  }

  if (mqttHostReady) {
    return true;
  }

  uint32_t now = millis();
  if (now - lastMqttResolve < MQTT_RESOLVE_MS) {
    return false;
  }
  lastMqttResolve = now;

  if (WiFi.hostByName(MQTT_HOST, mqttResolvedIp)) {
    mqttClient.setServer(mqttResolvedIp, MQTT_PORT);
    mqttHostReady = true;
    Serial.printf("MQTT host resuelto: %s -> %s\n", MQTT_HOST, mqttResolvedIp.toString().c_str());
    return true;
  }

  Serial.println("No se pudo resolver MQTT host");
  return false;
}

void ensureMqttConnected() {
  if (mqttClient.connected()) {
    return;
  }

  if (!resolveMqttHost()) {
    return;
  }

  uint32_t now = millis();
  if (now - lastMqttReconnect < MQTT_RECONNECT_MS) {
    return;
  }
  lastMqttReconnect = now;

  String clientId = "esp32-cam-tren-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
  bool connected = false;

  if (strlen(MQTT_USER) > 0) {
    connected = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, MQTT_TOPIC_STATE, 1, true, "OFFLINE");
  } else {
    connected = mqttClient.connect(clientId.c_str(), MQTT_TOPIC_STATE, 1, true, "OFFLINE");
  }

  if (connected) {
    mqttClient.subscribe(MQTT_TOPIC_SET);
    mqttClient.subscribe(MQTT_TOPIC_GET);
    mqttClient.publish(MQTT_TOPIC_STATE, ledOn ? "ON" : "OFF", true);
    Serial.println("MQTT conectado");
  } else {
    Serial.println("MQTT no disponible, reintentando...");
  }
}

void initHttpServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/stop", HTTP_POST, handleStopStream);
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
  initLed();
  initMqtt();
  initOTA();
  initHttpServer();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  ensureMqttConnected();
  mqttClient.loop();
}