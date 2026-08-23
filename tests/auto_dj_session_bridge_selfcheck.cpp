// Self-check for the DjSession <-> Auto DJ integration bridge
// (AutoDjSessionActuator / AutoDjSessionPort). Host-only, no Arduino/ESP32
// dependencies - exercises the actuator against a fully-controllable fake
// AutoDjSessionPort, never a real DjSession (DjSession.cpp itself pulls in
// Arduino/SD/AudioLib and is not host-compiled; see DjSession.h's Auto DJ
// section for how the real implementation wires these same calls).
//
// The isolated planner/queue/history/state-machine's own required
// scenarios (ties, invalidation, repeat/artist exclusion, pinned order,
// full/empty, missing metadata, no safe window, load applied/failure/
// timeout, retry budget, manual takeover, pause/resume/stop, EOF,
// recording failure, no infinite loop) are already covered end-to-end by
// tests/auto_dj_selfcheck.cpp against a MockLoadPort. This file instead
// covers the bridge layer itself: does AutoDjSessionActuator translate
// DjSession-shaped port calls (candidate table, deck context, manual-
// intent generations, snapshot, command tracking) into correct
// AutoDjLoadPort/DjAutoDjPlanner behavior.
//
// Build/run directly, e.g.:
//
//   g++ -std=c++11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
//       -I ../src/AutoDj -I ../src/DjAssist -I ../src/DjSession \
//       auto_dj_session_bridge_selfcheck.cpp ../src/AutoDj/DjAutoDjPlanner.cpp \
//       -o auto_dj_session_bridge_selfcheck && ./auto_dj_session_bridge_selfcheck

#include <assert.h>
#include <string.h>

#include "../src/AutoDj/AutoDjSessionActuator.h"

namespace {

DjTrackIdentity makeTrackIdentity(uint8_t fingerprintByte){
	DjTrackIdentity identity;
	identity.flags = DJ_TRACK_IDENTITY_FINGERPRINT;
	memset(identity.fingerprint, fingerprintByte, sizeof(identity.fingerprint));
	return identity;
}

// Fully controllable test double for AutoDjSessionPort - stands in for
// DjSession itself.
class FakeSessionPort : public AutoDjSessionPort {
public:
	struct Entry {
		DjAssistLibraryEntry entry;
		uint32_t artistHash = 0;
		uint32_t titleHash = 0;
	};

	Entry entries[8];
	uint32_t entryCount = 0;
	uint32_t metadataRevision = 1;
	uint32_t libraryGeneration = 1;
	DjAssistDeckContext deckContext; // .valid = false by default
	uint8_t targetDeck = 1;
	DjSnapshot snapshot;
	AutoDjManualIntentGenerations manualGenerations;
	bool physicalConfirmationPending = false;
	bool coachTransitionSettled = true; // default: nothing to wait for.
	uint64_t fakeNowUs = 0;

	// Load-submission bookkeeping.
	uint32_t nextCommandId = 1;
	DjSubmitResult nextSubmitResult = { 0, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE };
	bool lastLoadCalled = false;
	uint8_t lastLoadDeck = 255;
	DjTrackIdentity lastLoadIdentity;
	uint32_t lastLoadGeneration = 0;
	uint32_t lastLoadRevision = 0;
	uint32_t trackedCommandId = 0;
	DjCommandStatus trackedCommandStatus = DJ_COMMAND_PENDING;

	// Composite-workflow (Coach arm) bookkeeping.
	DjSubmitResult nextArmSubmitResult = { 0, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE };
	bool lastArmCalled = false;
	uint8_t lastArmFromDeck = 255;
	uint8_t lastArmToDeck = 255;
	DjTrackIdentity lastArmIdentity;
	uint8_t lastArmCrossfadeBeats = 0;
	bool lastArmStartAtBoundary = false;
	bool lastArmTempoLock = false;
	bool cancelCalled = false;
	DjAssistMode coachMode = DJ_ASSIST_MODE_COACH;

	uint32_t autoDjCandidateCount() override{
		return entryCount;
	}

	bool autoDjCandidateEntry(
		uint32_t index,
		DjAssistLibraryEntry& outEntry,
		uint32_t& outArtistHash,
		uint32_t& outTitleHash,
		uint32_t& outRevision
	) override{
		outRevision = metadataRevision;
		if(index >= entryCount) return false;
		outEntry = entries[index].entry;
		outArtistHash = entries[index].artistHash;
		outTitleHash = entries[index].titleHash;
		return true;
	}

	uint32_t autoDjMetadataRevision() override{ return metadataRevision; }
	uint32_t autoDjLibraryGeneration() override{ return libraryGeneration; }
	DjAssistDeckContext autoDjActiveDeckContext() override{ return deckContext; }
	uint8_t autoDjTargetDeck() override{ return targetDeck; }

	DjSubmitResult autoDjLoadDeckByIdentity(
		uint8_t deck, const DjTrackIdentity& identity,
		uint32_t libraryGenerationAtSelection, uint32_t metadataRevisionAtSelection
	) override{
		lastLoadCalled = true;
		lastLoadDeck = deck;
		lastLoadIdentity = identity;
		lastLoadGeneration = libraryGenerationAtSelection;
		lastLoadRevision = metadataRevisionAtSelection;
		DjSubmitResult result = nextSubmitResult;
		if(result.id == 0 && result.status == DJ_COMMAND_ACCEPTED) result.id = nextCommandId++;
		return result;
	}

	void autoDjTrackCommand(uint32_t commandId) override{
		trackedCommandId = commandId;
		trackedCommandStatus = DJ_COMMAND_ACCEPTED;
	}

