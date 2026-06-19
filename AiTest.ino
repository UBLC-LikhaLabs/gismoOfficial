// Sample working program with interrupt, esp with servo


#include <driver/i2s.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>


// ==================== CONFIGURATION ====================
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>


// SH1106 OLED Display Settings (1.3 inch)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define I2C_SDA 41
#define I2C_SCL 42


Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);


// I2S pins for Microphone
#define I2S_SD 6
#define I2S_WS 4
#define I2S_SCK 5
#define I2S_PORT_MIC I2S_NUM_0


// Audio Output Pins for MAX98357A
#define I2S_DOUT 7
#define I2S_BCLK 15
#define I2S_LRC 16
#define I2S_PORT_SPEAKER I2S_NUM_1


#define BUTTON_PIN 3
#define bufferCnt 10
#define bufferLen 1024


// ==================== SERVO 1 CONFIGURATION (Waggle/AI Speaking) ====================
#define SERVO1_PIN 2          // Servo 1 control pin
#define SERVO1_NEUTRAL 1500   // 1.5ms pulse - neutral position (harap)
#define SERVO1_MIN 600        // 1.0ms pulse - minimum angle (kaliwa)
#define SERVO1_MAX 2400       // 2.0ms pulse - maximum angle (kanan)
#define SERVO1_SPEED 30       // Speed ng movement (mas malaki = mas mabilis)


// ==================== SERVO 2 CONFIGURATION (Oscillating - Left/Right) ====================
#define SERVO2_PIN 1          // Servo 2 control pin
#define SERVO2_NEUTRAL 1500   // 1.5ms pulse - neutral position
#define SERVO2_MIN 1000       // 1.0ms pulse - minimum angle
#define SERVO2_MAX 2000       // 2.0ms pulse - maximum angle
#define SERVO2_SPEED 20       // Speed ng oscillation


// Oscillation settings para sa Servo 2
#define OSCILLATE2_MIN_ANGLE 30    // Minimum angle sa degrees (0-180)
#define OSCILLATE2_MAX_ANGLE 150   // Maximum angle sa degrees (0-180)
#define OSCILLATE2_SPEED 1         // Speed ng oscillation (1 = mabagal, 5 = mabilis)
#define OSCILLATE2_UPDATE_MS 30    // Update interval sa milliseconds


// ==================== SERVO 3 CONFIGURATION (Base - Oscillating but stops when AI speaks) ====================
#define SERVO3_PIN 10          // Servo 3 control pin (base)
#define SERVO3_NEUTRAL 1500   // 1.5ms pulse - neutral position
#define SERVO3_MIN 600        // 1.0ms pulse - minimum angle
#define SERVO3_MAX 2400       // 2.0ms pulse - maximum angle
#define SERVO3_SPEED 10       // Speed ng oscillation


// Oscillation settings para sa Servo 3 (base)
#define OSCILLATE3_MIN_ANGLE 45     // Minimum angle sa degrees (0-180)
#define OSCILLATE3_MAX_ANGLE 135    // Maximum angle sa degrees (0-180)
#define OSCILLATE3_SPEED 1          // Speed ng oscillation (1 = mabagal, 5 = mabilis)
#define OSCILLATE3_UPDATE_MS 35     // Update interval sa milliseconds


// ==================== WAGGLE CONFIGURATION ====================
#define WAGGLE_RANGE 450     // PALITAN ITO PARA PALAKIHIN O PALIITIN ANG GALAW
#define WAGGLE_SPEED 40      // Bilis ng paggalaw (mas malaki = mas mabilis)


// Servo states
// Servo 1 (Waggle/AI Speaking)
int servo1CurrentPulse = SERVO1_NEUTRAL;
int servo1TargetPulse = SERVO1_NEUTRAL;
bool servo1Moving = true;
unsigned long lastServo1PulseTime = 0;


// Servo 2 (Oscillating - continuous)
int servo2CurrentPulse = SERVO2_NEUTRAL;
int servo2TargetPulse = SERVO2_NEUTRAL;
bool servo2Moving = true;
unsigned long lastServo2PulseTime = 0;


// Servo 3 (Base - oscillates but stops when AI speaks)
int servo3CurrentPulse = SERVO3_NEUTRAL;
int servo3TargetPulse = SERVO3_NEUTRAL;
bool servo3Moving = true;
unsigned long lastServo3PulseTime = 0;


bool servosEnabled = true;  // Enable/disable lahat ng servo
volatile bool isAISpeaking = false;  // Track kung nagsasalita ang AI


// ==================== AUDIO BUFFER ====================
int16_t sBuffer[bufferLen];
#define AUDIO_BUFFER_SIZE 524288  // 1MB buffer (524288 samples * 2 bytes = 1,048,576 bytes)
int16_t* audioBuffer = NULL;
volatile int audioWriteIndex = 0;
volatile int audioReadIndex = 0;
volatile int audioSamplesAvailable = 0;
int actualAudioBufferSize = AUDIO_BUFFER_SIZE;
portMUX_TYPE audioBufferMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t playTaskHandle = NULL;


// ==================== CREDENTIALS ====================
String ssid = "";
String password = "";
String websocket_server_host = "";
uint16_t websocket_server_port = 8888;


using namespace websockets;
WebsocketsClient client;
volatile bool isWebSocketConnected = false;
volatile bool isStreamingAudio = false;
volatile bool isPlayingAudio = false;
volatile bool isAudioFinishedSending = false;
volatile bool triggerOLEDUpdateAfterAudio = false;
bool buttonPressed = false;
bool lastButtonState = false;


// ==================== CONFIGURATION ====================
Preferences preferences;
WebServer server(80);
DNSServer dns;
const char* apSSID = "ESP32-AI-Config";
bool inConfigMode = false;
bool configModeTriggered = false;


// ===============================================
// EYE ANIMATION SYSTEM
// ===============================================
struct EyeState {
  int height;
  int width;
  int x;
  int y;
};


enum AnimationState {
  ANIM_IDLE,
  ANIM_WAKEUP,
  ANIM_RESET,
  ANIM_MOVE_RIGHT_BIG,
  ANIM_MOVE_LEFT_BIG,
  ANIM_BLINK_LONG,
  ANIM_BLINK_SHORT,
  ANIM_HAPPY,
  ANIM_SLEEP,
  ANIM_SACCADE_RANDOM,
  ANIM_MOVE_BIG,
  ANIM_SACCADE
};


enum AnimationPhase {
  PHASE_START,
  PHASE_OUTWARD_1,
  PHASE_OUTWARD_2,
  PHASE_HOLD,
  PHASE_INWARD_1,
  PHASE_INWARD_2,
  PHASE_COMPLETE,
  PHASE_BLINK_DOWN,
  PHASE_BLINK_UP,
  PHASE_HAPPY_DRAW,
  PHASE_HAPPY_WAIT,
  PHASE_SACCADE_MOVE_OUT,
  PHASE_SACCADE_MOVE_BACK
};


struct AnimationStateMachine {
  AnimationState currentAnimation = ANIM_IDLE;
  AnimationPhase currentPhase = PHASE_START;
  int frameCounter = 0;
  int loopCounter = 0;
  int directionX = 1;
  int directionY = 1;
  int happyOffset = 0;
  unsigned long lastFrameTime = 0;
  unsigned long phaseStartTime = 0;
  bool animationActive = false;
  int startCornerRadius;
};


AnimationStateMachine animState;
unsigned long lastAnimationCycleTime = 0;
const unsigned long ANIMATION_CYCLE_INTERVAL = 4000;
const unsigned long FRAME_INTERVAL = 33;


enum Animation {
  WAKEUP,
  RESET,
  MOVE_RIGHT_BIG,
  MOVE_LEFT_BIG,
  BLINK_LONG,
  BLINK_SHORT,
  HAPPY,
  SLEEP,
  SACCADE_RANDOM,
  MAX_ANIMATIONS
};


int demo_mode = 1;
int current_animation_index = 0;


const int REF_EYE_HEIGHT = 40;
const int REF_EYE_WIDTH = 40;
const int REF_SPACE_BETWEEN_EYE = 10;
const int REF_CORNER_RADIUS = 10;


EyeState left_eye, right_eye;
int corner_radius = REF_CORNER_RADIUS;


// ===============================================
// TEXT DISPLAY SYSTEM
// ===============================================
String fullResponse = "";
String displayPages[15];
int currentPage = 0;
int totalPages = 0;
unsigned long lastPageChange = 0;
const int PAGE_CHANGE_DELAY = 5000;
bool isMultiPage = false;
String userQuestion = "";
unsigned long displayTimeout = 0;
const int DISPLAY_TIMEOUT = 80000;


bool shouldIgnoreOldResponse = false;


bool displayShowingListening = false;
bool displayShowingProcessing = false;
bool displayShowingTranscribed = false;
bool displayShowingAI = false;


static unsigned long lastScrollTime = 0;
static int scrollOffset = 0;
static bool isScrollingActive = false;
static String currentScrollingText = "";
static unsigned int scrollStartTime = 0;
static const int SCROLL_DELAY = 2000;


