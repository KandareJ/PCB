/*
`*`Copyright (c) 2015-2019, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 _______________________________________________________________________________
 *************************** NOTES ON USING THIS CODE **************************
 *******************************************************************************
 _______________________________________________________________________________
LAST MODIFIED: 20 July 2022 by Jared Brinkman

DESCRIPTION OF CODE FUNCTIONALITY:
This BLUETOOTH code is used on BACPAC PCBs in conjunction with the Spine Simulator app and BACPAC arrays in order
to read the impedance of each sensor on an array properly.

EXAMPLE OUTPUT:
6026,157.14,1209.18,475.99,804.80,41.14,264.74,71.10,49999.99,33474.11,3166.80,49999.99,49999.99,6816.37,49999.99,9870.85,4605.72
(Time(milliseconds),Sensor 1 Impedance, Sensor 2 Impedance, ... Sensor 16 Impedance\n)


 The sampling duration in the CC2640R2_LAUNCHXL.c file (line 131) is a limiting
 factor for the MUXFREQ settings. The maximums are listed below:

 MUXFREQ max | samplingDuration
 650         | 341_US
 800         | 170_US
 _______________________________________________________________________________

 These values must be changed otherwise there will be a bottleneck of information
 over the UART to USB connection that will limit the dataspeed

 Notes on MUXFREQ setting:
 Nominal    | Actual
 200        | 201.6
 400        | 403.3
 650        | 656.0
 _______________________________________________________________________________

 When changing the number of channels you want to measure you much change the
 variable "channels" on line 96 length of the lastAmp array on line 97 to the
 same number.

 I2C TRANSMISSION PROTOCOL
 txBuffer1[0] is the high byte while txBuffer1[1] is the low byte. (same with txBuffer3)
 In order to transfer an integer of size 12 bits we need at least 2 bytes.  This is why we have
 to declare and define two bytes in the code.  See the spec sheet via box Important
 Data Sheets for a more detailed explanation of how we are using I2C protocol.

 >>8 is used to not read the lower 8 bits while simply setting txBuffer1[1] = signal.ampAC will
 read in the first 8 bits (lowest 8 bits)

 hex converter: https://www.cs.princeton.edu/courses/archive/fall07/cos109/bc.html for Slave Address
 */

/* C included packages */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* Driver Header files */
#include <ti/drivers/GPIO.h>
#include <ti/drivers/PIN.h>
#include <ti/drivers/UART.h>
#include <ti/drivers/ADC.h> //http://dev.ti.com/tirex/content/simplelink_cc13x0_sdk_1_00_00_13/docs/tidrivers/doxygen/html/_a_d_c_8h.html
#include <ti/drivers/timer/GPTimerCC26XX.h>
#include <ti/drivers/I2C.h>
#include <ti/drivers/i2c/I2CCC26XX.h> //http://software-dl.ti.com/dsps/dsps_public_sw/sdo_sb/targetcontent/tirtos/2_20_00_06/exports/tirtos_full_2_20_00_06/products/tidrivers_full_2_20_00_08/docs/doxygen/html/_i2_c_c_c26_x_x_8h.html
#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26XX.h>
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <xdc/runtime/Types.h>
#include <xdc/runtime/Timestamp.h>
#include <ti/drivers/SD.h>
#include <xdc/runtime/System.h>
#include <ti/sysbios/hal/Hwi.h>

/* Board Header file */
#include "Board.h"
#include "DiskAccess.h"
#include "Storage.h"
#include "Serializer.h"
#include "sensors.h"
#include "ImpedanceCalc.h"

/////////////////////////// pin configuration ///////////////////////
/* Pin driver handles */
static PIN_Handle muxPinHandle;

/* Global memory storage for a PIN_Config table */
static PIN_State muxPinState;

