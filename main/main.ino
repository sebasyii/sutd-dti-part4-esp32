#include <FastLED.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_timer.h>
 
#define LED_PIN     15
#define COLOR_ORDER GRB
#define CHIPSET     WS2812B
#define NUM_LEDS    300
#define BRIGHTNESS  255
#define SERVICE_UUID "8c6332b8-bf32-4220-ad31-0d8c19003330"
#define LED_DATA_UUID "3a7f4056-0b5b-40be-99ba-fbe21644bd47"

BLECharacteristic *pLedDataCharacteristic;
BLEAdvertising *pAdvertising;
CRGB leds[NUM_LEDS];

// Number of steps for PWM resolution:
const int steps = 256;
int levelTable[steps];

class MyServerCallbacks : public BLEServerCallbacks {
  bool isAdvertising = false;

  void onConnect(BLEServer* pServer) {
    Serial.println("Connected");
  }

  void onDisconnect(BLEServer* pServer) {
    Serial.println("Disconnected");
    if (isAdvertising) {
      BLEDevice::stopAdvertising();
      isAdvertising = false;
    }
    BLEDevice::startAdvertising();
    pAdvertising->start();
    isAdvertising = true;
    Serial.println("BLE device starts advertising again");
  }
};
 
void setup() {
  delay(3000); // sanity delay
  FastLED.addLeds<CHIPSET, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.setTemperature(0xFF7029);

  FastLED.clear();
  FastLED.show();
  FastLED.setDither(0);

  Serial.begin(115200);

  // BLE
  BLEDevice::init("Moonlight");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);
  delay(100);

  // Create characteristics
  pLedDataCharacteristic = pService->createCharacteristic(LED_DATA_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pLedDataCharacteristic->addDescriptor(new BLE2902());

  // Start BLE server and advertising
  pService->start();
  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);

  BLEDevice::startAdvertising();
  Serial.println("BLE device is ready to be connected");

  // Initialize level table with sine values
  fillLevelTable();
}

enum BreathingState {
  INHALE,
  HOLD_INHALE,
  EXHALE,
  HOLD_EXHALE,
  SLEEP,
  WAKE_UP
};

BreathingState breathingState = INHALE;
unsigned long lastStateChangeMillis = 0;
unsigned long lastDebugPrintMillis = 0;  // For debug prints

void fillLevelTable() {
  // iterate over the array and calculate the right value for it:
  for (int l = 0; l < steps; l++) {
    // map input to a 0-90 range:
    float angle = map(l, 0, steps - 1, 0, 90);
    // convert to radians:
    float lightLevel = angle * PI / 180;
    // get the sine of that:
    lightLevel = sin(lightLevel);
    // multiply to get 0-255:
    lightLevel *= 255;
    // put it in the array:
    levelTable[l] = static_cast<int>(lightLevel);
  }
}


void updateLEDs(float progress) {
    FastLED.clear();
    Serial.println("Progress:" + String(progress));
    
    float mappedValue = progress * (steps - 1);
    int currentLevel = static_cast<int>(mappedValue);
    
    uint8_t scaledBrightness = levelTable[currentLevel];
    Serial.println("Current Level: " + String(currentLevel));
    Serial.println("Brightness: " + String(scaledBrightness));

    fill_solid(leds, NUM_LEDS, CHSV(30, 200, scaledBrightness));
    FastLED.show();
}

