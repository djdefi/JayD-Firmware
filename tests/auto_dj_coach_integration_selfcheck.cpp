// Full "real Auto DJ + Coach + DjSession-shaped ports" integration test
// (fix #5 independent review requirement: "Test full two-entry run through
// real Auto+Coach+DjSession ports and failures at every stage").
//
// Unlike tests/auto_dj_session_bridge_selfcheck.cpp (a real
// AutoDjSessionActuator against a FAKE AutoDjSessionPort that simply
// returns a controllable DjAssistMode string), this file drives BOTH the
// real DjAssistController (Coach) AND the real AutoDjSessionActuator (Auto
// DJ) against ONE shared hand-written port that implements both
// DjAssistSessionPort and AutoDjSessionPort - exactly mirroring how the
// real DjSession implements both interfaces on a single object (see
// DjSession.h/.cpp) - with a loopOnce() driver that reproduces
// DjSession::loop()'s own pop-one-command / apply / tick-Assist / tick-
// AutoDj sequence, reusing the real admitAssistCommand()/
// djIsAutoDjTakeoverSignal()/removeAutoDjOwned()/djBumpAutoDjManualIntent()
// free functions DjSession::submit() itself calls. This means Auto DJ's
// internal Coach-arm command is applied by actually calling
// DjAssistController::armTransition(), and Auto DJ's composite pollLoad()
// observes Coach's transition genuinely progressing (or failing) through
// its own real WAIT_BOUNDARY/START_DECK/LOCK_TEMPO/ENABLE_SYNC/CROSSFADE/
// STOP_DECK/RELEASE_SYNC plan - not a fake mode string set directly by the
// test.
//
// Build/run directly, e.g.:
//
//   g++ -std=c++11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
//       -DJAYD_ASSIST_HOST_TASK_SEAM -pthread \
//       -I tests/host_stubs \
//       tests/auto_dj_coach_integration_selfcheck.cpp \
//       tests/host_stubs/host_arduino_shim.cpp \
//       src/DjAssist/DjAssistController.cpp src/DjAssist/DjAssistFillWorker.cpp \
//       src/DjAssist/DjAssistScoring.cpp src/DjAssist/DjAssistSessionBridge.cpp \
//       src/DjAssist/DjAssistEngine.cpp src/AutoDj/DjAutoDjPlanner.cpp \
//       -o auto_dj_coach_integration_selfcheck && ./auto_dj_coach_integration_selfcheck

#include <assert.h>
#include <string.h>
#include <vector>

#include "../src/AutoDj/AutoDjSessionActuator.h"
#include "../src/DjAssist/DjAssistController.h"
#include "Arduino.h" // hostStubSetMicros()/hostStubAdvanceMicros() - real CROSSFADE ramp timing.

namespace {

DjTrackIdentity fingerprintIdentity(uint8_t seed){
	DjTrackIdentity identity = {};
	identity.flags = DJ_TRACK_IDENTITY_FINGERPRINT;
	memset(identity.fingerprint, seed, sizeof(identity.fingerprint));
	return identity;
}

// Shared test double standing in for DjSession itself: implements BOTH
// DjAssistSessionPort and AutoDjSessionPort on one object (identical
// inheritance shape to the real DjSession - see DjSession.h), backed by one
// real DjCommandQueue/DjCommandResults and the SAME two tracked-command
// slots (assistTracked/autoDjTracked) DjSession itself owns, so submit()
// below is a faithful copy of DjSession::submit()'s own body (manual-
// takeover purge, admitAssistCommand(), manual-intent-generation bump).
class FakePort : public DjAssistSessionPort, public AutoDjSessionPort {
public:
	struct Entry {
		DjAssistLibraryEntry entry;
		uint32_t artistHash = 0;
		uint32_t titleHash = 0;
	};

	// -- Shared command plumbing (mirrors DjSession's own members). --
	DjCommandQueue queue;
	DjCommandResults results;
	DjAssistTrackedCommand assistTracked;
	DjAssistTrackedCommand autoDjTracked;
	DjAssistIntentGenerations assistGenerations;
	AutoDjManualIntentGenerations autoDjManualGenerations;
	uint32_t nextCommandId = 1;
	// Set once, right after construction, once the controller this port
	// backs exists - mirrors DjSession owning both assistController and
	// this port on the same object; autoDjCoachTransitionMode() needs it to
	// read Coach's live mode, exactly like DjSession::autoDjCoachTransitionMode().
	DjAssistController* controller = nullptr;
	// Every command popped from `queue` and applied, in order - lets a
	// test assert on the exact sequence/interleaving of commands actually
	// executed (e.g. proving a specific rollback step genuinely ran, and
	// in what order relative to a later retry's own commands), not just
	// on a final snapshot that a self-healing later step could mask.
	std::vector<DjCommand> poppedLog;

	// -- Library/candidate state. --
	Entry entries[4];
	uint32_t entryCount = 0;
	uint32_t metadataRevision = 1;
	uint32_t libraryGeneration = 1;
	DjAssistDeckContext deckContext;
	uint8_t targetDeckOverride = 1;
	bool media = true;
	bool physicalConfirmationPending = false;
	// Deliberate fault injection for the "load resolves to nothing" stage
	// (mirrors resolveIdentityPath() rejecting an unknown/ambiguous
	// fingerprint) - never fabricates a path, just fails the load cleanly.
	bool forceLoadUnresolved = false;

	// -- DjSnapshot backing store (decks, mix, recording, sessionActive). --
	DjSnapshot snapshot;

	// -- DjAssistController boundary/timing plumbing (mirrors
	// FakeAssistSessionPort in tests/dj_assist_integration_selfcheck.cpp). --
	uint64_t deckFrames[DJ_DECK_COUNT] = {};
	bool downbeatAvailable[DJ_DECK_COUNT] = {};
	uint64_t downbeatFrame[DJ_DECK_COUNT] = {};

	int purgeCallCount = 0;
	int armAttemptCount = 0; // every time an ASSIST_ARM_TRANSITION command is actually applied (accepted or rejected).

	// -- DjAssistSessionPort --

	DjSubmitResult setPlaying(uint8_t deck, bool playing, DjCommandOrigin origin, bool autoDjOwned = false) override{
		DjCommand command = {};
		command.origin = origin;
		command.type = DJ_COMMAND_SET_PLAYING;
		command.deck = deck;
		command.value = playing ? 1 : 0;
		command.autoDjOwned = autoDjOwned;
		return submit(command);
	}

	DjSubmitResult setSync(
		uint8_t deck, bool armed, int8_t masterDeck, DjCommandOrigin origin, bool autoDjOwned = false
	) override{
		DjCommand command = {};
		command.origin = origin;
		command.type = DJ_COMMAND_SET_SYNC;
		command.deck = deck;
		command.value = armed ? 1 : 0;
		command.slot = masterDeck < 0 ? 0 : uint8_t(masterDeck) + 1;
		command.autoDjOwned = autoDjOwned;
		return submit(command);
	}

	DjSubmitResult setMix(uint8_t mix, DjCommandOrigin origin, bool autoDjOwned = false) override{
		DjCommand command = {};
		command.origin = origin;
		command.type = DJ_COMMAND_SET_MIX;
		command.value = mix;
		command.autoDjOwned = autoDjOwned;
		return submit(command);
	}

	void assistTrackCommand(uint32_t commandId) override{
		assistTracked.id = commandId;
		assistTracked.tracked = true;
		assistTracked.status = DJ_COMMAND_ACCEPTED;
	}

	DjCommandStatus assistTrackedStatus(uint32_t commandId) override{
		if(assistTracked.tracked && assistTracked.id == commandId) return assistTracked.status;
		return DJ_COMMAND_PENDING;
	}

	bool copySnapshot(DjSnapshot& out) override{
		snapshot.queueDepth = queue.depth();
		results.copyTo(snapshot.recentResults);
		out = snapshot;
		return true;
	}