bool showingEyeAnimation = true;
bool showingTextDisplay = false;


unsigned long connectionMessageTime = 0;
const int CONNECTION_MESSAGE_DURATION = 2000;


// ==================== AUDIO TASK HANDLE ====================
TaskHandle_t audioTaskHandle = NULL;


// ==================== FUNCTION PROTOTYPES ====================
void audioStreamTask(void* parameter);
void audioPlayTask(void* parameter);
void loadSettings();
void saveSettings(String newSSID, String newPass, String newHost, uint16_t newPort);
bool areSettingsValid();
void clearSettings();
String getConfigHTML();
void startConfigPortal();
bool connectWiFiWithRetry(int maxRetries = 3);
void connectWSServer();
void startRecording();
void stopRecording();
void handleTextMessage(String message);
void handleAudioData(const char* data, size_t length);
void setupI2SMicrophone();
void setupI2SSpeaker();
void playTestBeep();
void testPSRAM();
void displayMessage(String line1, String line2 = "", String line3 = "", String line4 = "");
void splitTextToPages(String text);
void displayCurrentPage();
void nextPage();
void resetDisplay();
void updateTranscribedDisplay();


// ==================== EYE FUNCTIONS ====================
int calculate_safe_radius(int r, int w, int h);
void draw_eyes();
void draw_eye_frame();
void reset_eyes(bool update=true);
void sleep_eyes();
bool shouldStopAnimation();
void startAnimation(AnimationState anim);
void updateWakeupAnimation();
void updateBlinkAnimation();
void updateSleepAnimation();
void updateHappyAnimation();
void updateSaccadeAnimation();
void updateMoveBigAnimation();
void updateAnimation();
void launchNextAnimation();


// ==================== SERVO FUNCTIONS ====================
void setServo1Pulse(int pulseWidth);
void setServo2Pulse(int pulseWidth);
void setServo3Pulse(int pulseWidth);
void updateServo1Position();
void updateServo2Position();
void updateServo3Position();
void moveServo1(int angle);
void moveServo2(int angle);
void moveServo3(int angle);
void waggleServo1();
void oscillateServo2();
void oscillateServo3();
void enableServos(bool enable);


// ==================== PSRAM FUNCTIONS ====================
void* audio_malloc(size_t size) {
    void *ptr = NULL;
    if (psramFound()) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (ptr) {
            Serial.printf("✅ PSRAM allocated: %d bytes\n", size);
        } else {
            Serial.printf("⚠️ PSRAM allocation failed (%d bytes), falling back to internal RAM\n", size);
            ptr = malloc(size);
        }
    } else {
        ptr = malloc(size);
    }
   
    if (!ptr) {
        Serial.printf("❌ Failed to allocate %d bytes\n", size);
    }
    return ptr;
}


void audio_free(void* ptr) {
    if (ptr) {
        free(ptr);
    }
}


void initAudioBuffer() {
  if (audioBuffer == NULL) {
    audioBuffer = (int16_t*)audio_malloc(AUDIO_BUFFER_SIZE * sizeof(int16_t));
   
    if (audioBuffer == NULL) {
      Serial.println("❌ Failed to allocate audio buffer memory");
      audioBuffer = sBuffer;
      actualAudioBufferSize = bufferLen;
      Serial.println("⚠️ Using temporary buffer as fallback");
    } else {
      memset(audioBuffer, 0, AUDIO_BUFFER_SIZE * sizeof(int16_t));
      actualAudioBufferSize = AUDIO_BUFFER_SIZE;
      audioWriteIndex = 0;
      audioReadIndex = 0;
      audioSamplesAvailable = 0;
      Serial.printf("✅ Audio buffer allocated: %d samples in %s\n",
                   AUDIO_BUFFER_SIZE,
                   psramFound() ? "PSRAM" : "Internal RAM");
    }
  }
}


bool writeToAudioBuffer(const int16_t* data, int length) {
  if (!audioBuffer || !data || actualAudioBufferSize == 0) {
    return false;
  }
 
  portENTER_CRITICAL(&audioBufferMux);
  if (audioSamplesAvailable + length > actualAudioBufferSize) {
    int samplesToClear = (audioSamplesAvailable + length) - actualAudioBufferSize;
    audioReadIndex = (audioReadIndex + samplesToClear) % actualAudioBufferSize;
    audioSamplesAvailable -= samplesToClear;
  }
 
  int idx = audioWriteIndex;
  for (int i = 0; i < length; i++) {
    audioBuffer[idx] = data[i];
    idx = (idx + 1) % actualAudioBufferSize;
  }
  audioWriteIndex = idx;
  audioSamplesAvailable += length;
  portEXIT_CRITICAL(&audioBufferMux);
  return true;
}


void clearAudioBuffer() {
  if (!audioBuffer) return;
  portENTER_CRITICAL(&audioBufferMux);
  audioWriteIndex = 0;
  audioReadIndex = 0;
  audioSamplesAvailable = 0;
  portEXIT_CRITICAL(&audioBufferMux);
  Serial.println("🧹 Audio buffer indices reset");
}


int audioBufferAvailable() {
  int available;
  portENTER_CRITICAL(&audioBufferMux);
  available = audioSamplesAvailable;
  portEXIT_CRITICAL(&audioBufferMux);
  return available;
}


// ===============================================
// SERVO 1 FUNCTIONS (Waggle/AI Speaking)
// ===============================================
void setServo1Pulse(int pulseWidth) {
  pulseWidth = constrain(pulseWidth, SERVO1_MIN, SERVO1_MAX);
  digitalWrite(SERVO1_PIN, HIGH);
  delayMicroseconds(pulseWidth);
  digitalWrite(SERVO1_PIN, LOW);
}


void updateServo1Position() {
    unsigned long currentTime = millis();
   
    if (currentTime - lastServo1PulseTime >= 20) {
        lastServo1PulseTime = currentTime;
       
        if (servo1Moving && servosEnabled) {
            if (servo1CurrentPulse < servo1TargetPulse) {
                servo1CurrentPulse += SERVO1_SPEED;
                if (servo1CurrentPulse > servo1TargetPulse) {
                    servo1CurrentPulse = servo1TargetPulse;
                }
            } else if (servo1CurrentPulse > servo1TargetPulse) {
                servo1CurrentPulse -= SERVO1_SPEED;
                if (servo1CurrentPulse < servo1TargetPulse) {
                    servo1CurrentPulse = servo1TargetPulse;
                }
            }
        }
       
        setServo1Pulse(servo1CurrentPulse);
    }
}


void moveServo1(int angle) {
  int targetPulse = map(angle, 0, 180, SERVO1_MIN, SERVO1_MAX);
  servo1TargetPulse = constrain(targetPulse, SERVO1_MIN, SERVO1_MAX);
  servo1Moving = true;
}


void waggleServo1() {
    static int wigglePosition = 0;
    static int wiggleDirection = 1;
    static unsigned long lastWiggleUpdate = 0;
   
    if (millis() - lastWiggleUpdate > 20) {
       
        wigglePosition += wiggleDirection * WAGGLE_SPEED;
       
        if (wigglePosition >= WAGGLE_RANGE) {
            wigglePosition = WAGGLE_RANGE;
            wiggleDirection = -1;
        } else if (wigglePosition <= -WAGGLE_RANGE) {
            wigglePosition = -WAGGLE_RANGE;
            wiggleDirection = 1;
        }
       
        servo1TargetPulse = SERVO1_NEUTRAL + wigglePosition;
       
        lastWiggleUpdate = millis();
    }
}


// ===============================================
// SERVO 2 FUNCTIONS (Oscillating - continuous)
// ===============================================
void setServo2Pulse(int pulseWidth) {
  pulseWidth = constrain(pulseWidth, SERVO2_MIN, SERVO2_MAX);
  digitalWrite(SERVO2_PIN, HIGH);
  delayMicroseconds(pulseWidth);
  digitalWrite(SERVO2_PIN, LOW);
}


void updateServo2Position() {
    unsigned long currentTime = millis();
   
    if (currentTime - lastServo2PulseTime >= 20) {
        lastServo2PulseTime = currentTime;
       
        if (servo2Moving && servosEnabled) {
            if (servo2CurrentPulse < servo2TargetPulse) {
                servo2CurrentPulse += SERVO2_SPEED;
                if (servo2CurrentPulse > servo2TargetPulse) {
                    servo2CurrentPulse = servo2TargetPulse;
                }
            } else if (servo2CurrentPulse > servo2TargetPulse) {
                servo2CurrentPulse -= SERVO2_SPEED;
                if (servo2CurrentPulse < servo2TargetPulse) {
                    servo2CurrentPulse = servo2TargetPulse;
                }
            }
        }
       
        setServo2Pulse(servo2CurrentPulse);
    }
}


