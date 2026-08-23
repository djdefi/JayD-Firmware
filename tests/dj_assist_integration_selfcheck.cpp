#include <assert.h>
#include <string.h>

#include "../src/DjAssist/DjAssistController.h"
#include "../src/DjAssist/DjAssistSessionPort.h"
#include "Arduino.h"

// Host integration harness for the Coach/one-shot-transition layer: drives
// the REAL, unmodified DjAssistController (and, through it, the real
// DjAssistEngine/DjAssistScoring/DjAssistSessionBridge logic) against a
// test-authored fake DjAssistSessionPort, instead of only ever exercising
// helper functions in isolation. This is what closes the "DjSession.cpp/
// DjAssistController.cpp are excluded from host tests" gap: the fake port
// stands in for DjSession (whose own .cpp still has real Arduino/CircuitOS
// dependencies - AudioLib, FS, LoopManager - well beyond this seam's scope),
// but everything on the controller/engine side of DjAssistSessionPort is the
// exact same code the firmware links.
//
// Command admission/lifecycle bookkeeping (DjCommandQueue, DjCommandResults,
// DjAssistTrackedCommand, DjAssistIntentGenerations, admitAssistCommand()) is
// never reimplemented here either - the fake port's setPlaying()/setSync()/
// setMix() call the exact same admitAssistCommand() free function
// DjSession::submit() calls (see DjSessionState.h), and driveOneQueuedCommand()
// below mirrors DjSession::loop()'s pop/apply/finish/assistTracked-update
// sequence. So this test exercises the real shared admission coordinator
// used by DjSession, not a parallel reimplementation of it.
//
// The background candidate-fill Task is never actually spawned (see
// tests/host_stubs/Util/Task.h) - this test drives DjAssistController::
// fillWorkerStep()/candidateTableReady() directly via the friend grant,
// exactly as many bounded, deterministic calls as it chooses, exercising the
// identical real stepping logic without needing real threading.

namespace {

// ---------------------------------------------------------------------
// Fake DjAssistSessionPort: a minimal, fully test-controlled stand-in for
// DjSession. Every method mirrors the real DjSession method it replaces
// (see DjAssistSessionPort.h's doc comment) - admission/queue bookkeeping
// specifically reuses the real, shared admitAssistCommand() rather than
// reimplementing origin-priority/supersede/tracked-slot semantics here.
// ---------------------------------------------------------------------
class FakeAssistSessionPort : public DjAssistSessionPort {
public:
	// Command lifecycle state - real production types (DjSessionState.h),
	// driven through the real admitAssistCommand() free function.
	DjCommandQueue queue;
	DjCommandResults results;
	DjAssistTrackedCommand tracked;
	DjAssistIntentGenerations generations;
	uint32_t nextCommandId = 1;

	// Published/live session state the test directly controls.
	DjSnapshot snapshot;

	// Candidate/library surface.
	uint32_t generation = 1;
	static const uint32_t kMaxEntries = 8;
	DjAssistLibraryEntry entries[kMaxEntries];
	uint32_t entryCount = 0;
	// When >= 0, the NEXT assistTrackEntry() call for this exact index
	// bumps `generation` immediately after capturing outRevision at the
	// old value - simulating a metadata refresh landing in the unlocked
	// gap between the entry read and fillWorkerStep()'s in-lock live-
	// generation re-check. Fires once, then resets to -1.
	int bumpGenerationOnReadIndex = -1;

	bool media = true;
	uint64_t deckFrames[DJ_DECK_COUNT] = {};
	bool downbeatAvailable[DJ_DECK_COUNT] = {};
	uint64_t downbeatFrame[DJ_DECK_COUNT] = {};
	bool phraseAvailable[DJ_DECK_COUNT] = {};
	uint64_t phraseFrame[DJ_DECK_COUNT] = {};

	int purgeCallCount = 0;

	DjSubmitResult setPlaying(uint8_t deck, bool playing, DjCommandOrigin origin) override{
		DjCommand command = {};
		command.origin = origin;
		command.type = DJ_COMMAND_SET_PLAYING;
		command.deck = deck;
		command.value = playing ? 1 : 0;
		return admit(command);
	}

