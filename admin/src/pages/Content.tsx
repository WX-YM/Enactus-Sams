import { useState, useEffect, useRef } from 'react';
import { Save, Upload, Trash2, CheckCircle2, Image as ImageIcon, Type, Sparkles, Layers, MessageSquare, Globe, Users as UsersIcon, Edit2, Check, X } from 'lucide-react';

export default function Content() {
  const [activeTab, setActiveTab] = useState<'hero' | 'about' | 'tafrah' | 'inside' | 'gallery' | 'join' | 'footer'>('hero');
  const [teams, setTeams] = useState<any[]>([]);
  const [newTeamName, setNewTeamName] = useState('');
  const [newTeamDesc, setNewTeamDesc] = useState('');
  const [addingTeam, setAddingTeam] = useState(false);
  const [editingTeamId, setEditingTeamId] = useState<string | null>(null);
  const [editTeamName, setEditTeamName] = useState('');
  const [editTeamDesc, setEditTeamDesc] = useState('');
  const [updatingTeam, setUpdatingTeam] = useState(false);
  const [content, setContent] = useState<any>({
    recruitmentOpen: true,
    // Hero
    heroCampus: 'Sadat Academy — Maadi',
    heroChapter: 'Student chapter · Enactus Egypt',
    heroTagline: 'Entrepreneurial action',
    heroHeadline1: 'We',
    heroHeadline2: 'Change',
    heroHeadline3: 'The World',
    heroSubtitle: 'A student team building real ventures for real problems — six teams, one project a year, and a campus that hears about all of it.',
    heroCta1: 'Join the next season',
    heroCta2: 'See Tafrah',
    heroStat1Num: '06',
    heroStat1Label: 'Teams',
    heroStat2Num: '01',
    heroStat2Label: 'Project at nationals',
    heroStat3Num: '∞',
    heroStat3Label: 'Reasons to start',
    // About
    aboutKicker: '01 — Who we are',
    aboutHeading: 'A club that builds businesses to solve problems.',
    aboutP1: 'Enactus is a global network of students, academics and business leaders. Our chapter at Sadat Academy — Maadi runs the full cycle ourselves: we find a problem in our community, design a venture that answers it, build it, measure it, and defend it in front of national judges.',
    aboutP2: 'Nobody here is only a member. You write, you pitch, you film, you negotiate, you manage a budget — usually in the same week.',
    aboutTags: 'Social entrepreneurship, Field research, Pitching, Production',
    aboutImages: [
      { url: 'assets/bench.jpg' },
      { url: 'assets/certificate.jpg' },
      { url: 'assets/glasses.jpg' }
    ],
    // Tafrah
    tafrahKicker: "02 — This year's project",
    tafrahTitle: 'Tafrah',
    tafrahTagline: 'طفـــرة — the leap',
    tafrahDesc: 'A training and employment platform built for autistic people in Egypt: a calm working environment, direct instruction, and real job opportunities that protect their rights.',
    tafrahSiteImage: 'assets/tafrah-site.jpg',
    tafrahH1Title: 'Courses',
    tafrahH1Desc: 'Skill tracks written in plain, literal language with predictable structure and no sensory noise.',
    tafrahH2Title: 'Assistant',
    tafrahH2Desc: 'Step-by-step guidance through every task, so nobody is left guessing what happens next.',
    tafrahH3Title: 'Jobs',
    tafrahH3Desc: 'Vetted employers, clear expectations, and roles matched to how each person actually works best.',
    tafrahH4Title: 'Dashboard',
    tafrahH4Desc: 'Progress, certificates and readiness in one view — for the trainee and for the employer.',
    tafrahFooter: 'Tafrah was the venture Enactus SAMS Maadi carried to the Enactus Egypt national competition this year.',
    tafrahImages: [],
    // Inside the club
    insideKicker: '03 — Inside the club',
    insideTitle: 'Six teams. One project.',
    insideDesc: 'Every team owns a real part of the outcome. You pick where you start — not where you stay.',
    // Gallery
    lifeKicker: '04 — Life at Enactus',
    lifeTitle: 'Long days, yellow everywhere.',
    lifeSubtitle: 'Tap any photo to enlarge',
    mediaGallery: [],
    // Join
    joinKickerOpen: 'Recruitment is open',
    joinKickerClosed: 'Recruitment',
    joinTitle: 'Join the next season.',
    joinDesc: 'Pick the team you want to start in, tell us why, and come to the interview. No experience required — only the willingness to do the work.',
    joinCtaOpen: 'Apply now',
    joinCtaClosed: 'Join the waitlist',
    joinNoteOpen: 'Applications close at the end of the month',
    joinNoteClosed: 'We open applications at the start of each semester',
    closedBannerTitle: 'Applications Closed.',
    closedBannerDesc: 'Recruitment for this semester has ended. Follow our socials to know when the next season begins!',
    // Footer
    footerAbout: 'Sadat Academy for Management Sciences — Maadi\nCairo, Egypt',
    footerSocialInsta: 'https://instagram.com',
    footerSocialFb: 'https://facebook.com',
    footerSocialTiktok: 'https://tiktok.com/@...',
    footerNote: 'Enactus SAMS Maadi is a student chapter of the global Enactus network.'
  });

  const [saving, setSaving] = useState(false);
  const [saveSuccess, setSaveSuccess] = useState(false);
  const [uploadingTarget, setUploadingTarget] = useState<'media' | 'tafrah' | 'about' | 'tafrahSite' | null>(null);

  const mediaInputRef = useRef<HTMLInputElement | null>(null);
  const tafrahInputRef = useRef<HTMLInputElement | null>(null);
  const aboutInputRef = useRef<HTMLInputElement | null>(null);
  const tafrahSiteInputRef = useRef<HTMLInputElement | null>(null);

  const fetchTeams = () => {
    fetch('/api/teams')
      .then(res => res.json())
      .then(data => {
        if (data && data.teams) {
          setTeams(data.teams.map((t: any) => ({ ...t, id: t._id?.$oid || t.id })));
        }
      })
      .catch(console.error);
  };

  useEffect(() => {
    fetchTeams();
    fetch('/api/content')
      .then(res => res.json())
      .then(data => {
        if (data.content) {
          const c = data.content;
          if (!c.aboutImages || c.aboutImages.length === 0) {
            c.aboutImages = [
              { url: 'assets/bench.jpg' },
              { url: 'assets/certificate.jpg' },
              { url: 'assets/glasses.jpg' }
            ];
          }
          if (!c.tafrahSiteImage) {
            c.tafrahSiteImage = 'assets/tafrah-site.jpg';
          }
          setContent((prev: any) => ({ ...prev, ...c }));
        }
      })
      .catch(console.error);
  }, []);

  const handleAddTeam = () => {
    if (!newTeamName.trim()) return;
    setAddingTeam(true);
    fetch('/api/teams', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: newTeamName.trim(), desc: newTeamDesc.trim() })
    })
    .then(r => r.json())
    .then(() => {
      setAddingTeam(false);
      setNewTeamName('');
      setNewTeamDesc('');
      fetchTeams();
    })
    .catch(err => {
      setAddingTeam(false);
      console.error(err);
    });
  };

  const handleDeleteTeam = (teamName: string, id?: string) => {
    if (!confirm(`Are you sure you want to remove the team "${teamName}"? This will remove it from the public site and manage teams.`)) return;
    fetch('/api/teams', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'delete', name: teamName, id })
    })
      .then(res => res.json())
      .then(() => {
        fetchTeams();
      })
      .catch(err => console.error(err));
  };

  const handleStartEditTeam = (t: any) => {
    setEditingTeamId(t.id || t._id?.$oid);
    setEditTeamName(t.name);
    setEditTeamDesc(t.desc || '');
  };

  const handleCancelEditTeam = () => {
    setEditingTeamId(null);
    setEditTeamName('');
    setEditTeamDesc('');
  };

  const handleSaveTeam = (t: any) => {
    if (!editTeamName.trim()) return;
    setUpdatingTeam(true);
    fetch('/api/teams', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        action: 'update',
        id: t.id || t._id?.$oid,
        oldName: t.name,
        name: editTeamName.trim(),
        desc: editTeamDesc.trim()
      })
    })
      .then(res => res.json())
      .then(data => {
        setUpdatingTeam(false);
        if (data.status === 'ok') {
          setEditingTeamId(null);
          fetchTeams();
        } else {
          alert(data.message || 'Failed to update team');
        }
      })
      .catch(err => {
        setUpdatingTeam(false);
        console.error(err);
      });
  };

  const handleSave = () => {
    setSaving(true);
    fetch('/api/content', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(content)
    }).then(res => res.json()).then(() => {
      setSaving(false);
      setSaveSuccess(true);
      setTimeout(() => setSaveSuccess(false), 3000);
    }).catch(err => {
      setSaving(false);
      console.error(err);
      alert('Failed to save content.');
    });
  };

  // Helper to compress large client-side image files before uploading
  const compressImage = (file: File): Promise<{ filename: string; dataUrl: string; blob: Blob }> => {
    return new Promise((resolve, reject) => {
      const img = new Image();
      img.src = URL.createObjectURL(file);
      img.onload = () => {
        URL.revokeObjectURL(img.src);
        const maxDimension = 1920;
        let width = img.width;
        let height = img.height;

        if (width > maxDimension || height > maxDimension) {
          if (width > height) {
            height = Math.round((height * maxDimension) / width);
            width = maxDimension;
          } else {
            width = Math.round((width * maxDimension) / height);
            height = maxDimension;
          }
        }

        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        const ctx = canvas.getContext('2d');
        if (!ctx) {
          reject(new Error('Canvas context unavailable'));
          return;
        }

        ctx.drawImage(img, 0, 0, width, height);
        const dataUrl = canvas.toDataURL('image/jpeg', 0.85);
        canvas.toBlob((blob) => {
          if (blob) {
            resolve({ filename: file.name.replace(/\.[^/.]+$/, "") + ".jpg", dataUrl, blob });
          } else {
            reject(new Error('Blob creation failed'));
          }
        }, 'image/jpeg', 0.85);
      };
      img.onerror = reject;
    });
  };

  const handleDeviceUpload = async (e: React.ChangeEvent<HTMLInputElement>, target: 'media' | 'tafrah' | 'about' | 'tafrahSite') => {
    const file = e.target.files?.[0];
    if (!file) return;

    setUploadingTarget(target);

    try {
      // Compress image client side
      const { filename, blob, dataUrl } = await compressImage(file);
      
      // Try multipart FormData upload first
      const formData = new FormData();
      formData.append('file', blob, filename);

      const res = await fetch('/api/upload', {
        method: 'POST',
        body: formData
      });

      const data = await res.json();
      setUploadingTarget(null);

      if (data && data.url) {
        if (target === 'media') {
          setContent((prev: any) => ({ ...prev, mediaGallery: [...(prev.mediaGallery || []), { url: data.url }] }));
        } else if (target === 'tafrah') {
          setContent((prev: any) => ({ ...prev, tafrahImages: [...(prev.tafrahImages || []), { url: data.url }] }));
        } else if (target === 'about') {
          setContent((prev: any) => ({ ...prev, aboutImages: [...(prev.aboutImages || []), { url: data.url }] }));
        } else if (target === 'tafrahSite') {
          setContent((prev: any) => ({ ...prev, tafrahSiteImage: data.url }));
        }
      } else {
        // Fallback to base64
        uploadBase64Fallback(filename, dataUrl, target);
      }
    } catch (err) {
      console.warn('FormData upload error, attempting base64 fallback:', err);
      // Read original file as base64
      const reader = new FileReader();
      reader.onload = () => {
        uploadBase64Fallback(file.name, reader.result as string, target);
      };
      reader.readAsDataURL(file);
    }

    if (e.target) e.target.value = '';
  };

  const uploadBase64Fallback = (filename: string, base64Data: string, target: 'media' | 'tafrah' | 'about' | 'tafrahSite') => {
    fetch('/api/upload', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ filename, data: base64Data })
    })
    .then(r => r.json())
    .then(data => {
      setUploadingTarget(null);
      if (data.url) {
        if (target === 'media') {
          setContent((prev: any) => ({ ...prev, mediaGallery: [...(prev.mediaGallery || []), { url: data.url }] }));
        } else if (target === 'tafrah') {
          setContent((prev: any) => ({ ...prev, tafrahImages: [...(prev.tafrahImages || []), { url: data.url }] }));
        } else if (target === 'about') {
          setContent((prev: any) => ({ ...prev, aboutImages: [...(prev.aboutImages || []), { url: data.url }] }));
        } else if (target === 'tafrahSite') {
          setContent((prev: any) => ({ ...prev, tafrahSiteImage: data.url }));
        }
      } else {
        alert('Upload failed.');
      }
    })
    .catch(err => {
      setUploadingTarget(null);
      console.error(err);
      alert('Upload failed.');
    });
  };

  const removeMedia = (url: string) => {
    setContent((prev: any) => ({
      ...prev,
      mediaGallery: (prev.mediaGallery || []).filter((m: any) => m.url !== url)
    }));
  };

  const removeTafrahImage = (url: string) => {
    setContent((prev: any) => ({
      ...prev,
      tafrahImages: (prev.tafrahImages || []).filter((m: any) => m.url !== url)
    }));
  };

  const removeAboutImage = (url: string) => {
    setContent((prev: any) => ({
      ...prev,
      aboutImages: (prev.aboutImages || []).filter((m: any) => m.url !== url)
    }));
  };

  return (
    <div className="fade-in" style={{ paddingBottom: '40px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', marginBottom: '28px', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">05 — Site CMS</span>
          <h1 className="heading-lg">Content CMS.</h1>
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>
            Site-wide content manager. Edit headlines, copy, stats, images, and settings for every section.
          </p>
        </div>

        <button
          className="btn-primary"
          style={{ display: 'flex', alignItems: 'center', gap: '8px', padding: '14px 28px' }}
          onClick={handleSave}
          disabled={saving}
        >
          {saveSuccess ? <CheckCircle2 size={18} color="#0E1013" /> : <Save size={18} />}
          {saving ? 'Saving...' : saveSuccess ? 'Saved Live!' : 'Save Changes'}
        </button>
      </div>

      {/* Brutalist Section Tabs */}
      <div style={{
        display: 'flex',
        flexWrap: 'wrap',
        gap: '8px',
        marginBottom: '28px',
        borderBottom: '3px solid #0E1013',
        paddingBottom: '16px'
      }}>
        {[
          { id: 'hero', label: '01 Hero Section', icon: <Sparkles size={16} /> },
          { id: 'about', label: '02 About Section', icon: <Type size={16} /> },
          { id: 'tafrah', label: '03 Tafrah Project', icon: <Layers size={16} /> },
          { id: 'inside', label: '04 Inside the Club', icon: <UsersIcon size={16} /> },
          { id: 'gallery', label: '05 Media Gallery', icon: <ImageIcon size={16} /> },
          { id: 'join', label: '06 Join Us / Recruitment', icon: <MessageSquare size={16} /> },
          { id: 'footer', label: '07 Footer & Socials', icon: <Globe size={16} /> }
        ].map(tab => (
          <button
            key={tab.id}
            onClick={() => setActiveTab(tab.id as any)}
            style={{
              padding: '10px 18px',
              border: '2.5px solid #0E1013',
              background: activeTab === tab.id ? '#FFC629' : '#FFF',
              color: '#0E1013',
              fontFamily: 'IBM Plex Mono, monospace',
              fontSize: '12px',
              fontWeight: 700,
              textTransform: 'uppercase',
              display: 'flex',
              alignItems: 'center',
              gap: '8px',
              cursor: 'pointer',
              boxShadow: activeTab === tab.id ? '4px 4px 0px #0E1013' : 'none',
              transform: activeTab === tab.id ? 'translate(-2px, -2px)' : 'none',
              transition: 'all 0.15s'
            }}
          >
            {tab.icon} {tab.label}
          </button>
        ))}
      </div>

      {/* TAB 1: HERO SECTION */}
      {activeTab === 'hero' && (
        <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
          <div>
            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' }}>
              Hero Header Configuration
            </span>
            <h2 className="heading-sm" style={{ margin: 0 }}>Hero Section</h2>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(280px, 1fr))', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Campus Location Tag
              </label>
              <input
                className="input-field"
                value={content.heroCampus || ''}
                onChange={e => setContent((p: any) => ({ ...p, heroCampus: e.target.value }))}
                placeholder="Sadat Academy — Maadi"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Chapter Affiliation Tag
              </label>
              <input
                className="input-field"
                value={content.heroChapter || ''}
                onChange={e => setContent((p: any) => ({ ...p, heroChapter: e.target.value }))}
                placeholder="Student chapter · Enactus Egypt"
              />
            </div>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(240px, 1fr))', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Tagline Highlight
              </label>
              <input
                className="input-field"
                value={content.heroTagline || ''}
                onChange={e => setContent((p: any) => ({ ...p, heroTagline: e.target.value }))}
                placeholder="Entrepreneurial action"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Headline Row 1
              </label>
              <input
                className="input-field"
                value={content.heroHeadline1 || ''}
                onChange={e => setContent((p: any) => ({ ...p, heroHeadline1: e.target.value }))}
                placeholder="We"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Headline Row 2
              </label>
              <input
                className="input-field"
                value={content.heroHeadline2 || ''}
                onChange={e => setContent((p: any) => ({ ...p, heroHeadline2: e.target.value }))}
                placeholder="Change"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Headline Row 3 (Accent)
              </label>
              <input
                className="input-field"
                value={content.heroHeadline3 || ''}
                onChange={e => setContent((p: any) => ({ ...p, heroHeadline3: e.target.value }))}
                placeholder="The World"
              />
            </div>
          </div>

          <div>
            <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
              Hero Subtitle / Description
            </label>
            <textarea
              className="input-field"
              rows={3}
              value={content.heroSubtitle || ''}
              onChange={e => setContent((p: any) => ({ ...p, heroSubtitle: e.target.value }))}
              placeholder="A student team building real ventures for real problems..."
            />
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Primary Button CTA
              </label>
              <input
                className="input-field"
                value={content.heroCta1 || ''}
                onChange={e => setContent((p: any) => ({ ...p, heroCta1: e.target.value }))}
                placeholder="Join the next season"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Secondary Button CTA
              </label>
              <input
                className="input-field"
                value={content.heroCta2 || ''}
                onChange={e => setContent((p: any) => ({ ...p, heroCta2: e.target.value }))}
                placeholder="See Tafrah"
              />
            </div>
          </div>

          {/* Hero Statistics */}
          <div style={{ borderTop: '2px solid #0E1013', paddingTop: '20px' }}>
            <h3 className="font-mono" style={{ fontSize: '13px', textTransform: 'uppercase', marginBottom: '16px' }}>
              Hero Stat Counters
            </h3>
            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(220px, 1fr))', gap: '20px' }}>
              <div style={{ padding: '16px', background: '#F7F5F0', border: '2px solid #0E1013' }}>
                <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Stat 1 Number</label>
                <input
                  className="input-field"
                  style={{ marginBottom: '10px' }}
                  value={content.heroStat1Num || ''}
                  onChange={e => setContent((p: any) => ({ ...p, heroStat1Num: e.target.value }))}
                  placeholder="06"
                />
                <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Stat 1 Label</label>
                <input
                  className="input-field"
                  value={content.heroStat1Label || ''}
                  onChange={e => setContent((p: any) => ({ ...p, heroStat1Label: e.target.value }))}
                  placeholder="Teams"
                />
              </div>

              <div style={{ padding: '16px', background: '#F7F5F0', border: '2px solid #0E1013' }}>
                <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Stat 2 Number</label>
                <input
                  className="input-field"
                  style={{ marginBottom: '10px' }}
                  value={content.heroStat2Num || ''}
                  onChange={e => setContent((p: any) => ({ ...p, heroStat2Num: e.target.value }))}
                  placeholder="01"
                />
                <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Stat 2 Label</label>
                <input
                  className="input-field"
                  value={content.heroStat2Label || ''}
                  onChange={e => setContent((p: any) => ({ ...p, heroStat2Label: e.target.value }))}
                  placeholder="Project at nationals"
                />
              </div>

              <div style={{ padding: '16px', background: '#F7F5F0', border: '2px solid #0E1013' }}>
                <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Stat 3 Number</label>
                <input
                  className="input-field"
                  style={{ marginBottom: '10px' }}
                  value={content.heroStat3Num || ''}
                  onChange={e => setContent((p: any) => ({ ...p, heroStat3Num: e.target.value }))}
                  placeholder="∞"
                />
                <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Stat 3 Label</label>
                <input
                  className="input-field"
                  value={content.heroStat3Label || ''}
                  onChange={e => setContent((p: any) => ({ ...p, heroStat3Label: e.target.value }))}
                  placeholder="Reasons to start"
                />
              </div>
            </div>
          </div>
        </div>
      )}

      {/* TAB 2: ABOUT SECTION */}
      {activeTab === 'about' && (
        <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
          <div>
            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' }}>
              Who We Are
            </span>
            <h2 className="heading-sm" style={{ margin: 0 }}>About Section</h2>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(280px, 1fr))', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Section Kicker
              </label>
              <input
                className="input-field"
                value={content.aboutKicker || ''}
                onChange={e => setContent((p: any) => ({ ...p, aboutKicker: e.target.value }))}
                placeholder="01 — Who we are"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Section Heading
              </label>
              <input
                className="input-field"
                value={content.aboutHeading || ''}
                onChange={e => setContent((p: any) => ({ ...p, aboutHeading: e.target.value }))}
                placeholder="A club that builds businesses to solve problems."
              />
            </div>
          </div>

          <div>
            <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
              Paragraph 1
            </label>
            <textarea
              className="input-field"
              rows={4}
              value={content.aboutP1 || ''}
              onChange={e => setContent((p: any) => ({ ...p, aboutP1: e.target.value }))}
              placeholder="Enactus is a global network of students..."
            />
          </div>

          <div>
            <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
              Paragraph 2
            </label>
            <textarea
              className="input-field"
              rows={3}
              value={content.aboutP2 || ''}
              onChange={e => setContent((p: any) => ({ ...p, aboutP2: e.target.value }))}
              placeholder="Nobody here is only a member..."
            />
          </div>

          <div>
            <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
              Tags / Pills (comma-separated)
            </label>
            <input
              className="input-field"
              value={content.aboutTags || ''}
              onChange={e => setContent((p: any) => ({ ...p, aboutTags: e.target.value }))}
              placeholder="Social entrepreneurship, Field research, Pitching, Production"
            />
          </div>

          {/* About Section Photos Upload */}
          <div style={{ borderTop: '2px solid #0E1013', paddingTop: '20px' }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px', flexWrap: 'wrap', gap: '12px' }}>
              <div>
                <h3 className="font-mono" style={{ fontSize: '13px', textTransform: 'uppercase', margin: 0 }}>
                  About Section Photos ({(content.aboutImages || []).length})
                </h3>
                <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: '4px 0 0 0' }}>
                  Photos displayed on the right side of the About section on the home page.
                </p>
              </div>

              <input
                type="file"
                ref={aboutInputRef}
                accept="image/*"
                style={{ display: 'none' }}
                onChange={e => handleDeviceUpload(e, 'about')}
              />
              <button
                className="btn-primary"
                style={{ display: 'flex', alignItems: 'center', gap: '6px', fontSize: '12px', padding: '10px 16px' }}
                onClick={() => aboutInputRef.current?.click()}
                disabled={uploadingTarget === 'about'}
              >
                <Upload size={14} /> {uploadingTarget === 'about' ? 'Uploading...' : 'Upload Photo'}
              </button>
            </div>

            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(140px, 1fr))', gap: '12px' }}>
              {(content.aboutImages || []).map((img: any, i: number) => (
                <div key={i} style={{ position: 'relative', border: '2px solid #0E1013', aspectRatio: '4/3', overflow: 'hidden' }}>
                  <img src={img.url} alt="About photo" style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
                  <button
                    onClick={() => removeAboutImage(img.url)}
                    style={{
                      position: 'absolute',
                      top: '6px',
                      right: '6px',
                      background: '#E53935',
                      color: '#FFF',
                      border: '1.5px solid #0E1013',
                      width: '26px',
                      height: '26px',
                      display: 'flex',
                      alignItems: 'center',
                      justifyContent: 'center',
                      cursor: 'pointer'
                    }}
                  >
                    <Trash2 size={14} />
                  </button>
                </div>
              ))}
            </div>
          </div>
        </div>
      )}

      {/* TAB 3: TAFRAH PROJECT */}
      {activeTab === 'tafrah' && (
        <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
          <div>
            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' }}>
              Flagship Project
            </span>
            <h2 className="heading-sm" style={{ margin: 0 }}>Tafrah Section</h2>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(240px, 1fr))', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Section Kicker
              </label>
              <input
                className="input-field"
                value={content.tafrahKicker || ''}
                onChange={e => setContent((p: any) => ({ ...p, tafrahKicker: e.target.value }))}
                placeholder="02 — This year's project"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Project Title / Name
              </label>
              <input
                className="input-field"
                value={content.tafrahTitle || ''}
                onChange={e => setContent((p: any) => ({ ...p, tafrahTitle: e.target.value }))}
                placeholder="Tafrah"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Arabic Tagline
              </label>
              <input
                className="input-field"
                value={content.tafrahTagline || ''}
                onChange={e => setContent((p: any) => ({ ...p, tafrahTagline: e.target.value }))}
                placeholder="طفـــرة — the leap"
              />
            </div>
          </div>

          <div>
            <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
              Project Main Description
            </label>
            <textarea
              className="input-field"
              rows={4}
              value={content.tafrahDesc || ''}
              onChange={e => setContent((p: any) => ({ ...p, tafrahDesc: e.target.value }))}
              placeholder="A training and employment platform built for autistic people in Egypt..."
            />
          </div>

          {/* 4 Feature Cards */}
          <div style={{ borderTop: '2px solid #0E1013', paddingTop: '20px' }}>
            <h3 className="font-mono" style={{ fontSize: '13px', textTransform: 'uppercase', marginBottom: '16px' }}>
              4 Feature Cards
            </h3>
            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(260px, 1fr))', gap: '20px' }}>
              <div style={{ padding: '16px', background: '#F7F5F0', border: '2px solid #0E1013' }}>
                <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, color: '#6D5E2C' }}>Card 01</span>
                <input
                  className="input-field"
                  style={{ margin: '8px 0' }}
                  value={content.tafrahH1Title || ''}
                  onChange={e => setContent((p: any) => ({ ...p, tafrahH1Title: e.target.value }))}
                  placeholder="Courses"
                />
                <textarea
                  className="input-field"
                  rows={2}
                  value={content.tafrahH1Desc || ''}
                  onChange={e => setContent((p: any) => ({ ...p, tafrahH1Desc: e.target.value }))}
                  placeholder="Skill tracks written in plain..."
                />
              </div>

              <div style={{ padding: '16px', background: '#F7F5F0', border: '2px solid #0E1013' }}>
                <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, color: '#6D5E2C' }}>Card 02</span>
                <input
                  className="input-field"
                  style={{ margin: '8px 0' }}
                  value={content.tafrahH2Title || ''}
                  onChange={e => setContent((p: any) => ({ ...p, tafrahH2Title: e.target.value }))}
                  placeholder="Assistant"
                />
                <textarea
                  className="input-field"
                  rows={2}
                  value={content.tafrahH2Desc || ''}
                  onChange={e => setContent((p: any) => ({ ...p, tafrahH2Desc: e.target.value }))}
                  placeholder="Step-by-step guidance..."
                />
              </div>

              <div style={{ padding: '16px', background: '#F7F5F0', border: '2px solid #0E1013' }}>
                <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, color: '#6D5E2C' }}>Card 03</span>
                <input
                  className="input-field"
                  style={{ margin: '8px 0' }}
                  value={content.tafrahH3Title || ''}
                  onChange={e => setContent((p: any) => ({ ...p, tafrahH3Title: e.target.value }))}
                  placeholder="Jobs"
                />
                <textarea
                  className="input-field"
                  rows={2}
                  value={content.tafrahH3Desc || ''}
                  onChange={e => setContent((p: any) => ({ ...p, tafrahH3Desc: e.target.value }))}
                  placeholder="Vetted employers, clear expectations..."
                />
              </div>

              <div style={{ padding: '16px', background: '#F7F5F0', border: '2px solid #0E1013' }}>
                <span className="font-mono" style={{ fontSize: '11px', fontWeight: 700, color: '#6D5E2C' }}>Card 04</span>
                <input
                  className="input-field"
                  style={{ margin: '8px 0' }}
                  value={content.tafrahH4Title || ''}
                  onChange={e => setContent((p: any) => ({ ...p, tafrahH4Title: e.target.value }))}
                  placeholder="Dashboard"
                />
                <textarea
                  className="input-field"
                  rows={2}
                  value={content.tafrahH4Desc || ''}
                  onChange={e => setContent((p: any) => ({ ...p, tafrahH4Desc: e.target.value }))}
                  placeholder="Progress, certificates and readiness..."
                />
              </div>
            </div>
          </div>

          <div>
            <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
              Tafrah Bottom Note
            </label>
            <input
              className="input-field"
              value={content.tafrahFooter || ''}
              onChange={e => setContent((p: any) => ({ ...p, tafrahFooter: e.target.value }))}
              placeholder="Tafrah was the venture Enactus SAMS Maadi carried..."
            />
          </div>

          {/* Tafrah Main Showcase Photo */}
          <div style={{ borderTop: '2px solid #0E1013', paddingTop: '20px' }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px', flexWrap: 'wrap', gap: '12px' }}>
              <div>
                <h3 className="font-mono" style={{ fontSize: '13px', textTransform: 'uppercase', margin: 0 }}>
                  Tafrah Platform Showcase Photo
                </h3>
                <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: '4px 0 0 0' }}>
                  Main large image displayed beside the 4 feature cards.
                </p>
              </div>

              <input
                type="file"
                ref={tafrahSiteInputRef}
                accept="image/*"
                style={{ display: 'none' }}
                onChange={e => handleDeviceUpload(e, 'tafrahSite')}
              />
              <button
                className="btn-primary"
                style={{ display: 'flex', alignItems: 'center', gap: '6px', fontSize: '12px', padding: '10px 16px' }}
                onClick={() => tafrahSiteInputRef.current?.click()}
                disabled={uploadingTarget === 'tafrahSite'}
              >
                <Upload size={14} /> {uploadingTarget === 'tafrahSite' ? 'Uploading...' : 'Replace Photo'}
              </button>
            </div>

            <div style={{ maxWidth: '360px', position: 'relative', border: '2px solid #0E1013', aspectRatio: '16/10', overflow: 'hidden' }}>
              <img
                src={content.tafrahSiteImage || 'assets/tafrah-site.jpg'}
                alt="Tafrah Showcase"
                style={{ width: '100%', height: '100%', objectFit: 'cover' }}
              />
            </div>
          </div>

          {/* Tafrah Images Upload */}
          <div style={{ borderTop: '2px solid #0E1013', paddingTop: '20px' }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px', flexWrap: 'wrap', gap: '12px' }}>
              <div>
                <h3 className="font-mono" style={{ fontSize: '13px', textTransform: 'uppercase', margin: 0 }}>
                  Tafrah Behind-the-Scenes Photos ({(content.tafrahImages || []).length})
                </h3>
              </div>
              <input
                type="file"
                ref={tafrahInputRef}
                accept="image/*"
                style={{ display: 'none' }}
                onChange={e => handleDeviceUpload(e, 'tafrah')}
              />
              <button
                className="btn-primary"
                style={{ display: 'flex', alignItems: 'center', gap: '6px', fontSize: '12px', padding: '10px 16px' }}
                onClick={() => tafrahInputRef.current?.click()}
                disabled={uploadingTarget === 'tafrah'}
              >
                <Upload size={14} /> {uploadingTarget === 'tafrah' ? 'Uploading...' : 'Upload Photo'}
              </button>
            </div>

            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(140px, 1fr))', gap: '12px' }}>
              {(content.tafrahImages || []).map((img: any, i: number) => (
                <div key={i} style={{ position: 'relative', border: '2px solid #0E1013', aspectRatio: '3/4', overflow: 'hidden' }}>
                  <img src={img.url} alt="Tafrah" style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
                  <button
                    onClick={() => removeTafrahImage(img.url)}
                    style={{
                      position: 'absolute',
                      top: '6px',
                      right: '6px',
                      background: '#E53935',
                      color: '#FFF',
                      border: '1.5px solid #0E1013',
                      width: '26px',
                      height: '26px',
                      display: 'flex',
                      alignItems: 'center',
                      justifyContent: 'center',
                      cursor: 'pointer'
                    }}
                  >
                    <Trash2 size={14} />
                  </button>
                </div>
              ))}
            </div>
          </div>
        </div>
      )}

      {/* TAB 4: INSIDE THE CLUB / TEAMS */}
      {activeTab === 'inside' && (
        <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
          <div>
            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' }}>
              Teams & Structure
            </span>
            <h2 className="heading-sm" style={{ margin: 0 }}>03 — Inside the Club Section</h2>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(280px, 1fr))', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Section Kicker
              </label>
              <input
                className="input-field"
                value={content.insideKicker || ''}
                onChange={e => setContent((p: any) => ({ ...p, insideKicker: e.target.value }))}
                placeholder="03 — Inside the club"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Section Title
              </label>
              <input
                className="input-field"
                value={content.insideTitle || ''}
                onChange={e => setContent((p: any) => ({ ...p, insideTitle: e.target.value }))}
                placeholder="Six teams. One project."
              />
            </div>
          </div>

          <div>
            <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
              Section Description / Subtitle
            </label>
            <textarea
              className="input-field"
              rows={3}
              value={content.insideDesc || ''}
              onChange={e => setContent((p: any) => ({ ...p, insideDesc: e.target.value }))}
              placeholder="Every team owns a real part of the outcome. You pick where you start — not where you stay."
            />
          </div>

          {/* Team Cards on Public Site */}
          <div style={{ borderTop: '2px solid #0E1013', paddingTop: '20px' }}>
            <h3 className="font-mono" style={{ fontSize: '13px', textTransform: 'uppercase', marginBottom: '16px' }}>
              Active Teams Displayed on Public Site ({teams.length})
            </h3>

            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(280px, 1fr))', gap: '16px', marginBottom: '24px' }}>
              {teams.map((t, idx) => {
                const teamId = t.id || t._id?.$oid;
                const isEditing = editingTeamId === teamId;

                return (
                  <div key={teamId || idx} style={{ background: '#0E1013', color: '#F7F5F0', border: '2.5px solid #0E1013', padding: '20px', display: 'flex', flexDirection: 'column', gap: '10px' }}>
                    <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
                      <span className="font-mono" style={{ fontSize: '11px', color: '#FFC629', fontWeight: 700 }}>
                        0{idx + 1} / {t.name}
                      </span>
                      <div style={{ display: 'flex', gap: '6px' }}>
                        {!isEditing ? (
                          <>
                            <button
                              onClick={() => handleStartEditTeam(t)}
                              title="Edit team title and description"
                              style={{
                                background: '#FFC629',
                                color: '#0E1013',
                                border: '1.5px solid #FFC629',
                                padding: '4px 8px',
                                cursor: 'pointer',
                                display: 'flex',
                                alignItems: 'center',
                                gap: '4px',
                                fontSize: '11px',
                                fontWeight: 700,
                                fontFamily: 'IBM Plex Mono'
                              }}
                            >
                              <Edit2 size={12} /> Edit
                            </button>
                            <button
                              onClick={() => handleDeleteTeam(t.name, teamId)}
                              title="Remove team"
                              style={{
                                background: '#DC2626',
                                color: '#FFF',
                                border: '1.5px solid #0E1013',
                                padding: '4px 8px',
                                cursor: 'pointer',
                                display: 'flex',
                                alignItems: 'center',
                                gap: '4px',
                                fontSize: '11px',
                                fontWeight: 700,
                                fontFamily: 'IBM Plex Mono'
                              }}
                            >
                              <Trash2 size={12} /> Remove
                            </button>
                          </>
                        ) : (
                          <>
                            <button
                              onClick={() => handleSaveTeam(t)}
                              disabled={updatingTeam || !editTeamName.trim()}
                              title="Save team details"
                              style={{
                                background: '#10B981',
                                color: '#FFF',
                                border: '1.5px solid #10B981',
                                padding: '4px 8px',
                                cursor: 'pointer',
                                display: 'flex',
                                alignItems: 'center',
                                gap: '4px',
                                fontSize: '11px',
                                fontWeight: 700,
                                fontFamily: 'IBM Plex Mono'
                              }}
                            >
                              <Check size={12} /> {updatingTeam ? 'Saving...' : 'Save'}
                            </button>
                            <button
                              onClick={handleCancelEditTeam}
                              disabled={updatingTeam}
                              title="Cancel editing"
                              style={{
                                background: '#374151',
                                color: '#FFF',
                                border: '1.5px solid #374151',
                                padding: '4px 8px',
                                cursor: 'pointer',
                                display: 'flex',
                                alignItems: 'center',
                                gap: '4px',
                                fontSize: '11px',
                                fontWeight: 700,
                                fontFamily: 'IBM Plex Mono'
                              }}
                            >
                              <X size={12} /> Cancel
                            </button>
                          </>
                        )}
                      </div>
                    </div>

                    {isEditing ? (
                      <div style={{ display: 'flex', flexDirection: 'column', gap: '10px', marginTop: '4px' }}>
                        <div>
                          <label className="font-mono" style={{ fontSize: '11px', color: '#FFC629', display: 'block', marginBottom: '4px', fontWeight: 600 }}>
                            Team Title / Name
                          </label>
                          <input
                            className="input-field"
                            style={{ background: '#181C22', color: '#F7F5F0', border: '1.5px solid #FFC629' }}
                            value={editTeamName}
                            onChange={e => setEditTeamName(e.target.value)}
                            placeholder="e.g. Research & Development"
                          />
                        </div>
                        <div>
                          <label className="font-mono" style={{ fontSize: '11px', color: '#FFC629', display: 'block', marginBottom: '4px', fontWeight: 600 }}>
                            Team Description
                          </label>
                          <textarea
                            className="input-field"
                            rows={3}
                            style={{ background: '#181C22', color: '#F7F5F0', border: '1.5px solid #FFC629' }}
                            value={editTeamDesc}
                            onChange={e => setEditTeamDesc(e.target.value)}
                            placeholder="What does this team do?"
                          />
                        </div>
                      </div>
                    ) : (
                      <>
                        <h4 style={{ margin: 0, fontSize: '18px', fontWeight: 800 }}>{t.name}</h4>
                        <p style={{ margin: 0, fontSize: '13px', opacity: 0.8, lineHeight: 1.5 }}>{t.desc || 'No description provided.'}</p>
                      </>
                    )}
                  </div>
                );
              })}
            </div>

            {/* Add New Team Form */}
            <div style={{ background: '#F7F5F0', border: '2px solid #0E1013', padding: '20px' }}>
              <h4 className="font-mono" style={{ margin: '0 0 16px 0', fontSize: '12px', textTransform: 'uppercase', fontWeight: 700 }}>
                Add New Team to Section & Public Site
              </h4>
              <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(240px, 1fr))', gap: '16px', marginBottom: '16px' }}>
                <div>
                  <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Team Name</label>
                  <input
                    className="input-field"
                    placeholder="e.g. Research & Development"
                    value={newTeamName}
                    onChange={e => setNewTeamName(e.target.value)}
                  />
                </div>
                <div>
                  <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Team Description</label>
                  <input
                    className="input-field"
                    placeholder="What does this team do?"
                    value={newTeamDesc}
                    onChange={e => setNewTeamDesc(e.target.value)}
                  />
                </div>
              </div>
              <button
                className="btn-primary"
                style={{ padding: '10px 20px', fontSize: '13px' }}
                onClick={handleAddTeam}
                disabled={addingTeam || !newTeamName.trim()}
              >
                {addingTeam ? 'Adding...' : '+ Add Team to Site'}
              </button>
            </div>
          </div>
        </div>
      )}

      {/* TAB 5: MEDIA GALLERY */}
      {activeTab === 'gallery' && (
        <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
          <div>
            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' }}>
              Campus Life
            </span>
            <h2 className="heading-sm" style={{ margin: 0 }}>Life at Enactus Gallery</h2>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(260px, 1fr))', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Gallery Kicker
              </label>
              <input
                className="input-field"
                value={content.lifeKicker || ''}
                onChange={e => setContent((p: any) => ({ ...p, lifeKicker: e.target.value }))}
                placeholder="04 — Life at Enactus"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Gallery Main Title
              </label>
              <input
                className="input-field"
                value={content.lifeTitle || ''}
                onChange={e => setContent((p: any) => ({ ...p, lifeTitle: e.target.value }))}
                placeholder="Long days, yellow everywhere."
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Subtitle / Hint
              </label>
              <input
                className="input-field"
                value={content.lifeSubtitle || ''}
                onChange={e => setContent((p: any) => ({ ...p, lifeSubtitle: e.target.value }))}
                placeholder="Tap any photo to enlarge"
              />
            </div>
          </div>

          <div style={{ borderTop: '2px solid #0E1013', paddingTop: '20px' }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px', flexWrap: 'wrap', gap: '12px' }}>
              <div>
                <h3 className="font-mono" style={{ fontSize: '13px', textTransform: 'uppercase', margin: 0 }}>
                  Uploaded Gallery Photos ({(content.mediaGallery || []).length})
                </h3>
                <p className="font-mono" style={{ fontSize: '11px', opacity: 0.6, margin: '4px 0 0 0' }}>
                  Images are automatically optimized and compressed for fast website loading.
                </p>
              </div>

              <input
                type="file"
                ref={mediaInputRef}
                accept="image/*"
                style={{ display: 'none' }}
                onChange={e => handleDeviceUpload(e, 'media')}
              />
              <button
                className="btn-primary"
                style={{ display: 'flex', alignItems: 'center', gap: '6px', fontSize: '12px', padding: '10px 16px' }}
                onClick={() => mediaInputRef.current?.click()}
                disabled={uploadingTarget === 'media'}
              >
                <Upload size={14} /> {uploadingTarget === 'media' ? 'Uploading & Optimizing...' : 'Upload Photo'}
              </button>
            </div>

            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(140px, 1fr))', gap: '12px' }}>
              {(content.mediaGallery || []).map((img: any, i: number) => (
                <div key={i} style={{ position: 'relative', border: '2px solid #0E1013', aspectRatio: '3/4', overflow: 'hidden' }}>
                  <img src={img.url} alt="Gallery" style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
                  <button
                    onClick={() => removeMedia(img.url)}
                    style={{
                      position: 'absolute',
                      top: '6px',
                      right: '6px',
                      background: '#E53935',
                      color: '#FFF',
                      border: '1.5px solid #0E1013',
                      width: '26px',
                      height: '26px',
                      display: 'flex',
                      alignItems: 'center',
                      justifyContent: 'center',
                      cursor: 'pointer'
                    }}
                  >
                    <Trash2 size={14} />
                  </button>
                </div>
              ))}
            </div>
          </div>
        </div>
      )}

      {/* TAB 5: JOIN US / RECRUITMENT */}
      {activeTab === 'join' && (
        <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
          <div>
            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' }}>
              Recruitment & Applications
            </span>
            <h2 className="heading-sm" style={{ margin: 0 }}>Join Us Section</h2>
          </div>

          {/* Toggle Switch */}
          <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: '16px', padding: '20px', border: '2.5px solid #0E1013', background: '#F7F5F0', flexWrap: 'wrap' }}>
            <div>
              <h3 style={{ margin: 0, fontSize: '20px', fontWeight: 900 }}>Recruitment Status</h3>
              <p style={{ margin: '4px 0 0 0', fontSize: '14px', color: '#555' }}>Turn candidate applications on or off. When closed, the public form is replaced with a closed notice.</p>
            </div>
            <button 
              className="btn-primary" 
              style={{ 
                background: content.recruitmentOpen ? '#4CAF50' : '#E53935', 
                color: '#FFF',
                padding: '12px 28px',
                fontSize: '16px'
              }}
              onClick={() => setContent((prev: any) => ({ ...prev, recruitmentOpen: !prev.recruitmentOpen }))}
            >
              {content.recruitmentOpen ? 'OPEN' : 'CLOSED'}
            </button>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(260px, 1fr))', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Join Section Title
              </label>
              <input
                className="input-field"
                value={content.joinTitle || ''}
                onChange={e => setContent((p: any) => ({ ...p, joinTitle: e.target.value }))}
                placeholder="Join the next season."
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Kicker (When Open)
              </label>
              <input
                className="input-field"
                value={content.joinKickerOpen || ''}
                onChange={e => setContent((p: any) => ({ ...p, joinKickerOpen: e.target.value }))}
                placeholder="Recruitment is open"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Kicker (When Closed)
              </label>
              <input
                className="input-field"
                value={content.joinKickerClosed || ''}
                onChange={e => setContent((p: any) => ({ ...p, joinKickerClosed: e.target.value }))}
                placeholder="Recruitment"
              />
            </div>
          </div>

          <div>
            <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
              Join Section Description
            </label>
            <textarea
              className="input-field"
              rows={3}
              value={content.joinDesc || ''}
              onChange={e => setContent((p: any) => ({ ...p, joinDesc: e.target.value }))}
              placeholder="Pick the team you want to start in, tell us why, and come to the interview..."
            />
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                CTA Button Text (When Open)
              </label>
              <input
                className="input-field"
                value={content.joinCtaOpen || ''}
                onChange={e => setContent((p: any) => ({ ...p, joinCtaOpen: e.target.value }))}
                placeholder="Apply now"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                CTA Button Text (When Closed)
              </label>
              <input
                className="input-field"
                value={content.joinCtaClosed || ''}
                onChange={e => setContent((p: any) => ({ ...p, joinCtaClosed: e.target.value }))}
                placeholder="Join the waitlist"
              />
            </div>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Notice Note (When Open)
              </label>
              <input
                className="input-field"
                value={content.joinNoteOpen || ''}
                onChange={e => setContent((p: any) => ({ ...p, joinNoteOpen: e.target.value }))}
                placeholder="Applications close at the end of the month"
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Notice Note (When Closed)
              </label>
              <input
                className="input-field"
                value={content.joinNoteClosed || ''}
                onChange={e => setContent((p: any) => ({ ...p, joinNoteClosed: e.target.value }))}
                placeholder="We open applications at the start of each semester"
              />
            </div>
          </div>

          <div style={{ borderTop: '2px solid #0E1013', paddingTop: '20px' }}>
            <h3 className="font-mono" style={{ fontSize: '13px', textTransform: 'uppercase', marginBottom: '16px' }}>
              Closed Recruitment Notice Box
            </h3>
            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(280px, 1fr))', gap: '20px' }}>
              <div>
                <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                  Box Title
                </label>
                <input
                  className="input-field"
                  value={content.closedBannerTitle || ''}
                  onChange={e => setContent((p: any) => ({ ...p, closedBannerTitle: e.target.value }))}
                  placeholder="Applications Closed."
                />
              </div>
              <div>
                <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                  Box Message Body
                </label>
                <textarea
                  className="input-field"
                  rows={2}
                  value={content.closedBannerDesc || ''}
                  onChange={e => setContent((p: any) => ({ ...p, closedBannerDesc: e.target.value }))}
                  placeholder="Recruitment for this semester has ended..."
                />
              </div>
            </div>
          </div>
        </div>
      )}

      {/* TAB 6: FOOTER & SOCIALS */}
      {activeTab === 'footer' && (
        <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
          <div>
            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '6px' }}>
              Links & Copyright
            </span>
            <h2 className="heading-sm" style={{ margin: 0 }}>Footer & Social Links</h2>
          </div>

          <div>
            <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
              Chapter Address & Campus
            </label>
            <textarea
              className="input-field"
              rows={2}
              value={content.footerAbout || ''}
              onChange={e => setContent((p: any) => ({ ...p, footerAbout: e.target.value }))}
              placeholder="Sadat Academy for Management Sciences — Maadi&#10;Cairo, Egypt"
            />
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(240px, 1fr))', gap: '20px' }}>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Instagram Profile URL
              </label>
              <input
                className="input-field"
                value={content.footerSocialInsta || ''}
                onChange={e => setContent((p: any) => ({ ...p, footerSocialInsta: e.target.value }))}
                placeholder="https://instagram.com/..."
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                Facebook Page URL
              </label>
              <input
                className="input-field"
                value={content.footerSocialFb || ''}
                onChange={e => setContent((p: any) => ({ ...p, footerSocialFb: e.target.value }))}
                placeholder="https://facebook.com/..."
              />
            </div>
            <div>
              <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
                TikTok Profile URL
              </label>
              <input
                className="input-field"
                value={content.footerSocialTiktok || content.footerSocialLinkedin || ''}
                onChange={e => setContent((p: any) => ({ ...p, footerSocialTiktok: e.target.value }))}
                placeholder="https://tiktok.com/@..."
              />
            </div>
          </div>

          <div>
            <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px' }}>
              Footer Disclaimer Note
            </label>
            <input
              className="input-field"
              value={content.footerNote || ''}
              onChange={e => setContent((p: any) => ({ ...p, footerNote: e.target.value }))}
              placeholder="Enactus SAMS Maadi is a student chapter of the global Enactus network."
            />
          </div>
        </div>
      )}
    </div>
  );
}
