#ifndef JAYD_FIRMWARE_DJSESSIONSTATE_H
#define JAYD_FIRMWARE_DJSESSIONSTATE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "DjBeatEngine.h"

static constexpr uint8_t DJ_DECK_COUNT = 2;
static constexpr uint8_t DJ_EFFECT_SLOT_COUNT = 3;
static constexpr uint8_t DJ_CUE_COUNT = 8;
static constexpr uint8_t DJ_COMMAND_CAPACITY = 16;
#if defined(JAYD_ENABLE_WIRELESS)
static constexpr uint8_t DJ_RECENT_RESULT_COUNT = DJ_COMMAND_CAPACITY + 8;
#else
static constexpr uint8_t DJ_RECENT_RESULT_COUNT = 8;
#endif
static constexpr size_t DJ_PATH_CAPACITY = 128;

enum DjMetadataState : uint8_t {
	DJ_METADATA_ABSENT,
	DJ_METADATA_VALID,
	DJ_METADATA_STALE,
	DJ_METADATA_CORRUPT,
	DJ_METADATA_UNSUPPORTED
};

enum DjMetadataCapability : uint16_t {
	DJ_METADATA_HAS_SOURCE_FRAMES = 1 << 0,
	DJ_METADATA_HAS_BPM = 1 << 1,
	DJ_METADATA_HAS_KEY = 1 << 2,
	DJ_METADATA_HAS_RATING = 1 << 3,
	DJ_METADATA_HAS_CUES = 1 << 4,
	DJ_METADATA_HAS_GRID = 1 << 5,
	DJ_METADATA_HAS_DOWNBEATS = 1 << 6,
	DJ_METADATA_HAS_PHRASES = 1 << 7
};

enum DjTrackIdentityFlag : uint8_t {
	DJ_TRACK_IDENTITY_FINGERPRINT = 1 << 0,
	DJ_TRACK_IDENTITY_SOURCE = 1 << 1
};

struct DjTrackIdentity {
	uint8_t flags = 0;
	uint8_t fingerprint[16] = {};
	uint8_t sourceId[16] = {};
};

struct DjTrackMetadataSnapshot {
	DjMetadataState state = DJ_METADATA_ABSENT;
	uint16_t capabilities = 0;
	uint32_t libraryGeneration = 0;
	uint32_t provenanceHash = 0;
	uint16_t confidence = 0;
	uint32_t sourceSampleRate = 0;
	uint64_t sourceDurationFrames = 0;
	uint32_t bpmMilli = 0;
	uint16_t key = 0;
	uint8_t rating = 255;
	uint16_t cueCount = 0;
	uint16_t gridCount = 0;
	uint16_t downbeatCount = 0;
	uint16_t phraseCount = 0;
};

enum DjCommandOrigin : uint8_t {
	DJ_ORIGIN_LOCAL_UI,
	DJ_ORIGIN_PHYSICAL,
	DJ_ORIGIN_HTTP,
	DJ_ORIGIN_SYSTEM
};

enum DjCommandType : uint8_t {
	DJ_COMMAND_LOAD_DECK,
	DJ_COMMAND_SET_PLAYING,
	DJ_COMMAND_SEEK,
	DJ_COMMAND_SET_GAIN,
	DJ_COMMAND_SET_MIX,
	DJ_COMMAND_SET_EFFECT_TYPE,
	DJ_COMMAND_SET_EFFECT_INTENSITY,
	DJ_COMMAND_SET_RECORDING,
	DJ_COMMAND_SET_QUANTIZE,
	DJ_COMMAND_LOOP_ENGAGE,
	DJ_COMMAND_LOOP_DISENGAGE,
	DJ_COMMAND_LOOP_RELOOP,
	DJ_COMMAND_SET_SYNC,
	DJ_COMMAND_SET_CUE,
	DJ_COMMAND_TRIGGER_CUE,
	DJ_COMMAND_CLEAR_CUE
#if defined(JAYD_ENABLE_WIRELESS)
	,DJ_COMMAND_OPEN_PAIRING
#endif
};

enum DjCommandStatus : uint8_t {
	DJ_COMMAND_ACCEPTED,
	DJ_COMMAND_APPLIED,
	DJ_COMMAND_FAILED,
	DJ_COMMAND_SUPERSEDED,
	DJ_COMMAND_REJECTED,
	DJ_COMMAND_PENDING
};

