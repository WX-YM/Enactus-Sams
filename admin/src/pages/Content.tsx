import { useEffect, useRef, useState } from 'react';
import type { ReactNode } from 'react';
import { Save, Upload, CheckCircle2, Image as ImageIcon, Type, Sparkles, Layers, MessageSquare, Globe, Users as UsersIcon, ArrowUp, ArrowDown } from 'lucide-react';
import { api } from '../app/platform';
import { imageUrl, kImageTypes, uploadImage } from '../app/media';
import { PhotoStrip, UploadButton, useGallery } from '../app/gallery';
import type { Gallery } from '../app/gallery';
import type { ImageRef, SectionValue, StoredSection, Team } from '../app/responses';
import { ErrorBanner, describe, run, useLoad, useSession } from '../app/ui';
import { routeGalleryAdd, routeSectionsList, routeSectionsPublish, routeTeamsList, routeTeamsReorder, routeTeamsUpdate, sections } from '../api/hammer.generated';

// The fields and their limits come from the descriptor the server's own build
// emitted (src/config/sections.h); this page only lays them out.
type SectionKey = keyof typeof sections;
type Values = Record<SectionKey, Record<string, SectionValue>>;
type TabId = 'hero' | 'about' | 'tafrah' | 'inside' | 'gallery' | 'join' | 'footer';

const kTabs: readonly { id: TabId; label: string; icon: ReactNode; sections: readonly SectionKey[] }[] = [
  { id: 'hero', label: '01 Hero Section', icon: <Sparkles size={16} />, sections: ['home.hero'] },
  { id: 'about', label: '02 About Section', icon: <Type size={16} />, sections: ['home.about'] },
  { id: 'tafrah', label: '03 Tafrah Project', icon: <Layers size={16} />, sections: ['home.tafrah'] },
  { id: 'inside', label: '04 Inside the Club', icon: <UsersIcon size={16} />, sections: ['home.inside'] },
  { id: 'gallery', label: '05 Media Gallery', icon: <ImageIcon size={16} />, sections: ['home.life'] },
  { id: 'join', label: '06 Join Us / Recruitment', icon: <MessageSquare size={16} />, sections: ['home.join'] },
  { id: 'footer', label: '07 Footer & Socials', icon: <Globe size={16} />, sections: ['home.footer'] },
];

const kAllSections = Object.keys(sections) as SectionKey[];
const kTitles: Record<SectionKey, string> = {
  'home.hero': 'Hero', 'home.about': 'About', 'home.tafrah': 'Tafrah', 'home.inside': 'Inside the Club',
  'home.life': 'Media Gallery', 'home.join': 'Join Us', 'home.footer': 'Footer',
};

