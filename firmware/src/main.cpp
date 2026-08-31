#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <Preferences.h>
#include <time.h>
#include <esp32-hal-psram.h>
#include <SPIFFS.h>
#include <PNGdec.h>
#include <ArduinoJson.h>
#define ENABLE_GxEPD2_GFX 1
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>
#include "esp_sleep.h"
#include "config.h"
#include "home_base.h"
#include "placeholder.h"
#include "todo_qr.h"
#include "book_text.h"

#define PIN_SCREEN_PWR 7
#define BATTERY_PIN -1
#define BATTERY_DIVIDER 2.0f
#define PIN_HOME 2
#define PIN_EXIT 1
#define PIN_PRV 6
#define PIN_NEXT 4
#define PIN_OK 5

#define SD_MOSI 40
#define SD_MISO 13
#define SD_SCK 39
#define SD_CS 10

GxEPD2_BW<GxEPD2_579_GDEY0579T93, GxEPD2_579_GDEY0579T93::HEIGHT> display(
    GxEPD2_579_GDEY0579T93(45, 46, 47, 48));

#define NUM_CARDS 8
#define SCREEN_W 272
#define SCREEN_H 792
#define BASE_ROW_BYTES (SCREEN_W / 8)

const char *CARD_NAMES[NUM_CARDS] = {
    "WEATHER", "NAV", "CALENDAR", "NEWS", "STOCKS", "BOOKS", "CARD", "TODO"};
const int CELL_X[2] = {8, 142};
const int CELL_ROWS[4] = {120, 276, 432, 588};
const int CELL_W = 122;
const int CELL_H = 140;
const int SEL_LINE = 2;

enum Screen { SCREEN_HOME, SCREEN_CARD };
Screen screen = SCREEN_HOME;
int sel = 0;
int curCard = 0;
bool wifiOk = false;
bool pageReady[NUM_CARDS] = {false, false, false, false, false, false, false, false};

#define MAX_PAGES 1200
#define IDLE_SLEEP_MS 120000
#define MAX_TODOS 12
unsigned long lastActivity = 0;
int readerPage = 0;
int pageCount = 0;
uint16_t pageOffsets[MAX_PAGES];
const char *activeBook = NULL;
const char *newsText = NULL;
bool inNews = false;
int newsPage = 0;
char rtUrls[NUM_CARDS][256];
bool rtStatic[NUM_CARDS] = {false, false, false, false, false, false, false, false};
char rtGallery[8][256];
char rtNews[256];
#define MAX_CFG_BOOKS 4
char rtBooksTitle[MAX_CFG_BOOKS][40];
char rtBooksUrl[MAX_CFG_BOOKS][256];
int bookConfigCount = 0;
int galCount = 0;
bool galMode = false;
int galIdx = 0;
bool bookModeList = false;
int selBook = 0;
const char *activeBookUrl = NULL;

