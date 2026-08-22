#include "WirelessBringup.h"

#if defined(JAYD_ENABLE_WIRELESS)

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdarg.h>
#include <string.h>
#include "../DjSession/DjSession.h"
#include "WirelessApiCore.h"
#include "WirelessUiAsset.h"

namespace {

static constexpr uint32_t STATION_TIMEOUT_MS = 10000;
static constexpr uint32_t REQUEST_TIMEOUT_MS = 75;
static constexpr uint32_t HANDLER_BUDGET_MS = 50;
static constexpr size_t MAX_REQUEST_LINE = 192;
static constexpr size_t MAX_HEADER_LINE = 256;
static constexpr size_t MAX_RESPONSE = 3072;

class BoundedWebServer : public WebServer {
public:
	explicit BoundedWebServer(uint16_t port) : WebServer(port){}

	void handleClient() override{
		WiFiClient incoming = _server.available();
		if(!incoming) return;
		_currentClient = incoming;
		_currentClient.setNoDelay(true);
		_currentClient.setTimeout(REQUEST_TIMEOUT_MS);
		bodyLength = 0;
		bodyBuffer[0] = '\0';
		requestDeadline = millis() + REQUEST_TIMEOUT_MS;
		if(requestDeadline == 0) requestDeadline = 1;

		const ParseResult parsed = parseBoundedRequest();
		if(parsed != PARSE_OK){
			if(parsed == PARSE_TOO_LARGE){
				oversizedRequests++;
				reject(413, "request_too_large");
			}else{
				malformedRequests++;
				reject(400, "invalid_request");
			}
			_currentClient.stop();
			_currentClient = WiFiClient();
			return;
		}

		_contentLength = CONTENT_LENGTH_NOT_SET;
		_chunked = false;
		_responseHeaders = "";
		const uint32_t started = millis();
		_handleRequest();
		if(millis() - started > HANDLER_BUDGET_MS) handlerBudgetOverruns++;
		_currentClient.stop();
		_currentClient = WiFiClient();
	}

	char* body(){
		return bodyBuffer;
	}

	size_t bodySize() const{
		return bodyLength;
	}

	void noteMalformed(){
		malformedRequests++;
	}

	uint32_t malformedCount() const{
		return malformedRequests;
	}

	uint32_t oversizedCount() const{
		return oversizedRequests;
	}

	uint32_t budgetOverrunCount() const{
		return handlerBudgetOverruns;
	}

private:
	enum ParseResult : uint8_t {
		PARSE_OK,
		PARSE_INVALID,
		PARSE_TOO_LARGE
	};

	char bodyBuffer[WirelessApi::MAX_REQUEST_BODY + 1] = {};
	size_t bodyLength = 0;
	uint32_t malformedRequests = 0;
	uint32_t oversizedRequests = 0;
	uint32_t handlerBudgetOverruns = 0;
	uint32_t requestDeadline = 0;

	bool beforeDeadline() const{
		return static_cast<int32_t>(requestDeadline - millis()) > 0;
	}

	ParseResult readLine(char* output, size_t capacity){
		size_t length = 0;
		while(_currentClient.connected() && beforeDeadline()){
			while(_currentClient.available()){
				const int value = _currentClient.read();
				if(value < 0) break;
				if(value == '\r') continue;
				if(value == '\n'){
					output[length] = '\0';
					return PARSE_OK;
				}
				if(length + 1 >= capacity) return PARSE_TOO_LARGE;
				output[length++] = static_cast<char>(value);
			}
			delay(1);
		}
		return PARSE_INVALID;
	}

	ParseResult readBody(size_t length){
		if(length > WirelessApi::MAX_REQUEST_BODY) return PARSE_TOO_LARGE;
		size_t received = 0;
		while(received < length && _currentClient.connected() && beforeDeadline()){
			const int available = _currentClient.available();
			if(available <= 0){
				delay(1);
				continue;
			}
			const size_t chunk = min(length - received, static_cast<size_t>(available));
			const int count = _currentClient.read(
				reinterpret_cast<uint8_t*>(bodyBuffer + received),
				chunk
			);
			if(count <= 0) return PARSE_INVALID;
			received += static_cast<size_t>(count);
		}
		if(received != length) return PARSE_INVALID;
		bodyBuffer[received] = '\0';
		bodyLength = received;
		return PARSE_OK;
	}

