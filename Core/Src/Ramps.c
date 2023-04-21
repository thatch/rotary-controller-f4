/**
 * Copyright © 2022 <Stefano Bertelli>
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the “Software”), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
 * LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdlib.h>
#include "Ramps.h"
#include "Scales.h"


const osThreadAttr_t taskRampsAttributes = {
.name = "taskRamps",
.stack_size = 128 * 4,
.priority = (osPriority_t) osPriorityNormal,
};

// This variable is the handler for the modbus communication
modbusHandler_t RampsModbusData;


void configureOutputPin(GPIO_TypeDef *Port, uint16_t Pin) {
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin : PtPin */
  GPIO_InitStruct.Pin = Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(Port, &GPIO_InitStruct);
}


void RampsStart(rampsHandler_t *rampsData) {
  rampsData->shared.acceleration = 10;
  rampsData->shared.maxSpeed = 10000;
  rampsData->shared.minSpeed = 100;

  // Configure Pins
  configureOutputPin(DIR_GPIO_PORT, DIR_PIN);
  configureOutputPin(ENA_GPIO_PORT, ENA_PIN);

  // Initialize and start encoder timer
  startScalesTimers(&rampsData->scales);

  // Start synchro interrupt
  HAL_TIM_Base_Start_IT(rampsData->synchroRefreshTimer);
  HAL_TIM_Base_Start_IT(rampsData->indexRefreshTimer);

  // Start Modbus
  RampsModbusData.uModbusType = MB_SLAVE;
  RampsModbusData.port = rampsData->modbusUart;
  RampsModbusData.u8id = MODBUS_ADDRESS;
  RampsModbusData.u16timeOut = 1000;
  RampsModbusData.EN_Port = NULL;
  RampsModbusData.u16regs = (uint16_t *) (&rampsData->shared);
  RampsModbusData.u16regsize = sizeof(rampsData->shared) / sizeof(uint16_t);
  RampsModbusData.xTypeHW = USART_HW;
  ModbusInit(&RampsModbusData);
  ModbusStart(&RampsModbusData);

  StartRampsTask(rampsData);
}

/**
 * This method implements the logic to generate a single pulse when the controlled axis is
 * configured in synchro mode.
 * When operating in this mode the timer is stopped immediately after reaching the
 * requested number of steps
 * @param data Reference to the ramps handler data structure
 */
void motorSynchroModeIsr(rampsHandler_t *data) {
  if (HAL_GPIO_ReadPin(DIR_GPIO_PORT, DIR_PIN) == GPIO_PIN_SET) {
    data->shared.currentPosition++;
  } else {
    data->shared.currentPosition--;
  }

  HAL_TIM_PWM_Stop_IT(data->motorPwmTimer, TIM_CHANNEL_1);
}


/**
 * Call this method from the interrupt service routine associated with the pwm generation timer
 * used to control the stepper shared steps generation
 * @param rampsTimer handle reference to the ramps generation time, the same as the calling isr
 * @param data the data structure holding all the rotary controller data
 */
void RampsMotionIsr(rampsHandler_t *data) {
  // Controller is in index mode
  if (data->shared.mode == MODE_SYNCHRO) {
    motorSynchroModeIsr(data);
  }
}

/**
 * This function initializes the data and resources for the operation of the controlled axis so that it moves
 * in sync with an encoder reference.
 */
void SyncMotionInit(rampsHandler_t *data) {
  rampsSharedData_t *shared = &data->shared;

  // Verify the ratio to be acceptable, return and set error otherwise
  if (shared->synRatioNum == 0 ||
      shared->synRatioDen == 0 ||
      shared->synRatioDen > shared->synRatioNum) {
    shared->mode = MODE_SYNCHRO_BAD_RATIO;
    return;
  }

  // Configure the values for the bresenham interpolation
  if (shared->synRatioDen < 0) { data->syncData.yi = -1; }
  else { data->syncData.yi = 1; }

  data->syncData.D = 2 * (shared->synRatioDen - shared->synRatioNum);

  // Configure the timer settings for the pwm generation, will be used as one pulse
  __HAL_TIM_SET_AUTORELOAD(data->motorPwmTimer, 150);
  __HAL_TIM_SET_COMPARE(data->motorPwmTimer, TIM_CHANNEL_1, 75);

  // Ensure the current and final positions are equal
  shared->currentPosition = shared->finalPosition;
  shared->mode = MODE_SYNCHRO;
}

