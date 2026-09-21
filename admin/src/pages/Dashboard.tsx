import { useState, useEffect } from 'react';
import { Activity, Users, Clock, CheckCircle, Shield, FileText, Layers, RefreshCw } from 'lucide-react';

interface LogItem {
  id: string;
  timestamp: number;
  action: string;
  details: string;
  type: string;
}

interface RankedTeam {
  name: string;
  count: number;
  pct: number;
}

export default function Dashboard() {
  const [stats, setStats] = useState({ apps: 0, teams: 0, visits: 0, todayVisits: 0 });
  const [rankedTeams, setRankedTeams] = useState<RankedTeam[]>([]);
  const [dailyVisits, setDailyVisits] = useState<Record<string, number>>({});
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
      const todayVisits = analyticsData.today_visits || 0;
      const daily = analyticsData.daily || {};

      setStats({ apps: apps.length, teams: teams.length, visits, todayVisits });
      setDailyVisits(daily);
      setLogs(logsData.logs || []);

      // Calculate pipeline and team applicant counts
      const counts: Record<string, number> = {};
      const p = { pending: 0, scheduled: 0, accepted: 0, rejected: 0, referred: 0 };

      // Initialize all active teams with 0
      teams.forEach((t: any) => {
        if (t && t.name) counts[t.name.trim()] = 0;
      });

      apps.forEach((a: any) => {
        const tName = (a.team || 'Unknown').trim();
        counts[tName] = (counts[tName] || 0) + 1;

        if (a.status === 'pending') p.pending++;
        else if (a.status === 'interview_scheduled') p.scheduled++;
        else if (a.status === 'accepted') p.accepted++;
        else if (a.status === 'rejected') p.rejected++;
        else if (a.status === 'referred') p.referred++;
      });

      setPipeline(p);

      const totalAppCount = apps.length || 1;
      const ranked: RankedTeam[] = Object.keys(counts).map(teamName => ({
        name: teamName,
        count: counts[teamName],
        pct: apps.length > 0 ? (counts[teamName] / totalAppCount) * 100 : 0
      })).sort((a, b) => b.count - a.count || a.name.localeCompare(b.name));

      setRankedTeams(ranked);
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

  // Prepare daily visits list (sorted by date descending)
  const dailyEntries = Object.entries(dailyVisits)
    .sort(([dateA], [dateB]) => dateB.localeCompare(dateA))
    .slice(0, 10);
  const maxDailyVisit = Math.max(...dailyEntries.map(([, cnt]) => cnt), 1);
  const todayDateStr = new Date().toISOString().slice(0, 10);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '40px', paddingBottom: '40px' }}>
      <div>
        <span className="heading-sm">01 — Overview</span>
        <h1 className="heading-lg">Platform Analytics.</h1>
      </div>
      
      {/* TOP ROW: KEY METRICS */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(min(100%, 220px), 1fr))', gap: '24px' }}>
        <div className="card">
          <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Activity size={16} /> Total Visits
          </span>
          <p style={{ fontSize: '52px', fontWeight: '900', margin: '16px 0 0 0', letterSpacing: '-0.04em' }}>
            {stats.visits.toLocaleString()}
          </p>
          <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center', marginTop: '12px' }}>
            <div style={{ background: '#4ADE80', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 8px', fontSize: '11px', fontWeight: 800 }}>
              REAL-TIME
            </div>
            <div style={{ background: 'var(--brand-yellow)', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 8px', fontSize: '11px', fontWeight: 800 }}>
              {stats.todayVisits.toLocaleString()} TODAY (24H)
            </div>
          </div>
        </div>

        <div className="card">
          <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Users size={16} /> Applications
          </span>
          <p style={{ fontSize: '52px', fontWeight: '900', margin: '16px 0 0 0', letterSpacing: '-0.04em' }}>
            {stats.apps}
          </p>
          <div style={{ background: 'var(--brand-yellow)', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 8px', fontSize: '11px', fontWeight: 800, marginTop: '12px' }}>
            CONVERSION: {conversionRate}%
          </div>
        </div>

        <div className="card">
          <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px' }}>
            <CheckCircle size={16} /> Active Teams
          </span>
          <p style={{ fontSize: '52px', fontWeight: '900', margin: '16px 0 0 0', letterSpacing: '-0.04em' }}>
            {stats.teams}
          </p>
          <div style={{ background: '#F7F5F0', border: '2px solid #0E1013', display: 'inline-block', padding: '2px 8px', fontSize: '11px', fontWeight: 800, marginTop: '12px' }}>
            CAPACITY: OK
          </div>
        </div>
      </div>
      
      {/* MIDDLE ROW: PIPELINE AND RANKED TRENDING TEAMS */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(min(100%, 320px), 1fr))', gap: '24px' }}>
        
        {/* RECRUITMENT PIPELINE */}
        <div className="card" style={{ display: 'flex', flexDirection: 'column' }}>
          <span className="font-mono" style={{ fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', background: '#0E1013', color: '#FFF', padding: '4px 8px', alignSelf: 'flex-start' }}>
            Recruitment Pipeline
          </span>
          
          {stats.apps > 0 ? (
            <div style={{ marginTop: '24px', display: 'flex', flexDirection: 'column', gap: '24px' }}>
              {/* Stacked Bar Chart */}
              <div style={{ display: 'flex', height: '48px', width: '100%', border: '2px solid #0E1013', boxShadow: '4px 4px 0px #0E1013' }}>
                {pipeline.accepted > 0 && <div style={{ width: `${acceptedPct}%`, background: '#4ADE80', borderRight: '2px solid #0E1013' }} title="Accepted"></div>}
                {pipeline.scheduled > 0 && <div style={{ width: `${scheduledPct}%`, background: '#60A5FA', borderRight: '2px solid #0E1013' }} title="Interviews"></div>}
                {pipeline.pending > 0 && <div style={{ width: `${pendingPct}%`, background: '#FFFFFF', borderRight: '2px solid #0E1013' }} title="Pending"></div>}
                {pipeline.rejected > 0 && <div style={{ width: `${rejectedPct}%`, background: '#F87171' }} title="Rejected"></div>}
              </div>

              {/* Legend */}
              <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '12px', marginTop: '4px' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                  <div style={{ width: '14px', height: '14px', background: '#4ADE80', border: '2px solid #0E1013' }}></div>
                  <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>ACCEPTED ({pipeline.accepted})</span>
                </div>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                  <div style={{ width: '14px', height: '14px', background: '#60A5FA', border: '2px solid #0E1013' }}></div>
                  <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>INTERVIEWS ({pipeline.scheduled})</span>
                </div>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                  <div style={{ width: '14px', height: '14px', background: '#FFFFFF', border: '2px solid #0E1013' }}></div>
                  <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>PENDING ({pipeline.pending})</span>
                </div>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                  <div style={{ width: '14px', height: '14px', background: '#F87171', border: '2px solid #0E1013' }}></div>
                  <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>REJECTED ({pipeline.rejected})</span>
                </div>
              </div>
            </div>
          ) : (
            <div style={{ marginTop: '32px', opacity: 0.5, fontWeight: 700, textTransform: 'uppercase' }}>
              No pipeline data yet.
            </div>
          )}
        </div>

        {/* TRENDING TEAMS: MOST TO LEAST TRENDING */}
        <div className="card" style={{ display: 'flex', flexDirection: 'column' }}>
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px', flexWrap: 'wrap', gap: '8px' }}>
            <span className="font-mono" style={{ fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', background: '#0E1013', color: '#FFF', padding: '4px 8px' }}>
              Trending Teams (Ranked)
            </span>
            <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, opacity: 0.7 }}>
              MOST → LEAST
            </span>
          </div>

          {rankedTeams.length > 0 ? (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '10px', marginTop: '8px', maxHeight: '340px', overflowY: 'auto', paddingRight: '4px' }}>
              {rankedTeams.map((t, idx) => {
                const isFirst = idx === 0 && t.count > 0;
                return (
                  <div 
                    key={t.name}
                    style={{ 
                      display: 'flex', 
                      flexDirection: 'column', 
                      gap: '4px', 
                      padding: '10px 12px', 
                      background: isFirst ? '#FFC629' : '#FFFFFF', 
                      border: '2px solid #0E1013', 
                      boxShadow: '3px 3px 0px #0E1013' 
                    }}
                  >
                    <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: '8px' }}>
                      <div style={{ display: 'flex', alignItems: 'center', gap: '8px', minWidth: 0 }}>
                        <span 
                          className="font-mono" 
                          style={{ 
                            fontSize: '11px', 
                            fontWeight: 900, 
                            background: isFirst ? '#0E1013' : '#F7F5F0', 
                            color: isFirst ? '#FFC629' : '#0E1013', 
                            padding: '2px 6px',
                            border: '1.5px solid #0E1013',
                            flexShrink: 0
                          }}
                        >
                          #{idx + 1}
                        </span>
                        <strong style={{ fontSize: '13px', textTransform: 'uppercase', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                          {t.name}
                        </strong>
                      </div>
                      <span className="font-mono" style={{ fontSize: '12px', fontWeight: 800, flexShrink: 0 }}>
                        {t.count} {t.count === 1 ? 'app' : 'apps'} ({t.pct.toFixed(0)}%)
                      </span>
                    </div>

                    {/* Popularity Bar */}
                    <div style={{ height: '6px', width: '100%', background: 'rgba(14,16,19,0.12)', border: '1px solid #0E1013', marginTop: '2px' }}>
                      <div style={{ height: '100%', width: `${Math.max(t.pct, t.count > 0 ? 5 : 0)}%`, background: isFirst ? '#0E1013' : '#FFC629' }}></div>
                    </div>
                  </div>
                );
              })}
            </div>
          ) : (
            <div style={{ marginTop: '32px', opacity: 0.5, fontWeight: 700, textTransform: 'uppercase' }}>
              No team application data yet.
            </div>
          )}
        </div>

      </div>

      {/* DAILY VISITS BREAKDOWN */}
      <div className="card">
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px', flexWrap: 'wrap', gap: '8px' }}>
          <div>
            <span className="font-mono" style={{ fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', background: '#0E1013', color: '#FFF', padding: '4px 8px' }}>
              Daily Visits (24H Breakdown)
            </span>
            <p className="font-mono" style={{ fontSize: '12px', opacity: 0.6, margin: '8px 0 0 0' }}>
              Inspect website visits recorded on each calendar day.
            </p>
          </div>
          <div style={{ background: '#FFC629', border: '2px solid #0E1013', padding: '4px 10px', fontWeight: 800, fontSize: '12px', fontFamily: 'var(--font-mono)' }}>
            TODAY: {stats.todayVisits} VISITS
          </div>
        </div>

        {dailyEntries.length > 0 ? (
          <div style={{ display: 'flex', flexDirection: 'column', gap: '8px', marginTop: '12px' }}>
            {dailyEntries.map(([dateStr, count]) => {
              const isToday = dateStr === todayDateStr;
              const barWidth = Math.max((count / maxDailyVisit) * 100, 4);
              return (
                <div 
                  key={dateStr}
                  style={{
                    display: 'flex',
                    alignItems: 'center',
                    gap: '12px',
                    padding: '8px 12px',
                    background: isToday ? '#FFF9E6' : '#FFFFFF',
                    border: '2px solid #0E1013',
                    boxShadow: '2px 2px 0px #0E1013'
                  }}
                >
                  <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, width: '110px', flexShrink: 0 }}>
                    {dateStr} {isToday ? '(Today)' : ''}
                  </span>
                  
                  <div style={{ flex: 1, height: '14px', background: 'rgba(14,16,19,0.08)', border: '1.5px solid #0E1013', overflow: 'hidden' }}>
                    <div style={{ height: '100%', width: `${barWidth}%`, background: isToday ? '#FFC629' : '#4ADE80' }}></div>
                  </div>

                  <span className="font-mono" style={{ fontSize: '12px', fontWeight: 900, minWidth: '70px', textAlign: 'right', flexShrink: 0 }}>
                    {count.toLocaleString()} visits
                  </span>
                </div>
              );
            })}
          </div>
        ) : (
          <div style={{ padding: '24px 0', textAlign: 'center', opacity: 0.6, fontWeight: 700, textTransform: 'uppercase', fontFamily: 'var(--font-mono)' }}>
            No daily visit records logged yet. Visits will accumulate as visitors browse the site.
          </div>
        )}
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

