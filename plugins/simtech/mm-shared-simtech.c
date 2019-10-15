/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2019 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <config.h>

#include <glib-object.h>
#include <gio/gio.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-voice.h"
#include "mm-iface-modem-location.h"
#include "mm-base-modem.h"
#include "mm-base-modem-at.h"
#include "mm-shared-simtech.h"
#include "mm-modem-helpers-simtech.h"

/*****************************************************************************/
/* Private data context */

#define PRIVATE_TAG "shared-simtech-private-tag"
static GQuark private_quark;

typedef enum {
    FEATURE_SUPPORT_UNKNOWN,
    FEATURE_NOT_SUPPORTED,
    FEATURE_SUPPORTED,
} FeatureSupport;

typedef struct {
    /* location */
    MMIfaceModemLocation  *iface_modem_location_parent;
    MMModemLocationSource  supported_sources;
    MMModemLocationSource  enabled_sources;
    FeatureSupport         cgps_support;
    /* voice */
    MMIfaceModemVoice     *iface_modem_voice_parent;
    FeatureSupport         clcc_urc_support;
    GRegex                *clcc_urc_regex;
    GRegex                *voice_call_regex;
    GRegex                *cring_regex;
} Private;

static void
private_free (Private *ctx)
{
    g_regex_unref (ctx->cring_regex);
    g_regex_unref (ctx->voice_call_regex);
    g_regex_unref (ctx->clcc_urc_regex);
    g_slice_free (Private, ctx);
}

static Private *
get_private (MMSharedSimtech *self)
{
    Private *priv;

    if (G_UNLIKELY (!private_quark))
        private_quark =  (g_quark_from_static_string (PRIVATE_TAG));

    priv = g_object_get_qdata (G_OBJECT (self), private_quark);
    if (!priv) {
        priv = g_slice_new0 (Private);
        priv->supported_sources = MM_MODEM_LOCATION_SOURCE_NONE;
        priv->enabled_sources = MM_MODEM_LOCATION_SOURCE_NONE;
        priv->cgps_support = FEATURE_SUPPORT_UNKNOWN;
        priv->clcc_urc_support = FEATURE_SUPPORT_UNKNOWN;
        priv->clcc_urc_regex = mm_simtech_get_clcc_urc_regex ();
        priv->voice_call_regex = mm_simtech_get_voice_call_urc_regex ();
        priv->cring_regex = mm_simtech_get_cring_urc_regex ();

        /* Setup parent class' MMIfaceModemLocation and MMIfaceModemVoice */

        g_assert (MM_SHARED_SIMTECH_GET_INTERFACE (self)->peek_parent_location_interface);
        priv->iface_modem_location_parent = MM_SHARED_SIMTECH_GET_INTERFACE (self)->peek_parent_location_interface (self);

        g_assert (MM_SHARED_SIMTECH_GET_INTERFACE (self)->peek_parent_voice_interface);
        priv->iface_modem_voice_parent = MM_SHARED_SIMTECH_GET_INTERFACE (self)->peek_parent_voice_interface (self);

        g_object_set_qdata_full (G_OBJECT (self), private_quark, priv, (GDestroyNotify)private_free);
    }

    return priv;
}

/*****************************************************************************/
/* GPS trace received */

static void
trace_received (MMPortSerialGps      *port,
                const gchar          *trace,
                MMIfaceModemLocation *self)
{
    /* Helper to debug GPS location related issues. Don't depend on a real GPS
     * fix for debugging, just use some random values to update */
#if 0
    if (g_str_has_prefix (trace, "$GPGGA")) {
        GString *str;
        GDateTime *now;

        now = g_date_time_new_now_utc ();
        str = g_string_new ("");
        g_string_append_printf (str,
                                "$GPGGA,%02u%02u%02u,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
                                g_date_time_get_hour (now),
                                g_date_time_get_minute (now),
                                g_date_time_get_second (now));
        mm_iface_modem_location_gps_update (self, str->str);
        g_string_free (str, TRUE);
        g_date_time_unref (now);
        return;
    }
#endif

    mm_iface_modem_location_gps_update (self, trace);
}

/*****************************************************************************/
/* Location capabilities loading (Location interface) */

