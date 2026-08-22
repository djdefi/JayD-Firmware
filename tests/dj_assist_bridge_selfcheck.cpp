#include <assert.h>
#include <string.h>

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
// -- exactly (minus the deliberately-out-of-scope downbeat enumeration).

void testBuildLibraryEntryFullCapabilities(){
	const DjTrackIdentity identity = fingerprintIdentity(7);
	const DjAssistLibraryEntry entry = buildLibraryEntry(
		42, identity, DJ_METADATA_VALID,
		44100, 44100ULL * 180ULL, 128000, 0x105, 4,
		/*cueCount*/ 3, /*gridCount*/ 64, /*phraseCount*/ 8
	);
	assert(entry.libraryIndex == 42);
	assert(entry.state == DJ_METADATA_VALID);
	assert(entry.bpmMilli == 128000);
	assert(entry.key == 0x105);
	assert(entry.rating == 4);
	assert(entry.durationFrames == 44100ULL * 180ULL);
	assert(entry.sampleRate == 44100);
	assert(memcmp(entry.identity.fingerprint, identity.fingerprint, 16) == 0);
	const uint16_t expected = DJ_METADATA_HAS_SOURCE_FRAMES | DJ_METADATA_HAS_BPM | DJ_METADATA_HAS_KEY |
		DJ_METADATA_HAS_RATING | DJ_METADATA_HAS_CUES | DJ_METADATA_HAS_GRID | DJ_METADATA_HAS_PHRASES;
	assert(entry.capabilities == expected);
	// Deliberately never set at the bulk-candidate-table level (see header
	// comment + DjAssistController): scoring never reads this bit.
	assert(!(entry.capabilities & DJ_METADATA_HAS_DOWNBEATS));
}

