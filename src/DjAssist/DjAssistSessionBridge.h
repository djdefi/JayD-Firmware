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

// Rollback phases for a failed/cancelled one-shot transition, owned by the
// controller (not the pure DjAssistEngine) - see
// DjAssistController::tickRollback(). Ordered mix -> sync -> stop-deck,
// matching the review's required undo order.
enum DjAssistRollbackPhase : uint8_t {
	DJ_ASSIST_ROLLBACK_IDLE,
	DJ_ASSIST_ROLLBACK_MIX,
	DJ_ASSIST_ROLLBACK_SYNC,
	DJ_ASSIST_ROLLBACK_STOP_DECK,
	DJ_ASSIST_ROLLBACK_DONE
};

// Given the current rollback phase and which of this plan's side effects
// were actually submitted (crossfade/sync/start-deck steps), returns the
// next phase to attempt - skipping any phase with nothing to undo. Pure and
// deterministic; the caller does the actual actuator submit/poll for
// whichever phase this returns and re-normalizes after each completed
// phase.
DjAssistRollbackPhase nextRollbackPhase(
	DjAssistRollbackPhase phase,
	bool crossfadeSubmitted,
	bool syncSubmitted,
	bool startDeckSubmitted
);

// Outcome of polling a single in-flight command's status - used by both the
// crossfade actuator's final-mix confirmation and the rollback state
// machine so the same status->outcome mapping isn't duplicated.
// DJ_COMMAND_APPLIED -> DONE, REJECTED/FAILED -> FAILED, SUPERSEDED ->
// RESUBMIT (bounded retry - e.g. a rollback mix command overtaken by
// unrelated traffic), anything else (ACCEPTED, or not yet observed) ->
// WAIT.
enum DjAssistCommandOutcome : uint8_t {
	DJ_ASSIST_COMMAND_WAIT,
	DJ_ASSIST_COMMAND_RESUBMIT,
	DJ_ASSIST_COMMAND_DONE,
	DJ_ASSIST_COMMAND_FAILED
};
DjAssistCommandOutcome evaluateCommandOutcome(DjCommandStatus status);

// True when a bounded scan of the most recent command results (the
// existing DJ_RECENT_RESULT_COUNT ring, never a fresh history scan) finds a
// SET_MIX command from a non-system origin (physical/browser) with an id
// greater than `watermarkId` - i.e. submitted after the transition armed.
// A programmatic crossfade must not silently overwrite this on its next
// ramp tick; see DjAssistGuardSnapshot::manualMixOverride.
bool detectManualMixOverride(
	const DjCommandResult* recentResults,
	uint8_t resultCount,
	uint32_t watermarkId
);

} // namespace DjAssistBridge

#endif