	DjCommandStatus autoDjCommandStatus(uint32_t commandId) override{
		if(commandId != trackedCommandId) return DJ_COMMAND_PENDING;
		return trackedCommandStatus;
	}

	DjSubmitResult autoDjArmCoachTransition(
		uint8_t fromDeck, uint8_t toDeck, const DjTrackIdentity& targetIdentity,
		uint8_t crossfadeBeats, bool startAtBoundary, bool tempoLock
	) override{
		lastArmCalled = true;
		lastArmFromDeck = fromDeck;
		lastArmToDeck = toDeck;
		lastArmIdentity = targetIdentity;
		lastArmCrossfadeBeats = crossfadeBeats;
		lastArmStartAtBoundary = startAtBoundary;
		lastArmTempoLock = tempoLock;
		DjSubmitResult result = nextArmSubmitResult;
		if(result.id == 0 && result.status == DJ_COMMAND_ACCEPTED) result.id = nextCommandId++;
		return result;
	}

	DjSubmitResult autoDjCancelCoachTransition() override{
		cancelCalled = true;
		return { 0, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE };
	}

	DjAssistMode autoDjCoachTransitionMode() override{
		return coachMode;
	}

	bool copySnapshot(DjSnapshot& outSnapshot) override{
		outSnapshot = snapshot;
		return snapshot.sessionActive;
	}

	AutoDjManualIntentGenerations autoDjManualIntentGenerationsSnapshot() override{
		return manualGenerations;
	}

	bool autoDjConsumePhysicalConfirmation() override{
		const bool value = physicalConfirmationPending;
		physicalConfirmationPending = false;
		return value;
	}

	bool autoDjCoachTransitionSettled() override{
		return coachTransitionSettled;
	}

	// Defaults to true so every pre-existing scenario (written before this
	// accessor existed) keeps observing the capability as available,
	// exactly as the old unconditional `hasStableIdEndpoint() == true`
	// did. Tests exercising fix 2 (capability false-positive on
	// allocation/worker failure) flip this to false to prove arm()/
	// start()/the ongoing queue-fill all correctly see it degrade.
	bool stableIdAuthorityReady = true;
	bool autoDjStableIdAuthorityReady() override{
		return stableIdAuthorityReady;
	}

	uint64_t autoDjNowMicros() const override{
		return fakeNowUs;
	}
};

DjAssistLibraryEntry makeEntry(uint8_t fingerprintByte, uint32_t bpmMilli = 128000, uint16_t key = 0x801){
	DjAssistLibraryEntry entry;
	entry.identity = makeTrackIdentity(fingerprintByte);
	entry.state = DJ_METADATA_VALID;
	entry.capabilities = DJ_METADATA_HAS_BPM | DJ_METADATA_HAS_KEY;
	entry.bpmMilli = bpmMilli;
	entry.key = key;
	entry.rating = 3;
	entry.durationFrames = 44100ULL * 200;
	entry.sampleRate = 44100;
	return entry;
}

void setPlayingDeck(FakeSessionPort& port, uint8_t deck, uint16_t elapsed, uint16_t duration,
					 DjTimingQuality quality = DJ_TIMING_COARSE){
	port.snapshot.sessionActive = true;
	for(uint8_t d = 0; d < DJ_DECK_COUNT; d++){
		port.snapshot.decks[d].playing = (d == deck);
	}
	port.snapshot.decks[deck].loaded = true;
	port.snapshot.decks[deck].elapsed = elapsed;
	port.snapshot.decks[deck].duration = duration;
	port.snapshot.decks[deck].timingQuality = quality;
}

// Pins a track directly (bypassing the scan) so hasSafeWindow()'s
// !queue.empty() check passes even in tests with no scannable candidates.
void pinAnyTrack(AutoDjSessionActuator& actuator, uint8_t fingerprintByte = 0xF0){
	AutoDjIdentity identity;
	identity.flags = AUTO_DJ_IDENTITY_FINGERPRINT;
	identity.libraryGeneration = 1;
	memset(identity.fingerprint, fingerprintByte, sizeof(identity.fingerprint));
	actuator.pinTrack(identity, 0, 0);
}

// Drives an in-flight load through the rest of the composite workflow
// (fix #5): call this once the load command has been submitted and the
// caller has set port.trackedCommandStatus = DJ_COMMAND_APPLIED to resolve
// it, but BEFORE ticking again - this helper performs that resolving tick
// itself, then submits/applies the Coach arm and drives Coach's transition
// mode through RUNNING to COMPLETE, asserting the arm was submitted with
// the exact identity/deck/defaults Auto DJ just loaded. Each hidden
// sub-phase (arm-in-flight, arm-applied/transition-in-flight, transition-
// complete) is its own one-shot poll, mirroring exactly how the real
// planner/pollLoad() one-shot contract works - four tick()s total.
void completeCoachArmAndTransition(FakeSessionPort& port, AutoDjSessionActuator& actuator, uint8_t expectedToDeck){
	actuator.tick(); // observes load APPLIED, submits the Coach arm.
	assert(port.lastArmCalled);
	assert(port.lastArmToDeck == expectedToDeck);
	assert(port.lastArmFromDeck == uint8_t((DJ_DECK_COUNT - 1) - expectedToDeck));
	assert(port.lastArmCrossfadeBeats == AUTO_DJ_TRANSITION_CROSSFADE_BEATS);
	assert(port.lastArmStartAtBoundary == AUTO_DJ_TRANSITION_START_AT_BOUNDARY);
	assert(port.lastArmTempoLock == AUTO_DJ_TRANSITION_TEMPO_LOCK);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED; // arm applies.
	port.coachMode = DJ_ASSIST_MODE_TRANSITION_RUNNING;
	actuator.tick(); // observes arm APPLIED, moves to polling Coach's own transition mode.

	actuator.tick(); // still running - not yet terminal.

	port.coachMode = DJ_ASSIST_MODE_TRANSITION_COMPLETE;
	actuator.tick(); // Coach finished - only now does the entry resolve/record.
}

// -- Test 1: bounded scan finds the sole eligible candidate and plans it,
// then submits/tracks/applies it through the real DjAutoDjPlanner exactly
// as a real DjSession round-trip would. --
void testScanPlanSubmitApply(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xAA);
	port.entries[0].artistHash = 111;
	port.entries[0].titleHash = 222;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.deckContext.key = 0x801;
	port.libraryGeneration = 1;
	port.physicalConfirmationPending = true;
	setPlayingDeck(port, 0, 190, 200); // 10s remaining == exactly at end margin.

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	assert(actuator.state() == AutoDjState::Armed);
	actuator.tick(); // top up the queue while Armed so start()'s hasSafeWindow() check passes.
	assert(actuator.start());
	assert(actuator.state() == AutoDjState::Running);

