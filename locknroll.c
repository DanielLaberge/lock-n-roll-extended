/*
locknroll - a pidgin plugin to set accounts to "Away" when Windows is locked

Copyright (c) 2009 Chris Sammis, http://csammisrun.net

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software
    in a product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

    3. This notice may not be removed or altered from any source
    distribution.
*/

#define PURPLE_PLUGINS

#include <windows.h>
#include <wtsapi32.h>
#include <glib.h>
#include <gdk/gdkwin32.h>

#ifndef G_GNUC_NULL_TERMINATED
#  if __GNUC__ >= 4
#    define G_GNUC_NULL_TERMINATED __attribute__((__sentinel__))
#  else
#    define G_GNUC_NULL_TERMINATED
#  endif /* __GNUC__ >= 4 */
#endif /* G_GNUC_NULL_TERMINATED */

#include "account.h"
#include "status.h"
#include "savedstatuses.h"
#include "gtkplugin.h"
#include "gtkprefs.h"
#include "notify.h"
#include "version.h"

#define LNR_PREF_MESSAGE "/plugins/gtk/win32/locknroll/message"
#define LNR_PREF_SAVED_MESSAGE "/plugins/gtk/win32/locknroll/saved_message"
#define LNR_PREF_STATUS "/plugins/gtk/win32/locknroll/status_when_locked"

// A handle to the window receiving TS session change notifications
static HWND lnr_hwnd;
// A handle to this plugin supplied by plugin_load
static PurplePlugin *lnr_handle;
// A list of PurpleAccount*s whose statuses are being modified by this plugin
static GList *lnr_accts;
// The status which will be reactivated for the members of lnr_accts
static PurpleSavedStatus *lnr_reactivate_status;

#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>

/////////////////////////////////////////////////////////////////////////////
// Debug functionality:  If you're trying to debug this code, you can add
//  calls to lnr_trace() and locknroll.log will be created in the Pidgin exe's
//  working directory.
// Please don't distribute a finalized plugin with this included, because no
//  one wants a Pidgin plugin creating big old logfiles.
/////////////////////////////////////////////////////////////////////////////
static void lnr_trace(const char *str, ...)
{
    char buf[500] = {0};
    FILE *log;
    time_t t;
    va_list ap;

    va_start(ap, str);
    vsnprintf(buf, 500, str, ap);
    va_end(ap);

    log = fopen("locknroll.log", "a");
    assert(log);
    time(&t);
    fprintf(log, "%s\t%s\n", ctime(&t), buf);
    fclose(log);
}

/////////////////////////////////////////////////////////////////////////////
// Determines whether Lock'n'Roll should update the status message when the
//  workstation is locked.  This returns false if the current status is away
//  already, invisible, or offline (fixes bug in 1.0.1)
/////////////////////////////////////////////////////////////////////////////
static gboolean lnr_should_change_message(PurpleAccount *acct)
{
    PurpleStatusType *activeStatusType = NULL;
    activeStatusType = purple_status_get_type(purple_account_get_active_status(acct));
    switch (purple_status_type_get_primitive(activeStatusType))
    {
    case PURPLE_STATUS_AWAY:
    case PURPLE_STATUS_EXTENDED_AWAY:
    case PURPLE_STATUS_UNAVAILABLE:
    case PURPLE_STATUS_INVISIBLE:
    case PURPLE_STATUS_OFFLINE:
        return FALSE;
    default:
        return TRUE;
    }
}

