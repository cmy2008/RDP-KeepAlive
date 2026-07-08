/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RDP Keep-Alive Client
 *
 * A headless command-line tool that connects to an RDP server and
 * periodically sends mouse move events to prevent server-side
 * idle timeout disconnection.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freerdp/freerdp.h>
#include <freerdp/constants.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/utils/signal.h>

#include <freerdp/client/file.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/channels.h>
#include <freerdp/channels/channels.h>

#include <winpr/crt.h>
#include <winpr/assert.h>
#include <winpr/synch.h>
#include <winpr/winsock.h>
#include <winpr/wlog.h>
#include <freerdp/log.h>

#include "rdp_keepalive.h"

#define TAG CLIENT_TAG("rdp-keepalive")

/* Custom command-line arguments */
static COMMAND_LINE_ARGUMENT_A ka_args[] = {
	{
	    "interval",
	    COMMAND_LINE_VALUE_REQUIRED,
	    "[<seconds>]",
	    "60",
	    nullptr,
	    -1,
	    nullptr,
	    "Keep-alive interval in seconds (default: 60)",
	},
	{
	    "cert-ignore",
	    COMMAND_LINE_VALUE_BOOL,
	    nullptr,
	    BoolValueFalse,
	    nullptr,
	    -1,
	    nullptr,
	    "Accept all server certificates without prompting",
	},
	{
	    "no-render",
	    COMMAND_LINE_VALUE_BOOL,
	    nullptr,
	    BoolValueFalse,
	    nullptr,
	    -1,
	    nullptr,
	    "Suppress graphics output to save server performance",
	},
	{
	    "mouse",
	    COMMAND_LINE_VALUE_BOOL,
	    nullptr,
	    BoolValueFalse,
	    nullptr,
	    -1,
	    nullptr,
	    "Use mouse move events for keep-alive (off by default)",
	},
	{
	    "log-level",
	    COMMAND_LINE_VALUE_REQUIRED,
	    "[<level>]",
	    "INFO",
	    nullptr,
	    -1,
	    nullptr,
	    "Log level: OFF, FATAL, ERROR, WARN, INFO, DEBUG, TRACE (default: INFO)",
	},
	{ nullptr, 0, nullptr, nullptr, nullptr, -1, nullptr, nullptr },
};

/* Called at the start of each frame paint cycle */
static BOOL ka_begin_paint(rdpContext* context)
{
	rdpGdi* gdi = nullptr;

	WINPR_ASSERT(context);

	gdi = context->gdi;
	WINPR_ASSERT(gdi);
	WINPR_ASSERT(gdi->primary);
	WINPR_ASSERT(gdi->primary->hdc);
	WINPR_ASSERT(gdi->primary->hdc->hwnd);
	WINPR_ASSERT(gdi->primary->hdc->hwnd->invalid);
	gdi->primary->hdc->hwnd->invalid->null = TRUE;
	return TRUE;
}

/* Called when the library completed composing a new frame */
static BOOL ka_end_paint(rdpContext* context)
{
	rdpGdi* gdi = nullptr;

	WINPR_ASSERT(context);

	gdi = context->gdi;
	WINPR_ASSERT(gdi);
	WINPR_ASSERT(gdi->primary);

	HGDI_DC hdc = gdi->primary->hdc;
	WINPR_ASSERT(hdc);
	if (!hdc->hwnd)
		return TRUE;

	HGDI_WND hwnd = hdc->hwnd;
	WINPR_ASSERT(hwnd->invalid || (hwnd->ninvalid == 0));

	if (hwnd->invalid->null)
		return TRUE;

	return TRUE;
}

/* Called when the desktop resolution changes */
static BOOL ka_desktop_resize(rdpContext* context)
{
	rdpGdi* gdi = nullptr;
	rdpSettings* settings = nullptr;

	WINPR_ASSERT(context);

	settings = context->settings;
	WINPR_ASSERT(settings);

	gdi = context->gdi;
	return gdi_resize(gdi, freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
	                  freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight));
}

/* Called to play a sound (unused in headless mode) */
static BOOL ka_play_sound(rdpContext* context, const PLAY_SOUND_UPDATE* play_sound)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(play_sound);
	return TRUE;
}

