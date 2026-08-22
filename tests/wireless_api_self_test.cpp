#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/DjSession/DjSessionState.h"
#include "../src/Wireless/WirelessApiCore.h"

using namespace WirelessApi;

namespace {

uint32_t randomValue = 1;

uint32_t deterministicRandom(){
	return randomValue++;
}

class FakeStore : public TokenStore {
public:
	bool load(StoredTokens& output) override{
		if(!present) return false;
		output = tokens;
		return true;
	}

	bool save(const StoredTokens& input) override{
		if(failSave) return false;
		tokens = input;
		present = true;
		return true;
	}

	StoredTokens tokens = {};
	bool present = false;
	bool failSave = false;
};

void pairingAndStorage(){
	FakeStore store;
	Security security;
	security.initialize(store);
	randomValue = 42;
	assert(security.openPairing(1000, deterministicRandom));
	char code[PAIRING_CODE_CAPACITY] = {};
	assert(security.copyPairingCode(code, 1001));
	assert(strcmp(code, "000042") == 0);

	char token[TOKEN_CAPACITY] = {};
	assert(security.exchangePairing(
		code, "tablet", 1002, store, deterministicRandom, token
	) == PAIR_OK);
	assert(strlen(token) == TOKEN_CAPACITY - 1);
	assert(store.present);
	assert(!security.pairingOpen(1003));
	assert(security.exchangePairing(
		code, "tablet", 1003, store, deterministicRandom, token
	) == PAIR_CLOSED);

	char authorization[48];
	snprintf(authorization, sizeof(authorization), "Bearer %s", token);
	char clientId[CLIENT_ID_CAPACITY] = {};
	assert(security.authenticate(authorization, clientId));
	assert(strcmp(clientId, "tablet") == 0);
	assert(!security.authenticate("Bearer 00000000000000000000000000000000", clientId));

	Security restored;
	restored.initialize(store);
	assert(restored.authenticate(authorization, clientId));

	Security expired;
	assert(expired.openPairing(2000, deterministicRandom));
	assert(expired.copyPairingCode(code, 2001));
	assert(expired.exchangePairing(
		code, "phone", 2000 + PAIRING_WINDOW_MS, store, deterministicRandom, token
	) == PAIR_CLOSED);
}

void leaseAndRateLimits(){
	Security security;
	assert(security.acquireLease("tablet", 100) == LEASE_OK);
	assert(security.hasWriterLease("tablet", 101));
	assert(security.acquireLease("phone", 102) == LEASE_CONFLICT);
	assert(security.renewLease("phone", 103) == LEASE_NOT_OWNER);
	assert(security.renewLease("tablet", 104) == LEASE_OK);
	assert(security.releaseLease("tablet", 105) == LEASE_OK);
	assert(!security.hasWriterLease("tablet", 106));

	assert(security.acquireLease("tablet", 1000) == LEASE_OK);
	assert(!security.hasWriterLease("tablet", 1000 + LEASE_DURATION_MS));
	assert(security.renewLease("tablet", 1000 + LEASE_DURATION_MS) == LEASE_NOT_OWNER);

	for(uint8_t i = 0; i < 8; i++) assert(security.allowRequest("tablet", true, 5000));
	assert(!security.allowRequest("tablet", true, 5000));
	assert(security.allowRequest("tablet", true, 6000));
}

void requestParsing(){
	char valid[] =
		"{\"boot_id\":42,\"session_id\":7,\"client_command_id\":\"cmd-1\","
		"\"action\":\"set_playing\",\"deck\":1,\"value\":true}";
	JsonObject object;
	assert(object.parse(valid, strlen(valid)));
	uint64_t bootId = 0;
	bool playing = false;
	const char* action = nullptr;
	assert(object.getNumber("boot_id", bootId) && bootId == 42);
	assert(object.getString("action", action) && strcmp(action, "set_playing") == 0);
	assert(object.getBool("value", playing) && playing);

	char escaped[] = "{\"ssid\":\"quote\\\"slash\\\\ok\"}";
	assert(object.parse(escaped, strlen(escaped)));
	const char* ssid = nullptr;
	assert(object.getString("ssid", ssid));
	assert(strcmp(ssid, "quote\"slash\\ok") == 0);

	char malformed[] = "{\"value\":1,\"value\":2}";
	assert(!object.parse(malformed, strlen(malformed)));
	char nested[] = "{\"value\":{\"nested\":1}}";
	assert(!object.parse(nested, strlen(nested)));
	char oversized[MAX_REQUEST_BODY + 2];
	memset(oversized, ' ', sizeof(oversized));
	oversized[0] = '{';
	oversized[sizeof(oversized) - 2] = '}';
	oversized[sizeof(oversized) - 1] = '\0';
	assert(!object.parse(oversized, sizeof(oversized) - 1));
}

void commandLifecycle(){
	DjCommand command = {};
	command.id = 10;
	command.origin = DJ_ORIGIN_HTTP;
	command.type = DJ_COMMAND_SET_MIX;
	command.requestBootId = 11;
	command.requestSessionId = 12;
	strcpy(command.clientId, "tablet");
	strcpy(command.clientCommandId, "cmd-10");
	assert(djCommandIdentityMatches(command, 11, 12));
	assert(!djCommandIdentityMatches(command, 11, 13));

	DjCommandResults results;
	results.record(command, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE);
	DjCommandResult duplicate = {};
	assert(results.findClientCommand("tablet", "cmd-10", duplicate));
	assert(duplicate.status == DJ_COMMAND_ACCEPTED);
	assert(!results.findClientCommand("phone", "cmd-10", duplicate));
	results.finish(command.id, DJ_COMMAND_APPLIED, DJ_COMMAND_ERROR_NONE);
	assert(results.findClientCommand("tablet", "cmd-10", duplicate));
	assert(duplicate.status == DJ_COMMAND_APPLIED);

	DjCommandQueue queue;
	DjCommand pending[DJ_COMMAND_CAPACITY] = {};
	for(uint32_t id = 1; id <= DJ_COMMAND_CAPACITY; id++){
		DjCommand queued = {};
		queued.id = id;
		queued.type = DJ_COMMAND_LOAD_DECK;
		strcpy(queued.clientId, "tablet");
		snprintf(queued.clientCommandId, sizeof(queued.clientCommandId), "pending-%lu",
			static_cast<unsigned long>(id));
		assert(queue.push(queued));
		results.record(queued, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE);
		pending[id - 1] = queued;
	}
	assert(!queue.push(command));
	for(uint32_t id = 100; id < 120; id++){
		DjCommand rejected = command;
		rejected.id = id;
		snprintf(rejected.clientCommandId, sizeof(rejected.clientCommandId), "rejected-%lu",
			static_cast<unsigned long>(id));
		results.record(rejected, DJ_COMMAND_REJECTED, DJ_COMMAND_ERROR_QUEUE_FULL);
	}
	for(const DjCommand& queued : pending){
		assert(results.findClientCommand("tablet", queued.clientCommandId, duplicate));
		assert(duplicate.status == DJ_COMMAND_ACCEPTED);
	}
	assert(strcmp(RECONNECT_DIRECTIVE, "fetch_state_and_results_never_replay") == 0);
}

}

int main(){
	pairingAndStorage();
	leaseAndRateLimits();
	requestParsing();
	commandLifecycle();
	return 0;
}
