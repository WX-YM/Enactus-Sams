// Run by tests/e2e/run.sh, which sets BASE, SHOTS, CHROMIUM and PLAYWRIGHT_CORE.
const { chromium } = await import(process.env.PLAYWRIGHT_CORE);
const SHOTS = process.env.SHOTS;
const BASE = process.env.BASE;
let failures = 0;
const check = (cond, label) => { console.log((cond ? 'PASS ' : 'FAIL ') + label); if (!cond) failures++; };

const browser = await chromium.launch({ executablePath: process.env.CHROMIUM });
const page = await browser.newPage();
const consoleErrors = [];
page.on('console', (m) => { if (m.type() === 'error' && !m.text().startsWith('Failed to load resource')) consoleErrors.push(m.text()); });
page.on('pageerror', (e) => consoleErrors.push(String(e)));

await page.goto(BASE + '/admin/');
await page.waitForSelector('#email', { timeout: 15000 });
check(true, 'login screen renders');
await page.fill('#email', 'owner@enactussams.org');
await page.fill('#password', 'wrong-password-123');
await page.click('button[type=submit]');
await page.waitForSelector('[role=alert]', { timeout: 30000 });
check((await page.textContent('[role=alert]')).includes('Invalid email or password'), 'wrong password shows generic error');

await page.fill('#password', 'superadmin-pass-123');
await page.click('button[type=submit]');
await page.waitForSelector('text=Platform Analytics.', { timeout: 30000 });
check(true, 'superadmin signs in through browser prehash');
const cookies = await page.context().cookies();
check(cookies.some((c) => c.name.startsWith('__Host-') && c.httpOnly), 'session is an HttpOnly __Host- cookie');
const stored = await page.evaluate(() => JSON.stringify(localStorage) + JSON.stringify(sessionStorage));
check(!/token|eyJ/i.test(stored), 'no token in web storage');
await page.screenshot({ path: SHOTS + '/dashboard.png', fullPage: true });

for (const [link, heading] of [['Application Responses', 'Applications.'], ['Form Responses', 'Form Responses.'], ['Form Maker', 'Form Maker.'], ['Manage Teams', 'Teams.'], ['Content CMS', 'Content CMS.'], ['Gallery', 'Gallery.'], ['Access Control', 'Access Control.'], ['Audit Log', 'Audit Log.']]) {
  await page.click(`nav >> text=${link}`);
  await page.waitForSelector(`h1:has-text("${heading}")`, { timeout: 15000 });
  await page.waitForTimeout(300);
  const alert = await page.$('[role=alert]');
  check(alert === null, `${link} page loads without error${alert ? ': ' + await alert.textContent() : ''}`);
  await page.screenshot({ path: SHOTS + `/${link.replace(/ /g, '_')}.png`, fullPage: true });
}

// Applications: legacy rows visible, review & accept
await page.click('nav >> text=Application Responses');
await page.waitForSelector('text=Mona Ahmed');
check(true, 'migrated applications listed');
await page.click('text=Mona Ahmed');
await page.selectOption('[role=dialog] select >> nth=0', 'interview_scheduled');
await page.click('[role=dialog] >> text=Save');
await page.waitForSelector('[role=dialog]', { state: 'detached' });
check((await page.textContent('table')).includes('Interview'), 'application status updated');

// Teams: create one
await page.click('nav >> text=Manage Teams');
await page.fill('input[placeholder="Team name"]', 'Logistics');
await page.fill('textarea[placeholder^="Short description"]', 'Moves things');
await page.click('text=Add team');
await page.waitForSelector('input[value="Logistics"]', { timeout: 10000 });
check(true, 'team created');

// Content: edit hero headline and publish
await page.click('nav >> text=Content CMS');
await page.waitForSelector('text=Publish');
const headline = page.locator('label:has-text("Headline line 1") input, label:has-text("headline1") input').first();
if (await headline.count()) {
  await headline.fill('Published from the new panel');
} else {
  await page.locator('.card input[type=text]').first().fill('Published from the new panel');
}
await page.click('button:has-text("Publish")');
await page.waitForSelector('text=Hero published.', { timeout: 10000 });
const site = await (await page.request.get(BASE + '/api/site')).json();
check(JSON.stringify(site.sections['home.hero']).includes('Published from the new panel'), 'published copy reaches the public site payload');

// Gallery: upload a photo
await page.click('nav >> text=Gallery');
await page.click('button:has-text("Life at Enactus")');
const before = await page.locator('.card img').count();
await page.setInputFiles('input[type=file]', process.env.REPO + '/public/assets/mic.jpg');
await page.waitForFunction((n) => document.querySelectorAll('.card img').length > n, before, { timeout: 60000 });
check(true, 'photo uploaded through the media pipeline');

// Form maker: create a form
await page.click('nav >> text=Form Maker');
await page.click('text=+ New form');
await page.fill('input[placeholder="Form title"]', 'UI Test Form');
await page.selectOption('select >> nth=0', 'active');
await page.click('text=Create form');
await page.waitForSelector('button:has-text("UI Test Form")', { timeout: 10000 });
check(true, 'form created');

// Users: create staff (browser enrolment prehash)
await page.click('nav >> text=Access Control');
await page.click('text=New account');
await page.fill('[role=dialog] input[type=email]', 'ui.member@enactussams.org');
await page.fill('[role=dialog] input[type=password]', 'ui-member-password');
await page.check('[role=dialog] label:has-text("Gallery") input');
await page.click('[role=dialog] button[type=submit]');
await page.waitForSelector('text=ui.member@enactussams.org', { timeout: 30000 });
check(true, 'staff account created');

await page.click('text=Sign Out');
await page.waitForSelector('#email', { timeout: 15000 });
check(true, 'signed out');

// The new member signs in and sees only the gallery
await page.fill('#email', 'ui.member@enactussams.org');
await page.fill('#password', 'ui-member-password');
await page.click('button[type=submit]');
await page.waitForSelector('h1:has-text("Gallery.")', { timeout: 30000 });
const nav = await page.textContent('nav');
check(nav.includes('Gallery') && !nav.includes('Access Control') && !nav.includes('Applications'), 'restricted member sees only the gallery: ' + nav.replace(/\s+/g, ' ').trim());

check(consoleErrors.length === 0, 'no console errors: ' + consoleErrors.slice(0, 3).join(' | '));
await browser.close();
console.log('failures:', failures);
process.exit(failures ? 1 : 0);
