/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RDP Keep-Alive Client
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

#ifndef FREERDP_CLIENT_KEEPALIVE_H
#define FREERDP_CLIENT_KEEPALIVE_H

#include <freerdp/freerdp.h>
#include <freerdp/client.h>

typedef struct
{
	rdpClientContext common;

	UINT64 lastKeepAliveTick;
	UINT32 keepAliveInterval; /* Keep-alive interval in milliseconds */
	BOOL noRender;            /* /no-render: suppress graphics output */
	BOOL useMouseInput;       /* /mouse: use mouse move for keep-alive (off by default) */
} rdpKeepAliveContext;

#define RDP_KEEPALIVE_DEFAULT_INTERVAL_MS 60000
#define KA_TICK_MS 1000

#endif /* FREERDP_CLIENT_KEEPALIVE_H */
