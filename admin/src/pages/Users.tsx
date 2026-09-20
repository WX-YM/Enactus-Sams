import { useState, useEffect } from 'react';
import { Trash2, Shield, X, Users as UsersIcon, Edit2 } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';

const ALL_PERMISSIONS = [
  { key: 'dashboard', label: 'Dashboard & Analytics' },
  { key: 'applications', label: 'Form Responses / Applications' },
  { key: 'form_maker', label: 'Form Maker / Builder' },
  { key: 'teams', label: 'Manage Teams' },
  { key: 'content', label: 'Content CMS' },
  { key: 'gallery', label: 'Gallery' },
  { key: 'users', label: 'Access Control (User Management)' },
];

const ROLE_DEFAULT_PERMISSIONS: Record<string, string[]> = {
  HR: ['applications', 'form_maker'],
  member: ['dashboard', 'gallery'],
  manager: ['dashboard', 'applications', 'form_maker', 'teams', 'gallery'],
  'vice manager': ['dashboard', 'applications', 'form_maker', 'teams', 'gallery'],
  director: ['dashboard', 'applications', 'form_maker', 'teams', 'content', 'gallery'],
  'high board': ['dashboard', 'applications', 'form_maker', 'teams', 'content', 'gallery'],
};

export default function Users() {
  const { confirm } = useConfirm();
  const [users, setUsers] = useState<any[]>([]);
  const [teams, setTeams] = useState<any[]>([]);
  const [newEmail, setNewEmail] = useState('');
  const [newPassword, setNewPassword] = useState('');
  const [newRole, setNewRole] = useState('manager');
  const [selectedTeam, setSelectedTeam] = useState('');
  const [showPermModal, setShowPermModal] = useState(false);
  const [pendingPerms, setPendingPerms] = useState<string[]>([]);

  // Edit user state
  const [showEditModal, setShowEditModal] = useState(false);
  const [editingUser, setEditingUser] = useState<any | null>(null);
  const [editRole, setEditRole] = useState('member');
  const [editTeam, setEditTeam] = useState('');
  const [editPerms, setEditPerms] = useState<string[]>([]);
  const [editPassword, setEditPassword] = useState('');

  const fetchUsers = () => {
    fetch('/api/users')
      .then(res => res.json())
      .then(data => setUsers(data.users || []))
      .catch(err => console.error(err));
  };

  const fetchTeams = () => {
    Promise.all([
      fetch('/api/teams').then(r => r.json()).catch(() => ({ teams: [] })),
      fetch('/api/content').then(r => r.json()).catch(() => ({ content: {} }))
    ]).then(([teamsData, contentData]) => {
      const dbTeams = (teamsData && teamsData.teams) ? teamsData.teams : [];
      const recruitmentTeams = (contentData && contentData.content && contentData.content.recruitmentTeams)
        ? contentData.content.recruitmentTeams.map((name: string) => ({ name, id: name }))
        : [];

      const map = new Map<string, any>();
      dbTeams.forEach((t: any) => {
        if (t && t.name) map.set(t.name.trim().toLowerCase(), { ...t, name: t.name.trim() });
      });
      recruitmentTeams.forEach((t: any) => {
        if (t && t.name) {
          const key = t.name.trim().toLowerCase();
          if (!map.has(key)) map.set(key, t);
        }
      });

      const merged = Array.from(map.values());
      setTeams(merged);
      if (merged.length > 0 && !selectedTeam) {
        setSelectedTeam(merged[0].name);
      }
    }).catch(err => console.error(err));
  };

  useEffect(() => { 
    fetchUsers(); 
    fetchTeams();
  }, []);

  const [error, setError] = useState('');

  const handleAddClick = () => {
    if (!newEmail || !newPassword) { 
      setError('Email and password are required.'); 
      return; 
    }
    setError('');
    setPendingPerms(ROLE_DEFAULT_PERMISSIONS[newRole] || []);
    if ((newRole === 'manager' || newRole === 'vice manager') && teams.length > 0 && !selectedTeam) {
      setSelectedTeam(teams[0].name);
    }
    setShowPermModal(true);
  };

  const handleConfirmAdd = () => {
    const isManagerRole = newRole === 'manager' || newRole === 'vice manager';
    const payload = {
      email: newEmail,
      password: newPassword,
      role: newRole,
      team: isManagerRole ? selectedTeam : '',
      permissions: pendingPerms
    };

    fetch('/api/users', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    }).then(() => {
      setNewEmail(''); 
      setNewPassword(''); 
      setNewRole('manager');
      setSelectedTeam(teams[0]?.name || '');
      setShowPermModal(false); 
      fetchUsers();
      fetchTeams();
    });
  };

  const handleStartEdit = (user: any) => {
    setEditingUser(user);
    setEditRole(user.role || 'member');
    setEditTeam(user.team || (teams[0]?.name || ''));
    setEditPerms(user.permissions || []);
    setEditPassword('');
    setShowEditModal(true);
  };

  const handleConfirmEdit = () => {
    if (!editingUser) return;
    const isManagerRole = editRole === 'manager' || editRole === 'vice manager';
    const payload: any = {
      action: 'update',
      email: editingUser.email,
      role: editRole,
      team: isManagerRole ? editTeam : '',
      permissions: editPerms
    };
    if (editPassword.trim()) {
      payload.password = editPassword.trim();
    }

    fetch('/api/users', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    }).then(() => {
      setShowEditModal(false);
      setEditingUser(null);
      fetchUsers();
      fetchTeams();
    });
  };

  const togglePerm = (key: string) => {
    setPendingPerms(prev => prev.includes(key) ? prev.filter(p => p !== key) : [...prev, key]);
  };

  const toggleEditPerm = (key: string) => {
    setEditPerms(prev => prev.includes(key) ? prev.filter(p => p !== key) : [...prev, key]);
  };

  const handleDelete = async (email: string) => {
    const ok = await confirm({
      title: 'Revoke User Access?',
      message: `Are you sure you want to remove ${email}? They will immediately lose all access permissions to the administrative panel.`,
      confirmText: 'Remove Access',
      cancelText: 'Cancel',
      type: 'danger'
    });
    if (!ok) return;
    fetch('/api/users', {
      method: 'DELETE', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email })
    }).then(() => {
      fetchUsers();
      fetchTeams();
    });
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

  return (
    <div className="fade-in">
      <div style={{ marginBottom: '32px' }}>
        <h1 className="heading-lg">Access Control</h1>
        <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>Manage admin users, roles, team assignments, and panel access permissions.</p>
      </div>

      <div className="card" style={{ marginBottom: '32px' }}>
        <h2 className="heading-sm" style={{ marginBottom: '24px' }}>Add New User</h2>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(180px, 1fr))', gap: '16px', alignItems: 'flex-end' }}>
          <div>
            <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '8px' }}>Email</label>
            <input className="input-field" type="email" placeholder="user@enactus.org" value={newEmail} onChange={e => setNewEmail(e.target.value)} style={{ width: '100%' }} />
          </div>
          <div>
            <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '8px' }}>Password</label>
            <input className="input-field" type="password" placeholder="password" value={newPassword} onChange={e => setNewPassword(e.target.value)} style={{ width: '100%' }} />
          </div>
          <div>
            <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '8px' }}>Role</label>
            <select className="input-field" value={newRole} onChange={e => setNewRole(e.target.value)} style={{ width: '100%' }}>
              <option value="director">Director</option>
              <option value="high board">High Board</option>
              <option value="manager">Manager</option>
              <option value="vice manager">Vice Manager</option>
              <option value="HR">HR</option>
              <option value="member">Member</option>
            </select>
          </div>
          <button className="btn-primary" style={{ display: 'flex', alignItems: 'center', gap: '8px', height: '54px', justifyContent: 'center' }} onClick={handleAddClick}>
            <Shield size={16} /> Add User
          </button>
        </div>
        {error && (
          <div style={{ marginTop: '16px', background: '#FFF0F0', border: '2px solid #E53935', padding: '10px 14px', color: '#E53935', fontWeight: 700, fontSize: '13px' }}>
            {error}
          </div>
        )}
      </div>

      <div className="card">
        <h2 className="heading-sm" style={{ marginBottom: '16px' }}>Active Users</h2>
        <table style={{ width: '100%', borderCollapse: 'collapse' }}>
          <thead>
            <tr style={{ borderBottom: '3px solid #0E1013', textAlign: 'left' }}>
              <th className="font-mono" style={{ padding: '12px 16px', fontSize: '12px', letterSpacing: '0.1em', textTransform: 'uppercase' }}>Email</th>
              <th className="font-mono" style={{ padding: '12px 16px', fontSize: '12px', letterSpacing: '0.1em', textTransform: 'uppercase' }}>Role</th>
              <th className="font-mono" style={{ padding: '12px 16px', fontSize: '12px', letterSpacing: '0.1em', textTransform: 'uppercase' }}>Team</th>
              <th className="font-mono" style={{ padding: '12px 16px', fontSize: '12px', letterSpacing: '0.1em', textTransform: 'uppercase' }}>Panels</th>
              <th className="font-mono" style={{ padding: '12px 16px', fontSize: '12px', letterSpacing: '0.1em', textTransform: 'uppercase' }}>Action</th>
            </tr>
          </thead>
          <tbody>
            {users.map((u, i) => (
              <tr key={i} style={{ borderBottom: '2px solid rgba(14,16,19,0.1)' }}>
                <td style={{ padding: '16px', fontWeight: 700 }}>{u.email}</td>
                <td style={{ padding: '16px' }}>
                  <span style={{ background: roleColor[u.role] || '#D4D4D4', color: '#0E1013', padding: '4px 10px', border: '2px solid #0E1013', fontFamily: 'IBM Plex Mono', fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', boxShadow: '2px 2px 0px #0E1013' }}>
                    {u.role}
                  </span>
                </td>
                <td style={{ padding: '16px' }}>
                  {u.team ? (
                    <span style={{ background: '#0E1013', color: '#FFC629', padding: '4px 10px', border: '1.5px solid #0E1013', fontFamily: 'IBM Plex Mono', fontSize: '11px', fontWeight: 700, textTransform: 'uppercase' }}>
                      {u.team}
                    </span>
                  ) : (
                    <span className="font-mono" style={{ fontSize: '11px', opacity: 0.4 }}>—</span>
                  )}
                </td>
                <td style={{ padding: '16px' }}>
                  <div style={{ display: 'flex', flexWrap: 'wrap', gap: '4px' }}>
                    {u.role === 'superadmin' ? (
                      <span className="font-mono" style={{ fontSize: '11px', color: '#B45309', background: '#FEF3C7', padding: '2px 8px', border: '1.5px solid #0E1013', fontWeight: 700 }}>⚡ Full Access</span>
                    ) : (u.permissions || []).length === 0 ? (
                      <span className="font-mono" style={{ fontSize: '11px', opacity: 0.5 }}>None assigned</span>
                    ) : (u.permissions || []).map((p: string) => (
                      <span key={p} style={{ background: '#F7F5F0', border: '1.5px solid #0E1013', padding: '2px 8px', fontSize: '11px', fontFamily: 'IBM Plex Mono', fontWeight: 600 }}>{p}</span>
                    ))}
                  </div>
                </td>
                <td style={{ padding: '16px' }}>
                  {u.role !== 'superadmin' && u.email !== 'admin@enactussams.org' && (
                    <div style={{ display: 'flex', gap: '8px' }}>
                      <button className="btn-outline" style={{ padding: '8px 12px', display: 'flex', alignItems: 'center', gap: '6px', fontSize: '13px', background: '#FFF' }} onClick={() => handleStartEdit(u)}>
                        <Edit2 size={14} /> Edit
                      </button>
                      <button className="btn-danger" style={{ padding: '8px 12px', display: 'flex', alignItems: 'center', gap: '6px', fontSize: '13px' }} onClick={() => handleDelete(u.email)}>
                        <Trash2 size={14} /> Revoke
                      </button>
                    </div>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {/* CREATE USER PERMISSION MODAL */}
      {showPermModal && (
        <div style={{ position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.75)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1000 }}>
          <div style={{ background: '#FFF', border: '3px solid #0E1013', padding: '40px', maxWidth: '520px', width: '90%', maxHeight: '90vh', overflowY: 'auto', boxShadow: '10px 10px 0px #0E1013', position: 'relative' }}>
            <button onClick={() => setShowPermModal(false)} style={{ position: 'absolute', top: '16px', right: '16px', background: 'none', border: 'none', cursor: 'pointer' }}>
              <X size={22} />
            </button>
            <span className="heading-sm" style={{ display: 'block', marginBottom: '8px' }}>Configure Access</span>
            <h2 style={{ margin: '0 0 4px 0', fontSize: '22px', fontWeight: 900, letterSpacing: '-0.02em' }}>{newEmail}</h2>
            <p className="font-mono" style={{ margin: '0 0 24px 0', fontSize: '12px', opacity: 0.6 }}>ROLE: {newRole.toUpperCase()}</p>

            {(newRole === 'manager' || newRole === 'vice manager') && (
              <div style={{ marginBottom: '24px', background: '#F7F5F0', padding: '16px', border: '2px solid #0E1013' }}>
                <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'flex', alignItems: 'center', gap: '6px', marginBottom: '8px', textTransform: 'uppercase' }}>
                  <UsersIcon size={14} /> Which team are they the {newRole} of?
                </label>
                <select
                  className="input-field"
                  value={selectedTeam}
                  onChange={e => setSelectedTeam(e.target.value)}
                  style={{ width: '100%', background: '#FFF' }}
                >
                  <option value="">-- Select Team --</option>
                  {teams.map(t => (
                    <option key={t.id || t.name} value={t.name}>{t.name}</option>
                  ))}
                </select>
                <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: '8px 0 0 0' }}>
                  Once saved, this user will be listed as the {newRole} in the Teams section.
                </p>
              </div>
            )}

            <p style={{ margin: '0 0 16px 0', fontWeight: 700 }}>Choose what this user should be able to see and access:</p>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '10px', marginBottom: '32px' }}>
              {ALL_PERMISSIONS.map(p => (
                <label key={p.key} style={{ display: 'flex', alignItems: 'center', gap: '14px', cursor: 'pointer', padding: '12px 16px', border: pendingPerms.includes(p.key) ? '2.5px solid #0E1013' : '2.5px solid rgba(14,16,19,0.15)', background: pendingPerms.includes(p.key) ? '#FFC629' : '#F7F5F0', boxShadow: pendingPerms.includes(p.key) ? '3px 3px 0px #0E1013' : 'none', transition: 'all 0.1s' }}>
                  <input type="checkbox" checked={pendingPerms.includes(p.key)} onChange={() => togglePerm(p.key)} style={{ width: '18px', height: '18px', accentColor: '#0E1013', cursor: 'pointer' }} />
                  <span style={{ fontWeight: 700, fontSize: '15px' }}>{p.label}</span>
                </label>
              ))}
            </div>
            <div style={{ display: 'flex', gap: '12px' }}>
              <button className="btn-primary" style={{ flex: 1, justifyContent: 'center' }} onClick={handleConfirmAdd}>Save & Create User</button>
              <button className="btn-outline" onClick={() => setShowPermModal(false)}>Cancel</button>
            </div>
          </div>
        </div>
      )}

      {/* EDIT USER MODAL */}
      {showEditModal && editingUser && (
        <div style={{ position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.75)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1000 }}>
          <div style={{ background: '#FFF', border: '3px solid #0E1013', padding: '40px', maxWidth: '520px', width: '90%', maxHeight: '90vh', overflowY: 'auto', boxShadow: '10px 10px 0px #0E1013', position: 'relative' }}>
            <button onClick={() => { setShowEditModal(false); setEditingUser(null); }} style={{ position: 'absolute', top: '16px', right: '16px', background: 'none', border: 'none', cursor: 'pointer' }}>
              <X size={22} />
            </button>
            <span className="heading-sm" style={{ display: 'block', marginBottom: '8px' }}>Edit Permissions & Role</span>
            <h2 style={{ margin: '0 0 4px 0', fontSize: '22px', fontWeight: 900, letterSpacing: '-0.02em' }}>{editingUser.email}</h2>

            <div style={{ marginTop: '20px', marginBottom: '20px' }}>
              <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '8px', fontWeight: 700, textTransform: 'uppercase' }}>Role</label>
              <select className="input-field" value={editRole} onChange={e => {
                setEditRole(e.target.value);
                if (e.target.value === 'manager' || e.target.value === 'vice manager') {
                  if (!editTeam && teams.length > 0) setEditTeam(teams[0].name);
                }
              }} style={{ width: '100%', background: '#FFF' }}>
                <option value="director">Director</option>
                <option value="high board">High Board</option>
                <option value="manager">Manager</option>
                <option value="vice manager">Vice Manager</option>
                <option value="HR">HR</option>
                <option value="member">Member</option>
              </select>
            </div>

            {(editRole === 'manager' || editRole === 'vice manager') && (
              <div style={{ marginBottom: '20px', background: '#F7F5F0', padding: '16px', border: '2px solid #0E1013' }}>
                <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'flex', alignItems: 'center', gap: '6px', marginBottom: '8px', textTransform: 'uppercase' }}>
                  <UsersIcon size={14} /> Assigned Team
                </label>
                <select
                  className="input-field"
                  value={editTeam}
                  onChange={e => setEditTeam(e.target.value)}
                  style={{ width: '100%', background: '#FFF' }}
                >
                  <option value="">-- Select Team --</option>
                  {teams.map(t => (
                    <option key={t.id || t.name} value={t.name}>{t.name}</option>
                  ))}
                </select>
                <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: '8px 0 0 0' }}>
                  This user will only have management controls for this team.
                </p>
              </div>
            )}

            <div style={{ marginBottom: '24px' }}>
              <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '8px', fontWeight: 700, textTransform: 'uppercase' }}>
                New Password (leave blank to keep current)
              </label>
              <input
                className="input-field"
                type="password"
                placeholder="••••••••"
                value={editPassword}
                onChange={e => setEditPassword(e.target.value)}
                style={{ width: '100%' }}
              />
            </div>

            <p style={{ margin: '0 0 16px 0', fontWeight: 700 }}>Customize Panel Permissions:</p>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '10px', marginBottom: '32px' }}>
              {ALL_PERMISSIONS.map(p => (
                <label key={p.key} style={{ display: 'flex', alignItems: 'center', gap: '14px', cursor: 'pointer', padding: '12px 16px', border: editPerms.includes(p.key) ? '2.5px solid #0E1013' : '2.5px solid rgba(14,16,19,0.15)', background: editPerms.includes(p.key) ? '#FFC629' : '#F7F5F0', boxShadow: editPerms.includes(p.key) ? '3px 3px 0px #0E1013' : 'none', transition: 'all 0.1s' }}>
                  <input type="checkbox" checked={editPerms.includes(p.key)} onChange={() => toggleEditPerm(p.key)} style={{ width: '18px', height: '18px', accentColor: '#0E1013', cursor: 'pointer' }} />
                  <span style={{ fontWeight: 700, fontSize: '15px' }}>{p.label}</span>
                </label>
              ))}
            </div>

            <div style={{ display: 'flex', gap: '12px' }}>
              <button className="btn-primary" style={{ flex: 1, justifyContent: 'center' }} onClick={handleConfirmEdit}>Save Changes</button>
              <button className="btn-outline" onClick={() => { setShowEditModal(false); setEditingUser(null); }}>Cancel</button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
