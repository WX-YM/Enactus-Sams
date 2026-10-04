import { useState } from 'react';
import { ArrowUp, ArrowDown, Trash2, Users, X } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { api } from '../app/platform';
import type { Team } from '../app/responses';
import type { CallResult } from '../app/ui';
import { ErrorBanner, describe, run, useLoad, useSession } from '../app/ui';
import {
  routeMembersAdd,
  routeMembersList,
  routeMembersRemove,
  routeMembersUpdate,
  routeTeamsCreate,
  routeTeamsDelete,
  routeTeamsList,
  routeTeamsReorder,
  routeTeamsUpdate,
} from '../api/hammer.generated';

export default function Teams() {
  const { confirm } = useConfirm();
  const { me, superadmin } = useSession();
  const teams = useLoad((signal) => api.call(routeTeamsList, { signal }), []);
  const [error, setError] = useState<string | null>(null);
  const [name, setName] = useState('');
  const [desc, setDesc] = useState('');
  const [roster, setRoster] = useState<Team | null>(null);

  // A team manager edits their own team; creating, deleting and reordering
  // are for the board. The server enforces this either way.
  const scoped = !superadmin && (me.role === 'manager' || me.role === 'vice manager') && me.team !== '';
  const list = teams.data?.teams ?? [];

  const act = async <T,>(action: (signal: AbortSignal) => Promise<CallResult<T>>) => {
    const result = await run(action);
    setError(result.ok ? null : describe(result.error));
    teams.reload();
  };

  const move = async (index: number, delta: number) => {
    const order = list.map((t) => t.id);
    const target = index + delta;
    if (target < 0 || target >= order.length) return;
    [order[index], order[target]] = [order[target], order[index]];
    await act((signal) => api.call(routeTeamsReorder, { body: { order }, signal }));
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px' }}>
      <div>
        <span className="heading-sm">03 — Structure</span>
        <h1 className="heading-lg">Teams.</h1>
      </div>
      <ErrorBanner message={teams.error ?? error} />

      {!scoped && (
        <form
          className="card"
          style={{ display: 'grid', gap: '12px' }}
          onSubmit={async (e) => {
            e.preventDefault();
            await act((signal) => api.call(routeTeamsCreate, { body: { name: name.trim(), desc: desc.trim(), recruiting: true, showcase: true }, signal }));
            setName('');
            setDesc('');
          }}
        >
          <h2 className="heading-sm">New team</h2>
          <input className="input-field" placeholder="Team name" value={name} maxLength={80} required onChange={(e) => setName(e.target.value)} />
          <textarea className="input-field" placeholder="Short description for the website" value={desc} maxLength={600} rows={2} onChange={(e) => setDesc(e.target.value)} />
          <button className="btn-primary" type="submit" style={{ justifySelf: 'start' }}>Add team</button>
        </form>
      )}

      <div style={{ display: 'grid', gap: '16px' }}>
        {list.map((team, index) => (
          <TeamCard
            key={team.id}
            team={team}
            canStructure={!scoped}
            onMove={(delta) => move(index, delta)}
            onRoster={() => setRoster(team)}
            onSave={(body) => act((signal) => api.call(routeTeamsUpdate, { params: { id: team.id }, body: { version: team.version, ...body }, signal }))}
            onDelete={async () => {
              const ok = await confirm({ title: 'Delete team', message: `Delete ${team.name} and its roster of ${team.members}? Applications keep the team name.`, type: 'danger', confirmText: 'Delete' });
              if (ok) await act((signal) => api.call(routeTeamsDelete, { params: { id: team.id }, body: {}, signal }));
            }}
          />
        ))}
        {!teams.loading && list.length === 0 && <p style={{ opacity: 0.6 }}>No teams yet.</p>}
      </div>

      {roster && <Roster team={roster} onClose={() => { setRoster(null); teams.reload(); }} />}
    </div>
  );
}

