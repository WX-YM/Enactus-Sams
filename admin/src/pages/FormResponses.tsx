import { useEffect, useState } from 'react';
import { FileText, Download, Eye, X, Calendar, Search, Trash2, ArrowUpDown } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { api } from '../app/platform';
import type { FormDefinition, FormResponse, TimeCursor } from '../app/responses';
import { ErrorBanner, describe, run, useSession } from '../app/ui';
import { csvCell, downloadCsv } from '../app/csv';
import { routeFormsList, routeResponsesDelete, routeResponsesList } from '../api/hammer.generated';

type Submission = {
  readonly id: string;
  readonly formId: string;
  readonly formTitle: string;
  readonly submittedAt: number;
  // Answers in the form's field order, as label → text.
  readonly data: readonly (readonly [string, string])[];
};

const formatStamp = (ms: number) => new Date(ms).toISOString().replace('T', ' ').slice(0, 19);

function answerText(form: FormDefinition, fid: string, value: FormResponse['answers'][string] | undefined): string {
  if (value === undefined) return '';
  const field = form.fields.find((f) => f.fid === fid);
  const label = (v: string) => field?.options.find((o) => o.value === v)?.label ?? v;
  if (Array.isArray(value)) return value.map((v) => label(String(v))).join('; ');
  return label(String(value));
}

