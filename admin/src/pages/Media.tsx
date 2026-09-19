import { useState, useEffect } from 'react';

export default function Media() {
  const [media, setMedia] = useState<string[]>([]);

  useEffect(() => {
    setMedia(JSON.parse(localStorage.getItem('sams_media') || '[]'));
  }, []);

  const handleUpload = () => {
    // Simulate upload by adding a placeholder image
    const newMedia = ['https://picsum.photos/400/300?random=' + Math.random(), ...media];
    setMedia(newMedia);
    localStorage.setItem('sams_media', JSON.stringify(newMedia));
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '32px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'end', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">06 — Assets</span>
          <h1 className="heading-lg">Media Gallery.</h1>
        </div>
        <button className="btn-primary" onClick={handleUpload}>+ Upload Photo</button>
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(220px, 1fr))', gap: '24px' }}>
        {media.map((url, i) => (
          <div key={i} className="card" style={{ padding: '0', overflow: 'hidden', height: '160px', position: 'relative', border: '2px solid #0E1013' }}>
            <img src={url} alt={`Gallery ${i}`} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
          </div>
        ))}
      </div>
    </div>
  );
}