	uint32_t assistMetadataRevision() override{ return metadataRevision; }

	uint32_t assistTrackCount() override{ return entryCount; }

	bool assistTrackEntry(uint32_t index, DjAssistLibraryEntry& outEntry, uint32_t& outRevision) override{
		outRevision = metadataRevision;
		if(index >= entryCount) return false;
		outEntry = entries[index].entry;
		return true;
	}

	// Fake on-demand grid-anchor read (see DjAssistGridCache): same
	// all-or-nothing capability gate as the real DjSession::
	// assistTrackGridAnchors(), but returns one fixed deterministic anchor
	// instead of a real readGrid() burst.
	int assistTrackGridAnchorsCallCount = 0;
	bool assistTrackGridAnchors(
		uint32_t index, DjGridAnchor* outAnchors, uint16_t& outAnchorCount, uint32_t& outRevision
	) override{
		++assistTrackGridAnchorsCallCount;
		outRevision = metadataRevision;
		outAnchorCount = 0;
		if(index >= entryCount) return false;
		const DjAssistLibraryEntry& entry = entries[index].entry;
		const uint16_t required = DJ_METADATA_HAS_GRID | DJ_METADATA_HAS_SOURCE_FRAMES | DJ_METADATA_HAS_BPM;
		if((entry.capabilities & required) != required) return false;
		if(outAnchors == nullptr) return false;
		outAnchors[0].frame = 0;
		outAnchors[0].quarterBeat = 0;
		outAnchorCount = 1;
		return true;
	}

	bool mediaPresent() const override{ return media; }

	uint64_t deckElapsedFrames(uint8_t deck) const override{
		return deck < DJ_DECK_COUNT ? deckFrames[deck] : 0;
	}

	bool nextDownbeatFrame(uint8_t deck, uint64_t /*currentFrame*/, uint64_t& outFrame) const override{
		if(deck >= DJ_DECK_COUNT || !downbeatAvailable[deck]) return false;
		outFrame = downbeatFrame[deck];
		return true;
	}

	bool nextPhraseFrame(uint8_t /*deck*/, uint64_t /*currentFrame*/, uint64_t& /*outFrame*/) override{
		return false; // not exercised by this test (startAtBoundary uses the downbeat only).
	}

	DjAssistIntentGenerations assistIntentGenerationsSnapshot() override{ return assistGenerations; }

	// Mirrors DjSession::assistPurgePendingSystemCommands() exactly.
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
				if(assistTracked.tracked && assistTracked.id == removedIds[i]) assistTracked.status = DJ_COMMAND_SUPERSEDED;
			}
		}
	}

	// -- AutoDjSessionPort --

	uint32_t autoDjCandidateCount() override{ return entryCount; }

	bool autoDjCandidateEntry(
		uint32_t index, DjAssistLibraryEntry& outEntry,
		uint32_t& outArtistHash, uint32_t& outTitleHash, uint32_t& outRevision
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
	uint8_t autoDjTargetDeck() override{ return targetDeckOverride; }

	DjSubmitResult autoDjLoadDeckByIdentity(
		uint8_t deck, const DjTrackIdentity& identity,
		uint32_t identityLibraryGeneration, uint32_t identityMetadataRevision
	) override{
		DjCommand command = {};
		command.origin = DJ_ORIGIN_SYSTEM;
		command.type = DJ_COMMAND_LOAD_DECK;
		command.deck = deck;
		command.libraryGeneration = identityLibraryGeneration;
		command.metadataRevision = identityMetadataRevision;
		command.trackIdentity = identity;
		command.autoDjOwned = true;
		const DjSubmitResult result = submit(command);
		// Mirrors DjSession::loadDeckByIdentity(): fire-and-forget grid-
		// anchor hydration request for this exact key, submitted only
		// after a successful admit - see DjAssistGridCache's doc comment.
		if(result.status == DJ_COMMAND_ACCEPTED && controller != nullptr){
			controller->requestGridHydration(identityLibraryGeneration, identityMetadataRevision, identity);
		}
		return result;
	}

	void autoDjTrackCommand(uint32_t commandId) override{
		autoDjTracked.id = commandId;
		autoDjTracked.tracked = true;
		autoDjTracked.status = DJ_COMMAND_ACCEPTED;
	}

	DjCommandStatus autoDjCommandStatus(uint32_t commandId) override{
		if(autoDjTracked.tracked && autoDjTracked.id == commandId) return autoDjTracked.status;
		return DJ_COMMAND_PENDING;
	}

	DjSubmitResult autoDjArmCoachTransition(
		uint8_t fromDeck, uint8_t toDeck, const DjTrackIdentity& targetIdentity,
		uint8_t crossfadeBeats, bool startAtBoundary, bool tempoLock
	) override{
		DjCommand command = {};
		command.origin = DJ_ORIGIN_SYSTEM;
		command.type = DJ_COMMAND_ASSIST_ARM_TRANSITION;
		command.deck = fromDeck;
		command.slot = toDeck;
		command.libraryIndex = 0;
		command.trackIdentity = targetIdentity;
		command.value = uint16_t(
			crossfadeBeats | (startAtBoundary ? (1 << 8) : 0) | (tempoLock ? (1 << 9) : 0)
		);
		command.autoDjOwned = true;
		return submit(command);
	}

	DjSubmitResult autoDjCancelCoachTransition() override{
		DjCommand command = {};
		command.origin = DJ_ORIGIN_SYSTEM;
		command.type = DJ_COMMAND_ASSIST_CANCEL_TRANSITION;
		command.autoDjOwned = true;
		return submit(command);
	}

	DjAssistMode autoDjCoachTransitionMode() override{
		if(!controller) return DJ_ASSIST_MODE_OFF;
		DjAssistSnapshot snap;
		controller->copySnapshot(snap);
		return snap.mode;
	}

	AutoDjManualIntentGenerations autoDjManualIntentGenerationsSnapshot() override{
		return autoDjManualGenerations;
	}

	bool autoDjConsumePhysicalConfirmation() override{
		const bool value = physicalConfirmationPending;
		physicalConfirmationPending = false;
		return value;
	}

	bool autoDjCoachTransitionSettled() override{
		if(!controller) return true;
		return controller->rollbackSettled();
	}

	// Real controller wiring (not a test-controllable flag) - this
	// harness exercises the actual DjAssistController::authorityReady()
	// signal end-to-end so allocation/worker-launch-failure tests prove
	// arm()/start()/tick() genuinely see it degrade, not merely that a
	// mock flag can be flipped.
	bool autoDjStableIdAuthorityReady() override{
		if(!controller) return false;
		return controller->authorityReady();
	}

	uint64_t autoDjNowMicros() const override{
		return uint64_t(micros());
	}

	// -- Shared submission path: a faithful copy of DjSession::submit()'s
	// own body (manual-takeover purge before admission, admitAssistCommand()
	// for the real FIFO/supersede/priority bookkeeping, then the manual-
	// intent-generation bump) - every command in this test (Coach's own
	// SET_PLAYING/SET_SYNC/SET_MIX plan steps, Auto DJ's stable-ID load, and
	// Auto DJ's internal Coach-arm/cancel) goes through this ONE path,
	// exactly as it would through the real DjSession. --
	DjSubmitResult submit(DjCommand command){
		command.id = nextCommandId++;
		if(command.origin != DJ_ORIGIN_SYSTEM && djIsAutoDjTakeoverSignal(command)){
			uint32_t removedIds[DJ_COMMAND_CAPACITY] = {};
			const uint8_t removedCount = queue.removeAutoDjOwned(removedIds, DJ_COMMAND_CAPACITY);
			for(uint8_t i = 0; i < removedCount && i < DJ_COMMAND_CAPACITY; i++){
				results.finish(removedIds[i], DJ_COMMAND_SUPERSEDED, DJ_COMMAND_ERROR_NONE);
				if(autoDjTracked.tracked && autoDjTracked.id == removedIds[i]) autoDjTracked.status = DJ_COMMAND_SUPERSEDED;
				if(assistTracked.tracked && assistTracked.id == removedIds[i]) assistTracked.status = DJ_COMMAND_SUPERSEDED;
			}
		}
		const DjSubmitResult result = admitAssistCommand(command, queue, results, assistTracked, assistGenerations);
		if(result.status == DJ_COMMAND_ACCEPTED) djBumpAutoDjManualIntent(autoDjManualGenerations, command);
		return result;
	}
};

