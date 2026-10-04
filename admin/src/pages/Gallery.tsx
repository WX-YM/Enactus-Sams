import { useState } from 'react';
import { Trash2, Copy, Check, Image as ImageIcon } from 'lucide-react';
import { imageUrl } from '../app/media';
import { UploadButton, useGallery } from '../app/gallery';
import type { GalleryKind } from '../app/gallery';
import type { GalleryItem } from '../app/responses';
import { ErrorBanner } from '../app/ui';
import { useConfirm } from '../context/ConfirmContext';

const kCategories: readonly { kind: GalleryKind; label: string; color: string }[] = [
  { kind: 'life', label: 'Life at Enactus', color: '#FFC629' },
  { kind: 'tafrah', label: 'Tafrah Project', color: '#60A5FA' },
  { kind: 'about', label: 'About Section', color: '#4ADE80' },
];

export default function Gallery() {
  const galleries = {
    life: useGallery('life'),
    tafrah: useGallery('tafrah'),
    about: useGallery('about'),
  };
  const [filter, setFilter] = useState<'all' | GalleryKind>('all');
  const [uploadCategory, setUploadCategory] = useState<GalleryKind>('life');
  const [copiedUrl, setCopiedUrl] = useState<string | null>(null);
  const { alert: showAlert } = useConfirm();

  const allItems = kCategories.flatMap((c) => galleries[c.kind].items.map((item) => ({ item, category: c })));
  const filteredItems = allItems.filter(({ category }) => filter === 'all' || category.kind === filter);
  const target = galleries[uploadCategory];
  const error = kCategories.map((c) => galleries[c.kind].error).find((e) => e !== null) ?? null;

  const copyUrl = (item: GalleryItem) => {
    if (!item.image) return;
    const url = window.location.origin + imageUrl(item.image.src, 'full');
    void navigator.clipboard.writeText(url);
    setCopiedUrl(item.id);
    setTimeout(() => setCopiedUrl(null), 2000);
  };

  return (
    <div className="fade-in" style={{ paddingBottom: '40px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', marginBottom: '32px', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">04 — Media</span>
          <h1 className="heading-lg">Site Gallery.</h1>
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>All pictures displayed across the public website. Upload new photos directly from your device.</p>
        </div>

        <div style={{ display: 'flex', gap: '12px', alignItems: 'center', background: '#FFF', padding: '12px 16px', border: '2.5px solid #0E1013', boxShadow: '4px 4px 0px #0E1013' }}>
          <div style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
            <span className="font-mono" style={{ fontSize: '10px', fontWeight: 700, textTransform: 'uppercase', opacity: 0.7 }}>Upload To:</span>
            <select value={uploadCategory} onChange={(e) => setUploadCategory(e.target.value as GalleryKind)} className="input-field" style={{ padding: '6px 10px', fontSize: '13px' }}>
              {kCategories.map((c) => <option key={c.kind} value={c.kind}>{c.label}</option>)}
            </select>
          </div>
          <UploadButton
            label="Upload Image"
            busy={target.uploading}
            style={{ marginTop: '16px', padding: '12px 20px', fontSize: '14px' }}
            onFile={async (file) => {
              const ok = await target.upload(file);
              const label = kCategories.find((c) => c.kind === uploadCategory)?.label;
              if (ok) void showAlert({ title: 'Image Uploaded', message: `Image uploaded and added to ${label}!`, type: 'primary' });
            }}
          />
        </div>
      </div>

      <div style={{ marginBottom: '16px' }}><ErrorBanner message={error} /></div>

      <div style={{ display: 'flex', gap: '8px', marginBottom: '24px', flexWrap: 'wrap' }}>
        {[{ key: 'all' as const, label: `All Pictures (${allItems.length})` }, ...kCategories.map((c) => ({ key: c.kind, label: `${c.label} (${galleries[c.kind].items.length})` }))].map((t) => (
          <button
            key={t.key}
            onClick={() => setFilter(t.key)}
            style={{ padding: '10px 18px', border: '2px solid #0E1013', background: filter === t.key ? '#FFC629' : '#FFF', fontFamily: 'IBM Plex Mono, monospace', fontSize: '13px', fontWeight: 700, textTransform: 'uppercase', boxShadow: filter === t.key ? '4px 4px 0px #0E1013' : 'none', cursor: 'pointer' }}
          >
            {t.label}
          </button>
        ))}
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(220px, 1fr))', gap: '20px' }}>
        {filteredItems.map(({ item, category }) => (
          <div key={item.id} className="card" style={{ padding: '12px', display: 'flex', flexDirection: 'column', gap: '10px', background: '#FFF' }}>
            <div style={{ position: 'relative', width: '100%', height: '180px', overflow: 'hidden', border: '2px solid #0E1013', background: '#E8E5DF' }}>
              {item.image && <img src={imageUrl(item.image.src, 'card')} alt={item.caption} loading="lazy" style={{ width: '100%', height: '100%', objectFit: 'cover' }} />}
              <span style={{ position: 'absolute', top: '8px', left: '8px', background: category.color, color: '#0E1013', padding: '2px 8px', border: '1.5px solid #0E1013', fontFamily: 'IBM Plex Mono, monospace', fontSize: '10px', fontWeight: 800, textTransform: 'uppercase', boxShadow: '2px 2px 0px #0E1013' }}>
                {category.label}
              </span>
            </div>
            <div style={{ display: 'flex', gap: '8px', marginTop: 'auto' }}>
              <button onClick={() => copyUrl(item)} className="btn-outline" style={{ flex: 1, padding: '8px 10px', fontSize: '11px', display: 'flex', alignItems: 'center', justifyContent: 'center', gap: '4px' }} title="Copy URL">
                {copiedUrl === item.id ? <Check size={14} color="#16A34A" /> : <Copy size={14} />}
                {copiedUrl === item.id ? 'Copied' : 'Copy URL'}
              </button>
              <button onClick={() => galleries[category.kind].remove(item)} className="btn-danger" style={{ padding: '8px 12px' }} title="Remove Image" aria-label="Remove image">
                <Trash2 size={14} />
              </button>
            </div>
          </div>
        ))}
      </div>

      {filteredItems.length === 0 && (
        <div className="card" style={{ textAlign: 'center', padding: '48px', background: '#FFF' }}>
          <ImageIcon size={48} style={{ opacity: 0.3, marginBottom: '16px' }} />
          <h3 style={{ margin: '0 0 8px 0', textTransform: 'uppercase' }}>No images found</h3>
          <p className="font-mono" style={{ opacity: 0.6, fontSize: '13px' }}>Upload an image using the button above to add to this category.</p>
        </div>
      )}
    </div>
  );
}
