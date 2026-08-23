# Crow — an ESP32-S3 e-Paper Personal Dashboard

A pocket-sized, offline-first personal dashboard built on the **Elecrow CrowPanel 5.79" e-Paper HMI Display** (ESP32-S3-WROOM-1-N8R8, dual SSD1683, 272×792 portrait, 1-bit).

The panel is a *typographic canvas*: the UI is an Editorial-Swiss-style card grid — weather, calendar, stocks, news, ebooks, a QR contact card, and an interactive todo list — all cached in flash so the device works fully offline. Cloud-rendered HTML pages, a scrollable text news reader, an embedded ebook reader, deep sleep with button wake, and a phone-controlled todo list linked by a QR code on the panel.

```
┌─────────────────────────────┐
│ PERSONAL DASHBOARD  ED.001  │
│ bart78@                     │
│ ─────────────────────────── │
│ ┌───────┐ ┌───────┐        │
│ │WEATHER│ │  NAV  │        │  ← card grid
│ └───────┘ └───────┘        │
│ ... 8 tiles (portrait)     │
│ ┌───────────────────────┐  │
│ │ TODO · sync · wifi    │  │
│ └───────────────────────┘  │
└─────────────────────────────┘
```

## Features

- **Card-based dashboard** — 8 tiles: WEATHER · NAV · CALENDAR · NEWS · STOCKS · BOOKS · CARD · TODO. Dial switch navigates, partial-refresh selection, thin L-shaped highlight.
- **Cloud-rendered pages** — each card's content is an HTML page rendered by a headless-browser screenshot service to a 1-bit 272×792 image, decoded and cached in flash. The device never holds an API key; pages fetch their own data in the cloud.
- **Offline-first** — every page, the book, news, and todos are cached in the SPIFFS partition; the device boots, opens and reads with no network.
- **News reader** — a text endpoint (e.g. an RSS summarizer) is fetched, paginated and scrolled with the dial, position remembered.
- **Ebook reader** — plain-text books seeded to flash, wrapped and paginated on-device; dial turns pages, position persisted. One book at a time, swap by editing the seed.
- **Interactive todos** — fetch the list from a Firebase Realtime Database, dial to select, OK to toggle done — offline toggles queue up and sync when the network returns. A QR on the panel links to a phone web app for editing.
- **Sleep** — panel hibernate after every draw, deep sleep after 2 min idle, wake on any button (EXT1), NTP re-sync on wake. Built for battery life (~weeks on a 300 mAh LiPo).
- **Editorial Swiss design** — the home screen is generated with PIL (supersampled 2× typography) and blitted; the design lives in `tools/render_home.py`, not in code.

## Hardware

