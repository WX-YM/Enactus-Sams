import base64, json, sys, unicodedata, http.cookiejar, urllib.request, urllib.error
from argon2.low_level import hash_secret_raw, Type

import os
BASE = os.environ["BASE"]
ORIGIN = BASE
fails = 0
def check(cond, label):
    global fails
    print(("PASS " if cond else "FAIL ") + label)
    if not cond: fails += 1

class Client:
    # The session cookies are __Host- and Secure; a browser on https sends them,
    # Python's jar refuses to over plain http, so they are carried by hand here.
    def __init__(self):
        self.cookies = {}
        self.op = urllib.request.build_opener()
    def req(self, method, path, body=None, origin=True, raw=False, headers=None):
        data = None
        h = dict(headers or {})
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            h.setdefault("Content-Type", "application/json")
        if origin: h["Origin"] = ORIGIN
        if self.cookies: h["Cookie"] = "; ".join("%s=%s" % kv for kv in self.cookies.items())
        r = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
        try:
            resp = self.op.open(r)
            code, payload, hdrs = resp.status, resp.read(), resp.headers
        except urllib.error.HTTPError as e:
            code, payload, hdrs = e.code, e.read(), e.headers
        for sc in hdrs.get_all("Set-Cookie") or []:
            name, _, rest = sc.partition("=")
            value = rest.split(";")[0]
            if "Max-Age=0" in sc or value == "": self.cookies.pop(name, None)
            else: self.cookies[name] = value
        if raw: return code, payload, hdrs
        try: return code, json.loads(payload or b"null")
        except Exception: return code, payload

def credential(password, answer):
    k = hash_secret_raw(unicodedata.normalize("NFC", password).encode(), base64.urlsafe_b64decode(answer["salt"] + "=="),
                        time_cost=answer["iterations"], memory_cost=answer["memory_kib"], parallelism=answer["parallelism"],
                        hash_len=32, type=Type.ID)
    return base64.urlsafe_b64encode(k).rstrip(b"=").decode()

def login(email, password, purpose=None):
    c = Client()
    code, salt = c.req("POST", "/api/auth/salt", {"identifier": email})
    if code != 200: return c, code
    code, out = c.req("POST", "/api/auth/login", {"identifier": email, "credential": credential(password, salt)})
    return c, code

# --- public site
anon = Client()
code, site = anon.req("GET", "/api/site", origin=False)
check(code == 200 and "sections" in site, "GET /api/site")
check(site["sections"]["home.hero"] is not None and "Legacy headline" in json.dumps(site["sections"]["home.hero"]), "legacy hero headline migrated")
check("Line two" in json.dumps(site["sections"]["home.footer"]), "footer address split")
check("not a url" not in json.dumps(site["sections"]["home.footer"]), "invalid legacy URL kept default")
names = [t["name"] for t in site["teams"]]
check("Robotics" in names and "Presentation" in names and "Social Media" not in names, "teams migrated, unused seeds removed: %s" % names)
check(len(site["gallery"]["life"]) == 2, "life gallery replaced by legacy photos")
check(site["open"] is True, "recruitment open")
check(len(site["forms"]) == 1 and site["forms"][0]["title"] == "Volunteer Day", "legacy form listed")
src = site["gallery"]["life"][0]["image"]["src"]
code, body, hdrs = anon.req("GET", src + "/card", origin=False, raw=True)
check(code == 200 and hdrs.get("Content-Type", "").startswith("image/") and len(body) > 1000, "media via X-Accel: %s %s" % (code, hdrs.get("Content-Type")))
code, _, _ = anon.req("GET", "/protected_storage/ns/site/x", origin=False, raw=True)
check(code == 404, "internal storage location not reachable directly (%s)" % code)
code, body, hdrs = anon.req("GET", "/", origin=False, raw=True)
check(code == 200 and b"<html" in body.lower() and "Content-Security-Policy" in hdrs, "home page served with CSP")

