import { Activity, Users, CheckCircle, RefreshCw } from 'lucide-react';
import { api } from '../app/platform';
import { ErrorBanner, useLoad } from '../app/ui';
import { routeDashboardGet } from '../api/hammer.generated';

const kPipeline = [
  { key: 'accepted', label: 'ACCEPTED', color: '#4ADE80' },
  { key: 'interview_scheduled', label: 'INTERVIEWS', color: '#FFC629' },
  { key: 'pending', label: 'PENDING', color: '#FFFFFF' },
  { key: 'referred', label: 'REFERRED', color: '#60A5FA' },
  { key: 'rejected', label: 'REJECTED', color: '#F87171' },
] as const;

const tag = { fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', background: '#0E1013', color: '#FFF', padding: '4px 8px', alignSelf: 'flex-start' } as const;
const statLabel = { fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px' } as const;

export default function Dashboard() {
  const { data, error, loading, reload } = useLoad((signal) => api.call(routeDashboardGet, { signal }), []);

  const days = data?.visits ?? [];
  const peak = Math.max(1, ...days.map((d) => d.count));
  const total = data?.applications_total ?? 0;
  const byTeam = Object.entries(data?.applications_by_team ?? {}).sort((a, b) => b[1] - a[1]);
  const conversion = data && data.visits_total > 0 ? ((total / data.visits_total) * 100).toFixed(1) : '0.0';

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'end', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">01 — Overview</span>
          <h1 className="heading-lg">Platform Analytics.</h1>
        </div>
        <button className="btn-outline" onClick={reload} disabled={loading} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
          <RefreshCw size={16} /> Refresh
        </button>
      </div>

      <ErrorBanner message={error} />

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(220px, 1fr))', gap: '24px' }}>
        <div className="card">
          <span className="font-mono" style={statLabel}><Activity size={16} /> Visits (30 days)</span>
          <div style={{ fontSize: '48px', fontWeight: 900, marginTop: '12px' }}>{(data?.visits_total ?? 0).toLocaleString()}</div>
          <span className="font-mono" style={{ fontSize: '11px', opacity: 0.6 }}>{conversion}% applied</span>
        </div>
        <div className="card">
          <span className="font-mono" style={statLabel}><CheckCircle size={16} /> Applications</span>
          <div style={{ fontSize: '48px', fontWeight: 900, marginTop: '12px' }}>{total}</div>
        </div>
        <div className="card">
          <span className="font-mono" style={statLabel}><Users size={16} /> Teams</span>
          <div style={{ fontSize: '48px', fontWeight: 900, marginTop: '12px' }}>{data?.teams.length ?? 0}</div>
          <span className="font-mono" style={{ fontSize: '11px', opacity: 0.6 }}>
            {(data?.teams ?? []).reduce((sum, t) => sum + t.members, 0)} members on rosters
          </span>
        </div>
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(320px, 1fr))', gap: '24px' }}>
        <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
          <span className="font-mono" style={tag}>Recruitment Pipeline</span>
          {total === 0 ? <p style={{ opacity: 0.6 }}>No applications yet.</p> : kPipeline.map((step) => {
            const count = data?.applications_by_status[step.key] ?? 0;
            return (
              <div key={step.key}>
                <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>{step.label} ({count})</span>
                <div style={{ height: '14px', border: '2px solid #0E1013', marginTop: '4px', background: '#F7F5F0' }}>
                  <div style={{ width: `${(count / total) * 100}%`, height: '100%', background: step.color }} />
                </div>
              </div>
            );
          })}
        </div>

        <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '12px' }}>
          <span className="font-mono" style={tag}>Applicants by Team</span>
          {byTeam.length === 0 ? <p style={{ opacity: 0.6 }}>No applications yet.</p> : byTeam.map(([team, count]) => (
            <div key={team} style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
              <span style={{ flex: '0 0 40%', fontWeight: 700, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} dir="auto">{team}</span>
              <div style={{ flex: 1, height: '12px', border: '2px solid #0E1013', background: '#F7F5F0' }}>
                <div style={{ width: `${(count / total) * 100}%`, height: '100%', background: '#FFC629' }} />
              </div>
              <span className="font-mono" style={{ fontSize: '12px', fontWeight: 800 }}>{count}</span>
            </div>
          ))}
        </div>
      </div>

      <div className="card">
        <span className="font-mono" style={tag}>Daily Visits</span>
        {days.length === 0 ? (
          <p style={{ opacity: 0.6 }}>No visits recorded yet. Counts appear after the hourly rollup.</p>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', gap: '6px', marginTop: '16px' }}>
            {days.slice(-14).reverse().map((d) => (
              <div key={d.at} style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
                <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, width: '110px', flexShrink: 0 }}>
                  {new Date(d.at).toLocaleDateString(undefined, { month: 'short', day: 'numeric' })}
                </span>
                <div style={{ flex: 1, height: '12px', background: '#F7F5F0', border: '2px solid #0E1013' }}>
                  <div style={{ width: `${(d.count / peak) * 100}%`, height: '100%', background: '#0E1013' }} />
                </div>
                <span className="font-mono" style={{ fontSize: '12px', fontWeight: 900, minWidth: '70px', textAlign: 'right' }}>{d.count}</span>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  );
}