uint32_t bookKeyHash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
    {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

int readBatteryPct()
{
    if (BATTERY_PIN < 0)
        return -1;
    uint32_t mv = analogReadMilliVolts(BATTERY_PIN);
    float v = mv * BATTERY_DIVIDER / 1000.0f;
    if (v > 4.2f)
        v = 4.2f;
    if (v < 3.2f)
        return 0;
    return (int)((v - 3.2f) / 1.0f * 100.0f);
}

void saveBookPage()
{
    Preferences bp;
    bp.begin("crow", false);
    char key[16];
    snprintf(key, sizeof(key), "bp%08x", bookKeyHash(activeBookUrl ? activeBookUrl : "/house.txt"));
    bp.putInt(key, readerPage);
    bp.end();
}

int loadBookPage()
{
    Preferences bp;
    bp.begin("crow", false);
    char key[16];
    snprintf(key, sizeof(key), "bp%08x", bookKeyHash(activeBookUrl ? activeBookUrl : "/house.txt"));
    int p = bp.getInt(key, 0);
    bp.end();
    return p;
}
bool todoMode = false;
int todoSel = 0;
int todoCount = 0;
struct TodoItem
{
    char key[16];
    char text[64];
    bool done;
};
TodoItem todos[MAX_TODOS];
char todoPending[160] = "";

bool btnState[5] = {false, false, false, false, false};
unsigned long btnDownAt[5] = {0, 0, 0, 0, 0};
const int BTN_PINS[5] = {PIN_HOME, PIN_EXIT, PIN_PRV, PIN_NEXT, PIN_OK};

SPIClass SD_SPI = SPIClass(HSPI);
bool sdBootOk = false;
uint8_t *pageBuf = NULL;
PNG png;
uint16_t line565[272];

char dateStr[24] = "--------";
char reminders[3][24] = {"", "", ""};

bool btnEvent(int i)
{
    bool pressed = digitalRead(BTN_PINS[i]) == LOW;
    unsigned long now = millis();
    if (pressed && !btnState[i])
    {
        btnState[i] = true;
        btnDownAt[i] = now;
    }
    else if (!pressed && btnState[i])
    {
        btnState[i] = false;
        if (now - btnDownAt[i] >= 20)
            return true;
    }
    return false;
}

const uint8_t *baseSubRect(int x, int y)
{
    return HOME_BASE + y * BASE_ROW_BYTES + x / 8;
}

void drawSelection(int x, int y, bool selected)
{
    if (selected)
    {
        display.fillRect(x, y + CELL_H - SEL_LINE, CELL_W, SEL_LINE, GxEPD_BLACK);
        display.fillRect(x + CELL_W - SEL_LINE, y, SEL_LINE, CELL_H, GxEPD_BLACK);
    }
}

void drawHomeOverlays()
{
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(12, 96);
    display.print(dateStr);
    int bat = readBatteryPct();
    if (bat >= 0)
    {
        display.drawRect(236, 86, 24, 12, GxEPD_BLACK);
        display.fillRect(260, 89, 3, 6, GxEPD_BLACK);
        int segs = (bat + 12) / 25;
        if (segs > 0)
            display.fillRect(238, 88, 20 * segs / 4, 8, GxEPD_BLACK);
    }
    if (sdBootOk)
    {
        display.setCursor(150, 636);
        display.print(reminders[0]);
        display.setCursor(150, 654);
        display.print(reminders[1]);
        display.setCursor(150, 672);
        display.print(reminders[2]);
    }
    display.setFont(NULL);
    display.setCursor(12, 782);
    char status[40];
    uint32_t freeK = (SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1024;
    snprintf(status, sizeof(status), "WIFI:%s FS:%d.%dM", wifiOk ? "OK" : "--", freeK / 1024, (freeK % 1024) / 102);
    display.print(status);
    display.setCursor(140, 782);
    display.print("MENU:SYNC");
    display.setCursor(222, 782);
    display.print("EXIT");
}

void drawHomeFull()
{
    display.fillScreen(GxEPD_WHITE);
    display.drawInvertedBitmap(0, 0, HOME_BASE, SCREEN_W, SCREEN_H, GxEPD_BLACK);
    drawHomeOverlays();
    int x = CELL_X[sel % 2];
    int y = CELL_ROWS[sel / 2];
    drawSelection(x, y, true);
}

void updateCell(int index)
{
    int x = CELL_X[index % 2];
    int y = CELL_ROWS[index / 2];
    int pw = CELL_W + SEL_LINE;
    int ph = CELL_H + SEL_LINE;
    display.setPartialWindow(x, y, pw, ph);
    display.firstPage();
    do
    {
        display.drawInvertedBitmap(0, 0, HOME_BASE, SCREEN_W, SCREEN_H, GxEPD_BLACK);
        drawSelection(x, y, index == sel);
    } while (display.nextPage());
}

int bookTextWidth(const char *s, int len)
{
    char tmp[96];
    if (len > 95)
        len = 95;
    memcpy(tmp, s, len);
    tmp[len] = 0;
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(tmp, 0, 0, &x1, &y1, &w, &h);
    return w + x1;
}

int wrapLine(const char *text, int start, int maxW, int *endP)
{
    int i = start;
    int lastSpace = -1;
    while (text[i] && text[i] != '\n')
    {
        if (text[i] == ' ')
            lastSpace = i;
        int len = i - start + 1;
        if (len >= 90)
        {
            *endP = i;
            return len - 1;
        }
        if (bookTextWidth(text + start, len) > maxW)
        {
            if (lastSpace > start)
            {
                *endP = lastSpace;
                return lastSpace - start;
            }
            *endP = i;
            return len - 1;
        }
        i++;
    }
    *endP = i;
    return i - start;
}

void buildPageOffsets(const char *text)
{
    const int maxW = 240;
    pageCount = 0;
    int pos = 0;
    while (text[pos] && pageCount < MAX_PAGES)
    {
        pageOffsets[pageCount++] = pos;
        int lines = 0;
        while (lines < 36)
        {
            if (!text[pos])
                break;
            int endP;
            wrapLine(text, pos, maxW, &endP);
            pos = endP;
            while (text[pos] == ' ')
                pos++;
            if (text[pos] == '\n')
                pos++;
            lines++;
        }
    }
}

void renderTextPage(const char *text, int page)
{
    int maxW = 240;
    int y = 16;
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSerif12pt7b);
    display.setTextColor(GxEPD_BLACK);
    int pos = pageOffsets[page];
    int lines = 0;
    while (lines < 36)
    {
        if (!text[pos])
            break;
        int endP;
        int wlen = wrapLine(text, pos, maxW, &endP);
        char line[96];
        int n = wlen;
        if (n > 95)
            n = 95;
        memcpy(line, text + pos, n);
        line[n] = 0;
        display.setCursor(16, y);
        display.print(line);
        y += 20;
        pos = endP;
        while (text[pos] == ' ')
            pos++;
        if (text[pos] == '\n')
            pos++;
        lines++;
    }
    display.setFont(&FreeSans9pt7b);
    char pg[16];
    snprintf(pg, sizeof(pg), "%d / %d", page + 1, pageCount);
    display.setCursor(170, 780);
    display.print(pg);
    display.setCursor(12, 780);
    display.print("EXIT:BACK");
}

void drawTodoRow(int i)
{
    int y = 78 + i * 40;
    if (i < 0 || i >= todoCount)
        return;
    if (i == todoSel)
    {
        display.fillTriangle(12, y - 10, 12, y, 21, y - 5, GxEPD_BLACK);
    }
    if (todos[i].done)
    {
        display.fillRect(28, y - 13, 16, 16, GxEPD_BLACK);
    }
    else
    {
        display.drawRect(28, y - 13, 16, 16, GxEPD_BLACK);
    }
    display.setFont(&FreeSans12pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(52, y);
    display.print(todos[i].text);
}

void updateTodoRow(int i)
{
    int y = 78 + i * 40;
    display.setPartialWindow(12, y - 26, 248, 36);
    display.firstPage();
    do
    {
        drawTodoRow(i);
    } while (display.nextPage());
}

void drawCardScreen()
{
    if (curCard == 5 && bookModeList && bookConfigCount > 0)
    {
        display.fillScreen(GxEPD_WHITE);
        display.setFont(&FreeSans12pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(12, 30);
        display.print("BOOKS");
        display.drawLine(12, 42, 260, 42, GxEPD_BLACK);
        for (int i = 0; i < bookConfigCount; i++)
        {
            int y = 70 + i * 30;
            if (i == selBook)
            {
                display.fillRect(12, y - 18, 248, 26, GxEPD_BLACK);
                display.setTextColor(GxEPD_WHITE);
            }
            else
            {
                display.setTextColor(GxEPD_BLACK);
            }
            display.setCursor(18, y);
            String t = rtBooksTitle[i];
            int16_t tx1, ty1;
            uint16_t tw, th;
            while (t.length() > 1)
            {
                display.getTextBounds(t.c_str(), 0, 0, &tx1, &ty1, &tw, &th);
                if (tw <= 232)
                    break;
                t = t.substring(0, t.length() - 1);
            }
            if (t != rtBooksTitle[i])
                t += "...";
            display.print(t);
        }
        return;
    }
    if (curCard == 5 && activeBook)
    {
        renderTextPage(activeBook, readerPage);
        return;
    }
    if (curCard == 3 && inNews && newsText)
    {
        renderTextPage(newsText, readerPage);
        return;
    }
    if (curCard == 6 && galMode)
    {
        char p[24];
        snprintf(p, sizeof(p), "/gal_%02d.bin", galIdx + 1);
        File gf = SPIFFS.open(p, "rb");
        if (gf)
        {
            gf.read(pageBuf, PAGE_BYTES);
            gf.close();
            display.fillScreen(GxEPD_WHITE);
            display.drawBitmap(0, 0, pageBuf, PAGE_W, PAGE_H, GxEPD_BLACK);
        }
        else
        {
            display.fillScreen(GxEPD_WHITE);
            display.drawInvertedBitmap(0, 0, PLACEHOLDER, SCREEN_W, SCREEN_H, GxEPD_BLACK);
        }
        return;
    }
    if (curCard == 7 && todoMode)
    {
        display.fillScreen(GxEPD_WHITE);
        display.setFont(&FreeSans12pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(12, 30);
        display.print("TODO");
        display.drawLine(12, 42, 260, 42, GxEPD_BLACK);
        for (int i = 0; i < todoCount && i < 9; i++)
        {
            drawTodoRow(i);
        }
        display.drawBitmap(37, 470, TODO_QR, TODO_QR_W, TODO_QR_H, GxEPD_BLACK);
        display.setFont(&FreeSans9pt7b);
        int16_t sx1, sy1;
        uint16_t sw, sh;
        display.getTextBounds("SCAN TO EDIT", 0, 0, &sx1, &sy1, &sw, &sh);
        display.setCursor((272 - sw - sx1) / 2, 690);
        display.print("SCAN TO EDIT");
        display.setFont(&FreeSans9pt7b);
        display.setCursor(12, 780);
        display.print("EXIT:BACK");
        display.setCursor(150, 780);
        display.print(todoPending[0] ? "PENDING" : "OK:TOGGLE");
        return;
    }
    if (pageReady[curCard])
    {
        char pp[32];
        snprintf(pp, sizeof(pp), "/page_%02d.bin", curCard + 1);
        File pf = SPIFFS.open(pp, "rb");
        if (pf)
        {
            pf.read(pageBuf, PAGE_BYTES);
            pf.close();
            Serial.printf("CARD %d: cache hit\n", curCard + 1);
            display.fillScreen(GxEPD_WHITE);
            display.drawBitmap(0, 0, pageBuf, PAGE_W, PAGE_H, GxEPD_BLACK);
            return;
        }
    }
    Serial.printf("CARD %d: placeholder\n", curCard + 1);
    display.fillScreen(GxEPD_WHITE);
    display.drawInvertedBitmap(0, 0, PLACEHOLDER, SCREEN_W, SCREEN_H, GxEPD_BLACK);
}

void render()
{
    if (screen == SCREEN_HOME)
    {
        display.setFullWindow();
        display.firstPage();
        do
        {
            drawHomeFull();
        } while (display.nextPage());
    }
    else
    {
        display.setFullWindow();
        display.firstPage();
        do
        {
            drawCardScreen();
        } while (display.nextPage());
    }
    display.hibernate();
}

void sleepNow()
{
    Serial.println("Sleeping");
    SD.end();
    esp_sleep_enable_ext1_wakeup(
        (1ULL << PIN_HOME) | (1ULL << PIN_EXIT) | (1ULL << PIN_PRV) | (1ULL << PIN_NEXT) | (1ULL << PIN_OK),
        ESP_EXT1_WAKEUP_ANY_LOW);
    esp_light_sleep_start();
    Serial.println("Woken");
    if (WiFi.status() != WL_CONNECTED)
        WiFi.reconnect();
}

bool sdInit(void)
{
    const uint32_t speeds[] = {80000000, 25000000, 4000000};
    SD_SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
    for (int i = 0; i < 3; i++)
    {
        if (SD.begin(SD_CS, SD_SPI, speeds[i]))
        {
            Serial.printf("SD: mounted at %lu Hz\n", speeds[i]);
            return true;
        }
        SD.end();
    }
    return false;
}

int pngDraw(PNGDRAW *pDraw)
{
    uint16_t w = pDraw->iWidth;
    uint16_t y = pDraw->y;
    png.getLineAsRGB565(pDraw, line565, PNG_RGB565_LITTLE_ENDIAN, 0xFFFF);
    for (uint16_t x = 0; x < w; x++)
    {
        uint16_t rgb = line565[x];
        uint8_t r = (rgb >> 11) & 0x1F;
        uint8_t g = (rgb >> 5) & 0x3F;
        uint8_t b = rgb & 0x1F;
        uint32_t lum = (r * 299 + g * 587 + b * 114) / 1000;
        bool black = lum < 16;
        uint32_t bit = y * PAGE_W + x;
        if (black)
        {
            pageBuf[bit / 8] |= 0x80 >> (bit % 8);
        }
    }
    return 1;
}

bool decodePage(const uint8_t *data, size_t len)
{
    memset(pageBuf, 0, PAGE_BYTES);
    int rc = png.openRAM((uint8_t *)data, len, pngDraw);
    if (rc != PNG_SUCCESS)
    {
        Serial.printf("PNG open failed rc=%d\n", rc);
        return false;
    }
    png.decode(NULL, 0);
    png.close();
    return true;
}

void urlEncode(const char *in, char *out, size_t outLen)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const char *p = in; *p && o + 3 < outLen; p++)
    {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            out[o++] = c;
        }
        else
        {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = 0;
}

bool fetchPageUrl(int idx, const char *pageUrl, uint8_t **outBuf, size_t *outLen)
{
    char enc[768];
    urlEncode(pageUrl, enc, sizeof(enc));
    char url[1024];
    snprintf(url, sizeof(url),
             "%s/?url=%s&width=%d&height=%d&monochrome=true&dither=true",
             SCREENSHOT_BASE, enc, PAGE_W, PAGE_H);
    HTTPClient http;
    if (!http.begin(url))
        return false;
    http.setTimeout(30000);
    int code = http.GET();
    if (code != 200)
    {
        Serial.printf("Fetch failed: HTTP %d\n", code);
        http.end();
        return false;
    }
    size_t total = http.getSize();
    if (total == 0 || total > 128 * 1024)
    {
        Serial.printf("Bad length: %d\n", (int)total);
        http.end();
        return false;
    }
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf)
    {
        http.end();
        return false;
    }
    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    while (got < total)
    {
        size_t n = stream->readBytes(buf + got, 4096);
        if (n == 0)
            break;
        got += n;
    }
    http.end();
    Serial.printf("Fetched %d/%d bytes\n", (int)got, (int)total);
    if (got != total)
    {
        free(buf);
        return false;
    }
    *outBuf = buf;
    *outLen = got;
    return true;
}

char *fetchText(const char *url, int timeoutMs = 90000)
{
    HTTPClient http;
    if (!http.begin(url))
    {
        Serial.println("fetchText: begin failed");
        return NULL;
    }
    http.setTimeout(timeoutMs);
    int code = http.GET();
    Serial.printf("fetchText: GET %d\n", code);
    if (code != 200)
    {
        http.end();
        return NULL;
    }
    size_t cap = 64 * 1024;
    char *buf = (char *)ps_malloc(cap);
    if (!buf)
    {
        http.end();
        return NULL;
    }
    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    while (true)
    {
        if (got + 4096 > cap)
        {
            size_t newCap = cap * 2;
            if (newCap > 1024 * 1024)
            {
                free(buf);
                http.end();
                return NULL;
            }
            char *nb = (char *)ps_realloc(buf, newCap);
            if (!nb)
            {
                free(buf);
                http.end();
                return NULL;
            }
            buf = nb;
            cap = newCap;
        }
        size_t n = stream->readBytes((uint8_t *)buf + got, 4096);
        if (n == 0)
            break;
        got += n;
    }
    http.end();
    buf[got] = 0;
    Serial.printf("Fetched text %d bytes\n", (int)got);
    return buf;
}

char *stripCdata(char *s)
{
    char *d = s;
    while (*s)
    {
        if (strncmp(s, "<![CDATA[", 9) == 0)
            s += 9;
        else if (strncmp(s, "]]>", 3) == 0)
            s += 3;
        else
            *d++ = *s++;
    }
    *d = 0;
    return d;
}

void saveTodoPending()
{
    Preferences tp;
    tp.begin("crow", false);
    tp.putString("todoPending", todoPending);
    tp.end();
}

bool toggleTodo(int idx)
{
    if (idx < 0 || idx >= todoCount)
        return false;
    todos[idx].done = !todos[idx].done;
    char ent[24];
    snprintf(ent, sizeof(ent), "%s:%d;", todos[idx].key, todos[idx].done ? 1 : 0);
    strncat(todoPending, ent, sizeof(todoPending) - strlen(todoPending) - 1);
    saveTodoPending();
    Serial.printf("Toggle %s locally (pending)\n", todos[idx].key);
    return true;
}

void syncPendingTodos()
{
    if (WiFi.status() != WL_CONNECTED || todoPending[0] == 0)
        return;
    char work[160];
    snprintf(work, sizeof(work), "%s", todoPending);
    bool allOk = true;
    char *tok = strtok(work, ";");
    while (tok)
    {
        char *colon = strchr(tok, ':');
        if (colon)
        {
            *colon = 0;
            char key[16];
            snprintf(key, sizeof(key), "%s", tok);
            bool done = atoi(colon + 1) == 1;
            char url[96];
            snprintf(url, sizeof(url), TODO_DB "/%s.json", key);
            HTTPClient http;
            if (http.begin(url))
            {
                http.addHeader("Content-Type", "application/json");
                char payload[32];
                snprintf(payload, sizeof(payload), "{\"done\":%s}", done ? "true" : "false");
                int code = http.PATCH(payload);
                http.end();
                Serial.printf("Sync todo %s -> HTTP %d\n", key, code);
                if (code != 200)
                    allOk = false;
            }
            else
            {
                allOk = false;
            }
        }
        tok = strtok(NULL, ";");
    }
    if (allOk)
    {
        todoPending[0] = 0;
        saveTodoPending();
        Serial.println("Pending todos synced");
    }
}

bool urlChanged(int slot, const char *url)
{
    Preferences p;
    p.begin("crow", false);
    char key[8];
    snprintf(key, sizeof(key), "u%d", slot);
    String last = p.getString(key, "");
    bool changed = last != url;
    p.end();
    return changed;
}

void markFetched(int slot, const char *url)
{
    Preferences p;
    p.begin("crow", false);
    char key[8];
    snprintf(key, sizeof(key), "u%d", slot);
    p.putString(key, url);
    p.end();
}

bool urlChangedBook(int slot, const char *url)
{
    Preferences p;
    p.begin("crow", false);
    char key[8];
    snprintf(key, sizeof(key), "ub%d", slot);
    String last = p.getString(key, "");
    bool changed = last != url;
    p.end();
    return changed;
}

void markFetchedBook(int slot, const char *url)
{
    Preferences p;
    p.begin("crow", false);
    char key[8];
    snprintf(key, sizeof(key), "ub%d", slot);
    p.putString(key, url);
    p.end();
}

void applyConfig(const String &body)
{
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, body);
    if (err)
    {
        Serial.println("Config parse failed");
        return;
    }
    JsonVariant cu = doc["card_urls"];
    if (!cu.isNull())
    {
        for (int i = 0; i < NUM_CARDS; i++)
            rtUrls[i][0] = 0;
        if (cu.is<JsonArray>())
        {
            int i = 0;
            for (JsonVariant v : cu.as<JsonArray>())
            {
                if (!v.isNull() && i < NUM_CARDS)
                {
                    if (v.is<JsonObject>())
                    {
                        snprintf(rtUrls[i], 256, "%s", v["url"].as<const char *>() ? v["url"].as<const char *>() : "");
                        rtStatic[i] = v["static"] | false;
                    }
                    else
                    {
                        snprintf(rtUrls[i], 256, "%s", v.as<const char *>() ? v.as<const char *>() : "");
                        rtStatic[i] = false;
                    }
                }
                i++;
            }
        }
        else if (cu.is<JsonObject>())
        {
            for (JsonPair kv : cu.as<JsonObject>())
            {
                int i = atoi(kv.key().c_str());
                if (i >= 0 && i < NUM_CARDS)
                {
                    JsonVariant v = kv.value();
                    if (v.is<JsonObject>())
                    {
                        snprintf(rtUrls[i], 256, "%s", v["url"].as<const char *>() ? v["url"].as<const char *>() : "");
                        rtStatic[i] = v["static"] | false;
                    }
                    else
                    {
                        snprintf(rtUrls[i], 256, "%s", v.as<const char *>() ? v.as<const char *>() : "");
                        rtStatic[i] = false;
                    }
                }
            }
        }
    }
    JsonVariant books = doc["books"];
    bookConfigCount = 0;
    if (!books.isNull())
    {
        if (books.is<JsonArray>())
        {
            int slot = 0;
            for (JsonVariant v : books.as<JsonArray>())
            {
                if (bookConfigCount >= MAX_CFG_BOOKS)
                    break;
                if (v.isNull())
                    continue;
                char keyBuf[8];
                snprintf(keyBuf, sizeof(keyBuf), "%d", slot + 1);
                if (v.is<JsonObject>())
                {
                    const char *t = v["title"] | "";
                    snprintf(rtBooksTitle[bookConfigCount], 40, "%s", (t && t[0]) ? t : keyBuf);
                    snprintf(rtBooksUrl[bookConfigCount], 256, "%s", v["url"].as<const char *>() ? v["url"].as<const char *>() : "");
                }
                else
                {
                    snprintf(rtBooksTitle[bookConfigCount], 40, "%s", keyBuf);
                    snprintf(rtBooksUrl[bookConfigCount], 256, "%s", v.as<const char *>() ? v.as<const char *>() : "");
                }
                bookConfigCount++;
                slot++;
            }
        }
        else if (books.is<JsonObject>())
        {
            for (JsonPair kv : books.as<JsonObject>())
            {
                if (bookConfigCount >= MAX_CFG_BOOKS)
                    break;
                JsonVariant v = kv.value();
                if (v.is<JsonObject>())
                {
                    const char *t = v["title"] | "";
                    snprintf(rtBooksTitle[bookConfigCount], 40, "%s", (t && t[0]) ? t : kv.key().c_str());
                    snprintf(rtBooksUrl[bookConfigCount], 256, "%s", v["url"].as<const char *>() ? v["url"].as<const char *>() : "");
                }
                else
                {
                    snprintf(rtBooksTitle[bookConfigCount], 40, "%s", kv.key().c_str());
                    snprintf(rtBooksUrl[bookConfigCount], 256, "%s", v.as<const char *>() ? v.as<const char *>() : "");
                }
                bookConfigCount++;
            }
        }
    }
    JsonVariant gal = doc["gallery"];
    galCount = 0;
    if (!gal.isNull())
    {
        if (gal.is<JsonArray>())
        {
            for (JsonVariant v : gal.as<JsonArray>())
            {
                if (galCount < 8 && !v.isNull())
                    snprintf(rtGallery[galCount++], 256, "%s", v.as<const char *>() ? v.as<const char *>() : "");
            }
        }
        else if (gal.is<JsonObject>())
        {
            for (JsonPair kv : gal.as<JsonObject>())
            {
                if (galCount < 8)
                    snprintf(rtGallery[galCount++], 256, "%s", kv.value().as<const char *>() ? kv.value().as<const char *>() : "");
            }
        }
    }
    const char *nu = doc["news_url"] | "";
    if (nu && nu[0])
        snprintf(rtNews, 256, "%s", nu);
    if (rtUrls[3][0])
        snprintf(rtNews, 256, "%s", rtUrls[3]);
    int nCards = 0;
    for (int i = 0; i < NUM_CARDS; i++)
        if (rtUrls[i][0])
            nCards++;
    Serial.printf("Config loaded: %d cards, %d gallery, %d books\n", nCards, galCount, bookConfigCount);
}

void fetchConfig()
{
    HTTPClient http;
    if (!http.begin(CONFIG_DB ".json"))
        return;
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200)
    {
        http.end();
        return;
    }
    String body = http.getString();
    http.end();
    File cf = SPIFFS.open("/config.json", "w");
    if (cf)
    {
        cf.print(body);
        cf.close();
    }
    applyConfig(body);
}

