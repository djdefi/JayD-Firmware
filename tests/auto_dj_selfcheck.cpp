// Self-check for the isolated Auto DJ planning layer (DjAutoDjQueue,
// DjAutoDjHistory, DjAutoDjStateMachine, DjAutoDjPlanner). Host-only, no
// Arduino/ESP32 dependencies. Build/run directly, e.g.:
//
//   g++ -std=c++11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
//       -I ../src/AutoDj auto_dj_selfcheck.cpp ../src/AutoDj/DjAutoDjPlanner.cpp \
//       -o auto_dj_selfcheck && ./auto_dj_selfcheck
//
// Also wired into CMakeLists.txt as the AutoDjSelfCheck CTest target.

#include <assert.h>
#include <string.h>

#include "../src/AutoDj/AutoDjLoadPort.h"
#include "../src/AutoDj/DjAutoDjHistory.h"
#include "../src/AutoDj/DjAutoDjPlanner.h"
#include "../src/AutoDj/DjAutoDjQueue.h"
#include "../src/AutoDj/DjAutoDjStateMachine.h"
#include "../src/AutoDj/DjAutoDjTypes.h"

namespace {

AutoDjIdentity makeIdentity(uint8_t fingerprintByte, uint32_t generation = 1, uint32_t revision = 1){
	AutoDjIdentity identity;
	identity.flags = AUTO_DJ_IDENTITY_FINGERPRINT;
	identity.libraryGeneration = generation;
	identity.metadataRevision = revision;
	memset(identity.fingerprint, fingerprintByte, sizeof(identity.fingerprint));
	return identity;
}

// Same fingerprint byte as makeIdentity(fingerprintByte, ...) would produce,
// but also carries distinct sourceId evidence - used to prove sameTrack()
// (and everything built on it: DjAutoDjQueue::containsIdentity()/isQueued(),
// DjAutoDjHistory::wasRecentlyPlayed()) never aliases two tracks that share
// a fingerprint but come from different sources.
AutoDjIdentity makeIdentityWithSource(uint8_t fingerprintByte, uint8_t sourceByte,
									   uint32_t generation = 1, uint32_t revision = 1){
	AutoDjIdentity identity = makeIdentity(fingerprintByte, generation, revision);
	identity.flags |= AUTO_DJ_IDENTITY_SOURCE;
	memset(identity.sourceId, sourceByte, sizeof(identity.sourceId));
	return identity;
}

AutoDjCandidate makeCandidate(uint8_t fingerprintByte, uint32_t artistHash = 0,
							   uint32_t titleHash = 0, bool hasMetadata = false, uint32_t score = 0){
	AutoDjCandidate candidate;
	candidate.identity = makeIdentity(fingerprintByte);
	candidate.artistHash = artistHash;
	candidate.titleHash = titleHash;
	candidate.hasMetadata = hasMetadata;
	candidate.score = score;
	return candidate;
}

// Fully controllable test double for AutoDjLoadPort.
class MockLoadPort : public AutoDjLoadPort {
public:
	bool stableIdEndpoint = true;
	bool manualTakeover = false;
	bool durationTrustworthy = true;
	bool trackAtEnd = false;
	bool recording = false;
	bool confirmed = true;
	bool submitShouldSucceed = true;
	AutoDjLoadOutcome nextOutcome = AutoDjLoadOutcome::Applied;
	AutoDjIdentity lastSubmitted;
	int submitCount = 0;

	bool hasStableIdEndpoint() const override{
		return stableIdEndpoint;
	}

	bool submitLoad(const AutoDjIdentity& identity) override{
		submitCount++;
		lastSubmitted = identity;
		return submitShouldSucceed;
	}

	AutoDjLoadOutcome pollLoad() override{
		return nextOutcome;
	}

	bool manualTakeoverActive() const override{
		return manualTakeover;
	}

	bool currentDurationTrustworthy() const override{
		return durationTrustworthy;
	}

	bool currentTrackAtEnd() const override{
		return trackAtEnd;
	}

	bool recordingFailed() const override{
		return recording;
	}

	bool physicalConfirmationPresent() const override{
		return confirmed;
	}

	// Fully controllable fake wall clock (deliberately independent of
	// tick/call count - see AutoDjLoadPort::nowMicros()'s own doc comment):
	// tests advance it explicitly via advanceMicros() rather than relying
	// on a fixed per-call increment, so a timeout test asserts exactly what
	// it claims (elapsed wall time crossed AUTO_DJ_LOAD_TIMEOUT_US) instead
	// of coincidentally working out at some tick count.
	uint64_t fakeNowUs = 0;

