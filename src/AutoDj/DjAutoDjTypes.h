#ifndef JAYD_FIRMWARE_DJ_AUTO_DJ_TYPES_H
#define JAYD_FIRMWARE_DJ_AUTO_DJ_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Dependency-free POD types shared by the Auto DJ planning layer
// (DjAutoDjQueue / DjAutoDjHistory / DjAutoDjStateMachine / DjAutoDjPlanner).
//
// This intentionally does NOT #include DjSession/DjSessionState.h or
// LibraryIndex.h: those live on the in-progress Coach branch and are still
// changing. AutoDjIdentity mirrors the shape of DjTrackIdentity (flags +
// fingerprint) plus a library generation so the two can be reconciled with a
// small adapter once the Coach branch lands, instead of duplicated forever.

static constexpr uint8_t AUTO_DJ_QUEUE_CAPACITY = 32;
static constexpr uint8_t AUTO_DJ_HISTORY_CAPACITY = 24;
static constexpr uint8_t AUTO_DJ_RETRY_BUDGET = 2; // retries allowed after the first attempt
// One full attempt-cycle's wall-clock budget, enforced solely by
// DjAutoDjPlanner::progressPending()/resolvePendingWhileStopping() around
// AutoDjLoadPort::pollLoad(), measured against AutoDjLoadPort::nowMicros()
// rather than a DjSession::loop() tick count. Originally sized (as a tick
// count) for a bare stable-ID deck load only; since the composite-workflow
// integration (RAM stable-ID resolve -> load submit -> load applied ->
// internal Auto-owned Coach arm -> poll Coach through boundary/start/sync/
// crossfade/stop/rollback -> only then Applied), pollLoad() does not report
// Applied until that entire sequence finishes, so this single budget must
// cover the worst case of all of it, not just the load. A tick count is the
// wrong unit for that: real DjSession::loop() throughput varies with audio-
// callback scheduling, so a fixed tick budget does not bound a fixed
// wall-clock duration. 150 wall-clock seconds is comfortably above even a
// slow-tempo (e.g. 70 BPM) worst-case phrase-boundary wait (~32 beats,
// ~27s) plus a 32-beat crossfade (~27s) plus load I/O.
static constexpr uint64_t AUTO_DJ_LOAD_TIMEOUT_US = 150000000ULL; // 150s
static constexpr uint8_t AUTO_DJ_DEFAULT_RECENT_EXCLUSION = 8;
static constexpr uint8_t AUTO_DJ_DEFAULT_ARTIST_EXCLUSION = 4;
static constexpr uint8_t AUTO_DJ_DEFAULT_TITLE_EXCLUSION = 6;

enum AutoDjIdentityFlag : uint8_t {
	AUTO_DJ_IDENTITY_FINGERPRINT = 1 << 0,
	AUTO_DJ_IDENTITY_SOURCE = 1 << 1
};

// A stable library identity for a single track. The planner and queue must
// only ever address tracks through this triple (generation + fingerprint) -
// never a raw filesystem path, so a queued entry can always be re-validated
// or safely dropped if the library re-indexes underneath it.
struct AutoDjIdentity {
	uint8_t flags = 0;
	uint32_t libraryGeneration = 0;
	uint8_t fingerprint[16] = {};
	// Mirrors DjTrackIdentity::sourceId exactly (same 16 bytes, same
	// AUTO_DJ_IDENTITY_SOURCE/DJ_TRACK_IDENTITY_SOURCE flag gating). Without
	// this field, a round trip through AutoDjIdentity silently zeroed
	// sourceId while still carrying the SOURCE flag - a review-flagged bug:
	// DjSession::resolveMetadata() passes trackByPath() a non-null zeroed
	// sourceId whenever the flag is set, and trackByPath() then requires an
	// exact match against the real (nonzero) on-disk sourceId, so every
	// nonzero-source track failed resolution and exhausted its retry budget.
	// Carried end-to-end: candidate table entry -> AutoDjCandidate ->
	// queue/history entry -> submitLoad()'s DjTrackIdentity -> resolveMetadata().
	uint8_t sourceId[16] = {};
	// Independent epoch from libraryGeneration: bumped on any metadata
	// content replacement even when the external library generation is
	// unchanged (e.g. a same-generation sidecar re-tag) - mirrors
	// DjSession::assistMetadataRevision()'s doc comment exactly. A queued/
	// pending candidate whose revision no longer matches live must be
	// invalidated the same way a generation change already invalidates it
	// (see DjAutoDjQueue::invalidateRevision()); 0 is a valid initial value
	// (matches an unstarted revision counter), never treated as "unknown".
	uint32_t metadataRevision = 0;

	bool valid() const{
		return flags != 0;
	}

	bool sameTrack(const AutoDjIdentity& other) const{
		if(!valid() || !other.valid()) return false;
		if(!(flags & AUTO_DJ_IDENTITY_FINGERPRINT) || !(other.flags & AUTO_DJ_IDENTITY_FINGERPRINT)){
			return false;
		}
		return memcmp(fingerprint, other.fingerprint, sizeof(fingerprint)) == 0;
	}
};

// Stable ordering used to break ties deterministically regardless of the
// order candidates happen to be presented in.
inline bool autoDjIdentityLess(const AutoDjIdentity& a, const AutoDjIdentity& b){
	return memcmp(a.fingerprint, b.fingerprint, sizeof(a.fingerprint)) < 0;
}

enum AutoDjReason : uint16_t {
	AUTO_DJ_REASON_NONE = 0,
	AUTO_DJ_REASON_FRESH = 1 << 0,               // not found in recent history
	AUTO_DJ_REASON_ARTIST_VARIETY = 1 << 1,      // artist not on cooldown
	AUTO_DJ_REASON_TITLE_VARIETY = 1 << 2,       // title not on cooldown
	AUTO_DJ_REASON_METADATA_MATCH = 1 << 3,      // reserved: Coach bpm/key/energy scoring
	AUTO_DJ_REASON_CONSERVATIVE_FALLBACK = 1 << 4, // no metadata; ordering only, no fabricated match
	AUTO_DJ_REASON_TIE_BREAK = 1 << 5             // selected via stable identity tie-break
};

// A track offered to the planner for consideration. `score` and metadata
// fields are placeholders for the final Coach scoring model; until that
// lands the planner only ever uses them when `hasMetadata` is true, and
// never invents a harmonic/beat match on missing data.
struct AutoDjCandidate {
	AutoDjIdentity identity = {};
	uint32_t artistHash = 0; // 0 == unknown, never used for exclusion
	uint32_t titleHash = 0;  // 0 == unknown, never used for exclusion
	bool hasMetadata = false;
	bool durationTrustworthy = false;
	uint32_t durationSeconds = 0;
	uint32_t bpmMilli = 0;
	uint16_t key = 0;
	uint32_t score = 0;
	uint16_t reasons = AUTO_DJ_REASON_NONE;
};

enum class AutoDjState : uint8_t {
	Off,
	Armed,
	Running,
	Paused,
	Stopping,
	Complete,
	Failed
};

enum class AutoDjFailReason : uint8_t {
	None,
	CapabilityDisabled,
	RetryBudgetExhausted,
	RecordingFailure
};

#endif //JAYD_FIRMWARE_DJ_AUTO_DJ_TYPES_H
