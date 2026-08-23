#include <assert.h>
#include <string.h>

#include "../src/DjAssist/DjAssistGridCache.h"
#include "../src/DjAssist/DjAssistSessionBridge.h"

using namespace DjAssistBridge;

namespace {

DjTrackIdentity fingerprintIdentity(uint8_t seed){
	DjTrackIdentity identity = {};
	identity.flags = DJ_TRACK_IDENTITY_FINGERPRINT;
	memset(identity.fingerprint, seed, sizeof(identity.fingerprint));
	return identity;
}

// -- buildTrackIdentity: zero-array evidence gating -----------------------

void testBuildTrackIdentityGatesZeroEvidence(){
	uint8_t zero[16] = {};
	uint8_t fp[16];
	memset(fp, 0xAB, sizeof(fp));
	uint8_t src[16];
	memset(src, 0xCD, sizeof(src));

	// Both zero: no evidence at all.
	const DjTrackIdentity none = buildTrackIdentity(zero, zero);
	assert(none.flags == 0);

	// Only fingerprint real.
	const DjTrackIdentity fpOnly = buildTrackIdentity(fp, zero);
	assert(fpOnly.flags == DJ_TRACK_IDENTITY_FINGERPRINT);
	assert(memcmp(fpOnly.fingerprint, fp, 16) == 0);

	// Only sourceId real.
	const DjTrackIdentity srcOnly = buildTrackIdentity(zero, src);
	assert(srcOnly.flags == DJ_TRACK_IDENTITY_SOURCE);
	assert(memcmp(srcOnly.sourceId, src, 16) == 0);

	// Both real.
	const DjTrackIdentity both = buildTrackIdentity(fp, src);
	assert(both.flags == (DJ_TRACK_IDENTITY_FINGERPRINT | DJ_TRACK_IDENTITY_SOURCE));
	assert(memcmp(both.fingerprint, fp, 16) == 0);
	assert(memcmp(both.sourceId, src, 16) == 0);

	// Two independently-built all-zero identities must never claim to
	// match via DjAssistScoring::identityMatches (no comparable evidence).
	const DjTrackIdentity otherNone = buildTrackIdentity(zero, zero);
	assert(none.flags == otherNone.flags && none.flags == 0);
}

// -- buildLibraryEntry: capability bits mirror DjSession::resolveMetadata()
// -- exactly (minus confidence/downbeatCount, which need their own reader
// -- calls beyond the single trackByIndex() this entry's other fields came
// -- from - DjSession::assistTrackEntry() fills those in separately).

void testBuildLibraryEntryFullCapabilities(){
	const DjTrackIdentity identity = fingerprintIdentity(7);
	const DjAssistLibraryEntry entry = buildLibraryEntry(
		42, identity, DJ_METADATA_VALID,
		44100, 44100ULL * 180ULL, 128000, 0x105, 4,
		/*cueCount*/ 3, /*gridCount*/ 64, /*phraseCount*/ 8,
		/*firstGrid*/ 1000, /*firstPhrase*/ 2000
	);
	assert(entry.libraryIndex == 42);
	assert(entry.state == DJ_METADATA_VALID);
	assert(entry.bpmMilli == 128000);
	assert(entry.key == 0x105);
	assert(entry.rating == 4);
	assert(entry.durationFrames == 44100ULL * 180ULL);
	assert(entry.sampleRate == 44100);
	assert(memcmp(entry.identity.fingerprint, identity.fingerprint, 16) == 0);
	// firstGrid/firstPhrase/gridCount/phraseCount/cueCount are threaded
	// through verbatim - DjSession::resolveIdentityLoad() reconstructs a
	// JaydMetadata::Track purely from these, so they must round-trip
	// exactly, not just influence capability bits.
	assert(entry.gridCount == 64);
	assert(entry.phraseCount == 8);
	assert(entry.cueCount == 3);
	assert(entry.firstGrid == 1000);
	assert(entry.firstPhrase == 2000);
	const uint16_t expected = DJ_METADATA_HAS_SOURCE_FRAMES | DJ_METADATA_HAS_BPM | DJ_METADATA_HAS_KEY |
		DJ_METADATA_HAS_RATING | DJ_METADATA_HAS_CUES | DJ_METADATA_HAS_GRID | DJ_METADATA_HAS_PHRASES;
	assert(entry.capabilities == expected);
	// Never set by buildLibraryEntry() itself (see header comment) -
	// assistTrackEntry() sets this bit only after its own bounded
	// grid-confidence scan finds at least one downbeat.
	assert(!(entry.capabilities & DJ_METADATA_HAS_DOWNBEATS));
}

void testBuildLibraryEntryMissingFieldsClearBitsOnly(){
	// Every field absent/zero/sentinel: every bit clears, but the call still
	// returns a usable (lower-confidence, never rejected) entry - no field
	// individually forces exclusion.
	const DjAssistLibraryEntry entry = buildLibraryEntry(
		0, DjTrackIdentity{}, DJ_METADATA_STALE,
		0, 0, 0, 0, 255,
		0, 0, 0, 0, 0
	);
	assert(entry.capabilities == 0);
	assert(entry.state == DJ_METADATA_STALE);
	assert(entry.rating == 255);

	// Rating sentinel boundary: 255 must never set HAS_RATING; any real
	// 0-5 rating (including 0, a legitimate unrated-low value) must.
	const DjAssistLibraryEntry rated = buildLibraryEntry(
		0, DjTrackIdentity{}, DJ_METADATA_VALID,
		44100, 1000, 120000, 5, 0,
		0, 0, 0, 0, 0
	);
	assert(rated.capabilities & DJ_METADATA_HAS_RATING);

	// sampleRate/durationFrames must BOTH be present for HAS_SOURCE_FRAMES,
	// matching resolveMetadata()'s `track.sampleRate && track.durationFrames`.
	const DjAssistLibraryEntry halfSource = buildLibraryEntry(
		0, DjTrackIdentity{}, DJ_METADATA_VALID,
		44100, 0, 0, 0, 255,
		0, 0, 0, 0, 0
	);
	assert(!(halfSource.capabilities & DJ_METADATA_HAS_SOURCE_FRAMES));
}

// -- computeCrossfadeMix: endpoints, direction, checked math --------------

void testCrossfadeMixEndpoints(){
	// toDeck==1: mix ramps 0 (start) -> 255 (complete).
	assert(computeCrossfadeMix(1, 0, 16, 128000) == 0);
	// toDeck==0: mix ramps 255 (start) -> 0 (complete) - the mirror image.
	assert(computeCrossfadeMix(0, 0, 16, 128000) == 255);

	// microsPerBeat at 120.000 bpm == 500,000us. 16 beats == 8,000,000us.
	// Well past the end: saturate exactly at the target endpoint.
	assert(computeCrossfadeMix(1, 100000000ULL, 16, 120000) == 255);
	assert(computeCrossfadeMix(0, 100000000ULL, 16, 120000) == 0);

	// Exactly halfway (8 of 16 beats): curve(8,16) == 127 (255*8/16).
	const uint64_t halfwayMicros = 4000000ULL; // 8 beats * 500,000us
	assert(computeCrossfadeMix(1, halfwayMicros, 16, 120000) == 127);
	assert(computeCrossfadeMix(0, halfwayMicros, 16, 120000) == 255 - 127);
}

void testCrossfadeMixCheckedDivideByZero(){
	// bpmMilli == 0: no valid tempo, snap immediately to the target rather
	// than dividing by zero.
	assert(computeCrossfadeMix(1, 0, 16, 0) == 255);
	assert(computeCrossfadeMix(0, 0, 16, 0) == 0);
	// crossfadeBeats == 0: same fallback (an instant cut is a degenerate
	// but valid "0-beat crossfade").
	assert(computeCrossfadeMix(1, 12345, 0, 128000) == 255);
	assert(computeCrossfadeMix(0, 12345, 0, 128000) == 0);
}

void testCrossfadeMixOverflowGuard(){
	// Absurdly large elapsed time and a tiny bpm: elapsedBeats would
	// overflow uint16_t internally; must clamp/saturate, never wrap back
	// around to a small stepIndex.
	assert(computeCrossfadeMix(1, 0xFFFFFFFFFFFFFFFFULL, 32, 1) == 255);
	assert(computeCrossfadeMix(0, 0xFFFFFFFFFFFFFFFFULL, 32, 1) == 0);
}

// -- nextRollbackPhase: skips phases with nothing to undo/nothing plan-
// -- owned, deterministic --------------------------------------------------

void testNextRollbackPhaseFullSequence(){
	// Every side effect was submitted and plan-owned: mix -> sync ->
	// stop-deck -> done, never skipping a phase.
	DjAssistRollbackPhase phase = DJ_ASSIST_ROLLBACK_IDLE;
	phase = nextRollbackPhase(phase, /*crossfade*/true, /*manualMixOccurred*/false,
		/*syncOwned*/true, /*startDeckOwned*/true);
	assert(phase == DJ_ASSIST_ROLLBACK_MIX);
	phase = nextRollbackPhase(DJ_ASSIST_ROLLBACK_SYNC, true, false, true, true);
	assert(phase == DJ_ASSIST_ROLLBACK_SYNC);
	phase = nextRollbackPhase(DJ_ASSIST_ROLLBACK_STOP_DECK, true, false, true, true);
	assert(phase == DJ_ASSIST_ROLLBACK_STOP_DECK);
	phase = nextRollbackPhase(DJ_ASSIST_ROLLBACK_DONE, true, false, true, true);
	assert(phase == DJ_ASSIST_ROLLBACK_DONE);
}

void testNextRollbackPhaseSkipsUnsubmittedSteps(){
	// Nothing was ever submitted (e.g. failed during WAIT_BOUNDARY, before
	// any actuator action): every phase must be skipped straight to DONE -
	// there is nothing to undo.
	assert(nextRollbackPhase(DJ_ASSIST_ROLLBACK_IDLE, false, false, false, false) == DJ_ASSIST_ROLLBACK_DONE);

	// Only the start-deck step was submitted and plan-owned (failed right
	// after START_DECK, before LOCK_TEMPO/ENABLE_SYNC or CROSSFADE ever
	// ran): mix and sync phases must both be skipped, landing directly on
	// STOP_DECK.
	assert(nextRollbackPhase(DJ_ASSIST_ROLLBACK_IDLE, false, false, false, true) == DJ_ASSIST_ROLLBACK_STOP_DECK);

	// Mix (crossfade) was submitted but sync/start-deck were not (e.g. a
	// tempoLock=false, startAtBoundary=false plan that failed mid-ramp):
	// only the MIX phase runs, then falls straight to DONE.
	assert(nextRollbackPhase(DJ_ASSIST_ROLLBACK_IDLE, true, false, false, false) == DJ_ASSIST_ROLLBACK_MIX);
	assert(nextRollbackPhase(DJ_ASSIST_ROLLBACK_SYNC, true, false, false, false) == DJ_ASSIST_ROLLBACK_DONE);
}

void testNextRollbackPhaseSkipsOwnershipWhenAlreadyInThatState(){
	// The sync/start-deck steps were both submitted (buildSteps() always
	// includes them), but the target deck was ALREADY playing/synced
	// before the transition armed - those steps were idempotent no-ops the
	// plan did not actually introduce, so rollback must skip straight from
	// MIX to DONE without touching sync/playback state a user set up
	// beforehand.
	DjAssistRollbackPhase phase = nextRollbackPhase(
		DJ_ASSIST_ROLLBACK_IDLE, /*crossfade*/true, /*manualMixOccurred*/false,
		/*syncOwned*/false, /*startDeckOwned*/false
	);
	assert(phase == DJ_ASSIST_ROLLBACK_MIX);
	phase = nextRollbackPhase(DJ_ASSIST_ROLLBACK_SYNC, true, false, false, false);
	assert(phase == DJ_ASSIST_ROLLBACK_DONE);
}

void testNextRollbackPhaseSkipsMixOnManualOverride(){
	// A manual (non-system) mix change happened at any point after arming:
	// the MIX phase must be skipped entirely, even though crossfade WAS
	// submitted by the plan - restoring armedMix would silently overwrite
	// the user's own action.
	DjAssistRollbackPhase phase = nextRollbackPhase(
		DJ_ASSIST_ROLLBACK_IDLE, /*crossfade*/true, /*manualMixOccurred*/true,
		/*syncOwned*/true, /*startDeckOwned*/true
	);
	assert(phase == DJ_ASSIST_ROLLBACK_SYNC);
}

// -- evaluateCommandOutcome: exhaustive status -> outcome mapping ---------

void testEvaluateCommandOutcomeMapping(){
	assert(evaluateCommandOutcome(DJ_COMMAND_APPLIED) == DJ_ASSIST_COMMAND_DONE);
	assert(evaluateCommandOutcome(DJ_COMMAND_REJECTED) == DJ_ASSIST_COMMAND_FAILED);
	assert(evaluateCommandOutcome(DJ_COMMAND_FAILED) == DJ_ASSIST_COMMAND_FAILED);
	assert(evaluateCommandOutcome(DJ_COMMAND_SUPERSEDED) == DJ_ASSIST_COMMAND_RESUBMIT);
	assert(evaluateCommandOutcome(DJ_COMMAND_ACCEPTED) == DJ_ASSIST_COMMAND_WAIT);
	assert(evaluateCommandOutcome(DJ_COMMAND_PENDING) == DJ_ASSIST_COMMAND_WAIT);
}

// -- evaluateBoundaryArrival: tolerance window, never released late ------

void testEvaluateBoundaryArrivalNotYet(){
	assert(evaluateBoundaryArrival(0, 1000) == DJ_ASSIST_BOUNDARY_NOT_YET);
	assert(evaluateBoundaryArrival(999, 1000) == DJ_ASSIST_BOUNDARY_NOT_YET);
}

void testEvaluateBoundaryArrivalReachedWithinTolerance(){
	// Exactly on target, and up to DJ_QUANTIZE_TOLERANCE_FRAMES past it,
	// both count as reached.
	assert(evaluateBoundaryArrival(1000, 1000) == DJ_ASSIST_BOUNDARY_REACHED);
	assert(evaluateBoundaryArrival(1000 + DJ_QUANTIZE_TOLERANCE_FRAMES, 1000) == DJ_ASSIST_BOUNDARY_REACHED);
}

void testEvaluateBoundaryArrivalMissedBeyondTolerance(){
	assert(evaluateBoundaryArrival(1000 + DJ_QUANTIZE_TOLERANCE_FRAMES + 1, 1000) == DJ_ASSIST_BOUNDARY_MISSED);
	assert(evaluateBoundaryArrival(1000000, 1000) == DJ_ASSIST_BOUNDARY_MISSED);
}

// -- phraseCacheNeedsRescan / updatePhraseCache: terminal-aware throttle -

// Fixed metadata generation/state used by every call below except the
// dedicated metadata-invalidation test - keeps the existing scenarios
// focused on identity/frame behavior without threading a real generation
// counter through them.
static const uint32_t kGen = 7;
static const DjMetadataState kMetaState = DJ_METADATA_VALID;

void testPhraseCacheNeedsRescanWhenEmpty(){
	DjAssistPhraseCacheState cache;
	const DjTrackIdentity identity = fingerprintIdentity(1);
	assert(phraseCacheNeedsRescan(cache, 0, 0, identity, kGen, kMetaState));
}

void testPhraseCacheDoesNotRescanBeforeFoundBoundaryPassed(){
	DjAssistPhraseCacheState cache;
	const DjTrackIdentity identity = fingerprintIdentity(1);
	updatePhraseCache(cache, /*deck*/0, /*currentFrame*/100, identity, kGen, kMetaState, /*found*/true, /*phraseFrame*/5000);
	assert(!phraseCacheNeedsRescan(cache, 0, 200, identity, kGen, kMetaState)); // still well before 5000.
	assert(!phraseCacheNeedsRescan(cache, 0, 4999, identity, kGen, kMetaState));
	assert(phraseCacheNeedsRescan(cache, 0, 5000, identity, kGen, kMetaState)); // reached/passed - refresh.
}

void testPhraseCacheDoesNotRescanTerminalJustBecauseStillTerminal(){
	// No future phrase exists (terminal). Forward playback alone must
	// NOT force a rescan - this is exactly the up-to-128-record-rescan-
	// every-tick bug the cache exists to prevent.
	DjAssistPhraseCacheState cache;
	const DjTrackIdentity identity = fingerprintIdentity(1);
	updatePhraseCache(cache, 0, 1000, identity, kGen, kMetaState, /*found*/false, 0);
	assert(!phraseCacheNeedsRescan(cache, 0, 1001, identity, kGen, kMetaState));
	assert(!phraseCacheNeedsRescan(cache, 0, 50000, identity, kGen, kMetaState));
}

void testPhraseCacheRescansOnIdentityChangeOrDeckChange(){
	DjAssistPhraseCacheState cache;
	const DjTrackIdentity a = fingerprintIdentity(1);
	const DjTrackIdentity b = fingerprintIdentity(2);
	updatePhraseCache(cache, 0, 1000, a, kGen, kMetaState, true, 5000);
	assert(phraseCacheNeedsRescan(cache, 0, 1500, b, kGen, kMetaState)); // new track loaded on same deck.
	assert(phraseCacheNeedsRescan(cache, 1, 1500, a, kGen, kMetaState)); // different deck entirely.
}

void testPhraseCacheRescansOnBackwardSeek(){
	DjAssistPhraseCacheState cache;
	const DjTrackIdentity identity = fingerprintIdentity(1);
	updatePhraseCache(cache, 0, 5000, identity, kGen, kMetaState, true, 9000);
	assert(!phraseCacheNeedsRescan(cache, 0, 5500, identity, kGen, kMetaState)); // still forward, before cached target.
	assert(phraseCacheNeedsRescan(cache, 0, 1000, identity, kGen, kMetaState)); // seeked backward.
}

// A metadata refresh that leaves the same track identity loaded on the same
// deck (e.g. re-resolved grid/phrase data, or a transient state cycle) must
// still invalidate a cached phrase result - the identity alone is not
// sufficient, since the underlying metadata backing that identity can
// change without the identity itself changing.
void testPhraseCacheRescansOnMetadataGenerationOrStateChange(){
	DjAssistPhraseCacheState cache;
	const DjTrackIdentity identity = fingerprintIdentity(1);
	updatePhraseCache(cache, 0, 1000, identity, kGen, kMetaState, true, 5000);
	assert(!phraseCacheNeedsRescan(cache, 0, 1500, identity, kGen, kMetaState)); // unchanged - no rescan.
	assert(phraseCacheNeedsRescan(cache, 0, 1500, identity, kGen + 1, kMetaState)); // generation bumped.
	assert(phraseCacheNeedsRescan(cache, 0, 1500, identity, kGen, DJ_METADATA_STALE)); // state changed.
}

// -- candidateGenerationCurrent: closes the fillWorkerStep() check-then- ---
// -- lock gap (issue #4). It is now a single equality check against a -----
// -- value that was itself captured atomically with the read it guards ----
// -- (DjSession::assistTrackEntry()'s outRevision, or a fresh immediate ----
// -- re-read taken right before the completion commit) - there is no ------
// -- separate "before" vs "after" probe left to reconcile here. -----------
void testCandidateGenerationCurrentMatchesExactly(){
	assert(DjAssistBridge::candidateGenerationCurrent(5, 5));
	assert(!DjAssistBridge::candidateGenerationCurrent(6, 5));
	assert(!DjAssistBridge::candidateGenerationCurrent(5, 6));
}

// -- DjAssistGridCache: bounded, few-slot on-demand grid-anchor hydration --
// -- cache (the round-4 PSRAM-budget fix's replacement for a per-candidate-
// -- entry gridAnchors[] array). Pure logic, no threading/locking of its
// -- own (DjAssistController serializes every call under candidateMutex_).

DjGridAnchor anchor(uint64_t frame, int64_t quarterBeat){
	DjGridAnchor a;
	a.frame = frame;
	a.quarterBeat = quarterBeat;
	return a;
}

void testGridCacheRequestFindPendingResolveLookupRoundTrip(){
	DjAssistGridCache cache;
	const DjTrackIdentity identity = fingerprintIdentity(1);
	assert(cache.request(10, 20, identity));

	const int pending = cache.findPending();
	assert(pending >= 0);
	assert(cache.at(uint8_t(pending)).state == DjAssistGridCacheState::Pending);

	DjGridAnchor anchors[2] = { anchor(100, 0), anchor(200, 4) };
	cache.resolve(pending, 10, 20, identity, true, anchors, 2);

	assert(cache.findPending() < 0); // no longer pending.
	DjGridAnchor out[DJ_GRID_ANCHOR_CAPACITY] = {};
	uint16_t outCount = 0;
	assert(cache.lookup(10, 20, identity, out, outCount));
	assert(outCount == 2);
	assert(out[0].frame == 100 && out[0].quarterBeat == 0);
	assert(out[1].frame == 200 && out[1].quarterBeat == 4);
}

void testGridCacheLookupMissForNeverRequestedKey(){
	DjAssistGridCache cache;
	DjGridAnchor out[DJ_GRID_ANCHOR_CAPACITY] = {};
	uint16_t outCount = 7; // non-zero sentinel - lookup() must reset it on a miss.
	assert(!cache.lookup(1, 1, fingerprintIdentity(9), out, outCount));
	assert(outCount == 0);
}

void testGridCacheLookupMissWhileStillPending(){
	DjAssistGridCache cache;
	const DjTrackIdentity identity = fingerprintIdentity(2);
	cache.request(1, 1, identity);
	DjGridAnchor out[DJ_GRID_ANCHOR_CAPACITY] = {};
	uint16_t outCount = 0;
	assert(!cache.lookup(1, 1, identity, out, outCount));
}

void testGridCacheResolveFailedLeavesLookupMiss(){
	DjAssistGridCache cache;
	const DjTrackIdentity identity = fingerprintIdentity(3);
	cache.request(1, 1, identity);
	const int pending = cache.findPending();
	assert(pending >= 0);
	cache.resolve(pending, 1, 1, identity, false, nullptr, 0);
	assert(cache.at(uint8_t(pending)).state == DjAssistGridCacheState::Failed);
	DjGridAnchor out[DJ_GRID_ANCHOR_CAPACITY] = {};
	uint16_t outCount = 0;
	assert(!cache.lookup(1, 1, identity, out, outCount));
}

// A resolve() targeting a slot that a NEWER request() has since evicted (or
// overwritten with a different key while the read was in flight) must be
// silently ignored - never let a late/stale result clobber the newer
// request's own state. Mirrors DjAssistController::stepGridHydration()'s
// "still Pending for the exact key" re-validation.
void testGridCacheResolveIgnoredAfterSlotReused(){
	DjAssistGridCache cache;
	const DjTrackIdentity identityA = fingerprintIdentity(4);
	cache.request(1, 1, identityA);
	const int pendingForA = cache.findPending();
	assert(pendingForA >= 0);

	// Evict slot `pendingForA` by cycling DJ_ASSIST_GRID_CACHE_SLOTS more
	// distinct requests through round-robin (request() itself never
	// blocks/fails - see its doc comment).
	for(uint8_t i = 0; i < DJ_ASSIST_GRID_CACHE_SLOTS; ++i){
		cache.request(1, 1, fingerprintIdentity(uint8_t(100 + i)));
	}
	// The original A key is no longer represented by ANY slot.
	DjGridAnchor out[DJ_GRID_ANCHOR_CAPACITY] = {};
	uint16_t outCount = 0;
	assert(!cache.lookup(1, 1, identityA, out, outCount));

	// A late resolve() for the now-stale (index, key) pair must be a no-op.
	DjGridAnchor anchors[1] = { anchor(1, 1) };
	cache.resolve(pendingForA, 1, 1, identityA, true, anchors, 1);
	assert(!cache.lookup(1, 1, identityA, out, outCount));
}

void testGridCacheRequestIdempotentWhilePendingOrReady(){
	DjAssistGridCache cache;
	const DjTrackIdentity identity = fingerprintIdentity(5);
	cache.request(1, 1, identity);
	const int firstPending = cache.findPending();
	assert(firstPending >= 0);
	// Re-requesting the same still-Pending key must not restart/duplicate
	// it - findPending() keeps reporting the exact same slot.
	cache.request(1, 1, identity);
	assert(cache.findPending() == firstPending);

	DjGridAnchor anchors[1] = { anchor(1, 1) };
	cache.resolve(firstPending, 1, 1, identity, true, anchors, 1);
	// Re-requesting an already-Ready key must not reset it back to Pending
	// (that would needlessly re-trigger a background read).
	cache.request(1, 1, identity);
	assert(cache.findPending() < 0);
	DjGridAnchor out[DJ_GRID_ANCHOR_CAPACITY] = {};
	uint16_t outCount = 0;
	assert(cache.lookup(1, 1, identity, out, outCount));
}

void testGridCacheAnchorCountCappedToCapacity(){
	DjAssistGridCache cache;
	const DjTrackIdentity identity = fingerprintIdentity(6);
	cache.request(1, 1, identity);
	const int pending = cache.findPending();
	assert(pending >= 0);
	DjGridAnchor anchors[DJ_GRID_ANCHOR_CAPACITY + 5] = {};
	for(uint16_t i = 0; i < DJ_GRID_ANCHOR_CAPACITY + 5; ++i) anchors[i] = anchor(i, i);
	cache.resolve(pending, 1, 1, identity, true, anchors, uint16_t(DJ_GRID_ANCHOR_CAPACITY + 5));
	DjGridAnchor out[DJ_GRID_ANCHOR_CAPACITY] = {};
	uint16_t outCount = 0;
	assert(cache.lookup(1, 1, identity, out, outCount));
	assert(outCount == DJ_GRID_ANCHOR_CAPACITY);
}

// A different metadataRevision (a same-generation metadata replacement) or
// libraryGeneration for the SAME identity must never be treated as the same
// cache entry - both must be part of the key, not just the identity.
void testGridCacheKeyIncludesGenerationAndRevision(){
	DjAssistGridCache cache;
	const DjTrackIdentity identity = fingerprintIdentity(7);
	cache.request(1, 5, identity);
	const int pending = cache.findPending();
	assert(pending >= 0);
	DjGridAnchor anchors[1] = { anchor(1, 1) };
	cache.resolve(pending, 1, 5, identity, true, anchors, 1);

	DjGridAnchor out[DJ_GRID_ANCHOR_CAPACITY] = {};
	uint16_t outCount = 0;
	assert(cache.lookup(1, 5, identity, out, outCount));
	assert(!cache.lookup(1, 6, identity, out, outCount)); // different revision.
	assert(!cache.lookup(2, 5, identity, out, outCount)); // different generation.
}

// -- round 5, fix #3 (exact-identity aliasing) --
// Two tracks sharing a fingerprint but differing only in sourceId must
// never resolve to each other's cached grid anchors: sameKey() must use
// djTrackIdentityExactMatch(), not DjAssistScoring::identityMatches()
// (which returns a match on fingerprint alone once both sides also carry a
// SOURCE flag, regardless of the sourceId bytes).
void testGridCacheDoesNotAliasSameFingerprintDifferentSource(){
	DjAssistGridCache cache;

	DjTrackIdentity x = fingerprintIdentity(9);
	x.flags |= DJ_TRACK_IDENTITY_SOURCE;
	memset(x.sourceId, 0xA1, sizeof(x.sourceId));

	DjTrackIdentity y = x; // same fingerprint, different source.
	memset(y.sourceId, 0xB2, sizeof(y.sourceId));

	cache.request(1, 1, x);
	const int pending = cache.findPending();
	assert(pending >= 0);
	DjGridAnchor anchors[1] = { anchor(1, 1) };
	cache.resolve(pending, 1, 1, x, true, anchors, 1);

	DjGridAnchor out[DJ_GRID_ANCHOR_CAPACITY] = {};
	uint16_t outCount = 0;
	assert(cache.lookup(1, 1, x, out, outCount)); // X's own request resolves.
	assert(!cache.lookup(1, 1, y, out, outCount)); // Y must never alias X's slot.
}

} // namespace

