// Run by tests/e2e/run.sh, which sets BASE, SHOTS, CHROMIUM and PLAYWRIGHT_CORE.
const { chromium } = await import(process.env.PLAYWRIGHT_CORE);
const BASE = process.env.BASE;
let failures = 0;
const check = (cond, label) => { console.log((cond ? 'PASS ' : 'FAIL ') + label); if (!cond) failures++; };
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM });
const page = await browser.newPage();
const errors = [];
page.on('pageerror', (e) => errors.push(String(e)));
page.on('console', (m) => { if (m.type() === 'error' && !m.text().startsWith('Failed to load resource')) errors.push(m.text()); });
const visits = [];
page.on('request', (r) => { if (r.url().endsWith('/api/visits')) visits.push(r.method()); });

await page.goto(BASE + '/');
await page.waitForFunction(() => document.body.innerText.toLowerCase().includes('legacy headline'), null, { timeout: 20000 });
check(true, 'home page renders CMS copy from /api/site');
await page.waitForFunction(() => document.querySelectorAll('#gallery-grid img').length > 0, null, { timeout: 10000 });
const src = await page.getAttribute('#gallery-grid img', 'src');
check(src.startsWith('/media/site/') && src.endsWith('/card'), 'gallery uses anvil media URLs: ' + src);
const loaded = await page.evaluate(() => { const i = document.querySelector('#gallery-grid img'); return new Promise((r) => (i.complete ? r(i.naturalWidth) : (i.onload = () => r(i.naturalWidth), i.onerror = () => r(0)))); });
check(loaded > 0, 'gallery image actually loads (' + loaded + 'px)');
const teamButtons = await page.$$eval('#team-selector .team-btn', (b) => b.map((x) => x.textContent));
check(teamButtons.includes('Robotics') && teamButtons.includes('Presentation'), 'recruiting teams offered: ' + teamButtons.join(', '));
check(visits.length === 1, 'exactly one visit beacon (' + visits.length + ')');
await page.screenshot({ path: process.env.SHOTS + '/home.png' });

// Apply
await page.fill('#apply-firstname', 'Hana');
await page.fill('#apply-lastname', 'Mostafa');
await page.fill('#apply-email', 'hana@example.com');
await page.fill('#apply-phone', '12345');
await page.click('#team-selector .team-btn >> text=Robotics');
await page.click('#apply-submit-btn-side');
await page.waitForSelector('#brutalist-alert-modal', { timeout: 10000 });
check((await page.textContent('#brutalist-alert-modal')).includes('Egyptian mobile'), 'bad phone explained');
await page.click('#brutalist-alert-modal button');
await page.fill('#apply-phone', '0101 234 5678');
await page.click('#apply-submit-btn-side');
await page.waitForFunction(() => !document.getElementById('apply-submit-btn-side') || document.getElementById('apply-submit-btn-side').style.display === 'none', null, { timeout: 10000 });
check(true, 'application submitted');

// Form page
await page.goto(BASE + '/apply');
await page.waitForSelector('#fields-container .field-group', { timeout: 15000 });
const title = await page.textContent('#form-title');
check(title.length > 0, 'form page renders an open form: ' + title);
const inputs = await page.$$('#fields-container input, #fields-container textarea, #fields-container select');
for (const el of inputs) {
  const tag = await el.evaluate((n) => n.tagName);
  if (tag === 'SELECT') { const v = await el.evaluate((n) => n.options[1] && n.options[1].value); if (v) await el.selectOption(v); }
  else { const type = await el.getAttribute('type'); await el.fill(type === 'email' ? 'a@example.com' : type === 'tel' ? '01012345678' : 'Answer'); }
}
await page.click('#submit-btn');
await page.waitForSelector('#success-container', { state: 'visible', timeout: 10000 });
check(true, 'form response submitted');
await page.goto(BASE + '/apply?form=not-a-uuid');
await page.waitForFunction(() => document.getElementById('loading-state').innerText.includes('not valid'), null, { timeout: 10000 });
check(true, 'bad form link handled');
check(errors.length === 0, 'no page errors: ' + errors.slice(0, 3).join(' | '));
await browser.close();
console.log('failures:', failures);
process.exit(failures ? 1 : 0);
