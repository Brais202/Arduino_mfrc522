#include <SPI.h>
#include <MFRC522_NTAG424DNA.h>
#include <SoftwareSerial.h>

SoftwareSerial espSerial(3, 2); // RX, TX
const unsigned long BAUDRATE = 115200; // Usar mismo baudrate en ambos dispositivos

// Buffer circular seguro
#define BUFFER_SIZE 64
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

// Prototipos de funciones
void generateRndA(byte *backRndA);
void printHex(byte *buffer, uint16_t bufferSize);
void resetSystem();
String statusCodeToString(MFRC522_NTAG424DNA::DNA_StatusCode status);

void setup() {
  Serial.begin(9600);
  espSerial.begin(9600);
  SPI.begin();
  ntag.PCD_Init();
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

//-------------------------------------------
// Funciones principales mejoradas
//-------------------------------------------

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
  unsigned long startTime = millis();
  
  while(millis() - startTime < 5000) { // Timeout 5 segundos
    while(espSerial.available()) {
      char c = espSerial.read();
      
      // Almacenar en buffer circular
      serialBuffer[bufferIndex] = c;
      bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
      
      // Detectar fin de mensaje (0x0A = '\n')
      if(c == 0x0A) {
        printRawDebug();
        extractAppKey2FromBuffer();
        resetBuffer();
        
        currentState = READ_CARDID;
        return;
      }
    }
  }
  
  Serial.println("[ERROR] Timeout sin fin de línea");
  resetBuffer();
  currentState = WAIT_CARD;
}
void readCardData() {
  MFRC522_NTAG424DNA::DNA_StatusCode status;
  static byte globalRetries = 0;
  const byte MAX_GLOBAL_RETRIES = 3;
  const uint16_t READ_RETRY_DELAY = 1000;

  // Reset inicial del hardware
  if(globalRetries == 0) {
    resetReaderHardware();
    if (!ntag.PICC_IsNewCardPresent() || !ntag.PICC_ReadCardSerial()) {
       return;
      }
  }

  // Paso 1: Selección de aplicación
  status = ntag.DNA_Plain_ISOSelectFile_Application();
  if(status != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    handleRetry("[SELEC] Fallo selección", globalRetries, MAX_GLOBAL_RETRIES);
    return;
  }
  Serial.println("Selección correcta");
  // Paso 2: Autenticación
  byte rndA[16];
  generateRndA(rndA);
  status = ntag.DNA_AuthenticateEV2First(2, appKey2, rndA);
  if(status != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    handleRetry("[AUTH] Fallo autenticación", globalRetries, MAX_GLOBAL_RETRIES);
    return;
  }

  // Paso 3: Lectura de datos con reintentos específicos
  uint16_t backLen = 128;
  byte* buffer = new byte[backLen];
  bool readSuccess = false;
  
  for(byte readAttempt = 0; readAttempt < 3; readAttempt++) {
    status = ntag.DNA_Full_ReadData(
      MFRC522_NTAG424DNA::DNA_FILE_PROPRIETARY, 
      backLen, 
      0, 
      buffer, 
      &backLen
    );

    if(status == MFRC522_NTAG424DNA::DNA_STATUS_OK) {
      memcpy(cardId, buffer, backLen);
      cardIdLength = backLen;
      readSuccess = true;
      break;
    }
    
    // Reset específico para fallos de lectura
    Serial.print("[LECT] Fallo lectura (");
    Serial.print(readAttempt + 1);
    Serial.println(") - Reiniciando comunicación");
    ntag.PCD_StopCrypto1();
    delay(READ_RETRY_DELAY);
  }

  delete[] buffer;

  if(!readSuccess) {
    handleRetry("[LECT] Fallo definitivo", globalRetries, MAX_GLOBAL_RETRIES);
    return;
  }

  // Éxito completo
  globalRetries = 0;
  Serial.println("[OK] Datos leídos correctamente");
  currentState = SEND_CARDID;
}

void handleRetry(const char* errorMsg, byte &retryCounter, byte maxRetries) {
  Serial.print(errorMsg);
  Serial.print(" (Intento ");
  Serial.print(retryCounter + 1);
  Serial.println("/3)");
  
  if(retryCounter < maxRetries - 1) {
    retryCounter++;
    resetReaderHardware();
    delay(2000);
  } else {
    Serial.println("[FATAL] Fallo crítico - Reiniciando sistema");
    retryCounter = 0;
    resetSystem();
  }
}
void sendCardId() {
  espSerial.print("CARDID:");
  Serial.print("[CARDID] ");
  for (uint16_t i = 0; i < cardIdLength; i++) {
    if (cardId[i] < 0x10) {
      espSerial.print("0");
      Serial.print("0");
    }
    espSerial.print(cardId[i], HEX);
    Serial.print(cardId[i], HEX);
  }
  espSerial.println();
  Serial.println();
  currentState = WAIT_APPKEY0;
}

void receiveAppKey0() {
  if (espSerial.available()) {
    String response = espSerial.readStringUntil('\n');
    if (response.startsWith("APPKEY0:")) {
      hexStringToBytes(response.substring(8), appKey0, 16);
      authenticateAppKey0();
    } else {
      Serial.print("[ERROR] Respuesta inválida: ");
      Serial.println(response);
    }
  }
}

void authenticateAppKey0() {
  MFRC522_NTAG424DNA::DNA_StatusCode status = ntag.DNA_Plain_ISOSelectFile_Application();
  if (status != MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.println(statusCodeToString(status));
    handleError(status);
    return;
  }

  byte rndA[16];
  generateRndA(rndA);
  status = ntag.DNA_AuthenticateEV2First(0, appKey0, rndA);
  if (status == MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    Serial.println("[AUTENTICACIÓN] AppKey0 validada");
    currentState = READ_UID;
  } else {
    Serial.println(statusCodeToString(status));
    handleError(status);
  }
}

