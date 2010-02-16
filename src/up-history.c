/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "egg-debug.h"
#include "up-history.h"
#include "up-stats-obj.h"
#include "up-history-obj.h"

static void	up_history_finalize	(GObject		*object);

#define UP_HISTORY_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), UP_TYPE_HISTORY, UpHistoryPrivate))

#define UP_HISTORY_SAVE_INTERVAL	10*60 /* seconds */

struct UpHistoryPrivate
{
	gchar			*id;
	gdouble			 rate_last;
	gint64			 time_full_last;
	gint64			 time_empty_last;
	gdouble			 percentage_last;
	UpDeviceState		 state;
	GPtrArray		*data_rate;
	GPtrArray		*data_charge;
	GPtrArray		*data_time_full;
	GPtrArray		*data_time_empty;
	guint			 save_id;
};

enum {
	UP_HISTORY_PROGRESS,
	UP_HISTORY_LAST_SIGNAL
};

G_DEFINE_TYPE (UpHistory, up_history, G_TYPE_OBJECT)
#define UP_HISTORY_FILE_HEADER	"PackageKit Profile"

/**
 * up_history_array_copy_cb:
 **/
static void
up_history_array_copy_cb (const UpHistoryObj *obj, GPtrArray *dest)
{
	g_ptr_array_add (dest, up_history_obj_copy (obj));
}

/**
 * up_history_array_limit_resolution:
 * @array: The data we have for a specific graph
 * @max_num: The max desired points
 *
 * We need to reduce the number of data points else the graph will take a long
 * time to plot accuracy we don't need at the larger scales.
 * This will not reduce the scale or range of the data.
 *
 *  100  +     + |  +   |  +   | +     +
 *       |  A    |      |      |
 *   80  +     + |  +   |  +   | +     +
 *       |       | B  C |      |
 *   60  +     + |  +   |  +   | +     +
 *       |       |      |      |
 *   40  +     + |  +   |  +   | + E   +
 *       |       |      |      | D
 *   20  +     + |  +   |  +   | +   F +
 *       |       |      |      |
 *    0  +-----+-----+-----+-----+-----+
 *            20    40    60    80   100
 *
 * A = 15,90
 * B = 30,70
 * C = 52,70
 * D = 80,30
 * E = 85,40
 * F = 90,20
 *
 * 1 = 15,90
 * 2 = 41,70
 * 3 = 85,30
 **/
static GPtrArray *
up_history_array_limit_resolution (GPtrArray *array, guint max_num)
{
	const UpHistoryObj *obj;
	UpHistoryObj *nobj;
	gfloat division;
	guint length;
	gint i;
	guint last;
	guint first;
	GPtrArray *new;
	UpDeviceState state = UP_DEVICE_STATE_UNKNOWN;
	guint64 time_s = 0;
	gdouble value = 0;
	guint64 count = 0;
	guint step = 1;
	gfloat preset;

	new = g_ptr_array_new_with_free_func ((GDestroyNotify) up_history_obj_free);
	egg_debug ("length of array (before) %i", array->len);

	/* check length */
	length = array->len;
	if (length == 0)
		goto out;
	if (length < max_num) {
		/* need to copy array */
		g_ptr_array_foreach (array, (GFunc) up_history_array_copy_cb, new);
		goto out;
	}

	/* last element */
	obj = (const UpHistoryObj *) g_ptr_array_index (array, length-1);
	last = obj->time;
	obj = (const UpHistoryObj *) g_ptr_array_index (array, 0);
	first = obj->time;

	division = (first - last) / (gfloat) max_num;
	egg_debug ("Using a x division of %f (first=%i,last=%i)", division, first, last);

	/* Reduces the number of points to a pre-set level using a time
	 * division algorithm so we don't keep diluting the previous
	 * data with a conventional 1-in-x type algorithm. */
	for (i=length-1; i>=0; i--) {
		obj = (const UpHistoryObj *) g_ptr_array_index (array, i);
		preset = last + (division * (gfloat) step);

		/* if state changed or we went over the preset do a new point */
		if (count > 0 && (obj->time > preset || obj->state != state)) {
			nobj = up_history_obj_new ();
			nobj->time = time_s / count;
			nobj->value = value / count;
			nobj->state = state;
			g_ptr_array_add (new, nobj);

			step++;
			time_s = obj->time;
			value = obj->value;
			state = obj->state;
			count = 1;
		} else {
			count++;
			time_s += obj->time;
			value += obj->value;
		}
	}

	/* only add if nonzero */
	if (count > 0) {
		nobj = up_history_obj_new ();
		nobj->time = time_s / count;
		nobj->value = value / count;
		nobj->state = state;
		g_ptr_array_add (new, nobj);
	}

	/* check length */
	egg_debug ("length of array (after) %i", new->len);
out:
	return new;
}

