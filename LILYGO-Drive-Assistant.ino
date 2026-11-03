#include <Arduino.h>
#include <NimBLEDevice.h>
#include <LilyGo_Wristband.h>
#include <LV_Helper.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <ctype.h>

// ============================================================
// T-Glass hardware
// ============================================================

LilyGo_Class amoled;
LilyGo_Button bootPin;

#define DISPLAY_W 126
#define DISPLAY_H 126

// ============================================================
// BLE
// ============================================================

#define DEVICE_NAME      "Drive Assistant"
#define SERVICE_UUID     "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NAV_CHAR_UUID    "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define ICON_CHAR_UUID   "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define MEDIA_CHAR_UUID  "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
#define CMD_CHAR_UUID    "6e400005-b5a3-f393-e0a9-e50e24dcca9e"

// ============================================================
// Firmware / timing
// ============================================================

#define FW_VERSION "1.71-TGlass"
#define LONGPRESS_MS 700
#define MULTICLICK_GAP_MS 420
#define SLEEP_TIMEOUT_MS (10UL * 60UL * 1000UL)

// ============================================================
// RGB565 colours
// ============================================================

#define COL_BG        0x0000
#define COL_ARROW     0x368B
#define COL_DIST      0xFFFF
#define COL_STREET    0x8C71
#define COL_DIM       0x4208
#define COL_RED       0xFA27
#define COL_YELLOW    0xFE60

// ============================================================
// LVGL framebuffer
// ============================================================

lv_color_t *lvBuffer = nullptr;
lv_obj_t *canvas = nullptr;

// ============================================================
// Screens
// ============================================================

enum Screen : uint8_t {
    SCR_MAIN = 0,
    SCR_MEDIA,
    SCR_INFO,
    SCR_COUNT
};

uint8_t g_screen = SCR_MAIN;

// ============================================================
// Application state
// ============================================================

volatile bool g_connected = false;

bool g_backlight = true;
uint32_t g_lastActivity = 0;

String g_maneuver;
String g_distance;
String g_street;
String g_lastPayload;

bool g_navEnded = false;
uint32_t g_navEndedAt = 0;
#define NAV_END_SHOW_MS (30UL * 1000UL)

bool g_dirtyAll = true;
bool g_navShown = false;

// ============================================================
// Media state
// ============================================================

String g_mediaTitle;
String g_mediaArtist;
uint8_t g_mediaState = 0; // 0 none, 1 pause, 2 play
bool g_dirtyMedia = false;

NimBLECharacteristic *g_cmdChar = nullptr;

// ============================================================
// Navigation icon
// ============================================================

#define ICON_W 40
#define ICON_H 40
uint8_t g_iconBits[ICON_W * ICON_H / 8];
bool g_iconValid = false;

// ============================================================
// Button handling
// ============================================================

volatile bool g_buttonPressed = false;
volatile uint32_t g_buttonPressedAt = 0;
volatile bool g_longPressSent = false;

uint8_t g_clickCount = 0;
uint32_t g_lastReleaseAt = 0;

// ============================================================
// Basic RGB565 drawing directly into the LVGL canvas
// ============================================================

static inline void putPixel(int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= DISPLAY_W || y >= DISPLAY_H) return;
    lvBuffer[y * DISPLAY_W + x] = lv_color_make(
        ((color >> 11) & 0x1F) << 3,
        ((color >> 5) & 0x3F) << 2,
        (color & 0x1F) << 3
    );
}

static inline void fillScreen(uint16_t color) {
    lv_color_t c = lv_color_make(
        ((color >> 11) & 0x1F) << 3,
        ((color >> 5) & 0x3F) << 2,
        (color & 0x1F) << 3
    );
    for (int i = 0; i < DISPLAY_W * DISPLAY_H; ++i) lvBuffer[i] = c;
}

void fillRect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            putPixel(xx, yy, color);
}

void drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        putPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void drawCircle(int cx, int cy, int r, uint16_t color) {
    int x = -r, y = 0, err = 2 - 2 * r;
    do {
        putPixel(cx - x, cy + y, color);
        putPixel(cx - y, cy - x, color);
        putPixel(cx + x, cy - y, color);
        putPixel(cx + y, cy + x, color);
        r = err;
        if (r <= y) err += ++y * 2 + 1;
        if (r > x || err > y) err += ++x * 2 + 1;
    } while (x < 0);
}

