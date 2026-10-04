# Enactus SAMS — Official Website & Management Platform

A high-performance, production-ready web application and administrative management system built for **Enactus SAMS**. The platform combines a responsive brutalist public website with an administrative panel for recruitment, content management, access control, and team management.

---

## 1. System Architecture

The project is architected with a high-throughput, low-latency backend and a modern responsive frontend:- **Backend Engine (`enactus_backend`)**:
  - Built with **C++20** and the **Drogon HTTP Framework**.
  - Powered by the **Anvil** core foundation library (structured concurrency, connection pooling, and security filters).
  - Database: **MongoDB** (`mongocxx` driver) for structured document storage (applications, teams, content, system logs, users).
  - Cache: **Redis** for fast session verification and rate limiting.
  - Pre-compression: Native runtime support for **Brotli (`.br`)** and **Gzip (`.gz`)** static asset delivery.
  - Server-Side Media Processing: Embedded **stb** image pipeline (`stb_image`, `stb_image_resize2`, `stb_image_write`) automatically downscales uploads > 1600px and re-encodes to optimized JPEG (quality 82).
- **Admin Control Panel (`/admin`)**:
  - Built with **React 18**, **TypeScript**, and **Vite**.
  - Styled with a high-contrast brutalist design language (`#FFC629`, `#0E1013`, `#F7F5F0`, hard shadows, monospace accents).
  - Component library using **Lucide Icons** and modular panel architecture.
- **Public Marketing Website (`/`)**:
  - Lightweight, responsive client architecture with zero runtime framework bloat.
  - Dynamic integration with backend APIs for real-time team listings, recruitment status, and media galleries.
  - Optimized responsive media delivery with native `loading="lazy"` on gallery assets.

---

## 2. Key Features

### Public Website
- **Brutalist Aesthetic**: Distinct typography, vibrant yellow accents, hard drop-shadows, and responsive layout for all device viewports.
- **Favicon & Identity**: Full multi-resolution `favicon.ico` (32x32, 16x16) and `apple-touch-icon.png` (180x180) across public pages, application forms, and admin dashboard.
- **Dynamic "Inside the Club" Showcase**: Live team cards dynamically populated from the database, featuring distinctive SVG iconography for each team (Presentation, Project Management, Human Resources, External Affairs, Social Media, Media Production).
- **Recruitment Application**: Integrated application form with real-time field validation, dynamic team selection, and instant submission feedback.
- **Campus Life Gallery**: Responsive image grid with full-screen interactive lightbox viewer and lazy-loaded assets.
- **Project Tafrah Section**: Dedicated project showcase with high-resolution imagery and narrative impact metrics.
- **Real-Time Traffic Tracking**: Asynchronous visit counter logging visits to MongoDB on every page load.
- **Social & Footer**: Links to official channels (TikTok, Facebook, Instagram).

### Admin Management Platform
- **Platform Analytics & Overview**:
  - Real-time visit counts and application conversion rate.
  - Interactive recruitment pipeline visualization (Accepted, Interviews Scheduled, Pending, Rejected).
  - Trending team metrics highlighting high-volume recruitment areas.
- **Live System Activity Logs**:
  - Database-backed event auditing (`application.logs`) tracking all critical system events: logins, user modifications, team updates, application reviews/referrals, and content saves.
  - Relative time indicators (`JUST NOW`, `15M AGO`, `2H AGO`), categorized event badges, and manual refresh controls.
- **Granular Access Control (RBAC)**:
  - Role hierarchy: `superadmin`, `high board`, `director`, `manager`, `vice_manager`, `member`, `hr`.
  - Panel-level permissions: `dashboard`, `applications`, `form_maker`, `teams`, `content`, `gallery`, `users`.
  - User permission editing: Update any user's role, permissions, and team assignment.
  - Super admin safeguards: Built-in protections preventing modification or deletion of the primary super admin account.
- **Logged-in User Role Badge**:
  - High-visibility brutalist yellow box in the sidebar displaying the active user's role, assigned team, and email directly above the sign-out action.
