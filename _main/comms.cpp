#include "calibration.hpp"
#include "comms.hpp"
#include "parser.hpp"
#include "globals.hpp"
#include <Wire.h>

volatile Comms* _comms;

extern int myID;

void Comms::init() volatile
{
    Wire.setSDA(SDA_MASTER_PIN);
    Wire.setSCL(SCL_MASTER_PIN);
    Wire.begin();
    Wire1.setSDA(SDA_SLAVE_PIN);
    Wire1.setSCL(SCL_SLAVE_PIN);

    Wire.setTimeout(timeout_ms);
    Wire1.setTimeout(timeout_ms);
    _comms = this;
}

bool Comms::joinNetwork() volatile
{
    int ret;
    for (int my_potential_addr = my_id + addr_offset; my_potential_addr < 128; my_potential_addr++)
    {
        do
        {
            Wire.beginTransmission(my_potential_addr);
            ret = Wire.endTransmission(false);
        } while (ret == 4);
        DEBUG_PRINT("Address %d done: ret = %d\n", my_potential_addr, ret)

        // If no one was at the address being probed, it is now
        // ours.
        if (ret == 2)
        {
            my_id = my_potential_addr - addr_offset;
            myID = my_id;
            Wire1.onReceive([](int i){ _comms->onReceive(i); });
            Wire1.onRequest([](){ _comms->onRequest(); });
            Wire1.begin(my_potential_addr);
            do
            {
                Wire.beginTransmission(0x0);
                Wire.write(MSG_TYPE_WAKEUP);
                ret = Wire.endTransmission(true);
            } while(ret == 4);
            DEBUG_PRINT("Broadcasting wakeup as id %d\n", my_id)

            if(my_id == 0)
                calibrator.resetWait();
            return true;
        }
    }
    return false;
}

void Comms::calibrateNetwork() volatile{
    // Warn everybody calibration is starting
    int ret, counter = 0;
    do {
        Wire.beginTransmission(0x0);
        Wire.write(MSG_TYPE_BEGIN_CALIBRATION);
        ret = Wire.endTransmission(true); // BUG: gets stuck here if alone in network
        Serial.printf("Ret: %d\n", ret);
        counter += 1;
    } while(ret == 4 && counter <= 10);
    // Become the maestro
    calibrator.becomeMaestro();
    // Everybody should know that now only calibration messages
    // are being traded. Every luminaire in the network now
    // replies with their id.
    // Block while waiting for the ids to stop arriving,
    // hijacking the eventLoop.
    calibrator.resetWaitId();
    while(calibrator.waitingIds()) eventLoop();
    Serial.print("Calibration starting.\n");
    // After the highest id has been found, order everyone to calibrate,
    // in order.
    for(signed char i = 0; i <= calibrator.getHighestId(); i++) {
        // Order luminaire i to run its calibration cycle. Also
        // lets the other luminaires know that i is about to run
        // its calibration cycle in order to calibrate coupled gains.
        Serial.printf("Calibrating luminaire %d\n", i);
        counter = 0;
        do {
            Wire.beginTransmission(0x0);
            Wire.write(MSG_TYPE_CALIBRATE_ID);
            Wire.write(i);
            ret = Wire.endTransmission(true);
            counter += 1;
        } while(ret == 4 && counter <= 10);

        // When the message gets sent, its either our time to calibrate
        // or we should look.
        if(i == my_id)
            calibrator.selfCalibrate(my_id);
        else
            calibrator.calibrateGainId(i);

        // Wait for the node that just ran its calibration cycle to turn off
        // (if it wasn't me) and then give some slack for messaging delays.
        if(i != my_id)
            delay(STEADY_STATE_WAIT_MS);
        delay(MESSAGE_SLACK_WAIT_MS);
    }

    // Let everyone know calibration is over.
    counter = 0;
    do {
        Wire.beginTransmission(0x0);
        Wire.write(MSG_TYPE_END_CALIBRATION);
        counter += 1;
    } while(Wire.endTransmission(true) == 4 && counter <= 10);

    calibrator.endCalibration();
    Serial.printf("Calibrated %d luminaires.\n", calibrator.getHighestId() + 1);
    Serial.printf("Calibration complete.\n");
}

ProcessingResult Comms::processCommand(const char *command) volatile
{
    signed char luminaireID = parser.getLuminaireId(command);
    int ret = 0;
    char *commandRet = NULL;

    //Serial.printf("My id: %d, luminaire id: %d\n", my_id, luminaireID);
    if (luminaireID == my_id)
    {
        commandRet = parser.executeCommand(command);
        Serial.println(commandRet == NULL ? "NULL" : commandRet);
        return commandRet == NULL ? PROCESSING_LOCAL_EXECUTION_FAILURE : PROCESSING_OK;
    }

    const char *strippedCommand = command;//= parser.strip(command);
    // Serial.printf("Sending command: '%s'\n", strippedCommand);
    Wire.beginTransmission(luminaireID + addr_offset);
    Wire.write(MSG_TYPE_COMMAND);
    while (*strippedCommand)
    {
        Wire.write(*strippedCommand);
        strippedCommand++;
    }
    Wire.write('\0');
    ret = Wire.endTransmission();
    if (ret == 0){
        return PROCESSING_OK;
    }
    else if (ret == 5){
        return PROCESSING_I2C_TIMEOUT;
    }
    else{
        return PROCESSING_I2C_OTHER_ERROR;
    }
}

