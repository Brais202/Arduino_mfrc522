#include <SPI.h>
#include <MFRC522_NTAG424DNA.h>
#include <SoftwareSerial.h>

SoftwareSerial espSerial(3, 2); // RX, TX
#define BUFFER_SIZE 128
char serialBuffer[BUFFER_SIZE];
uint8_t bufferIndex = 0;

#define SS_PIN  10
#define RST_PIN  9
#define LED_OK    6   // pin del LED que se enciende si está autorizado
#define LED_KO    7 
MFRC522_NTAG424DNA ntag(SS_PIN, RST_PIN);
bool deselectAndWakeupA = false;

enum State { WAIT_CARD, REQ_APPKEY2, WAIT_APPKEY2, READ_CARDID, SEND_CARDID, WAIT_APPKEY0, READ_UID, WAIT_AUTH  };
State currentState = WAIT_CARD;

byte appKey2[16], appKey0[16];
byte cardId[128]; uint16_t cardIdLength;
byte uidBuf[7];

// Guardamos el rndA para el segundo paso de EV2
byte lastRndA[16];

void setup() {
  Serial.begin(115200);
  espSerial.begin(9600);
  SPI.begin();
  ntag.PCD_Init();
  randomSeed(analogRead(0));
  Serial.println("[SISTEMA] Inicialización completada");
}

void loop() {
  switch (currentState) {
    case WAIT_CARD:    handleCardDetection();  break;
    case REQ_APPKEY2:  requestAppKey2();       break;
    case WAIT_APPKEY2: receiveAppKey2();       break;
    case READ_CARDID:  readCardData();         break;
    case SEND_CARDID:  sendCardId();           break;
    case WAIT_APPKEY0: receiveAppKey0();       break;
    case READ_UID:     readAndSendUid();       break;
    case WAIT_AUTH:     handleAuthorization();  break;
  }
}

// 1) Detección UID
void handleCardDetection() {
  /*if (!ntag.PICC_IsNewCardPresent() || !ntag.PICC_ReadCardSerial()) return;
  memcpy(uidBuf, ntag.uid.uidByte, ntag.uid.size);
  Serial.print("[TARJETA] UID: "); printHex(uidBuf, ntag.uid.size); Serial.println();*/
  currentState = REQ_APPKEY2;
}

// 2) Pedir AppKey2
void requestAppKey2() {
  memset(serialBuffer,0,BUFFER_SIZE);
  bufferIndex=0;
  while(espSerial.available()) espSerial.read();
  espSerial.println("GET_APPKEY2");
  Serial.println("[UART] GET_APPKEY2 enviado");
  currentState = WAIT_APPKEY2;
}

void receiveAppKey2() {
  bufferIndex = 0;
  memset(serialBuffer, 0, BUFFER_SIZE);

  unsigned long startTime = millis();
  const unsigned long TIMEOUT_MS = 15000;

  while (millis() - startTime < TIMEOUT_MS) {
    if (espSerial.available()) {
      char c = espSerial.read();
      if (bufferIndex < BUFFER_SIZE - 1) {
        serialBuffer[bufferIndex++] = c;
      }
      if (c == '\n') {
        // debug UART
        printRawDebug();

        // extrae la clave
        extractAppKey2FromBuffer();

        resetBuffer();
        currentState = READ_CARDID;
        return;
      }
    }
    delay(50);
  }

  Serial.println("[ERROR] Timeout esperando APPKEY2");
  resetBuffer();
  currentState = WAIT_CARD;
}

