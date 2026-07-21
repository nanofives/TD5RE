// TD5 Track Studio -- viewport + editor. Mirrors car_studio.js conventions.
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';

const $ = (id) => document.getElementById(id);
const SURF_COLORS = [0x4a4d55, 0x33414d, 0x6b5a3c, 0x7d7468]; // dry, wet, dirt, gravel

// ---------------------------------------------------------------- scene
const wrap = $('canvasWrap');
const renderer = new THREE.WebGLRenderer({ antialias: true, preserveDrawingBuffer: true });
renderer.setPixelRatio(devicePixelRatio);
wrap.appendChild(renderer.domElement);
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x12141a);
const camera = new THREE.PerspectiveCamera(50, 1, 1, 8_000_000);
camera.position.set(0, 90000, 90000);
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
scene.add(new THREE.HemisphereLight(0xffffff, 0x36404f, 1.15));
const sun = new THREE.DirectionalLight(0xffffff, 1.1); sun.position.set(1, 2, 1); scene.add(sun);
const root = new THREE.Group(); scene.add(root);   // track geometry, centered on `center`
const envRoot = new THREE.Group(); scene.add(envRoot);   // imported environment geometry
const lightsRoot = new THREE.Group(); scene.add(lightsRoot); // curated street-light markers
const gltfLoader = new GLTFLoader();
let grid = null;

function resize() {
  const w = wrap.clientWidth, h = wrap.clientHeight;
  renderer.setSize(w, h);                 // updateStyle=true: pin CSS so HiDPI buffer can't overflow panel
  camera.aspect = w / h; camera.updateProjectionMatrix();
}
addEventListener('resize', resize); resize();

// WASD fly/pan: W/S forward-back, A/D strafe (ground-projected), Q/E down/up,
// Shift = faster. Speed scales with zoom so it feels the same at any distance.
const moveKeys = {};
const _up = new THREE.Vector3(0, 1, 0);
const inField = (el) => el && (el.tagName === 'INPUT' || el.tagName === 'SELECT' || el.tagName === 'TEXTAREA');
function clearMove() { for (const k in moveKeys) moveKeys[k] = false; }
addEventListener('keydown', (e) => {
  if (inField(e.target)) return;
  const k = e.key.toLowerCase();
  if (k.length === 1 && 'wasdqe'.includes(k)) moveKeys[k] = true;
  moveKeys.shift = e.shiftKey; moveKeys.ctrl = e.ctrlKey;
});
addEventListener('keyup', (e) => { const k = e.key.toLowerCase(); if (k.length === 1 && k in moveKeys) moveKeys[k] = false; moveKeys.shift = e.shiftKey; moveKeys.ctrl = e.ctrlKey; });
addEventListener('blur', clearMove);
addEventListener('focusin', (e) => { if (inField(e.target)) clearMove(); });
function applyWASD() {
  const m = moveKeys;
  if (!(m.w || m.a || m.s || m.d || m.q || m.e)) return;
  const fwd = new THREE.Vector3(); camera.getWorldDirection(fwd); fwd.y = 0;
  if (fwd.lengthSq() < 1e-9) fwd.set(0, 0, -1); fwd.normalize();
  const right = new THREE.Vector3().crossVectors(fwd, _up).normalize();
  // Speed scales with the SMALLER of orbit distance and camera height, so
  // flying low over the scenery (target still far away) doesn't rocket past
  // it. Ctrl = precision (0.2x), Shift = fast (3x).
  const ref = Math.min(camera.position.distanceTo(controls.target),
                       Math.abs(camera.position.y) * 2 + 400);
  const speed = ref * 0.018 * (m.shift ? 3 : 1) * (m.ctrl ? 0.2 : 1);
  const d = new THREE.Vector3();
  if (m.w) d.add(fwd); if (m.s) d.sub(fwd);
  if (m.d) d.add(right); if (m.a) d.sub(right);
  if (d.lengthSq() > 0) { d.normalize().multiplyScalar(speed); camera.position.add(d); controls.target.add(d); }
  const vy = (m.e ? 1 : 0) - (m.q ? 1 : 0);
  if (vy) { camera.position.y += vy * speed; controls.target.y += vy * speed; }
}

(function loop() { requestAnimationFrame(loop); applyWASD(); controls.update(); renderer.render(scene, camera); })();

// ---------------------------------------------------------------- state
let spec = blankSpec();
let center = new THREE.Vector3();   // world centroid; view pos = world - center
let selected = -1;
let handles = [];                   // node handle meshes (index-aligned to spec.nodes)
let drawingBranch = null;           // {lanes, nodes:[]} while drawing
let roadTex = null, groundTex = null;   // preview textures
let showLanes = false, editNodes = false, showCpGates = true;
let currentLevel = null, currentAssets = null;   // imported track's source + its assets
let envLoaded = false;                            // environment geometry present -> hide ribbon fill
const raycaster = new THREE.Raycaster();
const plane = new THREE.Plane(new THREE.Vector3(0, 1, 0), 0);  // view-space ground (y=0)

function blankSpec() {
  return {
    name: 'MY TRACK', circuit: true, lane_width: 1500, default_lanes: 4,
    default_surface: 0, nodes: [], branches: [], checkpoints: 'auto:4',
    weather: 2, fog: { enabled: 0, r: 0, g: 0, b: 0 }, traffic_enable: 0,
  };
}
function setStatus(msg, cls) {
  const s = $('status'); s.textContent = msg;
  s.style.color = cls === 'ok' ? 'var(--good)' : cls === 'bad' ? 'var(--bad)' :
    cls === 'warn' ? 'var(--warn)' : 'var(--muted)';
}

