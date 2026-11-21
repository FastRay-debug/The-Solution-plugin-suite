// The Solution - Billet Audio's robotic hand for the Diagnostic Utility (three.js).
// window.roboHand.grab(fromX, fromY, toX, toY) reaches to a screen point, grips, and carries to another.
import * as THREE from 'three';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';

const cv = document.getElementById('roboHandCanvas');
let api = { show() {}, grab() {}, release() {} };
if (cv && ! window.SAFE_MODE) { try { api = setup(); } catch (e) { /* no WebGL: the panel works without the hand */ } }
window.roboHand = api;

function setup() {
  const renderer = new THREE.WebGLRenderer({ canvas: cv, alpha: true, antialias: true });
  renderer.setClearColor(0x000000, 0);
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  const scene = new THREE.Scene();
  const pmrem = new THREE.PMREMGenerator(renderer);
  scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
  const camera = new THREE.PerspectiveCamera(35, 1.6, 0.1, 100); camera.position.set(0, 0, 10); camera.lookAt(0, 0, 0);
  scene.add(new THREE.HemisphereLight(0xbcd2ff, 0x10131a, 0.6));
  const key = new THREE.DirectionalLight(0xffffff, 1.6); key.position.set(-3, 5, 8); scene.add(key);
  const warm = new THREE.PointLight(0xff8a1c, 30, 14); warm.position.set(-2, -1, 3); scene.add(warm);
  const cool = new THREE.PointLight(0x38bdf8, 18, 14); cool.position.set(3, 2, 2); scene.add(cool);

  const metal = new THREE.MeshStandardMaterial({ color: 0xc3c9d2, metalness: 0.95, roughness: 0.22 });
  const satin = new THREE.MeshStandardMaterial({ color: 0x8d96a3, metalness: 0.85, roughness: 0.38 });
  const carbon = new THREE.MeshStandardMaterial({ color: 0x1b1f26, metalness: 0.55, roughness: 0.42 });
  const ledG = new THREE.MeshStandardMaterial({ color: 0x22ff88, emissive: 0x22ff88, emissiveIntensity: 2.2 });
  const ledO = new THREE.MeshStandardMaterial({ color: 0xff8a1c, emissive: 0xff8a1c, emissiveIntensity: 2.4 });
  const ledR = new THREE.MeshStandardMaterial({ color: 0xff3030, emissive: 0xff3030, emissiveIntensity: 2.4 });

  const root = new THREE.Group(); scene.add(root);          // origin = the point between the fingertips
  const hand = new THREE.Group(); hand.position.set(0, -0.95, -0.3); root.add(hand);
  // palm (fingers point +Y, palm faces +Z)
  const palm = new THREE.Mesh(new THREE.BoxGeometry(1.05, 1.0, 0.3), satin); hand.add(palm);
  const back = new THREE.Mesh(new THREE.BoxGeometry(0.95, 0.85, 0.08), carbon); back.position.z = -0.18; hand.add(back);
  const palmPad = new THREE.Mesh(new THREE.CylinderGeometry(0.22, 0.22, 0.05, 24), carbon); palmPad.rotation.x = Math.PI / 2; palmPad.position.set(0, 0, 0.16); hand.add(palmPad);
  const palmLed = new THREE.Mesh(new THREE.SphereGeometry(0.07, 16, 12), ledR); palmLed.position.set(0, 0, 0.2); hand.add(palmLed);
  const fingers = [];
  [[-0.38, 0.92], [-0.13, 1.0], [0.13, 0.97], [0.38, 0.85]].forEach(([x, len], fi) => {
    let parent = new THREE.Group(); parent.position.set(x, 0.5, 0.02); hand.add(parent);
    const joints = [];
    [0.42, 0.3, 0.24].forEach((L0, si) => {
      const L = L0 * len;
      const knuckle = new THREE.Mesh(new THREE.SphereGeometry(0.1 - si * 0.012, 18, 12), metal); parent.add(knuckle);
      if (si === 0) { const led = new THREE.Mesh(new THREE.SphereGeometry(0.035, 10, 8), fi % 2 ? ledO : ledG); led.position.set(0, 0, 0.1); parent.add(led); }
      const seg = new THREE.Mesh(new THREE.CylinderGeometry(0.075 - si * 0.01, 0.085 - si * 0.01, L, 14), si === 1 ? carbon : satin);
      seg.position.y = L / 2; parent.add(seg);
      const plate = new THREE.Mesh(new THREE.BoxGeometry(0.1, L * 0.7, 0.03), metal); plate.position.set(0, L / 2, -0.08); parent.add(plate);
      joints.push(parent);
      const next = new THREE.Group(); next.position.y = L; parent.add(next); parent = next;
    });
    const tip = new THREE.Mesh(new THREE.SphereGeometry(0.06, 12, 10), metal); parent.add(tip);
    fingers.push(joints);
  });
  const thumb = new THREE.Group(); thumb.position.set(-0.55, -0.15, 0.08); thumb.rotation.z = 0.9; hand.add(thumb);
  const thumbJoints = [];
  { let parent = thumb; [0.4, 0.3].forEach((L, si) => {
      parent.add(new THREE.Mesh(new THREE.SphereGeometry(0.11, 16, 12), metal));
      const seg = new THREE.Mesh(new THREE.CylinderGeometry(0.08, 0.09, L, 14), si ? satin : carbon); seg.position.y = L / 2; parent.add(seg);
      thumbJoints.push(parent); const next = new THREE.Group(); next.position.y = L; parent.add(next); parent = next; });
    parent.add(new THREE.Mesh(new THREE.SphereGeometry(0.065, 12, 10), metal)); }
  // wrist + forearm with the data-link display
  const wrist = new THREE.Mesh(new THREE.CylinderGeometry(0.42, 0.46, 0.5, 28), metal); wrist.position.y = -0.72; hand.add(wrist);
  const forearm = new THREE.Mesh(new THREE.CylinderGeometry(0.5, 0.62, 7, 28), carbon); forearm.position.y = -4.4; hand.add(forearm);
  [-1.4, -2.6].forEach(y => { const ring = new THREE.Mesh(new THREE.TorusGeometry(0.55, 0.05, 10, 32), metal); ring.rotation.x = Math.PI / 2; ring.position.y = y; hand.add(ring);
    const led = new THREE.Mesh(new THREE.SphereGeometry(0.06, 10, 8), y > -2 ? ledG : ledR); led.position.set(0.3, y, 0.48); hand.add(led); });
  const disp = document.createElement('canvas'); disp.width = 256; disp.height = 128; const dctx = disp.getContext('2d');
  const dtex = new THREE.CanvasTexture(disp);
  const screen = new THREE.Mesh(new THREE.PlaneGeometry(0.95, 0.48), new THREE.MeshBasicMaterial({ map: dtex, transparent: true }));
  screen.position.set(0.05, -1.95, 0.6); screen.rotation.x = -0.15; hand.add(screen);
  const screenFrame = new THREE.Mesh(new THREE.BoxGeometry(1.05, 0.58, 0.06), metal); screenFrame.position.set(0.05, -1.95, 0.55); screenFrame.rotation.x = -0.15; hand.add(screenFrame);
  // what the hand carries: a glowing processor cube
  const cube = new THREE.Mesh(new THREE.BoxGeometry(0.62, 0.62, 0.62), new THREE.MeshStandardMaterial({ color: 0xff9d2e, emissive: 0xff7a18, emissiveIntensity: 1.1, transparent: true, opacity: 0.78, metalness: 0.2, roughness: 0.15 }));
  const cubeEdges = new THREE.LineSegments(new THREE.EdgesGeometry(cube.geometry), new THREE.LineBasicMaterial({ color: 0xffe2b0 })); cube.add(cubeEdges);
  cube.position.set(0, 0, 0.25); root.add(cube); cube.visible = false;
  root.scale.setScalar(0.78);

  // ---- motion ----
  let visible = false, raf = 0, last = performance.now(), grip = 0, gripTarget = 0, seq = [], pos = new THREE.Vector3(), target = new THREE.Vector3(), held = false;
  function toWorld(x, y) {
    const r = cv.getBoundingClientRect(), ndc = new THREE.Vector3((x - r.left) / r.width * 2 - 1, -((y - r.top) / r.height) * 2 + 1, 0.5).unproject(camera);
    const dir = ndc.sub(camera.position).normalize(), t = -camera.position.z / dir.z;
    return camera.position.clone().addScaledVector(dir, t);
  }
  function restPoint() { const r = cv.getBoundingClientRect(); return toWorld(r.right - r.width * 0.055, r.bottom - r.height * 0.16); }
  function drawDisplay(t) {
    dctx.clearRect(0, 0, 256, 128); dctx.fillStyle = 'rgba(4,20,28,0.92)'; dctx.fillRect(0, 0, 256, 128);
    dctx.strokeStyle = '#38e1ff'; dctx.lineWidth = 3; dctx.strokeRect(3, 3, 250, 122);
    dctx.fillStyle = '#7df3ff'; dctx.font = 'bold 21px monospace'; dctx.fillText('BILLET AUDIO', 14, 34);
    dctx.font = 'bold 16px monospace'; dctx.fillStyle = held ? '#ffb35c' : '#38e1ff'; dctx.fillText(held ? 'DATA-LINK: LOCKED' : 'DATA-LINK: READY', 14, 58);
    for (let i = 0; i < 14; i++) { const h = 8 + 30 * Math.abs(Math.sin(t * 3 + i * 0.7)); dctx.fillStyle = i % 4 ? '#22d3ee' : '#4ade80'; dctx.fillRect(14 + i * 16, 116 - h, 11, h); }
    dtex.needsUpdate = true;
  }
  function frame(now) {
    if (!visible) { raf = 0; return; }
    const dt = Math.min(0.05, (now - last) / 1000), t = now / 1000; last = now;
    const W = cv.clientWidth, H = cv.clientHeight, dpr = Math.min(2, window.devicePixelRatio || 1);
    if (renderer.getPixelRatio() !== dpr) renderer.setPixelRatio(dpr);
    const sz = renderer.getSize(new THREE.Vector2()); if (sz.x !== W || sz.y !== H) { renderer.setSize(W, H, false); camera.aspect = W / Math.max(1, H); camera.updateProjectionMatrix(); }
    if (seq.length) {
      const s = seq[0]; s.t = (s.t || 0) + dt;
      if (s.kind === 'move') { if (!s.from) s.from = pos.clone(); const k = Math.min(1, s.t / s.dur), e = k < 0.5 ? 2 * k * k : 1 - Math.pow(-2 * k + 2, 2) / 2; target.lerpVectors(s.from, s.to, e); }
      if (s.kind === 'grip') { gripTarget = s.value; if (s.value) held = true; else held = false; }
      if (s.t >= (s.dur || 0)) seq.shift();
    }
    pos.lerp(target, Math.min(1, dt * 14));
    root.position.copy(pos).add(new THREE.Vector3(0, Math.sin(t * 1.6) * 0.03, 0));
    const toward = Math.atan2(pos.y - restPoint().y + 6, pos.x - restPoint().x - 4);
    root.rotation.set(0.15 + Math.sin(t * 0.9) * 0.03, -0.35, toward - Math.PI / 2 + 0.2);
    grip += (gripTarget - grip) * Math.min(1, dt * 9);
    fingers.forEach((joints, fi) => joints.forEach((j, si) => { j.rotation.x = grip * (si === 0 ? 0.85 : 0.75) + Math.sin(t * 2 + fi + si) * 0.02 * (1 - grip); }));
    thumbJoints.forEach((j, si) => { j.rotation.x = grip * (si ? 0.6 : 0.35); j.rotation.y = -grip * 0.4; });
    cube.visible = held && grip > 0.5; cube.rotation.y += dt * 0.8; cube.rotation.x = 0.4;
    drawDisplay(t);
    renderer.render(scene, camera);
    raf = requestAnimationFrame(frame);
  }
  pos.copy(restPoint()); target.copy(pos);
  return {
    show(on) { visible = on; if (on) { pos.copy(restPoint()); target.copy(pos); seq = []; held = false; gripTarget = 0; if (!raf) { last = performance.now(); raf = requestAnimationFrame(frame); } } },
    grab(fx, fy, tx, ty, setDown) {
      const from = toWorld(fx, fy), to = toWorld(tx, ty);
      seq = [{ kind: 'grip', value: 0, dur: 0.05 }, { kind: 'move', to: from, dur: 0.45 }, { kind: 'grip', value: 1, dur: 0.22 }, { kind: 'move', to, dur: 0.55 }];
      if (setDown) seq.push({ kind: 'grip', value: 0, dur: 0.3 }, { kind: 'move', to: restPoint(), dur: 0.6 });   // place it, pull back
    },
    release() { seq = [{ kind: 'grip', value: 0, dur: 0.25 }, { kind: 'move', to: restPoint(), dur: 0.5 }]; },
  };
}
