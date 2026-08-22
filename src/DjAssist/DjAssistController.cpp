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
// DjSession::apply()); RELEASE_SYNC maps to setSync(deck, false, ...).
// CROSSFADE has no single DjCommand - it's a continuous ramp driven by
// repeated, supersedable DJ_COMMAND_SET_MIX submissions each poll, tracked
// under a locally-issued pseudo command id disjoint from real DjCommand
// ids. Completion of the crossfade is detected from
// DjAssistBridge::computeCrossfadeMix() itself reaching the exact target
// endpoint (0 or 255), which it only ever returns once the ramp has fully
// elapsed (see computeCrossfadeMix()/crossfadeCurve()'s saturation
// behaviour) - never partway through a legitimate ramp step.
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
			case DJ_ASSIST_ACTION_WAIT_BOUNDARY:
				break; // engine never submits this action to the actuator.
		}
		return false;
	}

	DjCommandStatus poll(uint32_t commandId) const override{
		if(commandId >= CrossfadeIdBase) return pollCrossfade(commandId);

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

private:
	static const uint32_t CrossfadeIdBase = 0x80000000UL;

	DjSession* session_;
	uint32_t crossfadeId_ = CrossfadeIdBase;
	uint8_t crossfadeToDeck_ = 0;
	uint8_t crossfadeBeats_ = 16;
	uint64_t crossfadeStartMicros_ = 0;
	mutable bool crossfadeDone_ = false;

	static bool acceptedResult(const DjSubmitResult& result, uint32_t& outCommandId){
		if(result.status == DJ_COMMAND_REJECTED) return false;
		outCommandId = result.id;
		return true;
	}

	bool beginCrossfade(const DjAssistTransitionStep& step, uint32_t& outCommandId){
		if(crossfadeId_ == 0xFFFFFFFFUL) crossfadeId_ = CrossfadeIdBase; // guard wrap (never reached in practice)
		++crossfadeId_;
		crossfadeToDeck_ = step.deck;
		crossfadeBeats_ = (step.param > 0 && step.param <= 255) ? uint8_t(step.param) : 16;
		crossfadeStartMicros_ = micros();
		crossfadeDone_ = false;
		outCommandId = crossfadeId_;
		return true;
	}

	DjCommandStatus pollCrossfade(uint32_t commandId) const{
		if(commandId != crossfadeId_) return DJ_COMMAND_FAILED;
		if(crossfadeDone_) return DJ_COMMAND_APPLIED;

		DjSnapshot snapshot;
		session_->copySnapshot(snapshot);
		const uint32_t bpmMilli = snapshot.decks[crossfadeToDeck_].metadata.bpmMilli;
		const uint64_t elapsedMicros = micros() - crossfadeStartMicros_;
		const uint8_t mixValue = DjAssistBridge::computeCrossfadeMix(
			crossfadeToDeck_, elapsedMicros, crossfadeBeats_, bpmMilli
		);
		session_->setMix(mixValue, DJ_ORIGIN_SYSTEM);

		const uint8_t targetEndpoint = crossfadeToDeck_ == 0 ? 0 : 255;
		if(mixValue == targetEndpoint){
			crossfadeDone_ = true;
			return DJ_COMMAND_APPLIED;
		}
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

	uint16_t filled = 0;
	while(fillCursor_ < cappedTotal && filled < DJ_ASSIST_DEFAULT_SCAN_BUDGET){
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
		resolveBoundary(snapshot, p.fromDeck, hint);
	}
	engine_.tick(*actuator_, guard, hint);
}

void DjAssistController::tick(){
	if(!session_) return;
	refreshCandidateTable();

	DjSnapshot snapshot;
	if(!session_->copySnapshot(snapshot)) return;

	updateRecentTracks(snapshot);
	if(fillComplete_) tickSuggestions(snapshot);
	tickTransition(snapshot);
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
	return engine_.armTransition(
		fromDeck, toDeck, libraryIndex, targetIdentity, crossfadeBeats, startAtBoundary, tempoLock, guard
	);
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
