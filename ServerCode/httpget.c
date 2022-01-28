#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// XDCtools Header files
#include <xdc/std.h>
#include <xdc/runtime/Error.h>
#include <xdc/runtime/System.h>

/* TI-RTOS Header files */
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Swi.h>
#include <ti/sysbios/knl/Queue.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Idle.h>
#include <ti/sysbios/knl/Mailbox.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/drivers/GPIO.h>
#include <ti/net/http/httpcli.h>
#include <ti/drivers/GPIO.h>
#include <ti/drivers/I2C.h>

#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "inc/hw_types.h"
#include "driverlib/adc.h"
#include "driverlib/gpio.h"
#include "driverlib/sysctl.h"

/* TI-RTOS Header files */
#include <ti/drivers/GPIO.h>

#include "Board.h"

#include <sys/socket.h>
#include <arpa/inet.h>

#define REQUEST_URI       "/data/2.5/forecast/?id=315202&APPID=b9bdaf75a7b1e96362a172ec83cb9303"
#define USER_AGENT        "HTTPCli (ARM; TI-RTOS)"
#define SOCKETTEST_IP     "192.168.1.46"

#define IP                "132.163.97.5"    //IP  for ntp server
char dataTime[4];                           //global variable to hold time data(4 byte from ntp)

char message[100];
char timeMessage[100];

#define TASKSTACKSIZE     4096
#define OUTGOING_PORT     5011
#define INCOMING_PORT     5030

extern Swi_Handle swi0;
extern Mailbox_Handle mailbox0;
extern Semaphore_Handle semaphore0;     // posted by httpTask and pended by clientTask
char   tempstr[20];                     //  string

uint32_t ADCValues[0];

uint16_t   temperature;
static uint32_t temp2 = 0;
static uint32_t temp1 = 0;
static bool firetemp=false;
/*
 *  ======== printError ========
 */

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
    PE3_value=(PE3_value*(5.0/1023.0));
    // send the ADC PE3 values to the taskAverage()
    //
    Mailbox_post(mailbox0, &PE3_value, BIOS_NO_WAIT);
}
Void taskFxn(UArg arg0, UArg arg1);
void taskAverage()
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
            Task_sleep(500);
//            if(pe3_average < 2047){
//                System_printf("YANGIN VAR!!!!!   Sensör degeri: %d\n", pe3_average);
//            }

            System_flush();


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

bool sendData2Server(char *serverIP, int serverPort, char *data, int size)
{
    int sockfd, connStat, numSend;
    bool retval=false;
    struct sockaddr_in serverAddr;

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == -1) {
        System_printf("Socket not created");
        close(sockfd);
        return false;
    }

    memset(&serverAddr, 0, sizeof(serverAddr));  // clear serverAddr structure
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);     // convert port # to network order
    inet_pton(AF_INET, serverIP, &(serverAddr.sin_addr));

    connStat = connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    if(connStat < 0) {
        System_printf("sendData2Server::Error while connecting to server\n");
    }
    else {
        numSend = send(sockfd, data, size, 0);       // send data to the server
        if(numSend < 0) {
            System_printf("sendData2Server::Error while sending data to server\n");
        }
        else {
            retval = true;      // we successfully sent the temperature string
        }
    }
    System_flush();
    close(sockfd);
    return retval;
}

Void clientSocketTask(UArg arg0, UArg arg1)
{
    while(1) {
        // wait for the semaphore that httpTask() will signal
        // when temperature string is retrieved from api.openweathermap.org site
        //
        Semaphore_pend(semaphore0, BIOS_WAIT_FOREVER);

        GPIO_write(Board_LED0, 1); // turn on the LED

        // connect to SocketTest program on the system with given IP/port
        // send hello message whihc has a length of 5.
        //
        if(sendData2Server(SOCKETTEST_IP, OUTGOING_PORT, tempstr, strlen(tempstr))) {
            System_printf("clientSocketTask:: Temperature is sent to the server\n");
            System_flush();
        }

        GPIO_write(Board_LED0, 0);  // turn off the LED
    }
}

void getTimeStr()  //I create function to get current time from NTP server
{

    //create necessarry socket connections & functions
    int sockfd, connStat;
    struct sockaddr_in serverAddr;
    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == -1) {
        System_printf("Socket not created");
        BIOS_exit(-1);

    }
    memset(&serverAddr, 0, sizeof(serverAddr));  // clear serverAddr structure
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(37);     // convert port # to network order
    inet_pton(AF_INET, IP , &(serverAddr.sin_addr));        //connect IP, which declared in globally
    connStat = connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    if(connStat < 0) {
        System_printf("sendData2Server::Error while connecting to server\n");
        if(sockfd>0) close(sockfd);
        BIOS_exit(-1);
    }
    System_flush();
    recv(sockfd, dataTime, 4, 0);       //receive 4 byte time value

    unsigned long int seconds= dataTime[0]*16777216 +  dataTime[1]*65536 + dataTime[2]*256 + dataTime[3];   //convert it to the total seconds
    seconds  += 10800;  //since our local time is 3 hours away, add 10800 seconds and equalize them
    char* buf = ctime(&seconds);    //convert total elapsed time to the current date value

    uint8_t i;  //loop counter


    for(i = 0; i < 27; i++)     //since my message 26 char length, read using pointer and write to the char array.
    {
        timeMessage[i] = buf[i];        //timeMessage is global variable and it will be sent to the server
    }


    System_printf("the system time message is: %s\n", timeMessage);
    System_flush();

    if(sockfd>0) close(sockfd);


}


