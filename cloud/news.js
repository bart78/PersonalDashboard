const { onRequest } = require("firebase-functions/v2/https");

function stripHtml(s) {
  return String(s)
    .replace(/&amp;/g, "&")
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/<[^>]+>/g, " ")
    .replace(/\s+/g, " ")
    .trim();
}

exports.newsText = onRequest({ memory: "256MiB" }, async (req, res) => {
  try {
    const r = await fetch("https://news.google.com/rss?hl=en-US&gl=US&ceid=US:en");
    const xml = await r.text();
    const items = [...xml.matchAll(/<item>([\s\S]*?)<\/item>/g)].slice(0, 24);
    const out = items.map((m) => {
      const t = (m[1].match(/<title>(.*?)<\/title>/) || [, ""])[1];
      const d = (m[1].match(/<description>(.*?)<\/description>/) || [, ""])[1];
      const title = stripHtml(t);
      const desc = stripHtml(d).slice(0, 220);
      return `${title}\n${desc}`;
    }).join("\n\n") + "\n";
    res.set("Cache-Control", "public, max-age=300");
    res.type("text").send(out);
  } catch (e) {
    res.status(502).send("news unavailable");
  }
});