	uint64_t nowMicros() const override{
		return fakeNowUs;
	}

	void advanceMicros(uint64_t deltaUs){
		fakeNowUs += deltaUs;
	}
};

void testQueuePinnedOrder(){
	DjAutoDjQueue queue;
	AutoDjQueueEntry entry;

	assert(queue.pushPlanned(makeIdentity(1), 0, 0, AUTO_DJ_REASON_NONE));
	assert(queue.pushPinned(makeIdentity(2)));
	assert(queue.pushPlanned(makeIdentity(3), 0, 0, AUTO_DJ_REASON_NONE));
	assert(queue.pushPinned(makeIdentity(4)));

	// Pinned entries are served first, in their own FIFO order, then the
	// planned entries in their FIFO order.
	assert(queue.popNext(entry) && entry.identity.fingerprint[0] == 2);
	assert(queue.popNext(entry) && entry.identity.fingerprint[0] == 4);
	assert(queue.popNext(entry) && entry.identity.fingerprint[0] == 1);
	assert(queue.popNext(entry) && entry.identity.fingerprint[0] == 3);
	assert(queue.empty());
}

void testQueueFullEmpty(){
	DjAutoDjQueue queue;
	AutoDjQueueEntry entry;

	assert(queue.empty());
	assert(!queue.popNext(entry));

	for(uint8_t i = 0; i < AUTO_DJ_QUEUE_CAPACITY; i++){
		assert(!queue.full());
		assert(queue.pushPlanned(makeIdentity(i), 0, 0, AUTO_DJ_REASON_NONE));
	}
	assert(queue.full());
	assert(!queue.pushPlanned(makeIdentity(200), 0, 0, AUTO_DJ_REASON_NONE));
	assert(!queue.pushPinned(makeIdentity(201)));

	for(uint8_t i = 0; i < AUTO_DJ_QUEUE_CAPACITY; i++){
		assert(queue.popNext(entry));
		assert(entry.identity.fingerprint[0] == i);
	}
	assert(queue.empty());
	assert(!queue.popNext(entry));
}

void testQueueInvalidation(){
	DjAutoDjQueue queue;
	assert(queue.pushPlanned(makeIdentity(1, 1), 0, 0, AUTO_DJ_REASON_NONE));
	assert(queue.pushPlanned(makeIdentity(2, 2), 0, 0, AUTO_DJ_REASON_NONE));
	assert(queue.pushPlanned(makeIdentity(3, 1), 0, 0, AUTO_DJ_REASON_NONE));

	const uint8_t removed = queue.invalidateGeneration(2);
	assert(removed == 2);
	assert(queue.depth() == 1);

	AutoDjQueueEntry entry;
	assert(queue.popNext(entry));
	assert(entry.identity.fingerprint[0] == 2);
	assert(queue.empty());
}

// Sibling of testQueueInvalidation() above, keyed on metadataRevision
// instead of libraryGeneration: a same-generation metadata replacement
// (see AutoDjIdentity::metadataRevision's doc comment) must invalidate a
// queued entry exactly like a generation change does, entirely
// independently of libraryGeneration.
void testQueueRevisionInvalidation(){
	DjAutoDjQueue queue;
	assert(queue.pushPlanned(makeIdentity(1, 1, 1), 0, 0, AUTO_DJ_REASON_NONE));
	assert(queue.pushPlanned(makeIdentity(2, 1, 2), 0, 0, AUTO_DJ_REASON_NONE));
	assert(queue.pushPlanned(makeIdentity(3, 1, 1), 0, 0, AUTO_DJ_REASON_NONE));

	const uint8_t removed = queue.invalidateRevision(2);
	assert(removed == 2);
	assert(queue.depth() == 1);

	AutoDjQueueEntry entry;
	assert(queue.popNext(entry));
	assert(entry.identity.fingerprint[0] == 2);
	assert(queue.empty());
}

// -- round 5, fix #3 (exact-identity aliasing) --
// Two tracks sharing a fingerprint but differing only in sourceId must
// never be treated as "the same queued track": containsIdentity()/
// isQueued() (used to prevent double-queuing the same candidate) and
// wasRecentlyPlayed() (recent-repeat exclusion) both go through
// AutoDjIdentity::sameTrack(), which previously matched on fingerprint
// alone.
void testQueueDoesNotAliasSameFingerprintDifferentSource(){
	DjAutoDjQueue queue;
	AutoDjIdentity x = makeIdentityWithSource(9, 0xA1);
	AutoDjIdentity y = makeIdentityWithSource(9, 0xB2); // same fingerprint, different source.

	assert(queue.pushPlanned(x, 0, 0, AUTO_DJ_REASON_NONE));
	assert(!queue.containsIdentity(y)); // Y must not alias X's queued entry.
	assert(queue.containsIdentity(x));

	// Y is genuinely a distinct track, so it must be queueable even while
	// X (same fingerprint, different source) is already queued.
	assert(queue.pushPlanned(y, 0, 0, AUTO_DJ_REASON_NONE));
	assert(queue.depth() == 2);
}

void testHistoryDoesNotAliasSameFingerprintDifferentSource(){
	DjAutoDjHistory history;
	AutoDjIdentity x = makeIdentityWithSource(9, 0xA1);
	AutoDjIdentity y = makeIdentityWithSource(9, 0xB2); // same fingerprint, different source.
	history.record(x, /*artistHash=*/0, /*titleHash=*/0);

	assert(history.wasRecentlyPlayed(x, AUTO_DJ_DEFAULT_RECENT_EXCLUSION));
	assert(!history.wasRecentlyPlayed(y, AUTO_DJ_DEFAULT_RECENT_EXCLUSION)); // must not alias X.
}

void testHistoryExclusion(){
	DjAutoDjHistory history;
	AutoDjIdentity played = makeIdentity(9);
	history.record(played, /*artistHash=*/100, /*titleHash=*/200);

	assert(history.wasRecentlyPlayed(played, AUTO_DJ_DEFAULT_RECENT_EXCLUSION));
	assert(!history.wasRecentlyPlayed(makeIdentity(10), AUTO_DJ_DEFAULT_RECENT_EXCLUSION));
	assert(history.artistOnCooldown(100, AUTO_DJ_DEFAULT_ARTIST_EXCLUSION));
	assert(!history.artistOnCooldown(101, AUTO_DJ_DEFAULT_ARTIST_EXCLUSION));
	assert(history.titleOnCooldown(200, AUTO_DJ_DEFAULT_TITLE_EXCLUSION));
	assert(!history.titleOnCooldown(201, AUTO_DJ_DEFAULT_TITLE_EXCLUSION));

	// Missing metadata (hash == 0) must never be treated as a match -
	// otherwise every "unknown artist" track would falsely exclude every
	// other "unknown artist" track.
	assert(!history.artistOnCooldown(0, AUTO_DJ_DEFAULT_ARTIST_EXCLUSION));
	assert(!history.titleOnCooldown(0, AUTO_DJ_DEFAULT_TITLE_EXCLUSION));

	// Falls off the window once enough other plays happen.
	for(uint8_t i = 0; i < AUTO_DJ_DEFAULT_RECENT_EXCLUSION; i++){
		history.record(makeIdentity(50 + i), 0, 0);
	}
	assert(!history.wasRecentlyPlayed(played, AUTO_DJ_DEFAULT_RECENT_EXCLUSION));
}

void testPlannerSelectionExcludesRecentArtistTitle(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);

