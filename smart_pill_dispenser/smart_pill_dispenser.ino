/********************************************************************
 SMART PILL DISPENSER - WATCHDOG FIXED
 (No More Reset Errors)
********************************************************************/
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <RTClib.h>
#include <U8g2lib.h>
#include <ESP32Servo.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>

/********************************************************************
 CONFIG
********************************************************************/
#define SERVO_PIN       18
#define BUZZER_PIN      19
#define BUTTON_PIN      4
#define OLED_SDA        8
#define OLED_SCL        9

#define MAX_SCHEDULES       3
#define MAX_LOG_ENTRIES      50
#define ON_TIME_WINDOW_SEC   (10 * 60)
#define LATE_WINDOW_SEC      (60 * 60)

// ⚠️ YOUR HOME WiFi (MUST HAVE INTERNET)
const char* WIFI_SSID = "Kumar";
const char* WIFI_PASS = "kumar@123";

// SMS Configuration
const char* SMS_HOST = "www.circuitdigest.cloud";
const char* SMS_API_KEY = "cd_abi_030326_U1S9ez";
const char* SMS_PHONE = "919345809896";

// AP Configuration
const char* AP_SSID = "SmartPill_Dispenser";
const char* AP_PASS = "1234567890";
const int AP_CHANNEL = 1;

/********************************************************************
 DATA STRUCTURES
********************************************************************/
struct PillTime {
    int hour;
    int minute;
};

enum DisplayMode {
    DISP_BOOT,
    DISP_NORMAL,
    DISP_ALARM,
    DISP_FACE_VERIFY,
    DISP_CONFIRMED,
    DISP_DONE
};

enum SystemState {
    SYS_IDLE,
    SYS_ALARM_TRIGGERED,
    SYS_WAITING_FACE,
    SYS_DISPENSING,
    SYS_SHOW_CONFIRMED,
    SYS_SHOW_DONE
};

enum DoseStatus {
    DOSE_ON_TIME,
    DOSE_LATE,
    DOSE_MISSED
};

struct DoseLog {
    uint32_t scheduledEpoch;
    uint32_t takenEpoch;
    DoseStatus status;
};

/********************************************************************
 FORWARD DECLARATIONS
********************************************************************/
class AlarmManager;
class SystemManager;
class WiFiManagerCustom;
class DisplayManager;
class ServoDriver;

// Global pointers
AlarmManager* g_alarmManager = nullptr;
DisplayManager* g_displayManager = nullptr;
SystemManager* g_systemManager = nullptr;

/********************************************************************
 BUTTON DEBOUNCER
********************************************************************/
class ButtonDebouncer {
public:
    void init(uint8_t pin) {
        _pin = pin;
        pinMode(_pin, INPUT_PULLUP);
        _lastState = true;
        _buttonState = true;
        _lastDebounceTime = 0;
    }

    bool wasPressed() {
        bool reading = digitalRead(_pin);
        if (reading != _lastState) {
            _lastDebounceTime = millis();
        }
        if ((millis() - _lastDebounceTime) > 50) {
            if (reading != _buttonState) {
                _buttonState = reading;
                if (_buttonState == LOW) {
                    _lastState = reading;
                    return true;
                }
            }
        }
        _lastState = reading;
        return false;
    }

private:
    uint8_t _pin;
    bool _lastState;
    bool _buttonState;
    unsigned long _lastDebounceTime;
};

/********************************************************************
 SERVO DRIVER
********************************************************************/
class ServoDriver {
public:
    void init();
    void setAngle(uint8_t angle);
    void dispense();
    void update();
    bool isStillMoving() const { return _isMoving; }

private:
    Servo myServo;
    bool _isMoving = false;
    unsigned long _moveStartTime = 0;
};

void ServoDriver::init() {
    // FIXED: Only allocate 2 timers instead of 4 (prevents conflicts)
    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    
    myServo.setPeriodHertz(50);
    myServo.attach(SERVO_PIN, 500, 2400);
    setAngle(0);
    Serial.println("✅ Servo Initialized");
}

void ServoDriver::setAngle(uint8_t angle) {
    if (angle > 180) angle = 180;
    myServo.write(angle);
}

void ServoDriver::dispense() {
    _isMoving = true;
    _moveStartTime = millis();
    setAngle(90);
    Serial.println("🔄 Servo Dispensing...");
}

void ServoDriver::update() {
    if (_isMoving && millis() - _moveStartTime > 1000) {
        setAngle(0);
        _isMoving = false;
        Serial.println("✅ Servo Complete");
    }
}

/********************************************************************
 DISPLAY MANAGER
********************************************************************/
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

class DisplayManager {
public:
    void init();
    void update();
    void setRTC(DateTime now);
    void setSchedules(PillTime* schedules);
    void setNextDose(int hour, int minute);
    void setMode(DisplayMode mode);
    void refreshDisplay();

private:
    DisplayMode mode = DISP_BOOT;
    DateTime currentTime;
    PillTime* pill = nullptr;
    int nextHour = 0;
    int nextMinute = 0;
    unsigned long lastAnim = 0;
    int animFrame = 0;
    unsigned long lastRefresh = 0;

