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

// Auto DJ's own Coach-arm defaults: matches MixScreen.cpp's physical Assist
// bank call site exactly (its own default crossfadeBeats/startAtBoundary/
// tempoLock), so an automated transition looks/behaves identically to a
// manually-armed one - Auto DJ reuses Coach as the sole transition engine,
// not a second implementation of the same choice.
static const uint8_t AUTO_DJ_TRANSITION_CROSSFADE_BEATS = 16;
static const bool AUTO_DJ_TRANSITION_START_AT_BOUNDARY = true;
static const bool AUTO_DJ_TRANSITION_TEMPO_LOCK = true;

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
//
// submitLoad()/pollLoad() implement a composite, multi-command workflow
// behind the planner's single one-shot "submit then poll until Applied/
// Failed" contract: RAM stable-ID load -> (once applied) internal
// Auto-owned Coach arm -> poll Coach's own transition through boundary/
// start/sync/crossfade/stop/rollback -> only then Applied. The planner
// (frozen/approved core) is never touched or made aware of this - from its
// perspective this is still exactly one pending attempt with Accepted/
// Pending/Applied/Failed outcomes; see AutoDjLoadSubPhase below for how
// those map onto the underlying load/arm/transition commands. This is
// deliberate: Auto DJ never implements its own crossfade/mix logic, it
// only ever drives the already-approved Coach engine, which is what
// inherits Coach's existing manual-override/media-loss/recording-conflict
// handling for free (see manualTakeoverActive() and pollTransitionPhase()
// below).
enum class AutoDjLoadSubPhase : uint8_t {
	Idle,
	LoadInFlight,       // stable-ID deck load submitted, not yet applied.
	ArmInFlight,         // load applied; Coach arm submitted, not yet applied.
	TransitionInFlight,  // arm applied; polling Coach's own transition mode.
	// Coach's transition failed/was cancelled and is now unwinding its own
	// rollback (see DjAssistController::tickRollback()). Waits for
	// AutoDjSessionPort::autoDjCoachTransitionSettled() before reporting
	// the terminal Failed outcome - see pollTeardownPhase()'s doc comment
	// for exactly why this must not resolve early.
	Teardown
};

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
	// Hard, immediate abandon (unlike stop(), which deliberately waits for
	// an in-flight attempt to resolve via Stopping - see
	// DjAutoDjPlanner::stop()'s doc comment): if Auto's own Coach-armed
	// transition may still be live (ArmInFlight/TransitionInFlight),
	// proactively cancel it before the planner forgets about the attempt
	// entirely, rather than leaving it running unsupervised. Never
	// required for correctness (Coach's own guard independently detects
	// and fails/rolls back on a genuine manual takeover regardless of
	// whether anything here still asks it to), but reset() is an explicit
	// deliberate abandon and should not leave a transition it no longer
	// intends to observe running.
	//
	// Two cases, and only two mutate anything:
	//  1. planner.reset() succeeds (state is Failed/Complete - the plain,
	//     documented reset path): an explicit terminal acknowledgment, so
	//     any straggling Coach transition is safely cancelled too and
	//     loadSubPhase clears to Idle.
	//  2. planner.reset() is rejected (state isn't Failed/Complete yet)
	//     AND a Coach arm/transition is still physically live: this is the
	//     hard-abandon case. Cancel it, but route loadSubPhase through the
	//     SAME bounded Teardown/settled-wait path a genuine Coach-side
	//     transition failure uses (see pollTransitionPhase()'s Teardown
	//     branch) instead of forcing Idle immediately. Forcing Idle here
	//     let the very next tick()'s progressPending() see an ordinary
	//     load failure and retry right away - regardless of whether the
	//     cancel's own rollback had actually finished - racing a fresh
	//     load against stale rollback commands on the same deck. Routing
	//     through Teardown reuses submitLoad()'s existing
	//     autoDjCoachTransitionSettled() hard-reject and bounded teardown
	//     deadline instead of a second, unreviewed safety mechanism.
	// Any other rejected reset (no live transition to abandon: Idle,
	// LoadInFlight, or already Teardown/Stopping) is a true no-op - zero
	// effect on Coach or loadSubPhase.
	bool reset(){
		if(planner.reset()){
			if(loadSubPhase == AutoDjLoadSubPhase::ArmInFlight || loadSubPhase == AutoDjLoadSubPhase::TransitionInFlight){
				sessionPort.autoDjCancelCoachTransition();
			}
			loadSubPhase = AutoDjLoadSubPhase::Idle;
			return true;
		}
		if(loadSubPhase == AutoDjLoadSubPhase::ArmInFlight || loadSubPhase == AutoDjLoadSubPhase::TransitionInFlight){
			sessionPort.autoDjCancelCoachTransition();
			loadSubPhase = AutoDjLoadSubPhase::Teardown;
			teardownDeadlineUs = nowMicros() + AUTO_DJ_TEARDOWN_TIMEOUT_US;
		}
		return false;
	}
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
	// library-generation and metadata-revision baselines, advances at most
	// one bounded scan chunk toward topping up the queue, then ticks the
	// planner exactly once.
	void tick(){
		refreshLibraryGeneration();
		refreshMetadataRevision();
		sessionPort.copySnapshot(cachedSnapshot);
		stepScan();
		planner.tick();
	}

	// -- AutoDjLoadPort --
	bool hasStableIdEndpoint() const override{
		return true; // the stable-ID load path is unconditionally compiled in.
	}

	bool submitLoad(const AutoDjIdentity& identity) override{
		// Hard, unconditional reject while Coach's own rollback (mix/sync/
		// stop-deck restore after a failed/cancelled transition) is still
		// unsettled - independent of loadSubPhase, so this guard holds even
		// if a future caller reaches submitLoad() through a path other than
		// the planner's own Teardown/Settling handling (see
		// AutoDjLoadOutcome::Settling/FailedTerminal doc comments). A fresh
		// load must never be able to race an in-flight rollback's own
		// commands against the same deck.
		if(!sessionPort.autoDjCoachTransitionSettled()) return false;
		DjTrackIdentity trackIdentity;
		trackIdentity.flags = identity.flags & (DJ_TRACK_IDENTITY_FINGERPRINT | DJ_TRACK_IDENTITY_SOURCE);
		memcpy(trackIdentity.fingerprint, identity.fingerprint, sizeof(trackIdentity.fingerprint));
		// sourceId must ride along with the SOURCE flag - see AutoDjIdentity::
		// sourceId's doc comment (DjAutoDjTypes.h) for the exact failure this
		// fixes (a zeroed sourceId here previously survived unnoticed because
		// the flag alone made resolveMetadata() treat it as "present").
		memcpy(trackIdentity.sourceId, identity.sourceId, sizeof(trackIdentity.sourceId));
		const uint8_t deck = sessionPort.autoDjTargetDeck();
		// Thread the exact epoch this candidate was captured/selected
		// under straight through to the session, rather than letting the
		// session re-read live values (which would make the whole "reject
		// a since-superseded candidate" contract tautological) - see
		// AutoDjIdentity::metadataRevision's doc comment.
		const DjSubmitResult result = sessionPort.autoDjLoadDeckByIdentity(
			deck, trackIdentity, identity.libraryGeneration, identity.metadataRevision
		);
		if(result.status != DJ_COMMAND_ACCEPTED){
			loadSubPhase = AutoDjLoadSubPhase::Idle;
			return false;
		}
		sessionPort.autoDjTrackCommand(result.id);
		trackedCommandId = result.id;
		inFlightToDeck = deck;
		inFlightIdentity = trackIdentity;
		loadSubPhase = AutoDjLoadSubPhase::LoadInFlight;
		return true;
	}

	AutoDjLoadOutcome pollLoad() override{
		switch(loadSubPhase){
			case AutoDjLoadSubPhase::LoadInFlight: return pollLoadPhase();
			case AutoDjLoadSubPhase::ArmInFlight: return pollArmPhase();
			case AutoDjLoadSubPhase::TransitionInFlight: return pollTransitionPhase();
			case AutoDjLoadSubPhase::Teardown: return pollTeardownPhase();
			case AutoDjLoadSubPhase::Idle: default: return AutoDjLoadOutcome::Failed;
		}
	}

	// A manual takeover pauses/cancels Auto DJ deterministically (see
	// DjAutoDjPlanner::tick()'s manualTakeoverActive() branch, which drops
	// the pending attempt and pauses on the very next detection). If Auto's
	// own Coach-armed transition may still be physically live at that
	// moment (ArmInFlight/TransitionInFlight), proactively cancel it here
	// rather than only relying on Coach's own, separately-keyed guard to
	// eventually notice - closing the gap deterministically instead of by
	// coincidence of the two generation-tracking mechanisms happening to
	// agree on every command type. Idempotent either way: cancelling an
	// already-finished/-failed transition is a harmless no-op.
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
		if(changed && (loadSubPhase == AutoDjLoadSubPhase::ArmInFlight ||
				loadSubPhase == AutoDjLoadSubPhase::TransitionInFlight)){
			sessionPort.autoDjCancelCoachTransition();
			loadSubPhase = AutoDjLoadSubPhase::Idle;
		}
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

	// See AutoDjLoadPort::nowMicros()'s doc comment: this port stays
	// Arduino-free (see AutoDjSessionPort::autoDjNowMicros()'s doc
	// comment), so every target that includes this header - including the
	// isolated-core/no-Arduino test doubles - keeps compiling without a
	// real or stub Arduino.h, while DjSession's real implementation still
	// backs this with an actual micros() read.
	uint64_t nowMicros() const override{
		return sessionPort.autoDjNowMicros();
	}

