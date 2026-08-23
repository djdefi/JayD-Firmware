#ifndef JAYD_FIRMWARE_AUTODJSESSIONPORT_H
#define JAYD_FIRMWARE_AUTODJSESSIONPORT_H

#include "../DjAssist/DjAssistTypes.h"

// Arduino-free seam between AutoDjSessionActuator and DjSession, mirroring
// DjAssistSessionPort's pattern exactly: every method here matches an
// existing DjSession method of the same name/signature/constness, and
// DjSession implements this interface directly (see DjSession.h) - the port
// only narrows what the actuator is allowed to see, it does not change what
// DjSession does.
//
// Deliberately a SEPARATE interface from DjAssistSessionPort (not reused or
// extended): Auto DJ's candidate-table shape needs artist/title hashes for
// cooldown exclusion that DjAssistLibraryEntry doesn't carry, and its
// manual-takeover surface (transport/crossfader/rate/load/cue/loop) is
// materially broader than Coach's mix/play/sync-only guard. Folding both
// into one port would make every method read as "this one is only for X".
// DjAssistTypes.h is included (not duplicated) because reusing
// DjAssistLibraryEntry/DjAssistDeckContext/DjAssistScoring::scoreEntry() for
// the actual scoring math is the whole point of this integration - see
// AutoDjSessionActuator.h.
class AutoDjSessionPort {
public:
	virtual ~AutoDjSessionPort() {}

	// Bounded candidate-table data source. RAM-only: delegates to
	// DjAssistController's background-filled candidate table (the exact
	// same table Coach's own suggestion scan reads), never the metadata
	// reader/SD card directly - see DjSession::autoDjCandidateEntry().
	// outRevision reports the exact fill generation this read was
	// performed under, captured under the same lock acquisition as the
	// entry read itself, so a caller can detect a reader swap mid-scan
	// rather than mixing two different reader states.
	virtual uint32_t autoDjCandidateCount() = 0;
	virtual bool autoDjCandidateEntry(
		uint32_t index,
		DjAssistLibraryEntry& outEntry,
		uint32_t& outArtistHash,
		uint32_t& outTitleHash,
		uint32_t& outRevision
	) = 0;
	virtual uint32_t autoDjMetadataRevision() = 0;
	// Current library generation, for detecting a reindex and invalidating
	// the queue/scan (see DjAutoDjPlanner::invalidateLibraryGeneration()
	// and AutoDjSessionActuator::refreshLibraryGeneration()).
	virtual uint32_t autoDjLibraryGeneration() = 0;

	// Deck context (bpm/key/remaining frames) of whichever deck is
	// currently playing, for scoring candidates via
	// DjAssistScoring::scoreEntry() - .valid is false if no deck is
	// currently playing (nothing to score against yet).
	virtual DjAssistDeckContext autoDjActiveDeckContext() = 0;
	// The deck Auto DJ should treat as the load target: the one NOT
	// currently playing. If neither deck is playing this still returns a
	// deterministic choice (deck 1) but callers must gate on
	// autoDjActiveDeckContext().valid first - loading into a target deck
	// without a trustworthy playing reference is never safe to act on.
	virtual uint8_t autoDjTargetDeck() = 0;

	// Non-blocking stable-ID deck load (see DjSession::loadDeckByIdentity()).
	// Always submitted with DJ_ORIGIN_SYSTEM so it can never itself count
	// as a manual takeover. Mirrors DjAssistController's
	// submit()-then-track two-step convention exactly. libraryGeneration/
	// metadataRevision are the exact epoch values the candidate was scored/
	// selected under (see AutoDjIdentity), captured explicitly by the
	// caller - never re-read live inside this call - so
	// resolveIdentityPath() can reject a load whose captured epoch has
	// since been superseded instead of silently resolving against a
	// different revision's metadata.
	virtual DjSubmitResult autoDjLoadDeckByIdentity(
		uint8_t deck, const DjTrackIdentity& identity,
		uint32_t libraryGeneration, uint32_t metadataRevision
	) = 0;
	virtual void autoDjTrackCommand(uint32_t commandId) = 0;
	virtual DjCommandStatus autoDjCommandStatus(uint32_t commandId) = 0;

