#include "DjAssistController.h"

#include "DjAssistScoring.h"
#include "DjAssistSessionBridge.h"
#include "../DjSession/DjSession.h"

#include <Arduino.h>

namespace {

// Drives the semantic actions DjAssistEngine's transition state machine
// needs from the real Sync/quantize/mix primitives. START_DECK/STOP_DECK
// map directly to setPlaying(); LOCK_TEMPO and ENABLE_SYNC are two separate
// engine steps but both resolve to the same idempotent setSync(deck, true,
// ...) call (re-arming an already-armed deck is a harmless no-op in
// DjSession::apply()); RELEASE_SYNC maps to setSync(deck, false, ...);
// SET_MIX (rollback-only) is a single exact-value setMix() tracked like any
// other real DjCommand. CROSSFADE has no single DjCommand up front - it's a
// continuous ramp driven by repeated, supersedable DJ_COMMAND_SET_MIX
// submissions each poll (intermediate values are fire-and-forget; their
// supersession by the next ramp tick is normal). Completion is NOT inferred
// from the computed ramp value alone: once DjAssistBridge::computeCrossfadeMix()
// reaches the target endpoint, the actuator submits that exact value as the
// one *tracked* final command and only reports APPLIED once DjSession
// confirms it (REJECTED/FAILED -> failed; SUPERSEDED -> resubmit once more,
// e.g. if unrelated traffic raced it out of the queue).
class DjAssistSessionActuator : public DjAssistActuator {
public:
	explicit DjAssistSessionActuator(DjSession* session) : session_(session){
	}

	bool submit(const DjAssistTransitionStep& step, uint32_t& outCommandId) override{
		switch(step.action){
			case DJ_ASSIST_ACTION_START_DECK:
				return acceptedResult(session_->setPlaying(step.deck, true, DJ_ORIGIN_SYSTEM), outCommandId);
			case DJ_ASSIST_ACTION_LOCK_TEMPO:
			case DJ_ASSIST_ACTION_ENABLE_SYNC: {
				const int8_t otherDeck = step.deck == 0 ? 1 : 0;
				return acceptedResult(
					session_->setSync(step.deck, true, otherDeck, DJ_ORIGIN_SYSTEM), outCommandId
				);
			}
			case DJ_ASSIST_ACTION_CROSSFADE:
				return beginCrossfade(step, outCommandId);
			case DJ_ASSIST_ACTION_STOP_DECK:
				return acceptedResult(session_->setPlaying(step.deck, false, DJ_ORIGIN_SYSTEM), outCommandId);
			case DJ_ASSIST_ACTION_RELEASE_SYNC:
				return acceptedResult(session_->setSync(step.deck, false, -1, DJ_ORIGIN_SYSTEM), outCommandId);
			case DJ_ASSIST_ACTION_SET_MIX: {
				const uint8_t mixValue = step.param > 255 ? 255 : uint8_t(step.param);
				return acceptedResult(session_->setMix(mixValue, DJ_ORIGIN_SYSTEM), outCommandId);
			}
			case DJ_ASSIST_ACTION_WAIT_BOUNDARY:
				break; // engine never submits this action to the actuator.
		}
		return false;
	}

	DjCommandStatus poll(uint32_t commandId) const override{
		if(commandId >= CrossfadeIdBase) return pollCrossfade(commandId);
		return lookupStatus(commandId);
	}

private:
	static const uint32_t CrossfadeIdBase = 0x80000000UL;

	DjSession* session_;
	uint32_t crossfadeId_ = CrossfadeIdBase;
	uint8_t crossfadeToDeck_ = 0;
	uint8_t crossfadeBeats_ = 16;
	uint64_t crossfadeStartMicros_ = 0;
	// True once the ramp's exact endpoint value has been submitted as a
	// single tracked command (crossfadeFinalCommandId_); before that,
	// intermediate ramp ticks are fire-and-forget.
	mutable bool crossfadeFinalSubmitted_ = false;
	mutable uint32_t crossfadeFinalCommandId_ = 0;

