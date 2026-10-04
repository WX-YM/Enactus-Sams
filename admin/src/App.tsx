import { useEffect, useState } from 'react';
import type { ReactNode } from 'react';
import { HashRouter as Router, Routes, Route, NavLink, Navigate } from 'react-router-dom';
import { LayoutDashboard, Users as UsersIcon, FileText, CheckSquare, Menu, X, Shield, LogOut, Image, PenTool, Inbox } from 'lucide-react';
import Dashboard from './pages/Dashboard';
import Teams from './pages/Teams';
import Content from './pages/Content';
import Applications from './pages/Applications';
import Gallery from './pages/Gallery';
import FormMaker from './pages/FormMaker';
import FormResponses from './pages/FormResponses';
import Users from './pages/Users';
import Login from './pages/Login';
import { ConfirmProvider } from './context/ConfirmContext';
import type { Me } from './app/responses';
import { SessionProvider, openSession, useSession } from './app/ui';
import {
  routeApplicationsList,
  routeDashboardGet,
  routeFormsCreate,
  routeGalleryList,
  routeResponsesList,
  routeSectionsList,
  routeStaffList,
  routeTeamsCreate,
} from './api/hammer.generated';

type State =
  | { readonly status: 'loading' }
  | { readonly status: 'signed-out'; readonly message: string }
  | { readonly status: 'signed-in'; readonly me: Me };

function App() {
  const [state, setState] = useState<State>({ status: 'loading' });

  // On load the session is whatever the cookies say: hammer refreshes once if
  // the access token has lapsed, and a null view means there is no session.
  useEffect(() => {
    const controller = new AbortController();
    void openSession(controller.signal).then((me) => {
      if (controller.signal.aborted) return;
      setState(me === null ? { status: 'signed-out', message: '' } : { status: 'signed-in', me });
    });
    return () => controller.abort();
  }, []);

  if (state.status === 'loading') {
    return <div style={{ minHeight: '100vh', background: '#0E1013' }} aria-busy="true" />;
  }
  if (state.status === 'signed-out') {
    return (
      <Login
        initialError={state.message}
        onLogin={(me) => setState({ status: 'signed-in', me })}
      />
    );
  }
  return (
    <SessionProvider
      me={state.me}
      onSignedOut={(message) => setState({ status: 'signed-out', message })}
      onRefreshed={(me) => setState({ status: 'signed-in', me })}
    >
      <Shell />
    </SessionProvider>
  );
}

const sectionLabel = { margin: '20px 0 6px', fontSize: '11px', textTransform: 'uppercase', letterSpacing: '0.1em', opacity: 0.5, paddingLeft: '16px' } as const;

