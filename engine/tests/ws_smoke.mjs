// End-to-end smoke test of the engine's app thread over WebSocket (Node >= 22, no dependencies).
// Expects a running core-linked drums-engine and a MIDI source sending pads while it plays
// (see engine/tests/ws_test.sh).   node ws_smoke.mjs [port]
const port = Number(process.argv[2] ?? 7979);
const ws = new WebSocket(`ws://127.0.0.1:${port}/`);
const seen = {};
const log = [];
let lastState = null;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const send = (o) => ws.send(JSON.stringify(o));

ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  seen[m.type] = (seen[m.type] ?? 0) + 1;
  if (m.type === "state") lastState = m;
  if (m.type !== "position" || !m.playing || seen.position % 60 === 1) log.push(m);
};

const failures = [];
const expect = (cond, what) => {
  console.log(`  ${cond ? "ok  " : "FAIL"} ${what}`);
  if (!cond) failures.push(what);
};

await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
await sleep(300);
const hello = log.find((m) => m.type === "hello");
console.log("hello:", JSON.stringify(hello));
console.log("state: kit=%s kits=%s exercise=%s bpm=%d notes=%d mixer.master=%s padMap[45]=%s",
  lastState?.kit?.name, lastState?.kits?.map((k) => k.name).join("|"), lastState?.exercise?.id,
  lastState?.bpm, lastState?.exercise?.notes?.length, lastState?.mixer?.master, lastState?.padMap?.["45"]);
expect(hello?.protocol === 1 && hello.sampleRate === 48000 && hello.bufferFrames > 0, "hello on connect");
expect(lastState?.exercise?.notes?.length === 17, "state on connect with the 12/8 exercise (17 notes)");

send({ type: "setBpm", bpm: 120 });
send({ type: "setCountIn", bars: 0 });
send({ type: "setMixer", channel: "snare", volume: 0.5 });
send({ type: "setClick", subdivisions: false });
send({ type: "bogus" });
send({ type: "startRecording" });
send({ type: "play" });
await sleep(300);
expect(lastState?.playing === true && lastState.bpm === 120 && lastState.recording === true, "play/setBpm/startRecording reflected in state");
expect(lastState?.mixer?.snare === 0.5 && lastState.click.subdivisions === false, "setMixer/setClick reflected in state");
expect(log.some((m) => m.type === "error" && /bogus/.test(m.message)), "error reply for an unknown command");

console.log("playing 6 s while pads are hit ...");
await sleep(6000);
const hits = log.filter((m) => m.type === "hit");
const pos = log.filter((m) => m.type === "position");
console.log("  first hits:", hits.slice(0, 4).map((h) => `${h.instrument}/${h.grade}@${h.loop}:${h.tickInLoop?.toFixed?.(1)} err=${h.errorMs}`).join("  "));
console.log("  sample position:", JSON.stringify(pos[pos.length - 1]));
console.log("  sample loopSummary:", JSON.stringify(log.find((m) => m.type === "loopSummary")));
expect((seen.position ?? 0) > 250, `~60 Hz position messages (${seen.position} in ~6.3 s)`);
expect(hits.length > 0 && hits.every((h) => h.grade !== "free"), `hits graded while playing (${hits.length})`);
expect((seen.missed ?? 0) > 0, `missed messages (${seen.missed})`);
expect((seen.loopSummary ?? 0) >= 2, `loopSummary after each 2 s loop (${seen.loopSummary})`);

send({ type: "setPadMap", note: 99, instrument: "ride" });
send({ type: "stop" });
send({ type: "stopRecording" });
await sleep(400);
const saved = log.find((m) => m.type === "recordingSaved");
console.log("  recordingSaved:", saved?.path);
expect(!!saved, "recordingSaved after stopRecording");
expect(lastState?.padMap?.["99"] === "ride", "setPadMap reflected in state");
expect(log.filter((m) => m.type === "position").some((m) => m.playing === false), "one position with playing=false after stop");

const hitsBefore = log.filter((m) => m.type === "hit").length;
await sleep(1500);
const freeHits = log.filter((m) => m.type === "hit").slice(hitsBefore);
expect(freeHits.length > 0 && freeHits.every((h) => h.grade === "free" && h.loop === null), `hits while stopped are free (${freeHits.length})`);

send({ type: "loadKit", path: "kits/sams-sonor/kit.json" });
await sleep(1500);
expect(lastState?.kit?.name === "Sam's Sonor", `loadKit -> ${lastState?.kit?.name}`);

send({ type: "setLoop", on: false });
send({ type: "play" });
await sleep(2600);  // one 2 s bar at 120 bpm, no count-in
expect(lastState?.playing === false, "transport stops by itself at the end of the exercise without loop");

console.log("message counts:", JSON.stringify(seen));
ws.close();
console.log(failures.length ? `WS SMOKE FAILED (${failures.length})` : "WS SMOKE OK");
process.exit(failures.length ? 1 : 0);
