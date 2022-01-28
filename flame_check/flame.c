/*
 *  ======== empty.c ========
 */
/* XDCtools Header files */
#include <xdc/std.h>
#include <xdc/runtime/System.h>

/* BIOS Header files */
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Swi.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Idle.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Mailbox.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Queue.h>
#include <xdc/runtime/System.h>
#include <ti/sysbios/BIOS.h>
#include <ti/drivers/GPIO.h>
#include <ti/drivers/I2C.h>

// new headers
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "inc/hw_types.h"
#include "driverlib/adc.h"
#include "driverlib/gpio.h"
#include "driverlib/sysctl.h"

/* TI-RTOS Header files */

/* Board Header file */
#include "Board.h"

// external variables that were created using XGConf
// If you do not write these, compiler will give "undefined symbol" error.
//
extern Swi_Handle swi0;
extern Mailbox_Handle mailbox0;

#define TASKSTACKSIZE       640
Task_Struct task0Struct;
Char task0Stack[TASKSTACKSIZE];

// ADC values will be put here
//
uint32_t ADCValues[2];

static uint16_t temp1 = 0;
static uint32_t temp2 = 0;

Void timerHWI(UArg arg1)
{
    //
    // Just trigger the ADC conversion for sequence 3. The rest will be done in SWI
    //
    ADCProcessorTrigger(ADC0_BASE, 3);

    // post the SWI for the rest of ADC data conversion and buffering
    //
    Swi_post(swi0);
}

Void ADCSwi(UArg arg1, UArg arg2)
{
    static uint32_t PE3_value;

    //
    // Wait for conversion to be completed for sequence 3
    //
    while(!ADCIntStatus(ADC0_BASE, 3, false));

    //
    // Clear the ADC interrupt flag for sequence 3
    //
    ADCIntClear(ADC0_BASE, 3);

    //
    // Read ADC Value from sequence 3
    //
    ADCSequenceDataGet(ADC0_BASE, 3, ADCValues);

    //
    // Port E Pin 3 is the AIN0 pin. Therefore connect PE3 pin to the line that you want to
    // acquire. +3.3V --> 4095 and 0V --> 0
    //
    PE3_value = ADCValues[0]; // PE3 : Port E pin 3

    // send the ADC PE3 values to the taskAverage()
    //
    Mailbox_post(mailbox0, &PE3_value, BIOS_NO_WAIT);
}

Void taskAverage(UArg arg1, UArg arg2)
{
    static uint32_t pe3_val, pe3_average, tot=0;
    int i;
    int iter=0;

    while(1) {

            tot = 0;                // clear total ADC values
            for(i=0;i<5;i++) {     // 10 ADC values will be retrieved

                // wait for the mailbox until the buffer is full
                //
                Mailbox_pend(mailbox0, &pe3_val, BIOS_WAIT_FOREVER);


                tot += pe3_val;
            }
            pe3_average = tot /10;
            temp2 = pe3_average;
            Task_sleep(1000);
//            if(pe3_average < 2047){
//                System_printf("YANGIN VAR!!!!!   Sensör degeri: %d\n", pe3_average);
//            }
            System_flush();

        // After 10 iterations, we will quit and Console shows the results
        //

    }
}

void initialize_ADC()
{
    // enable ADC and Port E
    //
    SysCtlPeripheralReset(SYSCTL_PERIPH_ADC0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);
    SysCtlPeripheralReset(SYSCTL_PERIPH_GPIOE);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);
    SysCtlDelay(10);

    // Select the analog ADC function for Port E pin 3 (PE3)
    //
    GPIOPinTypeADC(GPIO_PORTE_BASE, GPIO_PIN_3);

    // configure sequence 3
    //
    ADCSequenceConfigure(ADC0_BASE, 3, ADC_TRIGGER_PROCESSOR, 0);

    // every step, only PE3 will be acquired
    //
    ADCSequenceStepConfigure(ADC0_BASE, 3, 0, ADC_CTL_CH0 | ADC_CTL_IE | ADC_CTL_END);

    // Since sample sequence 3 is now configured, it must be enabled.
    //
    ADCSequenceEnable(ADC0_BASE, 3);

    // Clear the interrupt status flag.  This is done to make sure the
    // interrupt flag is cleared before we sample.
    //
    ADCIntClear(ADC0_BASE, 3);
}