	ParseResult parseBoundedRequest(){
		char line[MAX_REQUEST_LINE];
		ParseResult result = readLine(line, sizeof(line));
		if(result != PARSE_OK) return result;

		char* methodEnd = strchr(line, ' ');
		if(!methodEnd) return PARSE_INVALID;
		*methodEnd++ = '\0';
		char* version = strchr(methodEnd, ' ');
		if(!version) return PARSE_INVALID;
		*version++ = '\0';
		if(strchr(methodEnd, '?') || methodEnd[0] != '/') return PARSE_INVALID;
		if(strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) return PARSE_INVALID;
		if(strcmp(line, "GET") == 0) _currentMethod = HTTP_GET;
		else if(strcmp(line, "POST") == 0) _currentMethod = HTTP_POST;
		else return PARSE_INVALID;
		_currentUri = methodEnd;
		_currentVersion = strcmp(version, "HTTP/1.1") == 0 ? 1 : 0;
		_hostHeader = "";

		for(int i = 0; i < _headerKeysCount; i++) _currentHeaders[i].value = "";
		bool contentLengthSeen = false;
		bool hostSeen = false;
		bool contentTypeSeen = false;
		bool authorizationSeen = false;
		bool originSeen = false;
		bool transferEncodingSeen = false;
		size_t contentLength = 0;
		size_t headerBytes = 0;
		uint8_t headerCount = 0;

		while(true){
			char header[MAX_HEADER_LINE];
			result = readLine(header, sizeof(header));
			if(result != PARSE_OK) return result;
			if(header[0] == '\0') break;
			headerBytes += strlen(header);
			if(++headerCount > 16 || headerBytes > 1024) return PARSE_TOO_LARGE;
			char* separator = strchr(header, ':');
			if(!separator || separator == header) return PARSE_INVALID;
			*separator++ = '\0';
			while(*separator == ' ' || *separator == '\t') separator++;
			char* end = separator + strlen(separator);
			while(end > separator && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';

			bool* seen = nullptr;
			if(strcasecmp(header, "Content-Length") == 0) seen = &contentLengthSeen;
			else if(strcasecmp(header, "Host") == 0) seen = &hostSeen;
			else if(strcasecmp(header, "Content-Type") == 0) seen = &contentTypeSeen;
			else if(strcasecmp(header, "Authorization") == 0) seen = &authorizationSeen;
			else if(strcasecmp(header, "Origin") == 0) seen = &originSeen;
			else if(strcasecmp(header, "Transfer-Encoding") == 0) seen = &transferEncodingSeen;
			if(seen && *seen) return PARSE_INVALID;
			if(seen) *seen = true;

			if(strcasecmp(header, "Content-Length") == 0){
				if(*separator == '\0') return PARSE_INVALID;
				for(const char* digit = separator; *digit; digit++){
					if(*digit < '0' || *digit > '9') return PARSE_INVALID;
				}
				char* parsedEnd = nullptr;
				const unsigned long parsed = strtoul(separator, &parsedEnd, 10);
				if(!parsedEnd || *parsedEnd != '\0') return PARSE_INVALID;
				contentLength = parsed;
				if(contentLength > WirelessApi::MAX_REQUEST_BODY) return PARSE_TOO_LARGE;
			}else if(strcasecmp(header, "Host") == 0){
				if(*separator == '\0') return PARSE_INVALID;
				_hostHeader = separator;
			}
			_collectHeader(header, separator);
		}

		if(_currentVersion == 1 && !hostSeen) return PARSE_INVALID;
		if(transferEncodingSeen) return PARSE_INVALID;
		if(_currentMethod == HTTP_GET && contentLength != 0) return PARSE_INVALID;
		if(_currentMethod == HTTP_POST && (!contentLengthSeen || contentLength == 0)) return PARSE_INVALID;
		if(contentLength){
			result = readBody(contentLength);
			if(result != PARSE_OK) return result;
		}

		_currentHandler = nullptr;
		for(RequestHandler* handler = _firstHandler; handler; handler = handler->next()){
			if(handler->canHandle(_currentMethod, _currentUri)){
				_currentHandler = handler;
				break;
			}
		}
		return PARSE_OK;
	}

	void reject(int status, const char* error){
		char response[96];
		snprintf(response, sizeof(response), "{\"error\":\"%s\"}", error);
		_contentLength = CONTENT_LENGTH_NOT_SET;
		_chunked = false;
		sendHeader("Cache-Control", "no-store");
		sendHeader("X-Content-Type-Options", "nosniff");
		send(status, "application/json", response);
	}
};

class PreferencesTokenStore : public WirelessApi::TokenStore {
public:
	explicit PreferencesTokenStore(Preferences& preferences) : preferences(preferences){}

	bool load(WirelessApi::StoredTokens& tokens) override{
		if(preferences.getBytesLength("api_tokens") != sizeof(tokens)) return false;
		return preferences.getBytes("api_tokens", &tokens, sizeof(tokens)) == sizeof(tokens);
	}

	bool save(const WirelessApi::StoredTokens& tokens) override{
		return preferences.putBytes("api_tokens", &tokens, sizeof(tokens)) == sizeof(tokens);
	}

private:
	Preferences& preferences;
};

class JsonWriter {
public:
	JsonWriter(char* output, size_t capacity) : output(output), capacity(capacity){
		if(capacity) output[0] = '\0';
	}

	bool append(const char* format, ...){
		if(!ok || length >= capacity) return false;
		va_list args;
		va_start(args, format);
		const int written = vsnprintf(output + length, capacity - length, format, args);
		va_end(args);
		if(written < 0 || static_cast<size_t>(written) >= capacity - length){
			ok = false;
			return false;
		}
		length += static_cast<size_t>(written);
		return true;
	}