// ---------------------------------------------------------------- geometry (mirror td5_trackgen)
const V = (n) => new THREE.Vector3(n.x - center.x, (n.y || 0) - center.y, n.z - center.z);
function tangent(nodes, i, circuit) {
  const n = nodes.length;
  const a = circuit ? nodes[(i - 1 + n) % n] : nodes[Math.max(0, i - 1)];
  const b = circuit ? nodes[(i + 1) % n] : nodes[Math.min(n - 1, i + 1)];
  let dx = b.x - a.x, dz = b.z - a.z; const m = Math.hypot(dx, dz) || 1;
  return [dx / m, dz / m];
}
function nodeWidth(nd) { return nd.width || (nd.lanes || spec.default_lanes) * spec.lane_width; }
function nearestNode(p) {   // index of the main node closest to p (XZ) -- branch fork/rejoin
  let best = 0, bd = Infinity;
  for (let i = 0; i < spec.nodes.length; i++) {
    const dx = spec.nodes[i].x - p.x, dz = spec.nodes[i].z - p.z, d = dx * dx + dz * dz;
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

// Build a ribbon mesh (triangles) + edge/centre lines for a node list.
function ribbon(nodes, circuit, opts) {
  const g = new THREE.Group();
  if (nodes.length < 2) return g;
  if (envLoaded) return g;   // env holds the real road at full res -> hide the schematic ribbon
  const N = nodes.length, segs = circuit ? N : N - 1;
  const L = [], R = [], C = [], lanesAt = [];
  for (let i = 0; i < N; i++) {
    const t = tangent(nodes, i, circuit), lx = t[1], lz = -t[0];
    const half = nodeWidth(nodes[i]) * 0.5;
    const p = V(nodes[i]);
    L.push(new THREE.Vector3(p.x + lx * half, p.y, p.z + lz * half));
    R.push(new THREE.Vector3(p.x - lx * half, p.y, p.z - lz * half));
    C.push(p); lanesAt.push(Math.max(1, nodes[i].lanes || spec.default_lanes));
  }
  const cum = [0];
  for (let i = 1; i < N; i++) cum.push(cum[i - 1] + C[i].distanceTo(C[i - 1]));
  const tile = spec.lane_width || 1500;       // texture tile ~ one lane width
  const pos = [], col = [], uv = [];
  const col3 = (hex) => [((hex >> 16) & 255) / 255, ((hex >> 8) & 255) / 255, (hex & 255) / 255];
  for (let s = 0; s < segs; s++) {
    const a = s, b = (s + 1) % N;
    const va = cum[a] / tile;
    const vb = (b === 0 ? cum[a] + C[0].distanceTo(C[a]) : cum[b]) / tile;
    const c = col3(opts.color != null ? opts.color : SURF_COLORS[(nodes[a].surface || 0) & 3]);
    const quad = [[L[a], 0, va], [R[a], lanesAt[a], va], [R[b], lanesAt[b], vb],
                  [L[a], 0, va], [R[b], lanesAt[b], vb], [L[b], 0, vb]];
    for (const [v, u, vv] of quad) { pos.push(v.x, v.y + 2, v.z); col.push(c[0], c[1], c[2]); uv.push(u, vv); }
  }
  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
  geo.setAttribute('color', new THREE.Float32BufferAttribute(col, 3));
  geo.setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2));
  geo.computeVertexNormals();
  const useTex = roadTex && !opts.ghost;
  const mesh = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({
    map: useTex ? roadTex : null, vertexColors: !useTex,
    side: THREE.DoubleSide, transparent: !!opts.ghost, opacity: opts.ghost ? 0.6 : 1.0,
  }));
  g.add(mesh);
  // rail edge lines
  const railMat = new THREE.LineBasicMaterial({ color: opts.edge != null ? opts.edge : 0xc4c8d0 });
  for (const side of [L, R]) {
    const pts = side.slice(); if (circuit) pts.push(side[0]);
    g.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts.map(v => v.clone().setY(v.y + 4))), railMat));
  }
  // span-boundary rungs -- a cross line at every node so individual spans show
  const rp = [];
  for (let i = 0; i < N; i++) { rp.push(L[i].clone().setY(L[i].y + 3), R[i].clone().setY(R[i].y + 3)); }
  g.add(new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints(rp),
    new THREE.LineBasicMaterial({ color: opts.rung != null ? opts.rung : 0x707a8c, transparent: true, opacity: 0.85 })));
  // lane / sublane dividers (longitudinal) when enabled
  if (showLanes) {
    const lp = [];
    for (let s = 0; s < segs; s++) {
      const a = s, b = (s + 1) % N, k = Math.min(lanesAt[a], lanesAt[b]);
      for (let d = 1; d < k; d++)
        lp.push(L[a].clone().lerp(R[a], d / lanesAt[a]).setY(L[a].y + 5),
                L[b].clone().lerp(R[b], d / lanesAt[b]).setY(L[b].y + 5));
    }
    if (lp.length) g.add(new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints(lp),
      new THREE.LineBasicMaterial({ color: 0xe8e08a, transparent: true, opacity: 0.7 })));
  }
  return g;
}

function rebuild(fit) {
  // clear
  while (root.children.length) root.remove(root.children[0]);
  handles = [];
  if (grid) { scene.remove(grid); grid = null; }
  if (!spec.nodes.length) { updatePanel(); return; }

  // recenter
  const c = new THREE.Vector3(); let miny = 1e18, maxr = 0;
  for (const n of spec.nodes) { c.x += n.x; c.z += n.z; c.y += (n.y || 0); miny = Math.min(miny, n.y || 0); }
  c.multiplyScalar(1 / spec.nodes.length); center.copy(c);
  envRoot.position.set(-center.x, -center.y, -center.z);   // keep env aligned to track
  lightsRoot.position.copy(envRoot.position);              // lights share raw engine coords
  for (const n of spec.nodes) maxr = Math.max(maxr, Math.hypot(n.x - c.x, n.z - c.z));

  // grid sized to the track
  const gsize = Math.max(20000, maxr * 2.4);
  grid = new THREE.GridHelper(gsize, 24, 0x2c3340, 0x1e2430); scene.add(grid);

  // environment/ground texture plane (preview)
  if (groundTex) {
    const pm = new THREE.Mesh(new THREE.PlaneGeometry(gsize, gsize),
      new THREE.MeshStandardMaterial({ map: groundTex, roughness: 1.0 }));
    pm.rotation.x = -Math.PI / 2; pm.position.y = (miny - center.y) - 40; root.add(pm);
  }

  // main ribbon
  root.add(ribbon(spec.nodes, !!spec.circuit, {}));
  // branches (ghost tint) -- connect each to the main road at the nearest fork /
  // rejoin node (mirrors the converter), so the preview has no gap at the ends.
  for (const br of spec.branches) {
    if (!(br.nodes && br.nodes.length >= 2)) continue;
    const w = br.width || br.lanes * spec.lane_width;
    const fork = nearestNode(br.nodes[0]), rejoin = nearestNode(br.nodes[br.nodes.length - 1]);
    const conn = [spec.nodes[fork], ...br.nodes, spec.nodes[rejoin]]
      .map(p => ({ x: p.x, z: p.z, y: p.y || 0, lanes: br.lanes, width: w, surface: br.surface || 0 }));
    root.add(ribbon(conn, false, { color: 0x3a5a7a, edge: 0x6fa8e0, ghost: true }));
  }
  // node handles (only when node editing is enabled)
  if (editNodes) {
    const hgeo = new THREE.SphereGeometry(Math.max(220, maxr * 0.0085), 10, 8);
    spec.nodes.forEach((n, i) => {
      const m = new THREE.Mesh(hgeo, new THREE.MeshBasicMaterial({ color: i === selected ? 0xffd23a : 0x46c46a }));
      m.position.copy(V(n)).setY(V(n).y + 120); m.userData.idx = i; root.add(m); handles.push(m);
    });
  }
  // checkpoints (red cross-line + optional gate)
  const cps = checkpointNodes();
  for (const ci of cps) { markAcross(ci, 0xff4d4d); if (showCpGates) markGate(ci); }
  // start / finish
  if (spec.nodes.length) markAcross(0, 0x46c46a, 1.4);
  if (!spec.circuit && spec.nodes.length > 1) markAcross(spec.nodes.length - 1, 0x3aa0ff, 1.4);
  // branch-draw preview
  if (drawingBranch && drawingBranch.nodes.length) {
    const pts = drawingBranch.nodes.map(p => V(p).setY(200));
    root.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts),
      new THREE.LineBasicMaterial({ color: 0xffa53a })));
    for (const p of drawingBranch.nodes) {
      const m = new THREE.Mesh(hgeo, new THREE.MeshBasicMaterial({ color: 0xffa53a }));
      m.position.copy(V(p)).setY(200); root.add(m);
    }
  }

  if (fit) fitCamera(maxr);
  updatePanel();
}