/* Called before connection is established */
static BOOL ka_pre_connect(freerdp* instance)
{
	rdpSettings* settings = nullptr;

	WINPR_ASSERT(instance);
	WINPR_ASSERT(instance->context);

	settings = instance->context->settings;
	WINPR_ASSERT(settings);

	/* Enable mouse motion events so our keep-alive PTR_FLAGS_MOVE gets sent */
	if (!freerdp_settings_set_bool(settings, FreeRDP_MouseMotion, TRUE))
		return FALSE;

	/* Prefer PEM certificate to get full certificate details in callbacks */
	if (!freerdp_settings_set_bool(settings, FreeRDP_CertificateCallbackPreferPEM, TRUE))
		return FALSE;

	/* Identify as a Windows client to the server */
	if (!freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_WINDOWS))
		return FALSE;
	if (!freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_WINDOWS_NT))
		return FALSE;

	/* Enable auto-accept of certificates if cert-ignore was specified */
	if (freerdp_settings_get_bool(settings, FreeRDP_IgnoreCertificate))
	{
		if (!freerdp_settings_set_bool(settings, FreeRDP_AutoAcceptCertificate, TRUE))
			return FALSE;
	}

	/* If /no-render is set, suppress all graphics output to save server performance */
	{
		rdpKeepAliveContext* ka = (rdpKeepAliveContext*)instance->context;
		if (ka->noRender)
		{
			WLog_INFO(TAG, "Applying graphics suppression settings...");

			/* Tell server to suppress display updates (key 2307) */
			if (!freerdp_settings_set_bool(settings, FreeRDP_SuppressOutput, TRUE))
				return FALSE;

			/* Disable all GFX codecs */
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE))
				return FALSE;
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE))
				return FALSE;
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, FALSE))
				return FALSE;
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxProgressive, FALSE))
				return FALSE;
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxProgressiveV2, FALSE))
				return FALSE;
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxPlanar, FALSE))
				return FALSE;
			if (!freerdp_settings_set_bool(settings, FreeRDP_GfxCodecAV1, FALSE))
				return FALSE;

			/* Disable RemoteFX */
			if (!freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE))
				return FALSE;

			/* Disable Surface Commands */
			if (!freerdp_settings_set_bool(settings, FreeRDP_SurfaceCommandsEnabled, FALSE))
				return FALSE;
		}
	}

	/* Subscribe to channel connect/disconnect events */
	if (PubSub_SubscribeChannelConnected(instance->context->pubSub,
	                                     freerdp_client_OnChannelConnectedEventHandler) < 0)
		return FALSE;
	if (PubSub_SubscribeChannelDisconnected(instance->context->pubSub,
	                                        freerdp_client_OnChannelDisconnectedEventHandler) < 0)
		return FALSE;

	return TRUE;
}

/* Called after RDP connection is successfully established */
static BOOL ka_post_connect(freerdp* instance)
{
	rdpContext* context = nullptr;

	if (!gdi_init(instance, PIXEL_FORMAT_XRGB32))
		return FALSE;

	context = instance->context;
	WINPR_ASSERT(context);
	WINPR_ASSERT(context->update);

	/* Disable client-side decoding since we don't render frames */
	if (!freerdp_settings_set_bool(context->settings, FreeRDP_DeactivateClientDecoding, TRUE))
		return FALSE;

	context->update->BeginPaint = ka_begin_paint;
	context->update->EndPaint = ka_end_paint;
	context->update->PlaySound = ka_play_sound;
	context->update->DesktopResize = ka_desktop_resize;

	{
		const char* server = freerdp_settings_get_server_name(context->settings);
		WLog_INFO(TAG, "Connected to %s", server ? server : "unknown");
	}
	return TRUE;
}

/* Called when a session ends (success or failure) */
static void ka_post_disconnect(freerdp* instance)
{
	if (!instance || !instance->context)
		return;

	PubSub_UnsubscribeChannelConnected(instance->context->pubSub,
	                                   freerdp_client_OnChannelConnectedEventHandler);
	PubSub_UnsubscribeChannelDisconnected(instance->context->pubSub,
	                                      freerdp_client_OnChannelDisconnectedEventHandler);
	gdi_free(instance);
}