	AutoDjCandidate candidates[2] = {
		makeCandidate(1, /*artistHash=*/1, /*titleHash=*/1),
		makeCandidate(2, /*artistHash=*/2, /*titleHash=*/2)
	};

	AutoDjCandidate chosen;
	assert(planner.selectNext(candidates, 2, chosen));
	// Both are equally eligible with no metadata; deterministic tie-break by
	// smallest fingerprint picks candidate 1.
	assert(chosen.identity.fingerprint[0] == 1);
	assert(chosen.reasons & AUTO_DJ_REASON_CONSERVATIVE_FALLBACK);
	assert(!(chosen.reasons & AUTO_DJ_REASON_METADATA_MATCH));

	// Now pin candidate 1 as recently played via a fresh planner sharing a
	// pre-seeded history through planNext()/tick() would normally do this;
	// exercise it directly through selectNext with a planner that already
	// recorded it by driving one full load cycle.
	planner.pinTrack(candidates[0].identity, candidates[0].artistHash, candidates[0].titleHash);
	assert(planner.arm());
	assert(planner.start());
	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Applied;
	planner.tick(); // submit
	planner.tick(); // observe Applied, pop + record history

	assert(planner.selectNext(candidates, 2, chosen));
	assert(chosen.identity.fingerprint[0] == 2); // candidate 1 now excluded as recently played