function markAcross(i, color, scale) {
  const n = spec.nodes[i]; if (!n) return;
  const t = tangent(spec.nodes, i, !!spec.circuit), lx = t[1], lz = -t[0];
  const half = nodeWidth(n) * 0.5 * (scale || 1);
  const p = V(n);
  const a = new THREE.Vector3(p.x + lx * half, p.y + 10, p.z + lz * half);
  const b = new THREE.Vector3(p.x - lx * half, p.y + 10, p.z - lz * half);
  root.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints([a, b]),
    new THREE.LineBasicMaterial({ color, linewidth: 2 })));
}

function markGate(i) {   // checkpoint indicator: two posts + a top bar across the road
  const n = spec.nodes[i]; if (!n) return;
  const t = tangent(spec.nodes, i, !!spec.circuit), lx = t[1], lz = -t[0];
  const half = nodeWidth(n) * 0.5, p = V(n), h = Math.max(1200, half * 0.7);
  const lE = new THREE.Vector3(p.x + lx * half, p.y, p.z + lz * half);
  const rE = new THREE.Vector3(p.x - lx * half, p.y, p.z - lz * half);
  const top = (v) => v.clone().setY(v.y + h);
  root.add(new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints(
    [lE, top(lE), rE, top(rE), top(lE), top(rE)]),
    new THREE.LineBasicMaterial({ color: 0x3ad6ff })));
}

function checkpointNodes() {
  const N = spec.nodes.length; if (!N) return [];
  if (Array.isArray(spec.checkpoints)) return spec.checkpoints.filter(i => i > 0 && i < N);
  const m = String(spec.checkpoints).match(/auto:(\d+)/);
  const cnt = m ? Math.max(1, Math.min(7, +m[1])) : 4;
  const out = []; for (let k = 0; k < cnt; k++) out.push(Math.round((k + 1) * N / (cnt + 1)) % N);
  return out;
}

function fitCamera(maxr) {
  const d = Math.max(20000, maxr * 2.2);
  camera.position.set(0, d * 0.85, d * 0.85);
  controls.target.set(0, 0, 0); controls.update();
}

// ---------------------------------------------------------------- picking / editing
function groundHit(ev) {
  const r = renderer.domElement.getBoundingClientRect();
  const m = new THREE.Vector2(((ev.clientX - r.left) / r.width) * 2 - 1,
                              -((ev.clientY - r.top) / r.height) * 2 + 1);
  raycaster.setFromCamera(m, camera);
  const hit = new THREE.Vector3();
  return raycaster.ray.intersectPlane(plane, hit) ? hit : null;
}
function pickHandle(ev) {
  const r = renderer.domElement.getBoundingClientRect();
  const m = new THREE.Vector2(((ev.clientX - r.left) / r.width) * 2 - 1,
                              -((ev.clientY - r.top) / r.height) * 2 + 1);
  raycaster.setFromCamera(m, camera);
  const hits = raycaster.intersectObjects(handles, false);
  return hits.length ? hits[0].object.userData.idx : -1;
}

let dragIdx = -1, downXY = null;
renderer.domElement.addEventListener('pointerdown', (ev) => {
  if (ev.button !== 0) return;
  downXY = [ev.clientX, ev.clientY];
  if (drawingBranch || !editNodes) return;    // branch points added on click (up); locked = no drag
  const idx = pickHandle(ev);
  if (idx >= 0) { dragIdx = idx; selected = idx; controls.enabled = false; refreshHandleColors(); updatePanel(); }
});
renderer.domElement.addEventListener('pointermove', (ev) => {
  if (dragIdx < 0) return;
  const h = groundHit(ev); if (!h) return;
  spec.nodes[dragIdx].x = Math.round(h.x + center.x);
  spec.nodes[dragIdx].z = Math.round(h.z + center.z);
  rebuild(false);
});
addEventListener('pointerup', (ev) => {
  const moved = downXY && Math.hypot(ev.clientX - downXY[0], ev.clientY - downXY[1]) > 4;
  if (dragIdx >= 0) { dragIdx = -1; controls.enabled = true; downXY = null; return; }
  if (drawingBranch && !moved && ev.target === renderer.domElement) {
    const h = groundHit(ev);
    if (h) { drawingBranch.nodes.push({ x: Math.round(h.x + center.x), z: Math.round(h.z + center.z), y: 0 }); rebuild(false); }
    downXY = null; return;
  }
  if (!moved && editNodes && ev.target === renderer.domElement) {   // click = select
    const idx = pickHandle(ev);
    if (idx >= 0) { selected = idx; refreshHandleColors(); updatePanel(); }
    else if (ev.shiftKey) insertNodeAtCursor(ev);            // shift-click empty = add node near cursor
  }
  downXY = null;
});
function refreshHandleColors() {
  handles.forEach((m, i) => m.material.color.setHex(i === selected ? 0xffd23a : 0x46c46a));
}
function insertNodeAtCursor(ev) {
  const h = groundHit(ev); if (!h) return;
  const nn = { x: Math.round(h.x + center.x), z: Math.round(h.z + center.z), y: 0,
               lanes: spec.default_lanes, surface: 0 };
  const at = (selected >= 0 ? selected + 1 : spec.nodes.length);
  spec.nodes.splice(at, 0, nn); selected = at; rebuild(false);
}

// keyboard: Delete node, Shift+C checkpoint toggle
addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
  if ((e.key === 'Delete' || e.key === 'Backspace') && selected >= 0) { delSelected(); }
  if ((e.key === 'c' || e.key === 'C') && e.shiftKey && selected >= 0) { toggleCheckpoint(selected); }
});

// ---------------------------------------------------------------- panel sync
function updatePanel() {
  $('name').value = spec.name || '';
  $('circuit').value = spec.circuit ? '1' : '0';
  $('laneWidth').value = spec.lane_width;
  $('weather').value = String(spec.weather);
  $('traffic').value = String(spec.traffic_enable || 0);
  $('fog').value = String((spec.fog && spec.fog.enabled) || 0);
  $('fogColorRow').style.display = (spec.fog && spec.fog.enabled) ? '' : 'none';
  $('texturedRoad').checked = spec.textured !== false;
  // node editor
  const n = spec.nodes[selected];
  $('nodeInfo').textContent = n
    ? `Node ${selected + 1} / ${spec.nodes.length}  ·  (${n.x|0}, ${n.z|0})`
    : `${spec.nodes.length} node(s). Click one to select.`;
  if (n) { $('nLanes').value = n.lanes || spec.default_lanes; $('nWidth').value = Math.round(nodeWidth(n)); $('nSurface').value = String(n.surface || 0); }
  // checkpoints
  const auto = !Array.isArray(spec.checkpoints);
  $('cpMode').value = auto ? 'auto' : 'manual';
  $('cpAutoRow').style.display = auto ? '' : 'none';
  $('cpManual').style.display = auto ? 'none' : '';
  if (auto) { const m = String(spec.checkpoints).match(/auto:(\d+)/); $('cpCount').value = m ? +m[1] : 4; }
  // branches
  $('branchList').innerHTML = spec.branches.length
    ? spec.branches.map((b, i) => `<div>branch ${i + 1}: ${b.nodes.length} pts, ${b.lanes} lanes</div>`).join('')
    : 'No branches.';
  // hud
  const tag = spec.circuit ? '<span class="tag-circuit">circuit</span>' : '<span class="tag-p2p">point-to-point</span>';
  $('hudStats').innerHTML = `<b>${spec.name}</b> · ${spec.nodes.length} nodes · ${tag}`;
}