DjAssistLibraryEntry makeEntry(uint8_t fingerprintByte, uint32_t bpmMilli = 128000, uint16_t key = 0x801){
	DjAssistLibraryEntry entry;
	entry.identity = fingerprintIdentity(fingerprintByte);
	entry.state = DJ_METADATA_VALID;
	entry.capabilities = DJ_METADATA_HAS_BPM | DJ_METADATA_HAS_KEY;
	entry.bpmMilli = bpmMilli;
	entry.key = key;
	entry.rating = 3;
	entry.durationFrames = 44100ULL * 200;
	entry.sampleRate = 44100;
	return entry;
}

void setDeckPlaying(FakePort& port, uint8_t deck, uint16_t elapsed, uint16_t duration, uint8_t identitySeed, uint32_t bpmMilli){
	port.snapshot.sessionActive = true;
	DjDeckSnapshot& d = port.snapshot.decks[deck];
	d.loaded = true;
	d.playing = true;
	d.elapsed = elapsed;
	d.duration = duration;
	d.timingQuality = DJ_TIMING_COARSE;
	d.metadata.state = DJ_METADATA_VALID;
	d.metadata.bpmMilli = bpmMilli;
	d.metadata.sourceSampleRate = 44100;
	d.metadata.sourceDurationFrames = 44100ULL * 300ULL;
	d.identity = fingerprintIdentity(identitySeed);
}

// -- One production-shaped iteration: pop+apply exactly one queued
// command (mirroring DjSession::loop()'s own single-pop-per-tick contract
// and its apply()/finish()/tracked-slot-update sequence), then Coach's
// tick() (real engine), then Auto DJ's tick() (real actuator) - in that
// exact order, matching DjSession::loop()'s documented ordering
// requirement (tickAssist() before tickAutoDj()). --
void loopOnce(FakePort& port, DjAssistController& controller, AutoDjSessionActuator& actuator){
	DjCommand command;
	if(port.queue.pop(command)){
		port.poppedLog.push_back(command);
		DjCommandError error = DJ_COMMAND_ERROR_NONE;
		bool applied = true;
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
			case DJ_COMMAND_LOAD_DECK: {
				// Stable-ID resolve against the fake in-memory candidate
				// table (RAM-only, mirrors DjSession::loadDeckByIdentity()/
				// resolveIdentityPath()'s contract - fix #1 - at the
				// granularity this test needs): reject a stale epoch or an
				// unknown fingerprint, never fabricate a path.
				bool matches = false;
				if(!port.forceLoadUnresolved &&
						command.libraryGeneration == port.libraryGeneration &&
						command.metadataRevision == port.metadataRevision){
					for(uint32_t i = 0; i < port.entryCount; i++){
						if(memcmp(port.entries[i].entry.identity.fingerprint,
								command.trackIdentity.fingerprint, sizeof(command.trackIdentity.fingerprint)) == 0){
							matches = true;
							break;
						}
					}
				}
				if(!matches){
					error = DJ_COMMAND_ERROR_LIBRARY_IDENTITY_UNRESOLVED;
					applied = false;
					break;
				}
				DjDeckSnapshot& d = port.snapshot.decks[command.deck];
				d.loaded = true;
				d.playing = false;
				d.sync.state = DJ_SYNC_OFF;
				d.identity = command.trackIdentity;
				d.metadata.state = DJ_METADATA_VALID;
				d.metadata.bpmMilli = 128000;
				d.metadata.sourceSampleRate = 44100;
				d.metadata.sourceDurationFrames = 44100ULL * 300ULL;
				break;
			}
			case DJ_COMMAND_ASSIST_ARM_TRANSITION: {
				port.armAttemptCount++;
				const uint8_t crossfadeBeats = uint8_t(command.value & 0xFF);
				const bool startAtBoundary = (command.value & (1 << 8)) != 0;
				const bool tempoLock = (command.value & (1 << 9)) != 0;
				if(!controller.armTransition(
					command.deck, command.slot, command.libraryIndex, command.trackIdentity,
					crossfadeBeats, startAtBoundary, tempoLock, command.autoDjOwned
				)){
					error = DJ_COMMAND_ERROR_ASSIST_REJECTED;
					applied = false;
				}
				break;
			}
			case DJ_COMMAND_ASSIST_CANCEL_TRANSITION:
				controller.cancelTransition();
				break;
			default:
				break;
		}
		const DjCommandStatus status = applied ? DJ_COMMAND_APPLIED : DJ_COMMAND_FAILED;
		port.results.finish(command.id, status, error);
		if(port.assistTracked.tracked && port.assistTracked.id == command.id) port.assistTracked.status = status;
		if(port.autoDjTracked.tracked && port.autoDjTracked.id == command.id) port.autoDjTracked.status = status;
	}
	controller.tick(); // Coach first (matches DjSession::loop()'s real ordering).
	actuator.tick();   // Auto DJ second.
	// The CROSSFADE step is a genuine wall-clock ramp (DjAssistBridge::
	// computeCrossfadeMix() reads micros(), see DjAssistController.cpp) -
	// advance the host clock a fixed amount per iteration so it converges
	// within loopUntil()'s bounded iteration count instead of depending on
	// how fast this process happens to execute.
	hostStubAdvanceMicros(500000);
}

// Bounded convergence helper: keeps calling loopOnce() until `predicate`
// is true or the bound is exhausted, failing loudly rather than hanging -
// same "no infinite loop" discipline every other self-check in this
// codebase uses.
template <typename Predicate>
void loopUntil(FakePort& port, DjAssistController& controller, AutoDjSessionActuator& actuator, Predicate predicate, int maxIterations = 64){
	for(int i = 0; i < maxIterations; i++){
		if(predicate()) return;
		loopOnce(port, controller, actuator);
	}
	assert(predicate() && "loopUntil() bound exhausted without reaching the expected condition");
}

void pinTrack(AutoDjSessionActuator& actuator, uint8_t fingerprintByte, uint32_t libraryGeneration = 1){
	AutoDjIdentity identity;
	identity.flags = AUTO_DJ_IDENTITY_FINGERPRINT;
	identity.libraryGeneration = libraryGeneration;
	memset(identity.fingerprint, fingerprintByte, sizeof(identity.fingerprint));
	assert(actuator.pinTrack(identity, 0, 0));
}

