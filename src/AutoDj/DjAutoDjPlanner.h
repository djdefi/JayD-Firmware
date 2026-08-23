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

	// True if `identity` is currently queued (pinned, planned, or the
	// in-flight pending entry) - i.e. not eligible to be queued again.
	bool isQueued(const AutoDjIdentity& identity) const{
		return queue.containsIdentity(identity);
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

	// Sibling of invalidateLibraryGeneration(): see
	// DjAutoDjQueue::invalidateRevision()'s and AutoDjIdentity::
	// metadataRevision's doc comments. A pending in-flight entry dropped by
	// this call is picked up the same way an invalidateLibraryGeneration()
	// drop already is - beginNextLoad()'s containsSequence() re-check
	// before any retry abandons it cleanly instead of resubmitting.
	void invalidateMetadataRevision(uint32_t currentRevision){
		queue.invalidateRevision(currentRevision);
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

	// Transactional: the state machine's own reset() only mutates state
	// when current is Failed/Complete, but cancelPending() has no such
	// gate of its own - if it ran first and the machine then rejected the
	// reset, a legitimate reset() call elsewhere (e.g. from Running) would
	// have silently discarded live pending-load bookkeeping for nothing.
	// Check eligibility (via the machine) before mutating any planner
	// state, so a rejected reset is guaranteed to be a no-op.
	bool reset(){
		if(!machine.reset()) return false;
		cancelPending();
		return true;
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

	// Starts (or retries) exactly one load. `pendingEntry` is captured once
	// per attempt-cycle (pendingAttempts == 0) from whatever is currently at
	// the queue front; while pendingAttempts > 0 (waiting on an outcome, or
	// retrying after a failure) it is reused as-is rather than re-derived
	// from the queue front, which can change if a different track gets
	// pinned while this load is in flight or awaiting retry. Before any
	// retry, though, the captured entry's presence is re-checked: if it was
	// dropped out from under us (e.g. invalidateLibraryGeneration() removed
	// it because the library re-indexed), it is never resubmitted - that
	// attempt is abandoned cleanly instead, so a stale/foreign identity is
	// never sent to the load port.
	void beginNextLoad(){
		if(pendingAttempts == 0){
			if(!queue.peekNext(pendingEntry)){
				machine.complete();
				return;
			}
		} else if(!queue.containsSequence(pendingEntry.sequence)){
			cancelPending();
			return;
		}
		if(!port.submitLoad(pendingEntry.identity)){
			pendingAttempts++;
			handleLoadFailure();
			return;
		}
		pendingPhase = AutoDjPendingPhase::WaitingOutcome;
		pendingDeadlineUs = port.nowMicros() + AUTO_DJ_LOAD_TIMEOUT_US;
		pendingAttempts++;
	}

	// Wraparound-safe "has the deadline passed" check - shared with
	// AutoDjSessionActuator's own Teardown-phase deadline via
	// djAutoDjDeadlinePassed() (DjAutoDjTypes.h) so both use identical
	// wraparound handling.
	static bool deadlinePassed(uint64_t now, uint64_t deadline){
		return djAutoDjDeadlinePassed(now, deadline);
	}

	void progressPending(){
		const AutoDjLoadOutcome outcome = port.pollLoad();
		if(outcome == AutoDjLoadOutcome::Pending || outcome == AutoDjLoadOutcome::Accepted){
			if(deadlinePassed(port.nowMicros(), pendingDeadlineUs)) handleLoadFailure();
			return;
		}

		if(outcome == AutoDjLoadOutcome::Applied){
			removeResolvedEntry(true /* recordHistory */);
			pendingPhase = AutoDjPendingPhase::None;
			pendingAttempts = 0;
			pendingDeadlineUs = 0;
			return;
		}

		// Settling: the port is waiting for Coach's own rollback to settle
		// after a failed/cancelled transition and owns its own bounded
		// deadline for that wait (see AutoDjLoadOutcome::Settling's doc
		// comment) - this composite attempt's outer pendingDeadlineUs must
		// NOT be applied here (it may already be exhausted by the
		// transition that just failed), so this is deliberately a pure
		// no-op poll: neither a retry nor a terminal decision until the
		// port itself resolves to Applied/Failed/FailedTerminal.
		if(outcome == AutoDjLoadOutcome::Settling) return;

		if(outcome == AutoDjLoadOutcome::FailedTerminal){
			// Unsafe to retry or skip - see AutoDjFailReason::TeardownTimeout.
			// Deliberately does NOT call removeResolvedEntry()/touch the
			// queue: an unsettled rollback may still reference the deck
			// this attempt targeted, so the queue/pendingEntry are left
			// exactly as they were rather than risk a fresh submitLoad()
			// racing it. machine.fail() is terminal until an explicit
			// reset(), which also clears pendingEntry (see reset()).
			pendingPhase = AutoDjPendingPhase::None;
			pendingDeadlineUs = 0;
			machine.fail(AutoDjFailReason::TeardownTimeout);
			return;
		}

		handleLoadFailure();
	}

	// A failed/timed-out attempt either retries (within the bounded budget)
	// or, once the budget is exhausted, permanently skips that one entry so
	// the planner can never get stuck retrying forever. Either way this
	// resolves the exact entry that was submitted (`pendingEntry`), never
	// "whatever is currently at the queue front".
	void handleLoadFailure(){
		pendingPhase = AutoDjPendingPhase::None;
		pendingDeadlineUs = 0;
		if(pendingAttempts > AUTO_DJ_RETRY_BUDGET){
			removeResolvedEntry(false /* recordHistory */);
			pendingAttempts = 0;
		}
		// else: pendingAttempts stays > 0 and pendingEntry stays set, so the
		// next beginNextLoad() retries this exact entry regardless of what
		// else may have been queued/pinned meanwhile.
	}

	// While Stopping, an in-flight load is allowed to resolve exactly once
	// more (or time out) but is never retried - we're shutting down, not
	// continuing the session. A Settling outcome still defers entirely to
	// the port's own Teardown deadline (see progressPending()); a
	// FailedTerminal outcome still escalates to the terminal Failed state
	// rather than silently finishing the stop, since an unsettled rollback
	// must not be treated as "safely stopped".
	void resolvePendingWhileStopping(){
		const AutoDjLoadOutcome outcome = port.pollLoad();
		if(outcome == AutoDjLoadOutcome::Pending || outcome == AutoDjLoadOutcome::Accepted){
			if(!deadlinePassed(port.nowMicros(), pendingDeadlineUs)) return;
		} else if(outcome == AutoDjLoadOutcome::Applied){
			removeResolvedEntry(true /* recordHistory */);
		} else if(outcome == AutoDjLoadOutcome::Settling){
			return;
		} else if(outcome == AutoDjLoadOutcome::FailedTerminal){
			pendingPhase = AutoDjPendingPhase::None;
			pendingDeadlineUs = 0;
			machine.fail(AutoDjFailReason::TeardownTimeout);
			return;
		}
		cancelPending();
	}

	// Removes exactly the entry that was submitted for the just-resolved
	// attempt (matched by its unique insertion sequence, not queue
	// position), optionally recording it to history. If the entry is no
	// longer present (e.g. dropped already by invalidateGeneration()) this
	// is a no-op - there is nothing stale left to remove or record.
	void removeResolvedEntry(bool recordHistory){
		AutoDjQueueEntry resolved;
		if(!queue.removeBySequence(pendingEntry.sequence, resolved)) return;
		if(recordHistory) history.record(resolved.identity, resolved.artistHash, resolved.titleHash);
	}

	void cancelPending(){
		pendingPhase = AutoDjPendingPhase::None;
		pendingDeadlineUs = 0;
		pendingAttempts = 0;
	}

	AutoDjLoadPort& port;
	DjAutoDjStateMachine machine;
	DjAutoDjQueue queue;
	DjAutoDjHistory history;
	AutoDjPendingPhase pendingPhase = AutoDjPendingPhase::None;
	AutoDjQueueEntry pendingEntry = {}; // valid whenever pendingAttempts > 0
	uint64_t pendingDeadlineUs = 0; // valid whenever pendingPhase == WaitingOutcome
	uint8_t pendingAttempts = 0;
};

#endif //JAYD_FIRMWARE_DJ_AUTO_DJ_PLANNER_H