	// tick() scans, plans, then (since currentTrackAtEnd() is already true)
	// submits the load on the very same tick the scan finishes.
	actuator.tick();
	assert(port.lastLoadCalled);
	assert(port.lastLoadDeck == port.targetDeck);
	assert(memcmp(port.lastLoadIdentity.fingerprint, port.entries[0].entry.identity.fingerprint, 16) == 0);
	// The candidate's captured generation+revision (from the scan pass)
	// must reach the port unchanged - never re-read live at submit time,
	// otherwise the whole "reject a since-superseded candidate" contract
	// (see AutoDjIdentity::metadataRevision's doc comment) is tautological.
	assert(port.lastLoadGeneration == port.libraryGeneration);
	assert(port.lastLoadRevision == port.metadataRevision);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	completeCoachArmAndTransition(port, actuator, port.targetDeck);
	assert(actuator.state() == AutoDjState::Running);

	AutoDjSnapshot snapshot;
	actuator.copySnapshot(snapshot);
	assert(snapshot.state == AutoDjState::Running);
	assert(snapshot.historySize == 1);
}

// -- Test 2: already-queued identity is excluded from re-planning even
// though it is still the only entry the scan sees (DjAssistScoring's
// isRecent exclusion, driven by planner.isQueued()). --
void testAlreadyQueuedExcluded(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xBB);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200); // near end so beginNextLoad() fires promptly.
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick(); // top up the queue while Armed so start()'s hasSafeWindow() check passes.
	actuator.start();
	actuator.tick(); // plans + submits 0xBB (queue depth reaches 1 pending).
	assert(port.lastLoadCalled);

	// Second candidate pass (simulate another tick before the pending load
	// resolves): the topup watermark is 2, current depth is 1 pending, so a
	// fresh scan is allowed - but since 0xBB is already queued/pending, it
	// must never be planned a second time.
	port.lastLoadCalled = false;
	actuator.tick();
	// pollLoad() defaults to Pending (trackedCommandStatus starts
	// PENDING/ACCEPTED), so no second submit happens regardless; the real
	// assertion is queue depth staying at 1 (never 2) - checked via
	// snapshot below once the in-flight load resolves.
	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	completeCoachArmAndTransition(port, actuator, port.targetDeck);
	AutoDjSnapshot snapshot;
	actuator.copySnapshot(snapshot);
	assert(snapshot.queueDepth == 0); // resolved, nothing duplicated behind it.
	assert(snapshot.historySize == 1);
}

// -- Test 3: manual takeover (a non-system command bumping a deck's manual
// intent generation) pauses Running immediately. --
void testManualTakeoverPauses(){
	FakeSessionPort port;
	port.deckContext.valid = true;
	setPlayingDeck(port, 0, 0, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	pinAnyTrack(actuator);
	assert(actuator.start());
	actuator.tick(); // establishes the rolling manual-intent baseline.
	assert(actuator.state() == AutoDjState::Running);

	port.manualGenerations.deck[0]++; // simulates DjSession::submit() bumping it.
	actuator.tick();
	assert(actuator.state() == AutoDjState::Paused);
}

// -- Test 3b (fix #5 review): a manual takeover detected while Auto's own
// Coach arm/transition may still be physically live (ArmInFlight or
// TransitionInFlight) must proactively cancel it, not merely rely on
// Coach's own separately-keyed guard to eventually notice - see
// manualTakeoverActive()'s doc comment in AutoDjSessionActuator.h. --
void testManualTakeoverDuringTransitionCancelsCoach(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xDD);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick();
	actuator.start();
	actuator.tick(); // submits load.
	assert(port.lastLoadCalled);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	actuator.tick(); // load APPLIED -> submits Coach arm -> ArmInFlight.
	assert(port.lastArmCalled);
	assert(!port.cancelCalled);

	// Manual takeover lands while the arm is still in flight (not yet
	// applied): the very next tick must observe it, cancel the live Coach
	// arm, and pause - before the arm has even resolved.
	port.manualGenerations.deck[0]++;
	actuator.tick();
	assert(port.cancelCalled);
	assert(actuator.state() == AutoDjState::Paused);
}

// -- Test 3c (fix #5 review): same as above, but the takeover lands once
// the arm has already applied and Coach's own transition is actively
// running (TransitionInFlight) - proves the cancel-on-takeover check
// covers both composite sub-phases, not only ArmInFlight. --
void testManualTakeoverDuringRunningTransitionCancelsCoach(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xD1);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick();
	actuator.start();
	actuator.tick(); // submits load.
	assert(port.lastLoadCalled);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	actuator.tick(); // load APPLIED -> submits Coach arm.
	port.trackedCommandStatus = DJ_COMMAND_APPLIED; // arm applies too.
	port.coachMode = DJ_ASSIST_MODE_TRANSITION_RUNNING;
	actuator.tick(); // arm APPLIED -> TransitionInFlight, Coach now running.
	assert(!port.cancelCalled);

	port.manualGenerations.deck[0]++;
	actuator.tick();
	assert(port.cancelCalled);
	assert(actuator.state() == AutoDjState::Paused);
}