	// Same-artist / same-title exclusion even with a different fingerprint.
	AutoDjCandidate sameArtist = makeCandidate(3, /*artistHash=*/1, /*titleHash=*/99);
	AutoDjCandidate onlyOption[1] = {sameArtist};
	// With only the same-artist candidate available, the fallback path must
	// still return it (queue can't starve forever) but without claiming
	// artist/title variety.
	assert(planner.selectNext(onlyOption, 1, chosen));
	assert(chosen.identity.fingerprint[0] == 3);
	assert(chosen.reasons & AUTO_DJ_REASON_TIE_BREAK);
	assert(!(chosen.reasons & AUTO_DJ_REASON_ARTIST_VARIETY));
}

void testPlannerTieBreakIsOrderIndependent(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);

	AutoDjCandidate forward[2] = {makeCandidate(5), makeCandidate(9)};
	AutoDjCandidate reversed[2] = {makeCandidate(9), makeCandidate(5)};

	AutoDjCandidate chosenForward, chosenReversed;
	assert(planner.selectNext(forward, 2, chosenForward));
	assert(planner.selectNext(reversed, 2, chosenReversed));
	assert(chosenForward.identity.fingerprint[0] == 5);
	assert(chosenReversed.identity.fingerprint[0] == 5);
}

void testNoSafeWindowKeepsArmed(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);

	assert(planner.arm());
	assert(planner.state() == AutoDjState::Armed);
	// Empty queue: no safe window yet, must not start.
	assert(!planner.start());
	assert(planner.state() == AutoDjState::Armed);

	assert(planner.pinTrack(makeIdentity(1)));
	assert(planner.start());
	assert(planner.state() == AutoDjState::Running);
}

void testCapabilityDisabledBlocksArm(){
	MockLoadPort port;
	port.stableIdEndpoint = false;
	DjAutoDjPlanner planner(port);

	assert(!planner.arm());
	assert(planner.state() == AutoDjState::Failed);
	assert(planner.failReason() == AutoDjFailReason::CapabilityDisabled);
}

// -- Round-4 review fix #3: Failed is terminal and must only be left via an
// explicit reset() - stop() must never be usable to slip Failed straight
// to Stopping/Off (which would let Auto DJ re-arm without the caller ever
// having acknowledged the failure, and without whatever settled-rollback
// guarantee reset() provides at the actuator layer). --
void testStopRejectedFromFailedRequiresReset(){
	MockLoadPort port;
	port.stableIdEndpoint = false;
	DjAutoDjPlanner planner(port);

	assert(!planner.arm());
	assert(planner.state() == AutoDjState::Failed);

	assert(!planner.stop());
	assert(planner.state() == AutoDjState::Failed); // unchanged - stop() must not apply.

	assert(planner.reset());
	assert(planner.state() == AutoDjState::Off);

	port.stableIdEndpoint = true;
	assert(planner.arm());
	assert(planner.state() == AutoDjState::Armed);
}

void testMissingConfirmationKeepsOff(){
	MockLoadPort port;
	port.confirmed = false;
	DjAutoDjPlanner planner(port);

	assert(!planner.arm());
	assert(planner.state() == AutoDjState::Off);
}

void testLoadAppliedEndToEnd(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.pinTrack(makeIdentity(7)));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Applied;

	planner.tick(); // submits the load
	assert(port.submitCount == 1);
	assert(planner.queueDepth() == 1); // not popped until Applied is observed

	planner.tick(); // observes Applied
	assert(planner.queueDepth() == 0);
	assert(planner.historySize() == 1);
	assert(planner.state() == AutoDjState::Running);
}

void testLoadFailureRetryBudgetThenSkip(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.pinTrack(makeIdentity(8)));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Failed;

	const int maxTicksAllowed = 32; // generous bound; must converge well before this
	bool skipped = false;
	for(int i = 0; i < maxTicksAllowed && !skipped; i++){
		planner.tick();
		if(planner.queueDepth() == 0) skipped = true;
	}
	assert(skipped);
	// One initial attempt + AUTO_DJ_RETRY_BUDGET retries.
	assert(port.submitCount == 1 + AUTO_DJ_RETRY_BUDGET);
	assert(planner.state() == AutoDjState::Running); // a single bad track doesn't fail the session
}

void testLoadTimeoutIsBounded(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.pinTrack(makeIdentity(11)));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Pending; // never resolves on its own

	// Each attempt costs AUTO_DJ_LOAD_TIMEOUT_US of (fake) wall-clock time
	// before it's treated as a failure; (budget + 1) attempts must all
	// time out and skip. One tick submits the load, then one tick per
	// timed-out attempt after the fake clock crosses the deadline; a small
	// fixed number of ticks is more than enough headroom.
	bool skipped = false;
	for(int attempt = 0; attempt <= AUTO_DJ_RETRY_BUDGET && !skipped; attempt++){
		planner.tick(); // submits (or re-submits) the load
		port.advanceMicros(AUTO_DJ_LOAD_TIMEOUT_US + 1);
		planner.tick(); // observes the deadline has passed -> handleLoadFailure()
		if(planner.queueDepth() == 0) skipped = true;
	}
	assert(skipped); // proves the timeout path converges within a bounded number of ticks, never hangs
}

