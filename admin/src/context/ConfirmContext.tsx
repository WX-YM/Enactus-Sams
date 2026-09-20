import React, { createContext, useContext, useState, useCallback, useEffect } from 'react';
import { X, AlertTriangle, Info, Trash2 } from 'lucide-react';

export interface ConfirmOptions {
  title?: string;
  message: string;
  confirmText?: string;
  cancelText?: string;
  type?: 'danger' | 'warning' | 'primary' | 'info';
  isAlert?: boolean;
}

interface ConfirmContextType {
  confirm: (options: ConfirmOptions | string) => Promise<boolean>;
  alert: (options: ConfirmOptions | string) => Promise<void>;
}

const ConfirmContext = createContext<ConfirmContextType>({
  confirm: () => Promise.resolve(false),
  alert: () => Promise.resolve(),
});

export function useConfirm() {
  return useContext(ConfirmContext);
}

export function ConfirmProvider({ children }: { children: React.ReactNode }) {
  const [dialog, setDialog] = useState<{
    isOpen: boolean;
    title: string;
    message: string;
    confirmText: string;
    cancelText: string;
    type: 'danger' | 'warning' | 'primary' | 'info';
    isAlert: boolean;
    resolve: (val: boolean) => void;
  } | null>(null);

  const confirm = useCallback((options: ConfirmOptions | string): Promise<boolean> => {
    return new Promise<boolean>((resolve) => {
      if (typeof options === 'string') {
        setDialog({
          isOpen: true,
          title: 'Confirm Action',
          message: options,
          confirmText: 'Confirm',
          cancelText: 'Cancel',
          type: 'danger',
          isAlert: false,
          resolve,
        });
      } else {
        setDialog({
          isOpen: true,
          title: options.title || (options.type === 'danger' ? 'Confirm Deletion' : 'Confirm Action'),
          message: options.message,
          confirmText: options.confirmText || 'Confirm',
          cancelText: options.cancelText || 'Cancel',
          type: options.type || 'danger',
          isAlert: false,
          resolve,
        });
      }
    });
  }, []);

  const alert = useCallback((options: ConfirmOptions | string): Promise<void> => {
    return new Promise<void>((resolve) => {
      if (typeof options === 'string') {
        setDialog({
          isOpen: true,
          title: 'Notice',
          message: options,
          confirmText: 'OK',
          cancelText: '',
          type: 'info',
          isAlert: true,
          resolve: () => resolve(),
        });
      } else {
        setDialog({
          isOpen: true,
          title: options.title || 'Notice',
          message: options.message,
          confirmText: options.confirmText || 'OK',
          cancelText: '',
          type: options.type || 'info',
          isAlert: true,
          resolve: () => resolve(),
        });
      }
    });
  }, []);

  const handleClose = (result: boolean) => {
    if (dialog) {
      dialog.resolve(result);
    }
    setDialog(null);
  };

  useEffect(() => {
    if (!dialog) return;
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        handleClose(false);
      } else if (e.key === 'Enter') {
        handleClose(true);
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [dialog]);

  return (
    <ConfirmContext.Provider value={{ confirm, alert }}>
      {children}
      {dialog && dialog.isOpen && (
        <div
          style={{
            position: 'fixed',
            inset: 0,
            background: 'rgba(14, 16, 19, 0.75)',
            backdropFilter: 'blur(4px)',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            zIndex: 99999,
            padding: '20px',
          }}
          onClick={() => handleClose(false)}
        >
          <div
            className="card"
            style={{
              maxWidth: '480px',
              width: '100%',
              background: '#FFFFFF',
              border: '3px solid #0E1013',
              boxShadow: '10px 10px 0px #0E1013',
              padding: '32px',
              position: 'relative',
              display: 'flex',
              flexDirection: 'column',
              gap: '16px',
            }}
            onClick={(e) => e.stopPropagation()}
          >
            <button
              onClick={() => handleClose(false)}
              style={{
                position: 'absolute',
                top: '16px',
                right: '16px',
                background: 'transparent',
                border: 'none',
                cursor: 'pointer',
                padding: '4px',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
              }}
              title="Close"
            >
              <X size={20} color="#0E1013" />
            </button>

            <div style={{ display: 'flex', alignItems: 'center', gap: '10px' }}>
              <span
                className="font-mono"
                style={{
                  fontSize: '11px',
                  letterSpacing: '0.2em',
                  textTransform: 'uppercase',
                  fontWeight: 800,
                  background: dialog.type === 'danger' ? '#E53935' : '#FFC629',
                  color: dialog.type === 'danger' ? '#FFFFFF' : '#0E1013',
                  padding: '3px 8px',
                  border: '1.5px solid #0E1013',
                  boxShadow: '2px 2px 0px #0E1013',
                  display: 'inline-flex',
                  alignItems: 'center',
                  gap: '6px',
                }}
              >
                {dialog.type === 'danger' ? (
                  <>
                    <Trash2 size={12} />
                    DESTRUCTIVE ACTION
                  </>
                ) : dialog.type === 'warning' ? (
                  <>
                    <AlertTriangle size={12} />
                    CONFIRMATION REQUIRED
                  </>
                ) : (
                  <>
                    <Info size={12} />
                    SYSTEM NOTICE
                  </>
                )}
              </span>
            </div>

            <h2
              style={{
                margin: 0,
                fontSize: '24px',
                fontWeight: 900,
                textTransform: 'uppercase',
                letterSpacing: '-0.02em',
                lineHeight: 1.15,
                color: '#0E1013',
              }}
            >
              {dialog.title}
            </h2>

            <p
              style={{
                margin: 0,
                fontSize: '15px',
                lineHeight: 1.55,
                color: 'rgba(14, 16, 19, 0.82)',
                fontWeight: 500,
              }}
            >
              {dialog.message}
            </p>

            <div
              style={{
                display: 'flex',
                gap: '12px',
                marginTop: '8px',
              }}
            >
              {!dialog.isAlert && (
                <button
                  type="button"
                  className="btn-outline"
                  style={{
                    flex: 1,
                    justifyContent: 'center',
                    padding: '12px 20px',
                    fontSize: '13px',
                  }}
                  onClick={() => handleClose(false)}
                >
                  {dialog.cancelText || 'Cancel'}
                </button>
              )}
              <button
                type="button"
                className={dialog.type === 'danger' ? 'btn-danger' : 'btn-primary'}
                style={{
                  flex: 1,
                  justifyContent: 'center',
                  padding: '12px 20px',
                  fontSize: '13px',
                }}
                onClick={() => handleClose(true)}
              >
                {dialog.confirmText}
              </button>
            </div>
          </div>
        </div>
      )}
    </ConfirmContext.Provider>
  );
}
