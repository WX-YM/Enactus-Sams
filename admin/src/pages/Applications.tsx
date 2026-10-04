import { useMemo, useState } from 'react';
import { Trash2, Search, X, RefreshCw } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { api } from '../app/platform';
import type { Application, ApplicationStatus } from '../app/responses';
import { ErrorBanner, describe, formatDate, run, useLoad, useSession } from '../app/ui';
import {
  routeApplicationsDelete,
  routeApplicationsList,
  routeApplicationsUpdate,
  routeTeamsList,
} from '../api/hammer.generated';

const kStatuses: readonly { value: ApplicationStatus; label: string; badge: string }[] = [
  { value: 'pending', label: 'Pending', badge: 'badge-pending' },
  { value: 'interview_scheduled', label: 'Interview', badge: 'badge-pending' },
  { value: 'accepted', label: 'Accepted', badge: 'badge-accepted' },
  { value: 'referred', label: 'Referred', badge: 'badge-referred' },
  { value: 'rejected', label: 'Rejected', badge: 'badge-rejected' },
];

const statusOf = (value: string) => kStatuses.find((s) => s.value === value) ?? kStatuses[0];

export default function Applications() {
  const { confirm } = useConfirm();
  const { me, superadmin, affords } = useSession();
  const [filter, setFilter] = useState<'all' | ApplicationStatus>('all');
  const [search, setSearch] = useState('');
  const [pages, setPages] = useState<Application[]>([]);
  const [cursor, setCursor] = useState<string | null>(null);
  const [selected, setSelected] = useState<Application | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);

  const first = useLoad(async (signal) => {
    const result = await api.call(routeApplicationsList, {
      query: filter === 'all' ? {} : { status: filter },
      signal,
    });
    if (result.ok) {
      setPages([...result.value.applications]);
      setCursor(result.value.next);
    }
    return result;
  }, [filter]);
  const teams = useLoad((signal) => api.call(routeTeamsList, { signal }), []);

  const loadMore = async () => {
    if (cursor === null) return;
    const result = await run((signal) => api.call(routeApplicationsList, {
      query: filter === 'all' ? { after: cursor } : { after: cursor, status: filter },
      signal,
    }));
    if (result.ok) {
      setPages((prev) => [...prev, ...result.value.applications]);
      setCursor(result.value.next);
    } else {
      setActionError(describe(result.error));
    }
  };

  const shown = useMemo(() => {
    const q = search.trim().toLowerCase();
    if (!q) return pages;
    return pages.filter((a) =>
      `${a.first_name} ${a.last_name} ${a.email} ${a.phone} ${a.team}`.toLowerCase().includes(q));
  }, [pages, search]);

  const scoped = !superadmin && (me.role === 'manager' || me.role === 'vice manager') && me.team !== '';

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'end', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">02 — Recruitment</span>
          <h1 className="heading-lg">Applications.</h1>
          {scoped && <p className="font-mono" style={{ fontSize: '12px', opacity: 0.7 }}>Showing applications for {me.team} and referrals to it.</p>}
        </div>
        <button className="btn-outline" onClick={first.reload} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
          <RefreshCw size={16} /> Refresh
        </button>
      </div>

      <ErrorBanner message={first.error ?? actionError} />

      <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap', alignItems: 'center' }}>
        {(['all', ...kStatuses.map((s) => s.value)] as const).map((value) => (
          <button key={value} className={filter === value ? 'btn-primary' : 'btn-outline'} onClick={() => setFilter(value)}>
            {value === 'all' ? 'All' : statusOf(value).label}
          </button>
        ))}
        <div style={{ position: 'relative', marginLeft: 'auto', minWidth: '240px' }}>
          <Search size={16} style={{ position: 'absolute', left: '12px', top: '50%', transform: 'translateY(-50%)', opacity: 0.5 }} />
          <input className="input-field" style={{ paddingLeft: '36px' }} placeholder="Search loaded applications" value={search} onChange={(e) => setSearch(e.target.value)} />
        </div>
      </div>

      <div className="card" style={{ padding: 0, overflowX: 'auto' }}>
        <table className="table">
          <thead>
            <tr><th>Applicant</th><th>Team</th><th>Status</th><th>Received</th><th /></tr>
          </thead>
          <tbody>
            {shown.map((a) => (
              <tr key={a.id} style={{ cursor: 'pointer' }} onClick={() => { setSelected(a); setActionError(null); }}>
                <td>
                  <div style={{ fontWeight: 800 }} dir="auto">{a.first_name} {a.last_name}</div>
                  <div className="font-mono" style={{ fontSize: '12px', opacity: 0.7 }}>{a.email}</div>
                </td>
                <td dir="auto">{a.team}{a.referred_to && <div className="font-mono" style={{ fontSize: '11px' }}>→ {a.referred_to}</div>}</td>
                <td><span className={`badge ${statusOf(a.status).badge}`}>{statusOf(a.status).label}</span></td>
                <td className="font-mono" style={{ fontSize: '12px' }}>{formatDate(a.created_at)}</td>
                <td style={{ textAlign: 'right' }}>
                  {superadmin && affords(routeApplicationsDelete) && (
                    <button
                      className="btn-danger"
                      aria-label="Delete application"
                      onClick={async (e) => {
                        e.stopPropagation();
                        const ok = await confirm({ title: 'Delete application', message: `Permanently delete ${a.first_name} ${a.last_name}'s application? This cannot be undone.`, type: 'danger', confirmText: 'Delete' });
                        if (!ok) return;
                        const result = await run((signal) => api.call(routeApplicationsDelete, { params: { id: a.id }, body: {}, signal }));
                        if (result.ok) setPages((prev) => prev.filter((p) => p.id !== a.id));
                        else setActionError(describe(result.error));
                      }}
                    >
                      <Trash2 size={16} />
                    </button>
                  )}
                </td>
              </tr>
            ))}
            {!first.loading && shown.length === 0 && (
              <tr><td colSpan={5} style={{ textAlign: 'center', opacity: 0.6, padding: '32px' }}>No applications.</td></tr>
            )}
          </tbody>
        </table>
      </div>
      {cursor !== null && <button className="btn-outline" onClick={loadMore} style={{ alignSelf: 'center' }}>Load more</button>}

      {selected && (
        <Review
          application={selected}
          teams={(teams.data?.teams ?? []).map((t) => t.name)}
          canMoveTeam={!scoped}
          onClose={() => setSelected(null)}
          onSaved={(next) => {
            setPages((prev) => prev.map((p) => (p.id === next.id ? next : p)));
            setSelected(null);
          }}
        />
      )}
    </div>
  );
}

