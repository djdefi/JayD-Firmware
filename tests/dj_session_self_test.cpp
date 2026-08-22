#include <assert.h>
#include <string.h>
#include "../src/DjSession/DjSessionState.h"

static DjCommand command(uint32_t id, DjCommandType type, uint8_t deck = 0, uint8_t slot = 0){
	DjCommand result = {};
	result.id = id;
	result.type = type;
	result.deck = deck;
	result.slot = slot;
	return result;
}

int main(){
	DjCommandQueue queue;
	for(uint32_t id = 1; id <= DJ_COMMAND_CAPACITY; id++){
		assert(queue.push(command(id, DJ_COMMAND_LOAD_DECK)));
	}
	assert(!queue.push(command(100, DJ_COMMAND_LOAD_DECK)));
	assert(queue.depth() == DJ_COMMAND_CAPACITY);

	DjCommand popped;
	for(uint32_t id = 1; id <= DJ_COMMAND_CAPACITY; id++){
		assert(queue.pop(popped));
		assert(popped.id == id);
	}
	assert(!queue.pop(popped));

	assert(queue.push(command(20, DJ_COMMAND_SET_MIX)));
	uint32_t superseded = 0;
	assert(queue.supersede(command(21, DJ_COMMAND_SET_MIX), superseded));
	assert(superseded == 20);
	assert(queue.contains(DJ_COMMAND_SET_MIX));
	assert(queue.pop(popped));
	assert(popped.id == 21);

	assert(queue.push(command(22, DJ_COMMAND_SET_MIX)));
	assert(queue.push(command(23, DJ_COMMAND_LOAD_DECK)));
	assert(!queue.supersede(command(24, DJ_COMMAND_SET_MIX), superseded));
	assert(queue.push(command(24, DJ_COMMAND_SET_MIX)));
	assert(queue.pop(popped) && popped.id == 22);
	assert(queue.pop(popped) && popped.id == 23);
	assert(queue.pop(popped) && popped.id == 24);

	DjCommandResults results;
	DjCommand accepted = command(30, DJ_COMMAND_SET_PLAYING);
	results.record(accepted, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE);
	results.finish(accepted.id, DJ_COMMAND_APPLIED, DJ_COMMAND_ERROR_NONE);
	DjCommandResult recent[DJ_RECENT_RESULT_COUNT] = {};
	results.copyTo(recent);
	assert(recent[0].id == accepted.id);
	assert(recent[0].status == DJ_COMMAND_APPLIED);

	DjCommand failed = command(31, DJ_COMMAND_LOAD_DECK);
	results.record(failed, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE);
	results.finish(failed.id, DJ_COMMAND_FAILED, DJ_COMMAND_ERROR_OPEN_FAILED);
	results.copyTo(recent);
	assert(recent[0].status == DJ_COMMAND_FAILED);
	assert(recent[0].error == DJ_COMMAND_ERROR_OPEN_FAILED);

	DjCommand replaced = command(32, DJ_COMMAND_SET_GAIN);
	results.record(replaced, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE);
	results.finish(replaced.id, DJ_COMMAND_SUPERSEDED, DJ_COMMAND_ERROR_NONE);
	results.copyTo(recent);
	assert(recent[0].status == DJ_COMMAND_SUPERSEDED);

	DjEffectState effects;
	DjEffectTransition transition;
	assert(effects.setType(0, 0, DJ_EFFECT_LOWPASS, false, transition));
	assert(!transition.addSpeed && !transition.setSpeed);
	assert(effects.setIntensity(0, 0, 80, transition));
	assert(effects.get(0, 0).type == DJ_EFFECT_LOWPASS);
	assert(effects.get(0, 0).intensity == 80);
	DjEffectSnapshot visibleEffects[DJ_EFFECT_SLOT_COUNT] = {};
	effects.copyDeck(0, visibleEffects);
	assert(visibleEffects[0].type == DJ_EFFECT_LOWPASS);
	assert(visibleEffects[0].intensity == 80);
	effects.deckLoaded(0, transition);
	assert(!transition.addSpeed);

	transition = {};
	assert(effects.setType(1, 1, DJ_EFFECT_SPEED, false, transition));
	assert(transition.clearEffect);
	assert(!effects.isSpeedActive(1));
	assert(effects.setIntensity(1, 1, 200, transition));
	assert(!transition.setSpeed);
	transition = {};
	effects.deckLoaded(1, transition);
	assert(transition.addSpeed && transition.setSpeed);
	assert(effects.isSpeedActive(1));
	assert(effects.get(1, 1).intensity == 200);

	DjEffectState removedBeforeLoad;
	transition = {};
	assert(removedBeforeLoad.setType(0, 0, DJ_EFFECT_SPEED, false, transition));
	assert(removedBeforeLoad.setType(0, 0, DJ_EFFECT_HIGHPASS, false, transition));
	assert(removedBeforeLoad.setIntensity(0, 0, 64, transition));
	transition = {};
	removedBeforeLoad.deckLoaded(0, transition);
	assert(!transition.addSpeed && !removedBeforeLoad.isSpeedActive(0));
	assert(removedBeforeLoad.get(0, 0).type == DJ_EFFECT_HIGHPASS);
	assert(removedBeforeLoad.get(0, 0).intensity == 64);

	DjEffectState disabledBeforeLoad;
	transition = {};
	assert(disabledBeforeLoad.setType(0, 2, DJ_EFFECT_SPEED, false, transition));
	assert(disabledBeforeLoad.setType(0, 2, DJ_EFFECT_NONE, false, transition));
	transition = {};
	disabledBeforeLoad.deckLoaded(0, transition);
	assert(!transition.addSpeed && !disabledBeforeLoad.isSpeedActive(0));

	DjCommand effectCommand = command(33, DJ_COMMAND_SET_EFFECT_TYPE, 0, 0);
	results.record(effectCommand, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE);
	results.finish(effectCommand.id, DJ_COMMAND_APPLIED, DJ_COMMAND_ERROR_NONE);
	results.copyTo(recent);
	assert(recent[0].id == effectCommand.id);
	assert(recent[0].status == DJ_COMMAND_APPLIED);

	DjSnapshotBuffers snapshots;
	DjSnapshot source = {};
	source.seq = 1;
	source.sessionId = 42;
	strcpy(source.decks[0].path, "/track.aac");
	snapshots.publish(source);
	DjSnapshot copy = {};
	snapshots.copy(copy);
	assert(copy.seq == source.seq);
	assert(copy.sessionId == source.sessionId);
	assert(strcmp(copy.decks[0].path, source.decks[0].path) == 0);

	source.seq = 2;
	strcpy(source.decks[0].path, "/other.aac");
	snapshots.publish(source);
	assert(copy.seq == 1);
	assert(strcmp(copy.decks[0].path, "/track.aac") == 0);

	// Stale-identity regression: a command captured against a pre-reboot
	// boot_id/session_id must never match the device's current identity,
	// across the full uint64 range (not just small test values) - this is
	// the exact compare the boot_id-precision fix depends on staying
	// correct now that boot_id travels the wire as an opaque string.
	DjCommand preReboot = command(40, DJ_COMMAND_SET_MIX);
	preReboot.origin = DJ_ORIGIN_HTTP;
	preReboot.requestBootId = UINT64_MAX - 1;
	preReboot.requestSessionId = 42;
	assert(djCommandIdentityMatches(preReboot, UINT64_MAX - 1, 42));
	// Device rebooted: new boot_id, session reset to 0. The stale command
	// must be rejected, not accidentally accepted.
	assert(!djCommandIdentityMatches(preReboot, UINT64_MAX, 0));
	// Non-HTTP-origin commands (local UI on the device itself) carry no
	// request identity and must never be subject to this check.
	DjCommand local = command(41, DJ_COMMAND_SET_MIX);
	local.origin = DJ_ORIGIN_LOCAL_UI;
	assert(djCommandIdentityMatches(local, UINT64_MAX, 0));

	return 0;
}
