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
// ONE mouse scheme for the whole app: LEFT is reserved for selection (node
// handles, box select, the LIBRARY gap picker), so orbit lives on the RIGHT
// button and pan moves to the middle -- the wheel still zooms. Previously this
// mapping existed only inside pick mode, so the buttons changed meaning
// depending on a checkbox, which is exactly the kind of thing you relearn every
// time you come back to the tool. OrbitControls suppresses the context menu on
// its own element, so right-drag does not pop the browser menu.
const ORBIT_SCHEME = { LEFT: null, MIDDLE: THREE.MOUSE.PAN, RIGHT: THREE.MOUSE.ROTATE };
controls.mouseButtons = ORBIT_SCHEME;
scene.add(new THREE.HemisphereLight(0xffffff, 0x36404f, 1.15));
const sun = new THREE.DirectionalLight(0xffffff, 1.1); sun.position.set(1, 2, 1); scene.add(sun);
const root = new THREE.Group(); scene.add(root);   // track geometry, centered on `center`
// The ONE loaded-level geometry group. Both the TRACKS tab ("load
// environment") and the LIBRARY tab ("load geometry") fill this, from the same
// endpoint, at the same offset -- they used to be two loaders over two
// endpoints and disagreed about billboards, centring and pickability.
// `selRoot` further down is an alias for it, not a second group.
const envRoot = new THREE.Group(); scene.add(envRoot);
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

(function loop() { requestAnimationFrame(loop); applyWASD(); controls.update(); tickExtras(); renderer.render(scene, camera); })();

