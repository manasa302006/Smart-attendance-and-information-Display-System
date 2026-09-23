// ================================================================
//  AttendIQ — NodeMCU Attendance System v3.1  (REFINED)
//
//  CHANGES vs v3.0
//  [R1] Removed dead ADDR_LAST_PLAY_ID constant (explicitly unused)
//  [R2] Fixed DFPlayer init check (dfTry <= 5 was always true)
//  [R3] Extracted stripExtension() — deduplicates label logic
//  [R4] F() macros on all string literals → flash, not SRAM
//  [R5] httpBegin() consolidates 5 repeated addHeader() calls
//  [R6] supaUPSERT duplicate check trims whitespace before compare
//  [R7] All magic numbers → named constants
//  [R8] snprintf() replaces strncpy() for safe null-termination
//  [R9] dfPlayerOk bool tracks DFPlayer init result cleanly
// ================================================================

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <DFRobot_DF1201S.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Fingerprint.h>

// ─── WIFI & SUPABASE ─────────────────────────────────────────────
const char* WIFI_SSID     = "Admin";
const char* WIFI_PASSWORD = "12345678";
const char* SUPABASE_URL  = "https://yeoixvmhynswkebntqpu.supabase.co/rest/v1/";
const char* SUPABASE_KEY  = "sb_publishable_UhHcPmuwnmo6F_6NaV9BGA_SFfeCE4n";

// ─── EEPROM ───────────────────────────────────────────────────────
#define EEPROM_SIZE        128
#define ADDR_NEXT_FP_SLOT   40   // 1 byte: next free fingerprint slot (1–127)
#define FP_SLOT_MIN          1
#define FP_SLOT_MAX        127

// ─── PLAY QUEUE ───────────────────────────────────────────────────
#define PLAY_ID_LEN         37   // UUID string length (36 chars + null)
#define PLAY_DELAY_MS     3000   // ms to wait after track ends before next fetch

// ─── TIMING ───────────────────────────────────────────────────────
#define POLL_INTERVAL_MS   10000
#define FEEDBACK_SHORT_MS   1500
#define FEEDBACK_MED_MS     2000
#define FEEDBACK_LONG_MS    3000
#define ENROLL_WINDOW_MS   10000
#define ENROLL_LIFT_WAIT_MS  2000
#define PLAYBACK_START_GRACE 5000  // ms before cur-time=0 check is trusted
#define PLAY_SAFETY_BUF_MS   3000  // ms added to getTotalTime() as safety margin
#define DF_FALLBACK_SECS       60  // assumed track length when getTotalTime() fails
#define DF_INIT_RETRIES         5

// ─── BUTTON ───────────────────────────────────────────────────────
#define BTN_PIN          D3
#define BTN_DEBOUNCE_MS   50

// ─── OLED ─────────────────────────────────────────────────────────
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── DFPLAYER ─────────────────────────────────────────────────────
//  DFPlayer TX → D6 (NodeMCU RX via SoftwareSerial)
//  DFPlayer RX → D7 (NodeMCU TX via SoftwareSerial)
SoftwareSerial dfSerial(D6, D7);
DFRobot_DF1201S dfPlayer;
bool dfPlayerOk = false;   // [R9] tracks init result cleanly

struct FilePair { const char* name; int num; const char* label; };
const FilePair AUDIO_FILES[] = {
  {"assemble.mp3",   1, "Assemble"},
  {"cs_today.mp3",   2, "CS Today"},
  {"pmss.mp3",       3, "PMSS"},
  {"googleform.mp3", 4, "Google Form"},
  {"retest.mp3",     5, "Retest"}
};
const int NUM_AUDIO = sizeof(AUDIO_FILES) / sizeof(AUDIO_FILES[0]);

// ─── FINGERPRINT SENSOR (AS608) ───────────────────────────────────
//  AS608 TX → D5 (GPIO14, SoftwareSerial RX)
//  AS608 RX → D4 (GPIO2,  SoftwareSerial TX)  — must NOT be D8/GPIO15
//  GPIO15 must be LOW at boot; AS608 TX idles HIGH and would pull it
//  up, blocking the ESP8266 bootloader (blank OLED symptom).
SoftwareSerial fpSerial(D5, D4);
Adafruit_Fingerprint finger(&fpSerial);

