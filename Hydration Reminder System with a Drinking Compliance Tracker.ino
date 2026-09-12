// ============================================================
//  Hydration Reminder System with Drinking Compliance Tracker
//  Hardware : ESP32 Dev Module
//  Display  : 1.3" SH1106 OLED (I2C 0x3C)
//  Sensor   : Capacitive copper-strip (GPIO4=TX, GPIO15=RX)
//  RGB LED  : R=GPIO25  G=GPIO26  B=GPIO27
//  Buzzer   : GPIO18
// ============================================================

// ---------- Libraries ----------
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <EEPROM.h>

// ============================================================
//  CONFIGURATION
// ============================================================

// --- WiFi ---
const char* WIFI_SSID     = "Phone";
const char* WIFI_PASSWORD = "12345678901";

// --- Telegram ---
#define BOT_TOKEN  "8354884236:AAEYBkVlhDkGddBqakC-d1BNHVlTyyGbbzg"
#define CHAT_ID    "5656656056"

// --- OLED ---
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- Sensor Pins ---
const int TX_PIN = 4;   // drive HIGH -> provides charge path
const int RX_PIN = 15;  // read capacitive touch value

// --- RGB LED Pins ---
const int LED_RED   = 25;
const int LED_GREEN = 26;
const int LED_BLUE  = 27;

// --- Buzzer ---
const int BUZZER_PIN = 18;

// --- Bottle / Sensor Calibration ---
const int BOTTLE_CAPACITY_ML = 500;
const int SENSOR_EMPTY       = 1001;   // touch value when bottle empty
const int SENSOR_FULL        =  900;   // touch value when bottle full

// --- Thresholds ---
const int SIP_THRESHOLD    = 30;   // ml drop to count as a sip
const int REFILL_THRESHOLD = 40;   // ml rise to detect a refill
const int NOISE_GUARD_ML   = 15;   // ignore changes smaller than this

// --- Reminder ---
const unsigned long REMINDER_INTERVAL_MS = 180000UL; // 3 minutes (testing value for 1 hour)

// --- Moving-Average Filter ---
const int NUM_SAMPLES = 20;
int samples[NUM_SAMPLES];
int sampleIndex = 0;

// --- EEPROM addresses ---
#define EEPROM_SIZE        16
#define ADDR_GOAL          0   // 4 bytes  (int)
#define ADDR_CONSUMED      4   // 4 bytes  (int)
#define ADDR_SIP_COUNT     8   // 4 bytes  (int)

// --- Hourly graph storage (24 h) ---
int hourlyIntake[24] = {0};

// ============================================================
//  GLOBAL STATE
// ============================================================
int  currentML      = 0;
int  previousML     = 0;
int  lastRawValue   = 0;
int  consumedToday  = 0;
int  sipCount       = 0;
int  dailyGoal      = 3000;   // default, overridden from EEPROM

unsigned long lastDrinkTime  = 0;
unsigned long lastTelegramMs = 0;
unsigned long lastOledMs     = 0;
bool          reminderSent   = false;
String        activeChatId   = CHAT_ID;   // updated once a real message arrives

WiFiClientSecure    client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
void setLED(bool r, bool g, bool b);
void updateLED();
void beepReminder();
void sipBeep();
void startupBeep();
void showOLED();
void showOLEDMessage(const char* line1, const char* line2);
void connectWiFi();
void checkWiFiReconnect();
String buildStatusMessage();
String buildGraphURL();
void handleTelegramMessages();
void checkReminder();
void updateConsumption();
int readFilteredSensor();
int sensorToML(int raw);
void initSamples();
void loadFromEEPROM();
void saveToEEPROM();

// ============================================================
//  EEPROM HELPERS
// ============================================================
void eepromWriteInt(int addr, int value) {
    EEPROM.put(addr, value);
    EEPROM.commit();
}

int eepromReadInt(int addr) {
    int value = 0;
    EEPROM.get(addr, value);
    return value;
}

