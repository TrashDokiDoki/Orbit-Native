const $=id=>document.getElementById(id),api=window.orbit;let networkTimer, networkStatus, networkBusy = false;
let settings,state,systemLabel='',displays=[],open=false,tab='home',system={},nativeError='',activeNote=null,noteTimeout,hoverTimeout,mediaRecorder,mediaStream,recordChunks=[],recordMime='',recordStart=0;
const titles={home:'Главная',clipboard:'Буфер обмена',shelf:'Полка',notes:'Заметки',tools:'Инструменты',weather:'Погода',search:'Поиск',controls:'Управление',network:'Сетевые инструменты',settings:'Настройки'};
function text(tag,content,className){const n=document.createElement(tag);n.textContent=content;if(className)n.className=className;return n}function button(label,fn,className){const b=text('button',label,className);b.type='button';b.onclick=fn;return b}function message(t){$('message').textContent=t||''}async function run(p,success){try{const r=await p;if(r?.error)throw Error(r.error);if(success)success(r);return r}catch(e){message(e.message);return null}}
function expand(value) {
  clearTimeout(hoverTimeout);
  run(api.expand(value));
}
function renderExpanded(value) {
  open = value;
  syncPanelWork();
  if(value) setCompactPlayer(false,false);
  clearTimeout(pointerLeaveTimer);
  clearTimeout(hoverTimeout);
  const restorePillFocus = !value && $('panel').contains(document.activeElement);
  const moveKeyboardFocus = value && $('pill').matches(':focus-visible');
  $('pill').setAttribute('aria-expanded', String(value||miniOpen));
  $('pill').setAttribute('aria-label', value ? 'Свернуть панель' : 'Раскрыть панель');
  $('panel').inert = !value;
  $('island').classList.toggle('expanded', value);
  if (restorePillFocus) $('pill').focus();
  if (moveKeyboardFocus) $('collapse-panel').focus();
  if (!value && dropActive) hideDropMenu(false);
  if (value) {loadLyrics();refreshHapp();if(tab==='controls')refreshControls()}
}
let miniOpen=false;
let mascotShift;
$('pill').onclick = () => settings?.musicMode==='mini'&&system.media?.playing&&!open ? setCompactPlayer(!miniOpen) : expand(!open);
$('mini-open').onclick=()=>expand(true);
$('collapse-panel').onclick = () => expand(false);
let pointerLeaveTimer;
$('island').addEventListener('pointerenter', () => clearTimeout(pointerLeaveTimer));
$('island').addEventListener('pointerleave', () => {
  if ((!open && !miniOpen) || !settings?.collapseOnLeave) return;
  clearTimeout(pointerLeaveTimer);
  pointerLeaveTimer = setTimeout(() => {
    if (miniOpen) setCompactPlayer(false);
    if (open && settings.collapseOnLeave && $('alarm').hidden && !dropActive && !$('eq-dialog').open && !$('power-dialog').open && document.activeElement!==$('media-source') && document.activeElement !== $('audio-device')) expand(false);
  }, settings?.leaveDelay ?? 350);
});
let layoutTimer;
function renderIndicator(label, description) {
  const indicator = $('layout-indicator');
  clearTimeout(layoutTimer);
  indicator.textContent = label;
  indicator.setAttribute('aria-label', description);
  indicator.hidden = false;
  $('island').classList.add('has-indicator');
  updateMascot();
  const duration = settings?.eventDuration ?? 1650;
  indicator.getAnimations().forEach(animation => animation.cancel());
  if (!matchMedia('(prefers-reduced-motion: reduce)').matches) {
    indicator.animate([{opacity:0,transform:'translateY(-5px) scale(.9)'},{opacity:1,transform:'none',offset:.16},{opacity:1,offset:.78},{opacity:0,transform:'translateY(-4px)'}],{duration});
  }
  layoutTimer = setTimeout(() => { indicator.hidden = true; $('island').classList.remove('has-indicator'); updateMascot(); }, duration);
}
api.onSettings(next=>{settings=next;applyAppearance();renderApps()});
api.onLayout(label => renderIndicator('⌨ ' + label, 'Раскладка: ' + label));
api.onCaps(on => renderIndicator(on ? '⇪ Caps Lock вкл.' : '↓ Caps Lock выкл.', on ? 'Caps Lock включён' : 'Caps Lock выключен'));
api.onExpanded(renderExpanded);api.onTab(showTab);api.onError(message);api.onNativeError(e=>{nativeError=e;$('native-status').textContent=e});api.onHotkeys(keys=>{if(keys.length)message('Недоступны сочетания: '+keys.join(', '))});api.onClips(clips=>{state.clips=clips;if(tab==='clipboard')renderClips()});api.onSystem(snapshot => {
  const media = snapshot.media;
  if (media && !Object.hasOwn(media, 'cover') && media.coverRevision === system.media?.coverRevision) {
    system = { ...snapshot, media: { ...media, cover: system.media.cover } };
  } else {
    system = snapshot;
  }
  renderSystem();
});api.onState(s=>{state=s;renderTimer();renderReminders()});api.onAlarm(queueAlarm);api.onRecordToggle(handleRecordShortcut);api.onRecording(v=>{if(!v&&mediaRecorder?.state==='inactive')recordStart=0});
function showTab(name){if(name==='settings'){run(api.openSettings());return}if(!titles[name])return;if(tab==='notes')saveNotes();tab=name;syncPanelWork();document.querySelectorAll('[data-page]').forEach(el=>el.hidden=el.dataset.page!==name);document.querySelectorAll('[data-tab]').forEach(b=>b.classList.toggle('active',b.dataset.tab===name));$('title').textContent=titles[name];$('title').hidden=Boolean(document.querySelector('#tabs .active'));$('content').scrollTop=0;message('');if(name==='clipboard')renderClips();if(name==='shelf')renderShelf();if(name==='notes')renderNotes();if(name==='tools')renderColors();if(name==='search'){$('command').focus();renderSearch()}if(name==='controls'){$('native-status').textContent=nativeError;refreshControls();refreshHapp()}if(name==='home')loadLyrics()}
document.querySelectorAll('[data-tab]').forEach(b=>b.onclick=()=>showTab(b.dataset.tab));document.querySelectorAll('[data-goto]').forEach(b=>b.onclick=()=>showTab(b.dataset.goto));const settingsButton = $('settings-button');
function renderUpdateBadge(result) {
  settingsButton.classList.toggle('update-available', Boolean(result.available));
  settingsButton.title = result.available ? 'Доступен Orbit ' + result.latest : 'Настройки';
  settingsButton.setAttribute('aria-label', settingsButton.title);
}
settingsButton.onclick = () => run(api.openSettings(settingsButton.classList.contains('update-available') ? 'updates' : 'general'));
api.onUpdateResult(renderUpdateBadge);
run(api.updateStatus(), renderUpdateBadge);
$('pill').addEventListener('mouseenter',()=>{if(settings?.hover&&!settings.clickThrough&&!open&&!miniOpen&&!(system.restricted&&settings.ruleMode==='manual')&&!(system.media?.playing&&settings.musicMode==='mini'))hoverTimeout=setTimeout(()=>expand(true),settings.hoverDelay)});$('pill').addEventListener('mouseleave',()=>clearTimeout(hoverTimeout));$('pill').addEventListener('wheel',e=>{e.preventDefault();run(api.native('volume',{command:e.deltaY<0?'up':'down'}),()=>changeAudio('status'))},{passive:false});document.addEventListener('pointermove',e=>{const x=Math.max(-2,Math.min(2,(e.clientX-innerWidth/2)/60)),y=Math.max(-2,Math.min(2,e.clientY/60-1));$('mascot').style.setProperty('--eye-x',x+'px');$('mascot').style.setProperty('--eye-y',y+'px')});
function applyAppearance(){document.documentElement.style.setProperty('--user-accent',settings.accent);document.body.classList.toggle('light',settings.theme==='light'||settings.theme==='system'&&matchMedia('(prefers-color-scheme: light)').matches);document.body.dataset.position = settings.position;document.body.dataset.shape = settings.shape;document.body.dataset.musicMode=settings.musicMode;applyModules();updateMascot();if(settings.musicMode!=='mini'&&miniOpen)setCompactPlayer(false);$('shortcut-hint').textContent=settings.shortcut||'Control+Alt+I';applyTrackAccent();renderCompactMedia();syncPanelWork();if(lastForecast)checkRain(lastForecast)}
async function displayFileIcon(slot, file) {
  if (!/^(?:[a-z]:[\\/]|\/)/i.test(file)) return;
  try {
    const source = await api.fileIcon(file);
    if (!slot.isConnected || typeof source !== 'string' || !source.startsWith('data:image/png;')) return;
    const icon = document.createElement('img');
    icon.alt = '';
    icon.draggable = false;
    icon.className = 'file-icon';
    icon.onload = () => { if (slot.isConnected) slot.replaceChildren(icon); };
    icon.src = source;
  } catch {}
}