// -- Test 1: full two-entry run through the REAL DjAssistController and
// REAL AutoDjSessionActuator, both wired to one shared port exactly as
// DjSession wires them in production. Two Auto DJ entries resolve in turn -
// each one a genuine RAM stable-ID load, a genuine Coach arm, and a
// genuine Coach transition (WAIT_BOUNDARY -> START_DECK -> LOCK_TEMPO ->
// ENABLE_SYNC -> CROSSFADE -> STOP_DECK -> RELEASE_SYNC) driven entirely by
// the unmodified engine - proving Auto DJ's composite pollLoad() correctly
// observes a REAL transition complete, not a fake mode string. --
void testFullTwoEntryRunThroughRealPorts(){
	hostStubSetMicros(0);
	FakePort port;
	port.entries[0].entry = makeEntry(0x11);
	port.entries[1].entry = makeEntry(0x22);
	port.entryCount = 2;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	// Deck 0 already playing (about to run out - 10s remaining hits the
	// end margin immediately so the first entry submits promptly); deck 1
	// is the load target.
	setDeckPlaying(port, 0, 190, 200, 0x99, 128000);
	port.targetDeckOverride = 1;
	port.deckFrames[0] = 1000;
	port.downbeatAvailable[0] = true;
	port.downbeatFrame[0] = 1000; // boundary already reached - crosses immediately.

	DjAssistController controller;
	controller.begin(&port);
	port.controller = &controller;

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	assert(actuator.state() == AutoDjState::Armed);
	loopOnce(port, controller, actuator); // top up the queue while Armed.
	assert(actuator.start());
	assert(actuator.state() == AutoDjState::Running);

	// -- Entry 1: 0x11 loads onto deck 1, Coach transitions deck 0 -> 1. --
	AutoDjSnapshot snap;
	loopUntil(port, controller, actuator, [&]{
		actuator.copySnapshot(snap);
		return snap.historySize == 1;
	});
	assert(port.snapshot.decks[1].loaded);
	assert(memcmp(port.snapshot.decks[1].identity.fingerprint, port.entries[0].entry.identity.fingerprint, 16) == 0);
	assert(port.snapshot.decks[1].playing); // Coach's own START_DECK step actually ran.
	assert(!port.snapshot.decks[0].playing); // and its own STOP_DECK step actually ran.
	DjAssistSnapshot coachSnap;
	controller.copySnapshot(coachSnap);
	assert(coachSnap.mode == DJ_ASSIST_MODE_TRANSITION_COMPLETE); // transition fully
	// resolved - DjAssistEngine has no automatic "revert to idle" step;
	// TRANSITION_COMPLETE simply behaves as an idle mode too (armTransition()'s
	// own mode guard only blocks ARMED/RUNNING, not COMPLETE), which is
	// exactly what lets the very next arm below re-enter cleanly.

	// -- Entry 2: pin 0x22 explicitly (deck 0 is now the load target,
	// currently stopped/loaded-nothing) and let it run to completion too -
	// proves the composite workflow repeats cleanly for a second entry,
	// not just once. --
	port.targetDeckOverride = 0;
	// Deck 1 is now the active/playing deck (per the just-completed
	// transition); give it a trustworthy near-end duration so
	// currentTrackAtEnd() fires promptly for entry 2 as well.
	port.snapshot.decks[1].elapsed = 190;
	port.snapshot.decks[1].duration = 200;
	port.snapshot.decks[1].timingQuality = DJ_TIMING_COARSE;
	port.deckFrames[1] = 1000;
	port.downbeatAvailable[1] = true;
	port.downbeatFrame[1] = 1000;
	port.downbeatAvailable[0] = false;
	pinTrack(actuator, 0x22);

	loopUntil(port, controller, actuator, [&]{
		actuator.copySnapshot(snap);
		return snap.historySize == 2;
	});
	assert(port.snapshot.decks[0].loaded);
	assert(memcmp(port.snapshot.decks[0].identity.fingerprint, port.entries[1].entry.identity.fingerprint, 16) == 0);
	assert(port.snapshot.decks[0].playing);
	assert(!port.snapshot.decks[1].playing);
	assert(actuator.state() == AutoDjState::Running);

	controller.end();
}

// -- Test 2 (fix #5 review, "failures at every stage"): a genuine manual
// play re-assertion on the target deck, submitted through the SAME real
// submit()/admitAssistCommand() path DjSession itself uses, while Coach's
// transition is actively RUNNING. This must (a) trip Coach's own guard
// (a real engine-level manual-override failure, not a fake mode string)
// and (b) independently trip Auto DJ's manualTakeoverActive() (the manual
// signal bumps autoDjManualIntentGenerations too - djIsAutoDjManualSignal()
// covers SET_PLAYING), which proactively cancels Auto's own Coach-arm
// tracking and pauses - proving the two independently-keyed mechanisms
// resolve consistently together against real production logic, not just
// a fake port double. --
void testManualPlayDivergenceMidTransitionStopsBothCoachAndAutoDj(){
	hostStubSetMicros(0);
	FakePort port;
	port.entries[0].entry = makeEntry(0x33);
	port.entryCount = 1;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	setDeckPlaying(port, 0, 190, 200, 0x99, 128000);
	port.targetDeckOverride = 1;
	port.deckFrames[0] = 1000;
	port.downbeatAvailable[0] = true;
	port.downbeatFrame[0] = 1000;

	DjAssistController controller;
	controller.begin(&port);
	port.controller = &controller;

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	loopOnce(port, controller, actuator);
	assert(actuator.start());

	// Drive to TransitionInFlight (arm applied, Coach past ARMED into
	// RUNNING - the boundary step already applied since downbeatFrame ==
	// deckFrames[0] from tick 1).
	loopUntil(port, controller, actuator, [&]{
		DjAssistSnapshot snap;
		controller.copySnapshot(snap);
		return snap.mode == DJ_ASSIST_MODE_TRANSITION_RUNNING;
	});

	// The user re-asserts play on the target deck (1) themselves, mid-
	// transition - a real, non-system submit() through the shared port,
	// exactly as DjSession::setPlaying(..., DJ_ORIGIN_LOCAL_UI) would.
	port.setPlaying(1, true, DJ_ORIGIN_LOCAL_UI);

	loopUntil(port, controller, actuator, [&]{
		return actuator.state() == AutoDjState::Paused;
	});

	DjAssistSnapshot coachSnap;
	controller.copySnapshot(coachSnap);
	assert(coachSnap.mode == DJ_ASSIST_MODE_TRANSITION_FAILED);
	assert(coachSnap.plan.failure == DJ_ASSIST_FAIL_MANUAL_OVERRIDE);

	AutoDjSnapshot autoSnap;
	actuator.copySnapshot(autoSnap);
	assert(autoSnap.state == AutoDjState::Paused);
	assert(autoSnap.historySize == 0); // never resolved as Applied - the takeover pre-empted it.

	controller.end();
}

