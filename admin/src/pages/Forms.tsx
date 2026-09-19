import { useState } from 'react';

export default function Forms() {
  const [fields, setFields] = useState([{ id: 1, type: 'text', label: 'Full Name' }]);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '40px' }}>
      <div>
        <span className="heading-sm">04 — Operations</span>
        <h1 className="heading-lg">Form Maker.</h1>
      </div>

      <div className="card" style={{ maxWidth: '700px' }}>
        <input className="input-field" placeholder="Form Title (e.g., Season 2026 Recruitment)" style={{ marginBottom: '32px', fontSize: '24px', fontWeight: '800', border: 'none', borderBottom: '4px solid #0E1013', boxShadow: 'none', paddingLeft: 0, background: 'transparent' }} />
        
        <div style={{ display: 'flex', flexDirection: 'column', gap: '24px', marginBottom: '40px' }}>
          {fields.map(f => (
            <div key={f.id} style={{ display: 'flex', gap: '16px', alignItems: 'center' }}>
              <input className="input-field" style={{ flex: 1 }} value={f.label} onChange={() => {}} />
              <select className="input-field" value={f.type} onChange={() => {}} style={{ width: '200px' }}>
                <option value="text">Short Text</option>
                <option value="textarea">Long Text</option>
                <option value="select">Dropdown (Single)</option>
                <option value="multiselect">Dropdown (Multi)</option>
              </select>
              <button className="btn-danger" style={{ padding: '14px', height: '52px', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>✕</button>
            </div>
          ))}
        </div>

        <div style={{ display: 'flex', gap: '16px', flexWrap: 'wrap' }}>
          <button className="btn-outline" onClick={() => setFields([...fields, { id: Date.now(), type: 'text', label: 'New Field' }])}>
            + Add Field
          </button>
          <button className="btn-primary" style={{ marginLeft: 'auto' }}>Publish Form</button>
        </div>
      </div>
    </div>
  );
}
