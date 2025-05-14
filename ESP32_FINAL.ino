#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>

const char* ssid = "Cooper2";
const char* password = "Malena2011_";
const char* serverUrl = "http://192.168.1.53:8000";

#define UART_RX_PIN 27
#define UART_TX_PIN 26

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  
  WiFi.begin(ssid, password);
  Serial.print("\n[WiFi] Conectando...");
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }

  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Conectado!");
    Serial2.println("WIFI_OK");
  } else {
    Serial2.println("WIFI_ERROR");
    while(true);
  }
}

void loop() {
  static String buffer = "";
  
  while(Serial2.available()) {
    char c = Serial2.read();
    
    if(c == '\n') {
      processCommand(buffer);
      buffer = "";
    }
    else if(isPrintable(c)) {
      buffer += c;
    }
  }
}

void processCommand(String cmd) {
  cmd.trim();
  Serial.print("[UART] Comando: ");
  Serial.println(cmd);

  if(cmd.endsWith("GET_APPKEY2")) {
    fetchFromServer("/get_appkey2/", "APPKEY2");
  }
  else if(cmd.startsWith("CARDID:")) {
    String cardId = cmd.substring(7);
    fetchFromServer("/compute_appkey0?cardid=" + cardId + "&msg=READUID1", "APPKEY0");
  }
  else if(cmd.startsWith("UID:")) {
    String uid = cmd.substring(4);
    fetchFromServer("/submit_uid?uid=" + uid, "VALIDATION");
  }
}

void fetchFromServer(String endpoint, String prefix) {
  HTTPClient http;
  String fullUrl = serverUrl + endpoint;
  
  http.begin(fullUrl);
  http.setTimeout(1000);
  
  if(http.GET() == HTTP_CODE_OK) {
    String payload = http.getString();
    payload.trim();
    Serial.println(payload);
    // Forzar formato limpio
    payload.replace("\n", "");
    payload.replace("\r", "");
    payload.replace("\"", "");
    Serial2.flush();  // Limpiar buffer de salida
    delay(10);        
    if(payload.length() == 32) {
      String mensaje = "APPKEY2:" + payload + "\n";  // \n como terminador
      Serial2.print(mensaje);
      Serial.print(mensaje);
      Serial2.flush();  // Forzar envío inmediato
    }
  }
  http.end();
}