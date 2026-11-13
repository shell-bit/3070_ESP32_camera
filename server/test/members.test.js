'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const modulePath = path.join(__dirname, '..', 'members.js');
function ui() {
  assert.ok(fs.existsSync(modulePath), 'Member UI implementation is missing');
  return require(modulePath);
}

test('offline pending never becomes successful before a committed registration', () => {
  const {registrationView} = ui();
  const member = {enabled:1, devices:[{device_id:'a',state:'pending',command_status:'pending',online:false}]};
  assert.equal(registrationView(member,'a').label,'等待相机上线');
  assert.equal(registrationView(member,'a').active,false);
  member.devices[0].online=true;
  assert.equal(registrationView(member,'a').label,'等待相机开始登记');
  member.devices[0].command_status='sampling';
  assert.equal(registrationView(member,'a').label,'正在采样');
  assert.equal(registrationView(member,'b').label,'待登记');
  member.devices[0]={device_id:'a',state:'active',member_version:1,ack_version:1,recognizer_version:'m1',database_version:'d1'};
  assert.equal(registrationView(member,'a').active,true);
  assert.equal(registrationView(member,'b').active,false);
  member.enabled=0;
  assert.equal(registrationView(member,'a').active,false);
});

test('failed and revoked registrations expose retry state and sampling pauses honestly offline', () => {
  const {registrationView} = ui();
  assert.equal(registrationView({enabled:1,devices:[{device_id:'a',state:'failed',error:'Full'}]},'a').detail,'Full');
  assert.equal(registrationView({enabled:1,devices:[{device_id:'a',state:'revoked'}]},'a').active,false);
  assert.equal(registrationView({enabled:1,devices:[{device_id:'a',state:'pending',command_status:'sampling',online:false}]},'a').label,'采样中断／等待相机连接');
});

test('network retry reuses request ID for the same member and device', async () => {
  const {createEnrollmentSender} = ui();
  const requests=[];
  const send=createEnrollmentSender(async (url,method,body) => {
    requests.push({url,method,body});
    if (requests.length===1) throw Error('Connection lost');
    return {command:{id:'one'}};
  }, ()=>'request-one');
  await assert.rejects(send('member','a'));
  assert.deepEqual(await send('member','a'),{command:{id:'one'}});
  assert.equal(requests[0].body.request_id,requests[1].body.request_id);
  assert.equal(requests[1].url,'/api/family/members/member/enrollments');
});

test('member names and device errors render as text, never markup', () => {
  const {renderMember} = ui();
  class Element {
    constructor(tag) { this.tag=tag; this.children=[]; this.textContent=''; }
    append(...children) { this.children.push(...children); }
    addEventListener() {}
    set innerHTML(value) { throw Error('Unsafe HTML sink: '+value); }
  }
  const doc={createElement:tag=>new Element(tag)};
  const dangerous='<img src=x onerror=alert(1)>';
  const card=renderMember(doc,{id:'member',display_name:dangerous,enabled:1,devices:[{device_id:'a',state:'failed',error:dangerous}]},'a',()=>{});
  const nodes=[]; const walk=node=>{nodes.push(node); node.children.forEach(walk);}; walk(card);
  assert.ok(nodes.some(node=>node.textContent===dangerous));
  assert.equal(nodes.filter(node=>node.tag==='img').length,0);
});

test('private exemption records remain exempted and media only gets same-origin text links', () => {
  const {renderExemptedEvent}=ui();
  assert.equal(typeof renderExemptedEvent,'function','Private record renderer is missing');
  class Element {
    constructor(tag) {this.tag=tag;this.children=[];this.textContent='';}
    append(...nodes) {this.children.push(...nodes);}
    addEventListener() {}
    set innerHTML(value) {throw Error('Unsafe HTML sink: '+value);}
  }
  const card=renderExemptedEvent({createElement:tag=>new Element(tag)}, {
    id:'event-1',status:'exempted',description:'<script>bad()</script>',timestamp:'2026-11-11T00:00:00Z',
    frames:[{photo_index:1,photo_url:'/api/family/events/event-1/frames/1'},{photo_index:2,photo_url:'javascript:alert(1)'},{photo_index:3,photo_url:'//evil.example/image'}]
  },()=>{});
  const nodes=[];const walk=node=>{nodes.push(node);node.children.forEach(walk);};walk(card);
  assert.ok(nodes.some(node=>node.textContent==='家庭成员豁免 · exempted'));
  assert.ok(nodes.some(node=>node.textContent==='<script>bad()</script>'));
  assert.equal(nodes.filter(node=>node.tag==='img').length,0);
  assert.equal(nodes.filter(node=>node.tag==='a').length,1);
});