- [Elecrow CrowPanel ESP32 5.79" E-Paper HMI Display](https://www.elecrow.com/crowpanel-esp32-5-79-e-paper-hmi-display-with-272-792-resolution-black-white-color-driven-by-spi-interface.html)
  - ESP32-S3-WROOM-1-N8R8 (8 MB flash, 8 MB PSRAM)
  - 272×792 e-paper, 1-bit, two cascaded SSD1683 drivers
  - 5 inputs: dial (PRV/NEXT/OK) + MENU + EXIT; TF-card slot; battery header (SH1.0, TP4054 charging)

## Architecture

```
                 ┌───────────────────── cloud ─────────────────────┐
                 │                                                  │
  hosted HTML    │  headless-chrome screenshot service              │
  (weather, …) ──┼─► /?url=…&width=272&height=792&monochrome=true  │
                 │        └─► 1-bit PNG                              │
  text endpoint  │  news / book text endpoints (your own API)        │
  (news, books) ─┼─► plain text                                      │
                 │                                                  │
  Firebase RTDB  │  /todo (read + write)                             │
  (todos) ───────┼─► JSON                                            │
                 └──────────────────┬───────────────────────────────┘
                                    │
        ┌───────────────────────────▼───────────────────────────┐
        │  Device (ESP32-S3)                                     │
        │  · sync (MENU or boot): fetch pages → PNGdecode →      │
        │    1-bit → SPIFFS cache                                │
        │  · news/book text → paginate → dial-scroll readers     │
        │  · todos → local toggle → pending queue → PATCH sync   │
        │  · deep sleep between uses                             │
        └────────────────────────────────────────────────────────┘
```

### Why this design

E-paper shows a *static* image almost for free — so the device treats everything as **pre-rendered content** (pages, text) rather than live UI. HTML gives unlimited design freedom in the cloud; the panel only ever blits bitmaps. API keys live in the cloud pages, never on the device.

## Repository layout

```
firmware/     PlatformIO firmware (the shell, readers, todos, sleep)
cloud/        Firebase Functions: screenshot service, weather page,
              news text, todo page (recoverable, deployable)
web/          Static pages: todo phone app, weather/contacts samples
tools/        Design pipeline: render_home.py (UI), make_book.py (books),
              img2epd.py (image conversion)
books/        Public-domain seed book (The House on the Borderland)
```

## Getting started

### 1. Firmware

```bash
cd firmware
cp src/config.example.h src/config.h   # fill in your values
pio run -t upload
```

`config.h` holds: WiFi credentials, the screenshot-service base URL, per-card page URLs, the news text endpoint and the todo RTDB URL. `config.example.h` documents every field.

**The build stack that matters** (hard-won):

```ini
platform = espressif32@6.9.0
board = esp32-s3-devkitc-1
framework = arduino
board_build.arduino.memory_type = qio_opi
board_build.flash_mode = qio
board_build.psram_type = opi
board_upload.flash_size = 8MB
board_build.extra_flags = -DBOARD_HAS_PSRAM
lib_deps = GxEPD2, Adafruit GFX Library, PNGdec, ArduinoJson
```

- **Use GxEPD2** (`GxEPD2_579_GDEY0579T93`), not the Elecrow bit-bang driver: the bit-banged SPI + active PSRAM combination faults the flash cache on this board (cache errors / watchdog resets). GxEPD2's hardware-SPI path is stable.
- **WiFi first, display second**: connect WiFi and let the radio settle before the first panel refresh — the order the factory-style demos use.
- **SPIFFS filename limit is 31 chars** — `/the_house_on_the_borderland.txt` (32) silently fails to create; keep names short.
- **Chunked HTTP responses**: `HTTPClient::getSize()` returns −1 when the server omits Content-Length (e.g. Next.js streams) — read until EOF with a growing buffer instead of trusting the size.
- Platformio's `board_build.arduino.memory_type` resolves from the board JSON; the ini key alone silently drops PSRAM config.
- The serial console runs at 115200 on the CH340 UART bridge.

### 2. Cloud functions

```bash
cd cloud
cp .firebaserc.example .firebaserc   # your project id
firebase functions:secrets:set OWM_KEY  # weather
firebase functions:secrets:set TODO_DB  # todo page (RTDB URL)
firebase deploy --only functions
```

| Function | Purpose | Env |
|---|---|---|
| `screenshot` | HTML → 1-bit PNG (the heart of the pipeline) | `SCREENSHOT_API_KEY` (optional) |
| `weatherPage` | server-rendered weather (OpenWeatherMap, Seoul) | `OWM_KEY` |
| `newsText` | Google News RSS → plain text for the reader | — |
| `todoPage` | RTDB todos → styled page (optional; the device has its own todo UI) | `TODO_DB` |

### 3. Web

Deploy `web/` to any static host (Firebase Hosting):

```bash
firebase deploy --only hosting
```

- `todo.html` — the phone editor for the todo list. Point it at your RTDB with `?db=https://YOUR-PROJECT-default-rtdb.firebaseio.com`.
- The RTDB rules shipped in `cloud/database.rules.json`: public read, write only under `/todo`.

### 4. Todos

RTDB structure (console-editable):

```
/todo
  "milk": { "text": "Buy milk", "done": true }
  "mom":  { "text": "Call mom", "done": false }
```

Plain strings also work (`"key": "Walk the dog"` = not done). The device toggles locally (offline-queued, `PENDING` footer) and PATCHes on reconnect.

## Design pipeline

The home screen is not drawn in code — it's generated:

```bash
python3 tools/render_home.py    # regenerates firmware/src/home_base.h
pio run -t upload               # reflash to restyle
```

`render_home.py` draws the full 272×792 screen with PIL at 2× scale (clean strokes), downscales with thresholding, then the 1px rules are redrawn crisp. Typography is Helvetica Neue + a serif wordmark, supersampled for even 1-bit strokes.

Books: `tools/make_book.py` strips HTML → plain text → seeded to SPIFFS at first boot (`/house.txt`). Swap the seed in `firmware/src/book_text.h` (or point `openBook()` at any SPIFFS text file).

## Battery

Deep sleep keeps the board at ~0.2–0.5 mA. A 250–500 mAh LiPo (1.0 mm SH plug, charged via USB-C by the on-board TP4054) gives weeks: 2 syncs + light use ≈ 10–20 mAh/day.

## Roadmap

- **NAV** tile (planned as a separate project)
- Cloud-pages config in the RTDB (add/retarget cards from the phone, no reflash)
- Multi-book selection

## Credits & licenses

- Display driver: [GxEPD2](https://github.com/zinggjm/GxEPD2) (BSD-3-Clause)
- PNG decode: [PNGdec](https://github.com/bitbank2/PNGdec) (MIT)
- JSON: [ArduinoJson](https://arduinojson.org) (MIT)
- Seed book: *The House on the Borderland* by William Hope Hodgson (public domain)
- Hardware: [Elecrow](https://www.elecrow.com) CrowPanel 5.79" E-Paper — official repo: [Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792](https://github.com/Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792)

This project is MIT licensed — see [LICENSE](LICENSE).