/////////////////////////////////////////////////////////////////////////////
// The window procedure.  It handles WTS_SESSION_LOCK and WTS_SESSION_UNLOCK
//  messages sent to the LNR window.
/////////////////////////////////////////////////////////////////////////////
static LRESULT CALLBACK LnrWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    GList *accts, *iter;
    PurpleAccount *acct = NULL;
    PurpleStatusType *status_type = NULL;
    PurpleSavedStatus *saved_status = NULL;

    switch(uMsg)
    {
    case WM_WTSSESSION_CHANGE:
        switch(wParam)
        {
        case WTS_SESSION_LOCK:

            saved_status = purple_savedstatus_new(NULL, purple_prefs_get_int(LNR_PREF_STATUS));
            purple_savedstatus_set_message(saved_status, purple_prefs_get_string(LNR_PREF_MESSAGE));

            iter = accts = purple_accounts_get_all_active();
            for(; iter != NULL; iter = iter->next)
            {
                acct = (PurpleAccount*)iter->data;
                if(lnr_should_change_message(acct))
                {
                    status_type = purple_account_get_status_type_with_primitive(acct, purple_prefs_get_int(LNR_PREF_STATUS));
                    if(status_type != NULL)
                    {
                        // Set the substatus for this account
                        purple_savedstatus_set_substatus(saved_status, acct, status_type, purple_prefs_get_string(LNR_PREF_MESSAGE));
                        lnr_accts = g_list_append(lnr_accts, acct);
                    }
                }
            }

            if(g_list_length(lnr_accts))
            {
                lnr_reactivate_status = purple_savedstatus_get_current();
                purple_savedstatus_activate(saved_status);
            }
            else
            {
                lnr_reactivate_status = NULL;
                g_list_free(lnr_accts);
                lnr_accts = NULL;
            }

            g_list_free(accts);
            accts = NULL;

            break;
        case WTS_SESSION_UNLOCK:

            if(lnr_accts != NULL)
            {
                if (lnr_reactivate_status != NULL)
                {
                    purple_savedstatus_activate(lnr_reactivate_status);
                }
                else
                {
                    saved_status = purple_savedstatus_new(NULL, PURPLE_STATUS_AVAILABLE);
                    purple_savedstatus_set_message(saved_status, "");

                    for(iter = lnr_accts; iter != NULL; iter = iter->next)
                    {
                        acct = (PurpleAccount*)iter->data;
                        // Set the status up with a generic "Available" message for each account
                        status_type = purple_account_get_status_type_with_primitive(acct, PURPLE_STATUS_AVAILABLE);
                        if(status_type != NULL)
                        {
                            purple_savedstatus_set_substatus(saved_status, acct, status_type, "");
                        }
                    }

                    purple_savedstatus_activate(saved_status);
                }

                lnr_reactivate_status = NULL;
                g_list_free(lnr_accts);
                lnr_accts = NULL;
            }
            break;
        }
        break;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

/////////////////////////////////////////////////////////////////////////////
// Registers the LNR window and requests TS session change notifications.
/////////////////////////////////////////////////////////////////////////////
static gboolean plugin_load(PurplePlugin *plugin)
{
    WNDCLASSEX wcx;
    HWND hwnd;
    wcx.cbSize = sizeof(WNDCLASSEX);
    wcx.lpszClassName = "pidgin_lockonaway";
    wcx.lpfnWndProc = (WNDPROC)LnrWindowProc;
    wcx.style = wcx.cbClsExtra = wcx.cbWndExtra = 0;
    wcx.hbrBackground = NULL;
    wcx.hInstance = NULL;
    wcx.hIcon = NULL;
    wcx.hCursor = NULL;
    wcx.hIconSm = NULL;

    // Register the class with Windows
    if(!RegisterClassEx(&wcx))
    {
        purple_notify_message(plugin, PURPLE_NOTIFY_MSG_INFO, "Lock 'n' Roll",
            "RegisterClassEx failed, sorry.", NULL, NULL, NULL);
        return FALSE;
    }

    hwnd = CreateWindowEx(0, "pidgin_lockonaway", "", 0, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, NULL, NULL, NULL, NULL);
    if(hwnd == NULL)
    {
        purple_notify_message(plugin, PURPLE_NOTIFY_MSG_INFO, "Lock 'n' Roll",
            "CreateWindowEx failed, sorry.", NULL, NULL, NULL);
        return FALSE;
    }

    if(!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION))
    {
        purple_notify_message(plugin, PURPLE_NOTIFY_MSG_INFO, "Lock 'n' Roll",
            "WTSRegisterSessionNotification failed, sorry.", NULL, NULL, NULL);
        return FALSE;
    }

    lnr_reactivate_status = NULL;
    lnr_handle = plugin;
    lnr_hwnd = hwnd;

    return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// Destroys the LNR window and unregisters from TS session change
//  notifications.
/////////////////////////////////////////////////////////////////////////////
static gboolean plugin_unload(PurplePlugin *plugin)
{
    if(lnr_hwnd != NULL)
    {
        WTSUnRegisterSessionNotification(lnr_hwnd);
        DestroyWindow(lnr_hwnd);
        lnr_hwnd = NULL;
    }
    UnregisterClass("pidgin_lockonaway", NULL);
    return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// Builds the plugin configuration frame.
/////////////////////////////////////////////////////////////////////////////
static PurplePluginPrefFrame* get_config_frame(PurplePlugin *plugin)
{
    PurplePluginPrefFrame *frame;
    PurplePluginPref *pref;

    frame = purple_plugin_pref_frame_new();
    pref = purple_plugin_pref_new_with_name_and_label(LNR_PREF_MESSAGE, "Raise this message when the computer is locked:");
    purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(LNR_PREF_STATUS, "Which status should be set?");
	purple_plugin_pref_set_type(pref, PURPLE_PLUGIN_PREF_CHOICE);
	purple_plugin_pref_add_choice(pref, "Away", GINT_TO_POINTER(PURPLE_STATUS_AWAY));
	purple_plugin_pref_add_choice(pref, "Extended Away", GINT_TO_POINTER(PURPLE_STATUS_EXTENDED_AWAY));
	purple_plugin_pref_add_choice(pref, "Unavailable/DND", GINT_TO_POINTER(PURPLE_STATUS_UNAVAILABLE));
	purple_plugin_pref_add_choice(pref, "Invisible", GINT_TO_POINTER(PURPLE_STATUS_INVISIBLE));
	purple_plugin_pref_add_choice(pref, "Offline", GINT_TO_POINTER(PURPLE_STATUS_OFFLINE));
	purple_plugin_pref_frame_add(frame, pref);
	
    return frame;
}

static PurplePluginUiInfo prefs_info =
{
    get_config_frame,
    0,
    NULL,
    /* padding */
    NULL,
    NULL,
    NULL,
    NULL
};

static PurplePluginInfo info =
{
    PURPLE_PLUGIN_MAGIC,
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_STANDARD,
    PIDGIN_PLUGIN_TYPE,
    0,
    NULL,
    PURPLE_PRIORITY_DEFAULT,
    "gtk-win32-csammis-locknroll",
    "Lock 'n' Roll extended",
    "1.2",
    "Change status on workstation lock",
    "Sets specified account statuses on workstation lock",
    "Author: Chris Sammis (csammis@gmail.com) Extended by Daniel Laberge (daniel@sharpcoding.com)",
    "https://github.com/TiDaN/lock-n-roll-extended",
    plugin_load,
    plugin_unload,
    NULL,
    NULL,
    NULL,
    &prefs_info,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

/////////////////////////////////////////////////////////////////////////////
// Initializes the plugin by creating its default preference settings.
/////////////////////////////////////////////////////////////////////////////
static void init_plugin(PurplePlugin *plugin)
{
    purple_prefs_add_none("/plugins/gtk/win32");
    purple_prefs_add_none("/plugins/gtk/win32/locknroll");
    purple_prefs_add_string(LNR_PREF_MESSAGE, "I'm away right now");
    purple_prefs_add_string(LNR_PREF_SAVED_MESSAGE, "");
	purple_prefs_add_int(LNR_PREF_STATUS, PURPLE_STATUS_AWAY);
}

PURPLE_INIT_PLUGIN(locknroll, init_plugin, info)