// --------------------------------------------------------
// Paso 4) Selección + EV2First + Read (sin EV2NonFirst)
// --------------------------------------------------------
void readCardData() {
  MFRC522_NTAG424DNA::DNA_StatusCode st;

  // 1) Si venimos de un fallo, intenta wake‑up
  if (deselectAndWakeupA) {
    deselectAndWakeupA = false;
    if (!ntag.PICC_TryDeselectAndWakeupA()) {
      // no hemos podido "despertar" la tarjeta: la retiramos y esperamos
      return;
    }
  }
  // 2) Detecta la tarjeta
  if (!ntag.PICC_IsNewCardPresent() || !ntag.PICC_ReadCardSerial()) {
    return;
  }
  /*
  // 2) SelectFile (aplicación NTAG424)
  st = ntag.DNA_Plain_ISOSelectFile_Application();
  if (st != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print(F("Plain_ISOSelectFile STATUS NOT OK: "));
    Serial.println(st);
    if (st != MFRC522_NTAG424DNA::DNA_STATUS_TIMEOUT) deselectAndWakeupA = true;
    return;
  }
  Serial.println(F("[OK]   SelectFile"));
  delay(50);

  // 3) Authenticate EV2 First (Key2)
  byte keyNumber = 2;
  byte authKey[16] = {
    0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00
  };
byte rndA[16];
generateRndA(rndA);

  st = ntag.DNA_AuthenticateEV2First(keyNumber, authKey, rndA);
  if (st != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print(F("AuthenticateEV2First STATUS NOT OK: "));
    Serial.println(st);
    if (st != MFRC522_NTAG424DNA::DNA_STATUS_TIMEOUT) deselectAndWakeupA = true;
    return;
  }

  // 5) Authenticate EV2 Non‑First (completar handshake con misma key0 y rndA)
  st = ntag.DNA_AuthenticateEV2NonFirst(keyNumber, authKey, rndA);
  if (st != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print(F("AuthenticateEV2NonFirst STATUS NOT OK: "));
    Serial.println(st);
    if (st != MFRC522_NTAG424DNA::DNA_STATUS_TIMEOUT) deselectAndWakeupA = true;
    return;
  }

  
  
  // 5) Leemos el archivo propietario en bloques y volcamos a cardId[]
  const uint16_t TOTAL = 128;      // bytes totales que esperamos
  const uint16_t BLOCK = 48;       // tamaño de bloque de lectura
  uint16_t offset = 0;
  cardIdLength = 0;

  while (offset < TOTAL) {
    uint16_t toRead = (TOTAL - offset > BLOCK) ? BLOCK : (TOTAL - offset);
    uint16_t backLen = toRead;
    byte blockBuf[BLOCK];
    st = ntag.DNA_Full_ReadData(
      MFRC522_NTAG424DNA::DNA_FILE_PROPRIETARY,
      toRead,
      offset,
      blockBuf,
      &backLen
    );
    if (st != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
      Serial.print(F("[ERROR] ReadData @"));
      Serial.print(offset);
      Serial.print(F(" len="));
      Serial.print(toRead);
      Serial.print(F(": 0x"));
      Serial.println((uint8_t)st, HEX);
      ntag.PICC_HaltA();
      return;
    }
    // volcamos este bloque a cardId[]
    memcpy(cardId + cardIdLength, blockBuf, backLen);
    cardIdLength += backLen;
    offset     += backLen;
  }

  Serial.print(F("[OK]   ReadData total bytes="));
  Serial.println(cardIdLength);
  
  Serial.println("Cambiando de estado");
  // 6) cerramos sesión y pasamos a enviar
  ntag.PICC_HaltA();*/
  currentState = SEND_CARDID;
}



// 5) Enviar CardID
void sendCardId() {
  espSerial.print("CARDID:3F8A9C12D4B67E5F8A2C1E3D4B5F6A7B");
  /*for (uint16_t i=0;i<cardIdLength;i++){
    if (cardId[i]<0x10) espSerial.print('0');
    espSerial.print(cardId[i],HEX);
  }*/
  espSerial.println();
  currentState = WAIT_APPKEY0;
}

// 6) Recibir AppKey0
void receiveAppKey0() {
  if (!espSerial.available()) return;
  String s = espSerial.readStringUntil('\n');
  if (!s.startsWith("APPKEY0:")) return;
  String h = s.substring(8);
  for (uint8_t i=0;i<16;i++)
    appKey0[i] = strtoul(h.substring(2*i,2*i+2).c_str(),NULL,16);
  Serial.print("AppKey0: "); printHex(appKey0,16); Serial.println();
  // luego podrías autenticar con key0...
  currentState = READ_UID;
}
// 7) Leer y enviar UID exactamente como en el ejemplo oficial,
//    pero realizando primero la autenticación EV2 (Key0) sobre la aplicación NTAG424.
void readAndSendUid() {
  Serial.println("Entrando en estado lectura UID");
  delay(1000);
  MFRC522_NTAG424DNA::DNA_StatusCode st;

  // 1) Si venimos de un fallo, intenta wake‑up
  if (deselectAndWakeupA) {
    deselectAndWakeupA = false;
    if (!ntag.PICC_TryDeselectAndWakeupA()) {
      // no hemos podido "despertar" la tarjeta: la retiramos y esperamos
      return;
    }
  }
  // 2) Detecta la tarjeta
  if (!ntag.PICC_IsNewCardPresent() || !ntag.PICC_ReadCardSerial()) {
    return;
  }

  // 3) SelectFile (aplicación NTAG424DNA)
  st = ntag.DNA_Plain_ISOSelectFile_Application();
  if (st != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print(F("Plain_ISOSelectFile STATUS NOT OK: "));
    Serial.println(st);
    if (st != MFRC522_NTAG424DNA::DNA_STATUS_TIMEOUT) deselectAndWakeupA = true;
    return;
  }

  
  // 4) Authenticate EV2 First con Key0 
  byte keyNumber = 0;
  byte authKey[16] = { 0x62, 0xD5, 0xEA, 0xF0,
  0xD2, 0x74, 0xF1, 0xF4,
  0x39, 0x6A, 0xB4, 0x2B,
  0xC9, 0xF0, 0x3C, 0xBC };      
  byte rndA[16];
  generateRndA(rndA);

  st = ntag.DNA_AuthenticateEV2First(keyNumber, authKey, rndA);
  if (st != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print(F("AuthenticateEV2First STATUS NOT OK: "));
    Serial.println(st);
    if (st != MFRC522_NTAG424DNA::DNA_STATUS_TIMEOUT) deselectAndWakeupA = true;
    return;
  }

  // 5) Authenticate EV2 Non‑First (completar handshake con misma key0 y rndA)
  st = ntag.DNA_AuthenticateEV2NonFirst(keyNumber, authKey, rndA);
  if (st != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print(F("AuthenticateEV2NonFirst STATUS NOT OK: "));
    Serial.println(st);
    if (st != MFRC522_NTAG424DNA::DNA_STATUS_TIMEOUT) deselectAndWakeupA = true;
    return;
  }
  Serial.println("Autenticación correcta");
  // 6) Ya autenticados, podemos leer el UID protegido
  uint16_t ulen = sizeof(uidBuf);  // 7 bytes
  st = ntag.DNA_Full_GetCardUID(uidBuf);
  if (st != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print(F("Full_GetCardUID STATUS NOT OK: "));
    Serial.println(st);
    if (st != MFRC522_NTAG424DNA::DNA_STATUS_TIMEOUT) deselectAndWakeupA = true;
  } else {
    // 7) Envío por UART al servidor
    espSerial.print("UID:");
    for (uint16_t i = 0; i < ulen; ++i) {
      if (uidBuf[i] < 0x10) espSerial.print('0');
      espSerial.print(uidBuf[i], HEX);
    }
    espSerial.println();

    // (Opcional) también lo mostramos por el monitor serie
    Serial.print(F("CardUID: "));
    printHex(uidBuf, ulen);
    Serial.println();
  }

  currentState= WAIT_AUTH;
}
void handleAuthorization() {
  // lee línea completa
  if (!espSerial.available()) return;
  String line = espSerial.readStringUntil('\n');
  line.trim();
  Serial.print("[UART] Auth response: "); Serial.println(line);
  
  if (line.equals("1")) {
    digitalWrite(LED_OK, HIGH);
    digitalWrite(LED_KO, LOW);
    Serial.println("ACCESO PERMITIDO");
  }
  else if (line.equals("0")) {
    digitalWrite(LED_OK, LOW);
    digitalWrite(LED_KO, HIGH);
    Serial.println("ACCESO DENEGADO");
  }
  else {
    Serial.println("Error");
    // mensaje inesperado, ignorar
    return;
  }

  // tras procesar, volvemos al principio
  resetSystem();
}



