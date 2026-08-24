#ifndef CONFIG_H
#define CONFIG_H

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

#define WIFI_CONNECT_TIMEOUT_MS 20000

#endif

#define SCREENSHOT_BASE "https://YOUR-SCREENSHOT-SERVICE.a.run.app"
#define PAGE_W 272
#define PAGE_H 792
#define PAGE_BYTES (PAGE_W * PAGE_H / 8)

const char *CARD_URLS[8] = {
    "https://YOUR-WEATHER-ENDPOINT",
    "",
    "https://YOUR-CALENDAR-PAGE",
    "",
    "https://YOUR-STOCKS-PAGE",
    "",
    "https://YOUR-BIZCARD-PAGE",
    "https://YOUR-TODO-PAGE",
};

#define NEWS_URL "https://YOUR-NEWS-TEXT-ENDPOINT"
#define TODO_DB "https://YOUR-PROJECT-default-rtdb.firebaseio.com/todo"

#define CONFIG_DB "https://YOUR-PROJECT-default-rtdb.firebaseio.com/config"
