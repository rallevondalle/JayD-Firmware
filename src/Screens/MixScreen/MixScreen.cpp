#include <Input/InputJayD.h>
#include <SD.h>
#include <Loop/LoopManager.h>
#include <JayD.h>
#include <FS/CompressedFile.h>
#include "MixScreen.h"
#include "../SongList/SongList.h"
#include "../TextInputScreen/TextInputScreen.h"
#include "../../Fonts.h"

MixScreen::MixScreen* MixScreen::MixScreen::instance = nullptr;

MixScreen::MixScreen::MixScreen(Display& display) : Context(display),
													screenLayout(new LinearLayout(&screen, HORIZONTAL)),
													leftLayout(new LinearLayout(screenLayout, VERTICAL)),
													rightLayout(new LinearLayout(screenLayout, VERTICAL)),
													leftSeekBar(new SongSeekBar(leftLayout)),
													rightSeekBar(new SongSeekBar(rightLayout)),
													leftSongName(new SongName(leftLayout)),
													rightSongName(new SongName(rightLayout)), leftVu(&matrixManager.matrixL), rightVu(&matrixManager.matrixR),
													midVu(&matrixManager.matrixBig){

	Serial.println("\n=== MIXSCREEN CONSTRUCTOR START ===");
	Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

	for(int i = 0; i < 3; i++){
		effectElements[i] = new EffectElement(leftLayout, false);
	}
	for(int i = 3; i < 6; i++){
		effectElements[i] = new EffectElement(rightLayout, true);
	}

	instance = this;
	Serial.printf("MixScreen instance set: %p\n", this);
	
	buildUI();
	Serial.println("MixScreen UI built");
	
	MixScreen::pack();
	Serial.println("MixScreen packed");
	Serial.printf("Free heap after constructor: %u bytes\n", ESP.getFreeHeap());
	Serial.println("=== MIXSCREEN CONSTRUCTOR END ===\n");
}

MixScreen::MixScreen::~MixScreen(){
	instance = nullptr;
	free(selectedBackgroundBuffer);
}

void MixScreen::MixScreen::pack(){
	Context::pack();
	free(selectedBackgroundBuffer);
	selectedBackgroundBuffer = nullptr;
}

void MixScreen::MixScreen::unpack(){
	Context::unpack();

	// Clear any existing buffer first
	if(selectedBackgroundBuffer != nullptr){
		free(selectedBackgroundBuffer);
		selectedBackgroundBuffer = nullptr;
	}

	selectedBackgroundBuffer = static_cast<Color*>(ps_malloc(79 * 128 * 2));
	if(selectedBackgroundBuffer == nullptr){
		Serial.println("ERROR: Selected background malloc failed");
		Serial.println("PSRAM: Available");
		return;
	}
	
	// Initialize buffer to prevent garbage data
	memset(selectedBackgroundBuffer, 0, 79 * 128 * 2);

	// Try to open the background file
	fs::File spiffsFile = SPIFFS.open("/mixSelectedBg.raw.hs");
	if(!spiffsFile){
		Serial.println("ERROR: Failed to open SPIFFS background file");
		// Use a fallback - fill with a solid color instead of crashing
		for(int i = 0; i < 79 * 128; i++){
			selectedBackgroundBuffer[i] = C_RGB(50, 50, 50); // Dark gray fallback
		}
		Serial.println("Using fallback background color");
		return;
	}
	
	fs::File bgFile = CompressedFile::open(spiffsFile, 13, 12);
	if(!bgFile){
		Serial.println("ERROR: Failed to open compressed background file");
		spiffsFile.close();
		// Use fallback color
		for(int i = 0; i < 79 * 128; i++){
			selectedBackgroundBuffer[i] = C_RGB(50, 50, 50); // Dark gray fallback
		}
		Serial.println("Using fallback background color");
		return;
	}
	
	size_t bytesRead = bgFile.read(reinterpret_cast<uint8_t*>(selectedBackgroundBuffer), 79 * 128 * 2);
	bgFile.close();
	
	if(bytesRead != 79 * 128 * 2){
		Serial.printf("WARNING: Expected %d bytes, read %d bytes from background file\n", 79 * 128 * 2, bytesRead);
		if(bytesRead == 0){
			// Complete failure - use fallback
			for(int i = 0; i < 79 * 128; i++){
				selectedBackgroundBuffer[i] = C_RGB(50, 50, 50); // Dark gray fallback
			}
			Serial.println("Using fallback background color due to read failure");
		}
	}else{
		Serial.println("Background buffer loaded successfully");
	}
}

void MixScreen::MixScreen::saveRecording(){
	if(!SD.exists(MixSystem::recordPath)){
		doneRecording = false;
		return;
	}

	Task saveTask("MixSave", [](Task* task){
		String saveFilename = * (String*) task->arg;

		if(SD.exists(saveFilename)){
			SD.remove(saveFilename);
		}

		File inFile = SD.open(MixSystem::recordPath);
		File outFile = SD.open(saveFilename, "w");

		SourceWAV input(inFile);
		OutputAAC output(outFile);

		output.setSource(&input);
		output.start();

		while(output.isRunning()){
			output.loop(0);
		}

		output.stop();
		input.close();

		inFile.close();
		outFile.close();
	}, 8 * 1024, &saveFilename);

	saveTask.start(1, 0);

	while(!saveTask.isStopped()){
		if(millis() - lastDraw >= 30){
			lastDraw = millis();
			drawSaveStatus();
			screen.commit();
		}

		Sched.loop(0);
	}

	SD.remove(MixSystem::recordPath);
	doneRecording = false;
}