/* Logon error information callback */
static int ka_logon_error_info(freerdp* instance, UINT32 data, UINT32 type)
{
	const char* str_data = freerdp_get_logon_error_info_data(data);
	const char* str_type = freerdp_get_logon_error_info_type(type);

	if (!instance || !instance->context)
		return -1;

	WLog_INFO(TAG, "Logon Error Info %s [%s]", str_data, str_type);
	return 1;
}

/* Send a mouse move event to keep the session alive */
static BOOL ka_send_keepalive(rdpContext* context)
{
	rdpClientContext* cctx = (rdpClientContext*)context;

	WINPR_ASSERT(cctx);

	/* Send a mouse move to (0,0) - this resets server idle timer
	 * without actually moving the visible cursor since we're at the origin */
	if (!freerdp_client_send_button_event(cctx, FALSE, PTR_FLAGS_MOVE, 0, 0))
	{
		WLog_ERR(TAG, "Failed to send keep-alive mouse event");
		return FALSE;
	}

	WLog_INFO(TAG, "Keep-alive: sent mouse move event");
	return TRUE;
}

/* Custom command-line argument handler */
static int ka_handle_option(const COMMAND_LINE_ARGUMENT_A* arg, void* custom)
{
	rdpContext* context = (rdpContext*)custom;

	if (!arg || !arg->Name)
		return 0;

	if (strcmp(arg->Name, "interval") == 0)
	{
		rdpKeepAliveContext* ka = (rdpKeepAliveContext*)context;
		LONG val = strtol(arg->Value, nullptr, 10);

		if (val < 1 || val > 86400)
		{
			WLog_ERR(TAG, "Invalid interval value: %s (must be 1-86400 seconds)", arg->Value);
			return -1;
		}
		ka->keepAliveInterval = (UINT32)val * 1000;
		WLog_INFO(TAG, "Keep-alive interval set to %ld seconds", val);
		return 0;
	}

	if (strcmp(arg->Name, "cert-ignore") == 0)
	{
		/* Already handled via FreeRDP_IgnoreCertificate in global args;
		 * just ensure auto-accept is enabled */
		rdpSettings* settings = context->settings;
		if (!freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE))
			return -1;
		return 0;
	}

	if (strcmp(arg->Name, "no-render") == 0)
	{
		rdpKeepAliveContext* ka = (rdpKeepAliveContext*)context;
		ka->noRender = TRUE;
		WLog_INFO(TAG, "Graphics output suppression enabled (/no-render)");
		return 0;
	}

	if (strcmp(arg->Name, "mouse") == 0)
	{
		rdpKeepAliveContext* ka = (rdpKeepAliveContext*)context;
		ka->useMouseInput = TRUE;
		WLog_INFO(TAG, "Mouse-based keep-alive enabled (/mouse)");
		return 0;
	}

	if (strcmp(arg->Name, "log-level") == 0)
	{
		WLog_SetStringLogLevel(WLog_GetRoot(), arg->Value);
		WLog_INFO(TAG, "Log level set to %s", arg->Value);
		return 0;
	}

	/* Unknown custom argument */
	return -1;
}

