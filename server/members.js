/* Authenticated family enrollment. All device and member strings use textContent. */
(function (root, factory) {
  'use strict';
  const api=factory();
  if (typeof module==='object' && module.exports) module.exports=api;
  else root.document.addEventListener('DOMContentLoaded',()=>api.mount(root));
})(typeof window==='undefined' ? globalThis : window, function () {
  'use strict';

  function registrationView(member,device) {
    const registration=(member.devices || []).find(item=>item.device_id===device);
    if (!member.enabled) return {label:'已停用',detail:'新事件不再获得此成员授权；重新启用后仍需重新登记。',active:false,pending:false};
    if (!registration) return {label:'待登记',detail:'此相机尚未完成登记。',active:false,pending:false};
    if (registration.state==='pending') {
      const sampling=registration.command_status==='sampling';
      return {label:sampling ? (registration.online?'正在采样':'采样中断／等待相机连接') : (registration.online?'等待相机开始登记':'等待相机上线'),
        detail:'120 秒内完成单人、多角度采样。只有相机持久保存成功后才会显示已登记。',active:false,pending:true};
    }
    if (registration.state==='active' && registration.member_version===registration.ack_version && registration.database_version && registration.recognizer_version) {
      return {label:'已登记',detail:'此相机已确认保存。身份匹配仍需通过事件级验证。',active:true,pending:false};
    }
    if (registration.state==='revoked') return {label:'已撤销',detail:registration.error || '云端授权已撤销；相机特征清理由相机联网后确认。',active:false,pending:false};
    if (registration.state==='failed') return {label:'登记失败',detail:registration.error || '相机未完成持久保存，请检查相机后重新登记。',active:false,pending:false};
    return {label:'尚未确认',detail:'缺少完整的相机保存确认，请重新登记。',active:false,pending:false};
  }

  function createEnrollmentSender(api,newId) {
    const requests=new Map();
    return async (memberId,deviceId)=> {
      const key=memberId+'\n'+deviceId;
      if (!requests.has(key)) requests.set(key,newId());
      const result=await api('/api/family/members/'+encodeURIComponent(memberId)+'/enrollments','POST',
        {device_id:deviceId,request_id:requests.get(key)});
      requests.delete(key);
      return result;
    };
  }

  function renderMember(doc,member,device,onAction) {
    const view=registrationView(member,device);
    const make=(tag,text,className)=> {const node=doc.createElement(tag); if (text!==undefined) node.textContent=text; if (className) node.className=className; return node;};
    const card=make('article',undefined,'auth-card member-card');
    const heading=make('div',undefined,'member-heading');
    heading.append(make('h2',member.display_name),make('span',view.label,'member-state'+(view.active?' member-active':'')));
    card.append(heading,make('p',view.detail,'auth-muted'));
    const form=make('form');
    const label=make('label','显示名称'); const input=make('input'); input.value=member.display_name;
    input.name='display_name'; input.maxLength=80; input.required=true; label.append(input);
    const rename=make('button','保存名称'); rename.type='submit';
    form.append(label,rename); form.addEventListener('submit',event=>{event.preventDefault();onAction('rename',member,input.value);});
    card.append(form);
    const actions=make('div',undefined,'auth-nav member-actions');
    const button=(text,action,disabled=false)=> {
      const node=make('button',text); node.type='button'; node.disabled=disabled;
      node.addEventListener('click',()=>onAction(action,member)); actions.append(node);
    };
    button(view.active?'重新登记':'开始登记','enroll',!device || !member.enabled || view.pending);
    button(member.enabled?'停用':'重新启用',member.enabled?'disable':'enable');
    button('删除成员','delete'); card.append(actions);
    return card;
  }

  function renderExemptedEvent(doc,event,onDelete) {
    const make=(tag,text)=>{const node=doc.createElement(tag);if(text!==undefined)node.textContent=text;return node;};
    const card=make('article');card.className='auth-card member-card';
    card.append(make('h3','家庭成员豁免 · exempted'),make('p',event.description || '无行为说明'));
    const date=make('p',event.detected_at || event.timestamp || '时间未记录');date.className='auth-muted';card.append(date);
    const media=make('nav');media.className='auth-nav';
    for(const [index,frame] of (event.frames || []).entries()) {
      // Explicit user-open links only; no image prefetch and no cross-origin URLs.
      if(typeof frame.photo_url!=='string' || !/^\/(?![\/\\])[^\r\n\\]*$/.test(frame.photo_url))continue;
      const link=make('a','查看第 '+(frame.photo_index || index+1)+' 帧');link.href=frame.photo_url;
      link.target='_blank';link.rel='noopener noreferrer';media.append(link);
    }
    const remove=make('button','删除私有照片');remove.type='button';
    remove.addEventListener('click',()=>onDelete(event));
    card.append(media,remove);return card;
  }

  async function mount(win) {
    const doc=win.document, el=id=>doc.getElementById(id), list=el('member-list'), selector=el('member-device');
    if (!list || !selector) return;
    let latest=null,rendered='',eventRendered='',busy=false,loading=false;
    const message=text=>{el('members-message').textContent=text;};
    async function api(path,method='GET',body) {
      const response=await win.fetch(path,{method,headers:{'Content-Type':'application/json'},cache:'no-store',
        body:body===undefined?undefined:JSON.stringify(body)});
      const result=await response.json();
      if (!response.ok) throw Error(typeof result.detail==='string'?result.detail:'操作未完成，请检查输入后重试。');
      return result;
    }
    const enroll=createEnrollmentSender(api,()=>win.crypto.randomUUID());
    async function loadEvents() {
      const container=el('family-events');if(!container)return;
      const result=await api('/api/family/events');
      const key=JSON.stringify(result.items);if(key===eventRendered)return;
      eventRendered=key;container.textContent='';
      if(!result.items.length){const empty=doc.createElement('p');empty.className='auth-muted';empty.textContent='暂无家庭成员豁免记录。身份记录模式不会生成豁免决定。';container.append(empty);}
      for(const event of result.items)container.append(renderExemptedEvent(doc,event,async item=>{
        if(busy || !win.confirm('删除此记录的私有照片？'))return;
        busy=true;
        try{await api('/api/family/events/'+encodeURIComponent(item.id || item.event_id || item.report_id),'DELETE');message('私有照片已删除。');await loadEvents();}
        catch(error){message(error.message || '删除失败，请重试。');}
        finally{busy=false;}
      }));
    }
    function draw(force=false) {
      if (!latest) return;
      const selected=selector.value;
      const key=JSON.stringify([selected,latest.members]);
      if (!force && (key===rendered || list.contains(doc.activeElement))) return;
      rendered=key; list.textContent='';
      if (!latest.members.length) {
        const empty=doc.createElement('p');empty.className='auth-muted';empty.textContent='还没有家庭成员。添加档案后，选择相机开始登记。';list.append(empty);
      }
      for (const member of latest.members) list.append(renderMember(doc,member,selected,action));
    }
    async function reload(force=false) {
      if (loading) return;
      loading=true;
      try {
        latest=await api('/api/family/members');
        const selected=selector.value;
        if (JSON.stringify([...selector.options].map(option=>option.value))!==JSON.stringify(latest.devices)) {
          selector.textContent='';
          for (const device of latest.devices) {const option=doc.createElement('option');option.value=device;option.textContent=device;selector.append(option);}
          if (latest.devices.includes(selected)) selector.value=selected;
        }
        selector.disabled=!latest.devices.length || busy;
        el('device-help').textContent=latest.devices.length?'每台相机单独登记；保持相机联网，并让一人面对镜头。':'账户尚未分配相机，请联系管理员。';
        el('identity-mode').textContent=latest.exemptions_enabled
          ?'部分相机已开启家庭成员豁免：'+latest.exemption_devices.join('、')+'。仅对经验证的成员本人行为生效。'
          :'当前为身份记录模式，家庭成员报警豁免默认关闭。登记成功不会自动关闭报警。';
        draw(force);
        await loadEvents().catch(()=>{el('family-events-message').textContent='无法刷新豁免记录，稍后重试。';});
      } finally {loading=false;}
    }
    async function action(kind,member,value) {
      if (busy) return;
      if (kind==='delete' && !win.confirm('删除“'+member.display_name+'”？授权会立即撤销，历史记录中的成员引用会保留。')) return;
      busy=true;selector.disabled=true;
      for (const button of list.querySelectorAll('button')) button.disabled=true;
      message('正在处理…');
      try {
        const path='/api/family/members/'+encodeURIComponent(member.id);
        if (kind==='enroll') {await enroll(member.id,selector.value);message('任务已创建，等待相机采样和保存确认。');}
        else if (kind==='rename') {await api(path,'PATCH',{display_name:value});message('名称已更新。');}
        else if (kind==='delete') {await api(path,'DELETE');message('授权已撤销，成员已删除。');}
        else {await api(path,'PATCH',{enabled:kind==='enable'});message(kind==='enable'?'已启用，请在选定相机上重新登记。':'已停用，新事件不再获得此成员授权。');}
        await reload(true);
      } catch (error) {message(error.message || '连接失败，请重试。');}
      finally {busy=false;selector.disabled=!latest?.devices.length;draw(true);}
    }
    selector.addEventListener('change',()=>draw(true));
    el('member-create').addEventListener('submit',async event=> {
      event.preventDefault();if(busy)return;busy=true;
      const form=event.currentTarget,button=form.querySelector('button');button.disabled=true;
      try {await api('/api/family/members','POST',{display_name:form.elements.display_name.value});form.reset();message('成员档案已创建，请选择相机开始登记。');await reload(true);}
      catch(error){message(error.message || '连接失败，请重试。');}
      finally{busy=false;button.disabled=false;selector.disabled=!latest?.devices.length;}
    });
    try {
      const user=await win.authReady;if(!user)return;
      if(user.role!=='viewer'){win.location.replace('/admin');return;}
      await reload(true);
      win.setInterval(()=>{if(!busy && !doc.hidden)reload().catch(()=>message('连接中断，当前显示的是上次确认状态。'));},3000);
    } catch(error){message(error.message || '无法加载成员，请刷新页面。');}
  }
  return {registrationView,createEnrollmentSender,renderMember,renderExemptedEvent,mount};
});