private:
	// pollLoad()'s three composite sub-phases (see AutoDjLoadSubPhase's
	// class-comment). Each mirrors the same
	// submit-then-track/ACCEPTED-PENDING-APPLIED-else-Failed shape the
	// original single-phase load poll used, just against a different
	// tracked command / status source per phase.

	AutoDjLoadOutcome pollLoadPhase(){
		switch(sessionPort.autoDjCommandStatus(trackedCommandId)){
			case DJ_COMMAND_ACCEPTED:
				return AutoDjLoadOutcome::Accepted;
			case DJ_COMMAND_PENDING:
				return AutoDjLoadOutcome::Pending;
			case DJ_COMMAND_APPLIED:
				return beginArm();
			default: // FAILED / REJECTED / SUPERSEDED
				loadSubPhase = AutoDjLoadSubPhase::Idle;
				return AutoDjLoadOutcome::Failed;
		}
	}

	// The load just applied: inFlightToDeck is now loaded with
	// inFlightIdentity, stopped and sync-off by construction (a fresh
	// stable-ID load never starts/syncs the deck), which is exactly what
	// DjAssistEngine::armTransition()'s own guard requires of toDeck - no
	// separate "confirm identity" step is needed here, Coach's own guard
	// (re-validated live at arm time) is the confirmation. fromDeck is the
	// other deck (DJ_DECK_COUNT == 2), expected to still be playing
	// whatever Auto DJ is currently crossfading away from.
	AutoDjLoadOutcome beginArm(){
		const uint8_t fromDeck = uint8_t((DJ_DECK_COUNT - 1) - inFlightToDeck);
		const DjSubmitResult result = sessionPort.autoDjArmCoachTransition(
			fromDeck, inFlightToDeck, inFlightIdentity,
			AUTO_DJ_TRANSITION_CROSSFADE_BEATS, AUTO_DJ_TRANSITION_START_AT_BOUNDARY, AUTO_DJ_TRANSITION_TEMPO_LOCK
		);
		if(result.status != DJ_COMMAND_ACCEPTED){
			loadSubPhase = AutoDjLoadSubPhase::Idle;
			return AutoDjLoadOutcome::Failed;
		}
		sessionPort.autoDjTrackCommand(result.id);
		trackedCommandId = result.id;
		loadSubPhase = AutoDjLoadSubPhase::ArmInFlight;
		return AutoDjLoadOutcome::Accepted; // one attempt still in flight, not terminal yet.
	}

	AutoDjLoadOutcome pollArmPhase(){
		switch(sessionPort.autoDjCommandStatus(trackedCommandId)){
			case DJ_COMMAND_ACCEPTED:
				return AutoDjLoadOutcome::Accepted;
			case DJ_COMMAND_PENDING:
				return AutoDjLoadOutcome::Pending;
			case DJ_COMMAND_APPLIED:
				loadSubPhase = AutoDjLoadSubPhase::TransitionInFlight;
				return AutoDjLoadOutcome::Accepted; // armed; still not terminal.
			default: // FAILED / REJECTED / SUPERSEDED (e.g. purged by a takeover)
				loadSubPhase = AutoDjLoadSubPhase::Idle;
				return AutoDjLoadOutcome::Failed;
		}
	}

	// No separate crossfade logic lives here: this only ever reads Coach's
	// own mode, which Coach's own tick() (called unconditionally every
	// DjSession::loop(), independent of Auto DJ) advances through
	// boundary-wait/start/sync/crossfade/stop/rollback on its own. Coach
	// failing, rolling back, or being overridden/cancelled all surface
	// identically here (uniformly Failed) and defer entirely to the
	// planner's existing bounded retry/terminal-skip policy - see
	// DjAutoDjPlanner::handleLoadFailure().
	AutoDjLoadOutcome pollTransitionPhase(){
		switch(sessionPort.autoDjCoachTransitionMode()){
			case DJ_ASSIST_MODE_TRANSITION_ARMED:
			case DJ_ASSIST_MODE_TRANSITION_RUNNING:
				return AutoDjLoadOutcome::Pending;
			case DJ_ASSIST_MODE_TRANSITION_COMPLETE:
				loadSubPhase = AutoDjLoadSubPhase::Idle;
				return AutoDjLoadOutcome::Applied;
			default: // DJ_ASSIST_MODE_OFF / DJ_ASSIST_MODE_COACH / DJ_ASSIST_MODE_TRANSITION_FAILED
				loadSubPhase = AutoDjLoadSubPhase::Teardown;
				teardownDeadlineUs = nowMicros() + AUTO_DJ_TEARDOWN_TIMEOUT_US;
				return pollTeardownPhase();
		}
	}

	// Coach reported the transition as no longer running (failed, guard
	// cancel, media/recording conflict, etc.) and is now unwinding its own
	// rollback (mix -> sync -> stop-deck restore - see
	// DjAssistController::tickRollback()). This must keep reporting
	// Settling - never Pending, and never Failed - until
	// AutoDjSessionPort::autoDjCoachTransitionSettled() confirms rollback
	// has actually finished. Settling (rather than Pending) is deliberate:
	// it tells the planner not to apply the outer composite-attempt
	// deadline here (see AutoDjLoadOutcome::Settling's doc comment), since
	// that deadline may already be nearly or fully consumed by the
	// transition that just failed. Instead this phase owns its own bounded
	// teardownDeadlineUs (set when entering Teardown, above): if rollback
	// still has not settled by then, this resolves to the terminal
	// FailedTerminal outcome rather than ever letting a retry submit a
	// fresh load while the old rollback's commands may still be in flight.
	AutoDjLoadOutcome pollTeardownPhase(){
		if(sessionPort.autoDjCoachTransitionSettled()){
			loadSubPhase = AutoDjLoadSubPhase::Idle;
			return AutoDjLoadOutcome::Failed;
		}
		if(djAutoDjDeadlinePassed(nowMicros(), teardownDeadlineUs)){
			// Deliberately stays in Teardown (not reset to Idle): the
			// rollback may genuinely still be in flight on real hardware
			// even though this bounded wait gave up on it, and
			// submitLoad()'s own autoDjCoachTransitionSettled() guard above
			// keeps rejecting any future load attempt regardless of
			// loadSubPhase until it actually settles - reset() is the only
			// sanctioned way back to Idle from here (see its doc comment).
			return AutoDjLoadOutcome::FailedTerminal;
		}
		return AutoDjLoadOutcome::Settling;
	}

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

	// Sibling of refreshLibraryGeneration() above, keyed on the independent
	// metadataRevision epoch (see AutoDjIdentity::metadataRevision's doc
	// comment): a same-generation metadata replacement bumps this without
	// bumping libraryGeneration, and must still drop every queued/pending
	// candidate captured under the old revision.
	void refreshMetadataRevision(){
		const uint32_t revision = sessionPort.autoDjMetadataRevision();
		if(revision == lastKnownRevision) return;
		lastKnownRevision = revision;
		planner.invalidateMetadataRevision(revision);
		resetScan(); // any in-progress scan pass predates the new revision.
	}

	void resetScan(){
		scanInProgress = false;
		scanCursor = 0;
		scanTopNCount = 0;
	}

	static AutoDjIdentity toAutoDjIdentity(const DjTrackIdentity& identity, uint32_t generation, uint32_t revision){
		AutoDjIdentity out;
		out.flags = identity.flags & (AUTO_DJ_IDENTITY_FINGERPRINT | AUTO_DJ_IDENTITY_SOURCE);
		out.libraryGeneration = generation;
		out.metadataRevision = revision;
		memcpy(out.fingerprint, identity.fingerprint, sizeof(out.fingerprint));
		memcpy(out.sourceId, identity.sourceId, sizeof(out.sourceId));
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
			// Both autoDjMetadataRevision() (live atomic) and
			// autoDjCandidateEntry()'s outRevision (the candidate table's
			// own fill-pass revision, DjAssistController::loadedGeneration_)
			// are keyed to the SAME metadataRevision epoch - the fill
			// worker deliberately re-fills whenever assistMetadataRevision()
			// changes (see DjAssistController::fillWorkerStep()'s doc
			// comment), so this remains a valid same-epoch comparison even
			// though the per-entry read below no longer touches
			// metadataReader directly (see autoDjCandidateEntry()'s doc
			// comment). Every AutoDjIdentity produced by this scan pass
			// also captures this same revision (see toAutoDjIdentity()
			// below), so a queued/pending candidate can later be
			// invalidated against a fresher live revision - see
			// AutoDjIdentity::metadataRevision's doc comment.
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

			const AutoDjIdentity identity = toAutoDjIdentity(entry.identity, lastKnownGeneration, revision);
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
	uint32_t lastKnownRevision = 0;

	// mutable: manualTakeoverActive() is const (see AutoDjLoadPort) but
	// must be able to proactively abandon/cancel an in-flight composite
	// attempt the instant a takeover is detected - see that method's doc
	// comment.
	mutable AutoDjLoadSubPhase loadSubPhase = AutoDjLoadSubPhase::Idle;
	uint32_t trackedCommandId = 0; // single tracked slot, reused sequentially for load then arm.
	uint8_t inFlightToDeck = 0;
	DjTrackIdentity inFlightIdentity = {};
	// Valid only while loadSubPhase == Teardown - see pollTeardownPhase()'s
	// doc comment for why this is a separate, independently-bounded
	// deadline rather than reusing the planner's own composite-attempt
	// budget.
	uint64_t teardownDeadlineUs = 0;

	bool scanInProgress = false;
	uint32_t scanCursor = 0;
	uint32_t scanRevisionAtStart = 0;
	AutoDjCandidate scanTopN[AUTO_DJ_SCAN_TOPN_CAPACITY] = {};
	uint8_t scanTopNCount = 0;
};

#endif