	bool escaped(const char* input){
		if(!input) return append("");
		for(const unsigned char* cursor = reinterpret_cast<const unsigned char*>(input); *cursor; cursor++){
			const unsigned char c = *cursor;
			if(c == '"' || c == '\\'){
				if(!append("\\%c", c)) return false;
			}else if(c >= 0x20 && c <= 0x7e){
				if(!append("%c", c)) return false;
			}
		}
		return true;
	}

	bool valid() const{
		return ok;
	}

private:
	char* output;
	size_t capacity;
	size_t length = 0;
	bool ok = true;
};

BoundedWebServer server(80);
Preferences preferences;
PreferencesTokenStore tokenStore(preferences);
WirelessApi::Security security;
String stationSsid;
String stationPassword;
uint32_t stationDeadline = 0;
uint32_t restartAt = 0;
uint32_t lastPairingGeneration = 0;
uint32_t lastPairingSessionId = 0;
bool setupMode = false;
bool serverStarted = false;
char responseBuffer[MAX_RESPONSE];

uint32_t randomWord(){
	return esp_random();
}

const char* commandStatusName(DjCommandStatus status){
	switch(status){
		case DJ_COMMAND_ACCEPTED: return "accepted";
		case DJ_COMMAND_APPLIED: return "applied";
		case DJ_COMMAND_FAILED: return "failed";
		case DJ_COMMAND_SUPERSEDED: return "superseded";
		case DJ_COMMAND_REJECTED: return "rejected";
		default: return "rejected";
	}
}

const char* commandErrorName(DjCommandError error){
	switch(error){
		case DJ_COMMAND_ERROR_NONE: return "none";
		case DJ_COMMAND_ERROR_QUEUE_FULL: return "queue_full";
		case DJ_COMMAND_ERROR_INVALID_DECK: return "invalid_deck";
		case DJ_COMMAND_ERROR_INVALID_SLOT: return "invalid_slot";
		case DJ_COMMAND_ERROR_INVALID_VALUE: return "invalid_value";
		case DJ_COMMAND_ERROR_INVALID_PATH: return "invalid_path";
		case DJ_COMMAND_ERROR_NO_DECK: return "no_deck";
		case DJ_COMMAND_ERROR_NO_EFFECT: return "no_effect";
		case DJ_COMMAND_ERROR_OPEN_FAILED: return "open_failed";
		case DJ_COMMAND_ERROR_SESSION_ENDING: return "session_ending";
		case DJ_COMMAND_ERROR_STALE_IDENTITY: return "stale_identity";
		case DJ_COMMAND_ERROR_CLIENT_ID_REQUIRED: return "client_id_required";
		default: return "invalid_value";
	}
}

void sendJson(int status, const char* body){
	server.sendHeader("Cache-Control", "no-store");
	server.sendHeader("X-Content-Type-Options", "nosniff");
	server.sendHeader("Content-Security-Policy", "default-src 'none'");
	server.send(status, "application/json", body);
}

void sendError(int status, const char* error){
	char body[112];
	snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error);
	sendJson(status, body);
}

bool requireJsonBody(WirelessApi::JsonObject& object){
	if(server.header("Content-Type") != "application/json"){
		sendError(415, "content_type_must_be_application_json");
		return false;
	}
	if(!object.parse(server.body(), server.bodySize())){
		server.noteMalformed();
		sendError(400, "invalid_json");
		return false;
	}
	return true;
}

bool sameOrigin(){
	const String origin = server.header("Origin");
	if(origin.length() == 0) return true;
	const String host = server.header("Host");
	if(host.length() == 0 || origin.length() != host.length() + 7) return false;
	return origin.startsWith("http://") && origin.substring(7) == host;
}

bool authorize(bool write, char clientId[WirelessApi::CLIENT_ID_CAPACITY]){
	if(setupMode){
		sendError(503, "control_unavailable_in_setup_mode");
		return false;
	}
	if(!sameOrigin()){
		sendError(403, "origin_rejected");
		return false;
	}
	const String authorization = server.header("Authorization");
	if(!security.authenticate(authorization.c_str(), clientId)){
		server.sendHeader("WWW-Authenticate", "Bearer");
		sendError(401, "unauthorized");
		return false;
	}
	if(!security.allowRequest(clientId, write, millis())){
		server.sendHeader("Retry-After", "1");
		sendError(429, "rate_limited");
		return false;
	}
	return true;
}

bool copySnapshot(DjSnapshot& snapshot){
	DjSession* session = DjSession::get();
	if(!session) return false;
	session->copySnapshot(snapshot);
	return true;
}

void sendCommandResult(const DjSubmitResult& result, const char* clientCommandId){
	JsonWriter writer(responseBuffer, sizeof(responseBuffer));
	writer.append(
		"{\"client_command_id\":\"%s\",\"command_id\":%lu,\"status\":\"%s\","
		"\"error\":\"%s\",\"duplicate\":%s}",
		clientCommandId,
		static_cast<unsigned long>(result.id),
		commandStatusName(result.status),
		commandErrorName(result.error),
		result.duplicate ? "true" : "false"
	);
	if(!writer.valid()){
		sendError(500, "response_too_large");
		return;
	}
	int status = result.status == DJ_COMMAND_ACCEPTED ? 202 : 200;
	if(result.status == DJ_COMMAND_REJECTED){
		status = result.error == DJ_COMMAND_ERROR_QUEUE_FULL ? 503 : 409;
	}
	sendJson(status, responseBuffer);
}

// Single static browser control UI, gzip-embedded from src/Wireless/ui/.
// Served regardless of setup mode: the page itself detects setup vs. paired
// vs. control state at runtime by calling GET /setup then the v2 API. It
// carries no secrets, so it needs no auth/origin gate of its own.
void handleIndex(){
	server.sendHeader("Cache-Control", "no-store");
	server.sendHeader("X-Content-Type-Options", "nosniff");
	server.sendHeader(
		"Content-Security-Policy",
		"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
		"connect-src 'self'; base-uri 'none'; form-action 'none'"
	);
	server.sendHeader("Content-Encoding", "gzip");
	server.send_P(
		200,
		"text/html; charset=utf-8",
		reinterpret_cast<PGM_P>(WirelessUi::HTML_GZ),
		WirelessUi::HTML_GZ_LEN
	);
}

void handleSetupStatus(){
	if(!setupMode){
		sendError(404, "not_found");
		return;
	}
	char suffix[7];
	snprintf(suffix, sizeof(suffix), "%06llX", ESP.getEfuseMac() & 0xFFFFFF);
	snprintf(
		responseBuffer,
		sizeof(responseBuffer),
		"{\"mode\":\"setup\",\"ssid\":\"Jay-D-%s\",\"provision\":\"POST /setup/wifi\"}",
		suffix
	);
	sendJson(200, responseBuffer);
}

void handleSetupWifi(){
	if(!setupMode){
		sendError(404, "not_found");
		return;
	}
	if(!sameOrigin()){
		sendError(403, "origin_rejected");
		return;
	}
	if(!security.allowPairAttempt(millis())){
		sendError(429, "rate_limited");
		return;
	}
	WirelessApi::JsonObject object;
	if(!requireJsonBody(object)) return;
	static const char* allowed[] = {"ssid", "password"};
	const char* ssid = nullptr;
	const char* password = nullptr;
	if(object.size() != 2 || !object.hasOnly(allowed, 2) ||
	   !object.getString("ssid", ssid) || !object.getString("password", password)){
		sendError(400, "invalid_fields");
		return;
	}
	const size_t ssidLength = strlen(ssid);
	const size_t passwordLength = strlen(password);
	if(ssidLength == 0 || ssidLength > 32 ||
	   (passwordLength != 0 && (passwordLength < 8 || passwordLength > 63))){
		sendError(400, "invalid_wifi_credentials");
		return;
	}
	if(preferences.putString("ssid", ssid) != ssidLength ||
	   preferences.putString("password", password) != passwordLength){
		sendError(500, "credential_storage_failed");
		return;
	}
	restartAt = millis() + 250;
	sendJson(202, "{\"status\":\"saved\",\"restarting\":true}");
}

void handlePair(){
	if(setupMode){
		sendError(503, "pairing_unavailable_in_setup_mode");
		return;
	}
	if(!sameOrigin()){
		sendError(403, "origin_rejected");
		return;
	}
	if(!security.allowPairAttempt(millis())){
		sendError(429, "rate_limited");
		return;
	}
	WirelessApi::JsonObject object;
	if(!requireJsonBody(object)) return;
	static const char* allowed[] = {"code", "client_id"};
	const char* code = nullptr;
	const char* clientId = nullptr;
	if(object.size() != 2 || !object.hasOnly(allowed, 2) ||
	   !object.getString("code", code) || !object.getString("client_id", clientId)){
		sendError(400, "invalid_fields");
		return;
	}
	char token[WirelessApi::TOKEN_CAPACITY] = {};
	const WirelessApi::PairResult result = security.exchangePairing(
		code, clientId, millis(), tokenStore, randomWord, token
	);
	if(result != WirelessApi::PAIR_OK){
		if(result == WirelessApi::PAIR_CLOSED) sendError(403, "pairing_window_closed");
		else if(result == WirelessApi::PAIR_INVALID_CODE) sendError(403, "pairing_code_invalid");
		else if(result == WirelessApi::PAIR_INVALID_CLIENT) sendError(400, "client_id_invalid");
		else if(result == WirelessApi::PAIR_CLIENT_LIMIT) sendError(409, "paired_client_limit");
		else sendError(500, "token_storage_failed");
		return;
	}
	snprintf(
		responseBuffer,
		sizeof(responseBuffer),
		"{\"status\":\"paired\",\"token\":\"%s\",\"token_type\":\"Bearer\"}",
		token
	);
	memset(token, 0, sizeof(token));
	sendJson(201, responseBuffer);
}

void handleCapabilities(){
	char clientId[WirelessApi::CLIENT_ID_CAPACITY] = {};
	if(!authorize(false, clientId)) return;
	sendJson(200,
		"{\"api\":\"v2\",\"transport\":\"http_serial\","
		"\"actions\":[\"set_playing\",\"seek\",\"set_gain\",\"set_mix\","
		"\"set_effect_type\",\"set_effect_intensity\",\"set_recording\"],"
		"\"load_by_path\":false,\"writer_lease_ms\":15000,\"pairing_window_ms\":60000,"
		"\"request_body_max\":512,\"response_max\":3072,\"handler_budget_ms\":50,"
		"\"poll\":{\"active_ms\":1000,\"idle_ms\":3000,\"hidden_ms\":5000},"
		"\"reconnect\":\"fetch_state_and_results_never_replay\","
		"\"ota\":\"requires_recovery_partition\"}"
	);
}

void handleHealth(){
	char clientId[WirelessApi::CLIENT_ID_CAPACITY] = {};
	if(!authorize(false, clientId)) return;
	DjSnapshot snapshot = {};
	const bool active = copySnapshot(snapshot);
	const WirelessApi::Counters& counters = security.counters();
	snprintf(
		responseBuffer,
		sizeof(responseBuffer),
		"{\"wifi\":\"connected\",\"session_active\":%s,\"free_heap\":%lu,"
		"\"free_psram\":%lu,\"auth_rejects\":%lu,\"rate_rejects\":%lu,"
		"\"malformed_requests\":%lu,\"oversized_requests\":%lu,"
		"\"handler_budget_overruns\":%lu,\"queue_drops\":%lu}",
		active ? "true" : "false",
		static_cast<unsigned long>(ESP.getFreeHeap()),
		static_cast<unsigned long>(ESP.getFreePsram()),
		static_cast<unsigned long>(counters.authRejects),
		static_cast<unsigned long>(counters.rateRejects),
		static_cast<unsigned long>(server.malformedCount()),
		static_cast<unsigned long>(server.oversizedCount()),
		static_cast<unsigned long>(server.budgetOverrunCount()),
		static_cast<unsigned long>(snapshot.queueDrops)
	);
	sendJson(200, responseBuffer);
}

void handleState(){
	char clientId[WirelessApi::CLIENT_ID_CAPACITY] = {};
	if(!authorize(false, clientId)) return;
	DjSnapshot snapshot = {};
	if(!copySnapshot(snapshot)){
		sendError(409, "session_unavailable");
		return;
	}
	JsonWriter writer(responseBuffer, sizeof(responseBuffer));
	writer.append(
		// boot_id is a full-range random uint64_t and is therefore carried
		// as an opaque quoted decimal string, not a bare JSON number: a
		// JSON number can only be represented exactly up to 2^53-1 in a
		// browser's IEEE-754 double, so a bare-number boot_id would get
		// silently rounded by JSON.parse and no longer match on the next
		// command (stale_identity), even though nothing actually changed.
		// seq/session_id stay bare numbers - seq is monotonic and won't
		// realistically exceed 2^53 within a boot's uptime, and
		// session_id is a uint32_t, both safely exact as JS numbers.
		"{\"seq\":%llu,\"boot_id\":\"%llu\",\"session_id\":%lu,\"active\":%s,"
		"\"mixer_running\":%s,\"mix\":%u,\"recording\":%s,\"queue_depth\":%u,\"decks\":[",
		static_cast<unsigned long long>(snapshot.seq),
		static_cast<unsigned long long>(snapshot.bootId),
		static_cast<unsigned long>(snapshot.sessionId),
		snapshot.sessionActive ? "true" : "false",
		snapshot.mixerRunning ? "true" : "false",
		snapshot.mix,
		snapshot.recording ? "true" : "false",
		snapshot.queueDepth
	);
	for(uint8_t deck = 0; deck < DJ_DECK_COUNT; deck++){
		const DjDeckSnapshot& state = snapshot.decks[deck];
		writer.append(
			"%s{\"loaded\":%s,\"playing\":%s,\"elapsed\":%u,\"duration\":%u,"
			"\"timing\":\"%s\",\"gain\":%u,\"path\":\"",
			deck ? "," : "",
			state.loaded ? "true" : "false",
			state.playing ? "true" : "false",
			state.elapsed,
			state.duration,
			state.timingQuality == DJ_TIMING_COARSE ? "coarse" : "unavailable",
			state.gain
		);
		writer.escaped(state.path);
		writer.append("\",\"effects\":[");
		for(uint8_t slot = 0; slot < DJ_EFFECT_SLOT_COUNT; slot++){
			writer.append(
				"%s{\"type\":%u,\"intensity\":%u}",
				slot ? "," : "",
				state.effects[slot].type,
				state.effects[slot].intensity
			);
		}
		writer.append("]}");
	}
	writer.append("],\"recent_results\":[");
	bool first = true;
	uint8_t resultCount = 0;
	for(uint8_t i = 0; i < DJ_RECENT_RESULT_COUNT; i++){
		const DjCommandResult& result = snapshot.recentResults[i];
		if(result.clientCommandId[0] == '\0' || strcmp(result.clientId, clientId) != 0) continue;
		writer.append(
			"%s{\"client_command_id\":\"%s\",\"command_id\":%lu,\"status\":\"%s\",\"error\":\"%s\"}",
			first ? "" : ",",
			result.clientCommandId,
			static_cast<unsigned long>(result.id),
			commandStatusName(result.status),
			commandErrorName(result.error)
		);
		first = false;
		if(++resultCount == 8) break;
	}
	writer.append("]}");
	if(!writer.valid()){
		sendError(500, "response_too_large");
		return;
	}
	sendJson(200, responseBuffer);
}

void handlePairingState(){
	char clientId[WirelessApi::CLIENT_ID_CAPACITY] = {};
	if(!authorize(false, clientId)) return;
	const uint32_t remaining = security.pairingRemaining(millis());
	snprintf(
		responseBuffer,
		sizeof(responseBuffer),
		"{\"open\":%s,\"remaining_ms\":%lu}",
		remaining ? "true" : "false",
		static_cast<unsigned long>(remaining)
	);
	sendJson(200, responseBuffer);
}

void sendLeaseState(int status, const char* operation){
	char owner[WirelessApi::CLIENT_ID_CAPACITY] = {};
	const bool held = security.copyLeaseOwner(owner, millis());
	JsonWriter writer(responseBuffer, sizeof(responseBuffer));
	writer.append(
		"{\"operation\":\"%s\",\"held\":%s,\"owner\":\"",
		operation,
		held ? "true" : "false"
	);
	if(held) writer.escaped(owner);
	writer.append("\",\"remaining_ms\":%lu}", static_cast<unsigned long>(security.leaseRemaining(millis())));
	if(!writer.valid()) sendError(500, "response_too_large");
	else sendJson(status, responseBuffer);
}

void handleLeaseGet(){
	char clientId[WirelessApi::CLIENT_ID_CAPACITY] = {};
	if(!authorize(false, clientId)) return;
	sendLeaseState(200, "status");
}

void handleLeasePost(){
	char clientId[WirelessApi::CLIENT_ID_CAPACITY] = {};
	if(!authorize(true, clientId)) return;
	WirelessApi::JsonObject object;
	if(!requireJsonBody(object)) return;
	static const char* allowed[] = {"operation"};
	const char* operation = nullptr;
	if(object.size() != 1 || !object.hasOnly(allowed, 1) || !object.getString("operation", operation)){
		sendError(400, "invalid_fields");
		return;
	}
	WirelessApi::LeaseResult result;
	if(strcmp(operation, "acquire") == 0) result = security.acquireLease(clientId, millis());
	else if(strcmp(operation, "renew") == 0) result = security.renewLease(clientId, millis());
	else if(strcmp(operation, "release") == 0) result = security.releaseLease(clientId, millis());
	else{
		sendError(400, "invalid_lease_operation");
		return;
	}
	if(result != WirelessApi::LEASE_OK){
		if(result == WirelessApi::LEASE_CONFLICT) sendError(409, "writer_lease_conflict");
		else if(result == WirelessApi::LEASE_EXPIRED) sendError(409, "writer_lease_expired");
		else sendError(403, "writer_lease_not_owner");
		return;
	}
	sendLeaseState(200, operation);
}

bool parseUint8(const WirelessApi::JsonObject& object, const char* name, uint8_t& value){
	uint64_t parsed = 0;
	if(!object.getNumber(name, parsed) || parsed > UINT8_MAX) return false;
	value = static_cast<uint8_t>(parsed);
	return true;
}

bool parseUint16(const WirelessApi::JsonObject& object, const char* name, uint16_t& value){
	uint64_t parsed = 0;
	if(!object.getNumber(name, parsed) || parsed > UINT16_MAX) return false;
	value = static_cast<uint16_t>(parsed);
	return true;
}

void handleCommand(){
	char clientId[WirelessApi::CLIENT_ID_CAPACITY] = {};
	if(!authorize(true, clientId)) return;
	if(!security.hasWriterLease(clientId, millis())){
		sendError(409, "writer_lease_required");
		return;
	}
	WirelessApi::JsonObject object;
	if(!requireJsonBody(object)) return;
	static const char* allowed[] = {
		"boot_id", "session_id", "client_command_id", "action", "deck", "slot", "value"
	};
	if(!object.hasOnly(allowed, sizeof(allowed) / sizeof(allowed[0]))){
		sendError(400, "invalid_fields");
		return;
	}

	uint64_t bootId = 0;
	uint64_t sessionId = 0;
	const char* bootIdText = nullptr;
	const char* commandId = nullptr;
	const char* action = nullptr;
	// boot_id must arrive as the opaque decimal string handleState() emits
	// (see there for why) - never as a bare JSON number, and never
	// Number-coerced. An exact digit-by-digit parse rejects malformed or
	// overflowing input outright rather than truncating/wrapping it into a
	// value that could accidentally match.
	if(!object.getString("boot_id", bootIdText) || !WirelessApi::parseUint64Decimal(bootIdText, bootId) ||
	   !object.getNumber("session_id", sessionId) ||
	   sessionId > UINT32_MAX || !object.getString("client_command_id", commandId) ||
	   !WirelessApi::validClientCommandId(commandId) || !object.getString("action", action)){
		sendError(400, "invalid_fields");
		return;
	}

	DjCommand command = {};
	command.origin = DJ_ORIGIN_HTTP;
	command.requestBootId = bootId;
	command.requestSessionId = static_cast<uint32_t>(sessionId);
	memcpy(command.clientId, clientId, strlen(clientId) + 1);
	memcpy(command.clientCommandId, commandId, strlen(commandId) + 1);

	uint8_t expectedFields = 5;
	if(strcmp(action, "set_mix") == 0){
		command.type = DJ_COMMAND_SET_MIX;
		if(!parseUint16(object, "value", command.value) || command.value > 255) expectedFields = 0;
	}else if(strcmp(action, "set_recording") == 0){
		bool value = false;
		command.type = DJ_COMMAND_SET_RECORDING;
		if(!object.getBool("value", value)) expectedFields = 0;
		command.value = value;
	}else{
		expectedFields = 6;
		if(!parseUint8(object, "deck", command.deck)) expectedFields = 0;
		if(strcmp(action, "set_playing") == 0){
			bool value = false;
			command.type = DJ_COMMAND_SET_PLAYING;
			if(!object.getBool("value", value)) expectedFields = 0;
			command.value = value;
		}else if(strcmp(action, "seek") == 0){
			command.type = DJ_COMMAND_SEEK;
			if(!parseUint16(object, "value", command.value)) expectedFields = 0;
		}else if(strcmp(action, "set_gain") == 0){
			command.type = DJ_COMMAND_SET_GAIN;
			if(!parseUint16(object, "value", command.value) || command.value > 255) expectedFields = 0;
		}else if(strcmp(action, "set_effect_type") == 0 ||
				 strcmp(action, "set_effect_intensity") == 0){
			expectedFields = 7;
			command.type = strcmp(action, "set_effect_type") == 0 ?
				DJ_COMMAND_SET_EFFECT_TYPE : DJ_COMMAND_SET_EFFECT_INTENSITY;
			if(!parseUint8(object, "slot", command.slot) ||
			   !parseUint16(object, "value", command.value) || command.value > 255){
				expectedFields = 0;
			}
		}else{
			sendError(400, "unsupported_action");
			return;
		}
	}
	if(expectedFields == 0 || object.size() != expectedFields){
		sendError(400, "invalid_fields");
		return;
	}

	DjSession* session = DjSession::get();
	if(!session){
		sendError(409, "session_unavailable");
		return;
	}
	sendCommandResult(session->submit(command), commandId);
}

void handleOta(){
	char clientId[WirelessApi::CLIENT_ID_CAPACITY] = {};
	if(!authorize(true, clientId)) return;
	sendError(501, "ota_requires_recovery_partition");
}

bool validResultPath(const String& uri, const char*& commandId){
	static const char prefix[] = "/api/v2/commands/";
	if(!uri.startsWith(prefix)) return false;
	commandId = uri.c_str() + sizeof(prefix) - 1;
	return WirelessApi::validClientCommandId(commandId);
}

void handleNotFound(){
	const String uri = server.uri();
	const char* commandId = nullptr;
	if(server.method() == HTTP_GET && validResultPath(uri, commandId)){
		char clientId[WirelessApi::CLIENT_ID_CAPACITY] = {};
		if(!authorize(false, clientId)) return;
		DjSnapshot snapshot = {};
		if(!copySnapshot(snapshot)){
			sendError(409, "session_unavailable");
			return;
		}
		for(uint8_t i = 0; i < DJ_RECENT_RESULT_COUNT; i++){
			const DjCommandResult& result = snapshot.recentResults[i];
			if(strcmp(result.clientId, clientId) != 0 ||
			   strcmp(result.clientCommandId, commandId) != 0) continue;
			DjSubmitResult response(
				result.id, result.status, result.error, true
			);
			sendCommandResult(response, commandId);
			return;
		}
		sendError(404, "command_result_not_found");
		return;
	}

	static const char* known[] = {
		"/", "/setup", "/setup/wifi", "/api/v2/pair", "/api/v2/capabilities",
		"/api/v2/health", "/api/v2/state", "/api/v2/pairing", "/api/v2/lease",
		"/api/v2/command", "/api/v2/ota"
	};
	for(const char* path : known){
		if(uri == path){
			sendError(405, "method_not_allowed");
			return;
		}
	}
	sendError(404, "not_found");
}

String setupPassword(){
	String password = preferences.getString("ap_password", "");
	if(password.length() == 12) return password;
	char generated[13];
	snprintf(generated, sizeof(generated), "%08lX%04lX",
		static_cast<unsigned long>(esp_random()),
		static_cast<unsigned long>(esp_random() & 0xFFFF));
	preferences.putString("ap_password", generated);
	return String(generated);
}

void startServer(){
	if(serverStarted) return;
	static const char* headers[] = {
		"Authorization", "Content-Type", "Content-Length", "Host", "Origin", "Transfer-Encoding"
	};
	server.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
	server.on("/", HTTP_GET, handleIndex);
	server.on("/setup", HTTP_GET, handleSetupStatus);
	server.on("/setup/wifi", HTTP_POST, handleSetupWifi);
	server.on("/api/v2/pair", HTTP_POST, handlePair);
	server.on("/api/v2/capabilities", HTTP_GET, handleCapabilities);
	server.on("/api/v2/health", HTTP_GET, handleHealth);
	server.on("/api/v2/state", HTTP_GET, handleState);
	server.on("/api/v2/pairing", HTTP_GET, handlePairingState);
	server.on("/api/v2/lease", HTTP_GET, handleLeaseGet);
	server.on("/api/v2/lease", HTTP_POST, handleLeasePost);
	server.on("/api/v2/command", HTTP_POST, handleCommand);
	server.on("/api/v2/ota", HTTP_POST, handleOta);
	server.onNotFound(handleNotFound);
	server.begin();
	serverStarted = true;
#if defined(JAYD_WIRELESS_DEBUG)
	Serial.printf(
		"Wireless debug: free heap %u, free PSRAM %u\n",
		ESP.getFreeHeap(),
		ESP.getFreePsram()
	);
#endif
}

void startSetupAp(){
	char suffix[7];
	snprintf(suffix, sizeof(suffix), "%06llX", ESP.getEfuseMac() & 0xFFFFFF);
	const String ssid = String("Jay-D-") + suffix;
	WiFi.disconnect(true);
	WiFi.mode(WIFI_AP);
	const String password = setupPassword();
	setupMode = WiFi.softAP(ssid.c_str(), password.c_str());
	if(!setupMode){
		Serial.println("Wireless: setup AP failed");
		return;
	}
	Serial.printf("Wireless: setup AP %s\n", ssid.c_str());
	Serial.printf("Wireless: setup AP password %s\n", password.c_str());
	startServer();
}

void observePairingRequest(){
	DjSnapshot snapshot = {};
	if(!copySnapshot(snapshot)) return;
	if(snapshot.sessionId != lastPairingSessionId){
		lastPairingSessionId = snapshot.sessionId;
		lastPairingGeneration = 0;
	}
	if(snapshot.pairingGeneration == 0 || snapshot.pairingGeneration == lastPairingGeneration) return;
	lastPairingGeneration = snapshot.pairingGeneration;
	if(!security.openPairing(millis(), randomWord)) return;
#if defined(JAYD_WIRELESS_DEBUG)
	char code[WirelessApi::PAIRING_CODE_CAPACITY] = {};
	if(security.copyPairingCode(code, millis())){
		Serial.printf("Wireless debug: pairing code %s (60 seconds)\n", code);
	}
#endif
}

void debugPairingHook(){
#if defined(JAYD_WIRELESS_DEBUG)
	if(!Serial.available()) return;
	if(Serial.read() != 'P') return;
	DjSession* session = DjSession::get();
	if(session) session->requestPairing(DJ_ORIGIN_PHYSICAL);
#endif
}

}