function delSelected() {
  if (selected < 0) return;
  const min = spec.circuit ? 3 : 2;
  if (spec.nodes.length <= min) { setStatus(`Need at least ${min} nodes.`, 'warn'); return; }
  spec.nodes.splice(selected, 1); selected = Math.min(selected, spec.nodes.length - 1); rebuild(false);
}
function toggleCheckpoint(i) {
  if (!Array.isArray(spec.checkpoints)) spec.checkpoints = [];
  const k = spec.checkpoints.indexOf(i);
  if (k >= 0) spec.checkpoints.splice(k, 1); else spec.checkpoints.push(i);
  spec.checkpoints.sort((a, b) => a - b); rebuild(false);
}

// ---------------------------------------------------------------- panel events
$('name').addEventListener('input', e => { spec.name = e.target.value; updatePanel(); });
$('circuit').addEventListener('change', e => { spec.circuit = e.target.value === '1'; rebuild(false); });
$('laneWidth').addEventListener('change', e => { spec.lane_width = +e.target.value || 1500; rebuild(false); });
$('weather').addEventListener('change', e => { spec.weather = +e.target.value; });
$('traffic').addEventListener('change', e => { spec.traffic_enable = +e.target.value; });
$('fog').addEventListener('change', e => { spec.fog = spec.fog || {}; spec.fog.enabled = +e.target.value; updatePanel(); });
$('texturedRoad').addEventListener('change', e => { spec.textured = e.target.checked; });
$('fogColor').addEventListener('change', e => {
  const h = e.target.value; spec.fog = spec.fog || { enabled: 1 };
  spec.fog.r = parseInt(h.slice(1, 3), 16); spec.fog.g = parseInt(h.slice(3, 5), 16); spec.fog.b = parseInt(h.slice(5, 7), 16);
});
$('nLanes').addEventListener('change', e => { if (spec.nodes[selected]) { spec.nodes[selected].lanes = +e.target.value; spec.nodes[selected].width = (+e.target.value) * spec.lane_width; rebuild(false); } });
$('nWidth').addEventListener('change', e => { if (spec.nodes[selected]) { spec.nodes[selected].width = +e.target.value; rebuild(false); } });
$('nSurface').addEventListener('change', e => { if (spec.nodes[selected]) { spec.nodes[selected].surface = +e.target.value; rebuild(false); } });
$('addNode').addEventListener('click', () => {
  if (!spec.nodes.length) return;
  const i = selected >= 0 ? selected : spec.nodes.length - 1;
  const a = spec.nodes[i], b = spec.nodes[(i + 1) % spec.nodes.length];
  spec.nodes.splice(i + 1, 0, { x: Math.round((a.x + b.x) / 2), z: Math.round((a.z + b.z) / 2), y: a.y || 0, lanes: a.lanes || spec.default_lanes, surface: a.surface || 0 });
  selected = i + 1; rebuild(false);
});
$('delNode').addEventListener('click', delSelected);
$('applyAll').addEventListener('click', () => {
  const n = spec.nodes[selected]; if (!n) return;
  for (const m of spec.nodes) { m.lanes = n.lanes; m.width = nodeWidth(n); m.surface = n.surface; }
  rebuild(false); setStatus('Applied lanes/width/surface to all nodes.', 'ok');
});
$('cpMode').addEventListener('change', e => {
  spec.checkpoints = e.target.value === 'auto' ? `auto:${$('cpCount').value || 4}` : []; rebuild(false);
});
$('cpCount').addEventListener('change', e => { if (!Array.isArray(spec.checkpoints)) { spec.checkpoints = `auto:${e.target.value || 4}`; rebuild(false); } });

// branches
$('addBranch').addEventListener('click', () => {
  drawingBranch = { lanes: +$('branchLanes').value || 3, nodes: [] };
  $('branchDrawCtl').style.display = ''; setStatus('Drawing branch: click points across the track, then Finish.', 'warn');
});
$('finishBranch').addEventListener('click', () => {
  if (drawingBranch && drawingBranch.nodes.length >= 2) {
    drawingBranch.lanes = +$('branchLanes').value || 3; spec.branches.push(drawingBranch);
    setStatus(`Added branch (${drawingBranch.nodes.length} pts).`, 'ok');
  } else setStatus('Branch needs at least 2 points.', 'warn');
  drawingBranch = null; $('branchDrawCtl').style.display = 'none'; rebuild(false);
});
$('cancelBranch').addEventListener('click', () => { drawingBranch = null; $('branchDrawCtl').style.display = 'none'; rebuild(false); });
$('delBranch').addEventListener('click', () => { spec.branches.pop(); rebuild(false); });

// track source
async function loadList() {
  try {
    const r = await fetch('/api/tracks'); const d = await r.json();
    const sel = $('importSel'); sel.innerHTML = '';
    for (const lv of d.levels) {
      const o = document.createElement('option'); o.value = lv.level;
      o.textContent = `${lv.level.toString().padStart(3, '0')} · ${lv.name}${lv.custom ? ' (custom)' : ''}`;
      sel.appendChild(o);
    }
  } catch (e) { setStatus('list failed: ' + e, 'bad'); }
}
$('refreshBtn').addEventListener('click', loadList);
$('importBtn').addEventListener('click', async () => {
  const lvl = $('importSel').value; if (!lvl) return;
  setStatus('Importing level ' + lvl + '…');
  try {
    const r = await fetch('/api/import?level=' + lvl); const d = await r.json();
    if (d.error) { setStatus('import error: ' + d.error, 'bad'); return; }
    spec = normalizeIn(d.spec); selected = -1; rebuild(true);
    currentLevel = +lvl; refreshTrackAssets();
    setStatus('Imported. ' + (d.warnings && d.warnings.length ? d.warnings.join(' ') : 'Edit and Build.'),
      d.warnings && d.warnings.length ? 'warn' : 'ok');
  } catch (e) { setStatus('import failed: ' + e, 'bad'); }
});
document.querySelectorAll('[data-sample]').forEach(btn => btn.addEventListener('click', async () => {
  try {
    const r = await fetch('/api/sample?kind=' + btn.dataset.sample); const d = await r.json();
    spec = normalizeIn(d.spec); selected = -1; rebuild(true);
    currentLevel = null; refreshTrackAssets(); setStatus('Loaded sample. Edit and Build.', 'ok');
  } catch (e) { setStatus('sample failed: ' + e, 'bad'); }
}));
$('blankBtn').addEventListener('click', () => {
  spec = blankSpec();
  // seed a small starter square so there is something to edit
  const r = 20000;
  spec.nodes = [[r, 0], [0, r], [-r, 0], [0, -r]].map(([x, z]) => ({ x, z, y: 0, lanes: 4, width: 6000, surface: 0 }));
  selected = -1; rebuild(true); currentLevel = null; refreshTrackAssets();
  setStatus('Blank circuit. Drag nodes / shift-click to add.', 'ok');
});

