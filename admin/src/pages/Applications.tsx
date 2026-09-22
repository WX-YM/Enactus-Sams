import { useState, useEffect } from 'react';
import { Trash2, Download, ArrowUpDown, Calendar, Search, X } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { authFetch } from '../api';

export default function Applications({ role }: { role: string }) {
  const { confirm } = useConfirm();
  const [apps, setApps] = useState<any[]>([]);
  const [teams, setTeams] = useState<any[]>([]);
  const [selectedApp, setSelectedApp] = useState<any>(null);
  const [reason, setReason] = useState('');
  const [referTeam, setReferTeam] = useState('');
  const [filter, setFilter] = useState('all');
  const [sortOrder, setSortOrder] = useState<'newest' | 'oldest'>('newest');
  const [selectedTeams, setSelectedTeams] = useState<string[]>([]);
  const [searchQuery, setSearchQuery] = useState('');

  const userTeam = localStorage.getItem('admin_team') || '';
  const adminEmail = localStorage.getItem('admin_email') || '';
  const isSuperAdmin = role === 'superadmin' || adminEmail === 'admin@enactussams.org';
  const isTeamScoped = !isSuperAdmin && (role === 'manager' || role === 'vice manager') && !!userTeam;

  const fetchApps = () => {
    authFetch('/api/applications_list')
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

    authFetch('/api/teams')
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

    authFetch('/api/applications_update', {
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

  const handleDelete = async (id: string) => {
    const ok = await confirm({
      title: 'Delete Application?',
      message: 'Are you sure you want to permanently delete this application response? This cannot be undone.',
      confirmText: 'Delete',
      cancelText: 'Cancel',
      type: 'danger'
    });
    if (!ok) return;
    authFetch('/api/applications_update', {
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

  const allTeamNames = Array.from(
    new Set(
      [
        ...teams.map(t => (t.name || '').trim()),
        ...apps.map(a => (a.team || '').trim()),
        ...apps.map(a => (a.referredTo || '').trim())
      ].filter(Boolean)
    )
  ).sort((a, b) => a.localeCompare(b));

  const getAppTime = (a: any) => {
    if (a.submittedAt) {
      const t = new Date(a.submittedAt).getTime();
      if (!isNaN(t)) return t;
    }
    if (a._id?.$oid && typeof a._id.$oid === 'string' && a._id.$oid.length === 24) {
      return parseInt(a._id.$oid.substring(0, 8), 16) * 1000;
    }
    if (a.id && typeof a.id === 'string' && a.id.length === 24) {
      const t = parseInt(a.id.substring(0, 8), 16) * 1000;
      if (!isNaN(t)) return t;
    }
    return 0;
  };

  const formatAppDate = (a: any) => {
    if (a.submittedAt) {
      return a.submittedAt.replace('T', ' ').replace('Z', '').slice(0, 16);
    }
    const t = getAppTime(a);
    if (t > 0) {
      return new Date(t).toISOString().replace('T', ' ').replace('Z', '').slice(0, 16);
    }
    return 'Recent';
  };

  const toggleTeam = (teamName: string) => {
    const target = teamName.trim();
    setSelectedTeams(prev => {
      const exists = prev.some(t => t.trim().toLowerCase() === target.toLowerCase());
      if (exists) {
        return prev.filter(t => t.trim().toLowerCase() !== target.toLowerCase());
      } else {
        return [...prev, target];
      }
    });
  };

  // Scoped view for managers
  const visibleApps = apps.filter(a => {
    if (!isTeamScoped) return true;
    const matchDirect = a.team && a.team.trim().toLowerCase() === userTeam.trim().toLowerCase();
    const matchReferred = a.referredTo && a.referredTo.trim().toLowerCase() === userTeam.trim().toLowerCase();
    return matchDirect || matchReferred;
  });

  const filteredApps = visibleApps.filter(a => {
    if (filter !== 'all' && a.status !== filter) return false;

    if (selectedTeams.length > 0) {
      const direct = a.team && selectedTeams.some(t => t.trim().toLowerCase() === a.team.trim().toLowerCase());
      const referred = a.referredTo && selectedTeams.some(t => t.trim().toLowerCase() === a.referredTo.trim().toLowerCase());
      if (!direct && !referred) return false;
    }

    if (searchQuery) {
      const q = searchQuery.toLowerCase();
      const str = `${a.name || ''} ${a.email || ''} ${a.phone || ''} ${a.team || ''} ${a.reason || ''}`.toLowerCase();
      if (!str.includes(q)) return false;
    }

    return true;
  });

  const sortedApps = [...filteredApps].sort((a, b) => {
    const timeA = getAppTime(a);
    const timeB = getAppTime(b);
    return sortOrder === 'newest' ? timeB - timeA : timeA - timeB;
  });

  const exportCSV = () => {
    if (sortedApps.length === 0) return;

    const headers = ['Applicant Name', 'Email', 'Phone', 'Team Applied', 'Status', 'Referred To', 'Date Submitted', 'Reason / Notes'];
    const rows = sortedApps.map(a => {
      const row = [
        `"${String(a.name || '').replace(/"/g, '""')}"`,
        `"${String(a.email || '').replace(/"/g, '""')}"`,
        `"${String(a.phone || '').replace(/"/g, '""')}"`,
        `"${String(a.team || '').replace(/"/g, '""')}"`,
        `"${String(a.status || 'pending').replace(/"/g, '""')}"`,
        `"${String(a.referredTo || '').replace(/"/g, '""')}"`,
        `"${formatAppDate(a)}"`,
        `"${String(a.reason || '').replace(/"/g, '""')}"`
      ];
      return row.join(',');
    });

    const csvContent = 'data:text/csv;charset=utf-8,' + [headers.join(','), ...rows].join('\n');
    const encodedUri = encodeURI(csvContent);
    const link = document.createElement('a');
    link.setAttribute('href', encodedUri);
    link.setAttribute('download', `application_responses_${new Date().toISOString().slice(0, 10)}.csv`);
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  };

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
          <button
            className="btn-outline"
            onClick={exportCSV}
            disabled={visibleApps.length === 0}
            style={{ display: 'flex', alignItems: 'center', gap: '8px' }}
          >
            <Download size={18} /> Export CSV
          </button>
        </div>
      </div>

      {/* Filters & Sorting Bar */}
      <div style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
        {/* Status Filters & Sort Toggle */}
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: '12px' }}>
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

          <button
            className="btn-outline"
            onClick={() => setSortOrder(s => s === 'newest' ? 'oldest' : 'newest')}
            style={{ display: 'flex', alignItems: 'center', gap: '8px', padding: '8px 16px', fontSize: '12px', fontWeight: 700 }}
            title="Click to toggle sorting order"
          >
            <ArrowUpDown size={14} />
            Sort: {sortOrder === 'newest' ? 'Newest to Oldest' : 'Oldest to Newest'}
          </button>
        </div>

        {/* Team Filter & Search */}
        <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap', alignItems: 'center' }}>
          {/* Search Box */}
          <div style={{ position: 'relative', flex: '1 1 240px', maxWidth: '360px' }}>
            <Search size={16} style={{ position: 'absolute', left: '12px', top: '50%', transform: 'translateY(-50%)', opacity: 0.5 }} />
            <input
              className="input-field"
              style={{ paddingLeft: '36px', fontSize: '13px' }}
              placeholder="Search applicants..."
              value={searchQuery}
              onChange={e => setSearchQuery(e.target.value)}
            />
            {searchQuery && (
              <button
                onClick={() => setSearchQuery('')}
                style={{ position: 'absolute', right: '10px', top: '50%', transform: 'translateY(-50%)', background: 'transparent', border: 'none', cursor: 'pointer', opacity: 0.6 }}
              >
                <X size={14} />
              </button>
            )}
          </div>

          {/* Team Dropdown Filter */}
          {!isTeamScoped && allTeamNames.length > 0 && (
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px', flex: '1 1 200px', maxWidth: '280px' }}>
              <select
                className="input-field"
                style={{ fontSize: '13px', fontWeight: 600 }}
                value={selectedTeams.length === 1 ? selectedTeams[0] : (selectedTeams.length === 0 ? 'all' : 'multi')}
                onChange={e => {
                  if (e.target.value === 'all') {
                    setSelectedTeams([]);
                  } else {
                    setSelectedTeams([e.target.value]);
                  }
                }}
              >
                <option value="all">All Teams ({allTeamNames.length})</option>
                {selectedTeams.length > 1 && (
                  <option value="multi" disabled>
                    Multiple Teams ({selectedTeams.length})
                  </option>
                )}
                {allTeamNames.map(t => (
                  <option key={t} value={t}>
                    {t}
                  </option>
                ))}
              </select>
            </div>
          )}

          {/* Quick Team Chips */}
          {!isTeamScoped && allTeamNames.length > 0 && (
            <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
              <button
                onClick={() => setSelectedTeams([])}
                style={{
                  padding: '5px 10px',
                  border: '1.5px solid #0E1013',
                  background: selectedTeams.length === 0 ? '#0E1013' : '#FFF',
                  color: selectedTeams.length === 0 ? '#FFC629' : '#0E1013',
                  fontFamily: 'IBM Plex Mono, monospace',
                  fontSize: '11px',
                  fontWeight: 700,
                  cursor: 'pointer'
                }}
              >
                ALL
              </button>
              {allTeamNames.map(t => {
                const isSelected = selectedTeams.some(st => st.trim().toLowerCase() === t.trim().toLowerCase());
                return (
                  <button
                    key={t}
                    onClick={() => toggleTeam(t)}
                    style={{
                      padding: '5px 10px',
                      border: '1.5px solid #0E1013',
                      background: isSelected ? '#FFC629' : '#F7F5F0',
                      color: '#0E1013',
                      fontFamily: 'IBM Plex Mono, monospace',
                      fontSize: '11px',
                      fontWeight: isSelected ? 800 : 600,
                      boxShadow: isSelected ? '2px 2px 0px #0E1013' : 'none',
                      cursor: 'pointer'
                    }}
                  >
                    {t}
                  </button>
                );
              })}
              {selectedTeams.length > 0 && (
                <button
                  onClick={() => setSelectedTeams([])}
                  style={{
                    padding: '5px 8px',
                    border: 'none',
                    background: 'transparent',
                    color: '#E53935',
                    fontFamily: 'IBM Plex Mono, monospace',
                    fontSize: '11px',
                    fontWeight: 700,
                    cursor: 'pointer',
                    textDecoration: 'underline'
                  }}
                >
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
            {sortedApps.map(a => {
              const isReferredToMe = isTeamScoped && a.referredTo && a.referredTo.trim().toLowerCase() === userTeam.trim().toLowerCase();
              return (
                <tr key={a.id}>
                  <td>
                    <strong style={{ fontSize: '16px', textTransform: 'uppercase', letterSpacing: '0.02em', display: 'block' }}>{a.name}</strong>
                    <div style={{ display: 'flex', flexDirection: 'column', gap: '2px', marginTop: '4px' }}>
                      {a.email && <span className="font-mono" style={{ fontSize: '11px', opacity: 0.6 }}>{a.email}</span>}
                      {a.phone && <span className="font-mono" style={{ fontSize: '11px', opacity: 0.6 }}>{a.phone}</span>}
                      <div style={{ display: 'flex', alignItems: 'center', gap: '4px', opacity: 0.5, marginTop: '2px' }}>
                        <Calendar size={11} />
                        <span className="font-mono" style={{ fontSize: '10px' }}>{formatAppDate(a)}</span>
                      </div>
                    </div>
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
            {sortedApps.length === 0 && (
              <tr><td colSpan={5} style={{ textAlign: 'center', opacity: 0.7, padding: '32px' }}>No applications found matching your criteria.</td></tr>
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
                      {allTeamNames.map(name => <option key={name} value={name}>{name}</option>)}
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
