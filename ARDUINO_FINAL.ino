#include <SPI.h>
#include <MFRC522_NTAG424DNA.h>
#include <SoftwareSerial.h>

SoftwareSerial espSerial(3, 2); // RX, TX
const unsigned long BAUDRATE = 115200;

// Buffer lineal
#define BUFFER_SIZE 128
char serialBuffer[BUFFER_SIZE];
uint8_t bufferIndex = 0;

#define SS_PIN 10
#define RST_PIN 9

MFRC522_NTAG424DNA ntag(SS_PIN, RST_PIN);
bool deselectAndWakeupA = false;
unsigned long timeoutState;
unsigned long lastStateChange = 0;

enum State { 
  WAIT_CARD, 
  REQ_APPKEY2, 
  WAIT_APPKEY2, 
  READ_CARDID,
  SEND_CARDID, 
  WAIT_APPKEY0, 
  READ_UID 
};

State currentState = WAIT_CARD;

byte appKey2[16] = {0};
byte appKey0[16] = {0};
byte cardId[128];
byte cardIdLength = 0;
byte uid[7];

void generateRndA(byte *backRndA);
void printHex(byte *buffer, uint16_t bufferSize);
void resetSystem();
String statusCodeToString(MFRC522_NTAG424DNA::DNA_StatusCode status);

void setup() {
  Serial.begin(9600);
  espSerial.begin(9600);
  SPI.begin();
  ntag.PCD_Init();
 // ntag.PCD_SetCRCPadSettings(0x01);
  randomSeed(analogRead(0));
  Serial.println("[SISTEMA] Inicialización completada");
}

void loop() {
  switch(currentState) {
    case WAIT_CARD:        handleCardDetection(); lastStateChange = millis();  break;
    case REQ_APPKEY2:      requestAppKey2(); break;
    case WAIT_APPKEY2:     receiveAppKey2(); break;
    case READ_CARDID:      readCardData(); break;
    case SEND_CARDID:      sendCardId(); break;
    case WAIT_APPKEY0:     receiveAppKey0(); break;
    case READ_UID:         readAndSendUid(); break;
  }
}

void handleCardDetection() {
  if (!ntag.PICC_IsNewCardPresent() || !ntag.PICC_ReadCardSerial()) return;
  
  Serial.println("[TARJETA] Detección exitosa - Iniciando proceso");
  currentState = REQ_APPKEY2;
}

void requestAppKey2() {
  espSerial.flush();
  while(espSerial.available()) espSerial.read();
  espSerial.println("GET_APPKEY2"); 
  timeoutState = millis();
  currentState = WAIT_APPKEY2;
  Serial.println("[UART] Solicitud enviada. Esperando respuesta...");
}

void receiveAppKey2() {
  bufferIndex = 0;
  memset(serialBuffer, 0, BUFFER_SIZE);
  unsigned long startTime = millis();
  
  while(millis() - startTime < 10000) { // Timeout 10 segundos
    if(espSerial.available()) {
      char c = espSerial.read();
      
      if(bufferIndex >= BUFFER_SIZE-1) { // Prevenir overflow
        Serial.println("[ERROR] Buffer overflow");
        resetBuffer();
        currentState = WAIT_CARD;
        return;
      }
      
      serialBuffer[bufferIndex++] = c;
      
      if(c == '\n') { // Fin de mensaje
        printRawDebug();
        extractAppKey2FromBuffer();
        resetBuffer();
        currentState = READ_CARDID;
        return;
      }
    }
  }
  
  Serial.println("[ERROR] Timeout esperando APPKEY2");
  resetBuffer();
  currentState = WAIT_CARD;
}

bool processCard() {
   ntag.PCD_Init();
  unsigned long startTime = millis();
  
  // Bucle de espera activa (hasta 3 segundos)
  while (millis() - startTime < 3000) {
    if (ntag.PICC_IsNewCardPresent() && ntag.PICC_ReadCardSerial()) {
      Serial.print("UID Detectado: ");
      printHex(ntag.uid.uidByte, ntag.uid.size);
      Serial.println();
    }
    delay(100); // Pequeña pausa entre intentos
  }
  
  MFRC522_NTAG424DNA::DNA_StatusCode status = ntag.DNA_Plain_ISOSelectFile_Application();
  if (status != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print("SelectFile ERROR: ");
    Serial.println(statusCodeToString(status));
    return false;
  }

  byte rndA[16];
  generateRndA(rndA);
  status = ntag.DNA_AuthenticateEV2First(2, appKey2, rndA);
  if (status != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print("Auth ERROR: ");
    Serial.println(statusCodeToString(status));
    return false;
  }

  byte* backData = new byte[128];
  uint16_t backLen = 128;
  status = ntag.DNA_Full_ReadData(
    MFRC522_NTAG424DNA::DNA_FILE_PROPRIETARY,
    128,
    0,
    backData,
    &backLen
  );
  
  if (status == MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    memcpy(cardId, backData, backLen);
    cardIdLength = backLen;
  }
  
  delete[] backData;
  return (status == MFRC522_NTAG424DNA::DNA_STATUS_OK);
}