MMModemLocationSource
mm_shared_simtech_location_load_capabilities_finish (MMIfaceModemLocation  *self,
                                                     GAsyncResult          *res,
                                                     GError               **error)
{
    GError *inner_error = NULL;
    gssize aux;

    aux = g_task_propagate_int (G_TASK (res), &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return MM_MODEM_LOCATION_SOURCE_NONE;
    }
    return (MMModemLocationSource) aux;
}

static void probe_gps_features (GTask *task);

static void
cgps_test_ready (MMBaseModem  *self,
                 GAsyncResult *res,
                 GTask        *task)
{
    Private *priv;

    priv = get_private (MM_SHARED_SIMTECH (self));

    if (!mm_base_modem_at_command_finish (self, res, NULL))
        priv->cgps_support = FEATURE_NOT_SUPPORTED;
    else
        priv->cgps_support = FEATURE_SUPPORTED;

    probe_gps_features (task);
}

static void
probe_gps_features (GTask *task)
{
    MMSharedSimtech       *self;
    MMModemLocationSource  sources;
    Private               *priv;

    self = MM_SHARED_SIMTECH (g_task_get_source_object (task));
    priv = get_private (self);

    /* Need to check if CGPS supported... */
    if (priv->cgps_support == FEATURE_SUPPORT_UNKNOWN) {
        mm_base_modem_at_command (MM_BASE_MODEM (self), "+CGPS=?", 3, TRUE, (GAsyncReadyCallback) cgps_test_ready, task);
        return;
    }

    /* All GPS features probed */

    /* Recover parent sources */
    sources = GPOINTER_TO_UINT (g_task_get_task_data (task));

    if (priv->cgps_support == FEATURE_SUPPORTED) {
        mm_dbg ("GPS commands supported: GPS capabilities enabled");

        /* We only flag as supported by this implementation those sources not already
         * supported by the parent implementation */
        if (!(sources & MM_MODEM_LOCATION_SOURCE_GPS_NMEA))
            priv->supported_sources |= MM_MODEM_LOCATION_SOURCE_GPS_NMEA;
        if (!(sources & MM_MODEM_LOCATION_SOURCE_GPS_RAW))
            priv->supported_sources |= MM_MODEM_LOCATION_SOURCE_GPS_RAW;
        if (!(sources & MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED))
            priv->supported_sources |= MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED;

        sources |= priv->supported_sources;

        /* Add handler for the NMEA traces in the GPS data port */
        mm_port_serial_gps_add_trace_handler (mm_base_modem_peek_port_gps (MM_BASE_MODEM (self)),
                                              (MMPortSerialGpsTraceFn)trace_received,
                                              self,
                                              NULL);
    } else
        mm_dbg ("No GPS command supported: no GPS capabilities");

    g_task_return_int (task, (gssize) sources);
    g_object_unref (task);
}

static void
parent_load_capabilities_ready (MMIfaceModemLocation *self,
                                GAsyncResult         *res,
                                GTask                *task)
{
    MMModemLocationSource  sources;
    GError                *error = NULL;
    Private               *priv;

    priv = get_private (MM_SHARED_SIMTECH (self));

    sources = priv->iface_modem_location_parent->load_capabilities_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Now our own check. If we don't have any GPS port, we're done */
    if (!mm_base_modem_peek_port_gps (MM_BASE_MODEM (self))) {
        mm_dbg ("No GPS data port found: no GPS capabilities");
        g_task_return_int (task, sources);
        g_object_unref (task);
        return;
    }

    /* Cache sources supported by the parent */
    g_task_set_task_data (task, GUINT_TO_POINTER (sources), NULL);

    /* Probe all GPS features */
    probe_gps_features (task);
}

void
mm_shared_simtech_location_load_capabilities (MMIfaceModemLocation *self,
                                              GAsyncReadyCallback   callback,
                                              gpointer              user_data)
{
    Private *priv;
    GTask   *task;

    priv = get_private (MM_SHARED_SIMTECH (self));
    task = g_task_new (self, NULL, callback, user_data);

    g_assert (priv->iface_modem_location_parent);
    g_assert (priv->iface_modem_location_parent->load_capabilities);
    g_assert (priv->iface_modem_location_parent->load_capabilities_finish);

    priv->iface_modem_location_parent->load_capabilities (self,
                                                          (GAsyncReadyCallback)parent_load_capabilities_ready,
                                                          task);
}

