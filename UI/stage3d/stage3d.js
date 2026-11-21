// The Solution - WebGL creature world for the 3D stage (three.js, MIT licence).
// Every track is a living creature; you can orbit, fly through the mix, and grab creatures with a 3D hand.
// The main page provides window.stageApi (state, objects, moves); this file only renders and handles 3D input.
import * as THREE from 'three';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js';
import { UnrealBloomPass } from 'three/addons/postprocessing/UnrealBloomPass.js';
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js';
import './robohand.js';

const api = window.stageApi;
const glCanvas = document.getElementById('glCanvas');
const gl3d = window.gl3d = { active: false, flying: false, clashHits: [], gpuInfo: null, fps: 0, quality: 'auto', lowQuality: false };

let renderer;
if (window.SAFE_MODE) renderer = null; else try {
  renderer = new THREE.WebGLRenderer({ canvas: glCanvas, antialias: true, alpha: true, powerPreference: 'high-performance' });
} catch (e) { renderer = null; }
if (renderer && renderer.getContext()) {
  gl3d.active = true;
  const ctx = renderer.getContext(), dbg = ctx.getExtension('WEBGL_debug_renderer_info');
  gl3d.gpuInfo = {
    renderer: dbg ? ctx.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : ctx.getParameter(ctx.RENDERER),
    vendor: dbg ? ctx.getParameter(dbg.UNMASKED_VENDOR_WEBGL) : ctx.getParameter(ctx.VENDOR),
    webgl: renderer.capabilities.isWebGL2 ? 'WebGL 2' : 'WebGL 1',
    maxTexture: renderer.capabilities.maxTextureSize,
  };
  init();
}

