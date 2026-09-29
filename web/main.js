// Jumping Jack - browser host for the WebAssembly game core.
(async () => {
  const W = 320, H = 240, FPS = 50, STEP = 1000 / FPS;
  const KEY = { LEFT: 1, RIGHT: 2, JUMP: 4, START: 8, PAUSE: 16 };
  const KEYMAP = {
    ArrowLeft: KEY.LEFT, KeyZ: KEY.LEFT, KeyO: KEY.LEFT,
    ArrowRight: KEY.RIGHT, KeyX: KEY.RIGHT, KeyP: KEY.RIGHT,
    Space: KEY.JUMP, ArrowUp: KEY.JUMP, KeyQ: KEY.JUMP,
    Enter: KEY.START, KeyH: KEY.PAUSE, Escape: KEY.PAUSE,
  };
  const HISCORE_KEY = 'jumpingjack.hiscore';

  const canvas = document.getElementById('screen');
  const ctx2d = canvas.getContext('2d');

  async function loadWasm(url) {
    try {
      return (await WebAssembly.instantiateStreaming(fetch(url), {})).instance;
    } catch (err) {
      // Some servers send a wrong MIME type for .wasm; fall back to bytes.
      console.warn('instantiateStreaming failed, falling back:', err);
      const bytes = await (await fetch(url)).arrayBuffer();
      return (await WebAssembly.instantiate(bytes, {})).instance;
    }
  }

  const ex = (await loadWasm('jumpingjack.wasm')).exports;

  function readHiscore() {
    try {
      return parseInt(localStorage.getItem(HISCORE_KEY) || '0', 10) || 0;
    } catch (err) {
      console.warn('localStorage unavailable:', err);
      return 0;
    }
  }
  function writeHiscore(v) {
    try {
      localStorage.setItem(HISCORE_KEY, String(v));
    } catch (err) {
      console.warn('localStorage unavailable:', err);
    }
  }

  ex.jj_set_hiscore(readHiscore());
  ex.jj_init((Math.random() * 0xffffffff) >>> 0);
  let savedHi = ex.jj_hiscore();

  // ---- input
  let keys = 0;
  window.addEventListener('keydown', (e) => {
    if (e.code === 'KeyM') { muted = !muted; return; }
    const k = KEYMAP[e.code];
    if (k === undefined) return;
    e.preventDefault();
    keys |= k;
    ensureAudio();
  });
  window.addEventListener('keyup', (e) => {
    const k = KEYMAP[e.code];
    if (k === undefined) return;
    e.preventDefault();
    keys &= ~k;
  });
  window.addEventListener('blur', () => { keys = 0; });
  // Touch buttons: each finger is its own pointer, so move + jump works at once.
  for (const b of document.querySelectorAll('#touch button')) {
    const k = parseInt(b.dataset.key, 10);
    const release = () => { keys &= ~k; b.classList.remove('on'); };
    b.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      // Touch implicitly captures the pointer; release it so sliding off fires pointerleave.
      if (b.hasPointerCapture(e.pointerId)) b.releasePointerCapture(e.pointerId);
      keys |= k;
      b.classList.add('on');
      ensureAudio();
    });
    for (const ev of ['pointerup', 'pointercancel', 'pointerleave']) b.addEventListener(ev, release);
    b.addEventListener('contextmenu', (e) => e.preventDefault());
  }
  // Tapping the screen starts the game / skips the limerick.
  canvas.addEventListener('pointerdown', (e) => { e.preventDefault(); ensureAudio(); keys |= KEY.START; });
  canvas.addEventListener('pointerup', () => { keys &= ~KEY.START; });
  canvas.addEventListener('pointercancel', () => { keys &= ~KEY.START; });
  document.addEventListener('visibilitychange', () => { keys = 0; });

  // ---- audio (browsers only allow it after a user gesture)
  let actx = null, audioTime = 0, muted = false;
  function ensureAudio() {
    if (actx) {
      // iOS suspends the context until a gesture; resume on every gesture.
      if (actx.state === 'suspended') actx.resume().catch((err) => console.warn('audio resume failed:', err));
      return;
    }
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) { console.warn('Web Audio not supported'); return; }
    actx = new AC();
    ex.jj_set_sample_rate(actx.sampleRate);
    audioTime = actx.currentTime + 0.05;
  }
  function pushAudio() {
    if (!actx || muted) return;
    const n = ex.jj_audio_len();
    if (!n) return;
    const src = new Float32Array(ex.memory.buffer, ex.jj_audio(), n);
    const buf = actx.createBuffer(1, n, actx.sampleRate);
    buf.copyToChannel(src, 0);
    const node = actx.createBufferSource();
    node.buffer = buf;
    node.connect(actx.destination);
    const now = actx.currentTime;
    if (audioTime < now + 0.01 || audioTime > now + 0.25) audioTime = now + 0.05;
    node.start(audioTime);
    audioTime += n / actx.sampleRate;
  }

  // ---- main loop at a fixed 50 Hz
  const image = ctx2d.createImageData(W, H);
  let last = performance.now(), acc = 0;
  function frame(now) {
    acc += Math.min(now - last, 250);
    last = now;
    let ticked = false;
    while (acc >= STEP) {
      ex.jj_tick(keys);
      pushAudio();
      acc -= STEP;
      ticked = true;
    }
    if (ticked) {
      image.data.set(new Uint8ClampedArray(ex.memory.buffer, ex.jj_framebuffer(), W * H * 4));
      ctx2d.putImageData(image, 0, 0);
      const hi = ex.jj_hiscore();
      if (hi > savedHi) { savedHi = hi; writeHiscore(hi); }
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
})().catch((err) => {
  console.error(err);
  document.body.textContent = 'Failed to start Jumping Jack: ' + err;
});