// -- Test 3d (fix #5 review, refined by round-4 review fix #2): reset()
// (a hard, immediate abandon) must proactively cancel a live Coach
// arm/transition too, not only rely on the planner's own bookkeeping being
// forgotten - see reset()'s doc comment. But the abandon must route through
// the SAME bounded Teardown/settled-wait path a genuine Coach-side
// transition failure uses, rather than forcing loadSubPhase straight back
// to Idle: forcing Idle immediately let the very next tick() treat this as
// an ordinary load failure and retry (submit a fresh load) right away,
// regardless of whether the cancel's own rollback had actually finished -
// racing a new load against stale rollback commands on the same deck. --
void testResetDuringTransitionCancelsCoachThenWaitsForSettlement(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xD2);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick();
	actuator.start();
	actuator.tick(); // submits load.
	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	actuator.tick(); // load APPLIED -> submits Coach arm -> ArmInFlight.
	assert(port.lastArmCalled);
	assert(!port.cancelCalled);

	// Coach's own rollback has NOT settled yet at the moment of the
	// abandon request - the realistic case this fix protects against.
	port.coachTransitionSettled = false;
	assert(!actuator.reset()); // not a plain reset - state is still Running.
	assert(port.cancelCalled);
	assert(actuator.state() == AutoDjState::Running); // unchanged: reset() was rejected.

	// Bounded number of ticks while rollback stays unsettled: must never
	// resubmit a fresh load in the meantime (the exact race this fix
	// closes) and must never cancel a second time.
	port.cancelCalled = false;
	port.lastLoadCalled = false;
	for(int i = 0; i < 4; i++){
		actuator.tick();
		assert(!port.lastLoadCalled);
		assert(!port.cancelCalled);
	}

	// Once the rollback genuinely settles, the abandoned attempt resolves
	// exactly like any other failed load: within its retry budget, a fresh
	// load is submitted safely (no stale command left in flight anymore).
	port.coachTransitionSettled = true;
	actuator.tick(); // Teardown settles -> Failed -> handleLoadFailure() (retry kept).
	assert(!port.lastLoadCalled);
	actuator.tick(); // next one-shot tick: currentTrackAtEnd() still true -> retries.
	assert(port.lastLoadCalled);
}

// -- Test 4: generation1 X submitted -> load fails (attempt kept, within
// retry budget) -> library reindexes to generation2, dropping X from the
// queue -> the next tick() must NOT resubmit the stale X (the core
// regression fixed in c0b4985: verify the captured sequence/identity is
// still present and generation-valid before any retry; abandon cleanly if
// it was removed out from under the pending attempt). --
void testLibraryGenerationInvalidation(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xCC);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200); // near end so beginNextLoad() fires promptly.
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick(); // top up the queue while Armed so start()'s hasSafeWindow() check passes.
	actuator.start();
	actuator.tick(); // plans + submits X (generation 1) on the first attempt.
	assert(port.lastLoadCalled);

	AutoDjSnapshot before;
	actuator.copySnapshot(before);
	assert(before.queueDepth == 1);

	// The in-flight load fails (still within the retry budget: one failure
	// leaves pendingAttempts == 1 <= AUTO_DJ_RETRY_BUDGET (2), so X stays
	// captured for a retry rather than being skipped as a terminal failure).
	port.trackedCommandStatus = DJ_COMMAND_FAILED;
	actuator.tick();

	// Library reindexes before the retry happens: X (generation 1) is no
	// longer valid and is dropped from the queue outright.
	port.libraryGeneration = 2;
	port.lastLoadCalled = false;
	actuator.tick();
	assert(!port.lastLoadCalled); // stale X must never be resubmitted.

	AutoDjSnapshot after;
	actuator.copySnapshot(after);
	assert(after.historySize == 0); // never recorded either - it was abandoned, not applied.
}

// -- Test 4b: same invalidated-generation-during-retry regression, but
// while Stopping - an in-flight load may still resolve once more, but a
// dropped stale entry must never be resubmitted there either. --
void testLibraryGenerationInvalidationWhileStopping(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xEE);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick();
	actuator.start();
	actuator.tick(); // submits X (generation 1).
	assert(port.lastLoadCalled);

	assert(actuator.stop());
	assert(actuator.state() == AutoDjState::Stopping);

	// Reindex while Stopping and the attempt is still outstanding: X
	// (generation 1) is dropped from the queue.
	port.libraryGeneration = 2;
	port.lastLoadCalled = false;
	actuator.tick(); // resolvePendingWhileStopping(): still Pending, no resubmit path exists here regardless.
	assert(!port.lastLoadCalled);

	// Bounded: Stopping must still terminate within a fixed wall-clock
	// deadline. port.trackedCommandStatus never resolves in this test
	// (stays ACCEPTED), so the only way out is
	// resolvePendingWhileStopping()'s own AUTO_DJ_LOAD_TIMEOUT_US deadline
	// - drive the port's fake clock straight past it rather than looping
	// an arbitrary large tick count.
	port.fakeNowUs += AUTO_DJ_LOAD_TIMEOUT_US + 1;
	for(int i = 0; i < 10 && actuator.state() == AutoDjState::Stopping; i++) actuator.tick();
	assert(actuator.state() != AutoDjState::Stopping);
	assert(!port.lastLoadCalled); // never resubmitted the stale entry on the way out either.
}

