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
  const CREATURES = ['auto', 'manta', 'phoenix', 'jellyfish', 'serpent', 'fireflies', 'orb', 'whale', 'dragon', 'owl', 'butterfly', 'octopus', 'koi', 'comet', 'crystal'];
  gl3d.creatureNames = { auto: 'Auto', manta: 'Manta ray', phoenix: 'Phoenix', jellyfish: 'Jellyfish', serpent: 'Serpent', fireflies: 'Firefly swarm', orb: 'Orb guardian', whale: 'Whale', dragon: 'Dragon', owl: 'Owl', butterfly: 'Butterfly', octopus: 'Octopus', koi: 'Koi fish', comet: 'Comet', crystal: 'Crystal' };
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
    } else if (kind === 'whale') {
      const body = new THREE.Mesh(new THREE.SphereGeometry(0.3, 24, 14), mat); body.scale.set(0.8, 0.65, 1.6); g.add(body);
      const belly = new THREE.Mesh(new THREE.SphereGeometry(0.28, 20, 10), glow(0xe2e8f0, 0.5)); belly.scale.set(0.7, 0.4, 1.4); belly.position.y = -0.08; g.add(belly);
      parts.wings = [-1, 1].map(side => { const f = new THREE.Mesh(wingShape(0.28, 0.16), dim); f.rotation.x = -Math.PI / 2; f.scale.x = side; const pv = new THREE.Group(); pv.add(f); pv.position.set(side * 0.2, -0.08, 0.12); g.add(pv); return pv; });
      const fluke = new THREE.Group(); [-1, 1].forEach(side => { const f = new THREE.Mesh(wingShape(0.26, 0.16), dim); f.rotation.x = -Math.PI / 2; f.scale.x = side; fluke.add(f); }); fluke.position.z = -0.55; g.add(fluke); parts.fluke = fluke;
      eyes(g, 0.2, 0.02, 0.32, 0.028);
    } else if (kind === 'dragon') {
      parts.segs = [];
      for (let i = 0; i < 9; i++) { const s = new THREE.Mesh(new THREE.SphereGeometry(0.12 * (1 - i * 0.08), 14, 10), i === 0 ? mat : dim); g.add(s); parts.segs.push(s); }
      const head = parts.segs[0]; [-1, 1].forEach(side => { const h = new THREE.Mesh(new THREE.ConeGeometry(0.02, 0.12, 6), glow(0xffd27a, 1.8)); h.position.set(side * 0.05, 0.1, -0.02); h.rotation.x = -0.5; head.add(h); });
      const e = new THREE.Group(); eyes(e, 0.05, 0.04, 0.08, 0.022); head.add(e);
      parts.wings = [-1, 1].map(side => { const w = new THREE.Mesh(wingShape(0.6, 0.45), dim); w.rotation.x = -Math.PI / 2; w.scale.x = side; const pv = new THREE.Group(); pv.add(w); pv.position.set(side * 0.08, 0.05, -0.15); g.add(pv); return pv; });
    } else if (kind === 'owl') {
      const body = new THREE.Mesh(new THREE.SphereGeometry(0.2, 20, 14), mat); body.scale.set(1, 1.25, 0.9); g.add(body);
      const face = new THREE.Mesh(new THREE.CircleGeometry(0.15, 24), glow(0xf1f5f9, 0.4)); face.position.set(0, 0.08, 0.17); g.add(face);
      [-1, 1].forEach(side => { const ear = new THREE.Mesh(new THREE.ConeGeometry(0.04, 0.1, 6), mat); ear.position.set(side * 0.11, 0.27, 0.02); g.add(ear); });
      parts.wings = [-1, 1].map(side => { const w = new THREE.Mesh(wingShape(0.5, 0.3), dim); w.rotation.x = -Math.PI / 2; w.scale.x = side; const pv = new THREE.Group(); pv.add(w); pv.position.set(side * 0.15, 0.02, 0); g.add(pv); return pv; });
      eyes(g, 0.06, 0.1, 0.18, 0.045);
    } else if (kind === 'butterfly') {
      const body = new THREE.Mesh(new THREE.CapsuleGeometry(0.03, 0.3, 4, 10), mat); body.rotation.x = Math.PI / 2; g.add(body);
      const wingMat = glow(colour, 1.0); wingMat.side = THREE.DoubleSide; wingMat.transparent = true; wingMat.opacity = 0.85;
      parts.wings = [-1, 1].map(side => {
        const pv = new THREE.Group();
        const up = new THREE.Mesh(new THREE.CircleGeometry(0.28, 24), wingMat); up.scale.set(1, 0.75, 1); up.rotation.x = -Math.PI / 2; up.position.set(side * 0.26, 0, 0.08); pv.add(up);
        const lo = new THREE.Mesh(new THREE.CircleGeometry(0.18, 20), wingMat); lo.rotation.x = -Math.PI / 2; lo.position.set(side * 0.18, 0, -0.14); pv.add(lo);
        g.add(pv); return pv; });
      [-1, 1].forEach(side => { const a = new THREE.Mesh(new THREE.CylinderGeometry(0.004, 0.004, 0.18), mat); a.position.set(side * 0.04, 0.06, 0.24); a.rotation.set(0.9, 0, side * -0.3); g.add(a); });
    } else if (kind === 'octopus') {
      const head = new THREE.Mesh(new THREE.SphereGeometry(0.24, 24, 16), mat); head.scale.set(1, 1.2, 1); head.position.y = 0.12; g.add(head); parts.bell = head;
      parts.tentacles = [];
      for (let i = 0; i < 8; i++) {
        const a = i / 8 * Math.PI * 2, n = 16, pos = new Float32Array(n * 3);
        const geo = new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(pos, 3));
        const line = new THREE.Line(geo, new THREE.LineBasicMaterial({ color: colour })); line.userData = { a, n, curl: true }; g.add(line); parts.tentacles.push(line);
      }
      eyes(g, 0.09, 0.12, 0.2, 0.04);
    } else if (kind === 'koi') {
      parts.segs = [];
      for (let i = 0; i < 7; i++) { const s = new THREE.Mesh(new THREE.SphereGeometry(0.13 * (1 - i * 0.11), 16, 10), i % 2 ? glow(0xf8fafc, 0.6) : mat); s.scale.set(0.8, 0.7, 1.2); g.add(s); parts.segs.push(s); }
      const tail = new THREE.Mesh(wingShape(0.18, 0.2), dim); tail.rotation.set(0, Math.PI / 2, 0); parts.segs[6].add(tail); parts.koiTail = tail;
      const e = new THREE.Group(); eyes(e, 0.07, 0.03, 0.08, 0.02); parts.segs[0].add(e);
    } else if (kind === 'comet') {
      const core = new THREE.Mesh(new THREE.IcosahedronGeometry(0.13, 1), mat); g.add(core); parts.cage = core;
      const n = 90, pos = new Float32Array(n * 3), seeds = [];
      for (let i = 0; i < n; i++) seeds.push([Math.random(), (Math.random() - 0.5) * 2, (Math.random() - 0.5) * 2, 0.6 + Math.random()]);
      const pts = new THREE.Points(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(pos, 3)),
        new THREE.PointsMaterial({ color: colour, size: 0.06, transparent: true, opacity: 0.9, blending: THREE.AdditiveBlending, depthWrite: false }));
      g.add(pts); parts.trail = { pts, seeds };
    } else if (kind === 'crystal') {
      const gem = new THREE.Mesh(new THREE.OctahedronGeometry(0.26, 0), mat); gem.scale.set(0.7, 1.3, 0.7); g.add(gem); parts.cage = gem;
      parts.shards = [0, 1, 2, 3, 4].map(i => { const s = new THREE.Mesh(new THREE.OctahedronGeometry(0.06, 0), dim); s.userData.a = i / 5 * Math.PI * 2; g.add(s); return s; });
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
      if (line.userData.curl) { for (let k = 0; k < n; k++) { const d = k / (n - 1), r = 0.18 + d * 0.3, sw = Math.sin(t * 1.6 + a * 2 + d * 3) * 0.35 * d; arr[k * 3] = Math.cos(a + sw) * r; arr[k * 3 + 1] = -d * 0.55 + Math.sin(t * 2 + k * 0.6 + a) * 0.04 + d * d * 0.2; arr[k * 3 + 2] = Math.sin(a + sw) * r; } line.geometry.attributes.position.needsUpdate = true; return; }
      for (let k = 0; k < n; k++) { const d = k / (n - 1); arr[k * 3] = Math.cos(a) * 0.24 * (1 - d * 0.4) + Math.sin(t * 2 + k * 0.5 + a) * 0.05 * d; arr[k * 3 + 1] = -d * 0.85; arr[k * 3 + 2] = Math.sin(a) * 0.24 * (1 - d * 0.4) + Math.cos(t * 1.7 + k * 0.4) * 0.05 * d; }
      line.geometry.attributes.position.needsUpdate = true;
    });
    if (p.segs) p.segs.forEach((s, i) => s.position.set(Math.sin(t * 3 - i * 0.6) * 0.12 * (i / 10 + 0.2), Math.sin(t * 2 - i * 0.4) * 0.03, -i * 0.11));
    if (p.swarm) {
      const arr = p.swarm.pts.geometry.attributes.position.array;
      p.swarm.seeds.forEach(([a, b, r, sp], i) => { const tt = t * sp * (1 + loud); arr[i * 3] = Math.cos(a + tt) * r * Math.cos(b + tt * 0.7); arr[i * 3 + 1] = Math.sin(b + tt * 0.9) * r * 0.7; arr[i * 3 + 2] = Math.sin(a + tt) * r * Math.cos(b + tt * 0.5); });
      p.swarm.pts.geometry.attributes.position.needsUpdate = true;
    }
    if (p.fluke) p.fluke.rotation.x = Math.sin(t * speed * 0.5) * 0.35;
    if (p.koiTail) p.koiTail.rotation.y = Math.PI / 2 + Math.sin(t * speed) * 0.5;
    if (p.trail) {
      const arr = p.trail.pts.geometry.attributes.position.array;
      p.trail.seeds.forEach(([d0, x, y, sp], i) => { const d = (d0 + t * 0.35 * sp * (1 + loud)) % 1; arr[i * 3] = x * d * 0.12; arr[i * 3 + 1] = y * d * 0.12; arr[i * 3 + 2] = -d * (0.6 + 0.8 * loud); });
      p.trail.pts.geometry.attributes.position.needsUpdate = true;
    }
    if (p.shards) p.shards.forEach(s => { const a = s.userData.a + t * (0.6 + loud); s.position.set(Math.cos(a) * 0.38, Math.sin(t * 1.5 + s.userData.a) * 0.12, Math.sin(a) * 0.38); s.rotation.y += dt * 2; });
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
    } else if (gl3d.globe) {
      const cam = api.cam, pitch = 0.3, R = 2.85 / Math.max(0.6, Math.min(1.6, cam.zoom)), cp = Math.cos(pitch);
      camera.position.set(-R * Math.sin(cam.yaw) * cp, R * Math.sin(pitch), -R * Math.cos(cam.yaw) * cp); camera.lookAt(0, 0, 0);
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
  gl3d.pick = (mx, my) => { if (gl3d.globe) return null; raycaster.setFromCamera(ndc(mx, my), camera); return pick(raycaster); };
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
    if (gl3d.globe) {
      ctx.font = "600 11px PlateFont, 'Arial Narrow', sans-serif"; ctx.textAlign = 'center';
      const placed = [];
      [...gNodes.values()].map(n => ({ n, p: project(n.pos.clone().applyMatrix4(globe.matrixWorld), W, H) })).filter(e => e.p).sort((a, b) => a.p.y - b.p.y).forEach(({ n, p }) => {
        const o = n.obj, txt = (o.name || o.module || '').toUpperCase().slice(0, 18), w = ctx.measureText(txt).width + 6; let y = p.y - 12;
        for (let i = 0; i < 8; i++) { const hit = placed.find(r => Math.abs(r.x - p.x) < (r.w + w) / 2 && Math.abs(r.y - y) < 12); if (!hit) break; y = hit.y - 13; }
        placed.push({ x: p.x, y, w });
        if (Math.abs(y - (p.y - 12)) > 2) { ctx.strokeStyle = 'rgba(255,210,150,0.35)'; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(p.x, p.y - 4); ctx.lineTo(p.x, y + 2); ctx.stroke(); }
        ctx.fillStyle = 'rgba(0,0,0,0.6)'; ctx.fillText(txt, p.x + 1, y + 1); ctx.fillStyle = o.self ? '#FFE2A8' : 'rgba(190,230,255,0.95)'; ctx.fillText(txt, p.x, y);
      });
      ctx.textAlign = 'left'; gl3d.clashHits = []; return;
    }
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
  // ---------- EQ ribbon: the module's real response curve (measured live), over the live spectrum ----------
  const RIB = { cube: { w: 2.2, h: 0.09, y: -1.25, z: 0.75 }, ring: { w: 1.4, h: 0.05, y: 0, z: 0.05 }, dome: { w: 2.6, h: 0.1, y: -1.4, z: 0 }, spectral: { w: 8, h: 0.12, y: 0, z: -0.4 } }[kind];
  const RN = 64, ribbon = new THREE.Group(); ribbon.position.set(0, RIB.y, RIB.z); holo.add(ribbon);
  const mkLine = (n, mat) => { const l = new THREE.Line(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(new Float32Array(n * 3), 3)), mat); ribbon.add(l); return l; };
  const curveMatL = new THREE.LineBasicMaterial({ color: 0xffd27a, transparent: true, opacity: 1, blending: THREE.AdditiveBlending, depthWrite: false });
  const curveMatR = new THREE.LineBasicMaterial({ color: 0x7dd3fc, transparent: true, opacity: 0.7, blending: THREE.AdditiveBlending, depthWrite: false });
  const zeroMat = new THREE.LineBasicMaterial({ color: 0x94a3b8, transparent: true, opacity: 0.25, depthWrite: false });
  const specMat = new THREE.LineBasicMaterial({ color: 0x38bdf8, transparent: true, opacity: 0.35, blending: THREE.AdditiveBlending, depthWrite: false });
  const curveL = mkLine(RN, curveMatL), curveR = mkLine(RN, curveMatR), specLine = mkLine(RN, specMat);
  const zero = mkLine(2, zeroMat); zero.geometry.attributes.position.array.set([-RIB.w / 2, 0, 0, RIB.w / 2, 0, 0]);
  const fillGeo = new THREE.BufferGeometry(); fillGeo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(RN * 2 * 3), 3));
  const idx = []; for (let i = 0; i < RN - 1; i++) { const a = i * 2; idx.push(a, a + 1, a + 2, a + 1, a + 3, a + 2); } fillGeo.setIndex(idx);
  const fillMat = new THREE.MeshBasicMaterial({ color: 0xff8a1c, transparent: true, opacity: 0.18, side: THREE.DoubleSide, blending: THREE.AdditiveBlending, depthWrite: false });
  ribbon.add(new THREE.Mesh(fillGeo, fillMat));
  const shownL = new Float32Array(RN), shownR = new Float32Array(RN);
  // the camera looks toward +z, so screen-left is +x: lows on the left
  const ribX = (i) => RIB.w / 2 - RIB.w * i / (RN - 1), ribY = (db) => Math.max(-14, Math.min(14, db)) * RIB.h;
  function updateRibbon(dt) {
    const c = window._respCurve, k = 1 - Math.exp(-dt / 0.12);              // glide to each new measurement
    const L = curveL.geometry.attributes.position.array, R = curveR.geometry.attributes.position.array, F = fillGeo.attributes.position.array;
    let diff = 0;
    for (let i = 0; i < RN; i++) {
      const tl = c && c.l ? (c.l[Math.round(i * (c.l.length - 1) / (RN - 1))] || 0) : 0, tr = c && c.r ? (c.r[Math.round(i * (c.r.length - 1) / (RN - 1))] || 0) : 0;
      shownL[i] += (tl - shownL[i]) * k; shownR[i] += (tr - shownR[i]) * k; diff = Math.max(diff, Math.abs(shownL[i] - shownR[i]));
      L.set([ribX(i), ribY(shownL[i]), 0.01], i * 3); R.set([ribX(i), ribY(shownR[i]), -0.01], i * 3);
      F.set([ribX(i), 0, 0, ribX(i), ribY(shownL[i]), 0], i * 6);
    }
    curveR.visible = diff > 0.3;                                         // right curve only when L and R really differ
    curveL.geometry.attributes.position.needsUpdate = true; curveR.geometry.attributes.position.needsUpdate = true; fillGeo.attributes.position.needsUpdate = true;
    const sp = (window._scope || {}).spec;
    if (sp && sp.length) {
      const S = specLine.geometry.attributes.position.array;
      for (let i = 0; i < RN; i++) { const db = sp[Math.floor(i * (sp.length - 1) / (RN - 1))] ?? -90; S.set([ribX(i), Math.max(-14, (db + 60) / 4) * RIB.h, -0.02], i * 3); }
      specLine.geometry.attributes.position.needsUpdate = true;
    }
  }
  gl3d.ribbon = () => ({ l: Array.from(shownL), visibleR: curveR.visible });
  function updateHologram(t, dt) {
    updateRibbon(dt); ribbon.rotation.y = -holo.rotation.y;          // always faces you, whatever the hologram does
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
    holoMat.color.setHex(a); holoMat2.color.setHex(b); fillMat.color.setHex(a); specMat.color.setHex(b); goldRingMat.color.setHex(a); ringMat.color.setHex(b);
    bars.forEach(x => x.material.color.setHex(a)); if (dome) dome.material.color.setHex(b);
  };
  gl3d.setTheme(document.documentElement.getAttribute('data-theme') || '');
  // Your head in the 3D view turns with the real one (head tracking).
  gl3d.setHead = (yawDeg, pitchDeg) => { you.rotation.y = -yawDeg * Math.PI / 180; head.rotation.x = -pitchDeg * Math.PI / 180 * 0.6; };

  // ---------- holographic globe (the faceplate's glass window): wireframe world, orbit rings, live waveform, tracks as lights ----------
  const AB = THREE.AdditiveBlending;
  const globe = new THREE.Group(); globe.visible = false; scene.add(globe);
  const gCol = new THREE.Color(0xff9a2e), gCol2 = new THREE.Color(0xffd9a0);
  const gMats = [], gMats2 = [];
  const gLine = (op, col = gCol) => { const m = new THREE.LineBasicMaterial({ color: col, transparent: true, opacity: op, blending: AB, depthWrite: false, fog: false }); (col === gCol ? gMats : col === gCol2 ? gMats2 : []).push(m); return m; };
  const merMat = gLine(0.3), parMat = gLine(0.34), orbMat = gLine(0.5), netMat = gLine(0.035), waveMat = gLine(0.85, gCol2), scanMat = gLine(0.7, gCol2);
  const ring = (n, f) => { const pts = []; for (let i = 0; i <= n; i++) pts.push(f(i / n * Math.PI * 2)); return new THREE.BufferGeometry().setFromPoints(pts); };
  for (let m = 0; m < 8; m++) { const l = new THREE.Line(ring(96, t => new THREE.Vector3(Math.sin(t), Math.cos(t), 0)), merMat); l.rotation.y = m / 8 * Math.PI; globe.add(l); }
  const parallels = [];
  for (let k = -3; k <= 3; k++) {
    const lat = k * 22.5 * Math.PI / 180, l = new THREE.Line(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(new Float32Array(97 * 3), 3)), parMat);
    l.userData.lat = lat; globe.add(l); parallels.push(l);
  }
  const net = new THREE.LineSegments(new THREE.WireframeGeometry(new THREE.IcosahedronGeometry(1.002, 2)), netMat); globe.add(net);
  const shell = new THREE.Mesh(new THREE.SphereGeometry(1, 48, 32), new THREE.ShaderMaterial({
    uniforms: { c: { value: gCol }, k: { value: 1 } }, transparent: true, depthWrite: false, blending: AB,
    vertexShader: 'varying vec3 n; varying vec3 v; void main(){ vec4 mv = modelViewMatrix * vec4(position, 1.0); n = normalize(normalMatrix * normal); v = normalize(-mv.xyz); gl_Position = projectionMatrix * mv; }',
    fragmentShader: 'uniform vec3 c; uniform float k; varying vec3 n; varying vec3 v; void main(){ float f = pow(1.0 - abs(dot(n, v)), 2.6); gl_FragColor = vec4(c * f * 0.9 * k, f * 0.55 * k); }' }));
  globe.add(shell);
  // equator waveform (the live signal wrapped round the world) + a scanning ring
  const GW = 192, gWave = new THREE.Line(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(new Float32Array((GW + 1) * 3), 3)), waveMat); globe.add(gWave);
  const scan = new THREE.Line(ring(96, t => new THREE.Vector3(Math.cos(t), 0, Math.sin(t))), scanMat); globe.add(scan);
  // orbit rings with travelling sparks
  const orbits = [[1.22, 0.5, 0.3], [1.12, -0.8, 1.2], [1.3, 1.2, 2.1]].map(([r, tilt, spin], i) => {
    const g = new THREE.Group(); g.rotation.set(tilt, spin, 0.2 * i);
    g.add(new THREE.Line(ring(128, t => new THREE.Vector3(Math.cos(t) * r, 0, Math.sin(t) * r * 0.92)), orbMat));
    const sparks = [0, 1, 2].map(j => { const m = new THREE.Mesh(new THREE.SphereGeometry(0.022, 10, 8), new THREE.MeshBasicMaterial({ color: gCol2, transparent: true, blending: AB, depthWrite: false })); m.userData = { r, ph: j * 2.1 + i }; g.add(m); return m; });
    globe.add(g); return { g, sparks, speed: 0.35 + i * 0.17 };
  });
  // surface sparkles (twinkle) and the core
  const spk = (n) => { const a = new Float32Array(n * 3); for (let i = 0; i < n; i++) { const v = new THREE.Vector3().randomDirection(); a.set([v.x, v.y, v.z], i * 3); }
    return new THREE.Points(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(a, 3)), new THREE.PointsMaterial({ color: gCol2, size: 0.018, transparent: true, opacity: 0.6, blending: AB, depthWrite: false, fog: false })); };
  const sparkA = spk(220), sparkB = spk(220); globe.add(sparkA, sparkB);
  const core = new THREE.Mesh(new THREE.SphereGeometry(0.045, 16, 12), new THREE.MeshBasicMaterial({ color: 0xfff1d0 })); globe.add(core);
  const coreGlow = new THREE.Mesh(new THREE.SphereGeometry(0.07, 16, 12), new THREE.MeshBasicMaterial({ color: gCol, transparent: true, opacity: 0.28, blending: AB, depthWrite: false })); globe.add(coreGlow);
  // tracks: a light at each track's direction (radius = distance), tethered to the centre (you)
  const glowTex = (() => { const c = document.createElement('canvas'); c.width = c.height = 64; const x = c.getContext('2d'), g = x.createRadialGradient(32, 32, 0, 32, 32, 32);
    g.addColorStop(0, 'rgba(255,255,255,1)'); g.addColorStop(0.25, 'rgba(255,255,255,0.45)'); g.addColorStop(1, 'rgba(255,255,255,0)'); x.fillStyle = g; x.fillRect(0, 0, 64, 64); return new THREE.CanvasTexture(c); })();
  const gNodes = new Map();
  const nodePos = (o) => { const a = (o.az || 0) * Math.PI / 180, e = (o.el || 0) * Math.PI / 180, d = Math.max(0.5, o.dist || 2);
    const r = 0.3 + 0.62 * Math.min(1, Math.log(d / 0.5) / Math.log(64)); return new THREE.Vector3(r * Math.cos(e) * Math.sin(a), r * Math.sin(e), r * Math.cos(e) * Math.cos(a)); };
  function syncGlobeNodes(objs, t) {
    const seen = new Set();
    objs.forEach(o => {
      const key = o.self ? 'self' : 's' + o.slot; seen.add(key);
      let n = gNodes.get(key);
      if (!n) {
        const col = o.self ? gCol2 : new THREE.Color(0x7dd3fc);
        n = { dot: new THREE.Mesh(new THREE.SphereGeometry(o.self ? 0.026 : 0.02, 12, 8), new THREE.MeshBasicMaterial({ color: col })),
              halo: new THREE.Sprite(new THREE.SpriteMaterial({ map: glowTex, color: col, transparent: true, opacity: 0.7, blending: AB, depthWrite: false })),
              tether: new THREE.Line(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(new Float32Array(6), 3)), gLine(0.45, col)) };
        globe.add(n.dot, n.halo, n.tether); gNodes.set(key, n);
      }
      const p = nodePos(o); n.obj = o; n.pos = p;
      n.dot.position.copy(p); n.halo.position.copy(p);
      const loud = Math.max(0, Math.min(1, ((o.lufs ?? -40) + 50) / 44));
      n.halo.scale.setScalar((o.self ? 0.16 : 0.12) * (1 + 0.7 * loud + 0.1 * Math.sin(t * 3 + (o.slot || 0))));
      n.tether.geometry.attributes.position.array.set([0, 0, 0, p.x, p.y, p.z]); n.tether.geometry.attributes.position.needsUpdate = true;
    });
    for (const [k, n] of gNodes) if (!seen.has(k)) { globe.remove(n.dot, n.halo, n.tether); gNodes.delete(k); }
  }
  function updateGlobe(t, dt, objs) {
    const sc = window._scope || {}, spec = sc.spec || [];
    let level = 0; if (spec.length) { for (const db of spec) level += Math.max(0, (db + 70) / 60); level /= spec.length; }
    globe.rotation.y = Math.sin(t * 0.12) * 0.35;                           // a slow sway; front stays roughly front
    parallels.forEach((l, i) => {
      const a = l.geometry.attributes.position.array, lat = l.userData.lat, db = spec.length ? spec[Math.min(spec.length - 1, Math.round(i / (parallels.length - 1) * (spec.length - 1)))] ?? -90 : -90;
      const bump = 1 + 0.07 * Math.max(0, Math.min(1, (db + 60) / 50)), cr = Math.cos(lat) * bump, y = Math.sin(lat);
      for (let k = 0; k <= 96; k++) { const th = k / 96 * Math.PI * 2; a.set([Math.cos(th) * cr, y, Math.sin(th) * cr], k * 3); }
      l.geometry.attributes.position.needsUpdate = true;
    });
    const w = gWave.geometry.attributes.position.array, lr = sc.lr || [], step = Math.max(1, Math.floor(lr.length / 2 / GW));
    for (let k = 0; k <= GW; k++) {
      const th = k / GW * Math.PI * 2, i0 = (k % GW) * step; let v = 0;
      if (lr.length) { for (let j = 0; j < step; j++) v += lr[(i0 + j) * 2] || 0; v /= step; } else v = Math.sin(th * 9 + t * 2) * 0.02;
      const taper = 0.55 + 0.45 * Math.cos(th * 2), r = 1.04 + Math.max(-0.1, Math.min(0.1, v * 0.3)) * taper;
      w.set([Math.cos(th) * r, Math.max(-0.12, Math.min(0.12, v * 0.35)) * taper, Math.sin(th) * r], k * 3); }
    gWave.geometry.attributes.position.needsUpdate = true;
    const sy = Math.sin(t * 0.7); scan.position.y = sy; scan.scale.setScalar(Math.sqrt(Math.max(0, 1 - sy * sy)) * 1.004); scanMat.opacity = 0.25 + 0.4 * (1 - Math.abs(sy));
    orbits.forEach(o => { o.g.rotation.y += dt * 0.05; o.sparks.forEach(m => { const a = t * o.speed + m.userData.ph; m.position.set(Math.cos(a) * m.userData.r, 0, Math.sin(a) * m.userData.r * 0.92); }); });
    sparkA.material.opacity = 0.25 + 0.35 * (0.5 + 0.5 * Math.sin(t * 2.3)); sparkB.material.opacity = 0.25 + 0.35 * (0.5 + 0.5 * Math.sin(t * 2.3 + Math.PI));
    shell.material.uniforms.k.value = 0.8 + 0.6 * level; if (gl3d.globeStyle !== 'glass') merMat.opacity = 0.26 + 0.25 * level;
    coreGlow.scale.setScalar(1 + 0.6 * level + 0.08 * Math.sin(t * 4));
    syncGlobeNodes(objs, t);
    if (gl3d.globeStyle === 'glass') updateGlass(t, dt, objs);
    applyGlobeStyle();
  }
  // ---- 'glass' style (the Abyssal look): a water-filled glass sphere, flowing waveform ribbons, creatures swimming inside ----
  const glassParts = new THREE.Group(); globe.add(glassParts);
  const water = new THREE.Mesh(new THREE.SphereGeometry(0.985, 48, 32), new THREE.ShaderMaterial({
    uniforms: { c: { value: gCol }, t: { value: 0 } }, transparent: true, depthWrite: false, side: THREE.BackSide,
    vertexShader: 'varying vec3 p; void main(){ p = position; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }',
    fragmentShader: 'uniform vec3 c; uniform float t; varying vec3 p; void main(){ float h = clamp(p.y * 0.5 + 0.5, 0.0, 1.0); float caust = 0.5 + 0.5 * sin(p.x * 11.0 + t * 1.3) * sin(p.z * 9.0 - t * 1.1); vec3 col = mix(c * 0.02, c * 0.2, pow(h, 2.2)) + c * 0.08 * caust * h * h; gl_FragColor = vec4(col, 0.4); }' }));
  glassParts.add(water);
  const rays = [-0.35, 0, 0.3].map((x, i) => { const g = new THREE.ConeGeometry(0.22 + i * 0.05, 1.7, 24, 1, true); g.translate(0, -0.85, 0);
    const m = new THREE.Mesh(g, new THREE.MeshBasicMaterial({ color: gCol2, transparent: true, opacity: 0.05, blending: AB, depthWrite: false, side: THREE.DoubleSide }));
    m.position.set(x, 0.98, 0.1 * i); m.rotation.z = x * 0.6; glassParts.add(m); return m; });
  const RB = 96, ribbons = [0, 1, 2, 3, 4].map(k => { const l = new THREE.Line(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(new Float32Array((RB + 1) * 3), 3)), gLine(k ? 0.45 - k * 0.07 : 1, k ? gCol : gCol2)); glassParts.add(l); return l; });
  const waveHist = [];
  const NB = 70, bubPos = new Float32Array(NB * 3), bubSeed = [];
  for (let i = 0; i < NB; i++) { const a = Math.random() * 6.28, r = Math.sqrt(Math.random()) * 0.75; bubSeed.push([Math.cos(a) * r, Math.sin(a) * r, Math.random() * 2 - 1, 0.08 + Math.random() * 0.12]); }
  const bubbles = new THREE.Points(new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(bubPos, 3)), new THREE.PointsMaterial({ color: gCol2, size: 0.02, transparent: true, opacity: 0.55, blending: AB, depthWrite: false, fog: false }));
  glassParts.add(bubbles);
  const swimmers = new Map();
  function updateGlass(t, dt, objs) {
    water.material.uniforms.t.value = t;
    rays.forEach((r, i) => r.material.opacity = 0.016 + 0.012 * Math.sin(t * 0.6 + i * 2));
    const lr = (window._scope || {}).lr || [], cur = new Float32Array(RB + 1), step = Math.max(1, Math.floor(lr.length / 2 / RB));
    for (let k = 0; k <= RB; k++) { let v = 0; if (lr.length) { const i0 = (k % RB) * step; for (let j = 0; j < step; j++) { const x = lr[(i0 + j) * 2] || 0; if (Math.abs(x) > Math.abs(v)) v = x; } } else v = 0.04 * Math.sin(k / RB * 6.28 * 5 + t * 2); cur[k] = v; }
    const sm = new Float32Array(RB + 1);
    for (let pass = 0; pass < 1; pass++) { for (let k = 0; k <= RB; k++) { let a = 0; for (let j = -2; j <= 2; j++) a += cur[(k + j + RB) % RB]; sm[k] = a / 5; } cur.set(sm); }
    for (let k = 0; k <= RB; k++) cur[k] = Math.tanh(cur[k] * 3) * 0.26;
    cur[RB] = cur[0];
    waveHist.unshift(cur); if (waveHist.length > 24) waveHist.pop();
    ribbons.forEach((l, k) => {
      const src = waveHist[Math.min(waveHist.length - 1, k * 4)], a = l.geometry.attributes.position.array, off = k * 0.035 * (k % 2 ? 1 : -1);
      for (let i = 0; i <= RB; i++) { const th = i / RB * Math.PI * 2 + t * 0.15 + k * 0.08, v = src[i] * (1 - k * 0.12), r = 0.97 - Math.abs(v) * 0.25 + k * 0.012;
        a.set([Math.cos(th) * r, v + off, Math.sin(th) * r], i * 3); }
      l.geometry.attributes.position.needsUpdate = true;
    });
    for (let i = 0; i < NB; i++) { const b = bubSeed[i]; b[2] += dt * b[3]; if (b[2] > 0.95) b[2] = -0.95; const lim = Math.sqrt(Math.max(0, 0.9 - b[2] * b[2])); bubPos.set([b[0] * lim + Math.sin(t + i) * 0.01, b[2], b[1] * lim], i * 3); }
    bubbles.geometry.attributes.position.needsUpdate = true;
    // the tracks as little creatures swimming in the sphere (same creature each track has on the big stage)
    const seen = new Set();
    objs.forEach(o => {
      const key = o.self ? 'self' : 's' + o.slot; seen.add(key);
      const kind = (o.creature > 0 && CREATURES[o.creature]) ? CREATURES[o.creature] : autoKind(o), col = colourFor(o);
      let c = swimmers.get(key);
      if (!c || c.kind !== kind) { if (c) glassParts.remove(c.group); const b = build(kind, col); c = { group: b.group, parts: b.parts, kind, pos: null }; glassParts.add(c.group); swimmers.set(key, c); }
      c.obj = o; animateCreature(c, t, dt);
      const p = nodePos(o).multiplyScalar(0.92); p.y += Math.sin(t * 1.1 + (o.slot || 0)) * 0.03;
      c.group.position.copy(p); c.group.scale.setScalar((o.self ? 0.3 : 0.24) * (0.85 + 0.3 * Math.max(0, Math.min(1, ((o.lufs ?? -40) + 50) / 44))));
      c.group.rotation.y = t * 0.4 + (o.slot || 0);
    });
    for (const [k, c] of swimmers) if (!seen.has(k)) { glassParts.remove(c.group); swimmers.delete(k); }
  }
  gl3d.globeStyle = 'glass';
  function applyGlobeStyle() {
    const glass = gl3d.globeStyle === 'glass';
    glassParts.visible = glass; net.visible = scan.visible = gWave.visible = !glass; orbits.forEach(o => o.g.visible = !glass);
    merMat.opacity = glass ? 0.07 : 0.3; parMat.opacity = glass ? 0.06 : 0.34; sparkA.visible = sparkB.visible = !glass;
    for (const n of gNodes.values()) n.dot.visible = n.tether.visible = !glass;
  }
  gl3d.setGlobeStyle = (st) => { gl3d.globeStyle = st === 'wire' ? 'wire' : 'glass'; applyGlobeStyle(); };
  const bloomNormal = [bloom.strength, bloom.radius, bloom.threshold];
  gl3d.globe = false;
  gl3d.setGlobe = (on) => {
    gl3d.globe = !!on; globe.visible = gl3d.globe;
    [env, stars, you, holo, roomLines, pathLine].forEach(o => o.visible = !gl3d.globe);
    if (gl3d.globe) { bloom.strength = 1.15; bloom.radius = 0.55; bloom.threshold = 0.05; scene.fog = null; } else { [bloom.strength, bloom.radius, bloom.threshold] = bloomNormal; scene.fog = new THREE.FogExp2(0x05070b, 0.045); }
  };
  gl3d.setGlobeColour = (hex, hex2) => {
    gCol.setHex(hex); gCol2.set(hex2 !== undefined ? hex2 : new THREE.Color(hex).lerp(new THREE.Color(0xffffff), 0.55));
    gMats.forEach(m => m.color.copy(gCol)); gMats2.forEach(m => m.color.copy(gCol2));
    [bubbles, sparkA, sparkB].forEach(o => o.material.color.copy(gCol2)); rays.forEach(r => r.material.color.copy(gCol2)); coreGlow.material.color.copy(gCol);
    orbits.forEach(o => o.sparks.forEach(m => m.material.color.copy(gCol2)));
  };
  if (document.documentElement.classList.contains('plate-on')) { gl3d.setGlobe(true); window.applyGlobeLook && window.applyGlobeLook(); }      // faceplate already up before this script loaded

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
    for (const c of creatures.values()) { animateCreature(c, t, dt); c.group.visible = c.parts.shadow.visible = c.parts.heightLine.visible = !gl3d.globe; }
    if (gl3d.globe) updateGlobe(t, dt, objs);
    clashes = gl3d.globe ? [] : api.findClashes(objs);

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
    if (renderer.xr.isPresenting) {
      // In the headset: render straight (post-processing doesn't run per eye) and let your real head drive the binaural mix.
      renderer.render(scene, camera);
      const xrCam = renderer.xr.getCamera(), e = new THREE.Euler().setFromQuaternion(xrCam.quaternion, 'YXZ');
      if (now - lastXrPose > 33) { lastXrPose = now; gl3d.onXrPose && gl3d.onXrPose(-e.y * 180 / Math.PI, e.x * 180 / Math.PI); }
    } else composer.render(dt);
  };
  // ---------- VR headset (WebXR -> the system's OpenXR runtime) ----------
  let lastXrPose = 0, xrSession = null;
  gl3d.xrSupported = async () => { try { return !!(navigator.xr && await navigator.xr.isSessionSupported('immersive-vr')); } catch (e) { return false; } };
  gl3d.enterVr = async () => {
    if (xrSession) { await xrSession.end(); return false; }
    xrSession = await navigator.xr.requestSession('immersive-vr', { optionalFeatures: ['local-floor', 'bounded-floor'] });
    renderer.xr.enabled = true; renderer.xr.setReferenceSpaceType('local-floor');
    await renderer.xr.setSession(xrSession);
    camera.position.set(0, 1.6, 4.5);                                 // standing at the listening spot, looking at the stage
    renderer.setAnimationLoop(() => gl3d.render());
    xrSession.addEventListener('end', () => { xrSession = null; renderer.setAnimationLoop(null); renderer.xr.enabled = false; gl3d.onXrEnd && gl3d.onXrEnd(); });
    return true;
  };
}