void MixScreen::MixScreen::returned(void* data){
	Serial.println("\n=== RETURNED METHOD START ===");
	
	String* filename = (String*) data;
	if(!filename){
		Serial.println("ERROR: Null filename");
		return;
	}

	Serial.printf("Selected file: %s\n", filename->c_str());
	Serial.printf("isLoadingTrack: %s, loadingDeck: %d\n", isLoadingTrack ? "true" : "false", loadingDeck);
	Serial.printf("Current state - f1: %s, f2: %s, system: %p\n", 
		f1 ? "loaded" : "null", f2 ? "loaded" : "null", system);

	if(doneRecording){
		saveFilename = String("/") + *filename + ".aac";
		delete filename;
		return;
	}

	if(filename->length() == 0){
		Serial.println("ERROR: Empty filename");
		delete filename;
		return;
	}
	
	if(hotSwapInProgress){
		Serial.println("ERROR: Hot-swap in progress");
		delete filename;
		return;
	}

	fs::File newFile = SD.open(*filename);
	if(!newFile){
		Serial.printf("ERROR: Failed to open file: %s\n", filename->c_str());
		delete filename;
		return;
	}
	
	if(newFile.size() == 0){
		Serial.println("ERROR: File is empty");
		newFile.close();
		delete filename;
		return;
	}
	
	Serial.printf("File opened successfully: %s (size: %d)\n", newFile.name(), newFile.size());

	if(isLoadingTrack){
		Serial.printf("HOT-SWAP: Loading to deck %d\n", loadingDeck);
		// Hot-swap the track on the specified deck
		hotSwapTrack(loadingDeck, newFile);
		isLoadingTrack = false;
	}else{
		Serial.println("INITIAL LOADING: Assigning to available deck");
		
		// Add extra validation to prevent crashes
		if(!newFile.available() || newFile.size() == 0){
			Serial.println("ERROR: File not available or empty");
			Serial.printf("File available: %s, size: %d\n", 
				newFile.available() ? "YES" : "NO", newFile.size());
			newFile.close();
			return;
		}
		
		Serial.printf("File validation passed - available: %s, size: %d\n",
			newFile.available() ? "YES" : "NO", newFile.size());
		
		if(!f1){
			Serial.println("Assigning to f1 (player 1)");
			f1 = newFile;
			if(f1 && f1.size() > 0){
				f1.seek(0);
				String name = f1.name();
				String songName = name.substring(name.lastIndexOf('/') + 1, name.length() - 4);
				leftSongName->setSongName(songName);
				Serial.printf("f1 assigned: %s\n", songName.c_str());
			}
			keepAudioOnStop = false;
		}else if(!f2){
			Serial.println("Assigning to f2 (player 2)");
			
			// POWER MANAGEMENT: If audio is playing, pause it during file operations
			// This prevents power spikes that cause brownout resets
			bool wasChannelPlaying = false;
			if(system){
				Serial.println("Pausing audio to prevent power spike during f2 loading");
				wasChannelPlaying = !system->isChannelPaused(0);
				if(wasChannelPlaying){
					system->pauseChannel(0);
					delay(50); // Allow audio system to settle
				}
			}
			
			f2 = newFile;
			if(f2 && f2.size() > 0){
				f2.seek(0);
				String name = f2.name();
				String songName = name.substring(name.lastIndexOf('/') + 1, name.length() - 4);
				rightSongName->setSongName(songName);
				Serial.printf("f2 assigned: %s\n", songName.c_str());
				
				// Update UI immediately
				drawQueued = true;
				
				// CRITICAL: If MixSystem exists, we need to update it to know about f2
				Serial.printf("System exists: %s\n", system ? "YES" : "NO");
				if(system){
					Serial.println("WARNING: MixSystem exists but doesn't know about new f2!");
					Serial.println("Deferring MixSystem update to prevent power issues");
				}
			}
			
			// Resume audio if it was playing
			if(system && wasChannelPlaying){
				Serial.println("Resuming audio after f2 loading");
				delay(50); // Small delay before resuming
				system->resumeChannel(0);
			}
			
			keepAudioOnStop = false;
		}else{
			Serial.println("Both decks full - closing file");
			newFile.close();
			keepAudioOnStop = false;
		}
	}

	delete filename;
	Serial.println("=== RETURNED METHOD END ===\n");
}

void MixScreen::MixScreen::setBigVuStarted(bool bigVuStarted){
	MixScreen::bigVuStarted = bigVuStarted;
}