//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
void resetSystem() {
  ntag.PICC_HaltA();
  ntag.PCD_StopCrypto1();
  currentState = WAIT_CARD;
  Serial.println("[SISTEMA] Reiniciado");
}

void generateRndA(byte *r) {
  for (uint8_t i=0;i<16;i++) r[i]=random(0xFF);
}

void printHex(const byte *b, uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    if (b[i] < 0x10) Serial.print('0');
    Serial.print(b[i], HEX);
    Serial.print(' ');
  }
}


String statusToStr(MFRC522_NTAG424DNA::DNA_StatusCode s) {
  if (s == MFRC522_NTAG424DNA::DNA_STATUS_OK)      return "OK";
  if (s == MFRC522_NTAG424DNA::DNA_STATUS_TIMEOUT) return "Timeout";
  // otros estados según tu versión de la librería...
  return "Err0x"+String((uint8_t)s,HEX);
}

void extractAppKey2FromBuffer() {
  // Asegúrate de que serialBuffer esté terminado en '\0'
  serialBuffer[bufferIndex] = '\0';

  // Busca la cabecera
  char *p = strstr(serialBuffer, "APPKEY2:");
  if (!p) {
    Serial.println("[ERROR] Cabecera APPKEY2 no encontrada");
    return;
  }
  p += 8; // nos situamos tras "APPKEY2:"

  // Recortamos CR/LF al final
  size_t rem = strlen(p);
  while (rem && (p[rem - 1] == '\n' || p[rem - 1] == '\r')) {
    p[--rem] = '\0';
  }

  // Deben ser 32 caracteres hex exactos
  if (rem != 32) {
    Serial.print("[ERROR] Longitud inesperada de HEX: ");
    Serial.println(rem);
    return;
  }

  // Convertir cada par de caracteres hex en un byte
  for (uint8_t i = 0; i < 16; ++i) {
    char byteHex[3] = { p[2*i], p[2*i + 1], '\0' };
    appKey2[i] = (byte) strtoul(byteHex, nullptr, 16);
  }

  // Debug
  Serial.print("APPKEY2 binaria (16 bytes): ");
  for (uint8_t i = 0; i < 16; ++i) {
    if (appKey2[i] < 0x10) Serial.print('0');
    Serial.print(appKey2[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}




void printRawDebug() {
  Serial.print("[UART] Raw HEX: ");
  for (uint8_t i = 0; i < bufferIndex; i++) {
    uint8_t b = serialBuffer[i];
    Serial.print(b < 0x10 ? "0" : "");
    Serial.print(b, HEX);
    Serial.print(" ");
  }
  Serial.println();
}


void hexStringToBytes(String hex, byte* buffer, int length) {
  for (int i = 0; i < length; i++) {
    buffer[i] = strtoul(hex.substring(i*2, i*2+2).c_str(), NULL, 16);
  }
}
void resetBuffer() {
  memset(serialBuffer, 0, BUFFER_SIZE);
  bufferIndex = 0;
}