	static bool acceptedResult(const DjSubmitResult& result, uint32_t& outCommandId){
		if(result.status == DJ_COMMAND_REJECTED) return false;
		outCommandId = result.id;
		return true;
	}

	DjCommandStatus lookupStatus(uint32_t commandId) const{
		DjSnapshot snapshot;
		session_->copySnapshot(snapshot);
		for(uint8_t i = 0; i < DJ_RECENT_RESULT_COUNT; ++i){
			if(snapshot.recentResults[i].id == commandId) return snapshot.recentResults[i].status;
		}
		// Not seen yet (queued but not yet applied, or evicted from the
		// bounded result ring by unrelated traffic before we observed it) -
		// DjAssistEngine::tick() treats anything other than
		// APPLIED/FAILED/REJECTED as "keep waiting", so this is safe; an
		// explicit cancelTransition() remains the recovery path if a step
		// never resolves.
		return DJ_COMMAND_PENDING;
	}

	bool beginCrossfade(const DjAssistTransitionStep& step, uint32_t& outCommandId){
		if(crossfadeId_ == 0xFFFFFFFFUL) crossfadeId_ = CrossfadeIdBase; // guard wrap (never reached in practice)
		++crossfadeId_;
		crossfadeToDeck_ = step.deck;
		crossfadeBeats_ = (step.param > 0 && step.param <= 255) ? uint8_t(step.param) : 16;
		crossfadeStartMicros_ = micros();
		crossfadeFinalSubmitted_ = false;
		crossfadeFinalCommandId_ = 0;
		outCommandId = crossfadeId_;
		return true;
	}

	DjCommandStatus pollCrossfade(uint32_t commandId) const{
		if(commandId != crossfadeId_) return DJ_COMMAND_FAILED;

		if(crossfadeFinalSubmitted_){
			const DjCommandStatus status = lookupStatus(crossfadeFinalCommandId_);
			switch(DjAssistBridge::evaluateCommandOutcome(status)){
				case DjAssistBridge::DJ_ASSIST_COMMAND_DONE:
					return DJ_COMMAND_APPLIED;
				case DjAssistBridge::DJ_ASSIST_COMMAND_FAILED:
					return DJ_COMMAND_FAILED;
				case DjAssistBridge::DJ_ASSIST_COMMAND_RESUBMIT:
					// Overtaken by unrelated mix traffic before we saw it
					// applied - fall through and resubmit the exact
					// endpoint value once more below.
					crossfadeFinalSubmitted_ = false;
					break;
				case DjAssistBridge::DJ_ASSIST_COMMAND_WAIT:
				default:
					return DJ_COMMAND_PENDING;
			}
		}

		DjSnapshot snapshot;
		session_->copySnapshot(snapshot);
		const uint32_t bpmMilli = snapshot.decks[crossfadeToDeck_].metadata.bpmMilli;
		const uint64_t elapsedMicros = micros() - crossfadeStartMicros_;
		const uint8_t mixValue = DjAssistBridge::computeCrossfadeMix(
			crossfadeToDeck_, elapsedMicros, crossfadeBeats_, bpmMilli
		);
		const uint8_t targetEndpoint = crossfadeToDeck_ == 0 ? 0 : 255;

		if(mixValue == targetEndpoint){
			const DjSubmitResult result = session_->setMix(mixValue, DJ_ORIGIN_SYSTEM);
			if(result.status != DJ_COMMAND_REJECTED){
				crossfadeFinalSubmitted_ = true;
				crossfadeFinalCommandId_ = result.id;
			}
			// Rejected: retry next tick without marking submitted.
			return DJ_COMMAND_PENDING;
		}

		// Intermediate ramp value: fire-and-forget, superseded freely.
		session_->setMix(mixValue, DJ_ORIGIN_SYSTEM);
		return DJ_COMMAND_PENDING;
	}
};

} // namespace

