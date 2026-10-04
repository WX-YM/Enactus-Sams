import { useState } from 'react';
import { Plus, Trash2, ArrowUp, ArrowDown, ExternalLink } from 'lucide-react';
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
  TEXT_SHORT: 'Short text',
  TEXT_LONG: 'Long text',
  EMAIL: 'Email',
  PHONE: 'Phone',
  SELECT_SINGLE: 'Dropdown',
};

type DraftOption = { value: string; label: string; stored: boolean };
type DraftField = { fid: string; label: string; type: keyof typeof kFieldTypes; optional: boolean; options: DraftOption[]; stored: boolean };
type Draft = { id: string | null; version: number; title: string; status: FormStatus; closesAt: string; maxSubmissions: number; fields: DraftField[] };

const emptyDraft = (): Draft => ({
  id: null,
  version: 0,
  title: '',
  status: 'draft',
  closesAt: '',
  maxSubmissions: 0,
  fields: [{ fid: 'f1', label: 'Full name', type: 'TEXT_SHORT', optional: false, options: [], stored: false }],
});

function fromDefinition(form: FormDefinition): Draft {
  return {
    id: form.id,
    version: form.version,
    title: form.title,
    status: form.status,
    closesAt: form.closes_at === null ? '' : new Date(form.closes_at - new Date().getTimezoneOffset() * 60000).toISOString().slice(0, 16),
    maxSubmissions: form.max_submissions ?? 0,
    fields: form.fields.map((f) => ({
      fid: f.fid,
      label: f.label,
      type: (f.type in kFieldTypes ? f.type : 'TEXT_SHORT') as keyof typeof kFieldTypes,
      optional: f.optional,
      options: f.options.map((o) => ({ value: o.value, label: o.label, stored: true })),
      stored: true,
    })),
  };
}

// A stored option value is [A-Za-z0-9_-]; one derived from a label, unique in
// its field. Existing values are never changed: answers already point at them.
function valueFor(label: string, taken: readonly string[]): string {
  let base = label.toLowerCase().normalize('NFKD').replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 48);
  if (base === '') base = 'option';
  let value = base;
  for (let n = 2; taken.includes(value); n++) value = `${base}_${n}`;
  return value;
}

function nextFid(fields: readonly DraftField[]): string {
  const used = fields.map((f) => Number(f.fid.slice(1))).filter((n) => Number.isFinite(n));
  return `f${Math.max(0, ...used) + 1}`;
}