void fillCircle(int cx, int cy, int r, uint16_t color) {
    for (int y = -r; y <= r; ++y) {
        int dx = (int)sqrt((float)(r * r - y * y));
        for (int x = -dx; x <= dx; ++x) putPixel(cx + x, cy + y, color);
    }
}

void fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, uint16_t color) {
    int minX = min(x1, min(x2, x3));
    int maxX = max(x1, max(x2, x3));
    int minY = min(y1, min(y2, y3));
    int maxY = max(y1, max(y2, y3));

    auto edge = [](int ax, int ay, int bx, int by, int px, int py) {
        return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    };

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            int e1 = edge(x1, y1, x2, y2, x, y);
            int e2 = edge(x2, y2, x3, y3, x, y);
            int e3 = edge(x3, y3, x1, y1, x, y);
            if ((e1 >= 0 && e2 >= 0 && e3 >= 0) ||
                (e1 <= 0 && e2 <= 0 && e3 <= 0)) putPixel(x, y, color);
        }
    }
}

// ============================================================
// Tiny 5x7 font
// ============================================================

const uint8_t FONT5X7[][5] PROGMEM = {
    {0,0,0,0,0}, {0,0,95,0,0}, {0,7,0,7,0}, {20,127,20,127,20},
    {36,42,127,42,18}, {35,19,8,100,98}, {54,73,85,34,80}, {0,5,3,0,0},
    {0,28,34,65,0}, {0,65,34,28,0}, {20,8,62,8,20}, {8,8,62,8,8},
    {0,80,48,0,0}, {8,8,8,8,8}, {0,96,96,0,0}, {32,16,8,4,2},
    {62,81,73,69,62}, {0,66,127,64,0}, {66,97,81,73,70}, {33,65,69,75,49},
    {24,20,18,127,16}, {39,69,69,69,57}, {60,74,73,73,48}, {1,113,9,5,3},
    {54,73,73,73,54}, {6,73,73,41,30}, {0,54,54,0,0}, {0,86,54,0,0},
    {8,20,34,65,0}, {20,20,20,20,20}, {0,65,34,20,8}, {2,1,81,9,6},
    {50,73,121,65,62}, {126,17,17,17,126}, {127,73,73,73,54}, {62,65,65,65,34},
    {127,65,65,34,28}, {127,73,73,65,65}, {127,9,9,1,1}, {62,65,65,81,115},
    {127,8,8,8,127}, {0,65,127,65,0}, {32,64,65,63,1}, {127,8,20,34,65},
    {127,64,64,64,64}, {127,2,12,2,127}, {127,4,8,16,127}, {62,65,65,65,62},
    {127,9,9,9,6}, {62,65,97,33,94}, {127,9,25,41,70}, {38,73,73,73,50},
    {1,1,127,1,1}, {63,64,64,64,63}, {31,32,64,32,31}, {127,32,24,32,127},
    {99,20,8,20,99}, {3,4,120,4,3}, {97,81,73,69,67}, {0,127,65,65,0},
    {2,4,8,16,32}, {0,65,65,127,0}, {4,2,1,2,4}, {64,64,64,64,64},
    {0,1,2,4,0}
};

int fontIndex(char c) {
    if (c >= ' ' && c <= '_') return c - ' ';
    return 0;
}

int textWidth(const String &s, int scale) {
    if (!s.length()) return 0;
    return (int)s.length() * (6 * scale) - scale;
}

void drawChar(int x, int y, char c, int scale, uint16_t color) {
    c = (char)toupper((unsigned char)c);
    int idx = fontIndex(c);
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = pgm_read_byte(&FONT5X7[idx][col]);
        for (int row = 0; row < 7; ++row)
            if (bits & (1 << row)) fillRect(x + col * scale, y + row * scale, scale, scale, color);
    }
}

void drawText(const String &s, int centerX, int topY, int scale, uint16_t color) {
    int w = textWidth(s, scale);
    int x = centerX - w / 2;
    for (size_t i = 0; i < s.length(); ++i) {
        drawChar(x, topY, s[i], scale, color);
        x += 6 * scale;
    }
}

