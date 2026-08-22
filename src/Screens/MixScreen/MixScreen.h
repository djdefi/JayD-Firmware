#ifndef JAYD_FIRMWARE_MIXSCREEN_H
#define JAYD_FIRMWARE_MIXSCREEN_H

#include <Support/Context.h>
#include <UI/LinearLayout.h>
#include "SongSeekBar.h"
#include "SongName.h"
#include "EffectElement.h"
#include "MatrixPopUpPicker.h"
#include <Matrix/VuVisualizer.h>
#include <Matrix/RoundVuVisualiser.h>
#include <Input/InputJayD.h>
#include "../../InputKeys.h"
#include "../../DjSession/DjSession.h"

namespace MixScreen {
	class MixScreen : public Context, public LoopListener, public JayDInputListener, public InputListener {
		friend MatrixPopUpPicker;
	public:

		MixScreen(Display& display);

		void start();

		void stop();

		void draw();

		void returned(void* data) override;

		void loop(uint micros) override;

		virtual ~MixScreen();
		void pack() override;
		void unpack() override;
		void setBigVuStarted(bool bigVuStarted);

	private:
		static MixScreen* instance;

		Color *selectedBackgroundBuffer = nullptr;
		DjSession* session = nullptr;
		uint8_t loadingChannel = 0;
		bool songListOpen = false;
		char displayedPaths[DJ_DECK_COUNT][DJ_PATH_CAPACITY] = {};

		bool loadChannel(uint8_t channel, const String& path);
		bool syncFromSnapshot(const DjSnapshot& snapshot, bool force = false);

		LinearLayout* screenLayout;
		LinearLayout* leftLayout;
		LinearLayout* rightLayout;

		SongSeekBar* leftSeekBar;
		SongSeekBar* rightSeekBar;

		SongName* leftSongName;
		SongName* rightSongName;

		EffectElement* effectElements[6] = {nullptr};

		void buildUI();

		uint8_t selectedChannel = 0;
		bool isRecording = false;
		bool doneRecording = false;
		String saveFilename;
		void saveRecording();
		void drawSaveStatus();


		uint32_t lastDraw = 0;
		bool drawQueued = false;

		uint32_t seekTime = 0;
		bool wasRunning = false;
		int8_t seekChannel = -1;

		VuVisualizer leftVu;
		VuVisualizer rightVu;
		RoundVuVisualiser midVu;

		bool bigVuStarted = true;

		void startBigVu();
		void stopBigVu();

		void potMove(uint8_t id, uint8_t value) override;

		void encTwoBot() override;
		void encTwoTop() override;
		void btnCombination() override;
		void btn(uint8_t i) override;
		void btnEnc(uint8_t i) override;
		void enc(uint8_t id, int8_t value) override;
		void encBtnHold(uint8_t i) override;

	};
}


#endif //JAYD_FIRMWARE_MIXSCREEN_H