DjAssistController::DjAssistController(){
}

DjAssistController::~DjAssistController(){
	end();
}

void DjAssistController::begin(DjSession* session){
	session_ = session;
	entryCapacity_ = DJ_ASSIST_MAX_INDEX_ENTRIES;
	entries_ = static_cast<DjAssistLibraryEntry*>(ps_malloc(sizeof(DjAssistLibraryEntry) * entryCapacity_));
	if(!entries_){
		allocationFailed_ = true;
		entryCapacity_ = 0;
	}
	actuator_ = new DjAssistSessionActuator(session_);
}

void DjAssistController::end(){
	delete actuator_;
	actuator_ = nullptr;
	free(entries_);
	entries_ = nullptr;
	session_ = nullptr;
}

void DjAssistController::refreshCandidateTable(){
	if(!session_ || allocationFailed_) return;

	const uint32_t currentGeneration = session_->assistLibraryGeneration();
	if(!generationSeen_ || currentGeneration != loadedGeneration_){
		generationSeen_ = true;
		loadedGeneration_ = currentGeneration;
		fillCursor_ = 0;
		entryTotal_ = 0;
		scanCursor_ = 0;
		suggestionCount_ = 0;
		fillComplete_ = false;
	}
	if(fillComplete_) return;

	const uint32_t rawTotal = session_->assistTrackCount();
	const uint16_t cappedTotal = rawTotal > entryCapacity_ ? entryCapacity_ : uint16_t(rawTotal);
	if(cappedTotal == 0) return; // reader not ready yet, or an empty library.

	// DJ_ASSIST_FILL_RECORDS_PER_TICK is deliberately tiny (not
	// DJ_ASSIST_DEFAULT_SCAN_BUDGET): each iteration is a real, possibly
	// SD-backed metadata read (assistTrackEntry() ->
	// JaydMetadata::trackByIndex()), unlike the scoring scan below which
	// only touches the already-cached PSRAM entries_[] table. Populating
	// up to DJ_ASSIST_MAX_INDEX_ENTRIES this way is spread across many
	// DjSession::loop() ticks instead of a synchronous multi-record burst
	// while decks are playing.
	uint16_t filled = 0;
	while(fillCursor_ < cappedTotal && filled < DJ_ASSIST_FILL_RECORDS_PER_TICK){
		DjAssistLibraryEntry entry;
		if(!session_->assistTrackEntry(fillCursor_, entry)){
			// Missing/unreadable record: still occupies a slot (so indices
			// stay stable) but carries no capabilities/state, which the
			// scorer treats as reduced confidence rather than a rejection.
			entry = DjAssistLibraryEntry();
			entry.libraryIndex = fillCursor_;
		}
		entries_[fillCursor_] = entry;
		++fillCursor_;
		++filled;
	}
	if(fillCursor_ >= cappedTotal){
		entryTotal_ = cappedTotal;
		fillComplete_ = true;
	}
}

void DjAssistController::updateRecentTracks(const DjSnapshot& snapshot){
	for(uint8_t d = 0; d < DJ_DECK_COUNT; ++d){
		const bool loadedNow = snapshot.decks[d].loaded && snapshot.decks[d].metadata.state == DJ_METADATA_VALID;
		if(loadedNow){
			const DjTrackIdentity& identity = snapshot.decks[d].identity;
			const bool changed = !lastDeckLoaded_[d] ||
				!DjAssistScoring::identityMatches(lastDeckIdentity_[d], identity);
			if(changed){
				if(lastDeckLoaded_[d]){
					// The track that was previously loaded on this deck just
					// got replaced; remember it so a fresh scan doesn't
					// immediately re-suggest what was just played.
					recentTracks_[recentNext_] = lastDeckIdentity_[d];
					recentNext_ = uint8_t((recentNext_ + 1) % DJ_ASSIST_MAX_RECENT_TRACKS);
					if(recentCount_ < DJ_ASSIST_MAX_RECENT_TRACKS) ++recentCount_;
				}
				lastDeckIdentity_[d] = identity;
			}
		}
		lastDeckLoaded_[d] = loadedNow;
	}
}