String fitText(String s, int maxPixels, int scale) {
    while (s.length() > 1 && textWidth(s, scale) > maxPixels) {
        s.remove(s.length() - 1);
        if (textWidth(s + "...", scale) <= maxPixels) return s + "...";
    }
    return s;
}

// ============================================================
// Navigation drawing
// ============================================================

float maneuverAngle(const String &m, bool &valid) {
    valid = true;
    if (m == "left" || m == "turn-left" || m == "slight-left") return -90;
    if (m == "right" || m == "turn-right" || m == "slight-right") return 90;
    if (m == "uturn" || m == "u-turn") return 180;
    if (m == "straight" || m == "continue") return 0;
    if (m == "merge") return 45;
    valid = false;
    return 0;
}

void drawArrow(int cx, int cy, float scale, float angleDeg, uint16_t color) {
    float r = angleDeg * PI / 180.0f;
    auto rot = [&](float x, float y, int &ox, int &oy) {
        ox = (int)lround(cx + x * cos(r) - y * sin(r));
        oy = (int)lround(cy + x * sin(r) + y * cos(r));
    };
    int ax, ay, bx, by, cx1, cy1, dx, dy;
    rot(0, -30 * scale, ax, ay);
    rot(-18 * scale, -8 * scale, bx, by);
    rot(18 * scale, -8 * scale, cx1, cy1);
    fillTriangle(ax, ay, bx, by, cx1, cy1, color);
    rot(-7 * scale, -10 * scale, ax, ay);
    rot(7 * scale, -10 * scale, bx, by);
    rot(7 * scale, 28 * scale, cx1, cy1);
    rot(-7 * scale, 28 * scale, dx, dy);
    fillTriangle(ax, ay, bx, by, cx1, cy1, color);
    fillTriangle(ax, ay, cx1, cy1, dx, dy, color);
}

void drawIconFallback() {
    bool valid = false;
    float angle = maneuverAngle(g_maneuver, valid);
    if (g_maneuver == "roundabout") {
        drawCircle(63, 37, 15, COL_ARROW);
        drawArrow(63, 37, 0.28f, 60, COL_ARROW);
    } else if (g_maneuver == "arrive") {
        fillCircle(63, 37, 14, COL_ARROW);
        fillCircle(63, 37, 7, COL_BG);
        fillCircle(63, 37, 3, COL_ARROW);
    } else if (valid) {
        drawArrow(63, 37, 0.9f, angle, COL_ARROW);
    } else {
        drawText("?", 63, 27, 3, COL_ARROW);
    }
}

bool iconPx(int x, int y) {
    if (x < 0 || y < 0 || x >= ICON_W || y >= ICON_H) return false;
    int i = y * ICON_W + x;
    return g_iconBits[i >> 3] & (0x80 >> (i & 7));
}

void drawReceivedIcon() {
    const int scale = 2;
    const int w = ICON_W * scale;
    const int h = ICON_H * scale;
    int x0 = (DISPLAY_W - w) / 2;
    int y0 = 5;
    for (int y = 0; y < ICON_H; ++y)
        for (int x = 0; x < ICON_W; ++x)
            if (iconPx(x, y)) fillRect(x0 + x * scale, y0 + y * scale, scale, scale, COL_ARROW);
}

// ============================================================
// Screens
// ============================================================

void drawWaiting() {
    fillScreen(COL_BG);
    drawText("DRIVE ASSISTANT", 63, 43, 2, COL_DIST);
    drawText(g_connected ? "WAITING FOR MAPS" : "LOOKING FOR PHONE", 63, 60, 1, g_connected ? COL_ARROW : COL_STREET);
}

void drawEnded() {
    fillScreen(COL_BG);
    fillCircle(63, 40, 22, COL_ARROW);
    drawLine(53, 40, 60, 47, COL_BG);
    drawLine(60, 47, 75, 31, COL_BG);
    drawText("NAVIGATION ENDED", 63, 75, 1, COL_DIST);
}

void drawMain() {
    bool hasNav = g_maneuver.length() > 0 && g_maneuver != "clear" && g_maneuver != "end";
    if (!hasNav) {
        if (g_navEnded) drawEnded();
        else drawWaiting();
        g_navShown = false;
        return;
    }

    fillScreen(COL_BG);
    if (g_iconValid) drawReceivedIcon();
    else drawIconFallback();

    if (g_distance.length()) drawText(fitText(g_distance, 118, 2), 63, 82, 2, COL_DIST);
    if (g_street.length()) drawText(fitText(g_street, 120, 1), 63, 106, 1, COL_STREET);
    g_navShown = true;
}

