import { useEffect, useState } from 'react';
import { api } from '../app/platform';
import type { AuditRow, TimeCursor } from '../app/responses';
import { ErrorBanner, describe, formatDate, run } from '../app/ui';
import { routeAuditList } from '../api/hammer.generated';

// The audit log anvil writes for every staff change and every refused request,
// newest first. Addresses arrive already coarsened to their network.
export default function Audit() {
  const [rows, setRows] = useState<AuditRow[]>([]);
  const [cursor, setCursor] = useState<TimeCursor | null>(null);
  const [error, setError] = useState<string | null>(null);

  const load = async (after: TimeCursor | null) => {
    const result = await run((signal) => api.call(routeAuditList, {
      query: after === null ? {} : { after: after.after, at: String(after.at) },
      signal,
    }));
    if (!result.ok) { setError(describe(result.error)); return; }
    setError(null);
    setRows((prev) => (after === null ? [...result.value.rows] : [...prev, ...result.value.rows]));
    setCursor(result.value.next);
  };

  useEffect(() => { void load(null); }, []);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px' }}>
      <div>
        <span className="heading-sm">07 — Administration</span>
        <h1 className="heading-lg">Audit Log.</h1>
      </div>
      <ErrorBanner message={error} />
      <div className="card" style={{ padding: 0, overflowX: 'auto' }}>
        <table className="table">
          <thead><tr><th>When</th><th>Action</th><th>By</th><th>Result</th><th>Network</th></tr></thead>
          <tbody>
            {rows.map((row) => (
              <tr key={row.id}>
                <td className="font-mono" style={{ fontSize: '12px', whiteSpace: 'nowrap' }}>{formatDate(row.at)}</td>
                <td className="font-mono" style={{ fontSize: '12px' }}>{row.action || 'unknown'}{row.repeats > 1 ? ` ×${row.repeats}` : ''}</td>
                <td style={{ fontSize: '12px' }}>{row.actor ? (row.actor.name || row.actor.id) : '—'}</td>
                <td className="font-mono" style={{ fontSize: '12px' }}>{row.succeeded ? 'OK' : row.code}</td>
                <td className="font-mono" style={{ fontSize: '12px' }}>{row.network || '—'}</td>
              </tr>
            ))}
            {rows.length === 0 && <tr><td colSpan={5} style={{ textAlign: 'center', opacity: 0.6, padding: '32px' }}>Nothing logged yet.</td></tr>}
          </tbody>
        </table>
      </div>
      {cursor !== null && <button className="btn-outline" style={{ alignSelf: 'center' }} onClick={() => load(cursor)}>Load more</button>}
    </div>
  );
}