void loadFromEEPROM() {
    int g = eepromReadInt(ADDR_GOAL);
    if (g > 0 && g <= 10000) dailyGoal = g;

    int c = eepromReadInt(ADDR_CONSUMED);
    if (c >= 0 && c <= 50000) consumedToday = c;

    int s = eepromReadInt(ADDR_SIP_COUNT);
    if (s >= 0 && s <= 5000)  sipCount = s;
}

void saveToEEPROM() {
    eepromWriteInt(ADDR_GOAL,      dailyGoal);
    eepromWriteInt(ADDR_CONSUMED,  consumedToday);
    eepromWriteInt(ADDR_SIP_COUNT, sipCount);
}

// ============================================================
//  SENSOR - Moving-Average Filter
// ============================================================
void initSamples() {
    for (int i = 0; i < NUM_SAMPLES; i++) samples[i] = SENSOR_EMPTY;
}

int readFilteredSensor() {
    int readings[5];
    for (int i = 0; i < 5; i++) {
        readings[i] = touchRead(RX_PIN);
        delay(2);
    }

    // sort (simple bubble sort for 5 elements)
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            if (readings[j] < readings[i]) {
                int t = readings[i];
                readings[i] = readings[j];
                readings[j] = t;
            }
        }
    }

    int raw = readings[2]; // median

    samples[sampleIndex] = raw;
    sampleIndex = (sampleIndex + 1) % NUM_SAMPLES;

    long total = 0;
    for (int i = 0; i < NUM_SAMPLES; i++) total += samples[i];
    return (int)(total / NUM_SAMPLES);
}

int sensorToML(int raw) {
    int ml = map(raw, SENSOR_EMPTY, SENSOR_FULL, 0, BOTTLE_CAPACITY_ML);
    return constrain(ml, 0, BOTTLE_CAPACITY_ML);
}

// ============================================================
//  RGB LED
// ============================================================
void setLED(bool r, bool g, bool b) {
    digitalWrite(LED_RED,   r ? HIGH : LOW);
    digitalWrite(LED_GREEN, g ? HIGH : LOW);
    digitalWrite(LED_BLUE,  b ? HIGH : LOW);
}

void updateLED() {
    float progress = (dailyGoal > 0) ? ((float)consumedToday / dailyGoal) : 0.0f;
    bool overdue   = (millis() - lastDrinkTime) > REMINDER_INTERVAL_MS;

    if (progress >= 1.0f) {
        setLED(false, true, false);   // GREEN - goal achieved
    } else if (overdue) {
        setLED(true,  false, false);  // RED - no intake for 1 h
    } else {
        setLED(false, false, true);   // BLUE - normal hydration
    }
}

// ============================================================
//  BUZZER
// ============================================================
void beepReminder() {
    for (int i = 0; i < 3; i++) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(300);
        digitalWrite(BUZZER_PIN, LOW);
        delay(200);
    }
}

void sipBeep()
{
    digitalWrite(BUZZER_PIN, HIGH);
    delay(80);
    digitalWrite(BUZZER_PIN, LOW);
}

void startupBeep()
{
    for(int i=0;i<3;i++)
    {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(150);
        digitalWrite(BUZZER_PIN, LOW);
        delay(100);
    }
}

// ============================================================
//  OLED DISPLAY
// ============================================================
void showOLED() {
    int remaining = max(0, dailyGoal - consumedToday);
    int progress  = (dailyGoal > 0)
                      ? constrain((int)((float)consumedToday / dailyGoal * 100), 0, 100)
                      : 0;

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);

    // Title
    display.setCursor(0, 0);
    display.print("Hydration  Raw:");
    display.println(lastRawValue);
    display.drawLine(0, 9, SCREEN_WIDTH - 1, 9, SH110X_WHITE);

    // Current water level
    display.setCursor(0, 12);
    display.print("Level : ");
    display.print(currentML);
    display.println(" ml");

    // Consumed
    display.setCursor(0, 22);
    display.print("Used  : ");
    display.print(consumedToday);
    display.println(" ml");

    // Remaining to goal
    display.setCursor(0, 32);
    display.print("Left  : ");
    display.print(remaining);
    display.println(" ml");

    // Sip count
    display.setCursor(0, 42);
    display.print("Sips  : ");
    display.println(sipCount);

    // Progress bar
    display.setCursor(0, 54);
    display.print("Goal:");
    display.print(progress);
    display.print("% [");
    int barWidth = (int)((float)progress / 100.0f * 45);
    for (int i = 0; i < 45; i++) {
        display.print(i < barWidth ? '=' : ' ');
    }
    display.print("]");

    display.display();
}

