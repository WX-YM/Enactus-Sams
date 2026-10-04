import { useEffect, useState } from 'react';
import { Plus, Trash2, ArrowUp, ArrowDown, Save, CheckCircle2, Eye, FileText, Share2, Copy, ExternalLink, X } from 'lucide-react';
import { Link } from 'react-router-dom';
import { useConfirm } from '../context/ConfirmContext';
import { api } from '../app/platform';
import type { FormDefinition, FormStatus } from '../app/responses';
import { ErrorBanner, codeOf, describe, run, useLoad } from '../app/ui';
import {
  kFieldTypes,
  routeFormsCreate,
  routeFormsDelete,
  routeFormsList,
  routeFormsPublic,
  routeFormsUpdate,
} from '../api/hammer.generated';

// The field types are the server's table (src/config/field_types.h); these are
// only the words for them.
const kTypeWords: Record<keyof typeof kFieldTypes, string> = {
  TEXT_SHORT: 'Text Input',
  TEXT_LONG: 'Long Text (Textarea)',
  EMAIL: 'Email',
  PHONE: 'Phone / Number',
  SELECT_SINGLE: 'Dropdown (Select)',
};

type DraftOption = { value: string; label: string };
type DraftField = { fid: string; label: string; type: keyof typeof kFieldTypes; optional: boolean; options: DraftOption[]; stored: boolean; optionsText: string };
type Draft = { id: string | null; version: number; title: string; status: FormStatus; closesAt: string; maxSubmissions: number; fields: DraftField[] };

const optionsTextOf = (options: readonly DraftOption[]) => options.map((o) => o.label).join(', ');

const emptyDraft = (): Draft => ({
  id: null,
  version: 0,
  title: 'New Form',
  status: 'active',
  closesAt: '',
  maxSubmissions: 0,
  fields: [
    { fid: 'f1', label: 'Full Name', type: 'TEXT_SHORT', optional: false, options: [], stored: false, optionsText: '' },
    { fid: 'f2', label: 'Email Address', type: 'EMAIL', optional: false, options: [], stored: false, optionsText: '' },
  ],
});

function fromDefinition(form: FormDefinition): Draft {
  return {
    id: form.id,
    version: form.version,
    title: form.title,
    status: form.status,
    closesAt: form.closes_at === null ? '' : new Date(form.closes_at - new Date().getTimezoneOffset() * 60000).toISOString().slice(0, 16),
    maxSubmissions: form.max_submissions ?? 0,
    fields: form.fields.map((f) => {
      const options = f.options.map((o) => ({ value: o.value, label: o.label }));
      return {
        fid: f.fid,
        label: f.label,
        type: (f.type in kFieldTypes ? f.type : 'TEXT_SHORT') as keyof typeof kFieldTypes,
        optional: f.optional,
        options,
        stored: true,
        optionsText: optionsTextOf(options),
      };
    }),
  };
}

// A stored option value is [A-Za-z0-9_-]; one derived from a label, unique in
// its field. An existing option keeps its value (answers point at it) as long
// as its label is still in the list.
function valueFor(label: string, taken: readonly string[]): string {
  let base = label.toLowerCase().normalize('NFKD').replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 48);
  if (base === '') base = 'option';
  let value = base;
  for (let n = 2; taken.includes(value); n++) value = `${base}_${n}`;
  return value;
}

function parseOptions(text: string, previous: readonly DraftOption[]): DraftOption[] {
  const out: DraftOption[] = [];
  for (const raw of text.split(',')) {
    const label = raw.trim();
    if (!label || out.some((o) => o.label === label)) continue;
    const kept = previous.find((o) => o.label === label);
    out.push(kept ?? { label, value: valueFor(label, [...previous.map((o) => o.value), ...out.map((o) => o.value)]) });
  }
  return out;
}

function nextFid(fields: readonly DraftField[]): string {
  const used = fields.map((f) => Number(f.fid.slice(1))).filter((n) => Number.isFinite(n));
  return `f${Math.max(0, ...used) + 1}`;
}

const previewInput = { width: '100%', padding: '14px 16px', border: '2.5px solid #0E1013', background: '#F7F5F0', fontFamily: 'Archivo', fontSize: '15px', fontWeight: 600, outline: 'none', boxShadow: '4px 4px 0px #0E1013' } as const;

