/**
 * Import function triggers from their respective submodules:
 *
 * const {onCall} = require("firebase-functions/v2/https");
 * const {onDocumentWritten} = require("firebase-functions/v2/firestore");
 *
 * See a full list of supported triggers at https://firebase.google.com/docs/functions
 */

const { onRequest } = require("firebase-functions/v2/https");

// For global options applicable to all 2nd gen functions
const { setGlobalOptions } = require("firebase-functions/v2");
setGlobalOptions({ maxInstances: 10 });

// Puppeteer and Image Processing dependencies
const puppeteer = require("puppeteer-core");
const chromium = require('@sparticuz/chromium');
const sharp = require("sharp");

/**
 * Validates target URLs to prevent SSRF (Server-Side Request Forgery) attacks.
 * Blocks requests to localhost, metadata endpoints, and private network ranges.
 */
function isValidUrl(urlString) {
  try {
    const parsedUrl = new URL(urlString);
    
    // Only allow standard HTTP/HTTPS protocols
    if (parsedUrl.protocol !== 'http:' && parsedUrl.protocol !== 'https:') {
      return false;
    }
    
    const hostname = parsedUrl.hostname.toLowerCase();
    
    // Block common local/private and GCP metadata server hosts
    const privateHosts = [
      'localhost',
      '127.0.0.1',
      '0.0.0.0',
      '169.254.169.254',
      'metadata.google.internal',
      'metadata'
    ];
    
    if (privateHosts.includes(hostname)) {
      return false;
    }
    
    // Block RFC 1918 private network subnets:
    // - 10.0.0.0/8
    // - 172.16.0.0/12
    // - 192.168.0.0/16
    if (
      hostname.startsWith('10.') ||
      hostname.startsWith('192.168.') ||
      /^172\.(1[6-9]|2[0-9]|3[0-1])\./.test(hostname)
    ) {
      return false;
    }
    
    // Optional whitelist enforcement if ALLOWED_DOMAINS env is defined
    if (process.env.ALLOWED_DOMAINS) {
      const allowed = process.env.ALLOWED_DOMAINS.split(',').map(d => d.trim().toLowerCase());
      const hasMatch = allowed.some(domain => {
        return hostname === domain || hostname.endsWith('.' + domain);
      });
      if (!hasMatch) {
        return false;
      }
    }
    
    return true;
  } catch (e) {
    return false;
  }
}

/**
 * Helper function to scroll through the page to trigger lazy loading of images/components.
 */
async function autoScroll(page) {
  await page.evaluate(async () => {
    await new Promise((resolve) => {
      let totalHeight = 0;
      const distance = 100;
      const timer = setInterval(() => {
        const scrollHeight = document.body.scrollHeight;
        window.scrollBy(0, distance);
        totalHeight += distance;

        if (totalHeight >= scrollHeight - window.innerHeight) {
          clearInterval(timer);
          resolve();
        }
      }, 100);
    });
  });
}

/**
 * Helper function to inject CSS rules to hide specific elements on the page.
 */
async function hideElements(page, selectorString) {
  const selectors = selectorString.split(',').map(s => s.trim()).filter(Boolean);
  if (selectors.length > 0) {
    const css = selectors.map(s => `${s} { display: none !important; }`).join('\n');
    await page.addStyleTag({ content: css });
  }
}

