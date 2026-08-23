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

	// Bounded candidate-table data source, backed by the exact same
	// already-indexed metadata reader assistTrackEntry()/resolveMetadata()
	// use - never a fresh file read. outRevision reports the exact
	// metadataRevision this read was performed under, captured under the
	// same lock acquisition as the entry read itself (mirrors
	// assistTrackEntry()'s doc comment), so a caller can detect a reader
	// swap mid-scan rather than mixing two different reader states.
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
	// submit()-then-track two-step convention exactly.
	virtual DjSubmitResult autoDjLoadDeckByIdentity(uint8_t deck, const DjTrackIdentity& identity) = 0;
	virtual void autoDjTrackLoadCommand(uint32_t commandId) = 0;
	virtual DjCommandStatus autoDjLoadCommandStatus(uint32_t commandId) = 0;

	virtual bool copySnapshot(DjSnapshot& snapshot) = 0;

	// Baseline/diff pair for manual-takeover detection - see
	// AutoDjManualIntentGenerations (DjSessionState.h).
	virtual AutoDjManualIntentGenerations autoDjManualIntentGenerationsSnapshot() = 0;

	// True only immediately after a physical hold-confirm gesture (or an
	// authenticated browser confirm); consumed on read (one-shot), so a
	// stale confirmation can never be replayed by polling again.
	virtual bool autoDjConsumePhysicalConfirmation() = 0;
};

#endif
