#ifndef JAYD_FIRMWARE_SONGLIST_H
#define JAYD_FIRMWARE_SONGLIST_H

#include <Support/Context.h>
#include <UI/Screen.h>
#include <UI/Image.h>
#include <Input/InputJayD.h>
#include "../../InputKeys.h"

namespace SongList {
	class SongList : public Context, public LoopListener, public InputListener {
	public:

		explicit SongList(Display &display);

		virtual ~SongList() override;

		void start() override;

		void stop() override;

		void draw() override;

		void loop(uint t) override;

		void pack() override;

		void unpack() override;

	private:
		static SongList *instance;

		int selectedElement = 0;
		int firstVisible = 0;

		Color *backgroundBuffer = nullptr;
		char* pathBuffer = nullptr;
		uint32_t* songOffsets = nullptr;
		size_t pathBytes = 0;
		size_t pathCapacity = 0;
		size_t songCount = 0;
		size_t songCapacity = 0;

		void clearSongs();
		void checkSD();
		bool searchDirectories(File dir);
		bool addSong(const char* path);
		bool reservePaths(size_t required);
		bool reserveSongs(size_t required);
		const char* songPath(size_t index) const;

		void encTwoTop() override;
		bool waiting = false;
		bool insertedSD = true;
		bool empty = true;
		bool scanLimited = false;
		bool allocationFailed = false;

		static const size_t maxTrackCount = 4096;
		static const size_t maxPathLength = 255;
		static const size_t maxPathPayload = 128 * 1024;
		static const uint8_t visibleRows = 5;
		static const uint8_t rowHeight = 20;
	};
}
#endif //JAYD_FIRMWARE_SONGLIST_H
