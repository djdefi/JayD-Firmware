#include "DjAssistEngine.h"

#include "DjAssistScoring.h"
#include "DjAssistSessionBridge.h"

namespace {

// Small tolerance so mixer noise doesn't read as a manual override.
static const int16_t MIX_OVERRIDE_THRESHOLD = 3;

uint8_t absDiff(uint8_t a, uint8_t b){
	return a > b ? static_cast<uint8_t>(a - b) : static_cast<uint8_t>(b - a);
}

} // namespace

DjAssistEngine::DjAssistEngine() : mode_(DJ_ASSIST_MODE_OFF), plan_(){
}

DjAssistMode DjAssistEngine::mode() const{
	return mode_;
}

void DjAssistEngine::setCoachEnabled(bool enabled){
	if(mode_ == DJ_ASSIST_MODE_TRANSITION_ARMED || mode_ == DJ_ASSIST_MODE_TRANSITION_RUNNING) return;
	mode_ = enabled ? DJ_ASSIST_MODE_COACH : DJ_ASSIST_MODE_OFF;
}

DjAssistCoachAdvice DjAssistEngine::coachAdvice(
	uint8_t playingDeck,
	const DjAssistDeckContext& playing,
	const DjAssistDeckContext& candidate,
	const DjAssistBoundaryHint& boundary,
	const DjAssistGuardSnapshot& guard
) const{
	DjAssistCoachAdvice advice;
	if(playingDeck >= DJ_DECK_COUNT || !playing.valid || !candidate.valid) return advice;

	const uint8_t otherDeck = playingDeck == 0 ? 1 : 0;
	advice.suggestedDeck = otherDeck;

	if(boundary.hasPhrase){
		advice.boundaryIsPhrase = true;
		advice.boundaryFrame = boundary.phraseFrame;
	} else if(boundary.hasDownbeat){
		advice.boundaryIsPhrase = false;
		advice.boundaryFrame = boundary.downbeatFrame;
	} else {
		advice.warningFlags |= DJ_ASSIST_WARN_NO_GRID;
	}

	uint32_t rateMilli = DJ_ASSIST_RATE_UNITY_MILLI;
	const bool rateKnown = DjAssistScoring::requiredRateMilli(playing.bpmMilli, candidate.bpmMilli, rateMilli);
	advice.targetRateMilli = rateMilli;
	if(!rateKnown || rateMilli < DJ_ASSIST_RATE_MIN_MILLI || rateMilli > DJ_ASSIST_RATE_MAX_MILLI){
		advice.warningFlags |= DJ_ASSIST_WARN_OUT_OF_RANGE;
	}

	advice.crossfaderDirection = otherDeck == 1 ? 1 : -1;

	static const uint64_t minRemainingSeconds = 20;
	if(playing.sampleRate > 0 && (playing.remainingFrames / playing.sampleRate) < minRemainingSeconds){
		advice.warningFlags |= DJ_ASSIST_WARN_ENDING_SOON;
	}

	if(guard.recording) advice.warningFlags |= DJ_ASSIST_WARN_RECORDING_ACTIVE;
	if(guard.loopActive[playingDeck] || guard.loopActive[otherDeck]) advice.warningFlags |= DJ_ASSIST_WARN_LOOP_ACTIVE;
	if(!guard.metadataValid[playingDeck] || !guard.metadataValid[otherDeck]) advice.warningFlags |= DJ_ASSIST_WARN_NO_METADATA;

	advice.valid = true;
	return advice;
}

