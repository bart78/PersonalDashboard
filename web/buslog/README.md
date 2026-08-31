# BusLog — Seongnam stop

Offline-first PWA for manually logging bus arrivals at one bus stop
(Seongnam/Gyeonggi, Korea). Routes: 32, 73, 310, 340, 4103, 9409, 9507.

This is a **data-capture companion** to the ESP32 signboard, which already logs
arrivals automatically from the live feed. Use this app to capture ground truth
the feed misses — blind spots near the origin, delayed buses, afternoon
coverage gaps. One tap per bus as it physically passes the stop.

Everything is KST (`Asia/Seoul`), derived with `Intl.DateTimeFormat(..., { timeZone: 'Asia/Seoul' })`
— never the device's local timezone. Korea has no DST, so the offset is always
`+09:00`, but the app still computes KST explicitly.

## Files

| File | Purpose |
|---|---|
| `index.html` | The whole app (single file, no build step) |
| `manifest.webmanifest` | PWA manifest (installable) |
| `sw.js` | Service worker — precaches the app for full offline use |
| `icons/icon-192.png`, `icons/icon-512.png` | Install icons |
| `make_icons.py` | Regenerates the icons (stdlib-only, run `python3 make_icons.py`) |
| `README.md` | This file |

## Features

- Large KST clock (updates every second) + big 2-column route grid, one-handed
  in the dark (haptic `navigator.vibrate` + button flash + toast on each tap).
- **Undo** removes the last logged entry — each tap is one bus, so a mis-tap
  is fixed by undoing it.
- Optional **note per tap** ("crowded", "not my bus", …): type into the note
  field (or tap a chip) *before* tapping a route; it is attached to that tap
  and cleared. Stored in the `note` CSV column.
- **Coverage view**: heatmap of capture coverage — rows = routes, columns =
  30-minute buckets from 04:00 to 24:00, cell intensity = number of distinct
  days with ≥1 logged arrival in that bucket (legend: min/max). A
  **Days / Weekday days** counter per route. Use it to find the holes (e.g. an
  afternoon gap) and focus manual capture there.
- **Export CSV** (below) and **Delete all data** (with confirmation), both
  fully offline.
- Data lives in `localStorage` under key `buslog.v1` (versioned); nothing
  leaves the phone.

## Run it on a phone

**Dev / quick check** (same Wi-Fi as the phone):

```sh
cd web/buslog
python3 -m http.server 8000
# phone: http://<your-mac-lan-ip>:8000
```

Note: over plain `http://` on a LAN IP, browsers won't register the service
worker (secure-context rule) — the app still works fully, it just can't be
*installed* that way.

**Real use** — deploy this project's existing Firebase Hosting (public dir is
`web/`):

```sh
firebase deploy --only hosting
# → https://crow-c66ca.web.app/buslog/
```

That's an HTTPS origin, so the PWA install prompt ("Add to Home Screen" /
Install) works and the app is fully offline afterward.

## Export format (data contract — exact)

One **Export CSV** button → downloads `buslog_<KST-date>.csv`, generated
locally. Columns, in this exact order:

```
route,date,iso8601_kst,minute_of_day,note
32,2026-08-24,2026-08-24T18:42:00+09:00,1122,
```

- `route` — route number, as logged.
- `date` — KST calendar date `YYYY-MM-DD`. **Required** for the learner's
  weekday-vs-weekend/holiday bucketing (which happens downstream from the
  date).
- `iso8601_kst` — full KST timestamp with `+09:00` offset, minute precision,
  no milliseconds: `2026-08-24T18:42:00+09:00`.
- `minute_of_day` — minutes since midnight KST, `0`–`1439` (18:42 → `1122`).
- `note` — free text; **empty string** when unused. Always quoted only when it
  contains a comma/quote/newline (standard CSV escaping).

Rows are sorted chronologically by KST time; header row always included. The
existing Python tools (`csv` module with `utf-8`; parse `date` with
`datetime.fromisoformat`) can consume it directly. The export works offline —
it's a local `Blob` download (or share/save dialog).

## Heatmap caveat (important)

The heatmap counts **weekdays only** (Mon–Fri by the KST date). Weekends and
public holidays are excluded — the learner buckets day-type separately, and
weekend data is too sparse to be meaningful yet.

**A Korean public holiday falling on a weekday will be mis-bucketed as a
weekday** in the heatmap until a Korean holiday list is added to the app. The
CSV is unaffected: the learner already owns the holiday list and buckets the
raw rows correctly; only the in-app heatmap has this blind spot.