const kicker = { fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' } as const;
const fieldLabel = { fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' } as const;
const smallLabel = { fontSize: '11px', display: 'block', marginBottom: '4px' } as const;
const subHeading = { fontSize: '13px', textTransform: 'uppercase', margin: 0 } as const;
const grid = (min: number) => ({ display: 'grid', gridTemplateColumns: `repeat(auto-fit, minmax(min(100%, ${min}px), 1fr))`, gap: '20px' }) as const;
const divider = { borderTop: '2px solid #0E1013', paddingTop: '20px' } as const;

function maxOf(section: SectionKey, key: string): number {
  return sections[section].fields.find((f) => f.key === key)?.maxCodePoints ?? 200;
}

export default function Content() {
  const { affords } = useSession();
  const stored = useLoad((signal) => api.call(routeSectionsList, { signal }), []);
  const [activeTab, setActiveTab] = useState<TabId>('hero');
  const [values, setValues] = useState<Values | null>(null);
  const [siteImage, setSiteImage] = useState<ImageRef | null>(null);
  const [saving, setSaving] = useState(false);
  const [saveSuccess, setSaveSuccess] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const canGallery = affords(routeGalleryAdd);
  const canTeams = affords(routeTeamsUpdate);
  const about = useGallery('about', canGallery);
  const tafrah = useGallery('tafrah', canGallery);
  const life = useGallery('life', canGallery);

  const storedOf = (key: SectionKey): StoredSection | undefined => stored.data?.sections.find((s) => s.key === key);

  // The editable copy is (re)taken from what the server holds after each load.
  useEffect(() => {
    if (!stored.data) return;
    const next = {} as Values;
    for (const key of kAllSections) next[key] = { ...(storedOf(key)?.data ?? {}) };
    setValues(next);
    setSiteImage(storedOf('home.tafrah')?.images.site ?? null);
  }, [stored.data]);

  const get = (section: SectionKey, key: string): SectionValue => values?.[section]?.[key] ?? '';
  const set = (section: SectionKey, key: string, value: SectionValue) => {
    setSaveSuccess(false);
    setValues((prev) => (prev === null ? prev : { ...prev, [section]: { ...prev[section], [key]: value } }));
  };

  const changesOf = (section: SectionKey) => {
    const before = storedOf(section);
    const data: Record<string, SectionValue> = {};
    for (const field of sections[section].fields) {
      const now = values?.[section]?.[field.key];
      if (now !== undefined && now !== before?.data[field.key]) data[field.key] = now;
    }
    const images: Record<string, string> = {};
    if (section === 'home.tafrah' && siteImage && siteImage.id !== before?.images.site?.id) images.site = siteImage.id;
    return { data, images, dirty: Object.keys(data).length > 0 || Object.keys(images).length > 0 };
  };

  const dirtySections = kAllSections.filter((s) => changesOf(s).dirty);

  const handleSave = async () => {
    setSaving(true);
    setError(null);
    const failures: string[] = [];
    for (const section of dirtySections) {
      const { data, images } = changesOf(section);
      const result = await run((signal) => api.call(routeSectionsPublish, {
        params: { key: section },
        body: { version: storedOf(section)?.version ?? 0, data, images },
        signal,
      }));
      if (!result.ok) failures.push(`${kTitles[section]}: ${describe(result.error)}`);
    }
    setSaving(false);
    stored.reload();
    if (failures.length > 0) { setError(failures.join(' ')); return; }
    setSaveSuccess(true);
    setTimeout(() => setSaveSuccess(false), 3000);
  };

  // A labelled text input or textarea bound to one section field.
  const F = ({ section, k, label, placeholder, rows, dir }: { section: SectionKey; k: string; label: string; placeholder?: string; rows?: number; dir?: 'ltr' }) => (
    <div>
      <label className="font-mono" style={fieldLabel}>{label}</label>
      {rows ? (
        <textarea className="input-field" rows={rows} maxLength={maxOf(section, k)} value={String(get(section, k))} placeholder={placeholder} dir="auto" onChange={(e) => set(section, k, e.target.value)} />
      ) : (
        <input className="input-field" maxLength={maxOf(section, k)} value={String(get(section, k))} placeholder={placeholder} dir={dir ?? 'auto'} onChange={(e) => set(section, k, e.target.value)} />
      )}
    </div>
  );

  const Header = ({ over, title }: { over: string; title: string }) => (
    <div>
      <span className="font-mono" style={kicker}>{over}</span>
      <h2 className="heading-sm" style={{ margin: 0 }}>{title}</h2>
    </div>
  );

  const Photos = ({ gallery, title, hint }: { gallery: Gallery; title: string; hint: string }) => (
    <div style={divider}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px', flexWrap: 'wrap', gap: '12px' }}>
        <div>
          <h3 className="font-mono" style={subHeading}>{title} ({gallery.items.length})</h3>
          <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: '4px 0 0 0' }}>{hint}</p>
        </div>
        {canGallery && <UploadButton label="Upload Photo" busy={gallery.uploading} onFile={(file) => void gallery.upload(file)} />}
      </div>
      {canGallery ? (
        <>
          <ErrorBanner message={gallery.error} />
          <PhotoStrip gallery={gallery} />
          <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: '8px 0 0 0' }}>Photos are saved as soon as they upload. JPEG or PNG, at least 200×200.</p>
        </>
      ) : (
        <p className="font-mono" style={{ fontSize: '12px', opacity: 0.6 }}>Changing these photos needs the Gallery permission.</p>
      )}
    </div>
  );

  return (
    <div className="fade-in" style={{ paddingBottom: '40px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', marginBottom: '28px', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">05 — Site CMS</span>
          <h1 className="heading-lg">Content CMS.</h1>
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>Site-wide content manager. Edit headlines, copy, stats, images, and settings for every section.</p>
        </div>
        <button className="btn-primary" style={{ display: 'flex', alignItems: 'center', gap: '8px', padding: '14px 28px' }} onClick={handleSave} disabled={saving || dirtySections.length === 0}>
          {saveSuccess ? <CheckCircle2 size={18} color="#0E1013" /> : <Save size={18} />}
          {saving ? 'Saving...' : saveSuccess ? 'Saved Live!' : `Save Changes${dirtySections.length > 0 ? ` (${dirtySections.length})` : ''}`}
        </button>
      </div>

      <div style={{ display: 'flex', flexWrap: 'wrap', gap: '8px', marginBottom: '28px', borderBottom: '3px solid #0E1013', paddingBottom: '16px' }}>
        {kTabs.map((tab) => {
          const on = activeTab === tab.id;
          const dirty = tab.sections.some((s) => dirtySections.includes(s));
          return (
            <button
              key={tab.id}
              onClick={() => setActiveTab(tab.id)}
              style={{ padding: '10px 18px', border: '2.5px solid #0E1013', background: on ? '#FFC629' : '#FFF', color: '#0E1013', fontFamily: 'IBM Plex Mono, monospace', fontSize: '12px', fontWeight: 700, textTransform: 'uppercase', display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer', boxShadow: on ? '4px 4px 0px #0E1013' : 'none', transform: on ? 'translate(-2px, -2px)' : 'none', transition: 'all 0.15s' }}
            >
              {tab.icon} {tab.label}{dirty ? ' •' : ''}
            </button>
          );
        })}
      </div>

      <div style={{ marginBottom: '16px' }}><ErrorBanner message={stored.error ?? error} /></div>

      {values === null ? (
        <div className="card font-mono" style={{ opacity: 0.6 }}>Loading content…</div>
      ) : (
        <>
          {activeTab === 'hero' && (
            <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
              {Header({ over: "Hero Header Configuration", title: "Hero Section" })}
              <div style={grid(280)}>
                {F({ section: 'home.hero', k: 'campus', label: 'Campus Location Tag', placeholder: 'Sadat Academy — Maadi' })}
                {F({ section: 'home.hero', k: 'chapter', label: 'Chapter Affiliation Tag', placeholder: 'Student chapter · Enactus Egypt' })}
              </div>
              <div style={grid(200)}>
                {F({ section: 'home.hero', k: 'tagline', label: 'Tagline Highlight', placeholder: 'Entrepreneurial action' })}
                {F({ section: 'home.hero', k: 'headline1', label: 'Headline Row 1', placeholder: 'We' })}
                {F({ section: 'home.hero', k: 'headline2', label: 'Headline Row 2', placeholder: 'Change' })}
                {F({ section: 'home.hero', k: 'headline3', label: 'Headline Row 3 (Accent)', placeholder: 'The World' })}
              </div>
              {F({ section: 'home.hero', k: 'subtitle', label: 'Hero Subtitle / Description', rows: 3 })}
              <div style={grid(240)}>
                {F({ section: 'home.hero', k: 'cta1', label: 'Primary Button CTA', placeholder: 'Join the next season' })}
                {F({ section: 'home.hero', k: 'cta2', label: 'Secondary Button CTA', placeholder: 'See Tafrah' })}
              </div>
              <div style={divider}>
                <h3 className="font-mono" style={{ ...subHeading, marginBottom: '16px' }}>Hero Stat Counters</h3>
                <div style={grid(220)}>
                  {[1, 2, 3].map((n) => (
                    <div key={n} style={{ padding: '16px', background: '#F7F5F0', border: '2px solid #0E1013' }}>
                      <label className="font-mono" style={smallLabel}>Stat {n} Number</label>
                      <input className="input-field" style={{ marginBottom: '10px' }} maxLength={maxOf('home.hero', `stat${n}_num`)} value={String(get('home.hero', `stat${n}_num`))} onChange={(e) => set('home.hero', `stat${n}_num`, e.target.value)} />
                      <label className="font-mono" style={smallLabel}>Stat {n} Label</label>
                      <input className="input-field" maxLength={maxOf('home.hero', `stat${n}_label`)} value={String(get('home.hero', `stat${n}_label`))} dir="auto" onChange={(e) => set('home.hero', `stat${n}_label`, e.target.value)} />
                    </div>
                  ))}
                </div>
              </div>
            </div>
          )}

          {activeTab === 'about' && (
            <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
              {Header({ over: "Who We Are", title: "About Section" })}
              <div style={grid(280)}>
                {F({ section: 'home.about', k: 'kicker', label: 'Section Kicker', placeholder: '01 — Who we are' })}
                {F({ section: 'home.about', k: 'heading', label: 'Section Heading' })}
              </div>
              {F({ section: 'home.about', k: 'p1', label: 'Paragraph 1', rows: 4 })}
              {F({ section: 'home.about', k: 'p2', label: 'Paragraph 2', rows: 3 })}
              {F({ section: 'home.about', k: 'tags', label: 'Tags / Pills (comma-separated)', placeholder: 'Social entrepreneurship, Field research, Pitching, Production' })}
              {Photos({ gallery: about, title: "About Section Photos", hint: "Photos displayed on the right side of the About section on the home page." })}
            </div>
          )}

          {activeTab === 'tafrah' && (
            <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
              {Header({ over: "Flagship Project", title: "Tafrah Section" })}
              <label style={{ display: 'flex', alignItems: 'center', gap: '10px', fontWeight: 700 }}>
                <input type="checkbox" checked={get('home.tafrah', 'visible') === true} onChange={(e) => set('home.tafrah', 'visible', e.target.checked)} style={{ width: '18px', height: '18px', accentColor: '#0E1013' }} />
                Show the Tafrah section on the website
              </label>
              <div style={grid(240)}>
                {F({ section: 'home.tafrah', k: 'kicker', label: 'Section Kicker' })}
                {F({ section: 'home.tafrah', k: 'title', label: 'Project Title / Name' })}
                {F({ section: 'home.tafrah', k: 'tagline', label: 'Arabic Tagline' })}
              </div>
              {F({ section: 'home.tafrah', k: 'desc', label: 'Project Main Description', rows: 3 })}
              <div style={divider}>
                <h3 className="font-mono" style={{ ...subHeading, marginBottom: '16px' }}>4 Feature Cards</h3>
                <div style={grid(200)}>
                  {[1, 2, 3, 4].map((n) => (
                    <div key={n} style={{ padding: '12px', background: '#F7F5F0', border: '2px solid #0E1013', display: 'flex', flexDirection: 'column', gap: '8px' }}>
                      <span className="font-mono" style={{ fontSize: '11px', color: '#6D5E2C' }}>Card 0{n}</span>
                      <input className="input-field" maxLength={maxOf('home.tafrah', `h${n}_title`)} value={String(get('home.tafrah', `h${n}_title`))} dir="auto" onChange={(e) => set('home.tafrah', `h${n}_title`, e.target.value)} />
                      <textarea className="input-field" rows={3} maxLength={maxOf('home.tafrah', `h${n}_desc`)} value={String(get('home.tafrah', `h${n}_desc`))} dir="auto" onChange={(e) => set('home.tafrah', `h${n}_desc`, e.target.value)} />
                    </div>
                  ))}
                </div>
              </div>
              {F({ section: 'home.tafrah', k: 'footer', label: 'Tafrah Bottom Note' })}
              <SiteImage value={siteImage} onChange={(next) => { setSaveSuccess(false); setSiteImage(next); }} onError={setError} />
              {Photos({ gallery: tafrah, title: "Tafrah Behind-the-Scenes Photos", hint: "Photos shown in the Tafrah project strip." })}
            </div>
          )}

          {activeTab === 'inside' && (
            <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
              {Header({ over: "Teams & Structure", title: "03 — Inside the Club Section" })}
              <div style={grid(280)}>
                {F({ section: 'home.inside', k: 'kicker', label: 'Section Kicker' })}
                {F({ section: 'home.inside', k: 'title', label: 'Section Title' })}
              </div>
              {F({ section: 'home.inside', k: 'desc', label: 'Section Description / Subtitle', rows: 3 })}
              <TeamToggles flag="showcase" enabled={canTeams} title="Active Teams Displayed in This Section" hint="Tick the teams shown in Inside the Club, and set their order. Team names and descriptions are edited in Manage Teams." />
            </div>
          )}

          {activeTab === 'gallery' && (
            <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
              {Header({ over: "Campus Life", title: "Life at Enactus Gallery" })}
              <div style={grid(240)}>
                {F({ section: 'home.life', k: 'kicker', label: 'Gallery Kicker' })}
                {F({ section: 'home.life', k: 'title', label: 'Gallery Main Title' })}
                {F({ section: 'home.life', k: 'subtitle', label: 'Subtitle / Hint' })}
              </div>
              {Photos({ gallery: life, title: "Uploaded Gallery Photos", hint: "Images are automatically optimized and compressed for fast website loading." })}
            </div>
          )}

          {activeTab === 'join' && (
            <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
              {Header({ over: "Recruitment & Applications", title: "Join Us Section" })}
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: '16px', padding: '20px', border: '2.5px solid #0E1013', background: '#F7F5F0', flexWrap: 'wrap' }}>
                <div>
                  <h3 style={{ margin: 0, fontSize: '20px', fontWeight: 900 }}>Recruitment Status</h3>
                  <p style={{ margin: '4px 0 0 0', fontSize: '14px', color: '#555' }}>Turn candidate applications on or off. When closed, the public form is replaced with a closed notice. Press Save Changes to apply.</p>
                </div>
                <button className="btn-primary" style={{ background: get('home.join', 'open') === true ? '#4CAF50' : '#E53935', color: '#FFF', padding: '12px 28px', fontSize: '16px' }} onClick={() => set('home.join', 'open', get('home.join', 'open') !== true)}>
                  {get('home.join', 'open') === true ? 'OPEN' : 'CLOSED'}
                </button>
              </div>
              <div style={grid(240)}>
                {F({ section: 'home.join', k: 'title', label: 'Join Section Title' })}
                {F({ section: 'home.join', k: 'kicker_open', label: 'Kicker (When Open)' })}
                {F({ section: 'home.join', k: 'kicker_closed', label: 'Kicker (When Closed)' })}
              </div>
              {F({ section: 'home.join', k: 'desc', label: 'Join Section Description', rows: 3 })}
              <div style={grid(240)}>
                {F({ section: 'home.join', k: 'cta_open', label: 'CTA Button Text (When Open)' })}
                {F({ section: 'home.join', k: 'cta_closed', label: 'CTA Button Text (When Closed)' })}
              </div>
              <div style={grid(240)}>
                {F({ section: 'home.join', k: 'note_open', label: 'Notice Note (When Open)' })}
                {F({ section: 'home.join', k: 'note_closed', label: 'Notice Note (When Closed)' })}
              </div>
              <div style={divider}>
                <h3 className="font-mono" style={{ ...subHeading, marginBottom: '16px' }}>Closed Recruitment Notice Box</h3>
                <div style={grid(280)}>
                  {F({ section: 'home.join', k: 'closed_title', label: 'Box Title' })}
                  {F({ section: 'home.join', k: 'closed_desc', label: 'Box Message Body', rows: 3 })}
                </div>
              </div>
              <TeamToggles flag="recruiting" enabled={canTeams} title="Recruitment Team Choices" hint="Tick the teams applicants can choose on the application form. Add or rename teams in Manage Teams." />
            </div>
          )}

          {activeTab === 'footer' && (
            <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
              {Header({ over: "Links & Copyright", title: "Footer & Social Links" })}
              <div style={grid(280)}>
                {F({ section: 'home.footer', k: 'address_line1', label: 'Chapter Address & Campus' })}
                {F({ section: 'home.footer', k: 'address_line2', label: 'Address, Second Line' })}
              </div>
              <div style={grid(220)}>
                {F({ section: 'home.footer', k: 'instagram', label: 'Instagram Profile URL', dir: 'ltr' })}
                {F({ section: 'home.footer', k: 'facebook', label: 'Facebook Page URL', dir: 'ltr' })}
                {F({ section: 'home.footer', k: 'tiktok', label: 'TikTok Profile URL', dir: 'ltr' })}
                {F({ section: 'home.footer', k: 'linkedin', label: 'LinkedIn Page URL', dir: 'ltr' })}
              </div>
              {F({ section: 'home.footer', k: 'note', label: 'Footer Disclaimer Note' })}
            </div>
          )}
        </>
      )}
    </div>
  );
}

// The Tafrah platform screenshot: a fixed section image slot. The upload is
// stored at once; the slot points at it when the section is saved.
function SiteImage({ value, onChange, onError }: { value: ImageRef | null; onChange: (next: ImageRef) => void; onError: (message: string | null) => void }) {
  const input = useRef<HTMLInputElement | null>(null);
  const [busy, setBusy] = useState(false);
  return (
    <div style={divider}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px', flexWrap: 'wrap', gap: '12px' }}>
        <div>
          <h3 className="font-mono" style={subHeading}>Tafrah Platform Showcase Photo</h3>
          <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: '4px 0 0 0' }}>Main large image displayed beside the 4 feature cards. At least 320×200. Press Save Changes to apply.</p>
        </div>
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
        <button type="button" className="btn-primary" disabled={busy} onClick={() => input.current?.click()} style={{ display: 'flex', alignItems: 'center', gap: '6px', fontSize: '12px', padding: '8px 14px' }}>
          <Upload size={14} /> {busy ? 'Uploading...' : 'Replace Photo'}
        </button>
      </div>
      {value ? (
        <img src={imageUrl(value.src, 'card')} alt="Tafrah platform" style={{ maxWidth: '320px', width: '100%', border: '2px solid #0E1013', display: 'block' }} />
      ) : (
        <p className="font-mono" style={{ fontSize: '12px', opacity: 0.6 }}>No photo set.</p>
      )}
    </div>
  );
}

