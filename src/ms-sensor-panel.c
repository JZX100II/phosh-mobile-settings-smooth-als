/*
 * Copyright (C) 2023 Evangelos Ribeiro Tzaras <devrtz@fortysixandtwo.eu>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "ms-sensor-panel"

#include "mobile-settings-config.h"
#include "ms-sensor-panel.h"
#include "dbus/iio-sensor-proxy-dbus.h"

#include <glib/gi18n.h>


#define IIO_SENSOR_PROXY_DBUS_NAME       "net.hadess.SensorProxy"
#define IIO_SENSOR_PROXY_DBUS_IFACE_NAME "net.hadess.SensorProxy"
#define IIO_SENSOR_PROXY_DBUS_OBJECT     "/net/hadess/SensorProxy"

#define PHOSH_SCHEMA_ID "sm.puri.phosh"
#define PHOSH_KEY_AUTO_HC "automatic-high-contrast"
#define PHOSH_KEY_AUTO_HC_THRESHOLD "automatic-high-contrast-threshold"

#define HIGH_CONTRAST_LOWER_THRESHOLD 0
#define HIGH_CONTRAST_UPPER_THRESHOLD 1500

typedef enum {
  PROXIMITY,
  AMBIENT,
  ACCELEROMETER,
  N_SENSORS
} SensorType;

typedef void (*SensorClaimReleaseFunc) (MsDBusSensorProxy  *proxy,
                                        GCancellable       *cancel,
                                        GAsyncReadyCallback cb,
                                        gpointer            user_data);
typedef gboolean (*SensorClaimReleaseFinishFunc) (MsDBusSensorProxy *proxy,
                                                  GAsyncResult      *res,
                                                  GError           **error);
typedef struct {
  SensorType                   sensor_type;
  const char                  *name;
  const char                  *prop_name; /* used to notify in on_sensor_claimed() */
  gboolean                     active;
  SensorClaimReleaseFunc       claim_func;
  SensorClaimReleaseFinishFunc claim_finish_func;
  SensorClaimReleaseFunc       release_func;
  SensorClaimReleaseFinishFunc release_finish_func;
  GCancellable                *cancel;
  MsSensorPanel               *panel;
  GtkLabel                    *label;
  gulong                       notify_id;
} Sensor;

struct _MsSensorPanel {
  AdwBin             parent;

  guint              bus_watch_id;
  GCancellable      *cancel;
  MsDBusSensorProxy *proxy;

  GtkStack          *stack;
  GtkSpinner        *spinner;

  GSettings         *settings;
  GtkSwitch         *automatic_hc_switch;
  GtkScale          *automatic_hc_scale;
  GtkAdjustment     *automatic_hc_adjustment;

  GtkLabel          *accelerometer_label;
  GtkLabel          *light_label;
  GtkLabel          *proximity_label;


  Sensor             sensors[N_SENSORS];
  guint              n_sensors;
};

G_DEFINE_TYPE (MsSensorPanel, ms_sensor_panel, ADW_TYPE_BIN)


static gboolean
orientation_string_transform (GBinding     *binding,
                              const GValue *from_value,
                              GValue       *to_value,
                              gpointer      user_data)
{
  const char *orientation = g_value_get_string (from_value);

  if (g_strcmp0 (orientation, "normal") == 0)
    g_value_set_string (to_value, _("Normal"));
  else if (g_strcmp0 (orientation, "bottom-up") == 0)
    g_value_set_string (to_value, _("Bottom up"));
  else if (g_strcmp0 (orientation, "left-up") == 0)
    g_value_set_string (to_value, _("Left up"));
  else if (g_strcmp0 (orientation, "right-up") == 0)
    g_value_set_string (to_value, _("Right up"));
  else
    g_value_set_string (to_value, _("Undefined"));

  return TRUE;
}

static gdouble als_accumulator = 0.0f;
static gdouble alpha = 0.5f;

typedef struct {
  GBinding     *binding,
  gdouble       from_value,
  GValue       *to_value,
  gpointer      user_data
} LightLevelData;

gboolean light_level_smoothing (gpointer user_data)
{
  LightLevelData *data = (LightLevelData *) user_data;

  gdouble current_als_value = data->from_value;

  MsDBusSensorProxy *proxy = MS_DBUS_SENSOR_PROXY (data->user_data);
  const char* light_unit = ms_dbus_sensor_proxy_get_light_level_unit (proxy); 

  if (g_strcmp0 (light_unit, "vendor") == 0)
      light_unit = "%";

  als_accumulator = (alpha * current_als_value) + (1.0 - alpha) * als_accumulator;

   g_value_take_string (data->to_value,
                        g_strdup_printf ("%.1f %s",
                                         als_accumulator,
                                         light_unit ?: ""));

  return TRUE;
}