export default function FormResponses() {
  const { confirm } = useConfirm();
  const { affords } = useSession();
  const canDelete = affords(routeResponsesDelete);
  const [submissions, setSubmissions] = useState<Submission[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [selectedSub, setSelectedSub] = useState<Submission | null>(null);
  const [searchQuery, setSearchQuery] = useState('');
  const [sortOrder, setSortOrder] = useState<'newest' | 'oldest'>('newest');
  const [selectedForms, setSelectedForms] = useState<string[]>([]);

  const fetchPage = (id: string, cursor: TimeCursor | null) =>
    run((signal) => api.call(routeResponsesList, {
      params: { id },
      query: cursor === null ? {} : { after: cursor.after, at: String(cursor.at) },
      signal,
    }));

  // Every form, then every page of each form's responses.
  const fetchSubmissions = async () => {
    setLoading(true);
    const forms = await run((signal) => api.call(routeFormsList, { signal }));
    if (!forms.ok) { setError(describe(forms.error)); setLoading(false); return; }
    const all: Submission[] = [];
    for (const summary of forms.value.forms) {
      let after: TimeCursor | null = null;
      for (let i = 0; i < 100; i++) {
        const page = await fetchPage(summary.id, after);
        if (!page.ok) { setError(describe(page.error)); setLoading(false); return; }
        const form = page.value.form;
        for (const r of page.value.responses) {
          all.push({
            id: r.id,
            formId: summary.id,
            formTitle: form.title || 'Untitled Form',
            submittedAt: r.submitted_at,
            data: form.fields.map((f) => [f.label, answerText(form, f.fid, r.answers[f.fid])] as const),
          });
        }
        after = page.value.next;
        if (after === null) break;
      }
    }
    setError(null);
    setSubmissions(all);
    setLoading(false);
  };

  useEffect(() => { void fetchSubmissions(); }, []);

  const allFormTitles = Array.from(new Set(submissions.map((s) => s.formTitle))).sort();

  const toggleForm = (title: string) => {
    setSelectedForms((prev) => (prev.includes(title) ? prev.filter((t) => t !== title) : [...prev, title]));
  };

  const filtered = submissions.filter((s) => {
    if (selectedForms.length > 0 && !selectedForms.includes(s.formTitle)) return false;
    if (searchQuery) {
      const q = searchQuery.toLowerCase();
      const str = `${s.formTitle} ${s.data.map(([k, v]) => `${k} ${v}`).join(' ')}`.toLowerCase();
      if (!str.includes(q)) return false;
    }
    return true;
  });

  const sortedSubmissions = [...filtered].sort((a, b) => (sortOrder === 'newest' ? b.submittedAt - a.submittedAt : a.submittedAt - b.submittedAt));

  const exportCSV = () => {
    if (sortedSubmissions.length === 0) return;
    const allKeys: string[] = [];
    for (const s of sortedSubmissions) for (const [k] of s.data) if (!allKeys.includes(k)) allKeys.push(k);
    const headers = ['Submitted At', 'Form Title', ...allKeys];
    const rows = sortedSubmissions.map((s) => [
      formatStamp(s.submittedAt),
      s.formTitle,
      ...allKeys.map((k) => s.data.find(([key]) => key === k)?.[1] ?? ''),
    ]);
    downloadCsv(`form_responses_${new Date().toISOString().slice(0, 10)}.csv`, [headers, ...rows].map((r) => r.map(csvCell).join(',')));
  };

  const handleDelete = async (s: Submission) => {
    const ok = await confirm({
      title: 'Delete Form Response?',
      message: 'Are you sure you want to permanently delete this form response? This action cannot be undone.',
      confirmText: 'Delete',
      cancelText: 'Cancel',
      type: 'danger',
    });
    if (!ok) return;
    const result = await run((signal) => api.call(routeResponsesDelete, { params: { id: s.formId, response: s.id }, body: {}, signal }));
    if (!result.ok) { setError(describe(result.error)); return; }
    setSelectedSub(null);
    setSubmissions((prev) => prev.filter((x) => x.id !== s.id));
  };

  return (
    <div className="fade-in" style={{ paddingBottom: '40px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', marginBottom: '32px', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">03 — Form Responses</span>
          <h1 className="heading-lg">Form Responses.</h1>
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>Review, inspect, and export responses collected from custom published forms.</p>
        </div>
        <div style={{ display: 'flex', gap: '12px', alignItems: 'center', flexWrap: 'wrap' }}>
          <button className="btn-outline" onClick={exportCSV} disabled={sortedSubmissions.length === 0} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Download size={18} /> Export CSV
          </button>
        </div>
      </div>

      <div style={{ marginBottom: '16px' }}><ErrorBanner message={error} /></div>

      <div style={{ display: 'flex', flexDirection: 'column', gap: '16px', marginBottom: '24px' }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: '12px' }}>
          <div style={{ display: 'flex', gap: '12px', flex: '1 1 320px', maxWidth: '480px' }}>
            <div style={{ position: 'relative', flex: 1 }}>
              <Search size={16} style={{ position: 'absolute', left: '14px', top: '50%', transform: 'translateY(-50%)', opacity: 0.5 }} />
              <input className="input-field" style={{ paddingLeft: '38px' }} placeholder="Search responses..." value={searchQuery} onChange={(e) => setSearchQuery(e.target.value)} />
              {searchQuery && (
                <button aria-label="Clear search" onClick={() => setSearchQuery('')} style={{ position: 'absolute', right: '12px', top: '50%', transform: 'translateY(-50%)', background: 'transparent', border: 'none', cursor: 'pointer', opacity: 0.6 }}>
                  <X size={14} />
                </button>
              )}
            </div>
            <button className="btn-outline" onClick={() => void fetchSubmissions()} style={{ padding: '12px 18px', fontSize: '13px' }}>Refresh</button>
          </div>
          <button
            className="btn-outline"
            onClick={() => setSortOrder((s) => (s === 'newest' ? 'oldest' : 'newest'))}
            style={{ display: 'flex', alignItems: 'center', gap: '8px', padding: '10px 18px', fontSize: '12px', fontWeight: 700 }}
            title="Toggle sort order"
          >
            <ArrowUpDown size={14} />
            Sort: {sortOrder === 'newest' ? 'Newest to Oldest' : 'Oldest to Newest'}
          </button>
        </div>

        {allFormTitles.length > 0 && (
          <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap', alignItems: 'center' }}>
            <div style={{ flex: '1 1 200px', maxWidth: '280px' }}>
              <select
                className="input-field"
                style={{ fontSize: '13px', fontWeight: 600 }}
                value={selectedForms.length === 1 ? selectedForms[0] : selectedForms.length === 0 ? 'all' : 'multi'}
                onChange={(e) => setSelectedForms(e.target.value === 'all' ? [] : [e.target.value])}
              >
                <option value="all">All Forms ({allFormTitles.length})</option>
                {selectedForms.length > 1 && <option value="multi" disabled>Multiple Forms ({selectedForms.length})</option>}
                {allFormTitles.map((title) => <option key={title} value={title}>{title}</option>)}
              </select>
            </div>
            <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
              <button
                onClick={() => setSelectedForms([])}
                style={{ padding: '5px 10px', border: '1.5px solid #0E1013', background: selectedForms.length === 0 ? '#0E1013' : '#FFF', color: selectedForms.length === 0 ? '#FFC629' : '#0E1013', fontFamily: 'IBM Plex Mono, monospace', fontSize: '11px', fontWeight: 700, cursor: 'pointer' }}
              >
                ALL
              </button>
              {allFormTitles.map((title) => {
                const isSelected = selectedForms.includes(title);
                return (
                  <button
                    key={title}
                    onClick={() => toggleForm(title)}
                    style={{ padding: '5px 10px', border: '1.5px solid #0E1013', background: isSelected ? '#FFC629' : '#F7F5F0', color: '#0E1013', fontFamily: 'IBM Plex Mono, monospace', fontSize: '11px', fontWeight: isSelected ? 800 : 600, boxShadow: isSelected ? '2px 2px 0px #0E1013' : 'none', cursor: 'pointer' }}
                  >
                    {title}
                  </button>
                );
              })}
              {selectedForms.length > 0 && (
                <button onClick={() => setSelectedForms([])} style={{ padding: '5px 8px', border: 'none', background: 'transparent', color: '#E53935', fontFamily: 'IBM Plex Mono, monospace', fontSize: '11px', fontWeight: 700, cursor: 'pointer', textDecoration: 'underline' }}>
                  Clear
                </button>
              )}
            </div>
          </div>
        )}
      </div>

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
              <tr><td colSpan={4} style={{ textAlign: 'center', padding: '32px' }} className="font-mono">Loading responses...</td></tr>
            ) : sortedSubmissions.length === 0 ? (
              <tr>
                <td colSpan={4} style={{ textAlign: 'center', padding: '40px' }}>
                  <FileText size={36} style={{ opacity: 0.3, marginBottom: '8px' }} />
                  <p className="font-mono" style={{ opacity: 0.6, margin: 0 }}>
                    {searchQuery || selectedForms.length > 0 ? 'No responses match your search or filters.' : 'No form responses submitted yet.'}
                  </p>
                </td>
              </tr>
            ) : (
              sortedSubmissions.map((s) => {
                const summary = s.data.filter(([, v]) => v !== '').slice(0, 3).map(([k, v]) => `${k}: ${v}`).join(' · ');
                return (
                  <tr key={s.id}>
                    <td style={{ whiteSpace: 'nowrap' }}>
                      <div style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
                        <Calendar size={14} style={{ opacity: 0.5 }} />
                        <span className="font-mono" style={{ fontSize: '13px', fontWeight: 600 }}>{formatStamp(s.submittedAt)}</span>
                      </div>
                    </td>
                    <td>
                      <span className="badge badge-accepted" style={{ textTransform: 'none', fontWeight: 700, display: 'inline-flex', alignItems: 'center', whiteSpace: 'nowrap' }} dir="auto">{s.formTitle}</span>
                    </td>
                    <td dir="auto" style={{ maxWidth: '400px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', opacity: 0.85 }}>{summary || 'Empty response'}</td>
                    <td style={{ textAlign: 'right', whiteSpace: 'nowrap' }}>
                      <div style={{ display: 'inline-flex', gap: '8px', alignItems: 'center' }}>
                        <button className="btn-outline" style={{ padding: '6px 14px', fontSize: '12px', display: 'inline-flex', alignItems: 'center', gap: '6px' }} onClick={() => setSelectedSub(s)}>
                          <Eye size={14} /> View Details
                        </button>
                        {canDelete && (
                          <button className="btn-danger" style={{ padding: '6px 10px', fontSize: '12px', display: 'inline-flex', alignItems: 'center' }} onClick={() => handleDelete(s)} title="Delete Response" aria-label="Delete response">
                            <Trash2 size={14} />
                          </button>
                        )}
                      </div>
                    </td>
                  </tr>
                );
              })
            )}
          </tbody>
        </table>
      </div>

      {selectedSub && (
        <div role="dialog" aria-modal="true" style={{ position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.7)', backdropFilter: 'blur(4px)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1000, padding: '20px' }}>
          <div className="card" style={{ maxWidth: '640px', width: '100%', background: '#FFF', border: '3px solid #0E1013', boxShadow: '10px 10px 0px #0E1013', padding: '32px', position: 'relative', maxHeight: '90vh', overflowY: 'auto' }}>
            <button aria-label="Close" onClick={() => setSelectedSub(null)} style={{ position: 'absolute', top: '20px', right: '20px', background: 'transparent', border: 'none', cursor: 'pointer' }}>
              <X size={22} />
            </button>
            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' }}>Submission Details</span>
            <h2 style={{ fontSize: '26px', fontWeight: 900, textTransform: 'uppercase', margin: '0 0 4px 0' }} dir="auto">{selectedSub.formTitle}</h2>
            <p className="font-mono" style={{ fontSize: '12px', opacity: 0.6, marginBottom: '24px' }}>Submitted: {formatStamp(selectedSub.submittedAt)}</p>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
              {selectedSub.data.map(([key, value]) => (
                <div key={key} style={{ padding: '16px', background: '#F7F5F0', border: '2px solid #0E1013', boxShadow: '3px 3px 0px #0E1013' }}>
                  <div className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', opacity: 0.6, marginBottom: '4px' }} dir="auto">{key}</div>
                  <div style={{ fontSize: '15px', fontWeight: 600, color: '#0E1013', whiteSpace: 'pre-wrap' }} dir="auto">{value || '-'}</div>
                </div>
              ))}
            </div>
            {canDelete && (
              <button className="btn-danger" style={{ width: '100%', marginTop: '20px', display: 'flex', alignItems: 'center', justifyContent: 'center', gap: '8px', padding: '12px' }} onClick={() => handleDelete(selectedSub)}>
                <Trash2 size={16} /> Delete Response
              </button>
            )}
            <button className="btn-outline" style={{ width: '100%', marginTop: '12px' }} onClick={() => setSelectedSub(null)}>Close</button>
          </div>
        </div>
      )}
    </div>
  );
}
