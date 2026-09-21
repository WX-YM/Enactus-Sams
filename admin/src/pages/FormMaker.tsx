import { useState, useEffect } from 'react';
import { Plus, Trash2, ArrowUp, ArrowDown, Save, CheckCircle2, Eye, FileText, Share2, Copy, ExternalLink, X } from 'lucide-react';
import { Link } from 'react-router-dom';
import { useConfirm } from '../context/ConfirmContext';
import { authFetch } from '../api';

interface FormField {
  id: string;
  label: string;
  type: 'text' | 'email' | 'tel' | 'textarea' | 'select';
  placeholder: string;
  required: boolean;
  options?: string; // comma separated for select
}

const DEFAULT_FIELDS: FormField[] = [
  { id: '1', label: 'First Name', type: 'text', placeholder: 'First Name', required: true },
  { id: '2', label: 'Last Name', type: 'text', placeholder: 'Last Name', required: true },
  { id: '3', label: 'Email Address', type: 'email', placeholder: 'Email Address', required: true },
  { id: '4', label: 'Phone Number', type: 'tel', placeholder: 'Phone Number', required: true },
  { id: '5', label: 'Why do you want to join?', type: 'textarea', placeholder: 'What do you hope to achieve?', required: true },
  { id: '6', label: 'Preferred Team', type: 'select', placeholder: 'Select Team', required: true, options: 'Presentation, Project Management, HR, External Affairs, Social Media, Media Production' },
];