void moveServo2(int angle) {
  int targetPulse = map(angle, 0, 180, SERVO2_MIN, SERVO2_MAX);
  servo2TargetPulse = constrain(targetPulse, SERVO2_MIN, SERVO2_MAX);
  servo2Moving = true;
}


void oscillateServo2() {
    static int currentAngle = OSCILLATE2_MIN_ANGLE;
    static int direction = 1;
    static unsigned long lastOscillateUpdate = 0;
   
    if (millis() - lastOscillateUpdate > OSCILLATE2_UPDATE_MS) {
       
        currentAngle += direction * OSCILLATE2_SPEED;
       
        if (currentAngle >= OSCILLATE2_MAX_ANGLE) {
            currentAngle = OSCILLATE2_MAX_ANGLE;
            direction = -1;
        } else if (currentAngle <= OSCILLATE2_MIN_ANGLE) {
            currentAngle = OSCILLATE2_MIN_ANGLE;
            direction = 1;
        }
       
        moveServo2(currentAngle);
       
        lastOscillateUpdate = millis();
    }
}


// ===============================================
// SERVO 3 FUNCTIONS (Base - oscillates but stops when AI speaks)
// ===============================================
void setServo3Pulse(int pulseWidth) {
  pulseWidth = constrain(pulseWidth, SERVO3_MIN, SERVO3_MAX);
  digitalWrite(SERVO3_PIN, HIGH);
  delayMicroseconds(pulseWidth);
  digitalWrite(SERVO3_PIN, LOW);
}


void updateServo3Position() {
    unsigned long currentTime = millis();
   
    if (currentTime - lastServo3PulseTime >= 20) {
        lastServo3PulseTime = currentTime;
       
        if (servo3Moving && servosEnabled) {
            if (servo3CurrentPulse < servo3TargetPulse) {
                servo3CurrentPulse += SERVO3_SPEED;
                if (servo3CurrentPulse > servo3TargetPulse) {
                    servo3CurrentPulse = servo3TargetPulse;
                }
            } else if (servo3CurrentPulse > servo3TargetPulse) {
                servo3CurrentPulse -= SERVO3_SPEED;
                if (servo3CurrentPulse < servo3TargetPulse) {
                    servo3CurrentPulse = servo3TargetPulse;
                }
            }
        }
       
        setServo3Pulse(servo3CurrentPulse);
    }
}


void moveServo3(int angle) {
  int targetPulse = map(angle, 0, 180, SERVO3_MIN, SERVO3_MAX);
  servo3TargetPulse = constrain(targetPulse, SERVO3_MIN, SERVO3_MAX);
  servo3Moving = true;
}


void oscillateServo3() {
    static int currentAngle = OSCILLATE3_MIN_ANGLE;
    static int direction = 1;
    static unsigned long lastOscillateUpdate = 0;
   
    if (millis() - lastOscillateUpdate > OSCILLATE3_UPDATE_MS) {
       
        currentAngle += direction * OSCILLATE3_SPEED;
       
        if (currentAngle >= OSCILLATE3_MAX_ANGLE) {
            currentAngle = OSCILLATE3_MAX_ANGLE;
            direction = -1;
        } else if (currentAngle <= OSCILLATE3_MIN_ANGLE) {
            currentAngle = OSCILLATE3_MIN_ANGLE;
            direction = 1;
        }
       
        moveServo3(currentAngle);
       
        lastOscillateUpdate = millis();
    }
}


// ===============================================
// GLOBAL SERVO CONTROL
// ===============================================
void enableServos(bool enable) {
    servosEnabled = enable;
    if (!enable) {
        // I-neutral ang lahat ng servo
        moveServo1(90);  // 90 degrees = neutral/harap
        moveServo2(90);  // 90 degrees = neutral/harap
        moveServo3(90);  // 90 degrees = neutral/harap
    }
}


// ===============================================
// CONFIGURATION FUNCTIONS
// ===============================================
void loadSettings() {
  preferences.begin("wifi-config", false);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("pass", "");
  websocket_server_host = preferences.getString("host", "");
  websocket_server_port = preferences.getUShort("port", 8888);
  preferences.end();
 
  Serial.println("📂 Settings loaded from flash memory");
  Serial.printf("   SSID: %s\n", ssid.c_str());
  Serial.printf("   Host: %s\n", websocket_server_host.c_str());
  Serial.printf("   Port: %d\n", websocket_server_port);
}


void saveSettings(String newSSID, String newPass, String newHost, uint16_t newPort) {
  preferences.begin("wifi-config", false);
  preferences.putString("ssid", newSSID);
  preferences.putString("pass", newPass);
  preferences.putString("host", newHost);
  preferences.putUShort("port", newPort);
  preferences.end();
 
  Serial.println("💾 Settings saved to flash memory");
}


bool areSettingsValid() {
  return (ssid.length() > 0 && password.length() > 0 && websocket_server_host.length() > 0);
}


void clearSettings() {
  preferences.begin("wifi-config", false);
  preferences.clear();
  preferences.end();
  Serial.println("🗑️ Settings cleared");
}


String getConfigHTML() {
  String html = "<!DOCTYPE html><html>";
  html += "<head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta charset='UTF-8'>";
  html += "<style>";
  html += "body{font-family:Arial,Helvetica,sans-serif; background-color:#f0f0f0; margin:0; padding:20px; text-align:center;}";
  html += ".container{max-width:400px; margin:0 auto; background-color:white; padding:20px; border-radius:10px; box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
  html += "h2{color:#333;}";
  html += "input[type=text], input[type=password], input[type=number]{width:100%; padding:12px; margin:8px 0; border:1px solid #ddd; border-radius:4px; box-sizing:border-box;}";
  html += "button{background-color:#4CAF50; color:white; padding:14px 20px; margin:10px 0; border:none; border-radius:4px; cursor:pointer; width:100%; font-size:16px;}";
  html += "button:hover{background-color:#45a049;}";
  html += ".info{background-color:#e7f3fe; border-left:6px solid #2196F3; margin-bottom:15px; padding:10px;}";
  html += "</style>";
  html += "</head>";
  html += "<body>";
  html += "<div class='container'>";
  html += "<h2>🔧 ESP32 AI Assistant Configuration</h2>";
  html += "<div class='info'>";
  html += "<strong>📡 WiFi Settings & Server IP</strong><br>";
  html += "Ipasok ang iyong WiFi credentials at server IP address.";
  html += "</div>";
  html += "<form method='POST' action='/save'>";
  html += "<input type='text' name='ssid' placeholder='WiFi SSID' required><br>";
  html += "<input type='password' name='pass' placeholder='WiFi Password' required><br>";
  html += "<input type='text' name='host' placeholder='Server IP (e.g., 192.168.1.100)' required><br>";
  html += "<input type='number' name='port' placeholder='Port (default: 8888)' value='8888'><br>";
  html += "<button type='submit'>✅ Save & Restart</button>";
  html += "</form>";
  html += "</div>";
  html += "</body></html>";
  return html;
}


void startConfigPortal() {
  inConfigMode = true;
 
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("CONFIG MODE");
  display.println("");
  display.println("Connect to WiFi:");
  display.println(apSSID);
  display.println("");
  display.println("Open 192.168.4.1");
  display.display();
 
  Serial.println("📱 Starting Configuration Portal...");
  Serial.printf("   AP SSID: %s\n", apSSID);
  Serial.printf("   AP IP: 192.168.4.1\n");
 
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID);
 
  dns.start(53, "*", WiFi.softAPIP());
 
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getConfigHTML());
  });
 
  server.on("/save", HTTP_POST, []() {
    String newSSID = server.arg("ssid");
    String newPass = server.arg("pass");
    String newHost = server.arg("host");
    uint16_t newPort = server.arg("port").toInt();
   
    if (newSSID.length() == 0 || newPass.length() == 0 || newHost.length() == 0) {
      server.send(200, "text/html",
        "<html><body><h2> Error</h2><p>All fields are required!</p>"
        "<a href='/'>Go back</a></body></html>");
      return;
    }
   
    saveSettings(newSSID, newPass, newHost, newPort);
   
    ssid = newSSID;
    password = newPass;
    websocket_server_host = newHost;
    websocket_server_port = newPort;
   
    String html = "<!DOCTYPE html><html>";
    html += "<head><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial; text-align:center; margin-top:50px;}</style>";
    html += "</head><body>";
    html += "<h2> Settings Saved!</h2>";
    html += "<p>Restarting ESP32...</p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
   
    delay(2000);
    ESP.restart();
  });
 
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
 
  server.begin();
 
  while (inConfigMode) {
    dns.processNextRequest();
    server.handleClient();
   
    if (digitalRead(BUTTON_PIN) == LOW) {
      delay(3000);
      if (digitalRead(BUTTON_PIN) == LOW) {
        Serial.println("🚪 Button pressed - exiting config mode");
        inConfigMode = false;
        break;
      }
    }
   
    delay(10);
  }
 
  server.stop();
  dns.stop();
  WiFi.mode(WIFI_STA);
 
  Serial.println("✅ Configuration mode exited");
}