# --- applications
app = {"first_name": "Nour", "last_name": "Adel", "email": "nour@example.com", "phone": "0101 234 5678", "team": "robotics", "reason": "build things"}
code, out = anon.req("POST", "/api/applications", app)
check(code == 201, "apply accepted (%s %s)" % (code, out))
code, out2 = anon.req("POST", "/api/applications", app)
check(code == 201 and out2 == out, "duplicate application answers identically")
code, out = anon.req("POST", "/api/applications", app, origin=False)
check(code == 403, "apply without Origin refused (%s)" % code)
code, out = anon.req("POST", "/api/applications", dict(app, email="x@example.com", phone="12345"))
check(code == 400, "invalid phone refused (%s)" % code)
code, out = anon.req("POST", "/api/applications", dict(app, email="y@example.com", admin=True))
check(code == 400, "unknown field refused (%s)" % code)

# --- forms
fid = site["forms"][0]["id"]
code, form = anon.req("GET", "/api/forms/" + fid, origin=False)
check(code == 200 and len(form["fields"]) == 3 and "submission_count" not in form, "public form read")
opt = form["fields"][1]["options"][0]["value"]
code, out = anon.req("POST", "/api/forms/%s/responses" % fid, {"answers": {"f1": "=cmd|' /C calc'!A0", "f2": opt}})
check(code == 201, "form submit (%s %s)" % (code, out))
code, out = anon.req("POST", "/api/forms/%s/responses" % fid, {"answers": {"f1": "x", "f2": "not-an-option"}})
check(code == 400, "unknown option refused (%s)" % code)
code, out = anon.req("POST", "/api/forms/%s/responses" % fid, {"answers": {"f1": "x", "f2": opt, "$where": "1"}})
check(code == 400, "operator-shaped key refused (%s)" % code)

# --- staff access control
code, _ = anon.req("GET", "/api/staff", origin=False)
check(code == 401, "staff list anonymous -> 401 (%s)" % code)
admin, code = login("admin@enactussams.org", "legacy-admin-pass")
check(code == 200, "legacy superadmin signs in with old password (%s)" % code)
_, code = login("admin@enactussams.org", "wrong-password-x")
check(code == 401, "wrong password -> 401 (%s)" % code)
hr, code = login("hr.lead@enactussams.org", "hr-lead-password")
check(code == 200, "legacy manager signs in (%s)" % code)
plain, code = login("plain@enactussams.org", "plaintext-legacy-pw")
check(code == 200, "legacy plaintext user signs in after enrolment (%s)" % code)
owner, code = login("owner@enactussams.org", "superadmin-pass-123")
check(code == 200, "superadmin created by migrate signs in (%s)" % code)

code, me = hr.req("GET", "/api/me", origin=False)
check(code == 200 and me.get("role") == "manager" and me.get("team") == "Human Resources", "hr /api/me: %s" % me)
code, _ = hr.req("GET", "/api/staff", origin=False)
check(code == 403, "manager without users perm -> 403 on staff (%s)" % code)
code, _ = hr.req("GET", "/api/audit", origin=False)
check(code == 403, "manager without users perm -> 403 on audit (%s)" % code)
code, apps = hr.req("GET", "/api/applications", origin=False)
emails = sorted(a["email"] for a in apps.get("applications", []))
check(code == 200 and emails == ["karim@example.com"], "manager sees only own-team (referred) applications: %s" % emails)
code, apps = admin.req("GET", "/api/applications", origin=False)
check(code == 200 and len(apps["applications"]) == 3, "superadmin sees all applications (%d)" % len(apps.get("applications", [])))
nour = [a for a in apps["applications"] if a["email"] == "nour@example.com"][0]
check(nour["phone"] == "+201012345678" and nour["team"] == "Robotics", "phone normalised, team canonical")
code, out = admin.req("PATCH", "/api/applications/" + nour["id"], {"version": nour["version"], "status": "accepted"})
check(code == 200, "accept application (%s %s)" % (code, out))
code, site2 = anon.req("GET", "/api/site", origin=False)
rob = [t for t in admin.req("GET", "/api/teams", origin=False)[1]["teams"] if t["name"] == "Robotics"][0]
code, members = admin.req("GET", "/api/teams/%s/members" % rob["id"], origin=False)
check(any(m["name"] == "Nour Adel" for m in members["members"]), "accepted applicant added to roster")
code, out = hr.req("DELETE", "/api/applications/" + nour["id"])
check(code in (403, 404), "manager cannot delete applications (%s)" % code)