void boxBreathing(int napDuration, int breathingLightDuration, int wakingUpLightDuration) {
  int64_t startTime = esp_timer_get_time();
  int64_t totalDurationMicros = napDuration * 60000000LL;
  int64_t sleepStartMicros = startTime + breathingLightDuration * 60000000LL;
  int64_t wakeStartMicros = startTime + (napDuration - wakingUpLightDuration) * 60000000LL;

  int inhaleExhaleDuration = 4000000;
  int holdDuration = 4000000;

  lastStateChangeMillis = esp_timer_get_time() / 1000;
  lastDebugPrintMillis = esp_timer_get_time() / 1000;
  breathingState = INHALE;

  while (esp_timer_get_time() - startTime < totalDurationMicros) {
    int64_t currentTime = esp_timer_get_time();
    int64_t elapsedStateTime = currentTime - lastStateChangeMillis * 1000;

    // Debug print every second
    if (currentTime - lastDebugPrintMillis * 1000 >= 1000000) {
      Serial.println("Current Time since startTime (ms): " + String((currentTime - startTime) / 1000));
      Serial.println("Current State: " + String(breathingState));
      lastDebugPrintMillis = currentTime / 1000;
    }

    if (breathingState < WAKE_UP && currentTime >= wakeStartMicros) {
      breathingState = WAKE_UP;
      lastStateChangeMillis = currentTime / 1000;
    } else if (breathingState < SLEEP && currentTime >= sleepStartMicros) {
      breathingState = SLEEP;
      FastLED.clear();
      FastLED.show();
    }

    std::string bleDataValue = pLedDataCharacteristic->getValue();
    // Check if the BLE data is "0" which means stop the breathing
    if (bleDataValue == "0") {
      FastLED.clear();
      FastLED.show();
      return;
    }

    switch (breathingState) {
      case INHALE:
        if (elapsedStateTime <= inhaleExhaleDuration) {
          float progress = static_cast<float>(elapsedStateTime) / inhaleExhaleDuration;
          
          updateLEDs(progress);
        } else {
          breathingState = HOLD_INHALE;
          lastStateChangeMillis = currentTime / 1000;
        }
        break;

      case HOLD_INHALE:
        if (elapsedStateTime >= holdDuration) {
          breathingState = EXHALE;
          lastStateChangeMillis = currentTime / 1000;
        } else {
          CRGB targetColor = CHSV(0, 200, 255);
          CRGB currentColor = CHSV(30, 200, 255);

          // Define transition times (half a second each)
          int transitionDuration = 500000; // Transition duration in milliseconds
          float blendRatio;

          if (elapsedStateTime < transitionDuration) {
            // First transition: Current to Target
            blendRatio = float(elapsedStateTime) / transitionDuration;
          } else if (elapsedStateTime > holdDuration - transitionDuration) {
            // Last transition: Target back to Current
            blendRatio = float(holdDuration - elapsedStateTime) / transitionDuration;
          } else {
            // Maintain target color in the middle period
            blendRatio = 1.0;
          }

          CRGB blendedColor;
          if (elapsedStateTime <= transitionDuration || elapsedStateTime >= holdDuration - transitionDuration) {
            // During transitions, calculate the blended color
            blendedColor = blend(currentColor, targetColor, blendRatio * 255);
          } else {
            // Hold the target color steady during the middle period
            blendedColor = targetColor;
          }

          fill_solid(leds, NUM_LEDS, blendedColor);
          FastLED.show();
        }
        break;

      case EXHALE:
        if (elapsedStateTime <= inhaleExhaleDuration) {
          float progress = static_cast<float>(elapsedStateTime) / inhaleExhaleDuration;
          updateLEDs(1.0f - progress);
        } else {
          breathingState = HOLD_EXHALE;
          lastStateChangeMillis = currentTime / 1000;
        }
        break;

      case HOLD_EXHALE:
        if (elapsedStateTime >= holdDuration) {
          breathingState = INHALE;
          lastStateChangeMillis = currentTime / 1000;
        }
        break;

      case SLEEP:
        break;

      case WAKE_UP:
        if (elapsedStateTime <= wakingUpLightDuration * 60 * 1000000LL) {
          float progress = static_cast<float>(elapsedStateTime) / (wakingUpLightDuration * 60 * 1000000LL);
          updateLEDs(progress);
        }
        break;
    }
  }
  Serial.println("Nap Duration is over...");
  pLedDataCharacteristic->setValue("0");
}

