// Run by tests/e2e/run.sh, which sets BASE, SHOTS, CHROMIUM and PLAYWRIGHT_CORE.
const { chromium } = await import(process.env.PLAYWRIGHT_CORE);
const fs = await import('node:fs/promises');
const SHOTS = process.env.SHOTS;
const BASE = process.env.BASE;
let failures = 0;
const check = (cond, label) => { console.log((cond ? 'PASS ' : 'FAIL ') + label); if (!cond) failures++; };

const browser = await chromium.launch({ executablePath: process.env.CHROMIUM });
const context = await browser.newContext({ acceptDownloads: true });
const page = await context.newPage();
const consoleErrors = [];
page.on('console', (m) => { if (m.type() === 'error' && !m.text().startsWith('Failed to load resource')) consoleErrors.push(m.text()); });
page.on('pageerror', (e) => consoleErrors.push(String(e)));

const signIn = async (email, password, landing) => {
  await page.fill('#email', email);
  await page.fill('#password', password);
  await page.click('button[type=submit]');
  await page.waitForSelector(`h1:has-text("${landing}")`, { timeout: 30000 });
};
const signOut = async () => {
  await page.click('text=Sign Out');
  await page.waitForSelector('#email', { timeout: 15000 });
};
const row = (text) => page.locator('table tbody tr', { hasText: text });

// Two applications from the public form: one to accept directly, one to keep.
for (const [first, email] of [['Sara', 'sara@example.com'], ['Youssef', 'youssef@example.com']]) {
  const res = await page.request.post(BASE + '/api/applications', {
    headers: { Origin: BASE },
    data: { first_name: first, last_name: 'Applicant', email, phone: '0101 234 5678', team: 'Presentation', reason: `${first} wants in` },
  });
  check(res.status() === 201, `${first} applied through the public route (${res.status()})`);
}

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
const cookies = await context.cookies();
check(cookies.some((c) => c.name.startsWith('__Host-') && c.httpOnly), 'session is an HttpOnly __Host- cookie');
const stored = await page.evaluate(() => JSON.stringify(localStorage) + JSON.stringify(sessionStorage));
check(!/token|eyJ/i.test(stored), 'no token in web storage');
await page.waitForSelector('text=System Logs', { timeout: 15000 });
check((await page.textContent('body')).includes('Trending Teams (Ranked)'), 'dashboard shows ranked teams and system logs');
await page.screenshot({ path: SHOTS + '/dashboard.png', fullPage: true });

for (const [link, heading] of [['Application Responses', 'Application Responses.'], ['Form Responses', 'Form Responses.'], ['Form Maker', 'Form Maker.'], ['Manage Teams', 'Manage Teams.'], ['Content CMS', 'Content CMS.'], ['Gallery', 'Site Gallery.'], ['Access Control', 'Access Control']]) {
  await page.click(`nav >> text=${link}`);
  await page.waitForSelector(`h1:has-text("${heading}")`, { timeout: 15000 });
  await page.waitForTimeout(500);
  const alert = await page.$('[role=alert]');
  check(alert === null, `${link} page loads without error${alert ? ': ' + await alert.textContent() : ''}`);
  await page.screenshot({ path: SHOTS + `/${link.replace(/ /g, '_')}.png`, fullPage: true });
}
check(!(await page.textContent('nav')).includes('Audit Log'), 'no separate audit log entry in the navigation');

// --- Applications: accept, reject, refer, export ------------------------------
await page.click('nav >> text=Application Responses');
await page.waitForSelector('text=Mona Ahmed');
check(true, 'migrated applications listed');
check(await page.locator('button:has-text("Pending (")').count() === 1 && await page.locator('button:has-text("Referred (")').count() === 1, 'status filter tabs with counts');

await row('Sara Applicant').locator('button:has-text("Review")').click();
await page.click('[role=dialog] button:text-is("Accept")');
await page.waitForSelector('[role=dialog]', { state: 'detached' });
await page.waitForSelector('tr:has-text("Sara Applicant") .badge:has-text("accepted")', { timeout: 10000 });
check(true, 'accept marks the application accepted');

