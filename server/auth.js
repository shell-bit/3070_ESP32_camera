/* Same-origin cookie sessions. Credentials and CSRF tokens are never persisted in browser storage. */
(() => {
  'use strict';
  const originalFetch = window.fetch.bind(window);
  const onLogin = location.pathname === '/login';
  const el = id => document.getElementById(id);
  let me = null, resetId = null;
  window.authReady = onLogin ? Promise.resolve(null) : originalFetch('/api/auth/me', {credentials:'same-origin', cache:'no-store'})
    .then(async response => {
      if (response.status === 401) { location.replace('/login'); return null; }
      if (!response.ok) throw Error('Could not check your session. Reload this page.');
      me = await response.json(); return me;
    });
  window.fetch = async (input, options = {}) => {
    const url = new URL(typeof input === 'string' ? input : input.url, location.href);
    if (url.origin !== location.origin) return originalFetch(input, options);
    if (!onLogin) await window.authReady;
    const headers = new Headers(options.headers || (input instanceof Request ? input.headers : undefined));
    const method = (options.method || (input instanceof Request ? input.method : 'GET')).toUpperCase();
    if (!['GET','HEAD','OPTIONS'].includes(method) && me) headers.set('X-CSRF-Token', me.csrf);
    const response = await originalFetch(input, {...options, headers, credentials:'same-origin'});
    if (response.status === 401 && !['/api/auth/login','/api/auth/password'].includes(url.pathname)) location.replace('/login');
    return response;
  };
  async function api(path, method = 'GET', data) {
    const response = await fetch(path, {method, headers:{'Content-Type':'application/json','X-Sentinel-Request':'1'}, body:data === undefined ? undefined : JSON.stringify(data)});
    const result = await response.json();
    if (!response.ok) throw Error(typeof result.detail === 'string' ? result.detail : 'Please check the fields and try again.');
    return result;
  }
  function message(id, text, good = false) { const node=el(id); node.textContent=text; node.classList.toggle('success',good); }
  function form(id, status, action) {
    const node=el(id); if (!node) return;
    node.addEventListener('submit',async event => {
      event.preventDefault(); const button=node.querySelector('button[type=submit]'); button.disabled=true;
      message(status,'');
      try { await action(Object.fromEntries(new FormData(node)),node); }
      catch (error) { message(status,error.message || 'Connection failed. Try again.'); }
      finally { button.disabled=false; }
    });
  }
  async function loadUsers() {
    const data=await api('/api/admin/users'); const list=el('user-list'); list.textContent='';
    for (const user of data.users) {
      const row=document.createElement('div'); row.className='user-row';
      const label=document.createElement('div'); const name=document.createElement('strong'); name.textContent=user.username;
      const detail=document.createElement('small'); detail.textContent=(user.role==='admin'?'Administrator':'User')+' · '+(user.active?'Active':'Disabled')+(user.id===me.id?' · You':'');
      label.append(name,detail); const actions=document.createElement('div'); actions.className='auth-nav';
      function button(text, action) {
        const b=document.createElement('button'); b.type='button'; b.textContent=text;
        b.onclick=async()=> { b.disabled=true; try { await action(); } catch(error) {message('users-message',error.message);} finally {b.disabled=false;} }; actions.append(b);
      }
      button('Reset password',()=> { resetId=user.id; el('reset-name').textContent=user.username; el('reset-form').reset(); el('reset-message').textContent=''; el('reset-dialog').showModal(); });
      button(user.active?'Disable':'Enable',async()=> {
        if (!confirm((user.active?'Disable ':'Enable ')+user.username+'? Their current sessions will end.')) return;
        await api('/api/admin/users/'+user.id,'PATCH',{active:!user.active});
        if (user.id===me.id) location.replace('/login'); else await loadUsers();
      });
      button(user.role==='admin'?'Make user':'Make admin',async()=> {
        const role=user.role==='admin'?'viewer':'admin';
        if (!confirm('Change '+user.username+' to '+role+'?')) return;
        await api('/api/admin/users/'+user.id,'PATCH',{role});
        if (user.id===me.id) location.replace('/login'); else await loadUsers();
      });
      row.append(label,actions); list.append(row);
    }
    document.dispatchEvent(new Event('users-updated'));
  }
  document.addEventListener('DOMContentLoaded',async()=> {
    form('login-form','login-message',async(data,node)=> { await api('/api/auth/login','POST',data); node.reset(); location.replace('/'); });
    if (onLogin) return;
    try { await window.authReady; if (!me) return;
      if (el('auth-user')) el('auth-user').textContent=me.username+' · '+(me.role==='admin'?'Administrator':'User');
      if (el('review-section')) el('review-section').hidden=me.role!=='viewer';
      if (el('account-home') && me.role==='admin') { el('account-home').href='/admin'; el('account-home').textContent='← Administration'; }
      if (el('logout')) el('logout').onclick=async()=> { try { await api('/api/auth/logout','POST',{}); location.replace('/login'); } catch(error) {alert('Could not sign out: '+error.message);} };
      form('password-form','password-message',async(data,node)=> { await api('/api/auth/password','POST',data); node.reset(); alert('Password changed. Please sign in again.'); location.replace('/login'); });
      if (el('admin-panel') && me.role==='admin') {
        el('admin-panel').hidden=false; await loadUsers();
        form('create-user-form','create-message',async(data,node)=> { await api('/api/admin/users','POST',data); node.reset(); message('create-message','Account created. Share the initial password privately.',true); await loadUsers(); });
        el('cancel-reset').onclick=()=>el('reset-dialog').close();
        form('reset-form','reset-message',async(data,node)=> { await api('/api/admin/users/'+resetId,'PATCH',data); node.reset(); el('reset-dialog').close(); if(resetId===me.id) location.replace('/login'); else message('users-message','Password reset. Share the new password privately.',true); });
      }
    } catch(error) { alert(error.message); }
  });
})();
