#ifndef JAYD_FIRMWARE_DJASSISTTYPES_H
#define JAYD_FIRMWARE_DJASSISTTYPES_H

#include <stddef.h>
#include <stdint.h>
#include "../DjSession/DjSessionState.h"

// Coach/suggestions/one-shot-transition layer. Deliberately hardware/Arduino
// free (mirrors DjSessionState.h) so it stays host-testable and decoupled
// from the in-flight Sync/quantize/loop work landing separately. Integration
// with DjSession, the physical Assist bank, and the browser/API surfaces is
// intentionally deferred to a follow-up commit once those branches land -
// see DjAssistEngine.h's DjAssistActuator for the seam that binds this state
// machine to real Sync/quantize primitives.

static const uint8_t DJ_ASSIST_MAX_SUGGESTIONS = 5;
// Mirrors JaydMetadata::Reader::MaxTracks. Duplicated (not included) to keep
// this header free of Arduino/FS dependencies.
static const uint16_t DJ_ASSIST_MAX_INDEX_ENTRIES = 4096;
static const uint8_t DJ_ASSIST_MAX_RECENT_TRACKS = 16;
static const uint16_t DJ_ASSIST_DEFAULT_SCAN_BUDGET = 256;
static const uint8_t DJ_ASSIST_MAX_TRANSITION_STEPS = 8;

// Fixed-point rate, 1000 == 1.0x deck rate.
static const uint32_t DJ_ASSIST_RATE_UNITY_MILLI = 1000;
static const uint32_t DJ_ASSIST_RATE_MIN_MILLI = 500;
static const uint32_t DJ_ASSIST_RATE_MAX_MILLI = 1500;
static const uint32_t DJ_ASSIST_RATE_NARROW_MIN_MILLI = 920;
static const uint32_t DJ_ASSIST_RATE_NARROW_MAX_MILLI = 1080;

enum DjAssistMode : uint8_t {
	DJ_ASSIST_MODE_OFF,
	DJ_ASSIST_MODE_COACH,
	DJ_ASSIST_MODE_TRANSITION_ARMED,
	DJ_ASSIST_MODE_TRANSITION_RUNNING,
	DJ_ASSIST_MODE_TRANSITION_COMPLETE,
	DJ_ASSIST_MODE_TRANSITION_FAILED
};

enum DjAssistKeyRelationship : uint8_t {
	DJ_ASSIST_KEY_UNKNOWN,
	DJ_ASSIST_KEY_INCOMPATIBLE,
	DJ_ASSIST_KEY_RELATIVE,
	DJ_ASSIST_KEY_ADJACENT,
	DJ_ASSIST_KEY_SAME
};

enum DjAssistReasonFlag : uint16_t {
	DJ_ASSIST_REASON_TEMPO_NARROW       = 1 << 0,
	DJ_ASSIST_REASON_TEMPO_IN_RANGE     = 1 << 1,
	DJ_ASSIST_REASON_TEMPO_OUT_OF_RANGE = 1 << 2,
	DJ_ASSIST_REASON_KEY_SAME           = 1 << 3,
	DJ_ASSIST_REASON_KEY_ADJACENT       = 1 << 4,
	DJ_ASSIST_REASON_KEY_RELATIVE       = 1 << 5,
	DJ_ASSIST_REASON_KEY_UNKNOWN        = 1 << 6,
	DJ_ASSIST_REASON_GRID_AVAILABLE     = 1 << 7,
	DJ_ASSIST_REASON_PHRASE_AVAILABLE   = 1 << 8,
	DJ_ASSIST_REASON_RATING_KNOWN       = 1 << 9,
	DJ_ASSIST_REASON_DURATION_SHORT     = 1 << 10,
	DJ_ASSIST_REASON_LOW_CONFIDENCE     = 1 << 11
};

enum DjAssistExcludeReason : uint8_t {
	DJ_ASSIST_EXCLUDE_NONE,
	DJ_ASSIST_EXCLUDE_LOADED,
	DJ_ASSIST_EXCLUDE_RECENT,
	DJ_ASSIST_EXCLUDE_UNSUPPORTED_METADATA
};

enum DjAssistWarningFlag : uint16_t {
	DJ_ASSIST_WARN_OUT_OF_RANGE     = 1 << 0,
	DJ_ASSIST_WARN_NO_GRID          = 1 << 1,
	DJ_ASSIST_WARN_ENDING_SOON      = 1 << 2,
	DJ_ASSIST_WARN_RECORDING_ACTIVE = 1 << 3,
	DJ_ASSIST_WARN_LOOP_ACTIVE      = 1 << 4,
	DJ_ASSIST_WARN_NO_METADATA      = 1 << 5
};

// Bounded, POD description of one already-indexed library track. Populated
// from validated index/metadata only - never from a fresh file read.
struct DjAssistLibraryEntry {
	uint32_t libraryIndex = 0;
	DjTrackIdentity identity = {};
	DjMetadataState state = DJ_METADATA_ABSENT;
	uint16_t capabilities = 0;
	uint32_t bpmMilli = 0;
	uint16_t key = 0; // Camelot-equivalent: low byte 1-12, bit 0x100 = minor/"A". 0 = unknown.
	uint8_t rating = 255; // 0-5, 255 = unknown.
	uint64_t durationFrames = 0;
	uint32_t sampleRate = 0;
};