bool connectWiFiWithRetry(int maxRetries) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Connecting to WiFi...");
  display.println(ssid);
  display.display();
 
  Serial.printf("📡 Connecting to WiFi: %s\n", ssid.c_str());
 
  WiFi.begin(ssid.c_str(), password.c_str());
 
  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < maxRetries) {
    int attempt = 0;
    while (WiFi.status() != WL_CONNECTED && attempt < 20) {
      delay(500);
      Serial.print(".");
      attempt++;
    }
   
    if (WiFi.status() == WL_CONNECTED) {
      break;
    }
   
    retryCount++;
    if (retryCount < maxRetries) {
      Serial.printf("\n🔄 Retry %d/%d...\n", retryCount + 1, maxRetries);
      WiFi.begin(ssid.c_str(), password.c_str());
    }
  }
 
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n WiFi Connected!");
    Serial.printf(" IP: %s\n", WiFi.localIP().toString().c_str());
   
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi Connected");
    display.println(WiFi.localIP().toString());
    display.display();
    delay(2000);
    return true;
  } else {
    Serial.println("\n WiFi Failed to connect");
    return false;
  }
}


// ===============================================
// EYE FUNCTIONS
// ===============================================
int calculate_safe_radius(int r, int w, int h) {
    if (w < 2 * (r + 1)) {
        r = (w / 2) - 1;
    }
    if (h < 2 * (r + 1)) {
        r = (h / 2) - 1;
    }
    return (r < 0) ? 0 : r;
}


void draw_eyes() {
    int r_left = calculate_safe_radius(corner_radius, left_eye.width, left_eye.height);
    int x_left = int(left_eye.x - left_eye.width / 2);
    int y_left = int(left_eye.y - left_eye.height / 2);
    display.fillRoundRect(x_left, y_left, left_eye.width, left_eye.height, r_left, SH110X_WHITE);


    int r_right = calculate_safe_radius(corner_radius, right_eye.width, right_eye.height);
    int x_right = int(right_eye.x - right_eye.width / 2);
    int y_right = int(right_eye.y - right_eye.height / 2);
    display.fillRoundRect(x_right, y_right, right_eye.width, right_eye.height, r_right, SH110X_WHITE);
}


void draw_eye_frame() {
    display.clearDisplay();
    draw_eyes();
    display.display();
}


void reset_eyes(bool update) {
  left_eye.height = REF_EYE_HEIGHT;
  left_eye.width = REF_EYE_WIDTH;
  right_eye.height = REF_EYE_HEIGHT;
  right_eye.width = REF_EYE_WIDTH;
 
  left_eye.x = SCREEN_WIDTH / 2 - REF_EYE_WIDTH / 2 - REF_SPACE_BETWEEN_EYE / 2;
  left_eye.y = SCREEN_HEIGHT / 2;
  right_eye.x = SCREEN_WIDTH / 2 + REF_EYE_WIDTH / 2 + REF_SPACE_BETWEEN_EYE / 2;
  right_eye.y = SCREEN_HEIGHT / 2;


  corner_radius = REF_CORNER_RADIUS;
 
  if (update) {
    draw_eye_frame();
  }
}


void sleep_eyes() {
  reset_eyes(false);


  left_eye.height = 2;
  left_eye.width = REF_EYE_WIDTH;
  right_eye.height = 2;
  right_eye.width = REF_EYE_WIDTH;
 
  corner_radius = 0;
  draw_eye_frame();
}


bool shouldStopAnimation() {
  return (isStreamingAudio || isPlayingAudio || showingTextDisplay || inConfigMode);
}


void startAnimation(AnimationState anim) {
  if (shouldStopAnimation()) return;
 
  animState.currentAnimation = anim;
  animState.currentPhase = PHASE_START;
  animState.frameCounter = 0;
  animState.loopCounter = 0;
  animState.animationActive = true;
  animState.lastFrameTime = millis();
  animState.phaseStartTime = millis();
 
  if (anim == ANIM_MOVE_RIGHT_BIG) animState.directionX = 1;
  if (anim == ANIM_MOVE_LEFT_BIG) animState.directionX = -1;
 
  if (anim == ANIM_SACCADE_RANDOM) {
    animState.directionX = random(-1, 2);
    animState.directionY = random(-1, 2);
  }
}


void updateWakeupAnimation() {
  const int MAX_STEPS = (REF_EYE_HEIGHT - 2) / 2;
 
  if (animState.frameCounter <= MAX_STEPS) {
    int h = 2 + (animState.frameCounter * 2);
    left_eye.height = h;
    right_eye.height = h;
    int mapped_radius = map(h, 2, REF_EYE_HEIGHT, 1, REF_CORNER_RADIUS);
    corner_radius = (mapped_radius < h/2) ? mapped_radius : h/2;
    draw_eye_frame();
    animState.frameCounter++;
  } else {
    animState.animationActive = false;
    reset_eyes(true);
  }
}


void updateBlinkAnimation() {
  const int SPEED = 12;
  const int TOTAL_STEPS = 6;
 
  if (animState.frameCounter < 3) {
    left_eye.height -= SPEED;
    right_eye.height -= SPEED;
    int current_h = left_eye.height;
    int mapped_radius = map(current_h, 4, REF_EYE_HEIGHT, 1, REF_CORNER_RADIUS);
    corner_radius = (mapped_radius < current_h/2) ? mapped_radius : current_h/2;
    left_eye.width += 3;
    right_eye.width += 3;
    draw_eye_frame();
  } else if (animState.frameCounter < 6) {
    left_eye.height += SPEED;
    right_eye.height += SPEED;
    int current_h = left_eye.height;
    int mapped_radius = map(current_h, 4, REF_EYE_HEIGHT, 1, REF_CORNER_RADIUS);
    corner_radius = (mapped_radius < current_h/2) ? mapped_radius : current_h/2;
    left_eye.width -= 3;
    right_eye.width -= 3;
    draw_eye_frame();
  }
 
  animState.frameCounter++;
 
  if (animState.frameCounter >= TOTAL_STEPS) {
    if (animState.currentAnimation == ANIM_BLINK_LONG) {
      if (animState.currentPhase == PHASE_START) {
        animState.currentPhase = PHASE_HOLD;
        animState.phaseStartTime = millis();
      } else if (millis() - animState.phaseStartTime > 1000) {
        animState.animationActive = false;
        reset_eyes(true);
      }
    } else {
      animState.animationActive = false;
      reset_eyes(true);
    }
  }
}


void updateSleepAnimation() {
  left_eye.height = 2;
  left_eye.width = REF_EYE_WIDTH;
  right_eye.height = 2;
  right_eye.width = REF_EYE_WIDTH;
  corner_radius = 0;
  draw_eye_frame();
  animState.animationActive = false;
}


void updateHappyAnimation() {
  if (animState.frameCounter == 0) {
    reset_eyes(true);
  }
 
  if (animState.frameCounter < 10) {
    int offset = REF_EYE_HEIGHT / 2 - (animState.frameCounter * 2);
   
    display.fillTriangle(left_eye.x - left_eye.width / 2 - 1, left_eye.y + offset,
                        left_eye.x + left_eye.width / 2 + 1, left_eye.y + 5 + offset,
                        left_eye.x - left_eye.width / 2 - 1, left_eye.y + left_eye.height + offset,
                        SH110X_BLACK);
   
    display.fillTriangle(right_eye.x + right_eye.width / 2 + 1, right_eye.y + offset,
                        right_eye.x - right_eye.width / 2 - 2, right_eye.y + 5 + offset,
                        right_eye.x + right_eye.width / 2 + 1, right_eye.y + right_eye.height + offset,
                        SH110X_BLACK);
   
    display.display();
    animState.frameCounter++;
  } else {
    if (animState.currentPhase == PHASE_START) {
      animState.currentPhase = PHASE_HAPPY_WAIT;
      animState.phaseStartTime = millis();
    } else if (millis() - animState.phaseStartTime > 1000) {
      animState.animationActive = false;
      reset_eyes(true);
    }
  }
}


void updateSaccadeAnimation() {
  const int MOVEMENT_AMPLITUDE_X = 8;
  const int MOVEMENT_AMPLITUDE_Y = 6;
  const int BLINK_AMPLITUDE = 8;
 
  if (animState.frameCounter == 0) {
    reset_eyes(false);
  }
 
  if (animState.currentPhase == PHASE_SACCADE_MOVE_OUT) {
    left_eye.x += MOVEMENT_AMPLITUDE_X * animState.directionX;
    right_eye.x += MOVEMENT_AMPLITUDE_X * animState.directionX;
    left_eye.y += MOVEMENT_AMPLITUDE_Y * animState.directionY;
    right_eye.y += MOVEMENT_AMPLITUDE_Y * animState.directionY;
    right_eye.height -= BLINK_AMPLITUDE;
    left_eye.height -= BLINK_AMPLITUDE;
    draw_eye_frame();
    animState.currentPhase = PHASE_SACCADE_MOVE_BACK;
  } else {
    left_eye.x -= MOVEMENT_AMPLITUDE_X * animState.directionX;
    right_eye.x -= MOVEMENT_AMPLITUDE_X * animState.directionX;
    left_eye.y -= MOVEMENT_AMPLITUDE_Y * animState.directionY;
    right_eye.y -= MOVEMENT_AMPLITUDE_Y * animState.directionY;
    right_eye.height += BLINK_AMPLITUDE;
    left_eye.height += BLINK_AMPLITUDE;
    draw_eye_frame();
   
    animState.loopCounter++;
    if (animState.loopCounter < 20) {
      animState.currentPhase = PHASE_SACCADE_MOVE_OUT;
      animState.directionX = random(-1, 2);
      animState.directionY = random(-1, 2);
    } else {
      animState.animationActive = false;
      reset_eyes(true);
    }
  }
}


