// TD5 Car Studio — 3D authoring viewport for custom cars.
// Replicates re/tools/td5_car_import.py's transform (axis remap -> centre ->
// scale-to-length-1500) so the carparam wheel gizmos align with the mesh.
import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { OBJLoader } from 'three/addons/loaders/OBJLoader.js';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

const TARGET_LENGTH = 1500;            // must match importer TARGET_LENGTH
const $ = (id) => document.getElementById(id);

// ---- scene -----------------------------------------------------------------
const wrap = $('canvasWrap');
const renderer = new THREE.WebGLRenderer({ antialias: true, preserveDrawingBuffer: true });
renderer.setPixelRatio(devicePixelRatio);
wrap.appendChild(renderer.domElement);
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x12141a);
const camera = new THREE.PerspectiveCamera(45, 1, 1, 100000);
camera.position.set(1700, 1100, 2300);
const controls = new OrbitControls(camera, renderer.domElement);
controls.target.set(0, 0, 0);
controls.enableDamping = true;

scene.add(new THREE.HemisphereLight(0xffffff, 0x35404f, 1.05));
const sun = new THREE.DirectionalLight(0xffffff, 1.4);
sun.position.set(900, 1600, 1200);
scene.add(sun);
const grid = new THREE.GridHelper(4000, 20, 0x2c3340, 0x1e2430);
scene.add(grid);
scene.add(new THREE.AxesHelper(400));   // X=red(right) Y=green(up) Z=blue(length)

const modelRoot = new THREE.Group();    // holds the transformed model
scene.add(modelRoot);
const wheelGroup = new THREE.Group();   // wheel gizmos in game space
scene.add(wheelGroup);

function resize() {
  const w = wrap.clientWidth, h = wrap.clientHeight;
  renderer.setSize(w, h);   // updateStyle=true: pin canvas CSS to w×h so the
                            // drawing buffer (×devicePixelRatio) can't overflow
                            // the wrap and cover the side panel on HiDPI screens.
  camera.aspect = w / h; camera.updateProjectionMatrix();
}
addEventListener('resize', resize); resize();
(function loop() { requestAnimationFrame(loop); controls.update(); renderer.render(scene, camera); })();

// ---- state -----------------------------------------------------------------
let loadedObject = null;     // raw parsed THREE object
let modelBytes = null;       // ArrayBuffer of the uploaded file (for build)
let modelName = '';
let skinTexture = null;      // applied to preview materials
let skinDataURL = null;      // sent to build
let currentCarparam = null;  // donor carparam.json (edited in place)
let opts = { forward: '-z', up: 'y', scale: 'auto', flipv: false, showWheels: true };

// ---- transform (mirrors importer _axis_remap + centre + scale) -------------
function basisMatrix(up, forward) {
  const ax = { '+x': [1,0,0], '-x': [-1,0,0], '+y': [0,1,0], '-y': [0,-1,0], '+z': [0,0,1], '-z': [0,0,-1] };
  const u = ax[up === 'y' ? '+y' : '+z'], f = ax[forward];
  let r = [u[1]*f[2]-u[2]*f[1], u[2]*f[0]-u[0]*f[2], u[0]*f[1]-u[1]*f[0]];   // cross(u,f)
  if (Math.hypot(r[0], r[1], r[2]) < 1e-6) r = [1,0,0];
  const n = Math.hypot(r[0], r[1], r[2]) || 1; r = r.map(c => c / n);
  const m = new THREE.Matrix4();
  m.set(r[0],r[1],r[2],0,  u[0],u[1],u[2],0,  f[0],f[1],f[2],0,  0,0,0,1);   // rows r,u,f
  return m;
}