void loadConfigCache()
{
    File cf = SPIFFS.open("/config.json", "r");
    if (!cf)
        return;
    String body = cf.readString();
    cf.close();
    if (body.length() > 8)
        applyConfig(body);
}

bool parseTodoBody(const String &body)
{
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, body);
    if (err)
    {
        Serial.println("Todo parse failed");
        return false;
    }
    todoCount = 0;
    if (doc.is<JsonArray>())
    {
        int arrIdx = 0;
        for (JsonVariant v : doc.as<JsonArray>())
        {
            if (!v.isNull() && todoCount < MAX_TODOS)
            {
                snprintf(todos[todoCount].key, 16, "%d", arrIdx);
                const char *txt = v["text"] | "";
                snprintf(todos[todoCount].text, 64, "%s", txt ? txt : "");
                todos[todoCount].done = v["done"] | false;
                todoCount++;
            }
            arrIdx++;
        }
    }
    else if (doc.is<JsonObject>())
    {
        for (JsonPair kv : doc.as<JsonObject>())
        {
            if (todoCount >= MAX_TODOS)
                break;
            snprintf(todos[todoCount].key, 16, "%s", kv.key().c_str());
            JsonVariant v = kv.value();
            const char *txt = v["text"] | "";
            snprintf(todos[todoCount].text, 64, "%s", txt ? txt : "");
            todos[todoCount].done = v["done"] | false;
            todoCount++;
        }
    }
    Serial.printf("Todos loaded: %d\n", todoCount);
    return todoCount > 0;
}

