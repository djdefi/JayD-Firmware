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

	// Load-submission bookkeeping.
	uint32_t nextCommandId = 1;
	DjSubmitResult nextSubmitResult = { 0, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE };
	bool lastLoadCalled = false;
	uint8_t lastLoadDeck = 255;
	DjTrackIdentity lastLoadIdentity;
	uint32_t trackedCommandId = 0;
	DjCommandStatus trackedCommandStatus = DJ_COMMAND_PENDING;

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

	DjSubmitResult autoDjLoadDeckByIdentity(uint8_t deck, const DjTrackIdentity& identity) override{
		lastLoadCalled = true;
		lastLoadDeck = deck;
		lastLoadIdentity = identity;
		DjSubmitResult result = nextSubmitResult;
		if(result.id == 0 && result.status == DJ_COMMAND_ACCEPTED) result.id = nextCommandId++;
		return result;
	}

	void autoDjTrackLoadCommand(uint32_t commandId) override{
		trackedCommandId = commandId;
		trackedCommandStatus = DJ_COMMAND_ACCEPTED;
	}

	DjCommandStatus autoDjLoadCommandStatus(uint32_t commandId) override{
		if(commandId != trackedCommandId) return DJ_COMMAND_PENDING;
		return trackedCommandStatus;
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

	port.trackedCommandStatus = DJ_COMMAND_APPLIED;
	actuator.tick();
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
	actuator.tick();
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

	// Bounded: Stopping must still terminate within a fixed number of ticks.
	for(int i = 0; i < 60 && actuator.state() == AutoDjState::Stopping; i++) actuator.tick();
	assert(actuator.state() != AutoDjState::Stopping);
	assert(!port.lastLoadCalled); // never resubmitted the stale entry on the way out either.
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
	testLibraryGenerationInvalidation();
	testLibraryGenerationInvalidationWhileStopping();
	testNoSafeWindowWithoutTrustworthyDuration();
	testRecordingFailureFailsRun();
	testPinTrackTranslatesIdentity();
	testStopPauseResumeCycle();
	return 0;
}