void readAndSendUid() {
  MFRC522_NTAG424DNA::DNA_StatusCode status = ntag.DNA_Full_GetCardUID(uid);
  if (status == MFRC522_NTAG424DNA::DNA_STATUS_OK) {
    espSerial.print("UID:");
    Serial.print("[UID] ");
    for (uint16_t i = 0; i < 7; i++) {
      if (uid[i] < 0x10) {
        espSerial.print("0");
        Serial.print("0");
      }
      espSerial.print(uid[i], HEX);
      Serial.print(uid[i], HEX);
    }
    espSerial.println();
    Serial.println();
  }
  resetSystem();
}

//-------------------------------------------
// Funciones auxiliares
//-------------------------------------------

String statusCodeToString(MFRC522_NTAG424DNA::DNA_StatusCode status) {
  switch(status) {
    case 0x00: return "OK";
    case 0x0A: return "Auth Failed";
    case 0x0C: return "Formato inválido";
    case 0x0D: return "Error CRC";
    case 0x0E: return "Tarjeta no soportada";
    case 0x0F: return "Timeout";
    default: return "Error 0x" + String(status, HEX);
  }
}

void generateRndA(byte *backRndA) {
  for (byte i = 0; i < 16; i++) backRndA[i] = random(0xFF);
}

void printHex(byte *buffer, uint16_t bufferSize) {
  for (uint16_t i = 0; i < bufferSize; i++) {
    Serial.print(buffer[i] < 0x10 ? "0" : "");
    Serial.print(buffer[i], HEX);
  }
}

void hexStringToBytes(String hex, byte* buffer, int length) {
  for (int i = 0; i < length; i++) {
    buffer[i] = strtoul(hex.substring(i*2, i*2+2).c_str(), NULL, 16);
  }
}

void handleError(MFRC522_NTAG424DNA::DNA_StatusCode status) {
  if (status != MFRC522_NTAG424DNA::DNA_STATUS_TIMEOUT) {
    deselectAndWakeupA = true;
  }
  resetSystem();
}


String printRawDebug() {
  String asciiString = "";
  Serial.print("[HEX] ");
  for(uint8_t i = 0; i < bufferIndex; i++) {
    if(serialBuffer[i] < 0x10) Serial.print("0");
    Serial.print(serialBuffer[i], HEX);
    Serial.print(" ");
    
    // Construir String ASCII
    if(serialBuffer[i] >= 32 && serialBuffer[i] <= 126) {
      asciiString += (char)serialBuffer[i];
    } else {
      asciiString += "\\x";
      if(serialBuffer[i] < 0x10) asciiString += "0";
      asciiString += String(serialBuffer[i], HEX);
    }
  }
  Serial.println(asciiString);
}

void processBuffer() {
  String rawData = printRawDebug(); // Obtener datos en formato legible
  
  // Extraer la clave después de APPKEY2:
  int keyStart = rawData.indexOf("APPKEY2:");
  if(keyStart != -1) {
    String clave = rawData.substring(keyStart + 8); // Saltar "APPKEY2:"
    clave.replace("\\x", ""); // Eliminar códigos especiales
    
    // Validar longitud y guardar en appKey2
    if(clave.length() >= 16) {
      clave.substring(0, 16).getBytes(appKey2, 16);
      Serial.print("Clave recibida: ");
      Serial.println(clave.substring(0, 16));
      
      currentState = READ_CARDID;
    }
  }
}

void resetBuffer() {
  memset(serialBuffer, 0, BUFFER_SIZE);
  bufferIndex = 0;
}
void resetSystem() {
  Serial.println("[SISTEMA] Reinicio completo");
  
  // 1. Reset hardware
  ntag.PICC_HaltA();
  ntag.PCD_StopCrypto1();
  
  // 2. Reset variables
  currentState = WAIT_CARD;
  lastStateChange = millis();
  
  // 3. Limpiar buffers
  memset(appKey2, 0, sizeof(appKey2));
  memset(appKey0, 0, sizeof(appKey0));
  
  // 4. Reiniciar comunicación
  espSerial.flush();
  while(espSerial.available()) espSerial.read();
  
  
}

void extractAppKey2FromBuffer() {
  // Asegúrate de que el buffer contiene al menos 1 (:) + 16 dígitos + '\n'
  if (bufferIndex < 1 + 16 + 1) {
    Serial.println("[ERROR] Buffer demasiado pequeño para APPKEY2");
    return;
  }

  // El '\n' está en serialBuffer[bufferIndex-1]
  // Los 16 dígitos hex ASCII empiezan en:
  int start = bufferIndex - 1 - 16;

  // Copiamos directamente esos 16 caracteres a appKey2[]
  for (int i = 0; i < 16; ++i) {
    appKey2[i] = (byte)serialBuffer[start + i];
  }

  // Debug: imprimir lo copiado
  Serial.print("APPKEY2 extraída (ASCII): ");
  for (int i = 0; i < 16; ++i) {
    Serial.print((char)appKey2[i]);
  }
  Serial.println();

  Serial.print("APPKEY2 en HEX: ");
  for (int i = 0; i < 16; ++i) {
    byte b = appKey2[i];
    if (b < 0x10) Serial.print('0');
    Serial.print(b, HEX);
    Serial.print(' ');
  }
  Serial.println();
}

void resetReaderHardware() {
  ntag.PCD_Reset();
  delay(50);
  ntag.PCD_Init();
  Serial.println("[HARDWARE] Reseteo completo del lector");
}