void syncPendingTodos();

bool fetchTodos()
{
    syncPendingTodos();
    HTTPClient http;
    if (!http.begin(TODO_DB ".json"))
        return false;
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200)
    {
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();
    File cf = SPIFFS.open("/todos.txt", "w");
    if (cf)
    {
        cf.print(body);
        cf.close();
    }
    return parseTodoBody(body);
}

bool openBook(const char *path)
{
    File f = SPIFFS.open(path, "r");
    if (!f)
        return false;
    size_t sz = f.size();
    f.close();
    if (activeBook)
    {
        free((void *)activeBook);
        activeBook = NULL;
    }
    activeBook = (const char *)ps_malloc(sz + 1);
    if (!activeBook)
        return false;
    File rf = SPIFFS.open(path, "r");
    rf.read((uint8_t *)activeBook, sz);
    rf.close();
    ((char *)activeBook)[sz] = 0;
    buildPageOffsets(activeBook);
    return true;
}

void showSyncModal(const char *msg, int cur, int total)
{
    display.setPartialWindow(56, 368, 160, 68);
    display.firstPage();
    do
    {
        drawHomeFull();
        display.fillRect(56, 368, 160, 68, GxEPD_WHITE);
        display.drawRect(56, 368, 160, 68, GxEPD_BLACK);
        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(74, 400);
        display.print(msg);
        if (total > 0)
        {
            char prog[16];
            snprintf(prog, sizeof(prog), "%d/%d", cur, total);
            display.setCursor(74, 418);
            display.print(prog);
        }
    } while (display.nextPage());
}

