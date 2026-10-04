import { useEffect, useState } from 'react';
import { Trash2, Download, ArrowUpDown, Calendar, Search, X } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { api } from '../app/platform';
import type { Application, ApplicationStatus } from '../app/responses';
import { ErrorBanner, describe, run, useSession } from '../app/ui';
import { csvCell, downloadCsv } from '../app/csv';
import {
  routeApplicationsDelete,
  routeApplicationsList,
  routeApplicationsUpdate,
  routeTeamsList,
} from '../api/hammer.generated';

const kFilters = ['all', 'pending', 'accepted', 'rejected', 'referred'] as const;
type Filter = (typeof kFilters)[number];

const same = (a: string, b: string) => a.trim().toLowerCase() === b.trim().toLowerCase();
const nameOf = (a: Application) => `${a.first_name} ${a.last_name}`.trim();
const formatAppDate = (ms: number) => new Date(ms).toISOString().replace('T', ' ').slice(0, 16);

export default function Applications() {
  const { confirm } = useConfirm();
  const { me, superadmin } = useSession();
  const [apps, setApps] = useState<Application[]>([]);
  const [teams, setTeams] = useState<string[]>([]);
  const [selectedApp, setSelectedApp] = useState<Application | null>(null);
  const [decision, setDecision] = useState('');
  const [referTeam, setReferTeam] = useState('');
  const [filter, setFilter] = useState<Filter>('all');
  const [sortOrder, setSortOrder] = useState<'newest' | 'oldest'>('newest');
  const [selectedTeams, setSelectedTeams] = useState<string[]>([]);
  const [searchQuery, setSearchQuery] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const userTeam = me.team;
  const isTeamScoped = !superadmin && (me.role === 'manager' || me.role === 'vice manager') && !!userTeam;

  // The server already limits a manager to their own team and its referrals;
  // every page is fetched so the counts, filters and export see all of them.
  const fetchApps = async () => {
    const all: Application[] = [];
    let after: string | null = null;
    const page = (cursor: string | null) =>
      run((signal) => api.call(routeApplicationsList, { query: cursor === null ? {} : { after: cursor }, signal }));
    for (let i = 0; i < 100; i++) {
      const result = await page(after);
      if (!result.ok) { setError(describe(result.error)); return; }
      all.push(...result.value.applications);
      after = result.value.next;
      if (after === null) break;
    }
    setError(null);
    setApps(all);
  };

  useEffect(() => {
    void fetchApps();
    void run((signal) => api.call(routeTeamsList, { signal })).then((result) => {
      if (result.ok) setTeams(result.value.teams.map((t) => t.name));
    });
  }, []);

  const closeReview = () => {
    setSelectedApp(null);
    setDecision('');
    setReferTeam('');
  };

  const handleAction = async (app: Application, status: ApplicationStatus) => {
    if (status === 'referred' && !referTeam) { setError('Choose the team to refer them to.'); return; }
    const body: Record<string, unknown> = { version: app.version, status, decision };
    if (status === 'referred') body.referred_to = referTeam;
    setBusy(true);
    const result = await run((signal) => api.call(routeApplicationsUpdate, { params: { id: app.id }, body, signal }));
    setBusy(false);
    if (!result.ok) { setError(describe(result.error)); return; }
    closeReview();
    await fetchApps();
  };

  const handleDelete = async (id: string) => {
    const ok = await confirm({
      title: 'Delete Application?',
      message: 'Are you sure you want to permanently delete this application response? This cannot be undone.',
      confirmText: 'Delete',
      cancelText: 'Cancel',
      type: 'danger',
    });
    if (!ok) return;
    const result = await run((signal) => api.call(routeApplicationsDelete, { params: { id }, body: {}, signal }));
    if (!result.ok) { setError(describe(result.error)); return; }
    closeReview();
    await fetchApps();
  };

  const allTeamNames = Array.from(
    new Set([...teams, ...apps.map((a) => a.team), ...apps.map((a) => a.referred_to)].map((t) => t.trim()).filter(Boolean)),
  ).sort((a, b) => a.localeCompare(b));

  const toggleTeam = (teamName: string) => {
    setSelectedTeams((prev) => (prev.some((t) => same(t, teamName)) ? prev.filter((t) => !same(t, teamName)) : [...prev, teamName.trim()]));
  };

  const filteredApps = apps.filter((a) => {
    if (filter !== 'all' && a.status !== filter) return false;
    if (selectedTeams.length > 0) {
      const direct = a.team && selectedTeams.some((t) => same(t, a.team));
      const referred = a.referred_to && selectedTeams.some((t) => same(t, a.referred_to));
      if (!direct && !referred) return false;
    }
    if (searchQuery) {
      const q = searchQuery.toLowerCase();
      const str = `${nameOf(a)} ${a.email} ${a.phone} ${a.team} ${a.reason} ${a.decision}`.toLowerCase();
      if (!str.includes(q)) return false;
    }
    return true;
  });

  const sortedApps = [...filteredApps].sort((a, b) => (sortOrder === 'newest' ? b.created_at - a.created_at : a.created_at - b.created_at));

  const exportCSV = () => {
    if (sortedApps.length === 0) return;
    const headers = ['Applicant Name', 'Email', 'Phone', 'Team Applied', 'Status', 'Referred To', 'Date Submitted', 'Applicant Reason', 'Decision Notes'];
    const rows = sortedApps.map((a) => [nameOf(a), a.email, a.phone, a.team, a.status, a.referred_to, formatAppDate(a.created_at), a.reason, a.decision]);
    downloadCsv(`application_responses_${new Date().toISOString().slice(0, 10)}.csv`, [headers, ...rows].map((r) => r.map(csvCell).join(',')));
  };

  const filterButton = (active: boolean) => ({
    padding: '8px 16px',
    border: '2px solid #0E1013',
    background: active ? '#FFC629' : '#FFF',
    fontFamily: 'IBM Plex Mono, monospace',
    fontSize: '12px',
    fontWeight: 700,
    textTransform: 'uppercase' as const,
    boxShadow: active ? '3px 3px 0px #0E1013' : 'none',
    cursor: 'pointer',
  });

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px', paddingBottom: '40px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">02 — Recruitment</span>
          <h1 className="heading-lg">Application Responses.</h1>
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>
            {isTeamScoped ? `Showing candidate applications for team: ${userTeam}` : 'Review and manage candidate applications submitted via the website recruitment section.'}
          </p>
        </div>
        <div style={{ display: 'flex', gap: '12px', alignItems: 'center', flexWrap: 'wrap' }}>
          <button className="btn-outline" onClick={exportCSV} disabled={sortedApps.length === 0} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Download size={18} /> Export CSV
          </button>
        </div>
      </div>

      {selectedApp === null && <ErrorBanner message={error} />}

      <div style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: '12px' }}>
          <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap' }}>
            {kFilters.map((f) => (
              <button key={f} onClick={() => setFilter(f)} style={filterButton(filter === f)}>
                {f} ({apps.filter((a) => f === 'all' || a.status === f).length})
              </button>
            ))}
          </div>
          <button
            className="btn-outline"
            onClick={() => setSortOrder((s) => (s === 'newest' ? 'oldest' : 'newest'))}
            style={{ display: 'flex', alignItems: 'center', gap: '8px', padding: '8px 16px', fontSize: '12px', fontWeight: 700 }}
            title="Click to toggle sorting order"
          >
            <ArrowUpDown size={14} />
            Sort: {sortOrder === 'newest' ? 'Newest to Oldest' : 'Oldest to Newest'}
          </button>
        </div>

        <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap', alignItems: 'center' }}>
          <div style={{ position: 'relative', flex: '1 1 240px', maxWidth: '360px' }}>
            <Search size={16} style={{ position: 'absolute', left: '12px', top: '50%', transform: 'translateY(-50%)', opacity: 0.5 }} />
            <input className="input-field" style={{ paddingLeft: '36px', fontSize: '13px' }} placeholder="Search applicants..." value={searchQuery} onChange={(e) => setSearchQuery(e.target.value)} />
            {searchQuery && (
              <button aria-label="Clear search" onClick={() => setSearchQuery('')} style={{ position: 'absolute', right: '10px', top: '50%', transform: 'translateY(-50%)', background: 'transparent', border: 'none', cursor: 'pointer', opacity: 0.6 }}>
                <X size={14} />
              </button>
            )}
          </div>

          {!isTeamScoped && allTeamNames.length > 0 && (
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px', flex: '1 1 200px', maxWidth: '280px' }}>
              <select
                className="input-field"
                style={{ fontSize: '13px', fontWeight: 600 }}
                value={selectedTeams.length === 1 ? selectedTeams[0] : selectedTeams.length === 0 ? 'all' : 'multi'}
                onChange={(e) => setSelectedTeams(e.target.value === 'all' ? [] : [e.target.value])}
              >
                <option value="all">All Teams ({allTeamNames.length})</option>
                {selectedTeams.length > 1 && <option value="multi" disabled>Multiple Teams ({selectedTeams.length})</option>}
                {allTeamNames.map((t) => <option key={t} value={t}>{t}</option>)}
              </select>
            </div>
          )}

          {!isTeamScoped && allTeamNames.length > 0 && (
            <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
              <button
                onClick={() => setSelectedTeams([])}
                style={{ padding: '5px 10px', border: '1.5px solid #0E1013', background: selectedTeams.length === 0 ? '#0E1013' : '#FFF', color: selectedTeams.length === 0 ? '#FFC629' : '#0E1013', fontFamily: 'IBM Plex Mono, monospace', fontSize: '11px', fontWeight: 700, cursor: 'pointer' }}
              >
                ALL
              </button>
              {allTeamNames.map((t) => {
                const isSelected = selectedTeams.some((st) => same(st, t));
                return (
                  <button
                    key={t}
                    onClick={() => toggleTeam(t)}
                    style={{ padding: '5px 10px', border: '1.5px solid #0E1013', background: isSelected ? '#FFC629' : '#F7F5F0', color: '#0E1013', fontFamily: 'IBM Plex Mono, monospace', fontSize: '11px', fontWeight: isSelected ? 800 : 600, boxShadow: isSelected ? '2px 2px 0px #0E1013' : 'none', cursor: 'pointer' }}
                  >
                    {t}
                  </button>
                );
              })}
              {selectedTeams.length > 0 && (
                <button onClick={() => setSelectedTeams([])} style={{ padding: '5px 8px', border: 'none', background: 'transparent', color: '#E53935', fontFamily: 'IBM Plex Mono, monospace', fontSize: '11px', fontWeight: 700, cursor: 'pointer', textDecoration: 'underline' }}>
                  Clear
                </button>
              )}
            </div>
          )}
        </div>
      </div>

      <div className="card" style={{ padding: 0, overflowX: 'auto' }}>
        <table className="table">
          <thead>
            <tr>
              <th>Applicant</th>
              <th>Team Applied</th>
              <th>Status</th>
              <th>Reason / Notes</th>
              <th style={{ textAlign: 'right' }}>Action</th>
            </tr>
          </thead>
          <tbody>
            {sortedApps.map((a) => {
              const isReferredToMe = isTeamScoped && a.referred_to && same(a.referred_to, userTeam);
              return (
                <tr key={a.id}>
                  <td>
                    <strong style={{ fontSize: '16px', textTransform: 'uppercase', letterSpacing: '0.02em', display: 'block' }} dir="auto">{nameOf(a)}</strong>
                    <div style={{ display: 'flex', flexDirection: 'column', gap: '2px', marginTop: '4px' }}>
                      {a.email && <span className="font-mono" style={{ fontSize: '11px', opacity: 0.6 }}>{a.email}</span>}
                      {a.phone && <span className="font-mono" style={{ fontSize: '11px', opacity: 0.6 }} dir="ltr">{a.phone}</span>}
                      <div style={{ display: 'flex', alignItems: 'center', gap: '4px', opacity: 0.5, marginTop: '2px' }}>
                        <Calendar size={11} />
                        <span className="font-mono" style={{ fontSize: '10px' }}>{formatAppDate(a.created_at)}</span>
                      </div>
                    </div>
                  </td>
                  <td dir="auto">
                    {a.team}
                    {a.referred_to && (
                      <span className="font-mono" style={{ display: 'block', fontSize: '10px', color: '#B45309', fontWeight: 700 }}>→ Referred to {a.referred_to}</span>
                    )}
                  </td>
                  <td>
                    <span className={`badge badge-${a.status === 'interview_scheduled' ? 'accepted' : a.status}`}>
                      {isReferredToMe && a.status === 'referred' ? '★ Referred to You' : a.status.replace('_', ' ')}
                    </span>
                  </td>
                  <td dir="auto" style={{ color: 'var(--text-dark)', opacity: 0.7, maxWidth: '240px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                    {a.decision || a.reason || '-'}
                  </td>
                  <td style={{ textAlign: 'right', whiteSpace: 'nowrap' }}>
                    <div style={{ display: 'inline-flex', gap: '8px', alignItems: 'center' }}>
                      <button
                        className="btn-outline"
                        style={{ padding: '8px 16px', fontSize: '12px' }}
                        onClick={() => { setError(null); setSelectedApp(a); setDecision(a.decision || a.reason); setReferTeam(a.referred_to); }}
                      >
                        Review
                      </button>
                      {superadmin && (
                        <button className="btn-danger" style={{ padding: '8px 12px', fontSize: '12px', display: 'inline-flex', alignItems: 'center' }} onClick={() => handleDelete(a.id)} title="Delete Application" aria-label="Delete application">
                          <Trash2 size={14} />
                        </button>
                      )}
                    </div>
                  </td>
                </tr>
              );
            })}
            {sortedApps.length === 0 && (
              <tr><td colSpan={5} style={{ textAlign: 'center', opacity: 0.7, padding: '32px' }}>No applications found matching your criteria.</td></tr>
            )}
          </tbody>
        </table>
      </div>

      {selectedApp && (
        <div role="dialog" aria-modal="true" style={{ position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.85)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 100, padding: '20px' }}>
          <div className="card" style={{ width: '100%', maxWidth: '520px', maxHeight: '92vh', overflowY: 'auto', background: '#FFF' }}>
            <span className="heading-sm" style={{ marginBottom: '8px' }}>Applicant Review</span>
            <h2 style={{ marginTop: 0, fontSize: '32px', fontWeight: 900, marginBottom: '8px', textTransform: 'uppercase' }} dir="auto">{nameOf(selectedApp)}</h2>

            <div style={{ display: 'flex', gap: '8px', marginBottom: '16px', flexWrap: 'wrap' }}>
              <span style={{ fontWeight: 700, fontSize: '13px', background: 'var(--brand-yellow)', padding: '4px 8px', border: '2px solid #0E1013' }}>Team: {selectedApp.team}</span>
              {selectedApp.referred_to && (
                <span className="font-mono" style={{ fontWeight: 700, fontSize: '12px', background: '#FEF3C7', color: '#92400E', padding: '4px 8px', border: '2px solid #0E1013' }}>Referred to: {selectedApp.referred_to}</span>
              )}
              {selectedApp.email && <span className="font-mono" style={{ fontSize: '12px', background: '#F7F5F0', padding: '4px 8px', border: '2px solid #0E1013' }}>{selectedApp.email}</span>}
              {selectedApp.phone && <span className="font-mono" style={{ fontSize: '12px', background: '#F7F5F0', padding: '4px 8px', border: '2px solid #0E1013' }} dir="ltr">{selectedApp.phone}</span>}
            </div>

            {selectedApp.reason && (
              <div style={{ background: '#F7F5F0', border: '2px solid #0E1013', padding: '12px', marginBottom: '16px' }}>
                <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', opacity: 0.6, display: 'block', marginBottom: '4px' }}>Applicant's Reason:</span>
                <p style={{ margin: 0, fontSize: '14px', lineHeight: 1.4, whiteSpace: 'pre-wrap' }} dir="auto">{selectedApp.reason}</p>
              </div>
            )}

            {selectedApp.status === 'referred' ? (
              <div style={{ display: 'flex', flexDirection: 'column', gap: '16px', marginTop: '16px' }}>
                <div style={{ background: '#FEF3C7', border: '2px solid #0E1013', padding: '12px' }}>
                  <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', color: '#92400E', display: 'block', marginBottom: '4px' }}>
                    ★ Referral for {selectedApp.referred_to || userTeam}
                  </span>
                  <p style={{ margin: 0, fontSize: '14px', lineHeight: 1.4 }}>
                    Candidate was referred from <strong>{selectedApp.team}</strong>. Accepting adds them to {selectedApp.referred_to} as a member; rejecting marks them rejected.
                  </p>
                </div>
                <div>
                  <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>Manager Response / Feedback:</label>
                  <textarea className="input-field" placeholder="Enter notes or feedback..." rows={3} maxLength={2000} value={decision} onChange={(e) => setDecision(e.target.value)} />
                </div>
                <div style={{ display: 'flex', gap: '12px' }}>
                  <button className="btn-primary" disabled={busy} style={{ flex: 1, justifyContent: 'center', background: '#4ADE80' }} onClick={() => handleAction(selectedApp, 'accepted')}>Accept Referral</button>
                  <button className="btn-danger" disabled={busy} style={{ flex: 1, justifyContent: 'center' }} onClick={() => handleAction(selectedApp, 'rejected')}>Reject Referral</button>
                </div>
              </div>
            ) : (
              <div style={{ display: 'flex', flexDirection: 'column', gap: '16px', marginTop: '16px' }}>
                <div>
                  <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>Decision Notes / Reason:</label>
                  <textarea className="input-field" placeholder="Enter notes or feedback..." rows={3} maxLength={2000} value={decision} onChange={(e) => setDecision(e.target.value)} />
                </div>
                <div style={{ display: 'flex', gap: '12px' }}>
                  <button className="btn-primary" disabled={busy} style={{ flex: 1, justifyContent: 'center', background: '#4ADE80' }} onClick={() => handleAction(selectedApp, 'accepted')}>Accept</button>
                  <button className="btn-danger" disabled={busy} style={{ flex: 1, justifyContent: 'center' }} onClick={() => handleAction(selectedApp, 'rejected')}>Reject</button>
                </div>
                <div style={{ borderTop: '2px solid var(--text-dark)', paddingTop: '16px', marginTop: '8px' }}>
                  <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '8px', textTransform: 'uppercase' }}>Refer to another team:</span>
                  <div style={{ display: 'flex', gap: '12px' }}>
                    <select className="input-field" value={referTeam} onChange={(e) => setReferTeam(e.target.value)}>
                      <option value="">Select Team...</option>
                      {teams.filter((name) => !same(name, selectedApp.team)).map((name) => <option key={name} value={name}>{name}</option>)}
                    </select>
                    <button className="btn-outline" disabled={busy || !referTeam} onClick={() => handleAction(selectedApp, 'referred')}>Refer</button>
                  </div>
                </div>
              </div>
            )}

            <div style={{ marginTop: '16px' }}><ErrorBanner message={error} /></div>

            {superadmin && (
              <button className="btn-danger" style={{ width: '100%', marginTop: '16px', display: 'flex', alignItems: 'center', justifyContent: 'center', gap: '8px', padding: '12px' }} onClick={() => handleDelete(selectedApp.id)}>
                <Trash2 size={16} /> Delete Application
              </button>
            )}
            <button className="btn-outline" style={{ width: '100%', marginTop: '12px' }} onClick={() => { closeReview(); setError(null); }}>Close</button>
          </div>
        </div>
      )}
    </div>
  );
}