/*****************************************************************************/
/* Disable location gathering (Location interface) */

gboolean
mm_shared_simtech_disable_location_gathering_finish (MMIfaceModemLocation  *self,
                                                     GAsyncResult          *res,
                                                     GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
disable_cgps_ready (MMBaseModem  *self,
                    GAsyncResult *res,
                    GTask        *task)
{
    MMModemLocationSource  source;
    Private               *priv;
    GError                *error = NULL;

    priv = get_private (MM_SHARED_SIMTECH (self));

    mm_base_modem_at_command_finish (self, res, &error);

    /* Only use the GPS port in NMEA/RAW setups */
    source = (MMModemLocationSource) GPOINTER_TO_UINT (g_task_get_task_data (task));
    if (source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA | MM_MODEM_LOCATION_SOURCE_GPS_RAW)) {
        MMPortSerialGps *gps_port;

        /* Even if we get an error here, we try to close the GPS port */
        gps_port = mm_base_modem_peek_port_gps (MM_BASE_MODEM (self));
        if (gps_port)
            mm_port_serial_close (MM_PORT_SERIAL (gps_port));
    }

    if (error)
        g_task_return_error (task, error);
    else {
        priv->enabled_sources &= source;
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
parent_disable_location_gathering_ready (MMIfaceModemLocation *self,
                                         GAsyncResult         *res,
                                         GTask                *task)
{
    GError  *error;
    Private *priv;

    priv = get_private (MM_SHARED_SIMTECH (self));

    g_assert (priv->iface_modem_location_parent);
    if (!priv->iface_modem_location_parent->disable_location_gathering_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
mm_shared_simtech_disable_location_gathering (MMIfaceModemLocation  *self,
                                              MMModemLocationSource  source,
                                              GAsyncReadyCallback    callback,
                                              gpointer               user_data)
{
    MMModemLocationSource  enabled_sources;
    Private               *priv;
    GTask                 *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (source), NULL);

    priv = get_private (MM_SHARED_SIMTECH (self));
    g_assert (priv->iface_modem_location_parent);

    /* Only consider request if it applies to one of the sources we are
     * supporting, otherwise run parent disable */
    if (!(priv->supported_sources & source)) {
        /* If disabling implemented by the parent, run it. */
        if (priv->iface_modem_location_parent->disable_location_gathering &&
            priv->iface_modem_location_parent->disable_location_gathering_finish) {
            priv->iface_modem_location_parent->disable_location_gathering (self,
                                                                           source,
                                                                           (GAsyncReadyCallback)parent_disable_location_gathering_ready,
                                                                           task);
            return;
        }
        /* Otherwise, we're done */
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* We only expect GPS sources here */
    g_assert (source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                        MM_MODEM_LOCATION_SOURCE_GPS_RAW |
                        MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED));

    /* Flag as disabled to see how many others we would have left enabled */
    enabled_sources = priv->enabled_sources;
    enabled_sources &= ~source;

    /* If there are still GPS-related sources enabled, do nothing else */
    if (enabled_sources & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                           MM_MODEM_LOCATION_SOURCE_GPS_RAW |
                           MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED)) {
        priv->enabled_sources &= ~source;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* Stop GPS engine if all GPS-related sources are disabled */
    g_assert (priv->cgps_support == FEATURE_SUPPORTED);
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+CGPS=0",
                              10,
                              FALSE,
                              (GAsyncReadyCallback) disable_cgps_ready,
                              task);
}

/*****************************************************************************/
/* Enable location gathering (Location interface) */

gboolean
mm_shared_simtech_enable_location_gathering_finish (MMIfaceModemLocation  *self,
                                                    GAsyncResult          *res,
                                                    GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
enable_cgps_ready (MMBaseModem  *self,
                   GAsyncResult *res,
                   GTask        *task)
{
    MMModemLocationSource  source;
    GError                *error = NULL;
    Private               *priv;

    priv = get_private (MM_SHARED_SIMTECH (self));

    if (!mm_base_modem_at_command_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Only use the GPS port in NMEA/RAW setups */
    source = (MMModemLocationSource) GPOINTER_TO_UINT (g_task_get_task_data (task));
    if (source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                  MM_MODEM_LOCATION_SOURCE_GPS_RAW)) {
        MMPortSerialGps *gps_port;

        gps_port = mm_base_modem_peek_port_gps (MM_BASE_MODEM (self));
        if (!gps_port || !mm_port_serial_open (MM_PORT_SERIAL (gps_port), &error)) {
            if (error)
                g_task_return_error (task, error);
            else
                g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                         "Couldn't open raw GPS serial port");
            g_object_unref (task);
            return;
        }
    }

    priv->enabled_sources |= source;
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_enable_location_gathering_ready (MMIfaceModemLocation *self,
                                        GAsyncResult         *res,
                                        GTask                *task)
{
    GError  *error;
    Private *priv;

    priv = get_private (MM_SHARED_SIMTECH (self));

    g_assert (priv->iface_modem_location_parent);
    if (!priv->iface_modem_location_parent->enable_location_gathering_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
mm_shared_simtech_enable_location_gathering (MMIfaceModemLocation  *self,
                                             MMModemLocationSource  source,
                                             GAsyncReadyCallback    callback,
                                             gpointer               user_data)
{
    Private *priv;
    GTask   *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (source), NULL);

    priv = get_private (MM_SHARED_SIMTECH (self));
    g_assert (priv->iface_modem_location_parent);
    g_assert (priv->iface_modem_location_parent->enable_location_gathering);
    g_assert (priv->iface_modem_location_parent->enable_location_gathering_finish);

    /* Only consider request if it applies to one of the sources we are
     * supporting, otherwise run parent enable */
    if (!(priv->supported_sources & source)) {
        priv->iface_modem_location_parent->enable_location_gathering (self,
                                                                      source,
                                                                      (GAsyncReadyCallback)parent_enable_location_gathering_ready,
                                                                      task);
        return;
    }

    /* We only expect GPS sources here */
    g_assert (source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                        MM_MODEM_LOCATION_SOURCE_GPS_RAW |
                        MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED));

    /* If GPS already started, store new flag and we're done */
    if (priv->enabled_sources & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                                 MM_MODEM_LOCATION_SOURCE_GPS_RAW |
                                 MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED)) {
        priv->enabled_sources |= source;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    g_assert (priv->cgps_support == FEATURE_SUPPORTED);
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+CGPS=1,1",
                              10,
                              FALSE,
                              (GAsyncReadyCallback) enable_cgps_ready,
                              task);
}

/*****************************************************************************/
/* Common enable/disable voice unsolicited events */

typedef struct {
    gboolean        enable;
    MMPortSerialAt *primary;
    MMPortSerialAt *secondary;
    gchar          *clcc_command;
    gboolean        clcc_primary_done;
    gboolean        clcc_secondary_done;
} VoiceUnsolicitedEventsContext;

static void
voice_unsolicited_events_context_free (VoiceUnsolicitedEventsContext *ctx)
{
    g_clear_object (&ctx->secondary);
    g_clear_object (&ctx->primary);
    g_free (ctx->clcc_command);
    g_slice_free (VoiceUnsolicitedEventsContext, ctx);
}

static gboolean
common_voice_enable_disable_unsolicited_events_finish (MMSharedSimtech  *self,
                                                       GAsyncResult       *res,
                                                       GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void run_voice_enable_disable_unsolicited_events (GTask *task);

static void
clcc_command_ready (MMBaseModem  *self,
                    GAsyncResult *res,
                    GTask        *task)
{
    VoiceUnsolicitedEventsContext *ctx;
    GError                        *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (self, res, &error)) {
        mm_dbg ("Couldn't %s +CLCC reporting: '%s'",
                ctx->enable ? "enable" : "disable",
                error->message);
        g_error_free (error);
    }

    /* Continue on next port */
    run_voice_enable_disable_unsolicited_events (task);
}

static void
run_voice_enable_disable_unsolicited_events (GTask *task)
{
    MMSharedSimtech               *self;
    Private                       *priv;
    VoiceUnsolicitedEventsContext *ctx;
    MMPortSerialAt                *port = NULL;

    self = MM_SHARED_SIMTECH (g_task_get_source_object (task));
    priv = get_private (self);
    ctx  = g_task_get_task_data (task);

    /* If +CLCC URCs not supported, we're done */
    if (priv->clcc_urc_support == FEATURE_NOT_SUPPORTED) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    if (!ctx->clcc_primary_done && ctx->primary) {
        mm_dbg ("%s +CLCC extended list of current calls reporting in primary port...",
                ctx->enable ? "Enabling" : "Disabling");
        ctx->clcc_primary_done = TRUE;
        port = ctx->primary;
    } else if (!ctx->clcc_secondary_done && ctx->secondary) {
        mm_dbg ("%s +CLCC extended list of current calls reporting in secondary port...",
                ctx->enable ? "Enabling" : "Disabling");
        ctx->clcc_secondary_done = TRUE;
        port = ctx->secondary;
    }

    if (port) {
        mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                       port,
                                       ctx->clcc_command,
                                       3,
                                       FALSE,
                                       FALSE,
                                       NULL,
                                       (GAsyncReadyCallback)clcc_command_ready,
                                       task);
        return;
    }

    /* Fully done now */
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
common_voice_enable_disable_unsolicited_events (MMSharedSimtech     *self,
                                                gboolean             enable,
                                                GAsyncReadyCallback  callback,
                                                gpointer             user_data)
{
    VoiceUnsolicitedEventsContext *ctx;
    GTask                         *task;

    task = g_task_new (self, NULL, callback, user_data);

    ctx = g_slice_new0 (VoiceUnsolicitedEventsContext);
    ctx->enable = enable;
    if (enable)
        ctx->clcc_command = g_strdup ("+CLCC=1");
    else
        ctx->clcc_command = g_strdup ("+CLCC=0");
    ctx->primary = mm_base_modem_get_port_primary (MM_BASE_MODEM (self));
    ctx->secondary = mm_base_modem_get_port_secondary (MM_BASE_MODEM (self));
    g_task_set_task_data (task, ctx, (GDestroyNotify) voice_unsolicited_events_context_free);

    run_voice_enable_disable_unsolicited_events (task);
}

/*****************************************************************************/
/* Disable unsolicited events (Voice interface) */

gboolean
mm_shared_simtech_voice_disable_unsolicited_events_finish (MMIfaceModemVoice  *self,
                                                           GAsyncResult       *res,
                                                           GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_voice_disable_unsolicited_events_ready (MMIfaceModemVoice *self,
                                               GAsyncResult      *res,
                                               GTask             *task)
{
    GError  *error = NULL;
    Private *priv;

    priv = get_private (MM_SHARED_SIMTECH (self));

    if (!priv->iface_modem_voice_parent->disable_unsolicited_events_finish (self, res, &error)) {
        mm_warn ("Couldn't disable parent voice unsolicited events: %s", error->message);
        g_error_free (error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
voice_disable_unsolicited_events_ready (MMSharedSimtech *self,
                                        GAsyncResult      *res,
                                        GTask             *task)
{
    Private *priv;
    GError  *error = NULL;

    if (!common_voice_enable_disable_unsolicited_events_finish (self, res, &error)) {
        mm_warn ("Couldn't disable Simtech-specific voice unsolicited events: %s", error->message);
        g_error_free (error);
    }

    priv = get_private (MM_SHARED_SIMTECH (self));
    g_assert (priv->iface_modem_voice_parent);
    g_assert (priv->iface_modem_voice_parent->disable_unsolicited_events);
    g_assert (priv->iface_modem_voice_parent->disable_unsolicited_events_finish);

    /* Chain up parent's disable */
    priv->iface_modem_voice_parent->disable_unsolicited_events (
        MM_IFACE_MODEM_VOICE (self),
        (GAsyncReadyCallback)parent_voice_disable_unsolicited_events_ready,
        task);
}

void
mm_shared_simtech_voice_disable_unsolicited_events (MMIfaceModemVoice   *self,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* our own disabling first */
    common_voice_enable_disable_unsolicited_events (MM_SHARED_SIMTECH (self),
                                                    FALSE,
                                                    (GAsyncReadyCallback) voice_disable_unsolicited_events_ready,
                                                    task);
}

/*****************************************************************************/
/* Enable unsolicited events (Voice interface) */

gboolean
mm_shared_simtech_voice_enable_unsolicited_events_finish (MMIfaceModemVoice  *self,
                                                          GAsyncResult       *res,
                                                          GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
voice_enable_unsolicited_events_ready (MMSharedSimtech *self,
                                       GAsyncResult      *res,
                                       GTask             *task)
{
    GError *error = NULL;

    if (!common_voice_enable_disable_unsolicited_events_finish (self, res, &error)) {
        mm_warn ("Couldn't enable Simtech-specific voice unsolicited events: %s", error->message);
        g_error_free (error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_voice_enable_unsolicited_events_ready (MMIfaceModemVoice *self,
                                              GAsyncResult      *res,
                                              GTask             *task)
{
    GError  *error = NULL;
    Private *priv;

    priv = get_private (MM_SHARED_SIMTECH (self));

    if (!priv->iface_modem_voice_parent->enable_unsolicited_events_finish (self, res, &error)) {
        mm_warn ("Couldn't enable parent voice unsolicited events: %s", error->message);
        g_error_free (error);
    }

    /* our own enabling next */
    common_voice_enable_disable_unsolicited_events (MM_SHARED_SIMTECH (self),
                                                    TRUE,
                                                    (GAsyncReadyCallback) voice_enable_unsolicited_events_ready,
                                                    task);
}

void
mm_shared_simtech_voice_enable_unsolicited_events (MMIfaceModemVoice   *self,
                                                   GAsyncReadyCallback  callback,
                                                   gpointer             user_data)
{
    Private *priv;
    GTask   *task;

    task = g_task_new (self, NULL, callback, user_data);

    priv = get_private (MM_SHARED_SIMTECH (self));
    g_assert (priv->iface_modem_voice_parent);
    g_assert (priv->iface_modem_voice_parent->enable_unsolicited_events);
    g_assert (priv->iface_modem_voice_parent->enable_unsolicited_events_finish);

    /* chain up parent's enable first */
    priv->iface_modem_voice_parent->enable_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_voice_enable_unsolicited_events_ready,
        task);
}

/*****************************************************************************/
/* Common setup/cleanup voice unsolicited events */

static void
clcc_urc_received (MMPortSerialAt  *port,
                   GMatchInfo      *match_info,
                   MMSharedSimtech *self)
{
    gchar  *full;
    GError *error = NULL;
    GList  *call_info_list = NULL;

    full = g_match_info_fetch (match_info, 0);

    if (!mm_simtech_parse_clcc_list (full, &call_info_list, &error)) {
        mm_warn ("couldn't parse +CLCC list in URC: %s", error->message);
        g_error_free (error);
    } else
        mm_iface_modem_voice_report_all_calls (MM_IFACE_MODEM_VOICE (self), call_info_list);

    mm_simtech_call_info_list_free (call_info_list);
    g_free (full);
}

static void
voice_call_urc_received (MMPortSerialAt  *port,
                         GMatchInfo      *match_info,
                         MMSharedSimtech *self)
{
    GError   *error = NULL;
    gboolean  start_or_stop = FALSE; /* start = TRUE, stop = FALSE */
    guint     duration = 0;

    if (!mm_simtech_parse_voice_call_urc (match_info, &start_or_stop, &duration, &error)) {
        mm_warn ("couldn't parse VOICE CALL URC: %s", error->message);
        g_error_free (error);
        return;
    }

    if (start_or_stop) {
        mm_dbg ("voice call started");
        return;
    }

    if (duration) {
        mm_dbg ("voice call finished (duration: %us)", duration);
        return;
    }

    mm_dbg ("voice call finished");
}

static void
cring_urc_received (MMPortSerialAt  *port,
                    GMatchInfo      *info,
                    MMSharedSimtech *self)
{
    MMCallInfo  call_info;
    gchar      *str;

    /* We could have "VOICE" or "DATA". Now consider only "VOICE" */
    str = mm_get_string_unquoted_from_match_info (info, 1);
    mm_dbg ("Ringing (%s)", str);
    g_free (str);

    call_info.index     = 0;
    call_info.direction = MM_CALL_DIRECTION_INCOMING;
    call_info.state     = MM_CALL_STATE_RINGING_IN;
    call_info.number    = NULL;

    mm_iface_modem_voice_report_call (MM_IFACE_MODEM_VOICE (self), &call_info);
}

static void
common_voice_setup_cleanup_unsolicited_events (MMSharedSimtech *self,
                                               gboolean         enable)
{
    Private        *priv;
    MMPortSerialAt *ports[2];
    guint           i;

    priv = get_private (MM_SHARED_SIMTECH (self));

    ports[0] = mm_base_modem_peek_port_primary   (MM_BASE_MODEM (self));
    ports[1] = mm_base_modem_peek_port_secondary (MM_BASE_MODEM (self));

    for (i = 0; i < G_N_ELEMENTS (ports); i++) {
        if (!ports[i])
            continue;

        if (priv->clcc_urc_support == FEATURE_SUPPORTED)
            mm_port_serial_at_add_unsolicited_msg_handler (ports[i],
                                                           priv->clcc_urc_regex,
                                                           enable ? (MMPortSerialAtUnsolicitedMsgFn)clcc_urc_received : NULL,
                                                           enable ? self : NULL,
                                                           NULL);

        mm_port_serial_at_add_unsolicited_msg_handler (ports[i],
                                                       priv->voice_call_regex,
                                                       enable ? (MMPortSerialAtUnsolicitedMsgFn)voice_call_urc_received : NULL,
                                                       enable ? self : NULL,
                                                       NULL);

        mm_port_serial_at_add_unsolicited_msg_handler (ports[i],
                                                       priv->cring_regex,
                                                       enable ? (MMPortSerialAtUnsolicitedMsgFn)cring_urc_received : NULL,
                                                       enable ? self : NULL,
                                                       NULL);
    }
}

/*****************************************************************************/
/* Cleanup unsolicited events (Voice interface) */

gboolean
mm_shared_simtech_voice_cleanup_unsolicited_events_finish (MMIfaceModemVoice  *self,
                                                           GAsyncResult       *res,
                                                           GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_voice_cleanup_unsolicited_events_ready (MMIfaceModemVoice *self,
                                               GAsyncResult      *res,
                                               GTask             *task)
{
    GError  *error = NULL;
    Private *priv;

    priv = get_private (MM_SHARED_SIMTECH (self));

    if (!priv->iface_modem_voice_parent->cleanup_unsolicited_events_finish (self, res, &error)) {
        mm_warn ("Couldn't cleanup parent voice unsolicited events: %s", error->message);
        g_error_free (error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
mm_shared_simtech_voice_cleanup_unsolicited_events (MMIfaceModemVoice   *self,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data)
{
    Private *priv;
    GTask   *task;

    task = g_task_new (self, NULL, callback, user_data);

    priv = get_private (MM_SHARED_SIMTECH (self));
    g_assert (priv->iface_modem_voice_parent);
    g_assert (priv->iface_modem_voice_parent->cleanup_unsolicited_events);
    g_assert (priv->iface_modem_voice_parent->cleanup_unsolicited_events_finish);

    /* our own cleanup first */
    common_voice_setup_cleanup_unsolicited_events (MM_SHARED_SIMTECH (self), FALSE);

    /* Chain up parent's cleanup */
    priv->iface_modem_voice_parent->cleanup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_voice_cleanup_unsolicited_events_ready,
        task);
}

/*****************************************************************************/
/* Setup unsolicited events (Voice interface) */

gboolean
mm_shared_simtech_voice_setup_unsolicited_events_finish (MMIfaceModemVoice  *self,
                                                         GAsyncResult       *res,
                                                         GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_voice_setup_unsolicited_events_ready (MMIfaceModemVoice *self,
                                             GAsyncResult      *res,
                                             GTask             *task)
{
    GError  *error = NULL;
    Private *priv;

    priv = get_private (MM_SHARED_SIMTECH (self));

    if (!priv->iface_modem_voice_parent->setup_unsolicited_events_finish (self, res, &error)) {
        mm_warn ("Couldn't setup parent voice unsolicited events: %s", error->message);
        g_error_free (error);
    }

    /* our own setup next */
    common_voice_setup_cleanup_unsolicited_events (MM_SHARED_SIMTECH (self), TRUE);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
mm_shared_simtech_voice_setup_unsolicited_events (MMIfaceModemVoice   *self,
                                                  GAsyncReadyCallback  callback,
                                                  gpointer             user_data)
{
    Private *priv;
    GTask   *task;

    task = g_task_new (self, NULL, callback, user_data);

    priv = get_private (MM_SHARED_SIMTECH (self));
    g_assert (priv->iface_modem_voice_parent);
    g_assert (priv->iface_modem_voice_parent->setup_unsolicited_events);
    g_assert (priv->iface_modem_voice_parent->setup_unsolicited_events_finish);

    /* chain up parent's setup first */
    priv->iface_modem_voice_parent->setup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_voice_setup_unsolicited_events_ready,
        task);
}

/*****************************************************************************/
/* Check if Voice supported (Voice interface) */

gboolean
mm_shared_simtech_voice_check_support_finish (MMIfaceModemVoice  *self,
                                              GAsyncResult       *res,
                                              GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
clcc_format_check_ready (MMBroadbandModem *self,
                         GAsyncResult     *res,
                         GTask            *task)
{
    Private     *priv;
    GError      *error = NULL;
    const gchar *response;
    gboolean     clcc_urc_supported = FALSE;

    priv = get_private (MM_SHARED_SIMTECH (self));

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, NULL);
    if (response && !mm_simtech_parse_clcc_test (response, &clcc_urc_supported, &error)) {
        mm_dbg ("failed checking CLCC URC support: %s", error->message);
        g_clear_error (&error);
    }

    priv->clcc_urc_support = (clcc_urc_supported ? FEATURE_SUPPORTED : FEATURE_NOT_SUPPORTED);
    mm_dbg ("modem %s +CLCC URCs", (priv->clcc_urc_support == FEATURE_SUPPORTED) ? "supports" : "doesn't support");

    /* If +CLCC URC supported we won't need polling in the parent */
    g_object_set (self,
                  MM_IFACE_MODEM_VOICE_PERIODIC_CALL_LIST_CHECK_DISABLED, (priv->clcc_urc_support == FEATURE_SUPPORTED),
                  NULL);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_voice_check_support_ready (MMIfaceModemVoice *self,
                                  GAsyncResult      *res,
                                  GTask             *task)
{
    Private *priv;
    GError  *error = NULL;

    priv = get_private (MM_SHARED_SIMTECH (self));
    if (!priv->iface_modem_voice_parent->check_support_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* voice is supported, check if +CLCC URCs are available */
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+CLCC=?",
                              3,
                              TRUE,
                              (GAsyncReadyCallback) clcc_format_check_ready,
                              task);
}

void
mm_shared_simtech_voice_check_support (MMIfaceModemVoice   *self,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    Private *priv;
    GTask   *task;

    task = g_task_new (self, NULL, callback, user_data);

    priv = get_private (MM_SHARED_SIMTECH (self));
    g_assert (priv->iface_modem_voice_parent);
    g_assert (priv->iface_modem_voice_parent->check_support);
    g_assert (priv->iface_modem_voice_parent->check_support_finish);

    /* chain up parent's setup first */
    priv->iface_modem_voice_parent->check_support (
        self,
        (GAsyncReadyCallback)parent_voice_check_support_ready,
        task);
}

/*****************************************************************************/

static void
shared_simtech_init (gpointer g_iface)
{
}

GType
mm_shared_simtech_get_type (void)
{
    static GType shared_simtech_type = 0;

    if (!G_UNLIKELY (shared_simtech_type)) {
        static const GTypeInfo info = {
            sizeof (MMSharedSimtech),  /* class_size */
            shared_simtech_init,       /* base_init */
            NULL,                  /* base_finalize */
        };

        shared_simtech_type = g_type_register_static (G_TYPE_INTERFACE, "MMSharedSimtech", &info, 0);
        g_type_interface_add_prerequisite (shared_simtech_type, MM_TYPE_IFACE_MODEM_LOCATION);
    }

    return shared_simtech_type;
}