// ---------------------------------------------------------------- state
let spec = blankSpec();
let center = new THREE.Vector3();   // world centroid; view pos = world - center
let selected = -1;
let handles = [];                   // node handle meshes (index-aligned to spec.nodes)
let drawingBranch = null;           // {lanes, nodes:[]} while drawing
let roadTex = null, groundTex = null;   // preview textures
let showLanes = false, editNodes = false, showCpGates = true;
// AABB centre of the loaded level geometry, or null. Single source of truth for
// the view offset while a level is loaded, so the scenery, the billboards, the
// highlight and the centerline all sit in one frame.
let geomCenter = null;
function applyViewOffset() {
  const off = new THREE.Vector3(-center.x, -center.y, -center.z);
  envRoot.position.copy(off);      // == selRoot
  lightsRoot.position.copy(off);   // lights share raw engine coords
  // selBill/selHi are consts declared further down; safe because every call
  // site runs after module evaluation (the boot rebuild() is the last
  // statement). Do NOT add a `typeof` guard here -- typeof THROWS for a const
  // in its temporal dead zone, so it would hide nothing and break everything.
  selBill.position.copy(off);
  selHi.position.copy(off);
}
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
  if (!spec.nodes.length && geomCenter) { center.copy(geomCenter); applyViewOffset(); }
  if (!spec.nodes.length) {
    // No centerline: still give the viewport a floor reference, because the
    // studio now opens empty (authoring is parked) instead of on a sample loop.
    grid = new THREE.GridHelper(200000, 20, 0x2c3340, 0x1e2430); scene.add(grid);
    updatePanel(); return;
  }

  // recenter
  const c = new THREE.Vector3(); let miny = 1e18, maxr = 0;
  for (const n of spec.nodes) { c.x += n.x; c.z += n.z; c.y += (n.y || 0); miny = Math.min(miny, n.y || 0); }
  c.multiplyScalar(1 / spec.nodes.length);
  // Loaded geometry wins the offset. Otherwise rebuild() would keep resetting
  // it to the centerline centroid and shunt the scenery off the road, since
  // the two centres are not the same point.
  if (geomCenter) c.copy(geomCenter);
  center.copy(c);
  applyViewOffset();
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
// Same unload behind both buttons, for the same reason the load is shared.
$('clearEnvBtn').onclick = () => { unloadLevelGeometry(); setStatus('Cleared level geometry.', 'ok'); };
function envMaterial(tex, type) {   // unlit; transparency per page type (0 opaque,1 keyed,2 semi,3 additive)
  const base = { map: tex || null, color: tex ? 0xffffff : 0x8b9099, side: THREE.DoubleSide };
  if (!tex) return new THREE.MeshBasicMaterial(base);
  if (type === 1) return new THREE.MeshBasicMaterial({ ...base, transparent: true, alphaTest: 0.5 });
  if (type === 2) return new THREE.MeshBasicMaterial({ ...base, transparent: true });
  if (type === 3) return new THREE.MeshBasicMaterial({ ...base, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending });
  return new THREE.MeshBasicMaterial(base);
}
// Same call the LIBRARY tab makes -- see loadLevelGeometry. "Load environment"
// and "Load geometry" are one operation reached from two places, so the two
// tabs cannot show a different version of the same level.
$('loadEnvBtn').onclick = () => {
  if (currentLevel == null) { setStatus('Load a track first.', 'warn'); return; }
  loadLevelGeometry(currentLevel);
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
  if (!envLoaded) { setStatus('Load level geometry first (TRACKS section 1, or LIBRARY > Pick geometry).', 'warn'); return; }
  showGlowFixtures = !showGlowFixtures;
  $('glowShowBtn').textContent = showGlowFixtures ? 'hide glow fixtures' : 'show glow fixtures';
  rebuildGlowMarkers();
});
$('glowGenBtn').addEventListener('click', () => {
  if (!envLoaded) { setStatus('Load level geometry first (TRACKS section 1, or LIBRARY > Pick geometry).', 'warn'); return; }
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

// ---------------------------------------------------------------- library
// Browser over the shipped-geometry catalogue (re/assets/library). A prefab is
// previewed in ITS OWN local frame -- centred in XZ, base y=0 -- which is the
// frame td5_tg_prefab.c stamps it in, so what you see here is what the
// generator places. It is shown in a dedicated group rather than in envRoot so
// previewing never disturbs a loaded track.
const prefabRoot = new THREE.Group(); scene.add(prefabRoot);
let libIndex = null, libSel = null, libRows = [];

function libClearPreview() {
  while (prefabRoot.children.length) prefabRoot.remove(prefabRoot.children[0]);
}

async function libLoad() {
  try {
    libIndex = await (await fetch('/api/library')).json();
  } catch (e) { setStatus('library fetch failed: ' + e, 'bad'); return; }
  if (!libIndex.ok) { setStatus(libIndex.error || 'no library', 'warn'); return; }
  const sel = $('libLevel');
  sel.innerHTML = libIndex.levels.map((l) =>
    `<option value="${l}">level${String(l).padStart(3, '0')}</option>`).join('');
  if (libIndex.levels.includes(23)) sel.value = 23;   // Moscow: the worked example
  const ssel = $('selLevel');
  if (ssel) {
    ssel.innerHTML = sel.innerHTML;
    ssel.value = sel.value;
  }
  libList();
}

async function libList() {
  const lvl = $('libLevel').value, kind = $('libKind').value;
  const mf = $('libMinFaces').value || 0;
  let r;
  try {
    // "__lm" = the rarity-segmented SET PIECES, which is a different pass from
    // the object catalogue: catalogue objects are chunks of street frontage,
    // these are the whole pieces the generator actually stamps.
    r = (kind === '__lm')
      ? await (await fetch(`/api/library/landmarks?level=${lvl}`)).json()
      : await (await fetch(`/api/library/objects?level=${lvl}&kind=${encodeURIComponent(kind)}&min_faces=${mf}&limit=300`)).json();
  } catch (e) { setStatus('library list failed: ' + e, 'bad'); return; }
  if (!r.ok) { setStatus(r.error, 'warn'); return; }
  libRows = r.objects;
  $('libCount').textContent = `${r.total} object(s) match; showing ${libRows.length}.`;
  $('libList').innerHTML = libRows.map((o, i) =>
    `<div class="libitem" data-i="${i}" style="padding:2px 5px;cursor:pointer;
      border-bottom:1px solid #23262e;font-size:11px">
      <b>${o.kind}</b>${o.overridden ? ' *' : ''} &middot; ${o.faces}f &middot;
      ${Math.round(o.extent[0])}&times;${Math.round(o.extent[2])} h${Math.round(o.extent[1])}
      <span style="color:#78808f">${o.id}</span></div>`).join('');
  [...document.querySelectorAll('.libitem')].forEach((el) => {
    el.onclick = () => libShow(libRows[+el.dataset.i]);
  });
}

async function libShow(o) {
  libSel = o;
  const lvl = o.level;
  $('libInfo').innerHTML = `<b>${o.id}</b> &mdash; ${o.kind}, ${o.faces} faces,
    ${o.pages.length} page(s), footprint ${Math.round(o.extent[0])}&times;${Math.round(o.extent[2])},
    height ${Math.round(o.extent[1])}`;
  $('libPages').innerHTML = o.pages.slice(0, 24).map((p) =>
    `<img title="page ${p}" style="width:34px;height:34px;image-rendering:pixelated;
      border:1px solid #2c313c" src="/api/library/page?level=${lvl}&page=${p}">`).join('');
  const t = (libIndex && libIndex.tags && libIndex.tags[o.id]) || {};
  $('libKindSet').value = t.kind || '';
  $('libNote').value = t.note || '';

  setStatus(`Loading prefab ${o.id}…`);
  try {
    // fill=1 only affects segmented set pieces (L..lm..); harmless on catalogue
    // objects, which ignore it.
    const fill = $('libFill') && $('libFill').checked ? '&fill=1' : '';
    const buf = await (await fetch('/api/library/prefab?id=' + encodeURIComponent(o.id) + fill)).arrayBuffer();
    gltfLoader.parse(buf, '', async (gltf) => {
      const pages = new Set();
      gltf.scene.traverse((n) => { if (n.isMesh && n.userData && n.userData.page != null) pages.add(n.userData.page); });
      // Page TYPES come from the source level, so keyed art stays keyed. Without
      // this a railing or a window renders as a solid panel.
      let types = {};
      try { types = (await (await fetch('/api/assets?level=' + lvl)).json()).page_types || {}; } catch {}
      const texMap = {};
      await Promise.all([...pages].map(async (p) => {
        try { texMap[p] = await trackTexture(lvl, `page_${String(p).padStart(3, '0')}.png`, false); }
        catch { texMap[p] = null; }
      }));
      gltf.scene.traverse((n) => {
        if (n.isMesh) n.material = envMaterial(texMap[n.userData ? n.userData.page : -1],
                                               types[n.userData ? n.userData.page : -1] | 0);
      });
      libClearPreview();
      prefabRoot.add(gltf.scene);
      // The generator-kit list has no catalogue row behind it (it is parsed out
      // of a C header), so fall back to the GLB's own bounds rather than
      // fitting the camera to an extent of zero.
      let r = Math.max(o.extent[0] || 0, o.extent[1] || 0, o.extent[2] || 0) * 0.5;
      if (!r) {
        const b = new THREE.Box3().setFromObject(gltf.scene), s = new THREE.Vector3();
        b.getSize(s); r = Math.max(s.x, s.y, s.z) * 0.5;
      }
      fitCamera(r);
      setStatus(`${o.id}: ${o.faces || '?'} faces over ${pages.size} page(s).`, 'ok');
    }, (err) => setStatus('prefab GLB parse error: ' + err, 'bad'));
  } catch (e) { setStatus('prefab load failed: ' + e, 'bad'); }
}

async function libTag(clear) {
  if (!libSel) { setStatus('select an object first', 'warn'); return; }
  const body = { id: libSel.id, note: $('libNote').value };
  if (clear) body.clear = true; else body.kind = $('libKindSet').value;
  try {
    const r = await (await fetch('/api/library/tags', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body) })).json();
    if (r.ok) {
      setStatus(`Tagged ${r.id} (${r.tagged} total) -> ${r.path}`, 'ok');
      libIndex = await (await fetch('/api/library')).json();
      libList();
    } else setStatus(r.error || 'tag failed', 'bad');
  } catch (e) { setStatus('tag failed: ' + e, 'bad'); }
}

$('libRefresh').onclick = libList;
// The picker reads selLevel, which is now a hidden mirror of the one visible
// level control -- two level dropdowns in one tab was the bug waiting to happen.
$('libLevel').onchange = () => { $('selLevel').value = $('libLevel').value; libList(); };
$('libKind').onchange = libList;
$('libClearPreview').onclick = () => { libClearPreview(); setStatus('Preview cleared.', 'ok'); };
$('libFill').onchange = () => { if (libSel) libShow(libSel); };
$('libSaveTag').onclick = () => libTag(false);
$('libClearTag').onclick = () => libTag(true);

