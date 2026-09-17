#include <Arduino.h>
#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

TFT_eSPI tft = TFT_eSPI();

// ============================================================
// Display
// ============================================================

constexpr uint8_t DISPLAY_ROTATION = 1;  // 320 x 170 landscape
constexpr int SCREEN_MARGIN = 12;

constexpr int UNKNOWN_VALUE = -1;
constexpr int MAX_CURSOR_CENTS = 999999;

int codexPercent = UNKNOWN_VALUE;
int cursorUsedCents = UNKNOWN_VALUE;
int cursorLimitCents = UNKNOWN_VALUE;

// ============================================================
// BLE
// ============================================================

// 自訂 BLE Service / Characteristic UUID
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "87654321-4321-4321-4321-cba987654321"

BLEServer* bleServer = nullptr;
BLECharacteristic* usageCharacteristic = nullptr;

bool deviceConnected = false;

// Restarting advertising from inside the BLE disconnect callback can race
// with Bluedroid's connection cleanup.  Defer it to loop() and give the host
// task a short quiet period first.
constexpr uint32_t ADVERTISING_RESTART_DELAY_MS = 750;
constexpr uint8_t MAX_FAILED_CONNECTIONS = 5;

volatile bool advertisingRestartPending = false;
volatile uint32_t disconnectedAtMs = 0;
volatile uint8_t failedConnectionCount = 0;
volatile bool receivedValidWrite = false;

enum DisplayUpdate : uint8_t {
    DISPLAY_NONE,
    DISPLAY_WAITING,
    DISPLAY_SYNCED,
};

volatile DisplayUpdate pendingDisplayUpdate = DISPLAY_NONE;


// ============================================================
// Display functions
// ============================================================

int clampPercent(int percent) {
    return constrain(percent, 0, 100);
}


void drawProgressBar(
    int x,
    int y,
    int w,
    int h,
    int percent,
    uint16_t fillColor
) {

    percent = clampPercent(percent);

    const int inset = 2;
    const int innerWidth = max(0, w - inset * 2);
    const int innerHeight = max(0, h - inset * 2);

    const int fillWidth = innerWidth * percent / 100;

    tft.drawRoundRect(
        x,
        y,
        w,
        h,
        5,
        TFT_WHITE
    );

    tft.fillRect(
        x + inset,
        y + inset,
        innerWidth,
        innerHeight,
        TFT_BLACK
    );

    if (fillWidth > 0) {

        tft.fillRoundRect(
            x + inset,
            y + inset,
            fillWidth,
            innerHeight,
            3,
            fillColor
        );
    }
}


String formatCursorAmount(int usedCents, int limitCents) {
    if (usedCents < 0 || limitCents <= 0) {
        return "$--/$--";
    }

    auto formatOne = [](int cents) -> String {
        if (cents % 100 == 0) {
            return String(cents / 100);
        }

        String amount = String(cents / 100.0f, 2);
        while (amount.endsWith("0")) {
            amount.remove(amount.length() - 1);
        }
        return amount;
    };

    return "$" + formatOne(usedCents) + "/$" + formatOne(limitCents);
}


uint16_t cursorUsageColor(int percent) {
    if (percent >= 90) {
        return TFT_RED;
    }
    if (percent >= 70) {
        return TFT_YELLOW;
    }
    return TFT_GREEN;
}


void drawUsageRow(
    const char* label,
    const String& value,
    int y,
    int percent,
    uint16_t labelColor,
    uint16_t barColor
) {
    const int screenWidth = tft.width();
    const int barY = y + 22;
    const int barWidth = screenWidth - SCREEN_MARGIN * 2;

    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(labelColor, TFT_BLACK);
    tft.drawString(label, SCREEN_MARGIN, y);

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    // Long dollar values still fit by dropping to the small bitmap font.
    tft.setTextSize(value.length() > 14 ? 1 : 2);
    tft.drawString(value, screenWidth - SCREEN_MARGIN, y);

    drawProgressBar(
        SCREEN_MARGIN,
        barY,
        barWidth,
        14,
        percent < 0 ? 0 : percent,
        barColor
    );
}