	DjSubmitResult setSync(uint8_t deck, bool armed, int8_t masterDeck, DjCommandOrigin origin) override{
		DjCommand command = {};
		command.origin = origin;
		command.type = DJ_COMMAND_SET_SYNC;
		command.deck = deck;
		command.value = armed ? 1 : 0;
		command.slot = masterDeck < 0 ? 0 : uint8_t(masterDeck) + 1;
		return admit(command);
	}

	DjSubmitResult setMix(uint8_t mix, DjCommandOrigin origin) override{
		DjCommand command = {};
		command.origin = origin;
		command.type = DJ_COMMAND_SET_MIX;
		command.value = mix;
		return admit(command);
	}

	void assistTrackCommand(uint32_t commandId) override{
		tracked.id = commandId;
		tracked.tracked = true;
		tracked.status = DJ_COMMAND_ACCEPTED;
	}

	DjCommandStatus assistTrackedStatus(uint32_t commandId) override{
		if(tracked.tracked && tracked.id == commandId) return tracked.status;
		return DJ_COMMAND_PENDING;
	}

	bool copySnapshot(DjSnapshot& out) override{
		snapshot.queueDepth = queue.depth();
		results.copyTo(snapshot.recentResults);
		out = snapshot;
		return true;
	}

	uint32_t assistLibraryGeneration() override{
		return generation;
	}

	uint32_t assistTrackCount() override{
		return entryCount;
	}

	bool assistTrackEntry(uint32_t index, DjAssistLibraryEntry& outEntry, uint32_t& outRevision) override{
		outRevision = generation;
		if(int(index) == bumpGenerationOnReadIndex){
			bumpGenerationOnReadIndex = -1; // fires exactly once.
			generation++;
		}
		if(index >= entryCount) return false;
		outEntry = entries[index];
		return true;
	}

	bool mediaPresent() const override{
		return media;
	}

	uint64_t deckElapsedFrames(uint8_t deck) const override{
		return deck < DJ_DECK_COUNT ? deckFrames[deck] : 0;
	}

	bool nextDownbeatFrame(uint8_t deck, uint64_t /*currentFrame*/, uint64_t& outFrame) const override{
		if(deck >= DJ_DECK_COUNT || !downbeatAvailable[deck]) return false;
		outFrame = downbeatFrame[deck];
		return true;
	}

	bool nextPhraseFrame(uint8_t deck, uint64_t /*currentFrame*/, uint64_t& outFrame) override{
		if(deck >= DJ_DECK_COUNT || !phraseAvailable[deck]) return false;
		outFrame = phraseFrame[deck];
		return true;
	}

	DjAssistIntentGenerations assistIntentGenerationsSnapshot() override{
		return generations;
	}