// -- Test (round-2 fix #3 review, "manual mix + Coach ownership" /
// "test both queue orders with unrelated entries and pending Coach
// steps"): a manual SET_MIX arrives while Coach's own forward-plan
// START_DECK step (a SET_PLAYING command - a DIFFERENT command type/
// channel than SET_MIX) is genuinely submitted into the shared queue but
// not yet popped/applied. Before this round's autoDjOwned propagation,
// the OLDER, narrower removeSystemTargeting() purge (gated by
// sameTarget(), which requires an exact command-type match - see
// DjSessionState.h) could never reach a queued SET_PLAYING step from an
// incoming SET_MIX command, so that stale Coach step could still be
// popped and applied on the very next tick, ahead of guardOk()'s
// mixIntentGeneration divergence check (which only runs once
// DjAssistController::tick() is next called) ever getting a chance to
// abort it. This test proves the new autoDjOwned-tagged
// removeAutoDjOwned() purge (reachable now that djIsAutoDjTakeoverSignal()
// includes SET_MIX) removes and durably SUPERSEDEs that queued step
// synchronously, inside the very setMix() submit() call itself - strictly
// before any further loopOnce()/tick() ever runs - and that Coach's own
// independent guard still separately fails the transition right after,
// exactly as it did before this fix, so Auto DJ still converges to
// Paused either way. --
void testManualMixDuringTransitionPurgesQueuedCoachStep(){
	hostStubSetMicros(0);
	FakePort port;
	port.entries[0].entry = makeEntry(0x55);
	port.entryCount = 1;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	setDeckPlaying(port, 0, 190, 200, 0x99, 128000);
	port.targetDeckOverride = 1;
	port.deckFrames[0] = 1000;
	port.downbeatAvailable[0] = true;
	port.downbeatFrame[0] = 1000;

	DjAssistController controller;
	controller.begin(&port);
	port.controller = &controller;

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	loopOnce(port, controller, actuator);
	assert(actuator.start());

	// Drive to TRANSITION_RUNNING with the START_DECK step (SET_PLAYING on
	// the incoming deck) genuinely submitted into the shared queue but NOT
	// yet popped/applied - the exact window the review flagged.
	loopUntil(port, controller, actuator, [&]{
		DjAssistSnapshot snap;
		controller.copySnapshot(snap);
		return snap.mode == DJ_ASSIST_MODE_TRANSITION_RUNNING &&
			snap.plan.currentStep < snap.plan.stepCount &&
			snap.plan.steps[snap.plan.currentStep].action == DJ_ASSIST_ACTION_START_DECK &&
			snap.plan.steps[snap.plan.currentStep].submitted &&
			!snap.plan.steps[snap.plan.currentStep].applied;
	});

	DjAssistSnapshot armed;
	controller.copySnapshot(armed);
	const uint32_t queuedStepCommandId = armed.plan.steps[armed.plan.currentStep].commandId;
	assert(queuedStepCommandId != 0);
	assert(port.queue.depth() == 1); // exactly the queued START_DECK step, nothing else pending.
	assert(port.assistTracked.tracked && port.assistTracked.id == queuedStepCommandId);
	assert(port.assistTracked.status == DJ_COMMAND_ACCEPTED); // still awaiting application.

	// The user moves the crossfader themselves, mid-transition - a real,
	// non-system submit() through the shared port, exactly as
	// DjSession::setMix(..., DJ_ORIGIN_LOCAL_UI) would.
	port.setMix(200, DJ_ORIGIN_LOCAL_UI);

	// The purge must be synchronous, inside this one submit() call, before
	// any further loopOnce()/tick() ever runs - proving the stale step can
	// never be popped and applied out from under the user's own command.
	assert(port.queue.depth() == 1); // the queued START_DECK step is gone; only the user's SET_MIX remains.
	assert(port.assistTracked.status == DJ_COMMAND_SUPERSEDED);

	// Coach's own, independent mixIntentGeneration guard check (a second,
	// slower detection path this purge does not replace - see guardOk())
	// still separately fails the transition right after, and Auto DJ
	// still resolves to Paused, exactly as before this fix.
	loopUntil(port, controller, actuator, [&]{
		return actuator.state() == AutoDjState::Paused;
	});

	DjAssistSnapshot coachSnap;
	controller.copySnapshot(coachSnap);
	assert(coachSnap.mode == DJ_ASSIST_MODE_TRANSITION_FAILED);
	assert(coachSnap.plan.failure == DJ_ASSIST_FAIL_MANUAL_OVERRIDE);

	AutoDjSnapshot autoSnap;
	actuator.copySnapshot(autoSnap);
	assert(autoSnap.state == AutoDjState::Paused);
	assert(autoSnap.historySize == 0); // never resolved as Applied - the takeover pre-empted it.

	controller.end();
}

// -- Test 3 (fix #5 review, "failures at every stage"): the Coach arm
// submit itself is rejected by the REAL engine's own guard (not a fake
// return value) - the target deck is not stopped/sync-off at arm time
// (armTransition()'s own precondition, see DjAssistEngine.cpp) - because a
// stray manual play was left on it from a previous session. Auto DJ must
// fail that attempt (uniformly, via beginArm()'s Failed path) and retry
// from the load step; once the stray state clears, the retry succeeds. --
void testRealCoachGuardRejectionAtArmRetriesThenSucceeds(){
	hostStubSetMicros(0);
	FakePort port;
	port.entries[0].entry = makeEntry(0x44);
	port.entryCount = 1;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	setDeckPlaying(port, 0, 190, 200, 0x99, 128000);
	port.targetDeckOverride = 1;
	port.deckFrames[0] = 1000;
	port.downbeatAvailable[0] = true;
	port.downbeatFrame[0] = 1000;

	DjAssistController controller;
	controller.begin(&port);
	port.controller = &controller;

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	loopOnce(port, controller, actuator);
	assert(actuator.start());

	// Wait for the load to apply (deck 1 now loaded/stopped by the load
	// step), then - before the arm submit is processed - inject the stray
	// manual-play violation the real guard rejects on.
	loopUntil(port, controller, actuator, [&]{
		return port.snapshot.decks[1].loaded;
	});
	port.snapshot.decks[1].playing = true; // stray state; violates armTransition()'s own guard.

	// Let the planner run to completion unattended: the first arm attempt
	// is genuinely rejected by the real engine's guard (armAttemptCount's
	// first increment, rejected because deck 1 is unexpectedly playing),
	// the bounded retry policy resubmits from the load step (which, by
	// itself just being a fresh stable-ID load, naturally clears the deck
	// back to loaded/stopped - the same "load never starts/syncs the deck"
	// invariant beginArm() relies on in the happy path), and the second arm
	// attempt then succeeds against the now-clean guard state.
	AutoDjSnapshot after;
	loopUntil(port, controller, actuator, [&]{
		actuator.copySnapshot(after);
		return after.historySize == 1;
	}, 256);
	assert(port.armAttemptCount >= 2); // proves a genuine reject-then-retry cycle happened.
	assert(port.snapshot.decks[1].loaded);
	assert(port.snapshot.decks[1].playing); // this time Coach's own START_DECK actually ran.

	controller.end();
}