//////////////// Global Variables ///////////////////////
uint8_t muxmod = 0; // allocates which sensor is being read (Values 0-15)
uint16_t adcValue = 0; // adc read during that callback cycle
float impedance = 0; // impedance (resistance) calculated for the current sensor
char* uartBuf; // used to store data that will then be output to the serial monitor
int stutter = 0; //checks to make sure we don't stutter more than 3 times in one cycle
const int channels = 16; //the number of channels corresponds to the number of sensors and should always be 16.
static int MUXFREQ = 160;  //CHANGED FROM 400 // Frequency (this equals the number of channels to read each second). Must be less than half of DAC frequency (~line 320).
static float PERIOD_OF_TIME = 2.57546812; // time it takes to complete one round through the DACtimercallback
int res1 = 0; // confirms an adcRead
uint8_t adcPosRead= 0; // counts the number of positive adc reads
int AUTOMATE = 1; // AUTOCAL - increments tap.
uint16_t adcSum = 0; // compiles 4 adcReads in order to find average
//static float PERIOD_OF_TIME = 2.4067235; //WITH CODE STUTTER INTEGRATED
float milliseconds = 0; // current time stamp
uint8_t sensorValues[channels] = {125,125,125,125,125,125,125,125,125,125,125,125,125,125,125,125}; //initial tap value for each sensor (the tap value is a measure of the resistance of the potentiometer (variable resistor) in the circuit)
//int taps[8] = {1,7,13,31,60,125,200,250}; // The discrete tap values that we want to use, the 1 and 250 on the ends are for error handling and should never actually be used. We want to expand this to use every possible tap value.
int taps[255] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255};
//int currentTap[channels] = {5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5}; //stores the tap value for each sensor. i.e. currentTap[0]=5 means sensor 0 is at tap value 5.  taps[5] tells us the value of that tap value.
 int currentTap[channels] = {124,124,124,124,124,124,124,124,124,124,124,124,124,124,124,124};
int lowCuts[8] = {0,1925,1700,1650,1500,1750,0,0}; // the adc value at which we will switch to the next tap value because the adc value is getting too low
// int lowCuts[255] = {0, ... 0,0};
int highCuts[8] = {0,4000,2600,2700,2400,2250,2450,0}; // the adc value at which we will switch to the previous tap value because the adc value is getting too high
// int highCuts[255] = {0,4000 ... 0};
int lastAmp[channels] = {250,250,250,250,250,250,250,250,250,250,250,250,250,250,250,250};
//Used to be 500, but modified to 250 because we were just dividing it by 2 anyway. Initialize all sensors to the value (in milli-amps) you want to run the signal at (array must be as long as number of channels).
#define ADC_SAMPLE_COUNT    (1) //Number of samples to read each time we call the callback function (how many samples per row on output)
#define ADCMULTIPLE         (1) //How many times to call the ADC within one mux switch (total samples per switch will equal ADCMULTIPLE*ADC_SAMPLE_COUNT)
#define ADChiLimit          3000 //raw value that will be the adc upper threshold
#define ADCloLimit          200 // raw value that will be the adc lower threshold
#define minVOLT             (80)

 /* Starting sector to write/read to on the SD card*/
#define STARTINGSECTOR 0
#define BYTESPERKILOBYTE 1024

// this table declares the specific Mux Pins which are to be used later in the code
PIN_Config muxPinTable[] = {
    IOID_28 | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MAX, //ENABLE
    IOID_22 | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MAX, //A3
    IOID_23 | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MAX, //A2
    IOID_12 | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MAX, //A1
    IOID_15 | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MAX, //A0
    PIN_TERMINATE
};

///////////////////////////////////// I2C preamble //////////////////////////
uint8_t rxBuffer1[0];          // Receive buffer for the DAC that is on.
uint8_t txBuffer1[2];          // Transmit buffer for the DAC that is on.
uint8_t rxBuffer2[0];          // Receive buffer for the DAC that is off. When we remove it from the board delete these 2 lines.
uint8_t txBuffer2[2];          // Transmit buffer for the DAC that is off.
uint8_t rxBuffer3[1];          // Receive buffer for the potentiometer
uint8_t txBuffer3[2];          // Transmit buffer for the potentiometer
bool transferDone = false;     // signify the I2C has finished for this cycle
bool openDone = true;          // signify the I2C has opened successfully in order to transmit data
uint8_t counterDAC = 2;        // declaring the counterDac used in DACtimerCallback function