// load an external centerline file (CSV/JSON) -> auto-detect lanes + branches
$('loadFileBtn').addEventListener('click', () => $('trackFile').click());
$('trackFile').addEventListener('change', (e) => {
  const f = e.target.files[0]; if (!f) return;
  const r = new FileReader();
  r.onload = async () => {
    setStatus('Parsing ' + f.name + '…');
    try {
      const resp = await fetch('/api/loadfile', { method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ filename: f.name, text: r.result }) });
      const d = await resp.json();
      if (!d.ok) { setStatus('load error: ' + (d.error || JSON.stringify(d)), 'bad'); return; }
      spec = normalizeIn(d.spec); selected = -1;
      envLoaded = false; while (envRoot.children.length) envRoot.remove(envRoot.children[0]);
      rebuild(true); currentLevel = null; refreshTrackAssets();
      setStatus('Loaded ' + f.name + '. ' + (d.warnings || []).join(' · '), 'ok');
    } catch (err) { setStatus('load failed: ' + err, 'bad'); }
  };
  r.readAsText(f);
  e.target.value = '';
});
$('detectLanes').addEventListener('click', () => {
  const lw = spec.lane_width || 1500; let changed = 0;
  for (const n of spec.nodes) if (n.width) { const l = Math.max(1, Math.min(12, Math.round(n.width / lw))); if (l !== n.lanes) changed++; n.lanes = l; }
  for (const br of spec.branches || []) { const ws = (br.nodes || []).map(p => p.width).filter(Boolean); if (ws.length) br.lanes = Math.max(1, Math.min(12, Math.round(Math.max(...ws) / lw))); }
  rebuild(false); setStatus(`Re-detected lanes from width (${changed} node(s) changed).`, 'ok');
});

function normalizeIn(s) {
  s = s || blankSpec();
  s.nodes = (s.nodes || []).map(n => ({ x: +n.x, z: +n.z, y: +(n.y || 0),
    lanes: +(n.lanes || s.default_lanes || 4), width: n.width ? +n.width : undefined, surface: +(n.surface || 0) }));
  s.branches = s.branches || [];
  s.fog = s.fog || { enabled: 0, r: 0, g: 0, b: 0 };
  if (s.lane_width == null) s.lane_width = 1500;
  if (s.default_lanes == null) s.default_lanes = 4;
  if (s.textured === undefined) s.textured = true;
  return s;
}

