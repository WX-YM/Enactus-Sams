import { useState, useEffect, useRef } from 'react';
import { Upload, Trash2, Copy, Check, Image as ImageIcon } from 'lucide-react';

export default function Gallery() {
  const [content, setContent] = useState<any>({ mediaGallery: [], tafrahImages: [] });
  const [uploading, setUploading] = useState(false);
  const [copiedUrl, setCopiedUrl] = useState<string | null>(null);
  const [filter, setFilter] = useState<'all' | 'life' | 'tafrah' | 'uploads'>('all');
  const fileInputRef = useRef<HTMLInputElement | null>(null);
  const [uploadCategory, setUploadCategory] = useState<'life' | 'tafrah'>('life');

  const fetchContent = () => {
    fetch('/api/content')
      .then(r => r.json())
      .then(data => {
        if (data && data.content) {
          setContent(data.content);
        }
      });
  };

  useEffect(() => {
    fetchContent();
  }, []);

  const handleFileUpload = (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;

    setUploading(true);
    const reader = new FileReader();
    reader.onload = () => {
      const base64Data = reader.result as string;
      fetch('/api/upload', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ filename: file.name, data: base64Data })
      })
      .then(r => r.json())
      .then(data => {
        setUploading(false);
        if (data.url) {
          const newUrl = data.url;
          let updatedContent = { ...content };
          if (uploadCategory === 'life') {
            const list = [...(updatedContent.mediaGallery || []), { url: newUrl }];
            updatedContent.mediaGallery = list;
          } else {
            const list = [...(updatedContent.tafrahImages || []), { url: newUrl }];
            updatedContent.tafrahImages = list;
          }
          setContent(updatedContent);
          // Persist to content CMS
          fetch('/api/content', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(updatedContent)
          });
          alert('Image uploaded and added to ' + (uploadCategory === 'life' ? 'Life at Enactus' : 'Tafrah Project') + '!');
        } else {
          alert('Upload failed.');
        }
      })
      .catch(err => {
        setUploading(false);
        console.error(err);
        alert('Failed to upload image.');
      });
    };
    reader.readAsDataURL(file);
    if (fileInputRef.current) fileInputRef.current.value = '';
  };

  const deleteImage = (url: string, category: 'life' | 'tafrah') => {
    if (!confirm('Remove this image from the website?')) return;
    let updated = { ...content };
    if (category === 'life') {
      updated.mediaGallery = (updated.mediaGallery || []).filter((m: any) => m.url !== url);
    } else {
      updated.tafrahImages = (updated.tafrahImages || []).filter((m: any) => m.url !== url);
    }
    setContent(updated);
    fetch('/api/content', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(updated)
    });
  };

  const copyUrl = (url: string) => {
    navigator.clipboard.writeText(window.location.origin + url);
    setCopiedUrl(url);
    setTimeout(() => setCopiedUrl(null), 2000);
  };

  const allItems = [
    ...(content.mediaGallery || []).map((m: any) => ({ ...m, category: 'life' as const, label: 'Life at Enactus' })),
    ...(content.tafrahImages || []).map((m: any) => ({ ...m, category: 'tafrah' as const, label: 'Tafrah Project' })),
  ];

  const filteredItems = allItems.filter(item => {
    if (filter === 'all') return true;
    if (filter === 'life') return item.category === 'life';
    if (filter === 'tafrah') return item.category === 'tafrah';
    if (filter === 'uploads') return item.url.includes('/uploads/');
    return true;
  });

  return (
    <div className="fade-in" style={{ paddingBottom: '40px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', marginBottom: '32px', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">04 — Media</span>
          <h1 className="heading-lg">Site Gallery.</h1>
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>
            All pictures displayed across the public website. Upload new photos directly from your device.
          </p>
        </div>

        {/* Upload Button & Category Selector */}
        <div style={{ display: 'flex', gap: '12px', alignItems: 'center', background: '#FFF', padding: '12px 16px', border: '2.5px solid #0E1013', boxShadow: '4px 4px 0px #0E1013' }}>
          <div style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
            <span className="font-mono" style={{ fontSize: '10px', fontWeight: 700, textTransform: 'uppercase', opacity: 0.7 }}>Upload To:</span>
            <select
              value={uploadCategory}
              onChange={e => setUploadCategory(e.target.value as any)}
              className="input-field"
              style={{ padding: '6px 10px', fontSize: '13px' }}
            >
              <option value="life">Life at Enactus</option>
              <option value="tafrah">Tafrah Project</option>
            </select>
          </div>

          <input
            type="file"
            ref={fileInputRef}
            onChange={handleFileUpload}
            accept="image/*"
            style={{ display: 'none' }}
          />

          <button
            className="btn-primary"
            style={{ display: 'flex', alignItems: 'center', gap: '8px', marginTop: '16px' }}
            disabled={uploading}
            onClick={() => fileInputRef.current?.click()}
          >
            <Upload size={18} /> {uploading ? 'Uploading...' : 'Upload Image'}
          </button>
        </div>
      </div>

      {/* Filter Tabs */}
      <div style={{ display: 'flex', gap: '8px', marginBottom: '24px', flexWrap: 'wrap' }}>
        {[
          { key: 'all', label: `All Pictures (${allItems.length})` },
          { key: 'life', label: `Life at Enactus (${(content.mediaGallery || []).length})` },
          { key: 'tafrah', label: `Tafrah Project (${(content.tafrahImages || []).length})` },
          { key: 'uploads', label: `Uploaded (${allItems.filter(i => i.url.includes('/uploads/')).length})` }
        ].map(t => (
          <button
            key={t.key}
            onClick={() => setFilter(t.key as any)}
            style={{
              padding: '10px 18px',
              border: '2px solid #0E1013',
              background: filter === t.key ? '#FFC629' : '#FFF',
              fontFamily: 'IBM Plex Mono, monospace',
              fontSize: '13px',
              fontWeight: 700,
              textTransform: 'uppercase',
              boxShadow: filter === t.key ? '4px 4px 0px #0E1013' : 'none',
              cursor: 'pointer'
            }}
          >
            {t.label}
          </button>
        ))}
      </div>

      {/* Gallery Grid */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(220px, 1fr))', gap: '20px' }}>
        {filteredItems.map((item, idx) => (
          <div
            key={idx}
            className="card"
            style={{
              padding: '12px',
              display: 'flex',
              flexDirection: 'column',
              gap: '10px',
              background: '#FFF'
            }}
          >
            <div style={{ position: 'relative', width: '100%', height: '180px', overflow: 'hidden', border: '2px solid #0E1013' }}>
              <img
                src={item.url}
                alt=""
                style={{ width: '100%', height: '100%', objectFit: 'cover' }}
              />
              <span
                style={{
                  position: 'absolute',
                  top: '8px',
                  left: '8px',
                  background: item.category === 'life' ? '#FFC629' : '#60A5FA',
                  color: '#0E1013',
                  padding: '2px 8px',
                  border: '1.5px solid #0E1013',
                  fontFamily: 'IBM Plex Mono, monospace',
                  fontSize: '10px',
                  fontWeight: 800,
                  textTransform: 'uppercase',
                  boxShadow: '2px 2px 0px #0E1013'
                }}
              >
                {item.label}
              </span>
            </div>

            <div style={{ display: 'flex', gap: '8px', marginTop: 'auto' }}>
              <button
                onClick={() => copyUrl(item.url)}
                className="btn-outline"
                style={{ flex: 1, padding: '8px 10px', fontSize: '11px', display: 'flex', alignItems: 'center', justifyContent: 'center', gap: '4px' }}
                title="Copy URL"
              >
                {copiedUrl === item.url ? <Check size={14} color="#16A34A" /> : <Copy size={14} />}
                {copiedUrl === item.url ? 'Copied' : 'Copy URL'}
              </button>

              <button
                onClick={() => deleteImage(item.url, item.category)}
                className="btn-danger"
                style={{ padding: '8px 12px' }}
                title="Remove Image"
              >
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
