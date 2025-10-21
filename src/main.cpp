#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LoRa.h>
#include <ArduinoJson.h>

// Constantes para usar WiFi
const char *ssid = "UPB";
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

// Constantes del dispositivo receptor
const char *receiverId = "lora-receiver-01";
const char *receiverType = "LoRaReceiver";

HTTPClient http;
String output;
int state = 1;
int entityCounter = 1;

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
    
    Serial.println("Receptor LoRa FIWARE listo - Esperando datos...");
}

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
 * Extrae valores numéricos del mensaje LoRa
 */
bool extractGPSData(String message, float &lat, float &lon) {
    // Buscar patrones comunes en mensajes GPS
    int latIndex = message.indexOf("Lat:");
    int lonIndex = message.indexOf("Lon:");
    
    if (latIndex != -1 && lonIndex != -1) {
        String latStr = message.substring(latIndex + 4, lonIndex);
        String lonStr = message.substring(lonIndex + 4);
        
        // Limpiar y convertir
        latStr.trim();
        lonStr.trim();
        
        // Remover comas si existen
        latStr.replace(",", "");
        lonStr.replace(",", "");
        
        lat = latStr.toFloat();
        lon = lonStr.toFloat();
        
        return (lat != 0.0 && lon != 0.0);
    }
    
    return false;
}

/**
 * Crea una entidad NGSI-LD para FIWARE Orion
 */
String createFIWAREEntity(String loraMessage, int rssi, float snr) {
    JsonDocument doc;
    
    // ID único para la entidad
    String entityId = "urn:ngsi-ld:Sensor:" + String(receiverId) + "-" + String(entityCounter);
    entityCounter++;
    
    doc["id"] = entityId;
    doc["type"] = "Sensor";
    
    // Atributo: mensaje recibido
    JsonObject message = doc["message"].to<JsonObject>();
    message["type"] = "Property";
    message["value"] = loraMessage;
    
    // Atributo: intensidad de señal
    JsonObject signalStrength = doc["rssi"].to<JsonObject>();
    signalStrength["type"] = "Property";
    signalStrength["value"] = rssi;
    signalStrength["unitCode"] = "dBm";
    
    // Atributo: relación señal-ruido
    JsonObject signalNoise = doc["snr"].to<JsonObject>();
    signalNoise["type"] = "Property";
    signalNoise["value"] = snr;
    signalNoise["unitCode"] = "dB";
    
    // Atributo: timestamp
    JsonObject timestamp = doc["timestamp"].to<JsonObject>();
    timestamp["type"] = "Property";
    timestamp["value"] = millis();
    
    // Extraer coordenadas GPS si están en el mensaje
    float lat = 0.0, lon = 0.0;
    if (extractGPSData(loraMessage, lat, lon)) {
        JsonObject location = doc["location"].to<JsonObject>();
        location["type"] = "GeoProperty";
        location["value"]["type"] = "Point";
        location["value"]["coordinates"][0] = lon;
        location["value"]["coordinates"][1] = lat;
    }
    
    // Atributo: dispositivo receptor
    JsonObject receiver = doc["receiver"].to<JsonObject>();
    receiver["type"] = "Property";
    receiver["value"] = receiverId;
    
    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

/**
 * Envía entidad a Orion Context Broker
 */
bool sendToOrion(String entityJson) {
    // Primero intentamos crear la entidad
    http.begin(orionURL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "TTGO-LoRa-FIWARE/1.0");
    
    Serial.println("Enviando a Orion...");
    Serial.println(entityJson);
    
    int httpResponseCode = http.POST(entityJson);
    
    bool success = false;
    if (httpResponseCode == 201 || httpResponseCode == 204) {
        Serial.println("✅ Entidad creada en Orion");
        success = true;
    } else if (httpResponseCode == 422) {
        // La entidad ya existe, intentar actualizar
        Serial.println("⚠️  Entidad existe, actualizando...");
        
        // Extraer ID de la entidad para la URL de actualización
        JsonDocument doc;
        deserializeJson(doc, entityJson);
        String entityId = doc["id"].as<String>();
        
        // CORRECCIÓN: URL de actualización sin "/" duplicado
        String updateURL = String(orionURL) + "/" + entityId + "/attrs";
        Serial.print("URL de actualización: ");
        Serial.println(updateURL);
        
        http.begin(updateURL);
        http.addHeader("Content-Type", "application/json");
        
        httpResponseCode = http.PATCH(entityJson);
        if (httpResponseCode == 204) {
            Serial.println("✅ Entidad actualizada en Orion");
            success = true;
        } else {
            Serial.print("❌ Error en actualización: ");
            Serial.println(httpResponseCode);
        }
    } else {
        Serial.print("❌ Error en Orion: ");
        Serial.println(httpResponseCode);
        
        String response = http.getString();
        if (response.length() > 0) {
            Serial.print("Respuesta: ");
            Serial.println(response);
        }
    }
    
    http.end();
    return success;
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
            // Leer el mensaje
            String mensaje = "";
            while (LoRa.available()) {
                mensaje += (char)LoRa.read();
            }
            
            int rssi = LoRa.packetRssi();
            float snr = LoRa.packetSnr();
            
            Serial.println("=== DATO RECIBIDO ===");
            Serial.print("Mensaje: ");
            Serial.println(mensaje);
            Serial.print("RSSI: ");
            Serial.print(rssi);
            Serial.println(" dBm");
            Serial.print("SNR: ");
            Serial.print(snr);
            Serial.println(" dB");
            
            // Crear entidad FIWARE
            String entityJson = createFIWAREEntity(mensaje, rssi, snr);
            output = entityJson;
            state = 2;
            
            // LED indicador
            digitalWrite(LED, LOW);
            delay(100);
            digitalWrite(LED, HIGH);
        }
    }
    break;

    case 2: // SEND - Enviar a FIWARE Orion
    {
        Serial.println("Enviando datos a FIWARE Orion...");
        
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi desconectado - Reconectando...");
            connectWiFi();
            delay(1000);
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            bool success = sendToOrion(output);
            if (success) {
                Serial.println("✅ Datos enviados exitosamente a FIWARE");
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
        Serial.println("Esperando 2 segundos...");
        delay(2000);
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