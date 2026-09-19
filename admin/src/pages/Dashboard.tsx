import { useState, useEffect } from 'react';
import { Activity, Users, Clock, CheckCircle, Shield, FileText, Layers, RefreshCw } from 'lucide-react';

interface LogItem {
  id: string;
  timestamp: number;
  action: string;
  details: string;
  type: string;
}

export default function Dashboard() {
  const [stats, setStats] = useState({ apps: 0, teams: 0, visits: 0 });
  const [mostApplied, setMostApplied] = useState('N/A');
  const [pipeline, setPipeline] = useState({ pending: 0, scheduled: 0, accepted: 0, rejected: 0, referred: 0 });
  const [logs, setLogs] = useState<LogItem[]>([]);
  const [loadingLogs, setLoadingLogs] = useState(false);

  const fetchDashboardData = () => {
    setLoadingLogs(true);
    Promise.all([
      fetch('/api/applications_list').then(r => r.json()),
      fetch('/api/teams').then(r => r.json()),
      fetch('/api/analytics').then(r => r.json()),
      fetch('/api/logs').then(r => r.json()).catch(() => ({ logs: [] }))
    ]).then(([appsData, teamsData, analyticsData, logsData]) => {
      const apps = appsData.applications || [];
      const teams = teamsData.teams || [];
      const visits = analyticsData.visits || 0;
      setStats({ apps: apps.length, teams: teams.length, visits });
      setLogs(logsData.logs || []);

      if (apps.length > 0) {
        const counts: Record<string, number> = {};
        let maxCount = 0;
        let maxTeam = 'N/A';
        const p = { pending: 0, scheduled: 0, accepted: 0, rejected: 0, referred: 0 };

        apps.forEach((a: any) => {
          counts[a.team] = (counts[a.team] || 0) + 1;
          if (counts[a.team] > maxCount) {
            maxCount = counts[a.team];
            maxTeam = a.team;
          }

          if (a.status === 'pending') p.pending++;
          else if (a.status === 'interview_scheduled') p.scheduled++;
          else if (a.status === 'accepted') p.accepted++;
          else if (a.status === 'rejected') p.rejected++;
          else if (a.status === 'referred') p.referred++;
        });
        setMostApplied(maxTeam);
        setPipeline(p);
      }
    }).catch(err => console.error("Failed to fetch dashboard data:", err))
      .finally(() => setLoadingLogs(false));
  };

  useEffect(() => {
    fetchDashboardData();
  }, []);

  const totalApps = stats.apps || 1;
  const pendingPct = (pipeline.pending / totalApps) * 100;
  const acceptedPct = (pipeline.accepted / totalApps) * 100;
  const scheduledPct = (pipeline.scheduled / totalApps) * 100;
  const rejectedPct = (pipeline.rejected / totalApps) * 100;
  
  const conversionRate = stats.visits > 0 ? ((stats.apps / stats.visits) * 100).toFixed(1) : '0.0';

  const formatRelativeTime = (ts: number) => {
    if (!ts) return 'UNKNOWN';
    const timestampMs = ts > 1e11 ? ts : ts * 1000;
    const nowMs = Date.now();
    const diffSec = Math.max(0, Math.floor((nowMs - timestampMs) / 1000));
    if (diffSec < 60) return 'JUST NOW';
    if (diffSec < 3600) return `${Math.floor(diffSec / 60)}M AGO`;
    if (diffSec < 86400) return `${Math.floor(diffSec / 3600)}H AGO`;
    if (diffSec < 604800) return `${Math.floor(diffSec / 86400)}D AGO`;
    return new Date(timestampMs).toLocaleDateString(undefined, { month: 'short', day: 'numeric' }).toUpperCase();
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

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '40px', paddingBottom: '40px' }}>
      <div>
        <span className="heading-sm">01 — Overview</span>
        <h1 className="heading-lg">Platform Analytics.</h1>
      </div>
      
      {/* TOP ROW: KEY METRICS */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(240px, 1fr))', gap: '32px' }}>
        <div className="card">
          <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px' }}><Activity size={16} /> Total Visits</span>
          <p style={{ fontSize: '56px', fontWeight: '900', margin: '16px 0 0 0', letterSpacing: '-0.04em' }}>{stats.visits.toLocaleString()}</p>
          <div style={{ background: '#4ADE80', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 6px', fontSize: '12px', fontWeight: 800, marginTop: '8px' }}>REAL-TIME TRACKING</div>
        </div>
        <div className="card">
          <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px' }}><Users size={16} /> Applications</span>
          <p style={{ fontSize: '56px', fontWeight: '900', margin: '16px 0 0 0', letterSpacing: '-0.04em' }}>{stats.apps}</p>
          <div style={{ background: 'var(--brand-yellow)', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 6px', fontSize: '12px', fontWeight: 800, marginTop: '8px' }}>CONVERSION: {conversionRate}%</div>
        </div>
        <div className="card">
          <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px' }}><CheckCircle size={16} /> Active Teams</span>
          <p style={{ fontSize: '56px', fontWeight: '900', margin: '16px 0 0 0', letterSpacing: '-0.04em' }}>{stats.teams}</p>
          <div style={{ background: '#F7F5F0', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 6px', fontSize: '12px', fontWeight: 800, marginTop: '8px' }}>CAPACITY: OK</div>
        </div>
      </div>
      
      {/* MIDDLE ROW: PIPELINE AND TRENDING */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(400px, 1fr))', gap: '32px' }}>
        
        {/* RECRUITMENT PIPELINE */}
        <div className="card" style={{ display: 'flex', flexDirection: 'column' }}>
          <span className="font-mono" style={{ fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', background: '#0E1013', color: '#FFF', padding: '4px 8px', alignSelf: 'flex-start' }}>Recruitment Pipeline</span>
          
          {stats.apps > 0 ? (
            <div style={{ marginTop: '32px', display: 'flex', flexDirection: 'column', gap: '24px' }}>
              {/* Stacked Bar Chart */}
              <div style={{ display: 'flex', height: '48px', width: '100%', border: '2px solid #0E1013', boxShadow: '4px 4px 0px #0E1013' }}>
                {pipeline.accepted > 0 && <div style={{ width: `${acceptedPct}%`, background: '#4ADE80', borderRight: '2px solid #0E1013' }} title="Accepted"></div>}
                {pipeline.scheduled > 0 && <div style={{ width: `${scheduledPct}%`, background: '#60A5FA', borderRight: '2px solid #0E1013' }} title="Interviews"></div>}
                {pipeline.pending > 0 && <div style={{ width: `${pendingPct}%`, background: '#FFFFFF', borderRight: '2px solid #0E1013' }} title="Pending"></div>}
                {pipeline.rejected > 0 && <div style={{ width: `${rejectedPct}%`, background: '#F87171' }} title="Rejected"></div>}
              </div>

              {/* Legend */}
              <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '16px', marginTop: '8px' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}><div style={{ width: '16px', height: '16px', background: '#4ADE80', border: '2px solid #0E1013' }}></div><span className="font-mono" style={{ fontSize: '12px', fontWeight: 700 }}>ACCEPTED ({pipeline.accepted})</span></div>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}><div style={{ width: '16px', height: '16px', background: '#60A5FA', border: '2px solid #0E1013' }}></div><span className="font-mono" style={{ fontSize: '12px', fontWeight: 700 }}>INTERVIEWS ({pipeline.scheduled})</span></div>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}><div style={{ width: '16px', height: '16px', background: '#FFFFFF', border: '2px solid #0E1013' }}></div><span className="font-mono" style={{ fontSize: '12px', fontWeight: 700 }}>PENDING ({pipeline.pending})</span></div>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}><div style={{ width: '16px', height: '16px', background: '#F87171', border: '2px solid #0E1013' }}></div><span className="font-mono" style={{ fontSize: '12px', fontWeight: 700 }}>REJECTED ({pipeline.rejected})</span></div>
              </div>
            </div>
          ) : (
            <div style={{ marginTop: '32px', opacity: 0.5, fontWeight: 700, textTransform: 'uppercase' }}>No pipeline data yet.</div>
          )}
        </div>

        {/* TRENDING NOW */}
        <div className="card" style={{ background: 'var(--brand-yellow)', display: 'flex', flexDirection: 'column', justifyContent: 'center' }}>
          <span className="font-mono" style={{ fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', background: '#0E1013', color: '#FFF', padding: '4px 8px', alignSelf: 'flex-start' }}>Trending Team</span>
          <div style={{ display: 'flex', flexDirection: 'column', gap: '8px', marginTop: '24px' }}>
            <p style={{ fontSize: 'clamp(32px, 4vw, 56px)', fontWeight: '900', margin: 0, letterSpacing: '-0.04em', textTransform: 'uppercase', lineHeight: 1.1 }}>{mostApplied}</p>
            <span style={{ fontSize: '18px', fontWeight: '900', textTransform: 'uppercase', borderTop: '4px solid #0E1013', paddingTop: '16px', display: 'inline-block' }}>Receiving highest volume of applicants</span>
          </div>
        </div>

      </div>

      {/* BOTTOM ROW: RECENT ACTIVITY / SYSTEM LOGS */}
      <div className="card">
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px' }}>
          <span className="font-mono" style={{ fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', background: '#0E1013', color: '#FFF', padding: '4px 8px' }}>System Logs ({logs.length})</span>
          <button 
            onClick={fetchDashboardData} 
            disabled={loadingLogs}
            style={{ display: 'flex', alignItems: 'center', gap: '6px', padding: '6px 12px', background: '#FFF', border: '2px solid #0E1013', boxShadow: '2px 2px 0px #0E1013', fontWeight: 800, fontSize: '12px', cursor: 'pointer', fontFamily: 'inherit' }}
          >
            <RefreshCw size={14} className={loadingLogs ? 'animate-spin' : ''} /> REFRESH
          </button>
        </div>
        
        {logs.length > 0 ? (
          <div style={{ overflowX: 'auto' }}>
            <table className="table" style={{ width: '100%', borderCollapse: 'collapse' }}>
              <thead>
                <tr style={{ borderBottom: '2px solid #0E1013', textAlign: 'left' }}>
                  <th style={{ width: '40px', padding: '8px' }}></th>
                  <th style={{ width: '120px', padding: '8px', fontFamily: 'var(--font-mono)', fontSize: '12px' }}>TIME</th>
                  <th style={{ width: '120px', padding: '8px', fontFamily: 'var(--font-mono)', fontSize: '12px' }}>CATEGORY</th>
                  <th style={{ padding: '8px', fontFamily: 'var(--font-mono)', fontSize: '12px' }}>EVENT DETAILS</th>
                </tr>
              </thead>
              <tbody>
                {logs.map((log) => (
                  <tr key={log.id} style={{ borderBottom: '1px solid #E5E7EB' }}>
                    <td style={{ padding: '12px 8px' }}>{getLogIcon(log.type)}</td>
                    <td className="font-mono" style={{ fontSize: '12px', fontWeight: 700, color: 'var(--text-dark)', padding: '12px 8px', whiteSpace: 'nowrap' }}>
                      {formatRelativeTime(log.timestamp)}
                    </td>
                    <td style={{ padding: '12px 8px' }}>
                      <span style={{ 
                        display: 'inline-block', 
                        padding: '2px 8px', 
                        fontSize: '11px', 
                        fontWeight: 800, 
                        textTransform: 'uppercase',
                        borderRadius: '2px',
                        ...getTypeBadgeStyle(log.type)
                      }}>
                        {log.type}
                      </span>
                    </td>
                    <td style={{ fontWeight: 700, fontSize: '14px', padding: '12px 8px' }}>
                      {log.details || log.action}
                    </td>
                  </tr>
                ))}
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

    </div>
  );
}

