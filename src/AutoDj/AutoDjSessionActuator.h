#ifndef JAYD_FIRMWARE_AUTODJSESSIONACTUATOR_H
#define JAYD_FIRMWARE_AUTODJSESSIONACTUATOR_H

#include "AutoDjSessionPort.h"
#include "DjAutoDjPlanner.h"
#include "../DjAssist/DjAssistScoring.h"

// Bounded scan budget per tick(): pure CPU comparisons over already-cached/
// already-indexed metadata (autoDjCandidateEntry() never triggers a fresh
// file read), so a generous per-tick budget is still cheap. Mirrors
// DJ_ASSIST_DEFAULT_SCAN_BUDGET's reasoning (DjAssistTypes.h).
static const uint32_t AUTO_DJ_SCAN_BUDGET_PER_TICK = 256;
// Bounded top-N accumulator size for one scan pass - small on purpose:
// planNext() only ever needs to compare a handful of leading candidates,
// not the whole library, and this stays a fixed stack array (no heap).
static const uint8_t AUTO_DJ_SCAN_TOPN_CAPACITY = 8;
// Stop topping up once this many entries are already queued (pinned or
// planned) - one full track of lead time is enough; Auto DJ still owns up
// to AUTO_DJ_QUEUE_CAPACITY (32) slots for user pins.
static const uint8_t AUTO_DJ_QUEUE_TOPUP_WATERMARK = 2;
// Conservative lead time before the crossfade point: currentTrackAtEnd()
// reports true this many seconds before the trustworthy duration elapses,
// leaving enough margin for the stable-ID load (and Coach's own arm/
// crossfade plan) to complete before playback actually runs out.
static const uint32_t AUTO_DJ_END_MARGIN_SECONDS = 10;

// Bounded, POD snapshot of Auto DJ's current state - safe to copy into a
// browser/API payload or physical-bank UI cache, mirroring DjAssistSnapshot's
// role for Coach.
struct AutoDjSnapshot {
	AutoDjState state = AutoDjState::Off;
	AutoDjFailReason failReason = AutoDjFailReason::None;
	uint8_t queueDepth = 0;
	uint8_t historySize = 0;
};

// Concrete AutoDjLoadPort implementation adapting AutoDjSessionPort (i.e.
// DjSession) to the isolated DjAutoDjPlanner. Owns the planner itself (see
// DjAutoDjPlanner's AutoDjLoadPort& constructor - `*this` is passed in this
// class's own constructor init list) so DjSession's integration surface is
// a single `tick()` plus thin arm/start/pause/resume/stop/reset/pin
// wrappers, mirroring DjAssistController's role for Coach.
//
// Candidate scoring reuses DjAssistScoring::scoreEntry() verbatim (the
// exact same deterministic tempo/key/rating math and DjAssistExcludeReason
// gating Coach's own suggestion list uses) rather than reimplementing it -
// this class only adds the bounded incremental scan loop and the
// DjAssistSuggestion -> AutoDjCandidate field mapping the planner needs.
// mergeSuggestion()/scanTick() (DjAssistScoring.h) are not reused directly:
// both are shaped around DjAssistSuggestion's narrower field set and a
// fully-materialized entries[] array, neither of which fits this class's
// one-entry-at-a-time, DjAssistLibraryEntry-plus-artist/title-hash read
// path - see mergeCandidate()/stepScan() below for the bounded equivalent.
class AutoDjSessionActuator : public AutoDjLoadPort {
public:
	explicit AutoDjSessionActuator(AutoDjSessionPort& sessionPort) : sessionPort(sessionPort), planner(*this){}

	AutoDjState state() const{ return planner.state(); }
	AutoDjFailReason failReason() const{ return planner.failReason(); }

	bool arm(){ return planner.arm(); }
	bool start(){ return planner.start(); }
	bool pause(){ return planner.pause(); }
	bool resume(){ return planner.resume(); }
	bool stop(){ return planner.stop(); }
	bool reset(){ return planner.reset(); }
	bool pinTrack(const AutoDjIdentity& identity, uint32_t artistHash, uint32_t titleHash){
		return planner.pinTrack(identity, artistHash, titleHash);
	}

	void copySnapshot(AutoDjSnapshot& snapshot) const{
		snapshot.state = planner.state();
		snapshot.failReason = planner.failReason();
		snapshot.queueDepth = planner.queueDepth();
		snapshot.historySize = planner.historySize();
	}

	// One-shot runtime step, safe to call on any fixed cadence (mirrors
	// DjAutoDjPlanner::tick()'s own one-shot contract): refreshes the
	// library-generation baseline, advances at most one bounded scan
	// chunk toward topping up the queue, then ticks the planner exactly
	// once.
	void tick(){
		refreshLibraryGeneration();
		sessionPort.copySnapshot(cachedSnapshot);
		stepScan();
		planner.tick();
	}

	// -- AutoDjLoadPort --
	bool hasStableIdEndpoint() const override{
		return true; // the stable-ID load path is unconditionally compiled in.
	}