// -------------------------------------------------------- free selection
// Whole track as pickable geometry. Geometry stays grouped one node per page
// (458 draw calls, not 31,570 meshes) and every vertex carries its primitive id
// in _PRIMID, so a raycast hit reads the id back off the hit face. Highlight is
// drawn as wireframe boxes from the index AABBs rather than by recolouring,
// because a primitive is a slice of a merged buffer, not its own mesh.
const selRoot = envRoot;   // alias: one group, see its declaration at the top
const selBill = new THREE.Group(); scene.add(selBill);   // camera-facing sprites
const selHi = new THREE.Group(); scene.add(selHi);
let selPrims = null, selChosen = new Set(), selLevelNum = null;
let selDragFrom = null, selCenter = new THREE.Vector3();
// pid -> [{mesh, start, count}] vertex ranges inside the merged page buffers,
// recovered from _PRIMID at load. Needed for two things the AABB index cannot
// do: highlighting the REAL triangles, and addressing a single face.
let selRanges = null;
let selFaces = new Map();      // pid -> Set(face index within that primitive)
let selFlashUntil = 0;
// `var`, not `let`, and guarded below: the render loop is an IIFE near the top
// of this module and calls tickExtras on frame one, which is BEFORE a `let`
// here is initialised. That is a temporal-dead-zone ReferenceError thrown
// during module evaluation, which kills every statement after it -- the symptom
// is the whole panel silently failing to populate, not an obvious crash.
var selBillSets = [];      // [{mesh, items}] one InstancedMesh per page

// Billboards are rebuilt against the camera every frame, which is what the
// engine itself does (td5_render_mesh.c): they store a world position and LOCAL
// vertices, so baked flat they show edge-on or face an arbitrary direction.
// One InstancedMesh per page keeps this to ~50 draw calls for level023's 1406
// billboards instead of 1406.
function tickExtras() {
  // hoisted `var` (see its declaration): undefined on frame one, never a TDZ throw
  if (selHatchMat) selHatchMat.uniforms.uPhase.value = (performance.now() * 0.03) % 16.0;
  if (!selBillSets || !selBillSets.length) return;   // undefined on frame one
  const q = camera.quaternion, m = new THREE.Matrix4();
  const s = new THREE.Vector3(), p = new THREE.Vector3();
  for (const set of selBillSets) {
    for (let i = 0; i < set.items.length; i++) {
      const it = set.items[i];
      // RAW world centre. selBill.position already carries -selCenter (set in
      // applyViewOffset, same as selRoot), and the instance mesh is a child of
      // selBill, so subtracting selCenter here too double-offset every tree and
      // lamp glow by ~selCenter and scattered them far outside the track.
      p.set(it.c[0], it.c[1], it.c[2]);
      s.set(it.w || 1, it.h || 1, 1);
      m.compose(p, q, s);
      set.mesh.setMatrixAt(i, m);
    }
    set.mesh.instanceMatrix.needsUpdate = true;
  }
}

function selSetInfo(msg) {
  if (msg) { $('selInfo').innerHTML = msg; return; }
  if (!selPrims) { $('selInfo').innerHTML = 'Nothing loaded.'; return; }
  let nf = 0; for (const s of selFaces.values()) nf += s.size;
  const what = selGran() === 'face'
    ? `${nf} face(s) over ${selFaces.size} primitive(s)`
    : `${selChosen.size} primitive(s)`;
  $('selInfo').innerHTML = `${selPrims.length} primitives loaded &middot; ${what} selected`;
}

const selGran = () => $('selGran').value;

// ---- highlight, ported from the in-game free-cam picker -------------------
// td5_pick.c:305 outlines the picked mesh's REAL faces and fills them with
// marching 45-degree lines, always on top. The AABB wireframe this replaces
// was a lie about the shape: a cathedral and the block of air around it look
// identical as a box. Here the outline is EdgesGeometry over the actual
// triangles (which drops the quad diagonals, so quads read as quads like they
// do in-game) and the hatch is the same x+y screen-space march done in a
// fragment shader instead of by clipping lines on the CPU.
const SEL_HL = 0xffff00, SEL_HL_FLASH = 0x33ff33;   // yellow / green flash
// `var`, like selBillSets below: the render loop is an IIFE near the top of
// this module and calls tickExtras on frame one, before a `let`/`const` down
// here exists. `typeof` does NOT rescue that -- it throws for a const in its
// temporal dead zone, unlike an undeclared name -- so the guard has to be a
// plain truthiness test on a hoisted `var`.
var selHatchMat = new THREE.ShaderMaterial({
  uniforms: { uColor: { value: new THREE.Color(SEL_HL) }, uPhase: { value: 0 } },
  vertexShader: 'void main(){gl_Position=projectionMatrix*modelViewMatrix*vec4(position,1.0);}',
  fragmentShader: `uniform vec3 uColor; uniform float uPhase;
    void main(){
      if (mod(gl_FragCoord.x + gl_FragCoord.y + uPhase, 16.0) > 2.0) discard;
      gl_FragColor = vec4(uColor, 1.0);
    }`,
  side: THREE.DoubleSide, depthTest: false, depthWrite: false, transparent: true,
});
var selEdgeMat = new THREE.LineBasicMaterial({
  color: SEL_HL, depthTest: false, transparent: true });

function selHlColor() {
  const c = performance.now() < selFlashUntil ? SEL_HL_FLASH : SEL_HL;
  selHatchMat.uniforms.uColor.value.setHex(c);
  selEdgeMat.color.setHex(c);
}

// Every selected face's three positions, pulled straight out of the merged page
// buffers via the _PRIMID ranges.
function selHlPositions() {
  const out = [];
  const push = (r, f0, f1) => {
    const p = r.mesh.geometry.getAttribute('position');
    for (let v = r.start + f0 * 3; v < r.start + f1 * 3; v++)
      out.push(p.getX(v), p.getY(v), p.getZ(v));
  };
  if (selGran() === 'face') {
    for (const [pid, faces] of selFaces) {
      const rs = selRanges && selRanges.get(pid); if (!rs) continue;
      for (const f of faces) {
        // face index is primitive-local and runs across the ranges in order
        let base = 0;
        for (const r of rs) {
          const n = r.count / 3;
          if (f < base + n) { push(r, f - base, f - base + 1); break; }
          base += n;
        }
      }
    }
  } else {
    for (const pid of selChosen) {
      const rs = selRanges && selRanges.get(pid); if (!rs) continue;
      for (const r of rs) push(r, 0, r.count / 3);
    }
  }
  return out;
}

