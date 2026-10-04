import { useState } from 'react';
import { Trash2, Users as UsersIcon, Plus, UserPlus, Lock, Edit2, X } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { api } from '../app/platform';
import type { Member, Team } from '../app/responses';
import type { CallResult } from '../app/ui';
import { ErrorBanner, describe, run, useLoad, useSession } from '../app/ui';
import {
  routeMembersAdd,
  routeMembersList,
  routeMembersRemove,
  routeMembersUpdate,
  routeTeamsCreate,
  routeTeamsDelete,
  routeTeamsLeads,
  routeTeamsList,
  routeTeamsUpdate,
} from '../api/hammer.generated';

const kMemberRoles = ['Member', 'Manager', 'Vice Manager', 'Director'];
const same = (a: string, b: string) => a.trim().toLowerCase() === b.trim().toLowerCase();

export default function Teams() {
  const { confirm } = useConfirm();
  const { me, superadmin } = useSession();
  const teams = useLoad((signal) => api.call(routeTeamsList, { signal }), []);
  const leads = useLoad((signal) => api.call(routeTeamsLeads, { signal }), []);
  const [error, setError] = useState<string | null>(null);
  const [newTeam, setNewTeam] = useState({ name: '', desc: '' });
  const [expandedTeam, setExpandedTeam] = useState<string | null>(null);
  const [editingTeam, setEditingTeam] = useState<Team | null>(null);
  const [editName, setEditName] = useState('');
  const [editDesc, setEditDesc] = useState('');
  const [savingEdit, setSavingEdit] = useState(false);

  // A manager or vice manager edits their own team; creating and deleting
  // teams are for the board. The server enforces this either way.
  const userTeam = me.team;
  const isTeamScoped = !superadmin && (me.role === 'manager' || me.role === 'vice manager') && !!userTeam;
  const canEditTeam = (teamName: string) => !isTeamScoped || same(teamName, userTeam);
  const list = teams.data?.teams ?? [];
  const leadOf = (team: string, role: string) =>
    (leads.data?.leads ?? []).filter((l) => l.role === role && same(l.team, team)).map((l) => l.email).join(', ');

  const act = async <T,>(action: (signal: AbortSignal) => Promise<CallResult<T>>): Promise<boolean> => {
    const result = await run(action);
    setError(result.ok ? null : describe(result.error));
    teams.reload();
    return result.ok;
  };

  const addTeam = async () => {
    if (!newTeam.name.trim() || isTeamScoped) return;
    const ok = await act((signal) => api.call(routeTeamsCreate, { body: { name: newTeam.name.trim(), desc: newTeam.desc.trim(), recruiting: true, showcase: true }, signal }));
    if (ok) setNewTeam({ name: '', desc: '' });
  };

  const removeTeam = async (team: Team) => {
    const ok = await confirm({
      title: 'Delete Team?',
      message: `Are you sure you want to delete "${team.name}"? This will remove the team, its roster, and its recruitment choice. Applications keep the team name.`,
      confirmText: 'Delete Team',
      cancelText: 'Cancel',
      type: 'danger',
    });
    if (ok) await act((signal) => api.call(routeTeamsDelete, { params: { id: team.id }, body: {}, signal }));
  };

  const handleUpdateTeam = async () => {
    if (!editingTeam || !editName.trim()) return;
    setSavingEdit(true);
    const ok = await act((signal) => api.call(routeTeamsUpdate, { params: { id: editingTeam.id }, body: { version: editingTeam.version, name: editName.trim(), desc: editDesc.trim() }, signal }));
    setSavingEdit(false);
    if (ok) { setEditingTeam(null); leads.reload(); }
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '40px' }}>
      <div>
        <span className="heading-sm">03 — Structure</span>
        <h1 className="heading-lg">Manage Teams.</h1>
        {isTeamScoped && (
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px', fontSize: '13px' }}>
            Logged in as {me.role.toUpperCase()} of <strong>{userTeam}</strong>. You can view all teams and manage members for your assigned team.
          </p>
        )}
      </div>

      <ErrorBanner message={teams.error ?? error} />

      {!isTeamScoped && (
        <form className="card" style={{ display: 'flex', flexWrap: 'wrap', gap: '24px', alignItems: 'center' }} onSubmit={(e) => { e.preventDefault(); void addTeam(); }}>
          <input className="input-field" placeholder="Team Name" maxLength={80} value={newTeam.name} onChange={(e) => setNewTeam({ ...newTeam, name: e.target.value })} style={{ flex: '1 1 200px' }} />
          <input className="input-field" placeholder="Short Description" maxLength={600} value={newTeam.desc} onChange={(e) => setNewTeam({ ...newTeam, desc: e.target.value })} style={{ flex: '2 1 300px' }} />
          <button className="btn-primary" type="submit"><Plus size={18} /> Add Team</button>
        </form>
      )}

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(320px, 1fr))', gap: '32px', alignItems: 'start' }}>
        {list.map((t, i) => {
          const editable = canEditTeam(t.name);
          const isOwn = isTeamScoped && editable;
          const manager = leadOf(t.name, 'manager');
          const viceManager = leadOf(t.name, 'vice manager');
          return (
            <div key={t.id} className="card" style={{ position: 'relative', display: 'flex', flexDirection: 'column', border: isOwn ? '3px solid #0E1013' : undefined, boxShadow: isOwn ? '6px 6px 0px #FFC629' : undefined }}>
              {!isTeamScoped && (
                <button aria-label="Delete team" style={{ position: 'absolute', top: '-12px', right: '-12px', background: '#E53935', border: '2px solid #0E1013', color: '#FFF', padding: '8px', cursor: 'pointer', boxShadow: '2px 2px 0px #0E1013' }} onClick={() => removeTeam(t)}>
                  <Trash2 size={18} />
                </button>
              )}

              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '8px', gap: '8px' }}>
                <span className="font-mono" style={{ background: '#0E1013', color: '#FFF', padding: '4px 8px', fontSize: '12px', fontWeight: 700 }} dir="auto">
                  {String(i + 1).padStart(2, '0')} / {t.name}
                </span>
                {isOwn ? (
                  <span className="font-mono" style={{ background: '#FFC629', color: '#0E1013', border: '1.5px solid #0E1013', padding: '2px 8px', fontSize: '11px', fontWeight: 800, boxShadow: '2px 2px 0px #0E1013' }}>★ YOUR TEAM</span>
                ) : isTeamScoped ? (
                  <span className="font-mono" style={{ background: '#F7F5F0', color: '#0E1013', border: '1.5px solid rgba(14,16,19,0.3)', padding: '2px 8px', fontSize: '11px', fontWeight: 600, display: 'flex', alignItems: 'center', gap: '4px', opacity: 0.7 }}>
                    <Lock size={10} /> VIEW ONLY
                  </span>
                ) : null}
              </div>

              <h3 style={{ margin: '12px 0', fontSize: '22px', fontWeight: 900, textTransform: 'uppercase', lineHeight: 1.1 }} dir="auto">{t.desc || 'No description'}</h3>

              <div style={{ display: 'flex', flexDirection: 'column', gap: '8px', margin: '8px 0 20px 0', background: '#F7F5F0', padding: '12px 14px', border: '1.5px solid #0E1013' }}>
                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: '8px' }}>
                  <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', opacity: 0.7 }}>Manager:</span>
                  {manager ? (
                    <span style={{ background: '#4ADE80', color: '#0E1013', padding: '2px 8px', border: '1.5px solid #0E1013', fontSize: '11px', fontWeight: 700, fontFamily: 'IBM Plex Mono', boxShadow: '2px 2px 0px #0E1013', wordBreak: 'break-all' }}>{manager}</span>
                  ) : (
                    <span className="font-mono" style={{ fontSize: '11px', opacity: 0.4 }}>Not assigned</span>
                  )}
                </div>
                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: '8px' }}>
                  <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', opacity: 0.7 }}>Vice Manager:</span>
                  {viceManager ? (
                    <span style={{ background: '#FACC15', color: '#0E1013', padding: '2px 8px', border: '1.5px solid #0E1013', fontSize: '11px', fontWeight: 700, fontFamily: 'IBM Plex Mono', boxShadow: '2px 2px 0px #0E1013', wordBreak: 'break-all' }}>{viceManager}</span>
                  ) : (
                    <span className="font-mono" style={{ fontSize: '11px', opacity: 0.4 }}>Not assigned</span>
                  )}
                </div>
              </div>

              <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center' }}>
                <button
                  type="button"
                  style={{ display: 'inline-flex', alignItems: 'center', gap: '8px', border: '2px solid #0E1013', padding: '6px 12px', fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', cursor: 'pointer', alignSelf: 'flex-start', background: expandedTeam === t.id ? '#0E1013' : 'transparent', color: expandedTeam === t.id ? '#FFF' : '#0E1013', fontFamily: 'inherit' }}
                  onClick={() => setExpandedTeam(expandedTeam === t.id ? null : t.id)}
                >
                  <UsersIcon size={16} />
                  {t.members} active members
                </button>
                {editable && (
                  <button
                    className="btn-outline"
                    style={{ padding: '6px 12px', fontSize: '12px', display: 'inline-flex', alignItems: 'center', gap: '6px' }}
                    onClick={() => { setEditingTeam(t); setEditName(t.name); setEditDesc(t.desc); }}
                    title="Edit Team Title and Description"
                  >
                    <Edit2 size={13} /> Edit Details
                  </button>
                )}
              </div>

              {expandedTeam === t.id && <Roster team={t} editable={editable} onChanged={teams.reload} />}
            </div>
          );
        })}
        {!teams.loading && list.length === 0 && <p style={{ opacity: 0.6 }}>No teams yet.</p>}
      </div>

      {editingTeam && (
        <div role="dialog" aria-modal="true" style={{ position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.7)', backdropFilter: 'blur(4px)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1000, padding: '20px' }}>
          <div className="card" style={{ maxWidth: '520px', width: '100%', background: '#FFF', border: '3px solid #0E1013', boxShadow: '10px 10px 0px #0E1013', padding: '32px', position: 'relative' }}>
            <button aria-label="Close" onClick={() => setEditingTeam(null)} style={{ position: 'absolute', top: '20px', right: '20px', background: 'transparent', border: 'none', cursor: 'pointer' }}>
              <X size={22} />
            </button>
            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' }}>Team Configuration</span>
            <h2 style={{ fontSize: '26px', fontWeight: 900, textTransform: 'uppercase', margin: '0 0 20px 0' }}>Edit Team Details</h2>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
              <div>
                <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px', textTransform: 'uppercase' }}>Team Name / Title:</label>
                <input className="input-field" maxLength={80} value={editName} onChange={(e) => setEditName(e.target.value)} placeholder="e.g. Presentation, Social Media, etc." dir="auto" />
              </div>
              <div>
                <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px', textTransform: 'uppercase' }}>Description:</label>
                <textarea className="input-field" rows={4} maxLength={600} value={editDesc} onChange={(e) => setEditDesc(e.target.value)} placeholder="Describe the team's mission, goals, or responsibilities..." dir="auto" />
              </div>
            </div>
            <ErrorBanner message={error} />
            <div style={{ marginTop: '28px', display: 'flex', gap: '12px' }}>
              <button className="btn-primary" style={{ flex: 1, justifyContent: 'center' }} onClick={handleUpdateTeam} disabled={savingEdit || !editName.trim()}>
                {savingEdit ? 'Saving...' : 'Save Changes'}
              </button>
              <button className="btn-outline" style={{ flex: 1, justifyContent: 'center' }} onClick={() => setEditingTeam(null)} disabled={savingEdit}>Cancel</button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

function Roster({ team, editable, onChanged }: { team: Team; editable: boolean; onChanged: () => void }) {
  const { confirm } = useConfirm();
  const members = useLoad((signal) => api.call(routeMembersList, { params: { id: team.id }, signal }), [team.id]);
  const [newMember, setNewMember] = useState({ name: '', role: 'Member' });
  const [error, setError] = useState<string | null>(null);

  const check = (result: CallResult<unknown>) => {
    setError(result.ok ? null : describe(result.error));
    members.reload();
    onChanged();
  };

  const addMember = async () => {
    if (!newMember.name.trim()) return;
    const result = await run((signal) => api.call(routeMembersAdd, { params: { id: team.id }, body: { name: newMember.name.trim(), role: newMember.role }, signal }));
    check(result);
    if (result.ok) setNewMember({ name: '', role: 'Member' });
  };

  const removeMember = async (m: Member) => {
    const ok = await confirm({ title: 'Remove Member?', message: `Remove "${m.name}" from ${team.name}'s active roster?`, confirmText: 'Remove Member', cancelText: 'Cancel', type: 'danger' });
    if (!ok) return;
    check(await run((signal) => api.call(routeMembersRemove, { params: { id: team.id, member: m.id }, body: {}, signal })));
  };

  const list = members.data?.members ?? [];
  return (
    <div style={{ marginTop: '24px', paddingTop: '24px', borderTop: '2px solid #0E1013', display: 'flex', flexDirection: 'column', gap: '16px' }}>
      <span className="heading-sm">Team Roster</span>
      <ErrorBanner message={members.error ?? error} />
      {list.map((m) => (
        <div key={m.id} style={{ display: 'flex', alignItems: 'center', gap: '12px', background: '#F7F5F0', padding: '8px 12px', border: '1.5px solid #0E1013' }}>
          <div style={{ flex: 1, fontWeight: 800 }} dir="auto">{m.name}</div>
          {editable ? (
            <select
              className="input-field"
              style={{ padding: '4px 8px', fontSize: '12px', width: 'auto' }}
              value={m.role}
              onChange={async (e) => {
                const role = e.target.value;
                check(await run((signal) => api.call(routeMembersUpdate, { params: { id: team.id, member: m.id }, body: { version: m.version, role }, signal })));
              }}
            >
              {[m.role, ...kMemberRoles.filter((r) => r !== m.role)].map((r) => <option key={r} value={r}>{r}</option>)}
            </select>
          ) : (
            <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, padding: '2px 8px', background: '#FFF', border: '1px solid #0E1013' }}>{m.role}</span>
          )}
          {editable && (
            <button aria-label="Remove member" style={{ background: 'transparent', border: 'none', color: '#E53935', cursor: 'pointer', padding: '4px' }} onClick={() => removeMember(m)}>
              <Trash2 size={16} />
            </button>
          )}
        </div>
      ))}
      {!members.loading && list.length === 0 && <div style={{ fontSize: '12px', fontStyle: 'italic', opacity: 0.6 }}>No members added yet.</div>}
      {editable && (
        <form style={{ display: 'flex', gap: '8px', marginTop: '8px' }} onSubmit={(e) => { e.preventDefault(); void addMember(); }}>
          <input className="input-field" placeholder="New Member Name" maxLength={120} style={{ padding: '8px', fontSize: '12px', flex: 1 }} value={newMember.name} onChange={(e) => setNewMember({ ...newMember, name: e.target.value })} />
          <select className="input-field" style={{ padding: '8px', fontSize: '12px', width: 'auto' }} value={newMember.role} onChange={(e) => setNewMember({ ...newMember, role: e.target.value })}>
            {kMemberRoles.map((r) => <option key={r} value={r}>{r}</option>)}
          </select>
          <button className="btn-primary" type="submit" style={{ padding: '8px 12px', fontSize: '12px' }} title="Add Member to Team" aria-label="Add member">
            <UserPlus size={16} />
          </button>
        </form>
      )}
    </div>
  );
}