// -- Test 5 (round-3 fix #4 review, "rollback race"): Coach's transition
// fails mid-crossfade via its OWN guard (media removed) - deliberately NOT
// the manual-intent-generation channel testManualPlayDivergenceMid
// TransitionStopsBothCoachAndAutoDj above already covers, so this
// genuinely exercises AutoDjSessionActuator's Teardown sub-phase (
// pollTransitionPhase()'s default branch) rather than manualTakeoverActive()'s
// separate early-cancel path. Failing this late (after CROSSFADE has
// actually been submitted) forces a real, multi-step MIX -> SYNC ->
// STOP_DECK rollback (see DjAssistSessionBridge::nextRollbackPhase()), not
// an instant no-op DONE. Media is restored immediately once the failure
// trips, so a fresh arm attempt would otherwise be free to race the still-
// in-flight rollback exactly as the review described.
//
// The decisive assertion is NOT "rollbackSettled() before retry", nor "deck
// 1 keeps playing afterwards" - both are fooled by the bug's own self-
// healing side effect: a buggy re-arm's armTransition() unconditionally
// resets rollbackPhase_ to IDLE (so the accessor trivially reports settled
// once mode leaves TRANSITION_FAILED), and the retry's own forward plan
// re-issues START_DECK/ENABLE_SYNC on the SAME target deck anyway, so deck 1
// ends up looking fine either way. The one command a raced retry can never
// produce itself, only the ORIGINAL failed transition's rollback can, is
// its STOP_DECK step: SET_PLAYING(deck=toDeck(1), value=false) - the retry's
// own forward plan only ever stops fromDeck(0), never re-stops its own
// target. So the decisive, bug-unmaskable check is: that exact command must
// have actually been popped/applied (rollback ran to completion) BEFORE the
// retry's own LOAD_DECK(1) is popped - i.e. before any load resolves for
// the retry attempt, never after or interleaved mid-way.
void testCoachFailureRollbackSettlesBeforeAutoDjRetries(){
	hostStubSetMicros(0);
	FakePort port;
	port.entries[0].entry = makeEntry(0x66);
	port.entryCount = 1;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	setDeckPlaying(port, 0, 190, 200, 0x99, 128000);
	port.targetDeckOverride = 1;
	port.deckFrames[0] = 1000;
	port.downbeatAvailable[0] = true;
	port.downbeatFrame[0] = 1000;

	DjAssistController controller;
	controller.begin(&port);
	port.controller = &controller;

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	loopOnce(port, controller, actuator);
	assert(actuator.start());

	// Drive deep enough into the transition that CROSSFADE has genuinely
	// been submitted (not merely "current" - DjAssistEngine::tick() only
	// submits OR polls-and-advances a single step per call, so reaching
	// currentStep==CROSSFADE on its own does not yet mean its command was
	// actually pushed; require .submitted too, matching exactly what
	// nextRollbackPhase() itself checks via planStepSubmitted()/
	// crossfadeSubmitted).
	DjAssistSnapshot coachSnap;
	loopUntil(port, controller, actuator, [&]{
		controller.copySnapshot(coachSnap);
		return coachSnap.mode == DJ_ASSIST_MODE_TRANSITION_RUNNING &&
			coachSnap.plan.currentStep < coachSnap.plan.stepCount &&
			coachSnap.plan.steps[coachSnap.plan.currentStep].action == DJ_ASSIST_ACTION_CROSSFADE &&
			coachSnap.plan.steps[coachSnap.plan.currentStep].submitted;
	}, 128);

	const int armAttemptsBeforeFailure = port.armAttemptCount;
	const size_t poppedBeforeFailure = port.poppedLog.size();

	port.media = false; // trips guardOk()'s DJ_ASSIST_FAIL_MEDIA_REMOVED on the very next controller.tick().
	loopOnce(port, controller, actuator);
	controller.copySnapshot(coachSnap);
	assert(coachSnap.mode == DJ_ASSIST_MODE_TRANSITION_FAILED);
	assert(coachSnap.plan.failure == DJ_ASSIST_FAIL_MEDIA_REMOVED);
	assert(!controller.rollbackSettled()); // crossfade was submitted - MIX is still pending.

	// Media returns immediately: the only thing a correct implementation
	// still has to hold the retry back on is rollback settlement itself.
	port.media = true;

	// Drive forward (bounded) until the retry resolves as a second history
	// entry - the SAME candidate (0x66) is the only one in the library, so
	// this is genuinely the retried attempt, not a different entry.
	AutoDjSnapshot after;
	loopUntil(port, controller, actuator, [&]{
		actuator.copySnapshot(after);
		return after.historySize == 1;
	}, 256);
	assert(port.armAttemptCount > armAttemptsBeforeFailure); // a genuine retry arm happened.
	assert(port.snapshot.decks[1].loaded);
	assert(port.snapshot.decks[1].playing);
	assert(memcmp(port.snapshot.decks[1].identity.fingerprint, port.entries[0].entry.identity.fingerprint, 16) == 0);

	// Decisive regression check, scanning the exact command sequence
	// popped/applied since the failure tripped: the original transition's
	// own rollback STOP_DECK step - SET_PLAYING(deck=1, value=false) - is a
	// command only that rollback ever issues (the retry's own forward plan
	// only ever stops deck 0, its fromDeck, never re-stops its own target
	// deck 1), so its presence/position is an unmaskable fingerprint of
	// whether rollback actually ran to completion, and when.
	int rollbackStopIndex = -1;
	int retryLoadIndex = -1;
	for(size_t i = poppedBeforeFailure; i < port.poppedLog.size(); i++){
		const DjCommand& cmd = port.poppedLog[i];
		if(rollbackStopIndex < 0 && cmd.type == DJ_COMMAND_SET_PLAYING && cmd.deck == 1 && cmd.value == 0){
			rollbackStopIndex = int(i);
		}
		if(retryLoadIndex < 0 && cmd.type == DJ_COMMAND_LOAD_DECK && cmd.deck == 1){
			retryLoadIndex = int(i);
		}
	}
	assert(rollbackStopIndex >= 0); // the failed transition's own rollback actually ran to completion...
	assert(retryLoadIndex >= 0); // ...and the retry actually loaded (sanity: both events genuinely happened).
	assert(rollbackStopIndex < retryLoadIndex); // ...strictly before the retry's own load, never after/racing it.

	// Run a few more idle ticks (nothing new queued) as an additional,
	// cheap belt-and-braces sanity check that the now-healthy retry stays
	// stable afterwards.
	for(int i = 0; i < 8; i++){
		loopOnce(port, controller, actuator);
		assert(port.snapshot.decks[1].playing);
		assert(port.snapshot.decks[1].loaded);
	}

	controller.end();
}

} // namespace

// Host harness only - grants access to DjAssistController::fillWorkerStep()
// (via the `friend class AutoDjCoachGridHydrationHarness;` grant in
// DjAssistController.h) so this file's hydration test can deterministically
// resolve the bounded on-demand grid-anchor cache (DjAssistGridCache)
// without needing a real background thread. Adds no production API
// surface; every other test in this file drives the controller through its
// public API exactly as before.
class AutoDjCoachGridHydrationHarness {
public:
	static void stepFill(DjAssistController& controller){
		controller.fillWorkerStep();
	}

	// Round 5, fix #2 (capability false-positive) test-only seam: forces
	// the NEXT fillWorker_.begin() call (i.e. the one DjAssistController::
	// begin() itself makes) to report launch failure, exactly mirroring a
	// real xTaskCreate()/thread-creation failure on hardware. One-shot -
	// consumed by that single begin() call, same as
	// DjAssistFillWorker::forceLaunchFailureForTest's own contract.
	static void forceFillWorkerLaunchFailure(DjAssistController& controller){
		controller.fillWorker_.forceLaunchFailureForTest = true;
	}

	// Round 6 (continuous capability enforcement) test-only seam: flips
	// authorityReady() false the same way a real fill-worker exit or a
	// later candidate-table allocation failure would, WITHOUT touching
	// anything else DjAssistController owns. Deliberately narrower than
	// calling controller.end(): end() also tears down actuator_/session_,
	// which would freeze Coach's own tickTransition()/tickRollback()
	// machinery mid-flight (both bail out immediately once actuator_ is
	// null) - these tests specifically need an already-in-flight Coach
	// transition/rollback to keep resolving normally on its own while only
	// the candidate-fill authority itself is reported gone, so this must
	// mutate exactly (and only) what authorityReady() reads.
	static void forceAuthorityLossForTest(DjAssistController& controller){
		controller.allocationFailed_ = true;
	}
};

namespace {

// -- round 5, fix #2 (capability false-positive) --
// hasStableIdEndpoint() must reflect DjAssistController::authorityReady(),
// not unconditionally report true. When the fill worker fails to launch
// (a real task/thread-creation failure), the candidate table can never be
// filled, so Auto DJ must refuse to arm with CapabilityDisabled rather than
// arming and then having every load retry exhaust its budget against an
// empty table.
void testCapabilityDisabledWhenFillWorkerLaunchFails(){
	FakePort port;
	port.entries[0].entry = makeEntry(0x71);
	port.entryCount = 1;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	DjAssistController controller;
	AutoDjCoachGridHydrationHarness::forceFillWorkerLaunchFailure(controller);
	controller.begin(&port);
	port.controller = &controller;

	assert(!controller.authorityReady());

	AutoDjSessionActuator actuator(port);
	assert(!actuator.arm());
	assert(actuator.state() == AutoDjState::Failed);
	assert(actuator.failReason() == AutoDjFailReason::CapabilityDisabled);

	controller.end();
}

// Sibling of the above, keyed on candidate-table allocation failure
// (ps_malloc() returning nullptr - simulated PSRAM exhaustion) instead of
// fill-worker launch failure - the other half of authorityReady()'s guard.
void testCapabilityDisabledWhenCandidateAllocationFails(){
	FakePort port;
	port.entries[0].entry = makeEntry(0x72);
	port.entryCount = 1;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	hostStubSetForcePsMallocFailure(true);
	DjAssistController controller;
	controller.begin(&port);
	hostStubSetForcePsMallocFailure(false); // one-shot fault, clear immediately.
	port.controller = &controller;

	assert(!controller.authorityReady());

	AutoDjSessionActuator actuator(port);
	assert(!actuator.arm());
	assert(actuator.state() == AutoDjState::Failed);
	assert(actuator.failReason() == AutoDjFailReason::CapabilityDisabled);

	controller.end();
}

} // namespace

