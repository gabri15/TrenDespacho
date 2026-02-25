# ESP32-CAM Stream con OTA

Proyecto de cámara ESP32-CAM con streaming MJPEG y actualizaciones OTA.

## Características

- Stream MJPEG continuo en `/stream`
- Captura de imagen estática en `/capture`
- Actualizaciones OTA con contraseña
- IP fija configurable
- Web viewer externo con reconexión automática

## Configuración

1. Copia `src/credentials_example.h` como `src/credentials.h`
2. Edita `src/credentials.h` con tus credenciales WiFi y OTA:
   ```cpp
   const char* WIFI_SSID = "TU_RED_WIFI";
   const char* WIFI_PASS = "TU_PASSWORD_WIFI";
   const char* OTA_PASS = "TU_PASSWORD_OTA";
   ```
3. Ajusta la IP fija en `src/main.cpp` (línea ~11) si es necesario
4. Ajusta la IP en `index.html` (línea ~38) para que coincida

## Primera carga

```bash
pio run -e esp32cam_usb -t upload
```

## Actualizaciones OTA

```bash
pio run -e esp32cam_ota -t upload
```

## Uso

Abre `index.html` en tu navegador para ver el stream de la cámara.

**Endpoints disponibles:**
- `http://192.168.0.100/` - Info
- `http://192.168.0.100/stream` - Stream MJPEG continuo
- `http://192.168.0.100/capture` - Captura JPEG única
