/*
 * A fatal error: show the message in a system dialog and quit.
 * Based on the so-loader boilerplate (Andy Nguyen, fgsfds, Volodymyr Atamanenko; MIT).
 */
#include "util/fatal.h"

#include <stdarg.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/message_dialog.h>
#include <vitaGL.h>

static void show_message(const char *msg) {
    SceMsgDialogUserMessageParam msg_param;
    sceClibMemset(&msg_param, 0, sizeof(msg_param));
    msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    msg_param.msg = (SceChar8 *)msg;

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    _sceCommonDialogSetMagicNumber(&param.commonParam);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg_param;
    sceMsgDialogInit(&param);
}

void fatal_error(const char *fmt, ...) {
    char text[512];
    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(text, sizeof(text), fmt, list);
    va_end(list);

    vglInit(0);   /* the dialog needs a display; harmless if vitaGL is already up */
    show_message(text);
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        vglSwapBuffers(GL_TRUE);
    sceMsgDialogTerm();
    sceKernelExitProcess(0);
    while (1) {}
}