void testBuildLibraryEntryMissingFieldsClearBitsOnly(){
	// Every field absent/zero/sentinel: every bit clears, but the call still
	// returns a usable (lower-confidence, never rejected) entry - no field
	// individually forces exclusion.
	const DjAssistLibraryEntry entry = buildLibraryEntry(
		0, DjTrackIdentity{}, DJ_METADATA_STALE,
		0, 0, 0, 0, 255,
		0, 0, 0
	);
	assert(entry.capabilities == 0);
	assert(entry.state == DJ_METADATA_STALE);
	assert(entry.rating == 255);

	// Rating sentinel boundary: 255 must never set HAS_RATING; any real
	// 0-5 rating (including 0, a legitimate unrated-low value) must.
	const DjAssistLibraryEntry rated = buildLibraryEntry(
		0, DjTrackIdentity{}, DJ_METADATA_VALID,
		44100, 1000, 120000, 5, 0,
		0, 0, 0
	);
	assert(rated.capabilities & DJ_METADATA_HAS_RATING);

	// sampleRate/durationFrames must BOTH be present for HAS_SOURCE_FRAMES,
	// matching resolveMetadata()'s `track.sampleRate && track.durationFrames`.
	const DjAssistLibraryEntry halfSource = buildLibraryEntry(
		0, DjTrackIdentity{}, DJ_METADATA_VALID,
		44100, 0, 0, 0, 255,
		0, 0, 0
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

// -- nextRollbackPhase: skips phases with nothing to undo, deterministic --

void testNextRollbackPhaseFullSequence(){
	// Every side effect was submitted: mix -> sync -> stop-deck -> done,
	// never skipping a phase.
	DjAssistRollbackPhase phase = DJ_ASSIST_ROLLBACK_IDLE;
	phase = nextRollbackPhase(phase, /*crossfade*/true, /*sync*/true, /*startDeck*/true);
	assert(phase == DJ_ASSIST_ROLLBACK_MIX);
	phase = nextRollbackPhase(DJ_ASSIST_ROLLBACK_SYNC, true, true, true);
	assert(phase == DJ_ASSIST_ROLLBACK_SYNC);
	phase = nextRollbackPhase(DJ_ASSIST_ROLLBACK_STOP_DECK, true, true, true);
	assert(phase == DJ_ASSIST_ROLLBACK_STOP_DECK);
	phase = nextRollbackPhase(DJ_ASSIST_ROLLBACK_DONE, true, true, true);
	assert(phase == DJ_ASSIST_ROLLBACK_DONE);
}

void testNextRollbackPhaseSkipsUnsubmittedSteps(){
	// Nothing was ever submitted (e.g. failed during WAIT_BOUNDARY, before
	// any actuator action): every phase must be skipped straight to DONE -
	// there is nothing to undo.
	assert(nextRollbackPhase(DJ_ASSIST_ROLLBACK_IDLE, false, false, false) == DJ_ASSIST_ROLLBACK_DONE);

	// Only the start-deck step was submitted (failed right after START_DECK,
	// before LOCK_TEMPO/ENABLE_SYNC or CROSSFADE ever ran): mix and sync
	// phases must both be skipped, landing directly on STOP_DECK.
	assert(nextRollbackPhase(DJ_ASSIST_ROLLBACK_IDLE, false, false, true) == DJ_ASSIST_ROLLBACK_STOP_DECK);

	// Mix (crossfade) was submitted but sync/start-deck were not (e.g. a
	// tempoLock=false, startAtBoundary=false plan that failed mid-ramp):
	// only the MIX phase runs, then falls straight to DONE.
	assert(nextRollbackPhase(DJ_ASSIST_ROLLBACK_IDLE, true, false, false) == DJ_ASSIST_ROLLBACK_MIX);
	assert(nextRollbackPhase(DJ_ASSIST_ROLLBACK_SYNC, true, false, false) == DJ_ASSIST_ROLLBACK_DONE);
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

// -- detectManualMixOverride: bounded scan, watermark, origin/type gating -

DjCommandResult makeResult(uint32_t id, DjCommandType type, DjCommandOrigin origin, DjCommandStatus status){
	DjCommandResult result;
	result.id = id;
	result.type = type;
	result.origin = origin;
	result.status = status;
	return result;
}

void testDetectManualMixOverrideFindsPostWatermarkNonSystemMix(){
	DjCommandResult results[8] = {
		makeResult(1, DJ_COMMAND_SET_MIX, DJ_ORIGIN_SYSTEM, DJ_COMMAND_APPLIED), // before watermark
		makeResult(9, DJ_COMMAND_SET_MIX, DJ_ORIGIN_HTTP, DJ_COMMAND_APPLIED),   // after watermark, manual
	};
	assert(detectManualMixOverride(results, 2, /*watermarkId*/5));
}

void testDetectManualMixOverrideIgnoresSystemOrigin(){
	// A system-origin SET_MIX after the watermark is the transition's own
	// crossfade ramp, not a manual override.
	DjCommandResult results[1] = {
		makeResult(10, DJ_COMMAND_SET_MIX, DJ_ORIGIN_SYSTEM, DJ_COMMAND_ACCEPTED),
	};
	assert(!detectManualMixOverride(results, 1, 5));
}

void testDetectManualMixOverrideIgnoresPreWatermarkAndOtherTypes(){
	DjCommandResult results[3] = {
		makeResult(3, DJ_COMMAND_SET_MIX, DJ_ORIGIN_HTTP, DJ_COMMAND_APPLIED), // before watermark
		makeResult(11, DJ_COMMAND_SET_PLAYING, DJ_ORIGIN_HTTP, DJ_COMMAND_APPLIED), // wrong type
		makeResult(0, DJ_COMMAND_SET_MIX, DJ_ORIGIN_HTTP, DJ_COMMAND_APPLIED), // id 0 == empty slot
	};
	assert(!detectManualMixOverride(results, 3, 5));
}

void testDetectManualMixOverrideIgnoresRejected(){
	// A rejected manual mix command never actually took effect - it must
	// not itself count as an override.
	DjCommandResult results[1] = {
		makeResult(9, DJ_COMMAND_SET_MIX, DJ_ORIGIN_PHYSICAL, DJ_COMMAND_REJECTED),
	};
	assert(!detectManualMixOverride(results, 1, 5));
}

void testDetectManualMixOverrideHandlesNullAndEmpty(){
	assert(!detectManualMixOverride(nullptr, 0, 0));
	DjCommandResult empty[8] = {};
	assert(!detectManualMixOverride(empty, 8, 0));
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
	testEvaluateCommandOutcomeMapping();
	testDetectManualMixOverrideFindsPostWatermarkNonSystemMix();
	testDetectManualMixOverrideIgnoresSystemOrigin();
	testDetectManualMixOverrideIgnoresPreWatermarkAndOtherTypes();
	testDetectManualMixOverrideIgnoresRejected();
	testDetectManualMixOverrideHandlesNullAndEmpty();
	return 0;
}