void WirelessBringup::begin(){
	if(ESP.getPsramSize() != 0) heap_caps_malloc_extmem_enable(4096);
	if(!preferences.begin("wireless", false)){
		Serial.println("Wireless: NVS unavailable");
		return;
	}
	security.initialize(tokenStore);
	stationSsid = preferences.getString("ssid", "");
	stationPassword = preferences.getString("password", "");
	if(stationSsid.length() == 0){
		startSetupAp();
		return;
	}
	WiFi.mode(WIFI_STA);
	WiFi.begin(stationSsid.c_str(), stationPassword.c_str());
	stationDeadline = millis() + STATION_TIMEOUT_MS;
	Serial.printf("Wireless: connecting to %s\n", stationSsid.c_str());
}

void WirelessBringup::loop(){
	if(!setupMode && !serverStarted && WiFi.status() == WL_CONNECTED){
		Serial.printf("Wireless: connected, IP %s\n", WiFi.localIP().toString().c_str());
		startServer();
	}
	if(!setupMode && !serverStarted && stationDeadline != 0 &&
	   static_cast<int32_t>(millis() - stationDeadline) >= 0){
		Serial.println("Wireless: connection timeout, entering setup mode");
		startSetupAp();
	}
	if(serverStarted) server.handleClient();
	debugPairingHook();
	observePairingRequest();
	if(restartAt != 0 && static_cast<int32_t>(millis() - restartAt) >= 0) ESP.restart();
}

bool WirelessBringup::copyPairingStatus(WirelessPairingStatus& status){
	status = {};
	const uint32_t now = millis();
	status.remainingMs = security.pairingRemaining(now);
	status.open = status.remainingMs != 0 && security.copyPairingCode(status.code, now);
	if(!status.open) status.remainingMs = 0;
	return status.open;
}

#else

void WirelessBringup::begin(){}
void WirelessBringup::loop(){}
bool WirelessBringup::copyPairingStatus(WirelessPairingStatus& status){
	status = {};
	return false;
}

#endif