- **Content CMS**:
  - In-place editing of all website sections: Hero Section, Recruitment Status & Deadlines, About Section, Inside the Club, Media Gallery, Project Tafrah, and Footer/Socials.
  - **Tab 04 Inside the Club**: Dedicated showcase for public homepage team cards (`content.insideTeams`), allowing inline editing of titles, descriptions, adding new cards, or deleting cards. Kept strictly decoupled from operational recruitment teams.
  - **Tab 06 Join Us / Recruitment**: Complete control over application form team choices (`content.recruitmentTeams`) with inline editing, adding, and removing choices displayed on the public site application form.
- **Applications & Recruitment Workflow**:
  - Centralized application review table with status badges and detail view modals.
  - Actions: Accept, Reject, Delete (Super Admin only), and **Refer**.
  - **Referral Workflow**: Allows referring candidates to another team; the target team manager receives the referral in their scoped dashboard with dedicated Accept/Reject referral actions.
  - **Automatic Team Roster Sync**: Accepting an applicant automatically adds them to that team's active member list in the database and updates the team card.
  - **Date Sorting**: Sort applications from Newest to Oldest or Oldest to Newest with a single click.
  - **Team Filtering**: Filter by "All Teams", select any individual team, or toggle multiple teams via interactive chips.
  - **Search & Inspection**: Instant search across applicant names, emails, phones, and submission reasons.
  - **CSV Export**: Dedicated "Export CSV" button that generates a downloadable report of applicants respecting active filters and sort order, with columns for Name, Email, Phone, Team Applied, Status (dynamically reflecting the applicant's current status), Referred To, Date Submitted, and Reason / Notes.
- **Form Maker & Form Responses**:
  - Create and publish custom forms with shareable public links.
  - **Form Responses Panel**: Review, search, and inspect form submissions.
  - **Date Sorting**: Sort responses from Newest to Oldest or Oldest to Newest.
  - **Form Filtering**: Filter responses by "All Forms", select any individual form, or toggle multiple forms via interactive chips.
  - **Delete Responses**: Delete individual submissions directly from table rows or within the details modal, protected by the brutalist confirmation dialog.
  - **CSV Export**: Export all form responses to CSV respecting active filters and sort order, with dynamic columns based on submitted fields.
- **Team Management**:
  - **Team Creation & Editing**: Create new teams and edit the title or description of any team via the Edit Details modal.
  - **Roster Management**: Add, remove, and adjust roles of active members within each team.
  - **Automatic Recruitment & Access Sync**: Creating or renaming a team in Manage Teams automatically syncs it to the public recruitment choices (`recruitmentTeams`), user team assignments, and Access Control manager/vice-manager assignment dropdowns, while keeping the public homepage "Inside the Club" section (`insideTeams`) completely independent.
  - Scoped permissions: Team managers can only manage members within their assigned team.

---

## 3. Directory Structure

```
├── admin/                  # Admin panel frontend (React + TypeScript + Vite)
│   ├── src/
│   │   ├── pages/          # Panel views (Dashboard, Applications, Content, Teams, Users, etc.)
│   │   ├── App.tsx         # Admin layout, sidebar, role badge, routing
│   │   └── main.tsx        # React entry point
│   ├── package.json
│   └── vite.config.ts
├── anvil/                  # Anvil C++ foundational library & security services
│   ├── include/anvil/      # Public headers (access control, db, http, crypto, etc.)
│   ├── src/                # Anvil core implementation
│   ├── docs/               # In-depth architectural & subsystem documentation
│   └── ENGINEERING_RULES.md# Core engineering and code discipline rules
├── hammer/                 # Frontend client library & TypeScript SDK
│   ├── src/                # Core client implementation
│   └── docs/               # Hammer subsystem documentation
├── public/                 # Static web assets & public site
│   ├── index.html          # Public website HTML & interactive components
│   ├── admin/              # Compiled admin dashboard production build
│   └── assets/             # Compressed images, styles, and scripts
├── src/                    # Backend application source
│   ├── handlers/
│   │   └── api.cc          # Main API endpoints (auth, users, teams, content, logs, etc.)
│   └── main.cc             # Server startup, route definitions, and asset serving
├── uploads/                # File uploads storage (media, project imagery)
├── docker-compose.yml      # Local MongoDB & Redis service definitions
├── CMakeLists.txt          # CMake build configuration
├── CMakePresets.json       # CMake presets (asan, release, etc.)
├── vcpkg.json              # C++ dependencies manifest
└── README.md               # Project documentation
```

---

## 4. Setup & Installation

### Prerequisites
- **Operating System**: Linux (Ubuntu 22.04+, Debian 12+, Arch Linux, or compatible)
- **Compiler**: GCC 12+ or Clang 15+ supporting C++20
- **Build Tools**: CMake 3.25+, Ninja, vcpkg
- **Runtime Services**: MongoDB 6.0+, Redis 7.0+
- **Node.js**: Node.js 18+ and npm

### 1. Start Database Services
Using Docker Compose:
```bash
docker compose up -d
```
This starts:
- MongoDB on `mongodb://127.0.0.1:27017`
- Redis on `127.0.0.1:6379`

### 2. Build the C++ Backend
Install dependencies and compile using CMake presets:
```bash
# Configure with ASan preset (or release)
cmake --preset asan

# Compile the enactus_backend executable
cmake --build --preset asan --target enactus_backend -j$(nproc)
```

### 3. Build the Admin Frontend
```bash
cd admin
npm install
npm run build
cd ..

# Deploy admin build to public/admin
rm -rf public/admin
cp -r admin/dist public/admin
```

### 4. Pre-compress Static Assets
To ensure maximum transfer speed over the web, pre-compress all public assets with Gzip and Brotli:
```bash
find public/ -type f \( -name "*.html" -o -name "*.js" -o -name "*.css" -o -name "*.svg" -o -name "*.json" \) \
  -exec gzip -k -9 -f {} + \
  -exec brotli -k -q 11 -f {} +
```

### 5. Run the Server
```bash
export JWT_SECRET="$(openssl rand -base64 48)"   # store it securely; changing it signs everyone out
./build/asan/enactus_backend
```

#### Environment Variables
The server reads configuration from environment variables (with sensible local defaults):

| Variable | Description | Default |
|---|---|---|
| `PORT` | HTTP port to listen on | `8080` (or `8085` in production) |
| `BIND_ADDR` | Network interface IP address to bind | `0.0.0.0` (or `127.0.0.1` behind reverse proxy) |
| `DOC_ROOT` | Document root directory for static files | `public` |
| `MONGODB_URI` | MongoDB connection URI with optional auth credentials | `mongodb://127.0.0.1:27017/application` |
| `JWT_SECRET` | **Required.** Session-token signing key, at least 32 bytes. The server refuses to start without it. Generate one with `openssl rand -base64 48` and keep it out of the repository. | — |
| `TRUST_PROXY` | Set to `1` only when the backend sits behind a reverse proxy you control that sets `X-Real-IP`/`X-Forwarded-For`, and the backend port is not reachable directly. Rate limits then key on the real client IP instead of the proxy's. | unset |

The application will listen on the configured port:
- Public Website: `http://localhost:8080/`
- Admin Dashboard: `http://localhost:8080/admin/`

---

## 5. API Reference

| Method | Endpoint | Description | Auth Required |
|---|---|---|---|
| `POST` | `/api/auth/login` | Authenticate user and return session token & permissions | No |
| `POST` | `/api/auth/logout` | Sign out (invalidates every session token issued to the account so far) | Signed in |
| `GET` | `/api/auth/me` | Current account's role, team and permissions | Signed in |
| `GET` | `/api/analytics` | Retrieve total website visits and metrics | `dashboard` |
| `POST` | `/api/track_visit` | Increment website visit counter (rate limited per IP) | No |
| `GET` | `/api/logs` | Fetch recent database system activity logs | `dashboard` |
| `GET` | `/api/users` | List active users and their assigned roles/teams | `users` |
| `POST` | `/api/users` | Create or update user permissions, role, and team | `users` |
| `DELETE` | `/api/users` | Revoke access / delete user account | `users` |
| `GET` | `/api/teams` | List teams (public: name/description/count; signed in: full roster) | No |
| `POST` | `/api/teams` | Team management actions (`create`, `update`, `delete`, `update_roster`) | `teams` or `content` (`update_roster`: `teams`) |
| `GET` | `/api/content` | Fetch live website content configuration | No |
| `POST` | `/api/content` | Update website content across all sections | `content` (`gallery` may update image fields only) |
| `POST` | `/api/applications` | Submit a member recruitment application | No |
| `GET` | `/api/applications_list` | List submitted applications (scoped by user role) | `applications` (`dashboard`: statistics only, no applicant details) |
| `POST` | `/api/applications_update` | Update application status (accept/reject/refer; delete is super admin only) | `applications` |
| `GET` | `/api/form_schema` | Fetch the published custom form | No |
| `POST` | `/api/form_schema` | Publish the custom form | `form_maker` |
| `GET` | `/api/form_submissions` | List responses collected from custom published forms | `applications` or `form_maker` |
| `POST` | `/api/form_submissions` | Submit form response, or delete one (`action: "delete"`, needs `applications` or `form_maker`) | No |
| `DELETE` | `/api/form_submissions` | Permanently delete a form submission by ID | `applications` or `form_maker` |
| `POST` | `/api/upload` | Upload a JPEG or PNG image (≤ 15 MB; re-encoded to JPEG, downscaled > 1600px) | `content` or `gallery` |

Super admins hold every permission. Managers and vice managers with an assigned team are additionally scoped to that team: they only see and act on its applications (including candidates referred to it), can only edit its details and roster, and cannot create or delete teams.

---

## 6. Security & Engineering Standards

- **Server-side authorization**: Every administrative endpoint verifies the session token, re-loads the account from MongoDB (so role, team and permissions always reflect the database, and deleted or revoked accounts lose access immediately), and checks the permission listed in the API table above. The admin panel's menus are a convenience only; the API is the enforcement point.
- **Privilege boundaries**: Only a super admin can assign the `superadmin` role or modify/remove a super admin account, and a user with `users` permission can only grant permissions they hold themselves. The primary super admin account (`admin@enactussams.org`) cannot be modified or deleted through the API.
- **Sessions**: HMAC-SHA256 tokens valid for 12 hours, signed with the mandatory `JWT_SECRET`. Signing out, or a password change, invalidates every token issued to that account before that moment.
- **Passwords**: Argon2id hashes only; legacy plaintext values are migrated at boot. Login is rate limited per IP and per account, and unknown accounts cost the same time as known ones so emails cannot be enumerated by timing.
- **Input validation**: Request fields are type-checked and length-bounded; user input embedded in MongoDB regex lookups is escaped; client JSON stored as documents is checked for operator (`$`) and dotted keys before it reaches BSON.
- **Uploads**: Only JPEG and PNG are accepted. Every upload is decoded, size-checked against decompression bombs, and re-encoded as a fresh JPEG under a server-chosen name. Only the JPEG and PNG decoders are compiled into the binary.
- **Output encoding**: The public site escapes all CMS and team data before inserting it into the page and only allows `http(s)` or site-relative URLs in links and images.
- **HTTP headers**: Content-Security-Policy (strict script policy on the admin panel), HSTS, `X-Content-Type-Options`, `X-Frame-Options`, `Referrer-Policy`, `Permissions-Policy`, `Cross-Origin-Opener-Policy`; API responses are `Cache-Control: no-store`.
- **Infrastructure**: `docker-compose.yml` publishes MongoDB and Redis on `127.0.0.1` only. Run them with authentication enabled in production and never expose them to the internet.
- **Memory Safety**: Clean modern C++20 patterns throughout, using RAII, stack allocations where sizes are fixed, and AddressSanitizer (ASan) verification in development builds.

## 7. License

This repository is proprietary software belonging to **Enactus SAMS**. All rights reserved.
