import { useRef, useState } from 'react';
import { Upload, Trash2, ArrowUp, ArrowDown } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { api } from '../app/platform';
import { imageUrl, kImageTypes, uploadImage } from '../app/media';
import type { CallResult } from '../app/ui';
import { ErrorBanner, describe, run, useLoad } from '../app/ui';
import { routeGalleryAdd, routeGalleryList, routeGalleryRemove, routeGalleryReorder } from '../api/hammer.generated';

const kGalleries = [
  { kind: 'about', label: 'About section' },
  { kind: 'life', label: 'Life at Enactus' },
  { kind: 'tafrah', label: 'Tafrah project' },
] as const;

type Kind = (typeof kGalleries)[number]['kind'];

export default function Gallery() {
  const { confirm } = useConfirm();
  const [kind, setKind] = useState<Kind>('life');
  const items = useLoad((signal) => api.call(routeGalleryList, { params: { kind }, signal }), [kind]);
  const [error, setError] = useState<string | null>(null);
  const [uploading, setUploading] = useState(false);
  const [caption, setCaption] = useState('');
  const fileInput = useRef<HTMLInputElement | null>(null);
  const list = items.data?.items ?? [];

  const check = (result: CallResult<unknown>) => {
    setError(result.ok ? null : describe(result.error));
    items.reload();
  };

  const upload = async (file: File) => {
    setError(null);
    setUploading(true);
    const controller = new AbortController();
    const stored = await uploadImage(file, controller.signal);
    if (!stored.ok) {
      setUploading(false);
      setError(describe(stored.error));
      return;
    }
    const added = await api.call(routeGalleryAdd, { params: { kind }, body: { media: stored.value.id, caption: caption.trim() }, signal: controller.signal });
    setUploading(false);
    setCaption('');
    check(added);
  };

  const move = async (index: number, delta: number) => {
    const order = list.map((item) => item.id);
    const target = index + delta;
    if (target < 0 || target >= order.length) return;
    [order[index], order[target]] = [order[target], order[index]];
    check(await run((signal) => api.call(routeGalleryReorder, { params: { kind }, body: { order }, signal })));
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px' }}>
      <div>
        <span className="heading-sm">05 — Site</span>
        <h1 className="heading-lg">Gallery.</h1>
      </div>

      <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap' }}>
        {kGalleries.map((g) => (
          <button key={g.kind} className={kind === g.kind ? 'btn-primary' : 'btn-outline'} onClick={() => setKind(g.kind)}>{g.label}</button>
        ))}
      </div>

      <ErrorBanner message={items.error ?? error} />

      <div className="card" style={{ display: 'flex', gap: '12px', flexWrap: 'wrap', alignItems: 'center' }}>
        <input className="input-field" style={{ flex: 1, minWidth: '200px' }} placeholder="Caption for screen readers (optional)" maxLength={200} value={caption} onChange={(e) => setCaption(e.target.value)} />
        <input
          ref={fileInput}
          type="file"
          accept={kImageTypes.join(',')}
          hidden
          onChange={(e) => {
            const file = e.target.files?.[0];
            e.target.value = '';
            if (file) void upload(file);
          }}
        />
        <button className="btn-primary" disabled={uploading} onClick={() => fileInput.current?.click()} style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
          <Upload size={16} /> {uploading ? 'Uploading…' : 'Upload photo'}
        </button>
        <p className="font-mono" style={{ width: '100%', fontSize: '11px', opacity: 0.6, margin: 0 }}>JPEG or PNG, at least 200×200, up to 25 MB. Photos are re-encoded and their metadata (including location) removed.</p>
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(220px, 1fr))', gap: '24px' }}>
        {list.map((item, index) => (
          <div key={item.id} className="card" style={{ padding: 0, overflow: 'hidden', border: '2px solid #0E1013', display: 'flex', flexDirection: 'column' }}>
            {item.image ? (
              <img src={imageUrl(item.image.src, 'thumb')} alt={item.caption} loading="lazy" style={{ width: '100%', height: '160px', objectFit: 'cover' }} />
            ) : (
              <div style={{ height: '160px', background: '#E8E5DF' }} />
            )}
            <div style={{ padding: '8px', display: 'flex', gap: '6px', alignItems: 'center' }}>
              <span style={{ flex: 1, fontSize: '12px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} dir="auto">{item.caption || '—'}</span>
              <button className="btn-outline" aria-label="Move earlier" onClick={() => move(index, -1)}><ArrowUp size={14} /></button>
              <button className="btn-outline" aria-label="Move later" onClick={() => move(index, 1)}><ArrowDown size={14} /></button>
              <button
                className="btn-danger"
                aria-label="Remove photo"
                onClick={async () => {
                  const ok = await confirm({ title: 'Remove photo', message: 'Remove this photo from the gallery?', type: 'danger', confirmText: 'Remove' });
                  if (ok) check(await run((signal) => api.call(routeGalleryRemove, { params: { kind, id: item.id }, body: {}, signal })));
                }}
              >
                <Trash2 size={14} />
              </button>
            </div>
          </div>
        ))}
      </div>
      {!items.loading && list.length === 0 && <p style={{ opacity: 0.6 }}>No photos in this gallery.</p>}
    </div>
  );
}
