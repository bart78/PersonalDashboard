const { onRequest } = require("firebase-functions/v2/https");

const OWM_KEY = process.env.OWM_KEY || "";
if (!OWM_KEY) throw new Error("OWM_KEY env var required");

function esc(s) {
  return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;");
}

function page(cur, fc) {
  const now = Math.floor(Date.now() / 1000);
  const items = (fc.list || []).filter((x) => x.dt >= now).slice(0, 4);
  const fcRows = items.map((x) => {
    const h = new Date((x.dt + 9 * 3600) * 1000).getUTCHours();
    return `<div class="fcrow"><span class="fc-t">${String(h).padStart(2, "0")}:00</span><span class="fc-c">${esc(x.weather[0].main)}</span><span class="fc-d">${Math.round(x.main.temp)}&deg;</span></div>`;
  }).join("");
  const t = new Date();
  const time = String(t.getHours()).padStart(2, "0") + ":" + String(t.getMinutes()).padStart(2, "0");
  return `<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
* { margin:0; padding:0; box-sizing:border-box; }
html, body { width:272px; height:792px; background:#fff; color:#000; }
body { font-family:"Helvetica Neue", Helvetica, Arial, sans-serif; padding:14px; }
.topline { font-size:9px; letter-spacing:3px; text-transform:uppercase; display:flex; justify-content:space-between; }
.rule { border-top:2px solid #000; margin:12px 0; }
.temp { font-family:Georgia, serif; font-size:120px; font-weight:bold; margin-top:10px; line-height:1; }
.cond { font-size:15px; letter-spacing:3px; text-transform:uppercase; margin-top:8px; }
.detail { font-size:13px; letter-spacing:1px; margin-top:14px; line-height:1.8; }
.fhead { font-size:10px; letter-spacing:3px; text-transform:uppercase; margin-top:18px; }
.fcrow { display:flex; justify-content:space-between; align-items:baseline; padding:10px 0; border-bottom:1px solid #000; }
.fc-t { font-size:13px; letter-spacing:2px; }
.fc-c { font-size:11px; letter-spacing:2px; text-transform:uppercase; }
.fc-d { font-family:Georgia, serif; font-size:26px; font-weight:bold; }
.foot { margin-top:24px; font-size:9px; letter-spacing:3px; text-transform:uppercase; display:flex; justify-content:space-between; }
</style></head><body>
<div class="topline"><span>WEATHER</span><span>SEOUL</span></div>
<div class="rule"></div>
<div class="temp">${Math.round(cur.main.temp)}&deg;</div>
<div class="cond">${esc(cur.weather[0].description)}</div>
<div class="detail">HUM ${Math.round(cur.main.humidity)}%&nbsp;&nbsp;WIND ${Math.round(cur.wind.speed)}m/s<br>HI ${Math.round(cur.main.temp_max)}&deg;&nbsp;&nbsp;LO ${Math.round(cur.main.temp_min)}&deg;</div>
<div class="rule"></div>
<div class="fhead">NEXT 12 HOURS</div>
${fcRows}
<div class="rule"></div>
<div class="foot"><span>MENU · SYNC</span><span>${time}</span></div>
</body></html>`;
}

exports.weatherPage = onRequest({ memory: "256MiB" }, async (req, res) => {
  const lat = req.query.lat || 37.568;
  const lon = req.query.lon || 126.978;
  try {
    const [curR, fcR] = await Promise.all([
      fetch(`https://api.openweathermap.org/data/2.5/weather?lat=${lat}&lon=${lon}&appid=${OWM_KEY}&units=metric`),
      fetch(`https://api.openweathermap.org/data/2.5/forecast?lat=${lat}&lon=${lon}&appid=${OWM_KEY}&units=metric`),
    ]);
    if (!curR.ok || !fcR.ok) {
      res.status(502).send("upstream error");
      return;
    }
    const cur = await curR.json();
    const fc = await fcR.json();
    res.set("Cache-Control", "public, max-age=600");
    res.type("html").send(page(cur, fc));
  } catch (e) {
    res.status(502).send("weather unavailable");
  }
});