function applyTransform() {
  modelRoot.clear();
  if (!loadedObject) { updateWheels(); return; }
  const B = basisMatrix(opts.up, opts.forward);
  const holder = new THREE.Group();
  loadedObject.matrixAutoUpdate = false;
  loadedObject.matrix.copy(B);
  holder.add(loadedObject);
  holder.updateMatrixWorld(true);

  const box = new THREE.Box3().setFromObject(holder);
  const size = new THREE.Vector3(), centre = new THREE.Vector3();
  box.getSize(size); box.getCenter(centre);
  const length = Math.max(size.z, 1e-6);
  const s = (opts.scale === 'auto' || isNaN(parseFloat(opts.scale)))
            ? TARGET_LENGTH / length : parseFloat(opts.scale);

  const pivot = new THREE.Group();
  pivot.position.copy(centre.clone().multiplyScalar(-1));
  pivot.add(holder);
  modelRoot.scale.setScalar(s);
  modelRoot.add(pivot);

  $('modelInfo').innerHTML =
    `<b>${modelName}</b> · ${Math.round(size.x*s)}×${Math.round(size.y*s)}×${Math.round(size.z*s)} (W×H×L) · ×${s.toFixed(3)}`;
  $('hudStats').innerHTML = `scale ×${s.toFixed(3)} · length ${Math.round(size.z*s)}u`;
  applySkin();
  updateWheels();
}

// ---- wheels ----------------------------------------------------------------
const wheelGeo = new THREE.CylinderGeometry(95, 95, 60, 24);
wheelGeo.rotateZ(Math.PI / 2);  // axle along X
const wheelMat = new THREE.MeshStandardMaterial({ color: 0x36d6ff, transparent: true,
  opacity: 0.55, emissive: 0x0a3a4a });

function updateWheels() {
  wheelGroup.clear();
  wheelGroup.visible = opts.showWheels;
  if (!currentCarparam) return;
  for (const key of ['wheel_pos_FL', 'wheel_pos_FR', 'wheel_pos_RL', 'wheel_pos_RR']) {
    const f = currentCarparam[key];
    if (!f || !Array.isArray(f.value)) continue;
    const [x, y, z] = f.value;
    const w = new THREE.Mesh(wheelGeo, wheelMat);
    w.position.set(x, y, z);
    wheelGroup.add(w);
  }
}

// ---- skin ------------------------------------------------------------------
function applySkin() {
  if (!loadedObject) return;
  loadedObject.traverse((o) => {
    if (!o.isMesh) return;
    if (skinTexture) {
      const m = new THREE.MeshStandardMaterial({ map: skinTexture, roughness: 0.75, metalness: 0.05 });
      o.material = m;
    } else if (!o.material || !o.material.__studioGrey) {
      o.material = new THREE.MeshStandardMaterial({ color: 0x9a9aa2, roughness: 0.8 });
      o.material.__studioGrey = true;
    }
  });
}

// ---- model loading ---------------------------------------------------------
const gltf = new GLTFLoader(), obj = new OBJLoader();
$('modelFile').addEventListener('change', (e) => {
  const file = e.target.files[0]; if (!file) return;
  const fr = new FileReader();
  fr.onload = () => {
    modelBytes = fr.result; modelName = file.name;
    const ext = file.name.toLowerCase().split('.').pop();
    try {
      if (ext === 'obj') {
        loadedObject = obj.parse(new TextDecoder().decode(fr.result));
        applyTransform();
      } else {
        gltf.parse(fr.result, '', (g) => { loadedObject = g.scene; applyTransform(); },
          (err) => setStatus('glTF parse error: ' + err, 'bad'));
      }
    } catch (err) { setStatus('model load error: ' + err, 'bad'); }
  };
  fr.readAsArrayBuffer(file);
});

$('skinFile').addEventListener('change', (e) => {
  const file = e.target.files[0]; if (!file) return;
  const fr = new FileReader();
  fr.onload = () => {
    skinDataURL = fr.result;
    new THREE.TextureLoader().load(fr.result, (t) => {
      t.colorSpace = THREE.SRGBColorSpace; t.flipY = opts.flipv;
      skinTexture = t; $('skinInfo').textContent = file.name; applySkin();
    });
  };
  fr.readAsDataURL(file);
});

// ---- option controls -------------------------------------------------------
function seg(id, key) {
  $(id).querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
    $(id).querySelectorAll('button').forEach((x) => x.classList.remove('on'));
    b.classList.add('on'); opts[key] = b.dataset.v; applyTransform();
  }));
}
seg('segFwd', 'forward'); seg('segUp', 'up');
$('scale').addEventListener('change', (e) => { opts.scale = e.target.value.trim() || 'auto'; applyTransform(); });
$('flipv').addEventListener('change', (e) => { opts.flipv = e.target.checked; if (skinTexture) { skinTexture.flipY = opts.flipv; skinTexture.needsUpdate = true; } });
$('showWheels').addEventListener('change', (e) => { opts.showWheels = e.target.checked; updateWheels(); });

