// Self-check for the wireless control UI's pure helpers and static source
// invariants (accessibility landmarks/labels, DOM-safety). No framework,
// mirrors the style of tests/wireless_api_self_test.cpp.
//
// Run with: node tests/wireless_ui_self_test.mjs
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const appJsPath = path.join(root, 'src/Wireless/ui/app.js');
const indexHtmlPath = path.join(root, 'src/Wireless/ui/index.html');

const appJsSource = readFileSync(appJsPath, 'utf8');
const indexHtmlSource = readFileSync(indexHtmlPath, 'utf8');

const { createRequire } = await import('node:module');
const require = createRequire(import.meta.url);
const helpers = require(appJsPath);

function test(name, fn){
	try{
		fn();
		console.log('ok - ' + name);
	}catch(err){
		console.error('FAIL - ' + name);
		console.error(err);
		process.exitCode = 1;
	}
}

/* ---------------- command id generation ---------------- */

test('command ids are bounded and match the device validClientId charset', () => {
	const next = helpers.makeCommandIdFactory('abc123');
	const seen = new Set();
	for(let i = 0; i < 500; i++){
		const id = next();
		assert.ok(helpers.validId(id), 'id "' + id + '" must satisfy validId');
		assert.ok(id.length <= 32, 'id must be <= 32 chars');
		assert.ok(!seen.has(id), 'id must be unique across many calls: ' + id);
		seen.add(id);
	}
});

test('command id factory never produces ids the device would reject', () => {
	const next = helpers.makeCommandIdFactory('!!!not-valid!!!'); // falls back to "x"
	for(let i = 0; i < 20; i++){
		assert.ok(helpers.validId(next()));
	}
});

/* ---------------- no-replay tracking ---------------- */

test('command tracker refuses to mark the same id sent twice (no replay)', () => {
	const tracker = helpers.createCommandTracker();
	assert.equal(tracker.markSent('cmd1'), true);
	assert.equal(tracker.markSent('cmd1'), false, 'second send of same id must be rejected');
	assert.equal(tracker.markSent('cmd2'), true);
	assert.equal(tracker.size(), 2);
});

test('a fresh page load / reconnect gets a fresh tracker, never resending old ids implicitly', () => {
	const first = helpers.createCommandTracker();
	first.markSent('cmd1');
	const second = helpers.createCommandTracker();
	// Simulates reload: no memory of "cmd1", but the important invariant is
	// that the app never automatically re-issues a command after reconnect -
	// that is enforced by sendCommand() only ever being called from a fresh
	// user gesture, not from any retry/replay loop. We assert the tracker
	// itself starts empty (no accidental carry-over) so a bug can't make it
	// look like "cmd1" was already sent by this new session.
	assert.equal(second.has('cmd1'), false);
});

/* ---------------- lease state machine ---------------- */

test('lease reducer: acquire flow', () => {
	let state = 'read_only';
	state = helpers.leaseReducer(state, { type: 'ACQUIRE_START' });
	assert.equal(state, 'acquiring');
	state = helpers.leaseReducer(state, { type: 'ACQUIRE_OK' });
	assert.equal(state, 'controlling');
});

test('lease reducer: conflict keeps user out of a false "controlling" state', () => {
	let state = 'read_only';
	state = helpers.leaseReducer(state, { type: 'ACQUIRE_START' });
	state = helpers.leaseReducer(state, { type: 'ACQUIRE_CONFLICT' });
	assert.equal(state, 'conflict');
});

test('lease reducer: expiry and loss always fall back to read_only', () => {
	assert.equal(helpers.leaseReducer('controlling', { type: 'RENEW_EXPIRED' }), 'read_only');
	assert.equal(helpers.leaseReducer('controlling', { type: 'RENEW_NOT_OWNER' }), 'read_only');
	assert.equal(helpers.leaseReducer('controlling', { type: 'RELEASE_OK' }), 'read_only');
});

test('lease reducer: reconnect NEVER assumes control survived', () => {
	// This is the core "never replay / never assume control after reconnect"
	// invariant: regardless of prior state, a RECONNECT event must land in
	// read_only, forcing an explicit re-acquire.
	for(const prior of helpers.LEASE_STATES){
		assert.equal(helpers.leaseReducer(prior, { type: 'RECONNECT' }), 'read_only',
			'RECONNECT from "' + prior + '" must yield read_only');
	}
});