function selRedrawHighlight() {
  while (selHi.children.length) {
    const c = selHi.children[0];
    if (c.geometry) c.geometry.dispose();
    selHi.remove(c);
  }
  if (!selPrims) { selSetInfo(); return; }
  selHlColor();
  const pos = selHlPositions();
  if (pos.length) {
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
    const fill = new THREE.Mesh(g, selHatchMat); fill.renderOrder = 998;
    fill.frustumCulled = false; selHi.add(fill);
    // thresholdAngle 1 deg: the two halves of a source quad are coplanar, so
    // their shared diagonal drops out and the outline follows the real face.
    const edges = new THREE.LineSegments(new THREE.EdgesGeometry(g, 1), selEdgeMat);
    edges.renderOrder = 999; edges.frustumCulled = false; selHi.add(edges);
  }
  // Billboards live in selBill (rebuilt against the camera each frame), not in
  // the merged buffers, so they have no triangles to outline here -- fall back
  // to a box for those.
  for (const pid of selChosen) {
    if (!selPrims[pid] || !selPrims[pid].billboard) continue;
    const a = selPrims[pid].aabb;
    const h = new THREE.Box3Helper(new THREE.Box3(
      new THREE.Vector3(a[0], a[1], a[2]), new THREE.Vector3(a[3], a[4], a[5])), SEL_HL);
    h.material.depthTest = false; h.material.transparent = true;
    h.renderOrder = 999; selHi.add(h);
  }
  selHi.position.copy(selRoot.position);
  selSetInfo();
}

// THE loader. Both tabs call this: TRACKS via "Load environment", LIBRARY via
// "Load geometry". One endpoint, one offset, one group, always pickable --
// previously the track view decoded models.bin through a second exporter that
// baked billboards flat and emitted no _PRIMID, so the same level looked
// different and could not be selected depending on which button you pressed.
async function loadLevelGeometry(level) {
  const lvl = parseInt(level, 10);
  if (!Number.isFinite(lvl)) { setStatus('pick a level first', 'warn'); return; }
  selLevelNum = lvl;
  if ($('selLevel')) $('selLevel').value = String(lvl);
  setStatus(`Loading level${String(lvl).padStart(3, '0')} geometry…`);
  try {
    const ix = await (await fetch('/api/library/primindex?level=' + lvl)).json();
    selPrims = ix.prims;
    // The level lives in raw world coordinates -- level023 is centred near
    // (623121, -344, 295836) -- so without this offset it loads hundreds of
    // thousands of units from the camera and looks like nothing happened.
    selCenter.set(ix.center[0], ix.center[1], ix.center[2]);
    geomCenter = selCenter.clone();
    center.copy(selCenter);
    applyViewOffset();
    const buf = await (await fetch('/api/library/prims?level=' + lvl)).arrayBuffer();
    gltfLoader.parse(buf, '', async (gltf) => {
      const pages = new Set();
      gltf.scene.traverse((o) => { if (o.isMesh && o.userData && o.userData.page != null) pages.add(o.userData.page); });
      let types = {};
      try { types = (await (await fetch('/api/assets?level=' + lvl)).json()).page_types || {}; } catch {}
      const texMap = {};
      await Promise.all([...pages].map(async (p) => {
        try { texMap[p] = await trackTexture(lvl, `page_${String(p).padStart(3, '0')}.png`, false); }
        catch { texMap[p] = null; }
      }));
      gltf.scene.traverse((o) => {
        if (o.isMesh) o.material = envMaterial(texMap[o.userData.page], types[o.userData.page] | 0);
      });
      while (selRoot.children.length) selRoot.remove(selRoot.children[0]);
      selRoot.add(gltf.scene);
      selIndexRanges(gltf.scene);
      selChosen.clear(); selFaces.clear();
      await selLoadBillboards(lvl, types);
      // envLoaded hides the schematic ribbon: the real road is now here at full
      // resolution. rebuild() re-runs with geomCenter set, so the centerline
      // markers land in the same frame as the scenery.
      envLoaded = true; rebuild(false);
      selRedrawHighlight();
      const a = selPrims.reduce((acc, p) => {
        for (let k = 0; k < 3; k++) {
          acc[k] = Math.min(acc[k], p.aabb[k]); acc[k + 3] = Math.max(acc[k + 3], p.aabb[k + 3]);
        } return acc;
      }, [1e30, 1e30, 1e30, -1e30, -1e30, -1e30]);
      fitCamera(Math.max(a[3] - a[0], a[5] - a[2]) * 0.5);
      const k = ix.kinds || {};
      setStatus(`level${String(lvl).padStart(3, '0')}: ${selPrims.length} selectable primitives `
        + `(${k.structure || 0} structure, ${k.slab || 0} slab, ${k.billboard || 0} billboard, `
        + `${k.post || 0} post, ${k.ribbon || 0} rail). `
        + `Same geometry in both tabs; tick Pick mode in LIBRARY to select it.`, 'ok');
    }, (err) => setStatus('prims GLB parse error: ' + err, 'bad'));
  } catch (e) { setStatus('load failed: ' + e, 'bad'); }
}

function unloadLevelGeometry() {
  while (selRoot.children.length) selRoot.remove(selRoot.children[0]);
  while (selBill.children.length) selBill.remove(selBill.children[0]);
  selBillSets = [];
  selPrims = null; selRanges = null; selClearAll();
  geomCenter = null; envLoaded = false;
  selRedrawHighlight(); selSetInfo();
  rebuild(false);        // back to the centerline offset and the schematic ribbon
}

async function selLoadBillboards(lvl, types) {
  while (selBill.children.length) selBill.remove(selBill.children[0]);
  selBillSets = [];
  let r;
  try { r = await (await fetch('/api/library/billboards?level=' + lvl)).json(); }
  catch { return; }
  if (!r.ok || !r.count) return;
  const byPage = new Map();
  for (const b of r.billboards) {
    if (!byPage.has(b.page)) byPage.set(b.page, []);
    byPage.get(b.page).push(b);
  }
  const geo = new THREE.PlaneGeometry(1, 1);
  for (const [page, items] of byPage) {
    let tex = null;
    try { tex = await trackTexture(lvl, `page_${String(page).padStart(3, '0')}.png`, false); }
    catch { /* untextured is still selectable */ }
    // Billboards are keyed art (trees, glows), so alphaTest rather than opaque:
    // page type 0 here would draw the sprite's whole quad as a solid rectangle.
    const mat = new THREE.MeshBasicMaterial({
      map: tex, color: 0xffffff, side: THREE.DoubleSide,
      transparent: true, alphaTest: (types && types[page] === 2) ? 0.02 : 0.35,
      depthWrite: true });
    const mesh = new THREE.InstancedMesh(geo, mat, items.length);
    mesh.frustumCulled = false;    // matrices are rewritten every frame
    selBill.add(mesh);
    selBillSets.push({ mesh, items });
  }
}

