import { useState, useEffect } from 'react';
import { FileText, Download, Eye, X, Calendar, Search, Trash2 } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';

export default function FormResponses() {
  const { confirm } = useConfirm();
  const [submissions, setSubmissions] = useState<any[]>([]);
  const [loading, setLoading] = useState(true);
  const [selectedSub, setSelectedSub] = useState<any | null>(null);
  const [searchQuery, setSearchQuery] = useState('');

  const fetchSubmissions = () => {
    setLoading(true);
    fetch('/api/form_submissions')
      .then(r => r.json())
      .then(data => {
        setLoading(false);
        if (data && data.submissions) {
          setSubmissions(data.submissions.map((s: any) => ({
            ...s,
            id: s._id?.$oid || s.id || Math.random().toString()
          })));
        }
      })
      .catch(err => {
        setLoading(false);
        console.error('Failed to fetch submissions:', err);
      });
  };

  useEffect(() => {
    fetchSubmissions();
  }, []);

  const exportCSV = () => {
    if (submissions.length === 0) return;
    
    // Gather all unique keys across all submissions
    const allKeys = new Set<string>();
    submissions.forEach(s => {
      if (s.data && typeof s.data === 'object') {
        Object.keys(s.data).forEach(k => allKeys.add(k));
      }
    });

    const headers = ['Submitted At', 'Form Title', ...Array.from(allKeys)];
    const rows = submissions.map(s => {
      const row = [
        s.submittedAt || '',
        s.formTitle || 'General Form',
        ...Array.from(allKeys).map(k => {
          const val = s.data ? s.data[k] || '' : '';
          return `"${String(val).replace(/"/g, '""')}"`;
        })
      ];
      return row.join(',');
    });

    const csvContent = 'data:text/csv;charset=utf-8,' + [headers.join(','), ...rows].join('\n');
    const encodedUri = encodeURI(csvContent);
    const link = document.createElement('a');
    link.setAttribute('href', encodedUri);
    link.setAttribute('download', `form_responses_${new Date().toISOString().slice(0, 10)}.csv`);
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  };

  const handleDelete = async (id: string) => {
    const ok = await confirm({
      title: 'Delete Form Response?',
      message: 'Are you sure you want to permanently delete this form response? This action cannot be undone.',
      confirmText: 'Delete',
      cancelText: 'Cancel',
      type: 'danger'
    });
    if (!ok) return;

    try {
      const res = await fetch('/api/form_submissions', {
        method: 'DELETE',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ id })
      });
      if (!res.ok) {
        await fetch('/api/form_submissions', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ action: 'delete', id })
        });
      }
    } catch (err) {
      console.error('Failed to delete form submission:', err);
    }
    setSelectedSub(null);
    fetchSubmissions();
  };

  const filtered = submissions.filter(s => {
    if (!searchQuery) return true;
    const q = searchQuery.toLowerCase();
    const str = JSON.stringify(s).toLowerCase();
    return str.includes(q);
  });

  return (
    <div className="fade-in" style={{ paddingBottom: '40px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', marginBottom: '32px', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">03 — Form Responses</span>
          <h1 className="heading-lg">Form Responses.</h1>
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>
            Review, inspect, and export responses collected from custom published forms.
          </p>
        </div>

        <div style={{ display: 'flex', gap: '12px', alignItems: 'center', flexWrap: 'wrap' }}>
          <button
            className="btn-outline"
            onClick={exportCSV}
            disabled={submissions.length === 0}
            style={{ display: 'flex', alignItems: 'center', gap: '8px' }}
          >
            <Download size={18} /> Export CSV
          </button>
        </div>
      </div>

      {/* Filter / Search bar */}
      <div style={{ display: 'flex', gap: '12px', marginBottom: '24px' }}>
        <div style={{ position: 'relative', flex: 1, maxWidth: '400px' }}>
          <Search size={16} style={{ position: 'absolute', left: '14px', top: '50%', transform: 'translateY(-50%)', opacity: 0.5 }} />
          <input
            className="input-field"
            style={{ paddingLeft: '38px' }}
            placeholder="Search responses..."
            value={searchQuery}
            onChange={e => setSearchQuery(e.target.value)}
          />
        </div>
        <button className="btn-outline" onClick={fetchSubmissions} style={{ padding: '12px 18px', fontSize: '13px' }}>
          Refresh
        </button>
      </div>

      {/* Submissions Table */}
      <div className="card" style={{ padding: 0, overflowX: 'auto' }}>
        <table className="table">
          <thead>
            <tr>
              <th>Date / Time</th>
              <th>Form Name</th>
              <th>Summary of Answers</th>
              <th style={{ textAlign: 'right' }}>Action</th>
            </tr>
          </thead>
          <tbody>
            {loading ? (
              <tr>
                <td colSpan={4} style={{ textAlign: 'center', padding: '32px' }} className="font-mono">
                  Loading responses...
                </td>
              </tr>
            ) : filtered.length === 0 ? (
              <tr>
                <td colSpan={4} style={{ textAlign: 'center', padding: '40px' }}>
                  <FileText size={36} style={{ opacity: 0.3, marginBottom: '8px' }} />
                  <p className="font-mono" style={{ opacity: 0.6, margin: 0 }}>
                    {searchQuery ? 'No responses match your search.' : 'No form responses submitted yet.'}
                  </p>
                </td>
              </tr>
            ) : (
              filtered.map(s => {
                const fields = s.data || {};
                const keys = Object.keys(fields);
                const summary = keys.slice(0, 3).map(k => `${k}: ${fields[k]}`).join(' · ');

                return (
                  <tr key={s.id}>
                    <td style={{ whiteSpace: 'nowrap' }}>
                      <div style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
                        <Calendar size={14} style={{ opacity: 0.5 }} />
                        <span className="font-mono" style={{ fontSize: '13px', fontWeight: 600 }}>
                          {s.submittedAt ? s.submittedAt.replace('T', ' ').replace('Z', '') : 'Just now'}
                        </span>
                      </div>
                    </td>
                    <td>
                      <span className="badge badge-accepted" style={{ textTransform: 'none', fontWeight: 700 }}>
                        {s.formTitle || 'Recruitment Form'}
                      </span>
                    </td>
                    <td style={{ maxWidth: '400px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', opacity: 0.85 }}>
                      {summary || 'Empty response'}
                    </td>
                    <td style={{ textAlign: 'right', whiteSpace: 'nowrap' }}>
                      <div style={{ display: 'inline-flex', gap: '8px', alignItems: 'center' }}>
                        <button
                          className="btn-outline"
                          style={{ padding: '6px 14px', fontSize: '12px', display: 'inline-flex', alignItems: 'center', gap: '6px' }}
                          onClick={() => setSelectedSub(s)}
                        >
                          <Eye size={14} /> View Details
                        </button>
                        <button
                          className="btn-danger"
                          style={{ padding: '6px 10px', fontSize: '12px', display: 'inline-flex', alignItems: 'center' }}
                          onClick={() => handleDelete(s.id)}
                          title="Delete Response"
                        >
                          <Trash2 size={14} />
                        </button>
                      </div>
                    </td>
                  </tr>
                );
              })
            )}
          </tbody>
        </table>
      </div>

      {/* Response Details Modal */}
      {selectedSub && (
        <div style={{
          position: 'fixed',
          inset: 0,
          background: 'rgba(14,16,19,0.7)',
          backdropFilter: 'blur(4px)',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          zIndex: 1000,
          padding: '20px'
        }}>
          <div className="card" style={{
            maxWidth: '640px',
            width: '100%',
            background: '#FFF',
            border: '3px solid #0E1013',
            boxShadow: '10px 10px 0px #0E1013',
            padding: '32px',
            position: 'relative',
            maxHeight: '90vh',
            overflowY: 'auto'
          }}>
            <button
              onClick={() => setSelectedSub(null)}
              style={{
                position: 'absolute',
                top: '20px',
                right: '20px',
                background: 'transparent',
                border: 'none',
                cursor: 'pointer'
              }}
            >
              <X size={22} />
            </button>

            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' }}>
              Submission Details
            </span>
            <h2 style={{ fontSize: '26px', fontWeight: 900, textTransform: 'uppercase', margin: '0 0 4px 0' }}>
              {selectedSub.formTitle || 'Form Response'}
            </h2>
            <p className="font-mono" style={{ fontSize: '12px', opacity: 0.6, marginBottom: '24px' }}>
              Submitted: {selectedSub.submittedAt ? selectedSub.submittedAt.replace('T', ' ').replace('Z', '') : 'Unknown'}
            </p>

            <div style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
              {selectedSub.data && Object.keys(selectedSub.data).map(key => (
                <div key={key} style={{
                  padding: '16px',
                  background: '#F7F5F0',
                  border: '2px solid #0E1013',
                  boxShadow: '3px 3px 0px #0E1013'
                }}>
                  <div className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', opacity: 0.6, marginBottom: '4px' }}>
                    {key}
                  </div>
                  <div style={{ fontSize: '15px', fontWeight: 600, color: '#0E1013', whiteSpace: 'pre-wrap' }}>
                    {selectedSub.data[key] || '-'}
                  </div>
                </div>
              ))}
            </div>

            <button 
              className="btn-danger" 
              style={{ width: '100%', marginTop: '20px', display: 'flex', alignItems: 'center', justifyContent: 'center', gap: '8px', padding: '12px' }} 
              onClick={() => handleDelete(selectedSub.id)}
            >
              <Trash2 size={16} /> Delete Response
            </button>

            <button 
              className="btn-outline" 
              style={{ width: '100%', marginTop: '12px' }} 
              onClick={() => setSelectedSub(null)}
            >
              Close
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
