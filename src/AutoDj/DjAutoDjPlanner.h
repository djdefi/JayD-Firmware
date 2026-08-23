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
			// Nothing was pending by the time Stopping got here, but
			// failAfterPending may still be latched: it is set as soon as
			// authority is lost while an attempt is in flight, and is only
			// ever cleared by cancelPending() - if that attempt then
			// resolved via an ordinary Running tick's progressPending()
			// (which clears pendingPhase directly, never calling
			// cancelPending() - see failAfterPending's doc comment) before
			// stop() was requested, this is the first place afterward that
			// can observe it. resolvePendingWhileStopping()'s own
			// Applied/timeout fallthrough already calls cancelPending(),
			// so this call is only ever a no-op there; it is required
			// here so a stale latch can never survive through Off and
			// spuriously fail the next otherwise-healthy arm()/start(),
			// even once the authority has since recovered.
			cancelPending();
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
			// The stable-ID authority is checked at arm()/start() time, but
			// nothing previously re-checked it while Running: a fill-worker
			// exit or later allocation failure mid-session went unnoticed
			// until the next load attempt exhausted its retry budget and
			// was silently skipped (or retried forever against a dead
			// authority). A composite load/Coach-arm/transition/rollback
			// already in flight when the loss is detected must never be
			// interrupted or stranded, though - see failAfterPending's doc
			// comment - so this only ever records the loss and lets
			// progressPending() carry the in-flight attempt through to its
			// own outcome exactly as before.
			if(!port.hasStableIdEndpoint()) failAfterPending = true;
			progressPending();
			return;
		}

		// Nothing is in flight (the branch above returned already if it
		// were), so this is the "no composite operation pending" case: if
		// the authority has ever been lost - either just now, or while the
		// attempt that has since resolved was in flight - fail immediately
		// and admit no further Auto command, rather than letting
		// currentTrackAtEnd()/beginNextLoad() below attempt a fresh load
		// against an authority that can no longer produce a trustworthy
		// candidate. Terminal, like TeardownTimeout: only an explicit
		// reset() (and a live hasStableIdEndpoint() at the next arm())
		// returns Auto DJ to service.
		if(failAfterPending || !port.hasStableIdEndpoint()){
			failAfterPending = false;
			machine.fail(AutoDjFailReason::AuthorityUnavailable);
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
		// Defense-in-depth: tick()'s own gate above should already prevent
		// this call whenever the authority has dropped, but this must never
		// submit a load derived from a stale RAM authority once it is gone,
		// regardless of how this method is ever reached in the future.
		if(!port.hasStableIdEndpoint()){
			machine.fail(AutoDjFailReason::AuthorityUnavailable);
			return;
		}
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

	// Centralized "abandon whatever pending bookkeeping exists" reset, used
	// by reset(), by resolvePendingWhileStopping()'s own cleanup path, and
	// by tick()'s Stopping-with-nothing-pending path (finishStop()) - see
	// failAfterPending's doc comment for why this (rather than
	// progressPending()'s own Applied/ordinary-Failed resolution) is the
	// right place to clear it: those three call sites are the only ones
	// that abandon an attempt-cycle/session outright (explicit reset, the
	// stop sequence resolving an in-flight attempt, or Stopping finishing
	// with nothing in flight - which can still observe a latch left by an
	// attempt that resolved earlier via an ordinary Running tick, before
	// stop() was even requested), so a stale flag from a run that has
	// already ended can never leak into and immediately fail the next one.
	void cancelPending(){
		pendingPhase = AutoDjPendingPhase::None;
		pendingDeadlineUs = 0;
		pendingAttempts = 0;
		failAfterPending = false;
	}

	AutoDjLoadPort& port;
	DjAutoDjStateMachine machine;
	DjAutoDjQueue queue;
	DjAutoDjHistory history;
	AutoDjPendingPhase pendingPhase = AutoDjPendingPhase::None;
	AutoDjQueueEntry pendingEntry = {}; // valid whenever pendingAttempts > 0
	uint64_t pendingDeadlineUs = 0; // valid whenever pendingPhase == WaitingOutcome
	uint8_t pendingAttempts = 0;
	// Sticky "the stable-ID authority was observed unavailable while a
	// composite load/Coach-arm/transition/rollback attempt was already in
	// flight" latch. Set only from tick()'s pendingPhase-in-flight branch
	// (never while pendingPhase == None, so it can never fire mid-attempt-
	// start), and consumed by the very next tick() where pendingPhase has
	// resolved back to None - forcing an immediate terminal
	// AuthorityUnavailable there instead of letting handleLoadFailure()'s
	// ordinary retry/skip budget (or a fresh beginNextLoad()) run against
	// an authority that is already known to be gone. Cleared by
	// cancelPending() (see its doc comment) so it never survives past the
	// attempt-cycle/session boundary that observed the loss.
	bool failAfterPending = false;
};

#endif //JAYD_FIRMWARE_DJ_AUTO_DJ_PLANNER_H