code, dash = admin.req("GET", "/api/dashboard", origin=False)
check(code == 200 and dash["applications_total"] == 3, "dashboard (%s)" % code)
code, audit = admin.req("GET", "/api/audit", origin=False)
check(code == 200 and any(r["action"] for r in audit["rows"]), "audit log readable by superadmin")

code, exported = admin.req("GET", "/api/forms/%s/export" % fid, origin=False)
text = exported["csv"].lstrip("﻿") if code == 200 else ""
check(code == 200 and "'=cmd" in text or "\t=cmd" in text or "'=HYPERLINK" in text, "CSV export neutralises formulas")
check("=HYPERLINK" not in text.replace("'=HYPERLINK", ""), "no live formula in CSV")

# Staff creation with browser prehash at enrolment
code, salt = admin.req("POST", "/api/auth/salt", {"identifier": "new.member@enactussams.org", "purpose": "enroll"})
cred = credential("brand-new-password", salt)
code, out = admin.req("POST", "/api/staff", {"email": "new.member@enactussams.org", "credential": cred, "role": "member",
                                             "team": "", "permissions": ["content"]})
check(code == 201, "create staff (%s %s)" % (code, out))
newbie, code = login("new.member@enactussams.org", "brand-new-password")
check(code == 200, "new staff signs in (%s)" % code)
code, _ = newbie.req("GET", "/api/sections", origin=False)
check(code == 200, "content perm reaches sections (%s)" % code)
code, _ = newbie.req("GET", "/api/applications", origin=False)
check(code == 403, "content-only staff refused applications (%s)" % code)
code, out = hr.req("POST", "/api/staff", {"email": "x2@enactussams.org", "credential": cred, "role": "member", "team": "", "permissions": ["users"]})
check(code == 403, "manager cannot create staff (%s)" % code)

# Access Control is bounded by the holder's own access.
cred_users = credential("access-manager-pass", admin.req("POST", "/api/auth/salt", {"identifier": "access.mgr@enactussams.org", "purpose": "enroll"})[1])
check(admin.req("POST", "/api/staff", {"email": "access.mgr@enactussams.org", "credential": cred_users, "role": "member", "team": "", "permissions": ["users"]})[0] == 201, "create access-control-only staff")
cred_none = credential("no-access-password", admin.req("POST", "/api/auth/salt", {"identifier": "no.access@enactussams.org", "purpose": "enroll"})[1])
check(admin.req("POST", "/api/staff", {"email": "no.access@enactussams.org", "credential": cred_none, "role": "member", "team": "", "permissions": []})[0] == 201, "create no-access staff")
mgr, code = login("access.mgr@enactussams.org", "access-manager-pass")
check(code == 200, "access-control-only staff signs in (%s)" % code)
staff = {a["email"]: a for a in admin.req("GET", "/api/staff", origin=False)[1]["staff"]}
hr_acct = staff["hr.lead@enactussams.org"]
code, _ = mgr.req("PATCH", "/api/staff/" + hr_acct["id"], {"version": hr_acct["version"], "permissions": []})
check(code == 403, "cannot strip an account with more access (%s)" % code)
code, _ = mgr.req("DELETE", "/api/staff/" + hr_acct["id"], {})
check(code == 403, "cannot disable an account with more access (%s)" % code)
code, _ = mgr.req("DELETE", "/api/staff/" + staff["admin@enactussams.org"]["id"], {})
check(code == 403, "cannot disable a superadmin (%s)" % code)
code, _ = mgr.req("DELETE", "/api/staff/" + staff["no.access@enactussams.org"]["id"], {})
check(code == 200, "can disable an account within own access (%s)" % code)

# Reading form responses does not include deleting them.
code, listing = hr.req("GET", "/api/forms/%s/responses" % fid, origin=False)
check(code == 200 and len(listing["responses"]) > 0, "applications reviewer reads form responses (%s)" % code)
if code == 200 and listing["responses"]:
    code, _ = hr.req("DELETE", "/api/forms/%s/responses/%s" % (fid, listing["responses"][0]["id"]), {})
    check(code == 403, "applications reviewer cannot delete a response (%s)" % code)

code, _ = admin.req("POST", "/api/auth/logout", {})
code2, _ = admin.req("GET", "/api/me", origin=False)
check(code2 == 401, "after logout the session is gone (%s/%s)" % (code, code2))
print("failures:", fails)
sys.exit(1 if fails else 0)
