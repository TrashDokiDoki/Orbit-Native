const $ = id => document.getElementById(id);
const api = window.orbit;
const fields = ['accent','theme','character','shape','position','top','scale','opacity','shortcut','monitor','musicCharacter','musicMode','hoverDelay','leaveDelay','eventDuration','restrictedApps','ruleMode'];
const toggles = ['autostart','hover','collapseOnLeave','autohide','hideFullscreen','clipboard','clickThrough','rainAlerts','gpuAcceleration'];
let settings;
let previewReaction = 'idle';
let pendingSection = { section: 'general' };

function element(tag, value, className) {
  const node = document.createElement(tag);
  if (value) node.textContent = value;
  if (className) node.className = className;
  return node;
}

async function run(operation, success) {
  try {
    const result = await operation;
    if (result?.error) throw Error(result.error);
    success?.(result);
  } catch (error) {
    $('message').textContent = error.message;
  }
}

function addEntry(item = {}) {
  const root = element('div', '', 'entry');
  const input = (key, label, fallback = '') => {
    const field = element('input');
    field.value = item[key] || fallback;
    field.placeholder = label;
    field.setAttribute('aria-label', label);
    return field;
  };
  const name = input('name', 'Название');
  const target = input('target', 'Ссылка или путь');
  const symbol = input('symbol', 'Значок', '↗');
  const group = input('group', 'Группа');
  const hotkey = input('hotkey', 'Клавиши');
  const browse = element('button', '…');
  browse.type = 'button';
  browse.setAttribute('aria-label', 'Выбрать приложение или ярлык');
  browse.onclick = () => run(api.pick(), path => { if (path) target.value = path; });
  const remove = element('button', '×');
  remove.type = 'button';
  remove.setAttribute('aria-label', 'Удалить ярлык');
  remove.onclick = () => root.remove();
  symbol.className = 'entry-symbol';
  const row = element('div', '', 'entry-row');
  const pathRow = element('div', '', 'entry-row');
  const options = element('div', '', 'entry-row');
  row.append(symbol, name, remove);
  pathRow.append(target, browse);
  options.append(group, hotkey);
  root.append(row, pathRow, options);
  root.read = () => ({ ...item, name: name.value.trim(), target: target.value.trim(),
    symbol: symbol.value.trim(), group: group.value.trim(), hotkey: hotkey.value.trim() });
  $('entries').append(root);
  return name;
}

function showSection({ section, add }) {
  if (!settings) { pendingSection = { section, add }; return; }
  const tab = [...document.querySelectorAll('[data-section]')].find(button => button.dataset.section === section);
  if (!tab) return;
  const slot=document.querySelector('[data-preview-slot='+section+']');
  if(slot)slot.append($('island-preview'));
  document.querySelectorAll('[data-section]').forEach(button => {
    if (button === tab) button.setAttribute('aria-current', 'page');
    else button.removeAttribute('aria-current');
  });
  document.querySelectorAll('[data-page]').forEach(page => { page.hidden = page.dataset.page !== section; });
  document.querySelector('.settings-scroll').scrollTop = 0;
  if (section === 'updates' && !$('check-update').disabled) run(api.updateStatus(), renderUpdate);
  if (add) $('add').click();
}

function updatePreview() {
  const preview = document.querySelector('.preview-island');
  const chosen=$('musicCharacter').value;
  const character = previewReaction==='music'&&chosen!=='same'?chosen:$('character').value;
  const pet=$('preview-pet');
  pet.className='preview-pet'+(character==='pixel'?' pixel':'');
  pet.dataset.reaction=previewReaction;
  preview.dataset.shape = $('shape').value;
  preview.dataset.theme = $('theme').value;
  preview.style.setProperty('--accent', $('accent').value);
  preview.querySelector('img').hidden = character !== 'pug';
  const symbol = preview.querySelector('.preview-symbol');
  symbol.hidden = character === 'pug'||character==='pixel';
  symbol.textContent = ({ sphere: '●', eyes: '▮ ▮', spark: '✦' })[character] || '';
}

function renderUpdate(result) {
  $('update-current').textContent = result.current;
  $('update-release').hidden = !result.available;
  $('update-status').textContent = result.available ? 'Доступна версия ' + result.latest :
    result.latest === undefined ? 'Проверка через GitHub' :
    result.latest === null ? 'Опубликованных релизов нет' : 'Установлена актуальная версия';
}

$('check-update').onclick = async () => {
  const check = $('check-update');
  check.disabled = true;
  $('update-status').textContent = 'Проверяем…';
  try {
    const result = await api.checkUpdate();
    if (result.error) throw Error(result.error);
    renderUpdate(result);
  } catch {
    $('update-status').textContent = 'Не удалось проверить обновления. Повторите позже.';
  } finally {
    check.disabled = false;
  }
};
$('open-update').onclick = () => run(api.openUpdate());