function init() {
  renderer.setClearColor(0x000000, 0);
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  const scene = new THREE.Scene();
  scene.fog = new THREE.FogExp2(0x05070b, 0.045);
  const camera = new THREE.PerspectiveCamera(52, 1.6, 0.05, 200);
  const composer = new EffectComposer(renderer);
  composer.addPass(new RenderPass(scene, camera));
  const bloom = new UnrealBloomPass(new THREE.Vector2(512, 512), 0.6, 0.45, 0.32);
  composer.addPass(bloom);
  composer.addPass(new OutputPass());

  scene.add(new THREE.AmbientLight(0x6b7a99, 0.55));
  const key = new THREE.PointLight(0xffe2b0, 40, 40); key.position.set(3, 6, -4); scene.add(key);
  const rim = new THREE.PointLight(0x38bdf8, 30, 40); rim.position.set(-5, 3, 6); scene.add(rim);

  // ---------- environment ----------
  const STAGE_R = 7;
  const env = new THREE.Group(); scene.add(env);
  const floor = new THREE.Mesh(new THREE.CircleGeometry(STAGE_R + 1, 96),
    new THREE.MeshBasicMaterial({ color: 0x0a1422, transparent: true, opacity: 0.55, depthWrite: false }));
  floor.rotation.x = -Math.PI / 2; floor.position.y = -0.002; env.add(floor);
  const ringMat = new THREE.LineBasicMaterial({ color: 0x38bdf8, transparent: true, opacity: 0.22 });
  const goldRingMat = new THREE.LineBasicMaterial({ color: 0xd4af37, transparent: true, opacity: 0.6 });
  [1, 2, 4, 8, 16, 32].forEach(d => {
    const r = api.distToR(d), pts = [];
    for (let a = 0; a <= 128; a++) { const t = a / 128 * Math.PI * 2; pts.push(new THREE.Vector3(r * Math.sin(t), 0, r * Math.cos(t))); }
    env.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), d === 2 ? goldRingMat : ringMat));
  });
  const spokes = [];
  for (let a = 0; a < 360; a += 30) { const t = a * Math.PI / 180; spokes.push(new THREE.Vector3(0, 0, 0), new THREE.Vector3(STAGE_R * Math.sin(t), 0, STAGE_R * Math.cos(t))); }
  env.add(new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints(spokes), new THREE.LineBasicMaterial({ color: 0x38bdf8, transparent: true, opacity: 0.08 })));
  // stars
  const starPos = new Float32Array(1800 * 3);
  for (let i = 0; i < 1800; i++) { const v = new THREE.Vector3().randomDirection().multiplyScalar(60 + Math.random() * 60); v.y = Math.abs(v.y) * 0.8 + 2; starPos.set([v.x, v.y, v.z], i * 3); }
  const stars = new THREE.Points(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(starPos, 3)),
    new THREE.PointsMaterial({ color: 0xbfd6ff, size: 0.18, transparent: true, opacity: 0.7, fog: false }));
  scene.add(stars);

  // you: a glowing head (headphones in binaural mode) + a 360 sphere
  const you = new THREE.Group(); scene.add(you);
  const headMat = new THREE.MeshStandardMaterial({ color: 0xe5e7eb, emissive: 0x9aa4b2, emissiveIntensity: 0.35, roughness: 0.4 });
  const head = new THREE.Mesh(new THREE.SphereGeometry(0.16, 24, 16), headMat); head.position.y = 0.25; you.add(head);
  const nose = new THREE.Mesh(new THREE.ConeGeometry(0.035, 0.12, 12), headMat); nose.rotation.x = Math.PI / 2; nose.position.set(0, 0.25, 0.18); you.add(nose);
  const phones = new THREE.Mesh(new THREE.TorusGeometry(0.19, 0.022, 8, 32, Math.PI), new THREE.MeshStandardMaterial({ color: 0x38bdf8, emissive: 0x38bdf8, emissiveIntensity: 1.2 }));
  phones.position.y = 0.25; phones.rotation.y = Math.PI / 2; you.add(phones);
  const sphere360 = new THREE.LineSegments(new THREE.WireframeGeometry(new THREE.SphereGeometry(api.distToR(2), 24, 12)),
    new THREE.LineBasicMaterial({ color: 0x38bdf8, transparent: true, opacity: 0.12 }));
  you.add(sphere360);

  // room walls, motion path, clash arcs
  const roomLines = new THREE.LineSegments(new THREE.BufferGeometry(), new THREE.LineBasicMaterial({ color: 0xd4af37, transparent: true, opacity: 0.32 }));
  scene.add(roomLines); let roomKey = '';
  const pathLine = new THREE.Line(new THREE.BufferGeometry(), new THREE.LineDashedMaterial({ color: 0xffc94d, dashSize: 0.25, gapSize: 0.15 }));
  scene.add(pathLine); let pathKey = '';
  const arcMat = new THREE.LineBasicMaterial({ color: 0xff4040 });
  const arcs = [];

  // ---------- the hand ----------
  const hand = new THREE.Group();
  const skin = new THREE.MeshStandardMaterial({ color: 0xf2c79b, emissive: 0x6b3d1f, emissiveIntensity: 0.25, roughness: 0.55, metalness: 0.05 });
  const palm = new THREE.Mesh(new THREE.BoxGeometry(0.09, 0.03, 0.1), skin); hand.add(palm);
  const fingers = [];
  [-0.033, -0.011, 0.011, 0.033].forEach((x, i) => {
    const f = new THREE.Group(); f.position.set(x, 0, 0.05);
    const seg = new THREE.Mesh(new THREE.CapsuleGeometry(0.009, 0.045 - Math.abs(i - 1.5) * 0.006, 4, 8), skin);
    seg.rotation.x = Math.PI / 2; seg.position.z = 0.03; f.add(seg); hand.add(f); fingers.push(f);
  });
  const thumb = new THREE.Group(); thumb.position.set(-0.05, 0, 0.0);
  const tseg = new THREE.Mesh(new THREE.CapsuleGeometry(0.01, 0.035, 4, 8), skin); tseg.rotation.z = Math.PI / 2.6; tseg.position.set(-0.02, 0, 0.02); thumb.add(tseg); hand.add(thumb);
  const beam = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(), new THREE.Vector3()]), new THREE.LineBasicMaterial({ color: 0xffd27a, transparent: true, opacity: 0.85 }));
  scene.add(beam); beam.visible = false;
  camera.add(hand); scene.add(camera);
  hand.position.set(0.2, -0.1, -0.5); hand.rotation.set(-0.35, 0.25, 0.1); hand.scale.setScalar(0.72);

  // ---------- creatures ----------
  const CREATURES = ['auto', 'manta', 'phoenix', 'jellyfish', 'serpent', 'fireflies', 'orb'];
  gl3d.creatureNames = { auto: 'Auto', manta: 'Manta ray', phoenix: 'Phoenix', jellyfish: 'Jellyfish', serpent: 'Serpent', fireflies: 'Firefly swarm', orb: 'Orb guardian' };
  function autoKind(o) {
    if ((o.width || 1) > 1.55) return 'jellyfish';
    const t = o.tone || 1000;
    if (t < 220) return 'manta';
    if (t < 650) return 'serpent';
    if (t < 2600) return 'phoenix';
    return 'fireflies';
  }
  const glow = (c, k = 1.6) => new THREE.MeshStandardMaterial({ color: c, emissive: c, emissiveIntensity: k, roughness: 0.35, metalness: 0.1 });
  function eyes(group, x, y, z, s = 0.035) {
    const white = new THREE.MeshBasicMaterial({ color: 0xffffff }), black = new THREE.MeshBasicMaterial({ color: 0x050505 });
    [-1, 1].forEach(side => {
      const e = new THREE.Mesh(new THREE.SphereGeometry(s, 12, 8), white); e.position.set(x * side, y, z); group.add(e);
      const p = new THREE.Mesh(new THREE.SphereGeometry(s * 0.5, 8, 6), black); p.position.set(x * side, y, z + s * 0.75); group.add(p);
    });
  }
  function wingShape(span, chord) {
    const s = new THREE.Shape(); s.moveTo(0, 0); s.quadraticCurveTo(span * 0.5, chord * 0.9, span, chord * 0.1); s.quadraticCurveTo(span * 0.55, -chord * 0.2, 0, -chord * 0.5); return new THREE.ShapeGeometry(s);
  }
  function build(kind, colour) {
    const g = new THREE.Group(), parts = { kind };
    const mat = glow(colour), dim = glow(colour, 0.6); mat.side = dim.side = THREE.DoubleSide;
    if (kind === 'manta') {
      const body = new THREE.Mesh(new THREE.SphereGeometry(0.32, 24, 12), mat); body.scale.set(1.0, 0.28, 1.25); g.add(body);
      parts.wings = [-1, 1].map(side => { const w = new THREE.Mesh(wingShape(0.75, 0.7), dim); w.rotation.x = -Math.PI / 2; w.scale.x = side; const pivot = new THREE.Group(); pivot.add(w); pivot.position.x = side * 0.18; g.add(pivot); return pivot; });
      const tail = new THREE.Mesh(new THREE.ConeGeometry(0.03, 0.8, 8), dim); tail.rotation.x = -Math.PI / 2; tail.position.z = -0.75; g.add(tail);
      eyes(g, 0.13, 0.07, 0.33);
    } else if (kind === 'phoenix') {
      const body = new THREE.Mesh(new THREE.SphereGeometry(0.16, 20, 12), mat); body.scale.set(0.8, 0.8, 1.5); g.add(body);
      const headM = new THREE.Mesh(new THREE.SphereGeometry(0.1, 16, 12), mat); headM.position.set(0, 0.07, 0.25); g.add(headM);
      const beak = new THREE.Mesh(new THREE.ConeGeometry(0.035, 0.12, 8), glow(0xffd27a, 2)); beak.rotation.x = Math.PI / 2; beak.position.set(0, 0.06, 0.37); g.add(beak);
      parts.wings = [-1, 1].map(side => { const w = new THREE.Mesh(wingShape(0.65, 0.32), dim); w.rotation.x = -Math.PI / 2; w.scale.x = side; const pivot = new THREE.Group(); pivot.add(w); pivot.position.x = side * 0.1; g.add(pivot); return pivot; });
      parts.tail = [-0.12, 0, 0.12].map(a => { const f = new THREE.Mesh(new THREE.PlaneGeometry(0.06, 0.45), dim); f.position.z = -0.38; f.rotation.set(-Math.PI / 2, 0, a); g.add(f); return f; });
      eyes(g, 0.05, 0.11, 0.31, 0.025);
    } else if (kind === 'jellyfish') {
      const bellMat = glow(colour, 1.2); bellMat.transparent = true; bellMat.opacity = 0.75;
      const bell = new THREE.Mesh(new THREE.SphereGeometry(0.34, 28, 14, 0, Math.PI * 2, 0, Math.PI / 2), bellMat); g.add(bell); parts.bell = bell;
      parts.tentacles = [];
      for (let i = 0; i < 9; i++) {
        const a = i / 9 * Math.PI * 2, n = 14, pos = new Float32Array(n * 3);
        const geo = new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(pos, 3));
        const line = new THREE.Line(geo, new THREE.LineBasicMaterial({ color: colour, transparent: true, opacity: 0.8 }));
        line.userData = { a, n }; g.add(line); parts.tentacles.push(line);
      }
    } else if (kind === 'serpent') {
      parts.segs = [];
      for (let i = 0; i < 11; i++) { const s = new THREE.Mesh(new THREE.SphereGeometry(0.11 * (1 - i * 0.065), 14, 10), i === 0 ? mat : dim); g.add(s); parts.segs.push(s); }
      const e = new THREE.Group(); eyes(e, 0.05, 0.05, 0.07, 0.022); parts.segs[0].add(e);
    } else if (kind === 'fireflies') {
      const n = 70, pos = new Float32Array(n * 3), seeds = [];
      for (let i = 0; i < n; i++) seeds.push([Math.random() * 6.28, Math.random() * 6.28, 0.2 + Math.random() * 0.35, 0.5 + Math.random()]);
      const pts = new THREE.Points(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(pos, 3)),
        new THREE.PointsMaterial({ color: colour, size: 0.07, transparent: true, opacity: 0.95, blending: THREE.AdditiveBlending, depthWrite: false }));
      g.add(pts); parts.swarm = { pts, seeds };
      g.add(new THREE.Mesh(new THREE.SphereGeometry(0.05, 10, 8), mat));
    } else {
      const core = new THREE.Mesh(new THREE.SphereGeometry(0.16, 20, 14), mat); g.add(core);
      const cage = new THREE.Mesh(new THREE.IcosahedronGeometry(0.3, 1), new THREE.MeshBasicMaterial({ color: colour, wireframe: true, transparent: true, opacity: 0.5 })); g.add(cage); parts.cage = cage;
      eyes(g, 0.05, 0.04, 0.14, 0.03);
    }
    // shared: floor shadow, width halo, height line, hit sphere
    const shadow = new THREE.Mesh(new THREE.CircleGeometry(0.4, 32), new THREE.MeshBasicMaterial({ color: colour, transparent: true, opacity: 0.09, depthWrite: false }));
    shadow.rotation.x = -Math.PI / 2; parts.shadow = shadow;
    const heightLine = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(), new THREE.Vector3()]), new THREE.LineDashedMaterial({ color: 0x94a3b8, dashSize: 0.05, gapSize: 0.05, transparent: true, opacity: 0.5 }));
    parts.heightLine = heightLine;
    const hit = new THREE.Mesh(new THREE.SphereGeometry(0.55, 8, 6), new THREE.MeshBasicMaterial({ visible: false })); g.add(hit); parts.hit = hit;
    parts.mats = [mat, dim];
    return { group: g, parts };
  }

  const creatures = new Map();     // slot -> { group, parts, kind, colourKey }
  function colourFor(o) {
    if (o.self) return new THREE.Color(0xf5b22b);
    const t = Math.max(0, Math.min(1, Math.log((o.tone || 1000) / 60) / Math.log(8000 / 60)));
    const hue = t < 0.1 ? t * 180 : 70 + (t - 0.1) / 0.9 * 200;
    return new THREE.Color().setHSL(hue / 360, 0.85, 0.58);
  }
  function syncCreatures(objs) {
    const seen = new Set();
    objs.forEach(o => {
      const key = o.self ? 'self' : 's' + o.slot; seen.add(key);
      const kind = (o.creature > 0 && CREATURES[o.creature]) ? CREATURES[o.creature] : autoKind(o);
      const col = colourFor(o), colourKey = col.getHexString();
      let c = creatures.get(key);
      if (!c || c.kind !== kind || c.colourKey !== colourKey) {
        if (c) { scene.remove(c.group); scene.remove(c.parts.shadow); scene.remove(c.parts.heightLine); }
        const b = build(kind, col); c = { group: b.group, parts: b.parts, kind, colourKey, pos: null };
        scene.add(c.group); scene.add(c.parts.shadow); scene.add(c.parts.heightLine);
        creatures.set(key, c);
      }
      c.obj = o;
    });
    for (const [key, c] of creatures) if (!seen.has(key)) { scene.remove(c.group); scene.remove(c.parts.shadow); scene.remove(c.parts.heightLine); creatures.delete(key); }
  }

  function animateCreature(c, t, dt) {
    const o = c.obj, p = c.parts;
    const target = new THREE.Vector3(...api.worldPos(o.az, o.el, o.dist));
    if (!c.pos) c.pos = target.clone(); else c.pos.lerp(target, Math.min(1, dt * 10));
    const loud = Math.max(0, Math.min(1, ((o.lufs ?? -40) + 50) / 44));
    const bob = Math.sin(t * 1.3 + (o.slot || 0)) * 0.05;
    c.group.position.copy(c.pos).add(new THREE.Vector3(0, 0.15 + bob, 0));
    const toYou = new THREE.Vector3(-c.pos.x, 0, -c.pos.z);
    if (toYou.lengthSq() > 1e-4) c.group.rotation.y = Math.atan2(toYou.x, toYou.z);
    const size = 0.75 + 0.6 * loud, widen = 0.75 + 0.35 * (o.width ?? 1);
    c.group.scale.set(size * widen, size, size);
    p.mats.forEach((m, i) => m.emissiveIntensity = (i ? 0.5 : 1.2) + 1.6 * loud);
    const speed = 2 + 7 * loud;
    if (p.wings) p.wings.forEach((w, i) => w.rotation.z = (i ? -1 : 1) * Math.sin(t * speed) * (p.kind === 'manta' ? 0.45 : 0.9));
    if (p.tail) p.tail.forEach((f, i) => f.rotation.y = Math.sin(t * 3 + i) * 0.25);
    if (p.bell) { const s = 1 + 0.12 * Math.sin(t * (2 + 3 * loud)); p.bell.scale.set(s, 1 / s, s); }
    if (p.tentacles) p.tentacles.forEach(line => {
      const { a, n } = line.userData, arr = line.geometry.attributes.position.array;
      for (let k = 0; k < n; k++) { const d = k / (n - 1); arr[k * 3] = Math.cos(a) * 0.24 * (1 - d * 0.4) + Math.sin(t * 2 + k * 0.5 + a) * 0.05 * d; arr[k * 3 + 1] = -d * 0.85; arr[k * 3 + 2] = Math.sin(a) * 0.24 * (1 - d * 0.4) + Math.cos(t * 1.7 + k * 0.4) * 0.05 * d; }
      line.geometry.attributes.position.needsUpdate = true;
    });
    if (p.segs) p.segs.forEach((s, i) => s.position.set(Math.sin(t * 3 - i * 0.6) * 0.12 * (i / 10 + 0.2), Math.sin(t * 2 - i * 0.4) * 0.03, -i * 0.11));
    if (p.swarm) {
      const arr = p.swarm.pts.geometry.attributes.position.array;
      p.swarm.seeds.forEach(([a, b, r, sp], i) => { const tt = t * sp * (1 + loud); arr[i * 3] = Math.cos(a + tt) * r * Math.cos(b + tt * 0.7); arr[i * 3 + 1] = Math.sin(b + tt * 0.9) * r * 0.7; arr[i * 3 + 2] = Math.sin(a + tt) * r * Math.cos(b + tt * 0.5); });
      p.swarm.pts.geometry.attributes.position.needsUpdate = true;
    }
    if (p.cage) { p.cage.rotation.y += dt * (0.5 + loud * 2); p.cage.rotation.x += dt * 0.3; }
    p.shadow.position.set(c.pos.x, 0.003, c.pos.z);
    p.shadow.scale.setScalar(0.6 + 0.5 * (o.width ?? 1));
    const hl = p.heightLine.geometry.attributes.position.array;
    hl.set([c.pos.x, 0, c.pos.z, c.group.position.x, c.group.position.y, c.group.position.z]);
    p.heightLine.geometry.attributes.position.needsUpdate = true; p.heightLine.computeLineDistances();
  }

  // ---------- camera: orbit (shared with the 2D stage) or fly ----------
  const fly = { pos: new THREE.Vector3(), yaw: 0, pitch: 0, keys: {}, speed: 2.6 };
  gl3d.enterFly = () => {
    if (gl3d.flying) return;
    gl3d.flying = true; fly.pos.copy(camera.position); fly.yaw = api.cam.yaw; fly.pitch = -0.35;
    api.flash('✈ FLY: WASD move · Q/E down/up · Shift fast · drag to look · click a creature to grab · wheel pull/push · Esc exit');
  };
  gl3d.exitFly = () => { if (!gl3d.flying) return; gl3d.flying = false; fly.keys = {}; api.flash('Orbit view'); };
  window.addEventListener('keydown', e => { if (!gl3d.flying) return; const k = e.key.toLowerCase(); if ('wasdqe'.includes(k) && k.length === 1) { fly.keys[k] = true; e.stopImmediatePropagation(); e.preventDefault(); } if (e.key === 'Shift') fly.keys.shift = true; }, true);
  window.addEventListener('keyup', e => { const k = e.key.toLowerCase(); if (k.length === 1) fly.keys[k] = false; if (e.key === 'Shift') fly.keys.shift = false; }, true);
  gl3d.look = (dx, dy) => { fly.yaw += dx * 0.006; fly.pitch = Math.max(-1.45, Math.min(1.45, fly.pitch - dy * 0.005)); };

  function updateCamera(dt) {
    const W = glCanvas.clientWidth || 560, H = glCanvas.clientHeight || 360;
    camera.aspect = W / H;
    if (gl3d.flying) {
      const fwd = new THREE.Vector3(Math.sin(fly.yaw) * Math.cos(fly.pitch), Math.sin(fly.pitch), Math.cos(fly.yaw) * Math.cos(fly.pitch));
      const right = new THREE.Vector3(Math.cos(fly.yaw), 0, -Math.sin(fly.yaw));
      const v = new THREE.Vector3(), k = fly.keys;
      if (k.w) v.add(fwd); if (k.s) v.sub(fwd); if (k.d) v.add(right); if (k.a) v.sub(right); if (k.e) v.y += 1; if (k.q) v.y -= 1;
      pad.move && v.add(fwd.clone().multiplyScalar(-pad.move[1])).add(right.clone().multiplyScalar(pad.move[0]));
      if (pad.updown) v.y += pad.updown;
      if (v.lengthSq() > 0) fly.pos.addScaledVector(v.normalize(), dt * fly.speed * (k.shift || pad.fast ? 3 : 1));
      fly.pos.y = Math.max(0.15, Math.min(12, fly.pos.y));
      camera.position.copy(fly.pos);
      camera.lookAt(fly.pos.clone().add(fwd));
    } else {
      const top = api.vizMode() === 1, cam = api.cam, pitch = top ? 1.48 : cam.pitch, yaw = top ? 0 : cam.yaw;
      const R = (top ? 12.5 : 8.6) / cam.zoom, cp = Math.cos(pitch), target = new THREE.Vector3(0, top ? 0 : 0.5, top ? 0.4 : 1.8);
      camera.position.set(target.x - R * Math.sin(yaw) * cp, target.y + R * Math.sin(pitch), target.z - R * Math.cos(yaw) * cp);
      camera.lookAt(target);
    }
    camera.updateProjectionMatrix(); camera.updateMatrixWorld();
  }

  // ---------- gamepad / joystick ----------
  const pad = { move: null, updown: 0, fast: false, prev: {} };
  function pollGamepad(dt) {
    const gps = navigator.getGamepads ? Array.from(navigator.getGamepads()).filter(Boolean) : [];
    pad.move = null; pad.updown = 0; pad.fast = false;
    if (!gps.length) return;
    const g = gps[0], dz = (v) => Math.abs(v) < 0.15 ? 0 : v;
    const lx = dz(g.axes[0] || 0), ly = dz(g.axes[1] || 0), rx = dz(g.axes[2] || 0), ry = dz(g.axes[3] || 0);
    const btn = (i) => g.buttons[i] && g.buttons[i].pressed, val = (i) => g.buttons[i] ? g.buttons[i].value : 0;
    const pressedNow = (i) => btn(i) && !pad.prev[i];
    if (!gl3d.flying && (lx || ly || rx || ry)) gl3d.enterFly();
    if (pressedNow(9)) gl3d.flying ? gl3d.exitFly() : gl3d.enterFly();                    // Start
    if (gl3d.flying) {
      pad.move = [lx, ly]; fly.yaw += rx * dt * 2.2; fly.pitch = Math.max(-1.45, Math.min(1.45, fly.pitch - ry * dt * 1.8));
      pad.updown = val(7) - val(6); pad.fast = btn(10);
      if (pressedNow(0)) heldByPad ? releasePad() : grabCentre();                          // A: grab / drop
      if (pressedNow(1)) { if (heldByPad) releasePad(); gl3d.exitFly(); }                   // B: drop / exit
      if (heldByPad) { if (btn(4)) heldDist = Math.max(0.6, heldDist - dt * 2.5); if (btn(5)) heldDist = Math.min(STAGE_R, heldDist + dt * 2.5);
                       if (btn(12)) api.nudgeWidth(heldByPad, 0.6 * dt); if (btn(13)) api.nudgeWidth(heldByPad, -0.6 * dt); }
    }
    g.buttons.forEach((b, i) => pad.prev[i] = b.pressed);
  }
  let heldByPad = null, heldDist = 2, lastPadSend = 0;
  function grabCentre() {
    const ray = new THREE.Raycaster(); ray.setFromCamera(new THREE.Vector2(0, 0), camera);
    const hit = pick(ray); if (!hit) return;
    heldByPad = hit; heldDist = camera.position.distanceTo(creatureFor(hit).group.position); api.beginHold(hit);
  }
  function releasePad() { if (heldByPad) api.endHold(heldByPad); heldByPad = null; }
  function creatureFor(o) { return creatures.get(o.self ? 'self' : 's' + o.slot); }

  // ---------- picking & dragging (used by the main page's mouse handlers) ----------
  const raycaster = new THREE.Raycaster();
  function ndc(mx, my) { return new THREE.Vector2(mx / (glCanvas.clientWidth || 1) * 2 - 1, -(my / (glCanvas.clientHeight || 1)) * 2 + 1); }
  function pick(ray) {
    let best = null, bd = Infinity;
    for (const c of creatures.values()) { const h = ray.intersectObject(c.parts.hit, false)[0]; if (h && h.distance < bd) { bd = h.distance; best = c.obj; } }
    return best;
  }
  gl3d.pick = (mx, my) => { raycaster.setFromCamera(ndc(mx, my), camera); return pick(raycaster); };
  gl3d.floorHit = (mx, my) => {
    raycaster.setFromCamera(ndc(mx, my), camera);
    const p = new THREE.Vector3();
    return raycaster.ray.intersectPlane(new THREE.Plane(new THREE.Vector3(0, 1, 0), 0), p) ? [p.x, 0, p.z] : null;
  };
  let mouseHoldDist = null;
  gl3d.beginMouseHold = (o) => { const c = creatureFor(o); mouseHoldDist = c ? camera.position.distanceTo(c.group.position) : 3; };
  gl3d.endMouseHold = () => { mouseHoldDist = null; };
  gl3d.pull = (dy) => { if (mouseHoldDist !== null) mouseHoldDist = Math.max(0.6, Math.min(14, mouseHoldDist * (dy > 0 ? 1.08 : 1 / 1.08))); };
  function toStage(p) {     // display-space point -> azimuth / elevation / distance
    const r = Math.max(0.05, p.length());
    return { az: Math.atan2(p.x, p.z) * 180 / Math.PI, el: Math.max(-90, Math.min(90, Math.asin(Math.max(-1, Math.min(1, p.y / r))) * 180 / Math.PI)), dist: api.rToDist(Math.min(STAGE_R, r)) };
  }
  // Where a held creature should go: in fly mode it rides your hand at its grab distance.
  gl3d.dragTarget = (mx, my) => {
    if (!gl3d.flying || mouseHoldDist === null) return null;
    raycaster.setFromCamera(ndc(mx, my), camera);
    const p = raycaster.ray.origin.clone().addScaledVector(raycaster.ray.direction, mouseHoldDist);
    p.y -= 0.15;
    return toStage(p);
  };

  // ---------- overlay: labels, clash badges, crosshair (drawn on the 2D canvas) ----------
  function project(v, W, H) { const p = v.clone().project(camera); if (p.z > 1) return null; return { x: (p.x + 1) / 2 * W, y: (1 - p.y) / 2 * H }; }
  gl3d.drawOverlay = (ctx, W, H) => {
    const rects = [];
    const pill = (text, x, y, fill, colour, bold) => {
      ctx.font = (bold ? 'bold ' : '') + '10px monospace'; const w = ctx.measureText(text).width + 12, h = 15; let ry = y;
      for (let i = 0; i < 6; i++) { const hit = rects.find(r => x - w / 2 < r.x + r.w && x + w / 2 > r.x && ry - h < r.y + r.h && ry > r.y); if (!hit) break; ry = hit.y + hit.h + h + 1; }
      rects.push({ x: x - w / 2, y: ry - h, w, h });
      ctx.fillStyle = fill; ctx.beginPath(); ctx.roundRect ? ctx.roundRect(x - w / 2, ry - h, w, h, 7) : ctx.rect(x - w / 2, ry - h, w, h); ctx.fill();
      ctx.fillStyle = colour; ctx.textAlign = 'center'; ctx.fillText(text, x, ry - 4); ctx.textAlign = 'left';
      return { x: x - w / 2, y: ry - h, w, h };
    };
    const list = [...creatures.values()].map(c => ({ c, p: project(c.group.position.clone().add(new THREE.Vector3(0, -0.55, 0)), W, H), d: camera.position.distanceTo(c.group.position) }))
      .filter(e => e.p).sort((a, b) => a.d - b.d);
    list.forEach(({ c, p }) => { const o = c.obj; pill(((o.name || o.module || '') + (o.self ? ' (you)' : '')).slice(0, 26), p.x, p.y + 16, 'rgba(5,7,10,0.72)', o.self ? '#F5D27A' : 'rgba(229,231,235,0.92)', o.self); });
    gl3d.clashHits = [];
    clashes.forEach(cl => {
      const a = creatureFor(cl.a), b = creatureFor(cl.b); if (!a || !b) return;
      const mid = a.group.position.clone().add(b.group.position).multiplyScalar(0.5).add(new THREE.Vector3(0, 0.75, 0));
      const p = project(mid, W, H); if (!p) return;
      const r = pill(`⚠ CLASH ${cl.band} · CLICK TO UNMASK`, p.x, p.y, 'rgba(127,29,29,0.95)', '#fff', true);
      gl3d.clashHits.push(Object.assign(r, { clash: cl }));
    });
    if (gl3d.flying) {
      ctx.strokeStyle = 'rgba(245,210,122,0.85)'; ctx.lineWidth = 1.5;
      ctx.beginPath(); ctx.arc(W / 2, H / 2, 7, 0, Math.PI * 2); ctx.moveTo(W / 2 - 13, H / 2); ctx.lineTo(W / 2 - 9, H / 2); ctx.moveTo(W / 2 + 9, H / 2); ctx.lineTo(W / 2 + 13, H / 2); ctx.stroke();
      ctx.fillStyle = 'rgba(56,189,248,0.85)'; ctx.font = 'bold 10px monospace'; ctx.fillText('✈ FLYING · WASD · Q/E · drag to look · click to grab · Esc', 12, H - 92);
    }
  };

  // ---------- hologram centrepiece (one per module, coloured by the theme) ----------
  const moduleName = (new URLSearchParams(location.search).get('module') || '').toLowerCase();
  const holo = new THREE.Group(); scene.add(holo);
  const holoMat = new THREE.LineBasicMaterial({ color: 0xff8a1c, transparent: true, opacity: 0.85, blending: THREE.AdditiveBlending, depthWrite: false });
  const holoMat2 = new THREE.LineBasicMaterial({ color: 0x38bdf8, transparent: true, opacity: 0.6, blending: THREE.AdditiveBlending, depthWrite: false });
  const kind = moduleName.includes('bunker') ? 'cube' : moduleName.includes('orbit') ? 'dome' : moduleName.includes('aether') ? 'spectral' : 'ring';
  let wave = null, bars = [], dome = null, waterfall = [], wfTick = 0;
  if (kind === 'cube') {
    holo.position.set(0, 2.5, 7.2); holo.scale.setScalar(1.35);
    holo.add(new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.BoxGeometry(2.4, 1.4, 1.4)), holoMat));
    wave = new THREE.Line(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(new Float32Array(128 * 3), 3)), holoMat2); holo.add(wave);
    for (let i = 0; i < 12; i++) { const b = new THREE.Mesh(new THREE.BoxGeometry(0.14, 1, 0.14), new THREE.MeshBasicMaterial({ color: 0xff8a1c, transparent: true, opacity: 0.4, blending: THREE.AdditiveBlending, depthWrite: false }));
      b.position.set(-1.05 + i * 0.19, -0.7, 0.55); holo.add(b); bars.push(b); }
  } else if (kind === 'dome') {
    dome = new THREE.Mesh(new THREE.SphereGeometry(STAGE_R + 0.6, 48, 24, 0, Math.PI * 2, 0, Math.PI / 2),
      new THREE.MeshBasicMaterial({ color: 0x22d3ee, transparent: true, opacity: 0.05, side: THREE.DoubleSide, depthWrite: false, blending: THREE.AdditiveBlending }));
    scene.add(dome);
    const domeLines = new THREE.LineSegments(new THREE.WireframeGeometry(new THREE.SphereGeometry(STAGE_R + 0.6, 24, 8, 0, Math.PI * 2, 0, Math.PI / 2)), holoMat2);
    domeLines.material = holoMat2.clone(); domeLines.material.opacity = 0.12; scene.add(domeLines);
    for (let i = 0; i < 3; i++) { const ring = new THREE.Line(new THREE.BufferGeometry().setFromPoints(new THREE.EllipseCurve(0, 0, 1.2 + i * 0.5, 1.2 + i * 0.5).getPoints(96).map(p => new THREE.Vector3(p.x, 0, p.y))), holoMat);
      ring.rotation.set(0.4 * (i + 1), i, 0); holo.add(ring); }
    holo.position.set(0, 3.2, 6.5); holo.scale.setScalar(0.8);
  } else if (kind === 'spectral') {
    holo.position.set(0, 0.6, 7.4);
    for (let r = 0; r < 18; r++) { const line = new THREE.Line(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(new Float32Array(36 * 3), 3)), r ? holoMat2 : holoMat);
      holo.add(line); waterfall.push(line); }
  } else {
    holo.position.set(0, 3.1, 6.8);
    for (let i = 0; i < 2; i++) { const ring = new THREE.Line(new THREE.BufferGeometry().setFromPoints(new THREE.EllipseCurve(0, 0, 0.9 + i * 0.35, 0.9 + i * 0.35).getPoints(96).map(p => new THREE.Vector3(p.x, p.y, 0))), i ? holoMat2 : holoMat);
      holo.add(ring); }
  }
  function updateHologram(t, dt) {
    const sc = window._scope || {};
    holo.rotation.y = kind === 'cube' ? Math.sin(t * 0.3) * 0.35 : kind === 'ring' ? t * 0.4 : holo.rotation.y;
    if (kind === 'dome') holo.children.forEach((r, i) => { r.rotation.z += dt * (0.2 + i * 0.15); });
    if (wave && sc.lr && sc.lr.length) {
      const arr = wave.geometry.attributes.position.array, n = 128, step = Math.max(1, Math.floor(sc.lr.length / 2 / n));
      for (let i = 0; i < n; i++) { const v = sc.lr[i * step * 2] || 0; arr[i * 3] = -1.15 + 2.3 * i / (n - 1); arr[i * 3 + 1] = Math.max(-0.65, Math.min(0.65, v * 1.6)); arr[i * 3 + 2] = Math.sin(i * 0.2 + t) * 0.15; }
      wave.geometry.attributes.position.needsUpdate = true;
    }
    if (bars.length && sc.spec) bars.forEach((b, i) => { const db = sc.spec[Math.min(sc.spec.length - 1, i)] ?? -90, h = Math.max(0.05, (db + 70) / 60); b.scale.y = h; b.position.y = -0.7 + h / 2; });
    if (waterfall.length && sc.spec && (wfTick += dt) > 0.08) {
      wfTick = 0;
      for (let r = waterfall.length - 1; r > 0; r--) waterfall[r].geometry.attributes.position.array.set(waterfall[r - 1].geometry.attributes.position.array);
      const arr = waterfall[0].geometry.attributes.position.array;
      for (let i = 0; i < 36; i++) { const db = sc.spec[Math.floor(i * sc.spec.length / 36)] ?? -90; arr[i * 3] = -4 + 8 * i / 35; arr[i * 3 + 1] = Math.max(0, (db + 72) / 22); arr[i * 3 + 2] = 0; }
      waterfall.forEach((l, r) => { const a = l.geometry.attributes.position.array; for (let i = 0; i < 36; i++) a[i * 3 + 2] = r * 0.35; l.geometry.attributes.position.needsUpdate = true; });
    }
  }
  // Theme colours for the 3D world (matches the CSS hardware theme).
  const THEME_COLOURS = { '': [0xd4af37, 0x38bdf8], hypercar: [0xff8a1c, 0xffc266], deco: [0xe9b949, 0x5ed0ff], dome: [0x22d3ee, 0xe879f9],
                          abyssal: [0x2dd4bf, 0x60a5fa], brass: [0xf2b04b, 0xf7e2b0], holo: [0xfbbf24, 0x22d3ee], sunset: [0xff6a1a, 0x3ba9ff] };
  gl3d.setTheme = (id) => {
    const [a, b] = THEME_COLOURS[id] || THEME_COLOURS[''];
    holoMat.color.setHex(a); holoMat2.color.setHex(b); goldRingMat.color.setHex(a); ringMat.color.setHex(b);
    bars.forEach(x => x.material.color.setHex(a)); if (dome) dome.material.color.setHex(b);
  };
  gl3d.setTheme(document.documentElement.getAttribute('data-theme') || '');

  // ---------- main loop ----------
  let clashes = [], last = performance.now(), frames = 0, fpsT = 0, slowFor = 0, lastHoldSend = 0;
  gl3d.render = () => {
    const now = performance.now(), dt = Math.min(0.1, (now - last) / 1000), t = now / 1000; last = now;
    frames++; fpsT += dt; if (fpsT >= 1) { gl3d.fps = Math.round(frames / fpsT); frames = 0; fpsT = 0; }
    // graphics quality: Auto drops bloom + resolution if the GPU can't keep up
    const q = gl3d.quality;
    if (q === 'auto') { slowFor = dt > 1 / 35 ? slowFor + dt : Math.max(0, slowFor - dt * 0.5); if (slowFor > 2) gl3d.lowQuality = true; if (slowFor === 0 && gl3d.fps > 55) gl3d.lowQuality = false; }
    else gl3d.lowQuality = q === 'low';
    const W = glCanvas.clientWidth || 560, H = glCanvas.clientHeight || 360, dpr = gl3d.lowQuality ? 1 : Math.min(2, window.devicePixelRatio || 1);
    if (renderer.getPixelRatio() !== dpr) renderer.setPixelRatio(dpr);
    const size = renderer.getSize(new THREE.Vector2());
    if (size.x !== W || size.y !== H) { renderer.setSize(W, H, false); composer.setSize(W, H); }
    composer.setPixelRatio(dpr);

    pollGamepad(dt);
    updateCamera(dt);
    const objs = api.objects();
    syncCreatures(objs);
    for (const c of creatures.values()) animateCreature(c, t, dt);
    clashes = api.findClashes(objs);

    // headphone sphere + room + path + clash arcs
    const state = api.state(), rack = api.rack() || {};
    phones.visible = sphere360.visible = state.binaural_mode > 0.5;
    const rk = rack.roomDims ? rack.roomDims.join(',') : '';
    if (rk !== roomKey) {
      roomKey = rk; const pts = [];
      if (rack.roomDims) {
        const [rw, rl, rh] = rack.roomDims, x0 = -rw / 2, x1 = rw / 2, z0 = -0.4 * rl, z1 = 0.6 * rl;
        const c = [[x0, 0, z0], [x1, 0, z0], [x1, 0, z1], [x0, 0, z1], [x0, rh, z0], [x1, rh, z0], [x1, rh, z1], [x0, rh, z1]].map(q => new THREE.Vector3(...api.mapRoomPoint(q[0], q[1] - 1.2, q[2])).setY(api.mapRoomPoint(q[0], q[1] - 1.2, q[2])[1] + 1.2 * 0.35));
        [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]].forEach(([i, j]) => pts.push(c[i], c[j]));
      }
      roomLines.geometry.dispose(); roomLines.geometry = new THREE.BufferGeometry().setFromPoints(pts);
    }
    const path = api.pathPoints();
    const pk = JSON.stringify(path);
    if (pk !== pathKey) {
      pathKey = pk;
      const pts = path.map(p => new THREE.Vector3(...api.worldPos(p[0], p[1], p[2])).add(new THREE.Vector3(0, 0.15, 0)));
      pathLine.geometry.dispose();
      if (pts.length > 1) { const curve = new THREE.CatmullRomCurve3(pts, !api.isDrawingPath()); pathLine.geometry = new THREE.BufferGeometry().setFromPoints(curve.getPoints(pts.length * 12)); pathLine.computeLineDistances(); }
      else pathLine.geometry = new THREE.BufferGeometry();
    }
    pathLine.material.dashOffset = -t * 0.6;
    arcs.forEach(a => scene.remove(a)); arcs.length = 0;
    clashes.forEach(cl => {
      const a = creatureFor(cl.a), b = creatureFor(cl.b); if (!a || !b) return;
      const pts = []; for (let i = 0; i <= 12; i++) { const p = a.group.position.clone().lerp(b.group.position, i / 12); if (i > 0 && i < 12) p.add(new THREE.Vector3((Math.random() - 0.5) * 0.18, (Math.random() - 0.5) * 0.18, (Math.random() - 0.5) * 0.18)); pts.push(p); }
      const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), arcMat); scene.add(line); arcs.push(line);
    });

    // the hand: open, or closed around whatever you're holding, with a glowing tether
    const held = heldByPad || api.heldObject();
    hand.visible = gl3d.flying || !!held;
    const closed = held ? 1 : 0;
    fingers.forEach((f, i) => f.rotation.x = THREE.MathUtils.lerp(f.rotation.x, closed * 1.2 + Math.sin(t * 2 + i) * 0.03, 0.25));
    thumb.rotation.y = THREE.MathUtils.lerp(thumb.rotation.y, closed * 0.8, 0.25);
    beam.visible = !!held;
    if (held) {
      const c = creatureFor(held);
      if (c) { const from = hand.getWorldPosition(new THREE.Vector3()); beam.geometry.attributes.position.array.set([from.x, from.y, from.z, c.group.position.x, c.group.position.y, c.group.position.z]); beam.geometry.attributes.position.needsUpdate = true; }
    }
    if (heldByPad && now - lastHoldSend > 33) {
      lastHoldSend = now;
      const fwd = new THREE.Vector3(); camera.getWorldDirection(fwd);
      const p = camera.position.clone().addScaledVector(fwd, heldDist); p.y -= 0.15;
      api.move(heldByPad, toStage(p));
    }
    updateHologram(t, dt);
    bloom.enabled = !gl3d.lowQuality;
    stars.rotation.y += dt * 0.004;
    composer.render(dt);
  };
}
