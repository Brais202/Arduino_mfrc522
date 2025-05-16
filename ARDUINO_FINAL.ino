#include <SPI.h>
#include <MFRC522_NTAG424DNA.h>
#include <SoftwareSerial.h>

SoftwareSerial espSerial(3, 2); // RX, TX
#define BUFFER_SIZE 128
char serialBuffer[BUFFER_SIZE];
uint8_t bufferIndex = 0;

#define SS_PIN  10
#define RST_PIN  9

MFRC522_NTAG424DNA ntag(SS_PIN, RST_PIN);
bool deselectAndWakeupA = false;

enum State { WAIT_CARD, REQ_APPKEY2, WAIT_APPKEY2, READ_CARDID, SEND_CARDID, WAIT_APPKEY0, READ_UID };
State currentState = WAIT_CARD;

byte appKey2[16], appKey0[16];
byte cardId[128]; uint16_t cardIdLength;
byte uidBuf[7];

// Guardamos el rndA para el segundo paso de EV2
byte lastRndA[16];

void setup() {
  Serial.begin(9600);
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
  }
}

// 1) Detección UID
void handleCardDetection() {
  if (!ntag.PICC_IsNewCardPresent() || !ntag.PICC_ReadCardSerial()) return;
  memcpy(uidBuf, ntag.uid.uidByte, ntag.uid.size);
  Serial.print("[TARJETA] UID: "); printHex(uidBuf, ntag.uid.size); Serial.println();
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


// 4) Selección + EV2First + EV2NonFirst + Read
void readCardData() {
  // recarga UID
 /* if (!ntag.PICC_TryDeselectAndWakeupA()) {
    // Si no pudimos despertar la tarjeta, leemos de nuevo:
    while (!ntag.PICC_IsNewCardPresent() || !ntag.PICC_ReadCardSerial()) {
      Serial.println("[ERROR] Tarjeta no presente (wake failed)");
      delay(1000);
    }
  }*/

  // SelectFile
  auto st = ntag.DNA_Plain_ISOSelectFile_Application();
  if (st!=MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print("[ERROR] SelectFile: "); Serial.println(statusToStr(st));
    currentState = WAIT_CARD; return;
  }
  Serial.println("[OK] SelectFile");

  // AuthenticateEV2First
  generateRndA(lastRndA);
  st = ntag.DNA_AuthenticateEV2First(2, appKey2, lastRndA);
  if (st!=MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print("[ERROR] AuthFirst: "); Serial.println(statusToStr(st));
    currentState = WAIT_CARD; return;
  }
  Serial.println("[OK] AuthFirst");

  // AuthenticateEV2NonFirst
  st = ntag.DNA_AuthenticateEV2NonFirst(2, appKey2, lastRndA);
  if (st!=MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print("[ERROR] AuthNonFirst: "); Serial.println(statusToStr(st));
    currentState = WAIT_CARD; return;
  }
  Serial.println("[OK] AuthComplete");

  // Leer propietario
  uint16_t len=128;
  byte* buf = new byte[len];
  st = ntag.DNA_Full_ReadData(MFRC522_NTAG424DNA::DNA_FILE_PROPRIETARY,128,0,buf,&len);
  if (st!=MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.print("[ERROR] ReadData: "); Serial.println(statusToStr(st));
    delete[] buf; currentState=WAIT_CARD; return;
  }
  memcpy(cardId,buf,len);
  cardIdLength=len;
  delete[] buf;
  Serial.print("[OK] ReadData bytes="); Serial.println(cardIdLength);

  currentState = SEND_CARDID;
}

// 5) Enviar CardID
void sendCardId() {
  espSerial.print("CARDID:");
  for (uint16_t i=0;i<cardIdLength;i++){
    if (cardId[i]<0x10) espSerial.print('0');
    espSerial.print(cardId[i],HEX);
  }
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

// 7) Leer y enviar UID
void readAndSendUid() {
  byte out[7]; MFRC522_NTAG424DNA::DNA_StatusCode st;
  uint16_t ulen=7;
  st = ntag.DNA_Full_GetCardUID(out);
  if (st==MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    espSerial.print("UID:");
    for (uint8_t i=0;i<ulen;i++){
      if (out[i]<0x10) espSerial.print('0');
      espSerial.print(out[i],HEX);
    }
    espSerial.println();
  }
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
  // Asegúrate de que serialBuffer está NUL-terminated en bufferIndex
  serialBuffer[bufferIndex] = '\0';

  // Busca la cabecera con strstr (devuelve puntero al primer match o nullptr)
  char *p = strstr(serialBuffer, "APPKEY2:");
  if (!p) {
    Serial.println("[ERROR] Cabecera APPKEY2 no encontrada");
    return;
  }

  // Avanza 8 caracteres para situarte justo después de "APPKEY2:"
  p += 8;

  // Ahora p apunta al primer dígito hex. Deben venir 32 caracteres para 16 bytes.
  // Comprueba longitud suficiente:
  size_t remaining = strlen(p);
  if (remaining < 32) {
    Serial.print("[ERROR] No hay suficientes dígitos hex (esperaba 32, hay ");
    Serial.print(remaining);
    Serial.println(")");
    return;
  }

  // Convierte cada par de hex a un byte
  for (uint8_t i = 0; i < 16; ++i) {
    char byteHex[3] = { p[2*i], p[2*i + 1], '\0' };
    appKey2[i] = (byte) strtoul(byteHex, nullptr, 16);
  }

  // Debug final
  Serial.print("APPKEY2 binaria (16 bytes): ");
  printHex(appKey2, 16);
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