export default function FormMaker() {
  const { confirm } = useConfirm();
  const forms = useLoad((signal) => api.call(routeFormsList, { signal }), []);
  const [draft, setDraft] = useState<Draft | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [published, setPublished] = useState(false);
  const [previewMode, setPreviewMode] = useState(false);
  const [shareModalOpen, setShareModalOpen] = useState(false);
  const [copied, setCopied] = useState(false);

  const open = async (id: string) => {
    setError(null);
    const result = await run((signal) => api.call(routeFormsPublic, { params: { id }, signal }));
    if (result.ok) setDraft(fromDefinition(result.value));
    else setError(describe(result.error));
  };

  // Open the newest form on arrival, or start a fresh one when there is none.
  useEffect(() => {
    if (draft !== null || forms.data === null) return;
    if (forms.data.forms.length > 0) void open(forms.data.forms[0].id);
    else setDraft(emptyDraft());
  }, [forms.data]);

  const publicLink = draft?.id ? `${window.location.origin}/apply?form=${encodeURIComponent(draft.id)}` : '';

  const handlePublish = async () => {
    if (draft === null) return;
    setSaving(true);
    setError(null);
    const body = {
      title: draft.title.trim(),
      status: draft.status,
      closes_at: draft.closesAt === '' ? null : new Date(draft.closesAt).getTime(),
      max_submissions: draft.maxSubmissions,
      fields: draft.fields.map((f) => ({
        fid: f.fid,
        label: f.label.trim(),
        type: f.type,
        optional: f.optional,
        ...(f.type === 'SELECT_SINGLE' ? { options: f.options.map((o) => ({ value: o.value, label: o.label })) } : {}),
      })),
    };
    const result = draft.id === null
      ? await run((signal) => api.call(routeFormsCreate, { body, signal }))
      : await run((signal) => api.call(routeFormsUpdate, { params: { id: draft.id as string }, body: { version: draft.version, ...body }, signal }));
    setSaving(false);
    if (!result.ok) {
      setError(codeOf(result.error) === 'CONFLICT'
        ? 'This form already has responses, so fields and options cannot be removed or made required. Create a new form instead.'
        : describe(result.error));
      return;
    }
    forms.reload();
    await open(draft.id ?? (result.value as { id: string }).id);
    setPublished(true);
    if (draft.status === 'active') setShareModalOpen(true);
    setTimeout(() => setPublished(false), 3000);
  };

  const update = (patch: Partial<Draft>) => { setPublished(false); setDraft((d) => (d === null ? d : { ...d, ...patch })); };
  const updateField = (index: number, patch: Partial<DraftField>) => {
    setPublished(false);
    setDraft((d) => (d === null ? d : { ...d, fields: d.fields.map((f, i) => (i === index ? { ...f, ...patch } : f)) }));
  };

  const addField = () => {
    if (draft === null) return;
    update({ fields: [...draft.fields, { fid: nextFid(draft.fields), label: 'New Field', type: 'TEXT_SHORT', optional: true, options: [], stored: false, optionsText: '' }] });
  };

  const deleteField = async (index: number) => {
    if (draft === null) return;
    const ok = await confirm({ title: 'Delete Field?', message: `Are you sure you want to remove the field "${draft.fields[index].label || 'New Field'}"?`, confirmText: 'Delete Field', cancelText: 'Cancel', type: 'danger' });
    if (!ok) return;
    update({ fields: draft.fields.filter((_, i) => i !== index) });
  };

  const moveField = (index: number, direction: 'up' | 'down') => {
    if (draft === null) return;
    const target = direction === 'up' ? index - 1 : index + 1;
    if (target < 0 || target >= draft.fields.length) return;
    const copy = [...draft.fields];
    const [item] = copy.splice(index, 1);
    copy.splice(target, 0, item);
    update({ fields: copy });
  };

  const deleteForm = async () => {
    if (draft === null || draft.id === null) return;
    const ok = await confirm({ title: 'Delete Form?', message: `Delete “${draft.title}” and every response to it? Export the responses first if you need them.`, type: 'danger', confirmText: 'Delete' });
    if (!ok) return;
    const result = await run((signal) => api.call(routeFormsDelete, { params: { id: draft.id as string }, body: {}, signal }));
    if (!result.ok) { setError(describe(result.error)); return; }
    setDraft(null);
    forms.reload();
  };

  const copyShareLink = () => {
    void navigator.clipboard.writeText(publicLink).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 2500);
    });
  };

  return (
    <div className="fade-in" style={{ paddingBottom: '40px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', marginBottom: '32px', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">03 — Builder</span>
          <h1 className="heading-lg">Form Maker.</h1>
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>Build, customize, and publish forms for candidate recruitment and registrations.</p>
        </div>
        <div style={{ display: 'flex', gap: '12px', alignItems: 'center', flexWrap: 'wrap' }}>
          <button className="btn-outline" onClick={() => setShareModalOpen(true)} disabled={!draft?.id} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Share2 size={18} /> Share Form Link
          </button>
          <Link to="/form-responses" className="btn-outline" style={{ display: 'flex', alignItems: 'center', gap: '8px', textDecoration: 'none' }}>
            <FileText size={18} /> View Responses →
          </Link>
          <button className="btn-outline" onClick={() => setPreviewMode(!previewMode)} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Eye size={18} /> {previewMode ? 'Edit Fields' : 'Live Preview'}
          </button>
          <button className="btn-primary" onClick={handlePublish} disabled={saving || draft === null} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
            {published ? <CheckCircle2 size={18} color="#0E1013" /> : <Save size={18} />}
            {saving ? 'Publishing...' : published ? 'Published!' : 'Publish Form'}
          </button>
        </div>
      </div>

      <div style={{ marginBottom: '16px' }}><ErrorBanner message={forms.error ?? error} /></div>

      {/* Which form is being edited */}
      <div style={{ display: 'flex', gap: '12px', alignItems: 'center', flexWrap: 'wrap', marginBottom: '24px' }}>
        <select
          className="input-field"
          style={{ maxWidth: '360px', fontWeight: 700 }}
          value={draft?.id ?? 'new'}
          onChange={(e) => { if (e.target.value === 'new') setDraft(emptyDraft()); else void open(e.target.value); }}
        >
          {(forms.data?.forms ?? []).map((f) => <option key={f.id} value={f.id}>{f.title} — {f.status} ({f.submission_count} responses)</option>)}
          {draft !== null && draft.id === null && <option value="new">{draft.title || 'New Form'} (not published yet)</option>}
        </select>
        <button className="btn-outline" onClick={() => { setDraft(emptyDraft()); setError(null); }} style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
          <Plus size={16} /> New Form
        </button>
        {draft?.id && (
          <button className="btn-danger" onClick={deleteForm} style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
            <Trash2 size={16} /> Delete Form
          </button>
        )}
      </div>

      {draft === null ? null : previewMode ? (
        <div className="card" style={{ maxWidth: '640px', margin: '0 auto', background: '#FFC629', padding: '40px', border: '3px solid #0E1013', boxShadow: '8px 8px 0px #0E1013' }}>
          <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: 'rgba(14,16,19,0.7)', display: 'block', marginBottom: '8px' }}>05 — FORM PREVIEW</span>
          <h2 style={{ margin: '0 0 28px 0', fontSize: '36px', fontWeight: 900, textTransform: 'uppercase', color: '#0E1013' }} dir="auto">{draft.title}</h2>
          <form onSubmit={(e) => e.preventDefault()} style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
            {draft.fields.map((f) => (
              <div key={f.fid}>
                <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px', textTransform: 'uppercase' }} dir="auto">
                  {f.label} {!f.optional && <span style={{ color: '#E53935' }}>*</span>}
                </label>
                {f.type === 'TEXT_LONG' ? (
                  <textarea rows={3} style={previewInput} />
                ) : f.type === 'SELECT_SINGLE' ? (
                  <select style={previewInput}>
                    <option value="">Select...</option>
                    {f.options.map((o) => <option key={o.value} value={o.value}>{o.label}</option>)}
                  </select>
                ) : (
                  <input type={f.type === 'EMAIL' ? 'email' : f.type === 'PHONE' ? 'tel' : 'text'} style={previewInput} />
                )}
              </div>
            ))}
            <button type="button" className="btn-primary" style={{ marginTop: '12px', justifyContent: 'center', background: '#0E1013', color: '#FFC629' }}>Submit Application</button>
          </form>
        </div>
      ) : (
        <div style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
          <div className="card">
            <h2 className="heading-sm" style={{ marginBottom: '16px' }}>Form Details</h2>
            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(200px, 1fr))', gap: '20px' }}>
              <div>
                <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '6px' }}>Form Title</label>
                <input className="input-field" maxLength={200} value={draft.title} onChange={(e) => update({ title: e.target.value })} dir="auto" />
              </div>
              <div>
                <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '6px' }}>Status</label>
                <select className="input-field" value={draft.status} onChange={(e) => update({ status: e.target.value as FormStatus })}>
                  <option value="active">Published (accepting responses)</option>
                  <option value="draft">Draft (hidden)</option>
                  <option value="closed">Closed</option>
                </select>
              </div>
              <div>
                <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '6px' }}>Closes At (optional)</label>
                <input className="input-field" type="datetime-local" value={draft.closesAt} onChange={(e) => update({ closesAt: e.target.value })} />
              </div>
              <div>
                <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '6px' }}>Max Responses (0 = no limit)</label>
                <input className="input-field" type="number" min={0} max={1000000} value={draft.maxSubmissions} onChange={(e) => update({ maxSubmissions: Math.max(0, Number(e.target.value) || 0) })} />
              </div>
            </div>
          </div>

          <div className="card">
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '24px', flexWrap: 'wrap', gap: '12px' }}>
              <div>
                <h2 className="heading-sm" style={{ margin: 0 }}>Form Fields ({draft.fields.length})</h2>
                <p className="font-mono" style={{ margin: '4px 0 0 0', fontSize: '12px', opacity: 0.6 }}>Customize questions, inputs, and validation.</p>
              </div>
              <button className="btn-primary" onClick={addField} style={{ display: 'flex', alignItems: 'center', gap: '6px', fontSize: '13px' }}>
                <Plus size={16} /> Add Field
              </button>
            </div>

            <div style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
              {draft.fields.map((f, idx) => (
                <div key={f.fid} style={{ border: '2.5px solid #0E1013', padding: '20px', background: '#F7F5F0', boxShadow: '4px 4px 0px #0E1013', display: 'flex', flexDirection: 'column', gap: '12px' }}>
                  <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: '8px' }}>
                    <span className="font-mono" style={{ fontSize: '12px', fontWeight: 800, background: '#0E1013', color: '#FFF', padding: '2px 8px' }} dir="auto">#{idx + 1} — {f.label.toUpperCase()}</span>
                    <div style={{ display: 'flex', gap: '6px', alignItems: 'center' }}>
                      <button onClick={() => moveField(idx, 'up')} disabled={idx === 0} style={{ padding: '6px', border: '1.5px solid #0E1013', background: '#FFF', cursor: idx === 0 ? 'not-allowed' : 'pointer', opacity: idx === 0 ? 0.3 : 1 }} title="Move Up" aria-label="Move up">
                        <ArrowUp size={14} />
                      </button>
                      <button onClick={() => moveField(idx, 'down')} disabled={idx === draft.fields.length - 1} style={{ padding: '6px', border: '1.5px solid #0E1013', background: '#FFF', cursor: idx === draft.fields.length - 1 ? 'not-allowed' : 'pointer', opacity: idx === draft.fields.length - 1 ? 0.3 : 1 }} title="Move Down" aria-label="Move down">
                        <ArrowDown size={14} />
                      </button>
                      <button onClick={() => deleteField(idx)} className="btn-danger" style={{ padding: '6px 10px' }} title="Delete Field" aria-label="Delete field">
                        <Trash2 size={14} />
                      </button>
                    </div>
                  </div>

                  <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(160px, 1fr))', gap: '12px', alignItems: 'flex-end' }}>
                    <div>
                      <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Field Label</label>
                      <input className="input-field" style={{ padding: '8px 12px', fontSize: '14px' }} maxLength={200} value={f.label} onChange={(e) => updateField(idx, { label: e.target.value })} dir="auto" />
                    </div>
                    <div>
                      <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Field Type</label>
                      <select className="input-field" style={{ padding: '8px 12px', fontSize: '14px' }} value={f.type} disabled={f.stored} title={f.stored ? 'The type of a published field cannot change' : undefined} onChange={(e) => updateField(idx, { type: e.target.value as DraftField['type'] })}>
                        {(Object.keys(kFieldTypes) as (keyof typeof kFieldTypes)[]).map((t) => <option key={t} value={t}>{kTypeWords[t]}</option>)}
                      </select>
                    </div>
                    <div style={{ display: 'flex', alignItems: 'center', gap: '8px', paddingBottom: '10px' }}>
                      <label style={{ display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer' }}>
                        <input type="checkbox" checked={!f.optional} onChange={(e) => updateField(idx, { optional: !e.target.checked })} style={{ width: '18px', height: '18px', accentColor: '#0E1013' }} />
                        <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700 }}>Required</span>
                      </label>
                    </div>
                  </div>

                  {f.type === 'SELECT_SINGLE' && (
                    <div>
                      <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Options (comma-separated)</label>
                      <input
                        className="input-field"
                        style={{ padding: '8px 12px', fontSize: '13px' }}
                        placeholder="Option 1, Option 2, Option 3"
                        value={f.optionsText}
                        onChange={(e) => updateField(idx, { optionsText: e.target.value, options: parseOptions(e.target.value, f.options) })}
                        dir="auto"
                      />
                    </div>
                  )}
                </div>
              ))}
            </div>
          </div>
        </div>
      )}

      {shareModalOpen && draft?.id && (
        <div role="dialog" aria-modal="true" style={{ position: 'fixed', inset: 0, background: 'rgba(14,16,19,0.7)', backdropFilter: 'blur(4px)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1000, padding: '20px' }}>
          <div className="card" style={{ maxWidth: '560px', width: '100%', background: '#FFF', border: '3px solid #0E1013', boxShadow: '10px 10px 0px #0E1013', padding: '32px', position: 'relative' }}>
            <button aria-label="Close" onClick={() => setShareModalOpen(false)} style={{ position: 'absolute', top: '20px', right: '20px', background: 'transparent', border: 'none', cursor: 'pointer' }}>
              <X size={22} />
            </button>
            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '8px' }}>03 — Public Form</span>
            <h2 style={{ fontSize: '28px', fontWeight: 900, textTransform: 'uppercase', margin: '0 0 8px 0' }}>{draft.status === 'active' ? 'Form Published!' : 'Form Link'}</h2>
            <p style={{ fontSize: '14px', color: '#555', marginBottom: '24px', lineHeight: 1.5 }}>
              {draft.status === 'active'
                ? 'Your form is live and accessible at the public link below. Anyone with this link can fill out and submit responses directly.'
                : 'This form is not accepting responses. Set its status to Published and publish it to open the link.'}
            </p>
            <div style={{ display: 'flex', alignItems: 'center', background: '#F7F5F0', border: '2.5px solid #0E1013', padding: '12px 16px', gap: '12px', marginBottom: '20px', boxShadow: '4px 4px 0px #0E1013' }}>
              <input readOnly value={publicLink} style={{ border: 'none', background: 'transparent', fontFamily: 'IBM Plex Mono, monospace', fontSize: '14px', fontWeight: 600, width: '100%', outline: 'none', color: '#0E1013' }} />
              <button onClick={copyShareLink} className="btn-primary" style={{ padding: '8px 16px', fontSize: '12px', display: 'flex', alignItems: 'center', gap: '6px', whiteSpace: 'nowrap' }}>
                {copied ? <CheckCircle2 size={16} /> : <Copy size={16} />}
                {copied ? 'Copied!' : 'Copy Link'}
              </button>
            </div>
            <div style={{ display: 'flex', gap: '12px', justifyContent: 'flex-end' }}>
              <a href={publicLink} target="_blank" rel="noopener noreferrer" className="btn-outline" style={{ display: 'flex', alignItems: 'center', gap: '6px', textDecoration: 'none', fontSize: '13px', padding: '10px 18px' }}>
                <ExternalLink size={16} /> Open in New Tab
              </a>
              <button className="btn-primary" onClick={() => setShareModalOpen(false)} style={{ fontSize: '13px', padding: '10px 22px' }}>Done</button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