void printError(char *errString, int code)
{
    System_printf("Error! code = %d, desc = %s\n", code, errString);
    BIOS_exit(code);
}

Void taskFxn(UArg arg0, UArg arg1)
{
   while(1){
        uint16_t        temperature;
        uint8_t         txBuffer[6];
        uint8_t         rxBuffer[6];
        I2C_Handle      i2c;
        I2C_Params      i2cParams;
        I2C_Transaction i2cTransaction;

        // Create I2C interface for sensor usage
        //
        I2C_Params_init(&i2cParams);
        i2cParams.bitRate = I2C_400kHz;  // It can be I2C_400kHz orI2C_100kHz

        // Let's open the I2C interface
        //
        i2c = I2C_open(Board_I2C_TMP, &i2cParams);  // Board_I2C_TMP is actually I2C7
        if (i2c == NULL) {
            // error initializing IIC
            //
            System_abort("Error Initializing I2C\n");
        }

      //  System_printf("I2C Initialized!\n");

        // Point to the T ambient register and read its 2 bytes (actually 14 bits)
        // register number is 0x01.
        //
        txBuffer[0] = 0x01;                                 // Ambient temperature register
        i2cTransaction.slaveAddress = Board_TMP006_ADDR;    // For SENSHUB it is 0x41
        i2cTransaction.writeBuf = txBuffer;                 // transmit buffer
        i2cTransaction.writeCount = 1;                      // only one byte will be sent
        i2cTransaction.readBuf = rxBuffer;                  // receive buffer
        i2cTransaction.readCount = 2;                       // we are expecting 2 bytes

        // carry out the I2C transfer. The received 16 bits is in big endian format since IIC
        // protocol sends the most significant byte first (i.e. rxBuffer[0]) and then
        // least significant byte (i.e. rxBuffer[1]).
        //
        // Remember that temperature register is 14 bits and we need to shift right 2 bits
        // to get a number. We need to divide it by 32 to get the temperature value.
        //
        if (I2C_transfer(i2c, &i2cTransaction)) {

           // 14 bit to 16 bit conversion since least 2 bits are 0s
            //
           temperature = (rxBuffer[0] << 6) | (rxBuffer[1] >> 2);

           // This time we are going to check whether the number is negative or not.
           // If it is negative, we will sign extend to 16 bits.
           //
           if (rxBuffer[0] & 0x80) {
               temperature |= 0xF000;
           }

           // We need to divide by 32 to get the actual temperature value.
           // Check with the TMP006 datasheet
           //
           temperature /= 32;
           System_printf("Sample %d (C)\n", temperature);
           temp1 = temperature;

           if(temp1>24 && temp2<2000 && temp2!=NULL){
               System_printf("Yangýn varrrrrr!\n");
           }

        }
        else {

            // no response from TMP006. Is it there?
            //
           System_printf("I2C Bus fault\n");
        }

        // flush everything to the console
        //

        System_flush();

        // close the interface
        //
        Task_sleep(1000);
        I2C_close(i2c);
   }

}

/*
 *  ======== main ========
 */
int main(void)
{
    Task_Params taskParams;

    // Call board init functions
    Board_initGeneral();
    Board_initGPIO();
    Board_initI2C();

    // Construct tmp006 Task thread
    //
    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.stack = &task0Stack;
    Task_construct(&task0Struct, (Task_FuncPtr)taskFxn, &taskParams, NULL);

    initialize_ADC();



    // Turn on user LED
    GPIO_write(Board_LED0, Board_LED_ON);

    // Start BIOS
    //
    BIOS_start();

    return (0);
}
