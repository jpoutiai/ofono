/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (C) 2013 Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>
#include <ofono/types.h>

#include "gril.h"
#include "grilutil.h"
#include "common.h"
#include "rilmodem.h"

#include <ofono/netreg.h>

/*
 * This module is the ofono_gprs_driver implementation for rilmodem.
 *
 * Notes:
 *
 * 1. ofono_gprs_suspend/resume() are not used by this module, as
 *    the concept of suspended GPRS is not exposed by RILD.
 *
 * 2. ofono_gprs_bearer_notify() is never called as RILD does not
 *    expose an unsolicited event equivalent to +CPSB ( see 27.007
 *    7.29 ), and the tech values returned by REQUEST_DATA/VOICE
 *    _REGISTRATION requests do not match the values defined for
 *    <AcT> in the +CPSB definition.  Note, the values returned by
 *    the *REGISTRATION commands are aligned with those defined by
 *    +CREG ( see 27.003 7.2 ).
 */

struct gprs_data {
	GRil *ril;
	gboolean ofono_attached;
	int max_cids;
	int rild_status;
	int true_status;
	gboolean notified;
	guint registerid;
	guint timer_id;
	guint fake_timer_id;
};

gboolean registered;

static void ril_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb,
					void *data);

static void ril_gprs_state_change(struct ril_msg *message, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;

	g_assert(message->req == RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED);
	DBG("JPO RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED");
	DBG("JPO %s",ril_unsol_request_to_string(message->req));
	/* We need to notify core always to cover situations when
	 * connection drops temporarily for example when user is
	 * taking CS voice call from LTE or changing technology
	 * preference */
	ril_gprs_registration_status(gprs, NULL, NULL);
}

static gboolean ril_gprs_set_attached_callback(gpointer user_data)
{
	struct ofono_error error;
	struct cb_data *cbd = user_data;
	ofono_gprs_cb_t cb = cbd->cb;
	struct ofono_gprs *gprs = cbd->user;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	DBG("");

	gd->timer_id = 0;

	decode_ril_error(&error, "OK");

	cb(&error, cbd->data);

	g_free(cbd);

	return FALSE;
}

static void ril_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data);
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	DBG("attached: %d", attached);
	/*
	* As RIL offers no actual control over the GPRS 'attached'
	* state, we save the desired state, and use it to override
	* the actual modem's state in the 'attached_status' function.
	* This is similar to the way the core ofono gprs code handles
	* data roaming ( see src/gprs.c gprs_netreg_update().
	*
	* The core gprs code calls driver->set_attached() when a netreg
	* notification is received and any configured roaming conditions
	* are met.
	*/

	gd->ofono_attached = attached;

	cbd->user = gprs;

//	ril_gprs_registration_status(gprs, NULL, NULL);
	/*
	* However we cannot respond immediately, since core sets the
	* value of driver_attached after calling set_attached and that
	* leads to comparison failure in gprs_attached_update in
	* connection drop phase
	*/
	gd->timer_id = g_timeout_add_seconds(1, ril_gprs_set_attached_callback,
						cbd);
}
static gboolean ril_fake_response(gpointer user_data)
{
	struct cb_data *cbd = user_data;
//	ofono_gprs_status_cb_t cb = cbd->cb;
	struct ofono_gprs *gprs = cbd->user;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);


	DBG("");
	ofono_gprs_status_notify(gprs, gd->true_status);
	g_free(cbd);
	return FALSE;
}