enum DjCommandError : uint8_t {
	DJ_COMMAND_ERROR_NONE,
	DJ_COMMAND_ERROR_QUEUE_FULL,
	DJ_COMMAND_ERROR_INVALID_DECK,
	DJ_COMMAND_ERROR_INVALID_SLOT,
	DJ_COMMAND_ERROR_INVALID_VALUE,
	DJ_COMMAND_ERROR_INVALID_PATH,
	DJ_COMMAND_ERROR_NO_DECK,
	DJ_COMMAND_ERROR_NO_EFFECT,
	DJ_COMMAND_ERROR_OPEN_FAILED,
	DJ_COMMAND_ERROR_RECORDING_FAILED,
	DJ_COMMAND_ERROR_SESSION_ENDING,
	DJ_COMMAND_ERROR_NO_GRID,
	DJ_COMMAND_ERROR_LOOP_OUT_OF_RANGE,
	DJ_COMMAND_ERROR_LOOP_BUSY,
	DJ_COMMAND_ERROR_SYNC_UNAVAILABLE,
	DJ_COMMAND_ERROR_SYNC_CONFLICT,
	DJ_COMMAND_ERROR_INVALID_MASTER,
	DJ_COMMAND_ERROR_EMPTY_CUE,
	DJ_COMMAND_ERROR_RECORDING_ACTIVE,
	DJ_COMMAND_ERROR_RECORDING_BUSY
#if defined(JAYD_ENABLE_WIRELESS)
	,DJ_COMMAND_ERROR_STALE_IDENTITY,
	DJ_COMMAND_ERROR_CLIENT_ID_REQUIRED
#endif
};

enum DjTimingQuality : uint8_t {
	DJ_TIMING_UNAVAILABLE,
	DJ_TIMING_COARSE
};

enum DjEffectType : uint8_t {
	DJ_EFFECT_NONE,
	DJ_EFFECT_SPEED,
	DJ_EFFECT_LOWPASS,
	DJ_EFFECT_HIGHPASS,
	DJ_EFFECT_REVERB,
	DJ_EFFECT_BITCRUSHER,
	DJ_EFFECT_COUNT
};

// Recording-busy gate shared by DjSession::validate() and the host
// self-check: only a new *start* is rejected while a previous start/stop is
// still in flight (STARTING/ACTIVE/STOPPING). A stop is always allowed
// through, even while STARTING, so a stop issued before the library applies
// an accepted start is forwarded rather than silently rejected -- the
// library's own recording state machine is designed to accept a stop during
// STARTING and transition safely to STOPPING.
enum DjRecordingState : uint8_t {
	DJ_RECORDING_IDLE,
	DJ_RECORDING_STARTING,
	DJ_RECORDING_ACTIVE,
	DJ_RECORDING_STOPPING,
	DJ_RECORDING_COMPLETE,
	DJ_RECORDING_FAILED
};

enum DjRecordingError : uint8_t {
	DJ_RECORDING_ERROR_NONE,
	DJ_RECORDING_ERROR_SD_UNAVAILABLE,
	DJ_RECORDING_ERROR_OPEN_FAILED,
	DJ_RECORDING_ERROR_WRITE_FAILED,
	DJ_RECORDING_ERROR_FINALIZE_FAILED,
	DJ_RECORDING_ERROR_BUFFER_OVERRUN,
	DJ_RECORDING_ERROR_QUEUE_FULL,
	DJ_RECORDING_ERROR_NAME_EXHAUSTED,
	DJ_RECORDING_ERROR_RENAME_FAILED
};

inline bool djRecordingStartBusy(bool isStartCommand, DjRecordingState state){
	return isStartCommand &&
		   (state == DJ_RECORDING_STARTING || state == DJ_RECORDING_ACTIVE || state == DJ_RECORDING_STOPPING);
}

struct DjCommand {
	uint32_t id = 0;
	DjCommandOrigin origin = DJ_ORIGIN_SYSTEM;
	DjCommandType type = DJ_COMMAND_SET_PLAYING;
	uint8_t deck = 0;
	uint8_t slot = 0;
	uint16_t value = 0;
	uint32_t libraryGeneration = 0;
	uint64_t libraryKey = 0;
	DjTrackIdentity trackIdentity = {};
	char path[DJ_PATH_CAPACITY] = {};
#if defined(JAYD_ENABLE_WIRELESS)
	uint64_t requestBootId = 0;
	uint32_t requestSessionId = 0;
	char clientId[33] = {};
	char clientCommandId[33] = {};
#endif
};