// ---- physics reference (effect hints + fleet ranges + presets) -------------
let reference = null;        // /api/reference payload
let compareVals = null;      // {field: value} of the compare car (or null)
const fieldUI = {};          // key -> { setVal(v), bar, cmp }

function fieldHint(key) { return reference && reference.fields[key] ? reference.fields[key].hint : null; }
function fleetField(key) {
  const f = reference && reference.fields[key];
  return f && f.kind === 'scalar' ? f : null;
}
function fpct(f, v) { const r = (f.max - f.min) || 1; return Math.max(0, Math.min(1, (v - f.min) / r)) * 100; }

function makeFleetBar(key) {
  const f = fleetField(key); if (!f) return null;
  const wrap = document.createElement('div');
  const bar = document.createElement('div'); bar.className = 'fleetbar';
  for (const [v, cls] of [[f.min, ''], [f.median, 'med'], [f.max, '']]) {
    const t = document.createElement('div'); t.className = 'fb-tick ' + cls; t.style.left = fpct(f, v) + '%'; bar.appendChild(t);
  }
  const ghost = document.createElement('div'); ghost.className = 'fb-ghost'; ghost.style.display = 'none';
  const mark = document.createElement('div'); mark.className = 'fb-mark';
  bar.append(ghost, mark);
  const ends = document.createElement('div'); ends.className = 'fb-ends';
  ends.innerHTML = `<span title="lowest: ${f.min_car.name}">${f.min}</span>`
    + `<span>med ${Math.round(f.median)}</span>`
    + `<span title="highest: ${f.max_car.name}">${f.max}</span>`;
  wrap.append(bar, ends);
  return {
    el: wrap,
    update(v) { mark.style.left = fpct(f, v) + '%'; mark.classList.toggle('out', v < f.min || v > f.max); },
    ghost(v) { if (v == null) { ghost.style.display = 'none'; } else { ghost.style.display = ''; ghost.style.left = fpct(f, v) + '%'; } },
  };
}

function applyCompareGhosts() {
  for (const key in fieldUI) {
    const ui = fieldUI[key]; if (!ui) continue;
    const cv = compareVals ? compareVals[key] : undefined;
    if (ui.bar) ui.bar.ghost(cv == null || Array.isArray(cv) ? null : cv);
    if (ui.cmp) {
      if (cv == null || Array.isArray(cv)) { ui.cmp.style.display = 'none'; }
      else { ui.cmp.style.display = ''; ui.cmp.textContent = '→ ' + cv; ui.cmp.onclick = () => ui.setVal(cv); }
    }
  }
}

function applyPreset(name) {
  const preset = reference && reference.presets ? reference.presets[name] : null;
  if (!preset || !currentCarparam) return;
  for (const [k, val] of Object.entries(preset)) {
    if (!currentCarparam[k]) continue;
    currentCarparam[k].value = Array.isArray(val) ? val.slice() : val;
    if (fieldUI[k]) fieldUI[k].setVal(currentCarparam[k].value);
  }
  setStatus(`Applied "${name}" preset — tune from here, then Build.`, '');
}

async function loadReference() {
  try { reference = await (await fetch('/api/reference')).json(); }
  catch (e) { reference = null; return; }
  const ps = $('presetSel'); ps.innerHTML = '';
  for (const name of Object.keys(reference.presets || {})) {
    const n = reference.preset_members[name] ? reference.preset_members[name].length : 0;
    const o = document.createElement('option'); o.value = name; o.textContent = `${name} (${n} cars)`; ps.appendChild(o);
  }
  const cs = $('compareSel');
  for (const c of reference.cars || []) {
    const o = document.createElement('option'); o.value = c.code; o.textContent = `${c.name} (${c.code})`; cs.appendChild(o);
  }
}
async function loadCompare(code) {
  if (!code) { compareVals = null; applyCompareGhosts(); return; }
  const r = await fetch('/api/carparam?donor=' + encodeURIComponent(code));
  const cp = r.ok ? await r.json() : null;
  compareVals = {};
  if (cp) for (const [k, v] of Object.entries(cp)) if (v && v.kind === 'live') compareVals[k] = v.value;
  applyCompareGhosts();
}

