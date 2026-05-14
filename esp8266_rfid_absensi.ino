#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Wire.h>
#include <SPI.h>
#define MFRC522_SPICLOCK (1000000u  )
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>

const char* WIFI_SSID = "iPhone";
const char* WIFI_PASSWORD = "12345678";
const char* API_URL = "http://172.20.10.2/absensi/index.php/api/absen";

constexpr uint8_t RFID_SS_PIN = D4;
constexpr uint8_t RFID_RST_PIN = D0;
constexpr uint8_t LCD_SDA_PIN = D2;
constexpr uint8_t LCD_SCL_PIN = D1;
constexpr uint8_t BUZZER_PIN = D8;
constexpr bool BUZZER_PASSIVE = true;
constexpr bool BUZZER_ACTIVE_HIGH = true;

constexpr unsigned long DUPLICATE_SCAN_WINDOW_MS = 3000;
constexpr unsigned long LCD_RESULT_HOLD_MS = 2000;
constexpr unsigned long RFID_IDLE_REINIT_MS = 30000;
constexpr uint8_t LCD_COLUMNS = 16;
constexpr uint8_t LCD_ROWS = 2;

LiquidCrystal_I2C lcd(0x27, LCD_COLUMNS, LCD_ROWS);
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

String lastUid = "";
unsigned long lastScanAt = 0;
unsigned long lastRfidOkAt = 0;

void buzzerWrite(bool on)
{
  digitalWrite(BUZZER_PIN, (BUZZER_ACTIVE_HIGH ? on : !on) ? HIGH : LOW);
}

void buzzerStop()
{
  if (BUZZER_PASSIVE) {
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  buzzerWrite(false);
}

void buzzerBeep(unsigned int onMs, unsigned int offMs = 0, uint8_t repeat = 1, unsigned int frequency = 2000)
{
  for (uint8_t i = 0; i < repeat; i++) {
    if (BUZZER_PASSIVE) {
      tone(BUZZER_PIN, frequency);
    } else {
      buzzerWrite(true);
    }

    delay(onMs);
    buzzerStop();

    if (i + 1 < repeat && offMs > 0) {
      delay(offMs);
    }
  }
}

void buzzSuccess()
{
  buzzerBeep(140, 40, 2, 3200);
}

void buzzInvalid()
{
  buzzerBeep(450, 0, 1, 1800);
}

void buzzAlready()
{
  buzzerBeep(100, 70, 2, 2800);
}

void buzzError()
{
  buzzerBeep(650, 0, 1, 1500);
}

String fitToLcd(const String& text)
{
  String output = text.substring(0, LCD_COLUMNS);

  while (output.length() < LCD_COLUMNS) {
    output += " ";
  }

  return output;
}

String simplifyForLcd(const String& text)
{
  if (text == "Presensi berhasil") return "Tepat Waktu!";
  if (text == "Sudah presensi") return "Sudah Presensi";
  if (text == "Belum terdaftar") return "Belum daftar";
  if (text == "Anda terlambat") return "Terlambat!";
  if (text == "WiFi terhubung") return "WiFi OK";
  if (text == "Membaca kartu") return "Baca kartu";
  if (text == "Menghubungkan") return "Hubung WiFi";
  return text;
}

void renderMessage(const String& line1, const String& line2 = "")
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(fitToLcd(line1));
  lcd.setCursor(0, 1);
  lcd.print(fitToLcd(line2));
}

void showMessage(const String& line1, const String& line2 = "")
{
  String safeLine1 = simplifyForLcd(line1);
  String safeLine2 = simplifyForLcd(line2);

  renderMessage(safeLine1, safeLine2);

  bool line1Long = safeLine1.length() > LCD_COLUMNS;
  bool line2Long = safeLine2.length() > LCD_COLUMNS;

  if (!line1Long && !line2Long) {
    return;
  }

  delay(1200);

  if (line1Long) {
    renderMessage(safeLine1.substring(LCD_COLUMNS), safeLine2);
  } else {
    renderMessage(safeLine1, safeLine2.substring(LCD_COLUMNS));
  }
}

String extractJsonValue(const String& source, const String& key)
{
  const String pattern = "\"" + key + "\":\"";
  int start = source.indexOf(pattern);
  if (start < 0) {
    return "";
  }

  start += pattern.length();
  int end = source.indexOf('"', start);
  if (end < 0) {
    return "";
  }

  return source.substring(start, end);
}

String readUid()
{
  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) {
      uid += "0";
    }
    uid += String(rfid.uid.uidByte[i], HEX);
  }

  uid.toUpperCase();
  return uid;
}

