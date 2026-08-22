#ifndef JAYD_FIRMWARE_DJASSISTCONTROLLER_H
#define JAYD_FIRMWARE_DJASSISTCONTROLLER_H

#include "DjAssistEngine.h"

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
// (never a fresh file read per scoring tick), advances the scan/suggestion
// pipeline a bounded amount per DjSession::loop() tick, resolves boundary
// hints from DjSession's grid/phrase accessors only when actually needed
// (downbeats are cheap/in-memory and refreshed every tick; phrase lookups
// are a bounded real SD read and are throttled - only refreshed once the
// previously cached boundary has been passed), and drives the real
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

	// Allocates the PSRAM candidate table and binds the actuator to
	// `session`. Safe to call once, from DjSession's constructor. A failed
	// PSRAM allocation disables the candidate table (suggestions/advice
	// simply stay empty) without affecting the rest of the session.
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
	uint16_t entryTotal_ = 0;
	uint16_t scanCursor_ = 0;
	bool allocationFailed_ = false;

	// One-time (per library generation) bounded incremental fill of
	// entries_[] from DjSession::assistTrackEntry(). Kept separate from
	// scanCursor_ (the scoring pass) so scanTick() never sees `total`
	// change mid-pass - a fresh scoring pass over entries_ only begins once
	// the fill for the current generation has fully completed.
	uint32_t loadedGeneration_ = 0;
	bool generationSeen_ = false;
	uint16_t fillCursor_ = 0;
	bool fillComplete_ = false;

	DjAssistSuggestion suggestions_[DJ_ASSIST_MAX_SUGGESTIONS] = {};
	uint8_t suggestionCount_ = 0;

	DjTrackIdentity recentTracks_[DJ_ASSIST_MAX_RECENT_TRACKS] = {};
	uint8_t recentCount_ = 0;
	uint8_t recentNext_ = 0;
	DjTrackIdentity lastDeckIdentity_[DJ_DECK_COUNT] = {};
	bool lastDeckLoaded_[DJ_DECK_COUNT] = {};

	uint8_t phraseHintDeck_ = 0xFF;
	uint64_t phraseHintFrame_ = 0;
	bool phraseHintValid_ = false;

	DjAssistCoachAdvice lastAdvice_ = {};

	void refreshCandidateTable();
	void updateRecentTracks(const DjSnapshot& snapshot);
	DjAssistGuardSnapshot buildGuard(const DjSnapshot& snapshot) const;
	DjAssistDeckContext buildDeckContext(const DjSnapshot& snapshot, uint8_t deck) const;
	bool resolveBoundary(const DjSnapshot& snapshot, uint8_t deck, DjAssistBoundaryHint& hint);
	void tickSuggestions(const DjSnapshot& snapshot);
	void tickTransition(const DjSnapshot& snapshot);
};

#endif