struct DjSubmitResult {
	uint32_t id = 0;
	DjCommandStatus status = DJ_COMMAND_REJECTED;
	DjCommandError error = DJ_COMMAND_ERROR_NONE;
#if defined(JAYD_ENABLE_WIRELESS)
	bool duplicate = false;
#endif

	DjSubmitResult() = default;
	DjSubmitResult(
		uint32_t id,
		DjCommandStatus status,
		DjCommandError error
#if defined(JAYD_ENABLE_WIRELESS)
		,bool duplicate = false
#endif
	) : id(id), status(status), error(error)
#if defined(JAYD_ENABLE_WIRELESS)
		,duplicate(duplicate)
#endif
	{}

	bool accepted() const{
		return status == DJ_COMMAND_ACCEPTED;
	}
};

struct DjCommandResult {
	uint32_t id = 0;
	DjCommandOrigin origin = DJ_ORIGIN_SYSTEM;
	DjCommandType type = DJ_COMMAND_SET_PLAYING;
	DjCommandStatus status = DJ_COMMAND_REJECTED;
	DjCommandError error = DJ_COMMAND_ERROR_NONE;
	// Diagnostics for quantized/scheduled actions (play-start, loop
	// engage/reloop): the source frame the action targeted, how many frames
	// late it actually applied (0 if on time or not yet resolved), and
	// whether the original boundary was missed and the action fell back to
	// the next one. Zero/false for command types that do not schedule.
	uint64_t targetFrame = 0;
	int32_t lateFrames = 0;
	bool missed = false;
#if defined(JAYD_ENABLE_WIRELESS)
	char clientId[33] = {};
	char clientCommandId[33] = {};
	uint64_t sequence = 0;
#endif
};

struct DjEffectSnapshot {
	uint8_t type = 0;
	uint8_t intensity = 0;
};

struct DjEffectTransition {
	bool addSpeed = false;
	bool removeSpeed = false;
	bool setSpeed = false;
	bool clearEffect = false;
};

class DjEffectState {
public:
	bool setType(uint8_t deck, uint8_t slot, uint8_t type, bool deckLoaded, DjEffectTransition& transition){
		if(deck >= DJ_DECK_COUNT || slot >= DJ_EFFECT_SLOT_COUNT || type >= DJ_EFFECT_COUNT) return false;

		DjEffectSnapshot& effect = effects[deck][slot];
		if(type == DJ_EFFECT_SPEED){
			for(uint8_t other = 0; other < DJ_EFFECT_SLOT_COUNT; other++){
				if(other == slot || effects[deck][other].type != DJ_EFFECT_SPEED) continue;
				effects[deck][other] = {};
			}
			effect.type = type;
			effect.intensity = 127;
			transition.clearEffect = true;
			if(deckLoaded && !speedActive[deck]){
				speedActive[deck] = true;
				transition.addSpeed = true;
			}
			transition.setSpeed = speedActive[deck];
			return true;
		}

		if(effect.type == DJ_EFFECT_SPEED && speedActive[deck]){
			speedActive[deck] = false;
			transition.removeSpeed = true;
		}
		effect.type = type;
		effect.intensity = 0;
		return true;
	}

	bool setIntensity(uint8_t deck, uint8_t slot, uint8_t intensity, DjEffectTransition& transition){
		if(deck >= DJ_DECK_COUNT || slot >= DJ_EFFECT_SLOT_COUNT ||
		   effects[deck][slot].type == DJ_EFFECT_NONE) return false;
		effects[deck][slot].intensity = intensity;
		transition.setSpeed = effects[deck][slot].type == DJ_EFFECT_SPEED && speedActive[deck];
		return true;
	}

	void deckLoaded(uint8_t deck, DjEffectTransition& transition){
		if(deck >= DJ_DECK_COUNT || speedActive[deck]) return;
		for(uint8_t slot = 0; slot < DJ_EFFECT_SLOT_COUNT; slot++){
			if(effects[deck][slot].type != DJ_EFFECT_SPEED) continue;
			speedActive[deck] = true;
			transition.addSpeed = true;
			transition.setSpeed = true;
			return;
		}
	}

	const DjEffectSnapshot& get(uint8_t deck, uint8_t slot) const{
		return effects[deck][slot];
	}

	const DjEffectSnapshot* getDeck(uint8_t deck) const{
		return effects[deck];
	}

	void copyDeck(uint8_t deck, DjEffectSnapshot* destination) const{
		memcpy(destination, effects[deck], sizeof(effects[deck]));
	}