static gboolean
light_level_to_string_transform (GBinding     *binding,
                                 const GValue *from_value,
                                 GValue       *to_value,
                                 gpointer      user_data)
{
  gdouble current_als_value =  g_value_get_double (from_value);

  LightLevelData *data = g_new (LightLevelData, 1);

  data->binding = binding;
  data->from_value = g_value_get_string(from_value);
  data->to_value = g_new0 (GValue, 1);
  g_value_init(data->to_value, G_TYPE_STRING);
  data->user_data = user_data;

  g_idle_add (light_level_smoothing, data);

  g_value_take_string (to_value, g_value_get_string (data->to_value));

  return TRUE;
}


static gboolean
proximity_near_to_string_transform (GBinding     *binding,
                                    const GValue *from_value,
                                    GValue       *to_value,
                                    gpointer      unused)
{
  g_value_set_string (to_value,
                      g_value_get_boolean (from_value) ?
                      _("Near") : _("Far"));

  return TRUE;
}


static void
update_active_sensors (Sensor *sensor)
{
  guint n_active = 0;
  gboolean done;

  g_assert (sensor);
  g_assert (MS_IS_SENSOR_PANEL (sensor->panel));

  for (guint i = 0; i < N_SENSORS; i++) {
    if (sensor->panel->sensors[i].active)
      n_active++;
  }

  g_debug ("Sensors: %u Active: %u", sensor->panel->n_sensors, n_active);

  done = (sensor->panel->n_sensors == n_active);
  g_debug ("Setting spinner to %s", done ? "stop" : "spin");
  gtk_spinner_set_spinning (sensor->panel->spinner, !done);
}


static void
on_notify_sensor_prop (Sensor *sensor)
{
  g_signal_handler_disconnect (sensor->panel->proxy, sensor->notify_id);
  sensor->notify_id = 0;
  sensor->active = TRUE;

  update_active_sensors (sensor);
}


static void
on_sensor_claimed (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  Sensor *sensor = user_data;
  MsDBusSensorProxy *proxy = MS_DBUS_SENSOR_PROXY (source_object);
  gboolean ok;

  ok = sensor->claim_finish_func (proxy, res, &error);
  g_debug ("%s sensor claimed %ssuccesfully", sensor->name, ok ? "" : "un");

  if (!ok) {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug ("Cancelled claming %s sensor", sensor->name);
      return; /* We only cancel on shutdown */
    } else {
      g_warning ("Failed claiming %s sensor: %s", sensor->name, error->message);
    }
    sensor->panel->n_sensors--;
  }

  if (ok) {
    g_autofree char *notify = g_strdup_printf ("notify::%s", sensor->prop_name);

    sensor->notify_id = g_signal_connect_swapped (source_object,
                                                  notify,
                                                  G_CALLBACK (on_notify_sensor_prop),
                                                  sensor);

    g_object_notify (source_object, sensor->prop_name); /* SHOULD ensure the bound label updates */
  }

  update_active_sensors (sensor);
}


static void
on_sensor_released (GObject      *source_object,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  Sensor *sensor = user_data;
  MsDBusSensorProxy *proxy = MS_DBUS_SENSOR_PROXY (source_object);
  gboolean ok;

  ok = sensor->release_finish_func (proxy, res, &error);
  g_debug ("%s sensor released %ssucessfully", sensor->name, ok ? "" : "un");

  if (!ok) {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug ("Cancelled releasing %s sensor", sensor->name);
      return; /* We only cancel on shutdown */
    } else {
      g_warning ("Failed releasing %s sensor: %s", sensor->name, error->message);
    }
  }
}


static void
on_notify_sensor_available (GObject    *object,
                            GParamSpec *pspec,
                            gpointer    user_data)
{
  Sensor *sensor = user_data;
  gboolean has_sensor;
  g_autofree char *prop_name = g_strdup_printf ("has-%s", sensor->name);

  g_object_get (object, prop_name, &has_sensor, NULL);

  g_debug ("%s sensor %savailable", sensor->name, has_sensor ? "" : "un");

  if (has_sensor) {
    sensor->claim_func (MS_DBUS_SENSOR_PROXY (object), sensor->cancel, on_sensor_claimed, sensor);
    sensor->panel->n_sensors++;
    gtk_stack_set_visible_child_name (sensor->panel->stack, "have-sensors");
    gtk_label_set_label (sensor->label, _("Updating…"));
    update_active_sensors (sensor);
  }  else {
    sensor->active = FALSE;
  }
}