/**
 * up_history_copy_array_timespan:
 **/
static GPtrArray *
up_history_copy_array_timespan (const GPtrArray *array, guint timespan)
{
	guint i;
	const UpHistoryObj *obj;
	GPtrArray *array_new;
	GTimeVal timeval;

	/* no data */
	if (array->len == 0)
		return NULL;

	/* new data */
	array_new = g_ptr_array_new ();
	g_get_current_time (&timeval);

	/* treat the timespan like a range, and search backwards */
	timespan *= 0.95f;
	for (i=array->len-1; i>0; i--) {
		obj = (const UpHistoryObj *) g_ptr_array_index (array, i);
		if (timeval.tv_sec - obj->time < timespan)
			g_ptr_array_add (array_new, up_history_obj_copy (obj));
	}

	return array_new;
}

/**
 * up_history_get_data:
 **/
GPtrArray *
up_history_get_data (UpHistory *history, UpHistoryType type, guint timespan, guint resolution)
{
	GPtrArray *array;
	GPtrArray *array_resolution;
	const GPtrArray *array_data = NULL;

	g_return_val_if_fail (UP_IS_HISTORY (history), NULL);

	if (history->priv->id == NULL)
		return NULL;

	if (type == UP_HISTORY_TYPE_CHARGE)
		array_data = history->priv->data_charge;
	else if (type == UP_HISTORY_TYPE_RATE)
		array_data = history->priv->data_rate;
	else if (type == UP_HISTORY_TYPE_TIME_FULL)
		array_data = history->priv->data_time_full;
	else if (type == UP_HISTORY_TYPE_TIME_EMPTY)
		array_data = history->priv->data_time_empty;

	/* not recognised */
	if (array_data == NULL)
		return NULL;

	/* only return a certain time */
	array = up_history_copy_array_timespan (array_data, timespan);
	if (array == NULL)
		return NULL;

	/* only add a certain number of points */
	array_resolution = up_history_array_limit_resolution (array, resolution);
	g_ptr_array_unref (array);

	return array_resolution;
}

/**
 * up_history_get_profile_data:
 **/
