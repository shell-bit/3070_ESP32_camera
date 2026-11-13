// Review key stays in memory only; never added to photo URLs or localStorage
(() => {
  'use strict';
  const el = id => document.getElementById(id);
  const cards = new Map();
  const photoQueue = [];
  let key = '', session = 0, listRequest = null, activePhotos = 0;
  let batch = null;
  const toolbar = document.createElement('div');
  toolbar.className = 'review-bulk';
  const selectLabel = document.createElement('label');
  selectLabel.className = 'review-select';
  const selectAll = document.createElement('input');
  selectAll.type = 'checkbox';
  selectAll.id = 'review-select-all';
  const selectText = document.createElement('span');
  selectText.textContent = 'Select all';
  selectLabel.append(selectAll, selectText);
  const selectionCount = document.createElement('span');
  selectionCount.setAttribute('role', 'status');
  const deleteSelected = document.createElement('button');
  deleteSelected.type = 'button';
  deleteSelected.id = 'review-delete-selected';
  deleteSelected.textContent = 'Delete selected';
  const bulkStatus = document.createElement('p');
  bulkStatus.className = 'notice';
  bulkStatus.setAttribute('role', 'status');
  bulkStatus.textContent = 'Select all applies to the events currently listed in Manual review. Each action includes every photo in the selected event.';
  toolbar.append(selectLabel, selectionCount, deleteSelected, bulkStatus);
  el('review-list').before(toolbar);

  function updateSelection() {
    const available = [...cards.values()].filter(state => !state.busy && !state.reserved);
    const selected = [...cards.values()].filter(state => state.selected.checked);
    const availableSelected = available.filter(state => state.selected.checked);
    selectionCount.textContent = selected.length + ' selected';
    selectAll.checked = available.length > 0 && availableSelected.length === available.length;
    selectAll.indeterminate = availableSelected.length > 0 && availableSelected.length < available.length;
    selectAll.disabled = !!batch || !available.length;
    deleteSelected.disabled = !!batch || !availableSelected.length;
  }

  function setCardLock(state) {
    const locked = state.busy || state.reserved;
    state.note.disabled = state.selected.disabled = locked;
    for (const button of state.buttons) button.disabled = locked || (button.datasetAction === 'retry' && !state.inputComplete);
    updateSelection();
  }

  selectAll.addEventListener('change', () => {
    if (batch) return;
    for (const state of cards.values()) {
      if (!state.busy && !state.reserved) state.selected.checked = selectAll.checked;
    }
    updateSelection();
  });

  deleteSelected.addEventListener('click', async () => {
    if (batch) return;
    // Freeze the selection: events arriving later are never included implicitly.
    const targets = [...cards.values()].filter(state => state.selected.checked && !state.busy && !state.reserved);
    if (!targets.length || !confirm('Permanently delete ' + targets.length +
      ' selected event(s) and all their photos from Manual review? This cannot be undone.')) return;
    const run = { total: targets.length, completed: 0, deleted: 0, failed: 0, next: 0 };
    batch = run;
    invalidateList();
    el('review-refresh').disabled = el('review-key').disabled = true;
    for (const state of targets) {
      state.reserved = true;
      state.status.textContent = 'Waiting to delete…';
      setCardLock(state);
    }
    function progress() {
      bulkStatus.textContent = 'Deleting selected events: ' + run.completed + ' / ' + run.total +
        ' processed · ' + run.deleted + ' deleted · ' + run.failed + ' unconfirmed';
    }
    progress();
    async function worker() {
      while (run.next < targets.length) {
        const state = targets[run.next++];
        const success = await act(state, 'dismiss', true);
        if (success) run.deleted++; else run.failed++;
        run.completed++;
        progress();
      }
    }
    try {
      // Bounded concurrency avoids flooding the server and the browser connection pool.
      await Promise.all([worker(), worker()]);
      bulkStatus.textContent = 'Deleted ' + run.deleted + ' of ' + run.total + ' selected event(s) and their photos.' +
        (run.failed ? ' ' + run.failed + ' could not be confirmed. Refresh to check their status before retrying.' : '');
    } finally {
      for (const state of targets) { state.reserved = false; setCardLock(state); }
      batch = null;
      el('review-refresh').disabled = el('review-key').disabled = false;
      updateSelection();
      pumpPhotos();
      void refresh(true);
      updateDashboard();
    }
  });
  updateSelection();

  // Keep the timeout active while reading JSON or image data as well.
  async function call(url, options = {}, type = 'none', controller = new AbortController()) {
    const timer = setTimeout(() => controller.abort(), 15000);
    try {
      const response = await fetch(url, {
        ...options,
        signal: controller.signal,
        cache: 'no-store',
        headers: { 'X-Review-Token': key, ...options.headers }
      });
      if (!response.ok) {
        let message = 'HTTP ' + response.status;
        try {
          const data = await response.json();
          if (typeof data.detail === 'string') message = data.detail;
        } catch {}
        throw Error(message);
      }
      if (type === 'json') return await response.json();
      if (type === 'blob') return await response.blob();
      // Actions only need the HTTP success status (also supports 204).
      if (response.body) await response.body.cancel();
    } finally {
      clearTimeout(timer);
    }
  }

  function current(state) {
    return state.session === session && cards.get(state.id) === state;
  }

  function removeCard(state) {
    for (const photo of state.photos.values()) removePhoto(photo);
    state.photos.clear();
    state.card.remove();
    cards.delete(state.id);
  }

  function removePhoto(photo) {
    photo.controller?.abort();
    if (photo.url) URL.revokeObjectURL(photo.url);
    photo.figure.remove();
  }

  function currentPhoto(state, photo) {
    return current(state) && state.photos.get(photo.index) === photo;
  }

  function updateCount() {
    updateSelection();
    el('review-count').textContent = cards.size + ' awaiting review';
    const list = el('review-list');
    if (cards.size) {
      list.querySelector('.empty')?.remove();
    } else if (!list.querySelector('.empty')) {
      const empty = document.createElement('p');
      empty.className = 'empty';
      empty.textContent = 'No photos awaiting manual review.';
      list.append(empty);
    }
  }

  function invalidateList() {
    listRequest?.abort();
    listRequest = null;
    el('review-refresh').disabled = !!batch;
  }

  // Limit image downloads so action requests have room to run immediately.
  function pumpPhotos() {
    if (batch) return;
    while (activePhotos < 2 && photoQueue.length) {
      const { state, photo } = photoQueue.shift();
      if (!currentPhoto(state, photo)) continue;
      activePhotos++;
      void loadPhoto(state, photo).finally(() => {
        activePhotos--;
        pumpPhotos();
      });
    }
  }

  async function loadPhoto(state, photo) {
    photo.controller = new AbortController();
    try {
      // Construct the route from the ID, not arbitrary response URLs.
      const route = '/api/review/' + encodeURIComponent(state.id) + '/photo' +
        (photo.index === null ? '' : '?frame_index=' + photo.index);
      const blob = await call(route, {}, 'blob', photo.controller);
      if (!currentPhoto(state, photo)) return;
      photo.url = URL.createObjectURL(blob);
      photo.image.src = photo.url;
      photo.view.href = photo.url;
      photo.image.hidden = photo.view.hidden = false;
      photo.status.textContent = '';
    } catch (error) {
      if (currentPhoto(state, photo)) {
        photo.status.textContent = 'Photo unavailable: ' + error.message;
      }
    } finally {
      photo.queued = false;
      photo.controller = null;
    }
  }

  function queuePhoto(state, photo) {
    if (photo.url || photo.queued) return;
    photo.queued = true;
    photoQueue.push({ state, photo });
    pumpPhotos();
  }

  function syncPhotos(state, item) {
    const grouped = Array.isArray(item.frames), indices = new Set();
    const frames = grouped ? item.frames.filter(frame => {
      if (!frame || !Number.isInteger(frame.photo_index) || frame.photo_index < 1 || frame.photo_index > 5 || indices.has(frame.photo_index)) return false;
      indices.add(frame.photo_index); return true;
    }).sort((a, b) => a.photo_index - b.photo_index) : [{ photo_index: null, received_at: item.received_at }];
    const expected = Number.isInteger(item.expected_frames) && item.expected_frames >= 1 && item.expected_frames <= 5 ? item.expected_frames : frames.length;
    const missing = Array.isArray(item.missing_indices) ? item.missing_indices.filter(index => Number.isInteger(index) && index >= 1 && index <= expected) : [];
    state.inputComplete = item.input_complete !== false && (!grouped || (!missing.length && frames.length === expected));
    state.grouped = grouped;
    state.summary.textContent = grouped ? frames.length + ' / ' + expected + ' photos' + (missing.length ? ' · Missing: ' + missing.join(', ') : '') +
      '. Decisions apply to the entire event.' + (!state.inputComplete ? ' Retry AI requires all expected photos.' : '') : '';
    state.selectionText.textContent = grouped ? 'Select event' : 'Select photo';
    state.selected.setAttribute('aria-label', 'Select ' + (grouped ? 'event' : 'photo') + ' received ' + new Date(item.received_at).toLocaleString());
    const keys = new Set(frames.map(frame => frame.photo_index));
    for (const [index, photo] of state.photos) if (!keys.has(index)) { removePhoto(photo); state.photos.delete(index); }
    for (const frame of frames) {
      let photo = state.photos.get(frame.photo_index);
      if (!photo) {
        const figure = document.createElement('figure'); figure.className = 'review-frame';
        const image = document.createElement('img'); image.alt = grouped ? 'Event photo ' + frame.photo_index + ' awaiting manual review' : 'Photo awaiting manual review'; image.hidden = true;
        const caption = document.createElement('figcaption');
        const view = document.createElement('a'); view.target = '_blank'; view.rel = 'noopener'; view.textContent = 'Open full-size photo'; view.hidden = true;
        const status = document.createElement('p'); status.textContent = 'Loading photo…'; status.setAttribute('role', 'status');
        figure.append(image, caption, view, status);
        photo = { index: frame.photo_index, figure, image, caption, view, status, url: null, queued: false, controller: null };
        state.photos.set(frame.photo_index, photo);
      }
      photo.caption.textContent = (grouped ? 'Photo ' + frame.photo_index : 'Photo') +
        (frame.captured_at ? ' · Captured: ' + new Date(frame.captured_at).toLocaleString() : '') +
        (frame.received_at ? ' · Received: ' + new Date(frame.received_at).toLocaleString() : '');
      state.photoList.append(photo.figure);
      queuePhoto(state, photo);
    }
    setCardLock(state);
  }

  function updateDashboard() {
    // Dashboard polling must not hold a review card's action lock.
    const callbacks = [
      typeof poll === 'function' ? poll : null,
      typeof loadHistory === 'function' ? loadHistory : null,
      typeof pipelineStatus === 'function' ? pipelineStatus : null
    ];
    for (const callback of callbacks) {
      if (callback) void Promise.resolve().then(() => callback()).catch(() => {});
    }
  }

  async function act(state, action, fromBatch = false) {
    if (!current(state) || state.busy || (state.reserved && !fromBatch)) return false;
    if (action === 'retry' && !state.inputComplete) return false;
    const description = state.note.value.trim();
    if (action === 'keep' && !description) {
      state.status.textContent = 'Please add a description first.';
      state.note.focus();
      return;
    }
    if (action === 'dismiss' && !fromBatch && !confirm(state.grouped ? 'Dismiss this event and permanently delete all its retained photos?' : 'Dismiss this event and permanently delete its retained photo?')) return false;

    // Lock only this event; other cards and image loading remain independent.
    state.busy = true;
    setCardLock(state);
    state.status.textContent = action === 'retry' ? 'Queuing AI analysis…' : 'Saving…';
    invalidateList(); // A pre-action response must not resurrect removed cards.
    try {
      await call('/api/review/' + encodeURIComponent(state.id), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action, description })
      });
      if (!current(state)) return false;
      invalidateList();
      removeCard(state);
      updateCount();
      if (!fromBatch) el('review-message').textContent = action === 'keep'
        ? 'Saved as a human-reviewed alert in gallery and history.'
        : action === 'retry'
          ? 'Queued for AI analysis. Keep the worker running.'
          : state.grouped ? 'Event dismissed and all its photos deleted.' : 'Photo dismissed and deleted.';
      // Update in the background, without waiting for photos or other sections.
      if (!batch) { void refresh(true); updateDashboard(); }
      return true;
    } catch (error) {
      if (current(state)) {
        state.status.textContent = 'Could not confirm action: ' +
          (error.name === 'AbortError' ? 'request timed out' : error.message) +
          '. Refresh to check the result before trying again.';
      }
      return false;
    } finally {
      state.busy = false;
      setCardLock(state);
    }
  }

  function createCard(item) {
    const card = document.createElement('article');
    card.className = 'review-card';
    const selection = document.createElement('label');
    selection.className = 'review-select';
    const selected = document.createElement('input');
    selected.type = 'checkbox';
    selected.setAttribute('aria-label', 'Select photo received ' + new Date(item.received_at).toLocaleString());
    selected.addEventListener('change', updateSelection);
    const selectionText = document.createElement('span');
    selectionText.textContent = 'Select photo';
    selection.append(selected, selectionText);
    const photoList = document.createElement('div');
    photoList.className = 'review-frames';
    const summary = document.createElement('p');
    summary.className = 'frame-summary';
    const date = document.createElement('p');
    const reason = document.createElement('p');
    reason.className = 'review-reason';
    const label = document.createElement('label');
    label.textContent = 'Your description';
    const note = document.createElement('textarea');
    note.rows = 3;
    note.maxLength = 2000;
    note.placeholder = 'Describe what you see (required to keep as suspicious).';
    label.append(note);
    const actions = document.createElement('div');
    actions.className = 'review-actions';
    const status = document.createElement('p');
    status.setAttribute('role', 'status');
    const state = {
      id: String(item.id), session, card, photoList, summary, selectionText, date, reason, note,
      status, selected, reserved: false, buttons: [], busy: false, photos: new Map(), inputComplete: true, grouped: false
    };
    for (const [action, text] of [
      ['keep', 'Keep as suspicious'],
      ['dismiss', 'Dismiss & delete photo'],
      ['retry', 'Retry AI']
    ]) {
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = text;
      button.datasetAction = action;
      button.addEventListener('click', () => { void act(state, action); });
      state.buttons.push(button);
      actions.append(button);
    }
    card.append(selection, summary, photoList, date, reason, label, actions, status);
    return state;
  }

  async function refresh(force = false) {
    if (batch) return;
    if (listRequest && !force) return;
    if (force) invalidateList();
    const controller = new AbortController();
    const requestSession = session;
    listRequest = controller;
    el('review-refresh').disabled = true;
    try {
      const data = await call('/api/review', {}, 'json', controller);
      if (listRequest !== controller || requestSession !== session) return;
      if (!Array.isArray(data.items) || data.items.some(item =>
        !item || item.id == null || !['string', 'number'].includes(typeof item.id))) {
        throw Error('Invalid review response');
      }
      const ids = new Set(data.items.map(item => String(item.id)));
      for (const state of cards.values()) {
        if (!ids.has(state.id) && !state.busy) removeCard(state);
      }
      // Reuse cards so polling preserves notes, focus, photos and pending actions.
      el('review-list').querySelector('.empty')?.remove();
      for (const item of data.items) {
        const id = String(item.id);
        let state = cards.get(id);
        if (!state) {
          state = createCard(item);
          cards.set(id, state);
          el('review-list').append(state.card);
        }
        state.date.textContent = 'Received: ' + new Date(item.received_at).toLocaleString();
        state.reason.textContent = item.description || '';
        syncPhotos(state, item);
      }
      updateCount();
      if (el('review-message').textContent.startsWith('Manual review unavailable:')) {
        el('review-message').textContent = '';
      }
    } catch (error) {
      if (listRequest === controller && requestSession === session) {
        el('review-message').textContent = 'Manual review unavailable: ' + error.message;
        // Keep existing cards and unsaved notes on a transient network failure.
      }
    } finally {
      if (listRequest === controller) {
        listRequest = null;
        el('review-refresh').disabled = false;
      }
    }
  }

  el('review-refresh').addEventListener('click', () => {
    if (batch) return;
    const nextKey = el('review-key').value;
    if (nextKey !== key) {
      invalidateList();
      session++;
      key = nextKey;
      for (const state of cards.values()) removeCard(state);
      photoQueue.length = 0;
      el('review-list').replaceChildren();
      el('review-count').textContent = '';
      el('review-message').textContent = '';
    }
    void refresh(true);
  });
  window.authReady.then(user => {
    if (user && user.role === 'viewer') {
      void refresh(); setInterval(() => { void refresh(); }, 10000);
    }
  }).catch(() => {});
})();