static void ril_data_reg_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_status_cb_t cb = cbd->cb;
	struct ofono_gprs *gprs = cbd->user;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct ofono_error error;
	int status, lac, ci, tech;
	int max_cids = 1;

	DBG("");
	if (gd && message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("ril_data_reg_cb: reply failure: %s",
				ril_error_to_string(message->error));
		decode_ril_error(&error, "FAIL");
		error.error = message->error;
		status = -1;
		goto error;
	}

	if (ril_util_parse_reg(gd->ril, message, &status,
				&lac, &ci, &tech, &max_cids) == FALSE) {
		ofono_error("Failure parsing data registration response.");
		decode_ril_error(&error, "FAIL");
		status = -1;
		goto error;
	}

	if (((gd->fake_timer_id > 0) && (gd->true_status != status)) ||
			((gd->fake_timer_id > 0) && !(gd->ofono_attached))) {
		g_source_remove(gd->fake_timer_id);
		gd->true_status = -1;
	}

	if (status > 10)
		status = status - 10;

	if (!registered) {
		ofono_gprs_register(gprs);
		registered = TRUE;
	}

	if (max_cids > gd->max_cids) {
		DBG("Setting max cids to %d", max_cids);
		gd->max_cids = max_cids;
		ofono_gprs_set_cid_range(gprs, 1, max_cids);
	}

	ofono_info("data registration status is %d", status);

	if (status == NETWORK_REGISTRATION_STATUS_ROAMING)
		status = check_if_really_roaming(status);

	DBG(" attached:%d, status:%d",gd->ofono_attached,status);
	if (gd->ofono_attached /*&& !gd->notified*/) {
		if (status == NETWORK_REGISTRATION_STATUS_ROAMING ||
			status == NETWORK_REGISTRATION_STATUS_REGISTERED) {
//			DBG("connection becomes available");
//			gd->ofono_attached = TRUE;
			ofono_gprs_status_notify(gprs, status);
//			gd->notified = TRUE;
			gd->rild_status = status;
		} else {
			if (gd->fake_timer_id > 0)
				if (gd->true_status != status)
					gd->true_status = status;
			else {
				struct cb_data *fake_cbd = cb_data_new(NULL, NULL);
				fake_cbd->user = gprs;
				gd->true_status = status;
				DBG("JPO fake_timer");
				gd->fake_timer_id = g_timeout_add_seconds(5, ril_fake_response, fake_cbd);
			}
			status = NETWORK_REGISTRATION_STATUS_REGISTERED;
			gd->rild_status = status;
		}
		goto error;
	}else {
		ofono_gprs_status_notify(gprs, status);
		gd->rild_status = status;
		goto error;
	}


error:
	ofono_info("data registration status is %d", status);
	if (cb)
		cb(&error, status, cbd->data);
}

static void ril_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb,
					void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data);
	int request = RIL_REQUEST_DATA_REGISTRATION_STATE;
	guint ret;
	DBG("");
	if (gd == NULL || cbd == NULL)
		return;

	cbd->user = gprs;

	ret = g_ril_send(gd->ril, request,
				NULL, 0,
				((gd->rild_status == -1)
				? ril_data_probe_reg_cb
				: ril_data_reg_cb), cbd, g_free);

	g_ril_print_request_no_args(gd->ril, ret, request);

	if (ret <= 0) {
		ofono_error("Send RIL_REQUEST_DATA_RESTISTRATION_STATE failed.");
		g_free(cbd);
		if (cb)
			CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static int ril_gprs_probe(struct ofono_gprs *gprs,
				unsigned int vendor, void *data)
{
	GRil *ril = data;
	struct gprs_data *gd;

	gd = g_try_new0(struct gprs_data, 1);
	if (gd == NULL)
		return -ENOMEM;

	gd->ril = g_ril_clone(ril);
	gd->ofono_attached = FALSE;
	gd->max_cids = 0;
	gd->rild_status = -1;
	gd->true_status = -1;
	gd->notified = FALSE;
	gd->registerid = -1;
	gd->timer_id = 0;

	registered = FALSE;

	ofono_gprs_set_data(gprs, gd);

	ril_gprs_registration_status(gprs, NULL, NULL);

	return 0;
}

static void ril_gprs_remove(struct ofono_gprs *gprs)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	DBG("");

	ofono_gprs_set_data(gprs, NULL);

	if (gd->registerid != -1)
		g_ril_unregister(gd->ril, gd->registerid);

	if (gd->timer_id > 0)
		g_source_remove(gd->timer_id);

	if (gd->fake_timer_id > 0)
		g_source_remove(gd->fake_timer_id);

	g_ril_unref(gd->ril);
	g_free(gd);
}

static struct ofono_gprs_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_gprs_probe,
	.remove			= ril_gprs_remove,
	.set_attached		= ril_gprs_set_attached,
	.attached_status	= ril_gprs_registration_status,
};

void ril_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void ril_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