exports.screenshot = onRequest(
  {
    timeoutSeconds: 120, // 2 minutes
    memory: "2GiB",      // Use 'GiB' for 2nd Gen functions
  },
  async (req, res) => {
    // 1. Authenticate Request if SCREENSHOT_API_KEY is configured
    const apiKey = process.env.SCREENSHOT_API_KEY;
    if (apiKey) {
      const reqKey = req.query.key || req.headers['x-api-key'];
      if (reqKey !== apiKey) {
        return res.status(401).send("Unauthorized: Invalid or missing API key.");
      }
    }

    const url = req.query.url;
    
    // Ensure fresh responses
    res.set('Cache-Control', 'no-store, no-cache, must-revalidate, proxy-revalidate');

    // 2. Validate URL input and enforce SSRF protections
    if (!url) {
      return res.status(400).send("Please provide a URL query parameter.");
    }
    if (!isValidUrl(url)) {
      return res.status(400).send("Invalid target URL. The requested host is private or unauthorized.");
    }
    
    // 3. Resolve output format (default to 'png' to maintain backward compatibility)
    const format = (req.query.format || 'png').toLowerCase();
    const validFormats = ['png', 'jpeg', 'webp', 'pdf'];
    if (!validFormats.includes(format)) {
      return res.status(400).send(`Invalid format "${format}". Supported formats are: ${validFormats.join(', ')}`);
    }

    // 4. Resolve viewport dimensions locally per request to avoid race conditions
    const viewWidth = req.query.width ? parseInt(req.query.width) : 1280;
    const viewHeight = req.query.height ? parseInt(req.query.height) : 720;
    
    if (Number.isNaN(viewWidth) || Number.isNaN(viewHeight) || viewWidth <= 0 || viewHeight <= 0) {
      return res.status(400).send("Width and height must be positive integers.");
    }

    let browser = null;

    try {
      // 5. Launch Puppeteer
      browser = await puppeteer.launch({
        args: [
          ...chromium.args,
          '--disable-gpu',
          '--disable-dev-shm-usage',
          '--disable-setuid-sandbox',
          '--no-first-run',
          '--no-sandbox',
          '--no-zygote',
          '--single-process',
        ],
        defaultViewport: chromium.defaultViewport,
        executablePath: process.env.FUNCTIONS_EMULATOR ? '/opt/homebrew/bin/chromium' : await chromium.executablePath(),
        headless: chromium.headless,
        ignoreHTTPSErrors: true,
      });
      
      const page = await browser.newPage();

      // Configure viewport size
      await page.setViewport({
        width: viewWidth,
        height: viewHeight,
        deviceScaleFactor: 1
      });

      // 6. Apply Device Emulation if specified
      if (req.query.device) {
        const deviceTemplate = puppeteer.KnownDevices[req.query.device] || puppeteer.KnownDevices[decodeURIComponent(req.query.device)];
        if (deviceTemplate) {
          await page.emulate(deviceTemplate);
        }
      }

      // 7. Configure custom User-Agent if specified
      if (req.query.ua) {
        await page.setUserAgent(decodeURIComponent(req.query.ua));
      }

      // 8. Navigate to page
      await page.goto(url, { waitUntil: "networkidle2", timeout: 30000 });

      // 9. Execute Auto-Scroll to load lazy components if specified
      if (req.query.autoscroll === 'true') {
        await autoScroll(page);
      }

      // 10. Hide target elements if specified
      if (req.query.hide) {
        await hideElements(page, req.query.hide);
      }

      // 11. Evaluate custom JS if specified
      if (req.query.js) {
        await page.evaluate((code) => {
          try {
            // eslint-disable-next-line no-eval
            eval(code);
          } catch (e) {
            console.error("Failed executing custom JS parameter:", e);
          }
        }, decodeURIComponent(req.query.js));
      }

      // 12. Handle PDF output generation
      if (format === 'pdf') {
        const pdfOptions = {
          printBackground: true,
          format: 'A4',
        };
        // Use custom dimensions if height and width are explicitly specified
        if (req.query.width || req.query.height) {
          pdfOptions.width = `${viewWidth}px`;
          pdfOptions.height = `${viewHeight}px`;
          delete pdfOptions.format;
        }
        
        const pdfBuffer = await page.pdf(pdfOptions);
        res.setHeader("Content-Type", "application/pdf");
        res.setHeader("Content-Disposition", 'inline; filename="document.pdf"');
        return res.status(200).send(pdfBuffer);
      }

      // 13. Capture page or element screenshot
      let imageBuffer;
      const screenshotOptions = {
        type: format === 'jpeg' ? 'jpeg' : format === 'webp' ? 'webp' : 'png',
      };

      if (req.query.selector) {
        const element = await page.$(req.query.selector);
        if (!element) {
          return res.status(400).send(`Element matching selector "${req.query.selector}" not found.`);
        }
        imageBuffer = await element.screenshot(screenshotOptions);
      } else {
        if (req.query.fullpage === 'true') {
          screenshotOptions.fullPage = true;
        }
        imageBuffer = await page.screenshot(screenshotOptions);
      }

      // 14. Process image for E-Ink screen compatibility (sharp only supports images, not PDFs)
      const grayscale = req.query.grayscale === 'true';
      const monochrome = req.query.monochrome === 'true';
      const dither = req.query.dither === 'true';

      if (monochrome || grayscale) {
        let sharpImg = sharp(imageBuffer);
        
        if (monochrome) {
          if (dither) {
            // Apply Floyd-Steinberg dithering to black and white
            sharpImg = sharpImg.grayscale().png({ palette: true, colors: 2, dither: 1.0 });
          } else {
            // Apply crisp binary threshold (perfect for clean lines and text layouts)
            sharpImg = sharpImg.threshold(128);
          }
        } else if (grayscale) {
          sharpImg = sharpImg.grayscale();
        }
        
        imageBuffer = await sharpImg.toBuffer();
      }

      // 15. Return the processed image
      res.setHeader("Content-Type", `image/${format}`);
      res.setHeader("Content-Disposition", `inline; filename="screenshot.${format}"`);
      res.status(200).send(imageBuffer);

    } catch (error) {
      console.error("Error generating screenshot:", error);
      return res.status(500).send("An error occurred while generating the screenshot.");
    } finally {
      if (browser !== null) {
        await browser.close();
      }
    }
  }
);
exports.weatherPage = require("./weather").weatherPage;

exports.newsText = require("./news").newsText;

exports.todoPage = require("./todo").todoPage;
