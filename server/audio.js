(() => {
  'use strict';
  const section = document.createElement('section');
  section.className = 'gallery-section panel'; section.style.padding = '22px';
  section.innerHTML = `<h2>Camera audio</h2>
    <p class="notice">Recordings accompany a photo burst. Audio is for listening; the AI currently analyses photos only. Recordings are kept for 7 days.</p>
    <div style="display:flex;gap:12px;flex-wrap:wrap;align-items:center;margin:18px 0">
      <label>Camera <select id="audio-camera" style="font:inherit;padding:9px;max-width:100%"></select></label>
      <button id="talk-start" type="button" disabled>Turn on microphone</button>
      <button id="talk-stop" type="button" hidden>Stop talking</button>
    </div>
    <p id="audio-ready" role="status">Checking camera availability…</p>
    <p id="talk-status" role="status" class="notice">Your microphone is off. Talking uses short audio chunks, so expect a few seconds of delay.</p>
    <p id="talk-delivery" class="notice" role="status"></p>
    <p class="notice">A sleeping camera cannot receive speech. Trigger the PIR, wait for uploads, then talk during its 90-second listening window. Talking stops after two minutes at most.</p>
    <h3>Event recordings</h3><div id="audio-recordings">Loading recordings…</div>
    <button id="audio-more" type="button" hidden style="margin-top:12px">Older recordings</button>`;
  document.querySelector('main').append(section);
  const $ = id => document.getElementById(id);
  let devices = [], sid = null, stream = null, context = null, node = null, source = null;
  let sending = false, sequence = 0, stopping = false, starting = false, cancelled = false, timer;
  let next = null, loading = false; const recordings = new Map();
  async function api(url, options = {}) {
    const response = await fetch(url, {...options, signal: options.signal || AbortSignal.timeout(6000)});
    if (!response.ok) {
      let message = 'Connection failed. Try again.';
      try {const data = await response.json(); if (typeof data.detail === 'string') message = data.detail;} catch {}
      throw Error(message);
    }
    return response.status === 204 ? {} : response.json();
  }
  function controls() {
    const ready = devices.find(d => d.id === $('audio-camera').value)?.available;
    $('talk-start').disabled = !ready || !!sid || starting || stopping;
    $('talk-start').hidden = !!sid || starting;
    $('talk-stop').hidden = !sid && !starting;
    $('audio-camera').disabled = !!sid || starting;
    $('audio-ready').textContent = devices.length ? (ready ? 'Ready for speaker audio.' : 'Camera asleep, capturing, uploading or offline.') : 'No camera assigned. Ask the administrator to assign your camera.';
  }
  async function refreshDevices() {
    try {
      devices = (await api('/api/audio/devices')).devices;
      const old = $('audio-camera').value;
      if ([...$('audio-camera').options].map(o => o.value).join() !== devices.map(d => d.id).join()) {
        $('audio-camera').replaceChildren(...devices.map(d => new Option(d.id, d.id)));
        if (devices.some(d => d.id === old)) $('audio-camera').value = old;
      }
      controls();
      if (sid && !devices.find(d => d.id === $('audio-camera').value)?.available) await stop('Camera is unavailable. Microphone turned off.');
      if (sid) {
        const delivery = await api('/api/audio/talk/' + sid + '/status');
        $('talk-delivery').textContent = delivery.chunks.played ? 'Speaker playback confirmed.' : 'Waiting for speaker playback confirmation…';
        if (delivery.chunks.failed) await stop('The camera could not play the audio. Check the amplifier wiring and Serial Monitor.');
      }
    } catch { devices = []; controls(); if (sid) await stop('Connection lost. Microphone turned off.'); }
  }
  async function stop(message = 'Microphone off. Unplayed speech has been cancelled.') {
    if (stopping) return;
    cancelled = true; stopping = true; clearTimeout(timer);
    const old = sid; sid = null;
    if (node) {node.port.onmessage = null; node.disconnect(); node = null;}
    if (source) {source.disconnect(); source = null;}
    if (stream) {stream.getTracks().forEach(t => t.stop()); stream = null;}
    if (context) {const ctx = context; context = null; await ctx.close().catch(() => {});}
    controls();
    if (old) await api('/api/audio/talk/' + old + '/stop', {method: 'POST', keepalive: true}).catch(() => {});
    $('talk-status').textContent = message; stopping = false; controls();
  }
  async function start() {
    if (starting || sid) return;
    starting = true; cancelled = false; controls();
    try {
      if (!window.isSecureContext || !navigator.mediaDevices?.getUserMedia) throw Error('Open the HTTPS dashboard in Chrome or another browser that supports microphone access.');
      $('talk-status').textContent = 'Allow microphone access to start talking.';
      // Start the audio context in the user gesture for browser autoplay rules.
      context = new AudioContext({sampleRate: 16000}); await context.resume();
      if (context.sampleRate !== 16000) throw Error('This browser cannot record at 16000 Hz. Try Chrome.');
      stream = await navigator.mediaDevices.getUserMedia({audio: {channelCount: 1, echoCancellation: true, noiseSuppression: true}, video: false});
      if (cancelled) {stream.getTracks().forEach(t=>t.stop());stream=null;return;}
      const result = await api('/api/audio/talk/start', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({device_id: $('audio-camera').value})});
      sid = result.session_id; $('talk-delivery').textContent = '';
      if (cancelled) {await stop();return;}
      await context.audioWorklet.addModule('/audio-worklet.js');
      if (cancelled || !context) return;
      source = context.createMediaStreamSource(stream); node = new AudioWorkletNode(context, 'camera-mic');
      source.connect(node); node.connect(context.destination); sequence = 0;
      node.port.onmessage = async event => {
        if (!sid || stopping) return;
        if (sending) {await stop('Connection is too slow for talking. Microphone turned off.');return;}
        sending = true; const current = sid;
        try {
          await api('/api/audio/talk/' + current, {method: 'POST', headers: {'Content-Type': 'application/octet-stream', 'X-Audio-Sequence': String(sequence++)}, body: event.data});
          if (sid === current) $('talk-status').textContent = 'Microphone on — sending your voice to the camera speaker. Press Stop talking when finished.';
        } catch (error) {if (sid === current) await stop(error.message);}
        finally {sending = false;}
      };
      stream.getAudioTracks()[0].onended = () => stop('Microphone disconnected.');
      timer = setTimeout(() => stop('Two-minute talking limit reached. Start again if the camera is ready.'), 115000);
      $('talk-status').textContent = 'Microphone on — speak now. Audio will arrive with a short delay.';
    } catch (error) {await stop(error.name === 'NotAllowedError' ? 'Microphone permission was denied. Allow it in your browser settings and try again.' : error.message);}
    finally {starting = false;controls();}
  }
  async function loadRecordings(older = false) {
    if (loading) return; loading = true;
    try {
      const data = await api('/api/audio/recordings' + (older && next ? '?before=' + next : ''));
      if (older || recordings.size === 0) next = data.next_cursor;
      for (const r of data.items) {
        if (recordings.has(r.id)) continue;
        const card = document.createElement('article');card.className = 'photo-info';card.dataset.audioId = r.id;
        card.style.borderBottom = '1px solid var(--line)';
        const title = document.createElement('p');title.textContent = `${new Date(r.timestamp).toLocaleString()} · ${r.device_id} · ${r.duration.toFixed(1)} seconds`;
        const event = document.createElement('p');event.className = 'notice';event.textContent = 'Photo burst: ' + r.event_id;
        const audio = document.createElement('audio'); audio.controls = true; audio.preload = 'none'; audio.src = r.url; audio.style.cssText = 'width:100%;max-width:450px;margin-top:10px';
        card.append(title, event, audio);
        if (r.report_ids?.length) {
          const link = document.createElement('a'); link.href = '/?report=' + encodeURIComponent(r.report_ids[0]);
          link.textContent = 'View related activity report'; link.style.display = 'block'; card.append(link);
        }
        recordings.set(r.id, {card, time: r.timestamp});
      }
      const list = $('audio-recordings');
      if (recordings.size) {
        // Reuse audio elements so polling does not restart playback.
        if (!list.querySelector('article')) list.textContent = '';
        for (const r of [...recordings.values()].sort((a,b)=>b.time.localeCompare(a.time))) if (!r.card.isConnected) {
          const after = [...list.children].find(c => (recordings.get(c.dataset.audioId)?.time || '') < r.time);
          list.insertBefore(r.card, after || null);
        }
      } else list.textContent = 'No event audio uploaded yet.';
      $('audio-more').hidden = !next;
    } catch {if (!recordings.size) $('audio-recordings').textContent = 'Could not load recordings. Retrying shortly…';}
    finally {loading = false;}
  }
  $('talk-start').onclick = start; $('talk-stop').onclick = () => stop();
  $('audio-camera').onchange = controls; $('audio-more').onclick = () => loadRecordings(true);
  document.addEventListener('visibilitychange', () => {if (document.hidden) stop('Microphone turned off because the dashboard is in the background.');});
  window.addEventListener('pagehide', () => stop());
  $('logout')?.addEventListener('click', () => stop());
  refreshDevices();loadRecordings();setInterval(refreshDevices, 3000);setInterval(loadRecordings, 10000);
})();