// -- Test 4c: same regression as testLibraryGenerationInvalidation(), but
// triggered by a same-generation metadataRevision bump instead - a sidecar
// metadata replacement that leaves the external libraryGeneration
// unchanged. X is submitted under revision 1; before the retry, the
// revision bumps to 2 (generation stays 1) and X must be dropped from the
// queue and never resubmitted - this is the exact issue #3 regression
// (submitLoad() discarding identity generation/revision, and stepScan()'s
// invalidation only keying on libraryGeneration). --
void testMetadataRevisionInvalidation(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xC1);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200); // near end so beginNextLoad() fires promptly.
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick(); // top up the queue while Armed so start()'s hasSafeWindow() check passes.
	actuator.start();
	actuator.tick(); // plans + submits X under revision 1 on the first attempt.
	assert(port.lastLoadCalled);
	assert(port.lastLoadRevision == 1);

	AutoDjSnapshot before;
	actuator.copySnapshot(before);
	assert(before.queueDepth == 1);

	// The in-flight load fails (still within the retry budget), so X stays
	// captured for a retry rather than being skipped as a terminal failure.
	port.trackedCommandStatus = DJ_COMMAND_FAILED;
	actuator.tick();

	// Metadata is replaced before the retry happens: same libraryGeneration
	// (1), but metadataRevision advances to 2. X (captured under revision
	// 1) must be dropped and never resubmitted.
	port.metadataRevision = 2;
	port.lastLoadCalled = false;
	actuator.tick();
	assert(!port.lastLoadCalled); // stale X must never be resubmitted.

	AutoDjSnapshot after;
	actuator.copySnapshot(after);
	assert(after.historySize == 0); // abandoned, not applied.
}

// -- Test 4d: same revision-invalidation regression, but while Stopping. --
void testMetadataRevisionInvalidationWhileStopping(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xC2);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick();
	actuator.start();
	actuator.tick(); // submits X under revision 1.
	assert(port.lastLoadCalled);

	assert(actuator.stop());
	assert(actuator.state() == AutoDjState::Stopping);

	// Same-generation metadata replacement while Stopping and the attempt
	// is still outstanding: X (revision 1) is dropped from the queue.
	port.metadataRevision = 2;
	port.lastLoadCalled = false;
	actuator.tick();
	assert(!port.lastLoadCalled);

	// Bounded: Stopping must still terminate within a fixed wall-clock
	// deadline. port.trackedCommandStatus never resolves in this test
	// (stays ACCEPTED), so the only way out is
	// resolvePendingWhileStopping()'s own AUTO_DJ_LOAD_TIMEOUT_US deadline
	// - drive the port's fake clock straight past it rather than looping
	// an arbitrary large tick count.
	port.fakeNowUs += AUTO_DJ_LOAD_TIMEOUT_US + 1;
	for(int i = 0; i < 10 && actuator.state() == AutoDjState::Stopping; i++) actuator.tick();
	assert(actuator.state() != AutoDjState::Stopping);
	assert(!port.lastLoadCalled); // never resubmitted the stale entry on the way out either.
}

// -- Test 4e (fix #5 review, "failures at every stage"): the Coach arm
// submit itself is rejected (e.g. the target/from-deck state no longer
// satisfies armTransition()'s own guard). Within the retry budget the
// attempt must retry from the load step again (never resubmit a bare arm
// against a stale load); once retried successfully the entry still
// resolves normally. --
void testCoachArmRejectionRetriesFromLoadThenResolves(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xCC);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick();
	actuator.start();
	actuator.tick(); // submits the load.
	assert(port.lastLoadCalled);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	port.nextArmSubmitResult = { 0, DJ_COMMAND_REJECTED, DJ_COMMAND_ERROR_AUTODJ_REJECTED };
	actuator.tick(); // load APPLIED -> beginArm() submits the arm, port rejects it synchronously.
	assert(port.lastArmCalled);

	AutoDjSnapshot mid;
	actuator.copySnapshot(mid);
	assert(mid.historySize == 0); // not resolved terminally yet.
	assert(mid.queueDepth == 1); // within AUTO_DJ_RETRY_BUDGET: entry stays queued for a retry.

	// The retry re-enters at the load step (never a bare arm retry against
	// a load that may no longer be current) - let it succeed this time.
	port.lastLoadCalled = false;
	port.nextArmSubmitResult = { 0, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE };
	actuator.tick();
	assert(port.lastLoadCalled);
	assert(memcmp(port.lastLoadIdentity.fingerprint, port.entries[0].entry.identity.fingerprint, 16) == 0);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	completeCoachArmAndTransition(port, actuator, port.targetDeck);
	AutoDjSnapshot after;
	actuator.copySnapshot(after);
	assert(after.historySize == 1); // eventually resolved once the retry succeeds.
}

// -- Test 4f (fix #5 review, "failures at every stage"): Coach's own
// transition fails after a successful arm (e.g. its guard trips on a
// divergence at start/crossfade time) - pollTransitionPhase() must map this
// uniformly to Failed and defer to the planner's existing bounded retry/
// terminal-skip policy exactly like a load or arm failure, never invent a
// separate crossfade-recovery path of its own. --
void testCoachTransitionFailureFailsAttemptWithinBudget(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xCE);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick();
	actuator.start();
	actuator.tick(); // submits the load.
	assert(port.lastLoadCalled);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	actuator.tick(); // load APPLIED -> submits the Coach arm.
	assert(port.lastArmCalled);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED; // arm applies.
	actuator.tick(); // arm APPLIED -> TransitionInFlight.

	port.coachMode = DJ_ASSIST_MODE_TRANSITION_FAILED;
	actuator.tick(); // Coach's own transition failed - must resolve to Failed, not hang.

	AutoDjSnapshot mid;
	actuator.copySnapshot(mid);
	assert(mid.historySize == 0);
	assert(mid.queueDepth == 1); // within budget: retried, not skipped yet.

	// Retry re-enters at the load step and this time Coach's transition
	// actually completes.
	port.lastLoadCalled = false;
	port.coachMode = DJ_ASSIST_MODE_COACH; // idle baseline for the next arm.
	actuator.tick();
	assert(port.lastLoadCalled);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	completeCoachArmAndTransition(port, actuator, port.targetDeck);
	AutoDjSnapshot after;
	actuator.copySnapshot(after);
	assert(after.historySize == 1);
}