/* RDP main event loop with keep-alive timer */
static DWORD WINAPI ka_thread_proc(LPVOID arg)
{
	freerdp* instance = (freerdp*)arg;
	DWORD nCount = 0;
	DWORD status = 0;
	DWORD result = 0;
	HANDLE handles[MAXIMUM_WAIT_OBJECTS] = WINPR_C_ARRAY_INIT;
	rdpKeepAliveContext* ka = nullptr;
	BOOL rc = FALSE;

	WINPR_ASSERT(instance);
	WINPR_ASSERT(instance->context);

	ka = (rdpKeepAliveContext*)instance->context;
	WINPR_ASSERT(ka);

	rc = freerdp_connect(instance);

	if (freerdp_settings_get_bool(instance->context->settings, FreeRDP_AuthenticationOnly))
	{
		result = freerdp_get_last_error(instance->context);
		freerdp_abort_connect_context(instance->context);
		WLog_ERR(TAG, "Authentication only, exit status 0x%08" PRIx32 "", result);
		goto disconnect;
	}

	if (!rc)
	{
		result = freerdp_get_last_error(instance->context);
		WLog_ERR(TAG, "Connection failure 0x%08" PRIx32, result);
		goto disconnect;
	}

	WLog_INFO(TAG, "Keep-alive started (interval: %" PRIu32 " seconds)",
	          ka->keepAliveInterval / 1000);

	ka->lastKeepAliveTick = GetTickCount64();

	while (!freerdp_shall_disconnect_context(instance->context))
	{
		nCount = freerdp_get_event_handles(instance->context, handles, ARRAYSIZE(handles));

		if (nCount == 0)
		{
			WLog_ERR(TAG, "freerdp_get_event_handles failed");
			break;
		}

		status = WaitForMultipleObjects(nCount, handles, FALSE, KA_TICK_MS);

		if (status == WAIT_FAILED)
		{
			WLog_ERR(TAG, "WaitForMultipleObjects failed with %" PRIu32 "", status);
			break;
		}

		if (!freerdp_check_event_handles(instance->context))
		{
			if (freerdp_get_last_error(instance->context) == FREERDP_ERROR_SUCCESS)
				WLog_ERR(TAG, "Failed to check FreeRDP event handles");

			break;
		}

		/* Check if keep-alive interval has elapsed */
		{
			UINT64 now = GetTickCount64();

			if (now - ka->lastKeepAliveTick >= ka->keepAliveInterval)
			{
				if (ka->useMouseInput)
				{
					if (!ka_send_keepalive(instance->context))
						break;
				}
				else
				{
					/* Default: just log that we're still alive.
					 * RDP protocol heartbeats and TCP keepalive
					 * maintain the session without explicit input. */
					WLog_DBG(TAG, "Keep-alive tick (connection maintained)");
				}

				ka->lastKeepAliveTick = now;
			}
		}
	}

disconnect:
	freerdp_disconnect(instance);
	return result;
}

/* Global initializer - register signal handler for stack traces */
static BOOL ka_client_global_init(void)
{
	return freerdp_handle_signals() == 0;
}

/* Global cleanup (no-op) */
static void ka_client_global_uninit(void)
{
}

/* Called when a new client context is created - wire the callbacks */
static BOOL ka_client_new(freerdp* instance, rdpContext* context)
{
	rdpKeepAliveContext* ka = (rdpKeepAliveContext*)context;

	if (!instance || !context)
		return FALSE;

	instance->PreConnect = ka_pre_connect;
	instance->PostConnect = ka_post_connect;
	instance->PostDisconnect = ka_post_disconnect;
	instance->LogonErrorInfo = ka_logon_error_info;

	/* Initialize keep-alive state */
	ka->lastKeepAliveTick = 0;
	ka->keepAliveInterval = RDP_KEEPALIVE_DEFAULT_INTERVAL_MS;
	ka->noRender = FALSE;
	ka->useMouseInput = FALSE;

	return TRUE;
}

/* Called when client context is freed */
static void ka_client_free(freerdp* instance, rdpContext* context)
{
	WINPR_UNUSED(instance);
	WINPR_UNUSED(context);
}

/* Called to start the client */
static int ka_client_start(rdpContext* context)
{
	WINPR_UNUSED(context);
	return 0;
}

/* Called to stop the client */
static int ka_client_stop(rdpContext* context)
{
	WINPR_UNUSED(context);
	return 0;
}

/* Fill the client entry points */
static int RdpClientEntry(RDP_CLIENT_ENTRY_POINTS* pEntryPoints)
{
	WINPR_ASSERT(pEntryPoints);

	ZeroMemory(pEntryPoints, sizeof(RDP_CLIENT_ENTRY_POINTS));
	pEntryPoints->Version = RDP_CLIENT_INTERFACE_VERSION;
	pEntryPoints->Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
	pEntryPoints->GlobalInit = ka_client_global_init;
	pEntryPoints->GlobalUninit = ka_client_global_uninit;
	pEntryPoints->ContextSize = sizeof(rdpKeepAliveContext);
	pEntryPoints->ClientNew = ka_client_new;
	pEntryPoints->ClientFree = ka_client_free;
	pEntryPoints->ClientStart = ka_client_start;
	pEntryPoints->ClientStop = ka_client_stop;
	return 0;
}

