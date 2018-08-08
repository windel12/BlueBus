/*
 * File: ibus.c
 * Author: Ted Salmon <tass2001@gmail.com>
 * Description:
 *     This implements the I-Bus
 */
#include "ibus.h"

/**
 * IBusInit()
 *     Description:
 *         Returns a fresh IBus_t object to the caller
 *     Params:
 *         None
 *     Returns:
 *         IBus_t*
 */
IBus_t IBusInit()
{
    IBus_t ibus;
    ibus.uart = UARTInit(
        IBUS_UART_MODULE,
        IBUS_UART_RX_PIN,
        IBUS_UART_TX_PIN,
        IBUS_UART_RX_PRIORITY,
        IBUS_UART_TX_PRIORITY,
        UART_BAUD_9600,
        UART_PARITY_EVEN
    );
    ibus.cdChangerStatus = 0x01;
    ibus.ignitionStatus = 0x01;
    ibus.rxBufferIdx = 0;
    ibus.rxLastStamp = 0;
    ibus.txBufferReadIdx = 0;
    ibus.txBufferWriteIdx = 0;
    ibus.txLastStamp = TimerGetMillis();
    ibus.gtHardwareVersion = IBUS_GT_MKI;
    ibus.gtCanDisplayStatic = 0;
    return ibus;
}

static void IBusHandleBMBTMessage(unsigned char *pkt)
{
    if (pkt[3] == IBusAction_BMBT_BUTTON) {
        EventTriggerCallback(IBusEvent_BMBTButton, pkt);
    }
}

static void IBusHandleGTMessage(IBus_t *ibus, unsigned char *pkt)
{
    if (pkt[2] == IBusDevice_DIA &&
        pkt[3] == IBusAction_DIAG_DATA
    ) {
        // Decode the software and hardware versions
        char hwVersion[3] = {
            pkt[IBUS_GT_HW_ID_OFFSET],
            pkt[IBUS_GT_HW_ID_OFFSET + 1],
            '\0'
        };
        uint8_t hardwareVersion = strToInt(hwVersion);
        switch (hardwareVersion) {
            case 10:
                ibus->gtHardwareVersion = IBUS_GT_MKIV;
                break;
            case 11:
                ibus->gtHardwareVersion = IBUS_GT_MKIII;
            case 21:
                ibus->gtHardwareVersion = IBUS_GT_MKII;
            // No idea what an MKI reports -- Anything else must be it?
            default:
                ibus->gtHardwareVersion = IBUS_GT_MKI;
        }
        char swVersion[3] = {
            (char) pkt[IBUS_GT_SW_ID_OFFSET],
            (char) pkt[IBUS_GT_SW_ID_OFFSET + 1],
            '\0'
        };
        uint8_t softwareVersion = strToInt(swVersion);
        if (softwareVersion == 0 || softwareVersion >= 40) {
            ibus->gtCanDisplayStatic = 1;
        }
        LogDebug("IBus: Got GT HW %d SW %d", hardwareVersion, softwareVersion);
        EventTriggerCallback(IBusEvent_GTDiagResponse, pkt);
    } else if (pkt[3] == IBusAction_GT_MENU_SELECT) {
        EventTriggerCallback(IBusEvent_GTMenuSelect, pkt);
    }
}

static void IBusHandleIKEMessage(unsigned char *pkt)
{
    if (pkt[3] == IBusAction_IGN_STATUS_REQ) {
        EventTriggerCallback(IBusEvent_IgnitionStatus, pkt);
    }
}

static void IBusHandleRadioMessage(IBus_t *ibus, unsigned char *pkt)
{
    if (pkt[2] == IBusDevice_CDC) {
        if (pkt[3] == IBusAction_CD_KEEPALIVE) {
            EventTriggerCallback(IBusEvent_CDKeepAlive, pkt);
        } else if(pkt[3] == IBusAction_CD_STATUS_REQ) {
            if (pkt[4] == 0x01 || pkt[4] == 0x03) {
                ibus->cdChangerStatus = pkt[4];
            }
            EventTriggerCallback(IBusEvent_CDStatusRequest, pkt);
        }
    } else if (pkt[2] == IBusDevice_LOC) {
        if (pkt[3] == 0x3B) {
            EventTriggerCallback(IBusEvent_CDClearDisplay, pkt);
        }
    }
}

