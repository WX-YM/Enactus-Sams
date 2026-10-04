import { useEffect, useState } from 'react';
import { Download, Trash2 } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { api } from '../app/platform';
import type { FormDefinition, FormResponse, TimeCursor } from '../app/responses';
import { ErrorBanner, describe, formatDate, run, useLoad, useSession } from '../app/ui';
import { routeFormsList, routeResponsesDelete, routeResponsesExport, routeResponsesList } from '../api/hammer.generated';

export default function FormResponses() {
  const { confirm } = useConfirm();
  const { affords } = useSession();
  const forms = useLoad((signal) => api.call(routeFormsList, { signal }), []);
  const [formId, setFormId] = useState<string | null>(null);
  const [form, setForm] = useState<FormDefinition | null>(null);
  const [rows, setRows] = useState<FormResponse[]>([]);
  const [cursor, setCursor] = useState<TimeCursor | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (formId === null && forms.data && forms.data.forms.length > 0) setFormId(forms.data.forms[0].id);
  }, [forms.data, formId]);

  const load = async (id: string, after: TimeCursor | null) => {
    const result = await run((signal) => api.call(routeResponsesList, {
      params: { id },
      query: after === null ? {} : { after: after.after, at: String(after.at) },
      signal,
    }));
    if (!result.ok) { setError(describe(result.error)); return; }
    setError(null);
    setForm(result.value.form);
    setRows((prev) => (after === null ? [...result.value.responses] : [...prev, ...result.value.responses]));
    setCursor(result.value.next);
  };

  useEffect(() => {
    if (formId !== null) void load(formId, null);
  }, [formId]);

  const download = async () => {
    if (formId === null) return;
    const result = await run((signal) => api.call(routeResponsesExport, { params: { id: formId }, signal }));
    if (!result.ok) { setError(describe(result.error)); return; }
    // The server built the CSV (formula cells neutralised, UTF-8 BOM); this only
    // hands it to the browser as a file.
    const blob = new Blob([result.value.csv], { type: 'text/csv;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = `${(form?.title || 'responses').replace(/[^\p{L}\p{N} _-]+/gu, '').trim() || 'responses'}.csv`;
    link.click();
    URL.revokeObjectURL(url);
  };

  const answerText = (value: FormResponse['answers'][string] | undefined, fid: string): string => {
    if (value === undefined) return '';
    if (Array.isArray(value)) return value.join('; ');
    const field = form?.fields.find((f) => f.fid === fid);
    const option = field?.options.find((o) => o.value === value);
    return option ? option.label : String(value);
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'end', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">04 — Operations</span>
          <h1 className="heading-lg">Form Responses.</h1>
        </div>
        <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap' }}>
          <select className="input-field" value={formId ?? ''} onChange={(e) => setFormId(e.target.value)}>
            {(forms.data?.forms ?? []).map((f) => <option key={f.id} value={f.id}>{f.title} ({f.submission_count})</option>)}
          </select>
          {affords(routeResponsesExport) && (
            <button className="btn-outline" onClick={download} disabled={formId === null} style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
              <Download size={16} /> Export CSV
            </button>
          )}
        </div>
      </div>
      <ErrorBanner message={forms.error ?? error} />

      {form && (
        <div className="card" style={{ padding: 0, overflowX: 'auto' }}>
          <table className="table">
            <thead>
              <tr>
                <th>Received</th>
                {form.fields.map((f) => <th key={f.fid} dir="auto">{f.label}</th>)}
                <th />
              </tr>
            </thead>
            <tbody>
              {rows.map((row) => (
                <tr key={row.id}>
                  <td className="font-mono" style={{ fontSize: '12px', whiteSpace: 'nowrap' }}>{formatDate(row.submitted_at)}</td>
                  {form.fields.map((f) => <td key={f.fid} dir="auto" style={{ whiteSpace: 'pre-wrap' }}>{answerText(row.answers[f.fid], f.fid)}</td>)}
                  <td>
                    {affords(routeResponsesDelete) && <button
                      className="btn-danger"
                      aria-label="Delete response"
                      onClick={async () => {
                        const ok = await confirm({ title: 'Delete response', message: 'Delete this response permanently?', type: 'danger', confirmText: 'Delete' });
                        if (!ok || formId === null) return;
                        const result = await run((signal) => api.call(routeResponsesDelete, { params: { id: formId, response: row.id }, body: {}, signal }));
                        if (result.ok) setRows((prev) => prev.filter((r) => r.id !== row.id));
                        else setError(describe(result.error));
                      }}
                    >
                      <Trash2 size={14} />
                    </button>}
                  </td>
                </tr>
              ))}
              {rows.length === 0 && (
                <tr><td colSpan={form.fields.length + 2} style={{ textAlign: 'center', opacity: 0.6, padding: '32px' }}>No responses yet.</td></tr>
              )}
            </tbody>
          </table>
        </div>
      )}
      {cursor !== null && formId !== null && <button className="btn-outline" style={{ alignSelf: 'center' }} onClick={() => load(formId, cursor)}>Load more</button>}
    </div>
  );
}