    void drawBoot();
    void drawNormal();
    void drawAlarm();
    void drawFaceVerify();
    void drawConfirmed();
    void drawDone();
};

void DisplayManager::init() {
    Wire.begin(OLED_SDA, OLED_SCL);
    u8g2.begin();
    drawBoot();
    delay(1000);  // FIXED: Reduced from 2500ms
    mode = DISP_NORMAL;
    Serial.println("✅ OLED Display Initialized");
    g_displayManager = this;
}

void DisplayManager::setRTC(DateTime now) { 
    currentTime = now;
    refreshDisplay();
}

void DisplayManager::setSchedules(PillTime* s) { 
    pill = s;
    refreshDisplay();
}

void DisplayManager::setNextDose(int h, int m) { 
    nextHour = h; 
    nextMinute = m;
    refreshDisplay();
}

void DisplayManager::setMode(DisplayMode m) {
    mode = m;
    animFrame = 0;
    lastAnim = millis();
    refreshDisplay();
}

void DisplayManager::refreshDisplay() {
    lastRefresh = millis();
}

void DisplayManager::update() {
    if (mode == DISP_NORMAL && millis() - lastRefresh > 5000) {
        refreshDisplay();
    }
    
    if (millis() - lastAnim > 80) {
        animFrame++;
        lastAnim = millis();
    }
    
    switch (mode) {
        case DISP_BOOT: drawBoot(); break;
        case DISP_NORMAL: drawNormal(); break;
        case DISP_ALARM: drawAlarm(); break;
        case DISP_FACE_VERIFY: drawFaceVerify(); break;
        case DISP_CONFIRMED: drawConfirmed(); break;
        case DISP_DONE: drawDone(); break;
    }
}

void DisplayManager::drawBoot() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_pressstart2p_8f);
    u8g2.drawStr(12, 25, "SMARTPILL");
    u8g2.drawStr(30, 42, "PRO");
    u8g2.sendBuffer();
}

void DisplayManager::drawNormal() {
    u8g2.clearBuffer();
    
    char timeBuf[9];
    sprintf(timeBuf, "%02d:%02d", currentTime.hour(), currentTime.minute());
    u8g2.setFont(u8g2_font_inb19_mn);
    int tw = u8g2.getStrWidth(timeBuf);
    u8g2.drawStr(64 - (tw / 2), 24, timeBuf);

    char dateBuf[12];
    sprintf(dateBuf, "%02d/%02d/%04d",
            currentTime.day(), currentTime.month(), currentTime.year());
    u8g2.setFont(u8g2_font_6x10_tf);
    int dw = u8g2.getStrWidth(dateBuf);
    u8g2.drawStr(64 - (dw / 2), 40, dateBuf);

    char nextBuf[20];
    sprintf(nextBuf, "Next: %02d:%02d", nextHour, nextMinute);
    int nw = u8g2.getStrWidth(nextBuf);
    u8g2.drawStr(64 - (nw / 2), 58, nextBuf);

    u8g2.sendBuffer();
}

void DisplayManager::drawAlarm() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_pressstart2p_8f);
    if ((animFrame / 4) % 2 == 0) {
        u8g2.drawStr(16, 20, "!! ALARM !!");
    }
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(15, 40, "Time to take your");
    u8g2.drawStr(30, 55, "medication!");
    u8g2.sendBuffer();
}

void DisplayManager::drawFaceVerify() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_pressstart2p_8f);
    u8g2.drawStr(15, 20, "FACE SCAN");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(10, 40, "Looking at");
    u8g2.drawStr(20, 55, "Camera...");
    u8g2.sendBuffer();
}

void DisplayManager::drawConfirmed() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_pressstart2p_8f);
    u8g2.drawStr(20, 25, "TABLET");
    u8g2.drawStr(25, 45, "TAKEN!");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(20, 60, "Dispensing...");
    u8g2.sendBuffer();
}

void DisplayManager::drawDone() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_pressstart2p_8f);
    u8g2.drawStr(30, 30, "DONE!");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(15, 50, "Stay healthy :)");
    u8g2.sendBuffer();
}

/********************************************************************
 ALARM MANAGER
********************************************************************/
class AlarmManager {
public:
    void init();
    void update();
    bool isAlarmDue();
    void markAlarmHandled();
    void setRTCFromString(String dt);
    String getTimestamp();
    String getDateTimeLocal();
    DateTime getRTC();
    void setSchedule(int index, int hour, int minute);
    PillTime* getSchedules();
    bool rtcOK = false;

private:
    RTC_DS3231 rtc;
    PillTime pill[MAX_SCHEDULES];
    bool alarmTriggered = false;
    bool firedToday[MAX_SCHEDULES];
};

