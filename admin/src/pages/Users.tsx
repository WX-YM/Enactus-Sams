import { useState } from 'react';
import { Trash2, Shield, X, Users as UsersIcon, Edit2, RotateCcw } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { api, platform } from '../app/platform';
import type { StaffAccount } from '../app/responses';
import { ErrorBanner, describe, run, useLoad, useSession } from '../app/ui';
import { routeStaffCreate, routeStaffDisable, routeStaffList, routeStaffUpdate, routeTeamsList } from '../api/hammer.generated';

// The grantable permissions (src/config/perms.h kGrantable) and their words.
// Implied ones (media upload, reading form responses) follow from these
// server-side and are never granted directly.
const ALL_PERMISSIONS = [
  { key: 'dashboard', label: 'Dashboard & Analytics' },
  { key: 'applications', label: 'Form Responses / Applications' },
  { key: 'form_maker', label: 'Form Maker / Builder' },
  { key: 'teams', label: 'Manage Teams' },
  { key: 'content', label: 'Content CMS' },
  { key: 'gallery', label: 'Gallery' },
  { key: 'users', label: 'Access Control (User Management)' },
];

const kRoles = [
  { value: 'director', label: 'Director' },
  { value: 'high board', label: 'High Board' },
  { value: 'manager', label: 'Manager' },
  { value: 'vice manager', label: 'Vice Manager' },
  { value: 'HR', label: 'HR' },
  { value: 'member', label: 'Member' },
];

const ROLE_DEFAULT_PERMISSIONS: Record<string, string[]> = {
  HR: ['applications', 'form_maker'],
  member: ['dashboard', 'gallery'],
  manager: ['dashboard', 'applications', 'form_maker', 'teams', 'gallery'],
  'vice manager': ['dashboard', 'applications', 'form_maker', 'teams', 'gallery'],
  director: ['dashboard', 'applications', 'form_maker', 'teams', 'content', 'gallery'],
  'high board': ['dashboard', 'applications', 'form_maker', 'teams', 'content', 'gallery'],
};

const roleColor: Record<string, string> = {
  superadmin: '#FFC629',
  director: '#60A5FA',
  'high board': '#8B5CF6',
  manager: '#4ADE80',
  'vice manager': '#FACC15',
  HR: '#F472B6',
  member: '#D4D4D4',
};

const isManagerRole = (role: string) => role === 'manager' || role === 'vice manager';

const modalBackdrop = { position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.75)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1000 } as const;
const modalBox = { background: '#FFF', border: '3px solid #0E1013', padding: '40px', maxWidth: '520px', width: '90%', maxHeight: '90vh', overflowY: 'auto', boxShadow: '10px 10px 0px #0E1013', position: 'relative' } as const;
const th = { padding: '12px 16px', fontSize: '12px', letterSpacing: '0.1em', textTransform: 'uppercase' } as const;

function PermissionPicker({ choices, selected, onToggle }: { choices: typeof ALL_PERMISSIONS; selected: readonly string[]; onToggle: (key: string) => void }) {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '10px', marginBottom: '32px' }}>
      {choices.map((p) => {
        const on = selected.includes(p.key);
        return (
          <label key={p.key} style={{ display: 'flex', alignItems: 'center', gap: '14px', cursor: 'pointer', padding: '12px 16px', border: on ? '2.5px solid #0E1013' : '2.5px solid rgba(14,16,19,0.15)', background: on ? '#FFC629' : '#F7F5F0', boxShadow: on ? '3px 3px 0px #0E1013' : 'none', transition: 'all 0.1s' }}>
            <input type="checkbox" checked={on} onChange={() => onToggle(p.key)} style={{ width: '18px', height: '18px', accentColor: '#0E1013', cursor: 'pointer' }} />
            <span style={{ fontWeight: 700, fontSize: '15px' }}>{p.label}</span>
          </label>
        );
      })}
    </div>
  );
}