function renderApps(){const root=$('apps');root.replaceChildren();const groups=new Map();settings.items.forEach((item,i)=>{const group=item.group||'';if(!groups.has(group))groups.set(group,[]);groups.get(group).push({item,i})});for(const[group,items]of groups){if(group)root.append(text('div',group,'group-title'));for(const{item,i}of items){const b=button('',()=>run(api.launch(i)),'app');b.title=item.name;b.setAttribute('aria-label',item.name);const symbol=text('span',item.symbol||'↗','symbol');b.append(symbol,text('strong',item.name));root.append(b);if(!item.symbol||item.symbol==='↗')displayFileIcon(symbol,item.target)}}}
let lyricsTrack = '';
let lyricsRequest = 0;
let lyricsRequested = false;
let lyricsRetries = 0;
let lyricsRetryTimer;
let lyricLines = [];
let activeLyric = -1;

function parseLyrics(source) {
  const offset = Number(source.match(/\[offset:\s*([+-]?\d+)\]/i)?.[1] || 0) / 1000;
  const lines = new Map();
  for (const line of source.split(/\r?\n/)) {
    const stamps = /^\s*(?:\[\d+:[0-5]\d(?:[.:]\d+)?\])+/.exec(line)?.[0];
    if (!stamps) continue;
    const content = line.slice(stamps.length).trim();
    for (const [, minutes, seconds, fraction] of stamps.matchAll(/\[(\d+):(\d+)(?:[.:](\d+))?\]/g)) {
      const at = Number(minutes) * 60 + Number(seconds) + Number('0.' + (fraction || '0')) - offset;
      const previous = lines.get(at);
      lines.set(at, previous && content ? previous + '\n' + content : previous || content);
    }
  }
  return [...lines].sort(([a], [b]) => a - b).map(([at, content]) => ({ at, content }));
}

function playbackPosition(track) {
  if (!Number.isFinite(track?.position) || track.position < 0 || !(track.duration > 0)) return null;
  const age = (Date.now() - track.updatedAt) / 1000;
  const elapsed = track.playing && age > 0 && age < track.duration ? age : 0;
  return Math.max(0, Math.min(track.duration, track.position + elapsed));
}

function updatePlayback() {
  if (!open && !$('pill-media').hidden) updateCompactProgress();
  if (!open || tab !== 'home') return;
  const track = system.media;
  const position = playbackPosition(track);
  $('track-progress').style.width = position === null ? '0%' : (position / track.duration * 100) + '%';
  $('track-elapsed').textContent = duration(Math.floor(position || 0) * 1000);
  $('track-duration').textContent = track?.duration > 0 ? duration(Math.floor(track.duration) * 1000) : '—:—';
  const root = $('lyrics');
  if (root.hidden || !lyricLines.length) return;
  const index = position === null ? -1 : lyricLines.findLastIndex(line => line.at <= position);
  if (index === activeLyric) return;
  root.children[activeLyric]?.removeAttribute('aria-current');
  activeLyric = index;
  if (!lyricLines[index]?.content) return;
  const line = root.children[index];
  line.setAttribute('aria-current', 'true');
  const top = root.scrollTop + line.getBoundingClientRect().top - root.getBoundingClientRect().top;
  root.scrollTo({ top: top - (root.clientHeight - line.offsetHeight) / 2,
    behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'instant' : 'smooth' });
}

async function loadLyrics() {
  const root = $('lyrics');
  const track = system.media;
  if (lyricsRequested || !track?.title || !track.artist || (!track.playing && root.hidden)) return;
  lyricsRequested = true;
  const request = ++lyricsRequest;
  const { title, artist, album, duration } = track;
  root.textContent = 'Ищем текст в LRCLIB…';
  try {
    const result = await api.lyrics({ title, artist, album, duration });
    if (request !== lyricsRequest) return;
    if (result.error) throw Error(result.error);
    lyricLines = result.synced ? parseLyrics(result.synced) : [];
    if (!lyricLines.length) {
      root.textContent = result.text;
      return;
    }
    root.replaceChildren(...lyricLines.map(line => text('p', line.content, 'lyrics-line')));
    updatePlayback();
  } catch {
    if (request !== lyricsRequest) return;
    if (track.playing && lyricsRetries < 1) {
      lyricsRetries++;
      root.textContent = 'Повторяем поиск текста…';
      lyricsRetryTimer = setTimeout(() => {
        if (request !== lyricsRequest) return;
        lyricsRequested = false;
        loadLyrics();
      }, 2200);
    } else root.textContent = 'Не удалось загрузить текст. Открой текст песни ещё раз, чтобы повторить.';
  }
}
$('lyrics-toggle').onclick = () => {
  const root = $('lyrics');
  setLyricsVisible(root.hidden);
  if (!root.hidden) loadLyrics();
  else if (!lyricLines.length) {
    clearTimeout(lyricsRetryTimer);
    lyricsRetries = 0;
    lyricsRequest++;
    lyricsRequested = false;
  }
};
let mascotIdleTimer;
const reduceMotion = matchMedia('(prefers-reduced-motion: reduce)');
const pill = $('pill');
function syncIdleMotion() {
  const reaction = $('mascot').dataset.reaction;
  const idle = !open && document.visibilityState === 'visible' && !reduceMotion.matches &&
    !system.media?.playing && (reaction === 'idle' || reaction === 'sleep');
  if (!idle) {
    clearTimeout(mascotIdleTimer);
    mascotIdleTimer = undefined;
    pill.classList.remove('idle-motion');
  } else if (mascotIdleTimer === undefined) {
    mascotIdleTimer = setTimeout(animateIdle, 5500);
  }
}
function animateIdle() {
  const active = pill.classList.toggle('idle-motion');
  mascotIdleTimer = setTimeout(animateIdle, active ? 1400 : 5500);
}
reduceMotion.addEventListener('change', syncIdleMotion);

let playbackTimer;
let controlsWork = false;
function syncPanelWork() {
  syncIdleMotion();
  const visible = document.visibilityState === 'visible';
  const controls = visible && open && tab === 'controls';
  const network = visible && open && tab === 'network';
  if (network && networkTimer === undefined) {
    networkTimer = setInterval(refreshNetworkTools, 2500);
    void refreshNetworkTools();
  } else if (!network && networkTimer !== undefined) {
    clearInterval(networkTimer);
    networkTimer = undefined;
  }
  if (controls !== controlsWork) {
    controlsWork = controls;
    api.controlsView(controls);
  }
  const timeline = visible && system.media?.playing &&
    (open && tab === 'home' || !open && !$('pill-media').hidden);
  if (timeline && playbackTimer === undefined) playbackTimer = setInterval(updatePlayback, 120);
  else if (!timeline && playbackTimer !== undefined) {
    clearInterval(playbackTimer);
    playbackTimer = undefined;
  }
}
document.addEventListener('visibilitychange', syncPanelWork);
function setText(node, value) {
  if (node.textContent !== value) node.textContent = value;
}

let coverPalette;