//Used to store the signal2 current amplitude (max is 4095)
struct {
    uint16_t ampAC : 12; // signal to the system that will go high.
    uint16_t ampDC : 12; // reference signal. Remains 0 so we have something to measure against.
} Signal; //&&& signal-> Signal

/*///////////////////////////////////// Function Declarations /////////////////////////////////////
 These functions act as callback functions and will interrupt any running code every few milliseconds or so in order
 to perform their functions.  The majority of the functionality on the PCB happens in DACtimerCallback */

static void i2cWriteCallback(I2C_Handle handle, I2C_Transaction *transac, bool result);
void DACtimerCallback(GPTimerCC26XX_Handle handle, GPTimerCC26XX_IntMask interruptMask);

/* Driver handles */
GPTimerCC26XX_Handle hDACTimer;
I2C_Handle I2Chandle;
I2C_Params I2Cparams;
I2C_Transaction i2cTrans1;
I2C_Transaction i2cTrans2;
I2C_Transaction i2cTrans3;

///////////////////////////////////// ADC/Display Preamble /////////////////////////////////
/* ADC Global Variables */
ADC_Handle adc; // used in turning on adc
ADC_Params params; // used in turning on adc

#define UARTBUFFERSIZE   64
int modBuffer[ADC_SAMPLE_COUNT];
char uartTxBuffer[UARTBUFFERSIZE]; //  ...SIZE]= {0};

UART_Handle uart;

void uartCallback(UART_Handle handle, void *buf, size_t count) { return; }



//                              ======== MAIN THREAD ========

