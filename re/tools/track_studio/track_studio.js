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
  moveKeys.shift = e.shiftKey;
});
addEventListener('keyup', (e) => { const k = e.key.toLowerCase(); if (k.length === 1 && k in moveKeys) moveKeys[k] = false; moveKeys.shift = e.shiftKey; });
addEventListener('blur', clearMove);
addEventListener('focusin', (e) => { if (inField(e.target)) clearMove(); });
function applyWASD() {
  const m = moveKeys;
  if (!(m.w || m.a || m.s || m.d || m.q || m.e)) return;
  const fwd = new THREE.Vector3(); camera.getWorldDirection(fwd); fwd.y = 0;
  if (fwd.lengthSq() < 1e-9) fwd.set(0, 0, -1); fwd.normalize();
  const right = new THREE.Vector3().crossVectors(fwd, _up).normalize();
  const speed = camera.position.distanceTo(controls.target) * 0.018 * (m.shift ? 3 : 1);
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
let showLanes = false, editNodes = true, showCpGates = true;
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
  // when the real environment geometry is loaded, hide the filled main ribbon so
  // it doesn't z-fight the env road; keep rails/rungs/handles as editing overlay.
  if (!(envLoaded && !opts.ghost)) {
    const useTex = roadTex && !opts.ghost;
    const mesh = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({
      map: useTex ? roadTex : null, vertexColors: !useTex,
      side: THREE.DoubleSide, transparent: !!opts.ghost, opacity: opts.ghost ? 0.6 : 1.0,
    }));
    g.add(mesh);
  }
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

function normalizeIn(s) {
  s = s || blankSpec();
  s.nodes = (s.nodes || []).map(n => ({ x: +n.x, z: +n.z, y: +(n.y || 0),
    lanes: +(n.lanes || s.default_lanes || 4), width: n.width ? +n.width : undefined, surface: +(n.surface || 0) }));
  s.branches = s.branches || [];
  s.fog = s.fog || { enabled: 0, r: 0, g: 0, b: 0 };
  if (s.lane_width == null) s.lane_width = 1500;
  if (s.default_lanes == null) s.default_lanes = 4;
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
  $('roadTexSel').value = ''; $('groundTexSel').value = '';
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
  for (const sel of [$('roadTexSel'), $('groundTexSel')]) sel.innerHTML = '<option value="">— none —</option>';
  currentAssets = null;
  const env = $('loadEnvBtn'), sky = $('skyTrackBtn');
  env.disabled = sky.disabled = (currentLevel == null);
  if (currentLevel == null) return;
  try {
    currentAssets = await (await fetch('/api/assets?level=' + currentLevel)).json();
    for (const sel of [$('roadTexSel'), $('groundTexSel')])
      for (const tx of currentAssets.textures) { const o = document.createElement('option'); o.value = tx; o.textContent = tx; sel.appendChild(o); }
    env.disabled = !currentAssets.has_models;
    sky.disabled = !(currentAssets.skybox && currentAssets.skybox.length);
  } catch (e) { /* ignore */ }
}
$('roadTexSel').onchange = async (e) => {
  if (!e.target.value) { roadTex = null; rebuild(false); setStatus('Road texture cleared.', 'ok'); return; }
  if (currentLevel == null) return;
  try { roadTex = await trackTexture(currentLevel, e.target.value); rebuild(false); setStatus('Road texture from track applied.', 'ok'); }
  catch { setStatus('texture load failed', 'bad'); }
};
$('groundTexSel').onchange = async (e) => {
  if (!e.target.value) { groundTex = null; rebuild(false); setStatus('Ground texture cleared.', 'ok'); return; }
  if (currentLevel == null) return;
  try { const t = await trackTexture(currentLevel, e.target.value); t.repeat.set(24, 24); groundTex = t; rebuild(false); setStatus('Ground texture from track applied.', 'ok'); }
  catch { setStatus('texture load failed', 'bad'); }
};
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

// ---------------------------------------------------------------- boot
loadList();
$('blankBtn').click();
