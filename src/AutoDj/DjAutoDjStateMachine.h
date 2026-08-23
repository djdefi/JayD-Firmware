#ifndef JAYD_FIRMWARE_DJ_AUTO_DJ_STATE_MACHINE_H
#define JAYD_FIRMWARE_DJ_AUTO_DJ_STATE_MACHINE_H

#include "DjAutoDjTypes.h"

// Explicit Auto DJ state graph:
//
//   Off --arm()--> Armed --start()--> Running
//   Armed --arm() w/o confirmation--> Off (stays)
//   Running --pause()--> Paused --resume()--> Running
//   Running --complete()--> Complete        (queue drained naturally)
//   {Armed,Running,Paused} --stop()--> Stopping --finishStop()--> Off
//   any non-terminal --fail()--> Failed
//   {Failed,Complete} --reset()--> Off
//
// This class only tracks the state graph; it has no knowledge of the queue,
// history, or any I/O port. Every method call performs at most one
// transition and returns whether it applied, so callers can never observe a
// half-applied change and can never loop forever inside a single call.
class DjAutoDjStateMachine {
public:
	AutoDjState state() const{
		return current;
	}

	AutoDjFailReason failReason() const{
		return failReason_;
	}

	// Arming requires both a stable-ID load capability (a hard requirement -
	// there is no path to Running without it) and an explicit physical or
	// authenticated confirmation. Missing capability is terminal (Failed);
	// missing confirmation simply keeps Auto DJ Off so the caller can retry
	// once the user confirms.
	bool arm(bool capabilityAvailable, bool physicalConfirmed){
		if(current != AutoDjState::Off) return false;
		if(!capabilityAvailable){
			current = AutoDjState::Failed;
			failReason_ = AutoDjFailReason::CapabilityDisabled;
			return false;
		}
		if(!physicalConfirmed) return false;
		current = AutoDjState::Armed;
		failReason_ = AutoDjFailReason::None;
		return true;
	}

	// Only transitions to Running when a safe window exists (queue
	// non-empty, capability present, no manual takeover in progress).
	// Otherwise remains Armed - it never guesses.
	bool start(bool hasSafeWindow){
		if(current != AutoDjState::Armed) return false;
		if(!hasSafeWindow) return false;
		current = AutoDjState::Running;
		return true;
	}

	// Any deterministic reason to stop autonomous progress without ending
	// the session outright: manual takeover of transport/crossfader/rate/
	// load/cue/loop, or a safety condition (missing trustworthy duration,
	// recording failure) that requires the user to decide what happens next.
	bool pause(){
		if(current != AutoDjState::Running) return false;
		current = AutoDjState::Paused;
		return true;
	}

	bool resume(bool physicalConfirmed){
		if(current != AutoDjState::Paused) return false;
		if(!physicalConfirmed) return false;
		current = AutoDjState::Running;
		return true;
	}

	// Only Armed/Running/Paused may transition to Stopping (see the state
	// graph above). Off/Stopping/Complete are correctly excluded by not
	// being in that list, and so is Failed: Failed is a terminal state
	// that must go through reset() (which itself requires
	// Coach rollback/teardown to have settled) before Auto DJ can run
	// again - stop() must never be usable to escape Failed back to Off
	// without that settled-rollback guarantee.
	bool stop(){
		if(current != AutoDjState::Armed && current != AutoDjState::Running && current != AutoDjState::Paused){
			return false;
		}
		current = AutoDjState::Stopping;
		return true;
	}

	// Always resolves to Off. Callers are expected to let any in-flight
	// command settle (via the load port) before calling this, so a command
	// already accepted by hardware is never silently abandoned.
	bool finishStop(){
		if(current != AutoDjState::Stopping) return false;
		current = AutoDjState::Off;
		return true;
	}

	bool complete(){
		if(current != AutoDjState::Running) return false;
		current = AutoDjState::Complete;
		return true;
	}

	bool fail(AutoDjFailReason reason){
		if(current == AutoDjState::Off || current == AutoDjState::Complete || current == AutoDjState::Failed){
			return false;
		}
		current = AutoDjState::Failed;
		failReason_ = reason;
		return true;
	}

	// Failed and Complete are terminal until explicitly acknowledged.
	bool reset(){
		if(current != AutoDjState::Failed && current != AutoDjState::Complete) return false;
		current = AutoDjState::Off;
		failReason_ = AutoDjFailReason::None;
		return true;
	}

private:
	AutoDjState current = AutoDjState::Off;
	AutoDjFailReason failReason_ = AutoDjFailReason::None;
};

#endif //JAYD_FIRMWARE_DJ_AUTO_DJ_STATE_MACHINE_H