void showOLEDMessage(const char* line1, const char* line2) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 20);
    display.println(line1);
    display.setCursor(0, 35);
    display.println(line2);
    display.display();
}

// ============================================================
//  WiFi
// ============================================================
void connectWiFi() {
    Serial.print("Connecting to WiFi");
    showOLEDMessage("Connecting to", "WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print('.');
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());
    } else {
        Serial.println("\nWiFi FAILED - running offline");
        showOLEDMessage("WiFi Failed", "Offline Mode");
        delay(2000);
    }
}

void checkWiFiReconnect() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost - reconnecting...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            attempts++;
        }
        if (WiFi.status() == WL_CONNECTED)
            Serial.println("WiFi reconnected.");
    }
}

// ============================================================
//  TELEGRAM - Status & Graph
// ============================================================
String buildStatusMessage() {
    int remaining = max(0, dailyGoal - consumedToday);
    int progress  = (dailyGoal > 0)
                      ? constrain((int)((float)consumedToday / dailyGoal * 100), 0, 100)
                      : 0;

    String msg = "Hydration Status\n\n";
    msg += "Water Level : "  + String(currentML)    + " ml\n";
    msg += "Consumed    : "  + String(consumedToday) + " ml\n";
    msg += "Remaining   : "  + String(remaining)     + " ml\n";
    msg += "Sip Count   : "  + String(sipCount)      + "\n";
    msg += "Daily Goal  : "  + String(dailyGoal)     + " ml\n";
    msg += "Progress    : "  + String(progress)      + "%\n";
    return msg;
}

String buildGraphURL() {
    // Build chart config JSON with actual vs theoretical hourly intake
    String labels = "";
    String actualData = "";
    String theoreticalData = "";
    int perHourGoal = dailyGoal / 24;

    for (int h = 0; h < 24; h++) {
        if (h > 0) { labels += ","; actualData += ","; theoreticalData += ","; }
        labels        += String(h);
        actualData    += String(hourlyIntake[h]);
        theoreticalData += String(perHourGoal * (h + 1));
    }

    String config = "{\"type\":\"line\",\"data\":{\"labels\":["
                 + labels
                 + "],\"datasets\":["
                 "{\"label\":\"Actual\",\"data\":["    + actualData      + "],\"borderColor\":\"blue\",\"fill\":false},"
                 "{\"label\":\"Target\",\"data\":["    + theoreticalData + "],\"borderColor\":\"green\",\"fill\":false}"
                 "]}}";

    // URL-encode special characters for QuickChart
    String encoded = "";
    for (size_t i = 0; i < config.length(); i++) {
        char c = config[i];
        switch (c) {
            case '{': encoded += "%7B"; break;
            case '}': encoded += "%7D"; break;
            case '[': encoded += "%5B"; break;
            case ']': encoded += "%5D"; break;
            case '"': encoded += "%22"; break;
            case ':': encoded += "%3A"; break;
            case ',': encoded += "%2C"; break;
            case ' ': encoded += "%20"; break;
            default:  encoded += c;
        }
    }

    return "https://quickchart.io/chart?c=" + encoded;
}