// One pass over _PRIMID recovering where each primitive's vertices live. Every
// primitive in the corpus is single-page (measured: 32,444 of 32,444 on
// level023), so this is normally one contiguous run per id -- but it is stored
// as a list so a future multi-page split does not silently mis-slice.
// GLTFLoader LOWERCASES any attribute it does not recognise ("_PRIMID" ->
// "_primid"), so reading it back under the name the exporter wrote silently
// yields undefined. That is why click-to-select and the geometry highlight both
// did nothing while box select -- which works off the AABB index instead --
// looked fine. Accept both spellings.
const primIdAttr = (geo) => geo.getAttribute('_primid') || geo.getAttribute('_PRIMID');

function selIndexRanges(rootObj) {
  selRanges = new Map();
  const add = (pid, mesh, start, count) => {
    if (!selRanges.has(pid)) selRanges.set(pid, []);
    selRanges.get(pid).push({ mesh, start, count });
  };
  rootObj.traverse((o) => {
    if (!o.isMesh) return;
    const a = primIdAttr(o.geometry); if (!a) return;
    let cur = -1, start = 0;
    for (let i = 0; i < a.count; i++) {
      const v = Math.round(a.getX(i));
      if (v !== cur) { if (cur >= 0) add(cur, o, start, i - start); cur = v; start = i; }
    }
    if (cur >= 0) add(cur, o, start, a.count - start);
  });
}

// Ray hit -> {pid, face} where `face` is the triangle index WITHIN the
// primitive (see save_selection: primitive-local, so it survives a repack).
function selHitAt(ev) {
  const r = renderer.domElement.getBoundingClientRect();
  const m = new THREE.Vector2(((ev.clientX - r.left) / r.width) * 2 - 1,
                              -((ev.clientY - r.top) / r.height) * 2 + 1);
  const rc = new THREE.Raycaster(); rc.setFromCamera(m, camera);
  // Hits from BOTH the merged solid geometry (selRoot) and the camera-facing
  // billboards (selBill, instanced), tagged with distance so they interleave
  // correctly -- a tree sprite in front of a wall must win. Billboards carry no
  // per-face detail, so their whole quad is one "face".
  const hits = [];
  for (const h of rc.intersectObject(selRoot, true)) {
    const attr = h.object.geometry && primIdAttr(h.object.geometry);
    if (!attr || !h.face) continue;
    hits.push({ dist: h.distance, obj: h.object, faceA: h.face.a,
                pid: Math.round(attr.getX(h.face.a)) });
  }
  for (const set of selBillSets) {
    for (const bh of rc.intersectObject(set.mesh, true)) {
      const it = set.items[bh.instanceId];
      if (it) hits.push({ dist: bh.distance, bill: true, pid: it.id, faceA: -1 });
    }
  }
  hits.sort((a, b) => a.dist - b.dist);
  // Walk front-to-back and return the first hit the filter ALLOWS, not simply
  // the frontmost. A guardrail is a thin ribbon that usually sits in front of a
  // wall or over a slab; with the filter narrowed to rails, the frontmost hit
  // is often the very thing being excluded, and returning it would make the
  // click select nothing.
  for (const h of hits) {
    if (!selAllowed(h.pid)) continue;
    if (h.bill) return { pid: h.pid, face: 0 };   // one quad, one "face"
    const rs = (selRanges && selRanges.get(h.pid)) || [];
    let face = -1, base = 0;
    for (const rg of rs) {
      if (rg.mesh === h.obj && h.faceA >= rg.start && h.faceA < rg.start + rg.count) {
        face = base + Math.floor((h.faceA - rg.start) / 3); break;
      }
      base += rg.count / 3;
    }
    return { pid: h.pid, face };
  }
  return null;
}

// The segmenter's kind_hint vocabulary (td5_assetlib.all_prims) is finer than
// the four groups the filter offers, so map it. "rail" gathers the roadside
// furniture the old "structure only" gate silently hid -- guardrails are
// `ribbon`, their poles are `post`, fences are `ribbon` too -- which is what
// made them unpickable. "structure" here also takes `detail` (small attached
// trim) so a balustrade on a building is not orphaned from it.
function selGroupOf(p) {
  const k = p.kind;
  if (p.billboard || k === 'billboard') return 'billboard';
  if (k === 'slab') return 'slab';
  if (k === 'ribbon' || k === 'post') return 'rail';
  return 'structure';   // structure, detail, loose, wall, anything else
}
function selAllowed(id) {
  if (id < 0 || !selPrims || !selPrims[id]) return false;
  const f = $('selFilter') ? $('selFilter').value : 'all';
  return f === 'all' || selGroupOf(selPrims[id]) === f;
}

function selClearAll() { selChosen.clear(); selFaces.clear(); }
function selToggleFace(pid, face, remove) {
  if (!selFaces.has(pid)) selFaces.set(pid, new Set());
  const s = selFaces.get(pid);
  if (remove === true || (remove === undefined && s.has(face))) s.delete(face); else s.add(face);
  if (!s.size) selFaces.delete(pid);
}

// The buttons no longer change meaning with the mode -- see ORBIT_SCHEME at the
// top. Pick mode only changes what a LEFT drag selects, and the hint text.
function selApplyPickMode() {
  const on = $('selPick').checked;
  controls.mouseButtons = ORBIT_SCHEME;
  $('hudHelp').textContent = on
    ? 'PICK MODE · left-drag = box select · right-drag = orbit · middle-drag = pan · wheel = zoom · WASD = fly · Shift = add · Ctrl = remove · Esc = clear'
    : 'left-click = select · right-drag = orbit · middle-drag = pan · wheel = zoom · WASD = fly · Q/E = down/up · Shift = faster';
}

function selShowBox(a, b) {
  const el = $('dragBox'), r = renderer.domElement.getBoundingClientRect();
  if (!a) { el.style.display = 'none'; return; }
  el.style.display = 'block';
  el.style.left = (Math.min(a.x, b.x) - r.left) + 'px';
  el.style.top = (Math.min(a.y, b.y) - r.top) + 'px';
  el.style.width = Math.abs(b.x - a.x) + 'px';
  el.style.height = Math.abs(b.y - a.y) + 'px';
}

renderer.domElement.addEventListener('pointerdown', (ev) => {
  if (ev.button !== 0 || !$('selPick').checked || !selPrims) return;
  selDragFrom = { x: ev.clientX, y: ev.clientY, shift: ev.shiftKey, ctrl: ev.ctrlKey };
});
renderer.domElement.addEventListener('pointermove', (ev) => {
  if (!selDragFrom) return;
  const d = Math.hypot(ev.clientX - selDragFrom.x, ev.clientY - selDragFrom.y);
  if (d > 5) selShowBox(selDragFrom, { x: ev.clientX, y: ev.clientY });
});

