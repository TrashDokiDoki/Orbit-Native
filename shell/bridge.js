(() => {
  'use strict';
  if (location.href !== __ORBIT_URL__) return;
  const token = __ORBIT_TOKEN__;
  const contract = __ORBIT_CONTRACT__;
  const invoke = window.orbitNativeInvoke.bind(window);
  const listeners = new Map();
  const call = (name, ...args) => invoke(token, name, args).catch(() => ({
    error: 'Связь с движком Orbit прервана'
  }));
  const api = {};
  for (const [name, channel] of Object.entries(contract.calls)) api[name] = (...args) => call(channel, ...args);
  for (const [name, channel] of Object.entries(contract.events)) {
    api[name] = listener => {
      if (typeof listener !== 'function') return;
      if (!listeners.has(channel)) listeners.set(channel, []);
      listeners.get(channel).push(listener);
    };
  }
  api.controlsView = visible => void call('controls-view', visible);
  api.dragFile = id => void call('drag-file', id);
  api.filePath = () => '';
  if (location.pathname.endsWith('/capture.html')) {
    api.qr = async source => {
      try {
        const image = new Image();
        image.src = source;
        await image.decode();
        const canvas = document.createElement('canvas');
        canvas.width = image.naturalWidth;
        canvas.height = image.naturalHeight;
        const ctx = canvas.getContext('2d', {willReadFrequently:true});
        ctx.drawImage(image, 0, 0);
        const data = ctx.getImageData(0, 0, canvas.width, canvas.height);
        const result = window.jsQR(data.data, data.width, data.height);
        if (!result) return {error:'QR-код не найден. Выделите область точнее.'};
        const copied = await api.copy(result.data);
        return copied?.error ? copied : {text:result.data};
      } catch {
        return {error:'Не удалось прочитать изображение QR-кода'};
      }
    };
    document.addEventListener('DOMContentLoaded', async () => {
      const snapshot = await call('capture-ready');
      if (!snapshot.error) window.orbitNativeDispatch('capture', snapshot);
    });
  }
  document.addEventListener('DOMContentLoaded', () => {
    const island = document.getElementById('island');
    if (!island) return;
    let pending;
    const updateShape = () => {
      clearTimeout(pending);
      pending = setTimeout(() => {
        const rect = island.getBoundingClientRect();
        const style = getComputedStyle(island);
        void call('island-shape', {x:rect.x,y:rect.y,width:rect.width,height:rect.height,
          top:parseFloat(style.borderTopLeftRadius),bottom:parseFloat(style.borderBottomLeftRadius)});
      }, 85);
    };
    new ResizeObserver(updateShape).observe(island);
    window.addEventListener('resize', updateShape);
    updateShape();
  });
  Object.defineProperty(window, 'orbit', { value: Object.freeze(api) });
  Object.defineProperty(window, 'orbitNativeDispatch', {
    value: (channel, value) => {
      for (const listener of listeners.get(channel) || []) listener(value);
    }
  });
})();