	// Mirrors DjSession::assistPurgePendingSystemCommands() exactly (see
	// DjSession.cpp) - reuses the same real DjCommandQueue::
	// removeSystemTargeting() this test's queue member already exercises
	// everywhere else.
	void assistPurgePendingSystemCommands(uint8_t deck) override{
		purgeCallCount++;
		DjCommand probe = {};
		probe.origin = DJ_ORIGIN_LOCAL_UI;
		probe.deck = deck;
		const DjCommandType purgedTypes[3] = { DJ_COMMAND_SET_PLAYING, DJ_COMMAND_SET_SYNC, DJ_COMMAND_SET_MIX };
		for(uint8_t typeIndex = 0; typeIndex < 3; ++typeIndex){
			probe.type = purgedTypes[typeIndex];
			uint32_t removedIds[DJ_COMMAND_CAPACITY] = {};
			const uint8_t removedCount = queue.removeSystemTargeting(probe, removedIds, DJ_COMMAND_CAPACITY);
			for(uint8_t i = 0; i < removedCount && i < DJ_COMMAND_CAPACITY; i++){
				results.finish(removedIds[i], DJ_COMMAND_SUPERSEDED, DJ_COMMAND_ERROR_NONE);
				if(tracked.tracked && tracked.id == removedIds[i]) tracked.status = DJ_COMMAND_SUPERSEDED;
			}
		}
	}

private:
	DjSubmitResult admit(DjCommand& command){
		command.id = nextCommandId++;
		return admitAssistCommand(command, queue, results, tracked, generations);
	}
};

DjTrackIdentity fingerprintIdentity(uint8_t seed){
	DjTrackIdentity identity = {};
	identity.flags = DJ_TRACK_IDENTITY_FINGERPRINT;
	memset(identity.fingerprint, seed, sizeof(identity.fingerprint));
	return identity;
}

void setupPlayingDeck(DjSnapshot& snapshot, uint8_t deck, uint8_t identitySeed, uint32_t bpmMilli){
	DjDeckSnapshot& d = snapshot.decks[deck];
	d.loaded = true;
	d.playing = true;
	d.metadata.state = DJ_METADATA_VALID;
	d.metadata.bpmMilli = bpmMilli;
	d.metadata.sourceSampleRate = 44100;
	d.metadata.sourceDurationFrames = 44100ULL * 300ULL;
	d.identity = fingerprintIdentity(identitySeed);
}

void setupLoadedStoppedDeck(DjSnapshot& snapshot, uint8_t deck, uint8_t identitySeed, uint32_t bpmMilli){
	DjDeckSnapshot& d = snapshot.decks[deck];
	d.loaded = true;
	d.playing = false;
	d.metadata.state = DJ_METADATA_VALID;
	d.metadata.bpmMilli = bpmMilli;
	d.metadata.sourceSampleRate = 44100;
	d.metadata.sourceDurationFrames = 44100ULL * 300ULL;
	d.identity = fingerprintIdentity(identitySeed);
}

DjAssistLibraryEntry makeEntry(uint32_t index, uint8_t seed){
	DjAssistLibraryEntry entry;
	entry.libraryIndex = index;
	entry.identity = fingerprintIdentity(seed);
	entry.state = DJ_METADATA_VALID;
	entry.capabilities = DJ_METADATA_HAS_BPM;
	entry.bpmMilli = 120000;
	entry.key = 5;
	entry.rating = 4;
	entry.durationFrames = 44100ULL * 200ULL;
	entry.sampleRate = 44100;
	return entry;
}

// Mirrors DjSession::loop()'s pop -> apply -> finish -> assistTracked-update
// sequence for exactly one queued command. `succeed` selects whether the
// simulated apply() mutates state and reports APPLIED, or reports FAILED
// without mutating anything - either way the durable tracked slot and the
// presentation ring are updated exactly like the real loop() does.
bool driveOneQueuedCommand(FakeAssistSessionPort& port, bool succeed = true){
	DjCommand command;
	if(!port.queue.pop(command)) return false;
	const DjCommandStatus status = succeed ? DJ_COMMAND_APPLIED : DJ_COMMAND_FAILED;
	if(succeed){
		switch(command.type){
			case DJ_COMMAND_SET_PLAYING:
				port.snapshot.decks[command.deck].playing = command.value != 0;
				break;
			case DJ_COMMAND_SET_SYNC:
				port.snapshot.decks[command.deck].sync.state = command.value != 0 ? DJ_SYNC_LOCKED : DJ_SYNC_OFF;
				break;
			case DJ_COMMAND_SET_MIX:
				port.snapshot.mix = uint8_t(command.value);
				break;
			default:
				break;
		}
	}
	port.results.finish(command.id, status, succeed ? DJ_COMMAND_ERROR_NONE : DJ_COMMAND_ERROR_INVALID_VALUE);
	if(port.tracked.tracked && port.tracked.id == command.id) port.tracked.status = status;
	return true;
}

// Bounded (queue capacity is fixed) full drain, applying every currently
// queued command in FIFO order.
uint8_t driveAllQueuedCommands(FakeAssistSessionPort& port, bool succeed = true){
	uint8_t drained = 0;
	while(driveOneQueuedCommand(port, succeed)) ++drained;
	return drained;
}

} // namespace

// Grants access to DjAssistController's private stepping methods (see the
// `friend class DjAssistIntegrationSelfCheck;` in DjAssistController.h) -
// adds no production API surface, changes no behavior. Only the handful of
// static helpers below actually touch private members; every test function
// after this class drives the controller entirely through its public API
// (begin()/tick()/armTransition()/cancelTransition()/copySnapshot()/end()).
class DjAssistIntegrationSelfCheck {
public:
	static void stepFill(DjAssistController& controller){
		controller.fillWorkerStep();
	}