// ---- carparam form ---------------------------------------------------------
const GROUPS = [
  ['Wheels & body', (k) => k.startsWith('wheel_pos_') || k === 'suspension_height_ref' || k === 'envelope_reference_y'],
  ['Engine', (k) => ['torque_curve','max_rpm','drive_torque_multiplier','top_speed_limit','vehicle_inertia'].includes(k)],
  ['Drivetrain', (k) => ['drivetrain_type','gear_ratio_table','upshift_rpm_table','downshift_rpm_table','half_wheelbase'].includes(k)],
  ['Suspension', (k) => k.startsWith('suspension_') || k.startsWith('damping_')],
  ['Grip & brakes', (k) => ['brake_force','engine_brake_force','handbrake_grip_modifier','lateral_slip_stiffness','drag_coefficient'].includes(k)],
  ['Mass & balance', (k) => ['collision_mass','front_weight_dist','rear_weight_dist'].includes(k)],
];

function buildForm() {
  const host = $('physics'); host.innerHTML = '';
  Object.keys(fieldUI).forEach((k) => delete fieldUI[k]);
  if (!currentCarparam) return;
  const live = Object.entries(currentCarparam).filter(([, v]) => v && v.kind === 'live');
  const used = new Set();
  for (const [title, test] of GROUPS) {
    const fields = live.filter(([k]) => test(k) && !used.has(k));
    if (!fields.length) continue;
    fields.forEach(([k]) => used.add(k));
    host.appendChild(groupEl(title, fields));
  }
  const rest = live.filter(([k]) => !used.has(k));
  if (rest.length) host.appendChild(groupEl('Other', rest));
  applyCompareGhosts();
}

function addHintLine(body, key, fld) {
  const hint = fieldHint(key);
  if (!hint || !hint.effect) return null;
  const meta = document.createElement('div'); meta.className = 'fmeta';
  const e = document.createElement('div'); e.className = 'eff';
  e.innerHTML = hint.more ? `${hint.effect} &middot; <b>↑</b> ${hint.more}` : hint.effect;
  meta.appendChild(e);
  body.appendChild(meta);
  return meta;
}

function groupEl(title, fields) {
  const d = document.createElement('details'); d.className = 'grp';
  d.innerHTML = `<summary>${title}</summary>`;
  const body = document.createElement('div'); body.className = 'body'; d.appendChild(body);
  for (const [key, fld] of fields) {
    const row = document.createElement('label'); row.className = 'row';
    const lbl = document.createElement('span'); lbl.textContent = key.replace(/_/g, ' ').replace('wheel pos ', 'wheel ');
    const hint = fieldHint(key);
    lbl.title = (hint && hint.effect) || fld.description || '';
    row.appendChild(lbl);

    if (key.startsWith('wheel_pos_') && Array.isArray(fld.value)) {
      const box = document.createElement('div'); box.className = 'xyz';
      const inps = ['x','y','z'].map((ax, i) => {
        const inp = document.createElement('input'); inp.type = 'number'; inp.value = fld.value[i]; inp.title = ax.toUpperCase();
        inp.addEventListener('input', () => { fld.value[i] = parseFloat(inp.value) || 0; updateWheels(); });
        box.appendChild(inp); return inp;
      });
      row.appendChild(box); body.appendChild(row);
      fieldUI[key] = { bar: null, cmp: null,
        setVal: (arr) => { for (let i = 0; i < 3; i++) { fld.value[i] = arr[i]; inps[i].value = arr[i]; } updateWheels(); } };
    } else if (Array.isArray(fld.value)) {
      const inp = document.createElement('input'); inp.type = 'text'; inp.value = fld.value.join(', ');
      inp.addEventListener('change', () => { const a = inp.value.split(',').map((s) => parseFloat(s.trim()) || 0); if (a.length === fld.value.length) fld.value = a; });
      row.appendChild(inp); body.appendChild(row);
      addHintLine(body, key, fld);
      fieldUI[key] = { bar: null, cmp: null,
        setVal: (arr) => { if (Array.isArray(arr)) { for (let i = 0; i < fld.value.length && i < arr.length; i++) fld.value[i] = arr[i]; inp.value = fld.value.join(', '); } } };
    } else {
      const inp = document.createElement('input'); inp.type = 'number'; inp.value = fld.value;
      const bar = makeFleetBar(key);
      inp.addEventListener('input', () => { fld.value = parseFloat(inp.value) || 0; if (bar) bar.update(fld.value); });
      row.appendChild(inp); body.appendChild(row);
      const meta = addHintLine(body, key, fld) || (() => { const m = document.createElement('div'); m.className = 'fmeta'; body.appendChild(m); return m; })();
      if (bar) { bar.update(fld.value); meta.appendChild(bar.el); }
      const cmp = document.createElement('span'); cmp.className = 'cmpval'; cmp.style.display = 'none'; cmp.title = 'compare car value — click to copy'; meta.appendChild(cmp);
      fieldUI[key] = { bar, cmp,
        setVal: (v) => { fld.value = v; inp.value = v; if (bar) bar.update(v); } };
    }
  }
  return d;
}