// this function is run immediately when the PCB is programmed. Any time you reset the PCB it will run again.
void Sensors_init() {
    uartBuf = (char*) malloc(256 * sizeof(char));

    // Initialize Variables
    Signal.ampAC = lastAmp[0]; //Set the Signal to what it is initialized to in the array declared on line 138 (lastAmp[])

    // Call Driver Init Functions
    I2C_init();
    ADC_init();
    GPIO_init();
    da_initialize();

    ////////////////////////////////////////////// GPIO /////////////////////////////////////////
    /* Configure the LED pins */
    GPIO_setConfig(Board_GPIO_LED0, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    /* Turn on user LED */
    GPIO_write(Board_GPIO_LED0, Board_GPIO_LED_OFF);

    /* Configure the LED pins */
    GPIO_setConfig(Board_GPIO_LED1, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    /* Turn on user LED */
    GPIO_write(Board_GPIO_LED1, Board_GPIO_LED_OFF);

    /* Configure the LED pins */
    GPIO_setConfig(Board_DIO0, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    /* Turn on user LED */
    GPIO_write(Board_DIO0, 0);

    ////////////////////////////////////////////// I2C //////////////////////////////////////////
    // Configure I2C parameters.
    I2C_Params_init(&I2Cparams);
    I2Cparams.bitRate = I2C_400kHz;
    I2Cparams.transferMode = I2C_MODE_CALLBACK;
    I2Cparams.transferCallbackFxn = i2cWriteCallback;

    // Initialize master I2C transaction structure for the DAC that is ON
    i2cTrans1.writeBuf     = txBuffer1; //AC (square) signal2
    i2cTrans1.writeCount   = 2;
    i2cTrans1.readBuf      = NULL;
    i2cTrans1.readCount    = 0;
    i2cTrans1.slaveAddress = 0x4C; // See data sheet via Box-> Important Data Sheets for appropriate address for the DAC.

    // Initialize master I2C transaction structure for the DAC that is OFF (When we confirm this DAC isn't necessary delete this block of code)
    i2cTrans2.writeBuf     = txBuffer2; //DC signal2
    i2cTrans2.writeCount   = 2;
    i2cTrans2.readBuf      = NULL;
    i2cTrans2.readCount    = 0;
    i2cTrans2.slaveAddress = 0x4D; // See data sheet via Box-> Important Data Sheets for appropriate address for the DAC.

    // Initialize master I2C transaction structure for the potentiometer
        i2cTrans3.writeBuf     = txBuffer3; //DC Signal
        i2cTrans3.writeCount   = 2;
        i2cTrans3.readBuf      = NULL;
        i2cTrans3.readCount    = 0;
        i2cTrans3.slaveAddress = 0x2C; // See data sheet via Box-> Important Data Sheets for appropriate address for the Potentiometer.

    ////////////////////////////////////////////// GPTimer for DAC //////////////////////////////////////////
    GPTimerCC26XX_Params paramsDAC;
    GPTimerCC26XX_Params_init(&paramsDAC);
    paramsDAC.width          = GPT_CONFIG_32BIT;
    paramsDAC.mode           = GPT_MODE_PERIODIC_UP;
    paramsDAC.debugStallMode = GPTimerCC26XX_DEBUG_STALL_OFF;
    hDACTimer = GPTimerCC26XX_open(CC2640R2_LAUNCHXL_GPTIMER0A, &paramsDAC); //Need timer 0A for ADCbuf
    if(hDACTimer == NULL) {
        while(1);
    }

    Types_FreqHz  freq;
    BIOS_getCpuFreq(&freq); //48MHz
    GPTimerCC26XX_Value loadValDAC = 48000000/(MUXFREQ*4);
    loadValDAC = loadValDAC - 1;


    GPTimerCC26XX_setLoadValue(hDACTimer, loadValDAC);
    GPTimerCC26XX_registerInterrupt(hDACTimer, DACtimerCallback, GPT_INT_TIMEOUT);

    // Open I2C
    I2Chandle = I2C_open(Board_I2C0, &I2Cparams);

    if (I2Chandle == NULL) {
        // Error opening I2C
        openDone = false;
        while (1);
    }

////////////////////////////////////////////////////////////////// MUX //////////////////////////////////////////////////////////
    muxPinHandle = PIN_open(&muxPinState, muxPinTable);
    if(!muxPinHandle) {
        /* Error initializing mux output pins */
        while(1);
    }
    PIN_setOutputValue(muxPinHandle, IOID_28, 0); // turn the mux on by initializing the enable pin to 0 (0 means on for this mux)

////////////////////////////////////////////////////////////////// ADC/ UART //////////////////////////////////////////////////////////
    /* Create a UART with data processing off. */
    UART_Params uartParams;
    UART_init();
    UART_Params_init(&uartParams);
    uartParams.writeDataMode = UART_DATA_BINARY;
    uartParams.writeMode = UART_MODE_CALLBACK;
    uartParams.writeCallback = uartCallback;
    uartParams.baudRate = 460800; // Baud Rate. To read from your terminal baud rate to be the same.
    uart = UART_open(Board_UART0, &uartParams);

    ADC_Params_init(&params);
    adc = ADC_open(0, &params);
    if (adc == NULL) {
        // Error initializing ADC channel 0
        while(1);
    }

    Signal.ampAC = lastAmp[muxmod]; // this one will go high but is initialized to 0 to save battery
    Signal.ampDC = 0; // this signal is our reference signal.  It needs to be low so we can measure against something.
    txBuffer1[0] = Signal.ampAC >> 8; //high byte
    txBuffer1[1] = Signal.ampAC; //low byte
    txBuffer2[0] = Signal.ampDC >> 8; //high byte. Delete when confirmed DAC 2 unnecessary.
    txBuffer2[1] = Signal.ampDC; //low byte. Delete when confirmed DAC 2 unnecessary.
    I2C_transfer(I2Chandle, &i2cTrans1); // communication for the DAC that is currently ON.
    I2C_transfer(I2Chandle, &i2cTrans2); // communication for the DAC that is currently OFF. Delete when confirmed we don't need it.

    // set the mux to array 0 which is really pin 10 on the PCB
    PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
    PIN_setOutputValue(muxPinHandle, IOID_22, 1); //S3
    PIN_setOutputValue(muxPinHandle, IOID_23, 0); //S2
    PIN_setOutputValue(muxPinHandle, IOID_12, 1); //S1
    PIN_setOutputValue(muxPinHandle, IOID_15, 0); //S0

    // AUTOCAL CODE. Comment out DA_get_status and uncomment Sensors_start_timers()
//    DA_get_status(da_load(), "Loading Disk");
    Sensors_start_timers();

    // da_load will attempt to upload data to the SD card.
    // Sensors_start_timers() will automatically start spitting out data.
}

/////////////////////////////////////////// I2C Functions /////////////////////////////////////////////////
static void i2cWriteCallback(I2C_Handle handle, I2C_Transaction *transac, bool result){
    // Set length bytes
    if (result) {
        transferDone = true;
    }
    else {
        // Transaction failed, act accordingly...
        transferDone = false;
    }
};

void muxPinReset(uint8_t muxmod_GS){
    switch(muxmod_GS) {
    //  AUTOCAL CODE. SWITCH CASES
      case 14: // AUTOCAL
//        case 10: //sensor 0 array pin 10
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 0); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 0); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 0); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 0); //S0
            break;
        case 13: // sensor 1 array pin 13
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 0); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 0); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 0); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 1); //S0
            break;
        case 15: // AUTOCAL
