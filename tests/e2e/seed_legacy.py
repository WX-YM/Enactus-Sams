import pymongo, sys
from argon2 import PasswordHasher
# Legacy-shaped data, as the pre-anvil backend stored it, for the import.
# Usage: seed_legacy.py <mongodb uri> <database>. Drops that database first.
db = pymongo.MongoClient(sys.argv[1])[sys.argv[2]]
for c in db.list_collection_names(): db.drop_collection(c)
ph = PasswordHasher(time_cost=3, memory_cost=65536, parallelism=1, hash_len=32, salt_len=16)
db.users.insert_many([
  {"email": "admin@enactussams.org", "password": ph.hash("legacy-admin-pass"), "role": "superadmin", "permissions": []},
  {"email": "hr.lead@enactussams.org", "password": ph.hash("hr-lead-password"), "role": "manager", "team": "Human Resources", "permissions": ["dashboard","applications","teams"]},
  {"email": "plain@enactussams.org", "password": "plaintext-legacy-pw", "role": "member", "permissions": ["content"]},
  {"email": "broken@enactussams.org", "password": "", "role": "member"},
])
db.teams.insert_many([
  {"name": "Presentation", "desc": "Legacy presentation desc", "memberList": [{"id": 1, "name": "Omar Hassan", "role": "Head"}, {"id": 2, "name": "Laila Samir", "role": "Member"}]},
  {"name": "Human Resources", "desc": "People people", "memberList": []},
  {"name": " human resources ", "desc": "dupe"},
  {"name": "Robotics", "desc": "A team only the old site had"},
])
db.applications.insert_many([
  {"name": "Mona Ahmed", "email": "Mona@Example.com", "phone": "01012345678", "team": "presentation", "reason": "I love presenting", "status": "pending", "submittedAt": "2025-01-02T10:00:00Z"},
  {"name": "Karim", "email": "karim@example.com", "phone": "+20 100 000 0000", "team": "Robotics", "reason": "", "status": "referred", "referredTo": "Human Resources"},
  {"name": "Bad", "email": "not-an-email", "phone": "1", "team": "x"},
])
db.content.insert_one({
  "heroHeadline1": "Legacy headline", "aboutTags": "Impact, Teams", "recruitmentOpen": True,
  "footerAbout": "Line one\nLine two", "footerSocialFb": "https://facebook.com/enactussams",
  "footerSocialInsta": "not a url", "insideTeams": [{"name": "Presentation", "desc": "Shown inside"}],
  "recruitmentTeams": ["Presentation", "Human Resources", "Robotics"],
  "mediaGallery": [{"url": "/assets/bench.jpg"}, {"url": "assets/glasses.jpg"}, {"url": "https://evil.example/x.jpg"}, {"url": "/assets/../../etc/passwd"}],
  "tafrahSiteImage": "assets/tafrah-monitor.jpg",
})
db.form_schema.insert_one({"title": "Volunteer Day", "description": "x", "fields": [
  {"id": "n", "label": "Full Name", "type": "text", "required": True},
  {"id": "t", "label": "Preferred Team", "type": "select", "required": True, "options": "Presentation, Project Management, HR"},
  {"id": "w", "label": "Why", "type": "textarea"},
]})
db.form_submissions.insert_many([
  {"formTitle": "Volunteer Day", "data": {"Full Name": "Sara", "Preferred Team": "HR", "Why": "=HYPERLINK(\"x\")"}, "submittedAt": "2025-03-01T09:00:00Z"},
  {"formTitle": "Volunteer Day", "data": {"Full Name": "Ali", "Preferred Team": "Nope"}},
])
print("seeded", db.name)