await row('Youssef Applicant').locator('button:has-text("Review")').click();
await page.fill('[role=dialog] textarea', 'Not this season');
await page.click('[role=dialog] button:text-is("Reject")');
await page.waitForSelector('[role=dialog]', { state: 'detached' });
await page.waitForSelector('tr:has-text("Youssef Applicant") .badge:has-text("rejected")', { timeout: 10000 });
check((await row('Youssef Applicant').textContent()).includes('Not this season'), 'rejected application stays listed with its note');

await row('Mona Ahmed').locator('button:has-text("Review")').click();
await page.selectOption('[role=dialog] select', 'Human Resources');
await page.click('[role=dialog] button:text-is("Refer")');
await page.waitForSelector('[role=dialog]', { state: 'detached' });
await page.waitForSelector('tr:has-text("Mona Ahmed") >> text=Referred to Human Resources', { timeout: 10000 });
check(true, 'refer sends the application to another team');

await page.click('button:has-text("Referred (")');
const [download] = await Promise.all([page.waitForEvent('download'), page.click('button:has-text("Export CSV")')]);
const csv = await fs.readFile(await download.path(), 'utf8');
check(csv.includes('Mona Ahmed') && csv.includes('Karim') && !csv.includes('Sara Applicant'), 'export CSV follows the selected filter');
await page.click('button:has-text("All (")');

// --- Teams: the accepted applicant is on the roster ---------------------------
await page.click('nav >> text=Manage Teams');
await page.waitForSelector('text=Manager:');
const presentation = page.locator('.card', { hasText: '/ Presentation' }).first();
await presentation.locator('button:has-text("active members")').click();
await page.waitForSelector('text=Team Roster');
check((await presentation.textContent()).includes('Sara Applicant'), 'accepted applicant added to the team roster as a member');
check((await page.locator('.card', { hasText: '/ Human Resources' }).first().textContent()).includes('hr.lead@enactussams.org'), 'team card lists its manager from access control');

await page.fill('input[placeholder="Team Name"]', 'Logistics');
await page.fill('input[placeholder="Short Description"]', 'Moves things');
await page.click('button:has-text("Add Team")');
await page.waitForSelector('text=/ Logistics', { timeout: 10000 });
check(true, 'team created');

// --- Content CMS: copy, and the photos of each section ------------------------
await page.click('nav >> text=Content CMS');
await page.waitForSelector('text=Hero Section');
await page.locator('label:has-text("Headline Row 1") + input').fill('Published');
await page.click('button:has-text("Save Changes")');
await page.waitForSelector('text=Saved Live!', { timeout: 10000 });
const site = await (await page.request.get(BASE + '/api/site')).json();
check(site.sections['home.hero'].headline1 === 'Published' || JSON.stringify(site.sections['home.hero']).includes('"Published"'), 'saved copy reaches the public site payload');

await page.click('button:has-text("02 About Section")');
await page.waitForSelector('text=About Section Photos');
const aboutBefore = await page.locator('.card img').count();
await page.setInputFiles('input[type=file]', process.env.REPO + '/public/assets/mic.jpg');
await page.waitForFunction((n) => document.querySelectorAll('.card img').length > n, aboutBefore, { timeout: 60000 });
check(true, 'about section photo uploaded from the CMS');

await page.click('button:has-text("03 Tafrah Project")');
await page.waitForSelector('text=Tafrah Platform Showcase Photo');
check(await page.locator('button:has-text("Replace Photo")').count() === 1 && await page.locator('text=Tafrah Behind-the-Scenes Photos').count() === 1, 'tafrah showcase and behind-the-scenes photos editable in the CMS');
await page.click('button:has-text("05 Media Gallery")');
await page.waitForSelector('text=Uploaded Gallery Photos');
check(true, 'media gallery photos in the CMS');
await page.click('button:has-text("06 Join Us")');
await page.waitForSelector('text=Recruitment Team Choices');
check(true, 'recruitment team choices in the CMS');