bool DjAssistEngine::armTransition(
	uint8_t fromDeck,
	uint8_t toDeck,
	uint32_t libraryIndex,
	const DjTrackIdentity& targetIdentity,
	uint8_t crossfadeBeats,
	bool startAtBoundary,
	bool tempoLock,
	const DjAssistGuardSnapshot& guard
){
	if(mode_ == DJ_ASSIST_MODE_TRANSITION_ARMED || mode_ == DJ_ASSIST_MODE_TRANSITION_RUNNING) return false;
	if(fromDeck >= DJ_DECK_COUNT || toDeck >= DJ_DECK_COUNT || fromDeck == toDeck) return false;
	if(crossfadeBeats != 4 && crossfadeBeats != 8 && crossfadeBeats != 16 && crossfadeBeats != 32) return false;
	if(guard.recording) return false;
	if(!guard.deckLoaded[toDeck]) return false;
	if(!DjAssistScoring::identityMatches(guard.deckIdentity[toDeck], targetIdentity)) return false;
	if(!guard.deckPlaying[fromDeck]) return false;
	if(guard.loopActive[fromDeck] || guard.loopActive[toDeck]) return false;
	if(!guard.metadataValid[fromDeck]) return false;
	// Target deck must be stopped and sync-off at arm time: this is what
	// makes rollback's applied-mutation ownership tracking safe below. If
	// the target were already playing/synced, the plan's START_DECK/
	// ENABLE_SYNC steps would be idempotent no-ops against pre-existing
	// user state that a rollback must never touch - simplest safe contract
	// is to just refuse to arm until the user (or a prior plan) returns the
	// target deck to that baseline.
	if(guard.deckPlaying[toDeck] || guard.syncActive[toDeck]) return false;

	plan_ = DjAssistTransitionPlan();
	plan_.fromDeck = fromDeck;
	plan_.toDeck = toDeck;
	plan_.libraryIndex = libraryIndex;
	plan_.targetIdentity = targetIdentity;
	plan_.crossfadeBeats = crossfadeBeats;
	plan_.startAtBoundary = startAtBoundary;
	plan_.tempoLock = tempoLock;
	plan_.armedMix = guard.mix;
	plan_.armedFromPlaying = guard.deckPlaying[fromDeck];
	plan_.armedFromRateMilli = guard.rateMilli[fromDeck];

	buildSteps(plan_);
	mode_ = DJ_ASSIST_MODE_TRANSITION_ARMED;
	return true;
}

void DjAssistEngine::buildSteps(DjAssistTransitionPlan& plan) const{
	uint8_t n = 0;
	const uint8_t toDeck = plan.toDeck;
	const uint8_t fromDeck = plan.fromDeck;

	if(plan.startAtBoundary && n < DJ_ASSIST_MAX_TRANSITION_STEPS){
		plan.steps[n].action = DJ_ASSIST_ACTION_WAIT_BOUNDARY;
		plan.steps[n].deck = toDeck;
		n++;
	}
	if(n < DJ_ASSIST_MAX_TRANSITION_STEPS){
		plan.steps[n].action = DJ_ASSIST_ACTION_START_DECK;
		plan.steps[n].deck = toDeck;
		n++;
	}
	if(plan.tempoLock){
		if(n < DJ_ASSIST_MAX_TRANSITION_STEPS){
			plan.steps[n].action = DJ_ASSIST_ACTION_LOCK_TEMPO;
			plan.steps[n].deck = toDeck;
			n++;
		}
		if(n < DJ_ASSIST_MAX_TRANSITION_STEPS){
			plan.steps[n].action = DJ_ASSIST_ACTION_ENABLE_SYNC;
			plan.steps[n].deck = toDeck;
			n++;
		}
	}
	if(n < DJ_ASSIST_MAX_TRANSITION_STEPS){
		plan.steps[n].action = DJ_ASSIST_ACTION_CROSSFADE;
		plan.steps[n].deck = toDeck;
		plan.steps[n].param = plan.crossfadeBeats;
		n++;
	}
	if(n < DJ_ASSIST_MAX_TRANSITION_STEPS){
		plan.steps[n].action = DJ_ASSIST_ACTION_STOP_DECK;
		plan.steps[n].deck = fromDeck;
		n++;
	}
	if(plan.tempoLock && n < DJ_ASSIST_MAX_TRANSITION_STEPS){
		plan.steps[n].action = DJ_ASSIST_ACTION_RELEASE_SYNC;
		plan.steps[n].deck = toDeck;
		n++;
	}
	plan.stepCount = n;
}