DjAssistDeckContext DjAssistController::buildDeckContext(const DjSnapshot& snapshot, uint8_t deck) const{
	DjAssistDeckContext ctx;
	if(deck >= DJ_DECK_COUNT) return ctx;
	const DjDeckSnapshot& d = snapshot.decks[deck];
	if(!d.loaded || d.metadata.state != DJ_METADATA_VALID) return ctx;

	ctx.valid = true;
	ctx.bpmMilli = d.metadata.bpmMilli;
	ctx.key = d.metadata.key;
	ctx.sampleRate = d.metadata.sourceSampleRate;
	// Coarse (whole-second) remaining-time estimate from the already-
	// published elapsed/duration fields - deliberately not frame-accurate
	// (no per-tick frame-level snapshot field exists), which is sufficient
	// for a bounded "duration/remaining-time fit" scoring signal.
	const uint64_t elapsedFrames = ctx.sampleRate ? uint64_t(d.elapsed) * ctx.sampleRate : 0;
	const uint64_t totalFrames = d.metadata.sourceDurationFrames;
	ctx.remainingFrames = elapsedFrames < totalFrames ? totalFrames - elapsedFrames : 0;
	return ctx;
}

DjAssistGuardSnapshot DjAssistController::buildGuard(const DjSnapshot& snapshot) const{
	DjAssistGuardSnapshot guard;
	guard.recording = snapshot.recordingInfo.state == DJ_RECORDING_STARTING ||
					  snapshot.recordingInfo.state == DJ_RECORDING_ACTIVE ||
					  snapshot.recordingInfo.state == DJ_RECORDING_STOPPING;
	guard.mediaPresent = session_ ? session_->mediaPresent() : true;
	guard.mix = snapshot.mix;
	for(uint8_t d = 0; d < DJ_DECK_COUNT; ++d){
		guard.deckLoaded[d] = snapshot.decks[d].loaded;
		guard.deckPlaying[d] = snapshot.decks[d].playing;
		guard.loopActive[d] = snapshot.decks[d].loop.state != DJ_LOOP_INACTIVE;
		guard.metadataValid[d] = snapshot.decks[d].metadata.state == DJ_METADATA_VALID;
		guard.rateMilli[d] = uint32_t((uint64_t(snapshot.decks[d].sync.targetRate) * 1000ULL) / DJ_RATE_SCALE);
		guard.deckIdentity[d] = snapshot.decks[d].identity;
	}
	// Only meaningful once a transition has armed and set armWatermarkId_
	// (see armTransition()); before that it stays 0, so this scan simply
	// finds nothing relevant to a not-yet-existing plan.
	guard.manualMixOverride = DjAssistBridge::detectManualMixOverride(
		snapshot.recentResults, DJ_RECENT_RESULT_COUNT, armWatermarkId_
	);
	return guard;
}

bool DjAssistController::resolveBoundary(const DjSnapshot& snapshot, uint8_t deck, DjAssistBoundaryHint& hint){
	hint = DjAssistBoundaryHint();
	if(!session_ || deck >= DJ_DECK_COUNT || !snapshot.decks[deck].loaded) return false;

	const uint64_t currentFrame = session_->deckElapsedFrames(deck);

	uint64_t downbeatFrame = 0;
	if(session_->nextDownbeatFrame(deck, currentFrame, downbeatFrame)){
		hint.hasDownbeat = true;
		hint.downbeatFrame = downbeatFrame;
	}

	// Phrase lookups do a bounded but real SD read; only refresh the cached
	// value once we've passed it (or don't have one yet for this deck), not
	// on every tick, keeping this out of the per-tick file-read budget.
	if(phraseHintDeck_ != deck || !phraseHintValid_ || currentFrame >= phraseHintFrame_){
		uint64_t phraseFrame = 0;
		phraseHintValid_ = session_->nextPhraseFrame(deck, currentFrame, phraseFrame);
		phraseHintDeck_ = deck;
		phraseHintFrame_ = phraseFrame;
	}
	if(phraseHintValid_){
		hint.hasPhrase = true;
		hint.phraseFrame = phraseHintFrame_;
	}
	return hint.hasDownbeat || hint.hasPhrase;
}