	bool isSpeedActive(uint8_t deck) const{
		return deck < DJ_DECK_COUNT && speedActive[deck];
	}

private:
	DjEffectSnapshot effects[DJ_DECK_COUNT][DJ_EFFECT_SLOT_COUNT] = {};
	bool speedActive[DJ_DECK_COUNT] = {};
};

struct DjGridSnapshot {
	bool valid = false;
	uint16_t confidence = 0;
	int64_t currentQuarterBeat = 0;
};

struct DjQuantizeSnapshot {
	DjQuantizeResolution resolution = DJ_QUANTIZE_OFF;
	bool pending = false;
	uint64_t pendingTargetFrame = 0;
};

struct DjSyncSnapshot {
	DjSyncState state = DJ_SYNC_OFF;
	int8_t masterDeck = -1; // -1 = auto (the other deck) or unset
	DjRate targetRate = DJ_RATE_NEUTRAL;
	DjCommandError lastError = DJ_COMMAND_ERROR_NONE;
};

struct DjCueSnapshot {
	bool occupied = false;
	uint16_t position = 0;
};

struct DjDeckSnapshot {
	bool loaded = false;
	bool playing = false;
	uint16_t elapsed = 0;
	uint16_t duration = 0;
	DjTimingQuality timingQuality = DJ_TIMING_UNAVAILABLE;
	uint8_t gain = 255;
	char path[DJ_PATH_CAPACITY] = {};
	DjEffectSnapshot effects[DJ_EFFECT_SLOT_COUNT] = {};
	DjCueSnapshot cues[DJ_CUE_COUNT] = {};
	DjTrackMetadataSnapshot metadata = {};
	DjGridSnapshot grid = {};
	DjQuantizeSnapshot quantize = {};
	DjLoopSnapshot loop = {};
	DjSyncSnapshot sync = {};
};

// Authoritative recording lifecycle snapshot: accepted (STARTING/STOPPING)
// vs applied (ACTIVE/COMPLETE/FAILED) state, stable error mapping, validity,
// byte/duration counters, the finalized file path once available, and
// boot-time orphan recovery diagnostics.
struct DjRecordingSnapshot {
	DjRecordingState state = DJ_RECORDING_IDLE;
	DjRecordingError error = DJ_RECORDING_ERROR_NONE;
	bool valid = false;
	uint32_t bytes = 0;
	uint32_t durationMs = 0;
	char path[DJ_PATH_CAPACITY] = {};
	uint32_t orphansRepaired = 0;
	uint32_t orphansFailed = 0;
};

struct DjSnapshot {
	uint64_t seq = 0;
	uint64_t bootId = 0;
	uint32_t sessionId = 0;
	bool sessionActive = false;
	bool mixerRunning = false;
	uint8_t mix = 127;
	DjRecordingSnapshot recordingInfo;
	DjDeckSnapshot decks[DJ_DECK_COUNT] = {};
	uint8_t queueDepth = 0;
	uint32_t queueDrops = 0;
#if defined(JAYD_ENABLE_WIRELESS)
	uint32_t pairingGeneration = 0;
#endif
	DjCommandResult recentResults[DJ_RECENT_RESULT_COUNT] = {};
};

// Recording is considered "busy" for library-work purposes across the same
// STARTING/ACTIVE/STOPPING span that djRecordingStartBusy() gates for new
// start commands; COMPLETE/FAILED/IDLE do not block library work.
inline bool djAllowsLibraryWork(const DjSnapshot& snapshot){
	if(djRecordingStartBusy(true, snapshot.recordingInfo.state)) return false;
	for(uint8_t deck = 0; deck < DJ_DECK_COUNT; ++deck){
		if(snapshot.decks[deck].playing) return false;
	}
	return true;
}

class DjDeckMetadataState {
public:
	bool commitIfLoaded(
		const DjTrackMetadataSnapshot& metadata,
		bool attached,
		bool loadSucceeded
	){
		if(!loadSucceeded) return false;
		snapshot_ = metadata;
		attached_ = attached && metadata.state == DJ_METADATA_VALID;
		return true;
	}

	void invalidate(DjMetadataState state, uint32_t generation){
		snapshot_ = {};
		snapshot_.state = state;
		snapshot_.libraryGeneration = generation;
		attached_ = false;
	}

	bool attached() const{
		return attached_;
	}