void drawMedia() {
    fillScreen(COL_BG);
    drawText("MUSIC", 63, 7, 2, COL_DIST);
    if (g_mediaState == 0) {
        drawText("NOTHING PLAYING", 63, 52, 1, COL_STREET);
        return;
    }
    drawText(fitText(g_mediaTitle.length() ? g_mediaTitle : "UNKNOWN", 118, 1), 63, 32, 1, COL_DIST);
    drawText(fitText(g_mediaArtist, 118, 1), 63, 45, 1, COL_STREET);

    // Three visual button areas. The actual button action is determined by
    // short/double/triple press, so there is no touchscreen dependency.
    drawText("<<", 23, 75, 2, COL_STREET);
    drawCircle(63, 84, 14, COL_ARROW);
    if (g_mediaState == 2) {
        fillRect(58, 77, 4, 14, COL_ARROW);
        fillRect(65, 77, 4, 14, COL_ARROW);
    } else {
        fillTriangle(59, 77, 59, 91, 71, 84, COL_ARROW);
    }
    drawText(">>", 103, 75, 2, COL_STREET);
}

void drawInfo() {
    fillScreen(COL_BG);
    drawText("STATUS", 63, 8, 2, COL_DIST);
    drawText(g_connected ? "CONNECTED" : "DISCONNECTED", 63, 37, 1,
             g_connected ? COL_ARROW : COL_RED);
    drawText("T-GLASS", 63, 56, 1, COL_STREET);
    drawText("126 X 126", 63, 69, 1, COL_STREET);
    drawText(String("V") + FW_VERSION, 63, 82, 1, COL_DIM);
    drawText("1X NEXT  2X PREV", 63, 103, 1, COL_DIM);
}

void redraw() {
    if (!g_backlight) return;
    if (g_screen == SCR_MAIN) drawMain();
    else if (g_screen == SCR_MEDIA) drawMedia();
    else drawInfo();

    lv_canvas_set_buffer(canvas, lvBuffer, DISPLAY_W, DISPLAY_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(canvas);
    lv_timer_handler();

    g_dirtyAll = false;
    g_dirtyMedia = false;
}

// ============================================================
// Display power
// ============================================================

void setBacklight(bool on) {
    g_backlight = on;
    amoled.setBrightness(on ? 255 : 0);
    if (on) {
        g_dirtyAll = true;
        redraw();
    }
    Serial.printf("[UI] Display %s\n", on ? "ON" : "OFF");
}

// ============================================================
// Navigation payload
// ============================================================

void applyPayload(const String &payload) {
    if (payload == g_lastPayload) return;
    g_lastPayload = payload;

    int p1 = payload.indexOf('|');
    int p2 = (p1 >= 0) ? payload.indexOf('|', p1 + 1) : -1;

    String m, d, s;
    if (p1 >= 0) {
        m = payload.substring(0, p1);
        d = p2 >= 0 ? payload.substring(p1 + 1, p2) : payload.substring(p1 + 1);
        s = p2 >= 0 ? payload.substring(p2 + 1) : "";
    }
    m.trim(); d.trim(); s.trim();

    bool wasNav = g_maneuver.length() && g_maneuver != "clear" && g_maneuver != "end";
    bool isNav = m.length() && m != "clear" && m != "end";

    if (m == "end" && wasNav) {
        g_navEnded = true;
        g_navEndedAt = millis();
    }
    if (isNav) g_navEnded = false;

    g_maneuver = m;
    g_distance = d;
    g_street = s;
    if (!isNav) g_iconValid = false;
    g_dirtyAll = true;

    Serial.printf("[NAV] m='%s' d='%s' s='%s'\n", m.c_str(), d.c_str(), s.c_str());
    if (g_backlight) redraw();
}

// ============================================================
// Media payload
// ============================================================

void applyMediaPayload(const String &v) {
    int p1 = v.indexOf('|');
    int p2 = p1 >= 0 ? v.indexOf('|', p1 + 1) : -1;
    String st = p1 >= 0 ? v.substring(0, p1) : v;
    String ti = p2 >= 0 ? v.substring(p1 + 1, p2) : "";
    String ar = p2 >= 0 ? v.substring(p2 + 1) : "";
    ti.trim(); ar.trim();

    uint8_t state = st == "play" ? 2 : st == "pause" ? 1 : 0;
    if (state != g_mediaState || ti != g_mediaTitle || ar != g_mediaArtist) {
        g_mediaState = state;
        g_mediaTitle = ti;
        g_mediaArtist = ar;
        g_dirtyMedia = true;
        if (g_screen == SCR_MEDIA && g_backlight) redraw();
        Serial.printf("[MEDIA] state=%u '%s' - '%s'\n", state, ti.c_str(), ar.c_str());
    }
}

void sendMediaCommand(char cmd) {
    if (!g_cmdChar || !g_connected) return;
    g_cmdChar->setValue((uint8_t *)&cmd, 1);
    g_cmdChar->notify();
    Serial.printf("[MEDIA] Command '%c'\n", cmd);
}

// ============================================================
// BLE callbacks
// ============================================================

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
        g_connected = true;
        g_lastActivity = millis();
        g_dirtyAll = true;
        Serial.println("[BLE] connected");
    }

    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
        g_connected = false;
        g_lastPayload = "";
        g_maneuver = "";
        g_iconValid = false;
        g_dirtyAll = true;
        Serial.println("[BLE] disconnected -> advertising again");
        NimBLEDevice::startAdvertising();
    }
};

class NavCharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        applyPayload(String(c->getValue().c_str()));
    }
};

class IconCharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        NimBLEAttValue v = c->getValue();
        if (v.size() >= 3 && v[0] == 'I' && v[1] == 0 && v[2] == 0) {
            g_iconValid = false;
            g_dirtyAll = true;
            if (g_backlight) redraw();
            Serial.println("[NAV] icon cleared");
            return;
        }
        if (v.size() >= 3 + sizeof(g_iconBits) && v[0] == 'I' &&
            v[1] == ICON_W && v[2] == ICON_H) {
            memcpy(g_iconBits, v.data() + 3, sizeof(g_iconBits));
            g_iconValid = true;
            g_dirtyAll = true;
            if (g_backlight) redraw();
            Serial.println("[NAV] icon received");
        } else {
            Serial.printf("[NAV] Icon rejected (len=%u)\n", (unsigned)v.size());
        }
    }
};

class MediaCharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        applyMediaPayload(String(c->getValue().c_str()));
    }
};

// ============================================================
// Button logic
// ============================================================

void performButtonAction(uint8_t count) {
    if (!g_backlight) {
        setBacklight(true);
        g_lastActivity = millis();
        return;
    }

    g_lastActivity = millis();

    if (g_screen == SCR_MEDIA) {
        if (count == 1) sendMediaCommand('P');
        else if (count == 2) sendMediaCommand('N');
        else if (count >= 3) sendMediaCommand('V');
        return;
    }

    if (count == 1) {
        g_screen = (g_screen + 1) % SCR_COUNT;
    } else {
        g_screen = (g_screen + SCR_COUNT - 1) % SCR_COUNT;
    }
    g_dirtyAll = true;
    redraw();
    Serial.printf("[UI] Screen %u\n", g_screen);
}

void button_event_callback(ButtonState state) {
    if (state == BTN_PRESSED_EVENT) {
        g_buttonPressed = true;
        g_buttonPressedAt = millis();
        g_longPressSent = false;
        g_lastActivity = millis();
        return;
    }

    if (state == BTN_RELEASED_EVENT) {
        if (!g_buttonPressed) return;
        g_buttonPressed = false;

        uint32_t held = millis() - g_buttonPressedAt;
        if (held >= LONGPRESS_MS) {
            if (!g_longPressSent) {
                setBacklight(g_backlight ? false : true);
                g_longPressSent = true;
            }
            g_clickCount = 0;
            g_lastReleaseAt = 0;
            return;
        }

        g_clickCount++;
        g_lastReleaseAt = millis();
        return;
    }
}

void processButtonClicks() {
    if (g_clickCount == 0 || g_lastReleaseAt == 0) return;
    if (millis() - g_lastReleaseAt < MULTICLICK_GAP_MS) return;

    uint8_t count = g_clickCount;
    g_clickCount = 0;
    g_lastReleaseAt = 0;
    performButtonAction(count);
}

