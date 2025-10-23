#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LoRa.h>
#include <ArduinoJson.h>

// Constantes para usar WiFi
const char *ssid = "UPBWiFi";
const char *password = "";
const char *orionURL = "http://10.199.26.8:1026/v2/entities"; // Orion Context Broker

// Configuración LoRa para TTGO T-Beam
#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_CS 18
#define LORA_RST 14
#define LORA_DIO0 26
#define LED 13

// ID fijo del dispositivo sensor (debería coincidir con el que envía el transmisor) o el carrito
const char *sensorId = "sensor001";

HTTPClient http;
String output;
int state = 1;


void connectWiFi() {
    Serial.println("Connecting to " + String(ssid));
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
        digitalWrite(LED, !digitalRead(LED));
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        digitalWrite(LED, HIGH);
    } else {
        Serial.println("\nError conectando WiFi!");
        digitalWrite(LED, LOW);
    }
}

void setupLoRa() {
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
    
    if (!LoRa.begin(915E6)) {
        Serial.println("Error iniciando LoRa!");
        while (1);
    }
    
    Serial.println("LoRa iniciado en 915MHz");
}

/**
 * Extrae todos los datos del mensaje LoRa (GPS, temperatura, humedad)
 */
bool extractSensorData(String message, float &lat, float &lon, float &temp, float &hum) {
    // Formato esperado: "Lat: 40.712800, Lon: -74.006000, Temp: 25.5, Hum: 60.5"
    
    // Buscar índices de cada campo
    int latIndex = message.indexOf("Lat:");
    int lonIndex = message.indexOf("Lon:");
    int tempIndex = message.indexOf("Temp:");
    int humIndex = message.indexOf("Hum:");
    
    // Verificar que todos los campos estén presentes
    if (latIndex == -1 || lonIndex == -1 || tempIndex == -1 || humIndex == -1) {
        Serial.println("❌ Formato de mensaje incorrecto");
        return false;
    }
    
    // Extraer latitud (entre "Lat:" y "Lon:")
    String latStr = message.substring(latIndex + 4, lonIndex);
    latStr.trim();
    latStr.replace(",", "");
    lat = latStr.toFloat();
    
    // Extraer longitud (entre "Lon:" y "Temp:")
    String lonStr = message.substring(lonIndex + 4, tempIndex);
    lonStr.trim();
    lonStr.replace(",", "");
    lon = lonStr.toFloat();
    
    // Extraer temperatura (entre "Temp:" y "Hum:")
    String tempStr = message.substring(tempIndex + 5, humIndex);
    tempStr.trim();
    tempStr.replace(",", "");
    temp = tempStr.toFloat();
    
    // Extraer humedad (desde "Hum:" hasta el final)
    String humStr = message.substring(humIndex + 4);
    humStr.trim();
    hum = humStr.toFloat();
    
    Serial.printf("✅ Datos extraídos - Lat: %.6f, Lon: %.6f, Temp: %.2f, Hum: %.2f\n", 
                  lat, lon, temp, hum);
    
    return (lat != 0.0 && lon != 0.0);
}

/**
 * Crea el JSON para actualizar atributos (sin id ni type)
 */