void drawDashboard(
    int codexRemaining,
    int cursorUsed,
    int cursorLimit,
    const char* connectionText,
    uint16_t connectionColor
) {
    const int screenWidth = tft.width();
    const int screenHeight = tft.height();
    const int footerY = screenHeight - 8;
    const int cursorPercent =
        cursorUsed >= 0 && cursorLimit > 0
            ? clampPercent(static_cast<int>(
                  static_cast<int64_t>(cursorUsed) * 100 / cursorLimit
              ))
            : UNKNOWN_VALUE;

    // Clear screen
    tft.fillScreen(TFT_BLACK);


    // Screen boundary
    tft.drawRect(
        0,
        0,
        screenWidth,
        screenHeight,
        TFT_DARKGREY
    );


    // Title
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("AI USAGE", SCREEN_MARGIN, 7);

    drawUsageRow(
        "CODEX 7D",
        codexRemaining < 0 ? "--%" : String(clampPercent(codexRemaining)) + "%",
        34,
        codexRemaining,
        TFT_CYAN,
        TFT_GREEN
    );

    drawUsageRow(
        "CURSOR MO",
        formatCursorAmount(cursorUsed, cursorLimit),
        87,
        cursorPercent,
        TFT_MAGENTA,
        cursorUsageColor(cursorPercent < 0 ? 0 : cursorPercent)
    );


    // Footer left
    tft.setTextSize(1);
    tft.setTextDatum(BL_DATUM);

    tft.setTextColor(
        TFT_LIGHTGREY,
        TFT_BLACK
    );

    tft.drawString(
        "T-Display-S3",
        SCREEN_MARGIN,
        footerY
    );


    // Footer right
    tft.setTextDatum(BR_DATUM);

    tft.setTextColor(
        connectionColor,
        TFT_BLACK
    );

    tft.drawString(
        connectionText,
        screenWidth - SCREEN_MARGIN,
        footerY
    );


    Serial.printf(
        "Dashboard updated: Codex=%d%% Cursor=%d/%d cents | BLE=%s\n",
        codexRemaining,
        cursorUsed,
        cursorLimit,
        deviceConnected ? "CONNECTED" : "WAITING"
    );
}


// ============================================================
// BLE Server callbacks
// ============================================================

class ServerCallbacks : public BLEServerCallbacks {

    void onConnect(BLEServer* server) override {

        deviceConnected = true;
        advertisingRestartPending = false;
        receivedValidWrite = false;

        Serial.println("BLE client connected");
        // Do not schedule a redraw here. BlueZ is about to discover GATT
        // services, and even a redraw deferred to loop() can run during that
        // discovery and starve the BLE host task. The successful write will
        // schedule DISPLAY_SYNCED shortly afterward.
    }


    void onDisconnect(BLEServer* server) override {

        deviceConnected = false;

        Serial.println("BLE client disconnected");
        pendingDisplayUpdate = DISPLAY_WAITING;

        if (receivedValidWrite) {
            failedConnectionCount = 0;
        } else if (failedConnectionCount < UINT8_MAX) {
            ++failedConnectionCount;
        }

        disconnectedAtMs = millis();
        advertisingRestartPending = true;

        Serial.printf(
            "BLE advertising restart scheduled; failed connections=%u\n",
            failedConnectionCount
        );
    }
};


// ============================================================
// BLE Characteristic callback
//
// Ubuntu writes a compact payload such as:
//
//     C87;U6000;L15000
//
// C is Codex remaining percent. U and L are Cursor used/limit cents.
// A dash means unavailable. A legacy percentage such as "87" is accepted.
// ============================================================

bool parseOptionalNumber(
    const String& text,
    int maximum,
    int& parsedValue
) {
    if (text == "-") {
        parsedValue = UNKNOWN_VALUE;
        return true;
    }

    if (text.length() == 0) {
        return false;
    }

    for (unsigned int i = 0; i < text.length(); ++i) {
        if (!isDigit(text[i])) {
            return false;
        }
    }

    const long value = text.toInt();
    if (value < 0 || value > maximum) {
        return false;
    }

    parsedValue = static_cast<int>(value);
    return true;
}


bool parseUsagePayload(
    const String& payload,
    int& parsedCodex,
    int& parsedCursorUsed,
    int& parsedCursorLimit
) {
    // Backward compatibility with the original Codex-only sender.
    bool legacyNumber = payload.length() > 0;
    for (unsigned int i = 0; i < payload.length(); ++i) {
        legacyNumber = legacyNumber && isDigit(payload[i]);
    }
    if (legacyNumber) {
        parsedCursorUsed = cursorUsedCents;
        parsedCursorLimit = cursorLimitCents;
        return parseOptionalNumber(payload, 100, parsedCodex);
    }

    const int firstSeparator = payload.indexOf(';');
    const int secondSeparator = payload.indexOf(';', firstSeparator + 1);
    if (
        firstSeparator < 0 ||
        secondSeparator < 0 ||
        payload.indexOf(';', secondSeparator + 1) >= 0
    ) {
        return false;
    }

    const String codexField = payload.substring(0, firstSeparator);
    const String usedField = payload.substring(firstSeparator + 1, secondSeparator);
    const String limitField = payload.substring(secondSeparator + 1);

    if (
        !codexField.startsWith("C") ||
        !usedField.startsWith("U") ||
        !limitField.startsWith("L")
    ) {
        return false;
    }

    return
        parseOptionalNumber(codexField.substring(1), 100, parsedCodex) &&
        parseOptionalNumber(usedField.substring(1), MAX_CURSOR_CENTS, parsedCursorUsed) &&
        parseOptionalNumber(limitField.substring(1), MAX_CURSOR_CENTS, parsedCursorLimit) &&
        !(
            (parsedCursorUsed == UNKNOWN_VALUE) !=
            (parsedCursorLimit == UNKNOWN_VALUE)
        ) &&
        (
            parsedCursorLimit == UNKNOWN_VALUE ||
            parsedCursorLimit > 0
        );
}