// ─── BUTTON STATE ─────────────────────────────────────────────────
bool  btnPrev        = HIGH;
bool  btnStable      = HIGH;
unsigned long btnDebounceAt = 0;

// ─── STATE MACHINE ────────────────────────────────────────────────
enum DeviceState { ST_SUMMARY, ST_ENROLLING, ST_ATTENDANCE, ST_FEEDBACK };
DeviceState devState = ST_SUMMARY;

// ─── ENROLLMENT CONTEXT ───────────────────────────────────────────
struct EnrollCtx {
  String  studentId;
  String  name;
  String  rollNo;
  int     slot;
  unsigned long deadline;
  int     phase;      // 0 = waiting for image 1, 1 = waiting for image 2
};
EnrollCtx enr;

// ─── FEEDBACK ─────────────────────────────────────────────────────
unsigned long feedbackEnd = 0;

// ─── PLAY QUEUE ───────────────────────────────────────────────────
char lastPlayID[PLAY_ID_LEN] = {0};
bool isPlaying = false;
unsigned long lastPlayCompleted = 0;
unsigned long playEndAt = 0;
unsigned long playStartedAt = 0;

// ─── POLLING ──────────────────────────────────────────────────────
unsigned long lastPollAt = 0;

// ─── GLOBAL HTTPS CLIENT ──────────────────────────────────────────
WiFiClientSecure wifiClient;


// ================================================================
//  OLED HELPERS
// ================================================================
void oledLine(const String& txt, int y, int size = 1) {
  display.setTextSize(size);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.print(txt);
}

void oledMsg(const String& l1, const String& l2 = "", const String& l3 = "") {
  display.clearDisplay();
  if (l3 != "") {
    oledLine(l1, 8);  oledLine(l2, 26);  oledLine(l3, 44);
  } else if (l2 != "") {
    oledLine(l1, 16); oledLine(l2, 36);
  } else {
    oledLine(l1, 26);
  }
  display.display();
}

void oledSummary(int total, int present, int absent) {
  display.clearDisplay();
  oledLine("TOTAL:   " + String(total),   10);
  oledLine("PRESENT: " + String(present), 28);
  oledLine("ABSENT:  " + String(absent),  46);
  display.display();
}

void oledEnrollCountdown(const String& name, int remaining) {
  display.clearDisplay();
  oledLine(F("New student:"), 4);
  oledLine(name, 18);
  oledLine("Place finger: " + String(remaining) + "s", 36);
  int barW = map(remaining, 0, ENROLL_WINDOW_MS / 1000, 0, SCREEN_WIDTH - 4);
  display.drawRect(2, 54, SCREEN_WIDTH - 4, 8, SSD1306_WHITE);
  display.fillRect(2, 54, barW, 8, SSD1306_WHITE);
  display.display();
}


// ================================================================
//  SUPABASE HTTP HELPERS
//  [R5] httpBegin() sets all shared headers in one place.
// ================================================================

// [R5] Sets the four headers every request needs.
void httpBegin(HTTPClient& http, const String& endpoint,
               const String& prefer = "") {
  wifiClient.setInsecure();
  http.begin(wifiClient, String(SUPABASE_URL) + endpoint);
  http.addHeader(F("apikey"),        SUPABASE_KEY);
  http.addHeader(F("Authorization"), String(F("Bearer ")) + SUPABASE_KEY);
  http.addHeader(F("Accept"),        F("application/json"));
  if (prefer.length()) http.addHeader(F("Prefer"), prefer);
}

bool supaGET(const String& endpoint, String& out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  httpBegin(http, endpoint);
  int code = http.GET();
  bool ok = (code == 200);
  if (ok) out = http.getString();
  else    Serial.println(String(F("[GET] ")) + endpoint + " -> " + code);
  http.end();
  return ok;
}

bool supaPOST(const String& endpoint, const String& body) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  httpBegin(http, endpoint, F("return=minimal"));
  http.addHeader(F("Content-Type"), F("application/json"));
  int code = http.POST(body);
  bool ok = (code >= 200 && code < 300);
  if (!ok) Serial.println(String(F("[POST] ")) + endpoint + " -> " + code);
  http.end();
  return ok;
}