void testManualTakeoverPausesAndCancelsPending(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.pinTrack(makeIdentity(12)));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Pending;
	planner.tick(); // submits, now waiting
	assert(port.submitCount == 1);

	port.manualTakeover = true;
	planner.tick();
	assert(planner.state() == AutoDjState::Paused);

	// Resuming after takeover ends starts a fresh attempt, not a stale one.
	port.manualTakeover = false;
	assert(planner.resume());
	port.nextOutcome = AutoDjLoadOutcome::Applied;
	planner.tick(); // fresh submit
	assert(port.submitCount == 2);
	planner.tick(); // applied
	assert(planner.queueDepth() == 0);
}

void testRecordingFailureNeverAutonomouslyResolved(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.pinTrack(makeIdentity(13)));
	assert(planner.arm());
	assert(planner.start());

	port.recording = true;
	planner.tick();
	// Auto DJ never starts/stops a recording itself; a recording failure is
	// terminal for this run until the user acknowledges it via reset().
	assert(planner.state() == AutoDjState::Failed);
	assert(planner.failReason() == AutoDjFailReason::RecordingFailure);
	assert(!planner.resume());
	assert(planner.reset());
	assert(planner.state() == AutoDjState::Off);
}

void testPauseResumeStopSequence(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.pinTrack(makeIdentity(14)));
	assert(planner.arm());
	assert(planner.start());
	assert(planner.state() == AutoDjState::Running);

	assert(planner.pause());
	assert(planner.state() == AutoDjState::Paused);

	port.confirmed = false;
	assert(!planner.resume()); // resume requires confirmation, same as arm
	assert(planner.state() == AutoDjState::Paused);

	port.confirmed = true;
	assert(planner.resume());
	assert(planner.state() == AutoDjState::Running);

	assert(planner.stop());
	assert(planner.state() == AutoDjState::Stopping);
	planner.tick(); // finishStop: queue still has the pinned entry -> Off, not Complete
	assert(planner.state() == AutoDjState::Off);
}

void testStopWaitsForPendingLoadBeforeGoingOff(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.pinTrack(makeIdentity(16)));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Pending;
	planner.tick(); // submits, now waiting on hardware
	assert(port.submitCount == 1);

	assert(planner.stop());
	assert(planner.state() == AutoDjState::Stopping);
	planner.tick(); // still pending: must not abandon the in-flight command yet
	assert(planner.state() == AutoDjState::Stopping);
	assert(planner.queueDepth() == 1);

	port.nextOutcome = AutoDjLoadOutcome::Applied;
	planner.tick(); // resolves the in-flight command (recorded to history)
	assert(planner.queueDepth() == 0);
	assert(planner.historySize() == 1);
	planner.tick(); // no pending left: now finish stopping
	assert(planner.state() == AutoDjState::Off);
}

void testEndOfTrackTriggersLoadOnlyWhenTrustworthy(){
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.pinTrack(makeIdentity(15)));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = false;
	planner.tick();
	assert(port.submitCount == 0); // not at EOF yet, nothing to do

	port.trackAtEnd = true;
	port.durationTrustworthy = false;
	planner.tick();
	assert(port.submitCount == 0); // EOF but not trustworthy -> pause, never guess
	assert(planner.state() == AutoDjState::Paused);

	assert(planner.resume());
	port.durationTrustworthy = true;
	planner.tick();
	assert(port.submitCount == 1);
}

void testMissingMetadataNeverClaimsMatch(){
	AutoDjCandidate withMetadata = makeCandidate(20, 0, 0, /*hasMetadata=*/true, /*score=*/50);
	AutoDjCandidate withoutMetadata = makeCandidate(21, 0, 0, /*hasMetadata=*/false);
	AutoDjCandidate candidates[2] = {withoutMetadata, withMetadata};

	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	AutoDjCandidate chosen;
	assert(planner.selectNext(candidates, 2, chosen));
	// The candidate with real metadata always outranks the flat fallback score.
	assert(chosen.identity.fingerprint[0] == 20);
	assert(chosen.reasons & AUTO_DJ_REASON_METADATA_MATCH);
	assert(!(chosen.reasons & AUTO_DJ_REASON_CONSERVATIVE_FALLBACK));
}