GPtrArray *
up_history_get_profile_data (UpHistory *history, gboolean charging)
{
	guint i;
	guint non_zero_accuracy = 0;
	gfloat average = 0.0f;
	guint bin;
	guint oldbin = 999;
	const UpHistoryObj *obj_last = NULL;
	const UpHistoryObj *obj;
	const UpHistoryObj *obj_old = NULL;
	UpStatsObj *stats;
	GPtrArray *array;
	GPtrArray *data;
	guint time_s;
	gdouble value;
	gdouble total_value = 0.0f;

	g_return_val_if_fail (UP_IS_HISTORY (history), NULL);

	/* create 100 item list and set to zero */
	data = g_ptr_array_new ();
	for (i=0; i<101; i++) {
		stats = up_stats_obj_create (0.0f, 0.0f);
		g_ptr_array_add (data, stats);
	}

	array = history->priv->data_charge;
	for (i=0; i<array->len; i++) {
		obj = (const UpHistoryObj *) g_ptr_array_index (array, i);
		if (obj_last == NULL || obj->state != obj_last->state) {
			obj_old = NULL;
			goto cont;
		}

		/* round to the nearest int */
		bin = rint (obj->value);

		/* ensure bin is in range */
		if (bin >= data->len)
			bin = data->len - 1;

		/* different */
		if (oldbin != bin) {
			oldbin = bin;
			if (obj_old != NULL) {
				/* not enough or too much difference */
				value = fabs (obj->value - obj_old->value);
				if (value < 0.01f) {
					obj_old = NULL;
					goto cont;
				}
				if (value > 3.0f) {
					obj_old = NULL;
					goto cont;
				}

				time_s = obj->time - obj_old->time;
				/* use the accuracy field as a counter for now */
				if ((charging && obj->state == UP_DEVICE_STATE_CHARGING) ||
				    (!charging && obj->state == UP_DEVICE_STATE_DISCHARGING)) {
					stats = (UpStatsObj *) g_ptr_array_index (data, bin);
					stats->value += time_s;
					stats->accuracy++;
				}
			}
			obj_old = obj;
		}
cont:
		obj_last = obj;
	}

	/* divide the value by the number of samples to make the average */
	for (i=0; i<101; i++) {
		stats = (UpStatsObj *) g_ptr_array_index (data, i);
		if (stats->accuracy != 0)
			stats->value = stats->value / stats->accuracy;
	}

	/* find non-zero accuracy values for the average */
	for (i=0; i<101; i++) {
		stats = (UpStatsObj *) g_ptr_array_index (data, i);
		if (stats->accuracy > 0) {
			total_value += stats->value;
			non_zero_accuracy++;
		}
	}

	/* average */
	if (non_zero_accuracy != 0)
		average = total_value / non_zero_accuracy;
	egg_debug ("average is %f", average);

	/* make the values a factor of 0, so that 1.0 is twice the
	 * average, and -1.0 is half the average */
	for (i=0; i<101; i++) {
		stats = (UpStatsObj *) g_ptr_array_index (data, i);
		if (stats->accuracy > 0)
			stats->value = (stats->value - average) / average;
		else
			stats->value = 0.0f;
	}

	/* accuracy is a percentage scale, where each cycle = 20% */
	for (i=0; i<101; i++) {
		stats = (UpStatsObj *) g_ptr_array_index (data, i);
		stats->accuracy *= 20;
		if (stats->accuracy > 100.0f)
			stats->accuracy = 100.0f;
	}

	return data;
}

/**
 * up_history_get_filename:
 **/
static gchar *
up_history_get_filename (UpHistory *history, const gchar *type)
{
	gchar *path;
	gchar *filename;

	filename = g_strdup_printf ("history-%s-%s.dat", type, history->priv->id);
	path = g_build_filename (PACKAGE_LOCALSTATE_DIR, "lib", "upower", filename, NULL);
	g_free (filename);
	return path;
}

/**
 * up_history_array_to_file:
 * @list: a valid #GPtrArray instance
 * @filename: a filename
 *
 * Saves a copy of the list to a file
 **/
static gboolean
up_history_array_to_file (GPtrArray *list, const gchar *filename)
{
	guint i;
	const UpHistoryObj *obj;
	gchar *part;
	GString *string;
	gboolean ret = TRUE;
	GError *error = NULL;

	/* generate data */
	string = g_string_new ("");
	for (i=0; i<list->len; i++) {
		obj = g_ptr_array_index (list, i);
		part = up_history_obj_to_string (obj);
		if (part == NULL) {
			ret = FALSE;
			break;
		}
		g_string_append_printf (string, "%s\n", part);
		g_free (part);
	}
	part = g_string_free (string, FALSE);

	/* we failed to convert to string */
	if (!ret) {
		egg_warning ("failed to convert");
		goto out;
	}

	/* save to disk */
	ret = g_file_set_contents (filename, part, -1, &error);
	if (!ret) {
		egg_warning ("failed to set data: %s", error->message);
		g_error_free (error);
		goto out;
	}
	egg_debug ("saved %s", filename);

out:
	g_free (part);
	return ret;
}

/**
 * up_history_array_from_file:
 * @list: a valid #GPtrArray instance
 * @filename: a filename
 *
 * Appends the list from a file
 **/
