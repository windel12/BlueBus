/*
 * File: handler.c
 * Author: Ted Salmon <tass2001@gmail.com>
 * Description:
 *     Implement the logic to have the BC127 and IBus communicate
 */
#define _ADDED_C_LIB 1
#include <stdio.h>
#include "handler.h"
#include "lib/bc127.h"
#include "lib/debug.h"
#include "lib/event.h"
#include "lib/ibus.h"
#include "lib/timer.h"
struct ApplicationContext_t AppCtx;

void HandlerRegister(struct BC127_t *bt, struct IBus_t *ibus)
{
    AppCtx.bt = bt;
    AppCtx.ibus = ibus;
    memset(AppCtx.displayText, 0, HANDLER_DISPLAY_TEXT_SIZE);
    AppCtx.displayTextIdx = 0;
    AppCtx.cdChangeLastKeepAlive = TimerGetMillis();
    AppCtx.announceSelf = 0;
    EventRegisterCallback(
        BC127Event_Startup,
        &HandlerBC127Startup,
        &AppCtx
    );
    EventRegisterCallback(
        BC127Event_MetadataChange,
        &HandlerBC127Metadata,
        &AppCtx
    );
    EventRegisterCallback(
        BC127Event_PlaybackStatusChange,
        &HandlerBC127PlaybackStatus,
        &AppCtx
    );
    EventRegisterCallback(
        IBusEvent_Startup,
        &HandlerIBusStartup,
        &AppCtx
    );
    EventRegisterCallback(
        IBusEvent_CDKeepAlive,
        &HandlerIBusCDChangerKeepAlive,
        &AppCtx
    );
    EventRegisterCallback(
        IBusEvent_CDStatusRequest,
        &HandlerIBusCDChangerStatus,
        &AppCtx
    );
    TimerRegisterScheduledTask(
        &HandlerTimerCD53StatusScroll,
        &AppCtx,
        HANDLER_DISPLAY_CHANGE_INT
    );
    TimerRegisterScheduledTask(
        &HandlerTimerCDChangerAnnounce,
        &AppCtx,
        HANDLER_CDC_ANOUNCE_INT
    );
}

void HandlerBC127Startup(void *ctx, unsigned char *tmp)
{
    struct ApplicationContext_t *context = (ApplicationContext_t *) ctx;
    BC127CommandStatus(context->bt);
}

void HandlerBC127Metadata(void *ctx, unsigned char *metadata)
{
    struct ApplicationContext_t *context = (ApplicationContext_t *) ctx;
    snprintf(
        context->displayText,
        HANDLER_DISPLAY_TEXT_SIZE,
        "%s - %s on %s",
        context->bt->title,
        context->bt->artist,
        context->bt->album
    );
    context->displayTextIdx = 0;
}

void HandlerBC127PlaybackStatus(void *ctx, unsigned char *status)
{
    struct ApplicationContext_t *context = (ApplicationContext_t *) ctx;
    if (context->bt->avrcpStatus == BC127_AVRCP_STATUS_PAUSED) {
        IBusCommandDisplayText(context->ibus, "Paused");
    }
}

void HandlerIBusStartup(void *ctx, unsigned char *tmp)
{
    struct ApplicationContext_t *context = (ApplicationContext_t *) ctx;
    IBusCommandSendCdChangeAnnounce(context->ibus);
}

void HandlerIBusCDChangerKeepAlive(void *ctx, unsigned char *pkt)
{
    struct ApplicationContext_t *context = (ApplicationContext_t *) ctx;
    IBusCommandSendCdChangerKeepAlive(context->ibus);
    context->cdChangeLastKeepAlive = TimerGetMillis();
}

void HandlerIBusCDChangerStatus(void *ctx, unsigned char *pkt)
{
    struct ApplicationContext_t *context = (ApplicationContext_t *) ctx;
    unsigned char curAction = 0x00;
    unsigned char curStatus = 0x02;
    unsigned char changerStatus = pkt[4];
    if (changerStatus == 0x00) {
        // Asking for status
        curStatus = context->ibus->cdChangerStatus;
    } else if (changerStatus == 0x01) {
        // Stop Playing
        context->announceSelf = 0;
        IBusCommandDisplayTextClear(context->ibus);
    } else if (changerStatus == 0x03) {
        // Start Playing
        curAction = 0x02;
        curStatus = 0x09;
        if (context->announceSelf == 0) {
            IBusCommandDisplayText(context->ibus, "BlueBus");
            context->announceSelf = 1;
        }
    }
    if (changerStatus == 0x0A) {
        if (pkt[5] == 0x00) {
            BC127CommandForward(context->bt);
        } else {
            BC127CommandBackward(context->bt);
        }
    }
    // Radio Number pressed
    if (changerStatus == 0x06) {
        if (pkt[5] == 0x01) {
            if (context->bt->avrcpStatus == BC127_AVRCP_STATUS_PLAYING) {
                BC127CommandPause(context->bt);
            } else {
                BC127CommandPlay(context->bt);
            }
        }
    }
    IBusCommandSendCdChangerStatus(context->ibus, &curStatus, &curAction);
    context->cdChangeLastKeepAlive = TimerGetMillis();
}

void HandlerTimerCD53StatusScroll(void *ctx)
{
    struct ApplicationContext_t *context = (ApplicationContext_t *) ctx;
    uint8_t displayTextLength = strlen(context->displayText);
    if (displayTextLength > 0 &&
        context->ibus->cdChangerStatus > 0x01 &&
        context->bt->avrcpStatus == BC127_AVRCP_STATUS_PLAYING
    ) {
        uint32_t now = TimerGetMillis();
        if ((now - context->displayTextLastStamp) > HANDLER_DISPLAY_CHANGE_INT) {
            if (displayTextLength <= 11) {
                IBusCommandDisplayText(context->ibus, context->displayText);
            } else {
                char text[12];
                text[11] = '\0';
                uint8_t idx;
                uint8_t endOfText = 0;
                for (idx = 0; idx < 11; idx++) {
                    uint8_t offsetIdx = idx + context->displayTextIdx;
                    if (offsetIdx < displayTextLength) {
                        text[idx] = context->displayText[offsetIdx];
                    } else {
                        text[idx] = '\0';
                        endOfText = 1;
                    }
                }
                if (strlen(text) > 0) {
                    IBusCommandDisplayText(context->ibus, text);
                }
                if (endOfText == 1) {
                    context->displayTextIdx = 0;
                } else {
                    context->displayTextIdx = context->displayTextIdx + 11;
                }
            }
            context->displayTextLastStamp = now;
        }
    }
}

void HandlerTimerCDChangerAnnounce(void *ctx)
{
    struct ApplicationContext_t *context = (ApplicationContext_t *) ctx;
    uint32_t now = TimerGetMillis();
    if ((now - context->cdChangeLastKeepAlive) > HANDLER_CDC_ANOUNCE_INT) {
        IBusCommandSendCdChangeAnnounce(context->ibus);
        context->cdChangeLastKeepAlive = now;
    }

}