document.querySelectorAll('[data-section]').forEach(button => {
  button.onclick = () => showSection({ section: button.dataset.section });
});
$('add').onclick = () => {
  if ($('entries').children.length >= 24) $('message').textContent = 'Максимум 24 ярлыка';
  else addEntry().focus();
};
const modules = {player:'Плеер',audio:'Громкость',apps:'Лаунчер',edit:'Область',full:'Весь экран',ocr:'Текст с экрана',qr:'QR-код',color:'Пипетка',text:'Чистый текст',record:'Запись экрана',command:'Команда'};
const tabs = {clipboard:'Буфер',shelf:'Полка',notes:'Заметки',tools:'Инструменты',weather:'Погода',controls:'Управление'};
function addModuleChoices(root, choices, field) {
  for (const [key,name] of Object.entries(choices)) {
    const label=element('label', '', 'module-option');
    const checkbox=element('input');
    checkbox.type='checkbox';
    checkbox.dataset.choice=field;
    checkbox.value=key;
    label.append(checkbox,element('span',name));
    root.append(label);
  }
}
function updateModulePreview() {
  const root=$('module-preview');
  root.replaceChildren(...[...document.querySelectorAll('[data-choice=hiddenModules]:checked')].map(input=>{
    const tile=element('div', modules[input.value]);
    tile.dataset.previewModule=input.value;
    return tile;
  }));
  if(!root.children.length)root.append(element('p','Главная без модулей'));
}
addModuleChoices($('module-options'),modules,'hiddenModules');
addModuleChoices($('tab-options'),tabs,'hiddenTabs');
document.querySelectorAll('.preview-modes [data-reaction]').forEach(button=>{
  button.onclick=()=>{
    previewReaction=button.dataset.reaction;
    document.querySelectorAll('.preview-modes [data-reaction]').forEach(option=>option.setAttribute('aria-pressed',String(option===button)));
    updatePreview();
  };
});
$('preview-expand').onclick=()=>{
  const button=$('preview-expand');
  const expanded=button.getAttribute('aria-pressed')!=='true';
  button.setAttribute('aria-pressed',String(expanded));
  button.textContent=expanded?'Свернуть':'Раскрыть';
  document.querySelector('.preview-island').classList.toggle('preview-open',expanded);
};
$('config').oninput = ()=>{updatePreview();updateModulePreview()};
$('config').onsubmit = event => {
  event.preventDefault();
  if (!settings) return;
  const next = { ...settings, items: [...$('entries').children].map(entry => entry.read()) };
  if (next.items.some(item => !item.name || !item.target)) {
    $('message').textContent = 'Заполни название и путь для каждого ярлыка';
    showSection({ section: 'apps' });
    return;
  }
  fields.forEach(id => { next[id] = $(id).value; });
  toggles.forEach(id => { next[id] = $(id).checked; });
  document.querySelectorAll('[data-choice]').forEach(input=>{const key=input.dataset.choice;next[key]=[...document.querySelectorAll('[data-choice='+key+']:not(:checked)')].map(choice=>choice.value)});
  run(api.save(next), result => {
    settings = result.settings;
    $('message').textContent = result.hotkeys.length ? 'Сохранено. Недоступны сочетания: ' + result.hotkeys.join(', ') : 'Настройки сохранены';
  });
};
$('backup').onclick = () => run(api.dataExport(), saved => {
  if (saved) $('message').textContent = 'Резервная копия сохранена';
});
$('data-folder').onclick = () => run(api.dataFolder());
$('quit').onclick = () => api.quit();
api.onSettingsSection(showSection);
api.onLauncherAdded(items => { items.forEach(item => addEntry(item)); });

$('preview-clock').textContent = new Date().toLocaleTimeString('ru-RU', {hour:'2-digit', minute:'2-digit'});
run(api.get(), result => {
  settings = result.settings;
  $('version').textContent = 'Версия ' + result.version;
  $('monitor').replaceChildren(...result.displays.map(display => {
    const option = element('option', display.label);
    option.value = display.index;
    return option;
  }));
  fields.forEach(id => { $(id).value = settings[id]; });
  toggles.forEach(id => { $(id).checked = Boolean(settings[id]); });
  document.querySelectorAll('[data-choice]').forEach(input=>{input.checked=!settings[input.dataset.choice].includes(input.value)});
  updateModulePreview();
  settings.items.forEach(addEntry);
  updatePreview();
  showSection(pendingSection);
});
