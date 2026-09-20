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
- **Form Maker**:
  - Create and publish custom forms with shareable public links.
  - Dedicated "Application Responses" section to separate general form submissions from club recruitment.
- **Team Management**:
  - Team creation, title and description editing, and roster management.
  - **Automatic Recruitment & Access Sync**: Creating a team in Manage Teams automatically syncs it to the public recruitment choices (`recruitmentTeams`) and Access Control manager/vice-manager assignment dropdowns, while keeping the public homepage "Inside the Club" section (`insideTeams`) completely independent.
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

The application will listen on the configured port:
- Public Website: `http://localhost:8080/`
- Admin Dashboard: `http://localhost:8080/admin/`

---

## 5. API Reference

| Method | Endpoint | Description | Auth Required |
|---|---|---|---|
| `POST` | `/api/auth/login` | Authenticate user and return session token & permissions | No |
| `GET` | `/api/analytics` | Retrieve total website visits and metrics | Yes |
| `POST` | `/api/track_visit` | Increment website visit counter | No |
| `GET` | `/api/logs` | Fetch recent database system activity logs | Yes |
| `GET` | `/api/users` | List active users and their assigned roles/teams | Yes |
| `POST` | `/api/users` | Create or update user permissions, role, and team | Yes |
| `DELETE` | `/api/users` | Revoke access / delete user account | Yes (Super Admin) |
| `GET` | `/api/teams` | List active teams, descriptions, and member counts | No |
| `POST` | `/api/teams` | Team management actions (`create`, `update`, `delete`, `update_roster`) | Yes |
| `GET` | `/api/content` | Fetch live website content configuration | No |
| `POST` | `/api/content` | Update website content across all sections | Yes |
| `POST` | `/api/apply` | Submit a member recruitment application | No |
| `GET` | `/api/applications_list` | List submitted applications (scoped by user role) | Yes |
| `POST` | `/api/applications_update` | Update application status (accept/reject/refer/delete) | Yes |
| `POST` | `/api/upload` | Upload media assets (auto-downscales > 1600px, JPEG quality 82) | Yes |

---

## 6. Security & Engineering Standards

- **Role-Based Access Control (RBAC)**: All administrative endpoints validate user permissions against MongoDB and Redis session stores.
- **Super Admin Protection**: The primary super admin account (`admin@enactussams.org`) is protected at the API layer against modification, permission demotion, or deletion.
- **Input Validation**: All incoming requests are strictly checked and sanitized to prevent injection attacks and memory corruption.
- **Memory Safety**: Clean modern C++20 patterns throughout, using RAII, stack allocations where sizes are fixed, and AddressSanitizer (ASan) verification in development builds.

---

## 7. License

This repository is proprietary software belonging to **Enactus SAMS**. All rights reserved.