/**
 * Interrupt service routine to handle indexing with a dedicated timer, this is used for indexing
 * during synchro mode
 * @param data
 */
void IndexMotionIsr(rampsHandler_t *data) {
  // Stop the timer and exit if mode is 0
  rampsSharedData_t *shared = &data->shared;
  rampsIndexData_t *indexData = &data->indexData;

  // Check for start conditions, if start conditions then load the parameters
  if (shared->indexDeltaSteps != 0 && indexData->currentStep == indexData->totalSteps) {
    if (shared->indexDeltaSteps > 0) {
      indexData->direction = 1;
    } else {
      indexData->direction = -1;
    }

    indexData->currentStep = 0;
    indexData->totalSteps = abs(shared->indexDeltaSteps);
    // Set to 0 so the HMI knows we have set the new destination
    shared->indexDeltaSteps = 0;

    // Handle the initialization of the motion when step is 0
    indexData->floatAccelInterval = shared->acceleration;
    shared->currentSpeed = shared->minSpeed;
    indexData->stepRatio =
    (float) shared->stepRatioNum /
    (float) shared->stepRatioDen;
    indexData->decelSteps = 0;
  }

  // If we're not indexing, run the interrupt at slow speed and wait for running requests
  if (shared->indexDeltaSteps == 0 && indexData->currentStep == indexData->totalSteps) {
    __HAL_TIM_SET_AUTORELOAD(data->indexRefreshTimer, 10000);
    __HAL_TIM_SET_COMPARE(data->indexRefreshTimer, TIM_CHANNEL_1, 10);
    return;
  }

  // Handle acceleration phase
  if (shared->currentSpeed < shared->maxSpeed && (indexData->currentStep < indexData->totalSteps / 2)) {
    shared->currentSpeed = shared->currentSpeed + shared->acceleration;
    indexData->floatAccelInterval = (float) RAMPS_CLOCK_FREQUENCY * indexData->stepRatio / shared->currentSpeed;

    if (shared->currentSpeed > shared->maxSpeed) {
      shared->currentSpeed = shared->maxSpeed;
    }
  } else if (indexData->decelSteps == 0) {
    // Store the count of steps it took to accelerate, so it can be used to define when to start
    // decelerating without doing further calculations.
    indexData->decelSteps = indexData->currentStep;
  }

  // Handle deceleration phase
  if (
  shared->currentSpeed > shared->minSpeed &&
  (indexData->currentStep > indexData->totalSteps / 2) &&
  (indexData->currentStep > (indexData->totalSteps - indexData->decelSteps))
  ) {
    shared->currentSpeed = shared->currentSpeed - (float) shared->acceleration;
    indexData->floatAccelInterval = (float) RAMPS_CLOCK_FREQUENCY * indexData->stepRatio / shared->currentSpeed;
  }

  // Configure the timer preload and the pwm duty cycle to 50%
  if (indexData->floatAccelInterval > 65535) {
    __HAL_TIM_SET_AUTORELOAD(data->indexRefreshTimer, 65535);
    __HAL_TIM_SET_COMPARE(data->indexRefreshTimer, TIM_CHANNEL_1, 10);
  } else {
    __HAL_TIM_SET_AUTORELOAD(data->indexRefreshTimer, (uint16_t) indexData->floatAccelInterval);
    __HAL_TIM_SET_COMPARE(data->indexRefreshTimer, TIM_CHANNEL_1, 10);
  }

  // Increment the current step
  indexData->currentStep++;
  shared->finalPosition += indexData->direction;
}


/**
 * This function has to be called from a simple timer interrupt routine
 * happening at regular intervals which shall match the maximum frequency
 * supported by either the controller speed or the stepper shared controller
 * Starting timer interval frequency set to 50Khz
 * @param data Reference to the ramps handler data structure
 */
