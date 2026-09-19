import { useState, useEffect } from 'react';
import { Trash2 } from 'lucide-react';

export default function Applications({ role }: { role: string }) {
  const [apps, setApps] = useState<any[]>([]);
  const [teams, setTeams] = useState<any[]>([]);
  const [selectedApp, setSelectedApp] = useState<any>(null);
  const [reason, setReason] = useState('');
  const [referTeam, setReferTeam] = useState('');
  const [filter, setFilter] = useState('all');

  const userTeam = localStorage.getItem('admin_team') || '';
  const adminEmail = localStorage.getItem('admin_email') || '';
  const isSuperAdmin = role === 'superadmin' || adminEmail === 'admin@enactussams.org';
  const isTeamScoped = !isSuperAdmin && (role === 'manager' || role === 'vice manager') && !!userTeam;

  const fetchApps = () => {
    fetch('/api/applications_list')
      .then(res => res.json())
      .then(data => {
        if (data && data.applications) {
          setApps(data.applications.map((a: any) => ({ ...a, id: a._id?.$oid || a.id })));
        }
      })
      .catch(err => console.error("Failed to fetch applications:", err));
  };

  useEffect(() => {
    fetchApps();

    fetch('/api/teams')
      .then(res => res.json())
      .then(data => {
        if (data && data.teams) {
          setTeams(data.teams.map((t: any) => ({ ...t, id: t._id?.$oid || t.id })));
        }
      })
      .catch(err => console.error("Failed to fetch teams:", err));
  }, []);

  const saveApps = (newApps: any[], id: string, status: string, decisionReason: string, referredTo?: string, targetTeam?: string) => {
    setApps(newApps);
    setSelectedApp(null);
    setReason('');
    setReferTeam('');

    fetch('/api/applications_update', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ 
        id, 
        status, 
        reason: decisionReason, 
        referredTo: referredTo || '',
        team: targetTeam || ''
      })
    })
    .then(() => fetchApps())
    .catch(err => console.error('Failed to update status:', err));
  };

  const handleAction = (id: string, status: string, targetTeam?: string) => {
    const newApps = apps.map(a => {
      if (a.id === id) {
        return { 
          ...a, 
          status, 
          reason, 
          team: targetTeam || a.team,
          referredTo: status === 'referred' ? referTeam : a.referredTo 
        };
      }
      return a;
    });
    saveApps(newApps, id, status, reason, status === 'referred' ? referTeam : undefined, targetTeam);
  };

  const handleDelete = (id: string) => {
    if (!confirm('Are you sure you want to permanently delete this application?')) return;
    fetch('/api/applications_update', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'delete', id })
    })
    .then(() => {
      setSelectedApp(null);
      fetchApps();
    })
    .catch(err => console.error('Failed to delete application:', err));
  };

  // Scoped view for managers
  const visibleApps = apps.filter(a => {
    if (!isTeamScoped) return true;
    const matchDirect = a.team && a.team.trim().toLowerCase() === userTeam.trim().toLowerCase();
    const matchReferred = a.referredTo && a.referredTo.trim().toLowerCase() === userTeam.trim().toLowerCase();
    return matchDirect || matchReferred;
  });

  const filteredApps = visibleApps.filter(a => {
    if (filter === 'all') return true;
    return a.status === filter;
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

        {/* Status Filters */}
        <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap' }}>
          {['all', 'pending', 'accepted', 'rejected', 'referred'].map(f => (
            <button
              key={f}
              onClick={() => setFilter(f)}
              style={{
                padding: '8px 16px',
                border: '2px solid #0E1013',
                background: filter === f ? '#FFC629' : '#FFF',
                fontFamily: 'IBM Plex Mono, monospace',
                fontSize: '12px',
                fontWeight: 700,
                textTransform: 'uppercase',
                boxShadow: filter === f ? '3px 3px 0px #0E1013' : 'none',
                cursor: 'pointer'
              }}
            >
              {f} ({visibleApps.filter(a => f === 'all' || a.status === f).length})
            </button>
          ))}
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
            {filteredApps.map(a => {
              const isReferredToMe = isTeamScoped && a.referredTo && a.referredTo.trim().toLowerCase() === userTeam.trim().toLowerCase();
              return (
                <tr key={a.id}>
                  <td>
                    <strong style={{ fontSize: '16px', textTransform: 'uppercase', letterSpacing: '0.02em', display: 'block' }}>{a.name}</strong>
                    {a.email && <span className="font-mono" style={{ fontSize: '11px', opacity: 0.6 }}>{a.email}</span>}
                  </td>
                  <td>
                    {a.team}
                    {a.referredTo && (
                      <span className="font-mono" style={{ display: 'block', fontSize: '10px', color: '#B45309', fontWeight: 700 }}>
                        → Referred to {a.referredTo}
                      </span>
                    )}
                  </td>
                  <td>
                    <span className={`badge badge-${a.status === 'interview_scheduled' ? 'accepted' : a.status}`}>
                      {isReferredToMe && a.status === 'referred' ? '★ Referred to You' : a.status}
                    </span>
                  </td>
                  <td style={{ color: 'var(--text-dark)', opacity: 0.7, maxWidth: '240px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                    {a.reason || '-'}
                  </td>
                  <td style={{ textAlign: 'right', whiteSpace: 'nowrap' }}>
                    <div style={{ display: 'inline-flex', gap: '8px', alignItems: 'center' }}>
                      <button 
                        className="btn-outline" 
                        style={{ padding: '8px 16px', fontSize: '12px' }} 
                        onClick={() => {
                          setSelectedApp(a);
                          setReason(a.reason || '');
                          setReferTeam(a.referredTo || '');
                        }}
                      >
                        Review
                      </button>
                      {isSuperAdmin && (
                        <button
                          className="btn-danger"
                          style={{ padding: '8px 12px', fontSize: '12px', display: 'inline-flex', alignItems: 'center' }}
                          onClick={() => handleDelete(a.id)}
                          title="Delete Application"
                        >
                          <Trash2 size={14} />
                        </button>
                      )}
                    </div>
                  </td>
                </tr>
              );
            })}
            {filteredApps.length === 0 && (
              <tr><td colSpan={5} style={{ textAlign: 'center', opacity: 0.7, padding: '32px' }}>No applications found.</td></tr>
            )}
          </tbody>
        </table>
      </div>

      {selectedApp && (
        <div style={{ position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.85)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 100, padding: '20px' }}>
          <div className="card" style={{ width: '100%', maxWidth: '520px', background: '#FFF' }}>
            <span className="heading-sm" style={{ marginBottom: '8px' }}>Applicant Review</span>
            <h2 style={{ marginTop: 0, fontSize: '32px', fontWeight: 900, marginBottom: '8px', textTransform: 'uppercase' }}>{selectedApp.name}</h2>
            
            <div style={{ display: 'flex', gap: '8px', marginBottom: '16px', flexWrap: 'wrap' }}>
              <span style={{ fontWeight: 700, fontSize: '13px', background: 'var(--brand-yellow)', padding: '4px 8px', border: '2px solid #0E1013' }}>
                Team: {selectedApp.team}
              </span>
              {selectedApp.referredTo && (
                <span className="font-mono" style={{ fontWeight: 700, fontSize: '12px', background: '#FEF3C7', color: '#92400E', padding: '4px 8px', border: '2px solid #0E1013' }}>
                  Referred to: {selectedApp.referredTo}
                </span>
              )}
              {selectedApp.email && (
                <span className="font-mono" style={{ fontSize: '12px', background: '#F7F5F0', padding: '4px 8px', border: '2px solid #0E1013' }}>
                  {selectedApp.email}
                </span>
              )}
              {selectedApp.phone && (
                <span className="font-mono" style={{ fontSize: '12px', background: '#F7F5F0', padding: '4px 8px', border: '2px solid #0E1013' }}>
                  {selectedApp.phone}
                </span>
              )}
            </div>

            {selectedApp.reason && (
              <div style={{ background: '#F7F5F0', border: '2px solid #0E1013', padding: '12px', marginBottom: '16px' }}>
                <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', opacity: 0.6, display: 'block', marginBottom: '4px' }}>Applicant's Reason:</span>
                <p style={{ margin: 0, fontSize: '14px', lineHeight: 1.4 }}>{selectedApp.reason}</p>
              </div>
            )}
            
            {/* If application is referred, allow target team manager or admin to accept or reject referral */}
            {selectedApp.status === 'referred' ? (
              <div style={{ display: 'flex', flexDirection: 'column', gap: '16px', marginTop: '16px' }}>
                <div style={{ background: '#FEF3C7', border: '2px solid #0E1013', padding: '12px' }}>
                  <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', color: '#92400E', display: 'block', marginBottom: '4px' }}>
                    ★ Referral for {selectedApp.referredTo || userTeam}
                  </span>
                  <p style={{ margin: 0, fontSize: '14px', lineHeight: 1.4 }}>
                    Candidate was referred from <strong>{selectedApp.team}</strong>. You can accept this referral into your team or reject it.
                  </p>
                </div>

                <div>
                  <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>Manager Response / Feedback:</label>
                  <textarea 
                    className="input-field" 
                    placeholder="Enter notes or feedback..." 
                    rows={3}
                    value={reason}
                    onChange={e => setReason(e.target.value)}
                  />
                </div>

                <div style={{ display: 'flex', gap: '12px' }}>
                  <button 
                    className="btn-primary" 
                    style={{ flex: 1, justifyContent: 'center', background: '#4ADE80' }} 
                    onClick={() => handleAction(selectedApp.id, 'accepted', selectedApp.referredTo || userTeam)}
                  >
                    Accept Referral
                  </button>
                  <button 
                    className="btn-danger" 
                    style={{ flex: 1, justifyContent: 'center' }} 
                    onClick={() => handleAction(selectedApp.id, 'rejected')}
                  >
                    Reject Referral
                  </button>
                </div>
              </div>
            ) : (
              <div style={{ display: 'flex', flexDirection: 'column', gap: '16px', marginTop: '16px' }}>
                <div>
                  <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>Decision Notes / Reason:</label>
                  <textarea 
                    className="input-field" 
                    placeholder="Enter notes or feedback..." 
                    rows={3}
                    value={reason}
                    onChange={e => setReason(e.target.value)}
                  />
                </div>
                
                <div style={{ display: 'flex', gap: '12px' }}>
                  <button className="btn-primary" style={{ flex: 1, justifyContent: 'center', background: '#4ADE80' }} onClick={() => handleAction(selectedApp.id, 'accepted')}>Accept</button>
                  <button className="btn-danger" style={{ flex: 1, justifyContent: 'center' }} onClick={() => handleAction(selectedApp.id, 'rejected')}>Reject</button>
                </div>

                <div style={{ borderTop: '2px solid var(--text-dark)', paddingTop: '16px', marginTop: '8px' }}>
                  <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '8px', textTransform: 'uppercase' }}>Refer to another team:</span>
                  <div style={{ display: 'flex', gap: '12px' }}>
                    <select className="input-field" value={referTeam} onChange={e => setReferTeam(e.target.value)}>
                      <option value="">Select Team...</option>
                      {teams.map(t => <option key={t.id} value={t.name}>{t.name}</option>)}
                    </select>
                    <button className="btn-outline" onClick={() => handleAction(selectedApp.id, 'referred')}>Refer</button>
                  </div>
                </div>
              </div>
            )}

            {isSuperAdmin && (
              <button 
                className="btn-danger" 
                style={{ width: '100%', marginTop: '16px', display: 'flex', alignItems: 'center', justifyContent: 'center', gap: '8px', padding: '12px' }} 
                onClick={() => handleDelete(selectedApp.id)}
              >
                <Trash2 size={16} /> Delete Application
              </button>
            )}
            
            <button 
              className="btn-outline" 
              style={{ width: '100%', marginTop: '12px' }} 
              onClick={() => {
                setSelectedApp(null);
                setReason('');
                setReferTeam('');
              }}
            >
              Close
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
