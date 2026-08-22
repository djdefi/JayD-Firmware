#ifndef JAYD_FIRMWARE_DJSESSIONSTATE_H
#define JAYD_FIRMWARE_DJSESSIONSTATE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static constexpr uint8_t DJ_DECK_COUNT = 2;
static constexpr uint8_t DJ_EFFECT_SLOT_COUNT = 3;
static constexpr uint8_t DJ_COMMAND_CAPACITY = 16;
static constexpr uint8_t DJ_RECENT_RESULT_COUNT = 8;
static constexpr size_t DJ_PATH_CAPACITY = 128;

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
	DJ_COMMAND_SET_RECORDING
};

enum DjCommandStatus : uint8_t {
	DJ_COMMAND_ACCEPTED,
	DJ_COMMAND_APPLIED,
	DJ_COMMAND_FAILED,
	DJ_COMMAND_SUPERSEDED,
	DJ_COMMAND_REJECTED
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
	DJ_COMMAND_ERROR_SESSION_ENDING
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

struct DjCommand {
	uint32_t id = 0;
	DjCommandOrigin origin = DJ_ORIGIN_SYSTEM;
	DjCommandType type = DJ_COMMAND_SET_PLAYING;
	uint8_t deck = 0;
	uint8_t slot = 0;
	uint16_t value = 0;
	char path[DJ_PATH_CAPACITY] = {};
};

struct DjSubmitResult {
	uint32_t id = 0;
	DjCommandStatus status = DJ_COMMAND_REJECTED;
	DjCommandError error = DJ_COMMAND_ERROR_NONE;

	DjSubmitResult() = default;
	DjSubmitResult(uint32_t id, DjCommandStatus status, DjCommandError error) :
			id(id), status(status), error(error){}

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

struct DjDeckSnapshot {
	bool loaded = false;
	bool playing = false;
	uint16_t elapsed = 0;
	uint16_t duration = 0;
	DjTimingQuality timingQuality = DJ_TIMING_UNAVAILABLE;
	uint8_t gain = 255;
	char path[DJ_PATH_CAPACITY] = {};
	DjEffectSnapshot effects[DJ_EFFECT_SLOT_COUNT] = {};
};

struct DjSnapshot {
	uint64_t seq = 0;
	uint64_t bootId = 0;
	uint32_t sessionId = 0;
	bool sessionActive = false;
	bool mixerRunning = false;
	uint8_t mix = 127;
	bool recording = false;
	DjDeckSnapshot decks[DJ_DECK_COUNT] = {};
	uint8_t queueDepth = 0;
	uint32_t queueDrops = 0;
	DjCommandResult recentResults[DJ_RECENT_RESULT_COUNT] = {};
};

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
			   type == DJ_COMMAND_SET_EFFECT_INTENSITY;
	}

	static bool sameTarget(const DjCommand& first, const DjCommand& second){
		if(first.type != second.type) return false;
		if(first.type == DJ_COMMAND_SET_MIX) return true;
		if(first.deck != second.deck) return false;
		if(first.type == DJ_COMMAND_SET_EFFECT_TYPE ||
		   first.type == DJ_COMMAND_SET_EFFECT_INTENSITY){
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
		DjCommandResult& result = results[next];
		result.id = command.id;
		result.origin = command.origin;
		result.type = command.type;
		result.status = status;
		result.error = error;
		next = (next + 1) % DJ_RECENT_RESULT_COUNT;
	}

	void finish(uint32_t id, DjCommandStatus status, DjCommandError error){
		for(auto& result : results){
			if(result.id != id) continue;
			result.status = status;
			result.error = error;
			return;
		}
	}

	void copyTo(DjCommandResult* destination) const{
		for(uint8_t i = 0; i < DJ_RECENT_RESULT_COUNT; i++){
			const uint8_t index = (next + DJ_RECENT_RESULT_COUNT - 1 - i) % DJ_RECENT_RESULT_COUNT;
			destination[i] = results[index];
		}
	}

private:
	DjCommandResult results[DJ_RECENT_RESULT_COUNT] = {};
	uint8_t next = 0;
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
