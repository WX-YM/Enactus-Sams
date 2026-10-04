import { useEffect, useState } from 'react';
import { Activity, Users, Clock, CheckCircle, Shield, FileText, Layers, RefreshCw } from 'lucide-react';
import { api } from '../app/platform';
import type { AuditRow } from '../app/responses';
import { ErrorBanner, describe, run, useLoad, useSession } from '../app/ui';
import { routeAuditList, routeDashboardGet } from '../api/hammer.generated';

// The audit action names (src/config/audit_actions.h) in the old log's words.
const kActions: Record<string, { type: string; text: string }> = {
  AccessDenied: { type: 'auth', text: 'Refused request' },
  StaffCreated: { type: 'user', text: 'Created a staff account' },
  StaffUpdated: { type: 'user', text: 'Updated a staff account' },
  StaffDisabled: { type: 'user', text: 'Revoked a staff account' },
  ApplicationReceived: { type: 'application', text: 'New application received' },
  ApplicationReviewed: { type: 'application', text: 'Reviewed an application' },
  ApplicationDeleted: { type: 'application', text: 'Deleted an application' },
  TeamCreated: { type: 'team', text: 'Created a team' },
  TeamUpdated: { type: 'team', text: 'Updated a team' },
  TeamDeleted: { type: 'team', text: 'Deleted a team' },
  RosterUpdated: { type: 'team', text: 'Updated a team roster' },
  SectionPublished: { type: 'content', text: 'Published site content' },
  GalleryChanged: { type: 'content', text: 'Changed a gallery' },
  MediaUploaded: { type: 'content', text: 'Uploaded an image' },
  FormCreated: { type: 'content', text: 'Created a form' },
  FormUpdated: { type: 'content', text: 'Updated a form' },
  FormDeleted: { type: 'content', text: 'Deleted a form' },
  FormResponseDeleted: { type: 'content', text: 'Deleted a form response' },
  FormResponsesExported: { type: 'content', text: 'Exported form responses' },
};

const formatRelativeTime = (ms: number) => {
  const diffSec = Math.max(0, Math.floor((Date.now() - ms) / 1000));
  if (diffSec < 60) return 'JUST NOW';
  if (diffSec < 3600) return `${Math.floor(diffSec / 60)}M AGO`;
  if (diffSec < 86400) return `${Math.floor(diffSec / 3600)}H AGO`;
  if (diffSec < 604800) return `${Math.floor(diffSec / 86400)}D AGO`;
  return new Date(ms).toLocaleDateString(undefined, { month: 'short', day: 'numeric' }).toUpperCase();
};

const getLogIcon = (type: string) => {
  switch (type) {
    case 'auth': return <Shield size={16} color="#3B82F6" />;
    case 'user': return <Users size={16} color="#8B5CF6" />;
    case 'application': return <CheckCircle size={16} color="#10B981" />;
    case 'team': return <Layers size={16} color="#F59E0B" />;
    case 'content': return <FileText size={16} color="#EC4899" />;
    default: return <Activity size={16} color="#6B7280" />;
  }
};

const getTypeBadgeStyle = (type: string) => {
  switch (type) {
    case 'auth': return { background: '#DBEAFE', color: '#1E40AF', border: '1.5px solid #1E40AF' };
    case 'user': return { background: '#EDE9FE', color: '#5B21B6', border: '1.5px solid #5B21B6' };
    case 'application': return { background: '#D1FAE5', color: '#065F46', border: '1.5px solid #065F46' };
    case 'team': return { background: '#FEF3C7', color: '#92400E', border: '1.5px solid #92400E' };
    case 'content': return { background: '#FCE7F3', color: '#9D174D', border: '1.5px solid #9D174D' };
    default: return { background: '#F3F4F6', color: '#374151', border: '1.5px solid #374151' };
  }
};

const dayKey = (ms: number) => {
  const d = new Date(ms);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
};