	bool submitLoad(const AutoDjIdentity& identity) override{
		DjTrackIdentity trackIdentity;
		trackIdentity.flags = identity.flags & (DJ_TRACK_IDENTITY_FINGERPRINT | DJ_TRACK_IDENTITY_SOURCE);
		memcpy(trackIdentity.fingerprint, identity.fingerprint, sizeof(trackIdentity.fingerprint));
		const uint8_t deck = sessionPort.autoDjTargetDeck();
		const DjSubmitResult result = sessionPort.autoDjLoadDeckByIdentity(deck, trackIdentity);
		if(result.status != DJ_COMMAND_ACCEPTED){
			hasInFlightLoad = false;
			return false;
		}
		sessionPort.autoDjTrackLoadCommand(result.id);
		lastLoadCommandId = result.id;
		hasInFlightLoad = true;
		return true;
	}

	AutoDjLoadOutcome pollLoad() override{
		if(!hasInFlightLoad) return AutoDjLoadOutcome::Failed;
		switch(sessionPort.autoDjLoadCommandStatus(lastLoadCommandId)){
			case DJ_COMMAND_ACCEPTED:
				return AutoDjLoadOutcome::Accepted;
			case DJ_COMMAND_PENDING:
				return AutoDjLoadOutcome::Pending;
			case DJ_COMMAND_APPLIED:
				hasInFlightLoad = false;
				return AutoDjLoadOutcome::Applied;
			default: // FAILED / REJECTED / SUPERSEDED
				hasInFlightLoad = false;
				return AutoDjLoadOutcome::Failed;
		}
	}

	bool manualTakeoverActive() const override{
		const AutoDjManualIntentGenerations current = sessionPort.autoDjManualIntentGenerationsSnapshot();
		bool changed = current.mix != lastObserved.mix;
		for(uint8_t deck = 0; deck < DJ_DECK_COUNT; deck++){
			if(current.deck[deck] != lastObserved.deck[deck]) changed = true;
		}
		lastObserved = current; // continuously-rolling baseline, not arm-time: any
		// tick's takeover pauses immediately, so comparing against "what we
		// last observed" (rather than a stale arm-time snapshot spanning many
		// ticks) is both sufficient and simpler - see class comment.
		return changed;
	}

	bool currentDurationTrustworthy() const override{
		uint8_t deck = 0;
		return activeDeck(deck) && cachedSnapshot.decks[deck].timingQuality == DJ_TIMING_COARSE &&
			cachedSnapshot.decks[deck].duration > 0;
	}

	bool currentTrackAtEnd() const override{
		uint8_t deck = 0;
		if(!activeDeck(deck)) return false;
		const DjDeckSnapshot& active = cachedSnapshot.decks[deck];
		if(active.timingQuality != DJ_TIMING_COARSE || active.duration == 0) return false;
		const uint32_t remaining = active.duration > active.elapsed ? uint32_t(active.duration - active.elapsed) : 0;
		return remaining <= AUTO_DJ_END_MARGIN_SECONDS;
	}

	bool recordingFailed() const override{
		return cachedSnapshot.recordingInfo.state == DJ_RECORDING_FAILED;
	}

	bool physicalConfirmationPresent() const override{
		return sessionPort.autoDjConsumePhysicalConfirmation();
	}

private:
	// The deck currently playing is "active" (the one about to run out);
	// the other is the load target. If neither deck is playing there is
	// no trustworthy reference yet - callers must treat that as "not
	// active" rather than guessing deck 0.
	bool activeDeck(uint8_t& outDeck) const{
		for(uint8_t deck = 0; deck < DJ_DECK_COUNT; deck++){
			if(cachedSnapshot.decks[deck].playing){
				outDeck = deck;
				return true;
			}
		}
		return false;
	}

	void refreshLibraryGeneration(){
		const uint32_t generation = sessionPort.autoDjLibraryGeneration();
		if(generation == lastKnownGeneration) return;
		lastKnownGeneration = generation;
		planner.invalidateLibraryGeneration(generation);
		resetScan(); // any in-progress scan pass predates the new generation.
	}

	void resetScan(){
		scanInProgress = false;
		scanCursor = 0;
		scanTopNCount = 0;
	}

	static AutoDjIdentity toAutoDjIdentity(const DjTrackIdentity& identity, uint32_t generation){
		AutoDjIdentity out;
		out.flags = identity.flags & (AUTO_DJ_IDENTITY_FINGERPRINT | AUTO_DJ_IDENTITY_SOURCE);
		out.libraryGeneration = generation;
		memcpy(out.fingerprint, identity.fingerprint, sizeof(out.fingerprint));
		return out;
	}

