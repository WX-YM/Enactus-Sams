// One photo gallery (about, life, tafrah): its items, upload, removal and
// order. Shared by the Gallery page and the Content CMS tabs that show the
// photos of their own section.

import { useRef, useState } from 'react';
import type { ReactNode } from 'react';
import { Upload, Trash2, ArrowLeft, ArrowRight } from 'lucide-react';
import { useConfirm } from '../context/ConfirmContext';
import { api } from './platform';
import { imageUrl, kImageTypes, uploadImage } from './media';
import type { GalleryItem } from './responses';
import { describe, run, useLoad } from './ui';
import { routeGalleryAdd, routeGalleryList, routeGalleryRemove, routeGalleryReorder } from '../api/hammer.generated';

export type GalleryKind = 'about' | 'life' | 'tafrah';

export type Gallery = {
  readonly items: readonly GalleryItem[];
  readonly loading: boolean;
  readonly error: string | null;
  readonly uploading: boolean;
  readonly upload: (file: File, caption?: string) => Promise<boolean>;
  readonly remove: (item: GalleryItem) => Promise<void>;
  readonly move: (index: number, delta: number) => Promise<void>;
  readonly reload: () => void;
};

export function useGallery(kind: GalleryKind, enabled = true): Gallery {
  const { confirm } = useConfirm();
  const load = useLoad(
    async (signal) => (enabled ? api.call(routeGalleryList, { params: { kind }, signal }) : { ok: true as const, value: { items: [] } }),
    [kind, enabled],
  );
  const [error, setError] = useState<string | null>(null);
  const [uploading, setUploading] = useState(false);
  const items = load.data?.items ?? [];

  const upload = async (file: File, caption = '') => {
    setError(null);
    setUploading(true);
    const controller = new AbortController();
    const stored = await uploadImage(file, controller.signal);
    if (!stored.ok) {
      setUploading(false);
      setError(describe(stored.error));
      return false;
    }
    const added = await api.call(routeGalleryAdd, { params: { kind }, body: { media: stored.value.id, caption: caption.trim() }, signal: controller.signal });
    setUploading(false);
    if (!added.ok) setError(describe(added.error));
    load.reload();
    return added.ok;
  };

  const remove = async (item: GalleryItem) => {
    const ok = await confirm({ title: 'Remove Image?', message: 'Are you sure you want to remove this image from the website?', confirmText: 'Remove Image', cancelText: 'Cancel', type: 'danger' });
    if (!ok) return;
    const result = await run((signal) => api.call(routeGalleryRemove, { params: { kind, id: item.id }, body: {}, signal }));
    setError(result.ok ? null : describe(result.error));
    load.reload();
  };

  const move = async (index: number, delta: number) => {
    const order = items.map((item) => item.id);
    const target = index + delta;
    if (target < 0 || target >= order.length) return;
    [order[index], order[target]] = [order[target], order[index]];
    const result = await run((signal) => api.call(routeGalleryReorder, { params: { kind }, body: { order }, signal }));
    setError(result.ok ? null : describe(result.error));
    load.reload();
  };

  return { items, loading: load.loading, error: load.error ?? error, uploading, upload, remove, move, reload: load.reload };
}

// A hidden file input and the button that opens it.
export function UploadButton({ label, busy, onFile, style }: { label: string; busy: boolean; onFile: (file: File) => void; style?: React.CSSProperties }): ReactNode {
  const input = useRef<HTMLInputElement | null>(null);
  return (
    <>
      <input
        ref={input}
        type="file"
        accept={kImageTypes.join(',')}
        hidden
        onChange={(e) => {
          const file = e.target.files?.[0];
          e.target.value = '';
          if (file) onFile(file);
        }}
      />
      <button type="button" className="btn-primary" disabled={busy} onClick={() => input.current?.click()} style={{ display: 'flex', alignItems: 'center', gap: '6px', padding: '8px 14px', fontSize: '12px', ...style }}>
        <Upload size={14} /> {busy ? 'Uploading...' : label}
      </button>
    </>
  );
}

// The thumbnails strip the CMS tabs show under a section's fields.
export function PhotoStrip({ gallery }: { gallery: Gallery }): ReactNode {
  return (
    <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap' }}>
      {gallery.items.map((item, index) => (
        <div key={item.id} style={{ position: 'relative', width: '140px', height: '140px', border: '2px solid #0E1013', background: '#E8E5DF' }}>
          {item.image && <img src={imageUrl(item.image.src, 'thumb')} alt={item.caption} loading="lazy" style={{ width: '100%', height: '100%', objectFit: 'cover', display: 'block' }} />}
          <button type="button" aria-label="Remove photo" onClick={() => gallery.remove(item)} style={{ position: 'absolute', top: '6px', right: '6px', background: '#E53935', color: '#FFF', border: '1.5px solid #0E1013', padding: '4px', cursor: 'pointer', display: 'flex' }}>
            <Trash2 size={14} />
          </button>
          <div style={{ position: 'absolute', bottom: '6px', left: '6px', display: 'flex', gap: '4px' }}>
            <button type="button" aria-label="Move earlier" disabled={index === 0} onClick={() => gallery.move(index, -1)} style={{ background: '#FFF', border: '1.5px solid #0E1013', padding: '2px', cursor: 'pointer', display: 'flex', opacity: index === 0 ? 0.4 : 1 }}>
              <ArrowLeft size={12} />
            </button>
            <button type="button" aria-label="Move later" disabled={index === gallery.items.length - 1} onClick={() => gallery.move(index, 1)} style={{ background: '#FFF', border: '1.5px solid #0E1013', padding: '2px', cursor: 'pointer', display: 'flex', opacity: index === gallery.items.length - 1 ? 0.4 : 1 }}>
              <ArrowRight size={12} />
            </button>
          </div>
        </div>
      ))}
      {!gallery.loading && gallery.items.length === 0 && <p className="font-mono" style={{ fontSize: '12px', opacity: 0.6 }}>No photos yet.</p>}
    </div>
  );
}