void readCardData() {
  if (processCard()) {
    currentState = SEND_CARDID;
  } else {
    Serial.println("Error");
    currentState = WAIT_CARD;
  }
}

void sendCardId() {
  espSerial.print("CARDID:");
  for (uint16_t i = 0; i < cardIdLength; i++) {
    if (cardId[i] < 0x10) espSerial.print("0");
    espSerial.print(cardId[i], HEX);
  }
  espSerial.println();
  currentState = WAIT_APPKEY0;
}

void receiveAppKey0() {
  if (espSerial.available()) {
    String response = espSerial.readStringUntil('\n');
    if (response.startsWith("APPKEY0:")) {
      hexStringToBytes(response.substring(8), appKey0, 16);
      authenticateAppKey0();
    }
  }
}

void authenticateAppKey0() {
  MFRC522_NTAG424DNA::DNA_StatusCode status = ntag.DNA_Plain_ISOSelectFile_Application();
  if (status != MFRC522_NTAG424DNA::DNA_STATUS_OK) return;

  byte rndA[16];
  generateRndA(rndA);
  status = ntag.DNA_AuthenticateEV2First(0, appKey0, rndA);
  
  if (status == MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    currentState = READ_UID;
  } else {
    resetSystem();
  }
}

void readAndSendUid() {
  MFRC522_NTAG424DNA::DNA_StatusCode status = ntag.DNA_Full_GetCardUID(uid);
  if (status == MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    espSerial.print("UID:");
    for (uint16_t i = 0; i < 7; i++) {
      if (uid[i] < 0x10) espSerial.print("0");
      espSerial.print(uid[i], HEX);
    }
    espSerial.println();
  }
  resetSystem();
}

// Funciones auxiliares modificadas
void extractAppKey2FromBuffer() {
  String bufferStr = String(serialBuffer);
  int keyStart = bufferStr.indexOf("APPKEY2:");
  if (keyStart == -1) {
    Serial.println("[ERROR] Cabecera APPKEY2 no encontrada");
    return;
  }
  
  String hexStr = bufferStr.substring(keyStart + 8, keyStart + 8 + 32);
  hexStringToBytes(hexStr, appKey2, 16);
  
  Serial.print("APPKEY2 recibida: ");
  printHex(appKey2, 16);
  Serial.println();
}

void printRawDebug() {
  Serial.print("[UART] Raw: ");
  for(uint8_t i = 0; i < bufferIndex; i++) {
    if(serialBuffer[i] < 0x10) Serial.print("0");
    Serial.print(serialBuffer[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
}

void resetBuffer() {
  memset(serialBuffer, 0, BUFFER_SIZE);
  bufferIndex = 0;
}

void hexStringToBytes(String hex, byte* buffer, int length) {
  for (int i = 0; i < length; i++) {
    buffer[i] = strtoul(hex.substring(i*2, i*2+2).c_str(), NULL, 16);
  }
}

void resetSystem() {
  ntag.PICC_HaltA();
  ntag.PCD_StopCrypto1();
  ntag.PCD_Init();
  currentState = WAIT_CARD;
  memset(appKey2, 0, sizeof(appKey2));
  memset(appKey0, 0, sizeof(appKey0));
  espSerial.flush();
  while(espSerial.available()) espSerial.read();
  Serial.println("[SISTEMA] Reiniciado");
}

String statusCodeToString(MFRC522_NTAG424DNA::DNA_StatusCode status) {
  switch(status) {
    case 0x00: return "OK";
    case 0x0A: return "Auth Failed";
    case 0x0F: return "Timeout";
    default: return "Error 0x" + String(status, HEX);
  }
}

void generateRndA(byte *backRndA) {
  for (byte i = 0; i < 16; i++) backRndA[i] = random(0xFF);
}

void printHex(byte *buffer, uint16_t bufferSize) {
  for (uint16_t i = 0; i < bufferSize; i++) {
    if(buffer[i] < 0x10) Serial.print("0");
    Serial.print(buffer[i], HEX);
  }
}