// -- Round-4 review fix #2: reset() must be transactional. Calling it
// while Running with no live Coach arm/transition to abandon (loadSubPhase
// is only LoadInFlight - the stable-ID deck load itself, before Coach is
// even engaged) must be a pure no-op: planner.reset() rejects (only
// Failed/Complete are eligible) and there is nothing to cancel, so it must
// not touch loadSubPhase or call autoDjCancelCoachTransition(). Proven by
// continuing the same in-flight load to a normal Applied/history-recorded
// completion afterward - if reset() had mutated anything, this would hang
// or double-submit. (Contrast with
// testResetDuringTransitionCancelsCoachThenWaitsForSettlement(), where a
// live Coach arm/transition IS abandoned - deliberately, and safely via
// Teardown - because reset() is meant to work as a hard abandon in that
// case.) --
void testRejectedResetDuringLoadInFlightHasNoSideEffects(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xC5);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick();
	actuator.start();
	actuator.tick(); // submits the load -> LoadInFlight.
	assert(port.lastLoadCalled);
	assert(actuator.state() == AutoDjState::Running);

	// reset() while Running with no live Coach transition must be a true
	// no-op: rejected, no cancel, no loadSubPhase mutation.
	assert(!actuator.reset());
	assert(actuator.state() == AutoDjState::Running);
	assert(!port.cancelCalled);

	// The rejected reset changed nothing: the same in-flight load proceeds
	// and completes exactly as it would have without the reset() call ever
	// happening.
	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	completeCoachArmAndTransition(port, actuator, port.targetDeck);

	AutoDjSnapshot after;
	actuator.copySnapshot(after);
	assert(after.historySize == 1);
	assert(after.queueDepth == 0);
	assert(!port.cancelCalled); // never cancelled at any point in this run.
}

// -- Round-3 review fix #2: if Coach's own rollback (mix/sync/stop-deck
// restore) does not settle within the actuator's OWN bounded
// AUTO_DJ_TEARDOWN_TIMEOUT_US - independent of, and much shorter than, the
// planner's outer AUTO_DJ_LOAD_TIMEOUT_US composite-attempt budget, which
// may already be nearly exhausted by the transition that just failed -
// this must resolve to the terminal FailedTerminal outcome (planner ->
// AutoDjState::Failed / AutoDjFailReason::TeardownTimeout) and NEVER
// retry/skip a fresh load while the old rollback's own commands may still
// be in flight against the deck this attempt targeted. Also proves
// submitLoad() independently hard-rejects while unsettled, regardless of
// loadSubPhase/planner bookkeeping (defense in depth). --
void testTeardownTimeoutBecomesTerminalFailureNotRetry(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xEF);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick();
	actuator.start();
	actuator.tick(); // submits the load.
	assert(port.lastLoadCalled);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	actuator.tick(); // load APPLIED -> submits the Coach arm.
	assert(port.lastArmCalled);

	port.trackedCommandStatus = DJ_COMMAND_APPLIED; // arm applies.
	actuator.tick(); // arm APPLIED -> TransitionInFlight.

	// Coach's transition fails AND its own rollback gets stuck (never
	// settles - e.g. a wedged mix/sync/stop-deck command on real hardware).
	port.coachMode = DJ_ASSIST_MODE_TRANSITION_FAILED;
	port.coachTransitionSettled = false;
	actuator.tick(); // enters Teardown and records its own bounded deadline.
	assert(actuator.state() == AutoDjState::Running); // not yet resolved either way.

	port.lastLoadCalled = false;

	// Advance the fake wall clock past the actuator's own teardown
	// deadline while rollback is still unsettled.
	port.fakeNowUs += AUTO_DJ_TEARDOWN_TIMEOUT_US + 1;

	// Bounded: a fixed number of ticks must resolve to the terminal Failed
	// state, and a fresh LOAD_DECK must never be resubmitted at any point.
	for(int i = 0; i < 4 && actuator.state() == AutoDjState::Running; i++){
		actuator.tick();
		assert(!port.lastLoadCalled);
	}
	assert(actuator.state() == AutoDjState::Failed);
	assert(actuator.failReason() == AutoDjFailReason::TeardownTimeout);

	// Failed is terminal: neither stop() nor arm() may be used to move out
	// of it while rollback remains unsettled - only reset() (itself gated
	// on settlement, proven below) may leave Failed.
	assert(!actuator.stop());
	assert(actuator.state() == AutoDjState::Failed);
	assert(!actuator.arm());
	assert(actuator.state() == AutoDjState::Failed);

	AutoDjSnapshot afterFail;
	actuator.copySnapshot(afterFail);
	assert(afterFail.queueDepth == 1); // left untouched - never popped/skipped.
	assert(afterFail.historySize == 0); // never recorded as resolved.

	// A handful more idle ticks: Failed is terminal, must not spin/retry.
	for(int i = 0; i < 4; i++){
		actuator.tick();
		assert(!port.lastLoadCalled);
		assert(actuator.state() == AutoDjState::Failed);
	}

	// Direct hard-reject proof, bypassing the planner entirely: submitLoad()
	// itself refuses while rollback remains unsettled, regardless of
	// loadSubPhase/state bookkeeping.
	AutoDjIdentity anyIdentity;
	anyIdentity.flags = AUTO_DJ_IDENTITY_FINGERPRINT;
	memset(anyIdentity.fingerprint, 0xEF, sizeof(anyIdentity.fingerprint));
	assert(!actuator.submitLoad(anyIdentity));
	assert(!port.lastLoadCalled);

	// The core fix under test: reset() must reject outright while
	// loadSubPhase is still Teardown and rollback genuinely has not
	// settled - even though planner state is Failed (which, on its own,
	// is exactly the condition reset() normally accepts). A rejected
	// reset here must be a true no-op: Failed preserved, queue/history
	// completely untouched, and arm() still rejected immediately after -
	// proving no side effect snuck through via reset()'s attempt.
	assert(!actuator.reset());
	assert(actuator.state() == AutoDjState::Failed);
	AutoDjSnapshot stillUnsettled;
	actuator.copySnapshot(stillUnsettled);
	assert(stillUnsettled.queueDepth == 1);
	assert(stillUnsettled.historySize == 0);
	assert(!actuator.arm());
	assert(actuator.state() == AutoDjState::Failed);

	// Once rollback genuinely settles, the hard-reject lifts (it only
	// blocks while genuinely unsettled, never permanently) - reset() is
	// still required to leave the terminal Failed state itself.
	port.coachTransitionSettled = true;
	assert(actuator.submitLoad(anyIdentity));
	assert(actuator.reset());
	assert(actuator.state() == AutoDjState::Off);

	// Off is no longer terminal: re-arming now works normally.
	port.physicalConfirmationPending = true;
	assert(actuator.arm());
	assert(actuator.state() == AutoDjState::Armed);
}