// Which teams carry a flag (shown in Inside the Club, or offered on the
// application form), and the team order. These save immediately.
function TeamToggles({ flag, enabled, title, hint }: { flag: 'showcase' | 'recruiting'; enabled: boolean; title: string; hint: string }) {
  const teams = useLoad(async (signal) => api.call(routeTeamsList, { signal }), []);
  const [error, setError] = useState<string | null>(null);
  const list: readonly Team[] = teams.data?.teams ?? [];

  const toggle = async (team: Team, on: boolean) => {
    const result = await run((signal) => api.call(routeTeamsUpdate, { params: { id: team.id }, body: { version: team.version, [flag]: on }, signal }));
    setError(result.ok ? null : describe(result.error));
    teams.reload();
  };

  const move = async (index: number, delta: number) => {
    const order = list.map((t) => t.id);
    const target = index + delta;
    if (target < 0 || target >= order.length) return;
    [order[index], order[target]] = [order[target], order[index]];
    const result = await run((signal) => api.call(routeTeamsReorder, { body: { order }, signal }));
    setError(result.ok ? null : describe(result.error));
    teams.reload();
  };

  const count = list.filter((t) => t[flag]).length;
  return (
    <div style={divider}>
      <h3 className="font-mono" style={{ ...subHeading, marginBottom: '6px' }}>{title} ({count})</h3>
      <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: '0 0 16px 0' }}>{enabled ? `${hint} Changes here save immediately.` : 'Changing this needs the Manage Teams permission.'}</p>
      <ErrorBanner message={teams.error ?? error} />
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(min(100%, 260px), 1fr))', gap: '12px' }}>
        {list.map((t, idx) => (
          <div key={t.id} style={{ background: t[flag] ? '#0E1013' : '#F7F5F0', color: t[flag] ? '#F7F5F0' : '#0E1013', border: '2.5px solid #0E1013', padding: '14px 16px', display: 'flex', alignItems: 'center', gap: '10px' }}>
            <input type="checkbox" aria-label={`Include ${t.name}`} disabled={!enabled} checked={t[flag]} onChange={(e) => toggle(t, e.target.checked)} style={{ width: '18px', height: '18px', accentColor: '#FFC629' }} />
            <div style={{ flex: 1, minWidth: 0 }}>
              <span className="font-mono" style={{ fontSize: '11px', color: t[flag] ? '#FFC629' : '#6D5E2C', fontWeight: 700 }}>{String(idx + 1).padStart(2, '0')} / {t.name}</span>
              <div style={{ fontSize: '12px', opacity: 0.8, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} dir="auto">{t.desc || 'No description'}</div>
            </div>
            {enabled && (
              <div style={{ display: 'flex', flexDirection: 'column', gap: '2px' }}>
                <button type="button" aria-label="Move earlier" disabled={idx === 0} onClick={() => move(idx, -1)} style={{ background: '#FFF', border: '1.5px solid #0E1013', padding: '2px', cursor: 'pointer', display: 'flex', opacity: idx === 0 ? 0.4 : 1 }}><ArrowUp size={12} /></button>
                <button type="button" aria-label="Move later" disabled={idx === list.length - 1} onClick={() => move(idx, 1)} style={{ background: '#FFF', border: '1.5px solid #0E1013', padding: '2px', cursor: 'pointer', display: 'flex', opacity: idx === list.length - 1 ? 0.4 : 1 }}><ArrowDown size={12} /></button>
              </div>
            )}
          </div>
        ))}
      </div>
    </div>
  );
}
