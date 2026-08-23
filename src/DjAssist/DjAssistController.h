#ifndef JAYD_FIRMWARE_DJASSISTCONTROLLER_H
#define JAYD_FIRMWARE_DJASSISTCONTROLLER_H

#include "DjAssistEngine.h"
#include "DjAssistSessionBridge.h"

#include <Sync/Mutex.h>
#include <Util/Task.h>

class DjSession;

// Bounded, POD, browser/API/physical-bank-ready snapshot of Coach/transition
// state. Copied out of the live engine + suggestion table on request; safe
// to copy into a wireless payload or UI cache (no pointers, no owned
// storage).
struct DjAssistSnapshot {
	DjAssistMode mode = DJ_ASSIST_MODE_OFF;
	DjAssistTransitionPlan plan = {};
	uint8_t suggestionCount = 0;
	DjAssistSuggestion suggestions[DJ_ASSIST_MAX_SUGGESTIONS] = {};
	DjAssistCoachAdvice advice = {};
};

// ESP32-coupled owner of the Coach/one-shot-transition engine. Builds the
// bounded PSRAM candidate table from DjSession's already-indexed metadata
// on a dedicated low-priority background Task (see fillWorkerStep()) so no
// SD read ever happens on the DjSession::loop()/audio thread; advances the
// scan/suggestion pipeline a bounded amount per DjSession::loop() tick over
// the already-filled table, resolves boundary hints from DjSession's
// grid/phrase accessors only when actually needed (downbeats are cheap/
// in-memory and refreshed every tick; phrase lookups are a bounded real SD
// read and are throttled by a terminal-aware cache - see
// DjAssistBridge::DjAssistPhraseCacheState), and drives the real
// Sync/quantize/mix primitives through a concrete DjAssistActuator.
//
// Deliberately validated only by the real firmware build, matching
// DjSession.cpp's own precedent (never host-compiled) - the pure arithmetic
// it calls into (DjAssistSessionBridge, DjAssistScoring, DjAssistEngine) is
// separately host-tested with ASan/UBSan.
class DjAssistController {
public:
	DjAssistController();
	~DjAssistController();

	DjAssistController(const DjAssistController&) = delete;
	DjAssistController& operator=(const DjAssistController&) = delete;

	// Allocates the PSRAM candidate table, binds the actuator to `session`,
	// and starts the background fill task. Safe to call once, from
	// DjSession's constructor. A failed PSRAM allocation disables the
	// candidate table (suggestions/advice simply stay empty, and the fill
	// task is never started) without affecting the rest of the session.
	void begin(DjSession* session);
	void end();

	// Bounded per-loop-iteration work: advances the candidate-table fill
	// and/or scoring scan, recomputes Coach advice, and ticks the
	// transition state machine. Never blocks, never reads a file on every
	// call (see phrase-boundary throttling above), never touches the audio
	// path directly (only through DjSession's existing command surface).
	void tick();

	bool setCoachEnabled(bool enabled);

	// This call *is* the explicit user confirmation (mirrors
	// DjAssistEngine::armTransition()); builds the guard snapshot itself
	// from DjSession's current published state.
	bool armTransition(
		uint8_t fromDeck,
		uint8_t toDeck,
		uint32_t libraryIndex,
		const DjTrackIdentity& targetIdentity,
		uint8_t crossfadeBeats,
		bool startAtBoundary,
		bool tempoLock
	);
	void cancelTransition();

	void copySnapshot(DjAssistSnapshot& snapshot) const;

private:
	DjSession* session_ = nullptr;
	DjAssistEngine engine_;
	DjAssistActuator* actuator_ = nullptr;

	DjAssistLibraryEntry* entries_ = nullptr;
	uint16_t entryCapacity_ = 0;
	bool allocationFailed_ = false;

