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

} // namespace

int main(){
	testBuildLibraryEntryFullCapabilities();
	testBuildLibraryEntryMissingFieldsClearBitsOnly();
	testCrossfadeMixEndpoints();
	testCrossfadeMixCheckedDivideByZero();
	testCrossfadeMixOverflowGuard();
	return 0;
}