// ---- donors ----------------------------------------------------------------
async function loadDonors() {
  const sel = $('donor');
  const donors = await (await fetch('/api/donors')).json();
  sel.innerHTML = '';
  for (const d of donors) {
    if (d.custom) continue;   // donate physics from a stock car
    const o = document.createElement('option'); o.value = d.code; o.textContent = `${d.name} (${d.code})`;
    sel.appendChild(o);
  }
  if ([...sel.options].some((o) => o.value === 'vip')) sel.value = 'vip';
  await loadCarparam(sel.value);
  sel.addEventListener('change', () => loadCarparam(sel.value));
}
async function loadCarparam(donor) {
  const r = await fetch('/api/carparam?donor=' + encodeURIComponent(donor));
  currentCarparam = r.ok ? await r.json() : null;
  buildForm(); updateWheels();
}

// ---- build -----------------------------------------------------------------
function abToB64(ab) {
  let bin = ''; const b = new Uint8Array(ab), chunk = 0x8000;
  for (let i = 0; i < b.length; i += chunk) bin += String.fromCharCode.apply(null, b.subarray(i, i + chunk));
  return btoa(bin);
}
function setStatus(msg, cls) { const s = $('status'); s.className = cls || ''; s.textContent = msg; }

$('buildBtn').addEventListener('click', async () => {
  if (!modelBytes) { setStatus('Load a model first.', 'warn'); return; }
  setStatus('Building…');
  let carpic = null;
  if ($('useView').checked) { try { carpic = renderer.domElement.toDataURL('image/png'); } catch (e) {} }
  const payload = {
    name: $('name').value, short: $('short').value || $('name').value,
    code: $('code').value.trim(), donor: $('donor').value,
    scale: opts.scale, forward: opts.forward, up: opts.up, flip_v: opts.flipv,
    model_name: modelName, model_b64: abToB64(modelBytes),
    skin_b64: skinDataURL, carpic_b64: carpic, carparam: currentCarparam,
  };
  try {
    const r = await fetch('/api/build', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
    const res = await r.json();
    if (res.ok) {
      const doc = (res.doctor || '').trim();
      const cls = /FAIL/.test(doc) ? 'bad' : /WARN/.test(doc) ? 'warn' : 'ok';
      setStatus(`✔ Built ${res.code}\n${res.dir}\n\n${doc}\n\nLaunch the game → Quick Race → SELECT CAR.`, cls);
    } else {
      setStatus('Build failed:\n' + (res.error || '') + '\n' + (res.log || res.trace || ''), 'bad');
    }
  } catch (e) { setStatus('Build request error: ' + e, 'bad'); }
});

// ---- boot ------------------------------------------------------------------
$('ver').textContent = 'three r' + THREE.REVISION;
$('presetApply').addEventListener('click', () => applyPreset($('presetSel').value));
$('compareSel').addEventListener('change', () => loadCompare($('compareSel').value));
$('compareCopy').addEventListener('click', () => {
  if (!compareVals || !currentCarparam) return;
  for (const k in compareVals) {
    if (!currentCarparam[k] || !fieldUI[k]) continue;
    const v = compareVals[k];
    currentCarparam[k].value = Array.isArray(v) ? v.slice() : v;
    fieldUI[k].setVal(currentCarparam[k].value);
  }
  setStatus('Copied all physics from the compare car — tune, then Build.', '');
});
// Reference (hints/ranges/presets) must load before the first form build.
loadReference().then(loadDonors).catch((e) => setStatus('load error: ' + e, 'bad'));