function coverColor(image) {
  const canvas = document.createElement('canvas');
  canvas.width = canvas.height = 24;
  const context = canvas.getContext('2d', { willReadFrequently: true });
  context.drawImage(image, 0, 0, 24, 24);
  const { data } = context.getImageData(0, 0, 24, 24);
  const groups = new Map();
  for (let i = 0; i < data.length; i += 4) {
    const [r, g, b, alpha] = data.subarray(i, i + 4);
    if (alpha < 180) continue;
    const key = ((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5);
    const group = groups.get(key) || { rgb: [0, 0, 0], weight: 0, count: 0 };
    const chroma = Math.max(r, g, b) - Math.min(r, g, b);
    group.weight += 0.2 + chroma / 96;
    group.rgb[0] += r;
    group.rgb[1] += g;
    group.rgb[2] += b;
    group.count++;
    groups.set(key, group);
  }
  const dominant = [...groups.values()].sort((a, b) => b.weight - a.weight)[0];
  return dominant?.rgb.map(channel => Math.round(channel / dominant.count));
}

function luminance(rgb) {
  return rgb.reduce((sum, channel, i) => {
    const value = channel / 255;
    const linear = value <= .04045 ? value / 12.92 : ((value + .055) / 1.055) ** 2.4;
    return sum + linear * [.2126, .7152, .0722][i];
  }, 0);
}

const pugImage = new Image();
pugImage.src = 'pug.png';
pugImage.onload = () => renderPugTint(appliedPalette);
function renderPugTint(palette) {
  const style = document.documentElement.style;
  if (!palette || !pugImage.naturalWidth) {
    style.removeProperty('--pug-art');
    return;
  }
  const canvas = document.createElement('canvas');
  canvas.width = canvas.height = 64;
  const ctx = canvas.getContext('2d');
  ctx.drawImage(pugImage, 0, 0, 64, 64);
  ctx.globalCompositeOperation = 'color';
  ctx.fillStyle = 'rgb(' + palette.join(' ') + ')';
  ctx.fillRect(0, 0, 64, 64);
  ctx.globalCompositeOperation = 'destination-in';
  ctx.drawImage(pugImage, 0, 0, 64, 64);
  style.setProperty('--pug-art', 'url("' + canvas.toDataURL() + '")');
}
let appliedPalette;
let appliedLight;
function applyTrackAccent() {
  const root = document.documentElement;
  const track = system.media;
  const palette = track?.playing && coverPalette?.source === track.cover && coverPalette?.rgb;
  const light = document.body.classList.contains('light');
  if (palette === appliedPalette && light === appliedLight) return;
  if (palette !== appliedPalette) renderPugTint(palette);
  appliedPalette = palette;
  appliedLight = light;
  document.body.classList.toggle('has-track-accent', Boolean(palette));
  if (!palette) {
    root.style.removeProperty('--track-accent');
    root.style.removeProperty('--track-ink');
    root.style.removeProperty('--track-tint');
    return;
  }
  const surface = luminance(light ? [235, 232, 242] : [27, 26, 33]);
  const target = light ? 0 : 255;
  let accent = palette;
  for (let step = 1; step <= 20; step++) {
    const value = luminance(accent);
    if ((Math.max(value, surface) + .05) / (Math.min(value, surface) + .05) >= 4.5) break;
    accent = palette.map(channel => Math.round(channel + (target - channel) * step / 20));
  }
  root.style.setProperty('--track-tint', 'rgb(' + palette.join(' ') + ')');
  root.style.setProperty('--track-accent', 'rgb(' + accent.join(' ') + ')');
  root.style.setProperty('--track-ink', luminance(accent) > .179 ? '#000' : '#fff');
}

$('track-cover').addEventListener('load', event => {
  const image = event.currentTarget;
  coverPalette = { source: image.getAttribute('src'), rgb: coverColor(image) };
  applyTrackAccent();
});

function setLyricsVisible(visible) {
  const toggle = $('lyrics-toggle');
  $('lyrics').hidden = !visible;
  toggle.setAttribute('aria-expanded', String(visible));
  toggle.textContent = visible ? '×' : 'Текст песни';
  const label = visible ? 'Скрыть текст песни' : 'Показать текст песни';
  toggle.setAttribute('aria-label', label);
  toggle.title = label;
}

function renderTrackDetails(track) {
  const cover = $('track-cover');
  const source = track?.cover || '';
  if ((cover.getAttribute('src') || '') !== source) {
    if (source) cover.src = source;
    else cover.removeAttribute('src');
    cover.hidden = !source;
    $('cover-placeholder').hidden = Boolean(source);
  }
  const compactCover = $('pill-cover');
  if ((compactCover.getAttribute('src') || '') !== source) {
    if (source) compactCover.src = source;
    else compactCover.removeAttribute('src');
  }
  const showCover = Boolean(track?.playing && source);
  compactCover.hidden = !showCover;
  applyTrackAccent();
  const key = JSON.stringify([track?.source, track?.title, track?.artist, track?.album]);
  if (key !== lyricsTrack) {
    lyricsTrack = key;
    clearTimeout(lyricsRetryTimer);
    lyricsRetries = 0;
    lyricsRequest++;
    lyricsRequested = false;
    lyricLines = [];
    activeLyric = -1;
    $('lyrics').textContent = '';
    setLyricsVisible(Boolean(track?.title && track.artist));
  }
  $('lyrics-toggle').hidden = !(track?.title && track?.artist);
  if (!$('lyrics').hidden && $('lyrics-toggle').hidden) $('lyrics').textContent = 'Нет активного трека с текстом';
  else loadLyrics();
  $('eq-open').hidden = !track?.playing;
}
$('pill-cover').onerror = () => {
  $('pill-cover').hidden = true;
};
$('track-cover').onerror = () => {
  $('track-cover').hidden = true;
  $('cover-placeholder').hidden = false;
};
function renderSystem(){const m=system.media;renderTrackDetails(m);renderMediaSources();renderCompactMedia();setText($('track'),m?.title||'Музыка рядом');setText($('artist'),m?.artist||(m?.title?'':'Включи Spotify или другой плеер'));const playbackLabel=m?.playing?'Пауза':'Воспроизвести';$('play').title=playbackLabel;$('play').setAttribute('aria-label',playbackLabel);updatePlayback();syncPanelWork();animatePlayback(Boolean(m?.playing));$('equalizer').hidden=!m?.playing;$('equalizer').title=m?.title||'';const layout=system.layout===1049?'RU':system.layout===1033?'EN':system.layout?'⌨':'';const battery=system.battery>=0&&system.battery<=100?' · '+system.battery+'%'+(system.plugged?' ⚡':''):'';setText($('system-status'),(layout?layout+(system.caps?' · CAPS':'')+' · ':'')+systemLabel+battery)}
document.querySelectorAll('[data-media]').forEach(b=>b.onclick=()=>run(api.native('media',{command:b.dataset.media,source:system.media?.source})));document.querySelectorAll('[data-volume]').forEach(b=>b.onclick=()=>run(api.native('volume',{command:b.dataset.volume})));document.querySelectorAll('[data-system]').forEach(b=>b.onclick=()=>run(api.systemLink(b.dataset.system)));document.querySelectorAll('[data-dictation]').forEach(button=>button.onclick=()=>run(api.native('dictation')));
function renderClips(){if(!state)return;$('clip-notice').hidden=settings.clipboard;$('clips').replaceChildren();const q=$('clip-search').value.toLowerCase();const list=state.clips.filter(c=>c.type==='image'?!q:(c.text||'').toLowerCase().includes(q)).sort((a,b)=>Number(b.pinned)-Number(a.pinned));for(const c of list){const card=text('div','','card'+(c.pinned?' pinned':'')),main=button('',()=>run(api.clipAction(c.id,'paste')),'card-main');if(c.type==='image'){const img=document.createElement('img');img.src=c.data;img.alt='Изображение из буфера';main.append(img)}else main.textContent=c.text;const actions=text('div','','card-actions');actions.append(button('Копировать',()=>run(api.clipAction(c.id,'copy'),()=>message('Скопировано'))),button(c.pinned?'Открепить':'Закрепить',()=>run(api.clipAction(c.id,'pin'),clips=>{state.clips=clips;renderClips()})),button('×',()=>run(api.clipAction(c.id,'delete'),clips=>{state.clips=clips;renderClips()})));card.append(main,actions);$('clips').append(card)}if(!list.length&&settings.clipboard)$('clips').append(text('div','Пока пусто. Скопируй текст или картинку.','empty'))}
$('clip-search').oninput=renderClips;$('clip-clear').onclick=()=>run(api.clipAction(null,'clear'),clips=>{state.clips=clips;renderClips()});
function renderShelf(){$('shelf').replaceChildren();for(const f of state.shelf){const row=text('div','','card file-row'),drag=text('span','↗','drag-handle');drag.draggable=true;drag.title='Перетащи в другое приложение';drag.ondragstart=e=>{e.preventDefault();api.dragFile(f.id)};row.append(drag,text('strong',f.name),button('Открыть',()=>run(api.shelfAction(f.id,'open'))),button('⌕',()=>run(api.shelfAction(f.id,'reveal'))),button('×',()=>run(api.shelfAction(f.id,'remove'),s=>{state.shelf=s;renderShelf()})));row.title=f.path;$('shelf').append(row);displayFileIcon(drag,f.path)}}$('shelf-add').onclick=()=>run(api.shelfAdd(),s=>{state.shelf=s;renderShelf()});
let dropActive = false;
let dropOpenedPanel = false;
let dropLeaveTimer;
function hideDropMenu(collapse) {
  clearTimeout(dropLeaveTimer);
  if (!dropActive) return;
  dropActive = false;
  $('island').classList.remove('drop-active');updateMascot();
  $('drop-menu').hidden = true;
  document.querySelectorAll('.drop-target.is-target').forEach(tile => tile.classList.remove('is-target'));
  if (collapse && dropOpenedPanel) expand(false);
}
function handleFileDrag(event) {
  if (!event.dataTransfer.types.includes('Files')) return;
  event.preventDefault();
  if (!event.target.closest('#island')) {
    if (dropActive) scheduleDropClose();
    return;
  }
  clearTimeout(dropLeaveTimer);
  if (!dropActive) {
    dropActive = true;
    dropOpenedPanel = !open;
    clearTimeout(pointerLeaveTimer);
    $('island').classList.add('drop-active');updateMascot();
    $('drop-menu').hidden = false;
    if (dropOpenedPanel) expand(true);
  }
  const target = event.target.closest('[data-drop]');
  event.dataTransfer.dropEffect = target ? 'copy' : 'none';
  document.querySelectorAll('.drop-target').forEach(tile => tile.classList.toggle('is-target', tile === target));
}
function scheduleDropClose() {
  clearTimeout(dropLeaveTimer);
  dropLeaveTimer = setTimeout(() => hideDropMenu(true), 170);
}
document.addEventListener('dragenter', handleFileDrag);
document.addEventListener('dragover', handleFileDrag);
document.addEventListener('dragleave', event => {
  if (dropActive && !event.relatedTarget?.closest?.('#island')) scheduleDropClose();
});
document.addEventListener('drop', async event => {
  if (!event.dataTransfer.types.includes('Files')) return;
  event.preventDefault();
  const destination = event.target.closest('[data-drop]')?.dataset.drop;
  const paths = destination && api.resolveDropPaths ? await api.resolveDropPaths(event.dataTransfer) :
    [...event.dataTransfer.files].map(file => api.filePath(file)).filter(Boolean);
  hideDropMenu(!destination || !paths.length);
  if (!destination || !paths.length) return;
  if (destination === 'shelf') {
    showTab('shelf');
    await run(api.shelfAdd(paths), list => { state.shelf = list; renderShelf(); });
  } else if (destination === 'launcher') {
    showTab('home');
    await run(api.dropLauncher(paths), result => {
      settings = result.settings;
      renderApps();
      message(result.count ? 'Ярлыки добавлены: ' + result.count : 'Эти ярлыки уже в лаунчере');
    });
  } else {
    await run(api.copy(paths.join('\n')), () => {
      const index = settings.items.findIndex(item => item.name.toLowerCase() === 'chatgpt');
      const launchChat = index >= 0 ? api.launch(index) : api.web('https://chatgpt.com/');
      run(launchChat, () => message('Путь скопирован. Файл можно прикрепить в ChatGPT.'));
    });
  }
});
function currentNote(){return state.notes.find(n=>n.id===activeNote)}function syncNote(){const n=currentNote();if(n){n.name=$('note-name').value||'Без названия';n.folder=$('note-folder').value||'Личное';n.text=$('note-text').value}}async function saveNotes(){if(!state||tab!=='notes')return;syncNote();clearTimeout(noteTimeout);$('note-status').textContent='Сохраняем…';await run(api.notesSave(state.notes),()=>{$('note-status').textContent='Сохранено на этом компьютере'})}function renderNotes(){const select=$('note-select');select.replaceChildren();if(!state.notes.some(n=>n.id===activeNote))activeNote=state.notes[0]?.id;const groups=new Map();for(const n of state.notes){if(!groups.has(n.folder))groups.set(n.folder,[]);groups.get(n.folder).push(n)}for(const[folder,notes]of groups){const g=document.createElement('optgroup');g.label=folder;for(const n of notes){const o=text('option',n.name);o.value=n.id;g.append(o)}select.append(g)}select.value=activeNote||'';const n=currentNote();$('note-name').value=n?.name||'';$('note-folder').value=n?.folder||'';$('note-text').value=n?.text||'';renderReminders()}
$('note-select').onchange=async()=>{await saveNotes();activeNote=$('note-select').value;renderNotes()};$('note-add').onclick=async()=>{await saveNotes();const n={id:crypto.randomUUID(),name:'Новая заметка',folder:'Личное',text:''};state.notes.push(n);activeNote=n.id;renderNotes();await saveNotes()};$('note-delete').onclick=async()=>{clearTimeout(noteTimeout);state.notes=state.notes.filter(n=>n.id!==activeNote);activeNote=state.notes[0]?.id;await run(api.notesSave(state.notes));renderNotes()};for(const id of['note-name','note-folder','note-text'])$(id).oninput=()=>{syncNote();$('note-status').textContent='Сохраняем…';clearTimeout(noteTimeout);noteTimeout=setTimeout(saveNotes,400)};
function renderReminders(){if(!state)return;$('reminders').replaceChildren();for(const r of state.reminders.filter(r=>!r.done)){const card=text('div','','card file-row');card.append(text('strong',r.text),text('small',new Date(r.at).toLocaleString('ru-RU')+(r.repeat==='daily'?' · ежедневно':'')),button('×',()=>run(api.reminder('remove',{id:r.id}),list=>{state.reminders=list;renderReminders()})));$('reminders').append(card)}}$('reminder-add').onclick=()=>run(api.reminder('add',{text:$('reminder-text').value.trim(),at:new Date($('reminder-at').value).getTime(),repeat:$('reminder-repeat').value}),list=>{state.reminders=list;$('reminder-text').value='';renderReminders()});
let steamDownload = '';
api.onSteamDownload?.(value => { steamDownload = value; renderTimer(); });
const remaining=()=>state?.timer?(state.timer.paused?state.timer.remaining:Math.max(0,state.timer.end-Date.now())):0;function duration(ms){const sec=Math.ceil(ms/1000);return Math.floor(sec/60)+':'+String(sec%60).padStart(2,'0')}function renderTimer(){if(!state)return;const t=state.timer;$('timer-value').textContent=t?duration(remaining())+(t.paused?' · пауза':''):'Таймер';$('timer-presets').hidden=!!t;$('timer-active').hidden=!t;$('timer-pause').textContent=t?.paused?'▶':'Ⅱ';$('pill-status').textContent=recordStart?'● '+duration(Date.now()-recordStart):t?duration(remaining()):steamDownload;$('pill-status').hidden=!(recordStart||t||steamDownload)}
function startTimer(minutes){run(api.timer({action:'start',minutes}),t=>{state.timer=t;renderTimer()})}document.querySelectorAll('[data-minutes]').forEach(b=>b.onclick=()=>startTimer(Number(b.dataset.minutes)));$('timer-start').onclick=()=>startTimer(Number($('timer-minutes').value));$('timer-reset').onclick=()=>run(api.timer({action:'reset'}));$('timer-stop').onclick=()=>run(api.timer({action:'stop'}),t=>{state.timer=t;renderTimer()});$('timer-pause').onclick=()=>run(api.timer({action:'pause'}),t=>{state.timer=t;renderTimer()});
const textTransforms={upper:s=>s.toUpperCase(),lower:s=>s.toLowerCase(),layout:OrbitCore.convertLayout,translit:OrbitCore.translit,json:s=>JSON.stringify(JSON.parse(s),null,2),encode64:s=>btoa(String.fromCharCode(...new TextEncoder().encode(s))),decode64:s=>new TextDecoder('utf-8',{fatal:true}).decode(Uint8Array.from(atob(s),c=>c.charCodeAt(0))),url:encodeURIComponent,unurl:decodeURIComponent};document.querySelectorAll('[data-text]').forEach(b=>b.onclick=()=>{try{$('text-input').value=textTransforms[b.dataset.text]($('text-input').value);message('')}catch{message('Не удалось преобразовать текст. Проверь формат.')}});$('text-copy').onclick=()=>run(api.copy($('text-input').value),()=>message('Скопировано'));function renderColors(){$('colors').replaceChildren();for(const c of state.colors){const b=button('',()=>run(api.copy(c),()=>message(c+' скопирован')),'swatch');b.style.background=c;b.title=c;$('colors').append(b)}}
$('quick-add').onclick=()=>run(api.openSettings('apps',true));
$('quick-text').onclick=()=>{showTab('tools');$('text-input').focus()};
$('quick-record').onclick=()=>{showTab('tools');toggleRecord()};
document.querySelectorAll('[data-capture]').forEach(b=>b.onclick=()=>run(api.capture(b.dataset.capture)));
let recordStarting = false;
function handleRecordShortcut() {
  if (mediaRecorder?.state === 'recording' || !api.recordStream || navigator.userActivation?.isActive) {
    toggleRecord();
    return;
  }
  if (recordStarting || mediaRecorder) return;
  showTab('tools');
  $('record-button').focus();
  $('record-status').textContent = 'Нажми Enter или «Запись экрана», чтобы выбрать источник.';
}
async function toggleRecord() {
  if (mediaRecorder?.state === 'recording') {
    mediaRecorder.stop();
    return;
  }
  if (recordStarting || mediaRecorder) return;
  recordStarting = true;
  const streamWriter = api.recordStream;
  let saveQueue = Promise.resolve();
  let recordError;
  let firstFrameTimeout;
  try {
    if (!navigator.mediaDevices?.getDisplayMedia) throw Error('Этот сеанс не поддерживает запись экрана.');
    const wantsAudio = $('record-audio').checked;
    mediaStream = await navigator.mediaDevices.getDisplayMedia({
      video: { frameRate: 30 },
      audio: wantsAudio
    });
    recordChunks = [];
    const mime = ['video/mp4;codecs=avc1.42001E,mp4a.40.2','video/mp4','video/webm;codecs=vp9,opus','video/webm']
      .find(type => MediaRecorder.isTypeSupported(type));
    mediaRecorder = new MediaRecorder(mediaStream, { ...(mime ? {mimeType:mime} : {}), videoBitsPerSecond:8000000 });
    recordMime = mediaRecorder.mimeType || mime || 'video/webm';
    if (streamWriter) {
      const result = await streamWriter.begin(recordMime);
      if (result?.error) throw Error(result.error);
    }
    mediaRecorder.ondataavailable = ({data}) => {
      if (!data.size || recordError) return;
      clearTimeout(firstFrameTimeout);
      if (!streamWriter) {
        recordChunks.push(data);
        return;
      }
      saveQueue = saveQueue.then(async () => {
        if (recordError) return;
        const result = await streamWriter.chunk(data);
        if (result?.error) throw Error(result.error);
      }).catch(error => {
        recordError = error;
        if (mediaRecorder?.state === 'recording') mediaRecorder.stop();
      });
    };
    mediaRecorder.onstop = async () => {
      clearTimeout(firstFrameTimeout);
      mediaStream.getTracks().forEach(track => track.stop());
      recordStart = 0;
      $('record-label').textContent = 'Запись экрана';
      $('record-status').textContent = 'Сохраняем запись…';
      await api.recordState(false);
      await saveQueue;
      if (recordError) {
        await streamWriter?.discard();
        $('record-status').textContent = recordError.message;
      } else {
        const save = async () => streamWriter ? streamWriter.finish() :
          api.recordSave(await new Blob(recordChunks,{type:recordMime}).arrayBuffer(), recordMime);
        const result = await run(save());
        $('record-status').textContent = result?.saved ? 'Запись сохранена' :
          result ? 'Сохранение отменено' : 'Не удалось сохранить запись';
      }
      recordChunks = [];
      mediaRecorder = null;
      mediaStream = null;
    };
    mediaRecorder.onerror = () => {
      recordError = Error('Ошибка записи экрана');
      if (mediaRecorder.state === 'recording') mediaRecorder.stop();
    };
    mediaStream.getVideoTracks()[0].onended = () => {
      if (mediaRecorder?.state === 'recording') mediaRecorder.stop();
    };
    mediaRecorder.start(1000);
    firstFrameTimeout = setTimeout(() => {
      if (mediaRecorder?.state !== 'recording') return;
      recordError = Error('Источник экрана не передал данные для записи.');
      mediaRecorder.stop();
    }, 10000);
    recordStart = Date.now();
    await api.recordState(true);
    $('record-label').textContent = 'Остановить запись';
    $('record-status').textContent = 'Идёт запись · ' + (recordMime.includes('mp4')?'MP4':'WebM') +
      (wantsAudio && !mediaStream.getAudioTracks().length ? ' · без системного звука' : '') +
      ' · нажми ещё раз для сохранения';
    message('');
  } catch (error) {
    clearTimeout(firstFrameTimeout);
    mediaStream?.getTracks().forEach(track => track.stop());
    await streamWriter?.discard();
    recordStart = 0;
    mediaRecorder = null;
    mediaStream = null;
    const detail = systemLabel === 'Linux' && error.name === 'OverconstrainedError'
      ? 'Рабочий стол не предоставил видеопоток. Проверь доступность записи экрана в текущем сеансе.'
      : error.message;
    message('Не удалось начать запись: ' + detail);
  } finally {
    recordStarting = false;
  }
}
$('record-button').onclick = toggleRecord;
let weatherCity = localStorage.getItem('weather-city') || 'Москва';
let weatherRequest = 0;
let lastForecast;
function weatherCondition(code, daytime) {
  if (!Number.isFinite(code)) return ['—', 'Нет данных'];
  if (code <= 1) return [daytime === 0 ? '☾' : '☀', 'Ясно'];
  if (code === 2) return ['🌤', 'Переменная облачность'];
  if (code === 3) return ['☁', 'Облачно'];
  if (code === 45 || code === 48) return ['≋', 'Туман'];
  if ([71,73,75,77,85,86].includes(code)) return ['❄', 'Снег'];
  if (code >= 95) return ['ϟ', 'Гроза'];
  return ['☂', 'Дождь'];
}
let weatherPeriod = 'hour';
const weatherNumber = new Intl.NumberFormat('ru-RU', {maximumFractionDigits: 1});
const weatherDay = new Intl.DateTimeFormat('ru-RU', {weekday: 'short', day: 'numeric'});
function weatherValue(value, unit = '', rounded = true) {
  return Number.isFinite(value) ? weatherNumber.format(rounded ? Math.round(value) : value) + unit : '—';
}
function renderWeather(data) {
  const { current, hourly, daily } = data.forecast;
  const [icon, description] = weatherCondition(current.weather_code, current.is_day);
  const degree = weatherValue(current.temperature_2m, '°');
  $('weather-icon').textContent = icon;
  $('weather-degree').textContent = degree;
  $('pill-weather').title = data.location.name + ' · ' + description + ' · ' + degree;

  const root = $('weather-result');
  root.className = 'weather-dashboard';
  const hero = text('article', '', 'weather-hero');
  const heading = text('div', '', 'weather-heading');
  const city = button(data.location.name, () => {
    $('weather-form').hidden = !$('weather-form').hidden;
    city.setAttribute('aria-expanded', String(!$('weather-form').hidden));
    if (!$('weather-form').hidden) $('weather-city').select();
  });
  city.classList.add('weather-location');
  city.title = 'Выбрать город';
  city.setAttribute('aria-expanded', String(!$('weather-form').hidden));
  const refresh = button('⟳', () => refreshWeather(weatherCity, true));
  refresh.title = 'Обновить прогноз';
  refresh.setAttribute('aria-label', refresh.title);
  heading.append(city, refresh);

  const hours = hourly.time;
  const index = Math.max(0, hours.findIndex((time, i) => time <= current.time && (!hours[i + 1] || hours[i + 1] > current.time)));
  const day = Math.max(0, daily.time.indexOf(current.time.slice(0,10)));
  const rain = hourly.precipitation.findIndex((value, i) => i > index && i <= index + 12 && value >= .1);
  const notice = current.precipitation >= .1 ? 'Сейчас идут осадки' :
    rain >= 0 ? 'Около ' + hours[rain].slice(11,16) + ' ожидаются осадки' : 'В ближайшие часы без осадков';
  hero.append(heading, text('div', icon, 'weather-hero-icon'), text('div', degree, 'weather-degree'),
    text('strong', description, 'weather-condition'),
    text('span', 'Ощущается как ' + weatherValue(current.apparent_temperature, '°')),
    text('span', 'Сегодня ' + weatherValue(daily.temperature_2m_min[day], '°') + '…' + weatherValue(daily.temperature_2m_max[day], '°')),
    text('p', notice, 'weather-notice'), text('small', 'Данные Open-Meteo', 'weather-source'));

  const right = text('div', '', 'weather-detail');
  const strip = text('div', '', 'weather-strip');
  const tiles = text('div', '', 'weather-tiles');
  tiles.tabIndex = 0;
  tiles.setAttribute('aria-label', 'Прокручиваемый прогноз');
  const switcher = text('div', '', 'weather-switch');
  for (const [period, label] of [['hour', 'Часы'], ['week', 'Неделя']]) {
    const tab = button(label, () => {
      weatherPeriod = period;
      renderWeather(data);
      root.querySelector('[aria-pressed="true"]').focus({preventScroll:true});
    });
    tab.setAttribute('aria-pressed', String(period === weatherPeriod));
    switcher.append(tab);
  }
  const tile = (label, code, isDay, temperature, precipitation) => {
    const item = text('div', '', 'weather-tile');
    const [symbol, condition] = weatherCondition(code, isDay);
    const glyph = text('span', symbol, 'weather-tile-icon');
    glyph.title = condition;
    item.append(text('small', label), glyph, text('b', temperature),
      text('small', precipitation > 0 ? weatherValue(precipitation, ' мм', false) : '', 'weather-tile-rain'));
    tiles.append(item);
  };
  if (weatherPeriod === 'hour') {
    hours.slice(index, index + 24).forEach((time, offset) => {
      const i = index + offset;
      tile(offset ? time.slice(11,16) : 'Сейчас', offset ? hourly.weather_code[i] : current.weather_code,
        offset ? hourly.is_day[i] : current.is_day, weatherValue(offset ? hourly.temperature_2m[i] : current.temperature_2m, '°'),
        offset ? hourly.precipitation[i] : current.precipitation);
    });
  } else {
    daily.time.forEach((date, i) => tile(i === day ? 'Сегодня' : weatherDay.format(new Date(date + 'T12:00:00')),
      daily.weather_code[i], 1, weatherValue(daily.temperature_2m_min[i], '°') + '…' + weatherValue(daily.temperature_2m_max[i], '°'),
      daily.precipitation_sum[i]));
  }
  strip.append(tiles, switcher);

  const cards = text('div', '', 'weather-metrics');
  function metric(title, value, unit, footer, graphic) {
    const card = text('article', '', 'weather-metric');
    card.append(text('h3', title));
    if (value) {
      const reading = text('div', '', 'weather-reading');
      reading.append(text('strong', value), text('span', unit));
      card.append(reading);
    }
    if (graphic) card.append(graphic);
    if (footer) card.append(text('small', footer, 'weather-footer'));
    cards.append(card);
    return card;
  }
  function gauge(className, value, max) {
    const bar = text('div', '', 'weather-gauge ' + className);
    if (Number.isFinite(value)) bar.style.setProperty('--value', Math.min(100, Math.max(0, value / max * 100)) + '%');
    return bar;
  }
  const bearing = current.wind_direction_10m;
  const directions = ['Северный', 'Северо-восточный', 'Восточный', 'Юго-восточный',
    'Южный', 'Юго-западный', 'Западный', 'Северо-западный'];
  const compass = text('div', '', 'weather-compass');
  compass.append(...['С', 'В', 'Ю', 'З'].map(label => text('span', label)));
  if (Number.isFinite(bearing)) {
    const arrow = text('b', '↑');
    arrow.style.transform = 'rotate(' + bearing + 'deg)';
    compass.append(arrow);
  }
  metric('Ветер', weatherValue(current.wind_speed_10m), 'м/с',
    Number.isFinite(bearing) ? directions[Math.round(bearing / 45) % 8] : '', compass).classList.add('weather-wind');
  metric('Влажность', weatherValue(current.relative_humidity_2m, '%'), '',
    'Точка росы ' + weatherValue(hourly.dew_point_2m[index], '°'), gauge('weather-humidity', current.relative_humidity_2m, 100));
  const pressure = current.surface_pressure * .75006168;
  const previous = hourly.surface_pressure[index - 1];
  const trend = current.surface_pressure - previous;
  const dial = text('div', '', 'weather-pressure');
  if (Number.isFinite(current.surface_pressure)) {
    const needle = text('i', '');
    needle.style.transform = 'rotate(' + Math.min(85, Math.max(-85, (pressure - 750) * 2)) + 'deg)';
    dial.append(needle);
  }
  metric('Давление', weatherValue(Number.isFinite(current.surface_pressure) ? pressure : NaN), 'мм рт. ст.',
    !Number.isFinite(previous) || !Number.isFinite(current.surface_pressure) ? '' : Math.abs(trend) < .5 ? 'Стабильное' : trend > 0 ? '↑ растёт' : '↓ падает',
    dial).classList.add('weather-pressure-card');

  const sun = text('div', '', 'weather-sun');
  const sunrise = daily.sunrise[day];
  const sunset = daily.sunset[day];
  if (sunrise && sunset) {
    const minute = value => Number(value.slice(11,13)) * 60 + Number(value.slice(14,16));
    const daylight = minute(sunset) - minute(sunrise);
    const progress = daylight > 0 ? Math.max(0, Math.min(1, (minute(current.time) - minute(sunrise)) / daylight)) : 0;
    const point = text('i', '');
    point.style.left = progress * 100 + '%';
    point.style.bottom = Math.sin(Math.PI * progress) * 85 + '%';
    sun.append(point);
  }
  const sunTimes = text('div', '', 'weather-footer weather-sun-times');
  sunTimes.append(text('span', '↑ ' + (sunrise?.slice(11,16) || '—')), text('span', '↓ ' + (sunset?.slice(11,16) || '—')));
  metric('Солнце', '', '', '', sun).append(sunTimes);
  const bars = text('div', '', 'weather-bars');
  const firstHour = hours.findIndex(time => time.startsWith(daily.time[day]));
  const amounts = hourly.precipitation.slice(Math.max(0,firstHour), Math.max(0,firstHour) + 24);
  const maximum = Math.max(1, ...amounts.filter(Number.isFinite));
  for (const amount of amounts) {
    const bar = text('i', '');
    bar.style.height = Math.max(2, (amount || 0) / maximum * 28) + 'px';
    bar.title = weatherValue(amount, ' мм', false);
    bars.append(bar);
  }
  metric('Осадки', weatherValue(daily.precipitation_sum[day], '', false), 'мм за сутки', '', bars);
  const uv = hourly.uv_index[index];
  const risk = !Number.isFinite(uv) ? '' : uv < 3 ? 'низкий' : uv < 6 ? 'умеренный' : uv < 8 ? 'высокий' : uv < 11 ? 'очень высокий' : 'экстремальный';
  metric('УФ-индекс', weatherValue(uv), risk, 'Завтра до ' + weatherValue(daily.uv_index_max[day + 1]), gauge('weather-uv', uv, 11));
  right.append(strip, cards);
  const scroll = root.querySelector('.weather-tiles')?.scrollLeft || 0;
  const restoreScroll = root.dataset.period === weatherPeriod;
  root.dataset.period = weatherPeriod;
  root.replaceChildren(hero, right);
  if (restoreScroll) tiles.scrollLeft = scroll;
}
async function refreshWeather(city, manual = false) {
  const request = ++weatherRequest;
  if (manual && !lastForecast) $('weather-result').textContent = 'Загружаем прогноз…';
  $('weather-result').setAttribute('aria-busy', 'true');
  try {
    const data = await api.weather(city);
    if (request !== weatherRequest) return;
    if (data.error) throw Error(data.error);
    if (!Number.isFinite(data.forecast?.current?.temperature_2m)) throw Error('Нет данных о температуре');
    if (manual || !lastForecast) $('weather-form').hidden = true;
    if (manual) {
      message('');
      weatherCity = city;
      localStorage.setItem('weather-city', city);
    }
    renderWeather(data);
    lastForecast = data;
    checkRain(data);
  } catch (error) {
    if (request !== weatherRequest) return;
    const notice = 'Прогноз недоступен для города «' + city + '».';
    if (!lastForecast) {
      $('pill-weather').title = notice;
      $('weather-form').hidden = false;
    }
    if (manual && lastForecast) message(notice);
    if (!lastForecast) {
      $('weather-result').className = 'empty';
      $('weather-result').textContent = notice;
    }
  } finally {
    if (request === weatherRequest) $('weather-result').removeAttribute('aria-busy');
  }
}
$('weather-form').onsubmit = event => {
  event.preventDefault();
  const city = $('weather-city').value.trim();
  if (city) refreshWeather(city, true);
};
setInterval(() => refreshWeather(weatherCity), 15 * 60 * 1000);
let firstAction;function renderSearch(){const q=$('command').value.trim(),root=$('search-results');root.replaceChildren();firstAction=null;const result=(title,subtitle,fn)=>{const b=button('',fn,'result');b.append(text('span',title),text('small',subtitle));root.append(b);if(!firstAction)firstAction=fn};if(!q){result('Открыть настройки','Внешний вид, приложения и горячие клавиши',()=>showTab('settings'));result('Таймер на 25 минут','Пора сосредоточиться',()=>{startTimer(25);showTab('home')});return}settings.items.forEach((a,i)=>{if(a.name.toLowerCase().includes(q.toLowerCase()))result(a.name,a.group||a.target,()=>run(api.launch(i)))});if(/^[\d\s()+*/.,%×÷-]+$/.test(q)){try{const v=OrbitCore.calculate(q);result('= '+v,'Нажми, чтобы скопировать',()=>run(api.copy(String(v)),()=>message('Скопировано')))}catch{}}const timer=q.match(/^таймер\s+(\d+(?:[.,]\d+)?)\s*(?:мин(?:ут[аы]?)?|м)?$/i);if(timer)result('Запустить таймер на '+timer[1]+' мин','Таймер останется на островке',()=>{startTimer(Number(timer[1].replace(',','.')));showTab('home')});const reminder=q.match(/^напомни\s+через\s+(\d+)\s*(мин(?:ут[аы]?)?|ч(?:ас[аов]*)?)\s+(.+)$/i);if(reminder)result('Напомнить: '+reminder[3],'Через '+reminder[1]+' '+reminder[2],()=>run(api.reminder('add',{text:reminder[3],at:Date.now()+Number(reminder[1])*(reminder[2].startsWith('ч')?3600000:60000)}),list=>{state.reminders=list;message('Напоминание добавлено')}));const controls={wifi:'wifi','wi-fi':'wifi',bluetooth:'bluetooth',vpn:'vpn',звук:'sound',тема:'theme'};if(controls[q.toLowerCase()])result('Открыть '+q,'Параметры Windows',()=>run(api.systemLink(controls[q.toLowerCase()])));result('Поиск: '+q,'Открыть в браузере',()=>run(api.web('https://www.google.com/search?q='+encodeURIComponent(q))))}$('command').oninput=renderSearch;$('command').onkeydown=e=>{if(e.key==='Enter'&&firstAction){e.preventDefault();firstAction()}};
document.addEventListener('keydown',e=>{if(e.key==='Escape'&&!$('eq-dialog').open&&!$('power-dialog').open){if(miniOpen)setCompactPlayer(false);else expand(false)}});matchMedia('(prefers-color-scheme: light)').addEventListener('change',()=>settings&&applyAppearance());const clockFormat=new Intl.DateTimeFormat('ru-RU',{hour:'2-digit',minute:'2-digit'});function clock(){setText($('clock'),clockFormat.format(new Date()));if(state?.timer||recordStart||!$('pill-status').hidden)renderTimer()}setInterval(clock,1000);run(api.get(),r=>{settings=r.settings;state=r.state;steamDownload=r.steamDownload||'';renderTimer();systemLabel=r.systemLabel||'Windows';displays=r.displays;applyPlatformLabels();applyAppearance();renderApps();renderSystem();clock();$('weather-city').value=weatherCity;refreshWeather(weatherCity);if(r.hotkeys?.length)message('Недоступны сочетания: '+r.hotkeys.join(', '))});

const audioLevel = $('audio-level');
const audioDevice = $('audio-device');
let audioPending = false;
const audioQueue = [];
let displayedEndpoints;
let lastVolume;
function renderAudio(data) {
  if(Number.isFinite(data.level)&&lastVolume!==undefined&&data.level!==lastVolume&&!open)renderIndicator('♪ '+data.level+'%', 'Громкость '+data.level+' процентов');
  lastVolume=data.level;
  const endpoints = Object.entries(data.endpoints || {});
  const key = JSON.stringify(endpoints);
  if (key !== displayedEndpoints) {
    displayedEndpoints = key;
    audioDevice.replaceChildren(...endpoints.map(([id, name]) => {
      const option = text('option', name);
      option.value = id;
      return option;
    }));
    if (!endpoints.length) audioDevice.append(text('option', 'Нет устройств'));
  }
  audioDevice.disabled = !endpoints.length;
  if (data.id) audioDevice.value = data.id;
  audioDevice.title = data.endpoints?.[data.id] || '';
  audioLevel.disabled = $('audio-mute').disabled = !data.id;
  const level = audioQueue.findLast(([command]) => command === 'level')?.[1] ?? data.level;
  if (Number.isFinite(level)) audioLevel.value = level;
  $('audio-value').textContent = Number.isFinite(level) ? level : '—';
  audioLevel.style.setProperty('--volume', (Number.isFinite(level) ? level : 0) + '%');
  const muteLabel = data.muted ? 'Включить звук' : 'Выключить звук';
  $('audio-mute').setAttribute('aria-label', muteLabel);
  $('audio-mute').setAttribute('aria-pressed', String(Boolean(data.muted)));
  $('audio-mute').title = muteLabel;
}
async function changeAudio(command, value) {
  if (audioPending) {
    if (command === 'status') return;
    if (command === 'level' && audioQueue.at(-1)?.[0] === 'level') audioQueue[audioQueue.length - 1][1] = value;
    else audioQueue.push([command, value]);
    return;
  }
  audioPending = true;
  try {
    const result = await api.audio(command, value);
    if (result.error) throw Error(result.error);
    renderAudio(result);
  } catch (error) {
    if (command !== 'status') message(error.message);
    else {
      if (displayedEndpoints === undefined) message(error.message);
      renderAudio({});
    }
  } finally {
    audioPending = false;
    const next = audioQueue.shift();
    if (next) changeAudio(...next);
  }
}
audioLevel.oninput = () => {
  const next = Number(audioLevel.value);
  $('audio-value').textContent = next;
  audioLevel.style.setProperty('--volume', next + '%');
  changeAudio('level', next);
};
audioDevice.onchange = () => {
  changeAudio('select', audioDevice.value);
  audioDevice.blur();
};
$('audio-mute').onclick = () => changeAudio('mute');
setInterval(() => { if (open && tab === 'home') changeAudio('status'); }, 1200);
changeAudio('status');

const frequencies = [60, 120, 250, 500, 1000, 2000, 4000, 8000, 16000];
const eqSliders = frequencies.map(frequency => {
  const label = text('label', '', 'eq-band');
  const name = frequency >= 1000 ? frequency / 1000 + ' кГц' : frequency + ' Гц';
  const input = document.createElement('input');
  input.type = 'range';
  input.min = -12;
  input.max = 12;
  input.value = 0;
  input.setAttribute('aria-label', name);
  const output = text('span', '0 дБ');
  input.oninput = () => { output.textContent = (Number(input.value) > 0 ? '+' : '') + input.value + ' дБ'; };
  label.append(text('span', name), input, output);
  $('eq-bands').append(label);
  return input;
});
function setEqGains(gains) {
  eqSliders.forEach((input, i) => { input.value = gains[i] ?? 0; input.oninput(); });
}
$('eq-open').onclick = () => run(api.eqProfile(), profile => {
  setEqGains(profile.gains);
  $('eq-status').textContent = '';
  $('eq-dialog').showModal();
});
$('eq-reset').onclick = () => setEqGains([]);
function saveEq(copyInclude) {
  const gains = eqSliders.map(input => Number(input.value));
  run(api.eqProfile(gains), async profile => {
    if (copyInclude) {
      await run(api.copy('Include: ' + profile.file), () => {
        $('eq-status').textContent = 'Строка Include скопирована. Добавь её в config.txt Equalizer APO.';
      });
    } else {
      $('eq-status').textContent = 'Профиль сохранён в Orbit-EQ.txt.';
    }
  });
}
$('eq-save').onclick = () => saveEq(false);
$('eq-include').onclick = () => saveEq(true);

let happState;
let happInfo;
let happPending = false;
let happChanging = false;
const regionNames = new Intl.DisplayNames(['ru'], {type:'region'});
function renderHapp() {
  const badge = $('happ-vpn');
  const connected = happState?.mode === 'tun' || happState?.mode === 'proxy';
  badge.classList.toggle('is-on', connected);
  $('control-vpn').classList.toggle('is-on', connected);
  $('happ-label').textContent = happChanging ? '…' : connected ? happInfo?.country || 'Happ' : 'VPN';
  const status = !happState ? 'Проверяем Happ…' : !happState.installed ? 'Happ не найден' :
    connected ? 'Happ · подключён' : happState.mode === 'unknown' ? 'Happ · состояние неизвестно' : 'Happ · отключён';
  $('control-vpn-status').textContent = status;
  const details = happInfo ? [regionNames.of(happInfo.country), happInfo.ip] : [];
  if (happState?.adapter) details.push(happState.adapter);
  badge.title = [status, ...details].join(' · ') +
    '\nКлик — переключить Happ · правый клик — проверить внешний IP (IPinfo)';
}
async function refreshHapp() {
  if (happPending) return;
  happPending = true;
  try {
    const status = await api.happ('status');
    if (status.error) throw Error(status.error);
    if (status.mode !== happState?.mode) happInfo = undefined;
    happState = status;
    renderHapp();
  } catch {
    $('happ-vpn').title = 'Не удалось проверить Happ';
  } finally {
    happPending = false;
  }
}
async function toggleHapp() {
  if (happChanging) return;
  happChanging = true;
  happInfo = undefined;
  renderHapp();
  try {
    const result = await api.happ('toggle');
    if (result.error) throw Error(result.error);
    await new Promise(resolve => setTimeout(resolve, 1000));
    await refreshHapp();
  } catch (error) {
    message(error.message);
  } finally {
    happChanging = false;
    renderHapp();
  }
}
$('happ-vpn').onclick = $('control-vpn').onclick = toggleHapp;
$('happ-vpn').oncontextmenu = async event => {
  event.preventDefault();
  if (happChanging) return;
  await refreshHapp();
  const mode = happState?.mode;
  await run(api.happ('ip'), info => {
    if (happChanging || mode !== happState?.mode) return;
    happInfo = info;
    renderHapp();
    message(regionNames.of(info.country) + ' · ' + info.ip);
  });
};
setInterval(() => { if (open) refreshHapp(); }, 3500);

function applyPlatformLabels() {
  if (systemLabel !== 'Linux') return;
  $('dictation-hint').textContent = 'Офлайн · русский и английский';
  $('tg-pick').textContent = 'Выбрать файл';
  $('zapret-description').textContent = 'Управление установленной службой systemd.';
  $('zapret-install-hint').textContent = 'Установите службу zapret.service. Для переключения могут потребоваться системные права.';
  document.querySelector('[data-system="vpn"]').textContent = 'VPN системы';
}

function renderNetworkTools(status) {
  networkStatus = status;
  const zapret = status.zapret;
  $('zapret-state').textContent = zapret === 'Running' ? 'Включён' : zapret === 'Stopped' ? 'Выключен' :
    zapret === null ? 'Служба не установлена' : zapret ? 'Переходное состояние' : 'Статус недоступен';
  $('zapret-toggle').textContent = zapret === 'Running' ? 'Выключить' : 'Включить';
  $('zapret-toggle').disabled = networkBusy || !['Running', 'Stopped'].includes(zapret);
  $('tg-state').textContent = status.proxyConfigured === false ? 'Выберите файл приложения' :
    !status.proxyConfigured ? 'Статус недоступен' : status.proxyRunning ? 'Запущен' : 'Остановлен';
  $('tg-toggle').textContent = status.proxyRunning ? 'Выключить' : 'Включить';
  $('tg-toggle').disabled = networkBusy || !status.proxyConfigured;
  $('tg-pick').disabled = networkBusy;
}
async function refreshNetworkTools() {
  if (networkBusy) return;
  const result = await api.networkToolsStatus().catch(() => ({}));
  if (!networkBusy) renderNetworkTools(result?.error ? {} : result);
}
async function changeNetworkTool(operation) {
  if (networkBusy) return;
  networkBusy = true;
  renderNetworkTools(networkStatus || {});
  message('');
  try {
    const result = await operation();
    if (result.error) throw Error(result.error);
    renderNetworkTools(result);
  } catch (error) {
    message(error.message);
  } finally {
    networkBusy = false;
    renderNetworkTools(networkStatus || {});
    void refreshNetworkTools();
  }
}
$('zapret-toggle').onclick = () => changeNetworkTool(() => api.networkToolsSet('zapret', networkStatus?.zapret !== 'Running'));
$('tg-toggle').onclick = () => changeNetworkTool(() => api.networkToolsSet('tg', !networkStatus?.proxyRunning));
$('tg-pick').onclick = () => changeNetworkTool(() => api.networkToolsPickProxy());
$('zapret-guide').onclick = () => run(api.networkToolsLink('zapret'));
$('tg-guide').onclick = () => run(api.networkToolsLink('tg'));

let controlsState;
let controlsPending = false;
function renderControlTheme(dark) {
  $('control-theme').classList.toggle('is-on', dark);
  $('control-theme-status').textContent = dark ? 'Включена' : 'Выключена';
}
async function refreshControls() {
  if (controlsPending) return;
  controlsPending = true;
  try {
    const result = await api.controls('status');
    if (result.error) throw Error(result.error);
    controlsState = result;
    $('control-network-name').textContent = result.networkName;
    $('control-network-status').textContent = result.networkUp ? 'Подключён' : 'Отключён';
    $('control-network').classList.toggle('is-on', result.networkUp);
    $('control-bluetooth-status').textContent = {missing:'Нет адаптера',available:'Адаптер доступен',disabled:'Адаптер отключён'}[result.bluetooth];
    $('control-bluetooth').disabled = result.bluetooth === 'missing';
    renderControlTheme(result.dark);
  } catch (error) {
    message(error.message);
  } finally {
    controlsPending = false;
  }
}
$('control-network').onclick = () => run(api.systemLink(controlsState?.networkName === 'Wi-Fi' ? 'wifi' : 'ethernet'));
$('control-theme').onclick = () => run(api.controls('theme'), result => renderControlTheme(result.dark));
setInterval(() => { if (controlsWork) refreshControls(); }, 12000);
const powerDialog = $('power-dialog');
document.querySelectorAll('[data-power]').forEach(tile => {
  tile.onclick = () => {
    const command = tile.dataset.power;
    if (command === 'lock' || command === 'sleep') {
      run(api.controls(command));
      return;
    }
    $('power-title').textContent = command === 'restart' ? 'Перезагрузить компьютер?' : 'Выключить компьютер?';
    powerDialog.returnValue = 'cancel';
    powerDialog.onclose = () => {
      if (powerDialog.returnValue === 'confirm') run(api.controls(command));
    };
    powerDialog.showModal();
  };
});

function animatePlayback(playing) {
  const mascot = $('mascot');
  const changed = document.body.classList.contains('playing') !== playing;
  const motion = changed && !open && !matchMedia('(prefers-reduced-motion: reduce)').matches;
  const previous = motion ? mascot.getBoundingClientRect().left : 0;
  document.body.classList.toggle('playing', playing);
  updateMascot();
  if (!motion) return;
  mascotShift?.cancel();
  const distance = previous - mascot.getBoundingClientRect().left;
  if (distance) mascotShift = mascot.animate([{transform:'translateX('+distance+'px)'},{transform:'none'}],
    {duration:340,easing:'cubic-bezier(.22,1,.36,1)'});
}
function applyModules() {
  document.querySelectorAll('[data-module]').forEach(tile => {
    tile.hidden = settings.hiddenModules.includes(tile.dataset.module);
  });
  document.querySelectorAll('#tabs [data-tab]').forEach(button => {
    button.hidden = settings.hiddenTabs.includes(button.dataset.tab);
  });
  const tools = document.querySelector('.dashboard-tools');
  if (tools) tools.hidden = !tools.querySelector('button:not([hidden])');
}
function updateMascot() {
  const playing = Boolean(system.media?.playing);
  const preferred = playing && settings?.musicCharacter !== 'same' ? settings?.musicCharacter : settings?.character;
  const character = ['pug','sphere','eyes','spark','pixel'].includes(preferred) ? preferred : 'pug';
  const reaction = !$('alarm').hidden || !$('layout-indicator').hidden ? 'attention' :
    dropActive ? 'file' : playing ? 'music' : system.inactive ? 'sleep' : 'idle';
  if ($('mascot').className === 'orb ' + character && $('mascot').dataset.reaction === reaction) return;
  $('mascot').className = 'orb ' + character;
  $('mascot').dataset.reaction = reaction;
  syncIdleMotion();
  $('alarm-mascot').className = character === 'pixel' ? 'orb pixel' : 'orb';
  $('alarm-mascot').hidden = character !== 'pixel';
  $('alarm-mascot').dataset.reaction = 'attention';
  const dropPet = document.querySelector('[data-drop="shelf"] > span');
  if (dropPet) {
    dropPet.className = character === 'pixel' ? 'pixel' : '';
    dropPet.dataset.reaction = 'file';
    dropPet.textContent = character === 'pixel' ? '' : '▱';
  }
}
let mediaOptions = '';
let selectedMedia = '';
function mediaName(source) {
  if (/yandex.?music|ya\.music|yandex.*music/i.test(source)) return 'Яндекс Музыка';
  if (/spotify/i.test(source)) return 'Spotify';
  if (/msedge/i.test(source)) return 'Microsoft Edge';
  if (/chrom/i.test(source)) return 'Chrome';
  if (/firefox/i.test(source)) return 'Firefox';
  if (/yandex|browser\.exe/i.test(source)) return 'Яндекс Браузер';
  return source.replace(/\.exe$/i, '');
}
function renderMediaSources() {
  const select = $('media-source');
  const sources = system.sources || [];
  const key = JSON.stringify(sources);
  if (key !== mediaOptions) {
    mediaOptions = key;
    const auto = text('option', 'Авто');
    auto.value = '';
    select.replaceChildren(auto, ...sources.map(source => {
      const option = text('option', mediaName(source));
      option.value = source;
      return option;
    }));
  }
  select.value = sources.includes(selectedMedia) ? selectedMedia : '';
  select.hidden = !sources.length;
  select.title = system.media?.source ? 'Источник: ' + mediaName(system.media.source) : 'Источник музыки';
}
$('media-source').onchange = () => {
  selectedMedia = $('media-source').value;
  run(api.mediaSource(selectedMedia));
};
function updateCompactProgress() {
  const track = system.media;
  const position = playbackPosition(track);
  $('pill-progress').style.width = position === null ? '0%' : (100 * position / track.duration) + '%';
  const line = settings?.musicMode === 'lyrics' && position !== null ? lyricLines.findLast(item => item.at <= position) : null;
  setText($('pill-media-text'), line?.content || track?.title || '');
}
function renderCompactMedia() {
  const track = system.media;
  const detailed = track?.playing && ['title','lyrics','mini'].includes(settings?.musicMode);
  $('pill-media').hidden = !detailed;
  if (detailed) updateCompactProgress();
  $('mini-play').textContent = track?.playing ? 'Ⅱ' : '▶';
  if (!track?.playing && miniOpen) setCompactPlayer(false,false);
}
async function setCompactPlayer(value, notify = true) {
  const visible = notify ? await run(api.compactMini(value)) : value;
  miniOpen = Boolean(visible);
  $('compact-player').hidden = !miniOpen;
  $('pill').setAttribute('aria-expanded', String(open || miniOpen));
}

const alarms = [];
function queueAlarm(alarm) {
  const start = alarms.length === 0;
  alarms.push(alarm);
  if (start) renderAlarm();
}
function renderAlarm() {
  const alert = alarms[0];
  $('alarm').hidden = !alert;
  updateMascot();
  if (!alert) return;
  $('alarm-title').textContent = alert.title;
  $('alarm-body').textContent = alert.body;
  $('alarm-later').hidden = alert.kind !== 'reminder' && !(alert.kind === 'timer' && alert.total > 0);
  $('alarm-later').textContent = alert.kind === 'timer' ? 'Начать заново' : 'Через 10 мин';
  expand(true);
  try {
    const context = new AudioContext();
    const tone = context.createOscillator();
    const gain = context.createGain();
    tone.frequency.value = 660;
    gain.gain.value = .08;
    tone.connect(gain);
    gain.connect(context.destination);
    tone.start();
    tone.stop(context.currentTime + .3);
    tone.onended = () => context.close();
  } catch {}
}
$('alarm-dismiss').onclick = () => { alarms.shift(); renderAlarm(); };
$('alarm-later').onclick = () => {
  const alert = alarms[0];
  if (!alert) return;
  const operation = alert.kind === 'timer' ? api.timer({action:'start',minutes:alert.total/60000}) :
    api.reminder('add',{text:alert.body,at:Date.now()+600000,repeat:'once'});
  run(operation, result => {
    if (alert.kind === 'reminder' && state) {
      state.reminders = result;
      renderReminders();
    }
    if (alarms[0] === alert) { alarms.shift(); renderAlarm(); }
  });
};

let lastRainAlert = '';
function checkRain(data) {
  if (!settings?.rainAlerts) return;
  const { current, hourly } = data.forecast;
  if (!hourly || current.weather_code >= 51) return;
  const hour = hourly.time.findIndex(time => time > current.time);
  if (hour < 0 || hourly.precipitation_probability[hour] < 65) return;
  const key = data.location.name + ' ' + hourly.time[hour];
  if (key === lastRainAlert) return;
  lastRainAlert = key;
  renderIndicator('☂ Скоро осадки', 'Высокая вероятность осадков в ближайший час');
}