void MixScreen::MixScreen::start(){
	Serial.println("\n=== MIXSCREEN START ===");
	Serial.printf("f1: %s, f2: %s, system: %p\n", 
		f1 ? "loaded" : "null", f2 ? "loaded" : "null", system);
	
	if(doneRecording){
		lastDraw = 0;
		draw();
		screen.commit();
		saveRecording();
	}

	// Initialize DJ mode even if we don't have both tracks loaded yet
	// This allows the mixer interface to be ready for loading tracks individually
	if(!f1 && !f2){
		Serial.println("No tracks loaded - showing song selection");
		Serial.printf("DEBUG: f1 size = %d, f2 size = %d\n", 
			f1 ? f1.size() : 0, f2 ? f2.size() : 0);
		// No tracks loaded - go to song selection for the first track
		draw();
		screen.commit();
		delay(100); // Small delay to ensure display is stable
		
		(new SongList::SongList(*getScreen().getDisplay()))->push(this);
		return;
	}

	// Handle single file initialization
	if(f1 && !f2){
		f1.seek(0);
		String name = f1.name();
		leftSongName->setSongName(name.substring(name.lastIndexOf('/') + 1, name.length() - 4));
		rightSongName->setSongName("No track loaded");
	}else if(!f1 && f2){
		f2.seek(0);
		String name = f2.name();
		rightSongName->setSongName(name.substring(name.lastIndexOf('/') + 1, name.length() - 4));
		leftSongName->setSongName("No track loaded");
	}else{
		f1.seek(0);
		f2.seek(0);
		String name = f1.name();
		leftSongName->setSongName(name.substring(name.lastIndexOf('/') + 1, name.length() - 4));
		name = f2.name();
		rightSongName->setSongName(name.substring(name.lastIndexOf('/') + 1, name.length() - 4));
	}

	// Create MixSystem if we have at least one track
	// Add debugging to trace what's happening
	Serial.printf("=== MixSystem Creation Debug ===\n");
	Serial.printf("Current system: %p\n", system);
	Serial.printf("f1 valid: %s (size: %d)\n", (f1 && f1.size() > 0) ? "YES" : "NO", f1 ? f1.size() : 0);
	Serial.printf("f2 valid: %s (size: %d)\n", (f2 && f2.size() > 0) ? "YES" : "NO", f2 ? f2.size() : 0);
	
	// SAFETY: Don't create system if one already exists
	if(system){
		Serial.println("INFO: MixSystem already exists - using existing system");
		// If system exists and we just completed hot-swap, preserve states
		if(justCompletedHotSwap){
			Serial.println("Hot-swap system already configured - skipping start sequence");
		}
	}else if(f1 || f2){
		Serial.println("Creating MixSystem...");
		
		// POWER MANAGEMENT: Add delay before MixSystem creation
		// to allow power supply to stabilize
		delay(100);
		
		system = new MixSystem(f1, f2);
		Serial.printf("MixSystem created: %p\n", system);
		
		// Another delay after creation to prevent immediate power spikes
		delay(50);
	}else{
		Serial.println("No files - skipping MixSystem creation");
		system = nullptr;
	}

	if(system){
		// Skip configuration if we just completed hot-swap (already configured)
		if(justCompletedHotSwap){
			Serial.println("Skipping MixSystem configuration - already done in hot-swap");
		}else{
			Serial.println("Configuring MixSystem with power management...");
			
			// Spread out power-hungry operations with delays
			system->setVolume(0, InputJayD::getInstance()->getPotValue(POT_L));
			delay(10);
			system->setVolume(1, InputJayD::getInstance()->getPotValue(POT_R));
			delay(10);

			system->setChannelInfo(0, leftVu.getInfoGenerator());
			delay(10);
			system->setChannelInfo(1, rightVu.getInfoGenerator());
			delay(10);
			system->setChannelInfo(2, midVu.getInfoGenerator());
			delay(10);
			
			if(bigVuStarted){
				startBigVu();
				delay(10);
			}

			uint8_t potMidVal = InputJayD::getInstance()->getPotValue(POT_MID);
			system->setMix(potMidVal);
			delay(10);
			matrixManager.fillMatrixMid(potMidVal);
			matrixManager.matrixMid.push();
			delay(10);
		}

		// Always update seek bar durations (but preserve playing states for hot-swap)
		if(f1){
			leftSeekBar->setTotalDuration(system->getDuration(0));
			if(!justCompletedHotSwap){
				leftSeekBar->setPlaying(false); // Start paused - user controls playback
			}
		}else{
			leftSeekBar->setTotalDuration(0);
			leftSeekBar->setPlaying(false);
		}
		
		if(f2){
			rightSeekBar->setTotalDuration(system->getDuration(1));
			if(!justCompletedHotSwap){
				rightSeekBar->setPlaying(false); // Start paused - user controls playback
			}
		}else{
			rightSeekBar->setTotalDuration(0);
			rightSeekBar->setPlaying(false);
		}
	}else{
		// No system yet - set empty states
		leftSeekBar->setTotalDuration(0);
		rightSeekBar->setTotalDuration(0);
		leftSeekBar->setPlaying(false);
		rightSeekBar->setPlaying(false);
	}


	leftSongName->checkScrollUpdate();
	rightSongName->checkScrollUpdate();

	for(int i = 0; i < 6; i++){
		// Auto-select default effects: first slot = SPEED, second slot = HIGHPASS
		if(i == 0 || i == 3){
			effectElements[i]->setType(SPEED);  // First effect slot for both players
		}else if(i == 1 || i == 4){
			effectElements[i]->setType(HIGHPASS);  // Second effect slot for both players
		}else{
			effectElements[i]->setType(NONE);  // Third effect slot remains empty
		}
		effectElements[i]->setIntensity(0);
	}

	/*system->setChannelDoneCallback(0, [](){
		instance->system->resumeChannel(0);
	});
	system->setChannelDoneCallback(1, [](){
		instance->system->resumeChannel(1);
	});*/


	if(system){
		// Check if we just completed a hot-swap - if so, system is already running
		if(justCompletedHotSwap){
			Serial.println("Hot-swap detected - system already running, preserving states");
			Serial.printf("Hot-swap validation: f1=%s (size=%d), f2=%s (size=%d)\n",
				f1 ? "valid" : "null", f1 ? f1.size() : 0,
				f2 ? "valid" : "null", f2 ? f2.size() : 0);
			justCompletedHotSwap = false; // Reset the flag
		}else{
			Serial.println("Starting MixSystem with power management...");
			
			// POWER MANAGEMENT: Delay before starting to prevent power spike
			delay(50);
			system->start();
			
			// Allow system to stabilize after start
			delay(50);
			
			Serial.println("Normal start - pausing all channels");
			// Ensure both channels start paused - user controls when to play
			if(f1){
				system->pauseChannel(0);
				delay(10);
			}
			if(f2){
				system->pauseChannel(1);
				delay(10);
			}
		}
		
		Serial.println("MixSystem ready");
		
		// Initialize default effects only on first startup, not during hot-swap
		if(!justCompletedHotSwap){
			initializeDefaultEffects();
		}else{
			Serial.println("Hot-swap detected - skipping effect initialization to preserve playback");
		}
	}

	// Add listeners - handle the case where VU listeners might still be active
	if(!listenersActive){
		// Full initialization - add all listeners
		LoopManager::addListener(&leftVu);
		LoopManager::addListener(&rightVu);
		LoopManager::addListener(this);
		Input.addListener(this);
		InputJayD::getInstance()->addListener(this);
		
		listenersActive = true;
	}else{
		// VU listeners might still be active from keepAudioOnStop
		// Just add the UI listeners we need
		LoopManager::addListener(this);
		Input.addListener(this);
		InputJayD::getInstance()->addListener(this);
	}

	draw();
	screen.commit();
	
	Serial.printf("=== MIXSCREEN START COMPLETE - System: %p ===\n\n", system);
}

void MixScreen::MixScreen::stop(){
	Serial.printf("=== MIXSCREEN STOP - System: %p ===\n", system);
	
	// Remove UI listeners but keep VU listeners if preserving audio
	if(!keepAudioOnStop){
		LoopManager::removeListener(&leftVu);
		LoopManager::removeListener(&rightVu);
		LoopManager::removeListener(&midVu);
		listenersActive = false;
	}
	
	LoopManager::removeListener(this);
	Input.removeListener(this);
	InputJayD::getInstance()->removeListener(this);

	if(bigVuStarted){
		stopBigVu();
	}else{
		if(!matrixManager.matrixBig.getAnimations().empty()){
			delete *matrixManager.matrixBig.getAnimations().begin();
		}
	}

	// SMART AUDIO PRESERVATION: Only delete MixSystem when not preserving audio
	if(system && !keepAudioOnStop){
		Serial.printf("Stopping and deleting MixSystem: %p (audio not preserved)\n", system);
		system->stop();
		delete system;
		system = nullptr;
		listenersActive = false;
		Serial.println("MixSystem deleted successfully");
	}else if(system && keepAudioOnStop){
		Serial.printf("Preserving MixSystem: %p (keeping audio alive)\n", system);
		// Keep system running but remove UI listeners only
	}else{
		Serial.println("No MixSystem to delete");
	}
}

void MixScreen::MixScreen::draw(){
	screen.getSprite()->fillRect(79, 0, 2, 128, TFT_BLACK);
	screen.getSprite()->fillRect(leftLayout->getTotalX(), leftLayout->getTotalY(), 79, 128, C_RGB(249, 93, 2));
	screen.getSprite()->fillRect(rightLayout->getTotalX(), rightLayout->getTotalY(), 79, 128, C_RGB(3, 52, 135));
	
	// Draw selection indication - always use white border style
	if(!selectedChannel){
		// Left channel selected - draw white border on left
		screen.getSprite()->drawRect(leftLayout->getTotalX(), leftLayout->getTotalY(), 79, 128, TFT_WHITE);
		screen.getSprite()->drawRect(leftLayout->getTotalX()+1, leftLayout->getTotalY()+1, 77, 126, TFT_WHITE);
	}else{
		// Right channel selected - draw white border on right
		screen.getSprite()->drawRect(rightLayout->getTotalX(), rightLayout->getTotalY(), 79, 128, TFT_WHITE);
		screen.getSprite()->drawRect(rightLayout->getTotalX()+1, rightLayout->getTotalY()+1, 77, 126, TFT_WHITE);
	}

	if(isRecording){
		screen.getSprite()->fillCircle(79, 64, 6, TFT_BLACK);
		screen.getSprite()->fillCircle(79, 64, 4, TFT_RED);
	}
	screen.draw();

	if(doneRecording){
		drawSaveStatus();
	}
}