	// Candidate-table fill state below this point is owned by the
	// background fillTask_ (see fillWorkerStep()) and must only be touched
	// while holding candidateMutex_ - this is what keeps the real,
	// possibly SD-backed metadata read (assistTrackEntry()) off the main
	// DjSession::loop() thread entirely. entries_[]/entryTotal_ are read
	// by the main thread too (tickSuggestions()'s scanTick() call), always
	// under the same lock.
	Mutex candidateMutex_;
	Task* fillTask_ = nullptr;
	uint16_t entryTotal_ = 0;
	uint32_t loadedGeneration_ = 0;
	bool generationSeen_ = false;
	uint16_t fillCursor_ = 0;
	bool fillComplete_ = false;

	// Main-thread-only mirror of the fill generation, used solely to
	// notice (once per tick, via a single locked read) when a NEW
	// generation's fill has completed so scanCursor_/suggestionCount_ are
	// reset for the new table - never written by the background task.
	uint32_t scanGeneration_ = 0;
	bool scanGenerationSeen_ = false;
	uint16_t scanCursor_ = 0;

	DjAssistSuggestion suggestions_[DJ_ASSIST_MAX_SUGGESTIONS] = {};
	uint8_t suggestionCount_ = 0;

	DjTrackIdentity recentTracks_[DJ_ASSIST_MAX_RECENT_TRACKS] = {};
	uint8_t recentCount_ = 0;
	uint8_t recentNext_ = 0;
	DjTrackIdentity lastDeckIdentity_[DJ_DECK_COUNT] = {};
	bool lastDeckLoaded_[DJ_DECK_COUNT] = {};

	// Main-thread-only (Coach advice and the transition's
	// resolveWaitBoundaryReached() are mutually exclusive per mode, and
	// both only ever run from tick(), never the background fill task) -
	// see DjAssistBridge::DjAssistPhraseCacheState.
	DjAssistBridge::DjAssistPhraseCacheState phraseCache_;

	// WAIT_BOUNDARY capture-once state: the ONE target frame latched the
	// first tick this step becomes current, compared against the live
	// playhead every subsequent tick (see resolveWaitBoundaryReached()).
	// Reset whenever a new transition is armed/cancelled so a later
	// transition never reuses a stale target from a previous one.
	bool waitBoundaryCaptured_ = false;
	uint64_t waitBoundaryTargetFrame_ = 0;

	// Controller-owned rollback state (see tickRollback()): advances one
	// bounded actuator submit/poll per tick, exactly mirroring the forward
	// step machine, restoring only side effects this plan itself applied.
	DjAssistBridge::DjAssistRollbackPhase rollbackPhase_ = DjAssistBridge::DJ_ASSIST_ROLLBACK_IDLE;
	bool rollbackSubmitted_ = false;
	uint32_t rollbackCommandId_ = 0;
	// True once assistPurgePendingSystemCommands() has been called for
	// this failure/cancel episode - set the first time tickRollback()
	// runs after a transition enters DJ_ASSIST_MODE_TRANSITION_FAILED,
	// reset alongside rollbackPhase_ whenever a fresh transition arms.
	// Ensures a cancelled/failed transition's own still-queued
	// mix/play/sync commands are purged exactly once, before rollback
	// starts restoring state, so they can never re-appear afterward.
	bool pendingPurgeDone_ = false;

	DjAssistCoachAdvice lastAdvice_ = {};

	static void fillTaskTrampoline(Task* task);
	void fillWorkerStep();
	bool candidateTableReady(uint32_t& outGeneration);
	void updateRecentTracks(const DjSnapshot& snapshot);
	DjAssistGuardSnapshot buildGuard(const DjSnapshot& snapshot) const;
	DjAssistDeckContext buildDeckContext(const DjSnapshot& snapshot, uint8_t deck) const;
	bool resolveBoundary(const DjSnapshot& snapshot, uint8_t deck, DjAssistBoundaryHint& hint);
	bool resolveWaitBoundaryReached(const DjSnapshot& snapshot, uint8_t deck);
	void tickSuggestions(const DjSnapshot& snapshot);
	void tickTransition(const DjSnapshot& snapshot);
	void tickRollback();
};

#endif