	const DjTrackMetadataSnapshot& snapshot() const{
		return snapshot_;
	}

private:
	DjTrackMetadataSnapshot snapshot_ = {};
	bool attached_ = false;
};

class DjCueState {
public:
	bool set(uint8_t deck, uint8_t cue, uint16_t position){
		if(deck >= DJ_DECK_COUNT || cue >= DJ_CUE_COUNT) return false;
		cues[deck][cue].occupied = true;
		cues[deck][cue].position = position;
		return true;
	}

	bool trigger(uint8_t deck, uint8_t cue, uint16_t& position) const{
		if(deck >= DJ_DECK_COUNT || cue >= DJ_CUE_COUNT || !cues[deck][cue].occupied) return false;
		position = cues[deck][cue].position;
		return true;
	}

	bool clear(uint8_t deck, uint8_t cue){
		if(deck >= DJ_DECK_COUNT || cue >= DJ_CUE_COUNT) return false;
		cues[deck][cue] = {};
		return true;
	}

	void clearDeck(uint8_t deck){
		if(deck >= DJ_DECK_COUNT) return;
		memset(cues[deck], 0, sizeof(cues[deck]));
	}

	void copyDeck(uint8_t deck, DjCueSnapshot* destination) const{
		if(deck >= DJ_DECK_COUNT || !destination) return;
		memcpy(destination, cues[deck], sizeof(cues[deck]));
	}

private:
	DjCueSnapshot cues[DJ_DECK_COUNT][DJ_CUE_COUNT] = {};
};

#if defined(JAYD_ENABLE_WIRELESS)
inline bool djCommandIdentityMatches(const DjCommand& command, uint64_t bootId, uint32_t sessionId){
	return command.origin != DJ_ORIGIN_HTTP ||
		(command.requestBootId == bootId && command.requestSessionId == sessionId);
}
#endif

class DjCommandQueue {
public:
	bool push(const DjCommand& command){
		if(count == DJ_COMMAND_CAPACITY) return false;
		commands[tail] = command;
		tail = (tail + 1) % DJ_COMMAND_CAPACITY;
		count++;
		return true;
	}

	bool pop(DjCommand& command){
		if(count == 0) return false;
		command = commands[head];
		head = (head + 1) % DJ_COMMAND_CAPACITY;
		count--;
		return true;
	}

	bool supersede(const DjCommand& command, uint32_t& supersededId){
		if(count == 0 || !isSupersedable(command.type)) return false;
		const uint8_t index = (tail + DJ_COMMAND_CAPACITY - 1) % DJ_COMMAND_CAPACITY;
		if(!sameTarget(commands[index], command)) return false;
		supersededId = commands[index].id;
		commands[index] = command;
		return true;
	}

	void clear(){
		head = 0;
		tail = 0;
		count = 0;
	}

	uint8_t depth() const{
		return count;
	}

	bool contains(DjCommandType type) const{
		for(uint8_t offset = 0; offset < count; offset++){
			if(commands[(head + offset) % DJ_COMMAND_CAPACITY].type == type) return true;
		}
		return false;
	}

private:
	static bool isSupersedable(DjCommandType type){
		return type == DJ_COMMAND_SET_GAIN ||
			   type == DJ_COMMAND_SET_MIX ||
			   type == DJ_COMMAND_SET_EFFECT_TYPE ||
			   type == DJ_COMMAND_SET_EFFECT_INTENSITY ||
			   type == DJ_COMMAND_SET_QUANTIZE ||
			   type == DJ_COMMAND_SET_SYNC ||
			   type == DJ_COMMAND_SET_CUE ||
			   type == DJ_COMMAND_CLEAR_CUE;
	}

	static bool sameTarget(const DjCommand& first, const DjCommand& second){
		if(first.type != second.type) return false;
		if(first.type == DJ_COMMAND_SET_MIX) return true;
		if(first.deck != second.deck) return false;
		if(first.type == DJ_COMMAND_SET_EFFECT_TYPE ||
		   first.type == DJ_COMMAND_SET_EFFECT_INTENSITY ||
		   first.type == DJ_COMMAND_SET_CUE ||
		   first.type == DJ_COMMAND_CLEAR_CUE){
			return first.slot == second.slot;
		}
		return true;
	}

	DjCommand commands[DJ_COMMAND_CAPACITY] = {};
	uint8_t head = 0;
	uint8_t tail = 0;
	uint8_t count = 0;
};