void syncAll()
{
    syncPendingTodos();
    if (WiFi.status() == WL_CONNECTED)
    {
        fetchConfig();
    }
    int total = 0;
    for (int i = 0; i < NUM_CARDS; i++)
    {
        if (rtUrls[i][0])
            total++;
    }
    total += galCount + bookConfigCount;
    if (total == 0)
    {
        Serial.println("Sync: no urls");
        return;
    }
    Serial.printf("Sync all: %d cards + %d gallery + %d books\n", total - galCount - bookConfigCount, galCount, bookConfigCount);
    int done = 0;
    for (int i = 0; i < NUM_CARDS; i++)
    {
        if (!rtUrls[i][0])
            continue;
        if (i == 3 || i == 5)
            continue;
        done++;
        if (rtStatic[i] && !urlChanged(i, rtUrls[i]))
        {
            Serial.printf("Card %d static, unchanged\n", i + 1);
            continue;
        }
        showSyncModal("SYNCING", done, total);
        if (WiFi.status() != WL_CONNECTED)
        {
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            unsigned long t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000)
            {
                delay(200);
            }
        }
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("Sync: offline");
            showSyncModal("OFFLINE", done, total);
            delay(1500);
            render();
            return;
        }
        uint8_t *pngData = NULL;
        size_t pngLen = 0;
        if (!fetchPageUrl(i, rtUrls[i], &pngData, &pngLen))
        {
            showSyncModal("FAILED", done, total);
            delay(800);
            continue;
        }
        bool ok = decodePage(pngData, pngLen);
        free(pngData);
        if (ok)
        {
            char p[32];
            snprintf(p, sizeof(p), "/page_%02d.bin", i + 1);
            File pf = SPIFFS.open(p, "wb");
            if (pf)
            {
                pf.write(pageBuf, PAGE_BYTES);
                pf.close();
                pageReady[i] = true;
                Serial.printf("LS cache write %d ok\n", i + 1);
            }
            else
            {
                Serial.printf("LS cache write %d FAILED\n", i + 1);
            }
            markFetched(i, rtUrls[i]);
            Serial.printf("Card %d synced\n", i + 1);
        }
    }
    for (int g = 0; g < galCount; g++)
    {
        done++;
        if (WiFi.status() != WL_CONNECTED)
        {
            showSyncModal("OFFLINE", done, total);
            delay(1200);
            break;
        }
        showSyncModal("SYNCING", done, total);
        char path[32];
        snprintf(path, sizeof(path), "/gal_%02d.bin", g + 1);
        uint8_t *pngData = NULL;
        size_t pngLen = 0;
        if (!fetchPageUrl(100 + g, rtGallery[g], &pngData, &pngLen))
        {
            showSyncModal("FAILED", done, total);
            delay(800);
            Serial.printf("Gallery %d fetch FAILED\n", g + 1);
            continue;
        }
        bool ok = decodePage(pngData, pngLen);
        free(pngData);
        if (ok)
        {
            File gf = SPIFFS.open(path, "wb");
            if (gf)
            {
                gf.write(pageBuf, PAGE_BYTES);
                gf.close();
                Serial.printf("Gallery %d synced\n", g + 1);
            }
            else
            {
                Serial.printf("Gallery %d write FAILED\n", g + 1);
            }
        }
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        char *fresh = fetchText(NEWS_URL, 20000);
        if (fresh)
        {
            stripCdata(fresh);
            if (newsText)
                free((void *)newsText);
            newsText = fresh;
            File nf = SPIFFS.open("/news.txt", "w");
            if (nf)
            {
                nf.write((const uint8_t *)newsText, strlen(newsText));
                nf.close();
            }
            Serial.println("News text synced");
        }
    }
    for (int b = 0; b < bookConfigCount; b++)
    {
        done++;
        if (WiFi.status() != WL_CONNECTED)
        {
            showSyncModal("OFFLINE", done, total);
            delay(1200);
            break;
        }
        showSyncModal("SYNCING", done, total);
        char path[32];
        snprintf(path, sizeof(path), "/book_%d.txt", b + 1);
        if (!urlChangedBook(b, rtBooksUrl[b]) && SPIFFS.exists(path))
        {
            Serial.printf("Book %d unchanged\n", b + 1);
            continue;
        }
        char *fresh = fetchText(rtBooksUrl[b]);
        if (fresh)
        {
            stripCdata(fresh);
            File nf = SPIFFS.open(path, "w");
            if (nf)
            {
                nf.write((const uint8_t *)fresh, strlen(fresh));
                nf.close();
                markFetchedBook(b, rtBooksUrl[b]);
                Serial.printf("Book %d synced\n", b + 1);
            }
            else
            {
                Serial.printf("Book %d write FAILED\n", b + 1);
            }
            free((void *)fresh);
        }
        else
        {
            showSyncModal("FAILED", done, total);
            delay(800);
            Serial.printf("Book %d fetch FAILED\n", b + 1);
        }
    }
    showSyncModal("DONE", total, total);
    delay(1200);
    render();
    Serial.println("Sync all done");
}