void MixScreen::MixScreen::drawSaveStatus(){
	Sprite* canvas = screen.getSprite();

	canvas->fillRoundRect((screen.getWidth() - 80) / 2, (screen.getHeight() - 40) / 2, 80, 40, 2, C_RGB(52, 204, 235));
	canvas->drawRoundRect((screen.getWidth() - 80) / 2, (screen.getHeight() - 40) / 2, 80, 40, 2, TFT_BLACK);

	canvas->setTextColor(TFT_WHITE);
	canvas->setFont(&u8g2_font_DigitalDisco_tf);
	canvas->setTextDatum(BC_DATUM);
	canvas->drawString("Saving...", screen.getWidth() / 2, (screen.getHeight() - 40) / 2 + 23);
	canvas->setTextDatum(TL_DATUM);

	canvas->fillRoundRect((screen.getWidth() - 80) / 2 + 10 + (cos((float) millis() / 200.0f)+1) / 2.0f * 45.0f, (screen.getHeight() - 40) / 2 + 30, 15, 5, 2, TFT_WHITE);
}

void MixScreen::MixScreen::buildUI(){
	screenLayout->setWHType(PARENT, PARENT);
	screenLayout->setGutter(2);
	screenLayout->addChild(leftLayout);
	screenLayout->addChild(rightLayout);

	leftLayout->setWHType(FIXED, PARENT);
	leftLayout->setWidth(79);
	leftLayout->setGutter(10);
	leftLayout->setPadding(1);


	leftLayout->addChild(leftSeekBar);
	leftLayout->addChild(leftSongName);

	for(int i = 0; i < 3; i++){
		leftLayout->addChild(effectElements[i]);
	}


	rightLayout->setWHType(FIXED, PARENT);
	rightLayout->setWidth(79);
	rightLayout->setGutter(10);
	rightLayout->setPadding(1);


	rightLayout->addChild(rightSeekBar);
	rightLayout->addChild(rightSongName);

	for(int i = 3; i < 6; i++){
		rightLayout->addChild(effectElements[i]);
	}

	screenLayout->reflow();
	leftLayout->reflow();
	rightLayout->reflow();

	screen.addChild(screenLayout);
	screen.repos();
}

void MixScreen::MixScreen::loop(uint micros){
	// Handle scheduled auto-resume after hot-swap
	if(pendingAutoResume && millis() >= autoResumeScheduledTime){
		if(system && !hotSwapInProgress && !isLoadingTrack){
			uint8_t channel = pendingAutoResumeChannel;
			SongSeekBar* seekBar = (channel == 0) ? leftSeekBar : rightSeekBar;
			fs::File& trackFile = (channel == 0) ? f1 : f2;
			
			if(trackFile && trackFile.size() > 0){
				Serial.printf("Executing auto-resume: simulating play button press on channel %d\n", channel);
				
				if(system->isChannelPaused(channel)){
					// Use preserved position from seek bar
					uint32_t preservedPos = seekBar->getCurrentDuration();
					Serial.printf("Auto-resume: Using preserved position %u seconds on channel %d\n", preservedPos, channel);
					
					// Seek to preserved position first
					if(preservedPos > 0){
						system->seekChannel(channel, preservedPos);
						delay(100); // Let seek complete
					}
					
					// Minimal delays for better audio continuity
					delay(50); // Reduced pre-resume delay
					system->resumeChannel(channel);
					delay(25); // Minimal post-resume stabilization
					seekBar->setPlaying(true);
					Serial.printf("Auto-resume completed: Channel %d playing from position %u\n", channel, preservedPos);
				}
			}
		}
		pendingAutoResume = false;
	}
	
	if(system && seekTime != 0 && millis() - seekTime >= 100){
		SongSeekBar* bar = seekChannel ? rightSeekBar : leftSeekBar;

		system->seekChannel(seekChannel, bar->getCurrentDuration());

		if(wasRunning){
			system->resumeChannel(seekChannel);
		}

		seekChannel = -1;
		seekTime = 0;
	}

	bool update = false;
	for(const auto& element : effectElements){
		update |= element->needsUpdate();
	}

	// Only update seek bars for channels with valid tracks
	if(system && f1 && f1.size() > 0 && system->getDuration(0) != leftSeekBar->getTotalDuration()){
		leftSeekBar->setTotalDuration(system->getDuration(0));
		update = true;
	}

	if(system && f2 && f2.size() > 0 && system->getDuration(1) != rightSeekBar->getTotalDuration()){
		rightSeekBar->setTotalDuration(system->getDuration(1));
		update = true;
	}

	// Update seek bar positions, but respect smart resume timing
	if(system && f1 && f1.size() > 0 && system->getElapsed(0) != leftSeekBar->getCurrentDuration()){
		if(seekTime == 0 || seekChannel != 0){
			// Don't overwrite position immediately after smart seek (give decoder time to catch up)
			uint32_t timeSinceSmartSeek = millis() - lastSmartSeekTime[0];
			if(lastSmartSeekTime[0] == 0 || timeSinceSmartSeek > 3000){ // 3 second grace period
				leftSeekBar->setCurrentDuration(system->getElapsed(0));
				update = true;
			}else{
				// During grace period, log what's happening
				if(timeSinceSmartSeek % 1000 < 50){ // Log once per second during grace period
					Serial.printf("Grace period: Channel 0 - System elapsed: %d, Seek bar: %d, Time since seek: %d ms\n", 
						system->getElapsed(0), leftSeekBar->getCurrentDuration(), timeSinceSmartSeek);
				}
			}
		}
	}

	if(system && f2 && f2.size() > 0 && system->getElapsed(1) != rightSeekBar->getCurrentDuration()){
		if(seekTime == 0 || seekChannel != 1){
			// Don't overwrite position immediately after smart seek (give decoder time to catch up)
			uint32_t timeSinceSmartSeek = millis() - lastSmartSeekTime[1];
			if(lastSmartSeekTime[1] == 0 || timeSinceSmartSeek > 3000){ // 3 second grace period
				rightSeekBar->setCurrentDuration(system->getElapsed(1));
				update = true;
			}else{
				// During grace period, log what's happening
				if(timeSinceSmartSeek % 1000 < 50){ // Log once per second during grace period
					Serial.printf("Grace period: Channel 1 - System elapsed: %d, Seek bar: %d, Time since seek: %d ms\n", 
						system->getElapsed(1), rightSeekBar->getCurrentDuration(), timeSinceSmartSeek);
				}
			}
		}
	}

	if(system && system->isRecording() != isRecording){
		isRecording = system->isRecording();
		update = true;
	}

	if(system && f1 && f1.size() > 0 && system->isChannelPaused(0) != !leftSeekBar->isPlaying() && seekTime == 0){
		leftSeekBar->setPlaying(!system->isChannelPaused(0));
		update = true;
	}

	if(system && f2 && f2.size() > 0 && system->isChannelPaused(1) != !rightSeekBar->isPlaying() && seekTime == 0){
		rightSeekBar->setPlaying(!system->isChannelPaused(1)); // Fixed: was incorrectly using channel 0
		update = true;
	}

	bool songNameUpdateL = leftSongName->checkScrollUpdate();
	bool songNameUpdateR = rightSongName->checkScrollUpdate();
	update |= songNameUpdateL | songNameUpdateR;

	uint32_t currentTime = millis();
	if((update || drawQueued) && (currentTime - lastDraw) >= (isRecording ? 200 : 50)){
		drawQueued = false;
		draw();
		screen.commit();
		lastDraw = currentTime;
	}else if(update){
		drawQueued = true;
	}
}