void updateMoveBigAnimation() {
  const int OVERSIZE_AMOUNT = 1;
  const int MOVEMENT_AMPLITUDE = 8;
  const int BLINK_AMPLITUDE = 5;
  int direction = animState.directionX;
 
  switch(animState.currentPhase) {
    case PHASE_OUTWARD_1:
      left_eye.x += MOVEMENT_AMPLITUDE * direction;
      right_eye.x += MOVEMENT_AMPLITUDE * direction;    
      right_eye.height -= BLINK_AMPLITUDE;
      left_eye.height -= BLINK_AMPLITUDE;
     
      if (direction > 0) {
        right_eye.height += OVERSIZE_AMOUNT;
        right_eye.width += OVERSIZE_AMOUNT;
      } else {
        left_eye.height += OVERSIZE_AMOUNT;
        left_eye.width += OVERSIZE_AMOUNT;
      }
     
      draw_eye_frame();
      animState.currentPhase = PHASE_OUTWARD_2;
      break;
     
    case PHASE_OUTWARD_2:
      left_eye.x += MOVEMENT_AMPLITUDE * direction;
      right_eye.x += MOVEMENT_AMPLITUDE * direction;
      right_eye.height += BLINK_AMPLITUDE;
      left_eye.height += BLINK_AMPLITUDE;
     
      if (direction > 0) {
        right_eye.height += OVERSIZE_AMOUNT;
        right_eye.width += OVERSIZE_AMOUNT;
      } else {
        left_eye.height += OVERSIZE_AMOUNT;
        left_eye.width += OVERSIZE_AMOUNT;
      }
     
      draw_eye_frame();
      animState.currentPhase = PHASE_HOLD;
      animState.phaseStartTime = millis();
      break;
     
    case PHASE_HOLD:
      if (millis() - animState.phaseStartTime > 1000) {
        animState.currentPhase = PHASE_INWARD_1;
      }
      break;
     
    case PHASE_INWARD_1:
      left_eye.x -= MOVEMENT_AMPLITUDE * direction;
      right_eye.x -= MOVEMENT_AMPLITUDE * direction;    
      right_eye.height -= BLINK_AMPLITUDE;
      left_eye.height -= BLINK_AMPLITUDE;
     
      if (direction > 0) {
        right_eye.height -= OVERSIZE_AMOUNT;
        right_eye.width -= OVERSIZE_AMOUNT;
      } else {
        left_eye.height -= OVERSIZE_AMOUNT;
        left_eye.width -= OVERSIZE_AMOUNT;
      }
     
      draw_eye_frame();
      animState.currentPhase = PHASE_INWARD_2;
      break;
     
    case PHASE_INWARD_2:
      left_eye.x -= MOVEMENT_AMPLITUDE * direction;
      right_eye.x -= MOVEMENT_AMPLITUDE * direction;    
      right_eye.height += BLINK_AMPLITUDE;
      left_eye.height += BLINK_AMPLITUDE;
     
      if (direction > 0) {
        right_eye.height -= OVERSIZE_AMOUNT;
        right_eye.width -= OVERSIZE_AMOUNT;
      } else {
        left_eye.height -= OVERSIZE_AMOUNT;
        left_eye.width -= OVERSIZE_AMOUNT;
      }
     
      draw_eye_frame();
      animState.currentPhase = PHASE_COMPLETE;
      break;
     
    case PHASE_COMPLETE:
      animState.animationActive = false;
      reset_eyes(true);
      break;
     
    default:
      animState.currentPhase = PHASE_OUTWARD_1;
      break;
  }
}


void updateAnimation() {
  if (!animState.animationActive) return;
 
  if (shouldStopAnimation()) {
    animState.animationActive = false;
    reset_eyes(true);
    return;
  }
 
  unsigned long currentTime = millis();
  if (currentTime - animState.lastFrameTime < FRAME_INTERVAL) {
    return;
  }
  animState.lastFrameTime = currentTime;
 
  switch(animState.currentAnimation) {
    case ANIM_WAKEUP:
      updateWakeupAnimation();
      break;
     
    case ANIM_BLINK_SHORT:
    case ANIM_BLINK_LONG:
      updateBlinkAnimation();
      break;
     
    case ANIM_SLEEP:
      updateSleepAnimation();
      break;
     
    case ANIM_HAPPY:
      updateHappyAnimation();
      break;
     
    case ANIM_SACCADE_RANDOM:
      updateSaccadeAnimation();
      break;
     
    case ANIM_MOVE_RIGHT_BIG:
    case ANIM_MOVE_LEFT_BIG:
      updateMoveBigAnimation();
      break;
     
    case ANIM_RESET:
      reset_eyes(true);
      animState.animationActive = false;
      break;
     
    default:
      animState.animationActive = false;
      break;
  }
}


void launchNextAnimation() {
  if (shouldStopAnimation()) return;
 
  switch(current_animation_index) {
    case WAKEUP:
      startAnimation(ANIM_WAKEUP);
      break;
    case RESET:
      startAnimation(ANIM_RESET);
      break;
    case MOVE_RIGHT_BIG:
      startAnimation(ANIM_MOVE_RIGHT_BIG);
      break;
    case MOVE_LEFT_BIG:
      startAnimation(ANIM_MOVE_LEFT_BIG);
      break;
    case BLINK_LONG:
      startAnimation(ANIM_BLINK_LONG);
      break;
    case BLINK_SHORT:
      startAnimation(ANIM_BLINK_SHORT);
      break;
    case HAPPY:
      startAnimation(ANIM_HAPPY);
      break;
    case SLEEP:
      startAnimation(ANIM_SLEEP);
      break;
    case SACCADE_RANDOM:
      startAnimation(ANIM_SACCADE_RANDOM);
      break;
  }
 
  current_animation_index++;
  if (current_animation_index >= MAX_ANIMATIONS) {
    current_animation_index = 0;
  }
}