function Shell(): ReactNode {
  const { me, affords, superadmin, signOut, refresh } = useSession();
  const [menuOpen, setMenuOpen] = useState(false);
  const closeMenu = () => setMenuOpen(false);

  // A permission change made by someone else reaches this tab on the next
  // request anyway (the server bumps the epoch); re-reading the session on
  // focus also refreshes what the navigation offers.
  useEffect(() => {
    const onFocus = () => refresh();
    window.addEventListener('focus', onFocus);
    return () => window.removeEventListener('focus', onFocus);
  }, [refresh]);

  const can = {
    dashboard: affords(routeDashboardGet),
    applications: affords(routeApplicationsList),
    responses: affords(routeResponsesList),
    forms: affords(routeFormsCreate),
    teams: affords(routeTeamsCreate),
    content: affords(routeSectionsList),
    gallery: affords(routeGalleryList),
    users: affords(routeStaffList),
  };
  const roleLabel = superadmin ? 'SUPER ADMIN' : me.role.toUpperCase();
  const firstPath = can.dashboard ? '/' : can.applications ? '/applications' : can.forms ? '/form-maker'
    : can.responses ? '/form-responses' : can.teams ? '/teams' : can.content ? '/content'
    : can.gallery ? '/gallery' : can.users ? '/users' : '/none';
  const guard = (allowed: boolean, page: ReactNode) => (allowed ? page : <Navigate to={firstPath} replace />);

  return (
    <Router>
      <ConfirmProvider>
        <div style={{ display: 'flex', minHeight: '100vh', width: '100%', maxWidth: '100vw', overflowX: 'hidden', background: 'var(--bg-main)' }}>
          <button className="mobile-toggle" aria-label="Menu" onClick={() => setMenuOpen(!menuOpen)}>
            {menuOpen ? <X /> : <Menu />}
          </button>

          <aside className={`layout-sidebar ${menuOpen ? 'open' : ''}`}>
            <div style={{ marginBottom: '40px' }}>
              <img src="/assets/logo-trim.png" alt="Enactus SAMS" style={{ width: '120px', objectFit: 'contain' }} />
              <p className="font-mono" style={{ margin: '8px 0 0 0', fontSize: '11px', opacity: 0.5, letterSpacing: '0.1em' }}>
                SAMS / MAADI / {roleLabel}
              </p>
              {me.team && !superadmin && (
                <p className="font-mono" style={{ margin: '4px 0 0 0', fontSize: '10px', color: '#FFC629', letterSpacing: '0.08em', fontWeight: 700, textTransform: 'uppercase' }}>
                  TEAM: {me.team}
                </p>
              )}
            </div>

            <nav style={{ display: 'flex', flexDirection: 'column', gap: '4px', flex: 1, overflowY: 'auto' }}>
              {can.dashboard && (
                <NavLink to="/" end onClick={closeMenu} className={({ isActive }) => `nav-link ${isActive ? 'active' : ''}`}>
                  <LayoutDashboard size={18} /> Dashboard
                </NavLink>
              )}

              {(can.applications || can.forms || can.responses || can.teams) && (
                <>
                  <div style={sectionLabel} className="font-mono">Recruitment & Forms</div>
                  {can.applications && (
                    <NavLink to="/applications" onClick={closeMenu} className={({ isActive }) => `nav-link ${isActive ? 'active' : ''}`}>
                      <CheckSquare size={18} /> Application Responses
                    </NavLink>
                  )}
                  {can.responses && (
                    <NavLink to="/form-responses" onClick={closeMenu} className={({ isActive }) => `nav-link ${isActive ? 'active' : ''}`}>
                      <Inbox size={18} /> Form Responses
                    </NavLink>
                  )}
                  {can.forms && (
                    <NavLink to="/form-maker" onClick={closeMenu} className={({ isActive }) => `nav-link ${isActive ? 'active' : ''}`}>
                      <PenTool size={18} /> Form Maker
                    </NavLink>
                  )}
                  {can.teams && (
                    <NavLink to="/teams" onClick={closeMenu} className={({ isActive }) => `nav-link ${isActive ? 'active' : ''}`}>
                      <UsersIcon size={18} /> Manage Teams
                    </NavLink>
                  )}
                </>
              )}

              {(can.content || can.gallery) && (
                <>
                  <div style={sectionLabel} className="font-mono">Site Content</div>
                  {can.content && (
                    <NavLink to="/content" onClick={closeMenu} className={({ isActive }) => `nav-link ${isActive ? 'active' : ''}`}>
                      <FileText size={18} /> Content CMS
                    </NavLink>
                  )}
                  {can.gallery && (
                    <NavLink to="/gallery" onClick={closeMenu} className={({ isActive }) => `nav-link ${isActive ? 'active' : ''}`}>
                      <Image size={18} /> Gallery
                    </NavLink>
                  )}
                </>
              )}

              {can.users && (
                <>
                  <div style={sectionLabel} className="font-mono">Administration</div>
                  {can.users && (
                    <NavLink to="/users" onClick={closeMenu} className={({ isActive }) => `nav-link ${isActive ? 'active' : ''}`}>
                      <Shield size={18} /> Access Control
                    </NavLink>
                  )}
                </>
              )}
            </nav>

            <div style={{ marginTop: 'auto', paddingTop: '20px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
              <div style={{ background: '#FFC629', border: '2.5px solid #0E1013', padding: '12px 14px', boxShadow: '3px 3px 0px #0E1013', color: '#0E1013', display: 'flex', flexDirection: 'column', gap: '6px' }}>
                <span className="font-mono" style={{ fontSize: '10px', textTransform: 'uppercase', letterSpacing: '0.12em', opacity: 0.8, fontWeight: 700 }}>
                  LOGGED IN AS
                </span>
                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: '6px' }}>
                  <span className="font-mono" style={{ fontSize: '13px', fontWeight: 900, textTransform: 'uppercase', letterSpacing: '0.04em' }}>
                    {roleLabel}
                  </span>
                  {me.team && !superadmin && (
                    <span style={{ background: '#0E1013', color: '#FFC629', fontSize: '10px', padding: '2px 6px', fontWeight: 700, fontFamily: 'IBM Plex Mono', textTransform: 'uppercase' }}>
                      {me.team}
                    </span>
                  )}
                </div>
                <span style={{ fontSize: '12px', fontWeight: 600, opacity: 0.85, wordBreak: 'break-all' }}>{me.email}</span>
              </div>

              <button className="nav-link" style={{ width: '100%', color: '#E53935', background: 'transparent', border: 'none', cursor: 'pointer' }} onClick={signOut}>
                <LogOut size={18} /> Sign Out
              </button>
            </div>
          </aside>

          <main className="layout-main">
            <div style={{ maxWidth: '1200px', margin: '0 auto' }}>
              <Routes>
                <Route path="/" element={guard(can.dashboard, <Dashboard />)} />
                <Route path="/applications" element={guard(can.applications, <Applications />)} />
                <Route path="/form-responses" element={guard(can.responses, <FormResponses />)} />
                <Route path="/form-maker" element={guard(can.forms, <FormMaker />)} />
                <Route path="/teams" element={guard(can.teams, <Teams />)} />
                <Route path="/content" element={guard(can.content, <Content />)} />
                <Route path="/gallery" element={guard(can.gallery, <Gallery />)} />
                <Route path="/users" element={guard(can.users, <Users />)} />
                <Route path="/none" element={<NoAccess />} />
                <Route path="*" element={<Navigate to={firstPath} replace />} />
              </Routes>
            </div>
          </main>
        </div>
      </ConfirmProvider>
    </Router>
  );
}

function NoAccess(): ReactNode {
  return (
    <div className="card">
      <h1 className="heading-lg">No access yet.</h1>
      <p>Your account has no sections assigned. Ask whoever manages Access Control to grant you one.</p>
    </div>
  );
}

export default App;