function Review({ application, teams, canMoveTeam, onClose, onSaved }: {
  application: Application;
  teams: readonly string[];
  canMoveTeam: boolean;
  onClose: () => void;
  onSaved: (next: Application) => void;
}) {
  const [status, setStatus] = useState<ApplicationStatus>(application.status);
  const [decision, setDecision] = useState(application.decision);
  const [referral, setReferral] = useState(application.referred_to);
  const [team, setTeam] = useState(application.team);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  const save = async () => {
    setSaving(true);
    setError(null);
    const body: Record<string, unknown> = { version: application.version, status, decision };
    if (referral !== application.referred_to) body.referred_to = referral === '' ? null : referral;
    if (canMoveTeam && team !== application.team) body.team = team;
    const result = await run((signal) => api.call(routeApplicationsUpdate, { params: { id: application.id }, body, signal }));
    setSaving(false);
    if (!result.ok) {
      setError(describe(result.error));
      return;
    }
    onSaved({ ...application, version: result.value.version, status, decision, referred_to: referral, team });
  };

  return (
    <div role="dialog" aria-modal="true" style={{ position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.6)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 50, padding: '16px' }} onClick={onClose}>
      <div className="card" style={{ maxWidth: '640px', width: '100%', maxHeight: '90vh', overflowY: 'auto', background: '#FFF' }} onClick={(e) => e.stopPropagation()}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'start' }}>
          <div>
            <h2 style={{ margin: 0, fontWeight: 900 }} dir="auto">{application.first_name} {application.last_name}</h2>
            <p className="font-mono" style={{ fontSize: '13px', margin: '4px 0' }}>{application.email} · <span dir="ltr">{application.phone}</span></p>
            <p className="font-mono" style={{ fontSize: '12px', opacity: 0.6, margin: 0 }}>Received {formatDate(application.created_at)}</p>
          </div>
          <button className="btn-outline" aria-label="Close" onClick={onClose}><X size={16} /></button>
        </div>

        <h3 className="heading-sm" style={{ marginTop: '24px' }}>Why they want to join</h3>
        <p style={{ whiteSpace: 'pre-wrap' }} dir="auto">{application.reason || '—'}</p>

        <div style={{ display: 'grid', gap: '16px', marginTop: '24px' }}>
          <label>
            <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase' }}>Status</span>
            <select className="input-field" value={status} onChange={(e) => setStatus(e.target.value as ApplicationStatus)}>
              {kStatuses.map((s) => <option key={s.value} value={s.value}>{s.label}</option>)}
            </select>
          </label>
          <label>
            <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase' }}>Team applied to</span>
            <select className="input-field" value={team} disabled={!canMoveTeam} onChange={(e) => setTeam(e.target.value)}>
              {[team, ...teams.filter((t) => t !== team)].map((t) => <option key={t} value={t}>{t}</option>)}
            </select>
          </label>
          <label>
            <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase' }}>Referred to</span>
            <select className="input-field" value={referral} onChange={(e) => setReferral(e.target.value)}>
              <option value="">No referral</option>
              {teams.map((t) => <option key={t} value={t}>{t}</option>)}
            </select>
          </label>
          <label>
            <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase' }}>Decision note (staff only)</span>
            <textarea className="input-field" rows={3} maxLength={2000} value={decision} onChange={(e) => setDecision(e.target.value)} />
          </label>
          {status === 'accepted' && application.status !== 'accepted' && (
            <p className="font-mono" style={{ fontSize: '12px' }}>Accepting adds them to the roster of {referral || team}.</p>
          )}
        </div>

        <div style={{ marginTop: '16px' }}><ErrorBanner message={error} /></div>
        <div style={{ display: 'flex', gap: '12px', justifyContent: 'flex-end', marginTop: '24px' }}>
          <button className="btn-outline" onClick={onClose}>Cancel</button>
          <button className="btn-primary" onClick={save} disabled={saving}>{saving ? 'Saving…' : 'Save'}</button>
        </div>
      </div>
    </div>
  );
}
