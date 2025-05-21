#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>

const char* ssid = "Cooper2";
const char* password = "Malena2011_";
const char* serverUrl = "http://192.168.1.53:8000";

#define UART_RX_PIN 27
#define UART_TX_PIN 26

void setup() {
  Serial.begin(9600);
  Serial2.begin(9600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  
  WiFi.begin(ssid, password);
  Serial.print("\n[WiFi] Conectando...");
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Conectado!");
    Serial2.println("WIFI_OK");
  } else {
    Serial2.println("WIFI_ERROR");
    while (true);
  }
}

void loop() {
  static String buffer = "";
  
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      processCommand(buffer);
      buffer = "";
    }
    else if (isPrintable(c)) {
      buffer += c;
    }
  }
}
void processCommand(const String& cmd) {
  String s = cmd;
  s.trim();
  Serial.print("[UART] Comando: ");
  Serial.println(s);

  if (s == "GET_APPKEY2") {
    fetchFromServer("/get_appkey2/", "APPKEY2");
  }
  else if (s.startsWith("CARDID:")) {
    String cardId = s.substring(7);
    // AÑADIMOS la barra FINAL antes del '?'
    fetchFromServer("/compute_appkey0/?cardid=" + cardId + "&msg=READUID1", "APPKEY0");
  }
  else if (s.startsWith("UID:")) {
    String uid = s.substring(4);
    // TAMBIÉN para submit_uid
    fetchFromServer("/submit/?uid=" + uid, "VALIDATION");
  }
}

void fetchFromServer(const String& endpoint, const String& prefix) {
  HTTPClient http;
  String fullUrl = String(serverUrl) + endpoint;
  Serial.print("[HTTP] GET ");
  Serial.println(fullUrl);
  
  http.begin(fullUrl);
  http.setTimeout(5000);
  
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    payload.trim();
    // Limpiar comillas JSON
    payload.replace("\"", "");
    payload.replace("\r", "");
    payload.replace("\n", "");
    Serial.print("[HTTP] Payload: ");
    Serial.println(payload);

    Serial2.flush();
    delay(10);
    
    String mensaje;
    if (prefix == "VALIDATION") {
      // aquí parseamos el JSON a ojo:
      // buscamos 'authorized: true' o 'authorized: false'
      if (payload.indexOf("authorized: true") >= 0) {
        mensaje = "1\n";
      } else if (payload.indexOf("authorized: false") >= 0) {
        mensaje = "0\n";
      } else {
        mensaje = "ERROR\n";
      }
    }
    else if ((prefix == "APPKEY2" || prefix == "APPKEY0") && payload.length() == 32) {
      // Clave de 16 bytes en hex (32 caracteres)
      mensaje = prefix + ":" + payload + "\n";
    }
    else {
      
      mensaje = prefix + ":" + payload + "\n";
    }
    
    Serial2.print(mensaje);
    Serial.print("[UART]> ");
    Serial.print(mensaje);
    Serial2.flush();
  } else {
    Serial.print("[HTTP] Error en GET, code=");
    Serial.println(code);
    // opcional: reenviar error via UART
    Serial2.print(prefix + ":ERROR\n");
  }
  
  http.end();
}