bool DjAssistEngine::stepSubmitted(DjAssistTransitionAction action) const{
	for(uint8_t i = 0; i < plan_.stepCount; i++){
		if(plan_.steps[i].action == action) return plan_.steps[i].submitted;
	}
	return false;
}

bool DjAssistEngine::guardOk(const DjAssistGuardSnapshot& guard, DjAssistTransitionFailure& failure) const{
	if(!guard.mediaPresent){
		failure = DJ_ASSIST_FAIL_MEDIA_REMOVED;
		return false;
	}
	if(!guard.metadataValid[plan_.fromDeck] || !guard.metadataValid[plan_.toDeck]){
		failure = DJ_ASSIST_FAIL_METADATA_LOST;
		return false;
	}
	if(!guard.deckLoaded[plan_.toDeck]){
		failure = DJ_ASSIST_FAIL_TARGET_NOT_LOADED;
		return false;
	}
	if(!DjAssistScoring::identityMatches(guard.deckIdentity[plan_.toDeck], plan_.targetIdentity)){
		failure = DJ_ASSIST_FAIL_TARGET_CHANGED;
		return false;
	}
	if(guard.recording){
		failure = DJ_ASSIST_FAIL_CONFLICT;
		return false;
	}
	if(guard.loopActive[plan_.fromDeck] || guard.loopActive[plan_.toDeck]){
		failure = DJ_ASSIST_FAIL_CONFLICT;
		return false;
	}

	const bool stopSubmitted = stepSubmitted(DJ_ASSIST_ACTION_STOP_DECK);
	const bool crossfadeSubmitted = stepSubmitted(DJ_ASSIST_ACTION_CROSSFADE);
	const bool startDeckSubmitted = stepSubmitted(DJ_ASSIST_ACTION_START_DECK);
	const bool syncSubmitted = stepSubmitted(DJ_ASSIST_ACTION_ENABLE_SYNC);

	// armTransition() requires the target deck stopped and sync-off at arm
	// time (see DjAssistTransitionPlan::toDeckStartOwnedByPlan/
	// toDeckSyncOwnedByPlan). If the user starts playback or engages sync
	// on the target deck before the plan's own START_DECK/ENABLE_SYNC step
	// has been submitted, that divergence must abort the transition rather
	// than let the later idempotent system command silently be credited as
	// plan-owned (which would make rollback stop/release state the user,
	// not the plan, introduced).
	if(!startDeckSubmitted && guard.deckPlaying[plan_.toDeck]){
		failure = DJ_ASSIST_FAIL_MANUAL_OVERRIDE;
		return false;
	}
	if(!syncSubmitted && guard.syncActive[plan_.toDeck]){
		failure = DJ_ASSIST_FAIL_MANUAL_OVERRIDE;
		return false;
	}

	if(!stopSubmitted && guard.deckPlaying[plan_.fromDeck] != plan_.armedFromPlaying){
		failure = DJ_ASSIST_FAIL_MANUAL_OVERRIDE;
		return false;
	}
	// Checked regardless of crossfadeSubmitted: once the ramp begins, mix
	// legitimately moves away from armedMix, so the plain threshold check
	// below is skipped - but a manual (non-system) mix command observed
	// during that same window must still abort rather than let the next
	// programmatic ramp tick silently overwrite it.
	if(guard.manualMixOverride){
		failure = DJ_ASSIST_FAIL_MANUAL_OVERRIDE;
		return false;
	}
	if(!crossfadeSubmitted && absDiff(guard.mix, plan_.armedMix) > MIX_OVERRIDE_THRESHOLD){
		failure = DJ_ASSIST_FAIL_MANUAL_OVERRIDE;
		return false;
	}
	if(!stopSubmitted && guard.rateMilli[plan_.fromDeck] != plan_.armedFromRateMilli){
		failure = DJ_ASSIST_FAIL_MANUAL_OVERRIDE;
		return false;
	}

	return true;
}

