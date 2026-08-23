const { onRequest } = require("firebase-functions/v2/https");

const DB_URL = process.env.TODO_DB || "";
if (!DB_URL) throw new Error("TODO_DB env var required");

function esc(s) {
  return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

exports.todoPage = onRequest({ memory: "256MiB" }, async (req, res) => {
  try {
    const r = await fetch(DB_URL);
    const data = await r.json();
    const items = Object.entries(data || {}).filter(([, v]) => v != null).sort((a, b) => (a[0] < b[0] ? -1 : 1));
    const doneCount = items.filter(([, v]) => v && v.done).length;
    const rows = items
      .map(([k, v]) => {
        const text = v && typeof v === "object" && v.text ? String(v.text) : String(v);
        const done = !!(v && v.done);
        const box = done ? "<span class='box bd'></span>" : "<span class='box'></span>";
        const cls = done ? "row ro" : "row";
        return `<div class="${cls}">${box}<span class="txt">${esc(text)}</span></div>`;
      })
      .join("");
    const t = new Date();
    const time = String(t.getHours()).padStart(2, "0") + ":" + String(t.getMinutes()).padStart(2, "0");
    res.set("Cache-Control", "public, max-age=60");
    res.type("html").send(`<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
* { margin:0; padding:0; box-sizing:border-box; }
html, body { width:272px; height:792px; background:#fff; color:#000; }
body { font-family:"Helvetica Neue", Helvetica, Arial, sans-serif; padding:14px; }
.topline { font-size:9px; letter-spacing:3px; text-transform:uppercase; display:flex; justify-content:space-between; }
.rule { border-top:2px solid #000; margin:12px 0; }
.fhead { font-size:14px; letter-spacing:3px; text-transform:uppercase; margin-bottom:10px; }
.row { display:flex; align-items:flex-start; padding:20px 0; border-bottom:1px solid #000; }
.box { width:22px; height:22px; border:2px solid #000; flex-shrink:0; margin-top:2px; }
.box.bd { background:#000; }
.txt { font-size:19px; margin-left:14px; line-height:1.35; }
.row.ro .txt { text-decoration: line-through; opacity:.55; }
.foot { margin-top:20px; font-size:9px; letter-spacing:3px; text-transform:uppercase; display:flex; justify-content:space-between; }
</style></head><body>
<div class="topline"><span>TODO</span><span>${doneCount}/${items.length}</span></div>
<div class="rule"></div>
<div class="fhead">TASKS</div>
${rows || '<div class="row"><span class="txt">EMPTY</span></div>'}
<div class="rule"></div>
<div class="foot"><span>MENU · SYNC</span><span>${time}</span></div>
</body></html>`);
  } catch (e) {
    res.status(502).send("todo unavailable");
  }
});