static uint8_t IBusValidateChecksum(unsigned char *msg)
{
    uint8_t chk = 0;
    uint8_t msgSize = msg[1] + 2;
    uint8_t idx;
    for (idx = 0; idx < msgSize; idx++) {
        chk =  chk ^ msg[idx];
    }
    if (chk == 0) {
        return 1;
    } else {
        return 0;
    }
}

/**
 * IBusProcess()
 *     Description:
 *         Process messages in the IBus RX queue
 *     Params:
 *         IBus_t *ibus
 *     Returns:
 *         void
 */
void IBusProcess(IBus_t *ibus)
{
    // Read messages from the IBus and if none are available, attempt to
    // transmit whatever is sitting in the transmit buffer
    if (ibus->uart.rxQueue.size > 0) {
        ibus->rxBuffer[ibus->rxBufferIdx] = CharQueueNext(&ibus->uart.rxQueue);
        ibus->rxBufferIdx++;
        if (ibus->rxBufferIdx > 1) {
            uint8_t msgLength = (uint8_t) ibus->rxBuffer[1] + 2;
            // Make sure we do not read more than the maximum packet length
            if (msgLength > IBUS_MAX_MSG_LENGTH) {
                LogError("IBus: Invalid Message Length of %d found", msgLength);
                ibus->rxBufferIdx = 0;
                memset(ibus->rxBuffer, 0, IBUS_RX_BUFFER_SIZE);
                CharQueueReset(&ibus->uart.rxQueue);
                UARTSetModuleState(&ibus->uart, UART_STATE_IDLE);
            } else if (msgLength == ibus->rxBufferIdx) {
                uint8_t idx;
                unsigned char pkt[msgLength];
                for(idx = 0; idx < msgLength; idx++) {
                    pkt[idx] = ibus->rxBuffer[idx];
                    ibus->rxBuffer[idx] = 0x00;
                }
                UARTSetModuleState(&ibus->uart, UART_STATE_IDLE);
                if (IBusValidateChecksum(pkt) == 1) {
                    LogDebug(
                        "IBus: %02X -> %02X Action: %02X Length: %d",
                        pkt[0],
                        pkt[2],
                        pkt[3],
                        (uint8_t) pkt[1]
                    );
                    unsigned char srcSystem = pkt[0];
                    if (srcSystem == IBusDevice_BMBT) {
                        IBusHandleBMBTMessage(pkt);
                    }
                    if (srcSystem == IBusDevice_IKE) {
                        IBusHandleIKEMessage(pkt);
                    }
                    if (srcSystem == IBusDevice_GT) {
                        IBusHandleGTMessage(ibus, pkt);
                    }
                    if (srcSystem == IBusDevice_RAD) {
                        IBusHandleRadioMessage(ibus, pkt);
                    }
                } else {
                    LogError(
                        "IBus: %02X -> %02X Length: %d has invalid checksum",
                        pkt[0],
                        pkt[2],
                        msgLength,
                        (uint8_t) pkt[1]
                    );
                }
                ibus->rxBufferIdx = 0;
            }
        }
        ibus->rxLastStamp = TimerGetMillis();
    /*
     * The following requirements must be met to transmit:
     *     * There must be at least one packet waiting for transmission
     *     * At least IBUS_TX_BUFFER_WAIT milliseconds must have passed since
     *     our last transmission. This ensures that we do not spam the bus.
     *     * The UART status must be idle, indicating that we are not receiving
     *     data. This loop defines the UART Idle status (post-packet read).
     */
    } else if (ibus->txBufferWriteIdx != ibus->txBufferReadIdx &&
        (TimerGetMillis() - ibus->txLastStamp) >= IBUS_TX_BUFFER_WAIT &&
        UARTGetModuleState(&ibus->uart) == UART_STATE_IDLE
    ) {
        uint8_t msgLen = (uint8_t) ibus->txBuffer[ibus->txBufferReadIdx][1] + 2;
        uint8_t idx;
        /* At a glance this looks redundant, but it's not. It's plausible for
         * the module to have gotten data in the time it took us to run the
         * the code in the `else if` and here, so we check prior to transmission
         */
        if (UARTGetModuleState(&ibus->uart) == UART_STATE_IDLE &&
            IBUS_UART_RX_STATUS == 1
        ) {
            for (idx = 0; idx < msgLen; idx++) {
                ibus->uart.registers->uxtxreg = ibus->txBuffer[ibus->txBufferReadIdx][idx];
                // Wait for the data to leave the TX buffer
                while ((ibus->uart.registers->uxsta & (1 << 9)) != 0);
            }
            // Clear the slot and advance the index
            memset(ibus->txBuffer[ibus->txBufferReadIdx], 0, msgLen);
            if (ibus->txBufferReadIdx + 1 == IBUS_TX_BUFFER_SIZE) {
                ibus->txBufferReadIdx = 0;
            } else {
                ibus->txBufferReadIdx++;
            }
            LogDebug("IBus: TX Complete");
        }
        ibus->txLastStamp = TimerGetMillis();
    }

    // Clear the RX Buffer if it's over the timeout or about to overflow
    if (ibus->rxBufferIdx > 0) {
        uint32_t now = TimerGetMillis();
        if ((now - ibus->rxLastStamp) > IBUS_RX_BUFFER_TIMEOUT ||
            (ibus->rxBufferIdx + 1) == IBUS_RX_BUFFER_SIZE
        ) {
            LogError(
                "IBus: %d bytes in the RX buffer timed out",
                ibus->rxBufferIdx + 1
            );
            ibus->rxBufferIdx = 0;
            memset(ibus->rxBuffer, 0, IBUS_RX_BUFFER_SIZE);
            UARTSetModuleState(&ibus->uart, UART_STATE_IDLE);
        }
    }
}