// build
$('buildBtn').addEventListener('click', async () => {
  if (spec.nodes.length < (spec.circuit ? 3 : 2)) { setStatus('Add more nodes first.', 'warn'); return; }
  setStatus('Building…');
  try {
    const payload = { spec, slot: spec._slot, level: spec._level };
    const r = await fetch('/api/build', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
    const d = await r.json();
    if (!d.ok) { setStatus('build error: ' + (d.error || JSON.stringify(d)), 'bad'); return; }
    spec._slot = d.slot; spec._level = d.level;
    setStatus(`Built "${spec.name}" → slot ${d.slot}, level ${d.level} (${d.spans} spans).\n${d.drive}`, 'ok');
    loadList();
  } catch (e) { setStatus('build failed: ' + e, 'bad'); }
});

// ---------------------------------------------------------------- view / textures
function fileToTexture(file, repeat, cb) {
  const url = URL.createObjectURL(file);
  new THREE.TextureLoader().load(url, (tex) => {
    tex.colorSpace = THREE.SRGBColorSpace; tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
    if (repeat) tex.repeat.set(repeat, repeat);
    URL.revokeObjectURL(url); cb(tex);
  }, undefined, () => { URL.revokeObjectURL(url); setStatus('texture load failed', 'bad'); });
}
$('roadTexBtn').onclick = () => $('roadTexFile').click();
$('roadTexFile').onchange = (e) => { const f = e.target.files[0]; if (f) fileToTexture(f, 0, t => { roadTex = t; rebuild(false); setStatus('Road texture applied (preview).', 'ok'); }); };
$('groundTexBtn').onclick = () => $('groundTexFile').click();
$('groundTexFile').onchange = (e) => { const f = e.target.files[0]; if (f) fileToTexture(f, 24, t => { groundTex = t; rebuild(false); setStatus('Ground texture applied (preview).', 'ok'); }); };
$('skyTexBtn').onclick = () => $('skyTexFile').click();
$('skyTexFile').onchange = (e) => {
  const f = e.target.files[0]; if (!f) return;
  const url = URL.createObjectURL(f);
  new THREE.TextureLoader().load(url, (t) => {
    t.mapping = THREE.EquirectangularReflectionMapping; t.colorSpace = THREE.SRGBColorSpace;
    scene.background = t; URL.revokeObjectURL(url); setStatus('Skybox applied.', 'ok');
  }, undefined, () => { URL.revokeObjectURL(url); setStatus('skybox load failed', 'bad'); });
};
$('clearTexBtn').onclick = () => {
  roadTex = groundTex = null; scene.background = new THREE.Color(0x12141a);
  rebuild(false); setStatus('Cleared textures + skybox.', 'ok');
};
$('showLanes').onchange = (e) => { showLanes = e.target.checked; rebuild(false); };
$('editNodes').onchange = (e) => { editNodes = e.target.checked; if (!editNodes) selected = -1; rebuild(false); };

// ---------------------------------------------------------------- from selected track
function trackTexture(level, name, flipY) {
  return new Promise((res, rej) => {
    new THREE.TextureLoader().load(`/api/asset?level=${level}&name=${encodeURIComponent(name)}`,
      (t) => { t.colorSpace = THREE.SRGBColorSpace; t.wrapS = t.wrapT = THREE.RepeatWrapping; if (flipY === false) t.flipY = false; res(t); },
      undefined, rej);
  });
}
async function refreshTrackAssets() {
  currentAssets = null;
  const env = $('loadEnvBtn'), sky = $('skyTrackBtn');
  env.disabled = sky.disabled = (currentLevel == null);
  if (currentLevel == null) return;
  try {
    currentAssets = await (await fetch('/api/assets?level=' + currentLevel)).json();
    env.disabled = !currentAssets.has_models;
    sky.disabled = !(currentAssets.skybox && currentAssets.skybox.length);
  } catch (e) { /* ignore */ }
}
$('skyTrackBtn').onclick = async () => {
  if (!currentAssets || !currentAssets.skybox.length) { setStatus('no skybox in this track', 'warn'); return; }
  try { const t = await trackTexture(currentLevel, currentAssets.skybox[0]); t.mapping = THREE.EquirectangularReflectionMapping; scene.background = t; setStatus('Skybox from track applied.', 'ok'); }
  catch { setStatus('skybox load failed', 'bad'); }
};
$('cpGates').onchange = (e) => { showCpGates = e.target.checked; rebuild(false); };
$('clearEnvBtn').onclick = () => { while (envRoot.children.length) envRoot.remove(envRoot.children[0]); envLoaded = false; rebuild(false); setStatus('Cleared environment.', 'ok'); };
function envMaterial(tex, type) {   // unlit; transparency per page type (0 opaque,1 keyed,2 semi,3 additive)
  const base = { map: tex || null, color: tex ? 0xffffff : 0x8b9099, side: THREE.DoubleSide };
  if (!tex) return new THREE.MeshBasicMaterial(base);
  if (type === 1) return new THREE.MeshBasicMaterial({ ...base, transparent: true, alphaTest: 0.5 });
  if (type === 2) return new THREE.MeshBasicMaterial({ ...base, transparent: true });
  if (type === 3) return new THREE.MeshBasicMaterial({ ...base, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending });
  return new THREE.MeshBasicMaterial(base);
}
$('loadEnvBtn').onclick = async () => {
  if (currentLevel == null) { setStatus('Import a track first.', 'warn'); return; }
  setStatus('Loading environment (decoding models.bin, may take a moment)…');
  try {
    const buf = await (await fetch('/api/model?level=' + currentLevel)).arrayBuffer();
    gltfLoader.parse(buf, '', async (gltf) => {
      // each node carries its real per-command texture page in extras; load just
      // those pages (textures.src/pages/page_NNN.png) in parallel, then assign.
      const pages = new Set();
      gltf.scene.traverse((o) => { if (o.isMesh && o.userData && o.userData.page != null) pages.add(o.userData.page); });
      setStatus(`Loading environment textures (${pages.size} pages)…`);
      const types = (currentAssets && currentAssets.page_types) || {};
      const texMap = {};
      await Promise.all([...pages].map(async (p) => {
        try { texMap[p] = await trackTexture(currentLevel, `page_${String(p).padStart(3, '0')}.png`, false); }
        catch { texMap[p] = null; }
      }));
      gltf.scene.traverse((o) => {
        if (o.isMesh) {
          const page = o.userData ? o.userData.page : -1;
          o.material = envMaterial(texMap[page], types[page] | 0);
        }
      });
      while (envRoot.children.length) envRoot.remove(envRoot.children[0]);
      envRoot.add(gltf.scene); envRoot.position.set(-center.x, -center.y, -center.z);
      envLoaded = true; rebuild(false);
      setStatus(`Environment loaded (${pages.size} texture pages).`, 'ok');
    }, (err) => setStatus('GLB parse error: ' + err, 'bad'));
  } catch (e) { setStatus('environment load failed: ' + e, 'bad'); }
};

// ---------------------------------------------------------------- lights editor
// Curated street lights (td5mod/src/td5re/lights/levelNNN_lights.json — the
// td5_lightsrc.c runtime schema). Positions are RAW engine world coordinates
// (24.8/256 floats, Y-DOWN: smaller y = higher), the same space the env GLB
// and strip nodes already use, so markers land exactly on the scenery.
let lightsData = null;        // { defaults, lights[], emissive_pages[] }
let lightMarkers = [];        // sphere meshes, index-aligned to lightsData.lights
let selLight = -1, editLights = false, dragLightIdx = -1;
let additivePages = [];       // suppression candidates for the current level

function lightsDefaults() {
  return {
    range: +$('ldRange').value || 2400,
    intensity: +$('ldIntensity').value || 1.0,
    color: hexToRgb($('ldColor').value),
  };
}
function hexToRgb(h) {
  return [parseInt(h.slice(1, 3), 16) / 255, parseInt(h.slice(3, 5), 16) / 255,
          parseInt(h.slice(5, 7), 16) / 255].map(v => Math.round(v * 100) / 100);
}
function rgbToHex(c) {
  const b = (v) => Math.max(0, Math.min(255, Math.round((v == null ? 1 : v) * 255)))
    .toString(16).padStart(2, '0');
  return '#' + b(c && c[0]) + b(c && c[1]) + b(c && c[2]);
}
function blankLights() {
  return { defaults: lightsDefaults(), lights: [], emissive_pages: [] };
}

function rebuildLights() {
  while (lightsRoot.children.length) lightsRoot.remove(lightsRoot.children[0]);
  lightMarkers = [];
  if (!lightsData) { updateLightPanel(); return; }
  const rad = 260;
  lightsData.lights.forEach((L, i) => {
    const col = L.color ? rgbToHex(L.color) : rgbToHex(lightsData.defaults.color);
    const m = new THREE.Mesh(new THREE.SphereGeometry(rad, 12, 10),
      new THREE.MeshBasicMaterial({ color: i === selLight ? 0xffd23a : new THREE.Color(col).getHex() }));
    m.position.set(L.x, L.y, L.z); m.userData.lightIdx = i;
    lightsRoot.add(m); lightMarkers.push(m);
    // range ring on the (engine) ground under the head — draw at the light's own y
    const rng = L.range || lightsData.defaults.range;
    const ring = new THREE.Mesh(new THREE.RingGeometry(rng * 0.97, rng, 40),
      new THREE.MeshBasicMaterial({ color: 0xe2b13a, side: THREE.DoubleSide,
        transparent: true, opacity: i === selLight ? 0.5 : 0.18 }));
    ring.rotation.x = -Math.PI / 2; ring.position.set(L.x, L.y, L.z);
    lightsRoot.add(ring);
    // drop line: head -> +range*0.25 toward engine-ground (bigger y) as a cue
    const pts = [new THREE.Vector3(L.x, L.y, L.z), new THREE.Vector3(L.x, L.y + 550, L.z)];
    lightsRoot.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts),
      new THREE.LineBasicMaterial({ color: 0x8b93a3 })));
  });
  lightsRoot.position.set(-center.x, -center.y, -center.z);
  updateLightPanel();
}

function updateLightPanel() {
  const n = lightsData ? lightsData.lights.length : 0;
  const L = (lightsData && selLight >= 0) ? lightsData.lights[selLight] : null;
  $('lightInfo').textContent = lightsData
    ? (L ? `Light ${selLight + 1} / ${n}` : `${n} light(s). Click a marker to select.`)
    : 'No lights loaded.';
  $('lX').value = L ? Math.round(L.x) : '';
  $('lY').value = L ? Math.round(L.y) : '';
  $('lZ').value = L ? Math.round(L.z) : '';
  $('lRange').value = L && L.range != null ? L.range : '';
  $('lIntensity').value = L && L.intensity != null ? L.intensity : '';
  $('lColor').value = L && L.color ? rgbToHex(L.color) : rgbToHex(lightsData ? lightsData.defaults.color : null);
  renderGlowPages();
}