function TeamCard({ team, canStructure, onMove, onRoster, onSave, onDelete }: {
  team: Team;
  canStructure: boolean;
  onMove: (delta: number) => void;
  onRoster: () => void;
  onSave: (body: Record<string, unknown>) => void;
  onDelete: () => void;
}) {
  const [name, setName] = useState(team.name);
  const [desc, setDesc] = useState(team.desc);
  const dirty = name !== team.name || desc !== team.desc;
  return (
    <div className="card" style={{ display: 'grid', gap: '12px' }}>
      <div style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
        <input className="input-field" style={{ fontWeight: 800, flex: 1 }} value={name} maxLength={80} onChange={(e) => setName(e.target.value)} dir="auto" />
        {canStructure && (
          <>
            <button className="btn-outline" aria-label="Move up" onClick={() => onMove(-1)}><ArrowUp size={16} /></button>
            <button className="btn-outline" aria-label="Move down" onClick={() => onMove(1)}><ArrowDown size={16} /></button>
          </>
        )}
      </div>
      <textarea className="input-field" rows={2} value={desc} maxLength={600} onChange={(e) => setDesc(e.target.value)} dir="auto" />
      <div style={{ display: 'flex', gap: '16px', flexWrap: 'wrap', alignItems: 'center' }}>
        <label style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
          <input type="checkbox" checked={team.recruiting} onChange={(e) => onSave({ recruiting: e.target.checked })} /> On the application form
        </label>
        <label style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
          <input type="checkbox" checked={team.showcase} onChange={(e) => onSave({ showcase: e.target.checked })} /> Shown in “Inside the club”
        </label>
        <button className="btn-outline" onClick={onRoster} style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
          <Users size={16} /> Roster ({team.members})
        </button>
        {dirty && <button className="btn-primary" onClick={() => onSave({ name: name.trim(), desc: desc.trim() })}>Save</button>}
        {canStructure && <button className="btn-danger" style={{ marginLeft: 'auto' }} aria-label="Delete team" onClick={onDelete}><Trash2 size={16} /></button>}
      </div>
    </div>
  );
}

function Roster({ team, onClose }: { team: Team; onClose: () => void }) {
  const members = useLoad((signal) => api.call(routeMembersList, { params: { id: team.id }, signal }), [team.id]);
  const [name, setName] = useState('');
  const [role, setRole] = useState('Member');
  const [error, setError] = useState<string | null>(null);

  const check = (result: CallResult<unknown>) => {
    setError(result.ok ? null : describe(result.error));
    members.reload();
  };

  return (
    <div role="dialog" aria-modal="true" style={{ position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.6)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 50, padding: '16px' }} onClick={onClose}>
      <div className="card" style={{ maxWidth: '640px', width: '100%', maxHeight: '90vh', overflowY: 'auto', background: '#FFF' }} onClick={(e) => e.stopPropagation()}>
        <div style={{ display: 'flex', justifyContent: 'space-between' }}>
          <h2 style={{ margin: 0, fontWeight: 900 }} dir="auto">{team.name} — roster</h2>
          <button className="btn-outline" aria-label="Close" onClick={onClose}><X size={16} /></button>
        </div>
        <ErrorBanner message={members.error ?? error} />
        <table className="table" style={{ marginTop: '16px' }}>
          <tbody>
            {(members.data?.members ?? []).map((m) => (
              <tr key={m.id}>
                <td dir="auto">{m.name}</td>
                <td>
                  <input
                    className="input-field"
                    defaultValue={m.role}
                    maxLength={60}
                    onBlur={async (e) => {
                      const next = e.target.value.trim();
                      if (next === m.role || next === '') return;
                      check(await run((signal) => api.call(routeMembersUpdate, { params: { id: team.id, member: m.id }, body: { version: m.version, role: next }, signal })));
                    }}
                  />
                </td>
                <td style={{ textAlign: 'right' }}>
                  <button className="btn-danger" aria-label="Remove member" onClick={async () => check(await run((signal) => api.call(routeMembersRemove, { params: { id: team.id, member: m.id }, body: {}, signal })))}>
                    <Trash2 size={16} />
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <form
          style={{ display: 'flex', gap: '8px', marginTop: '16px', flexWrap: 'wrap' }}
          onSubmit={async (e) => {
            e.preventDefault();
            check(await run((signal) => api.call(routeMembersAdd, { params: { id: team.id }, body: { name: name.trim(), role: role.trim() }, signal })));
            setName('');
          }}
        >
          <input className="input-field" style={{ flex: 2 }} placeholder="Name" required maxLength={120} value={name} onChange={(e) => setName(e.target.value)} />
          <input className="input-field" style={{ flex: 1 }} placeholder="Role" required maxLength={60} value={role} onChange={(e) => setRole(e.target.value)} />
          <button className="btn-primary" type="submit">Add</button>
        </form>
      </div>
    </div>
  );
}
