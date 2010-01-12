/* dkp-acpi-native.h generated by valac, the Vala compiler, do not modify */


#ifndef __DKP_ACPI_NATIVE_H__
#define __DKP_ACPI_NATIVE_H__

#include <glib.h>
#include <glib-object.h>
#include <stdlib.h>
#include <string.h>

G_BEGIN_DECLS


#define TYPE_DKP_ACPI_NATIVE (dkp_acpi_native_get_type ())
#define DKP_ACPI_NATIVE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), TYPE_DKP_ACPI_NATIVE, DkpAcpiNative))
#define DKP_ACPI_NATIVE_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), TYPE_DKP_ACPI_NATIVE, DkpAcpiNativeClass))
#define IS_DKP_ACPI_NATIVE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), TYPE_DKP_ACPI_NATIVE))
#define IS_DKP_ACPI_NATIVE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), TYPE_DKP_ACPI_NATIVE))
#define DKP_ACPI_NATIVE_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), TYPE_DKP_ACPI_NATIVE, DkpAcpiNativeClass))

typedef struct _DkpAcpiNative DkpAcpiNative;
typedef struct _DkpAcpiNativeClass DkpAcpiNativeClass;
typedef struct _DkpAcpiNativePrivate DkpAcpiNativePrivate;

struct _DkpAcpiNative {
	GObject parent_instance;
	DkpAcpiNativePrivate * priv;
};

struct _DkpAcpiNativeClass {
	GObjectClass parent_class;
};


GType dkp_acpi_native_get_type (void);
DkpAcpiNative* dkp_acpi_native_new (const char* path);
DkpAcpiNative* dkp_acpi_native_construct (GType object_type, const char* path);
DkpAcpiNative* dkp_acpi_native_new_driver_unit (const char* driver, gint unit);
DkpAcpiNative* dkp_acpi_native_construct_driver_unit (GType object_type, const char* driver, gint unit);
const char* dkp_acpi_native_get_driver (DkpAcpiNative* self);
gint dkp_acpi_native_get_unit (DkpAcpiNative* self);
const char* dkp_acpi_native_get_path (DkpAcpiNative* self);


G_END_DECLS

#endif