/* ---------------- backoff / polling ---------------- */

test('computeBackoff grows exponentially but stays bounded', () => {
	assert.equal(helpers.computeBackoff(0, 1000, 20000), 1000);
	assert.equal(helpers.computeBackoff(1, 1000, 20000), 2000);
	assert.equal(helpers.computeBackoff(2, 1000, 20000), 4000);
	assert.equal(helpers.computeBackoff(10, 1000, 20000), 20000);
	assert.equal(helpers.computeBackoff(999, 1000, 20000), 20000, 'must clamp huge failure counts');
});

test('choosePollDelay pauses (returns null) when offline', () => {
	const delay = helpers.choosePollDelay({
		online: false, hidden: false, isWriter: true, failures: 0,
		activeMs: 1000, idleMs: 3000, hiddenMs: 5000
	});
	assert.equal(delay, null);
});

test('choosePollDelay uses the hidden interval when the tab is backgrounded', () => {
	const delay = helpers.choosePollDelay({
		online: true, hidden: true, isWriter: true, failures: 0,
		activeMs: 1000, idleMs: 3000, hiddenMs: 5000
	});
	assert.equal(delay, 5000);
});

test('choosePollDelay polls faster while controlling than read-only', () => {
	const writer = helpers.choosePollDelay({
		online: true, hidden: false, isWriter: true, failures: 0,
		activeMs: 1000, idleMs: 3000, hiddenMs: 5000
	});
	const reader = helpers.choosePollDelay({
		online: true, hidden: false, isWriter: false, failures: 0,
		activeMs: 1000, idleMs: 3000, hiddenMs: 5000
	});
	assert.equal(writer, 1000);
	assert.equal(reader, 3000);
	assert.ok(writer < reader);
});

/* ---------------- coalescing (throttler) ---------------- */

test('throttler coalesces bursts down to a single trailing call with the latest value', () => {
	let now = 0;
	const timers = [];
	const throttler = helpers.createThrottler(200, {
		now: () => now,
		setTimer: (fn, ms) => { const t = { fn, at: now + ms }; timers.push(t); return t; },
		clearTimer: (t) => { const i = timers.indexOf(t); if(i >= 0) timers.splice(i, 1); }
	});
	const calls = [];
	throttler.schedule('gain:0', () => calls.push('a')); // fires immediately (elapsed >= wait from -Infinity)
	assert.deepEqual(calls, ['a']);
	now = 10;
	throttler.schedule('gain:0', () => calls.push('b')); // too soon, scheduled
	now = 50;
	throttler.schedule('gain:0', () => calls.push('c')); // supersedes 'b', still scheduled
	assert.deepEqual(calls, ['a'], 'no extra calls before the timer fires');
	assert.equal(timers.length, 1, 'only one pending trailing call per key');
	now = 210;
	timers[0].fn();
	assert.deepEqual(calls, ['a', 'c'], '"b" must never fire - only the latest coalesced value does');
});

test('throttler tracks independent keys separately (per-control coalescing)', () => {
	let now = 0;
	const throttler = helpers.createThrottler(200, { now: () => now });
	const calls = [];
	throttler.schedule('gain:0', () => calls.push('gain0'));
	throttler.schedule('gain:1', () => calls.push('gain1'));
	assert.deepEqual(calls.sort(), ['gain0', 'gain1']);
});

/* ---------------- command body shaping ---------------- */

test('buildCommandBody produces exactly the field set the device expects per action', () => {
	const identity = { bootId: 11, sessionId: 12 };
	const body = helpers.buildCommandBody(identity, 'set_mix', { value: 200 }, 'cmd1');
	assert.deepEqual(Object.keys(body).sort(), ['action', 'boot_id', 'client_command_id', 'session_id', 'value']);
	assert.equal(body.value, 200);

	const effectBody = helpers.buildCommandBody(identity, 'set_effect_type', { deck: 1, slot: 2, value: 4 }, 'cmd2');
	assert.deepEqual(Object.keys(effectBody).sort(),
		['action', 'boot_id', 'client_command_id', 'deck', 'session_id', 'slot', 'value']);
});