renderer.domElement.addEventListener('pointerup', (ev) => {
  if (ev.button !== 0 || !$('selPick').checked || !selPrims || !selDragFrom) return;
  const from = selDragFrom; selDragFrom = null; selShowBox(null);
  const dx = Math.abs(ev.clientX - from.x), dy = Math.abs(ev.clientY - from.y);
  const add = from.shift || ev.shiftKey, sub = from.ctrl || ev.ctrlKey;
  const faceMode = selGran() === 'face';

  if (dx < 5 && dy < 5) {
    const hit = selHitAt(ev);
    if (!hit || !selAllowed(hit.pid)) {
      if (!add && !sub) { selClearAll(); selRedrawHighlight(); }
      return;
    }
    if (faceMode) {
      if (!add && !sub) selClearAll();
      selToggleFace(hit.pid, hit.face, sub ? true : (add ? undefined : false));
    } else {
      if (!add && !sub) selChosen.clear();
      if (sub) selChosen.delete(hit.pid);
      else if (add) selChosen.has(hit.pid) ? selChosen.delete(hit.pid) : selChosen.add(hit.pid);
      else selChosen.add(hit.pid);
    }
    selFlashUntil = performance.now() + 180;   // click flash, as td5_pick.c does
  } else {
    // BOX SELECT. Primitive mode tests each primitive's AABB centre; face mode
    // tests every face centroid, which is the only way a drag can carve a
    // window out of one wall rather than taking the whole wall.
    const r = renderer.domElement.getBoundingClientRect();
    const x0 = Math.min(from.x, ev.clientX), x1 = Math.max(from.x, ev.clientX);
    const y0 = Math.min(from.y, ev.clientY), y1 = Math.max(from.y, ev.clientY);
    const v = new THREE.Vector3();
    const inBox = () => {
      v.add(selRoot.position).project(camera);
      if (v.z > 1) return false;
      const sx = r.left + (v.x * 0.5 + 0.5) * r.width;
      const sy = r.top + (-v.y * 0.5 + 0.5) * r.height;
      return sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1;
    };
    if (!add && !sub) selClearAll();
    if (faceMode) {
      for (const [pid, rs] of (selRanges || new Map())) {
        if (!selAllowed(pid)) continue;
        let base = 0;
        for (const rg of rs) {
          const p = rg.mesh.geometry.getAttribute('position');
          for (let f = 0; f * 3 < rg.count; f++) {
            const i = rg.start + f * 3;
            v.set((p.getX(i) + p.getX(i + 1) + p.getX(i + 2)) / 3,
                  (p.getY(i) + p.getY(i + 1) + p.getY(i + 2)) / 3,
                  (p.getZ(i) + p.getZ(i + 1) + p.getZ(i + 2)) / 3);
            if (inBox()) selToggleFace(pid, base + f, sub);
          }
          base += rg.count / 3;
        }
      }
    } else {
      for (let i = 0; i < selPrims.length; i++) {
        if (!selAllowed(i)) continue;
        const a = selPrims[i].aabb;
        v.set((a[0] + a[3]) / 2, (a[1] + a[4]) / 2, (a[2] + a[5]) / 2);
        if (inBox()) { if (sub) selChosen.delete(i); else selChosen.add(i); }
      }
    }
  }
  selRedrawHighlight();
});

window.addEventListener('keydown', (e) => {
  if (inField(e.target)) return;
  if (e.key === 'Escape' && selPrims) { selClearAll(); selRedrawHighlight(); }
});

$('selGran').onchange = () => { selRedrawHighlight(); };
// Changing the filter is a lens on what a CLICK can hit; it does not retro-drop
// what is already selected, which would silently erase a careful selection when
// you narrow the lens to inspect one group.
$('selFilter').onchange = () => selSetInfo();
// Promote whatever faces are picked to their whole primitives -- the usual move
// after using face mode to find which piece a detail belongs to.
$('selGrow').onclick = () => {
  if (!selPrims) return;
  for (const pid of selFaces.keys()) selChosen.add(pid);
  selFaces.clear(); $('selGran').value = 'prim'; selRedrawHighlight();
};
$('selInvert').onclick = () => {
  if (!selPrims) return;
  const keep = selChosen;
  selChosen = new Set();
  for (let i = 0; i < selPrims.length; i++) if (selAllowed(i) && !keep.has(i)) selChosen.add(i);
  selFaces.clear(); selRedrawHighlight();
};

async function selRefreshList() {
  try {
    const r = await (await fetch('/api/library/selections?level=' + (selLevelNum || ''))).json();
    if ($('selKind').options.length === 0) {
      $('selKind').innerHTML = r.kinds.map((k) => `<option>${k}</option>`).join('');
    }
    $('selList').innerHTML = r.selections.length
      ? r.selections.map((s) => `<div><b>${s.kind}</b> · ${s.name} · ${s.ids.length} prims</div>`).join('')
      : 'No saved selections.';
  } catch (e) { setStatus('selection list failed: ' + e, 'bad'); }
}

async function selSave(del) {
  const name = $('selName').value.trim();
  if (!name) { setStatus('name the selection first', 'warn'); return; }
  // Face picks are saved as their own list AND as their owning primitive ids,
  // so a consumer that only understands whole primitives still sees the piece.
  const faces = [];
  const ids = new Set(selChosen);
  for (const [pid, fs] of selFaces) { ids.add(pid); for (const f of fs) faces.push([pid, f]); }
  if (!del && !ids.size) { setStatus('select some geometry first', 'warn'); return; }
  const body = { level: selLevelNum, name, kind: $('selKind').value,
                 ids: [...ids], faces };
  if (del) body.delete = true;
  try {
    const r = await (await fetch('/api/library/selection', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body) })).json();
    if (r.ok) {
      setStatus(`${del ? 'Deleted' : 'Saved'} "${name}" — ${r.prims} prim(s)`
        + (r.faces ? `, ${r.faces} face(s)` : '') + ` (${r.total} total) -> ${r.path}`, 'ok');
      selRefreshList();
    }
    else setStatus(r.error || 'save failed', 'bad');
  } catch (e) { setStatus('save failed: ' + e, 'bad'); }
}

$('selLoad').onclick = () => loadLevelGeometry($('libLevel').value);
$('selClearGeom').onclick = unloadLevelGeometry;