/**
 * IBusSendCommand()
 *     Description:
 *         Take a Destination, source and message and add it to the transmit
 *         char queue so we can send it later.
 *     Params:
 *         IBus_t *ibus,
 *         const unsigned char src,
 *         const unsigned char dst,
 *         const unsigned char *data
 *     Returns:
 *         void
 */
void IBusSendCommand(
    IBus_t *ibus,
    const unsigned char src,
    const unsigned char dst,
    const unsigned char *data,
    const size_t dataSize
) {
    uint8_t idx, msgSize;
    msgSize = dataSize + 4;
    unsigned char msg[msgSize];
    msg[0] = src;
    msg[1] = dataSize + 2;
    msg[2] = dst;
    idx = 3;
    uint8_t i;
    // Add the Data to the packet
    for (i = 0; i < dataSize; i++) {
        msg[idx++] = data[i];
    }
    // Calculate the CRC
    uint8_t crc = 0;
    uint8_t maxIdx = msgSize - 1;
    for (idx = 0; idx < maxIdx; idx++) {
        crc ^= msg[idx];
    }
    msg[msgSize - 1] = (unsigned char) crc;
    for (idx = 0; idx < msgSize; idx++) {
        ibus->txBuffer[ibus->txBufferWriteIdx][idx] = msg[idx];
    }
    /* Store the data into a buffer, so we can spread out their transmission */
    if (ibus->txBufferWriteIdx + 1 == IBUS_TX_BUFFER_SIZE) {
        ibus->txBufferWriteIdx = 0;
    } else {
        ibus->txBufferWriteIdx++;
    }
}

/**
 * IBusStartup()
 *     Description:
 *        Trigger any callbacks listening for our Startup event
 *     Params:
 *         None
 *     Returns:
 *         void
 */
void IBusStartup()
{
    EventTriggerCallback(IBusEvent_Startup, 0);
}

void IBusCommandDisplayMIDText(IBus_t *ibus, char *message)
{
    unsigned char displayText[strlen(message) + 3];
    displayText[0] = 0x23;
    displayText[1] = 0x42;
    displayText[2] = 0x32;
    uint8_t idx;
    for (idx = 0; idx < strlen(message); idx++) {
        displayText[idx + 3] = message[idx];
    }
    IBusSendCommand(
        ibus,
        IBusDevice_TEL,
        IBusDevice_IKE,
        displayText,
        sizeof(displayText)
    );
}

void IBusCommandDisplayMIDTextClear(IBus_t *ibus)
{
    IBusCommandDisplayMIDText(ibus, 0);
}

void IBusCommandGTGetDiagnostics(IBus_t *ibus)
{
    unsigned char msg[1] = {0x00};
    IBusSendCommand(ibus, IBusDevice_DIA, IBusDevice_GT, msg, 1);
}

void IBusCommandGTUpdate(IBus_t *ibus, unsigned char updateType)
{
    unsigned char msg[4] = {
        IBusAction_GT_WRITE_MK2,
        updateType,
        0x01,
        0x00
    };
    IBusSendCommand(ibus, IBusDevice_RAD, IBusDevice_GT, msg, 4);
}

void IBusCommandGTWriteIndexMk2(IBus_t *ibus, uint8_t index, char *message) {
    IBusCommandGTWriteIndex(
        ibus,
        index,
        message,
        IBusAction_GT_WRITE_MK2,
        IBusAction_GT_WRITE_ZONE
    );
}

