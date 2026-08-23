#include "Arduino.h"

#include <cstdlib>

namespace {
unsigned long g_hostMicros = 0;
}

unsigned long micros(){
	return g_hostMicros;
}

void delay(uint32_t){
	// No real sleeping in host tests - fillTaskTrampoline's background
	// while(task->running) loop is never invoked directly (the Task stub
	// never spawns a thread); DjAssistIntegrationSelfCheck calls
	// fillWorkerStep() itself, one bounded step at a time. This exists
	// only so the reference in that trampoline still links.
}

void* ps_malloc(size_t size){
	return malloc(size);
}

void hostStubSetMicros(unsigned long value){
	g_hostMicros = value;
}

void hostStubAdvanceMicros(unsigned long deltaMicros){
	g_hostMicros += deltaMicros;
}
