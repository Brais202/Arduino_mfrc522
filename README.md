# NACU Firmware Deployment and NFC Operations

This README explains how to deploy and integrate the NACU (NFC Access Control Unit) sketches and implement secure NFC operations using NTAG424 DNA cards.

---

## 🚀 Deploying NACU Sketches

### ESP32 Sketch

* **Wi-Fi Setup**:
  Connect to the configured SSID and password. After a successful connection, send `WIFI_OK` over `Serial2` to notify the Arduino.

* **UART Command Handling**:
  Read lines from `Serial2` into a buffer. When a newline is received, call `processCommand(buffer)`. Check for these prefixes:

  * `CARDID:` → Build URL `/compute_appkey0/?cardid=...`
  * `UID:` → Build URL `/submit/?uid=...`

* **HTTPS Requests**:
  Use `WiFiClientSecure` with `setInsecure()` for self-signed certs:

  1. `https.begin(client, fullUrl)`
  2. `https.GET()` and check for HTTP 200
  3. Parse response via `https.getString()`
  4. Forward over UART:

     * For UID key: `APPKEY0:`
     * For auth result: `1` (allowed), `0` (denied)

### Arduino Sketch (Microcontroller)

* **State Machine Logic**:

```cpp
enum State { WAIT_CARD, ..., READ_UID, WAIT_AUTH };
switch (currentState) {
    case WAIT_CARD:    handleCardDetection(); break;
    case READ_UID:     readAndSendUid();      break;
    case WAIT_AUTH:    handleAuthorization(); break;
}
```

* **Plain NDEF Read**:

```cpp
ntag.DNA_Plain_ReadData(DNA_FILE_NDEF, len, 0, buffer, &len);
espSerial.print("CARDID:"); printHex(cardId, len); espSerial.println();
```

* **EV2 Authentication & UID**:

```cpp
ntag.DNA_AuthenticateEV2First(0, appKey0, rndA);
ntag.DNA_AuthenticateEV2NonFirst(0, appKey0, rndA);
ntag.DNA_Full_GetCardUID(uidBuf);
espSerial.print("UID:"); printHex(uidBuf, 7); espSerial.println();
```

* **Lock Activation**:

```cpp
if (authorized) digitalWrite(5, LOW);  // unlock
else            digitalWrite(5, HIGH); // lock
```

> Upload the sketches via Arduino IDE (ESP32 first, then Arduino) to ensure UART handshake works properly.

---

## 🔐 NFC Operations with NTAG424 DNA

All examples use the `MFRC522_NTAG424DNA` Arduino library.

### EV2 Authentication

```cpp
// 1. Select application file
status = ntag.DNA_Plain_ISOSelectFile_Application();
if (status != OK) return;

// 2. Perform EV2 auth
byte keyNumber = 0;
byte authKey[16] = {}; // Default key (zeros)
byte rndA[16];
generateRndA(rndA);
status = ntag.DNA_AuthenticateEV2First(keyNumber, authKey, rndA);
```

### Plain-Mode NDEF Read/Write

**Write Data**

```cpp
status = reader.writeDataFilePlain(0x01, 0x00, sizeof(cardId)-1, cardId);
```

**Read Data**

```cpp
status = reader.readDataFilePlain(0x01, 0x00, sizeof(buffer), buffer);
Serial.println((char*)buffer);
```

### Secure UID Retrieval

```cpp
byte backUID[7];
status = ntag.DNA_Full_GetCardUID(backUID);
printHex(backUID, 7);
```

### Secure Key Change

```cpp
byte oldKey[16] = {};
byte newKey[16] = {1};
status = ntag.DNA_Full_ChangeKey(1, oldKey, newKey, 1);
```

---

## 🛠️ Notes

* Always perform EV2 authentication before accessing secure features.
* Integrate code blocks into your loop logic for full operation.
* Use secure key management when replacing default keys.

---

© NACU Firmware — Secure NFC Access Control with NTAG424 DNA
