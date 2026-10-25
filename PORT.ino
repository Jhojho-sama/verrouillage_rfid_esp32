#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>

#include <Wire.h>
#include <U8g2lib.h>

// ============ WIFI ============
const char* WIFI_SSID = "Sony xperia XZ3";
const char* WIFI_PASS = "01234567";

// ============ TELEGRAM ============
String BOT_TOKEN = "8379306582:AAG5pZ21yhaCnYL6Y3Xe0f-H8VgJuSFGwnI";
String CHAT_ID   = "6353398984";   // ex: 6353398984
String CAM_URL   = "http://192.168.43.21";   // URL ESP32-CAM

// ============ RFID ============
String ALLOWED_UID = "73 29 01 4B";  // ta carte autorisée

// ============ PINS (ESP32 DEV KIT) ============
#define RFID_SS   5
#define RFID_RST  4

#define SERVO_PIN 14

// I2C OLED (par défaut ESP32)
#define I2C_SDA   21
#define I2C_SCL   22

// ============ OBJETS ============
MFRC522 rfid(RFID_SS, RFID_RST);
Servo doorServo;

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

WiFiClientSecure secureClient;

// ============ VARIABLES ============
unsigned long lastAlertMs = 0;
const unsigned long ALERT_COOLDOWN_MS = 8000;

bool pendingRemoteApproval = false;
String lastDeniedUID = "";
long telegramOffset = 0;
void oledMessage(const char* l1, const char* l2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 18, l1);
  u8g2.drawStr(0, 38, l2);
  u8g2.sendBuffer();
}

int i2cScan() {
  int found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("I2C OK @ 0x%02X\n", addr);
      found++;
    }
  }
  return found;
}
String readUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uid += " ";
  }
  uid.toUpperCase();
  uid.trim();
  return uid;
}
String urlEncode(const String &s) {
  String out = "";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c)) out += c;
    else if (c == ' ') out += "%20";
    else if (c == '\n') out += "%0A";
    else {
      char buf[4];
      sprintf(buf, "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

bool telegramSend(const String &msg) {
  if (WiFi.status() != WL_CONNECTED) return false;

  secureClient.setInsecure();
  HTTPClient https;

  String url = "https://api.telegram.org/bot" + BOT_TOKEN +
               "/sendMessage?chat_id=" + CHAT_ID +
               "&text=" + urlEncode(msg);

  if (!https.begin(secureClient, url)) return false;
  int code = https.GET();
  https.end();

  Serial.printf("Telegram code: %d\n", code);
  return (code == 200);
}

// récupère le JSON brut
String telegramGetUpdates() {
  secureClient.setInsecure();
  HTTPClient https;

  String url = "https://api.telegram.org/bot" + BOT_TOKEN +
               "/getUpdates?timeout=1&offset=" + String(telegramOffset);

  if (!https.begin(secureClient, url)) return "";
  int code = https.GET();
  String payload = (code > 0) ? https.getString() : "";
  https.end();

  return payload;
}

// parse simple: cherche "update_id": puis "text":"..."
void handleTelegramCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  String json = telegramGetUpdates();
  if (json.length() < 10) return;

  // update_id (prendre le dernier)
  int pos = 0;
  while (true) {
    int u = json.indexOf("\"update_id\":", pos);
    if (u < 0) break;
    int start = u + 12;
    int end = json.indexOf(",", start);
    if (end < 0) break;
    long id = json.substring(start, end).toInt();
    telegramOffset = id + 1;
    pos = end;
  }

  // text
  int t = json.lastIndexOf("\"text\":\"");
  if (t < 0) return;
  int ts = t + 8;
  int te = json.indexOf("\"", ts);
  if (te < 0) return;

  String text = json.substring(ts, te);
  text.replace("\\/", "/");
  Serial.println("Telegram cmd: " + text);

  if (text == "/open") {
    if (pendingRemoteApproval) {
      telegramSend("✅ Ouverture autorisée à distance (dernier UID refusé: " + lastDeniedUID + ")");
      openDoor(5000);
      pendingRemoteApproval = false;
    } else {
      telegramSend("ℹ️ Aucun accès refusé en attente. Scannez une carte refusée d'abord.");
    }
  } 
  else if (text == "/status") {
    String s = "📡 STATUS\nWiFi: " + String(WiFi.status() == WL_CONNECTED ? "OK" : "OFF")
             + "\nDernier refus: " + (lastDeniedUID.length() ? lastDeniedUID : "Aucun")
             + "\nCam: " + CAM_URL;
    telegramSend(s);
  }
}
void openDoor(int msOpen) {
  doorServo.setPeriodHertz(50);
  doorServo.attach(SERVO_PIN);
  doorServo.write(90);
  delay(msOpen);
  doorServo.write(0);
  delay(500);
  doorServo.detach();
}
void setup() {
  Serial.begin(115200);

  // I2C + OLED
  Wire.begin(I2C_SDA, I2C_SCL);
  u8g2.begin();

  oledMessage("DEMARRAGE...", "Scan I2C");
  int n = i2cScan();
  if (n == 0) {
    Serial.println("⚠️ Aucun device I2C detecte (OLED?)");
    oledMessage("I2C: 0 device", "Verif cablage");
    delay(1500);
  }

  // WiFi
  oledMessage("Connexion", "WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi OK");
    Serial.println(WiFi.localIP());
    oledMessage("WiFi OK", "Systeme pret");
    telegramSend("✅ ESP32 en ligne. Tape /status ou /open après un refus.");
  } else {
    Serial.println("WiFi ECHEC");
    oledMessage("WiFi ECHEC", "Mode offline");
  }

  // RFID
  SPI.begin(18, 19, 23, RFID_SS); // SCK,MISO,MOSI,SS
  rfid.PCD_Init();

  oledMessage("SYSTEME PRET", "Passez carte");
}
void loop() {
  // 1) écoute commandes Telegram (toutes les ~1s)
  static unsigned long lastTg = 0;
  if (millis() - lastTg > 1200) {
    handleTelegramCommands();
    lastTg = millis();
  }

  // 2) lecture RFID
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String uid = readUID();
  Serial.println("UID: " + uid);

  if (uid == ALLOWED_UID) {
    Serial.println("Acces Autorise");
    oledMessage("ACCES AUTORISE", "Ouverture...");
    openDoor(3000);
    oledMessage("SYSTEME PRET", "Passez carte");
    pendingRemoteApproval = false;
    lastDeniedUID = "";
  } else {
    Serial.println("Acces Refuse");
    oledMessage("ACCES REFUSE", "Notif Telegram");

    // marquer comme "en attente" pour /open
    pendingRemoteApproval = true;
    lastDeniedUID = uid;

    if (WiFi.status() == WL_CONNECTED && millis() - lastAlertMs > ALERT_COOLDOWN_MS) {
      String msg = "🚫 Accès refusé (UID: " + uid + ")\n"
                   "📷 Caméra: " + CAM_URL + "\n"
                   "✅ Si tu acceptes: tape /open";
      telegramSend(msg);
      lastAlertMs = millis();
    }
    delay(600);
    oledMessage("SYSTEME PRET", "Passez carte");
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(250);
}