static gboolean
up_history_array_from_file (GPtrArray *list, const gchar *filename)
{
	gboolean ret;
	GError *error = NULL;
	gchar *data = NULL;
	gchar **parts = NULL;
	guint i;
	guint length;
	UpHistoryObj *obj;

	/* do we exist */
	ret = g_file_test (filename, G_FILE_TEST_EXISTS);
	if (!ret) {
		egg_debug ("failed to get data from %s as file does not exist", filename);
		goto out;
	}

	/* get contents */
	ret = g_file_get_contents (filename, &data, NULL, &error);
	if (!ret) {
		egg_warning ("failed to get data: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* split by line ending */
	parts = g_strsplit (data, "\n", 0);
	length = g_strv_length (parts);
	if (length == 0) {
		egg_debug ("no data in %s", filename);
		goto out;
	}

	/* add valid entries */
	egg_debug ("loading %i items of data from %s", length, filename);
	for (i=0; i<length-1; i++) {
		obj = up_history_obj_from_string (parts[i]);
		if (obj != NULL)
			g_ptr_array_add (list, obj);
	}

out:
	g_strfreev (parts);
	g_free (data);
	return ret;
}

/**
 * up_history_save_data:
 **/
static gboolean
up_history_save_data (UpHistory *history)
{
	gchar *filename;

	/* we have an ID? */
	if (history->priv->id == NULL) {
		egg_warning ("no ID, cannot save");
		return FALSE;
	}

	/* save rate history to disk */
	filename = up_history_get_filename (history, "rate");
	up_history_array_to_file (history->priv->data_rate, filename);
	g_free (filename);

	/* save charge history to disk */
	filename = up_history_get_filename (history, "charge");
	up_history_array_to_file (history->priv->data_charge, filename);
	g_free (filename);

	/* save charge history to disk */
	filename = up_history_get_filename (history, "time-full");
	up_history_array_to_file (history->priv->data_time_full, filename);
	g_free (filename);

	/* save charge history to disk */
	filename = up_history_get_filename (history, "time-empty");
	up_history_array_to_file (history->priv->data_time_empty, filename);
	g_free (filename);

	return TRUE;
}

/**
 * up_history_schedule_save_cb:
 **/
static gboolean
up_history_schedule_save_cb (UpHistory *history)
{
	up_history_save_data (history);
	history->priv->save_id = 0;
	return FALSE;
}

/**
 * up_history_is_low_power:
 **/
static gboolean
up_history_is_low_power (UpHistory *history)
{
	guint length;
	const UpHistoryObj *obj;

	/* current status is always up to date */
	if (history->priv->state != UP_DEVICE_STATE_DISCHARGING)
		return FALSE;

	/* have we got any data? */
	length = history->priv->data_charge->len;
	if (length == 0)
		return FALSE;

	/* get the last saved charge object */
	obj = (const UpHistoryObj *) g_ptr_array_index (history->priv->data_charge, length-1);
	if (obj->state != UP_DEVICE_STATE_DISCHARGING)
		return FALSE;

	/* high enough */
	if (obj->value > 10)
		return FALSE;

	/* we are low power */
	return TRUE;
}

/**
 * up_history_schedule_save:
 **/
static gboolean
up_history_schedule_save (UpHistory *history)
{
	gboolean ret;

	/* if low power, then don't batch up save requests */
	ret = up_history_is_low_power (history);
	if (ret) {
		egg_warning ("saving directly to disk as low power");
		up_history_save_data (history);
		return TRUE;
	}

	/* we already have one saved */
	if (history->priv->save_id != 0) {
		egg_debug ("deferring as others queued");
		return TRUE;
	}

	/* nothing scheduled, do new */
	egg_debug ("saving in %i seconds", UP_HISTORY_SAVE_INTERVAL);
	history->priv->save_id = g_timeout_add_seconds (UP_HISTORY_SAVE_INTERVAL,
							(GSourceFunc) up_history_schedule_save_cb, history);

	return TRUE;
}

/**
 * up_history_load_data:
 **/
static gboolean
up_history_load_data (UpHistory *history)
{
	gchar *filename;
	UpHistoryObj *obj;

	/* load rate history from disk */
	filename = up_history_get_filename (history, "rate");
	up_history_array_from_file (history->priv->data_rate, filename);
	g_free (filename);

	/* load charge history from disk */
	filename = up_history_get_filename (history, "charge");
	up_history_array_from_file (history->priv->data_charge, filename);
	g_free (filename);

	/* load charge history from disk */
	filename = up_history_get_filename (history, "time-full");
	up_history_array_from_file (history->priv->data_time_full, filename);
	g_free (filename);

	/* load charge history from disk */
	filename = up_history_get_filename (history, "time-empty");
	up_history_array_from_file (history->priv->data_time_empty, filename);
	g_free (filename);

	/* save a marker so we don't use incomplete percentages */
	obj = up_history_obj_create (0, UP_DEVICE_STATE_UNKNOWN);
	g_ptr_array_add (history->priv->data_rate, up_history_obj_copy (obj));
	g_ptr_array_add (history->priv->data_charge, up_history_obj_copy (obj));
	g_ptr_array_add (history->priv->data_time_full, up_history_obj_copy (obj));
	g_ptr_array_add (history->priv->data_time_empty, up_history_obj_copy (obj));
	up_history_obj_free (obj);
	up_history_schedule_save (history);

	return TRUE;
}

/**
 * up_history_set_id:
 **/
gboolean
up_history_set_id (UpHistory *history, const gchar *id)
{
	gboolean ret;

	g_return_val_if_fail (UP_IS_HISTORY (history), FALSE);

	if (history->priv->id != NULL)
		return FALSE;
	if (id == NULL)
		return FALSE;

	egg_debug ("using id: %s", id);
	history->priv->id = g_strdup (id);
	/* load all previous data */
	ret = up_history_load_data (history);
	return ret;
}

/**
 * up_history_set_state:
 **/
gboolean
up_history_set_state (UpHistory *history, UpDeviceState state)
{
	g_return_val_if_fail (UP_IS_HISTORY (history), FALSE);

	if (history->priv->id == NULL)
		return FALSE;
	history->priv->state = state;
	return TRUE;
}

/**
 * up_history_set_charge_data:
 **/
gboolean
up_history_set_charge_data (UpHistory *history, gdouble percentage)
{
	UpHistoryObj *obj;

	g_return_val_if_fail (UP_IS_HISTORY (history), FALSE);

	if (history->priv->id == NULL)
		return FALSE;
	if (history->priv->state == UP_DEVICE_STATE_UNKNOWN)
		return FALSE;
	if (history->priv->percentage_last == percentage)
		return FALSE;

	/* add to array and schedule save file */
	obj = up_history_obj_create (percentage, history->priv->state);
	g_ptr_array_add (history->priv->data_charge, obj);
	up_history_schedule_save (history);

	/* save last value */
	history->priv->percentage_last = percentage;

	return TRUE;
}

/**
 * up_history_set_rate_data:
 **/
gboolean
up_history_set_rate_data (UpHistory *history, gdouble rate)
{
	UpHistoryObj *obj;

	g_return_val_if_fail (UP_IS_HISTORY (history), FALSE);

	if (history->priv->id == NULL)
		return FALSE;
	if (history->priv->state == UP_DEVICE_STATE_UNKNOWN)
		return FALSE;
	if (history->priv->rate_last == rate)
		return FALSE;

	/* add to array and schedule save file */
	obj = up_history_obj_create (rate, history->priv->state);
	g_ptr_array_add (history->priv->data_rate, obj);
	up_history_schedule_save (history);

	/* save last value */
	history->priv->rate_last = rate;

	return TRUE;
}

/**
 * up_history_set_time_full_data:
 **/
gboolean
up_history_set_time_full_data (UpHistory *history, gint64 time_s)
{
	UpHistoryObj *obj;

	g_return_val_if_fail (UP_IS_HISTORY (history), FALSE);

	if (history->priv->id == NULL)
		return FALSE;
	if (history->priv->state == UP_DEVICE_STATE_UNKNOWN)
		return FALSE;
	if (time_s < 0)
		return FALSE;
	if (history->priv->time_full_last == time_s)
		return FALSE;

	/* add to array and schedule save file */
	obj = up_history_obj_create ((gdouble) time_s, history->priv->state);
	g_ptr_array_add (history->priv->data_time_full, obj);
	up_history_schedule_save (history);

	/* save last value */
	history->priv->time_full_last = time_s;

	return TRUE;
}

/**
 * up_history_set_time_empty_data:
 **/
gboolean
up_history_set_time_empty_data (UpHistory *history, gint64 time_s)
{
	UpHistoryObj *obj;

	g_return_val_if_fail (UP_IS_HISTORY (history), FALSE);

	if (history->priv->id == NULL)
		return FALSE;
	if (history->priv->state == UP_DEVICE_STATE_UNKNOWN)
		return FALSE;
	if (time_s < 0)
		return FALSE;
	if (history->priv->time_empty_last == time_s)
		return FALSE;

	/* add to array and schedule save file */
	obj = up_history_obj_create ((gdouble) time_s, history->priv->state);
	g_ptr_array_add (history->priv->data_time_empty, obj);
	up_history_schedule_save (history);

	/* save last value */
	history->priv->time_empty_last = time_s;

	return TRUE;
}

/**
 * up_history_class_init:
 * @klass: The UpHistoryClass
 **/
static void
up_history_class_init (UpHistoryClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = up_history_finalize;
	g_type_class_add_private (klass, sizeof (UpHistoryPrivate));
}

/**
 * up_history_init:
 * @history: This class instance
 **/
static void
up_history_init (UpHistory *history)
{
	history->priv = UP_HISTORY_GET_PRIVATE (history);
	history->priv->id = NULL;
	history->priv->rate_last = 0;
	history->priv->percentage_last = 0;
	history->priv->state = UP_DEVICE_STATE_UNKNOWN;
	history->priv->data_rate = g_ptr_array_new_with_free_func ((GDestroyNotify) up_history_obj_free);
	history->priv->data_charge = g_ptr_array_new_with_free_func ((GDestroyNotify) up_history_obj_free);
	history->priv->data_time_full = g_ptr_array_new_with_free_func ((GDestroyNotify) up_history_obj_free);
	history->priv->data_time_empty = g_ptr_array_new_with_free_func ((GDestroyNotify) up_history_obj_free);
	history->priv->save_id = 0;
}

/**
 * up_history_finalize:
 * @object: The object to finalize
 **/
static void
up_history_finalize (GObject *object)
{
	UpHistory *history;

	g_return_if_fail (UP_IS_HISTORY (object));

	history = UP_HISTORY (object);

	/* save */
	if (history->priv->save_id > 0)
		g_source_remove (history->priv->save_id);
	if (history->priv->id != NULL)
		up_history_save_data (history);

	g_ptr_array_unref (history->priv->data_rate);
	g_ptr_array_unref (history->priv->data_charge);
	g_ptr_array_unref (history->priv->data_time_full);
	g_ptr_array_unref (history->priv->data_time_empty);

	g_free (history->priv->id);

	g_return_if_fail (history->priv != NULL);

	G_OBJECT_CLASS (up_history_parent_class)->finalize (object);
}

/**
 * up_history_new:
 *
 * Return value: a new UpHistory object.
 **/
UpHistory *
up_history_new (void)
{
	UpHistory *history;
	history = g_object_new (UP_TYPE_HISTORY, NULL);
	return UP_HISTORY (history);
}

/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef EGG_TEST
#include "egg-test.h"

void
up_history_test (gpointer user_data)
{
	EggTest *test = (EggTest *) user_data;
	UpHistory *history;

	if (!egg_test_start (test, "UpHistory"))
		return;

	/************************************************************/
	egg_test_title (test, "get instance");
	history = up_history_new ();
	egg_test_assert (test, history != NULL);

	/* unref */
	g_object_unref (history);

	egg_test_end (test);
}
#endif
