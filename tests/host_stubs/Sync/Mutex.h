#ifndef JAYD_HOST_STUB_SYNC_MUTEX_H
#define JAYD_HOST_STUB_SYNC_MUTEX_H

// Host-test stand-in for CircuitOS's Sync/Mutex.h. DjAssistIntegrationSelfCheck
// drives DjAssistController single-threaded and deterministically (the real
// background fill Task is never started under the Task.h stub - see that
// header), so a real semaphore is unnecessary here; this exists purely so
// the real, unmodified DjAssistController.cpp/DjSession.h compile for the
// host build. lock()/unlock() are trivial no-ops.
class Mutex {
public:
	Mutex(){}
	~Mutex(){}

	bool lock(){ return true; }
	void unlock(){}
};

#endif