void IBusCommandGTWriteIndexMk4(IBus_t *ibus, uint8_t index, char *message) {
    IBusCommandGTWriteIndex(
        ibus,
        index,
        message,
        IBusAction_GT_WRITE_MK4,
        IBusAction_GT_WRITE_MENU
    );
}

void IBusCommandGTWriteIndex(
    IBus_t *ibus,
    uint8_t index,
    char *message,
    unsigned char command,
    unsigned char mode
) {
    uint8_t length = strlen(message);
    if (length > 20) {
        length = 20;
    }
    const size_t pktLenght = length + 4;
    unsigned char text[pktLenght];
    text[0] = command;
    text[1] = mode;
    text[2] = 0x00;
    text[3] = 0x40 + (unsigned char) index;
    uint8_t idx;
    for (idx = 0; idx < length; idx++) {
        text[idx + 4] = message[idx];
    }
    IBusSendCommand(ibus, IBusDevice_RAD, IBusDevice_GT, text, pktLenght);
}

void IBusCommandGTWriteIndexStatic(IBus_t *ibus, uint8_t index, char *message)
{
    uint8_t length = strlen(message);
    if (length > 38) {
        length = 38;
    }
    const size_t pktLenght = length + 4;
    unsigned char text[pktLenght];
    text[0] = IBusAction_GT_WRITE_MK4;
    text[1] = IBusAction_GT_WRITE_STATIC;
    text[2] = 0x00;
    text[3] = 0x40 + (unsigned char) index;
    uint8_t idx;
    for (idx = 0; idx < length; idx++) {
        text[idx + 4] = message[idx];
    }
    IBusSendCommand(ibus, IBusDevice_RAD, IBusDevice_GT, text, pktLenght);
}

void IBusCommandGTWriteTitle(IBus_t *ibus, char *message)
{
    uint8_t length = strlen(message);
    if (length > 11) {
        length = 11;
    }
    const size_t pktLenght = length + 3;
    unsigned char text[pktLenght];
    text[0] = IBusAction_GT_WRITE_TITLE;
    text[1] = IBusAction_GT_WRITE_ZONE;
    text[2] = 0x30;
    uint8_t idx;
    for (idx = 0; idx < length; idx++) {
        text[idx + 3] = message[idx];
    }
    IBusSendCommand(ibus, IBusDevice_RAD, IBusDevice_GT, text, pktLenght);
}

void IBusCommandGTWriteZone(IBus_t *ibus, uint8_t index, char *message)
{
    uint8_t length = strlen(message);
    if (length > 11) {
        length = 11;
    }
    const size_t pktLenght = length + 4;
    unsigned char text[pktLenght];
    text[0] = IBusAction_GT_WRITE_MK2;
    text[1] = IBusAction_GT_WRITE_ZONE;
    text[2] = 0x01;
    text[3] = (unsigned char) index;
    uint8_t idx;
    for (idx = 0; idx < length; idx++) {
        text[idx + 4] = message[idx];
    }
    IBusSendCommand(ibus, IBusDevice_RAD, IBusDevice_GT, text, pktLenght);
}

void IBusCommandSendCdChangerAnnounce(IBus_t *ibus)
{
    LogDebug("IBus: Announce CD Changer");
    const unsigned char cdcAlive[] = {0x02, 0x01};
    IBusSendCommand(ibus, IBusDevice_CDC, IBusDevice_LOC, cdcAlive, sizeof(cdcAlive));
}

void IBusCommandSendCdChangerKeepAlive(IBus_t *ibus)
{
    LogDebug("IBus: Send CD Changer Keep-Alive");
    const unsigned char cdcPing[] = {0x02, 0x00};
    IBusSendCommand(ibus, IBusDevice_CDC, IBusDevice_RAD, cdcPing, sizeof(cdcPing));
}

void IBusCommandSendCdChangerStatus(
    IBus_t *ibus,
    unsigned char *curStatus,
    unsigned char *curAction
) {
    LogDebug("IBus: Send CD Changer Status");
    const unsigned char cdcStatus[] = {
        IBusAction_CD_STATUS_REP,
        *curAction,
        *curStatus,
        0x00,
        0x3F,
        0x00,
        0x01,
        0x01
    };
    IBusSendCommand(
        ibus,
        IBusDevice_CDC,
        IBusDevice_RAD,
        cdcStatus,
        sizeof(cdcStatus)
    );
}