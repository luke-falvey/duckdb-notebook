// Opens single.html from file:// in headless Chromium with all network requests blocked.
const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();
  const blocked = [];
  await page.route(/^https?:/, route => { blocked.push(route.request().url()); return route.abort(); });
  await page.goto('file://' + __dirname + '/single.html');
  await page.waitForFunction(() => window.__R && window.__R.done, null, { timeout: 60000 }).catch(() => {});
  const R = await page.evaluate(() => window.__R);
  console.log(`protocol=${R.protocol} origin=${R.origin}`);
  for (const k in R.steps) console.log(`${k.padEnd(12)} ${String(R.t[k]).padStart(5)} ms  ${JSON.stringify(R.steps[k]).slice(0, 160)}`);
  console.log('network requests attempted (all blocked):', blocked);
  await browser.close();
})();