// Regression for a review finding: a pending load must resolve (record to
// history / drop / retry) the exact queue entry that was submitted, never
// "whatever is currently at the queue front" - which changes if a track is
// pinned while that load is in flight. Covered for every resolution path:
// Applied, Failed (retry-then-skip), Pending timeout, and Stopping.

void testPinDuringPendingLoadAppliedResolvesSubmittedEntry(){
	AutoDjCandidate candidateX = makeCandidate(30);
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.planNext(&candidateX, 1)); // X queued as a planned entry
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Pending; // stays in flight so we can pin mid-load
	planner.tick(); // submits X
	assert(port.submitCount == 1);
	assert(port.lastSubmitted.sameTrack(candidateX.identity));

	AutoDjIdentity trackY = makeIdentity(31);
	assert(planner.pinTrack(trackY)); // user pins Y while X's load is still pending
	assert(planner.queueDepth() == 2);

	port.nextOutcome = AutoDjLoadOutcome::Applied;
	planner.tick(); // resolves X's outcome specifically, not Y's

	assert(planner.historySize() == 1); // only X recorded
	assert(!planner.isQueued(candidateX.identity)); // X removed
	assert(planner.isQueued(trackY)); // Y untouched, still queued
	assert(planner.queueDepth() == 1);
}

void testPinDuringPendingLoadFailureDropsSubmittedEntryNotFront(){
	AutoDjCandidate candidateX = makeCandidate(32);
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.planNext(&candidateX, 1));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Failed;

	planner.tick(); // attempt 1: submits X
	assert(port.submitCount == 1);
	assert(port.lastSubmitted.sameTrack(candidateX.identity));

	AutoDjIdentity trackY = makeIdentity(33);
	// Pinned entries are served ahead of planned ones, so without the fix
	// this would become the new queue front and hijack X's retries.
	assert(planner.pinTrack(trackY));

	const int maxTicksAllowed = 32;
	bool dropped = false;
	for(int i = 0; i < maxTicksAllowed && !dropped; i++){
		planner.tick();
		if(!planner.isQueued(candidateX.identity)) dropped = true;
	}
	assert(dropped);
	assert(port.submitCount == 1 + AUTO_DJ_RETRY_BUDGET); // every retry targeted X
	assert(port.lastSubmitted.sameTrack(candidateX.identity)); // never Y
	assert(planner.isQueued(trackY)); // Y untouched throughout
	assert(planner.historySize() == 0); // dropped, never recorded as played
	assert(planner.queueDepth() == 1); // only Y remains
}

void testPinDuringPendingLoadTimeoutRetriesSubmittedEntryNotFront(){
	AutoDjCandidate candidateX = makeCandidate(34);
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.planNext(&candidateX, 1));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Pending; // never resolves on its own -> times out

	planner.tick(); // attempt 1: submits X
	assert(port.submitCount == 1);

	AutoDjIdentity trackY = makeIdentity(35);
	assert(planner.pinTrack(trackY));

	bool dropped = false;
	for(int attempt = 0; attempt <= AUTO_DJ_RETRY_BUDGET && !dropped; attempt++){
		port.advanceMicros(AUTO_DJ_LOAD_TIMEOUT_US + 1);
		planner.tick(); // observes the deadline has passed for this attempt
		if(!planner.isQueued(candidateX.identity)) dropped = true;
		else planner.tick(); // re-submits X for the next attempt
	}
	assert(dropped);
	assert(port.submitCount == 1 + AUTO_DJ_RETRY_BUDGET); // every timeout retried X specifically
	assert(planner.isQueued(trackY));
	assert(planner.historySize() == 0);
}

void testPinDuringStopResolvesSubmittedEntryNotFront(){
	AutoDjCandidate candidateX = makeCandidate(36);
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.planNext(&candidateX, 1));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Pending;
	planner.tick(); // submits X, now waiting on hardware
	assert(port.submitCount == 1);

	AutoDjIdentity trackY = makeIdentity(37);
	assert(planner.pinTrack(trackY)); // pinned while X's load is in flight

	assert(planner.stop());
	assert(planner.state() == AutoDjState::Stopping);
	planner.tick(); // still pending: must not abandon X yet
	assert(planner.state() == AutoDjState::Stopping);

	port.nextOutcome = AutoDjLoadOutcome::Applied;
	planner.tick(); // resolves X specifically into history, not Y
	assert(planner.historySize() == 1);
	assert(!planner.isQueued(candidateX.identity));
	assert(planner.isQueued(trackY)); // Y untouched, remains queued

	planner.tick(); // no pending left: finish stopping
	assert(planner.state() == AutoDjState::Off);
	assert(planner.queueDepth() == 1); // only Y remains
}

