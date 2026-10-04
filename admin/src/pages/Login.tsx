import { useEffect, useState } from 'react';
import type { Me } from '../app/responses';
import { platform } from '../app/platform';
import { describe, openSession } from '../app/ui';

// Sign-in through anvil's account flow. The password never leaves this tab:
// hammer asks the salt route for this email's salt, runs Argon2id in a worker,
// and sends only the derived credential. The session itself is a pair of
// HttpOnly cookies this code never sees.
export default function Login({ onLogin, initialError }: { onLogin: (me: Me) => void; initialError?: string }) {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState(initialError || '');
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    if (initialError) setError(initialError);
  }, [initialError]);

  const handleLogin = async (e: React.FormEvent) => {
    e.preventDefault();
    setError('');
    setLoading(true);
    const controller = new AbortController();
    const signedIn = await platform.accounts.signIn({ identifier: email.trim(), password }, controller.signal);
    if (!signedIn.ok) {
      setLoading(false);
      const failure = signedIn.error;
      // A wrong password and an unknown email answer identically on purpose.
      setError(failure.kind === 'server' && failure.code === 'UNAUTHENTICATED'
        ? 'Invalid email or password.'
        : describe(failure));
      return;
    }
    setPassword('');
    const me = await openSession(controller.signal);
    setLoading(false);
    if (me === null) {
      setError('Signed in, but the session could not be opened. Try again.');
      return;
    }
    onLogin(me);
  };

  const inputStyle = { width: '100%', padding: '14px 16px', border: '2.5px solid #0E1013', borderRadius: '0', background: '#F7F5F0', fontSize: '15px', fontFamily: 'Archivo, sans-serif', fontWeight: 700, outline: 'none', boxShadow: '4px 4px 0px #0E1013', boxSizing: 'border-box' } as const;
  const labelStyle = { fontSize: '11px', fontWeight: 700, display: 'block', marginBottom: '8px', color: '#0E1013', textTransform: 'uppercase', letterSpacing: '0.1em' } as const;

  return (
    <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', minHeight: '100vh', background: '#0E1013' }}>
      <div style={{ width: '100%', maxWidth: '480px', border: '3px solid #0E1013', boxShadow: '10px 10px 0px #FFC629' }}>
        <div style={{ background: '#0E1013', padding: '28px 40px', display: 'flex', alignItems: 'center', gap: '16px', borderBottom: '3px solid #FFC629' }}>
          <img src="/assets/logo-trim.png" alt="Enactus" style={{ height: '48px', objectFit: 'contain' }} />
        </div>

        <div style={{ background: '#FFFFFF', padding: '40px' }}>
          <h1 style={{ margin: '0 0 4px 0', fontSize: '48px', fontWeight: 900, lineHeight: 1, letterSpacing: '-0.03em', textTransform: 'uppercase', color: '#0E1013' }}>
            SAMS<br />Admin.
          </h1>
          <p className="font-mono" style={{ margin: '0 0 36px 0', fontSize: '13px', color: '#777', letterSpacing: '0.05em' }}>
            Admin Panel Login
          </p>

          <form onSubmit={handleLogin} style={{ display: 'flex', flexDirection: 'column', gap: '20px' }}>
            <div>
              <label htmlFor="email" className="font-mono" style={labelStyle}>Email</label>
              <input
                id="email"
                type="email"
                autoComplete="username"
                required
                style={inputStyle}
                value={email}
                onChange={e => setEmail(e.target.value)}
                onBlur={() => { if (email.trim()) platform.accounts.prefetch(email.trim()); }}
                placeholder="you@enactussams.org"
              />
            </div>
            <div>
              <label htmlFor="password" className="font-mono" style={labelStyle}>Password</label>
              <input
                id="password"
                type="password"
                autoComplete="current-password"
                required
                style={inputStyle}
                value={password}
                onChange={e => setPassword(e.target.value)}
                placeholder="••••••••"
              />
            </div>

            {error && (
              <div role="alert" style={{ background: '#FFF0F0', border: '2.5px solid #E53935', padding: '12px 16px', color: '#E53935', fontSize: '14px', fontWeight: 700 }}>
                {error}
              </div>
            )}

            <button
              type="submit"
              disabled={loading}
              style={{ width: '100%', padding: '16px', border: '2.5px solid #0E1013', borderRadius: '0', background: loading ? '#D4A800' : '#FFC629', color: '#0E1013', fontSize: '16px', fontFamily: 'Archivo, sans-serif', fontWeight: 900, textTransform: 'uppercase', letterSpacing: '0.08em', cursor: loading ? 'not-allowed' : 'pointer', boxShadow: '4px 4px 0px #0E1013', marginTop: '4px', transition: 'all 0.1s' }}
            >
              {loading ? 'Signing in…' : 'Login →'}
            </button>
          </form>
        </div>
      </div>
    </div>
  );
}