int main(){
	testBuildTrackIdentityGatesZeroEvidence();
	testBuildLibraryEntryFullCapabilities();
	testBuildLibraryEntryMissingFieldsClearBitsOnly();
	testCrossfadeMixEndpoints();
	testCrossfadeMixCheckedDivideByZero();
	testCrossfadeMixOverflowGuard();
	testNextRollbackPhaseFullSequence();
	testNextRollbackPhaseSkipsUnsubmittedSteps();
	testNextRollbackPhaseSkipsOwnershipWhenAlreadyInThatState();
	testNextRollbackPhaseSkipsMixOnManualOverride();
	testEvaluateCommandOutcomeMapping();
	testEvaluateBoundaryArrivalNotYet();
	testEvaluateBoundaryArrivalReachedWithinTolerance();
	testEvaluateBoundaryArrivalMissedBeyondTolerance();
	testPhraseCacheNeedsRescanWhenEmpty();
	testPhraseCacheDoesNotRescanBeforeFoundBoundaryPassed();
	testPhraseCacheDoesNotRescanTerminalJustBecauseStillTerminal();
	testPhraseCacheRescansOnIdentityChangeOrDeckChange();
	testPhraseCacheRescansOnBackwardSeek();
	testPhraseCacheRescansOnMetadataGenerationOrStateChange();
	testCandidateGenerationCurrentMatchesExactly();
	testGridCacheRequestFindPendingResolveLookupRoundTrip();
	testGridCacheLookupMissForNeverRequestedKey();
	testGridCacheLookupMissWhileStillPending();
	testGridCacheResolveFailedLeavesLookupMiss();
	testGridCacheResolveIgnoredAfterSlotReused();
	testGridCacheRequestIdempotentWhilePendingOrReady();
	testGridCacheAnchorCountCappedToCapacity();
	testGridCacheKeyIncludesGenerationAndRevision();
	testGridCacheDoesNotAliasSameFingerprintDifferentSource();
	return 0;
}