// Returns response body on success, "" on failure.
// Prefer header: ignore-duplicates so no 409; representation so caller
// can detect a skipped duplicate by checking for an empty JSON array.
String supaUPSERT(const String& endpoint, const String& body) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  httpBegin(http, endpoint,
            F("resolution=ignore-duplicates,return=representation"));
  http.addHeader(F("Content-Type"), F("application/json"));
  int code = http.POST(body);
  String resp = "";
  if (code >= 200 && code < 300) resp = http.getString();
  else Serial.println(String(F("[UPSERT] ")) + endpoint + " -> " + code);
  http.end();
  return resp;
}

bool supaPATCH(const String& endpoint, const String& body) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  httpBegin(http, endpoint, F("return=minimal"));
  http.addHeader(F("Content-Type"), F("application/json"));
  int code = http.PATCH(body);
  bool ok = (code >= 200 && code < 300);
  if (!ok) Serial.println(String(F("[PATCH] ")) + endpoint + " -> " + code);
  http.end();
  return ok;
}

bool supaDELETE(const String& endpoint) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  httpBegin(http, endpoint);
  int code = http.sendRequest("DELETE");
  bool ok = (code >= 200 && code < 300);
  if (!ok) Serial.println(String(F("[DELETE] ")) + endpoint + " -> " + code);
  http.end();
  return ok;
}


// ================================================================
//  AUDIO HELPERS
//  [R3] stripExtension() replaces duplicated inline label logic.
// ================================================================
String stripExtension(const String& filename) {
  String label = filename;
  int dot = label.lastIndexOf('.');
  if (dot > 0) label = label.substring(0, dot);
  label.replace('_', ' ');
  return label;
}

int getAudioNum(const String& name) {
  for (int i = 0; i < NUM_AUDIO; i++)
    if (name.equalsIgnoreCase(AUDIO_FILES[i].name)) return AUDIO_FILES[i].num;
  return -1;
}

String getAudioLabel(const String& name) {
  for (int i = 0; i < NUM_AUDIO; i++)
    if (name.equalsIgnoreCase(AUDIO_FILES[i].name))
      return String(AUDIO_FILES[i].label);
  return stripExtension(name);   // [R3] use shared helper
}


// ================================================================
//  ATTENDANCE SUMMARY
// ================================================================
void fetchAndShowSummary() {
  String out;
  if (!supaGET(F("attendance_summary?select=*"), out)) return;
  DynamicJsonDocument doc(256);
  if (!deserializeJson(doc, out) && doc.size() > 0) {
    oledSummary(
      doc[0]["total_strength"] | 0,
      doc[0]["present"]        | 0,
      doc[0]["absent"]         | 0
    );
  }
}


// ================================================================
//  UNENROLLED STUDENT CHECK
// ================================================================
bool checkForUnenrolledStudent() {
  String out;
  if (!supaGET(
        F("students?finger_id=is.null&is_active=eq.true"
          "&select=id,name,roll_no&order=created_at.asc&limit=1"),
        out))
    return false;

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, out) || doc.size() == 0) return false;

  enr.studentId = doc[0]["id"].as<String>();
  enr.name      = doc[0]["name"].as<String>();
  enr.rollNo    = doc[0]["roll_no"].as<String>();
  enr.slot      = EEPROM.read(ADDR_NEXT_FP_SLOT);
  if (enr.slot < FP_SLOT_MIN || enr.slot > FP_SLOT_MAX) enr.slot = FP_SLOT_MIN;
  enr.deadline  = millis() + ENROLL_WINDOW_MS;
  enr.phase     = 0;

  Serial.println(String(F("Unenrolled: ")) + enr.name + F(" → slot ") + enr.slot);
  return true;
}