// ===============================================
// DISPLAY FUNCTIONS
// ===============================================
void displayMessage(String line1, String line2, String line3, String line4) {
  if (inConfigMode) return;
 
  showingTextDisplay = true;
  showingEyeAnimation = false;
 
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
 
  int yPos = 0;
  if (line1 != "") {
    display.setCursor(0, yPos);
    display.println(line1);
    yPos += 10;
  }
  if (line2 != "") {
    display.setCursor(0, yPos);
    display.println(line2);
    yPos += 10;
  }
  if (line3 != "") {
    display.setCursor(0, yPos);
    display.println(line3);
    yPos += 10;
  }
  if (line4 != "") {
    display.setCursor(0, yPos);
    display.println(line4);
  }
 
  if (isMultiPage && totalPages > 1) {
    String pageIndicator = String(currentPage + 1) + "/" + String(totalPages);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(pageIndicator, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(SCREEN_WIDTH - w - 2, SCREEN_HEIGHT - h - 2);
    display.println(pageIndicator);
  }
 
  display.display();
}


void splitTextToPages(String text) {
  for (int i = 0; i < 15; i++) {
    displayPages[i] = "";
  }
 
  text.replace("**", "");
  text.replace("*", "");
  text.replace("###", "");
  text.replace("AI: ", "");
  text.trim();
 
  if (text.length() <= 84) {
    displayPages[0] = text;
    totalPages = 1;
    isMultiPage = false;
    return;
  }
 
  String words[300];
  int wordCount = 0;
  int startPos = 0;
  int endPos = text.indexOf(' ');
 
  while (endPos != -1 && wordCount < 299) {
    words[wordCount++] = text.substring(startPos, endPos);
    startPos = endPos + 1;
    endPos = text.indexOf(' ', startPos);
  }
  if (startPos < text.length()) {
    words[wordCount++] = text.substring(startPos);
  }
 
  int pageIndex = 0;
  int lineIndex = 0;
  String currentLine = "";
 
  for (int i = 0; i < wordCount && pageIndex < 15; i++) {
    String testLine = currentLine;
    if (testLine != "") testLine += " ";
    testLine += words[i];
   
    if (testLine.length() <= 21) {
      currentLine = testLine;
    } else {
      displayPages[pageIndex] += currentLine + "\n";
      lineIndex++;
     
      if (lineIndex >= 4) {
        pageIndex++;
        lineIndex = 0;
        if (pageIndex >= 15) break;
      }
     
      currentLine = words[i];
    }
  }
 
  if (currentLine != "" && pageIndex < 15) {
    displayPages[pageIndex] += currentLine;
    lineIndex++;
  }
 
  totalPages = pageIndex + 1;
  isMultiPage = (totalPages > 1);
 
  Serial.printf("Split text into %d pages\n", totalPages);
}


void displayCurrentPage() {
  if (totalPages == 0) return;
 
  String pageText = displayPages[currentPage];
  int lastIndex = 0;
  String lines[4];
 
  for (int i = 0; i < 4; i++) {
    int nextBreak = pageText.indexOf('\n', lastIndex);
    if (nextBreak == -1) {
      lines[i] = pageText.substring(lastIndex);
      break;
    } else {
      lines[i] = pageText.substring(lastIndex, nextBreak);
      lastIndex = nextBreak + 1;
    }
  }
 
  displayMessage(lines[0], lines[1], lines[2], lines[3]);
}


void nextPage() {
  if (totalPages <= 1) return;
 
  currentPage = (currentPage + 1) % totalPages;
  lastPageChange = millis();
  displayCurrentPage();
 
  Serial.printf("Switched to page %d/%d\n", currentPage + 1, totalPages);
}


void resetDisplay() {
  showingTextDisplay = false;
  showingEyeAnimation = true;
 
  fullResponse = "";
  userQuestion = "";
  currentPage = 0;
  totalPages = 0;
  isMultiPage = false;
  displayTimeout = 0;
  shouldIgnoreOldResponse = false;
 
  displayShowingListening = false;
  displayShowingProcessing = false;
  displayShowingTranscribed = false;
  displayShowingAI = false;
 
  isScrollingActive = false;
  currentScrollingText = "";
 
  connectionMessageTime = 0;
 
  reset_eyes(true);
}


void updateTranscribedDisplay() {
  if (currentScrollingText.length() == 0 || !isScrollingActive) return;
 
  String line1, line2, line3, line4;
  const int MAX_CHARS = 21;
 
  String displayText = currentScrollingText.substring(scrollOffset);
 
  auto getWrappedLine = [&](int &startPos) -> String {
    if (startPos >= displayText.length()) return "";
   
    if (displayText.length() - startPos <= MAX_CHARS) {
      String line = displayText.substring(startPos);
      startPos = displayText.length();
      return line;
    }
   
    int endPos = startPos + MAX_CHARS;
   
    if (endPos < displayText.length() && displayText.charAt(endPos) != ' ') {
      for (int i = endPos; i > startPos; i--) {
        if (displayText.charAt(i) == ' ') {
          endPos = i;
          break;
        }
      }
    }
   
    String line = displayText.substring(startPos, endPos);
    startPos = endPos;
    if (startPos < displayText.length() && displayText.charAt(startPos) == ' ') {
      startPos++;
    }
   
    return line;
  };
 
  int pos = 0;
  line1 = getWrappedLine(pos);
  line2 = getWrappedLine(pos);
  line3 = getWrappedLine(pos);
  line4 = getWrappedLine(pos);
 
  displayMessage(line1, line2, line3, line4);
}


// ===============================================
// AUDIO STREAMING TASK
// ===============================================
void audioStreamTask(void* parameter) {
  Serial.println("🎤 Audio streaming task started on Core 0");
 
  size_t bytesRead;
  int chunkCount = 0;
 
  while (true) {
    if (isWebSocketConnected && isStreamingAudio) {
      esp_err_t result = i2s_read(I2S_PORT_MIC,
                                  sBuffer,
                                  bufferLen * sizeof(int16_t),
                                  &bytesRead,
                                  portMAX_DELAY);
     
      if (result == ESP_OK && bytesRead > 0) {
        client.sendBinary((const char*)sBuffer, bytesRead);
       
        chunkCount++;
        if (chunkCount % 50 == 0) {
          Serial.printf("📤 Sent %d audio chunks\n", chunkCount);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
   
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}


void audioPlayTask(void* parameter) {
  Serial.println("🔊 Audio playback task started on Core 0");
  
  const size_t PLAY_BLOCK_SAMPLES = 256;
  int16_t playBuf[PLAY_BLOCK_SAMPLES];
  
  bool isBuffering = true;
  const int PREROLL_SAMPLES = 4800; // 200ms of audio buffer at 24000Hz
  bool wasUnderrunLogged = false;
  
  while (true) {
    if (isPlayingAudio) {
      if (isBuffering) {
        // If the server finished sending, we start playing immediately to drain whatever remains
        if (audioSamplesAvailable >= PREROLL_SAMPLES || 
            audioSamplesAvailable == actualAudioBufferSize || 
            isAudioFinishedSending) {
          isBuffering = false;
          wasUnderrunLogged = false;
          Serial.println("🔊 Buffer ready, starting playback");
        } else {
          // Keep speaker quiet while buffering
          memset(playBuf, 0, PLAY_BLOCK_SAMPLES * sizeof(int16_t));
          size_t bytesWritten;
          i2s_write(I2S_PORT_SPEAKER, playBuf, PLAY_BLOCK_SAMPLES * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
          vTaskDelay(pdMS_TO_TICKS(5));
          continue;
        }
      }
      
      int take = 0;
      portENTER_CRITICAL(&audioBufferMux);
      if (audioSamplesAvailable > 0) {
        take = min((int)PLAY_BLOCK_SAMPLES, (int)audioSamplesAvailable);
        int idx = audioReadIndex;
        for (int i = 0; i < take; i++) {
          playBuf[i] = audioBuffer[idx];
          idx = (idx + 1) % actualAudioBufferSize;
        }
        audioReadIndex = idx;
        audioSamplesAvailable -= take;
      }
      portEXIT_CRITICAL(&audioBufferMux);
      
      if (take > 0) {
        size_t bytesWritten;
        i2s_write(I2S_PORT_SPEAKER, playBuf, take * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
      } else {
        // Buffer is empty
        if (isAudioFinishedSending) {
          // Playback completed cleanly. Wait for hardware DMA buffer to finish playing (approx 340ms)
          Serial.println("🔊 Buffer drained. Flushing hardware I2S DMA...");
          vTaskDelay(pdMS_TO_TICKS(350));
          
          Serial.println("🔊 Audio playback completed cleanly (buffer fully drained)");
          isPlayingAudio = false;
          isAISpeaking = false;
          isAudioFinishedSending = false;
          
          // Return Servo 1 to neutral position (harap)
          moveServo1(90);  // 90 degrees = harap
          
          // Trigger OLED updates on Core 1 safely
          triggerOLEDUpdateAfterAudio = true;
        } else {
          // Network jitter underrun
          isBuffering = true;
          if (!wasUnderrunLogged) {
            Serial.println("⚠️ Speaker buffer underrun - buffering...");
            wasUnderrunLogged = true;
          }
          vTaskDelay(pdMS_TO_TICKS(5));
        }
      }
    } else {
      isBuffering = true;
      wasUnderrunLogged = false;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}


// ===============================================
// MAIN FUNCTIONS
// ===============================================
void startRecording() {
  if (isWebSocketConnected && !isStreamingAudio) {
    isStreamingAudio = true;
    Serial.println("🎤 Recording STARTED - speak now!");
   
    shouldIgnoreOldResponse = true;
   
    fullResponse = "";
    userQuestion = "";
    currentPage = 0;
    totalPages = 0;
    isMultiPage = false;
   
    for (int i = 0; i < 15; i++) {
      displayPages[i] = "";
    }
   
    isScrollingActive = false;
    currentScrollingText = "";
   
    displayTimeout = 0;
   
    clearAudioBuffer();
   
    client.send("START_RECORDING");
   
    Serial.println("✅ OLD RESPONSE COMPLETELY FLUSHED - Ready for new query");
  }
}


void stopRecording() {
  if (isWebSocketConnected && isStreamingAudio) {
    isStreamingAudio = false;
    Serial.println("🛑 Recording STOPPED");
   
    delay(100);
    shouldIgnoreOldResponse = false;
   
    client.send("STOP_RECORDING");
  }
}


void connectWSServer() {
  if (inConfigMode) return;
 
  displayMessage("Connecting to", "Server...");
  Serial.printf("🌐 Connecting to server: %s:%d\n", websocket_server_host.c_str(), websocket_server_port);
 
  client.onMessage([&](WebsocketsMessage message) {
    if (message.isBinary()) {
      handleAudioData(message.c_str(), message.length());
    } else if (message.isText()) {
      handleTextMessage(message.data());
    }
  });
 
  client.onEvent([&](WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
      Serial.println("\n✅ WebSocket Connected!");
      displayMessage("Connected!", "Ready to use");
      isWebSocketConnected = true;
      client.send("AUDIO_READY");
      connectionMessageTime = millis();
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      Serial.println("\n❌ WebSocket Disconnected!");
      displayMessage("Disconnected");
      isWebSocketConnected = false;
      isStreamingAudio = false;
      isPlayingAudio = false;
    }
  });
 
  bool connected = client.connect(websocket_server_host.c_str(), websocket_server_port, "/");
  if (!connected) {
    Serial.println("\n❌ Connection Failed");
    displayMessage("Connection", "Failed");
  }
}


void handleTextMessage(String message) {
  Serial.printf("📨 Server: %s\n", message.c_str());
 
  if (shouldIgnoreOldResponse) {
    if (message.startsWith("AI_RESPONSE:") || message.startsWith("TRANSCRIBED:")) {
      Serial.println("🚫 IGNORING OLD RESPONSE - New query in progress");
      return;
    }
  }
 
  if (message.startsWith("AUDIO_START")) {
    Serial.println("🔊 Starting audio playback from AI");
    clearAudioBuffer();
    isPlayingAudio = true;
    isAISpeaking = true;  // Set AI speaking flag to TRUE
    isAudioFinishedSending = false;
    displayMessage("AI Speaking...");
    
    displayShowingAI = true;
    displayShowingTranscribed = false;
    
    // Servo 3 (base) ay pupunta sa neutral position kapag nagsasalita ang AI
    moveServo3(90);  // 90 degrees = neutral/harap
    
  } else if (message == "AUDIO_END") {
    Serial.println("🔊 Server signaled AUDIO_END - draining buffer");
    isAudioFinishedSending = true;
    // We let audioPlayTask handle the transition of isPlayingAudio, isAISpeaking, and display updates when the buffer is empty.
   
  } else if (message.startsWith("TRANSCRIBED:")) {
    String transcribedText = message.substring(12);
    Serial.printf("👤 You said: %s\n", transcribedText.c_str());
   
    userQuestion = transcribedText;
    transcribedText.trim();
   
    const int MAX_CHARS = 21;
    String displayText = "You: " + transcribedText;
   
    if (displayText.length() <= (MAX_CHARS * 4)) {
        String line1, line2, line3, line4;
       
        auto getWrappedLine = [&](int &startPos) -> String {
            if (startPos >= displayText.length()) return "";
           
            if (displayText.length() - startPos <= MAX_CHARS) {
                String line = displayText.substring(startPos);
                startPos = displayText.length();
                return line;
            }
           
            int endPos = startPos + MAX_CHARS;
           
            if (endPos < displayText.length() && displayText.charAt(endPos) != ' ') {
                for (int i = endPos; i > startPos; i--) {
                    if (displayText.charAt(i) == ' ') {
                        endPos = i;
                        break;
                    }
                }
            }
           
            String line = displayText.substring(startPos, endPos);
            startPos = endPos;
            if (startPos < displayText.length() && displayText.charAt(startPos) == ' ') {
                startPos++;
            }
           
            return line;
        };
       
        int pos = 0;
        line1 = getWrappedLine(pos);
        line2 = getWrappedLine(pos);
        line3 = getWrappedLine(pos);
        line4 = getWrappedLine(pos);
       
        displayMessage(line1, line2, line3, line4);
       
        isScrollingActive = false;
    } else {
        currentScrollingText = displayText;
        scrollOffset = 0;
        isScrollingActive = true;
        scrollStartTime = millis();
        lastScrollTime = millis();
       
        Serial.println("🔄 Starting text scrolling");
       
        updateTranscribedDisplay();
    }
   
    displayShowingTranscribed = true;
    displayShowingProcessing = false;
   
  } else if (message.startsWith("AI_RESPONSE:")) {
    String aiResponse = message.substring(12);
    Serial.printf("🤖 AI: %s\n", aiResponse.c_str());
   
    fullResponse = aiResponse;
   
    isScrollingActive = false;
   
    if (!shouldIgnoreOldResponse) {
      splitTextToPages(fullResponse);
      currentPage = 0;
      isMultiPage = (totalPages > 1);
      lastPageChange = millis();
      displayCurrentPage();
      displayTimeout = millis();
     
      displayShowingAI = true;
      displayShowingTranscribed = false;
     
      Serial.printf("✅ Displaying new response. Pages: %d, Current page: %d\n", totalPages, currentPage + 1);
    } else {
      Serial.println("🚫 New AI response ignored - waiting for new query completion");
    }
   
  } else if (message == "NO_SPEECH_DETECTED") {
    Serial.println("❌ No speech detected in audio");
    if (!shouldIgnoreOldResponse) {
      displayMessage("No speech", "detected");
      displayTimeout = millis();
      resetDisplay();
    }
   
  } else if (message.startsWith("DEBUG:")) {
    String debugMsg = message.substring(6);
    Serial.printf("🐛 DEBUG: %s\n", debugMsg.c_str());
   
  } else if (message == "PROCESSING_AUDIO") {
    if (!shouldIgnoreOldResponse) {
      displayMessage("Processing", "audio...");
      displayShowingProcessing = true;
    }
   
  } else if (message == "RECORDING_STARTED") {
  } else if (message == "INTERRUPT_ACK") {
    Serial.println("✅ Python backend acknowledged interrupt");
  }
}


void handleAudioData(const char* data, size_t length) {
  if (length == 0) return;

  // Discard incoming audio data during new recordings/interrupted states
  if (isStreamingAudio || shouldIgnoreOldResponse) {
    return;
  }

  const uint8_t* bytes = (const uint8_t*)data;
  size_t offset = 0;

  if (length >= 12 && bytes[0]=='R' && bytes[1]=='I' && bytes[2]=='F' && bytes[3]=='F') {
    offset = 44;
  }

  size_t remaining = (length > offset) ? (length - offset) : 0;
  if (remaining == 0) return;

  const int16_t* in16 = (const int16_t*)(bytes + offset);
  size_t samples = remaining / 2;

  writeToAudioBuffer(in16, samples);

  isPlayingAudio = true;
  isAISpeaking = true;
}


void setupI2SMicrophone() {
  Serial.println("🎤 Initializing I2S microphone...");
  displayMessage("Init Mic...");
 
  const i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 24000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = bufferCnt,
    .dma_buf_len = bufferLen,
    .use_apll = false
  };
 
  esp_err_t err = i2s_driver_install(I2S_PORT_MIC, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S install failed: %d\n", err);
    return;
  }
 
  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };
 
  i2s_set_pin(I2S_PORT_MIC, &pin_config);
  i2s_start(I2S_PORT_MIC);
 
  Serial.println("✅ I2S Microphone Ready");
  displayMessage("Mic Ready");
  delay(500);
}


void setupI2SSpeaker() {
  Serial.println("🔊 Initializing I2S speaker...");
  displayMessage("Init Speaker...");
 
  const i2s_config_t i2s_speaker_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 24000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 32,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };
 
  esp_err_t result = i2s_driver_install(I2S_PORT_SPEAKER, &i2s_speaker_config, 0, NULL);
  if (result != ESP_OK) {
    Serial.printf("❌ I2S driver install failed: %d\n", result);
    displayMessage("Speaker Error");
    return;
  }
 
  const i2s_pin_config_t speaker_pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = -1
  };
 
  i2s_set_pin(I2S_PORT_SPEAKER, &speaker_pin_config);
  i2s_start(I2S_PORT_SPEAKER);
 
  Serial.println("✅ I2S Speaker Ready");
  displayMessage("Speaker Ready");
  delay(500);
 
  initAudioBuffer();
 
  playTestBeep();
}


void playTestBeep() {
  Serial.println("🔊 TEST: Playing test beep...");
 
  int16_t beepBuffer[1200];
  for (int i = 0; i < 1200; i++) {
    float progress = (float)i / 1200.0;
    float fade = 1.0;
    if (progress < 0.1) fade = progress / 0.1;
    if (progress > 0.9) fade = (1.0 - progress) / 0.1;
   
    float sample = sin(2 * PI * 440 * i / 24000.0);
    beepBuffer[i] = (int16_t)(sample * 15000 * fade);
  }
 
  size_t bytesWritten;
  i2s_write(I2S_PORT_SPEAKER, beepBuffer, sizeof(beepBuffer), &bytesWritten, portMAX_DELAY);
 
  Serial.printf("🔊 TEST: Beep played (%d bytes)\n", bytesWritten);
}


void testPSRAM() {
  Serial.println("\n===== PSRAM TEST =====");
  if (psramFound()) {
    Serial.println("✅ PSRAM WORKING!");
    Serial.printf("Size: %d MB\n", ESP.getPsramSize() / 1048576);
   
    void* testPtr = ps_malloc(1024);
    if (testPtr) {
      Serial.println("✅ Can allocate memory!");
      free(testPtr);
    } else {
      Serial.println("❌ Cannot allocate memory!");
    }
  } else {
    Serial.println("❌ PSRAM NOT FOUND");
  }
  Serial.println("=====================\n");
}


void setup() {
  Serial.begin(115200);
  delay(2000);
 
  Serial.println();
  Serial.println("🚀 ESP32-S3 Voice Client with Triple Servo");
  Serial.println("=========================================");
 
  testPSRAM();
 
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(SERVO1_PIN, OUTPUT);
  pinMode(SERVO2_PIN, OUTPUT);
  pinMode(SERVO3_PIN, OUTPUT);
  Serial.println("🔘 Button initialized on GPIO 3");
  Serial.println("🦾 Servo 1 initialized on GPIO 2 (Waggle/AI Speaking)");
  Serial.println("🦾 Servo 2 initialized on GPIO 8 (Oscillating - continuous)");
  Serial.println("🦾 Servo 3 initialized on GPIO 9 (Base - oscillates, stops when AI speaks)");
 
  // Initialize all servos to neutral position
  digitalWrite(SERVO1_PIN, LOW);
  digitalWrite(SERVO2_PIN, LOW);
  digitalWrite(SERVO3_PIN, LOW);
  moveServo1(90);  // 90 degrees = harap
  moveServo2(90);  // 90 degrees = harap
  moveServo3(90);  // 90 degrees = harap
 
  Wire.begin(I2C_SDA, I2C_SCL);
 
  if(!display.begin(0x3C, true)) {
    Serial.println(F("❌ SH1106 allocation failed"));
    for(;;);
  }
 
  Serial.println("✅ SH1106 OLED Initialized");
 
  loadSettings();
 
  bool buttonPressed = (digitalRead(BUTTON_PIN) == LOW);
 
  if (buttonPressed && !configModeTriggered) {
    Serial.println("🔘 Button held during boot - ENTERING CONFIG MODE");
    configModeTriggered = true;
   
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("⚙️ CONFIG MODE");
    display.println("");
    display.println("Hold button for");
    display.println("3 seconds to enter");
    display.println("configuration...");
    display.display();
   
    delay(3000);
   
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("✅ Button still held - starting config portal");
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("⚙️ CONFIG MODE");
      display.println("ACTIVATED");
      display.display();
      delay(2000);
     
      startConfigPortal();
    } else {
      Serial.println("ℹ️ Button released - normal boot");
      configModeTriggered = false;
    }
  }
 
  if (!areSettingsValid() && !configModeTriggered) {
    Serial.println("⚠️ No valid settings found");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("⚠️ NO SETTINGS");
    display.println("");
    display.println("Hold button at");
    display.println("boot for config");
    display.println("mode");
    display.display();
    delay(3000);
  }
 
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0,0);
  display.println("AI Assistant");
  display.println("Starting...");
  display.display();
 
  randomSeed(analogRead(0));
 
  if (areSettingsValid()) {
    if (!connectWiFiWithRetry(3)) {
      Serial.println("❌ WiFi connection failed");
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("❌ WiFi Failed");
      display.println("Hold button at");
      display.println("boot for config");
      display.println("mode");
      display.display();
      delay(3000);
    }
  } else {
    Serial.println("❌ No valid WiFi settings");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("❌ NO SETTINGS");
    display.println("Hold button at");
    display.println("boot for config");
    display.println("mode");
    display.display();
    delay(3000);
  }
 
  if (WiFi.status() == WL_CONNECTED) {
    connectWSServer();
  }
 
  setupI2SMicrophone();
  setupI2SSpeaker();
 
  xTaskCreatePinnedToCore(
    audioStreamTask,
    "AudioTask",
    4096,
    NULL,
    1,
    &audioTaskHandle,
    0
  );

  xTaskCreatePinnedToCore(
    audioPlayTask,
    "PlayTask",
    8192,
    NULL,
    2,
    &playTaskHandle,
    0
  );
 
  sleep_eyes();
 
  Serial.println("✅ Setup Complete");
  Serial.println("🎤 Hold button to speak");
  Serial.println("🔧 Config: HOLD button during boot");
  Serial.println("🦾 Servo 1: Wiggles when AI speaks, returns to neutral when done");
  Serial.println("🦾 Servo 2: Continuously oscillates (never stops)");
  Serial.println("🦾 Servo 3: Oscillates normally, goes to neutral when AI speaks");
}


void loop() {
  // Safe multi-core OLED display updates
  if (triggerOLEDUpdateAfterAudio) {
    triggerOLEDUpdateAfterAudio = false;
    Serial.println("📺 Core 1: Executing post-playback OLED update");
    if (fullResponse.length() > 0 && !shouldIgnoreOldResponse) {
      currentPage = 0;
      lastPageChange = millis();
      displayCurrentPage();
      displayTimeout = millis();
    } else {
      if (!shouldIgnoreOldResponse) {
        resetDisplay();
      }
    }
  }

  bool currentButtonState2 = digitalRead(BUTTON_PIN) == LOW;
 
  // Button handling
  if (currentButtonState2 && !lastButtonState) {
    buttonPressed = true;
    
    if (isPlayingAudio || isAISpeaking) {
      Serial.println("🛑 Interrupting AI Speech...");
      client.send("INTERRUPT");
      isPlayingAudio = false;
      isAISpeaking = false;
      isAudioFinishedSending = false;
      triggerOLEDUpdateAfterAudio = false;
      i2s_zero_dma_buffer(I2S_PORT_SPEAKER);
      clearAudioBuffer();
    }
    
    startRecording();
    displayMessage("Listening...");
    isStreamingAudio = true;
   
    displayShowingListening = true;
    displayShowingProcessing = false;
    displayShowingTranscribed = false;
    displayShowingAI = false;
   
    Serial.println("✅ Button PRESSED");
  }
 
  if (!currentButtonState2 && lastButtonState && buttonPressed) {
    buttonPressed = false;
    stopRecording();
   
    displayMessage("Processing...");
   
    displayShowingListening = false;
    displayShowingProcessing = true;
    displayShowingTranscribed = false;
    displayShowingAI = false;
   
    Serial.println("✅ Button RELEASED");
  }
 
  lastButtonState = currentButtonState2;
 
  // UPDATE ALL SERVOS - every loop
  updateServo1Position();
  updateServo2Position();
  updateServo3Position();
 
  // SERVO 1: Waggle habang nagsasalita ang AI
  if (isAISpeaking) {
    waggleServo1();
  } else {
    // Kapag hindi nagsasalita, balik sa neutral position (harap)
    moveServo1(90);  // 90 degrees = harap
  }
 
  // SERVO 2: Continuous oscillation (hindi tumitigil kahit nagsasalita ang AI)
  oscillateServo2();
 
  // SERVO 3: Oscillate kapag HINDI nagsasalita ang AI
  if (!isAISpeaking) {
    oscillateServo3();
  } else {
    // Kapag nagsasalita ang AI, manatili sa neutral position
    // (ito ay na-set na sa AUDIO_START message)
  }
 
  // Scroll handling
  if (isScrollingActive) {
    unsigned long currentTime = millis();
    if (currentTime - scrollStartTime > 30000) {
      isScrollingActive = false;
    } else if (currentTime - lastScrollTime > SCROLL_DELAY) {
      scrollOffset += 63;
     
      if (scrollOffset >= (int)currentScrollingText.length()) {
        scrollOffset = 0;
      }
     
      lastScrollTime = currentTime;
     
      if (displayShowingTranscribed) {
        updateTranscribedDisplay();
      }
    }
  }
 
  // WebSocket handling
  if (isWebSocketConnected) {
    client.poll();
  } else {
    if (WiFi.status() == WL_CONNECTED) {
      static unsigned long lastReconnectAttempt = 0;
      if (millis() - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = millis();
        Serial.println("🔄 Attempting to reconnect...");
        displayMessage("Reconnecting...");
        connectWSServer();
      }
    }
  }
 
  // Page turning
  if (isMultiPage && totalPages > 1) {
    if (millis() - lastPageChange > PAGE_CHANGE_DELAY) {
      nextPage();
    }
  }
 
  // Display timeout
  if (displayTimeout > 0 && millis() - displayTimeout > DISPLAY_TIMEOUT) {
    if (!isStreamingAudio && !isPlayingAudio) {
      resetDisplay();
    }
  }
 
  // Connection message timeout
  if (connectionMessageTime > 0) {
    if (millis() - connectionMessageTime > CONNECTION_MESSAGE_DURATION) {
      Serial.println("⏰ Connection message timeout - resetting to eye animation");
      connectionMessageTime = 0;
      resetDisplay();
    }
  }
 
  // Eye animation logic
  if (showingEyeAnimation &&
      !isStreamingAudio &&
      !isPlayingAudio &&
      fullResponse.length() == 0 &&
      !showingTextDisplay &&
      !inConfigMode &&
      connectionMessageTime == 0) {
   
    if (animState.animationActive) {
      updateAnimation();
    } else {
      unsigned long currentTime = millis();
      if (currentTime - lastAnimationCycleTime > ANIMATION_CYCLE_INTERVAL) {
        lastAnimationCycleTime = currentTime;
        launchNextAnimation();
      }
    }
  }
 
  yield();
}

