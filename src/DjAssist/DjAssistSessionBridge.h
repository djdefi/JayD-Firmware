#ifndef JAYD_FIRMWARE_DJASSISTSESSIONBRIDGE_H
#define JAYD_FIRMWARE_DJASSISTSESSIONBRIDGE_H

#include "DjAssistTypes.h"

// Pure, bounded, host-testable glue between DjSession's authoritative state
// and the Coach/transition engine's POD inputs. Deliberately free of
// FS/Arduino dependencies (no JaydMetadata.h, no MixSystem.h) so the exact
// arithmetic used by the real DjAssistController/DjAssistSessionActuator can
// be exercised by a host self-check, matching DjAssistScoring's bar. Callers
// on the ESP32 side pass already-resolved scalars (from Track/snapshot
// fields); this module never touches storage or hardware.
namespace DjAssistBridge {

// Builds a DjTrackIdentity from raw fingerprint/sourceId bytes as read from
// JaydMetadata::Track. Each evidence flag is gated on its array being
// non-all-zero, so an unpopulated field is treated as "no evidence" rather
// than a spurious 16-zero-byte value that could falsely match another
// unresolved identity. Used both for building candidate-table entries and
// for DjSession::publishSnapshot()'s per-deck loaded identity.
DjTrackIdentity buildTrackIdentity(const uint8_t fingerprint[16], const uint8_t sourceId[16]);

// Builds one candidate-table entry's capability bitmask from cheap,
// already-resolved integer fields (mirrors DjSession::resolveMetadata()'s
// capability formula, minus the per-grid/per-phrase downbeat-count/
// confidence enumeration - deliberately out of scope for the bulk candidate
// table; see DjAssistController for the disclosed tradeoff). Missing fields
// simply leave the corresponding bit unset (reduces scoring confidence
// rather than rejecting the track).
DjAssistLibraryEntry buildLibraryEntry(
	uint32_t libraryIndex,
	const DjTrackIdentity& identity,
	DjMetadataState state,
	uint32_t sampleRate,
	uint64_t durationFrames,
	uint32_t bpmMilli,
	uint16_t key,
	uint8_t rating,
	uint32_t cueCount,
	uint32_t gridCount,
	uint32_t phraseCount
);

// Deterministic crossfade mix value in [0,255] for a fromDeck->toDeck
// transition, `elapsedMicros` into a `crossfadeBeats`-long ramp at
// `bpmMilli`. Checked against bpmMilli==0/crossfadeBeats==0 (falls back to
// the immediate target endpoint rather than dividing by zero); saturates at
// the endpoint once elapsed time reaches/exceeds the ramp duration. Mirrors
// Mixer::setMixRatio semantics: 0 == full deck0, 255 == full deck1.
uint8_t computeCrossfadeMix(
	uint8_t toDeck,
	uint64_t elapsedMicros,
	uint8_t crossfadeBeats,
	uint32_t bpmMilli
);

} // namespace DjAssistBridge

#endif