class DjCommandResults {
public:
	void record(const DjCommand& command, DjCommandStatus status, DjCommandError error){
#if defined(JAYD_ENABLE_WIRELESS)
		uint8_t selected = next;
		for(uint8_t offset = 0; offset < DJ_RECENT_RESULT_COUNT; offset++){
			const uint8_t candidate = (next + offset) % DJ_RECENT_RESULT_COUNT;
			if(results[candidate].id == 0 || results[candidate].status != DJ_COMMAND_ACCEPTED){
				selected = candidate;
				break;
			}
		}
		DjCommandResult& result = results[selected];
#else
		DjCommandResult& result = results[next];
#endif
		result.id = command.id;
		result.origin = command.origin;
		result.type = command.type;
		result.status = status;
		result.error = error;
#if defined(JAYD_ENABLE_WIRELESS)
		memcpy(result.clientId, command.clientId, sizeof(result.clientId));
		memcpy(result.clientCommandId, command.clientCommandId, sizeof(result.clientCommandId));
		result.sequence = ++nextSequence;
		if(result.sequence == 0) result.sequence = ++nextSequence;
#endif
#if defined(JAYD_ENABLE_WIRELESS)
		next = (selected + 1) % DJ_RECENT_RESULT_COUNT;
#else
		next = (next + 1) % DJ_RECENT_RESULT_COUNT;
#endif
	}

	void finish(uint32_t id, DjCommandStatus status, DjCommandError error){
		for(auto& result : results){
			if(result.id != id) continue;
			result.status = status;
			result.error = error;
			return;
		}
	}

	// Overload for commands with scheduling diagnostics (quantized play-start,
	// loop engage/reloop). Leaves id/origin/type untouched; only updates the
	// status/error/diagnostics fields, mirroring finish() above.
	void finishWithDiagnostics(uint32_t id, DjCommandStatus status, DjCommandError error,
							   uint64_t targetFrame, int32_t lateFrames, bool missed){
		for(auto& result : results){
			if(result.id != id) continue;
			result.status = status;
			result.error = error;
			result.targetFrame = targetFrame;
			result.lateFrames = lateFrames;
			result.missed = missed;
			return;
		}
	}

	void copyTo(DjCommandResult* destination) const{
#if defined(JAYD_ENABLE_WIRELESS)
		bool copied[DJ_RECENT_RESULT_COUNT] = {};
		for(uint8_t output = 0; output < DJ_RECENT_RESULT_COUNT; output++){
			bool found = false;
			uint8_t selected = 0;
			for(uint8_t candidate = 0; candidate < DJ_RECENT_RESULT_COUNT; candidate++){
				if(copied[candidate] || results[candidate].sequence == 0) continue;
				if(!found || results[candidate].sequence > results[selected].sequence){
					selected = candidate;
					found = true;
				}
			}
			destination[output] = found ? results[selected] : DjCommandResult{};
			if(found) copied[selected] = true;
		}
#else
		for(uint8_t i = 0; i < DJ_RECENT_RESULT_COUNT; i++){
			const uint8_t index = (next + DJ_RECENT_RESULT_COUNT - 1 - i) % DJ_RECENT_RESULT_COUNT;
			destination[i] = results[index];
		}
#endif
	}

#if defined(JAYD_ENABLE_WIRELESS)
	bool findClientCommand(const char* clientId, const char* clientCommandId, DjCommandResult& destination) const{
		if(!clientId || clientId[0] == '\0' || !clientCommandId || clientCommandId[0] == '\0') return false;
		for(uint8_t i = 0; i < DJ_RECENT_RESULT_COUNT; i++){
			const DjCommandResult& result = results[i];
			if(strcmp(result.clientId, clientId) != 0) continue;
			if(strcmp(result.clientCommandId, clientCommandId) != 0) continue;
			destination = result;
			return true;
		}
		return false;
	}
#endif

private:
	DjCommandResult results[DJ_RECENT_RESULT_COUNT] = {};
	uint8_t next = 0;
#if defined(JAYD_ENABLE_WIRELESS)
	uint64_t nextSequence = 0;
#endif
};

class DjSnapshotBuffers {
public:
	void publish(const DjSnapshot& snapshot){
		const uint8_t next = active == 0 ? 1 : 0;
		snapshots[next] = snapshot;
		active = next;
	}

	void copy(DjSnapshot& snapshot) const{
		snapshot = snapshots[active];
	}

private:
	DjSnapshot snapshots[2] = {};
	uint8_t active = 0;
};

#endif
