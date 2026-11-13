(() => {
 'use strict';
 const el=id=>document.getElementById(id);
 let loading=false;
 async function api(path,options={}) {
  const r=await fetch(path,options); const data=await r.json();
  if(!r.ok) throw Error(typeof data.detail==='string'?data.detail:'Check the camera ID and selected user.');
  return data;
 }
 function owners(select,users,current) {
  select.textContent=''; const none=document.createElement('option'); none.value='';none.textContent='Unassigned';select.append(none);
  for(const user of users.filter(u=>u.active&&u.role==='viewer')) {const o=document.createElement('option');o.value=user.id;o.textContent=user.username;select.append(o);}
  if(current!=null&&!users.some(u=>u.id===current&&u.active&&u.role==='viewer')) {const o=document.createElement('option');o.value=current;o.textContent='Owner unavailable — choose another user';o.disabled=true;select.append(o);}
  select.value=current==null?'':String(current);
 }
 async function assign(device,owner,include) {
  const data=await api('/api/admin/devices',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({device_id:device,owner_id:owner===''?null:Number(owner),include_unassigned:include})});
  el('token-panel').hidden=false; el('issued-device').value=data.device_id;el('issued-token').value=data.upload_token;
  el('token-message').textContent='Assignment saved. '+data.imported+' unassigned report(s) imported.';
  await load();el('token-panel').scrollIntoView({behavior:'smooth',block:'center'});
 }
 async function load() {
  if(loading)return;loading=true;
  try {
   const [u,d]=await Promise.all([api('/api/admin/users'),api('/api/admin/devices')]);
   const list=el('devices-list');list.textContent='';owners(el('new-device-owner'),u.users,null);
   if(!d.devices.length) {const p=document.createElement('p');p.textContent='No cameras yet. Register a camera below.';list.append(p);}
   for(const camera of d.devices) {
    const card=document.createElement('article');card.className='camera-card';const title=document.createElement('h3');title.textContent=camera.device;
    const detail=document.createElement('p');detail.className='auth-muted';const owner=u.users.find(x=>x.id===camera.owner_id);
    detail.textContent=(owner?'Owner: '+owner.username:'Unassigned')+' · '+camera.reports+' total records · '+camera.unassigned+' unassigned'+(camera.registered?'':' · Legacy camera');
    const form=document.createElement('form'); const ownerLabel=document.createElement('label');ownerLabel.textContent='Assign to';const select=document.createElement('select');owners(select,u.users,camera.owner_id);ownerLabel.append(select);
    const checkLabel=document.createElement('label');checkLabel.className='check-label';const check=document.createElement('input');check.type='checkbox';check.disabled=!camera.unassigned;
    const text=document.createElement('span');text.textContent='Also give this user the '+camera.unassigned+' unassigned historical records. Existing users’ reports will not move.';checkLabel.append(check,text);
    const button=document.createElement('button');button.type='submit';button.textContent='Save assignment & generate token';
    form.append(ownerLabel,checkLabel,button);form.onsubmit=async event=> {event.preventDefault();
     if(check.checked&&!select.value){el('devices-message').textContent='Select a user to import history.';return;}
     if(!confirm('Save ownership for '+camera.device+' and replace its upload token? You must update this camera’s firmware token.'+(check.checked?' Its unassigned history will become visible to the selected user.':'')))return;
     button.disabled=true;try{await assign(camera.device,select.value,check.checked);}catch(error){el('devices-message').textContent=error.message;}finally{button.disabled=false;}
    };card.append(title,detail,form);list.append(card);
   }
   el('devices-message').textContent='';
  }catch(error){el('devices-message').textContent=error.message;}finally{loading=false;}
 }
 document.addEventListener('DOMContentLoaded',async()=>{
  const me=await window.authReady;if(!me||me.role!=='admin')return;
  el('devices-refresh').onclick=load;document.addEventListener('users-updated',load);
  el('new-device-form').onsubmit=async event=>{event.preventDefault();const form=event.currentTarget;const button=form.querySelector('button');button.disabled=true;
   try{const values=new FormData(form);const id=values.get('device_id');const data=await api('/api/admin/devices');if(data.devices.some(d=>d.device===id))throw Error('This camera already exists. Use its assignment card above.');await assign(id,values.get('owner_id'),false);form.reset();}
   catch(error){el('devices-message').textContent=error.message;}finally{button.disabled=false;}
  };
  el('copy-token').onclick=async()=>{try{await navigator.clipboard.writeText(el('issued-token').value);el('copy-token').textContent='Copied';}catch{el('issued-token').focus();el('issued-token').select();}};
  await load();
 });
})();