void SyncMotionIsr(rampsHandler_t *data) {
  // Skip running the routine if the interrupt didn't trigger for our timer
  if ((data->synchroRefreshTimer->Instance->SR & 0b1) == 0) {
    return;
  }

  rampsSharedData_t *shared = &(data->shared);
  rampsSyncData_t *sync_data = &(data->syncData);
  updateScales(&data->scales);

  if (shared->mode == MODE_SYNCHRO && shared->finalPosition != shared->currentPosition) {
    if (shared->finalPosition > shared->currentPosition) {
      HAL_GPIO_WritePin(DIR_GPIO_PORT, DIR_PIN, GPIO_PIN_SET);
    } else {
      HAL_GPIO_WritePin(DIR_GPIO_PORT, DIR_PIN, GPIO_PIN_RESET);
    }

    if ((data->synchroRefreshTimer->Instance->CR1 & 1) == 1) {
      HAL_TIM_PWM_Start_IT(data->motorPwmTimer, TIM_CHANNEL_1);
    }

    return;
  }

  // Skip Conditions
  // The routine will be skipped if there is motion in progress or if the synchro is disabled
  if (shared->mode != MODE_SYNCHRO) {
    return;
  }

  sync_data->positionPrevious = sync_data->positionCurrent;
  sync_data->positionCurrent = data->scales.scalePosition[shared->synScaleIndex].positionCurrent;

  if (sync_data->positionPrevious < sync_data->positionCurrent) {
    HAL_GPIO_WritePin(DIR_GPIO_PORT, DIR_PIN, GPIO_PIN_SET);
    sync_data->direction = +1;
    for (int32_t x = sync_data->positionPrevious; x < sync_data->positionCurrent; ++x) {
      if (sync_data->D > 0) {
        // Error greater than 0, step forward the controlled axis
        shared->finalPosition += sync_data->yi;
        sync_data->D = sync_data->D + (2 * (shared->synRatioDen - shared->synRatioNum));
      } else {
        sync_data->D = sync_data->D + 2 * shared->synRatioDen;
      }
    }
  } else if (sync_data->positionPrevious > sync_data->positionCurrent) {
    HAL_GPIO_WritePin(DIR_GPIO_PORT, DIR_PIN, GPIO_PIN_RESET);
    sync_data->direction = -1;
    for (int32_t x = sync_data->positionPrevious; x > sync_data->positionCurrent; --x) {
      if (sync_data->D < 0) {
        // Error greater than 0, step forward the controlled axis
        shared->finalPosition -= sync_data->yi;
        sync_data->D = sync_data->D - (2 * (shared->synRatioDen - shared->synRatioNum));
      } else {
        sync_data->D = sync_data->D - 2 * shared->synRatioDen;
      }
    }
  }
}

/**
 * This method is used to initialize the RTOS task responsible for controlling the ramps
 * ramp generator.
 */
void StartRampsTask(rampsHandler_t *rampsData) {
  rampsData->TaskRampsHandle = osThreadNew(RampsTask, rampsData, &taskRampsAttributes);
}

/**
 * This is the FreeRTOS task invoked to handle the general low priority task responsible
 * for the management of all the ramps system operation.
 * @param argument Reference to the ramps handler data structure
 */
void RampsTask(void *argument) {
  rampsHandler_t *data = (rampsHandler_t *) argument;
  rampsSharedData_t *shared = &data->shared;

  uint16_t ledTicks = 0;
  for (;;) {
    osDelay(50);
    // Refresh scales position reporting in the modbus shared data
    for (int i = 0; i < SCALES_COUNT; ++i) {
      data->shared.scalesPosition[i] = data->scales.scalePosition[i].positionCurrent;
    }

    // Handle sync mode request
    if (shared->mode == MODE_SYNCHRO_INIT) {
      SyncMotionInit(data);
    }

    // Handle request to set encoder count value
    if (shared->mode == MODE_SET_ENCODER) {
      // Reset everything and configure the provided value
      int scaleIndex = data->shared.encoderPresetIndex;
      // Counter reset
      __HAL_TIM_SET_COUNTER(data->scales.scaleTimer[scaleIndex], 0);
      data->scales.scalePosition[scaleIndex].encoderCurrent = 0;
      data->scales.scalePosition[scaleIndex].encoderPrevious = 0;

      // Sync data struct reset
      data->scales.scalePosition[scaleIndex].positionCurrent = shared->encoderPresetValue;

      // Shared data struct reset
      shared->scalesPosition[scaleIndex] = shared->encoderPresetValue;
      shared->mode = MODE_HALT; // Set proper mode
    }
  }
}