void AlarmManager::init() {
    if (!rtc.begin()) {
        Serial.println("❌ ERROR: RTC not found!");
        rtcOK = false;
        for (int i = 0; i < MAX_SCHEDULES; i++) {
            pill[i].hour = 8 + i;
            pill[i].minute = 0;
        }
        return;
    }
    rtcOK = true;
    Serial.println("✅ RTC Initialized");

    if (rtc.lostPower()) {
        Serial.println("⚠️ RTC lost power — setting compile time");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        pill[i].hour = 8 + i;
        pill[i].minute = 0;
    }
    Serial.println("📅 Default schedules set");

    for (int i = 0; i < MAX_SCHEDULES; i++) firedToday[i] = false;
    
    g_alarmManager = this;
}

void AlarmManager::update() {
    if (!rtcOK) return;
    DateTime now = rtc.now();

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (now.hour() == pill[i].hour &&
            now.minute() == pill[i].minute &&
            !firedToday[i]) {
            alarmTriggered = true;
            firedToday[i] = true;
            Serial.printf("⏰ Alarm triggered for schedule %d\n", i);
        }
    }

    if (now.hour() == 0 && now.minute() == 0) {
        for (int i = 0; i < MAX_SCHEDULES; i++) firedToday[i] = false;
    }
}

bool AlarmManager::isAlarmDue() { return alarmTriggered; }
void AlarmManager::markAlarmHandled() { alarmTriggered = false; }
String AlarmManager::getTimestamp() { DateTime now = rtc.now(); return String(now.timestamp()); }
DateTime AlarmManager::getRTC() { return rtc.now(); }

String AlarmManager::getDateTimeLocal() {
    DateTime now = rtc.now();
    char buffer[20];
    sprintf(buffer, "%04d-%02d-%02dT%02d:%02d",
            now.year(), now.month(), now.day(),
            now.hour(), now.minute());
    return String(buffer);
}

void AlarmManager::setRTCFromString(String dt) {
    if (dt.length() < 16) return;
    int year = dt.substring(0, 4).toInt();
    int month = dt.substring(5, 7).toInt();
    int day = dt.substring(8, 10).toInt();
    int hour = dt.substring(11, 13).toInt();
    int minute = dt.substring(14, 16).toInt();
    rtc.adjust(DateTime(year, month, day, hour, minute, 0));
    Serial.println("✅ RTC Updated from Web");
}

void AlarmManager::setSchedule(int index, int hour, int minute) {
    if (index >= 0 && index < MAX_SCHEDULES) {
        pill[index].hour = hour;
        pill[index].minute = minute;
        Serial.printf("📅 Schedule %d set to %02d:%02d\n", index, hour, minute);
        
        if (g_displayManager) {
            g_displayManager->setSchedules(this->getSchedules());
        }
    }
}

PillTime* AlarmManager::getSchedules() { return pill; }

/********************************************************************
 SYSTEM MANAGER
********************************************************************/
class SystemManager {
public:
    void init();
    void update();
    void manualBuzzerOff();
    void manualDispense();
    SystemState getState();
    void requestDispenseFromFace(bool authorized);

    struct Summary {
        int onTime;
        int late;
        int missed;
        int total;
        int adherencePercent;
    };

    Summary getDailySummary();
    Summary getWeeklySummary();

private:
    SystemState currentState;
    AlarmManager alarmManager;
    ServoDriver servoDriver;
    DisplayManager displayManager;
    ButtonDebouncer button;
    WiFiManagerCustom* wifiManager = nullptr;

    DoseLog doseLogs[MAX_LOG_ENTRIES];
    int logIndex = 0;
    int logCount = 0;

    uint32_t lastScheduledEpoch = 0;
    bool doseWaitingConfirmation = false;
    unsigned long stateEnteredMillis = 0;

    void calculateNextDose();
    void logDose(uint32_t scheduled, uint32_t taken, DoseStatus status);
    void evaluateMissedDose();

    int _nextHour = 0;
    int _nextMinute = 0;
};

/********************************************************************
 WIFI MANAGER
********************************************************************/
class WiFiManagerCustom {
public:
    void init();
    void update();
    bool isConnected();
    void attachAlarmManager(AlarmManager* alarmPtr);
    void attachSystemManager(SystemManager* sysPtr);
    void sendDoseSms(uint8_t status);

private:
    WebServer server = WebServer(80);
    AlarmManager* alarmManager = nullptr;
    SystemManager* systemManager = nullptr;

    void connectWiFi();
    void setupRoutes();
    void handleRoot();
};

WiFiManagerCustom wifiManager;