	// Composite-workflow step 2: arm the already-approved Coach transition
	// engine as the sole mechanism that actually advances play/mix/stop -
	// Auto DJ never runs its own crossfade logic. Submitted with
	// DJ_ORIGIN_SYSTEM and tagged autoDjOwned so a manual takeover purges a
	// still-queued arm exactly like it purges a still-queued load; once
	// applied, Coach's own guard (identity/deck-state re-validated at arm
	// time) is the confirmation that the loaded target is correct, and
	// Coach's own manual-override/media/recording-conflict handling is
	// inherited unmodified - see AutoDjSessionActuator's composite
	// pollLoad(). targetIdentity/toDeck must be the exact identity/deck
	// Auto just loaded; fromDeck is the other, currently-playing deck.
	virtual DjSubmitResult autoDjArmCoachTransition(
		uint8_t fromDeck, uint8_t toDeck, const DjTrackIdentity& targetIdentity,
		uint8_t crossfadeBeats, bool startAtBoundary, bool tempoLock
	) = 0;
	// Best-effort teardown of an Auto-owned Coach arm/transition (e.g. a
	// hard reset()); never required for correctness since Coach's own guard
	// independently detects and fails/rolls back on genuine manual
	// takeover, but avoids leaving a transition Auto no longer wants
	// running unsupervised after an explicit abandon.
	virtual DjSubmitResult autoDjCancelCoachTransition() = 0;
	// Coach's current mode, polled once per tick while a transition Auto
	// armed is in flight. Always succeeds (no fallible snapshot path).
	virtual DjAssistMode autoDjCoachTransitionMode() = 0;

	virtual bool copySnapshot(DjSnapshot& snapshot) = 0;

	// Baseline/diff pair for manual-takeover detection - see
	// AutoDjManualIntentGenerations (DjSessionState.h).
	virtual AutoDjManualIntentGenerations autoDjManualIntentGenerationsSnapshot() = 0;

	// True only immediately after a physical hold-confirm gesture (or an
	// authenticated browser confirm); consumed on read (one-shot), so a
	// stale confirmation can never be replayed by polling again.
	virtual bool autoDjConsumePhysicalConfirmation() = 0;

	// True once Coach has fully settled after a FAILED/cancelled transition
	// - i.e. either Coach was never in TRANSITION_FAILED mode at all, or its
	// own rollback (mix/sync/stop-deck restore - see DjAssistController::
	// tickRollback()) has finished. False while rollback is still actively
	// undoing a failed transition's mutations. AutoDjSessionActuator's
	// composite pollLoad() must wait for this before reporting a terminal
	// Failed outcome to the planner: reporting Failed (and thus allowing a
	// retry/new arm) while rollback is still in flight would let a fresh
	// Coach arm reset the controller's rollback-tracking state out from
	// under the still-in-flight rollback commands, orphaning them so they
	// can go on to mutate the NEW transition's target deck.
	virtual bool autoDjCoachTransitionSettled() = 0;

	// True only while the live stable-ID candidate/load authority behind
	// Auto DJ is actually functional right now - see DjAssistController::
	// authorityReady()'s doc comment for exactly what this covers
	// (candidate-table allocation + fill-worker launch/liveness). This is
	// the dynamic signal AutoDjLoadPort::hasStableIdEndpoint() is backed
	// by: unlike a static "the code path is compiled in" answer, this
	// degrades to false the moment the authority that would actually
	// resolve/serve a stable-ID load stops being real (allocation
	// failure, fill-worker launch failure, or the worker having since
	// exited), so capability-gated admission (arm()/start()) and the
	// ongoing tick() loop both see the same live truth rather than
	// advertising a capability that can never produce anything.
	virtual bool autoDjStableIdAuthorityReady() = 0;

	// Monotonic wall-clock read (microseconds) - see AutoDjLoadPort::
	// nowMicros()'s own doc comment for why this replaced a loop-tick
	// count. DjSession implements this with a real micros() read;
	// AutoDjSessionActuator forwards its own nowMicros() override here so
	// this port (like DjAssistSessionPort) stays entirely Arduino-free and
	// host-testable without pulling a real or stub Arduino.h into every
	// target that includes AutoDjSessionActuator.h.
	virtual uint64_t autoDjNowMicros() const = 0;
};

#endif