// Compact, bounded, POD suggestion - safe to copy into a snapshot/API/browser
// payload as-is.
struct DjAssistSuggestion {
	uint32_t libraryIndex = 0;
	DjTrackIdentity identity = {};
	int32_t tempoDeltaMilli = 0;
	uint32_t requiredRateMilli = DJ_ASSIST_RATE_UNITY_MILLI;
	DjAssistKeyRelationship keyRelationship = DJ_ASSIST_KEY_UNKNOWN;
	uint8_t rating = 255;
	uint16_t confidence = 0;
	uint16_t reasonFlags = 0;
	DjAssistExcludeReason excludeReason = DJ_ASSIST_EXCLUDE_NONE;
	uint16_t score = 0;
};

struct DjAssistDeckContext {
	bool valid = false;
	uint32_t bpmMilli = 0;
	uint16_t key = 0;
	uint64_t remainingFrames = 0;
	uint32_t sampleRate = 0;
};

// Caller-resolved boundary hint. The assist layer never reads Grid/Phrase
// records itself; the caller resolves these once (e.g. from DjSession's
// already-bounded copyGrid/copyPhrase) and passes the result in.
struct DjAssistBoundaryHint {
	bool hasDownbeat = false;
	uint64_t downbeatFrame = 0;
	bool hasPhrase = false;
	uint64_t phraseFrame = 0;
};

struct DjAssistCoachAdvice {
	bool valid = false;
	uint8_t suggestedDeck = 0;
	bool boundaryIsPhrase = false;
	uint64_t boundaryFrame = 0;
	uint32_t targetRateMilli = DJ_ASSIST_RATE_UNITY_MILLI;
	int8_t crossfaderDirection = 0; // -1 toward deck 0, +1 toward deck 1.
	uint16_t warningFlags = 0;
};

enum DjAssistTransitionAction : uint8_t {
	DJ_ASSIST_ACTION_WAIT_BOUNDARY,
	DJ_ASSIST_ACTION_START_DECK,
	DJ_ASSIST_ACTION_LOCK_TEMPO,
	DJ_ASSIST_ACTION_ENABLE_SYNC,
	DJ_ASSIST_ACTION_CROSSFADE,
	DJ_ASSIST_ACTION_STOP_DECK,
	DJ_ASSIST_ACTION_RELEASE_SYNC
};

struct DjAssistTransitionStep {
	DjAssistTransitionAction action = DJ_ASSIST_ACTION_WAIT_BOUNDARY;
	uint8_t deck = 0;
	uint32_t param = 0;
	uint32_t commandId = 0;
	bool submitted = false;
	bool applied = false;
};

enum DjAssistTransitionFailure : uint8_t {
	DJ_ASSIST_FAIL_NONE,
	DJ_ASSIST_FAIL_METADATA_LOST,
	DJ_ASSIST_FAIL_COMMAND_REJECTED,
	DJ_ASSIST_FAIL_MEDIA_REMOVED,
	DJ_ASSIST_FAIL_END_OF_TRACK,
	DJ_ASSIST_FAIL_MANUAL_OVERRIDE,
	DJ_ASSIST_FAIL_CONFLICT,
	DJ_ASSIST_FAIL_TARGET_NOT_LOADED,
	DJ_ASSIST_FAIL_CANCELLED
};

struct DjAssistTransitionPlan {
	uint8_t fromDeck = 0;
	uint8_t toDeck = 0;
	uint32_t libraryIndex = 0;
	DjTrackIdentity targetIdentity = {};
	uint8_t crossfadeBeats = 16;
	bool startAtBoundary = true;
	bool tempoLock = true;
	uint8_t stepCount = 0;
	DjAssistTransitionStep steps[DJ_ASSIST_MAX_TRANSITION_STEPS] = {};
	uint8_t currentStep = 0;
	DjAssistTransitionFailure failure = DJ_ASSIST_FAIL_NONE;

	// State captured at arm() time, used to detect a manual override of the
	// in-progress transition (crossfader/play/rate changed by the user).
	uint8_t armedMix = 127;
	bool armedFromPlaying = false;
	uint32_t armedFromRateMilli = DJ_ASSIST_RATE_UNITY_MILLI;
};

// Bounded snapshot supplied every tick so the state machine can detect
// manual overrides, conflicts, and media loss without performing its own
// file/audio IO.
struct DjAssistGuardSnapshot {
	bool recording = false;
	bool mediaPresent = true;
	uint8_t mix = 127;
	bool deckLoaded[DJ_DECK_COUNT] = {};
	bool deckPlaying[DJ_DECK_COUNT] = {};
	bool loopActive[DJ_DECK_COUNT] = {};
	bool metadataValid[DJ_DECK_COUNT] = {};
	uint32_t rateMilli[DJ_DECK_COUNT] = {};
};

#endif