String createUpdatePayload(float lat, float lon, float temp, float hum) {
    JsonDocument doc;
    
    // Solo los atributos a actualizar (sin id ni type)
    JsonObject humedad = doc["humedad"].to<JsonObject>();
    humedad["type"] = "float";
    humedad["value"] = hum;
    humedad["metadata"] = JsonObject();
    
    JsonObject temperatura = doc["temperatura"].to<JsonObject>();
    temperatura["type"] = "float";
    temperatura["value"] = temp;
    temperatura["metadata"] = JsonObject();
    
    JsonObject location = doc["location"].to<JsonObject>();
    location["type"] = "geo:point";
    location["value"] = String(lat, 6) + "," + String(lon, 6);
    location["metadata"] = JsonObject();
    
    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

/**
 * Envía actualización a Orion Context Broker usando PATCH
 */
bool sendToOrion(String updatePayload) {
    // URL para actualizar atributos específicos
    String updateURL = String(orionURL) + "/" + sensorId + "/attrs";
    Serial.print("URL de actualización: ");
    Serial.println(updateURL);
    
    http.begin(updateURL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "TTGO-LoRa-FIWARE/1.0");
    
    Serial.println("Actualizando entidad en Orion...");
    Serial.println(updatePayload);
    
    // USAR PATCH para actualizar solo los atributos enviados
    int httpResponseCode = http.PATCH(updatePayload);
    
    bool success = false;
    if (httpResponseCode == 204) {
        Serial.println("✅ Atributos actualizados exitosamente con PATCH");
        success = true;
    } else if (httpResponseCode == 404) {
        // La entidad no existe, necesitamos crearla primero
        Serial.println("⚠️  Entidad no existe, creando...");
        
        // Crear JSON completo para la entidad
        JsonDocument entityDoc;
        entityDoc["id"] = sensorId;
        entityDoc["type"] = "sensorTempHum";
        
        // Copiar los atributos del payload de actualización
        JsonDocument updateDoc;
        deserializeJson(updateDoc, updatePayload);
        
        JsonObject humedad = entityDoc["humedad"].to<JsonObject>();
        humedad["type"] = "float";
        humedad["value"] = updateDoc["humedad"]["value"];
        humedad["metadata"] = JsonObject();
        
        JsonObject temperatura = entityDoc["temperatura"].to<JsonObject>();
        temperatura["type"] = "float";
        temperatura["value"] = updateDoc["temperatura"]["value"];
        temperatura["metadata"] = JsonObject();
        
        JsonObject location = entityDoc["location"].to<JsonObject>();
        location["type"] = "geo:point";
        location["value"] = updateDoc["location"]["value"];
        location["metadata"] = JsonObject();
        
        String entityJson;
        serializeJson(entityDoc, entityJson);
        
        // Crear entidad con POST
        http.begin(orionURL);
        http.addHeader("Content-Type", "application/json");
        
        httpResponseCode = http.POST(entityJson);
        if (httpResponseCode == 201) {
            Serial.println("✅ Entidad creada exitosamente");
            success = true;
        } else {
            Serial.print("❌ Error creando entidad: ");
            Serial.println(httpResponseCode);
        }
    } else {
        Serial.print("❌ Error en Orion: ");
        Serial.println(httpResponseCode);
        
        String response = http.getString();
        if (response.length() > 0) {
            Serial.print("Respuesta del servidor: ");
            Serial.println(response);
        }
    }
    
    http.end();
    return success;
}

/**
 * Configuración inicial
 */
void setup()
{
    Serial.begin(115200);
    pinMode(LED, OUTPUT);
    
    // Configurar LoRa
    setupLoRa();
    
    // Conectar WiFi
    connectWiFi();
    
    Serial.println("Receptor LoRa FIWARE listo - Esperando datos GPS+Temperatura+Humedad...");
}

/**
 * Máquina de estados para receptor LoRa + FIWARE
 */
void loop()
{
    switch (state)
    {
    case 1: // RECEIVE - Esperar datos LoRa
    {
        int packetSize = LoRa.parsePacket();
        
        if (packetSize) {
            // Leer el mensaje completo
            String mensaje = "";
            while (LoRa.available()) {
                mensaje += (char)LoRa.read();
            }
            
            Serial.println("\n=== DATO RECIBIDO POR LoRa ===");
            Serial.print("Mensaje: ");
            Serial.println(mensaje);
            
            // Extraer datos del sensor
            float lat = 0.0, lon = 0.0, temp = 0.0, hum = 0.0;
            if (extractSensorData(mensaje, lat, lon, temp, hum)) {
                // Crear payload de actualización (solo atributos)
                String updatePayload = createUpdatePayload(lat, lon, temp, hum);
                output = updatePayload;
                state = 2;
                
                // LED indicador
                digitalWrite(LED, LOW);
                delay(100);
                digitalWrite(LED, HIGH);
            } else {
                Serial.println("❌ No se pudieron extraer los datos del mensaje");
                state = 3; // Saltar a espera
            }
        }
    }
    break;

    case 2: // SEND - Enviar a FIWARE Orion
    {
        Serial.println("Actualizando datos en FIWARE Orion...");
        
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi desconectado - Reconectando...");
            connectWiFi();
            delay(1000);
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            bool success = sendToOrion(output);
            if (success) {
                Serial.println("✅ Datos esenciales actualizados exitosamente en FIWARE");
            } else {
                Serial.println("❌ Falló el envío a FIWARE");
            }
        } else {
            Serial.println("No se pudo conectar a WiFi");
        }
        
        state = 3;
    }
    break;

    case 3: // WAIT - Breve espera
    {
        Serial.println("Esperando 5 segundos para siguiente recepción...");
        delay(5000);
        state = 1;
    }
    break;

    default:
        Serial.println("Estado no reconocido - Reiniciando");
        state = 1;
        break;
    }
    
    delay(50);
}