// --- Gallery ------------------------------------------------------------------
await page.click('nav >> text=Gallery');
await page.waitForSelector('text=Upload To:');
const before = await page.locator('.card img').count();
await page.setInputFiles('input[type=file]', process.env.REPO + '/public/assets/mic.jpg');
await page.waitForFunction((n) => document.querySelectorAll('.card img').length > n, before, { timeout: 60000 });
check(true, 'photo uploaded through the media pipeline');
await page.waitForSelector('text=Image Uploaded', { timeout: 10000 });
await page.click('button:text-is("OK")');

// --- Form maker -----------------------------------------------------------------
await page.click('nav >> text=Form Maker');
await page.waitForSelector('text=Form Details');
await page.click('button:has-text("New Form")');
await page.locator('label:has-text("Form Title") + input').fill('UI Test Form');
await page.click('button:has-text("Add Field")');
await page.click('button:has-text("Publish Form")');
await page.waitForSelector('text=Form Published!', { timeout: 10000 });
check((await page.inputValue('[role=dialog] input[readonly]')).includes('/apply?form='), 'published form has a share link');
await page.click('[role=dialog] button:has-text("Done")');

// --- Access control: team question only for manager roles ---------------------
await page.click('nav >> text=Access Control');
await page.fill('input[placeholder="user@enactus.org"]', 'ui.manager@enactussams.org');
await page.fill('input[type=password]', 'ui-manager-password');
await page.selectOption('select', 'manager');
await page.click('button:has-text("Add User")');
await page.waitForSelector('text=Configure Access');
check(await page.locator('text=Which team are they the manager of?').count() === 1, 'manager role asks which team');
await page.selectOption('[role=dialog] select', 'Logistics');
await page.click('[role=dialog] button:has-text("Save & Create User")');
await page.waitForSelector('tr:has-text("ui.manager@enactussams.org")', { timeout: 30000 });
check((await row('ui.manager@enactussams.org').textContent()).includes('Logistics'), 'manager created for the chosen team');

await page.fill('input[placeholder="user@enactus.org"]', 'ui.member@enactussams.org');
await page.fill('input[type=password]', 'ui-member-password');
await page.selectOption('select', 'member');
await page.click('button:has-text("Add User")');
await page.waitForSelector('text=Configure Access');
check(await page.locator('text=Which team are they').count() === 0, 'member role does not ask for a team');
for (const label of ['Dashboard & Analytics']) await page.uncheck(`[role=dialog] label:has-text("${label}") input`);
await page.click('[role=dialog] button:has-text("Save & Create User")');
await page.waitForSelector('tr:has-text("ui.member@enactussams.org")', { timeout: 30000 });
check(true, 'member created with only the gallery');

await signOut();
check(true, 'signed out');

// --- The HR manager answers the referrals sent to their team ----------------
await signIn('hr.lead@enactussams.org', 'hr-lead-password', 'Platform Analytics.');
await page.click('nav >> text=Application Responses');
await page.waitForSelector('text=Mona Ahmed');
check((await page.textContent('table')).includes('Referred to You'), 'manager sees referrals to their team');
await row('Karim').locator('button:has-text("Review")').click();
await page.click('[role=dialog] button:has-text("Accept Referral")');
await page.waitForSelector('[role=dialog]', { state: 'detached' });
await page.waitForSelector('tr:has-text("Karim") .badge:has-text("accepted")', { timeout: 10000 });
check(true, 'manager accepts a referral');
await page.click('nav >> text=Manage Teams');
const hr = page.locator('.card', { hasText: '/ Human Resources' }).first();
await hr.locator('button:has-text("active members")').click();
await page.waitForSelector('text=Team Roster');
await page.waitForTimeout(500);
check((await hr.textContent()).includes('Karim'), 'accepted referral joins the referred team roster');
await signOut();

// --- The new member sees only the gallery -------------------------------------
await signIn('ui.member@enactussams.org', 'ui-member-password', 'Site Gallery.');
const nav = await page.textContent('nav');
check(nav.includes('Gallery') && !nav.includes('Access Control') && !nav.includes('Applications'), 'restricted member sees only the gallery: ' + nav.replace(/\s+/g, ' ').trim());

check(consoleErrors.length === 0, 'no console errors: ' + consoleErrors.slice(0, 3).join(' | '));
await browser.close();
console.log('failures:', failures);
process.exit(failures ? 1 : 0);