export default function FormMaker() {
  const { confirm } = useConfirm();
  const forms = useLoad((signal) => api.call(routeFormsList, { signal }), []);
  const [draft, setDraft] = useState<Draft | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  const open = async (id: string) => {
    setError(null);
    const result = await run((signal) => api.call(routeFormsPublic, { params: { id }, signal }));
    if (result.ok) setDraft(fromDefinition(result.value));
    else setError(describe(result.error));
  };

  const save = async () => {
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
        ...(f.type === 'SELECT_SINGLE' ? { options: f.options.map((o) => ({ value: o.value, label: o.label.trim() })) } : {}),
      })),
    };
    const result = draft.id === null
      ? await run((signal) => api.call(routeFormsCreate, { body, signal }))
      : await run((signal) => api.call(routeFormsUpdate, { params: { id: draft.id as string }, body: { version: draft.version, ...body }, signal }));
    setSaving(false);
    if (!result.ok) {
      const message = describe(result.error);
      setError(codeOf(result.error) === 'CONFLICT'
        ? 'This form already has responses, so fields and options cannot be removed or made required. Duplicate it instead.'
        : message);
      return;
    }
    forms.reload();
    await open(draft.id ?? (result.value as { id: string }).id);
  };

  const update = (patch: Partial<Draft>) => setDraft((d) => (d === null ? d : { ...d, ...patch }));
  const updateField = (index: number, patch: Partial<DraftField>) =>
    setDraft((d) => (d === null ? d : { ...d, fields: d.fields.map((f, i) => (i === index ? { ...f, ...patch } : f)) }));

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'end', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">04 — Operations</span>
          <h1 className="heading-lg">Form Maker.</h1>
        </div>
        <button className="btn-primary" onClick={() => { setDraft(emptyDraft()); setError(null); }}>+ New form</button>
      </div>
      <ErrorBanner message={forms.error ?? error} />

      <div style={{ display: 'grid', gridTemplateColumns: 'minmax(220px, 1fr) minmax(0, 2.5fr)', gap: '24px', alignItems: 'start' }}>
        <div className="card" style={{ display: 'grid', gap: '8px' }}>
          {(forms.data?.forms ?? []).map((f) => (
            <button key={f.id} className={draft?.id === f.id ? 'btn-primary' : 'btn-outline'} style={{ textAlign: 'left' }} onClick={() => open(f.id)}>
              <div style={{ fontWeight: 800 }} dir="auto">{f.title}</div>
              <div className="font-mono" style={{ fontSize: '11px' }}>{f.status.toUpperCase()} · {f.submission_count} responses</div>
            </button>
          ))}
          {!forms.loading && (forms.data?.forms.length ?? 0) === 0 && <p style={{ opacity: 0.6 }}>No forms yet.</p>}
        </div>

        {draft && (
          <div className="card" style={{ display: 'grid', gap: '20px' }}>
            <input className="input-field" style={{ fontSize: '22px', fontWeight: 800 }} placeholder="Form title" maxLength={200} value={draft.title} onChange={(e) => update({ title: e.target.value })} dir="auto" />
            <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap' }}>
              <label style={{ display: 'grid', gap: '4px' }}>
                <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>STATUS</span>
                <select className="input-field" value={draft.status} onChange={(e) => update({ status: e.target.value as FormStatus })}>
                  <option value="draft">Draft (hidden)</option>
                  <option value="active">Active (accepting responses)</option>
                  <option value="closed">Closed</option>
                </select>
              </label>
              <label style={{ display: 'grid', gap: '4px' }}>
                <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>CLOSES AT (optional)</span>
                <input className="input-field" type="datetime-local" value={draft.closesAt} onChange={(e) => update({ closesAt: e.target.value })} />
              </label>
              <label style={{ display: 'grid', gap: '4px' }}>
                <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700 }}>MAX RESPONSES (0 = no limit)</span>
                <input className="input-field" type="number" min={0} max={1000000} value={draft.maxSubmissions} onChange={(e) => update({ maxSubmissions: Math.max(0, Number(e.target.value) || 0) })} />
              </label>
            </div>

            {draft.fields.map((field, index) => (
              <div key={field.fid} style={{ border: '2px solid #0E1013', padding: '12px', display: 'grid', gap: '8px' }}>
                <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap' }}>
                  <input className="input-field" style={{ flex: 2, minWidth: '180px' }} placeholder="Question" maxLength={200} value={field.label} onChange={(e) => updateField(index, { label: e.target.value })} dir="auto" />
                  <select className="input-field" style={{ flex: 1, minWidth: '140px' }} value={field.type} disabled={field.stored} onChange={(e) => updateField(index, { type: e.target.value as DraftField['type'] })}>
                    {(Object.keys(kFieldTypes) as (keyof typeof kFieldTypes)[]).map((t) => <option key={t} value={t}>{kTypeWords[t]}</option>)}
                  </select>
                  <label style={{ display: 'flex', gap: '6px', alignItems: 'center' }}>
                    <input type="checkbox" checked={!field.optional} onChange={(e) => updateField(index, { optional: !e.target.checked })} /> Required
                  </label>
                  <button className="btn-outline" aria-label="Move up" onClick={() => index > 0 && update({ fields: draft.fields.map((f, i) => (i === index - 1 ? draft.fields[index] : i === index ? draft.fields[index - 1] : f)) })}><ArrowUp size={14} /></button>
                  <button className="btn-outline" aria-label="Move down" onClick={() => index < draft.fields.length - 1 && update({ fields: draft.fields.map((f, i) => (i === index + 1 ? draft.fields[index] : i === index ? draft.fields[index + 1] : f)) })}><ArrowDown size={14} /></button>
                  <button className="btn-danger" aria-label="Remove question" onClick={() => update({ fields: draft.fields.filter((_, i) => i !== index) })}><Trash2 size={14} /></button>
                </div>
                {field.type === 'SELECT_SINGLE' && (
                  <div style={{ display: 'grid', gap: '6px', paddingLeft: '12px' }}>
                    {field.options.map((option, oi) => (
                      <div key={option.value} style={{ display: 'flex', gap: '6px' }}>
                        <input className="input-field" style={{ flex: 1 }} maxLength={200} value={option.label} onChange={(e) => updateField(index, { options: field.options.map((o, j) => (j === oi ? { ...o, label: e.target.value } : o)) })} dir="auto" />
                        <button className="btn-danger" aria-label="Remove option" onClick={() => updateField(index, { options: field.options.filter((_, j) => j !== oi) })}><Trash2 size={14} /></button>
                      </div>
                    ))}
                    <button
                      className="btn-outline"
                      style={{ justifySelf: 'start' }}
                      onClick={() => {
                        const label = `Option ${field.options.length + 1}`;
                        updateField(index, { options: [...field.options, { value: valueFor(label, field.options.map((o) => o.value)), label, stored: false }] });
                      }}
                    >
                      + Option
                    </button>
                  </div>
                )}
              </div>
            ))}

            <ErrorBanner message={error} />
            <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap', alignItems: 'center' }}>
              <button className="btn-outline" onClick={() => update({ fields: [...draft.fields, { fid: nextFid(draft.fields), label: '', type: 'TEXT_SHORT', optional: true, options: [], stored: false }] })} style={{ display: 'flex', gap: '6px', alignItems: 'center' }}>
                <Plus size={16} /> Add question
              </button>
              {draft.id && draft.status === 'active' && (
                <a className="btn-outline" href={`/apply?form=${encodeURIComponent(draft.id)}`} target="_blank" rel="noopener noreferrer" style={{ display: 'flex', gap: '6px', alignItems: 'center' }}>
                  <ExternalLink size={16} /> Open public form
                </a>
              )}
              {draft.id && (
                <button
                  className="btn-danger"
                  onClick={async () => {
                    const ok = await confirm({ title: 'Delete form', message: `Delete “${draft.title}” and every response to it? Export the responses first if you need them.`, type: 'danger', confirmText: 'Delete' });
                    if (!ok) return;
                    const result = await run((signal) => api.call(routeFormsDelete, { params: { id: draft.id as string }, body: {}, signal }));
                    if (!result.ok) { setError(describe(result.error)); return; }
                    setDraft(null);
                    forms.reload();
                  }}
                >
                  Delete
                </button>
              )}
              <button className="btn-primary" style={{ marginLeft: 'auto' }} disabled={saving} onClick={save}>{saving ? 'Saving…' : draft.id ? 'Save changes' : 'Create form'}</button>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