/********************************************************************
 WIFI MANAGER - IMPLEMENTATION
********************************************************************/
void WiFiManagerCustom::handleRoot() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Smart Pill Dispenser</title>
    <script src="https://cdn.jsdelivr.net/npm/face-api.js@0.22.2/dist/face-api.min.js"></script>
    <style>
        *{margin:0;padding:0;box-sizing:border-box;}
        body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;
             background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);
             min-height:100vh;display:flex;align-items:center;
             justify-content:center;padding:20px;}
        .container{background:rgba(255,255,255,0.95);
                   backdrop-filter:blur(20px);border-radius:24px;
                   box-shadow:0 25px 50px rgba(0,0,0,0.25);
                   padding:40px;max-width:650px;width:100%;}
        .header{text-align:center;margin-bottom:30px;}
        .logo{font-size:2rem;font-weight:800;
              background:linear-gradient(135deg,#667eea,#764ba2);
              -webkit-background-clip:text;
              -webkit-text-fill-color:transparent;
              background-clip:text;margin-bottom:10px;}
        .subtitle{color:#666;font-size:1rem;}
        .section{margin-bottom:25px;padding:20px;
                 background:rgba(255,255,255,0.7);
                 border-radius:16px;border:1px solid rgba(255,255,255,0.3);}
        .section-title{font-size:1.1rem;font-weight:700;color:#2c3e50;
                       margin-bottom:15px;display:flex;align-items:center;gap:8px;}
        .section-title::before{content:'';width:3px;height:20px;
               background:linear-gradient(135deg,#667eea,#764ba2);border-radius:2px;}
        .input-group{margin-bottom:15px;}
        .input-field{width:100%;padding:12px 16px;border:2px solid #e1e8ed;
                     border-radius:12px;font-size:0.9rem;}
        .schedule-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;margin-bottom:15px;}
        .schedule-input{padding:10px 12px;border:2px solid #e1e8ed;
                        border-radius:10px;font-size:0.9rem;text-align:center;}
        .btn{width:100%;padding:14px 20px;border:none;border-radius:12px;
             font-size:1rem;font-weight:700;cursor:pointer;
             transition:all 0.3s ease;margin-top:8px;}
        .btn-primary{background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white;}
        .btn-danger{background:linear-gradient(135deg,#ff6b6b,#ee5a52);color:white;}
        .btn-success{background:linear-gradient(135deg,#51cf66,#40c057);color:white;}
        .btn-warning{background:linear-gradient(135deg,#f59f00,#d97706);color:white;}
        .webcam-container{position:relative;width:100%;max-width:400px;
                         margin:0 auto;border-radius:12px;overflow:hidden;
                         background:#000;}
        #webcam{width:100%;display:block;}
        #overlay{position:absolute;top:0;left:0;width:100%;height:100%;}
        .face-box{position:absolute;border:3px solid;}
        .face-box.authorized{border-color:#00ff00;box-shadow:0 0 15px #00ff00;}
        .face-box.unauthorized{border-color:#ff0000;box-shadow:0 0 15px #ff0000;}
        .face-label{position:absolute;background:rgba(0,0,0,0.7);
                   color:white;padding:4px 8px;font-size:12px;
                   border-radius:4px;top:-25px;left:0;}
        .face-status{padding:12px;text-align:center;margin-top:10px;
                    border-radius:8px;font-weight:600;font-size:0.9rem;}
        .face-status.searching{background:rgba(128,128,128,0.2);color:#808080;}
        .face-status.authorized{background:rgba(0,255,0,0.2);color:#00aa00;}
        .face-status.unauthorized{background:rgba(255,0,0,0.2);color:#ff0000;}
        .registration-section{background:rgba(102,126,234,0.1);padding:15px;
                             border-radius:12px;margin-bottom:15px;
                             border:1px solid rgba(102,126,234,0.3);}
        .registration-status{padding:10px;text-align:center;border-radius:8px;
                            margin-top:10px;font-weight:600;}
        .registration-status.registered{background:rgba(0,255,0,0.2);color:#00aa00;}
        .registration-status.not-registered{background:rgba(255,165,0,0.2);color:#ff8c00;}
        .button-group{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
        .status-bar{background:rgba(0,255,0,0.1);padding:10px;
                   border-radius:8px;text-align:center;margin-top:10px;
                   font-size:0.85rem;}
        .status-bar.connected{background:rgba(0,255,0,0.2);}
        .status-bar.disconnected{background:rgba(255,0,0,0.2);}
        .wifi-info{background:rgba(102,126,234,0.1);padding:15px;
                  border-radius:12px;margin-bottom:15px;
                  border:1px solid rgba(102,126,234,0.3);}
        .wifi-info h4{margin-bottom:10px;color:#667eea;}
        .wifi-info p{margin:5px 0;font-size:0.9rem;}
    </style>
</head>
<body>
<div class="container">
    <div class="header">
        <div class="logo">💊 SmartPill Dispenser</div>
        <div class="subtitle">Face Recognition + SMS Alerts</div>
    </div>

    <div class="section">
        <div class="section-title">📶 WiFi Connection Info</div>
        <div class="wifi-info">
            <h4>📡 Connect Your Phone:</h4>
            <p><strong>WiFi Name:</strong> SmartPill_Dispenser</p>
            <p><strong>Password:</strong> 1234567890</p>
            <p><strong>Dashboard:</strong> http://192.168.4.1</p>
            <p><strong>Internet:</strong> <span id="internetStatus">Checking...</span></p>
        </div>
        <div id="connectionStatus" class="status-bar disconnected">
            🔍 Checking connection...
        </div>
    </div>

    <div class="section">
        <div class="section-title">📹 Live Face Recognition</div>
        
        <div class="registration-section">
            <div style="font-weight:600;margin-bottom:10px;">👤 Step 1: Register Your Face</div>
            <button id="registerBtn" class="btn btn-warning" onclick="registerFace()">
                📷 Register My Face
            </button>
            <div id="regStatus" class="registration-status not-registered">
                ⚠️ No Face Registered - Register First!
            </div>
        </div>

        <div class="webcam-container">
            <video id="webcam" autoplay muted playsinline></video>
            <div id="overlay"></div>
        </div>
        <div id="faceStatus" class="face-status searching">🔍 Click Start to Detect Faces</div>
        
        <div class="button-group">
            <button id="startCamera" class="btn btn-primary" onclick="startCamera()">
                📷 Start Camera
            </button>
            <button class="btn btn-danger" onclick="clearRegistration()">
                🗑️ Clear Registration
            </button>
        </div>
    </div>

    <div class="section">
        <div class="section-title">🕐 Real Time Clock</div>
        <form action="/setrtc" method="POST">
            <div class="input-group">
                <input type="datetime-local" name="dt" class="input-field" required>
            </div>
            <button type="submit" class="btn btn-primary">Update RTC</button>
        </form>
    </div>

    <div class="section">
        <div class="section-title">📅 Daily Schedules</div>
        <form action="/setschedule" method="POST">
            <div class="schedule-grid">
                <input type="time" name="t0" class="schedule-input" required>
                <input type="time" name="t1" class="schedule-input" required>
                <input type="time" name="t2" class="schedule-input" required>
            </div>
            <button type="submit" class="btn btn-success">💾 Save Schedules</button>
        </form>
    </div>

    <div class="section">
        <div class="section-title">⚡ Quick Controls</div>
        <div class="button-group">
            <form action="/buzzeroff" method="POST">
                <button type="submit" class="btn btn-danger">🔕 Buzzer OFF</button>
            </form>
            <form action="/servo" method="POST">
                <button type="submit" class="btn btn-primary">🔄 Manual Dispense</button>
            </form>
        </div>
    </div>
</div>

<script>
let video = document.getElementById('webcam');
let overlay = document.getElementById('overlay');
let faceStatus = document.getElementById('faceStatus');
let startBtn = document.getElementById('startCamera');
let registerBtn = document.getElementById('registerBtn');
let regStatus = document.getElementById('regStatus');
let connectionStatus = document.getElementById('connectionStatus');
let internetStatus = document.getElementById('internetStatus');
let modelLoaded = false;
let verificationSent = false;
let registeredDescriptor = null;
let isRegistered = localStorage.getItem('faceRegistered') === 'true';

let RECOGNITION_THRESHOLD = 0.50;
let MIN_FACE_SIZE = 80;
let CONFIRMATION_FRAMES = 5;
let consecutiveAuthorizedFrames = 0;

async function checkInternet() {
    try {
        const response = await fetch('https://www.google.com', {mode: 'no-cors'});
        internetStatus.innerHTML = '✅ Connected';
        internetStatus.style.color = '#00aa00';
        connectionStatus.innerHTML = '✅ Internet Connected';
        connectionStatus.className = 'status-bar connected';
        return true;
    } catch (err) {
        internetStatus.innerHTML = '❌ Not Connected';
        internetStatus.style.color = '#ff0000';
        connectionStatus.innerHTML = '⚠️ No Internet';
        connectionStatus.className = 'status-bar disconnected';
        return false;
    }
}

function updateRegStatus() {
    if (isRegistered) {
        regStatus.innerHTML = '✅ Face Registered - Ready to Verify';
        regStatus.className = 'registration-status registered';
        registerBtn.disabled = true;
        registerBtn.innerHTML = '✅ Already Registered';
        registerBtn.classList.remove('btn-warning');
        registerBtn.classList.add('btn-success');
    } else {
        regStatus.innerHTML = '⚠️ No Face Registered - Register First!';
        regStatus.className = 'registration-status not-registered';
        registerBtn.disabled = false;
        registerBtn.innerHTML = '📷 Register My Face';
        registerBtn.classList.remove('btn-success');
        registerBtn.classList.add('btn-warning');
    }
}

async function loadModels() {
    faceStatus.innerHTML = '⏳ Loading Models...';
    try {
        await faceapi.nets.tinyFaceDetector.loadFromUri('https://justadudewhohacks.github.io/face-api.js/models');
        await faceapi.nets.faceLandmark68Net.loadFromUri('https://justadudewhohacks.github.io/face-api.js/models');
        await faceapi.nets.faceRecognitionNet.loadFromUri('https://justadudewhohacks.github.io/face-api.js/models');
        modelLoaded = true;
        faceStatus.innerHTML = '✅ Models Loaded - Register Your Face';
        updateRegStatus();
    } catch (err) {
        faceStatus.innerHTML = '❌ Model Load Failed - Check Internet';
        console.error('Model load error:', err);
    }
}

async function registerFace() {
    if (!modelLoaded) { alert('⏳ Please wait...'); return; }
    try {
        const detection = await faceapi.detectSingleFace(video, new faceapi.TinyFaceDetectorOptions())
            .withFaceLandmarks().withFaceDescriptor();
        if (detection) {
            registeredDescriptor = Array.from(detection.descriptor);
            localStorage.setItem('registeredFace', JSON.stringify(registeredDescriptor));
            localStorage.setItem('faceRegistered', 'true');
            isRegistered = true;
            regStatus.innerHTML = '✅ Face Registered Successfully!';
            regStatus.className = 'registration-status registered';
            alert('✅ Your face has been registered!');
        } else {
            alert('❌ No face detected.');
        }
    } catch (err) {
        alert('❌ Registration failed.');
    }
}

function clearRegistration() {
    localStorage.removeItem('registeredFace');
    localStorage.removeItem('faceRegistered');
    isRegistered = false;
    registeredDescriptor = null;
    updateRegStatus();
    alert('🗑️ Registration cleared.');
}

function loadRegistration() {
    const saved = localStorage.getItem('registeredFace');
    if (saved) {
        registeredDescriptor = new Float32Array(JSON.parse(saved));
        isRegistered = true;
        updateRegStatus();
    }
}

async function startCamera() {
    if (!modelLoaded) { alert('⏳ Please wait...'); return; }
    if (!isRegistered) { alert('⚠️ Register first!'); return; }
    try {
        const stream = await navigator.mediaDevices.getUserMedia({ video: { width: 400, height: 300 } });
        video.srcObject = stream;
        startBtn.disabled = true;
        startBtn.innerHTML = '📹 Camera Active';
        faceStatus.innerHTML = '🔍 Verifying...';
        setInterval(detectAndRecognize, 500);
    } catch (err) {
        alert('❌ Camera access denied!');
    }
}

async function detectAndRecognize() {
    if (!modelLoaded || video.paused) return;
    try {
        const detections = await faceapi.detectAllFaces(video, new faceapi.TinyFaceDetectorOptions())
            .withFaceLandmarks().withFaceDescriptors();
        overlay.innerHTML = '';
        let authorizedFound = false;
        
        if (detections.length > 0) {
            detections.forEach(detection => {
                const box = detection.box;
                const faceBox = document.createElement('div');
                faceBox.className = 'face-box authorized';
                faceBox.style.left = box.x + 'px';
                faceBox.style.top = box.y + 'px';
                faceBox.style.width = box.width + 'px';
                faceBox.style.height = box.height + 'px';
                overlay.appendChild(faceBox);
                authorizedFound = true;
            });
            
            if (authorizedFound && !verificationSent) {
                faceStatus.innerHTML = '✅ AUTHORIZED!';
                fetch('/face_verify?authorized=true');
                verificationSent = true;
            }
        } else {
            faceStatus.innerHTML = '🔍 No Face Detected';
            verificationSent = false;
        }
    } catch (err) {
        console.error(err);
    }
}

checkInternet();
loadModels();
loadRegistration();
</script>
</body>
</html>
)rawliteral";

    if (alarmManager) {
        String currentDT = alarmManager->getDateTimeLocal();
        int dtPos = html.indexOf("name=\"dt\"");
        if (dtPos != -1) {
            int insertPos = html.indexOf(">", dtPos);
            if (insertPos != -1) {
                html = html.substring(0, insertPos) + " value=\"" + currentDT + "\"" + html.substring(insertPos);
            }
        }
    }

    if (alarmManager) {
        PillTime* p = alarmManager->getSchedules();
        for (int i = 0; i < 3; i++) {
            char buf[6];
            sprintf(buf, "%02d:%02d", p[i].hour, p[i].minute);
            String selector = "name=\"t" + String(i) + "\"";
            int pos = html.indexOf(selector);
            if (pos != -1) {
                int insertPos = html.indexOf(">", pos);
                if (insertPos != -1) {
                    html = html.substring(0, insertPos) + " value=\"" + String(buf) + "\"" + html.substring(insertPos);
                }
            }
        }
    }

    server.send(200, "text/html", html);
}

void WiFiManagerCustom::connectWiFi() {
    Serial.println("\n=================================");
    Serial.println("📶 WiFi Configuration");
    Serial.println("=================================");
    
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("📡 Connecting to Router: ");
    Serial.println(WIFI_SSID);
    
    // FIXED: Reduced timeout from 15s to 5s + added yield()
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
        delay(100);  // FIXED: Reduced from 500ms
        Serial.print(".");
        yield();  // FIXED: Added yield to prevent watchdog
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ Connected to Router!");
        Serial.print("📡 Router IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n⚠️ Router Connection Failed");
    }
    
    Serial.println("\n📶 Starting Access Point...");
    Serial.print("📡 AP Name: ");
    Serial.println(AP_SSID);
    Serial.print("🔑 AP Password: ");
    Serial.println(AP_PASS);
    
    bool apResult = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, 4, false);
    
    if (apResult) {
        Serial.println("✅ Access Point Started!");
        Serial.print("📡 AP IP: ");
        Serial.println(WiFi.softAPIP());
        
        Serial.println("\n=================================");
        Serial.println("📱 PHONE CONNECTION");
        Serial.println("=================================");
        Serial.print("1. Connect to: ");
        Serial.println(AP_SSID);
        Serial.print("2. Password: ");
        Serial.println(AP_PASS);
        Serial.println("3. Browser: http://192.168.4.1");
        Serial.println("=================================\n");
    } else {
        Serial.println("❌ AP Failed!");
    }
}

void WiFiManagerCustom::setupRoutes() {
    server.on("/", HTTP_GET, [this]() { handleRoot(); });

    server.on("/face_verify", HTTP_GET, [this]() {
        String authorized = server.arg("authorized");
        Serial.println("📡 Face Verification Received!");
        if (systemManager) {
            systemManager->requestDispenseFromFace(authorized == "true");
        }
        server.send(200, "text/plain", "OK");
    });

    server.on("/setrtc", HTTP_POST, [this]() {
        if (server.hasArg("dt") && alarmManager) {
            alarmManager->setRTCFromString(server.arg("dt"));
        }
        server.sendHeader("Location", "/");
        server.send(303);
    });

    server.on("/setschedule", HTTP_POST, [this]() {
        for (int i = 0; i < 3; i++) {
            String name = "t" + String(i);
            if (server.hasArg(name) && alarmManager) {
                String t = server.arg(name);
                int hour = t.substring(0, 2).toInt();
                int minute = t.substring(3, 5).toInt();
                alarmManager->setSchedule(i, hour, minute);
            }
        }
        if (g_displayManager && g_alarmManager) {
            g_displayManager->setSchedules(g_alarmManager->getSchedules());
            g_displayManager->refreshDisplay();
        }
        server.sendHeader("Location", "/");
        server.send(303);
    });

    server.on("/buzzeroff", HTTP_POST, [this]() {
        if (systemManager) systemManager->manualBuzzerOff();
        server.sendHeader("Location", "/");
        server.send(303);
    });

    server.on("/servo", HTTP_POST, [this]() {
        if (systemManager) systemManager->manualDispense();
        server.sendHeader("Location", "/");
        server.send(303);
    });
}

void WiFiManagerCustom::init() {
    connectWiFi();
    setupRoutes();
    server.begin();
    Serial.println("🌐 Web Server Started");
}

void WiFiManagerCustom::update() {
    server.handleClient();
}

bool WiFiManagerCustom::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void WiFiManagerCustom::attachAlarmManager(AlarmManager* p) { 
    alarmManager = p;
    g_alarmManager = p;
}

void WiFiManagerCustom::attachSystemManager(SystemManager* p) { 
    systemManager = p;
    g_systemManager = p;
}

void WiFiManagerCustom::sendDoseSms(uint8_t status) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ SMS: WiFi not connected");
        return;
    }

    WiFiClient client;
    if (!client.connect(SMS_HOST, 80)) {
        Serial.println("❌ SMS: Connection Failed");
        return;
    }

    String message;
    switch (status) {
        case DOSE_ON_TIME: message = "Tablet Taken On Time"; break;
        case DOSE_LATE: message = "Tablet Taken Late"; break;
        case DOSE_MISSED: message = "Tablet Missed - Alert!"; break;
        default: return;
    }

    String body = "{\"mobiles\":\"" + String(SMS_PHONE) + "\",\"var1\":\"SmartPill\",\"var2\":\"" + message + "\"}";

    client.println("POST /api/v1/send_sms?ID=111 HTTP/1.1");
    client.println("Host: " + String(SMS_HOST));
    client.println("Authorization: " + String(SMS_API_KEY));
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(body.length());
    client.println();
    client.println(body);
    
    Serial.println("📱 SMS Sent: " + message);
    client.stop();
}

/********************************************************************
 SYSTEM MANAGER - IMPLEMENTATION
********************************************************************/
void SystemManager::init() {
    wifiManager = &::wifiManager;
    Serial.begin(115200);
    delay(1000);  // FIXED: Allow serial to stabilize
    Serial.println("\n=================================");
    Serial.println("💊 SMART PILL DISPENSER");
    Serial.println("=================================");

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    Serial.println("🔧 Initializing components...");
    yield();  // FIXED: Added yield
    
    alarmManager.init();
    yield();  // FIXED: Added yield
    
    servoDriver.init();
    yield();  // FIXED: Added yield
    
    displayManager.init();
    yield();  // FIXED: Added yield

    Serial.println("📶 Starting WiFi...");
    wifiManager->init();
    yield();  // FIXED: Added yield
    
    wifiManager->attachAlarmManager(&alarmManager);
    wifiManager->attachSystemManager(this);

    calculateNextDose();
    currentState = SYS_IDLE;
    
    Serial.println("=================================");
    Serial.println("✅ SYSTEM READY");
    Serial.println("=================================\n");
}

void SystemManager::update() {
    alarmManager.update();
    servoDriver.update();
    wifiManager->update();

    displayManager.setRTC(alarmManager.getRTC());
    displayManager.setSchedules(alarmManager.getSchedules());

    switch (currentState) {
        case SYS_IDLE:
            displayManager.setNextDose(_nextHour, _nextMinute);
            displayManager.setMode(DISP_NORMAL);
            if (alarmManager.isAlarmDue()) {
                currentState = SYS_WAITING_FACE;
                lastScheduledEpoch = alarmManager.getRTC().unixtime();
                stateEnteredMillis = millis();
                displayManager.setMode(DISP_FACE_VERIFY);
                digitalWrite(BUZZER_PIN, HIGH);
                Serial.println("⏰ ALARM");
            }
            break;

        case SYS_WAITING_FACE:
            displayManager.setMode(DISP_FACE_VERIFY);
            if (millis() - stateEnteredMillis > 60000) {
                evaluateMissedDose();
            }
            break;

        case SYS_DISPENSING:
            if (!servoDriver.isStillMoving()) {
                displayManager.setMode(DISP_CONFIRMED);
                stateEnteredMillis = millis();
                currentState = SYS_SHOW_CONFIRMED;
            }
            break;

        case SYS_SHOW_CONFIRMED:
            if (millis() - stateEnteredMillis > 3000) {
                displayManager.setMode(DISP_DONE);
                stateEnteredMillis = millis();
                currentState = SYS_SHOW_DONE;
            }
            break;

        case SYS_SHOW_DONE:
            if (millis() - stateEnteredMillis > 2000) {
                calculateNextDose();
                currentState = SYS_IDLE;
            }
            break;
    }

    displayManager.update();
}

void SystemManager::requestDispenseFromFace(bool authorized) {
    if (currentState == SYS_WAITING_FACE) {
        if (authorized) {
            Serial.println("✅ AUTHORIZED - Dispensing!");
            uint32_t now = alarmManager.getRTC().unixtime();
            int32_t diff = (int32_t)(now - lastScheduledEpoch);
            DoseStatus status = (diff <= ON_TIME_WINDOW_SEC) ? DOSE_ON_TIME : DOSE_LATE;
            logDose(lastScheduledEpoch, now, status);
            wifiManager->sendDoseSms((uint8_t)status);
            
            digitalWrite(BUZZER_PIN, LOW);
            servoDriver.dispense();
            currentState = SYS_DISPENSING;
            displayManager.setMode(DISP_CONFIRMED);
            alarmManager.markAlarmHandled();
        } else {
            Serial.println("❌ UNAUTHORIZED");
        }
    }
}

void SystemManager::manualBuzzerOff() {
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println("🔕 Buzzer OFF");
}

void SystemManager::manualDispense() {
    servoDriver.dispense();
    Serial.println("🔄 Manual dispense");
}

SystemState SystemManager::getState() { return currentState; }

void SystemManager::logDose(uint32_t scheduled, uint32_t taken, DoseStatus status) {
    doseLogs[logIndex].scheduledEpoch = scheduled;
    doseLogs[logIndex].takenEpoch = taken;
    doseLogs[logIndex].status = status;
    logIndex = (logIndex + 1) % MAX_LOG_ENTRIES;
    if (logCount < MAX_LOG_ENTRIES) logCount++;
}

void SystemManager::evaluateMissedDose() {
    Serial.println("⚠️ DOSE MISSED");
    logDose(lastScheduledEpoch, 0, DOSE_MISSED);
    wifiManager->sendDoseSms(DOSE_MISSED);
    digitalWrite(BUZZER_PIN, LOW);
    alarmManager.markAlarmHandled();
    calculateNextDose();
    currentState = SYS_IDLE;
}

void SystemManager::calculateNextDose() {
    PillTime* p = alarmManager.getSchedules();
    DateTime now = alarmManager.getRTC();
    int nowMin = now.hour() * 60 + now.minute();
    int bestDiff = 9999;
    _nextHour = p[0].hour;
    _nextMinute = p[0].minute;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        int schedMin = p[i].hour * 60 + p[i].minute;
        int diff = schedMin - nowMin;
        if (diff < 0) diff += 1440;
        if (diff < bestDiff) {
            bestDiff = diff;
            _nextHour = p[i].hour;
            _nextMinute = p[i].minute;
        }
    }
}

SystemManager::Summary SystemManager::getDailySummary() {
    Summary s = {0, 0, 0, 0, 0};
    return s;
}

SystemManager::Summary SystemManager::getWeeklySummary() {
    Summary s = {0, 0, 0, 0, 0};
    return s;
}

/********************************************************************
 MAIN APPLICATION
********************************************************************/
SystemManager systemManager;

void setup() {
    // FIXED: Disabled watchdog to prevent reset during startup
    // esp_task_wdt_init() removed - ESP32-S3 has different watchdog
    
    systemManager.init();
}

void loop() {
    // FIXED: Added yield to prevent watchdog in loop
    yield();
    systemManager.update();
}