void setup()
{
    Serial.begin(115200);
    pinMode(PIN_SCREEN_PWR, OUTPUT);
    digitalWrite(PIN_SCREEN_PWR, HIGH);
    for (int i = 0; i < 5; i++)
    {
        pinMode(BTN_PINS[i], INPUT_PULLUP);
    }

    pageBuf = (uint8_t *)malloc(PAGE_BYTES);

    for (int i = 0; i < NUM_CARDS; i++)
    {
        snprintf(rtUrls[i], 256, "%s", CARD_URLS[i]);
    }
    snprintf(rtNews, 256, "%s", NEWS_URL);

    bool lsOk = SPIFFS.begin(true);
    Serial.printf("SPIFFS: %s\n", lsOk ? "ok" : "FAIL");

    if (!SPIFFS.exists("/house.txt"))
    {
        File bf = SPIFFS.open("/house.txt", "w");
        if (bf)
        {
            bf.write(BOOK_TEXT, BOOK_TEXT_LEN);
            bf.close();
            Serial.println("Book seeded");
        }
    }
    if (!SPIFFS.exists("/book_1.txt"))
    {
        File bf = SPIFFS.open("/book_1.txt", "w");
        if (bf)
        {
            bf.write(BOOK_TEXT, BOOK_TEXT_LEN);
            bf.close();
            Serial.println("Book 1 seeded");
        }
    }
    if (SPIFFS.exists("/house.txt"))
    {
        Serial.println("Book available");
    }

    for (int i = 0; i < NUM_CARDS; i++)
    {
        char p[32];
        snprintf(p, sizeof(p), "/page_%02d.bin", i + 1);
        if (SPIFFS.exists(p))
        {
            pageReady[i] = true;
            Serial.printf("Cached page: %d\n", i + 1);
        }
    }
    for (int g = 1; g <= 8; g++)
    {
        char p[24];
        snprintf(p, sizeof(p), "/gal_%02d.bin", g);
        if (SPIFFS.exists(p))
        {
            galCount = g;
        }
    }
    if (galCount > 0)
    {
        Serial.printf("Gallery cache: %d items\n", galCount);
    }

    Preferences tprefs;
    tprefs.begin("crow", false);
    String pend = tprefs.getString("todoPending", "");
    if (pend.length() > 0)
    {
        snprintf(todoPending, sizeof(todoPending), "%s", pend.c_str());
    }
    tprefs.end();
    if (SPIFFS.exists("/todos.txt"))
    {
        File tf = SPIFFS.open("/todos.txt", "r");
        if (tf)
        {
            String body = tf.readString();
            tf.close();
            parseTodoBody(body);
            Serial.println("Cached todos loaded");
        }
    }
    if (SPIFFS.exists("/news.txt"))
    {
        File nf = SPIFFS.open("/news.txt", "r");
        if (nf)
        {
            size_t sz = nf.size();
            nf.close();
            newsText = (const char *)ps_malloc(sz + 1);
            if (newsText)
            {
                File rf = SPIFFS.open("/news.txt", "r");
                rf.read((uint8_t *)newsText, sz);
                rf.close();
                ((char *)newsText)[sz] = 0;
                Serial.println("News cache loaded");
            }
        }
    }

    sdBootOk = sdInit();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000)
    {
        delay(200);
    }
    wifiOk = WiFi.status() == WL_CONNECTED;
    Serial.printf("WiFi: %s\n", wifiOk ? "ok" : "no");
    if (wifiOk)
    {
        configTime(9 * 3600, 0, "pool.ntp.org");
        unsigned long n0 = millis();
        while (time(nullptr) < 100000 && millis() - n0 < 5000)
        {
            delay(200);
        }
        struct tm tmInfo;
        time_t nowT = time(nullptr);
        if (localtime_r(&nowT, &tmInfo))
        {
            snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%d", tmInfo.tm_mon + 1, tmInfo.tm_mday, tmInfo.tm_year + 1900);
        }
    }
    Preferences prefs;
    prefs.begin("crow", false);
    String lastDate = prefs.getString("lastDate", "");
    if (strcmp(dateStr, "--------") == 0 && lastDate.length() > 0)
    {
        snprintf(dateStr, sizeof(dateStr), "%s", lastDate.c_str());
    }
    else if (strcmp(dateStr, "--------") != 0)
    {
        prefs.putString("lastDate", dateStr);
    }
    prefs.end();
    if (sdBootOk)
    {
        File rf = SD.open("/reminders.txt");
        if (rf)
        {
            int n = 0;
            while (rf.available() && n < 3)
            {
                String s = rf.readStringUntil('\n');
                s.trim();
                if (s.length() > 0)
                {
                    snprintf(reminders[n], sizeof(reminders[n]), "%s", s.c_str());
                    n++;
                }
            }
            rf.close();
        }
    }

    SPI.begin(12, -1, 11, 45);
    display.init(115200, true, 2, false);
    display.setRotation(1);
    display.fillScreen(GxEPD_WHITE);
    display.display();

    render();
    Serial.println("Shell booted");
    if (wifiOk)
    {
        fetchConfig();
        unsigned long lastSyncT = 0;
        Preferences sp;
        sp.begin("crow", false);
        lastSyncT = sp.getLong("lastSyncT", 0);
        sp.end();
        if (millis() - lastSyncT > 600000)
        {
            Serial.println("Auto-sync on boot");
            syncAll();
            sp.begin("crow", false);
            sp.putLong("lastSyncT", millis());
            sp.end();
        }
        else
        {
            Serial.println("Sync skipped (recent)");
        }
    }
    else
    {
        loadConfigCache();
        Serial.println("Offline boot: config from cache");
    }
}

