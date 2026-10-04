import { useRef, useState } from 'react';
import { Upload } from 'lucide-react';
import { api } from '../app/platform';
import { imageUrl, kImageTypes, uploadImage } from '../app/media';
import type { ImageRef, SectionValue, StoredSection } from '../app/responses';
import { ErrorBanner, describe, run, useLoad } from '../app/ui';
import { routeSectionsList, routeSectionsPublish, sections } from '../api/hammer.generated';

// Every editable section and field comes from the descriptor the server's own
// build emitted (src/config/sections.h), so the editor offers exactly the
// fields the server accepts, with the same limits.
type SectionKey = keyof typeof sections;
type Spec = (typeof sections)[SectionKey];

const kTitles: Record<SectionKey, string> = {
  'home.hero': 'Hero',
  'home.about': 'About',
  'home.inside': 'Inside the club',
  'home.life': 'Life at Enactus',
  'home.tafrah': 'Tafrah',
  'home.join': 'Recruitment',
  'home.footer': 'Footer',
};

const kOrder: readonly SectionKey[] = ['home.hero', 'home.about', 'home.inside', 'home.life', 'home.tafrah', 'home.join', 'home.footer'];

export default function Content() {
  const stored = useLoad((signal) => api.call(routeSectionsList, { signal }), []);
  const [active, setActive] = useState<SectionKey>('home.hero');
  const [notice, setNotice] = useState<string | null>(null);
  const current = stored.data?.sections.find((s) => s.key === active) ?? null;

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px' }}>
      <div>
        <span className="heading-sm">05 — Site</span>
        <h1 className="heading-lg">Content CMS.</h1>
        <p style={{ opacity: 0.7, margin: 0 }}>Changes go live on the website as soon as you publish.</p>
      </div>
      <div style={{ display: 'flex', gap: '12px', flexWrap: 'wrap' }}>
        {kOrder.map((key) => (
          <button key={key} className={active === key ? 'btn-primary' : 'btn-outline'} onClick={() => { setActive(key); setNotice(null); }}>{kTitles[key]}</button>
        ))}
      </div>
      <ErrorBanner message={stored.error} />
      {notice && <p role="status" className="font-mono" style={{ margin: 0, fontWeight: 700 }}>{notice}</p>}
      {current && (
        <SectionEditor
          key={`${current.key}:${current.version}`}
          spec={sections[active]}
          stored={current}
          onEdited={() => setNotice(null)}
          onPublished={() => { setNotice(`${kTitles[active]} published.`); stored.reload(); }}
        />
      )}
    </div>
  );
}

function SectionEditor({ spec, stored, onEdited, onPublished }: { spec: Spec; stored: StoredSection; onEdited: () => void; onPublished: () => void }) {
  const [values, setValues] = useState<Record<string, SectionValue>>({ ...stored.data });
  const [images, setImages] = useState<Record<string, ImageRef | null>>({ ...stored.images });
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  const changedData: Record<string, SectionValue> = {};
  for (const field of spec.fields) {
    if (values[field.key] !== stored.data[field.key]) changedData[field.key] = values[field.key];
  }
  const changedImages: Record<string, string> = {};
  for (const slot of spec.images) {
    const next = images[slot.slot];
    if (next && next.id !== stored.images[slot.slot]?.id) changedImages[slot.slot] = next.id;
  }
  const dirty = Object.keys(changedData).length > 0 || Object.keys(changedImages).length > 0;

  const publish = async () => {
    setSaving(true);
    setError(null);
    const result = await run((signal) => api.call(routeSectionsPublish, {
      params: { key: spec.key },
      body: { version: stored.version, data: changedData, images: changedImages },
      signal,
    }));
    setSaving(false);
    if (!result.ok) {
      setError(describe(result.error));
      return;
    }
    onPublished();
  };

  return (
    <div className="card" style={{ display: 'grid', gap: '20px' }}>
      {spec.fields.map((field) => {
        const label = field.labels[0];
        const value = values[field.key];
        if (field.type === 'Bool') {
          return (
            <label key={field.key} style={{ display: 'flex', gap: '10px', alignItems: 'center', fontWeight: 700 }}>
              <input type="checkbox" checked={value === true} onChange={(e) => { setValues({ ...values, [field.key]: e.target.checked }); onEdited(); }} />
              {label}
            </label>
          );
        }
        const text = typeof value === 'string' ? value : value === undefined ? '' : String(value);
        const long = field.maxCodePoints > 160;
        return (
          <label key={field.key} style={{ display: 'grid', gap: '6px' }}>
            <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', letterSpacing: '0.08em' }}>
              {label}{field.required ? ' *' : ''} <span style={{ opacity: 0.5 }}>({[...text].length}/{field.maxCodePoints})</span>
            </span>
            {long ? (
              <textarea className="input-field" rows={3} value={text} dir="auto" onChange={(e) => { setValues({ ...values, [field.key]: e.target.value }); onEdited(); }} />
            ) : (
              <input className="input-field" type={field.type === 'Url' ? 'url' : 'text'} value={text} dir={field.type === 'Url' ? 'ltr' : 'auto'} onChange={(e) => { setValues({ ...values, [field.key]: e.target.value }); onEdited(); }} />
            )}
          </label>
        );
      })}

      {spec.images.map((slot) => (
        <ImageSlot
          key={slot.slot}
          label={slot.labels[0]}
          hint={`At least ${slot.minWidth}×${slot.minHeight}px`}
          value={images[slot.slot] ?? null}
          onChange={(next) => setImages({ ...images, [slot.slot]: next })}
          onError={setError}
        />
      ))}

      <ErrorBanner message={error} />
      <div style={{ display: 'flex', gap: '12px', alignItems: 'center' }}>
        <button className="btn-primary" disabled={!dirty || saving} onClick={publish}>{saving ? 'Publishing…' : 'Publish'}</button>
        {dirty && <button className="btn-outline" onClick={() => { setValues({ ...stored.data }); setImages({ ...stored.images }); }}>Discard changes</button>}
      </div>
    </div>
  );
}

function ImageSlot({ label, hint, value, onChange, onError }: {
  label: string;
  hint: string;
  value: ImageRef | null;
  onChange: (next: ImageRef) => void;
  onError: (message: string | null) => void;
}) {
  const input = useRef<HTMLInputElement | null>(null);
  const [busy, setBusy] = useState(false);
  return (
    <div style={{ display: 'grid', gap: '8px' }}>
      <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', letterSpacing: '0.08em' }}>{label}</span>
      {value && <img src={imageUrl(value.src, 'card')} alt="" style={{ maxWidth: '320px', border: '2px solid #0E1013' }} />}
      <input
        ref={input}
        type="file"
        hidden
        accept={kImageTypes.join(',')}
        onChange={async (e) => {
          const file = e.target.files?.[0];
          e.target.value = '';
          if (!file) return;
          setBusy(true);
          const stored = await uploadImage(file, new AbortController().signal);
          setBusy(false);
          if (!stored.ok) { onError(describe(stored.error)); return; }
          onError(null);
          onChange({ id: stored.value.id, src: `/media/site/${stored.value.id}` });
        }}
      />
      <button className="btn-outline" disabled={busy} onClick={() => input.current?.click()} style={{ justifySelf: 'start', display: 'flex', gap: '8px', alignItems: 'center' }}>
        <Upload size={16} /> {busy ? 'Uploading…' : 'Replace image'}
      </button>
      <span className="font-mono" style={{ fontSize: '11px', opacity: 0.6 }}>{hint}. JPEG or PNG.</span>
    </div>
  );
}