// ---- library overview: what is actually ON DISK right now ----------------
// The library is built by several independent passes (catalogue sweep, road
// curation, landmark export, hand selections) that are regenerated separately,
// so the useful question is not "what could exist" but "what is here, and is it
// stale relative to the rest".
async function libOverview() {
  let o;
  try { o = await (await fetch('/api/library/overview')).json(); }
  catch (e) { $('libOverview').textContent = 'overview failed: ' + e; return; }
  if (!o.ok) { $('libOverview').textContent = o.error || 'no library'; return; }
  const rs = Object.entries(o.road_sets || {}).map(([k, v]) => `${k} ${v}`).join(' · ');
  const kt = Object.entries(o.kind_totals || {}).sort((a, b) => b[1] - a[1])
    .map(([k, v]) => `${k} ${v.toLocaleString()}`).join(' · ');
  const pr = Object.entries(o.page_roles || {}).sort((a, b) => b[1] - a[1])
    .map(([k, v]) => `${k} ${v}`).join(' · ');
  $('libOverview').innerHTML = `
    <div><b>Catalogue</b> · built ${o.generated || '?'} · ${o.levels.length} levels</div>
    <div>${(o.objects_total || 0).toLocaleString()} objects &mdash; ${kt}</div>
    <div><b>Pages</b> ${o.page_total} &mdash; ${pr}</div>
    <div><b>Night tracks</b> ${(o.night || []).join(', ') || 'none'}</div>
    <div><b>Curated roads</b> ${rs || 'none'}</div>
    <div><b>Landmark pages</b> ${o.landmark_pages} · <b>prefabs in build</b> ${o.prefabs_in_build}</div>
    <div><b>Hand work</b> ${o.tags} tag(s), ${o.selections} selection(s)
      ${Object.keys(o.selections_by_kind || {}).length
        ? '(' + Object.entries(o.selections_by_kind).map(([k, v]) => `${k} ${v}`).join(', ') + ')' : ''}</div>`;
}
$('selSave').onclick = () => selSave(false);
$('selDelete').onclick = () => selSave(true);
$('selRefresh').onclick = selRefreshList;
$('selPick').onchange = selApplyPickMode;

// ---------------------------------------------------------------- tabs
// TRACKS looks at one shipped track. LIBRARY looks at the catalogue -- either
// per level, or the subset the generator actually compiled in. They are
// different questions over the same data, which is why the library stopped
// being a section inside the track panel.
function showTab(which) {
  const lib = which === 'library';
  $('tabTracks').style.display = lib ? 'none' : '';
  $('tabLibrary').style.display = lib ? '' : 'none';
  $('tabTracksBtn').classList.toggle('on', !lib);
  $('tabLibraryBtn').classList.toggle('on', lib);
}
$('tabTracksBtn').onclick = () => showTab('tracks');
$('tabLibraryBtn').onclick = () => showTab('library');

function libApplySource() {
  const gen = $('libSource').value === 'gen';
  $('libLevelRow').style.display = gen ? 'none' : '';
  $('libLevelPanes').style.display = gen ? 'none' : '';
  $('libGenPanes').style.display = gen ? '' : 'none';
  if (gen) genLoad();
}
$('libSource').onchange = libApplySource;

// ---------------------------------------------------------------- generator kit
// What is compiled into td5re.exe, parsed back out of the generated headers, as
// opposed to the 205k-object catalogue on disk. The generator can only place
// what is in here, and the two drift apart every time the catalogue is re-swept
// without re-running the exporters.
async function genLoad() {
  let g;
  try { g = await (await fetch('/api/library/genkit')).json(); }
  catch (e) { $('genSummary').textContent = 'genkit failed: ' + e; return; }
  if (!g.ok) { $('genSummary').textContent = g.error || 'no generator kit'; return; }
  const roadN = Object.values(g.roads).reduce((a, v) => a + v.length, 0);
  $('genSummary').innerHTML = `
    <div><b>${g.prefab_count}</b> set piece(s) &middot; <b>${g.landmark_pages}</b> landmark
      texture page(s) &middot; <b>${roadN}</b> road page(s)</div>
    <div style="color:#78808f">${g.header}</div>`;

  $('genPrefabs').innerHTML = g.prefabs.length
    ? g.prefabs.map((p, i) => `<div class="genpf" data-id="${p.id}" style="padding:2px 5px;
        cursor:pointer;border-bottom:1px solid #23262e;font-size:11px">
        <b>SET PIECE ${String(i).padStart(2, '0')}</b> &middot; ${p.faces}f &middot;
        ${p.footprint[0]}&times;${p.footprint[1]} h${p.height}
        <span style="color:#78808f">${p.id}</span></div>`).join('')
    : 'No prefabs in the build.';
  [...document.querySelectorAll('.genpf')].forEach((el) => {
    el.onclick = () => libShow({ id: el.dataset.id, level: +el.dataset.id.match(/^L(\d+)/)[1],
                                 kind: 'set piece', faces: 0, pages: [], extent: [0, 0, 0] });
  });

  $('genRoads').innerHTML = Object.entries(g.roads).map(([set, rows]) => `
    <div style="margin-bottom:6px"><b>${set.toUpperCase()}</b> (${rows.length})<br>
    ${rows.map((r) => `<img title="level${String(r.level).padStart(3, '0')} p${r.page} — ${r.desc}"
        style="width:40px;height:40px;image-rendering:pixelated;margin:1px;
               border:1px solid #2c313c"
        src="/api/library/page?level=${r.level}&page=${r.page}">`).join('')}</div>`).join('');
}

// A handle on the live scene for the console and for headless UI tests, which
// otherwise have no way to aim a click at a specific piece of geometry --
// clicking the middle of a fit-to-whole-level view hits sky and proves nothing.
window.__studio = { THREE, scene, camera, controls, selRoot, selHi,
                    get selPrims() { return selPrims; },
                    get selChosen() { return selChosen; },
                    get selFaces() { return selFaces; } };

// ---------------------------------------------------------------- boot
loadList();
libLoad();
libOverview();
selRefreshList();
selApplyPickMode();
rebuild(true);          // empty scene + reference grid; authoring is parked

// ---------------------------------------------------------------- gap picker
// DOUBLE-CLICK a set piece in the LIBRARY preview to mark exactly where you
// clicked: the triangle lights up, a marker drops on the hit point, and a
// readout pinned to the TOP of the viewport gives the WORLD coordinates.
//
// Why it exists: automatic hole-finding on these landmarks kept measuring the
// wrong thing -- a ray metric counted legitimate sky between spires and real
// window openings as holes -- so the reliable way to locate a gap is for a
// human to look at it and point. This turns "there is a hole over there" into
// numbers that can be authored against. Double-click (not click) so
// OrbitControls and the track-node handle picking are untouched.
//
// The preview is in the PREFAB frame (centred in XZ, base y=0); world =
// local + (cx, minY, cz) taken from the catalogue row's aabb.
const pickRoot = new THREE.Group();
scene.add(pickRoot);