// ============================================================
//  TELEGRAM - Command Handler
// ============================================================
void handleTelegramMessages() {
    if (WiFi.status() != WL_CONNECTED) return;

    int numMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numMessages) {
        for (int i = 0; i < numMessages; i++) {
            String text     = bot.messages[i].text;
            String fromChat = bot.messages[i].chat_id;

            activeChatId = fromChat;   // remember the real chat ID for proactive messages

            Serial.println("TG command: " + text);
            Serial.println("Chat ID    : " + fromChat);

            // -- /start --
            if (text == "/start") {
                String reply  = "Hydration Reminder System\n\n";
                       reply += "Available commands:\n";
                       reply += "/status - current hydration data\n";
                       reply += "/goal - show current goal\n";
                       reply += "/setgoal <ml> - set new goal\n";
                       reply += "/graph - intake graph\n";
                       reply += "/reset - reset today's data\n";
                bot.sendMessage(fromChat, reply, "");
            }

            // -- /status --
            else if (text == "/status") {
                bot.sendMessage(fromChat, buildStatusMessage(), "");
            }

            // -- /goal --
            else if (text == "/goal") {
                bot.sendMessage(fromChat,
                    "Daily goal: " + String(dailyGoal) + " ml", "");
            }

            // -- /setgoal <value> --
            else if (text.startsWith("/setgoal")) {
                int spaceIdx = text.indexOf(' ');
                if (spaceIdx != -1) {
                    int newGoal = text.substring(spaceIdx + 1).toInt();
                    if (newGoal >= 100 && newGoal <= 10000) {
                        dailyGoal = newGoal;
                        saveToEEPROM();
                        bot.sendMessage(fromChat,
                            "Goal updated to " + String(dailyGoal) + " ml", "");
                    } else {
                        bot.sendMessage(fromChat,
                            "Enter a value between 100 and 10000 ml.", "");
                    }
                } else {
                    bot.sendMessage(fromChat,
                        "Usage: /setgoal 2500", "");
                }
            }

            // -- /reset --
            else if (text == "/reset") {
                consumedToday = 0;
                sipCount      = 0;
                memset(hourlyIntake, 0, sizeof(hourlyIntake));
                lastDrinkTime = millis();
                reminderSent  = false;
                saveToEEPROM();
                bot.sendMessage(fromChat,
                    "Data reset. New day started!", "");
            }

            // -- /graph --
            else if (text == "/graph") {
                String url = buildGraphURL();
                bot.sendMessage(fromChat,
                    "Intake graph:\n" + url, "");
            }

            // -- Unknown --
            else {
                bot.sendMessage(fromChat,
                    "Unknown command. Send /start for help.", "");
            }
        }
        numMessages = bot.getUpdates(bot.last_message_received + 1);
    }
}

// ============================================================
//  REMINDER LOGIC
// ============================================================
void checkReminder() {
    unsigned long now = millis();
    if ((now - lastDrinkTime) >= REMINDER_INTERVAL_MS && !reminderSent) {
        Serial.println("Reminder: No water for set interval!");
        beepReminder();
        if (WiFi.status() == WL_CONNECTED) {
            bool sent = bot.sendMessage(activeChatId,
                "Hydration Reminder\n\nYou haven't drunk water for a while!\n"
                "Please hydrate now.", "");
            Serial.println(sent ? "Reminder message sent to Telegram."
                                 : "Failed to send reminder message!");
        } else {
            Serial.println("WiFi not connected - cannot send reminder message.");
        }
        reminderSent = true;
    }
}

