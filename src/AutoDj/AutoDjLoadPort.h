#ifndef JAYD_FIRMWARE_DJ_AUTO_DJ_LOAD_PORT_H
#define JAYD_FIRMWARE_DJ_AUTO_DJ_LOAD_PORT_H

#include "DjAutoDjTypes.h"

enum class AutoDjLoadOutcome : uint8_t {
	Pending,  // submitted, no result yet
	Accepted, // acknowledged but not yet applied
	Applied,  // the deck is now playing this identity
	Failed    // rejected or the load attempt errored out
};

// Seam between the planner and the rest of the firmware. Until the final
// Coach/DjSession/browser-control interfaces are rebased in, this is
// implemented only by a test double; production wiring plugs in an adapter
// over the authoritative DjSession command queue + physical/browser control
// state once that lands. Runtime playback control must only ever happen
// through this one-shot submit/poll pair and a stable-ID `AutoDjIdentity` -
// never a raw path.
class AutoDjLoadPort {
public:
	virtual ~AutoDjLoadPort() = default;

	// Checked once at arm() time. If the firmware build has no stable-ID
	// load endpoint at all, Auto DJ must never be armable.
	virtual bool hasStableIdEndpoint() const = 0;

	// Non-blocking. At most one load may be in flight; the planner never
	// calls this again before the previous one resolves via pollLoad().
	virtual bool submitLoad(const AutoDjIdentity& identity) = 0;

	virtual AutoDjLoadOutcome pollLoad() = 0;

	// True while the user is actively driving transport, crossfader, rate,
	// load, cue, or loop controls directly - Auto DJ must yield immediately
	// and deterministically rather than fight the user for control.
	virtual bool manualTakeoverActive() const = 0;

	// True only when the remaining time on the current deck is known with
	// confidence (e.g. real decoded duration + timing source available).
	// The planner is not allowed to guess a crossfade point otherwise.
	virtual bool currentDurationTrustworthy() const = 0;

	virtual bool currentTrackAtEnd() const = 0;

	// True if recording is active and has failed. Auto DJ never starts or
	// stops a recording itself; a failure here only ever causes a pause so
	// the user can decide what to do.
	virtual bool recordingFailed() const = 0;

	// Physical or authenticated confirmation gate for arm()/resume().
	virtual bool physicalConfirmationPresent() const = 0;
};

#endif //JAYD_FIRMWARE_DJ_AUTO_DJ_LOAD_PORT_H
