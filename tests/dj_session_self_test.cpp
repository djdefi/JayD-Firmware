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
	return 0;
}