	static bool tableReady(DjAssistController& controller, uint32_t& outGeneration){
		return controller.candidateTableReady(outGeneration);
	}

	// Bounded convergence: repeatedly steps the (never-actually-threaded)
	// background fill until the candidate table reports ready, or fails the
	// test outright if that never happens within a generous bound - the
	// real fillWorkerStep() always makes forward progress one record (or
	// one generation restart) per call, so this must terminate quickly for
	// any correctly-behaving build.
	static void fillCandidateTableToReady(DjAssistController& controller){
		uint32_t generation = 0;
		for(int i = 0; i < 100000; ++i){
			controller.fillWorkerStep();
			if(controller.candidateTableReady(generation)) return;
		}
		assert(false && "candidate table never became ready");
	}
};

namespace {

// Drives one full submit(+drain)+poll cycle of whichever transition step is
// currently pending, returning once the plan's currentStep actually
// advances (a step applied) or the transition reaches a terminal mode
// (FAILED/COMPLETE). The ARMED->RUNNING mode transition happens on the very
// same tick as the first step's submit - so "any mode change" is not by
// itself evidence that a step applied, only that the state machine is
// progressing towards it. Bounded (20 controller ticks) so a stuck/
// regressed state machine fails the test loudly instead of hanging.
void advanceOneStep(DjAssistController& controller, FakeAssistSessionPort& port){
	DjAssistSnapshot before;
	controller.copySnapshot(before);
	const uint8_t startStep = before.plan.currentStep;
	for(int i = 0; i < 20; ++i){
		controller.tick();
		driveAllQueuedCommands(port);
		DjAssistSnapshot after;
		controller.copySnapshot(after);
		if(after.mode == DJ_ASSIST_MODE_TRANSITION_FAILED || after.mode == DJ_ASSIST_MODE_TRANSITION_COMPLETE) return;
		if(after.plan.currentStep != startStep) return;
	}
	assert(false && "transition step never advanced");
}

// -- full default transition: arm -> boundary -> LOCK_TEMPO/ENABLE_SYNC -> --
// -- start -> crossfade -> stop, driven entirely through the real ---------
// -- controller/engine/actuator. -------------------------------------------
void testFullDefaultTransitionHappyPath(){
	FakeAssistSessionPort port;
	setupPlayingDeck(port.snapshot, 0, 1, 120000);
	setupLoadedStoppedDeck(port.snapshot, 1, 2, 128000);
	port.deckFrames[0] = 1000;
	port.downbeatAvailable[0] = true;
	port.downbeatFrame[0] = 5000;
	hostStubSetMicros(0);

	DjAssistController controller;
	controller.begin(&port);

	const DjTrackIdentity target = fingerprintIdentity(2);
	assert(controller.armTransition(0, 1, 7, target, 4, /*startAtBoundary=*/true, /*tempoLock=*/true));

	DjAssistSnapshot snap;
	controller.copySnapshot(snap);
	assert(snap.mode == DJ_ASSIST_MODE_TRANSITION_ARMED);
	assert(snap.plan.stepCount == 7); // WAIT_BOUNDARY, START_DECK, LOCK_TEMPO, ENABLE_SYNC, CROSSFADE, STOP_DECK, RELEASE_SYNC.

	// First tick: guard passes (ARMED -> RUNNING) but the boundary hasn't
	// been reached yet (fromDeck elapsed 1000 < the captured target 5000).
	controller.tick();
	controller.copySnapshot(snap);
	assert(snap.mode == DJ_ASSIST_MODE_TRANSITION_RUNNING);
	assert(snap.plan.currentStep == 0);

	// Cross the captured boundary, within DJ_QUANTIZE_TOLERANCE_FRAMES.
	port.deckFrames[0] = 5010;
	advanceOneStep(controller, port); // WAIT_BOUNDARY -> START_DECK
	controller.copySnapshot(snap);
	assert(snap.plan.currentStep == 1);

	advanceOneStep(controller, port); // START_DECK -> LOCK_TEMPO
	controller.copySnapshot(snap);
	assert(snap.plan.currentStep == 2);
	assert(snap.plan.toDeckStartOwnedByPlan);
	assert(port.snapshot.decks[1].playing);

	advanceOneStep(controller, port); // LOCK_TEMPO -> ENABLE_SYNC
	controller.copySnapshot(snap);
	assert(snap.plan.currentStep == 3);
	assert(snap.plan.toDeckSyncOwnedByPlan);
	assert(port.snapshot.decks[1].sync.state == DJ_SYNC_LOCKED);

	advanceOneStep(controller, port); // ENABLE_SYNC -> CROSSFADE (idempotent re-arm, must not fail).
	controller.copySnapshot(snap);
	assert(snap.plan.currentStep == 4);

	controller.tick(); // submits CROSSFADE, capturing the ramp's start time.
	hostStubAdvanceMicros(10000000); // far past a 4-beat ramp at 128 BPM (~1.875s).
	advanceOneStep(controller, port); // CROSSFADE -> STOP_DECK
	controller.copySnapshot(snap);
	assert(snap.plan.currentStep == 5);
	assert(port.snapshot.mix == 255); // toDeck == 1 -> full deck1 endpoint.

	advanceOneStep(controller, port); // STOP_DECK -> RELEASE_SYNC
	controller.copySnapshot(snap);
	assert(snap.plan.currentStep == 6);
	assert(!port.snapshot.decks[0].playing);

	advanceOneStep(controller, port); // RELEASE_SYNC -> COMPLETE
	controller.copySnapshot(snap);
	assert(snap.mode == DJ_ASSIST_MODE_TRANSITION_COMPLETE);
	assert(port.snapshot.decks[1].sync.state == DJ_SYNC_OFF);

	controller.end();
}

// -- cancel mid-transition: bounded rollback restores only plan-owned -----
// -- side effects (mix skipped - crossfade never submitted; sync released; -
// -- started deck stopped), and purges any still-queued plan-owned writes. -
void testCancelRollbackRestoresOnlyPlanOwnedState(){
	FakeAssistSessionPort port;
	setupPlayingDeck(port.snapshot, 0, 1, 120000);
	setupLoadedStoppedDeck(port.snapshot, 1, 2, 128000);
	port.snapshot.mix = 40;

	DjAssistController controller;
	controller.begin(&port);
	const DjTrackIdentity target = fingerprintIdentity(2);
	assert(controller.armTransition(0, 1, 3, target, 4, /*startAtBoundary=*/false, /*tempoLock=*/true));

	advanceOneStep(controller, port); // START_DECK applied.
	advanceOneStep(controller, port); // LOCK_TEMPO applied.
	DjAssistSnapshot snap;
	controller.copySnapshot(snap);
	assert(snap.mode == DJ_ASSIST_MODE_TRANSITION_RUNNING);
	assert(snap.plan.toDeckStartOwnedByPlan);
	assert(port.snapshot.decks[1].playing);

	controller.cancelTransition();
	controller.copySnapshot(snap);
	assert(snap.mode == DJ_ASSIST_MODE_TRANSITION_FAILED);
	assert(snap.plan.failure == DJ_ASSIST_FAIL_CANCELLED);

	for(int i = 0; i < 20; ++i){
		controller.tick();
		driveAllQueuedCommands(port);
	}
	assert(!port.snapshot.decks[1].playing); // STOP_DECK rollback undid the plan's own start.
	assert(port.snapshot.decks[1].sync.state == DJ_SYNC_OFF); // sync released.
	assert(port.snapshot.mix == 40); // MIX phase skipped (crossfade never submitted) - untouched.
	assert(port.purgeCallCount >= 1); // still-queued plan-owned writes purged exactly once.

	controller.end();
}

// -- issue #1: a user re-asserting play on the target deck AFTER the -------
// -- plan's own START_DECK step already applied must relinquish rollback --
// -- ownership of that property, so a subsequent rollback never stops the -
// -- user's own newer intent. -----------------------------------------------
void testUserPlayDivergenceRelinquishesRollbackOwnership(){
	FakeAssistSessionPort port;
	setupPlayingDeck(port.snapshot, 0, 1, 120000);
	setupLoadedStoppedDeck(port.snapshot, 1, 2, 128000);

	DjAssistController controller;
	controller.begin(&port);
	const DjTrackIdentity target = fingerprintIdentity(2);
	assert(controller.armTransition(0, 1, 3, target, 4, /*startAtBoundary=*/false, /*tempoLock=*/false));

	advanceOneStep(controller, port); // START_DECK applied -> CROSSFADE current.
	DjAssistSnapshot snap;
	controller.copySnapshot(snap);
	assert(snap.plan.toDeckStartOwnedByPlan);
	assert(port.snapshot.decks[1].playing);

	// The user re-asserts play on the target deck themselves, after the
	// plan's own step already applied.
	port.setPlaying(1, true, DJ_ORIGIN_LOCAL_UI);
	driveAllQueuedCommands(port);

	controller.tick(); // guardOk() sees playIntentGeneration[toDeck] diverged.
	controller.copySnapshot(snap);
	assert(snap.mode == DJ_ASSIST_MODE_TRANSITION_FAILED);
	assert(snap.plan.failure == DJ_ASSIST_FAIL_MANUAL_OVERRIDE);
	assert(!snap.plan.toDeckStartOwnedByPlan); // ownership relinquished (issue #1 fix).

	for(int i = 0; i < 20; ++i){
		controller.tick();
		driveAllQueuedCommands(port);
	}
	// Rollback must never have stopped the deck: the user's own later
	// intent is authoritative now, not the plan.
	assert(port.snapshot.decks[1].playing);

	controller.end();
}

// -- issue #2: a non-system sync command on the SOURCE deck (never plan- --
// -- owned) is still a genuine manual override and must abort the ---------
// -- transition rather than silently continue past it. ---------------------
void testFromDeckSyncDivergenceAbortsTransition(){
	FakeAssistSessionPort port;
	setupPlayingDeck(port.snapshot, 0, 1, 120000);
	setupLoadedStoppedDeck(port.snapshot, 1, 2, 128000);

	DjAssistController controller;
	controller.begin(&port);
	const DjTrackIdentity target = fingerprintIdentity(2);
	assert(controller.armTransition(0, 1, 3, target, 4, /*startAtBoundary=*/false, /*tempoLock=*/false));

	controller.tick(); // ARMED -> RUNNING, submits START_DECK.

	port.setSync(0, true, 1, DJ_ORIGIN_LOCAL_UI); // user arms sync on the SOURCE deck.
	driveAllQueuedCommands(port);

	controller.tick();
	DjAssistSnapshot snap;
	controller.copySnapshot(snap);
	assert(snap.mode == DJ_ASSIST_MODE_TRANSITION_FAILED);
	assert(snap.plan.failure == DJ_ASSIST_FAIL_MANUAL_OVERRIDE);

	controller.end();
}

// -- manual mix admitted mid-crossfade must win over Assist's own ramp: ----
// -- the transition aborts, and rollback's MIX phase must never restore ---
// -- armedMix over the user's own later action. -----------------------------
void testManualMixDuringCrossfadeAbortsTransition(){
	FakeAssistSessionPort port;
	setupPlayingDeck(port.snapshot, 0, 1, 120000);
	setupLoadedStoppedDeck(port.snapshot, 1, 2, 128000);
	hostStubSetMicros(0);

	DjAssistController controller;
	controller.begin(&port);
	const DjTrackIdentity target = fingerprintIdentity(2);
	// A long (32-beat) ramp so the very first poll doesn't already saturate
	// at the endpoint - the manual mix must land mid-ramp.
	assert(controller.armTransition(0, 1, 3, target, 32, /*startAtBoundary=*/false, /*tempoLock=*/false));

	advanceOneStep(controller, port); // START_DECK -> CROSSFADE
	DjAssistSnapshot snap;
	controller.copySnapshot(snap);
	assert(snap.plan.currentStep == 1);

	controller.tick(); // submits CROSSFADE (captures ramp start time).
	hostStubAdvanceMicros(100000); // a small step into a long ramp - not yet at the endpoint.
	controller.tick(); // polls: computes an intermediate value, fire-and-forget submit (still queued).

	port.setMix(10, DJ_ORIGIN_LOCAL_UI); // the user's own hand on the crossfader, mid-ramp.
	driveAllQueuedCommands(port);

	controller.tick(); // guardOk() sees mixIntentGeneration diverged.
	controller.copySnapshot(snap);
	assert(snap.mode == DJ_ASSIST_MODE_TRANSITION_FAILED);
	assert(snap.plan.failure == DJ_ASSIST_FAIL_MANUAL_OVERRIDE);
	assert(port.snapshot.mix == 10); // never silently overwritten by the pending Assist ramp value.

	for(int i = 0; i < 20; ++i){
		controller.tick();
		driveAllQueuedCommands(port);
	}
	assert(port.snapshot.mix == 10); // rollback's MIX phase skipped (manualMixOccurred == true).

	controller.end();
}

// -- queue-wide origin policy / durable-slot supersede, exercised through --
// -- the real, shared admitAssistCommand() coordinator (not reimplemented -
// -- here) - the same function DjSession::submit() calls. ------------------
void testQueueSupersedeAndRemovalThroughRealAdmission(){
	FakeAssistSessionPort port;

	DjSubmitResult first = port.setMix(50, DJ_ORIGIN_SYSTEM);
	assert(first.accepted());
	port.assistTrackCommand(first.id);

	// A manual mix command purges the still-queued system one and takes
	// its place - the durable tracked slot reflects SUPERSEDED, not just
	// the evictable presentation ring.
	DjSubmitResult manual = port.setMix(10, DJ_ORIGIN_LOCAL_UI);
	assert(manual.accepted());
	assert(port.assistTrackedStatus(first.id) == DJ_COMMAND_SUPERSEDED);
	assert(port.queue.depth() == 1);

	// A later system (Assist) command targeting the same channel is
	// rejected outright while the manual one is still pending.
	DjSubmitResult systemAfter = port.setMix(20, DJ_ORIGIN_SYSTEM);
	assert(systemAfter.status == DJ_COMMAND_REJECTED);
	assert(systemAfter.error == DJ_COMMAND_ERROR_ASSIST_OVERRIDE_PENDING);

	driveAllQueuedCommands(port);
	assert(port.snapshot.mix == 10); // the manual command is what actually applied.
}

// -- issue #3: a metadata refresh landing exactly between the unlocked ----
// -- entry read and the in-lock live-generation re-check must be caught: --
// -- the record is discarded (not committed), the fill restarts for the --
// -- new generation, and eventually converges cleanly. ---------------------
void testCandidateFillDiscardsRecordAcrossGenerationRace(){
	FakeAssistSessionPort port;
	port.entryCount = 3;
	for(uint32_t i = 0; i < port.entryCount; ++i){
		port.entries[i] = makeEntry(i, uint8_t(i + 10));
	}
	port.generation = 1;

	DjAssistController controller;
	controller.begin(&port);

	DjAssistIntegrationSelfCheck::stepFill(controller); // commits record 0 under generation 1.
	uint32_t generation = 0;
	assert(!DjAssistIntegrationSelfCheck::tableReady(controller, generation)); // 2 records still pending.

	// Inject a refresh landing in the gap fillWorkerStep() must survive:
	// record 1's unlocked read succeeds under revision 1, but the live
	// generation has already moved to 2 by the time the worker reacquires
	// candidateMutex_ for the commit.
	port.bumpGenerationOnReadIndex = 1;
	DjAssistIntegrationSelfCheck::stepFill(controller);
	assert(port.generation == 2); // confirms the injected race actually happened.
	assert(!DjAssistIntegrationSelfCheck::tableReady(controller, generation)); // never wrongly reports ready off a stale/discarded record.

	// The fill must restart cleanly for generation 2 and eventually
	// converge - bounded, deterministic re-reads only.
	DjAssistIntegrationSelfCheck::fillCandidateTableToReady(controller);
	assert(DjAssistIntegrationSelfCheck::tableReady(controller, generation));
	assert(generation == 2);

	controller.end();
}

} // namespace

int main(){
	testFullDefaultTransitionHappyPath();
	testCancelRollbackRestoresOnlyPlanOwnedState();
	testUserPlayDivergenceRelinquishesRollbackOwnership();
	testFromDeckSyncDivergenceAbortsTransition();
	testManualMixDuringCrossfadeAbortsTransition();
	testQueueSupersedeAndRemovalThroughRealAdmission();
	testCandidateFillDiscardsRecordAcrossGenerationRace();
	return 0;
}