class UsageCallbacks : public BLECharacteristicCallbacks {

    void onWrite(BLECharacteristic* characteristic) override {

        std::string value =
            characteristic->getValue();

        if (value.empty()) {
            return;
        }


        Serial.print("BLE received: ");

        for (char c : value) {
            Serial.print(c);
        }

        Serial.println();


        // Convert the received bytes to an Arduino String.
        String received = "";

        for (char c : value) {
            received += c;
        }


        received.trim();


        int newCodex = UNKNOWN_VALUE;
        int newCursorUsed = UNKNOWN_VALUE;
        int newCursorLimit = UNKNOWN_VALUE;

        if (!parseUsagePayload(
                received,
                newCodex,
                newCursorUsed,
                newCursorLimit
            )) {
            Serial.printf("Invalid BLE usage payload: %s\n", received.c_str());
            return;
        }

        codexPercent = newCodex;
        cursorUsedCents = newCursorUsed;
        cursorLimitCents = newCursorLimit;
        receivedValidWrite = true;
        failedConnectionCount = 0;

        Serial.printf(
            "Usage: Codex=%d%% Cursor=%d/%d cents\n",
            codexPercent,
            cursorUsedCents,
            cursorLimitCents
        );
        pendingDisplayUpdate = DISPLAY_SYNCED;
    }
};


// ============================================================
// BLE setup
// ============================================================

void setupBLE() {

    Serial.println("Starting BLE...");


    // Device name visible from Ubuntu
    BLEDevice::init(
        "Codex-Usage-Monitor"
    );


    bleServer =
        BLEDevice::createServer();


    bleServer->setCallbacks(
        new ServerCallbacks()
    );


    BLEService* service =
        bleServer->createService(
            SERVICE_UUID
        );


    usageCharacteristic =
        service->createCharacteristic(
            CHARACTERISTIC_UUID,
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_READ
        );


    usageCharacteristic->setCallbacks(
        new UsageCallbacks()
    );


    usageCharacteristic->setValue(
        "C-;U-;L-"
    );


    service->start();


    BLEAdvertising* advertising =
        BLEDevice::getAdvertising();


    advertising->addServiceUUID(
        SERVICE_UUID
    );


    advertising->setScanResponse(true);


    BLEDevice::startAdvertising();


    Serial.println(
        "BLE advertising started"
    );

    Serial.println(
        "Device name: Codex-Usage-Monitor"
    );

    Serial.printf(
        "Service UUID: %s\n",
        SERVICE_UUID
    );

    Serial.printf(
        "Characteristic UUID: %s\n",
        CHARACTERISTIC_UUID
    );
}


// ============================================================
// Setup
// ============================================================

void setup() {

    Serial.begin(115200);

    delay(300);


    // -------------------------
    // Display
    // -------------------------

    tft.init();

    tft.setRotation(
        DISPLAY_ROTATION
    );


    Serial.printf(
        "T-Display-S3 initialized: rotation=%u size=%dx%d\n",
        DISPLAY_ROTATION,
        tft.width(),
        tft.height()
    );


    drawDashboard(
        codexPercent,
        cursorUsedCents,
        cursorLimitCents,
        "BLE STARTING",
        TFT_YELLOW
    );


    // -------------------------
    // BLE
    // -------------------------

    setupBLE();


    drawDashboard(
        codexPercent,
        cursorUsedCents,
        cursorLimitCents,
        "BLE WAITING",
        TFT_YELLOW
    );
}


// ============================================================
// Loop
// ============================================================

void loop() {

    if (advertisingRestartPending &&
        !deviceConnected &&
        millis() - disconnectedAtMs >= ADVERTISING_RESTART_DELAY_MS) {

        // Rebooting after several connections that never reach a valid GATT
        // write recovers a wedged BLE host without requiring a power cycle.
        if (failedConnectionCount >= MAX_FAILED_CONNECTIONS) {
            Serial.println("BLE repeatedly failed; restarting ESP32");
            delay(100);
            ESP.restart();
        }

        advertisingRestartPending = false;
        BLEDevice::startAdvertising();
        Serial.println("BLE advertising restarted");
    }

    // TFT drawing is deliberately handled on the Arduino task instead of the
    // BLE host callback. Blocking the callback can disconnect a client while
    // BlueZ is still discovering GATT services.
    const DisplayUpdate update = pendingDisplayUpdate;

    if (update != DISPLAY_NONE) {
        pendingDisplayUpdate = DISPLAY_NONE;

        if (update == DISPLAY_WAITING) {
            drawDashboard(
                codexPercent,
                cursorUsedCents,
                cursorLimitCents,
                "BLE WAITING",
                TFT_YELLOW
            );
        } else if (update == DISPLAY_SYNCED) {
            drawDashboard(
                codexPercent,
                cursorUsedCents,
                cursorLimitCents,
                "BLE SYNCED",
                TFT_GREEN
            );
        }
    }

    delay(100);
}