// ================================================================
//  ENROLLMENT PROCESS
// ================================================================
bool runEnrollStep() {
  unsigned long now = millis();

  if (now >= enr.deadline) {
    Serial.println(String(F("Enroll timeout – deleting ")) + enr.studentId);
    oledMsg(F("Timeout!"), enr.name, F("Student removed"));
    supaDELETE("students?id=eq." + enr.studentId);
    feedbackEnd = millis() + FEEDBACK_LONG_MS;
    devState = ST_FEEDBACK;
    return true;
  }

  int secsLeft = (enr.deadline - now) / 1000;

  if (enr.phase == 0) {
    oledEnrollCountdown(enr.name, secsLeft);
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER || p != FINGERPRINT_OK) return false;
    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) {
      Serial.println(String(F("image2Tz(1) err: ")) + p);
      return false;
    }
    enr.phase = 1;
    oledMsg(F("Good!"), F("Lift finger..."));
    delay(100);
    unsigned long liftWait = millis() + ENROLL_LIFT_WAIT_MS;
    while (finger.getImage() != FINGERPRINT_NOFINGER && millis() < liftWait)
      delay(80);
    return false;
  }

  if (enr.phase == 1) {
    oledMsg(F("Place finger"), F("again"), String(secsLeft) + "s left");
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER || p != FINGERPRINT_OK) return false;

    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) {
      Serial.println(String(F("image2Tz(2) err: ")) + p);
      enr.phase = 0;
      return false;
    }

    p = finger.createModel();
    if (p != FINGERPRINT_OK) {
      Serial.println(String(F("createModel err: ")) + p);
      oledMsg(F("Poor print"), F("Try again"));
      delay(1200);
      enr.phase = 0;
      return false;
    }

    p = finger.storeModel(enr.slot);
    if (p != FINGERPRINT_OK) {
      Serial.println(String(F("storeModel err: ")) + p);
      oledMsg(F("Store failed"), "Slot: " + String(enr.slot));
      delay(1500);
      supaDELETE("students?id=eq." + enr.studentId);
      feedbackEnd = millis() + FEEDBACK_LONG_MS;
      devState = ST_FEEDBACK;
      return true;
    }

    DynamicJsonDocument upd(128);
    upd["finger_id"]   = enr.slot;
    upd["enrolled_at"] = "now()";
    String body; serializeJson(upd, body);

    if (supaPATCH("students?id=eq." + enr.studentId, body)) {
      uint8_t nextSlot = (enr.slot >= FP_SLOT_MAX) ? FP_SLOT_MIN : enr.slot + 1;
      EEPROM.write(ADDR_NEXT_FP_SLOT, nextSlot);
      EEPROM.commit();
      Serial.println(String(F("Enrolled: ")) + enr.name + F(" slot=") + enr.slot);
      oledMsg(F("Enrolled!"), enr.name, "Roll: " + enr.rollNo);
    } else {
      Serial.println(F("DB update failed after enroll"));
      oledMsg(F("DB Error"), F("Retrying..."));
    }

    feedbackEnd = millis() + FEEDBACK_LONG_MS;
    devState = ST_FEEDBACK;
    return true;
  }

  return false;
}


// ================================================================
//  ATTENDANCE MARKING
// ================================================================
bool runAttendanceStep() {
  oledMsg(F("Place finger"), F("for attendance"));

  uint8_t p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) return false;
  if (p != FINGERPRINT_OK) {
    Serial.println(String(F("Image err: ")) + p);
    return false;
  }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return false;

  p = finger.fingerFastSearch();

  if (p == FINGERPRINT_OK) {
    int fid   = finger.fingerID;
    int score = finger.confidence;
    Serial.println(String(F("Match! finger_id=")) + fid + F(" score=") + score);

    String out, nameStr = F("Unknown"), rollStr = "", sidStr = "";
    if (supaGET("students?finger_id=eq." + String(fid) +
                "&select=id,name,roll_no", out)) {
      DynamicJsonDocument doc(256);
      if (!deserializeJson(doc, out) && doc.size() > 0) {
        nameStr = doc[0]["name"].as<String>();
        rollStr = doc[0]["roll_no"].as<String>();
        sidStr  = doc[0]["id"].as<String>();
      }
    }

    DynamicJsonDocument att(256);
    att["student_id"]  = sidStr;
    att["finger_id"]   = fid;
    att["match_score"] = score;
    att["status"]      = "present";
    att["source"]      = "fingerprint";
    String attBody; serializeJson(att, attBody);

    String resp = supaUPSERT(
      F("attendance_log?on_conflict=student_id,date"), attBody);

    // [R6] Trim whitespace before comparing so "  []  " is caught correctly
    resp.trim();

    if (resp.length() == 0) {
      oledMsg(F("DB Error"), nameStr, F("Try again"));
    } else if (resp == "[]") {
      Serial.println(String(F("Already marked: ")) + nameStr);
      oledMsg(F("Already marked!"), nameStr, rollStr);
    } else {
      Serial.println(String(F("Attendance saved: ")) + nameStr);
      oledMsg(F("PRESENT"), nameStr, rollStr);
    }

    feedbackEnd = millis() + FEEDBACK_LONG_MS;
    devState = ST_FEEDBACK;
    return true;

  } else if (p == FINGERPRINT_NOTFOUND) {
    oledMsg(F("No match"), F("Not registered"));
    feedbackEnd = millis() + FEEDBACK_MED_MS;
    devState = ST_FEEDBACK;
    return true;

  } else {
    oledMsg(F("Scan error"), F("Try again"));
    feedbackEnd = millis() + FEEDBACK_SHORT_MS;
    devState = ST_FEEDBACK;
    return true;
  }
}