static void
update_panel_sensors (MsSensorPanel *self)
{
  g_assert (MS_IS_SENSOR_PANEL (self));

  self->n_sensors = 0;
  gtk_stack_set_visible_child_name (self->stack, "no-sensors");

  gtk_label_set_label (self->proximity_label, _("Not available"));
  gtk_label_set_label (self->light_label, _("Not available"));
  gtk_label_set_label (self->accelerometer_label, _("Not available"));

  if (!self->proxy)
    return;

  if (gtk_widget_get_mapped (GTK_WIDGET (self))) {
    for (guint i = 0; i < N_SENSORS; i++) {
      Sensor *sensor = &self->sensors[i];
      g_autofree char *notify_prop = g_strdup_printf ("notify::has-%s", sensor->name);

      g_signal_connect (self->proxy,
                        notify_prop,
                        G_CALLBACK (on_notify_sensor_available),
                        sensor);
      on_notify_sensor_available (G_OBJECT (self->proxy), NULL, sensor);
    }
  } else {
    for (guint i = 0; i < N_SENSORS; i++) {
      Sensor *sensor = &self->sensors[i];

      g_signal_handlers_disconnect_by_data (self->proxy, sensor);
      if (sensor->active) {
        sensor->release_func (self->proxy, self->cancel, on_sensor_released, sensor);
        /* we don't particularly care whether releasing is successful, so just set it to inactive */
        sensor->active = FALSE;
      }
    }
  }
}


static void
on_new_proxy (GObject      *source_object,
              GAsyncResult *res,
              gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  MsDBusSensorProxy *proxy = ms_dbus_sensor_proxy_proxy_new_finish (res, &error);
  MsSensorPanel *self = user_data;

  if (!proxy) {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug ("Cancelled creating new proxy");
    else
      g_warning ("Error creating new proxy %s", error->message);
    return;
  }

  g_debug ("Got sensor proxy");

  g_assert (MS_IS_SENSOR_PANEL (self));
  self->proxy = proxy;

  update_panel_sensors (self);

  g_object_bind_property_full (self->proxy, "proximity-near",
                               self->proximity_label, "label",
                               G_BINDING_DEFAULT,
                               proximity_near_to_string_transform,
                               NULL, NULL, NULL);

  g_object_bind_property_full (self->proxy, "light-level",
                               self->light_label, "label",
                               G_BINDING_DEFAULT,
                               light_level_to_string_transform,
                               NULL,
                               self->proxy,
                               NULL);

  g_object_bind_property_full (self->proxy, "accelerometer-orientation",
                               self->accelerometer_label, "label",
                               G_BINDING_DEFAULT,
                               orientation_string_transform,
                               NULL, NULL, NULL);

  g_object_bind_property (self->proxy, "has-ambient-light",
                          self->automatic_hc_switch, "sensitive",
                          G_BINDING_SYNC_CREATE);

  g_object_bind_property (self->proxy, "has-ambient-light",
                          self->automatic_hc_scale, "sensitive",
                          G_BINDING_SYNC_CREATE);
}


static void
on_iio_sensor_proxy_appeared (GDBusConnection *conn,
                              const char      *name,
                              const char      *name_owner,
                              gpointer         user_data)
{
  MsSensorPanel *self = user_data;

  g_debug ("Sensor proxy appeared");

  self->n_sensors = 0;

  ms_dbus_sensor_proxy_proxy_new (conn,
                                  G_DBUS_PROXY_FLAGS_NONE,
                                  name,
                                  IIO_SENSOR_PROXY_DBUS_OBJECT,
                                  NULL,
                                  on_new_proxy,
                                  self);
}


static void
on_iio_sensor_proxy_vanished (GDBusConnection *conn,
                              const char      *name,
                              gpointer         user_data)
{
  MsSensorPanel *self = user_data;

  g_debug ("Sensor proxy vanished");
  g_clear_object (&self->proxy);

  self->sensors[PROXIMITY].active = FALSE;
  self->sensors[AMBIENT].active = FALSE;
  self->sensors[ACCELEROMETER].active = FALSE;

  gtk_widget_set_sensitive (GTK_WIDGET (self->automatic_hc_switch), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->automatic_hc_scale), FALSE);

  update_panel_sensors (self);
}


static void
ms_sensor_panel_finalize (GObject *object)
{
  MsSensorPanel *self = MS_SENSOR_PANEL (object);

  g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  g_clear_object (&self->proxy);
  g_clear_object (&self->settings);
  g_clear_handle_id (&self->bus_watch_id, g_bus_unwatch_name);

  G_OBJECT_CLASS (ms_sensor_panel_parent_class)->finalize (object);
}