// ============================================================
//  CONSUMPTION LOGIC
// ============================================================
void updateConsumption() {
    int diff = previousML - currentML;   // positive = water level dropped

    Serial.printf("Diff = %d ml  (prev=%d  curr=%d)\n", diff, previousML, currentML);

    if (previousML == 0) {
        // First valid reading - just initialise, don't count
        previousML   = currentML;
        lastDrinkTime = millis();
        return;
    }

    // Ignore tiny fluctuations (sensor noise)
    if (abs(diff) < NOISE_GUARD_ML) {
        return;
    }

    // ---- SIP DETECTED: any meaningful decrease in water level ----
    if (diff > 0) {
        consumedToday += diff;
        sipCount++;
        lastDrinkTime = millis();
        reminderSent  = false;
        sipBeep();

        // Store in scaled-hour bucket (1 "hour" = REMINDER_INTERVAL_MS)
        int bucket = (int)((millis() / REMINDER_INTERVAL_MS) % 24);
        hourlyIntake[bucket] += diff;

        Serial.printf(">>> SIP #%d detected! -%d ml | Total consumed = %d ml\n",
                      sipCount, diff, consumedToday);
        saveToEEPROM();
    }

    // ---- REFILL DETECTED: water level rose significantly ----
    else if (diff <= -REFILL_THRESHOLD) {
        int rise = -diff;
        Serial.printf(">>> REFILL detected! +%d ml\n", rise);

        if (WiFi.status() == WL_CONNECTED) {
            bool sent = bot.sendMessage(activeChatId,
                "Bottle refilled! +" + String(rise) + " ml. Keep it up.", "");
            Serial.println(sent ? "Refill message sent to Telegram."
                                 : "Failed to send refill message!");
        } else {
            Serial.println("WiFi not connected - cannot send refill message.");
        }
    }

    previousML = currentML;
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    // --- Pin modes ---
    pinMode(TX_PIN,    OUTPUT);
    pinMode(LED_RED,   OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_BLUE,  OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(TX_PIN, HIGH);
    setLED(false, false, false);
    digitalWrite(BUZZER_PIN, LOW);

    // --- EEPROM ---
    EEPROM.begin(EEPROM_SIZE);
    loadFromEEPROM();

    // --- OLED ---
    Wire.begin(21, 22);
    if (!display.begin(0x3C, true)) {
        Serial.println("OLED init failed!");
        while (1) delay(1000);
    }
    display.clearDisplay();
    display.display();

    // --- Sensor filter init ---
    initSamples();
    // Warm up filter
    for (int i = 0; i < NUM_SAMPLES * 2; i++) readFilteredSensor();

    // --- First sensor read to set baseline ---
    int raw = readFilteredSensor();
    currentML  = sensorToML(raw);
    previousML = currentML;
    lastDrinkTime = millis();

    // --- WiFi ---
    connectWiFi();
    client.setInsecure();   // Skip TLS certificate verification

    // --- Telegram startup message ---
    if (WiFi.status() == WL_CONNECTED) {
        String startupMsg;
        startupMsg += "Hydration Reminder System\n\n";
        startupMsg += "System Initialized\n";
        startupMsg += "WiFi Connected\n";
        startupMsg += "OLED Ready\n";
        startupMsg += "Telegram Ready\n";
        startupMsg += "Monitoring Started\n\n";
        startupMsg += "Daily Goal: " + String(dailyGoal) + " ml\n";
        startupMsg += "Type /start to see available commands.";
        bool sent = bot.sendMessage(activeChatId, startupMsg, "");
        Serial.println(sent ? "Startup message sent to Telegram."
                             : "Failed to send startup message! (check CHAT_ID)");
    }

    startupBeep();

    // --- OLED startup screen ---
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0);
    display.println("Hydration Tracker");
    display.setCursor(0, 20);
    display.println("System");
    display.setCursor(0, 35);
    display.println("Initialized!");
    display.display();
    delay(3000);

    Serial.println("System ready.");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
    unsigned long now = millis();

    // -- 1. Read sensor --
    int raw = readFilteredSensor();
    lastRawValue = raw;
    currentML = sensorToML(raw);

    Serial.printf("Raw=%d  Level=%d ml  Prev=%d ml\n", raw, currentML, previousML);

    // -- 2. Consumption / refill logic --
    updateConsumption();

    // -- 3. OLED update every 1 s --
    if (now - lastOledMs >= 1000) {
        lastOledMs = now;
        showOLED();
    }

    // -- 4. LED update --
    updateLED();

    // -- 5. Reminder check --
    checkReminder();

    // -- 6. Telegram polling every 2 s --
    if (now - lastTelegramMs >= 2000) {
        lastTelegramMs = now;
        checkWiFiReconnect();
        handleTelegramMessages();
    }

    delay(200);   // small loop delay for stability
}
// ============================================================
//  END OF FILE
// ============================================================