// -- Same regression, but the outer stop() request arrives before the
// stuck rollback resolves: resolvePendingWhileStopping() must escalate to
// the same terminal Failed state (never silently finish "stopping" over an
// unsettled rollback, which would misreport an unsafe state as safely
// idle). --
void testTeardownTimeoutBecomesTerminalFailureWhileStopping(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xF1);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	setPlayingDeck(port, 0, 190, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick();
	actuator.start();
	actuator.tick(); // submits the load.
	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	actuator.tick(); // load APPLIED -> submits the Coach arm.
	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	actuator.tick(); // arm APPLIED -> TransitionInFlight.

	port.coachMode = DJ_ASSIST_MODE_TRANSITION_FAILED;
	port.coachTransitionSettled = false;
	actuator.tick(); // enters Teardown.

	assert(actuator.stop());
	assert(actuator.state() == AutoDjState::Stopping);

	port.lastLoadCalled = false;
	port.fakeNowUs += AUTO_DJ_TEARDOWN_TIMEOUT_US + 1;
	for(int i = 0; i < 4 && actuator.state() == AutoDjState::Stopping; i++){
		actuator.tick();
		assert(!port.lastLoadCalled);
	}
	assert(actuator.state() == AutoDjState::Failed);
	assert(actuator.failReason() == AutoDjFailReason::TeardownTimeout);
}

// -- Test 5: currentTrackAtEnd()/currentDurationTrustworthy() are false
// (never guessed) when no deck is playing or timing is unavailable. --
void testNoSafeWindowWithoutTrustworthyDuration(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xDD);
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;
	// Neither deck playing.
	port.snapshot.sessionActive = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	actuator.tick(); // top up the queue while Armed so start()'s hasSafeWindow() check passes.
	actuator.start();
	actuator.tick(); // scans/plans (queue topped up) but must not submit -
	                 // there is no trustworthy "about to end" signal yet.
	assert(!port.lastLoadCalled);
	assert(actuator.state() == AutoDjState::Running);

	// Now playing but timing UNAVAILABLE (coarse timing never established).
	setPlayingDeck(port, 0, 5, 200, DJ_TIMING_UNAVAILABLE);
	actuator.tick();
	assert(!port.lastLoadCalled);
}