// Regression for a review finding: planNext()/selectNext() only checked
// history, not identities already sitting in the queue, so repeatedly
// planning against the same candidate list could queue the same track more
// than once. Every distinct candidate must be queued exactly once, and once
// they're all queued, planNext() must report no eligible candidate rather
// than duplicating one.
void testPlanNextNeverQueuesSameIdentityTwice(){
	AutoDjCandidate candidates[3] = {
		makeCandidate(40),
		makeCandidate(41),
		makeCandidate(42)
	};
	MockLoadPort port;
	DjAutoDjPlanner planner(port);

	assert(planner.planNext(candidates, 3));
	assert(planner.planNext(candidates, 3));
	assert(planner.planNext(candidates, 3));
	assert(planner.queueDepth() == 3);
	assert(planner.isQueued(candidates[0].identity));
	assert(planner.isQueued(candidates[1].identity));
	assert(planner.isQueued(candidates[2].identity));

	assert(!planner.planNext(candidates, 3)); // every candidate is already queued
	assert(planner.queueDepth() == 3); // unchanged - no duplicate pushed
}

// Regression for a review finding: a retry must never resubmit an entry
// that was dropped out from under it. If the library re-indexes (a new
// generation) between a failed attempt and its retry, the stale entry is
// invalidated out of the queue; beginNextLoad() must notice this before
// resubmitting and abandon the attempt cleanly instead of sending a
// foreign/stale identity to the load port.
void testInvalidatedGenerationDuringRetryNeverResubmitsStaleEntry(){
	AutoDjCandidate candidateX = makeCandidate(50); // generation 1, via makeIdentity()'s default
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.planNext(&candidateX, 1)); // X queued, generation 1
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.submitShouldSucceed = false; // first attempt fails immediately, retry scheduled

	planner.tick(); // attempt 1: submitLoad(X) fails -> pendingAttempts == 1, retry pending
	assert(port.submitCount == 1);
	assert(port.lastSubmitted.sameTrack(candidateX.identity));

	// The library re-indexes to generation 2 while X is still the planner's
	// captured retry target - this drops X out of the queue outright.
	planner.invalidateLibraryGeneration(2);
	assert(!planner.isQueued(candidateX.identity));

	const int maxTicksAllowed = 8; // generous bound; must converge well before this
	for(int i = 0; i < maxTicksAllowed; i++){
		planner.tick();
	}
	// The stale entry must never be resubmitted - the one failed attempt
	// above is the only submission that ever happens. With nothing left to
	// queue, the planner converges to Complete rather than spinning.
	assert(port.submitCount == 1);
	assert(planner.historySize() == 0);
	assert(planner.queueDepth() == 0);
	assert(planner.state() == AutoDjState::Complete);
}

// Same invalidation-during-flight scenario, but for the Stopping path: if
// the queue entry is invalidated while its load is in flight during
// shutdown, resolving that outcome (even a belated Applied) must not
// fabricate a history record for an entry that's already gone.
void testInvalidatedGenerationDuringStoppingNeverRecordsStaleEntry(){
	AutoDjCandidate candidateX = makeCandidate(51);
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.planNext(&candidateX, 1));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Pending; // stays in flight
	planner.tick(); // submits X successfully, now WaitingOutcome
	assert(port.submitCount == 1);

	assert(planner.stop());
	assert(planner.state() == AutoDjState::Stopping);

	// The library re-indexes while X's load is still in flight during
	// shutdown; invalidation drops the now-stale queue entry outright.
	planner.invalidateLibraryGeneration(2);
	assert(!planner.isQueued(candidateX.identity));

	port.nextOutcome = AutoDjLoadOutcome::Applied; // hardware reports success for the invalidated load
	planner.tick(); // resolvePendingWhileStopping(): must not record a stale entry
	assert(planner.historySize() == 0);

	planner.tick(); // no pending left: finish stopping
	assert(planner.state() == AutoDjState::Off);
}