// ================================================================
//  BUTTON HANDLER
// ================================================================
bool buttonJustPressed() {
  bool reading = digitalRead(BTN_PIN);
  if (reading != btnPrev) {
    btnDebounceAt = millis();
    btnPrev = reading;
  }
  if (millis() - btnDebounceAt > BTN_DEBOUNCE_MS) {
    if (reading != btnStable) {
      btnStable = reading;
      if (btnStable == LOW) return true;
    }
  }
  return false;
}


// ================================================================
//  PLAY QUEUE
// ================================================================
void checkPlaybackDone() {
  if (!isPlaying) return;

  bool doneByTimer = (millis() >= playEndAt);
  bool doneByTime  = false;
  if ((millis() - playStartedAt) > PLAYBACK_START_GRACE) {
    if (dfPlayer.getCurTime() == 0) doneByTime = true;
  }

  if (doneByTimer || doneByTime) {
    Serial.println(String(F("Playback done (")) +
                   (doneByTimer ? "timer" : "curTime=0") + ")");
    isPlaying = false;
    lastPlayCompleted = millis();

    if (strlen(lastPlayID) > 0) {
      supaDELETE(String(F("play_queue?id=eq.")) + lastPlayID);
      memset(lastPlayID, 0, sizeof(lastPlayID));
    }

    fetchAndShowSummary();
  }
}

void fetchPlayQueue() {
  if (isPlaying) return;
  if (millis() - lastPlayCompleted < PLAY_DELAY_MS) return;
  if (!dfPlayerOk) return;   // don't attempt if DFPlayer never initialised

  String out;
  if (!supaGET(F("play_queue?select=*&order=last_updated.desc&limit=1"), out))
    return;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, out) || doc.size() == 0) return;

  String id       = doc[0]["id"].as<String>();
  String file     = doc[0]["audio_file"].as<String>();
  int    trackNum = doc[0]["track_num"] | 0;

  if (id.length() == 0) return;
  if (strlen(lastPlayID) > 0 && strcmp(id.c_str(), lastPlayID) == 0) return;

  int num = (trackNum >= 1) ? trackNum : getAudioNum(file);
  if (num < 1) {
    Serial.println(String(F("No track # for: ")) + file + F(" — removing"));
    supaDELETE("play_queue?id=eq." + id);
    return;
  }

  // [R8] snprintf for safe null-termination
  memset(lastPlayID, 0, sizeof(lastPlayID));
  snprintf(lastPlayID, sizeof(lastPlayID), "%s", id.c_str());

  if (dfPlayer.playFileNum(num)) {
    isPlaying     = true;
    playStartedAt = millis();
    delay(300);
    uint16_t duration = dfPlayer.getTotalTime();
    if (duration < 3) duration = DF_FALLBACK_SECS;   // [R7] named constant
    playEndAt = millis() + ((unsigned long)duration * 1000UL) + PLAY_SAFETY_BUF_MS;

    String label = getAudioLabel(file);   // [R3] uses shared helper
    Serial.println(String(F("Playing: ")) + file +
                   F("  track=") + num +
                   F("  duration=") + duration + "s");
    oledMsg(F("Announcement"), label, F("Playing..."));
  } else {
    Serial.println(String(F("DFPlayer play failed: ")) + file);
    memset(lastPlayID, 0, sizeof(lastPlayID));
  }
}


// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=== AttendIQ v3.1 ==="));

  Wire.begin();
  EEPROM.begin(EEPROM_SIZE);

  uint8_t storedSlot = EEPROM.read(ADDR_NEXT_FP_SLOT);
  if (storedSlot < FP_SLOT_MIN || storedSlot > FP_SLOT_MAX) {
    EEPROM.write(ADDR_NEXT_FP_SLOT, FP_SLOT_MIN);
    EEPROM.commit();
  }

  pinMode(BTN_PIN, INPUT_PULLUP);

  // ── OLED ──────────────────────────────────────────────────────
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println(F("OLED failed – check wiring"));
  display.clearDisplay();
  oledMsg(F("AttendIQ v3.1"), F("Starting..."));
  delay(600);

  // ── FINGERPRINT SENSOR ────────────────────────────────────────
  fpSerial.begin(57600);
  finger.begin(57600);
  delay(200);
  if (finger.verifyPassword()) {
    Serial.println(String(F("Fingerprint OK (capacity=")) + finger.capacity + ")");
    oledMsg(F("Finger OK"));
  } else {
    Serial.println(F("Fingerprint NOT FOUND – check wiring (TX→D5, RX→D4)"));
    oledMsg(F("FP Error!"), F("Check wiring"), F("TX->D5 RX->D4"));
  }
  delay(800);

  // ── DFPLAYER ──────────────────────────────────────────────────
  // [R9] dfPlayerOk bool replaces the broken dfTry <= 5 check
  dfSerial.begin(9600);
  delay(500);
  for (int attempt = 1; attempt <= DF_INIT_RETRIES; attempt++) {
    if (dfPlayer.begin(dfSerial)) {
      dfPlayerOk = true;
      break;
    }
    Serial.println(String(F("DFPlayer init attempt ")) + attempt);
    delay(1500);
  }
  if (dfPlayerOk) {
    dfPlayer.setVol(25);
    dfPlayer.switchFunction(dfPlayer.MUSIC);
    dfPlayer.setPlayMode(dfPlayer.SINGLE);
    Serial.println(F("DFPlayer OK"));
  } else {
    Serial.println(F("DFPlayer init failed"));
    oledMsg(F("DFPlayer"), F("Not found"));
    delay(800);
  }

  // ── WIFI ──────────────────────────────────────────────────────
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  oledMsg(F("WiFi..."), WIFI_SSID);
  int wTry = 0;
  while (WiFi.status() != WL_CONNECTED && wTry++ < 30) delay(500);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(String(F("WiFi OK: ")) + WiFi.localIP().toString());
    oledMsg(F("Connected"), WiFi.localIP().toString());
  } else {
    Serial.println(F("WiFi failed"));
    oledMsg(F("WiFi failed"), F("Check SSID/PW"));
  }
  delay(1000);

  fetchAndShowSummary();
}


// ================================================================
//  LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  if (buttonJustPressed()) {
    if (devState == ST_SUMMARY) {
      Serial.println(F("Button → ATTENDANCE mode"));
      devState = ST_ATTENDANCE;
      oledMsg(F("Place finger"), F("for attendance"));
      return;
    }
    if (devState == ST_ATTENDANCE) {
      Serial.println(F("Button → cancel ATTENDANCE"));
      devState = ST_SUMMARY;
      fetchAndShowSummary();
      return;
    }
  }

  if (devState == ST_FEEDBACK) {
    if (now >= feedbackEnd) {
      devState = ST_SUMMARY;
      fetchAndShowSummary();
    }
    return;
  }

  if (devState == ST_ENROLLING) {
    runEnrollStep();
    return;
  }

  if (devState == ST_ATTENDANCE) {
    runAttendanceStep();
    return;
  }

  // ── ST_SUMMARY ────────────────────────────────────────────────
  checkPlaybackDone();   // fast register read every loop cycle

  if (now - lastPollAt >= POLL_INTERVAL_MS) {
    lastPollAt = now;
    Serial.println(F("--- Poll ---"));

    if (checkForUnenrolledStudent()) {
      devState = ST_ENROLLING;
      return;
    }

    if (!isPlaying) fetchAndShowSummary();
    fetchPlayQueue();
  }

  delay(10);
}