void DjAssistController::tickSuggestions(const DjSnapshot& snapshot){
	const DjAssistMode mode = engine_.mode();
	if(mode == DJ_ASSIST_MODE_OFF){
		suggestionCount_ = 0;
		lastAdvice_ = DjAssistCoachAdvice();
		return;
	}

	uint8_t referenceDeck = 0xFF;
	for(uint8_t d = 0; d < DJ_DECK_COUNT; ++d){
		if(snapshot.decks[d].playing && snapshot.decks[d].metadata.state == DJ_METADATA_VALID){
			referenceDeck = d;
			break;
		}
	}
	if(referenceDeck == 0xFF){
		suggestionCount_ = 0;
		lastAdvice_ = DjAssistCoachAdvice();
		return;
	}

	const DjAssistDeckContext deckCtx = buildDeckContext(snapshot, referenceDeck);

	DjTrackIdentity loaded[DJ_DECK_COUNT];
	uint8_t loadedCount = 0;
	for(uint8_t d = 0; d < DJ_DECK_COUNT; ++d){
		if(snapshot.decks[d].loaded && snapshot.decks[d].metadata.state == DJ_METADATA_VALID){
			loaded[loadedCount++] = snapshot.decks[d].identity;
		}
	}

	DjAssistScoring::scanTick(
		entries_, entryTotal_, scanCursor_, DJ_ASSIST_DEFAULT_SCAN_BUDGET,
		deckCtx, loaded, loadedCount, recentTracks_, recentCount_,
		suggestions_, suggestionCount_, DJ_ASSIST_MAX_SUGGESTIONS
	);

	if(mode == DJ_ASSIST_MODE_COACH){
		const uint8_t otherDeck = referenceDeck == 0 ? 1 : 0;
		const DjAssistDeckContext candidateCtx = buildDeckContext(snapshot, otherDeck);
		DjAssistBoundaryHint hint;
		resolveBoundary(snapshot, referenceDeck, hint);
		const DjAssistGuardSnapshot guard = buildGuard(snapshot);
		lastAdvice_ = engine_.coachAdvice(referenceDeck, deckCtx, candidateCtx, hint, guard);
	}else{
		lastAdvice_ = DjAssistCoachAdvice();
	}
}

// Capture-once-then-compare arrival check for the WAIT_BOUNDARY transition
// step. The first tick this step is current, latches ONE target boundary
// frame (prefer phrase, matching Coach's own preference, else the next
// downbeat); every subsequent tick just compares the live playhead against
// that fixed target - never re-derives a fresh "next" boundary, which would
// always be some future frame and therefore never actually "arrive". No
// SD-read cost beyond what resolveBoundary() already spends once per
// capture (phrase lookups stay throttled by its own cache).
bool DjAssistController::resolveWaitBoundaryReached(const DjSnapshot& snapshot, uint8_t deck){
	if(!session_ || deck >= DJ_DECK_COUNT || !snapshot.decks[deck].loaded) return false;

	if(!waitBoundaryCaptured_){
		DjAssistBoundaryHint fresh;
		if(!resolveBoundary(snapshot, deck, fresh)) return false; // no grid yet - retry next tick.
		waitBoundaryTargetFrame_ = fresh.hasPhrase ? fresh.phraseFrame : fresh.downbeatFrame;
		waitBoundaryCaptured_ = true;
	}

	const uint64_t currentFrame = session_->deckElapsedFrames(deck);
	return currentFrame >= waitBoundaryTargetFrame_;
}

