import { useState, useEffect } from 'react';
import { Trash2, Users as UsersIcon, Plus, UserPlus, Lock } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';

export default function Teams() {
  const { confirm } = useConfirm();
  const [teams, setTeams] = useState<any[]>([]);
  const [newTeam, setNewTeam] = useState({ name: '', desc: '' });
  const [expandedTeam, setExpandedTeam] = useState<string | null>(null);
  const [newMember, setNewMember] = useState({ name: '', role: 'Member' });

  const role = localStorage.getItem('admin_role') || '';
  const userTeam = localStorage.getItem('admin_team') || '';
  const adminEmail = localStorage.getItem('admin_email') || '';
  const isSuperAdmin = role === 'superadmin' || adminEmail === 'admin@enactussams.org';
  const isTeamScoped = !isSuperAdmin && (role === 'manager' || role === 'vice manager') && !!userTeam;

  const canEditTeam = (teamName: string) => {
    if (!isTeamScoped) return true;
    return !!userTeam && teamName.trim().toLowerCase() === userTeam.trim().toLowerCase();
  };

  useEffect(() => {
    fetch('/api/teams')
      .then(res => res.json())
      .then(data => {
        if (data && data.teams) {
          setTeams(data.teams.map((t: any) => ({ ...t, id: t._id?.$oid || t.id })));
        }
      })
      .catch(err => console.error("Failed to fetch teams:", err));
  }, []);

  const saveRoster = (teamId: string, teamName: string, members: number, memberList: any[]) => {
    fetch('/api/teams', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'update_roster', id: teamId, name: teamName, members, memberList })
    }).catch(err => console.error("Failed to update roster:", err));
  };

  const addTeam = () => {
    if (!newTeam.name || isTeamScoped) return;
    
    fetch('/api/teams', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: newTeam.name, desc: newTeam.desc })
    })
    .then(res => res.json())
    .then(() => {
      fetch('/api/teams')
        .then(res => res.json())
        .then(data => {
          if (data && data.teams) {
            setTeams(data.teams.map((t: any) => ({ ...t, id: t._id?.$oid || t.id })));
          }
        });
      setNewTeam({ name: '', desc: '' });
    })
    .catch(err => console.error("Failed to add team:", err));
  };

  const removeTeam = async (id: string, teamName: string) => {
    if (!canEditTeam(teamName) || isTeamScoped) return;
    const ok = await confirm({
      title: 'Delete Team?',
      message: `Are you sure you want to delete "${teamName}"? This will remove the team, its roster, and its recruitment choices.`,
      confirmText: 'Delete Team',
      cancelText: 'Cancel',
      type: 'danger'
    });
    if (!ok) return;
    fetch('/api/teams', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'delete', id, name: teamName })
    })
      .then(res => res.json())
      .then(() => {
        setTeams(teams.filter(t => t.id !== id));
      })
      .catch(err => console.error("Failed to delete team:", err));
  };

  const addMember = (teamId: string) => {
    if (!newMember.name) return;
    const targetTeam = teams.find(t => t.id === teamId);
    if (!targetTeam || !canEditTeam(targetTeam.name)) return;

    const memberList = targetTeam.memberList || [];
    const updatedList = [...memberList, { id: Date.now(), name: newMember.name, role: newMember.role }];
    const newCount = (targetTeam.members || 0) + 1;

    setTeams(teams.map(t => t.id === teamId ? { ...t, members: newCount, memberList: updatedList } : t));
    saveRoster(teamId, targetTeam.name, newCount, updatedList);
    setNewMember({ name: '', role: 'Member' });
  };

  const removeMember = async (teamId: string, memberId: number, memberName?: string) => {
    const targetTeam = teams.find(t => t.id === teamId);
    if (!targetTeam || !canEditTeam(targetTeam.name)) return;
    const ok = await confirm({
      title: 'Remove Member?',
      message: `Remove ${memberName ? `"${memberName}"` : 'this member'} from ${targetTeam.name}'s active roster?`,
      confirmText: 'Remove Member',
      cancelText: 'Cancel',
      type: 'danger'
    });
    if (!ok) return;

    const memberList = (targetTeam.memberList || []).filter((m: any) => m.id !== memberId);
    const newCount = Math.max(0, (targetTeam.members || 0) - 1);

    setTeams(teams.map(t => t.id === teamId ? { ...t, members: newCount, memberList } : t));
    saveRoster(teamId, targetTeam.name, newCount, memberList);
  };

  const updateMemberRole = (teamId: string, memberId: number, newRole: string) => {
    const targetTeam = teams.find(t => t.id === teamId);
    if (!targetTeam || !canEditTeam(targetTeam.name)) return;

    const memberList = (targetTeam.memberList || []).map((m: any) => m.id === memberId ? { ...m, role: newRole } : m);
    setTeams(teams.map(t => t.id === teamId ? { ...t, memberList } : t));
    saveRoster(teamId, targetTeam.name, targetTeam.members || 0, memberList);
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '40px' }}>
      <div>
        <span className="heading-sm">03 — Structure</span>
        <h1 className="heading-lg">Manage Teams.</h1>
        {isTeamScoped && (
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px', fontSize: '13px' }}>
            Logged in as {role.toUpperCase()} of <strong>{userTeam}</strong>. You can view all teams and manage members for your assigned team.
          </p>
        )}
      </div>

      {!isTeamScoped && (
        <div className="card" style={{ display: 'flex', flexWrap: 'wrap', gap: '24px', alignItems: 'center' }}>
          <input 
            className="input-field" 
            placeholder="Team Name" 
            value={newTeam.name} 
            onChange={e => setNewTeam({...newTeam, name: e.target.value})} 
            style={{ flex: '1 1 200px' }}
          />
          <input 
            className="input-field" 
            placeholder="Short Description" 
            value={newTeam.desc} 
            onChange={e => setNewTeam({...newTeam, desc: e.target.value})} 
            style={{ flex: '2 1 300px' }}
          />
          <button className="btn-primary" onClick={addTeam}><Plus size={18} /> Add Team</button>
        </div>
      )}

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(320px, 1fr))', gap: '32px', alignItems: 'start' }}>
        {teams.map((t, i) => {
          const editable = canEditTeam(t.name);
          const isOwn = isTeamScoped && editable;

          return (
            <div 
              key={t.id} 
              className="card" 
              style={{ 
                position: 'relative', 
                display: 'flex', 
                flexDirection: 'column',
                border: isOwn ? '3px solid #0E1013' : undefined,
                boxShadow: isOwn ? '6px 6px 0px #FFC629' : undefined
              }}
            >
              {!isTeamScoped && (
                <button 
                  style={{ position: 'absolute', top: '-12px', right: '-12px', background: '#E53935', border: '2px solid #0E1013', color: '#FFF', padding: '8px', cursor: 'pointer', boxShadow: '2px 2px 0px #0E1013' }} 
                  onClick={() => removeTeam(t.id, t.name)}
                >
                  <Trash2 size={18} />
                </button>
              )}
              
              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '8px' }}>
                <span className="font-mono" style={{ background: '#0E1013', color: '#FFF', padding: '4px 8px', fontSize: '12px', fontWeight: 700 }}>
                  0{i+1} / {t.name}
                </span>
                {isOwn ? (
                  <span className="font-mono" style={{ background: '#FFC629', color: '#0E1013', border: '1.5px solid #0E1013', padding: '2px 8px', fontSize: '11px', fontWeight: 800, boxShadow: '2px 2px 0px #0E1013' }}>
                    ★ YOUR TEAM
                  </span>
                ) : isTeamScoped ? (
                  <span className="font-mono" style={{ background: '#F7F5F0', color: '#0E1013', border: '1.5px solid rgba(14,16,19,0.3)', padding: '2px 8px', fontSize: '11px', fontWeight: 600, display: 'flex', alignItems: 'center', gap: '4px', opacity: 0.7 }}>
                    <Lock size={10} /> VIEW ONLY
                  </span>
                ) : null}
              </div>

              <h3 style={{ margin: '12px 0', fontSize: '22px', fontWeight: '900', textTransform: 'uppercase', lineHeight: 1.1 }}>
                {t.desc || 'No description'}
              </h3>
              
              <div style={{ display: 'flex', flexDirection: 'column', gap: '8px', margin: '8px 0 20px 0', background: '#F7F5F0', padding: '12px 14px', border: '1.5px solid #0E1013' }}>
                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: '8px' }}>
                  <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', opacity: 0.7 }}>Manager:</span>
                  {t.manager ? (
                    <span style={{ background: '#4ADE80', color: '#0E1013', padding: '2px 8px', border: '1.5px solid #0E1013', fontSize: '11px', fontWeight: 700, fontFamily: 'IBM Plex Mono', boxShadow: '2px 2px 0px #0E1013' }}>
                      {t.manager}
                    </span>
                  ) : (
                    <span className="font-mono" style={{ fontSize: '11px', opacity: 0.4 }}>Not assigned</span>
                  )}
                </div>
                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: '8px' }}>
                  <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', opacity: 0.7 }}>Vice Manager:</span>
                  {t.viceManager ? (
                    <span style={{ background: '#FACC15', color: '#0E1013', padding: '2px 8px', border: '1.5px solid #0E1013', fontSize: '11px', fontWeight: 700, fontFamily: 'IBM Plex Mono', boxShadow: '2px 2px 0px #0E1013' }}>
                      {t.viceManager}
                    </span>
                  ) : (
                    <span className="font-mono" style={{ fontSize: '11px', opacity: 0.4 }}>Not assigned</span>
                  )}
                </div>
              </div>

              <div 
                style={{ display: 'inline-flex', alignItems: 'center', gap: '8px', border: '2px solid #0E1013', padding: '6px 12px', fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', cursor: 'pointer', alignSelf: 'flex-start', background: expandedTeam === t.id ? '#0E1013' : 'transparent', color: expandedTeam === t.id ? '#FFF' : '#0E1013' }}
                onClick={() => setExpandedTeam(expandedTeam === t.id ? null : t.id)}
              >
                <UsersIcon size={16} />
                {t.members || (t.memberList || []).length} active members
              </div>

              {expandedTeam === t.id && (
                <div style={{ marginTop: '24px', paddingTop: '24px', borderTop: '2px solid #0E1013', display: 'flex', flexDirection: 'column', gap: '16px' }}>
                  <span className="heading-sm">Team Roster</span>
                  
                  {(t.memberList || []).map((m: any) => (
                    <div key={m.id} style={{ display: 'flex', alignItems: 'center', gap: '12px', background: '#F7F5F0', padding: '8px 12px', border: '1.5px solid #0E1013' }}>
                      <div style={{ flex: 1, fontWeight: 800 }}>{m.name}</div>
                      {editable ? (
                        <select 
                          className="input-field" 
                          style={{ padding: '4px 8px', fontSize: '12px', width: 'auto' }}
                          value={m.role}
                          onChange={e => updateMemberRole(t.id, m.id, e.target.value)}
                        >
                          <option value="Member">Member</option>
                          <option value="Manager">Manager</option>
                          <option value="Director">Director</option>
                        </select>
                      ) : (
                        <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, padding: '2px 8px', background: '#FFF', border: '1px solid #0E1013' }}>
                          {m.role}
                        </span>
                      )}
                      {editable && (
                        <button style={{ background: 'transparent', border: 'none', color: '#E53935', cursor: 'pointer', padding: '4px' }} onClick={() => removeMember(t.id, m.id, m.name)}>
                          <Trash2 size={16} />
                        </button>
                      )}
                    </div>
                  ))}
                  
                  {(t.memberList || []).length === 0 && (
                    <div style={{ fontSize: '12px', fontStyle: 'italic', opacity: 0.6 }}>No members added yet.</div>
                  )}

                  {editable && (
                    <div style={{ display: 'flex', gap: '8px', marginTop: '8px' }}>
                      <input 
                        className="input-field" 
                        placeholder="New Member Name"
                        style={{ padding: '8px', fontSize: '12px', flex: 1 }}
                        value={newMember.name}
                        onChange={e => setNewMember({...newMember, name: e.target.value})}
                      />
                      <select 
                        className="input-field" 
                        style={{ padding: '8px', fontSize: '12px', width: 'auto' }}
                        value={newMember.role}
                        onChange={e => setNewMember({...newMember, role: e.target.value})}
                      >
                        <option value="Member">Member</option>
                        <option value="Manager">Manager</option>
                        <option value="Director">Director</option>
                      </select>
                      <button className="btn-primary" style={{ padding: '8px 12px', fontSize: '12px' }} onClick={() => addMember(t.id)} title="Add Member to Team">
                        <UserPlus size={16} />
                      </button>
                    </div>
                  )}
                </div>
              )}
            </div>
          );
        })}
      </div>
    </div>
  );
}