	// Bounded (capacity AUTO_DJ_SCAN_TOPN_CAPACITY) descending-score,
	// ascending-identity-tie-break insert - the AutoDjCandidate analogue of
	// DjAssistScoring::mergeSuggestion(), sized for this class's candidate
	// shape instead of DjAssistSuggestion's.
	void mergeCandidate(const AutoDjCandidate& candidate){
		uint8_t insertAt = scanTopNCount;
		for(uint8_t i = 0; i < scanTopNCount; i++){
			const bool worse = candidate.score > scanTopN[i].score ||
				(candidate.score == scanTopN[i].score && autoDjIdentityLess(candidate.identity, scanTopN[i].identity));
			if(worse){
				insertAt = i;
				break;
			}
		}
		if(insertAt >= AUTO_DJ_SCAN_TOPN_CAPACITY) return; // strictly worse than every kept entry - drop.
		const uint8_t last = scanTopNCount < AUTO_DJ_SCAN_TOPN_CAPACITY ? scanTopNCount : AUTO_DJ_SCAN_TOPN_CAPACITY - 1;
		for(uint8_t i = last; i > insertAt; i--) scanTopN[i] = scanTopN[i - 1];
		scanTopN[insertAt] = candidate;
		if(scanTopNCount < AUTO_DJ_SCAN_TOPN_CAPACITY) scanTopNCount++;
	}

	// Advances at most AUTO_DJ_SCAN_BUDGET_PER_TICK entries of one bounded
	// scan pass toward a fresh planNext() call. No-ops (and drops any
	// in-progress pass) whenever the queue already has enough lead time,
	// there is nothing to scan, or there is no trustworthy active-deck
	// context to score against yet.
	void stepScan(){
		if(planner.queueDepth() >= AUTO_DJ_QUEUE_TOPUP_WATERMARK){
			resetScan();
			return;
		}
		const uint32_t total = sessionPort.autoDjCandidateCount();
		if(total == 0){
			resetScan();
			return;
		}
		const DjAssistDeckContext deckContext = sessionPort.autoDjActiveDeckContext();
		if(!deckContext.valid){
			resetScan();
			return;
		}
		if(!scanInProgress){
			scanInProgress = true;
			scanCursor = 0;
			scanTopNCount = 0;
			scanRevisionAtStart = sessionPort.autoDjMetadataRevision();
		}

		uint32_t processed = 0;
		while(processed < AUTO_DJ_SCAN_BUDGET_PER_TICK && processed < total){
			const uint32_t index = scanCursor;
			scanCursor = (scanCursor + 1) % total;
			processed++;

			DjAssistLibraryEntry entry;
			uint32_t artistHash = 0;
			uint32_t titleHash = 0;
			uint32_t revision = 0;
			const bool ok = sessionPort.autoDjCandidateEntry(index, entry, artistHash, titleHash, revision);
			if(revision != scanRevisionAtStart){
				// The reader swapped mid-pass: never mix two different
				// reader states into one top-N result - abort cleanly and
				// restart fresh next tick instead.
				resetScan();
				return;
			}
			if(!ok){
				if(scanCursor == 0){ finishScan(); return; }
				continue;
			}

			const AutoDjIdentity identity = toAutoDjIdentity(entry.identity, lastKnownGeneration);
			const bool alreadyQueued = planner.isQueued(identity);
			const DjAssistSuggestion suggestion =
				DjAssistScoring::scoreEntry(entry, deckContext, false /* isLoaded */, alreadyQueued /* isRecent */);
			if(suggestion.excludeReason == DJ_ASSIST_EXCLUDE_NONE){
				AutoDjCandidate candidate;
				candidate.identity = identity;
				candidate.artistHash = artistHash;
				candidate.titleHash = titleHash;
				candidate.hasMetadata = entry.state == DJ_METADATA_VALID;
				candidate.durationTrustworthy = entry.sampleRate != 0 && entry.durationFrames != 0;
				candidate.durationSeconds = candidate.durationTrustworthy ?
					uint32_t(entry.durationFrames / entry.sampleRate) : 0;
				candidate.bpmMilli = entry.bpmMilli;
				candidate.key = entry.key;
				candidate.score = suggestion.score;
				candidate.reasons = candidate.hasMetadata ?
					AUTO_DJ_REASON_METADATA_MATCH : AUTO_DJ_REASON_CONSERVATIVE_FALLBACK;
				mergeCandidate(candidate);
			}

			if(scanCursor == 0){ finishScan(); return; } // full pass complete.
		}
	}

	void finishScan(){
		if(scanTopNCount > 0) planner.planNext(scanTopN, scanTopNCount);
		resetScan();
	}

	AutoDjSessionPort& sessionPort;
	DjAutoDjPlanner planner;

	DjSnapshot cachedSnapshot = {};
	mutable AutoDjManualIntentGenerations lastObserved;
	uint32_t lastKnownGeneration = 0;

	bool hasInFlightLoad = false;
	uint32_t lastLoadCommandId = 0;

	bool scanInProgress = false;
	uint32_t scanCursor = 0;
	uint32_t scanRevisionAtStart = 0;
	AutoDjCandidate scanTopN[AUTO_DJ_SCAN_TOPN_CAPACITY] = {};
	uint8_t scanTopNCount = 0;
};

#endif