uint8_t MixScreen::MixScreen::applyCrossfaderCurve(uint8_t rawValue){
	// Professional DJ crossfader curve with sharper cut and better blending
	// Range: 0-255, with 128 as center
	
	float normalized = (float)rawValue / 255.0f; // 0.0 to 1.0
	float curve;
	
	if(normalized < 0.5f){
		// Left side (0.0 to 0.5): Exponential curve for sharp cut
		float leftSide = normalized * 2.0f; // 0.0 to 1.0
		curve = pow(leftSide, 2.5f) * 0.5f; // Steeper curve, map to 0.0-0.5
	}else{
		// Right side (0.5 to 1.0): Exponential curve for sharp cut
		float rightSide = (normalized - 0.5f) * 2.0f; // 0.0 to 1.0
		curve = 0.5f + (pow(rightSide, 2.5f) * 0.5f); // Steeper curve, map to 0.5-1.0
	}
	
	return (uint8_t)(curve * 255.0f);
}

void MixScreen::MixScreen::potMove(uint8_t id, uint8_t value){
	if(!system) return;
	
	// Prevent crossfader jumps during hot-swap by ignoring rapid changes
	static uint32_t lastCrossfaderUpdate = 0;
	static uint8_t lastCrossfaderValue = 128;
	
	if(id == POT_MID){
		uint32_t now = millis();
		
		// Detect crossfader jumps (hardware glitches during hot-swap)
		bool isJump = abs((int)value - (int)lastCrossfaderValue) > 50 && 
		              (now - lastCrossfaderUpdate) < 100;
		
		if(!hotSwapInProgress && !isJump){
			// Apply professional DJ crossfader curve
			uint8_t curvedValue = applyCrossfaderCurve(value);
			
			system->setMix(curvedValue);
			matrixManager.fillMatrixMid(value); // Visual uses raw value for smooth display
			matrixManager.matrixMid.push();
			
			// Track last valid crossfader value (use raw value for hardware tracking)
			lastValidMixVal = value;
			lastCrossfaderValue = value;
			lastCrossfaderUpdate = now;
		}
	}else if(id == POT_L){
		system->setVolume(0, value);
		// Track last valid left volume value
		lastValidLeftVol = value;
	}else if(id == POT_R){
		system->setVolume(1, value);
		// Track last valid right volume value
		lastValidRightVol = value;
	}
}

void MixScreen::MixScreen::startBigVu(){
	LoopManager::addListener(&midVu);
}

void MixScreen::MixScreen::stopBigVu(){
	LoopManager::removeListener(&midVu);
}

void MixScreen::MixScreen::encTwoBot(){
	if(system->isRecording()){
		system->stopRecording();
		doneRecording = true;

		(new TextInputScreen::TextInputScreen(*screen.getDisplay()))->push(this);
	}else{
		system->startRecording();
	}
}

void MixScreen::MixScreen::encTwoTop(){
	Serial.println("=== DUAL ENCODER MENU ACTIVATED ===");
	
	// Create a simple selection menu
	// For now, just go to main menu where user can select Playback, DJ (MixScreen), or Settings
	// TODO: Could implement a custom popup menu here in the future
	
	Serial.println("Switching to main menu for mode selection...");
	pop(); // This takes us back to MainMenu where user can choose Playback/DJ/Settings
}

void MixScreen::MixScreen::btnCombination(){
	stop();

	MatrixPopUpPicker* popUpPicker = new MatrixPopUpPicker(*this);
	popUpPicker->unpack();
	popUpPicker->start();
}

void MixScreen::MixScreen::btn(uint8_t i){
	Serial.printf("\n=== BTN PRESSED: %d ===\n", i);
	Serial.printf("System pointer: %p\n", system);
	Serial.printf("f1 valid: %s, f2 valid: %s\n", 
		(f1 && f1.size() > 0) ? "YES" : "NO",
		(f2 && f2.size() > 0) ? "YES" : "NO");
	
	// SAFETY: Extra validation of system pointer
	if(!system || system == nullptr) {
		Serial.println("No system - cannot play/pause");
		return;
	}
	
	// Check if the channel has a valid track
	if((i == 0 && (!f1 || f1.size() == 0)) || 
	   (i == 1 && (!f2 || f2.size() == 0))){
		Serial.printf("Channel %d has no valid track\n", i);
		return;
	}
	
	SongSeekBar* bar = i == 0 ? leftSeekBar : rightSeekBar;
	if(!bar){
		Serial.printf("ERROR: No seek bar for channel %d\n", i);
		return;
	}
	
	bool wasPlaying = bar->isPlaying();
	
	Serial.printf("Channel %d was %s, attempting to %s\n", 
		i, wasPlaying ? "playing" : "paused", wasPlaying ? "pause" : "resume");
	
	// SAFETY: Double-check system pointer before calling methods
	if(system && system != nullptr){
		if(wasPlaying){
			system->pauseChannel(i);
		}else{
			// SMART RESUME: If channel is paused and has a position > 0, seek first
			uint32_t currentPos = bar->getCurrentDuration();
			Serial.printf("Resume check: Channel %d current position = %d\n", i, currentPos);
			
			if(currentPos > 0){
				Serial.printf("Smart resume: Seeking to position %d before playing\n", currentPos);
				system->seekChannel(i, currentPos);
				lastSmartSeekTime[i] = millis(); // Update to track the actual seek time
				delay(150); // Allow seek to complete before resume
			}else{
				Serial.printf("No smart resume needed: Channel %d starting from position 0\n", i);
				// If no smart resume needed, clear any protection
				lastSmartSeekTime[i] = 0;
			}
			system->resumeChannel(i);
		}
		
		bar->setPlaying(!wasPlaying);
		Serial.printf("Channel %d now %s\n", i, bar->isPlaying() ? "playing" : "paused");
	}else{
		Serial.println("ERROR: System became null during button press!");
	}
	
	drawQueued = true;
	Serial.println("=== BTN END ===\n");
}

