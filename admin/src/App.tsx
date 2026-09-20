import { useState } from 'react';
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

function App() {
  const [role, setRole] = useState(localStorage.getItem('admin_role') || 'superadmin'); 
  const [email, setEmail] = useState(localStorage.getItem('admin_email') || '');
  const [team, setTeam] = useState(localStorage.getItem('admin_team') || '');
  const [permissions, setPermissions] = useState<string[]>(() => {
    try {
      return JSON.parse(localStorage.getItem('admin_permissions') || '[]');
    } catch {
      return [];
    }
  });
  const [menuOpen, setMenuOpen] = useState(false);
  const [isAuthenticated, setIsAuthenticated] = useState(localStorage.getItem('admin_auth') === 'true');

  const closeMenu = () => setMenuOpen(false);

  const handleLogout = () => {
    localStorage.removeItem('admin_auth');
    localStorage.removeItem('admin_role');
    localStorage.removeItem('admin_email');
    localStorage.removeItem('admin_team');
    localStorage.removeItem('admin_permissions');
    setIsAuthenticated(false);
  };

  if (!isAuthenticated) {
    return (
      <Login 
        onLogin={(r: string, em?: string, perms?: string[], tm?: string) => { 
          setRole(r); 
          setEmail(em || ''); 
          setPermissions(perms || []);
          setTeam(tm || '');
          setIsAuthenticated(true); 
        }} 
      />
    );
  }

  const isSuperAdmin = role === 'superadmin' || email === 'admin@enactussams.org';
  const hasPerm = (perm: string) => isSuperAdmin || permissions.includes(perm);

  const firstPermittedPath = isSuperAdmin || hasPerm('dashboard') ? '/'
    : hasPerm('applications') ? '/applications'
    : hasPerm('form_maker') ? '/form-maker'
    : hasPerm('teams') ? '/teams'
    : hasPerm('content') ? '/content'
    : hasPerm('gallery') ? '/gallery'
    : (isSuperAdmin || hasPerm('users')) ? '/users'
    : '/';

  return (
    <Router>
      <ConfirmProvider>
        <div style={{ display: 'flex', minHeight: '100vh', width: '100vw', background: 'var(--bg-main)' }}>
        
        {/* Mobile Nav Toggle */}
        <button 
          className="mobile-toggle"
          onClick={() => setMenuOpen(!menuOpen)}
        >
          {menuOpen ? <X /> : <Menu />}
        </button>

        {/* Sidebar */}
        <aside className={`layout-sidebar ${menuOpen ? 'open' : ''}`}>
          <div style={{ marginBottom: '40px' }}>
            <img src="/assets/logo-trim.png" alt="Enactus SAMS" style={{ width: '120px', objectFit: 'contain' }} />
            <p className="font-mono" style={{ margin: '8px 0 0 0', fontSize: '11px', opacity: 0.5, letterSpacing: '0.1em' }}>
              SAMS / MAADI / {isSuperAdmin ? 'SUPERADMIN' : role.toUpperCase()}
            </p>
            {team && !isSuperAdmin && (
              <p className="font-mono" style={{ margin: '4px 0 0 0', fontSize: '10px', color: '#FFC629', letterSpacing: '0.08em', fontWeight: 700, textTransform: 'uppercase' }}>
                TEAM: {team}
              </p>
            )}
          </div>

          <nav style={{ display: 'flex', flexDirection: 'column', gap: '4px', flex: 1, overflowY: 'auto' }}>
            {hasPerm('dashboard') && (
              <NavLink to="/" onClick={closeMenu} className={({isActive}) => `nav-link ${isActive ? 'active' : ''}`}>
                <LayoutDashboard size={18} /> Dashboard
              </NavLink>
            )}
            
            {(hasPerm('applications') || hasPerm('form_maker') || hasPerm('teams')) && (
              <>
                <div style={{ margin: '20px 0 6px', fontSize: '11px', textTransform: 'uppercase', letterSpacing: '0.1em', opacity: 0.5, paddingLeft: '16px' }} className="font-mono">
                  Recruitment & Forms
                </div>
                
                {hasPerm('applications') && (
                  <NavLink to="/applications" onClick={closeMenu} className={({isActive}) => `nav-link ${isActive ? 'active' : ''}`}>
                    <CheckSquare size={18} /> Application Responses
                  </NavLink>
                )}

                {(hasPerm('applications') || hasPerm('form_maker')) && (
                  <NavLink to="/form-responses" onClick={closeMenu} className={({isActive}) => `nav-link ${isActive ? 'active' : ''}`}>
                    <Inbox size={18} /> Form Responses
                  </NavLink>
                )}

                {hasPerm('form_maker') && (
                  <NavLink to="/form-maker" onClick={closeMenu} className={({isActive}) => `nav-link ${isActive ? 'active' : ''}`}>
                    <PenTool size={18} /> Form Maker
                  </NavLink>
                )}

                {hasPerm('teams') && (
                  <NavLink to="/teams" onClick={closeMenu} className={({isActive}) => `nav-link ${isActive ? 'active' : ''}`}>
                    <UsersIcon size={18} /> Manage Teams
                  </NavLink>
                )}
              </>
            )}

            {(hasPerm('content') || hasPerm('gallery')) && (
              <>
                <div style={{ margin: '20px 0 6px', fontSize: '11px', textTransform: 'uppercase', letterSpacing: '0.1em', opacity: 0.5, paddingLeft: '16px' }} className="font-mono">
                  Site Content
                </div>
                {hasPerm('content') && (
                  <NavLink to="/content" onClick={closeMenu} className={({isActive}) => `nav-link ${isActive ? 'active' : ''}`}>
                    <FileText size={18} /> Content CMS
                  </NavLink>
                )}
                {hasPerm('gallery') && (
                  <NavLink to="/gallery" onClick={closeMenu} className={({isActive}) => `nav-link ${isActive ? 'active' : ''}`}>
                    <Image size={18} /> Gallery
                  </NavLink>
                )}
              </>
            )}

            {(isSuperAdmin || hasPerm('users')) && (
              <>
                <div style={{ margin: '20px 0 6px', fontSize: '11px', textTransform: 'uppercase', letterSpacing: '0.1em', opacity: 0.5, paddingLeft: '16px' }} className="font-mono">
                  Administration
                </div>
                <NavLink to="/users" onClick={closeMenu} className={({isActive}) => `nav-link ${isActive ? 'active' : ''}`}>
                  <Shield size={18} /> Access Control
                </NavLink>
              </>
            )}
          </nav>

          <div style={{ marginTop: 'auto', paddingTop: '20px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
            <div style={{
              background: '#FFC629',
              border: '2.5px solid #0E1013',
              padding: '12px 14px',
              boxShadow: '3px 3px 0px #0E1013',
              color: '#0E1013',
              display: 'flex',
              flexDirection: 'column',
              gap: '6px'
            }}>
              <span className="font-mono" style={{ fontSize: '10px', textTransform: 'uppercase', letterSpacing: '0.12em', opacity: 0.8, fontWeight: 700 }}>
                LOGGED IN AS
              </span>
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: '6px' }}>
                <span className="font-mono" style={{ fontSize: '13px', fontWeight: 900, textTransform: 'uppercase', letterSpacing: '0.04em' }}>
                  {isSuperAdmin ? 'SUPER ADMIN' : role.toUpperCase()}
                </span>
                {team && !isSuperAdmin && (
                  <span style={{ background: '#0E1013', color: '#FFC629', fontSize: '10px', padding: '2px 6px', fontWeight: 700, fontFamily: 'IBM Plex Mono', textTransform: 'uppercase' }}>
                    {team}
                  </span>
                )}
              </div>
              <span style={{ fontSize: '12px', fontWeight: 600, opacity: 0.85, wordBreak: 'break-all' }}>
                {email}
              </span>
            </div>

            <button className="nav-link" style={{ width: '100%', color: '#E53935', background: 'transparent', border: 'none', cursor: 'pointer' }} onClick={handleLogout}>
              <LogOut size={18} /> Sign Out
            </button>
          </div>
        </aside>

        {/* Main Content */}
        <main className="layout-main">
          <div style={{ maxWidth: '1200px', margin: '0 auto' }}>
            <Routes>
              <Route path="/" element={hasPerm('dashboard') ? <Dashboard /> : <Navigate to={firstPermittedPath} replace />} />
              <Route path="/applications" element={hasPerm('applications') ? <Applications role={role} /> : <Navigate to={firstPermittedPath} replace />} />
              <Route path="/form-responses" element={hasPerm('applications') || hasPerm('form_maker') ? <FormResponses /> : <Navigate to={firstPermittedPath} replace />} />
              <Route path="/form-maker" element={hasPerm('form_maker') ? <FormMaker /> : <Navigate to={firstPermittedPath} replace />} />
              <Route path="/teams" element={hasPerm('teams') ? <Teams /> : <Navigate to={firstPermittedPath} replace />} />
              <Route path="/content" element={hasPerm('content') ? <Content /> : <Navigate to={firstPermittedPath} replace />} />
              <Route path="/gallery" element={hasPerm('gallery') ? <Gallery /> : <Navigate to={firstPermittedPath} replace />} />
              <Route path="/users" element={(isSuperAdmin || hasPerm('users')) ? <Users /> : <Navigate to={firstPermittedPath} replace />} />
              <Route path="*" element={<Navigate to={firstPermittedPath} replace />} />
            </Routes>
          </div>
        </main>
      </div>
      </ConfirmProvider>
    </Router>
  );
}

export default App;
