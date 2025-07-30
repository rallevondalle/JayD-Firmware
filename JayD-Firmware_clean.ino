#include <Arduino.h>
#include <CircuitOS.h>
#include <JayD.h>
#include <Display/Display.h>
#include <Settings.h>
#include <Loop/LoopManager.h>
#include <Input/InputJayD.h>
#include <esp_system.h>
#include "src/InputKeys.h"
#include "src/HardwareTest.h"
#include "src/Screens/IntroScreen/IntroScreen.h"
#include "src/Screens/InputTest/InputTest.h"

bool checkJig(){
	pinMode(PIN_BL, INPUT_PULLUP);
	return digitalRead(PIN_BL) == LOW;
}

void launch(){
	Context* introScreen = new IntroScreen::IntroScreen(JayD.getDisplay());
	introScreen->unpack();
	introScreen->start();
}

void setup(){
	Serial.begin(115200);
	delay(2000); // Wait for serial connection
	
	Serial.println("\n==================================================");
	Serial.println("JAY-D FIRMWARE STARTUP");
	Serial.println("==================================================");
	Serial.printf("ESP32 Chip Rev %d\n", ESP.getChipRevision());
	Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
	Serial.printf("Flash size: %u bytes\n", ESP.getFlashChipSize());
	
	// DEBUG: Check reset reason
	esp_reset_reason_t resetReason = esp_reset_reason();
	Serial.print("Reset reason: ");
	switch(resetReason) {
		case ESP_RST_POWERON: Serial.println("POWERON (cold boot)"); break;
		case ESP_RST_EXT: Serial.println("EXTERNAL PIN"); break;
		case ESP_RST_SW: Serial.println("SOFTWARE"); break;
		case ESP_RST_PANIC: Serial.println("PANIC/EXCEPTION"); break;
		case ESP_RST_INT_WDT: Serial.println("INTERRUPT WATCHDOG"); break;
		case ESP_RST_TASK_WDT: Serial.println("TASK WATCHDOG"); break;
		case ESP_RST_WDT: Serial.println("OTHER WATCHDOG"); break;
		case ESP_RST_DEEPSLEEP: Serial.println("DEEP SLEEP"); break;
		case ESP_RST_BROWNOUT: Serial.println("BROWNOUT"); break;
		case ESP_RST_SDIO: Serial.println("SDIO"); break;
		default: Serial.printf("UNKNOWN (%d)\n", resetReason); break;
	}
	
	Serial.println("==================================================\n");

	if(checkJig()){
		pinMode(PIN_BL, OUTPUT);
		digitalWrite(PIN_BL, LOW);

		Display display(160, 128, -1, -1);
		display.getTft()->setPanel(JayDDisplay::panel1());
		display.begin();

		HardwareTest test(display);
		test.start();

		for(;;);
	}

	pinMode(PIN_BL, OUTPUT);
	digitalWrite(PIN_BL, HIGH);

	Serial.println("=== INITIALIZING JAYD ===");
	JayD.begin();
	Serial.println("JayD initialization complete");
	Serial.printf("Free heap after JayD.begin(): %u bytes\n", ESP.getFreeHeap());

	InputJayD::getInstance()->addListener(&Input);
	LoopManager::addListener(&Input);

	Context::setDeleteOnPop(true);

	if(!Settings.get().inputTested){
		InputTest::InputTest* test = new InputTest::InputTest(JayD.getDisplay());
		test->setDoneCallback([](InputTest::InputTest* test){
			Settings.get().inputTested = true;
			Settings.store();

			ESP.restart();
		});

		test->unpack();
		test->start();
	}else{
		Serial.println("=== LAUNCHING MAIN APPLICATION ===");
		launch();
		Serial.println("Main application launched");
	}

	digitalWrite(PIN_BL, LOW);
}

uint32_t lastLoopTime = 0;
uint32_t loopCounter = 0;

void loop(){
	uint32_t currentTime = millis();
	
	// Track loop performance - detect hangs
	if(currentTime - lastLoopTime > 1000) { // Every second
		Serial.printf("Loop %u: Running at %u ms intervals\n", loopCounter, (unsigned int)(currentTime - lastLoopTime));
		Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
		lastLoopTime = currentTime;
		loopCounter++;
	}
	
	LoopManager::loop();
}