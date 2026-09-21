export async function authFetch(url: string, options: RequestInit = {}): Promise<Response> {
  const token = localStorage.getItem('admin_token');
  const headers = new Headers(options.headers || {});
  
  if (token) {
    headers.set('Authorization', `Bearer ${token}`);
  }
  
  const res = await fetch(url, {
    ...options,
    headers,
  });

  if (res.status === 401) {
    // Session unauthorized or expired
    localStorage.removeItem('admin_auth');
    localStorage.removeItem('admin_token');
    localStorage.removeItem('admin_role');
    localStorage.removeItem('admin_email');
    localStorage.removeItem('admin_team');
    localStorage.removeItem('admin_permissions');
    localStorage.removeItem('admin_session_expires_at');
    localStorage.removeItem('admin_last_activity');
    window.location.hash = '#/login';
    window.location.reload();
  }

  return res;
}
