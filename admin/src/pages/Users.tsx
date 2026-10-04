import { useState } from 'react';
import { UserPlus, Ban, CheckCircle, X } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { api, platform } from '../app/platform';
import type { StaffAccount } from '../app/responses';
import { ErrorBanner, describe, run, useLoad, useSession } from '../app/ui';
import { routeStaffCreate, routeStaffDisable, routeStaffList, routeStaffUpdate, routeTeamsList } from '../api/hammer.generated';

// The grantable permissions (src/config/perms.h kGrantable) and their words.
// Implied ones (media upload, reading form responses) follow from these
// server-side and are never granted directly.
const kPermissions = [
  { name: 'dashboard', label: 'Dashboard' },
  { name: 'applications', label: 'Applications' },
  { name: 'form_maker', label: 'Form Maker' },
  { name: 'teams', label: 'Teams' },
  { name: 'content', label: 'Content CMS' },
  { name: 'gallery', label: 'Gallery' },
  { name: 'users', label: 'Access Control' },
] as const;

const kRoles = ['member', 'HR', 'manager', 'vice manager', 'director', 'high board'] as const;

type Editing = { account: StaffAccount | null; email: string; password: string; role: string; team: string; permissions: string[]; superadmin: boolean };

export default function Users() {
  const { confirm } = useConfirm();
  const { me, superadmin } = useSession();
  const staff = useLoad((signal) => api.call(routeStaffList, { signal }), []);
  const teams = useLoad((signal) => api.call(routeTeamsList, { signal }), []);
  const [editing, setEditing] = useState<Editing | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  // A non-superadmin may grant only what they hold; the server refuses the rest.
  const grantable = kPermissions.filter((p) => superadmin || me.permissions.includes(p.name));

  const save = async () => {
    if (editing === null) return;
    setSaving(true);
    setError(null);
    const controller = new AbortController();
    if (editing.account === null) {
      const email = editing.email.trim();
      const credential = await platform.enrolment.credential(email, editing.password, controller.signal, 'enroll');
      if (!credential.ok) {
        setSaving(false);
        setError(describe(credential.error));
        return;
      }
      const result = await api.call(routeStaffCreate, {
        body: {
          email,
          credential: credential.value,
          role: editing.role,
          team: editing.team,
          permissions: editing.permissions,
          ...(superadmin && editing.superadmin ? { superadmin: true } : {}),
        },
        signal: controller.signal,
      });
      setSaving(false);
      if (!result.ok) { setError(describe(result.error)); return; }
    } else {
      const account = editing.account;
      const body: Record<string, unknown> = { version: account.version, role: editing.role, team: editing.team, permissions: editing.permissions };
      if (superadmin && editing.superadmin !== (account.type === 'superadmin')) body.superadmin = editing.superadmin;
      const result = await api.call(routeStaffUpdate, { params: { id: account.id }, body, signal: controller.signal });
      setSaving(false);
      if (!result.ok) { setError(describe(result.error)); return; }
    }
    setEditing(null);
    staff.reload();
  };

  const toggleActive = async (account: StaffAccount) => {
    setError(null);
    if (account.status === 'active') {
      const ok = await confirm({ title: 'Disable account', message: `Disable ${account.email}? They are signed out everywhere immediately.`, type: 'danger', confirmText: 'Disable' });
      if (!ok) return;
      const result = await run((signal) => api.call(routeStaffDisable, { params: { id: account.id }, body: {}, signal }));
      if (!result.ok) setError(describe(result.error));
    } else {
      const result = await run((signal) => api.call(routeStaffUpdate, { params: { id: account.id }, body: { version: account.version, active: true }, signal }));
      if (!result.ok) setError(describe(result.error));
    }
    staff.reload();
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'end', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">07 — Administration</span>
          <h1 className="heading-lg">Access Control.</h1>
        </div>
        <button className="btn-primary" style={{ display: 'flex', gap: '8px', alignItems: 'center' }} onClick={() => { setError(null); setEditing({ account: null, email: '', password: '', role: 'member', team: '', permissions: [], superadmin: false }); }}>
          <UserPlus size={16} /> New account
        </button>
      </div>
      <ErrorBanner message={staff.error ?? (editing === null ? error : null)} />

      <div className="card" style={{ padding: 0, overflowX: 'auto' }}>
        <table className="table">
          <thead><tr><th>Account</th><th>Role</th><th>Access</th><th>Status</th><th /></tr></thead>
          <tbody>
            {(staff.data?.staff ?? []).map((account) => (
              <tr key={account.id} style={{ opacity: account.status === 'active' ? 1 : 0.55 }}>
                <td>
                  <div style={{ fontWeight: 800 }}>{account.email}</div>
                  {account.type === 'superadmin' && <span className="badge badge-accepted">Superadmin</span>}
                </td>
                <td>{account.role}{account.team && <div className="font-mono" style={{ fontSize: '11px' }}>{account.team}</div>}</td>
                <td style={{ fontSize: '12px' }}>{account.type === 'superadmin' ? 'Everything' : account.permissions.map((p) => kPermissions.find((k) => k.name === p)?.label ?? p).join(', ') || '—'}</td>
                <td className="font-mono" style={{ fontSize: '12px', textTransform: 'uppercase' }}>{account.status}</td>
                <td style={{ textAlign: 'right', whiteSpace: 'nowrap' }}>
                  <button className="btn-outline" onClick={() => { setError(null); setEditing({ account, email: account.email, password: '', role: account.role || 'member', team: account.team, permissions: [...account.permissions], superadmin: account.type === 'superadmin' }); }}>Edit</button>{' '}
                  {account.id !== me.id && (
                    <button className={account.status === 'active' ? 'btn-danger' : 'btn-outline'} aria-label={account.status === 'active' ? 'Disable account' : 'Enable account'} onClick={() => toggleActive(account)}>
                      {account.status === 'active' ? <Ban size={14} /> : <CheckCircle size={14} />}
                    </button>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {editing && (
        <div role="dialog" aria-modal="true" style={{ position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.6)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 50, padding: '16px' }} onClick={() => setEditing(null)}>
          <form
            className="card"
            style={{ maxWidth: '560px', width: '100%', maxHeight: '90vh', overflowY: 'auto', background: '#FFF', display: 'grid', gap: '14px' }}
            onClick={(e) => e.stopPropagation()}
            onSubmit={(e) => { e.preventDefault(); void save(); }}
          >
            <div style={{ display: 'flex', justifyContent: 'space-between' }}>
              <h2 style={{ margin: 0, fontWeight: 900 }}>{editing.account === null ? 'New account' : editing.account.email}</h2>
              <button type="button" className="btn-outline" aria-label="Close" onClick={() => setEditing(null)}><X size={16} /></button>
            </div>
            {editing.account === null && (
              <>
                <input className="input-field" type="email" required placeholder="Email" autoComplete="off" value={editing.email} onChange={(e) => setEditing({ ...editing, email: e.target.value })} />
                <input className="input-field" type="password" required minLength={12} maxLength={128} placeholder="Temporary password (12+ characters)" autoComplete="new-password" value={editing.password} onChange={(e) => setEditing({ ...editing, password: e.target.value })} />
                <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: 0 }}>Hashed in this browser before it is sent. Share it with them privately; they can change it after signing in.</p>
              </>
            )}
            <label style={{ display: 'grid', gap: '4px' }}>
              <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>ROLE</span>
              <select className="input-field" value={editing.role} onChange={(e) => setEditing({ ...editing, role: e.target.value })}>
                {kRoles.map((r) => <option key={r} value={r}>{r}</option>)}
              </select>
            </label>
            <label style={{ display: 'grid', gap: '4px' }}>
              <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>TEAM {editing.role === 'manager' || editing.role === 'vice manager' ? '(limits them to this team)' : ''}</span>
              <select className="input-field" value={editing.team} onChange={(e) => setEditing({ ...editing, team: e.target.value })}>
                <option value="">No team</option>
                {(teams.data?.teams ?? []).map((t) => <option key={t.id} value={t.name}>{t.name}</option>)}
              </select>
            </label>
            <fieldset style={{ border: '2px solid #0E1013', padding: '12px', display: 'grid', gap: '6px' }}>
              <legend className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>ACCESS</legend>
              {grantable.map((p) => (
                <label key={p.name} style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
                  <input
                    type="checkbox"
                    checked={editing.permissions.includes(p.name)}
                    onChange={(e) => setEditing({ ...editing, permissions: e.target.checked ? [...editing.permissions, p.name] : editing.permissions.filter((x) => x !== p.name) })}
                  />
                  {p.label}
                </label>
              ))}
              {superadmin && (
                <label style={{ display: 'flex', gap: '8px', alignItems: 'center', marginTop: '6px', fontWeight: 800 }}>
                  <input type="checkbox" checked={editing.superadmin} onChange={(e) => setEditing({ ...editing, superadmin: e.target.checked })} />
                  Superadmin (full access, including other superadmins)
                </label>
              )}
            </fieldset>
            <ErrorBanner message={error} />
            <div style={{ display: 'flex', gap: '12px', justifyContent: 'flex-end' }}>
              <button type="button" className="btn-outline" onClick={() => setEditing(null)}>Cancel</button>
              <button type="submit" className="btn-primary" disabled={saving}>{saving ? 'Saving…' : 'Save'}</button>
            </div>
          </form>
        </div>
      )}
    </div>
  );
}