void loop()
{
    bool anyActivity = false;
    if (btnEvent(0))
    {
        anyActivity = true;
        Serial.println("EV HOME");
        if (screen == SCREEN_CARD)
        {
            activeBook = NULL;
            inNews = false;
            todoMode = false;
            galMode = false;
            bookModeList = false;
            screen = SCREEN_HOME;
            render();
        }
        else if (!sdBootOk && sdInit())
        {
            sdBootOk = true;
            File rf = SD.open("/reminders.txt");
            if (rf)
            {
                int n = 0;
                while (rf.available() && n < 3)
                {
                    String s = rf.readStringUntil('\n');
                    s.trim();
                    if (s.length() > 0)
                    {
                        snprintf(reminders[n], sizeof(reminders[n]), "%s", s.c_str());
                        n++;
                    }
                }
                rf.close();
            }
            render();
        }
        else
        {
            syncAll();
        }
    }
    if (btnEvent(1))
    {
        anyActivity = true;
        Serial.println("EV EXIT");
        if (screen == SCREEN_CARD)
        {
            if (curCard == 5 && activeBook)
            {
                activeBook = NULL;
                bookModeList = true;
                render();
            }
            else if (curCard == 5 && bookModeList)
            {
                bookModeList = false;
                screen = SCREEN_HOME;
                render();
            }
            else if (curCard == 6 && galMode)
            {
                galMode = false;
                screen = SCREEN_HOME;
                render();
            }
            else if (curCard == 3 && inNews)
            {
                inNews = false;
                screen = SCREEN_HOME;
                render();
            }
            else if (curCard == 7 && todoMode)
            {
                todoMode = false;
                screen = SCREEN_HOME;
                render();
            }
            else
            {
                screen = SCREEN_HOME;
                render();
            }
        }
    }
    if (btnEvent(2))
    {
        anyActivity = true;
        Serial.println("EV PRV");
        if (screen == SCREEN_HOME)
        {
            int old = sel;
            sel = (sel + NUM_CARDS - 1) % NUM_CARDS;
            updateCell(old);
            updateCell(sel);
        }
        else if (screen == SCREEN_CARD && curCard == 5 && activeBook && readerPage > 0)
        {
            readerPage--;
            saveBookPage();
            render();
        }
        else if (screen == SCREEN_CARD && curCard == 5 && bookModeList && selBook > 0)
        {
            selBook--;
            render();
        }
        else if (screen == SCREEN_CARD && curCard == 6 && galMode && galIdx > 0)
        {
            galIdx--;
            render();
        }
        else if (screen == SCREEN_CARD && curCard == 7 && todoMode && todoSel > 0)
        {
            int old = todoSel;
            todoSel--;
            updateTodoRow(old);
            updateTodoRow(todoSel);
        }
        else if (screen == SCREEN_CARD && curCard == 3 && inNews && readerPage > 0)
        {
            readerPage--;
            newsPage = readerPage;
            render();
        }
    }
    if (btnEvent(3))
    {
        anyActivity = true;
        Serial.println("EV NEXT");
        if (screen == SCREEN_HOME)
        {
            int old = sel;
            sel = (sel + 1) % NUM_CARDS;
            updateCell(old);
            updateCell(sel);
        }
        else if (screen == SCREEN_CARD && curCard == 5 && activeBook && readerPage < pageCount - 1)
        {
            readerPage++;
            saveBookPage();
            render();
        }
        else if (screen == SCREEN_CARD && curCard == 5 && bookModeList && selBook < bookConfigCount - 1)
        {
            selBook++;
            render();
        }
        else if (screen == SCREEN_CARD && curCard == 6 && galMode && galIdx < galCount - 1)
        {
            galIdx++;
            render();
        }
        else if (screen == SCREEN_CARD && curCard == 7 && todoMode && todoSel < todoCount - 1)
        {
            int old = todoSel;
            todoSel++;
            updateTodoRow(old);
            updateTodoRow(todoSel);
        }
        else if (screen == SCREEN_CARD && curCard == 3 && inNews && readerPage < pageCount - 1)
        {
            readerPage++;
            newsPage = readerPage;
            render();
        }
    }
    if (btnEvent(4))
    {
        anyActivity = true;
        Serial.println("EV OK");
        if (screen == SCREEN_HOME)
        {
            curCard = sel;
            screen = SCREEN_CARD;
            if (curCard == 6 && galCount > 0)
            {
                galIdx = 0;
                galMode = true;
                Serial.printf("Gallery open: %d items\n", galCount);
            }
            else if (curCard == 7)
            {
                todoMode = false;
                if (WiFi.status() == WL_CONNECTED)
                {
                    if (fetchTodos())
                    {
                        todoSel = 0;
                        todoMode = true;
                        Serial.println("Todos open");
                    }
                    else
                    {
                        Serial.println("Todos unavailable");
                    }
                }
                else if (todoCount > 0)
                {
                    todoSel = 0;
                    todoMode = true;
                    Serial.println("Todos open (cached)");
                }
            }
            else if (curCard == 5)
            {
                if (bookConfigCount > 0)
                {
                    bookModeList = true;
                    selBook = 0;
                    Serial.printf("Books list: %d\n", bookConfigCount);
                }
                else if (openBook("/house.txt"))
                {
                    activeBookUrl = NULL;
                    readerPage = loadBookPage();
                    if (readerPage >= pageCount)
                        readerPage = 0;
                    Serial.printf("Book open: %d pages, at %d\n", pageCount, readerPage + 1);
                }
            }
            else if (curCard == 3)
            {
                inNews = false;
                if (WiFi.status() == WL_CONNECTED)
                {
                    char *fresh = fetchText(NEWS_URL, 12000);
                    if (fresh)
                    {
                        stripCdata(fresh);
                        if (newsText)
                            free((void *)newsText);
                        newsText = fresh;
                        File nf = SPIFFS.open("/news.txt", "w");
                        if (nf)
                        {
                            nf.write((const uint8_t *)newsText, strlen(newsText));
                            nf.close();
                        }
                    }
                }
                if (newsText)
                {
                    buildPageOffsets(newsText);
                    inNews = true;
                    readerPage = newsPage;
                    if (readerPage >= pageCount)
                        readerPage = 0;
                    Serial.printf("News open: %d pages\n", pageCount);
                }
                else
                {
                    Serial.println("News unavailable");
                }
            }
            render();
        }
        else if (screen == SCREEN_CARD && curCard == 5 && bookModeList)
        {
            char path[32];
            snprintf(path, sizeof(path), "/book_%d.txt", selBook + 1);
            if (openBook(path))
            {
                activeBookUrl = rtBooksUrl[selBook];
                readerPage = loadBookPage();
                if (readerPage >= pageCount)
                    readerPage = 0;
                bookModeList = false;
                Serial.printf("Book open: %s (%d pages, at %d)\n", rtBooksTitle[selBook], pageCount, readerPage + 1);
            }
            else
            {
                Serial.println("Book unavailable");
            }
            render();
        }
        else if (screen == SCREEN_CARD && curCard == 7 && todoMode)
        {
            if (toggleTodo(todoSel))
            {
                updateTodoRow(todoSel);
            }
        }
    }
    if (anyActivity)
    {
        lastActivity = millis();
    }
    delay(2);
    if (millis() - lastActivity > IDLE_SLEEP_MS)
    {
        sleepNow();
    }
}