Void taskFxn(UArg arg0, UArg arg1)
{

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

    System_printf("I2C Initialized!\n");

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

       temp1=temperature;

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
    I2C_close(i2c);
}


float getTemperature(void)
{
    // dummy return
    //
    //return atof(temp2);

    static Task_Handle taskHandle5;
        Task_Params taskParams;
        Error_Block eb;

        Error_init(&eb);

        Task_Params_init(&taskParams);
        taskParams.stackSize = 640;
        taskParams.priority = 1;
        taskHandle5 = Task_create((Task_FuncPtr) taskFxn, &taskParams, &eb);

}

Void serverSocketTask(UArg arg0, UArg arg1)
{
    int serverfd, new_socket, valread, len;
    struct sockaddr_in serverAddr, clientAddr;
    float temp;
    bool fire=false;
    char buffer[30];
    char outstr[30], tmpstr[30];
    bool quit_protocol;

    serverfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverfd == -1) {
        System_printf("serverSocketTask::Socket not created.. quiting the task.\n");
        return;     // we just quit the tasks. nothing else to do.
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(INCOMING_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    // Attaching socket to the port
    //
    if (bind(serverfd, (struct sockaddr *)&serverAddr,  sizeof(serverAddr))<0) {
         System_printf("serverSocketTask::bind failed..\n");

         // nothing else to do, since without bind nothing else works
         // we need to terminate the task
         return;
    }
    if (listen(serverfd, 3) < 0) {

        System_printf("serverSocketTask::listen() failed\n");
        // nothing else to do, since without bind nothing else works
        // we need to terminate the task
        return;
    }

    while(1) {

        len = sizeof(clientAddr);
        if ((new_socket = accept(serverfd, (struct sockaddr *)&clientAddr, &len))<0) {
            System_printf("serverSocketTask::accept() failed\n");
            continue;               // get back to the beginning of the while loop
        }

        System_printf("Accepted connection\n"); // IP address is in clientAddr.sin_addr
        System_flush();

        // task while loop
        //
        quit_protocol = false;
        do {

            // let's receive data string
            if((valread = recv(new_socket, buffer, 10, 0))<0) {

                // there is an error. Let's terminate the connection and get out of the loop
                //
                close(new_socket);
                break;
            }

            // let's truncate the received string
            //
            buffer[10]=0;
            if(valread<10) buffer[valread]=0;

            System_printf("message received: %s\n", buffer);

            if(!strcmp(buffer, "HELLO")) {
                strcpy(outstr,"GREETINGS 200\n");
                send(new_socket , outstr , strlen(outstr) , 0);
                System_printf("Server <-- GREETINGS 200\n");
            }
            else if(!strcmp(buffer, "GETTIME")) {
                getTimeStr(tmpstr);
                strcpy(outstr, " OK ");
                strcat(outstr, tmpstr);
                strcat(outstr, "\n");
                send(new_socket , outstr , strlen(outstr) , 0);
            }
            else if(!strcmp(buffer, "GETTEMP")) {
                temp = getTemperature();
                sprintf(outstr, " TEMPERATURE %5.2d\n", temperature);
                send(new_socket , outstr , strlen(outstr) , 0);
            }
            else if(!strcmp(buffer, "CHECKFIRE")) {
                if(temp1>24 && temp2<3000){
                    sprintf(outstr, " YANGINNN\n");
                }
                else
                    sprintf(outstr, " YANGIN YOK!\n");

                send(new_socket , outstr , strlen(outstr) , 0);
            }
            else if(!strcmp(buffer, "QUIT")) {
                quit_protocol = true;     // it will allow us to get out of while loop
                strcpy(outstr, "BYE 200");
                send(new_socket , outstr , strlen(outstr) , 0);
            }

        }
        while(!quit_protocol);

        System_flush();
        close(new_socket);
    }

    close(serverfd);
    return;
}

//handle 1 delete
bool createTasks(void)
{
    static Task_Handle taskHandle2, taskHandle3, taskHandle4;
    Task_Params taskParams;
    Error_Block eb;

    Error_init(&eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle2 = Task_create((Task_FuncPtr)clientSocketTask, &taskParams, &eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle3 = Task_create((Task_FuncPtr)serverSocketTask, &taskParams, &eb);


    if (taskHandle2 == NULL || taskHandle3 == NULL || taskHandle3 ==NULL) {
        printError("netIPAddrHook: Failed to create HTTP, Socket and Server Tasks\n", -1);
        return false;
    }

    return true;
}

void netIPAddrHook(unsigned int IPAddr, unsigned int IfIdx, unsigned int fAdd)
{
    // Create a HTTP task when the IP address is added
    if (fAdd) {
        createTasks();
    }
}

int main(void)
{
    /* Call board init functions */
    Board_initGeneral();
    Board_initGPIO();
    Board_initEMAC();
    Board_initGPIO();
    Board_initI2C();

    /* Turn on user LED */
    GPIO_write(Board_LED0, Board_LED_ON);

    System_printf("Starting the HTTP GET example\nSystem provider is set to "
            "SysMin. Halt the target to view any SysMin contents in ROV.\n");
    /* SysMin will only print to the console when you call flush or exit */
    System_flush();


    /* Start BIOS */
    BIOS_start();

    return (0);
}