//        case 11: // sensor 2 array pin 11
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 0); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 0); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 1); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 0); //S0
            break;
         case 12: // ATUOCAL
//        case 8: // sensor 3 array pin 8
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 0); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 0); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 1); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 1); //S0
            break;
        case 11: // AUTOCAL
//        case 14: //sensor 4 array pin 14
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 0); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 1); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 0); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 0); //S0
            break;
        case 10: // AUTOCAL
//        case 12: //sensor 5 array pin 12
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 0); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 1); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 0); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 1); //S0
            break;
        case 9: // AUTOCAL
//        case 15: //sensor 6 array pin 15
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 0); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 1); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 1); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 0); //S0
            break;
        case 0: // AUTOCAL
//        case 7: // sensor 7 array pin 7
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 0); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 1); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 1); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 1); //S0
            break;
        case 1: // AUTOCAL
//        case 4: // sensor 8 array pin 4
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 1); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 0); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 0); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 0); //S0
            break;
        case 2: // AUTOCAL
//        case 6: //sensor 9 array pin 6
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 1); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 0); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 0); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 1); //S0
            break;
        case 3: // AUTOCAL
//        case 0: // sensor 10 array pin 0
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 1); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 0); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 1); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 0); //S0
            break;
        case 4: // AUTOCAL
//        case 5: // sensor 11 array pin 5
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 1); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 0); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 1); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 1); //S0
            break;
        case 5: // AUTOCAL
//        case 2: // sensor 12 array pin 2
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 1); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 1); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 0); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 0); //S0
            break;
        case 6: // AUTOCAL
//        case 3: // sensor 13 array pin 3
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 1); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 1); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 0); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 1); //S0
            break;
       case 7: // AUTOCAL
//        case 1: // sensor 14 array pin 1
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 1); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 1); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 1); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 0); //S0
            break;
        case 8: // AUTOCAL
//        case 9: // sensor 15 array pin 9
            PIN_setOutputValue(muxPinHandle, IOID_28, 0); //E
            PIN_setOutputValue(muxPinHandle, IOID_22, 1); //S3
            PIN_setOutputValue(muxPinHandle, IOID_23, 1); //S2
            PIN_setOutputValue(muxPinHandle, IOID_12, 1); //S1
            PIN_setOutputValue(muxPinHandle, IOID_15, 1); //S0
            break;
    }
}