namespace {

// -- round 4, issue #1 (PSRAM budget): proves the PRODUCTION call path -
// -- FakePort::autoDjLoadDeckByIdentity() (a faithful copy of DjSession::
// -- loadDeckByIdentity()'s body, see that method's own definition above) -
// -- actually fires DjAssistController::requestGridHydration() on a
// -- successful Auto DJ load submit, and that the bounded background
// -- hydration step then makes the anchors available via gridAnchorsFor(),
// -- exactly as DjSession::resolveIdentityLoad() would look them up at
// -- apply time. Not merely a unit test of DjAssistGridCache in isolation -
// -- this goes through the real AutoDjSessionActuator's submitLoad() and
// -- the real DjAssistController together, as production wires them.
void testAutoDjLoadTriggersGridHydrationEndToEnd(){
	hostStubSetMicros(0);
	FakePort port;
	port.entries[0].entry = makeEntry(0x44);
	port.entries[0].entry.capabilities = DJ_METADATA_HAS_BPM | DJ_METADATA_HAS_SOURCE_FRAMES | DJ_METADATA_HAS_GRID;
	port.entryCount = 1;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	setDeckPlaying(port, 0, 190, 200, 0x99, 128000);
	port.targetDeckOverride = 1;
	port.deckFrames[0] = 1000;
	port.downbeatAvailable[0] = true;
	port.downbeatFrame[0] = 1000;

	DjAssistController controller;
	controller.begin(&port);
	port.controller = &controller;

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	loopOnce(port, controller, actuator); // top up the queue while Armed.
	assert(actuator.start());

	// Wait for the LOAD_DECK command to actually apply - that is the exact
	// moment autoDjLoadDeckByIdentity() (and, inside it, the fire-and-
	// forget requestGridHydration() call) runs.
	loopUntil(port, controller, actuator, [&]{
		return port.snapshot.decks[1].loaded;
	});
	const DjTrackIdentity loaded = port.entries[0].entry.identity;

	// Not resolved yet - stepGridHydration() only advances via
	// fillWorkerStep(), which loopOnce()/tick() never calls (see
	// DjAssistFillWorker's host manual-stepping default).
	DjGridAnchor anchors[DJ_GRID_ANCHOR_CAPACITY] = {};
	uint16_t anchorCount = 0;
	assert(!controller.gridAnchorsFor(port.libraryGeneration, port.metadataRevision, loaded, anchors, anchorCount));

	// The main candidate table must finish a full fill pass before
	// stepGridHydration() will trust entries_[] enough to resolve a
	// pending request against it (see stepGridHydration()'s doc comment) -
	// bounded by entryCount+margin calls, exactly like
	// DjAssistIntegrationSelfCheck::fillCandidateTableToReady().
	for(int i = 0; i < 16 && controller.candidateCount() == 0; ++i){
		AutoDjCoachGridHydrationHarness::stepFill(controller);
	}
	assert(controller.candidateCount() == port.entryCount);

	// One more bounded call resolves the still-pending hydration request
	// now that the table is current and complete.
	AutoDjCoachGridHydrationHarness::stepFill(controller);
	assert(port.assistTrackGridAnchorsCallCount == 1);

	assert(controller.gridAnchorsFor(port.libraryGeneration, port.metadataRevision, loaded, anchors, anchorCount));
	assert(anchorCount == 1);

	controller.end();
}

} // namespace