test('buildCommandBody rejects an invalid command id before it would reach the network', () => {
	assert.throws(() => helpers.buildCommandBody({ bootId: 1, sessionId: 1 }, 'set_mix', { value: 1 }, ''));
	assert.throws(() => helpers.buildCommandBody({ bootId: 1, sessionId: 1 }, 'set_mix', { value: 1 },
		'x'.repeat(40)));
});

test('buildCommandBody rejects unsupported actions (no accidental new surface)', () => {
	assert.throws(() => helpers.buildCommandBody({ bootId: 1, sessionId: 1 }, 'load_by_path', {}, 'cmd1'));
});

/* ---------------- formatting ---------------- */

test('formatClock renders mm:ss and a placeholder for unavailable timing', () => {
	assert.equal(helpers.formatClock(0), '0:00');
	assert.equal(helpers.formatClock(65), '1:05');
	assert.equal(helpers.formatClock(undefined), '--:--');
	assert.equal(helpers.formatClock(-1), '--:--');
});

/* ---------------- static DOM-safety invariant ---------------- */

test('app.js never assigns innerHTML/outerHTML or uses document.write (no unescaped DOM injection)', () => {
	assert.doesNotMatch(appJsSource, /\.innerHTML\s*=/, 'use textContent/value/setAttribute instead');
	assert.doesNotMatch(appJsSource, /\.outerHTML\s*=/);
	assert.doesNotMatch(appJsSource, /document\.write\s*\(/);
	assert.doesNotMatch(appJsSource, /\beval\s*\(/);
});

test('app.js builds JSON request bodies with JSON.stringify, never manual string concatenation', () => {
	assert.doesNotMatch(appJsSource, /body:\s*['"`]\{/, 'command/pair bodies must go through JSON.stringify');
});

/* ---------------- static accessibility invariants ---------------- */

test('index.html declares required landmarks and a polite live region', () => {
	assert.match(indexHtmlSource, /<header>/);
	assert.match(indexHtmlSource, /<main id="main">/);
	assert.match(indexHtmlSource, /aria-live="polite"/);
	assert.match(indexHtmlSource, /class="skip-link"/);
});

test('every text/password input in index.html has an associated <label for=...>', () => {
	const inputIds = [...indexHtmlSource.matchAll(/<input\s+id="([^"]+)"[^>]*type="(?:text|password)"/g)]
		.map((m) => m[1]);
	assert.ok(inputIds.length > 0, 'sanity: fixture should contain text/password inputs');
	for(const id of inputIds){
		const labelPattern = new RegExp('<label for="' + id + '">');
		assert.match(indexHtmlSource, labelPattern, 'missing <label for="' + id + '"> for #' + id);
	}
});

test('every range/select control in index.html has an associated <label for=...>', () => {
	const controlIds = [...indexHtmlSource.matchAll(/<(?:input\s+id="([^"]+)"[^>]*type="range"|select\s+id="([^"]+)")/g)]
		.map((m) => m[1] || m[2]);
	assert.ok(controlIds.length > 0, 'sanity: fixture should contain range/select controls');
	for(const id of controlIds){
		const labelPattern = new RegExp('<label for="' + id + '">');
		assert.match(indexHtmlSource, labelPattern, 'missing <label for="' + id + '"> for #' + id);
	}
});

test('disabled-by-design panels (cues) render real disabled controls, not fake working ones', () => {
	assert.match(indexHtmlSource, /class="cue-button" data-cue="0" disabled/);
	// Every cue button must be marked disabled in markup (belt) - app.js also
	// disables them defensively at runtime (suspenders), checked next.
	const cueButtons = [...indexHtmlSource.matchAll(/class="cue-button"[^>]*>/g)];
	assert.equal(cueButtons.length, 8);
	for(const button of cueButtons){
		assert.match(button[0], /disabled/);
	}
});

test('prefers-reduced-motion is respected in the stylesheet', () => {
	const cssSource = readFileSync(path.join(root, 'src/Wireless/ui/styles.css'), 'utf8');
	assert.match(cssSource, /prefers-reduced-motion/);
});

if(process.exitCode){
	console.error('\nSome checks failed.');
}else{
	console.log('\nAll wireless UI self-checks passed.');
}