const tag = { fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', background: '#0E1013', color: '#FFF', padding: '4px 8px' } as const;

export default function Dashboard() {
  const { affords } = useSession();
  const { data, error, loading, reload } = useLoad((signal) => api.call(routeDashboardGet, { signal }), []);
  const canLogs = affords(routeAuditList);
  const [logs, setLogs] = useState<AuditRow[]>([]);
  const [logError, setLogError] = useState<string | null>(null);
  const [loadingLogs, setLoadingLogs] = useState(false);

  const fetchLogs = async () => {
    if (!canLogs) return;
    setLoadingLogs(true);
    const result = await run((signal) => api.call(routeAuditList, { query: {}, signal }));
    setLoadingLogs(false);
    if (!result.ok) { setLogError(describe(result.error)); return; }
    setLogError(null);
    setLogs([...result.value.rows]);
  };

  useEffect(() => { void fetchLogs(); }, []);

  const refreshAll = () => { reload(); void fetchLogs(); };

  const apps = data?.applications_total ?? 0;
  const byStatus = data?.applications_by_status ?? {};
  const pipeline = {
    accepted: byStatus.accepted ?? 0,
    scheduled: byStatus.interview_scheduled ?? 0,
    pending: byStatus.pending ?? 0,
    referred: byStatus.referred ?? 0,
    rejected: byStatus.rejected ?? 0,
  };
  const totalApps = apps || 1;
  const visits = data?.visits_total ?? 0;
  const todayStr = dayKey(Date.now());
  const days = (data?.visits ?? []).map((d) => ({ date: dayKey(d.at), count: d.count }));
  const todayVisits = days.find((d) => d.date === todayStr)?.count ?? 0;
  const dailyEntries = [...days].filter((d) => d.count > 0).sort((a, b) => b.date.localeCompare(a.date)).slice(0, 10);
  const maxDailyVisit = Math.max(...dailyEntries.map((d) => d.count), 1);
  const conversionRate = visits > 0 ? ((apps / visits) * 100).toFixed(1) : '0.0';

  const counts: Record<string, number> = {};
  for (const t of data?.teams ?? []) counts[t.name.trim()] = 0;
  for (const [team, n] of Object.entries(data?.applications_by_team ?? {})) counts[team.trim()] = (counts[team.trim()] ?? 0) + n;
  const rankedTeams = Object.entries(counts)
    .map(([name, count]) => ({ name, count, pct: apps > 0 ? (count / totalApps) * 100 : 0 }))
    .sort((a, b) => b.count - a.count || a.name.localeCompare(b.name));

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '40px', paddingBottom: '40px' }}>
      <div>
        <span className="heading-sm">01 — Overview</span>
        <h1 className="heading-lg">Platform Analytics.</h1>
      </div>

      <ErrorBanner message={error} />

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(min(100%, 220px), 1fr))', gap: '24px' }}>
        <div className="card">
          <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Activity size={16} /> Total Visits
          </span>
          <p style={{ fontSize: '52px', fontWeight: 900, margin: '16px 0 0 0', letterSpacing: '-0.04em' }}>{visits.toLocaleString()}</p>
          <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center', marginTop: '12px' }}>
            <div style={{ background: '#4ADE80', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 8px', fontSize: '11px', fontWeight: 800 }}>LAST 30 DAYS</div>
            <div style={{ background: 'var(--brand-yellow)', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 8px', fontSize: '11px', fontWeight: 800 }}>{todayVisits.toLocaleString()} TODAY (24H)</div>
          </div>
        </div>

        <div className="card">
          <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Users size={16} /> Applications
          </span>
          <p style={{ fontSize: '52px', fontWeight: 900, margin: '16px 0 0 0', letterSpacing: '-0.04em' }}>{apps}</p>
          <div style={{ background: 'var(--brand-yellow)', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 8px', fontSize: '11px', fontWeight: 800, marginTop: '12px' }}>
            CONVERSION: {conversionRate}%
          </div>
        </div>

        <div className="card">
          <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px' }}>
            <CheckCircle size={16} /> Active Teams
          </span>
          <p style={{ fontSize: '52px', fontWeight: 900, margin: '16px 0 0 0', letterSpacing: '-0.04em' }}>{data?.teams.length ?? 0}</p>
          <div style={{ background: '#F7F5F0', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 8px', fontSize: '11px', fontWeight: 800, marginTop: '12px' }}>
            {(data?.teams ?? []).reduce((sum, t) => sum + t.members, 0)} MEMBERS
          </div>
        </div>
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(min(100%, 320px), 1fr))', gap: '24px' }}>
        <div className="card" style={{ display: 'flex', flexDirection: 'column' }}>
          <span className="font-mono" style={{ ...tag, alignSelf: 'flex-start' }}>Recruitment Pipeline</span>
          {apps > 0 ? (
            <div style={{ marginTop: '24px', display: 'flex', flexDirection: 'column', gap: '24px' }}>
              <div style={{ display: 'flex', height: '48px', width: '100%', border: '2px solid #0E1013', boxShadow: '4px 4px 0px #0E1013' }}>
                {pipeline.accepted > 0 && <div style={{ width: `${(pipeline.accepted / totalApps) * 100}%`, background: '#4ADE80', borderRight: '2px solid #0E1013' }} title="Accepted" />}
                {pipeline.scheduled > 0 && <div style={{ width: `${(pipeline.scheduled / totalApps) * 100}%`, background: '#60A5FA', borderRight: '2px solid #0E1013' }} title="Interviews" />}
                {pipeline.pending > 0 && <div style={{ width: `${(pipeline.pending / totalApps) * 100}%`, background: '#FFFFFF', borderRight: '2px solid #0E1013' }} title="Pending" />}
                {pipeline.referred > 0 && <div style={{ width: `${(pipeline.referred / totalApps) * 100}%`, background: '#FFC629', borderRight: '2px solid #0E1013' }} title="Referred" />}
                {pipeline.rejected > 0 && <div style={{ width: `${(pipeline.rejected / totalApps) * 100}%`, background: '#F87171' }} title="Rejected" />}
              </div>
              <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '12px', marginTop: '4px' }}>
                {[
                  ['#4ADE80', `ACCEPTED (${pipeline.accepted})`],
                  ['#60A5FA', `INTERVIEWS (${pipeline.scheduled})`],
                  ['#FFFFFF', `PENDING (${pipeline.pending})`],
                  ['#F87171', `REJECTED (${pipeline.rejected})`],
                  ['#FFC629', `REFERRED (${pipeline.referred})`],
                ].map(([color, label]) => (
                  <div key={label} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                    <div style={{ width: '14px', height: '14px', background: color, border: '2px solid #0E1013' }} />
                    <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>{label}</span>
                  </div>
                ))}
              </div>
            </div>
          ) : (
            <div style={{ marginTop: '32px', opacity: 0.5, fontWeight: 700, textTransform: 'uppercase' }}>{loading ? 'Loading…' : 'No pipeline data yet.'}</div>
          )}
        </div>

        <div className="card" style={{ display: 'flex', flexDirection: 'column' }}>
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px', flexWrap: 'wrap', gap: '8px' }}>
            <span className="font-mono" style={tag}>Trending Teams (Ranked)</span>
            <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, opacity: 0.7 }}>MOST → LEAST</span>
          </div>
          {rankedTeams.length > 0 ? (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '10px', marginTop: '8px', maxHeight: '340px', overflowY: 'auto', paddingRight: '4px' }}>
              {rankedTeams.map((t, idx) => {
                const isFirst = idx === 0 && t.count > 0;
                return (
                  <div key={t.name} style={{ display: 'flex', flexDirection: 'column', gap: '4px', padding: '10px 12px', background: isFirst ? '#FFC629' : '#FFFFFF', border: '2px solid #0E1013', boxShadow: '3px 3px 0px #0E1013' }}>
                    <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: '8px' }}>
                      <div style={{ display: 'flex', alignItems: 'center', gap: '8px', minWidth: 0 }}>
                        <span className="font-mono" style={{ fontSize: '11px', fontWeight: 900, background: isFirst ? '#0E1013' : '#F7F5F0', color: isFirst ? '#FFC629' : '#0E1013', padding: '2px 6px', border: '1.5px solid #0E1013', flexShrink: 0 }}>#{idx + 1}</span>
                        <strong style={{ fontSize: '13px', textTransform: 'uppercase', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} dir="auto">{t.name}</strong>
                      </div>
                      <span className="font-mono" style={{ fontSize: '12px', fontWeight: 800, flexShrink: 0 }}>{t.count} {t.count === 1 ? 'app' : 'apps'} ({t.pct.toFixed(0)}%)</span>
                    </div>
                    <div style={{ height: '6px', width: '100%', background: 'rgba(14,16,19,0.12)', border: '1px solid #0E1013', marginTop: '2px' }}>
                      <div style={{ height: '100%', width: `${Math.max(t.pct, t.count > 0 ? 5 : 0)}%`, background: isFirst ? '#0E1013' : '#FFC629' }} />
                    </div>
                  </div>
                );
              })}
            </div>
          ) : (
            <div style={{ marginTop: '32px', opacity: 0.5, fontWeight: 700, textTransform: 'uppercase' }}>No team application data yet.</div>
          )}
        </div>
      </div>

      <div className="card">
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px', flexWrap: 'wrap', gap: '8px' }}>
          <div>
            <span className="font-mono" style={tag}>Daily Visits (24H Breakdown)</span>
            <p className="font-mono" style={{ fontSize: '12px', opacity: 0.6, margin: '8px 0 0 0' }}>Inspect website visits recorded on each calendar day.</p>
          </div>
          <div style={{ background: '#FFC629', border: '2px solid #0E1013', padding: '4px 10px', fontWeight: 800, fontSize: '12px', fontFamily: 'var(--font-mono)' }}>TODAY: {todayVisits} VISITS</div>
        </div>
        {dailyEntries.length > 0 ? (
          <div style={{ display: 'flex', flexDirection: 'column', gap: '8px', marginTop: '12px' }}>
            {dailyEntries.map(({ date, count }) => {
              const isToday = date === todayStr;
              return (
                <div key={date} style={{ display: 'flex', alignItems: 'center', gap: '12px', padding: '8px 12px', background: isToday ? '#FFF9E6' : '#FFFFFF', border: '2px solid #0E1013', boxShadow: '2px 2px 0px #0E1013' }}>
                  <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, width: '110px', flexShrink: 0 }}>{date} {isToday ? '(Today)' : ''}</span>
                  <div style={{ flex: 1, height: '14px', background: 'rgba(14,16,19,0.08)', border: '1.5px solid #0E1013', overflow: 'hidden' }}>
                    <div style={{ height: '100%', width: `${Math.max((count / maxDailyVisit) * 100, 4)}%`, background: isToday ? '#FFC629' : '#4ADE80' }} />
                  </div>
                  <span className="font-mono" style={{ fontSize: '12px', fontWeight: 900, minWidth: '70px', textAlign: 'right', flexShrink: 0 }}>{count.toLocaleString()} visits</span>
                </div>
              );
            })}
          </div>
        ) : (
          <div style={{ padding: '24px 0', textAlign: 'center', opacity: 0.6, fontWeight: 700, textTransform: 'uppercase', fontFamily: 'var(--font-mono)' }}>
            No daily visit records logged yet. Visits are counted in hourly batches.
          </div>
        )}
      </div>

      {canLogs && (
        <div className="card">
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px' }}>
            <span className="font-mono" style={tag}>System Logs ({logs.length})</span>
            <button
              onClick={refreshAll}
              disabled={loadingLogs}
              style={{ display: 'flex', alignItems: 'center', gap: '6px', padding: '6px 12px', background: '#FFF', border: '2px solid #0E1013', boxShadow: '2px 2px 0px #0E1013', fontWeight: 800, fontSize: '12px', cursor: 'pointer', fontFamily: 'inherit' }}
            >
              <RefreshCw size={14} /> REFRESH
            </button>
          </div>
          <ErrorBanner message={logError} />
          {logs.length > 0 ? (
            <div style={{ overflowX: 'auto' }}>
              <table className="table" style={{ width: '100%', borderCollapse: 'collapse' }}>
                <thead>
                  <tr style={{ borderBottom: '2px solid #0E1013', textAlign: 'left' }}>
                    <th style={{ width: '40px', padding: '8px' }} />
                    <th style={{ width: '120px', padding: '8px', fontFamily: 'var(--font-mono)', fontSize: '12px' }}>TIME</th>
                    <th style={{ width: '120px', padding: '8px', fontFamily: 'var(--font-mono)', fontSize: '12px' }}>CATEGORY</th>
                    <th style={{ padding: '8px', fontFamily: 'var(--font-mono)', fontSize: '12px' }}>EVENT DETAILS</th>
                  </tr>
                </thead>
                <tbody>
                  {logs.map((log) => {
                    const known = kActions[log.action] ?? { type: 'system', text: log.action || 'Event' };
                    const who = log.actor ? (log.actor.name || 'A staff member') : (log.action === 'ApplicationReceived' ? 'Website' : 'Anonymous');
                    const detail = `${who}: ${known.text}${log.succeeded ? '' : ` (refused: ${log.code})`}${log.repeats > 1 ? ` ×${log.repeats}` : ''}`;
                    return (
                      <tr key={log.id} style={{ borderBottom: '1px solid #E5E7EB' }}>
                        <td style={{ padding: '12px 8px' }}>{getLogIcon(known.type)}</td>
                        <td className="font-mono" style={{ fontSize: '12px', fontWeight: 700, color: 'var(--text-dark)', padding: '12px 8px', whiteSpace: 'nowrap' }} title={new Date(log.at).toLocaleString()}>{formatRelativeTime(log.at)}</td>
                        <td style={{ padding: '12px 8px' }}>
                          <span style={{ display: 'inline-block', padding: '2px 8px', fontSize: '11px', fontWeight: 800, textTransform: 'uppercase', borderRadius: '2px', ...getTypeBadgeStyle(known.type) }}>{known.type}</span>
                        </td>
                        <td style={{ fontWeight: 700, fontSize: '14px', padding: '12px 8px' }}>{detail}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          ) : (
            <div style={{ padding: '32px 0', textAlign: 'center', opacity: 0.6, fontWeight: 700, textTransform: 'uppercase', fontFamily: 'var(--font-mono)' }}>
              <Clock size={28} style={{ margin: '0 auto 8px auto', display: 'block' }} />
              No system activity logs recorded yet.
            </div>
          )}
        </div>
      )}
    </div>
  );
}