// ============================================================
// Setup
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("======================================");
    Serial.println("Drive Assistant - LILYGO T-Glass");
    Serial.println("======================================");
    Serial.println("Display: 126 x 126");

    if (!amoled.begin()) {
        Serial.println("[T-GLASS] initialisation ERROR!");
        while (true) delay(1000);
    }

    amoled.setRotation(0);
    amoled.setBrightness(255);

    // The working T-Glass examples use LilyGo_Button and BOARD_BOOT_PIN.
    bootPin.init(BOARD_BOOT_PIN);
    bootPin.setEventCallback(button_event_callback);

    beginLvglHelper(amoled);
    Serial.println("[LVGL] OK");

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    lvBuffer = (lv_color_t *)heap_caps_malloc(
        DISPLAY_W * DISPLAY_H * sizeof(lv_color_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!lvBuffer) {
        lvBuffer = (lv_color_t *)heap_caps_malloc(
            DISPLAY_W * DISPLAY_H * sizeof(lv_color_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
    if (!lvBuffer) {
        Serial.println("[LVGL] framebuffer allocation failed");
        while (true) delay(1000);
    }

    canvas = lv_canvas_create(lv_scr_act());
    if (!canvas) {
        Serial.println("[LVGL] canvas creation failed");
        while (true) delay(1000);
    }

    lv_canvas_set_buffer(canvas, lvBuffer, DISPLAY_W, DISPLAY_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(canvas, DISPLAY_W, DISPLAY_H);
    lv_obj_align(canvas, LV_ALIGN_BOTTOM_MID, -2, 0);

    fillScreen(COL_BG);
    lv_canvas_set_buffer(canvas, lvBuffer, DISPLAY_W, DISPLAY_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(canvas);
    lv_timer_handler();

    // BLE
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setMTU(256);

    NimBLEServer *bleServer = NimBLEDevice::createServer();
    bleServer->setCallbacks(new ServerCallbacks());

    NimBLEService *service = bleServer->createService(SERVICE_UUID);

    NimBLECharacteristic *navChar = service->createCharacteristic(
        NAV_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    navChar->setCallbacks(new NavCharCallbacks());

    NimBLECharacteristic *iconChar = service->createCharacteristic(
        ICON_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    iconChar->setCallbacks(new IconCharCallbacks());

    NimBLECharacteristic *mediaChar = service->createCharacteristic(
        MEDIA_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    mediaChar->setCallbacks(new MediaCharCallbacks());

    g_cmdChar = service->createCharacteristic(CMD_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
    service->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->enableScanResponse(true);

    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.setCompleteServices(NimBLEUUID(SERVICE_UUID));

    NimBLEAdvertisementData scanData;
    scanData.setName(DEVICE_NAME);

    adv->setAdvertisementData(advData);
    adv->setScanResponseData(scanData);
    NimBLEDevice::startAdvertising();

    Serial.println("[BLE] advertising as 'Drive Assistant'");
    g_lastActivity = millis();
    redraw();
}

// ============================================================
// Loop
// ============================================================

void loop() {
    // Physical T-Glass button.
    bootPin.update();
    processButtonClicks();

    if (g_buttonPressed && !g_longPressSent &&
        millis() - g_buttonPressedAt >= LONGPRESS_MS) {
        g_longPressSent = true;
        setBacklight(g_backlight ? false : true);
        g_clickCount = 0;
        g_lastReleaseAt = 0;
    }

    // Keep the device awake while BLE is connected.
    if (g_connected) g_lastActivity = millis();

    // Navigation ended screen.
    if (g_navEnded && millis() - g_navEndedAt > NAV_END_SHOW_MS) {
        g_navEnded = false;
        g_dirtyAll = true;
        if (g_backlight) redraw();
    }

    // Auto-sleep is intentionally retained from the original firmware.
    // The T-Glass side button / power switch can be used to wake/power the
    // device depending on the hardware power state.
    if (!g_connected && millis() - g_lastActivity > SLEEP_TIMEOUT_MS) {
        Serial.println("[PWR] 10 min without BLE connection -> deep sleep");
        amoled.setBrightness(0);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_BOOT_PIN, 0);
        delay(50);
        esp_deep_sleep_start();
    }

    if (g_dirtyAll && g_backlight) redraw();

    lv_timer_handler();
    delay(5);
}