static void
ms_sensor_panel_class_init (MsSensorPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = ms_sensor_panel_finalize;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/mobi/phosh/MobileSettings/ui/ms-sensor-panel.ui");

  gtk_widget_class_bind_template_child (widget_class, MsSensorPanel, accelerometer_label);
  gtk_widget_class_bind_template_child (widget_class, MsSensorPanel, light_label);
  gtk_widget_class_bind_template_child (widget_class, MsSensorPanel, proximity_label);
  gtk_widget_class_bind_template_child (widget_class, MsSensorPanel, automatic_hc_switch);
  gtk_widget_class_bind_template_child (widget_class, MsSensorPanel, automatic_hc_scale);
  gtk_widget_class_bind_template_child (widget_class, MsSensorPanel, stack);
  gtk_widget_class_bind_template_child (widget_class, MsSensorPanel, spinner);
}


static void
ms_sensor_panel_init (MsSensorPanel *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->cancel = g_cancellable_new ();

  self->sensors[PROXIMITY].name = "proximity";
  self->sensors[PROXIMITY].prop_name = "proximity-near";
  self->sensors[PROXIMITY].claim_func = ms_dbus_sensor_proxy_call_claim_proximity;
  self->sensors[PROXIMITY].claim_finish_func = ms_dbus_sensor_proxy_call_claim_proximity_finish;
  self->sensors[PROXIMITY].release_func = ms_dbus_sensor_proxy_call_claim_proximity;
  self->sensors[PROXIMITY].release_finish_func = ms_dbus_sensor_proxy_call_release_proximity_finish;
  self->sensors[PROXIMITY].cancel = self->cancel;
  self->sensors[PROXIMITY].label = self->proximity_label;
  self->sensors[PROXIMITY].panel = self;

  self->sensors[AMBIENT].name = "ambient-light";
  self->sensors[AMBIENT].prop_name = "light-level";
  self->sensors[AMBIENT].claim_func = ms_dbus_sensor_proxy_call_claim_light;
  self->sensors[AMBIENT].claim_finish_func = ms_dbus_sensor_proxy_call_claim_light_finish;
  self->sensors[AMBIENT].release_func = ms_dbus_sensor_proxy_call_claim_light;
  self->sensors[AMBIENT].release_finish_func = ms_dbus_sensor_proxy_call_release_light_finish;
  self->sensors[AMBIENT].cancel = self->cancel;
  self->sensors[AMBIENT].label = self->light_label;
  self->sensors[AMBIENT].panel = self;

  self->sensors[ACCELEROMETER].name = "accelerometer";
  self->sensors[ACCELEROMETER].prop_name = "accelerometer-orientation";
  self->sensors[ACCELEROMETER].claim_func = ms_dbus_sensor_proxy_call_claim_accelerometer;
  self->sensors[ACCELEROMETER].claim_finish_func = ms_dbus_sensor_proxy_call_claim_accelerometer_finish;
  self->sensors[ACCELEROMETER].release_func = ms_dbus_sensor_proxy_call_claim_accelerometer;
  self->sensors[ACCELEROMETER].release_finish_func = ms_dbus_sensor_proxy_call_release_accelerometer_finish;
  self->sensors[ACCELEROMETER].cancel = self->cancel;
  self->sensors[ACCELEROMETER].label = self->accelerometer_label;
  self->sensors[ACCELEROMETER].panel = self;

  self->bus_watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                                         IIO_SENSOR_PROXY_DBUS_NAME,
                                         G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                         on_iio_sensor_proxy_appeared,
                                         on_iio_sensor_proxy_vanished,
                                         self,
                                         NULL);

  g_signal_connect (self, "map", G_CALLBACK (update_panel_sensors), NULL);
  g_signal_connect (self, "unmap", G_CALLBACK (update_panel_sensors), NULL);

  self->settings = g_settings_new (PHOSH_SCHEMA_ID);
  g_settings_bind (self->settings, PHOSH_KEY_AUTO_HC,
                   self->automatic_hc_switch, "active",
                   G_SETTINGS_BIND_DEFAULT);

  self->automatic_hc_adjustment = gtk_range_get_adjustment (GTK_RANGE (self->automatic_hc_scale));
  gtk_adjustment_set_lower (self->automatic_hc_adjustment, HIGH_CONTRAST_LOWER_THRESHOLD);
  gtk_adjustment_set_upper (self->automatic_hc_adjustment, HIGH_CONTRAST_UPPER_THRESHOLD);
  g_settings_bind (self->settings, PHOSH_KEY_AUTO_HC_THRESHOLD,
                   self->automatic_hc_adjustment, "value",
                   G_SETTINGS_BIND_DEFAULT);
}