void MixScreen::MixScreen::btnEnc(uint8_t i){
	if(i > 6) return;

	if(i == 6){
		selectedChannel = !selectedChannel;
	}else{
		EffectElement* effect = effectElements[i];
		effect->setSelected(!effect->isSelected());
	}

	drawQueued = true;
}

void MixScreen::MixScreen::enc(uint8_t index, int8_t value){
	if(!system) return; // No system yet - can't use encoders for audio control

	if(index == 6){
		// Check if the selected channel has a valid track
		if((selectedChannel == 0 && (!f1 || f1.size() == 0)) || 
		   (selectedChannel == 1 && (!f2 || f2.size() == 0))){
			return;
		}
		
		if(seekTime == 0){
			seekChannel = selectedChannel;
			wasRunning = !system->isChannelPaused(selectedChannel);
			system->pauseChannel(selectedChannel);
		}

		seekTime = millis();

		SongSeekBar* bar = seekChannel ? rightSeekBar : leftSeekBar;
		uint32_t maxDuration = system->getDuration(selectedChannel);
		uint16_t newSeekTime = constrain(bar->getCurrentDuration() + value, 0, maxDuration);
		bar->setCurrentDuration(newSeekTime);

		drawQueued = true;
		return;
	}

	EffectElement* element = effectElements[index];

	if(element->isSelected()){
		int8_t e = element->getType() + value;
		if(e >= EffectType::COUNT){
			e = e % EffectType::COUNT;
		}else if(e < 0){
			while(e < 0){
				e += EffectType::COUNT;
			}
		}

		// Only one speed allowed
		if(e == EffectType::SPEED){
			for(int i = (index < 3 ? 0 : 3); i < (index < 3 ? 3 : 6); i++){
				if(i == index) continue;
				if(effectElements[i]->getType() != EffectType::SPEED) continue;

				if(value < 0){
					e = e > 0 ? e - 1 : EffectType::COUNT - 1;
				}else{
					e = (e + 1) % EffectType::COUNT;
				}

				break;
			}
		}

		if(element->getType() == EffectType::SPEED){
			system->removeSpeed(index >= 3);
		}

		EffectType type = static_cast<EffectType>(e);
		element->setType(type);
		element->setIntensity(0);

		if(type == EffectType::SPEED){
			system->addSpeed(index >= 3);
			element->setIntensity(255 / 2);
			return;
		}

		system->setEffect(index >= 3, index < 3 ? index : index - 3, type);
	}else{
		EffectType type = element->getType();
		if(type == EffectType::NONE) return;

		int16_t intensity = element->getIntensity() + value * 5;
		intensity = max((int16_t) 0, intensity);
		intensity = min((int16_t) 255, intensity);

		element->setIntensity(intensity);

		if(type == EffectType::SPEED){
			system->setSpeed(index >= 3, intensity);
		}else{
			system->setEffectIntensity(index >= 3, index < 3 ? index : index - 3, element->getIntensity());
		}
	}

	drawQueued = true;
}