// this is where the bulk of the functionality of this file takes place.
void DACtimerCallback(GPTimerCC26XX_Handle handle, GPTimerCC26XX_IntMask interruptMask) {

    if(counterDAC == 0){
                                           ////////// ADC Read #1 ///////////

        PIN_setOutputValue(muxPinHandle, IOID_28, 0); // turn the mux on by initializing the enable pin to 0 (0 means on for this mux)
        res1 = ADC_convert(adc, &adcValue); // read the current adc Value
        if (res1 == ADC_STATUS_SUCCESS) {
            adcSum += adcValue; // add the read to the adc Sum if it is a positive read
            adcPosRead +=1; // increment our number of positive reads
        }
        else {
            res1 = ADC_convert(adc, &adcValue);
            if (res1 == ADC_STATUS_SUCCESS) { // if it didn't work the first time try another adc Read
                adcSum += adcValue;
                adcPosRead +=1;
            }
        }
        PIN_setOutputValue(muxPinHandle, IOID_28, 1); // turn the mux ff by initializing the enable pin to 1 (0 means on for this mux)

        counterDAC++; //increments DACtimerCallback counter to 1
    }
    else if (counterDAC == 1){

                                          ////////// ADC Read #2 (refer to adc Read #1 for comments) ///////////

        PIN_setOutputValue(muxPinHandle, IOID_28, 0);
        res1 = ADC_convert(adc, &adcValue);
        if (res1 == ADC_STATUS_SUCCESS) {
            adcSum += adcValue;
            adcPosRead +=1;
        }
        else {
            res1 = ADC_convert(adc, &adcValue);
            if (res1 == ADC_STATUS_SUCCESS) {
                adcSum += adcValue;
                adcPosRead +=1;
            }
        }
        PIN_setOutputValue(muxPinHandle, IOID_28, 1);

        counterDAC++; // increments DACtimerCallback counter to 2
    }

    else if (counterDAC == 2){

                                            ////////// ADC Read #3 (refer to adc Read #1 for comments) ///////////

        PIN_setOutputValue(muxPinHandle, IOID_28, 0);
        res1 = ADC_convert(adc, &adcValue);
        if (res1 == ADC_STATUS_SUCCESS) {
            adcSum += adcValue;
            adcPosRead +=1;
        }
        else {
            res1 = ADC_convert(adc, &adcValue);
            if (res1 == ADC_STATUS_SUCCESS) {
                adcSum += adcValue; //
                adcPosRead +=1;
            }
        }
        PIN_setOutputValue(muxPinHandle, IOID_28, 1);

        counterDAC++; // increments DACtimerCallback counter to 3
    }
    else if (counterDAC == 3) {

        storage_buffer_length = 0; // stores length of the data in the buffer. Useful for writing purposes.

                                                ////////// ADC Read #4 (refer to adc Read #1 for comments)///////////

        PIN_setOutputValue(muxPinHandle, IOID_28, 0);
        res1 = ADC_convert(adc, &adcValue);
        if (res1 == ADC_STATUS_SUCCESS) {
            adcSum += adcValue;
            adcPosRead +=1;
        }
        else {
            res1 = ADC_convert(adc, &adcValue);
            if (res1 == ADC_STATUS_SUCCESS) {
                adcSum += adcValue;
                adcPosRead +=1;
            }
        }
        PIN_setOutputValue(muxPinHandle, IOID_28, 1);

                                        ////////// AVERAGE ADCVALUE ///////////

        // divide the adcSum by the number of positive reads
        switch(adcPosRead) {
            case 1:
                adcValue = adcSum;
                break;
            case 2:
                adcValue = adcSum/2;
                break;
            case 3:
                adcValue = adcSum/3;
                break;
            case 4:
                adcValue = adcSum/4;
                break;
            case 0:
                adcValue = 1;
                break;
            default:
                adcValue = 2;

        }

        // AUTOCAL CODE FOR CALLIBRATION - Uncomment out this section. Comment out everything from "ADC STUTTER" until "RESET POT..." besides the "INCREMENT SENSOR" section
        if (muxmod == 0) {
            System_sprintf(uartBuf, "%u,%u,%u,", (uint32_t)milliseconds, taps[AUTOMATE], adcValue);
            print(uartBuf);
        }
        else if (muxmod < 15) {
            System_sprintf(uartBuf, "%u,", adcValue);
            print(uartBuf);
        }
        else{
            System_sprintf(uartBuf, "%u\n\r", adcValue);
            print(uartBuf);

            AUTOMATE++;

            if (AUTOMATE > 253 ) {
                AUTOMATE = 1;
            }
        }

//                                        ////////// ADC STUTTER ////////// -- JR
//
//        /*Run the normal process (Calculate and write impedance to SD card) if adc Value is below the upper fence, or if we have already stuttered 3 times */

//        if ( (adcValue < 2950) || (stutter > 3) ) {
//
//                                       ////////// CALCULATE IMPEDANCE //////////
//
//            if (adcValue < 400){
//                impedance = 49999.99; // if our adcValue is too low. We don't want to interpret it as valid data.
//            }
//                    else {
//                        for (uint16_t impCounter = 1; impCounter < 254; impCounter++) {
//                            if (sensorValues[muxmod] == taps[impCounter]) {
//                                impedance = ImpedanceCalc(impCounter, adcValue, taps);
//                                break;
//                            }
//                        }
//                    }
////             else if (sensorValues[muxmod] == taps[1]){
////                 impedance = fabs((-189.4 * adcValue + 518778)/(adcValue + -929.0));
////             }
////             else if (sensorValues[muxmod] == taps[2]){
////                 impedance = fabs((-206.0 * adcValue + 764491)/(adcValue + -919.3));
////             }
////             else if (sensorValues[muxmod] == taps[3]){
////                 impedance = fabs((-213.7 * adcValue + 1407631)/(adcValue + -918.7));
////             }
////             else if (sensorValues[muxmod] == taps[4]){
////                 impedance = fabs((-221.1 * adcValue + 2427576)/(adcValue + -917.6));
////             }
////             else if (sensorValues[muxmod] == taps[5]){
////                 impedance = fabs((-307.9 * adcValue + 4850328)/(adcValue + -916.3));
////             }
////             else if (sensorValues[muxmod] == taps[6]){
////                 impedance = fabs((-66.6 * adcValue + 6980902)/(adcValue + -922.7));
////             }
//             if (impedance > 49999.99){
//                 impedance = 49999.99; // we only need impedance values within a certain range. This is our cap.
//             }
//
//                             /* IMPORTANT: WRITE IMPEDANCE VALUE TO SD CARD AND/OR UART BUF */
////             System_sprintf(uartBuf, "%u, %u, %u, %u, %u \n\r", sensorValues[muxmod], adcValue, lowCuts[currentTap[muxmod]], highCuts[currentTap[muxmod]], currentTap[muxmod]);
//
//             if (serializer_isFull()) serializer_setTimestamp((uint16_t)milliseconds); // checking if 16 impedance values have been added to the array
//                 serializer_addImpedance(impedance); // adding the current impedance value to the serializer array
//             if (serializer_isFull() && Semaphore_pend(storage_buffer_mutex, 0)) {
//                 storage_buffer_length += serializer_serialize(storage_buffer);
//                 serializer_serializeReadable(uartBuf); // convert serializer array so it is readable by UART (comment out if UART is unnecessary)
//                 print(uartBuf); // write to the UART Buf (comment out if UART is unnecessary)
//                 Semaphore_post(storage_buffer_mailbox); // writing to the sd card
//              }
//
//              GPIO_write(Board_GPIO_LED1, Board_GPIO_LED_OFF);
//
//                                       ////////// CHANGE TAP VALUE FOR NEXT READ IF NECESSARY //////////
//
//              /* Before we change to the next sensor, if the adcValue was too low this time around (i.e. doesn't fall into the "most accurate" adc range for that tap value,
//              we want the tap value (potentiometer value) for that pin to shift higher (increasing the resistance and the adc) for the next read of that pin. */
//
//              if (adcValue < lowCuts[currentTap[muxmod]]) {
//                  currentTap[muxmod]++; // move up a tap
//                  sensorValues[muxmod]  = taps[currentTap[muxmod]]; // assign the value of that tap to the sensor
//              }
//              else if (adcValue > highCuts[currentTap[muxmod]]) {
//                  currentTap[muxmod]--; // move down a tap
//                  sensorValues[muxmod] = taps[currentTap[muxmod]]; // assign the value of that tap to the sensor
//              }
//              if (currentTap[muxmod] > 6) // if we are out of our tap value range we want to bring it back.
//              {
//                  currentTap[muxmod] = 6;
//                  sensorValues[muxmod] = taps[currentTap[muxmod]]; // assign the value of that tap to the sensor
//              }
//              else if (currentTap[muxmod] < 1) // if we are out of our tap value range we want to bring it back.
//              {
//                  currentTap[muxmod] = 1;
//                  sensorValues[muxmod] = taps[currentTap[muxmod]]; // assign the value of that tap to the sensor
//              }
                                          //////// INCREMENT SENSOR ///////
              /*
              * IMPORTANT: this is where the sensor we are dealing with changes. i.e. from sensor 1 to sensor 2.  The whole process repeats here.
              */
              muxmod += 1;
              if(muxmod == channels){
                  muxmod = 0; // reset counter back to zero if it equals the number of channels
              }
                                         ///////// STUTTER CODE ///////////

//              stutter = 0; //The ADC value is below 2950, so no stuttering occured
//        }
//
//        else {  //Stutter the code and move down in the tap if the ADC value is too high
//            currentTap[muxmod]--; // move down a tap
//            sensorValues[muxmod] = taps[currentTap[muxmod]]; // assign the value of that tap to the sensor
//            stutter++;
//                }

                            /////////// RESET POTENTIOMETER AND MUX FOR NEXT SENSOR READ  ///////////

        muxPinReset(muxmod); // convert the mux to account for new sensor channel

        PIN_setOutputValue(muxPinHandle, IOID_28, 1); // turn the mux off by initializing the enable pin to 1 (0 means on for this mux)

        // To prevent values carrying over from cycle to cycle.
        adcValue = 0;
        impedance = 0;
        adcSum = 0;
        adcPosRead = 0;

        // Set potentiometer value for next read //
        txBuffer3[0] = 0; // 8 bit device so we don't need the high byte

        // AUTOCAL CODE. SWITCH muxmod with AUTOMATE
//        txBuffer3[1] = sensorValues[muxmod]; // write to the potentiometer
        txBuffer3[1] = taps[AUTOMATE];

        // To prevent values carrying over from cycle to cycle, we reset adcValue and impedance
        I2C_transfer(I2Chandle, &i2cTrans3);

        /// Updates milliseconds variable (time stamp) //
        milliseconds = milliseconds + PERIOD_OF_TIME; // End of a cycle. Update current time stamp.

        counterDAC = 0; // Reset DACtimerCallback to case 0
        }
}

