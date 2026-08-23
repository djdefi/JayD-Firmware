#ifndef JAYD_FIRMWARE_DJASSISTENGINE_H
#define JAYD_FIRMWARE_DJASSISTENGINE_H

#include "DjAssistTypes.h"

// Drives the semantic actions a one-shot transition plan needs from the
// real Sync/quantize/loop layer. That layer is still being implemented on a
// separate branch (djdefi-psychic-fiesta); until it lands, a concrete
// adapter binds these calls to real primitives. This is the conflict-
// isolation seam - DjAssistEngine never talks to Sync/quantize/Deck code
// directly, only through this interface, so it can be authored and tested
// now and wired up later without touching the state machine.
class DjAssistActuator {
public:
	virtual ~DjAssistActuator(){}

	// Submits one semantic action. Returns true and fills outCommandId when
	// accepted for asynchronous completion; returns false on immediate
	// rejection (e.g. invalid deck, no target loaded).
	virtual bool submit(const DjAssistTransitionStep& step, uint32_t& outCommandId) = 0;

	// Polls a previously submitted command's status.
	virtual DjCommandStatus poll(uint32_t commandId) const = 0;
};

// Coach/suggestions/one-shot-transition state machine. Coach advice is
// always nonbinding (const, never mutates playback). Sync is never enabled
// silently - only armTransition(), an explicit user confirmation, can queue
// an DJ_ASSIST_ACTION_ENABLE_SYNC step.
class DjAssistEngine {
public:
	DjAssistEngine();

	DjAssistMode mode() const;

	// Toggles Coach on/off. Never disturbs an active transition.
	void setCoachEnabled(bool enabled);

	// Bounded, nonbinding advice. Never mutates engine or playback state.
	DjAssistCoachAdvice coachAdvice(
		uint8_t playingDeck,
		const DjAssistDeckContext& playing,
		const DjAssistDeckContext& candidate,
		const DjAssistBoundaryHint& boundary,
		const DjAssistGuardSnapshot& guard
	) const;

	// Arms a one-shot transition. This call *is* the explicit user
	// confirmation; it never fires implicitly from suggestions/coach advice.
	// Fails (returns false, mode unchanged) when the target isn't loaded,
	// the source deck isn't playing, recording/looping conflicts exist, or
	// arguments are invalid.
	bool armTransition(
		uint8_t fromDeck,
		uint8_t toDeck,
		uint32_t libraryIndex,
		const DjTrackIdentity& targetIdentity,
		uint8_t crossfadeBeats,
		bool startAtBoundary,
		bool tempoLock,
		const DjAssistGuardSnapshot& guard
	);

	// Advances the running transition by at most one bounded unit of work
	// (submit a step, or check a previously submitted one for its applied
	// result). Never blocks; safe to call from a non-audio, non-UI loop.
	void tick(DjAssistActuator& actuator, const DjAssistGuardSnapshot& guard, const DjAssistBoundaryHint& boundary);

	// Explicit cancel; transitions ARMED/RUNNING to FAILED with
	// DJ_ASSIST_FAIL_CANCELLED so the caller can drive a rollback.
	void cancelTransition();

	const DjAssistTransitionPlan& plan() const;

private:
	DjAssistMode mode_;
	DjAssistTransitionPlan plan_;

	void buildSteps(DjAssistTransitionPlan& plan) const;
	bool stepSubmitted(DjAssistTransitionAction action) const;
	// Non-const: a detected per-property manual-override divergence
	// (toDeck play/sync) immediately relinquishes that property's
	// rollback-ownership flag on plan_ - see the definition for why.
	bool guardOk(const DjAssistGuardSnapshot& guard, DjAssistTransitionFailure& failure);
	void fail(DjAssistTransitionFailure reason);
};

#endif