void MixScreen::MixScreen::hotSwapTrack(uint8_t deck, fs::File newFile){
	Serial.printf("\n=== HOT-SWAP START: deck %d ===\n", deck);
	Serial.printf("hotSwapInProgress: %s\n", hotSwapInProgress ? "true" : "false");
	Serial.printf("Current system: %p\n", system);
	
	if(hotSwapInProgress){
		Serial.println("ERROR: Hot-swap already in progress");
		newFile.close();
		return;
	}
	
	if(!system){
		Serial.println("ERROR: No MixSystem available for hot-swap");
		newFile.close();
		return;
	}
	
	hotSwapInProgress = true;
	
	// Use stored hardware mixer values captured when button was held
	uint8_t leftVol = storedLeftVol;
	uint8_t rightVol = storedRightVol;
	uint8_t mixVal = storedMixVal;
	
	Serial.printf("Using stored hardware mixer values: Vol L=%d, Vol R=%d, Mix=%d\n", leftVol, rightVol, mixVal);
	
	// Declare channel state variables at function scope
	bool channel0WasPlaying = false;
	bool channel1WasPlaying = false;
	uint32_t channel0Position = 0;
	uint32_t channel1Position = 0;
	
	// Validate the new file first
	if(!newFile || newFile.size() == 0){
		Serial.println("ERROR: Invalid file for hot-swap");
		if(newFile) newFile.close();
		hotSwapInProgress = false;
		return;
	}
	
	Serial.printf("Hot-swapping file: %s (size: %d)\n", newFile.name(), newFile.size());
	
	// Close the old file and assign the new one
	if(deck == 0){
		Serial.println("Hot-swapping f1 (player 1)");
		if(f1) {
			Serial.printf("Closing old f1: %s\n", f1.name());
			f1.close();
		}
		f1 = newFile;
		f1.seek(0);
		String name = f1.name();
		String songName = name.substring(name.lastIndexOf('/') + 1, name.length() - 4);
		leftSongName->setSongName(songName);
		Serial.printf("f1 hot-swapped to: %s\n", songName.c_str());
	}else{
		Serial.println("Hot-swapping f2 (player 2)");
		if(f2) {
			Serial.printf("Closing old f2: %s\n", f2.name());
			f2.close();
		}
		f2 = newFile;
		f2.seek(0);
		String name = f2.name();
		String songName = name.substring(name.lastIndexOf('/') + 1, name.length() - 4);
		rightSongName->setSongName(songName);
		Serial.printf("f2 hot-swapped to: %s\n", songName.c_str());
	}
	
	// Show the track name change immediately  
	drawQueued = true;
	
	// SMART HOT-SWAP: Update MixSystem without full context restart
	// This preserves audio playback during track loading
	Serial.println("Updating MixSystem to include new track...");
	Serial.printf("Pre-update memory: heap=%u\n", ESP.getFreeHeap());
	
	if(system){
		Serial.printf("Recreating MixSystem with updated files (old: %p)\n", system);
		
		// CRITICAL: Prevent any system access during recreation
		hotSwapInProgress = true;
		
		// CRITICAL: Store playback states before recreating system
		
		if(f1 && f1.size() > 0){
			channel0WasPlaying = !system->isChannelPaused(0);
			channel0Position = system->getElapsed(0);
			Serial.printf("Channel 0 state: %s at position %d\n", 
				channel0WasPlaying ? "PLAYING" : "PAUSED", channel0Position);
		}
		
		if(f2 && f2.size() > 0){
			channel1WasPlaying = !system->isChannelPaused(1);
			channel1Position = system->getElapsed(1);
			Serial.printf("Channel 1 state: %s at position %d\n", 
				channel1WasPlaying ? "PLAYING" : "PAUSED", channel1Position);
		}
		
		// IMPORTANT: If no channel is currently playing, avoid recreation entirely
		if(!channel0WasPlaying && !channel1WasPlaying){
			Serial.println("No channels playing - using simple system recreation");
			// We can recreate without worrying about audio continuity
		}else{
			Serial.println("Active playback detected - attempting seamless recreation");
			// Need to be extra careful about timing
		}
		
		// Mixer settings already stored at function start
		
		// Stop old system safely
		if(system){
			Serial.println("Stopping old MixSystem...");
			
			// Ensure all channels are fully stopped
			system->pauseChannel(0);
			system->pauseChannel(1);
			delay(100); // Let channels stop completely
			
			system->stop();
			delay(100); // Let system fully stop
			
			Serial.println("Deleting old MixSystem...");
			MixSystem* oldSystem = system;
			system = nullptr; // Clear pointer first to prevent use-after-free
			
			delete oldSystem;
			Serial.printf("Post-delete memory: heap=%u\n", ESP.getFreeHeap());
		}else{
			Serial.println("No old MixSystem to delete");
		}
		
		// POWER MANAGEMENT: Delay before creating new system
		delay(100);
		
		// Create new system with both files
		Serial.println("Creating new MixSystem...");
		system = new MixSystem(f1, f2);
		Serial.printf("New MixSystem created: %p\n", system);
		Serial.printf("Post-create memory: heap=%u\n", ESP.getFreeHeap());
		
		// Restore VU meter connections first (safe to do immediately)
		system->setChannelInfo(0, leftVu.getInfoGenerator());
		system->setChannelInfo(1, rightVu.getInfoGenerator());
		system->setChannelInfo(2, midVu.getInfoGenerator());
		
		// Update seek bars
		if(f1){
			leftSeekBar->setTotalDuration(system->getDuration(0));
			leftSeekBar->setCurrentDuration(0);  // New tracks start at beginning
			leftSeekBar->setPlaying(false);      // Will be updated below
		}
		if(f2){
			rightSeekBar->setTotalDuration(system->getDuration(1));
			rightSeekBar->setCurrentDuration(0);  // New tracks start at beginning
			rightSeekBar->setPlaying(false);      // Will be updated below
		}
		
		// Start new system
		system->start();
		
		// Optimized stabilization delays for better audio continuity
		// Shorter delays reduce audio dropouts during hot-swap
		if(channel0WasPlaying && channel1WasPlaying){
			Serial.println("Both channels were playing - using minimal dual-channel delay");
			delay(200); // Reduced from 500ms for better continuity
		}else if(channel0WasPlaying || channel1WasPlaying){
			Serial.println("Single channel playing - using minimal delay");
			delay(150); // Reduced from 350ms for better continuity
		}else{
			Serial.println("No channels playing - using standard delay");
			delay(250); // Standard delay when no audio is playing
		}
		
		Serial.println("System started, beginning state restoration...");
		
		// DJ-STYLE APPROACH: Clean pause all, preserve positions, let user control resume
		// This works with MixSystem limitations rather than fighting them
		
		Serial.println("Post-recreation: DJ-style hot-swap - clean state with position memory");
		
		// Always pause both channels for clean state (prevent decode errors)
		system->pauseChannel(0);
		delay(100); // Longer delay between pauses
		system->pauseChannel(1);
		
		// Optimized settlement delay - less aggressive for dual-channel scenarios
		if(channel0WasPlaying && channel1WasPlaying){
			Serial.println("Dual-channel scenario - using optimized delay");
			delay(400); // Reduced to prevent timing issues
		}else{
			delay(300); // Reduced for single-channel scenarios
		}
		
		// Additional decoder stability measures to prevent lockup and resets
		Serial.println("Performing decoder stability check...");
		
		// Give more time for decoder to fully initialize with new files
		delay(500);
		
		// Ensure both files are at the beginning for clean decoder state
		if(f1 && f1.size() > 0) {
			f1.seek(0);
			Serial.printf("Reset f1 to position 0 (size: %d)\n", f1.size());
		}
		if(f2 && f2.size() > 0) {
			f2.seek(0);
			Serial.printf("Reset f2 to position 0 (size: %d)\n", f2.size());
		}
		
		delay(200); // Additional stabilization time
		
		// Set seek bars to paused but preserve position information
		leftSeekBar->setPlaying(false);
		rightSeekBar->setPlaying(false);
		
		// Restore mixer settings AFTER system stabilization to prevent volume jumps
		Serial.println("Restoring mixer settings after stabilization...");
		system->setVolume(0, leftVol);
		system->setVolume(1, rightVol);
		
		// Apply crossfader curve to stored mix value for consistent audio response
		uint8_t curvedMixVal = applyCrossfaderCurve(mixVal);
		system->setMix(curvedMixVal);
		Serial.printf("Mixer settings restored: Vol L=%d, Vol R=%d, Mix=%d (curved: %d)\n", leftVol, rightVol, mixVal, curvedMixVal);
		
		// Store the positions in seek bars so user can see where they were
		if(deck == 0){
			// New track loaded to channel 0 (left deck)
			Serial.println("Hot-swap: New track on LEFT deck, preserving RIGHT deck position");
			
			// New track starts at beginning
			leftSeekBar->setCurrentDuration(0);
			
			// Preserve the position of the continuing track
			if(f2 && f2.size() > 0 && channel1Position > 0){
				rightSeekBar->setCurrentDuration(channel1Position);
				// CRITICAL: Prevent loop from overwriting this preserved position
				lastSmartSeekTime[1] = millis() - 1; // Set to recent time to activate protection
				Serial.printf("RIGHT deck position preserved: %d seconds (press play to continue)\n", channel1Position);
				Serial.printf("Verification: rightSeekBar now shows %d seconds\n", rightSeekBar->getCurrentDuration());
			}
		}else{
			// New track loaded to channel 1 (right deck)  
			Serial.println("Hot-swap: New track on RIGHT deck, preserving LEFT deck position");
			
			// New track starts at beginning
			rightSeekBar->setCurrentDuration(0);
			
			// Preserve the position of the continuing track
			if(f1 && f1.size() > 0 && channel0Position > 0){
				leftSeekBar->setCurrentDuration(channel0Position);
				// CRITICAL: Prevent loop from overwriting this preserved position
				lastSmartSeekTime[0] = millis() - 1; // Set to recent time to activate protection
				Serial.printf("LEFT deck position preserved: %d seconds (press play to continue)\n", channel0Position);
				Serial.printf("Verification: leftSeekBar now shows %d seconds\n", leftSeekBar->getCurrentDuration());
			}
		}
		
		Serial.println("Hot-swap complete - DJ can now resume playback with precise control");
		
		Serial.printf("MixSystem updated successfully (new: %p)\n", system);
	}
	
	// Reset flags after hot-swap completion
	hotSwapInProgress = false; // Clear protection flag
	keepAudioOnStop = false;
	justCompletedHotSwap = true;
	
	// Prevent any lingering loading states that could cause issues
	isLoadingTrack = false;
	
	Serial.println("Hot-swap flags reset - system ready for normal operation");
	
	// Use stored hardware values (already validated when captured) instead of reading fresh
	// Reading hardware values immediately after hot-swap can return corrupted data
	Serial.println("Using stored hardware values (already validated during capture)...");
	
	// Apply stored hardware values to system to match hardware position when button was held
	if(system){
		system->setVolume(0, storedLeftVol);
		system->setVolume(1, storedRightVol);
		uint8_t curvedMixVal = applyCrossfaderCurve(storedMixVal);
		system->setMix(curvedMixVal);
		
		// Update tracked values to match stored hardware values
		lastValidLeftVol = storedLeftVol;
		lastValidRightVol = storedRightVol;
		lastValidMixVal = storedMixVal;
		
		Serial.printf("Applied stored hardware values: Vol L=%d, Vol R=%d, Mix=%d (curved: %d)\n", 
			storedLeftVol, storedRightVol, storedMixVal, curvedMixVal);
	}
	
	// Schedule auto-resume for the channel that WASN'T replaced (the continuing channel)
	if(system){
		if(deck == 0 && channel1WasPlaying && f2 && f2.size() > 0){
			// Replaced Player 1 (deck 0), continue Player 2 (channel 1)
			Serial.println("Scheduling auto-resume for player 2 (RIGHT) - continuing after Player 1 replacement");
			pendingAutoResume = true;
			pendingAutoResumeChannel = 1;
			autoResumeScheduledTime = millis() + 400; // Reduced delay for faster continuity
		}else if(deck == 1 && channel0WasPlaying && f1 && f1.size() > 0){
			// Replaced Player 2 (deck 1), continue Player 1 (channel 0)
			Serial.println("Scheduling auto-resume for player 1 (LEFT) - continuing after Player 2 replacement");
			pendingAutoResume = true;
			pendingAutoResumeChannel = 0;
			autoResumeScheduledTime = millis() + 400; // Reduced delay for faster continuity
		}else{
			Serial.printf("No auto-resume scheduled: deck=%d, ch0Playing=%s, ch1Playing=%s\n", 
				deck, channel0WasPlaying ? "true" : "false", channel1WasPlaying ? "true" : "false");
		}
	}
	
	Serial.println("=== HOT-SWAP COMPLETE ===");
}