function renderGlowPages() {
  const el = $('glowPages');
  if (currentLevel == null) { el.textContent = 'Import a level to list its additive pages.'; return; }
  // classified candidates first (additive pages + pages already in the file),
  // then — once the env is loaded — EVERY page the scenery draws, so any
  // texture (lamp head, neon sign) can be picked as a light-fixture source.
  const listed = (lightsData ? (lightsData.emissive_pages || []) : []).map(p => p.page);
  const cand = new Set([...additivePages, ...listed]);
  const all = envLoaded ? [...envPageSet()].filter(p => !cand.has(p)).sort((a, b) => a - b) : [];
  const pages = [...cand].sort((a, b) => a - b);
  if (!pages.length && !all.length) { el.textContent = 'No additive (type-3) pages in this level.'; return; }
  const sup = new Set((lightsData ? lightsData.emissive_pages : [])
    .filter(p => p.suppress).map(p => p.page));
  const cell = (p, dim) => `
    <span style="display:inline-block;text-align:center;margin:3px">
      <img src="/api/asset?level=${currentLevel}&name=page_${String(p).padStart(3, '0')}.png"
           width="48" height="48" data-pick="${p}" title="click: select as fixture source"
           style="image-rendering:pixelated;cursor:pointer;background:#000;${dim ? 'opacity:.75;' : ''}
                  border:2px solid ${selectedPages.has(p) ? 'var(--accent)' : 'var(--edge)'}"><br>
      <span style="font-size:10px">${p}</span>
      <input type="checkbox" data-page="${p}" ${sup.has(p) ? 'checked' : ''} title="suppress in-game">
    </span>`;
  el.innerHTML = pages.map(p => cell(p, false)).join('') +
    (all.length ? `<div class="hint" style="margin-top:4px">All scenery pages (click one to use
       it as the fixture source — e.g. the lamp-head glow):</div>
       <div style="max-height:180px;overflow-y:auto">${all.map(p => cell(p, true)).join('')}</div>` : '');
  el.querySelectorAll('input[type=checkbox]').forEach(cb => cb.addEventListener('change', () => {
    if (!lightsData) lightsData = blankLights();
    const pg = +cb.dataset.page;
    lightsData.emissive_pages = (lightsData.emissive_pages || []).filter(p => p.page !== pg);
    lightsData.emissive_pages.push({ page: pg, suppress: cb.checked });
  }));
  el.querySelectorAll('img[data-pick]').forEach(img => img.addEventListener('click', () => {
    const pg = +img.dataset.pick;
    if (selectedPages.has(pg)) selectedPages.delete(pg); else selectedPages.add(pg);
    renderGlowPages();
    if (showGlowFixtures) rebuildGlowMarkers();
  }));
}

// ---- glow fixtures: cluster the emissive pages' geometry into per-fixture
// centroids so lights can be placed ON the painted glow, not just captured
// pole positions. Works on the loaded env GLB (one merged mesh per page).
const glowRoot = new THREE.Group(); scene.add(glowRoot);
let glowClusters = [], showGlowFixtures = false;

function emissivePageSet() {
  const listed = (lightsData ? (lightsData.emissive_pages || []) : []).map(p => p.page);
  return new Set([...additivePages, ...listed]);
}
// pages the user clicked in the palette — fixture clustering targets THESE
// when non-empty (pick the lamp-head texture, generate lights from its
// fixtures), falling back to the emissive set otherwise.
const selectedPages = new Set();
function envPageSet() {   // every page the loaded env actually draws
  const s = new Set();
  envRoot.traverse((o) => { if (o.isMesh && o.userData && o.userData.page != null) s.add(o.userData.page); });
  return s;
}
function computeGlowClusters() {
  glowClusters = [];
  if (!envLoaded) return;
  const pages = selectedPages.size ? selectedPages : emissivePageSet();
  const cents = [];   // triangle centroids of all emissive-page meshes (raw engine coords)
  envRoot.traverse((o) => {
    if (!o.isMesh || !o.userData || !pages.has(o.userData.page)) return;
    const pos = o.geometry.getAttribute('position');
    for (let t = 0; t + 2 < pos.count; t += 3) {
      cents.push([
        (pos.getX(t) + pos.getX(t + 1) + pos.getX(t + 2)) / 3,
        (pos.getY(t) + pos.getY(t + 1) + pos.getY(t + 2)) / 3,
        (pos.getZ(t) + pos.getZ(t + 1) + pos.getZ(t + 2)) / 3]);
    }
  });
  // greedy clustering: merge centroids within 900 units (a fixture's quadrant
  // quads sit a few hundred apart; posts are >=1400 apart)
  const R2 = 900 * 900;
  for (const c of cents) {
    let best = null;
    for (const cl of glowClusters) {
      const dx = cl.x / cl.n - c[0], dy = cl.y / cl.n - c[1], dz = cl.z / cl.n - c[2];
      if (dx * dx + dy * dy + dz * dz < R2) { best = cl; break; }
    }
    if (best) { best.x += c[0]; best.y += c[1]; best.z += c[2]; best.n++; }
    else glowClusters.push({ x: c[0], y: c[1], z: c[2], n: 1 });
  }
  // finalize to means + tag "near the road" (backdrop glow strings sit 25k+
  // out — generating lights there would litter the skyline)
  glowClusters = glowClusters.map(cl => {
    const x = cl.x / cl.n, y = cl.y / cl.n, z = cl.z / cl.n;
    let near = false;
    for (const nd of spec.nodes) {
      const dx = nd.x - x, dz = nd.z - z, dy = (nd.y || 0) - y;
      // near = close in XZ AND roughly road level (Moscow's quay glows sit
      // ~3000 BELOW the embankment — a light there illuminates nothing)
      if (dx * dx + dz * dz < 8000 * 8000 && Math.abs(dy) < 2500) { near = true; break; }
    }
    return { x, y, z, n: cl.n, near };
  });
}
function rebuildGlowMarkers() {
  while (glowRoot.children.length) glowRoot.remove(glowRoot.children[0]);
  if (!showGlowFixtures) return;
  computeGlowClusters();
  const geo = new THREE.OctahedronGeometry(320);
  const matNear = new THREE.MeshBasicMaterial({ color: 0x3ad6ff, wireframe: true });
  const matFar = new THREE.MeshBasicMaterial({ color: 0x555f70, wireframe: true });
  for (const cl of glowClusters) {
    const m = new THREE.Mesh(geo, cl.near ? matNear : matFar);
    m.position.set(cl.x, cl.y, cl.z); glowRoot.add(m);
  }
  glowRoot.position.set(-center.x, -center.y, -center.z);
  const near = glowClusters.filter(c => c.near).length;
  setStatus(`Glow fixtures: ${glowClusters.length} cluster(s), ${near} near the road (cyan). ` +
    `Grey = backdrop/off-road (skipped by "+ lights at fixtures").`, 'ok');
}
$('glowShowBtn').addEventListener('click', () => {
  if (!envLoaded) { setStatus('Load environment first (7 · View / textures).', 'warn'); return; }
  showGlowFixtures = !showGlowFixtures;
  $('glowShowBtn').textContent = showGlowFixtures ? 'hide glow fixtures' : 'show glow fixtures';
  rebuildGlowMarkers();
});
$('glowGenBtn').addEventListener('click', () => {
  if (!envLoaded) { setStatus('Load environment first (7 · View / textures).', 'warn'); return; }
  computeGlowClusters();
  if (!lightsData) lightsData = blankLights();
  let added = 0;
  for (const cl of glowClusters) {
    if (!cl.near) continue;
    // dedupe against existing lights (same 650 radius the runtime capture uses)
    let dup = false;
    for (const L of lightsData.lights) {
      const dx = L.x - cl.x, dy = L.y - cl.y, dz = L.z - cl.z;
      if (dx * dx + dy * dy + dz * dz < 650 * 650) { dup = true; break; }
    }
    if (!dup) { lightsData.lights.push({ x: Math.round(cl.x), y: Math.round(cl.y), z: Math.round(cl.z) }); added++; }
  }
  selLight = -1; rebuildLights();
  setStatus(`Added ${added} light(s) at glow fixtures (deduped vs existing).`, 'ok');
});