const pickBox = document.createElement('div');
pickBox.id = 'pickBox';
pickBox.style.cssText = 'position:absolute;left:50%;top:8px;transform:translateX(-50%);' +
  'z-index:20;display:none;max-width:94%;padding:6px 10px;border-radius:4px;' +
  'border:1px solid var(--edge,#2c313c);background:rgba(14,17,23,.92);' +
  'font:12px/1.45 ui-monospace,Consolas,monospace;color:#d7dce5;' +
  'white-space:nowrap;overflow:hidden;text-overflow:ellipsis;pointer-events:none';
if (wrap) {
  if (getComputedStyle(wrap).position === 'static') wrap.style.position = 'relative';
  wrap.appendChild(pickBox);
}

function pickClear() {
  while (pickRoot.children.length) {
    const c = pickRoot.children.pop();
    if (c.geometry) c.geometry.dispose();
    if (c.material) c.material.dispose();
  }
}

function pickShow(html, cls) {
  pickBox.style.display = 'block';
  pickBox.style.borderColor = cls === 'hole' ? '#c9863f' : '#3f6ec9';
  pickBox.innerHTML = html;
}

// keep the old clear-preview behaviour honest: no stale marker on an empty stage
const _libClearPreviewBtn = $('libClearPreview');
if (_libClearPreviewBtn) _libClearPreviewBtn.addEventListener('click', () => {
  pickClear(); pickBox.style.display = 'none';
});

renderer.domElement.addEventListener('dblclick', (ev) => {
  if (!prefabRoot.children.length || !libSel || !libSel.aabb) return;
  const r = renderer.domElement.getBoundingClientRect();
  const m = new THREE.Vector2(((ev.clientX - r.left) / r.width) * 2 - 1,
                              -((ev.clientY - r.top) / r.height) * 2 + 1);
  raycaster.setFromCamera(m, camera);
  const hits = raycaster.intersectObjects(prefabRoot.children, true);
  const A = libSel.aabb;
  const off = new THREE.Vector3((A[0] + A[3]) / 2, A[1], (A[2] + A[5]) / 2);
  const big = Math.max(libSel.extent[0] || 0, libSel.extent[1] || 0, libSel.extent[2] || 0);
  pickClear();

  if (!hits.length) {
    // Pointing at a HOLE is the useful case, so say so rather than going quiet,
    // and draw the ray that went through so the hole is visible on screen.
    const d = raycaster.ray.direction.clone();
    const a = raycaster.ray.origin.clone();
    const b = a.clone().add(d.clone().multiplyScalar(big * 3));
    const ln = new THREE.Line(new THREE.BufferGeometry().setFromPoints([a, b]),
      new THREE.LineBasicMaterial({ color: 0xffa04a, depthTest: false, transparent: true }));
    ln.renderOrder = 999; pickRoot.add(ln);
    const line = `${libSel.id} HOLE — ray dir (${d.x.toFixed(4)}, ${d.y.toFixed(4)}, ${d.z.toFixed(4)})`;
    pickShow(`<b style="color:#ffa04a">HOLE</b> &nbsp;${libSel.id}&nbsp; ` +
      `ray origin (${a.clone().add(off).toArray().map((v) => v.toFixed(0)).join(', ')}) ` +
      `dir (${d.toArray().map((v) => v.toFixed(4)).join(', ')})`, 'hole');
    setStatus('no geometry under the cursor — that is a HOLE (ray drawn)', 'warn');
    console.log('[gap] MISS ' + libSel.id
      + ' ray origin (' + a.clone().add(off).toArray().map((v) => v.toFixed(0)).join(', ')
      + ') dir (' + d.toArray().map((v) => v.toFixed(4)).join(', ') + ')');
    try { navigator.clipboard.writeText(line); } catch (e) {}
    return;
  }

  const h = hits[0];
  const w = h.point.clone().add(off);
  const page = (h.object.userData && h.object.userData.page != null) ? h.object.userData.page : '?';

  // highlight the exact triangle, drawn over the top so it reads at any angle
  let verts = '(unavailable)';
  try {
    const pos = h.object.geometry.getAttribute('position');
    const P = [h.face.a, h.face.b, h.face.c].map((i) =>
      h.object.localToWorld(new THREE.Vector3().fromBufferAttribute(pos, i)));
    const g = new THREE.BufferGeometry().setFromPoints(P);
    const tri = new THREE.Mesh(g, new THREE.MeshBasicMaterial({
      color: 0x35d6ff, side: THREE.DoubleSide, transparent: true, opacity: 0.55,
      depthTest: false }));
    tri.renderOrder = 998; pickRoot.add(tri);
    const out = new THREE.LineLoop(new THREE.BufferGeometry().setFromPoints(P),
      new THREE.LineBasicMaterial({ color: 0x7ef2ff, depthTest: false, transparent: true }));
    out.renderOrder = 999; pickRoot.add(out);
    verts = P.map((v) => {
      const q = v.clone().add(off);
      return `(${q.x.toFixed(0)},${q.y.toFixed(0)},${q.z.toFixed(0)})`;
    }).join(' ');
  } catch (e) { /* highlight is a nicety; the numbers still go out */ }

  const dot = new THREE.Mesh(new THREE.SphereGeometry(Math.max(20, big * 0.012), 12, 8),
    new THREE.MeshBasicMaterial({ color: 0xffe36e, depthTest: false }));
  dot.position.copy(h.point); dot.renderOrder = 1000; pickRoot.add(dot);

  const line = `${libSel.id} world (${w.x.toFixed(0)}, ${w.y.toFixed(0)}, ${w.z.toFixed(0)}) page ${page} tri ${verts}`;
  pickShow(`<b style="color:#7ef2ff">PICK</b> &nbsp;${libSel.id}&nbsp; ` +
    `world <b>(${w.x.toFixed(0)}, ${w.y.toFixed(0)}, ${w.z.toFixed(0)})</b> ` +
    `page <b>${page}</b> &nbsp;tri ${verts}`);
  setStatus('picked — copied to clipboard', 'ok');
  console.log('[gap] ' + line);
  try { navigator.clipboard.writeText(line); } catch (e) {}
});