namespace {

// -- round 6: "authorityReady is checked only arm/start. If fill worker
// exits while Running, planner can begin another cached load; no pause/
// fail." -- these four tests drive the REAL controller/actuator through
// AutoDjCoachGridHydrationHarness::forceAuthorityLossForTest() (which
// flips authorityReady() false exactly the way a real fill-worker exit or
// later candidate-table allocation failure would, without disturbing
// Coach's own transition/rollback machinery - see that helper's doc
// comment) at each of the four distinct points the review calls out, and
// assert in every case that: an already-in-flight command/transition/
// rollback is never stranded or interrupted, only ever allowed to settle
// to its own real outcome; no LOAD_DECK is ever submitted afterward
// (checked against a 2-entry library, so a healthy planner would
// otherwise have topped up and loaded the second entry); and Auto DJ
// converges on the terminal AutoDjState::Failed /
// AutoDjFailReason::AuthorityUnavailable state, never a silent retry.

int countLoadDeckCommands(const FakePort& port){
	int count = 0;
	for(const DjCommand& cmd : port.poppedLog){
		if(cmd.type == DJ_COMMAND_LOAD_DECK) count++;
	}
	return count;
}

void assertTerminalAuthorityUnavailable(AutoDjSessionActuator& actuator){
	assert(actuator.state() == AutoDjState::Failed);
	assert(actuator.failReason() == AutoDjFailReason::AuthorityUnavailable);
}

// 1) Idle Running, nothing pending yet (deck far from end - no load has
// ever been attempted). The authority loss must be caught on the very
// next tick, before currentTrackAtEnd()/beginNextLoad() ever gets a
// chance to run.
void testAuthorityLossIdleRunningBeforeLoadFailsWithoutLoad(){
	hostStubSetMicros(0);
	FakePort port;
	port.entries[0].entry = makeEntry(0x81);
	port.entries[1].entry = makeEntry(0x82);
	port.entryCount = 2;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	// Plenty of time left - currentTrackAtEnd() stays false throughout.
	setDeckPlaying(port, 0, 10, 200, 0x99, 128000);
	port.targetDeckOverride = 1;

	DjAssistController controller;
	controller.begin(&port);
	port.controller = &controller;

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	loopOnce(port, controller, actuator); // top up the queue while Armed.
	assert(actuator.start());
	assert(actuator.state() == AutoDjState::Running);

	for(int i = 0; i < 4; i++) loopOnce(port, controller, actuator); // idle Running.
	assert(countLoadDeckCommands(port) == 0);

	assert(controller.authorityReady());
	AutoDjCoachGridHydrationHarness::forceAuthorityLossForTest(controller);
	assert(!controller.authorityReady());

	loopOnce(port, controller, actuator);
	assertTerminalAuthorityUnavailable(actuator);
	assert(countLoadDeckCommands(port) == 0); // never even attempted a load.

	for(int i = 0; i < 4; i++) loopOnce(port, controller, actuator); // must not resurrect.
	assert(countLoadDeckCommands(port) == 0);
	assertTerminalAuthorityUnavailable(actuator);

	controller.end();
}

// 2) A LOAD_DECK has just been submitted into the shared queue but not
// yet popped/applied when the authority is lost. That already-submitted
// command must still be allowed to apply (and, since nothing else here
// interferes, the whole composite load->arm->transition attempt must
// still be allowed to run to a real Applied) - only the NEXT attempt is
// forbidden.
void testAuthorityLossAfterLoadQueuedBeforeApplyStillSettles(){
	hostStubSetMicros(0);
	FakePort port;
	port.entries[0].entry = makeEntry(0x83);
	port.entries[1].entry = makeEntry(0x84);
	port.entryCount = 2;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	setDeckPlaying(port, 0, 190, 200, 0x99, 128000);
	port.targetDeckOverride = 1;
	port.deckFrames[0] = 1000;
	port.downbeatAvailable[0] = true;
	port.downbeatFrame[0] = 1000;

	DjAssistController controller;
	controller.begin(&port);
	port.controller = &controller;

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	loopOnce(port, controller, actuator); // top up the queue while Armed.
	assert(actuator.start());

	// One Running tick submits the first LOAD_DECK into port.queue via
	// actuator.tick() - loopOnce() pops/applies BEFORE ticking, so
	// immediately after this call the command is genuinely still queued,
	// not yet applied.
	loopOnce(port, controller, actuator);
	assert(!port.snapshot.decks[1].loaded);
	assert(countLoadDeckCommands(port) == 0); // submitted, not yet popped/applied.

	AutoDjCoachGridHydrationHarness::forceAuthorityLossForTest(controller);
	assert(!controller.authorityReady());

	// The already-in-flight attempt must still be allowed to settle to a
	// genuine Applied - authority loss must never strand it.
	AutoDjSnapshot snap;
	loopUntil(port, controller, actuator, [&]{
		actuator.copySnapshot(snap);
		return snap.historySize == 1;
	});
	assert(port.snapshot.decks[1].loaded);
	assert(port.snapshot.decks[1].playing);
	assert(countLoadDeckCommands(port) == 1);

	// The very next settle must forbid a second load (the library still
	// has a second, otherwise-eligible entry) and converge terminal.
	loopUntil(port, controller, actuator, [&]{
		return actuator.state() == AutoDjState::Failed;
	});
	assertTerminalAuthorityUnavailable(actuator);
	assert(countLoadDeckCommands(port) == 1); // no subsequent load.

	controller.end();
}

// 3) Authority is lost mid Coach TRANSITION_RUNNING (arm already applied,
// crossfade plan actively advancing). The transition must be allowed to
// keep running to its own real completion (nothing here gates on
// authorityReady()) before Auto DJ ever reacts to the loss.
void testAuthorityLossDuringCoachTransitionStillSettles(){
	hostStubSetMicros(0);
	FakePort port;
	port.entries[0].entry = makeEntry(0x85);
	port.entries[1].entry = makeEntry(0x86);
	port.entryCount = 2;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	setDeckPlaying(port, 0, 190, 200, 0x99, 128000);
	port.targetDeckOverride = 1;
	port.deckFrames[0] = 1000;
	port.downbeatAvailable[0] = true;
	port.downbeatFrame[0] = 1000;

	DjAssistController controller;
	controller.begin(&port);
	port.controller = &controller;

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	loopOnce(port, controller, actuator); // top up the queue while Armed.
	assert(actuator.start());

	loopUntil(port, controller, actuator, [&]{
		DjAssistSnapshot coachSnap;
		controller.copySnapshot(coachSnap);
		return coachSnap.mode == DJ_ASSIST_MODE_TRANSITION_RUNNING;
	});

	AutoDjCoachGridHydrationHarness::forceAuthorityLossForTest(controller);
	assert(!controller.authorityReady());

	AutoDjSnapshot snap;
	loopUntil(port, controller, actuator, [&]{
		actuator.copySnapshot(snap);
		return snap.historySize == 1;
	}, 128);
	assert(port.snapshot.decks[1].loaded);
	assert(port.snapshot.decks[1].playing);
	assert(countLoadDeckCommands(port) == 1);

	loopUntil(port, controller, actuator, [&]{
		return actuator.state() == AutoDjState::Failed;
	});
	assertTerminalAuthorityUnavailable(actuator);
	assert(countLoadDeckCommands(port) == 1); // no subsequent load.

	controller.end();
}

// 4) Authority is lost while Coach's own rollback (after a genuine
// transition failure) is still unsettled. The rollback must be allowed to
// run to completion exactly as it would with a healthy authority (proven
// the same way testCoachFailureRollbackSettlesBeforeAutoDjRetries proves
// it: the rollback's own STOP_DECK on the target deck must actually be
// observed in the popped-command log) - but afterward, unlike that
// sibling test, the failed attempt must NEVER retry; it must converge
// terminal AuthorityUnavailable instead.
void testAuthorityLossDuringRollbackNeverRetries(){
	hostStubSetMicros(0);
	FakePort port;
	port.entries[0].entry = makeEntry(0x87);
	port.entries[1].entry = makeEntry(0x88);
	port.entryCount = 2;
	port.deckContext.valid = true;
	port.deckContext.bpmMilli = 128000;
	port.physicalConfirmationPending = true;

	setDeckPlaying(port, 0, 190, 200, 0x99, 128000);
	port.targetDeckOverride = 1;
	port.deckFrames[0] = 1000;
	port.downbeatAvailable[0] = true;
	port.downbeatFrame[0] = 1000;

	DjAssistController controller;
	controller.begin(&port);
	port.controller = &controller;

	AutoDjSessionActuator actuator(port);
	assert(actuator.arm());
	loopOnce(port, controller, actuator); // top up the queue while Armed.
	assert(actuator.start());

	// Drive deep enough that CROSSFADE has genuinely been submitted (see
	// testCoachFailureRollbackSettlesBeforeAutoDjRetries's identical
	// wait for exactly why .submitted, not just currentStep, is required).
	DjAssistSnapshot coachSnap;
	loopUntil(port, controller, actuator, [&]{
		controller.copySnapshot(coachSnap);
		return coachSnap.mode == DJ_ASSIST_MODE_TRANSITION_RUNNING &&
			coachSnap.plan.currentStep < coachSnap.plan.stepCount &&
			coachSnap.plan.steps[coachSnap.plan.currentStep].action == DJ_ASSIST_ACTION_CROSSFADE &&
			coachSnap.plan.steps[coachSnap.plan.currentStep].submitted;
	}, 128);

	const size_t poppedBeforeFailure = port.poppedLog.size();

	port.media = false; // trips guardOk()'s DJ_ASSIST_FAIL_MEDIA_REMOVED on the next controller.tick().
	loopOnce(port, controller, actuator);
	controller.copySnapshot(coachSnap);
	assert(coachSnap.mode == DJ_ASSIST_MODE_TRANSITION_FAILED);
	assert(!controller.rollbackSettled()); // crossfade was submitted - MIX is still pending.

	// Authority is lost while the rollback is genuinely still unsettled.
	AutoDjCoachGridHydrationHarness::forceAuthorityLossForTest(controller);
	assert(!controller.authorityReady());
	port.media = true; // media returning must not be what gates the retry here.

	// The rollback itself must still be allowed to run to completion -
	// its own STOP_DECK on the target deck (deck 1) must actually appear
	// in the popped-command log, exactly as the healthy-authority sibling
	// test proves.
	loopUntil(port, controller, actuator, [&]{
		return controller.rollbackSettled();
	}, 128);
	bool rollbackStopSeen = false;
	for(size_t i = poppedBeforeFailure; i < port.poppedLog.size(); i++){
		const DjCommand& cmd = port.poppedLog[i];
		if(cmd.type == DJ_COMMAND_SET_PLAYING && cmd.deck == 1 && cmd.value == 0){
			rollbackStopSeen = true;
			break;
		}
	}
	assert(rollbackStopSeen); // the rollback genuinely ran to completion, not merely abandoned.

	// Now that rollback has settled, Auto DJ must fail terminally instead
	// of retrying the same entry (contrast testCoachFailureRollbackSettlesBeforeAutoDjRetries,
	// where a healthy authority genuinely retries and succeeds here).
	loopUntil(port, controller, actuator, [&]{
		return actuator.state() == AutoDjState::Failed;
	}, 128);
	assertTerminalAuthorityUnavailable(actuator);

	// No retry LOAD_DECK for deck 1 after the failure was first observed.
	int retryLoadCount = 0;
	for(size_t i = poppedBeforeFailure; i < port.poppedLog.size(); i++){
		const DjCommand& cmd = port.poppedLog[i];
		if(cmd.type == DJ_COMMAND_LOAD_DECK && cmd.deck == 1) retryLoadCount++;
	}
	assert(retryLoadCount == 0);

	controller.end();
}

} // namespace

int main(){
	testFullTwoEntryRunThroughRealPorts();
	testManualPlayDivergenceMidTransitionStopsBothCoachAndAutoDj();
	testManualMixDuringTransitionPurgesQueuedCoachStep();
	testRealCoachGuardRejectionAtArmRetriesThenSucceeds();
	testCoachFailureRollbackSettlesBeforeAutoDjRetries();
	testAutoDjLoadTriggersGridHydrationEndToEnd();
	testCapabilityDisabledWhenFillWorkerLaunchFails();
	testCapabilityDisabledWhenCandidateAllocationFails();
	testAuthorityLossIdleRunningBeforeLoadFailsWithoutLoad();
	testAuthorityLossAfterLoadQueuedBeforeApplyStillSettles();
	testAuthorityLossDuringCoachTransitionStillSettles();
	testAuthorityLossDuringRollbackNeverRetries();
	return 0;
}