export default function FormMaker() {
  const { confirm, alert: showAlert } = useConfirm();
  const [formTitle, setFormTitle] = useState('Recruitment Application Form');
  const [formDescription, setFormDescription] = useState('Pick the team you want to start in, tell us why, and come to the interview.');
  const [fields, setFields] = useState<FormField[]>(DEFAULT_FIELDS);
  const [saving, setSaving] = useState(false);
  const [published, setPublished] = useState(false);
  const [previewMode, setPreviewMode] = useState(false);
  const [shareModalOpen, setShareModalOpen] = useState(false);
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    authFetch('/api/form_schema')
      .then(r => r.json())
      .then(data => {
        if (data && data.schema && data.schema.fields) {
          setFormTitle(data.schema.title || 'Recruitment Application Form');
          setFormDescription(data.schema.description || '');
          setFields(data.schema.fields);
        }
      })
      .catch(console.error);
  }, []);

  const addField = () => {
    const newField: FormField = {
      id: Date.now().toString(),
      label: 'New Field',
      type: 'text',
      placeholder: 'Enter value...',
      required: false,
    };
    setFields([...fields, newField]);
    setPublished(false);
  };

  const updateField = (id: string, prop: keyof FormField, value: any) => {
    setFields(fields.map(f => f.id === id ? { ...f, [prop]: value } : f));
    setPublished(false);
  };

  const deleteField = async (id: string) => {
    const field = fields.find(f => f.id === id);
    const ok = await confirm({
      title: 'Delete Field?',
      message: `Are you sure you want to remove the field "${field?.label || 'New Field'}"?`,
      confirmText: 'Delete Field',
      cancelText: 'Cancel',
      type: 'danger'
    });
    if (!ok) return;
    setFields(fields.filter(f => f.id !== id));
    setPublished(false);
  };

  const moveField = (index: number, direction: 'up' | 'down') => {
    const newIndex = direction === 'up' ? index - 1 : index + 1;
    if (newIndex < 0 || newIndex >= fields.length) return;
    const copy = [...fields];
    const item = copy.splice(index, 1)[0];
    copy.splice(newIndex, 0, item);
    setFields(copy);
    setPublished(false);
  };

  const handlePublish = () => {
    setSaving(true);
    authFetch('/api/form_schema', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        title: formTitle,
        description: formDescription,
        fields: fields,
        updatedAt: new Date().toISOString()
      })
    })
    .then(r => r.json())
    .then(() => {
      setSaving(false);
      setPublished(true);
      setShareModalOpen(true);
      setTimeout(() => setPublished(false), 3000);
    })
    .catch(err => {
      setSaving(false);
      console.error(err);
      showAlert({ title: 'Publish Failed', message: 'Failed to publish form schema to the server.', type: 'danger' });
    });
  };

  const copyShareLink = () => {
    const url = `${window.location.origin}/apply`;
    navigator.clipboard.writeText(url).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 2500);
    });
  };

  return (
    <div className="fade-in" style={{ paddingBottom: '40px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', marginBottom: '32px', flexWrap: 'wrap', gap: '16px' }}>
        <div>
          <span className="heading-sm">03 — Builder</span>
          <h1 className="heading-lg">Form Maker.</h1>
          <p className="font-mono" style={{ opacity: 0.7, marginTop: '8px' }}>
            Build, customize, and publish forms for candidate recruitment and registrations.
          </p>
        </div>

        <div style={{ display: 'flex', gap: '12px', alignItems: 'center', flexWrap: 'wrap' }}>
          <button
            className="btn-outline"
            onClick={() => setShareModalOpen(true)}
            style={{ display: 'flex', alignItems: 'center', gap: '8px' }}
          >
            <Share2 size={18} /> Share Form Link
          </button>

          <Link to="/form-responses" className="btn-outline" style={{ display: 'flex', alignItems: 'center', gap: '8px', textDecoration: 'none' }}>
            <FileText size={18} /> View Responses →
          </Link>

          <button
            className="btn-outline"
            onClick={() => setPreviewMode(!previewMode)}
            style={{ display: 'flex', alignItems: 'center', gap: '8px' }}
          >
            <Eye size={18} /> {previewMode ? 'Edit Fields' : 'Live Preview'}
          </button>

          <button
            className="btn-primary"
            onClick={handlePublish}
            disabled={saving}
            style={{ display: 'flex', alignItems: 'center', gap: '8px' }}
          >
            {published ? <CheckCircle2 size={18} color="#0E1013" /> : <Save size={18} />}
            {saving ? 'Publishing...' : published ? 'Published!' : 'Publish Form'}
          </button>
        </div>
      </div>

      {previewMode ? (
        /* LIVE FORM PREVIEW */
        <div className="card" style={{ maxWidth: '640px', margin: '0 auto', background: '#FFC629', padding: '40px', border: '3px solid #0E1013', boxShadow: '8px 8px 0px #0E1013' }}>
          <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: 'rgba(14,16,19,0.7)', display: 'block', marginBottom: '8px' }}>
            05 — FORM PREVIEW
          </span>
          <h2 style={{ margin: '0 0 12px 0', fontSize: '36px', fontWeight: 900, textTransform: 'uppercase', color: '#0E1013' }}>{formTitle}</h2>
          <p style={{ margin: '0 0 28px 0', fontSize: '16px', color: '#26292E', lineHeight: 1.5 }}>{formDescription}</p>

          <form onSubmit={e => e.preventDefault()} style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
            {fields.map(f => (
              <div key={f.id}>
                <label className="font-mono" style={{ fontSize: '12px', fontWeight: 700, display: 'block', marginBottom: '6px', textTransform: 'uppercase' }}>
                  {f.label} {f.required && <span style={{ color: '#E53935' }}>*</span>}
                </label>

                {f.type === 'textarea' ? (
                  <textarea
                    placeholder={f.placeholder}
                    rows={3}
                    style={{ width: '100%', padding: '14px 16px', border: '2.5px solid #0E1013', background: '#F7F5F0', fontFamily: 'Archivo', fontSize: '15px', fontWeight: 600, outline: 'none', boxShadow: '4px 4px 0px #0E1013' }}
                  />
                ) : f.type === 'select' ? (
                  <select style={{ width: '100%', padding: '14px 16px', border: '2.5px solid #0E1013', background: '#F7F5F0', fontFamily: 'Archivo', fontSize: '15px', fontWeight: 600, outline: 'none', boxShadow: '4px 4px 0px #0E1013' }}>
                    <option value="">{f.placeholder || 'Select...'}</option>
                    {(f.options || '').split(',').map((opt, i) => (
                      <option key={i} value={opt.trim()}>{opt.trim()}</option>
                    ))}
                  </select>
                ) : (
                  <input
                    type={f.type}
                    placeholder={f.placeholder}
                    style={{ width: '100%', padding: '14px 16px', border: '2.5px solid #0E1013', background: '#F7F5F0', fontFamily: 'Archivo', fontSize: '15px', fontWeight: 600, outline: 'none', boxShadow: '4px 4px 0px #0E1013' }}
                  />
                )}
              </div>
            ))}

            <button type="button" className="btn-primary" style={{ marginTop: '12px', justifyContent: 'center', background: '#0E1013', color: '#FFC629' }}>
              Submit Application
            </button>
          </form>
        </div>
      ) : (
        /* FORM BUILDER EDITOR */
        <div style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
          {/* Header Card */}
          <div className="card">
            <h2 className="heading-sm" style={{ marginBottom: '16px' }}>Form Details</h2>
            <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '20px' }}>
              <div>
                <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '6px' }}>Form Title</label>
                <input
                  className="input-field"
                  value={formTitle}
                  onChange={e => { setFormTitle(e.target.value); setPublished(false); }}
                />
              </div>
              <div>
                <label className="font-mono" style={{ fontSize: '12px', display: 'block', marginBottom: '6px' }}>Form Description</label>
                <input
                  className="input-field"
                  value={formDescription}
                  onChange={e => { setFormDescription(e.target.value); setPublished(false); }}
                />
              </div>
            </div>
          </div>

          {/* Fields List */}
          <div className="card">
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '24px' }}>
              <div>
                <h2 className="heading-sm" style={{ margin: 0 }}>Form Fields ({fields.length})</h2>
                <p className="font-mono" style={{ margin: '4px 0 0 0', fontSize: '12px', opacity: 0.6 }}>Customize questions, inputs, and validation.</p>
              </div>

              <button className="btn-primary" onClick={addField} style={{ display: 'flex', alignItems: 'center', gap: '6px', fontSize: '13px' }}>
                <Plus size={16} /> Add Field
              </button>
            </div>

            <div style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
              {fields.map((f, idx) => (
                <div
                  key={f.id}
                  style={{
                    border: '2.5px solid #0E1013',
                    padding: '20px',
                    background: '#F7F5F0',
                    boxShadow: '4px 4px 0px #0E1013',
                    display: 'flex',
                    flexDirection: 'column',
                    gap: '12px'
                  }}
                >
                  <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: '8px' }}>
                    <span className="font-mono" style={{ fontSize: '12px', fontWeight: 800, background: '#0E1013', color: '#FFF', padding: '2px 8px' }}>
                      #{idx + 1} — {f.label.toUpperCase()}
                    </span>

                    <div style={{ display: 'flex', gap: '6px', alignItems: 'center' }}>
                      <button
                        onClick={() => moveField(idx, 'up')}
                        disabled={idx === 0}
                        style={{ padding: '6px', border: '1.5px solid #0E1013', background: '#FFF', cursor: idx === 0 ? 'not-allowed' : 'pointer', opacity: idx === 0 ? 0.3 : 1 }}
                        title="Move Up"
                      >
                        <ArrowUp size={14} />
                      </button>
                      <button
                        onClick={() => moveField(idx, 'down')}
                        disabled={idx === fields.length - 1}
                        style={{ padding: '6px', border: '1.5px solid #0E1013', background: '#FFF', cursor: idx === fields.length - 1 ? 'not-allowed' : 'pointer', opacity: idx === fields.length - 1 ? 0.3 : 1 }}
                        title="Move Down"
                      >
                        <ArrowDown size={14} />
                      </button>
                      <button
                        onClick={() => deleteField(f.id)}
                        className="btn-danger"
                        style={{ padding: '6px 10px' }}
                        title="Delete Field"
                      >
                        <Trash2 size={14} />
                      </button>
                    </div>
                  </div>

                  <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(160px, 1fr))', gap: '12px', alignItems: 'flex-end' }}>
                    <div>
                      <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Field Label</label>
                      <input
                        className="input-field"
                        style={{ padding: '8px 12px', fontSize: '14px' }}
                        value={f.label}
                        onChange={e => updateField(f.id, 'label', e.target.value)}
                      />
                    </div>

                    <div>
                      <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Field Type</label>
                      <select
                        className="input-field"
                        style={{ padding: '8px 12px', fontSize: '14px' }}
                        value={f.type}
                        onChange={e => updateField(f.id, 'type', e.target.value)}
                      >
                        <option value="text">Text Input</option>
                        <option value="email">Email</option>
                        <option value="tel">Phone / Number</option>
                        <option value="textarea">Long Text (Textarea)</option>
                        <option value="select">Dropdown (Select)</option>
                      </select>
                    </div>

                    <div>
                      <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Placeholder Text</label>
                      <input
                        className="input-field"
                        style={{ padding: '8px 12px', fontSize: '14px' }}
                        value={f.placeholder}
                        onChange={e => updateField(f.id, 'placeholder', e.target.value)}
                      />
                    </div>

                    <div style={{ display: 'flex', alignItems: 'center', gap: '8px', paddingBottom: '10px' }}>
                      <label style={{ display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer' }}>
                        <input
                          type="checkbox"
                          checked={f.required}
                          onChange={e => updateField(f.id, 'required', e.target.checked)}
                          style={{ width: '18px', height: '18px', accentColor: '#0E1013' }}
                        />
                        <span className="font-mono" style={{ fontSize: '12px', fontWeight: 700 }}>Required</span>
                      </label>
                    </div>
                  </div>

                  {f.type === 'select' && (
                    <div>
                      <label className="font-mono" style={{ fontSize: '11px', display: 'block', marginBottom: '4px' }}>Options (comma-separated)</label>
                      <input
                        className="input-field"
                        style={{ padding: '8px 12px', fontSize: '13px' }}
                        placeholder="Option 1, Option 2, Option 3"
                        value={f.options || ''}
                        onChange={e => updateField(f.id, 'options', e.target.value)}
                      />
                    </div>
                  )}
                </div>
              ))}
            </div>
          </div>
        </div>
      )}

      {/* Shareable Link Modal */}
      {shareModalOpen && (
        <div style={{
          position: 'fixed',
          inset: 0,
          background: 'rgba(14,16,19,0.7)',
          backdropFilter: 'blur(4px)',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          zIndex: 1000,
          padding: '20px'
        }}>
          <div className="card" style={{
            maxWidth: '560px',
            width: '100%',
            background: '#FFF',
            border: '3px solid #0E1013',
            boxShadow: '10px 10px 0px #0E1013',
            padding: '32px',
            position: 'relative'
          }}>
            <button
              onClick={() => setShareModalOpen(false)}
              style={{
                position: 'absolute',
                top: '20px',
                right: '20px',
                background: 'transparent',
                border: 'none',
                cursor: 'pointer'
              }}
            >
              <X size={22} />
            </button>

            <span className="font-mono" style={{ fontSize: '11px', letterSpacing: '0.2em', textTransform: 'uppercase', color: '#6D5E2C', display: 'block', marginBottom: '8px' }}>
              03 — Public Form
            </span>
            <h2 style={{ fontSize: '28px', fontWeight: 900, textTransform: 'uppercase', margin: '0 0 8px 0' }}>
              Form Published!
            </h2>
            <p style={{ fontSize: '14px', color: '#555', marginBottom: '24px', lineHeight: 1.5 }}>
              Your form is live and accessible at the public link below. Anyone with this link can fill out and submit responses directly.
            </p>

            <div style={{
              display: 'flex',
              alignItems: 'center',
              background: '#F7F5F0',
              border: '2.5px solid #0E1013',
              padding: '12px 16px',
              gap: '12px',
              marginBottom: '20px',
              boxShadow: '4px 4px 0px #0E1013'
            }}>
              <input
                readOnly
                value={`${window.location.origin}/apply`}
                style={{
                  border: 'none',
                  background: 'transparent',
                  fontFamily: 'IBM Plex Mono, monospace',
                  fontSize: '14px',
                  fontWeight: 600,
                  width: '100%',
                  outline: 'none',
                  color: '#0E1013'
                }}
              />
              <button
                onClick={copyShareLink}
                className="btn-primary"
                style={{
                  padding: '8px 16px',
                  fontSize: '12px',
                  display: 'flex',
                  alignItems: 'center',
                  gap: '6px',
                  whiteSpace: 'nowrap'
                }}
              >
                {copied ? <CheckCircle2 size={16} /> : <Copy size={16} />}
                {copied ? 'Copied!' : 'Copy Link'}
              </button>
            </div>

            <div style={{ display: 'flex', gap: '12px', justifyContent: 'flex-end' }}>
              <a
                href="/apply"
                target="_blank"
                rel="noreferrer"
                className="btn-outline"
                style={{
                  display: 'flex',
                  alignItems: 'center',
                  gap: '6px',
                  textDecoration: 'none',
                  fontSize: '13px',
                  padding: '10px 18px'
                }}
              >
                <ExternalLink size={16} /> Open in New Tab
              </a>
              <button
                className="btn-primary"
                onClick={() => setShareModalOpen(false)}
                style={{ fontSize: '13px', padding: '10px 22px' }}
              >
                Done
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