void DjAssistController::tickTransition(const DjSnapshot& snapshot){
	if(!actuator_) return;
	const DjAssistMode mode = engine_.mode();
	if(mode != DJ_ASSIST_MODE_TRANSITION_ARMED && mode != DJ_ASSIST_MODE_TRANSITION_RUNNING) return;

	const DjAssistGuardSnapshot guard = buildGuard(snapshot);

	DjAssistBoundaryHint hint;
	const DjAssistTransitionPlan& p = engine_.plan();
	if(p.currentStep < p.stepCount && p.steps[p.currentStep].action == DJ_ASSIST_ACTION_WAIT_BOUNDARY){
		// Boundary timing is anchored to the currently-audible outgoing
		// deck (fromDeck), matching Coach's own "next safe window" advice;
		// the step's `deck` field (toDeck) only tags which deck is being
		// prepared, not which grid the wait is measured against.
		hint.reached = resolveWaitBoundaryReached(snapshot, p.fromDeck);
	}
	engine_.tick(*actuator_, guard, hint);
}

namespace {
bool planStepSubmitted(const DjAssistTransitionPlan& plan, DjAssistTransitionAction action){
	for(uint8_t i = 0; i < plan.stepCount; ++i){
		if(plan.steps[i].action == action) return plan.steps[i].submitted;
	}
	return false;
}
} // namespace

// Bounded rollback for a terminally-failed/cancelled transition: undoes
// only the side effects THIS plan itself submitted (mix/sync/started-deck),
// in mix -> sync -> stop-deck order, one actuator submit/poll per tick,
// waiting for each command's real applied/failed/superseded result before
// advancing (never assumes success from elapsed time). Deliberately never
// attempts to restart fromDeck once STOP_DECK has run - by that point the
// transition is essentially complete and un-stopping the new deck would be
// more disruptive than leaving it playing.
void DjAssistController::tickRollback(){
	if(!actuator_) return;
	if(engine_.mode() != DJ_ASSIST_MODE_TRANSITION_FAILED) return;
	if(rollbackPhase_ == DjAssistBridge::DJ_ASSIST_ROLLBACK_DONE) return;

	const DjAssistTransitionPlan& p = engine_.plan();
	const bool crossfadeSubmitted = planStepSubmitted(p, DJ_ASSIST_ACTION_CROSSFADE);
	const bool syncSubmitted = planStepSubmitted(p, DJ_ASSIST_ACTION_LOCK_TEMPO) ||
		planStepSubmitted(p, DJ_ASSIST_ACTION_ENABLE_SYNC);
	const bool startDeckSubmitted = planStepSubmitted(p, DJ_ASSIST_ACTION_START_DECK);

	rollbackPhase_ = DjAssistBridge::nextRollbackPhase(
		rollbackPhase_, crossfadeSubmitted, syncSubmitted, startDeckSubmitted
	);
	if(rollbackPhase_ == DjAssistBridge::DJ_ASSIST_ROLLBACK_DONE) return;

	DjAssistTransitionStep step;
	if(rollbackPhase_ == DjAssistBridge::DJ_ASSIST_ROLLBACK_MIX){
		step.action = DJ_ASSIST_ACTION_SET_MIX;
		step.deck = p.fromDeck;
		step.param = p.armedMix;
	}else if(rollbackPhase_ == DjAssistBridge::DJ_ASSIST_ROLLBACK_SYNC){
		step.action = DJ_ASSIST_ACTION_RELEASE_SYNC;
		step.deck = p.toDeck;
	}else{ // DJ_ASSIST_ROLLBACK_STOP_DECK
		step.action = DJ_ASSIST_ACTION_STOP_DECK;
		step.deck = p.toDeck;
	}

	if(!rollbackSubmitted_){
		uint32_t commandId = 0;
		if(actuator_->submit(step, commandId)){
			rollbackCommandId_ = commandId;
			rollbackSubmitted_ = true;
		}
		// Rejected: retry the same bounded submit next tick.
		return;
	}

	switch(DjAssistBridge::evaluateCommandOutcome(actuator_->poll(rollbackCommandId_))){
		case DjAssistBridge::DJ_ASSIST_COMMAND_DONE:
			rollbackSubmitted_ = false;
			if(rollbackPhase_ == DjAssistBridge::DJ_ASSIST_ROLLBACK_MIX){
				rollbackPhase_ = DjAssistBridge::DJ_ASSIST_ROLLBACK_SYNC;
			}else if(rollbackPhase_ == DjAssistBridge::DJ_ASSIST_ROLLBACK_SYNC){
				rollbackPhase_ = DjAssistBridge::DJ_ASSIST_ROLLBACK_STOP_DECK;
			}else{
				rollbackPhase_ = DjAssistBridge::DJ_ASSIST_ROLLBACK_DONE;
			}
			break;
		case DjAssistBridge::DJ_ASSIST_COMMAND_RESUBMIT:
			rollbackSubmitted_ = false; // retry this same phase next tick.
			break;
		case DjAssistBridge::DJ_ASSIST_COMMAND_FAILED:
			// No further automated recovery is safe to attempt here (undoing
			// an undo is out of scope); stop rather than spin forever.
			rollbackPhase_ = DjAssistBridge::DJ_ASSIST_ROLLBACK_DONE;
			break;
		case DjAssistBridge::DJ_ASSIST_COMMAND_WAIT:
		default:
			break; // keep waiting.
	}
}