// Sibling of testInvalidatedGenerationDuringRetryNeverResubmitsStaleEntry()
// above, keyed on metadataRevision instead of libraryGeneration: a
// same-generation metadata replacement (sidecar re-tag) mid-retry must
// abandon the stale attempt exactly like a generation change already does
// - this is the exact issue #3 regression (submitLoad() discarding
// identity generation/revision).
void testInvalidatedRevisionDuringRetryNeverResubmitsStaleEntry(){
	AutoDjCandidate candidateX = makeCandidate(52); // generation 1, revision 1 via makeIdentity()'s defaults
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.planNext(&candidateX, 1)); // X queued, revision 1
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.submitShouldSucceed = false; // first attempt fails immediately, retry scheduled

	planner.tick(); // attempt 1: submitLoad(X) fails -> pendingAttempts == 1, retry pending
	assert(port.submitCount == 1);
	assert(port.lastSubmitted.sameTrack(candidateX.identity));

	// Metadata is replaced (same generation, revision advances to 2) while
	// X is still the planner's captured retry target - this drops X out of
	// the queue outright.
	planner.invalidateMetadataRevision(2);
	assert(!planner.isQueued(candidateX.identity));

	const int maxTicksAllowed = 8; // generous bound; must converge well before this
	for(int i = 0; i < maxTicksAllowed; i++){
		planner.tick();
	}
	// The stale entry must never be resubmitted - the one failed attempt
	// above is the only submission that ever happens.
	assert(port.submitCount == 1);
	assert(planner.historySize() == 0);
	assert(planner.queueDepth() == 0);
	assert(planner.state() == AutoDjState::Complete);
}

// Same revision-invalidation-during-flight scenario, but for the Stopping
// path - mirrors testInvalidatedGenerationDuringStoppingNeverRecordsStaleEntry().
void testInvalidatedRevisionDuringStoppingNeverRecordsStaleEntry(){
	AutoDjCandidate candidateX = makeCandidate(53);
	MockLoadPort port;
	DjAutoDjPlanner planner(port);
	assert(planner.planNext(&candidateX, 1));
	assert(planner.arm());
	assert(planner.start());

	port.trackAtEnd = true;
	port.durationTrustworthy = true;
	port.nextOutcome = AutoDjLoadOutcome::Pending; // stays in flight
	planner.tick(); // submits X successfully, now WaitingOutcome
	assert(port.submitCount == 1);

	assert(planner.stop());
	assert(planner.state() == AutoDjState::Stopping);

	// Same-generation metadata replacement while X's load is still in
	// flight during shutdown; invalidation drops the now-stale queue entry.
	planner.invalidateMetadataRevision(2);
	assert(!planner.isQueued(candidateX.identity));

	port.nextOutcome = AutoDjLoadOutcome::Applied; // hardware reports success for the invalidated load
	planner.tick(); // resolvePendingWhileStopping(): must not record a stale entry
	assert(planner.historySize() == 0);

	planner.tick(); // no pending left: finish stopping
	assert(planner.state() == AutoDjState::Off);
}

} // namespace

int main(){
	testQueuePinnedOrder();
	testQueueFullEmpty();
	testQueueInvalidation();
	testQueueRevisionInvalidation();
	testQueueDoesNotAliasSameFingerprintDifferentSource();
	testHistoryDoesNotAliasSameFingerprintDifferentSource();
	testHistoryExclusion();
	testPlannerSelectionExcludesRecentArtistTitle();
	testPlannerTieBreakIsOrderIndependent();
	testNoSafeWindowKeepsArmed();
	testCapabilityDisabledBlocksArm();
	testStopRejectedFromFailedRequiresReset();
	testMissingConfirmationKeepsOff();
	testLoadAppliedEndToEnd();
	testLoadFailureRetryBudgetThenSkip();
	testLoadTimeoutIsBounded();
	testManualTakeoverPausesAndCancelsPending();
	testRecordingFailureNeverAutonomouslyResolved();
	testPauseResumeStopSequence();
	testStopWaitsForPendingLoadBeforeGoingOff();
	testEndOfTrackTriggersLoadOnlyWhenTrustworthy();
	testMissingMetadataNeverClaimsMatch();
	testPinDuringPendingLoadAppliedResolvesSubmittedEntry();
	testPinDuringPendingLoadFailureDropsSubmittedEntryNotFront();
	testPinDuringPendingLoadTimeoutRetriesSubmittedEntryNotFront();
	testPinDuringStopResolvesSubmittedEntryNotFront();
	testPlanNextNeverQueuesSameIdentityTwice();
	testInvalidatedGenerationDuringRetryNeverResubmitsStaleEntry();
	testInvalidatedGenerationDuringStoppingNeverRecordsStaleEntry();
	testInvalidatedRevisionDuringRetryNeverResubmitsStaleEntry();
	testInvalidatedRevisionDuringStoppingNeverRecordsStaleEntry();
	return 0;
}