// pick / drag / add
function pickLight(ev) {
  const r = renderer.domElement.getBoundingClientRect();
  const m = new THREE.Vector2(((ev.clientX - r.left) / r.width) * 2 - 1,
                              -((ev.clientY - r.top) / r.height) * 2 + 1);
  raycaster.setFromCamera(m, camera);
  const hits = raycaster.intersectObjects(lightMarkers, false);
  return hits.length ? hits[0].object.userData.lightIdx : -1;
}
let lightDownXY = null;   // own copy — the node editor's pointerup nulls downXY before ours runs
renderer.domElement.addEventListener('pointerdown', (ev) => {
  if (ev.button !== 0) return;
  lightDownXY = [ev.clientX, ev.clientY];
  if (!editLights || !lightsData) return;
  const idx = pickLight(ev);
  if (idx >= 0) { dragLightIdx = idx; selLight = idx; controls.enabled = false; rebuildLights(); }
});
renderer.domElement.addEventListener('pointermove', (ev) => {
  if (dragLightIdx < 0) return;
  const h = groundHit(ev); if (!h) return;
  const L = lightsData.lights[dragLightIdx];
  L.x = Math.round(h.x + center.x); L.z = Math.round(h.z + center.z);
  rebuildLights();
});
addEventListener('pointerup', (ev) => {
  const wasDown = lightDownXY; lightDownXY = null;
  if (dragLightIdx >= 0) { dragLightIdx = -1; controls.enabled = true; return; }
  if (!editLights || !lightsData || ev.target !== renderer.domElement) return;
  const moved = wasDown && Math.hypot(ev.clientX - wasDown[0], ev.clientY - wasDown[1]) > 4;
  if (moved) return;
  const idx = pickLight(ev);
  if (idx >= 0) { selLight = idx; rebuildLights(); }
  else if (ev.shiftKey) {
    const h = groundHit(ev); if (!h) return;
    addLightAt(Math.round(h.x + center.x), Math.round(h.z + center.z));
  }
});
addEventListener('keydown', (e) => {
  if (inField(e.target)) return;
  if ((e.key === 'Delete' || e.key === 'Backspace') && editLights && selLight >= 0 && lightsData) {
    lightsData.lights.splice(selLight, 1);
    selLight = Math.min(selLight, lightsData.lights.length - 1);
    rebuildLights();
  }
});
function addLightAt(x, z) {
  if (!lightsData) lightsData = blankLights();
  // head height: ~550 above (engine Y-DOWN => minus) the nearest road node
  let y = -550;
  if (spec.nodes.length) { const ni = nearestNode({ x, z }); y = (spec.nodes[ni].y || 0) - 550; }
  lightsData.lights.push({ x, y, z });
  selLight = lightsData.lights.length - 1;
  rebuildLights();
}

// panel events
const selLightObj = () => (lightsData && selLight >= 0) ? lightsData.lights[selLight] : null;
for (const [id, key] of [['lX', 'x'], ['lY', 'y'], ['lZ', 'z']])
  $(id).addEventListener('change', (e) => { const L = selLightObj(); if (L) { L[key] = +e.target.value; rebuildLights(); } });
$('lRange').addEventListener('change', (e) => {
  const L = selLightObj(); if (!L) return;
  if (e.target.value === '') delete L.range; else L.range = +e.target.value;
  rebuildLights();
});
$('lIntensity').addEventListener('change', (e) => {
  const L = selLightObj(); if (!L) return;
  if (e.target.value === '') delete L.intensity; else L.intensity = +e.target.value;
});
$('lColor').addEventListener('change', (e) => { const L = selLightObj(); if (L) { L.color = hexToRgb(e.target.value); rebuildLights(); } });
$('lColorClear').addEventListener('click', () => { const L = selLightObj(); if (L) { delete L.color; rebuildLights(); } });
for (const id of ['ldRange', 'ldIntensity', 'ldColor'])
  $(id).addEventListener('change', () => { if (lightsData) { lightsData.defaults = lightsDefaults(); rebuildLights(); } });
$('editLights').addEventListener('change', (e) => { editLights = e.target.checked; if (!editLights) { selLight = -1; rebuildLights(); } });
$('addLight').addEventListener('click', () => {
  const t = controls.target;   // drop at the orbit target
  addLightAt(Math.round(t.x + center.x), Math.round(t.z + center.z));
});
$('dupLight').addEventListener('click', () => {
  const L = selLightObj(); if (!L) return;
  lightsData.lights.push({ ...L, x: L.x + 1500 });
  selLight = lightsData.lights.length - 1; rebuildLights();
});
$('delLight').addEventListener('click', () => {
  if (!lightsData || selLight < 0) return;
  lightsData.lights.splice(selLight, 1);
  selLight = Math.min(selLight, lightsData.lights.length - 1); rebuildLights();
});
$('clearLights').addEventListener('click', () => {
  if (!lightsData || !lightsData.lights.length) return;
  const n = lightsData.lights.length;
  lightsData.lights = []; selLight = -1; rebuildLights();
  setStatus(`Cleared ${n} light(s) (not saved yet — Save writes the empty/regenerated set).`, 'warn');
});

function applyLoadedLights(doc, label) {
  lightsData = {
    defaults: (doc && doc.defaults) || lightsDefaults(),
    lights: (doc && doc.lights) || [],
    emissive_pages: (doc && doc.emissive_pages) || [],
  };
  $('ldRange').value = lightsData.defaults.range != null ? lightsData.defaults.range : 2400;
  $('ldIntensity').value = lightsData.defaults.intensity != null ? lightsData.defaults.intensity : 1.0;
  $('ldColor').value = rgbToHex(lightsData.defaults.color);
  selLight = -1; rebuildLights();
  setStatus(`${label}: ${lightsData.lights.length} light(s).`, 'ok');
}
async function fetchLights() {
  if (currentLevel == null) { setStatus('Import a level first (Lights are per level).', 'warn'); return null; }
  const d = await (await fetch('/api/lights?level=' + currentLevel)).json();
  additivePages = d.additive_pages || [];
  return d;
}
$('lightsLoadBtn').addEventListener('click', async () => {
  try {
    const d = await fetchLights(); if (!d) return;
    if (d.lights) applyLoadedLights(d.lights, 'Loaded ' + d.path);
    else { applyLoadedLights(null, 'No curated file yet — starting empty'); }
  } catch (e) { setStatus('lights load failed: ' + e, 'bad'); }
});
$('lightsSeedBtn').addEventListener('click', async () => {
  try {
    const d = await fetchLights(); if (!d) return;
    if (!d.captured) { setStatus('No captured seed. Run td5re with TD5RE_LAMP_FREEZE=1 first.', 'warn'); return; }
    applyLoadedLights(d.captured, 'Imported captured seed (review, tune, Save)');
  } catch (e) { setStatus('seed load failed: ' + e, 'bad'); }
});
$('lightsSaveBtn').addEventListener('click', async () => {
  if (currentLevel == null || !lightsData) { setStatus('Import a level and load/add lights first.', 'warn'); return; }
  lightsData.defaults = lightsDefaults();
  try {
    const r = await fetch('/api/lights', { method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ level: currentLevel, data: lightsData }) });
    const d = await r.json();
    if (!d.ok) { setStatus('save error: ' + (d.error || JSON.stringify(d)), 'bad'); return; }
    setStatus(`Saved ${d.path} (${d.lights} light(s), ${d.suppressed} suppressed page(s)).\nIn-game: the level now uses curated lights.`, 'ok');
  } catch (e) { setStatus('save failed: ' + e, 'bad'); }
});

// ---------------------------------------------------------------- boot
loadList();
$('blankBtn').click();