/* Every time we start recording data we need our time stamp and sensor channel to reset to 0 */
void Sensors_start_timers() {
    milliseconds = 0;
    muxmod = 0;
    GPTimerCC26XX_start(hDACTimer);
}

/* Every time we stop recording data we clear our serializer because our sensors channel will reset next time we start writing again */
void Sensors_stop_timers() {
    serializer_clear();
    GPTimerCC26XX_stop(hDACTimer);
}
/*
 * DA_get_status returns an explanation of what is happening with the SD Card.
 * Success means everything is working. Failed to initialize SD card implies there is no SD card present.
 */
void DA_get_status(int status_code, char* message) {
    switch (status_code) {
        case DISK_NULL_HANDLE:
            System_sprintf(uartBuf, "%s: SD handle null\n\0", message);
            break;
        case DISK_FAILED_INIT:
            System_sprintf(uartBuf, "%s: Failed to initialize SD card\n\0", message);
            break;
        case DISK_FAILED_READ:
            System_sprintf(uartBuf, "%s: Sector reading error\n\0", message);
            break;
        case DISK_FAILED_WRITE:
            System_sprintf(uartBuf, "%s: Sector writing error\n\0", message);
            break;
        case DISK_SUCCESS:
            System_sprintf(uartBuf, "%s: Success\n\0", message);
            break;
        case DISK_LOCKED:
            System_sprintf(uartBuf, "%s: Disk locked\n\0", message);
            break;
        default:
            System_sprintf(uartBuf, "%s: Unknown status: %d\n\0", message, status_code);
   }
   UART_write(uart, uartBuf, strlen(uartBuf));
}

// Write to the UART/terminal with print()
void print(char* str) {
    UART_write(uart, str, strlen(str));
}