void fourSevenEightBreathing(int napDuration, int breathingLightDuration, int wakingUpLightDuration) {
  int64_t startTime = esp_timer_get_time();
  int64_t totalDurationMicros = napDuration * 60000000LL;
  int64_t sleepStartMicros = startTime + breathingLightDuration * 60000000LL;
  int64_t wakeStartMicros = startTime + (napDuration - wakingUpLightDuration) * 60000000LL;

  int inhaleDuration = 4000000;
  int holdDuration = 7000000;
  int exhaleDuration = 8000000;

  lastStateChangeMillis = esp_timer_get_time() / 1000;
  lastDebugPrintMillis = esp_timer_get_time() / 1000;
  breathingState = INHALE;

  while (esp_timer_get_time() - startTime < totalDurationMicros) {
    int64_t currentTime = esp_timer_get_time();
    int64_t elapsedStateTime = currentTime - lastStateChangeMillis * 1000;

    // Debug print every second
    if (currentTime - lastDebugPrintMillis * 1000 >= 1000000) {
      Serial.println("Current Time since startTime (ms): " + String((currentTime - startTime) / 1000));
      Serial.println("Current State: " + String(breathingState));
      lastDebugPrintMillis = currentTime / 1000;
    }

    if (breathingState < WAKE_UP && currentTime >= wakeStartMicros) {
      breathingState = WAKE_UP;
      lastStateChangeMillis = currentTime / 1000;
    } else if (breathingState < SLEEP && currentTime >= sleepStartMicros) {
      breathingState = SLEEP;
      FastLED.clear();
      FastLED.show();
    }

    std::string bleDataValue = pLedDataCharacteristic->getValue();
    // Check if the BLE data is "0" which means stop the breathing
    if (bleDataValue == "0") {
      FastLED.clear();
      FastLED.show();
      return;
    }

    switch (breathingState) {
      case INHALE:
        if (elapsedStateTime <= inhaleDuration) {
          float progress = static_cast<float>(elapsedStateTime) / inhaleDuration;
          updateLEDs(progress);
        } else {
          breathingState = HOLD_INHALE;
          lastStateChangeMillis = currentTime / 1000;
        }
        break;

      case HOLD_INHALE:
        if (elapsedStateTime >= holdDuration) {
          breathingState = EXHALE;
          lastStateChangeMillis = currentTime / 1000;
        } else {
          CRGB targetColor = CHSV(0, 200, 255);
          CRGB currentColor = CHSV(30, 200, 255);

          // Define transition times (half a second each)
          int transitionDuration = 500000; // Transition duration in milliseconds
          float blendRatio;

          if (elapsedStateTime < transitionDuration) {
            // First transition: Current to Target
            blendRatio = float(elapsedStateTime) / transitionDuration;
          } else if (elapsedStateTime > holdDuration - transitionDuration) {
            // Last transition: Target back to Current
            blendRatio = float(holdDuration - elapsedStateTime) / transitionDuration;
          } else {
            // Maintain target color in the middle period
            blendRatio = 1.0;
          }

          CRGB blendedColor;
          if (elapsedStateTime <= transitionDuration || elapsedStateTime >= holdDuration - transitionDuration) {
            // During transitions, calculate the blended color
            blendedColor = blend(currentColor, targetColor, blendRatio * 255);
          } else {
            // Hold the target color steady during the middle period
            blendedColor = targetColor;
          }

          fill_solid(leds, NUM_LEDS, blendedColor);
          FastLED.show();
        }
        break;

      case EXHALE:
        if (elapsedStateTime <= exhaleDuration) {
          float progress = static_cast<float>(elapsedStateTime) / exhaleDuration;
          updateLEDs(1.0f - progress);
        } else {
          breathingState = HOLD_EXHALE;
          lastStateChangeMillis = currentTime / 1000;
        }
        break;

      case HOLD_EXHALE:
        if (elapsedStateTime >= 1000000) {  // Hold for 1 second
          breathingState = INHALE;
          lastStateChangeMillis = currentTime / 1000;
        }
        break;

      case SLEEP:
        break;

      case WAKE_UP:
        if (elapsedStateTime <= wakingUpLightDuration * 60 * 1000000LL) {
          float progress = static_cast<float>(elapsedStateTime) / (wakingUpLightDuration * 60 * 1000000LL);
          updateLEDs(progress);
        }
        break;
    }
  }
  Serial.println("Nap Duration is over...");
  pLedDataCharacteristic->setValue("0");
}

void loop() {
  std::string bleDataValue = pLedDataCharacteristic->getValue();

  std::string breathingMethod;
  int napDuration;
  int breathingLightDuration;
  int wakingUpLightDuration;

  if (bleDataValue.length() > 0) {
    std::vector<std::string> components;
    size_t startPos = 0;
    size_t endPos = bleDataValue.find('-');
    while (endPos != std::string::npos) {
        components.push_back(bleDataValue.substr(startPos, endPos - startPos));
        startPos = endPos + 1;
        endPos = bleDataValue.find('-', startPos);
    }
    // Add the last part
    components.push_back(bleDataValue.substr(startPos));

    if (components.size() == 4 && components[0] == "Box Breathing") {
      breathingMethod = components[0];
      napDuration = std::stoi(components[1]);
      breathingLightDuration = std::stoi(components[2]);
      wakingUpLightDuration = std::stoi(components[3]);
    } else if (components.size() == 6) {
      breathingMethod = "4-7-8";
      napDuration = std::stoi(components[3]);
      breathingLightDuration = std::stoi(components[4]);
      wakingUpLightDuration = std::stoi(components[5]);
    } else {
      FastLED.clear();
      FastLED.show();
      return;
    }

    if (breathingMethod == "Box Breathing") {
      boxBreathing(napDuration, breathingLightDuration, wakingUpLightDuration);
    } else if (breathingMethod == "4-7-8") {
      fourSevenEightBreathing(napDuration, breathingLightDuration, wakingUpLightDuration);
    }
  }
}