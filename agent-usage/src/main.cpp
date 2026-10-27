#include <Arduino.h>
#include <TFT_eSPI.h>
#include <NimBLEDevice.h>

TFT_eSPI tft = TFT_eSPI();

// ============================================================
// Display
// ============================================================

constexpr uint8_t DISPLAY_ROTATION = 1;  // 320 x 170 landscape
constexpr int SCREEN_MARGIN = 12;
constexpr int USAGE_BAR_WIDTH = 180;

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

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* usageCharacteristic = nullptr;

bool deviceConnected = false;

// Restart advertising outside the disconnect callback so the NimBLE host can
// finish connection cleanup before accepting the next BlueZ connection.
constexpr uint32_t ADVERTISING_RESTART_DELAY_MS = 750;

volatile bool advertisingRestartPending = false;
volatile uint32_t disconnectedAtMs = 0;
volatile uint8_t failedConnectionCount = 0;
volatile bool receivedValidWrite = false;
volatile uint32_t connectedAtMs = 0;

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


void drawPixelPikachu(int x, int y) {
    // Derived from the Generation I Pokémon Yellow Pikachu sprite provided
    // by PokéAPI, reduced to a 24 x 24 five-colour indexed image.
    // https://github.com/PokeAPI/sprites
    constexpr int SPRITE_SIZE = 24;
    static const uint16_t PIKACHU_COLORS[] PROGMEM = {
        TFT_BLACK, 0x3186, 0xE5E9, 0xFFF3, TFT_WHITE,
    };
    // Two 4-bit palette indices are packed into each byte to keep the
    // 24 x 24 colour sprite compact in flash.
    static const uint8_t PIKACHU_PIXELS[] PROGMEM = {
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x14, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x11, 0x00,
        0x00, 0x14, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x24, 0x11, 0x00,
        0x00, 0x13, 0x44, 0x42, 0x44, 0x44, 0x42, 0x44, 0x44, 0x42, 0x20, 0x00,
        0x00, 0x00, 0x33, 0x44, 0x44, 0x44, 0x44, 0x43, 0x33, 0x10, 0x00, 0x00,
        0x00, 0x00, 0x24, 0x44, 0x44, 0x44, 0x44, 0x44, 0x32, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x24, 0x14, 0x44, 0x44, 0x41, 0x44, 0x20, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x41, 0x11, 0x44, 0x44, 0x41, 0x14, 0x40, 0x00, 0x00, 0x31,
        0x00, 0x00, 0x44, 0x14, 0x44, 0x44, 0x41, 0x44, 0x40, 0x00, 0x01, 0x41,
        0x00, 0x01, 0x22, 0x44, 0x44, 0x44, 0x44, 0x22, 0x21, 0x00, 0x34, 0x41,
        0x00, 0x02, 0x24, 0x44, 0x24, 0x24, 0x44, 0x42, 0x42, 0x03, 0x44, 0x41,
        0x00, 0x00, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x41, 0x14, 0x44, 0x41,
        0x00, 0x00, 0x24, 0x44, 0x44, 0x44, 0x44, 0x44, 0x22, 0x44, 0x44, 0x40,
        0x00, 0x00, 0x44, 0x44, 0x43, 0x34, 0x44, 0x44, 0x44, 0x44, 0x44, 0x00,
        0x00, 0x00, 0x44, 0x41, 0x44, 0x44, 0x14, 0x44, 0x44, 0x41, 0x44, 0x10,
        0x00, 0x02, 0x44, 0x44, 0x44, 0x42, 0x44, 0x14, 0x41, 0x14, 0x44, 0x00,
        0x00, 0x01, 0x42, 0x44, 0x44, 0x41, 0x44, 0x34, 0x41, 0x13, 0x20, 0x00,
        0x00, 0x01, 0x43, 0x14, 0x44, 0x42, 0x41, 0x44, 0x41, 0x23, 0x00, 0x00,
        0x00, 0x02, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x41, 0x22, 0x00, 0x00,
        0x00, 0x00, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x41, 0x10, 0x00, 0x00,
        0x00, 0x00, 0x14, 0x44, 0x44, 0x44, 0x44, 0x44, 0x30, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x02, 0x33, 0x22, 0x22, 0x23, 0x43, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x12, 0x00, 0x00, 0x00, 0x11, 0x20, 0x00, 0x00, 0x00,
    };

    for (int row = 0; row < SPRITE_SIZE; ++row) {
        for (int column = 0; column < SPRITE_SIZE; ++column) {
            const int pixel = row * SPRITE_SIZE + column;
            const uint8_t packed = pgm_read_byte(&PIKACHU_PIXELS[pixel / 2]);
            const uint8_t colorIndex =
                pixel % 2 == 0 ? packed >> 4 : packed & 0x0F;
            tft.drawPixel(
                x + column,
                y + row,
                pgm_read_word(&PIKACHU_COLORS[colorIndex])
            );
        }
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


String formatCursorRemainingRate(int usedCents, int limitCents) {
    if (usedCents < 0 || limitCents <= 0) {
        return "--%";
    }

    // Keep one decimal place so the display reports the monthly remaining
    // rate instead of only the rounded whole-percent progress-bar value.
    const int rateTenths = constrain(
        static_cast<int>(
            (static_cast<int64_t>(limitCents - usedCents) * 1000 +
             limitCents / 2) /
            limitCents
        ),
        0,
        1000
    );

    if (rateTenths % 10 == 0) {
        return String(rateTenths / 10) + "%";
    }

    return String(rateTenths / 10) + "." + String(rateTenths % 10) + "%";
}


uint16_t cursorRemainingColor(int percent) {
    if (percent <= 10) {
        return TFT_RED;
    }
    if (percent <= 30) {
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
    const int barY = y + 22;
    const int barWidth = min(USAGE_BAR_WIDTH, tft.width() - SCREEN_MARGIN * 2);

    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(labelColor, TFT_BLACK);
    tft.drawString(label, SCREEN_MARGIN, y);

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    // Long dollar values still fit by dropping to the small bitmap font.
    tft.setTextSize(value.length() > 14 ? 1 : 2);
    tft.drawString(value, tft.width() - SCREEN_MARGIN, y);

    drawProgressBar(
        SCREEN_MARGIN,
        barY,
        barWidth,
        14,
        percent < 0 ? 0 : percent,
        barColor
    );
}


void drawCursorUsageRow(
    int usedCents,
    int limitCents,
    int y,
    int percent
) {
    const int screenWidth = tft.width();
    const int barY = y + 36;
    const int barWidth = min(USAGE_BAR_WIDTH, screenWidth - SCREEN_MARGIN * 2);

    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
    tft.drawString("CURSOR MO", SCREEN_MARGIN, y);

    // Put the monthly rate on its own line so it remains as large as the
    // Codex percentage, while the dollar estimate stays readable below it.
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(
        formatCursorRemainingRate(usedCents, limitCents),
        screenWidth - SCREEN_MARGIN,
        y
    );

    tft.setTextSize(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(
        formatCursorAmount(usedCents, limitCents),
        screenWidth - SCREEN_MARGIN,
        y + 19
    );

    drawProgressBar(
        SCREEN_MARGIN,
        barY,
        barWidth,
        14,
        percent < 0 ? 0 : percent,
        cursorRemainingColor(percent < 0 ? 0 : percent)
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
    const int cursorRemainingPercent =
        cursorUsed >= 0 && cursorLimit > 0
            ? clampPercent(static_cast<int>(
                  static_cast<int64_t>(cursorLimit - cursorUsed) * 100 /
                  cursorLimit
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

    drawPixelPikachu(screenWidth - SCREEN_MARGIN - 24, 5);

    drawUsageRow(
        "CODEX 7D",
        codexRemaining < 0 ? "--%" : String(clampPercent(codexRemaining)) + "%",
        34,
        codexRemaining,
        TFT_CYAN,
        TFT_GREEN
    );

    drawCursorUsageRow(
        cursorUsed,
        cursorLimit,
        87,
        cursorRemainingPercent
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

class ServerCallbacks : public NimBLEServerCallbacks {

    void onConnect(
        NimBLEServer* server,
        NimBLEConnInfo& connection
    ) override {

        deviceConnected = true;
        advertisingRestartPending = false;
        receivedValidWrite = false;
        connectedAtMs = millis();

        Serial.printf(
            "BLE client connected; interval=%.1fms timeout=%ums latency=%u\n",
            connection.getConnInterval() * 1.25f,
            connection.getConnTimeout() * 10,
            connection.getConnLatency()
        );

        // BlueZ initially proposed only a 420 ms supervision timeout on the
        // test machine. Request a relaxed 30-50 ms interval and 6 s timeout
        // before GATT discovery so brief radio/host scheduling stalls do not
        // tear down the link.
        server->updateConnParams(
            connection.getConnHandle(),
            24,
            40,
            0,
            600
        );
        // Do not schedule a redraw here. BlueZ is about to discover GATT
        // services, and even a redraw deferred to loop() can run during that
        // discovery and starve the BLE host task. The successful write will
        // schedule DISPLAY_SYNCED shortly afterward.
    }


    void onDisconnect(
        NimBLEServer* server,
        NimBLEConnInfo& connection,
        int reason
    ) override {

        deviceConnected = false;

        Serial.printf(
            "BLE client disconnected after %ums; reason=%d (%s)\n",
            millis() - connectedAtMs,
            reason,
            NimBLEUtils::returnCodeToString(reason)
        );
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

class UsageCallbacks : public NimBLECharacteristicCallbacks {

    void onWrite(
        NimBLECharacteristic* characteristic,
        NimBLEConnInfo& connection
    ) override {

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
    NimBLEDevice::init(
        "Codex-Usage-Monitor"
    );

    // Keep the initial ATT exchange at the BLE default. The complete usage
    // payload fits in its 20-byte application payload, and avoiding an MTU
    // renegotiation makes BlueZ service discovery more reliable.
    NimBLEDevice::setMTU(23);

    // Use the ESP32-S3's maximum BLE transmit power. Connection supervision
    // timeouts are link-layer failures, so extra RF margin is more useful than
    // repeatedly restarting the application.
    NimBLEDevice::setPower(9);


    bleServer =
        NimBLEDevice::createServer();


    bleServer->setCallbacks(
        new ServerCallbacks()
    );


    NimBLEService* service =
        bleServer->createService(
            SERVICE_UUID
        );


    usageCharacteristic =
        service->createCharacteristic(
            CHARACTERISTIC_UUID,
            NIMBLE_PROPERTY::WRITE |
            NIMBLE_PROPERTY::READ
        );


    usageCharacteristic->setCallbacks(
        new UsageCallbacks()
    );


    usageCharacteristic->setValue(
        "C-;U-;L-"
    );


    NimBLEAdvertising* advertising =
        NimBLEDevice::getAdvertising();


    advertising->addServiceUUID(
        SERVICE_UUID
    );


    advertising->setName("Codex-Usage-Monitor");
    advertising->enableScanResponse(true);
    advertising->setPreferredParams(24, 40);

    advertising->start();


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

        advertisingRestartPending = false;
        NimBLEDevice::getAdvertising()->start();
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