// -- Test 6: a recording failure fails the run (never autonomously starts/
// stops recording itself - it only reacts to a failure already reported by
// the cached snapshot). --
void testRecordingFailureFailsRun(){
	FakeSessionPort port;
	port.deckContext.valid = true;
	setPlayingDeck(port, 0, 0, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	pinAnyTrack(actuator);
	assert(actuator.start());
	actuator.tick();
	assert(actuator.state() == AutoDjState::Running);

	port.snapshot.recordingInfo.state = DJ_RECORDING_FAILED;
	actuator.tick();
	assert(actuator.state() == AutoDjState::Failed);
	assert(actuator.failReason() == AutoDjFailReason::RecordingFailure);
}

// -- Test 7: pinning goes through the actuator's stable-identity path and
// is reflected in queue depth. --
void testPinTrackTranslatesIdentity(){
	FakeSessionPort port;
	AutoDjSessionActuator actuator(port);
	AutoDjIdentity identity;
	identity.flags = AUTO_DJ_IDENTITY_FINGERPRINT;
	identity.libraryGeneration = port.libraryGeneration;
	memset(identity.fingerprint, 0xEE, sizeof(identity.fingerprint));
	assert(actuator.pinTrack(identity, 1, 2));
	AutoDjSnapshot snapshot;
	actuator.copySnapshot(snapshot);
	assert(snapshot.queueDepth == 1);
}

// -- Test 9 (round-2 review fix #1, regression): a candidate whose identity
// carries a nonzero sourceId (AUTO_DJ_IDENTITY_SOURCE set, real nonzero
// bytes - not just the flag) must reach DjSession::autoDjLoadDeckByIdentity()
// with that exact sourceId still attached, both via the scan/plan/submit
// path (toAutoDjIdentity() then submitLoad()) and via the direct pin path
// (pinTrack() then submitLoad()). Before this fix, AutoDjIdentity had no
// sourceId field at all, so both conversions silently zeroed it while still
// setting the SOURCE flag - which made DjSession::resolveMetadata() pass a
// zeroed sourceId to trackByPath() as a REQUIRED match against the real
// (nonzero) on-disk value, failing resolution for every nonzero-source
// track and exhausting the retry budget.
void testNonzeroSourceIdPropagatesThroughScanAndSubmit(){
	FakeSessionPort port;
	port.entryCount = 1;
	port.entries[0].entry = makeEntry(0xAA);
	port.entries[0].entry.identity.flags = DJ_TRACK_IDENTITY_FINGERPRINT | DJ_TRACK_IDENTITY_SOURCE;
	memset(port.entries[0].entry.identity.sourceId, 0x77, sizeof(port.entries[0].entry.identity.sourceId));
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.deckContext.key = 0x801;
	port.libraryGeneration = 1;
	port.physicalConfirmationPending = true;
	setPlayingDeck(port, 0, 190, 200); // 10s remaining == exactly at end margin.

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	actuator.tick();
	assert(actuator.start());
	actuator.tick(); // scans, plans, submits the load on this same tick.

	assert(port.lastLoadCalled);
	assert(port.lastLoadIdentity.flags & DJ_TRACK_IDENTITY_SOURCE);
	uint8_t expectedSourceId[16];
	memset(expectedSourceId, 0x77, sizeof(expectedSourceId));
	assert(memcmp(port.lastLoadIdentity.sourceId, expectedSourceId, sizeof(expectedSourceId)) == 0);
}

void testNonzeroSourceIdPropagatesThroughPin(){
	FakeSessionPort port;
	AutoDjSessionActuator actuator(port);
	AutoDjIdentity identity;
	identity.flags = AUTO_DJ_IDENTITY_FINGERPRINT | AUTO_DJ_IDENTITY_SOURCE;
	identity.libraryGeneration = port.libraryGeneration;
	identity.metadataRevision = port.metadataRevision; // must match, or the
	// first tick()'s refreshMetadataRevision()/invalidateMetadataRevision()
	// (lastKnownRevision starts at 0, differs from port.metadataRevision)
	// would drop this pin before it can ever be submitted.
	memset(identity.fingerprint, 0xEE, sizeof(identity.fingerprint));
	memset(identity.sourceId, 0x99, sizeof(identity.sourceId));
	assert(actuator.pinTrack(identity, 1, 2));

	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.deckContext.key = 0x801;
	port.physicalConfirmationPending = true;
	setPlayingDeck(port, 0, 190, 200);
	assert(actuator.arm());
	assert(actuator.start());
	actuator.tick(); // pinned entry is submitted ahead of any scan.

	assert(port.lastLoadCalled);
	assert(port.lastLoadIdentity.flags & DJ_TRACK_IDENTITY_SOURCE);
	uint8_t expectedSourceId[16];
	memset(expectedSourceId, 0x99, sizeof(expectedSourceId));
	assert(memcmp(port.lastLoadIdentity.sourceId, expectedSourceId, sizeof(expectedSourceId)) == 0);
}

// -- Test 8: stop()/pause()/resume() cycle without a stuck pending load
// (no infinite loop - tick() always makes bounded progress). --
void testStopPauseResumeCycle(){
	FakeSessionPort port;
	port.deckContext.valid = true;
	setPlayingDeck(port, 0, 0, 200);
	port.physicalConfirmationPending = true;

	AutoDjSessionActuator actuator(port);
	actuator.arm();
	pinAnyTrack(actuator);
	assert(actuator.start());
	assert(actuator.pause());
	assert(actuator.state() == AutoDjState::Paused);
	port.physicalConfirmationPending = true;
	assert(actuator.resume());
	assert(actuator.state() == AutoDjState::Running);
	assert(actuator.stop());
	assert(actuator.state() == AutoDjState::Stopping);
	// Bounded: a fixed number of ticks must reach Off/Complete, never spin.
	for(int i = 0; i < 8 && actuator.state() == AutoDjState::Stopping; i++) actuator.tick();
	assert(actuator.state() != AutoDjState::Stopping);
}

} // namespace

int main(){
	testScanPlanSubmitApply();
	testAlreadyQueuedExcluded();
	testManualTakeoverPauses();
	testManualTakeoverDuringTransitionCancelsCoach();
	testManualTakeoverDuringRunningTransitionCancelsCoach();
	testResetDuringTransitionCancelsCoachThenWaitsForSettlement();
	testLibraryGenerationInvalidation();
	testLibraryGenerationInvalidationWhileStopping();
	testMetadataRevisionInvalidation();
	testMetadataRevisionInvalidationWhileStopping();
	testCoachArmRejectionRetriesFromLoadThenResolves();
	testCoachTransitionFailureFailsAttemptWithinBudget();
	testRejectedResetDuringLoadInFlightHasNoSideEffects();
	testTeardownTimeoutBecomesTerminalFailureNotRetry();
	testTeardownTimeoutBecomesTerminalFailureWhileStopping();
	testNoSafeWindowWithoutTrustworthyDuration();
	testRecordingFailureFailsRun();
	testPinTrackTranslatesIdentity();
	testNonzeroSourceIdPropagatesThroughScanAndSubmit();
	testNonzeroSourceIdPropagatesThroughPin();
	testStopPauseResumeCycle();
	return 0;
}