void DjAssistEngine::fail(DjAssistTransitionFailure reason){
	plan_.failure = reason;
	mode_ = DJ_ASSIST_MODE_TRANSITION_FAILED;
}

void DjAssistEngine::tick(DjAssistActuator& actuator, const DjAssistGuardSnapshot& guard, const DjAssistBoundaryHint& boundary){
	if(mode_ != DJ_ASSIST_MODE_TRANSITION_ARMED && mode_ != DJ_ASSIST_MODE_TRANSITION_RUNNING) return;

	DjAssistTransitionFailure failure = DJ_ASSIST_FAIL_NONE;
	if(!guardOk(guard, failure)){
		fail(failure);
		return;
	}

	if(mode_ == DJ_ASSIST_MODE_TRANSITION_ARMED) mode_ = DJ_ASSIST_MODE_TRANSITION_RUNNING;

	if(plan_.currentStep >= plan_.stepCount){
		mode_ = DJ_ASSIST_MODE_TRANSITION_COMPLETE;
		return;
	}

	DjAssistTransitionStep& step = plan_.steps[plan_.currentStep];

	if(step.action == DJ_ASSIST_ACTION_WAIT_BOUNDARY){
		// Only `reached` (the playhead having actually arrived at the one
		// captured target boundary) may advance this step - hasPhrase/
		// hasDownbeat describe a perpetually-recomputed *next* boundary
		// and are true almost every tick, which previously made this step
		// advance immediately instead of waiting.
		if(!boundary.reached) return;
		step.submitted = true;
		step.applied = true;
		plan_.currentStep++;
		return;
	}

	if(!step.submitted){
		uint32_t commandId = 0;
		if(!actuator.submit(step, commandId)){
			fail(DJ_ASSIST_FAIL_COMMAND_REJECTED);
			return;
		}
		step.commandId = commandId;
		step.submitted = true;
		return;
	}

	const DjCommandStatus status = actuator.poll(step.commandId);
	switch(DjAssistBridge::evaluateCommandOutcome(status)){
		case DjAssistBridge::DJ_ASSIST_COMMAND_DONE:
			step.applied = true;
			// Ownership is recorded from the exact applied mutation, never
			// inferred from a baseline+submitted heuristic - a manual
			// play/sync between arm and this step's submit is already
			// caught by guardOk()'s divergence checks above, so reaching
			// APPLIED here means this system command is what produced the
			// current playing/synced state.
			if(step.action == DJ_ASSIST_ACTION_START_DECK) plan_.toDeckStartOwnedByPlan = true;
			if(step.action == DJ_ASSIST_ACTION_ENABLE_SYNC) plan_.toDeckSyncOwnedByPlan = true;
			plan_.currentStep++;
			if(plan_.currentStep >= plan_.stepCount) mode_ = DJ_ASSIST_MODE_TRANSITION_COMPLETE;
			return;
		case DjAssistBridge::DJ_ASSIST_COMMAND_FAILED:
		case DjAssistBridge::DJ_ASSIST_COMMAND_RESUBMIT:
			// SUPERSEDED (RESUBMIT) is only a bounded, same-plan retry
			// policy for the crossfade actuator's own final-mix command,
			// which never surfaces raw SUPERSEDED here (pollCrossfade()
			// maps it internally to PENDING). For every normal step, a
			// command that was superseded before it ever ran will never
			// apply - waiting on it forever would strand the transition.
			fail(DJ_ASSIST_FAIL_COMMAND_REJECTED);
			return;
		case DjAssistBridge::DJ_ASSIST_COMMAND_WAIT:
		default:
			return; // still in flight, keep waiting.
	}
}

void DjAssistEngine::cancelTransition(){
	if(mode_ == DJ_ASSIST_MODE_TRANSITION_ARMED || mode_ == DJ_ASSIST_MODE_TRANSITION_RUNNING){
		fail(DJ_ASSIST_FAIL_CANCELLED);
	}
}

const DjAssistTransitionPlan& DjAssistEngine::plan() const{
	return plan_;
}