static void ka_print_usage(void)
{
	const char* usage =
	    "\n"
	    "RDP Keep-Alive Tool - Prevents RDP session idle timeout\n"
	    "\n"
	    "Usage: rdp-keepalive /v:<server>[:port] [/u:<username>] [/p:<password>]\n"
	    "                    [/d:<domain>] [/size:<W>x<H>]\n"
	    "                    [/scale:<percent>] [/interval:<seconds>]\n"
	    "                    [/cert-ignore] [/?]\n"
	    "\n"
	    "Common options:\n"
	    "  /v:<server>[:port]    RDP server address (required)\n"
	    "  /u:<username>         Login username\n"
	    "  /p:<password>         Login password\n"
	    "  /d:<domain>           Domain\n"
	    "  /size:<W>x<H>         Desktop resolution (e.g. 800x600)\n"
	    "  /w:<width>            Desktop width\n"
	    "  /h:<height>           Desktop height\n"
	    "  /scale:<percent>      DPI scaling percentage (e.g. 150)\n"
	    "  /scale-desktop:<pct>  Desktop scale factor only\n"
	    "  /scale-device:<pct>   Device scale factor only\n"
	    "  /bpp:<depth>          Color depth (default: 32, use 16 with /no-render)\n"
	    "  /admin                Connect to admin session\n"
	    "  /network:<type>       Network type: auto, lan, broadband, modem\n"
	    "  /restricted-admin     Restricted admin mode\n"
	    "\n"
	    "Keep-alive options:\n"
	    "  /interval:<seconds>   Keep-alive interval (default: 60, range: 1-86400)\n"
	    "  /cert-ignore          Accept all server certificates\n"
	    "  /no-render            Suppress graphics output (saves server performance)\n"
	    "  /mouse                Use mouse move events for keep-alive (off by default)\n"
	    "  /log-level:<level>    Log level: OFF, FATAL, ERROR, WARN, INFO, DEBUG, TRACE\n"
	    "\n"
	    "Other:\n"
	    "  /help, /?, -h         Show this help message\n"
	    "  /list                 Show all available options\n"
	    "  /buildconfig          Show build configuration\n"
	    "\n"
	    "Performance example:\n"
	    "  rdp-keepalive /v:server /u:admin /p:pass\n"
	    "                /size:800x600 /bpp:16 /no-render /cert-ignore\n"
	    "\n"
	    "Debug example:\n"
	    "  rdp-keepalive /v:server /u:admin /p:pass /log-level:DEBUG\n"
	    "                /size:800x600 /no-render /cert-ignore\n";

	fprintf(stderr, "%s", usage);
}

int main(int argc, char* argv[])
{
	int rc = -1;
	RDP_CLIENT_ENTRY_POINTS clientEntryPoints = WINPR_C_ARRAY_INIT;

	/* Initialize Winsock on Windows (required for getaddrinfo) */
	{
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
	}

	if (argc < 2)
	{
		ka_print_usage();
		return 1;
	}

	RdpClientEntry(&clientEntryPoints);
	rdpContext* context = freerdp_client_context_new(&clientEntryPoints);

	if (!context)
	{
		fprintf(stderr, "Failed to create FreeRDP client context\n");
		return 1;
	}

	{
		const int status = freerdp_client_settings_parse_command_line_ex(
		    context->settings, argc, argv, FALSE, ka_args, ARRAYSIZE(ka_args) - 1,
		    ka_handle_option, context);

		if (status)
		{
			rc = freerdp_client_settings_command_line_status_print_ex(
			    context->settings, status, argc, argv, ka_args);

			if (status == COMMAND_LINE_STATUS_PRINT_HELP)
				ka_print_usage();

			goto fail;
		}
	}

	if (freerdp_client_start(context) != 0)
	{
		fprintf(stderr, "Failed to start FreeRDP client\n");
		goto fail;
	}

	{
		const DWORD res = ka_thread_proc(context->instance);
		rc = (int)res;
	}

	if (freerdp_client_stop(context) != 0)
		rc = -1;

fail:
	freerdp_client_context_free(context);

	if (rc != 0)
		fprintf(stderr, "rdp-keepalive exited with code %d\n", rc);

	return rc;
}
