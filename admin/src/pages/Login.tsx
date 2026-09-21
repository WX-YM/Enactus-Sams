import { useState, useEffect } from 'react';

export default function Login({ 
  onLogin, 
  initialError 
}: { 
  onLogin: (role: string, email?: string, permissions?: string[], team?: string) => void;
  initialError?: string;
}) {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState(initialError || '');
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    if (initialError) setError(initialError);
  }, [initialError]);

  const handleLogin = (e: React.FormEvent) => {
    e.preventDefault();
    setLoading(true);
    fetch('/api/auth/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email, password })
    })
    .then(res => res.json())
    .then(data => {
      setLoading(false);
      if (data.status === 'ok') {
        const role = data.role || 'superadmin';
        const perms = data.permissions || [];
        const team = data.team || '';
        const now = Date.now();
        const expiresInSec = data.expires_in || (12 * 3600); // 12 hours
        const expiresAt = now + expiresInSec * 1000;

        localStorage.setItem('admin_auth', 'true');
        if (data.token) {
          localStorage.setItem('admin_token', data.token);
        }
        localStorage.setItem('admin_role', role);
        localStorage.setItem('admin_email', email);
        localStorage.setItem('admin_team', team);
        localStorage.setItem('admin_permissions', JSON.stringify(perms));
        localStorage.setItem('admin_session_expires_at', String(expiresAt));
        localStorage.setItem('admin_last_activity', String(now));
        onLogin(role, email, perms, team);
      } else {
        setError(data.message || 'Invalid email or password');
      }
    })
    .catch(() => { setLoading(false); setError('Failed to connect to backend'); });
  };

  return (
    <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', minHeight: '100vh', background: '#0E1013' }}>
      <div style={{ width: '100%', maxWidth: '480px', border: '3px solid #0E1013', boxShadow: '10px 10px 0px #FFC629' }}>

        {/* Dark header band with logo */}
        <div style={{ background: '#0E1013', padding: '28px 40px', display: 'flex', alignItems: 'center', gap: '16px', borderBottom: '3px solid #FFC629' }}>
          <img src="/assets/logo-trim.png" alt="Enactus" style={{ height: '48px', objectFit: 'contain' }} />
        </div>

        {/* White form body */}
        <div style={{ background: '#FFFFFF', padding: '40px' }}>
          <h1 style={{ margin: '0 0 4px 0', fontSize: '48px', fontWeight: 900, lineHeight: 1, letterSpacing: '-0.03em', textTransform: 'uppercase', color: '#0E1013' }}>
            SAMS<br/>Admin.
          </h1>
          <p className="font-mono" style={{ margin: '0 0 36px 0', fontSize: '13px', color: '#777', letterSpacing: '0.05em' }}>
            Admin Panel Login
          </p>

          <form onSubmit={handleLogin} style={{ display: 'flex', flexDirection: 'column', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '11px', fontWeight: 700, display: 'block', marginBottom: '8px', color: '#0E1013', textTransform: 'uppercase', letterSpacing: '0.1em' }}>Email</label>
              <input
                type="email"
                style={{ width: '100%', padding: '14px 16px', border: '2.5px solid #0E1013', borderRadius: '0', background: '#F7F5F0', fontSize: '15px', fontFamily: 'Archivo, sans-serif', fontWeight: 700, outline: 'none', boxShadow: '4px 4px 0px #0E1013', boxSizing: 'border-box' }}
                value={email}
                onChange={e => setEmail(e.target.value)}
                placeholder="admin@enactussams.org"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '11px', fontWeight: 700, display: 'block', marginBottom: '8px', color: '#0E1013', textTransform: 'uppercase', letterSpacing: '0.1em' }}>Password</label>
              <input
                type="password"
                style={{ width: '100%', padding: '14px 16px', border: '2.5px solid #0E1013', borderRadius: '0', background: '#F7F5F0', fontSize: '15px', fontFamily: 'Archivo, sans-serif', fontWeight: 700, outline: 'none', boxShadow: '4px 4px 0px #0E1013', boxSizing: 'border-box' }}
                value={password}
                onChange={e => setPassword(e.target.value)}
                placeholder="••••••••"
              />
            </div>

            {error && (
              <div style={{ background: '#FFF0F0', border: '2.5px solid #E53935', padding: '12px 16px', color: '#E53935', fontSize: '14px', fontWeight: 700 }}>
                {error}
              </div>
            )}

            <button
              type="submit"
              disabled={loading}
              style={{ width: '100%', padding: '16px', border: '2.5px solid #0E1013', borderRadius: '0', background: loading ? '#D4A800' : '#FFC629', color: '#0E1013', fontSize: '16px', fontFamily: 'Archivo, sans-serif', fontWeight: 900, textTransform: 'uppercase', letterSpacing: '0.08em', cursor: loading ? 'not-allowed' : 'pointer', boxShadow: '4px 4px 0px #0E1013', marginTop: '4px', transition: 'all 0.1s' }}
            >
              {loading ? 'Authenticating...' : 'Login →'}
            </button>
          </form>
        </div>
      </div>
    </div>
  );
}