function TeamPicker({ role, teams, value, onChange, note }: { role: string; teams: readonly string[]; value: string; onChange: (team: string) => void; note: string }) {
  return (
    <div style={{ marginBottom: '24px', background: '#F7F5F0', padding: '16px', border: '2px solid #0E1013' }}>
      <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'flex', alignItems: 'center', gap: '6px', marginBottom: '8px', textTransform: 'uppercase' }}>
        <UsersIcon size={14} /> Which team are they the {role} of?
      </label>
      <select className="input-field" value={value} onChange={(e) => onChange(e.target.value)} style={{ width: '100%', background: '#FFF' }}>
        <option value="">-- Select Team --</option>
        {teams.map((t) => <option key={t} value={t}>{t}</option>)}
      </select>
      <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: '8px 0 0 0' }}>{note}</p>
    </div>
  );
}

export default function Users() {
  const { confirm } = useConfirm();
  const { me, superadmin } = useSession();
  const staff = useLoad((signal) => api.call(routeStaffList, { signal }), []);
  const teamsLoad = useLoad((signal) => api.call(routeTeamsList, { signal }), []);
  const teams = (teamsLoad.data?.teams ?? []).map((t) => t.name);

  const [newEmail, setNewEmail] = useState('');
  const [newPassword, setNewPassword] = useState('');
  const [newRole, setNewRole] = useState('manager');
  const [selectedTeam, setSelectedTeam] = useState('');
  const [showPermModal, setShowPermModal] = useState(false);
  const [pendingPerms, setPendingPerms] = useState<string[]>([]);

  const [editingUser, setEditingUser] = useState<StaffAccount | null>(null);
  const [editRole, setEditRole] = useState('member');
  const [editTeam, setEditTeam] = useState('');
  const [editPerms, setEditPerms] = useState<string[]>([]);
  const [editSuperadmin, setEditSuperadmin] = useState(false);

  const [error, setError] = useState<string | null>(null);
  const [modalError, setModalError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  // A non-superadmin may grant only what they hold, and manage only accounts
  // whose access is within their own; the server refuses the rest.
  const grantable = ALL_PERMISSIONS.filter((p) => superadmin || me.permissions.includes(p.key));
  const manageable = (account: StaffAccount) =>
    account.id !== me.id && (superadmin || (account.type !== 'superadmin' && account.permissions.every((p) => me.permissions.includes(p))));
  const allowed = (perms: readonly string[]) => perms.filter((p) => grantable.some((g) => g.key === p));

  const accounts = staff.data?.staff ?? [];
  const active = accounts.filter((a) => a.status !== 'disabled');
  const revoked = accounts.filter((a) => a.status === 'disabled');

  const handleAddClick = () => {
    if (!newEmail.trim() || !newPassword) { setError('Email and password are required.'); return; }
    if (newPassword.length < 12) { setError('The password must be at least 12 characters.'); return; }
    setError(null);
    setModalError(null);
    setPendingPerms(allowed(ROLE_DEFAULT_PERMISSIONS[newRole] ?? []));
    if (isManagerRole(newRole) && !selectedTeam && teams.length > 0) setSelectedTeam(teams[0]);
    setShowPermModal(true);
  };

  const handleConfirmAdd = async () => {
    if (isManagerRole(newRole) && !selectedTeam) { setModalError(`Choose the team they are the ${newRole} of.`); return; }
    setSaving(true);
    setModalError(null);
    const controller = new AbortController();
    const email = newEmail.trim();
    // The temporary password is hashed in this browser; only the derived
    // credential reaches the server.
    const credential = await platform.enrolment.credential(email, newPassword, controller.signal, 'enroll');
    if (!credential.ok) { setSaving(false); setModalError(describe(credential.error)); return; }
    const result = await api.call(routeStaffCreate, {
      body: { email, credential: credential.value, role: newRole, team: isManagerRole(newRole) ? selectedTeam : '', permissions: pendingPerms },
      signal: controller.signal,
    });
    setSaving(false);
    if (!result.ok) { setModalError(describe(result.error)); return; }
    setNewEmail('');
    setNewPassword('');
    setNewRole('manager');
    setSelectedTeam('');
    setShowPermModal(false);
    staff.reload();
  };

  const handleStartEdit = (user: StaffAccount) => {
    setModalError(null);
    setEditingUser(user);
    setEditRole(user.role || 'member');
    setEditTeam(user.team || '');
    setEditPerms([...user.permissions]);
    setEditSuperadmin(user.type === 'superadmin');
  };

  const handleConfirmEdit = async () => {
    if (!editingUser) return;
    if (isManagerRole(editRole) && !editTeam) { setModalError(`Choose the team they are the ${editRole} of.`); return; }
    setSaving(true);
    setModalError(null);
    const body: Record<string, unknown> = { version: editingUser.version, role: editRole, team: isManagerRole(editRole) ? editTeam : '', permissions: editPerms };
    if (superadmin && editSuperadmin !== (editingUser.type === 'superadmin')) body.superadmin = editSuperadmin;
    const result = await run((signal) => api.call(routeStaffUpdate, { params: { id: editingUser.id }, body, signal }));
    setSaving(false);
    if (!result.ok) { setModalError(describe(result.error)); return; }
    setEditingUser(null);
    staff.reload();
  };

  const handleRevoke = async (user: StaffAccount) => {
    const ok = await confirm({
      title: 'Revoke User Access?',
      message: `Are you sure you want to revoke ${user.email}? They are signed out everywhere and immediately lose all access to the administrative panel.`,
      confirmText: 'Revoke Access',
      cancelText: 'Cancel',
      type: 'danger',
    });
    if (!ok) return;
    const result = await run((signal) => api.call(routeStaffDisable, { params: { id: user.id }, body: {}, signal }));
    setError(result.ok ? null : describe(result.error));
    staff.reload();
  };

  const handleRestore = async (user: StaffAccount) => {
    const result = await run((signal) => api.call(routeStaffUpdate, { params: { id: user.id }, body: { version: user.version, active: true }, signal }));
    setError(result.ok ? null : describe(result.error));
    staff.reload();
  };

  const toggle = (list: string[], key: string) => (list.includes(key) ? list.filter((p) => p !== key) : [...list, key]);

  const row = (u: StaffAccount, revokedRow: boolean) => (
    <tr key={u.id} style={{ borderBottom: '2px solid rgba(14,16,19,0.1)', opacity: revokedRow ? 0.6 : 1 }}>
      <td style={{ padding: '16px', fontWeight: 700, wordBreak: 'break-all' }}>{u.email}</td>
      <td style={{ padding: '16px' }}>
        <span style={{ background: roleColor[u.type === 'superadmin' ? 'superadmin' : u.role] ?? '#D4D4D4', color: '#0E1013', padding: '4px 10px', border: '2px solid #0E1013', fontFamily: 'IBM Plex Mono', fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', boxShadow: '2px 2px 0px #0E1013', whiteSpace: 'nowrap' }}>
          {u.type === 'superadmin' ? 'superadmin' : (u.role || 'member').replace(' ', '_')}
        </span>
      </td>
      <td style={{ padding: '16px' }}>
        {u.team ? (
          <span style={{ background: '#0E1013', color: '#FFC629', padding: '4px 10px', border: '1.5px solid #0E1013', fontFamily: 'IBM Plex Mono', fontSize: '11px', fontWeight: 700, textTransform: 'uppercase' }}>{u.team}</span>
        ) : (
          <span className="font-mono" style={{ fontSize: '11px', opacity: 0.4 }}>—</span>
        )}
      </td>
      <td style={{ padding: '16px' }}>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '4px' }}>
          {u.type === 'superadmin' ? (
            <span className="font-mono" style={{ fontSize: '11px', color: '#B45309', background: '#FEF3C7', padding: '2px 8px', border: '1.5px solid #0E1013', fontWeight: 700 }}>⚡ Full Access</span>
          ) : u.permissions.length === 0 ? (
            <span className="font-mono" style={{ fontSize: '11px', opacity: 0.5 }}>None assigned</span>
          ) : u.permissions.map((p) => (
            <span key={p} style={{ background: '#F7F5F0', border: '1.5px solid #0E1013', padding: '2px 8px', fontSize: '11px', fontFamily: 'IBM Plex Mono', fontWeight: 600 }}>{p}</span>
          ))}
        </div>
      </td>
      <td style={{ padding: '16px' }}>
        {manageable(u) && (
          <div style={{ display: 'flex', gap: '8px' }}>
            {revokedRow ? (
              <button className="btn-outline" style={{ padding: '8px 12px', display: 'flex', alignItems: 'center', gap: '6px', fontSize: '13px', background: '#FFF' }} onClick={() => handleRestore(u)}>
                <RotateCcw size={14} /> Restore
              </button>
            ) : (
              <>
                <button className="btn-outline" style={{ padding: '8px 12px', display: 'flex', alignItems: 'center', gap: '6px', fontSize: '13px', background: '#FFF' }} onClick={() => handleStartEdit(u)}>
                  <Edit2 size={14} /> Edit
                </button>
                <button className="btn-danger" style={{ padding: '8px 12px', display: 'flex', alignItems: 'center', gap: '6px', fontSize: '13px' }} onClick={() => handleRevoke(u)}>
                  <Trash2 size={14} /> Revoke
                </button>
              </>
            )}
          </div>
        )}
      </td>
    </tr>
  );

  const table = (rows: readonly StaffAccount[], revokedRows: boolean) => (
    <div style={{ width: '100%', overflowX: 'auto' }}>
      <table style={{ width: '100%', borderCollapse: 'collapse', minWidth: '600px' }}>
        <thead>
          <tr style={{ borderBottom: '3px solid #0E1013', textAlign: 'left' }}>
            <th className="font-mono" style={th}>Email</th>
            <th className="font-mono" style={th}>Role</th>
            <th className="font-mono" style={th}>Team</th>
            <th className="font-mono" style={th}>Panels</th>
            <th className="font-mono" style={th}>Action</th>
          </tr>
        </thead>
        <tbody>{rows.map((u) => row(u, revokedRows))}</tbody>
      </table>
    </div>
  );

  return (
    <div className="fade-in">
      <div style={{ marginBottom: '32px' }}>
        <h1 className="heading-lg">Access Control</h1>
        <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>Manage admin users, roles, team assignments, and panel access permissions.</p>
      </div>

      <form className="card" style={{ marginBottom: '32px' }} onSubmit={(e) => { e.preventDefault(); handleAddClick(); }}>
        <h2 className="heading-sm" style={{ marginBottom: '24px' }}>Add New User</h2>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(min(100%, 180px), 1fr))', gap: '16px', alignItems: 'flex-end' }}>
          <div>
            <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '8px' }}>Email</label>
            <input className="input-field" type="email" autoComplete="off" placeholder="user@enactus.org" value={newEmail} onChange={(e) => setNewEmail(e.target.value)} style={{ width: '100%' }} />
          </div>
          <div>
            <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '8px' }}>Password</label>
            <input className="input-field" type="password" autoComplete="new-password" maxLength={128} placeholder="password (12+ characters)" value={newPassword} onChange={(e) => setNewPassword(e.target.value)} style={{ width: '100%' }} />
          </div>
          <div>
            <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '8px' }}>Role</label>
            <select className="input-field" value={newRole} onChange={(e) => setNewRole(e.target.value)} style={{ width: '100%' }}>
              {kRoles.map((r) => <option key={r.value} value={r.value}>{r.label}</option>)}
            </select>
          </div>
          <button type="submit" className="btn-primary" style={{ display: 'flex', alignItems: 'center', gap: '8px', height: '54px', justifyContent: 'center' }}>
            <Shield size={16} /> Add User
          </button>
        </div>
        {error && <div style={{ marginTop: '16px' }}><ErrorBanner message={error} /></div>}
      </form>

      <ErrorBanner message={staff.error} />

      <div className="card" style={{ padding: 0, overflowX: 'auto' }}>
        <div style={{ padding: '24px 24px 8px 24px' }}>
          <h2 className="heading-sm" style={{ marginBottom: '8px' }}>Active Users</h2>
        </div>
        {table(active, false)}
      </div>

      {revoked.length > 0 && (
        <div className="card" style={{ padding: 0, overflowX: 'auto', marginTop: '32px' }}>
          <div style={{ padding: '24px 24px 8px 24px' }}>
            <h2 className="heading-sm" style={{ marginBottom: '8px' }}>Revoked Users ({revoked.length})</h2>
          </div>
          {table(revoked, true)}
        </div>
      )}

      {showPermModal && (
        <div role="dialog" aria-modal="true" style={modalBackdrop}>
          <div style={modalBox}>
            <button aria-label="Close" onClick={() => setShowPermModal(false)} style={{ position: 'absolute', top: '16px', right: '16px', background: 'none', border: 'none', cursor: 'pointer' }}>
              <X size={22} />
            </button>
            <span className="heading-sm" style={{ display: 'block', marginBottom: '8px' }}>Configure Access</span>
            <h2 style={{ margin: '0 0 4px 0', fontSize: '22px', fontWeight: 900, letterSpacing: '-0.02em', wordBreak: 'break-all' }}>{newEmail}</h2>
            <p className="font-mono" style={{ margin: '0 0 24px 0', fontSize: '12px', opacity: 0.6 }}>ROLE: {newRole.toUpperCase()}</p>

            {isManagerRole(newRole) && (
              <TeamPicker role={newRole} teams={teams} value={selectedTeam} onChange={setSelectedTeam} note={`Once saved, this user will be listed as the ${newRole} in the Teams section.`} />
            )}

            <p style={{ margin: '0 0 16px 0', fontWeight: 700 }}>Choose what this user should be able to see and access:</p>
            <PermissionPicker choices={grantable} selected={pendingPerms} onToggle={(key) => setPendingPerms((prev) => toggle(prev, key))} />
            <ErrorBanner message={modalError} />
            <div style={{ display: 'flex', gap: '12px', marginTop: modalError ? '16px' : 0 }}>
              <button className="btn-primary" style={{ flex: 1, justifyContent: 'center' }} disabled={saving} onClick={handleConfirmAdd}>{saving ? 'Saving…' : 'Save & Create User'}</button>
              <button className="btn-outline" onClick={() => setShowPermModal(false)}>Cancel</button>
            </div>
          </div>
        </div>
      )}

      {editingUser && (
        <div role="dialog" aria-modal="true" style={modalBackdrop}>
          <div style={modalBox}>
            <button aria-label="Close" onClick={() => setEditingUser(null)} style={{ position: 'absolute', top: '16px', right: '16px', background: 'none', border: 'none', cursor: 'pointer' }}>
              <X size={22} />
            </button>
            <span className="heading-sm" style={{ display: 'block', marginBottom: '8px' }}>Edit Permissions & Role</span>
            <h2 style={{ margin: '0 0 4px 0', fontSize: '22px', fontWeight: 900, letterSpacing: '-0.02em', wordBreak: 'break-all' }}>{editingUser.email}</h2>

            <div style={{ marginTop: '20px', marginBottom: '20px' }}>
              <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '8px', fontWeight: 700, textTransform: 'uppercase' }}>Role</label>
              <select
                className="input-field"
                value={editRole}
                onChange={(e) => {
                  setEditRole(e.target.value);
                  if (isManagerRole(e.target.value) && !editTeam && teams.length > 0) setEditTeam(teams[0]);
                }}
                style={{ width: '100%', background: '#FFF' }}
              >
                {kRoles.map((r) => <option key={r.value} value={r.value}>{r.label}</option>)}
              </select>
            </div>

            {isManagerRole(editRole) && (
              <TeamPicker role={editRole} teams={teams} value={editTeam} onChange={setEditTeam} note="This user will only have management controls for this team." />
            )}

            <p style={{ margin: '0 0 16px 0', fontWeight: 700 }}>Customize Panel Permissions:</p>
            <PermissionPicker choices={grantable} selected={editPerms} onToggle={(key) => setEditPerms((prev) => toggle(prev, key))} />

            {superadmin && (
              <label style={{ display: 'flex', alignItems: 'center', gap: '10px', marginTop: '-16px', marginBottom: '24px', fontWeight: 800 }}>
                <input type="checkbox" checked={editSuperadmin} onChange={(e) => setEditSuperadmin(e.target.checked)} style={{ width: '18px', height: '18px', accentColor: '#0E1013' }} />
                Superadmin (full access, including other superadmins)
              </label>
            )}

            <ErrorBanner message={modalError} />
            <div style={{ display: 'flex', gap: '12px', marginTop: modalError ? '16px' : 0 }}>
              <button className="btn-primary" style={{ flex: 1, justifyContent: 'center' }} disabled={saving} onClick={handleConfirmEdit}>{saving ? 'Saving…' : 'Save Changes'}</button>
              <button className="btn-outline" onClick={() => setEditingUser(null)}>Cancel</button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