void Comms::onReceive(int signed bytesReceived) volatile
{
    MSG_TYPE msgType = (MSG_TYPE) Wire1.read();
    //error = true;
    //sprintf(errorMsg, "Received %d bytes. Message type: %d\n", bytesReceived, msgType);

    if (bytesReceived + 1 > receivedDataBufferSize)
    {
        error = true;
        sprintf((char *)errorMsg, "Received %d/%d bytes. Message type: %d. Buffer too small.\n", bytesReceived, receivedDataBufferSize, msgType);
        return;
    }

    receivedMsgType = msgType;
    receivedDataSize = 0;
    receivedData[0] = '\0';
    if (bytesReceived == 1)
        return;
    
    for (int buf_idx = 0; buf_idx < bytesReceived - 1; buf_idx++)
    {
        receivedData[buf_idx] = Wire1.read();
    }
    receivedData[bytesReceived] = '\0';
    receivedDataSize = bytesReceived - 1;
}

void Comms::onRequest() volatile
{
    Wire.write(1);
}

void Comms::flushError() volatile{
    if (error)
        Serial.printf("%s\n", errorMsg);
    error = false;
}

void Comms::processReceivedData() volatile
{
    noInterrupts();
    MSG_TYPE receivedMsg = receivedMsgType;
    receivedMsgType = MSG_TYPE_NONE;
    char receivedDataBuffer[receivedDataBufferSize];
    strncpy(receivedDataBuffer, (const char *)receivedData, receivedDataBufferSize);
    interrupts();

    if (receivedMsg == MSG_TYPE_NONE)
        return;

    char *commandRet = NULL;
    switch (receivedMsg)
    {
    // If a command was issued to me, I will execute it and reply with the result.
    case MSG_TYPE_COMMAND:
        commandRet = parser.executeCommand((const char *)receivedDataBuffer);
        SEND_MSG(0, timeout_ms,
            Wire.write(MSG_TYPE_REPLY);
            Wire.write(commandRet);
        )
        DEBUG_PRINT("Received MSG_TYPE_COMMAND with ret=%s\n", commandRet);
        break;

    // If a reply is coming my way, I will relay it to the Serial interface.
    // Same for stream.
    case MSG_TYPE_REPLY:
    case MSG_TYPE_STREAM:
        Serial.printf("%s\n", receivedDataBuffer);
        break;
        
    // In case someone has just woken up and I'm id=0, I'll see if we're waiting
    // to start calibration. If yes, reset the counter back to the start.
    case MSG_TYPE_WAKEUP:
        if(my_id == 0 && calibrator.waiting()) {
            calibrator.resetWait();
        }
        DEBUG_PRINT("Received MSG_TYPE_WAKEUP\n")
        break;

    case MSG_TYPE_BEGIN_CALIBRATION:
        Serial.printf("Received begin calibration signal.\n");
        // The maestro ignores its own calls to calibrate
        if(!calibrator.isMaestro()) {
            // Broadcast our id so the highest id can be determined
            do {
                Wire.beginTransmission(0x0);
                Wire.write(MSG_TYPE_FIND_HIGHEST_ID);
                Wire.write(my_id);
            } while(Wire.endTransmission(true) == 4);
        } else {
            calibrator.setHighestId(my_id);
            calibrator.resetWaitId();
        }
        DEBUG_PRINT("Received MSG_TYPE_BEGIN_CALIBRATION\n")
        break;

    case MSG_TYPE_FIND_HIGHEST_ID:
        Serial.printf("Received highest id: %d\n", receivedDataBuffer[0]);
        calibrator.setHighestId((signed char) receivedDataBuffer[0]);
        calibrator.resetWaitId();
        DEBUG_PRINT("Received MSG_TYPE_FIND_HIGHEST_ID\n")
        break;

    case MSG_TYPE_CALIBRATE_ID:
        // The maestro ignores its own calls to calibrate
        if(!calibrator.isMaestro()) {
            if(receivedDataBuffer[0] != my_id)
                calibrator.calibrateGainId(receivedDataBuffer[0]);
            else
                calibrator.selfCalibrate(my_id);
        }
        DEBUG_PRINT("Received MSG_TYPE_CALIBRATE_ID\n")
        break;

    case MSG_TYPE_END_CALIBRATION:
        // The maestro ignores its own calls to calibrate
        if(!calibrator.isMaestro()) {
            calibrator.endCalibration();
        }
        DEBUG_PRINT("Received MSG_TYPE_END_CALIBRATION\n")
        break;

    default:
        DEBUG_PRINT("Message wasn't well read. Code %d\n", receivedMsg)
        break;
    }
}

void Comms::eventLoop() volatile
{
    flushError();
    processReceivedData();
}

void parseSerial(volatile Comms &comms) {
    char *string = (char *)Serial.readStringUntil('\n').c_str();
    DEBUG_PRINT("Received: '%s'\n", string)
    int ret = comms.processCommand(string);
    (void)ret;
    //DEBUG_PRINT("Process command returned %d\n", ret)
}