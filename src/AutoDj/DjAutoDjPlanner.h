#ifndef JAYD_FIRMWARE_DJ_AUTO_DJ_PLANNER_H
#define JAYD_FIRMWARE_DJ_AUTO_DJ_PLANNER_H

#include "AutoDjLoadPort.h"
#include "DjAutoDjHistory.h"
#include "DjAutoDjQueue.h"
#include "DjAutoDjStateMachine.h"
#include "DjAutoDjTypes.h"

enum class AutoDjPendingPhase : uint8_t {
	None,
	WaitingOutcome // a load has been submitted and we're polling for its result
};

// Orchestrates the bounded queue, bounded history, and state machine into a
// single deterministic Auto DJ planner. `tick()` is the one-shot runtime
// transition engine: each call performs at most one state or queue mutation,
// so it can never spin indefinitely regardless of what the load port reports.
//
// Track selection (`selectNext`) is deliberately simple and conservative: it
// is the seam that gets replaced with the final Coach scoring model on
// rebase, but the exclusion/tie-break/fallback behavior around it (bounded
// recent/artist/title exclusion, stable ties, no fabricated harmonic/beat
// claims on missing metadata) is expected to remain the same.
class DjAutoDjPlanner {
public:
	explicit DjAutoDjPlanner(AutoDjLoadPort& port) : port(port){}

	AutoDjState state() const{
		return machine.state();
	}

	AutoDjFailReason failReason() const{
		return machine.failReason();
	}

	uint8_t queueDepth() const{
		return queue.depth();
	}

	uint8_t historySize() const{
		return history.size();
	}

	bool pinTrack(const AutoDjIdentity& identity, uint32_t artistHash = 0, uint32_t titleHash = 0){
		return queue.pushPinned(identity, artistHash, titleHash);
	}

	// Chooses the best remaining candidate not already queued/excluded and
	// appends it to the queue as a planned entry. Returns false if there is
	// no eligible candidate or the queue is full.
	bool planNext(const AutoDjCandidate* candidates, uint8_t count){
		AutoDjCandidate chosen;
		if(!selectNext(candidates, count, chosen)) return false;
		return queue.pushPlanned(chosen.identity, chosen.artistHash, chosen.titleHash, chosen.reasons);
	}

	// Deterministic candidate selection used by planNext(). Exposed
	// separately so it can be unit tested without touching the queue.
	bool selectNext(const AutoDjCandidate* candidates, uint8_t count, AutoDjCandidate& chosen) const;

	void invalidateLibraryGeneration(uint32_t currentGeneration){
		queue.invalidateGeneration(currentGeneration);
	}

	bool arm(){
		return machine.arm(port.hasStableIdEndpoint(), port.physicalConfirmationPresent());
	}

	bool start(){
		return machine.start(hasSafeWindow());
	}

	bool pause(){
		return machine.pause();
	}

	bool resume(){
		return machine.resume(port.physicalConfirmationPresent());
	}

	// Requests a stop. Does not discard an in-flight load: a command already
	// submitted to hardware may still apply, so Stopping waits for it to
	// resolve (or time out) via tick() before finishing.
	bool stop(){
		return machine.stop();
	}

	bool reset(){
		cancelPending();
		return machine.reset();
	}

	// One-shot runtime step. Safe to call on any fixed cadence (e.g. once
	// per main-loop tick); each call does at most one thing.
	void tick(){
		if(machine.state() == AutoDjState::Stopping){
			if(pendingPhase != AutoDjPendingPhase::None){
				resolvePendingWhileStopping();
				return;
			}
			machine.finishStop();
			return;
		}
		if(machine.state() != AutoDjState::Running) return;

		if(port.manualTakeoverActive()){
			cancelPending();
			machine.pause();
			return;
		}

		if(port.recordingFailed()){
			cancelPending();
			machine.fail(AutoDjFailReason::RecordingFailure);
			return;
		}

		if(pendingPhase != AutoDjPendingPhase::None){
			progressPending();
			return;
		}

		if(!port.currentTrackAtEnd()) return;

		if(!port.currentDurationTrustworthy()){
			// Conservative fallback: never guess a crossfade point when the
			// remaining duration isn't trustworthy - hand control back.
			machine.pause();
			return;
		}

		beginNextLoad();
	}

private:
	bool hasSafeWindow() const{
		return port.hasStableIdEndpoint() && !port.manualTakeoverActive() && !queue.empty();
	}

	void beginNextLoad(){
		AutoDjQueueEntry entry;
		if(!queue.peekNext(entry)){
			machine.complete();
			return;
		}
		if(!port.submitLoad(entry.identity)){
			pendingAttempts++;
			handleLoadFailure();
			return;
		}
		pendingPhase = AutoDjPendingPhase::WaitingOutcome;
		pendingTimeoutTicks = 0;
		pendingAttempts++;
	}

	void progressPending(){
		const AutoDjLoadOutcome outcome = port.pollLoad();
		if(outcome == AutoDjLoadOutcome::Pending || outcome == AutoDjLoadOutcome::Accepted){
			pendingTimeoutTicks++;
			if(pendingTimeoutTicks >= AUTO_DJ_LOAD_TIMEOUT_TICKS) handleLoadFailure();
			return;
		}

		if(outcome == AutoDjLoadOutcome::Applied){
			AutoDjQueueEntry entry;
			if(queue.popNext(entry)){
				history.record(entry.identity, entry.artistHash, entry.titleHash);
			}
			pendingPhase = AutoDjPendingPhase::None;
			pendingAttempts = 0;
			pendingTimeoutTicks = 0;
			return;
		}

		handleLoadFailure();
	}

	// A failed/timed-out attempt either retries (within the bounded budget)
	// or, once the budget is exhausted, permanently skips that one entry so
	// the planner can never get stuck retrying forever.
	void handleLoadFailure(){
		pendingPhase = AutoDjPendingPhase::None;
		pendingTimeoutTicks = 0;
		if(pendingAttempts > AUTO_DJ_RETRY_BUDGET){
			AutoDjQueueEntry dropped;
			queue.popNext(dropped);
			pendingAttempts = 0;
		}
		// else: leave the entry at the front of the queue; the next tick()
		// retries it as long as the deck is still reporting end-of-track.
	}

	// While Stopping, an in-flight load is allowed to resolve exactly once
	// more (or time out) but is never retried - we're shutting down, not
	// continuing the session.
	void resolvePendingWhileStopping(){
		const AutoDjLoadOutcome outcome = port.pollLoad();
		if(outcome == AutoDjLoadOutcome::Pending || outcome == AutoDjLoadOutcome::Accepted){
			pendingTimeoutTicks++;
			if(pendingTimeoutTicks < AUTO_DJ_LOAD_TIMEOUT_TICKS) return;
		} else if(outcome == AutoDjLoadOutcome::Applied){
			AutoDjQueueEntry entry;
			if(queue.popNext(entry)) history.record(entry.identity, entry.artistHash, entry.titleHash);
		}
		cancelPending();
	}

	void cancelPending(){
		pendingPhase = AutoDjPendingPhase::None;
		pendingTimeoutTicks = 0;
		pendingAttempts = 0;
	}

	AutoDjLoadPort& port;
	DjAutoDjStateMachine machine;
	DjAutoDjQueue queue;
	DjAutoDjHistory history;
	AutoDjPendingPhase pendingPhase = AutoDjPendingPhase::None;
	uint16_t pendingTimeoutTicks = 0;
	uint8_t pendingAttempts = 0;
};

#endif //JAYD_FIRMWARE_DJ_AUTO_DJ_PLANNER_H
