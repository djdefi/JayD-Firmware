#include <assert.h>
#include <string.h>
#include "../src/DjSession/DjSessionState.h"
#include "../src/Screens/MixScreen/MixControlState.h"

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
	source.decks[0].metadata.state = DJ_METADATA_VALID;
	source.decks[0].metadata.bpmMilli = 128000;
	snapshots.publish(source);
	DjSnapshot copy = {};
	snapshots.copy(copy);
	assert(copy.seq == source.seq);
	assert(copy.sessionId == source.sessionId);
	assert(strcmp(copy.decks[0].path, source.decks[0].path) == 0);
	assert(copy.decks[0].metadata.bpmMilli == 128000);

	source.seq = 2;
	strcpy(source.decks[0].path, "/other.aac");
	source.decks[0].metadata.bpmMilli = 130000;
	snapshots.publish(source);
	assert(copy.seq == 1);
	assert(strcmp(copy.decks[0].path, "/track.aac") == 0);
	assert(copy.decks[0].metadata.bpmMilli == 128000);

	// --- Quantize/Loop/Sync command supersede + lifecycle -----------------
	// DJ_COMMAND_SET_QUANTIZE and DJ_COMMAND_SET_SYNC are supersedable (rapid
	// dial changes on the same deck coalesce instead of queuing up), while
	// loop engage/reloop/disengage are not (each is a distinct action).
	assert(queue.push(command(40, DJ_COMMAND_SET_QUANTIZE, 0)));
	uint32_t supersededQuantize = 0;
	assert(queue.supersede(command(41, DJ_COMMAND_SET_QUANTIZE, 0), supersededQuantize));
	assert(supersededQuantize == 40);
	assert(queue.pop(popped) && popped.id == 41);

	assert(queue.push(command(42, DJ_COMMAND_SET_SYNC, 1)));
	uint32_t supersededSync = 0;
	assert(queue.supersede(command(43, DJ_COMMAND_SET_SYNC, 1), supersededSync));
	assert(supersededSync == 42);
	assert(queue.pop(popped) && popped.id == 43);

	assert(queue.push(command(44, DJ_COMMAND_LOOP_ENGAGE, 0)));
	uint32_t supersededLoop = 0;
	assert(!queue.supersede(command(45, DJ_COMMAND_LOOP_ENGAGE, 0), supersededLoop));
	assert(queue.push(command(45, DJ_COMMAND_LOOP_ENGAGE, 0)));
	assert(queue.pop(popped) && popped.id == 44);
	assert(queue.pop(popped) && popped.id == 45);

	// Command lifecycle with scheduling diagnostics: a loop engage is
	// recorded ACCEPTED, transitions to PENDING while the boundary seek is
	// outstanding, then to a terminal APPLIED with the resolved target frame
	// and lateness -- mirroring DjSession::loop()/tickLoops() exactly.
	DjCommand loopEngageCmd = command(50, DJ_COMMAND_LOOP_ENGAGE, 0);
	results.record(loopEngageCmd, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE);
	results.finishWithDiagnostics(loopEngageCmd.id, DJ_COMMAND_PENDING, DJ_COMMAND_ERROR_NONE,
								  4096, 0, false);
	results.copyTo(recent);
	assert(recent[0].status == DJ_COMMAND_PENDING);
	assert(recent[0].targetFrame == 4096);
	results.finishWithDiagnostics(loopEngageCmd.id, DJ_COMMAND_APPLIED, DJ_COMMAND_ERROR_NONE,
								  4096, 12, false);
	results.copyTo(recent);
	assert(recent[0].status == DJ_COMMAND_APPLIED);
	assert(recent[0].targetFrame == 4096);
	assert(recent[0].lateFrames == 12);
	assert(!recent[0].missed);

	// A loop whose seek retries are exhausted resolves FAILED/missed instead
	// of silently vanishing (DjSession::tickLoops()'s terminal-outcome path).
	DjCommand loopFailCmd = command(51, DJ_COMMAND_LOOP_ENGAGE, 1);
	results.record(loopFailCmd, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE);
	results.finishWithDiagnostics(loopFailCmd.id, DJ_COMMAND_FAILED, DJ_COMMAND_ERROR_LOOP_OUT_OF_RANGE,
								  8192, 200, true);
	results.copyTo(recent);
	assert(recent[0].status == DJ_COMMAND_FAILED);
	assert(recent[0].error == DJ_COMMAND_ERROR_LOOP_OUT_OF_RANGE);
	assert(recent[0].missed);

	// --- Capability-disabled default snapshot state ------------------------
	// A deck with no grid/quantize/loop/sync capability yet (or one where
	// buildGrid() rejected the metadata) must default-report as disabled,
	// not as a stale/ambiguous "on" state, so an API consumer never mistakes
	// an absent capability for an active one.
	DjSnapshot disabledSnapshot = {};
	assert(!disabledSnapshot.decks[0].grid.valid);
	assert(disabledSnapshot.decks[0].grid.currentQuarterBeat == 0);
	assert(disabledSnapshot.decks[0].quantize.resolution == DJ_QUANTIZE_OFF);
	assert(!disabledSnapshot.decks[0].quantize.pending);
	assert(disabledSnapshot.decks[0].loop.state == DJ_LOOP_INACTIVE);
	assert(disabledSnapshot.decks[0].loop.validLengthMask == 0);
	assert(disabledSnapshot.decks[0].sync.state == DJ_SYNC_OFF);
	assert(disabledSnapshot.decks[0].sync.masterDeck == -1);
	assert(disabledSnapshot.decks[0].sync.lastError == DJ_COMMAND_ERROR_NONE);

	DjCueState cues;
	uint16_t cuePosition = 0;
	assert(!cues.trigger(0, 0, cuePosition));
	assert(cues.set(0, 0, 42));
	assert(cues.trigger(0, 0, cuePosition) && cuePosition == 42);
	assert(cues.clear(0, 0));
	assert(!cues.trigger(0, 0, cuePosition));
	assert(!cues.set(DJ_DECK_COUNT, 0, 1));
	assert(!cues.set(0, DJ_CUE_COUNT, 1));

	MixControlState controls;
	assert(controls.bank == MIX_BANK_MIX);
	controls.openPalette();
	controls.movePalette(1);
	assert(controls.confirmPalette() == MIX_PALETTE_CUES);
	assert(controls.bank == MIX_BANK_CUES);
	controls.moveCuePage(1);
	assert(controls.cueForEncoder(0) == 3);
	assert(controls.cueForEncoder(5) == 5);
	controls.moveCuePage(2);
	assert(controls.cueForEncoder(0) == 6);
	assert(controls.cueForEncoder(2) == -1);
	assert(MixControlState::browseDeckForButton(0) == 0);
	assert(MixControlState::browseDeckForButton(1) == 1);
	assert(MixControlState::browseDeckForButton(2) == -1);
	assert(!MixControlState::encoderChordsEnabled());
	assert(MixControlState::suppressDeckReleaseAfterChord());

	// LOOP/SYNC bank is reachable via the palette alongside MIX/CUES/BROWSE,
	// preserving the ordinal alignment confirmPalette() relies on.
	controls.openPalette();
	controls.movePalette(3);
	assert(controls.confirmPalette() == MIX_PALETTE_LOOPSYNC);
	assert(controls.bank == MIX_BANK_LOOPSYNC);

	return 0;
}