void DjAssistController::tick(){
	if(!session_) return;
	refreshCandidateTable();

	DjSnapshot snapshot;
	if(!session_->copySnapshot(snapshot)) return;

	updateRecentTracks(snapshot);
	if(fillComplete_) tickSuggestions(snapshot);
	tickTransition(snapshot);
	tickRollback();
}

bool DjAssistController::setCoachEnabled(bool enabled){
	engine_.setCoachEnabled(enabled);
	return true;
}

bool DjAssistController::armTransition(
	uint8_t fromDeck,
	uint8_t toDeck,
	uint32_t libraryIndex,
	const DjTrackIdentity& targetIdentity,
	uint8_t crossfadeBeats,
	bool startAtBoundary,
	bool tempoLock
){
	if(!session_) return false;
	DjSnapshot snapshot;
	session_->copySnapshot(snapshot);
	const DjAssistGuardSnapshot guard = buildGuard(snapshot);
	const bool armed = engine_.armTransition(
		fromDeck, toDeck, libraryIndex, targetIdentity, crossfadeBeats, startAtBoundary, tempoLock, guard
	);
	// A fresh plan must never reuse a WAIT_BOUNDARY target or rollback
	// state latched by a previous transition (cancelled/failed/completed).
	if(armed){
		waitBoundaryCaptured_ = false;
		rollbackPhase_ = DjAssistBridge::DJ_ASSIST_ROLLBACK_IDLE;
		rollbackSubmitted_ = false;
		uint32_t maxId = 0;
		for(uint8_t i = 0; i < DJ_RECENT_RESULT_COUNT; ++i){
			if(snapshot.recentResults[i].id > maxId) maxId = snapshot.recentResults[i].id;
		}
		armWatermarkId_ = maxId;
	}
	return armed;
}

void DjAssistController::cancelTransition(){
	engine_.cancelTransition();
}

void DjAssistController::copySnapshot(DjAssistSnapshot& snapshot) const{
	snapshot.mode = engine_.mode();
	snapshot.plan = engine_.plan();
	snapshot.suggestionCount = suggestionCount_;
	for(uint8_t i = 0; i < suggestionCount_ && i < DJ_ASSIST_MAX_SUGGESTIONS; ++i){
		snapshot.suggestions[i] = suggestions_[i];
	}
	snapshot.advice = lastAdvice_;
}