void reinitRfid(const char* reason)
{
  Serial.print("RFID reinit: ");
  Serial.println(reason);
  rfid.PCD_Reset();
  delay(50);
  rfid.PCD_Init();
  delay(4);
  lastRfidOkAt = millis();
}

bool connectWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  showMessage("Hubung WiFi", "Mohon tunggu");

  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");

    if (millis() - startedAt > 20000) {
      Serial.println();
      Serial.println("WiFi gagal: timeout 20 detik");
      Serial.print("WiFi.status(): ");
      Serial.println(WiFi.status());
      showMessage("WiFi gagal", "cek SSID/pass");
      return false;
    }
  }

  Serial.println();
  Serial.print("WiFi OK. IP ESP8266: ");
  Serial.println(WiFi.localIP());
  showMessage("WiFi OK", WiFi.localIP().toString());
  delay(1500);
  return true;
}

bool ensureWifi()
{
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  return connectWifi();
}

void postUidToServer(const String& uid)
{
  if (!ensureWifi()) {
    buzzError();
    showMessage("Server gagal", "WiFi putus");
    delay(LCD_RESULT_HOLD_MS);
    return;
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, API_URL);
  http.addHeader("Content-Type", "application/json");

  const String payload = "{\"uid\":\"" + uid + "\"}";

  Serial.print("POST UID: ");
  Serial.println(uid);

  int httpCode = http.POST(payload);
  String response = http.getString();

  Serial.print("HTTP code: ");
  Serial.println(httpCode);
  Serial.println(response);

  if (httpCode <= 0) {
    buzzError();
    showMessage("HTTP error", http.errorToString(httpCode));
    http.end();
    delay(LCD_RESULT_HOLD_MS);
    return;
  }

  String apiStatus = extractJsonValue(response, "status");
  String apiMessage = extractJsonValue(response, "message");
  String name = extractJsonValue(response, "nama");
  String line1 = extractJsonValue(response, "line1");
  String line2 = extractJsonValue(response, "line2");

  if (apiStatus == "success") {
    buzzSuccess();
    showMessage(line1.length() ? line1 : (name.length() ? name : uid),
                line2.length() ? line2 : (apiMessage.length() ? apiMessage : "Presensi OK"));
  } else if (apiStatus == "already") {
    buzzAlready();
    showMessage(line1.length() ? line1 : (name.length() ? name : uid),
                line2.length() ? line2 : "Sudah presensi");
  } else if (apiStatus == "invalid") {
    buzzInvalid();
    showMessage(line1.length() ? line1 : "Kartu invalid",
                line2.length() ? line2 : uid);
  } else {
    buzzError();
    showMessage("Server error", apiMessage.length() ? apiMessage : "cek API");
  }

  http.end();
  delay(LCD_RESULT_HOLD_MS);
}

void setup()
{
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("BOOT: setup mulai");

  pinMode(BUZZER_PIN, OUTPUT);
  buzzerStop();

  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Serial.println("BOOT: Wire.begin OK");
  lcd.init();
  Serial.println("BOOT: lcd.init OK");
  lcd.backlight();
  Serial.println("BOOT: lcd.backlight OK");
  showMessage("Booting...", "ESP8266 RFID");

  SPI.begin();
  Serial.println("BOOT: SPI.begin OK");
  rfid.PCD_Init();
  delay(4);
  Serial.println("BOOT: RFID init OK");
  lastRfidOkAt = millis();

  Serial.println("BOOT: mulai koneksi WiFi");
  if (connectWifi()) {
    Serial.println("Siap scan kartu RFID.");
    showMessage("Tempel kartu", "ke reader");
  } else {
    Serial.println("Board siap, tapi WiFi belum tersambung.");
    Serial.println("Cek hotspot 2.4GHz, SSID, password, dan IP server.");
    showMessage("WiFi belum OK", "scan tetap bisa");
  }
}

void loop()
{
  ensureWifi();

  if ((millis() - lastRfidOkAt) > RFID_IDLE_REINIT_MS) {
    reinitRfid("idle timeout");
  }

  if (!rfid.PICC_IsNewCardPresent()) {
    return;
  }

  lastRfidOkAt = millis();

  if (!rfid.PICC_ReadCardSerial()) {
    reinitRfid("read serial failed");
    return;
  }

  String uid = readUid();
  unsigned long now = millis();

  if (uid == lastUid && (now - lastScanAt) < DUPLICATE_SCAN_WINDOW_MS) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  lastUid = uid;
  lastScanAt = now;

  showMessage("Baca kartu", uid);
  postUidToServer(uid);
  showMessage("Tempel kartu", "ke reader");

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