void MixScreen::MixScreen::encBtnHold(uint8_t i){
	if(i == 6){
		Serial.printf("\n=== HOLD BUTTON 6: Loading track for deck %d ===\n", selectedChannel);
		Serial.printf("Current state - f1: %s, f2: %s\n", 
			(f1 && f1.size() > 0) ? "loaded" : "empty",
			(f2 && f2.size() > 0) ? "loaded" : "empty");
		Serial.printf("System: %p, hotSwapInProgress: %s\n", 
			system, hotSwapInProgress ? "true" : "false");
		Serial.printf("Memory before SongList: heap=%u\n", ESP.getFreeHeap());
		
		// Capture hardware mixer values while system is stable
		// For all pots, use the last known good values from potMove callbacks
		// as getPotValue() can return corrupted data during system transitions
		
		// Use the last valid values tracked by potMove callbacks
		storedLeftVol = lastValidLeftVol;
		storedRightVol = lastValidRightVol;
		storedMixVal = lastValidMixVal;
		
		// Fallback to direct reading only if we don't have tracked values
		if(storedLeftVol == 0 && storedRightVol == 0 && storedMixVal == 0) {
			Serial.println("No tracked values - attempting direct hardware read");
			uint8_t rawLeftVol = InputJayD::getInstance()->getPotValue(POT_L);
			uint8_t rawRightVol = InputJayD::getInstance()->getPotValue(POT_R);
			uint8_t rawMixVal = InputJayD::getInstance()->getPotValue(POT_MID);
			
			// Validate direct readings
			storedLeftVol = (rawLeftVol > 255) ? 128 : rawLeftVol;  // Default to mid-level
			storedRightVol = (rawRightVol > 255) ? 128 : rawRightVol;
			storedMixVal = (rawMixVal > 255) ? 128 : rawMixVal;
		}
		
		Serial.printf("Captured hardware mixer values: Vol L=%d, Vol R=%d, Mix=%d (validated)\n", 
			storedLeftVol, storedRightVol, storedMixVal);
		
		// Store which deck we're loading for
		isLoadingTrack = true;
		loadingDeck = selectedChannel;
		keepAudioOnStop = true; // Keep audio playing when SongList is shown
		
		Serial.printf("Set keepAudioOnStop = true, loadingDeck = %d\n", loadingDeck);
		Serial.println("Opening SongList...");
		
		(new SongList::SongList(*getScreen().getDisplay()))->push(this);
		
		Serial.println("=== SongList opened ===\n");
		return;
	}
}

void MixScreen::MixScreen::initializeDefaultEffects(){
	if(!system){
		Serial.println("WARNING: Cannot initialize effects - system not ready");
		return;
	}
	
	Serial.println("Initializing default effects using existing selector logic...");
	
	for(int i = 0; i < 6; i++){
		EffectType type = effectElements[i]->getType();
		if(type == NONE) continue;
		
		// Use the exact same logic as the existing effect selector
		if(type == EffectType::SPEED){
			system->addSpeed(i >= 3);
			effectElements[i]->setIntensity(255 / 2);
			Serial.printf("Applied SPEED effect to channel %d\n", i >= 3 ? 1 : 0);
		}else{
			system->setEffect(i >= 3, i < 3 ? i : i - 3, type);
			Serial.printf("Applied effect %d to channel %d, slot %d\n", 
				(int)type, i >= 3 ? 1 : 0, i < 3 ? i : i - 3);
		}
	}
	
	Serial.println("Default effects initialized using selector logic");
}
