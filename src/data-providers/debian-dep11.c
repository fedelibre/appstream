/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*-
 *
 * Copyright (C) 2012-2014 Matthias Klumpp <matthias@tenstral.net>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the license, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "debian-dep11.h"

#include <glib.h>
#include <glib-object.h>
#include <yaml.h>

#include "../as-utils.h"

static gpointer as_provider_dep11_parent_class = NULL;

enum YamlNodeKind {
	YAML_VAR,
	YAML_VAL,
	YAML_SEQ
};

/**
 * as_provider_dep11_construct:
 */
AsProviderDEP11*
as_provider_dep11_construct (GType object_type)
{
	AsProviderDEP11 * self = NULL;
	self = (AsProviderDEP11*) as_data_provider_construct (object_type);
	return self;
}

/**
 * as_provider_dep11_new:
 */
AsProviderDEP11*
as_provider_dep11_new (void) {
	return as_provider_dep11_construct (AS_PROVIDER_TYPE_DEP11);
}

/**
 * dep11_yaml_process_layer:
 *
 * Create GNode tree from DEP-11 YAML document
 */
static void
dep11_yaml_process_layer (yaml_parser_t *parser, GNode *data)
{
	GNode *last_leaf = data;
	yaml_event_t event;
	gboolean parse = TRUE;
	int storage = YAML_VAR; /* the first element must always be of type VAR */

    while (parse) {
    	yaml_parser_parse (parser, &event);

		/* Parse value either as a new leaf in the mapping
		 * or as a leaf value (one of them, in case it's a sequence) */
		switch (event.type) {
			case YAML_SCALAR_EVENT:
				if (storage)
					g_node_append_data(last_leaf, g_strdup((gchar*) event.data.scalar.value));
				else
					last_leaf = g_node_append(data, g_node_new(g_strdup((gchar*) event.data.scalar.value)));
				storage ^= YAML_VAL;
				break;
			case YAML_SEQUENCE_START_EVENT:
				storage = YAML_SEQ;
				break;
			case YAML_SEQUENCE_END_EVENT:
				storage = YAML_VAR;
				break;
			case YAML_MAPPING_START_EVENT:
				/* depth += 1 */
				dep11_yaml_process_layer (parser, last_leaf);
				storage ^= YAML_VAL; /* Flip VAR/VAL, without touching SEQ */
				break;
			case YAML_MAPPING_END_EVENT:
			case YAML_STREAM_END_EVENT:
			case YAML_DOCUMENT_END_EVENT:
				/* depth -= 1 */
				parse = FALSE;
				break;
			default:
				break;
		}
    	yaml_event_delete(&event);
    }
}

/**
 * dep11_get_localized_node:
 */
static GNode*
dep11_get_localized_node (GNode *node, const gchar *locale)
{
	GNode *n;
	GNode *tnode = NULL;
	gchar *key;
	gchar *locale_short = NULL;

	if (locale != NULL) {
		gchar **strv;
		strv = g_strsplit (locale, "_", 0);
		locale_short = g_strdup (strv[0]);
		g_strfreev (strv);
	}

	for (n = node->children; n != NULL; n = n->next) {
			key = (gchar*) n->data;

			if ((tnode == NULL) && (g_strcmp0 (key, "C") == 0)) {
				tnode = n;
				if (locale == NULL)
					goto out;
			}

			if (g_strcmp0 (key, locale) == 0) {
				tnode = n;
				goto out;
			}

			if (g_strcmp0 (key, locale_short) == 0) {
				tnode = n;
				goto out;
			}
	}

out:
	g_free (locale_short);
	return tnode;
}

/**
 * dep11_get_localized_value:
 *
 * Get localized string from a translated DEP-11 key
 */
static gchar*
dep11_get_localized_value (GNode *node, const gchar *locale)
{
	GNode *tnode;

	tnode = dep11_get_localized_node (node, locale);
	if (tnode == NULL)
		return NULL;

	return g_strdup ((gchar*) tnode->children->data);
}

/**
 * dep11_list_to_string_array:
 */
static void
dep11_list_to_string_array (GNode *node, GPtrArray *array)
{
	GNode *n;

	for (n = node->children; n != NULL; n = n->next) {
		g_ptr_array_add (array, g_strdup ((gchar*) n->data));
	}
}

/**
 * dep11_list_to_string_array:
 *
 * Process a keywords node and add the data to an #AsComponent
 */
static void
dep11_process_keywords (GNode *node, AsComponent *cpt, const gchar *locale)
{
	GNode *tnode;
	GPtrArray *keywords;
	gchar **strv;

	keywords = g_ptr_array_new_with_free_func (g_free);

	tnode = dep11_get_localized_node (node, locale);
	/* no node found? */
	if (tnode == NULL)
		return;

	dep11_list_to_string_array (tnode, keywords);

	strv = as_ptr_array_to_strv (keywords);
	as_component_set_keywords (cpt, strv);
	g_ptr_array_unref (keywords);
	g_strfreev (strv);
}

/**
 * as_provider_dep11_process_component_doc:
 */
AsComponent*
as_provider_dep11_process_component_node (AsProviderDEP11 *dproc, GNode *root, const gchar *locale, const gchar *origin)
{
	GNode *node;
	AsComponent *cpt;

	gchar **strv;
	GPtrArray *pkgnames;
	GPtrArray *categories;
	GPtrArray *compulsory_for_desktops;

	cpt = as_component_new ();

	pkgnames = g_ptr_array_new_with_free_func (g_free);
	categories = g_ptr_array_new_with_free_func (g_free);
	compulsory_for_desktops = g_ptr_array_new_with_free_func (g_free);

	for (node = root->children; node != NULL; node = node->next) {
		gchar *key;
		gchar *value;
		gchar *lvalue;

		key = (gchar*) node->data;
		value = (gchar*) node->children->data;

		if (g_strcmp0 (key, "Type") == 0) {
			if (g_strcmp0 (value, "desktop-app") == 0)
				as_component_set_kind (cpt, AS_COMPONENT_KIND_DESKTOP_APP);
			else if (g_strcmp0 (value, "generic") == 0)
				as_component_set_kind (cpt, AS_COMPONENT_KIND_GENERIC);
			else
				as_component_set_kind (cpt, as_component_kind_from_string (value));
		} else if (g_strcmp0 (key, "ID") == 0) {
			as_component_set_id (cpt, value);
		} else if (g_strcmp0 (key, "Packages") == 0) {
			dep11_list_to_string_array (node, pkgnames);
		} else if (g_strcmp0 (key, "Name") == 0) {
			lvalue = dep11_get_localized_value (node, NULL);
			if (lvalue != NULL) {
				as_component_set_name_original (cpt, lvalue);
				g_free (lvalue);
			}
			lvalue = dep11_get_localized_value (node, locale);
			as_component_set_name (cpt, lvalue);
			g_free (lvalue);
		} else if (g_strcmp0 (key, "Summary") == 0) {
			lvalue = dep11_get_localized_value (node, locale);
			as_component_set_summary (cpt, lvalue);
			g_free (lvalue);
		} else if (g_strcmp0 (key, "Description") == 0) {
			lvalue = dep11_get_localized_value (node, locale);
			as_component_set_description (cpt, lvalue);
			g_free (lvalue);
		} else if (g_strcmp0 (key, "DeveloperName") == 0) {
			lvalue = dep11_get_localized_value (node, locale);
			as_component_set_developer_name (cpt, lvalue);
			g_free (lvalue);
		} else if (g_strcmp0 (key, "ProjectLicense") == 0) {
			as_component_set_project_license (cpt, value);
		} else if (g_strcmp0 (key, "ProjectGroup") == 0) {
			as_component_set_project_group (cpt, value);
		} else if (g_strcmp0 (key, "Categories") == 0) {
			dep11_list_to_string_array (node, categories);
		} else if (g_strcmp0 (key, "CompulsoryForDesktops") == 0) {
			dep11_list_to_string_array (node, compulsory_for_desktops);
		} else if (g_strcmp0 (key, "Extends") == 0) {
			dep11_list_to_string_array (node, as_component_get_extends (cpt));
		} else if (g_strcmp0 (key, "Keywords") == 0) {
			dep11_process_keywords (node, cpt, locale);
		} else {
			//printf("%s: %s\n", key, value);
		}
	}

	/* add package name information to component */
	strv = as_ptr_array_to_strv (pkgnames);
	as_component_set_pkgnames (cpt, strv);
	g_ptr_array_unref (pkgnames);
	g_strfreev (strv);

	/* add category information to component */
	strv = as_ptr_array_to_strv (categories);
	as_component_set_categories (cpt, strv);
	g_ptr_array_unref (categories);
	g_strfreev (strv);

	/* add desktop-compulsority information to component */
	strv = as_ptr_array_to_strv (compulsory_for_desktops);
	as_component_set_compulsory_for_desktops (cpt, strv);
	g_ptr_array_unref (compulsory_for_desktops);
	g_strfreev (strv);

	return cpt;
}

/**
 * as_provider_dep11_process_data:
 */
gboolean
as_provider_dep11_process_data (AsProviderDEP11 *dprov, const gchar *data)
{
	yaml_parser_t parser;
	yaml_event_t event;
	gboolean ret;
	gboolean header = TRUE;
	gboolean parse = TRUE;
	gchar *origin = NULL;
	const gchar *locale;

    yaml_parser_initialize (&parser);
    yaml_parser_set_input_string (&parser, (unsigned char*) data, strlen(data));

	locale = as_data_provider_get_locale (AS_DATA_PROVIDER (dprov));
	ret = TRUE;

	while (parse) {
    	yaml_parser_parse(&parser, &event);
		if (event.type == YAML_DOCUMENT_START_EVENT) {
			GNode *n;
			gchar *key;
			gchar *value;
			AsComponent *cpt;
			GNode *root = g_node_new("");

			dep11_yaml_process_layer (&parser, root);

			if (header) {
				for (n = root->children; n != NULL; n = n->next) {
					key = (gchar*) n->data;
					value = (gchar*) n->children->data;
					if (g_strcmp0 (key, "File") == 0) {
						if (g_strcmp0 (value, "DEP-11") != 0) {
							ret = FALSE;
							g_warning ("Invalid DEP-11 file found: Header invalid");
						}
					} else if (g_strcmp0 (key, "Origin") == 0) {
						if ((value != NULL) && (origin == NULL)) {
							origin = g_strdup (value);
						} else {
							ret = FALSE;
							g_warning ("Invalid DEP-11 file found: No origin set in header.");
						}
					}
				}
			} else {
				cpt = as_provider_dep11_process_component_node (dprov, root, locale, origin);
				if (cpt == NULL)
					parse = FALSE;

				if (as_component_is_valid (cpt)) {
					/* everything is fine with this component, we can emit it */
					as_data_provider_emit_component (AS_DATA_PROVIDER (dprov), cpt);
				} else {
					gchar *str;
					gchar *str2;
					str = as_component_to_string (cpt);
					str2 = g_strdup_printf ("Invalid component found: %s\n", str);
					as_data_provider_log_warning (AS_DATA_PROVIDER (dprov), str2);
					g_free (str);
					g_free (str2);
				}
				g_debug ("%s", as_component_to_string (cpt));
				g_object_unref (cpt);
			}

			header = FALSE;
			g_node_destroy(root);
		}

		/* stop if end of stream is reached */
		if (event.type == YAML_STREAM_END_EVENT)
			parse = FALSE;

		/* we don't continue on error */
		if (!ret)
			parse = FALSE;

		yaml_event_delete(&event);
	}

    yaml_parser_delete (&parser);
	g_free (origin);

	return ret;
}

/**
 * as_provider_dep11_process_compressed_file:
 */
gboolean
as_provider_dep11_process_compressed_file (AsProviderDEP11 *dprov, GFile *infile)
{
	GFileInputStream* src_stream;
	GMemoryOutputStream* mem_os;
	GInputStream *conv_stream;
	GZlibDecompressor* zdecomp;
	guint8* data;
	gboolean ret;

	g_return_val_if_fail (dprov != NULL, FALSE);
	g_return_val_if_fail (infile != NULL, FALSE);

	src_stream = g_file_read (infile, NULL, NULL);
	mem_os = (GMemoryOutputStream*) g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
	zdecomp = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
	conv_stream = g_converter_input_stream_new (G_INPUT_STREAM (src_stream), G_CONVERTER (zdecomp));
	g_object_unref (zdecomp);

	g_output_stream_splice ((GOutputStream*) mem_os, conv_stream, 0, NULL, NULL);
	data = g_memory_output_stream_get_data (mem_os);
	ret = as_provider_dep11_process_data (dprov, (const gchar*) data);

	g_object_unref (conv_stream);
	g_object_unref (mem_os);
	g_object_unref (src_stream);

	return ret;
}

/**
 * as_provider_dep11_process_file:
 */
gboolean
as_provider_dep11_process_file (AsProviderDEP11 *dprov, GFile *infile)
{
	gboolean ret;
	gchar *yml_data;
	gchar *line = NULL;
	GFileInputStream* ir;
	GDataInputStream* dis;
	GString *str = NULL;

	g_return_val_if_fail (infile != NULL, FALSE);

	ir = g_file_read (infile, NULL, NULL);
	dis = g_data_input_stream_new ((GInputStream*) ir);
	g_object_unref (ir);

	str = g_string_new ("");
	while (TRUE) {
		line = g_data_input_stream_read_line (dis, NULL, NULL, NULL);
		if (line == NULL) {
			break;
		}

		if (str->len > 0)
			g_string_append (str, "\n");
		g_string_append_printf (str, "%s\n", line);
		g_free (line);
	}
	yml_data = g_string_free (str, FALSE);

	ret = as_provider_dep11_process_data (dprov, yml_data);
	g_object_unref (dis);
	g_free (yml_data);

	return ret;
}

/**
 * as_provider_dep11_real_execute:
 */
static gboolean
as_provider_dep11_real_execute (AsDataProvider* base)
{
	AsProviderDEP11 *dprov;
	GPtrArray* yaml_files;
	guint i;
	GFile *infile;
	gchar **paths;
	gboolean ret = TRUE;
	const gchar *content_type;

	dprov = AS_PROVIDER_DEP11 (base);
	yaml_files = g_ptr_array_new_with_free_func (g_free);

	paths = as_data_provider_get_watch_files (base);
	if (paths == NULL)
		return TRUE;
	for (i = 0; paths[i] != NULL; i++) {
		gchar *path;
		GPtrArray *yamls;
		guint j;
		path = paths[i];

		if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
			continue;
		}

		yamls = as_utils_find_files_matching (path, "*.yml*", FALSE);
		if (yamls == NULL)
			continue;
		for (j = 0; j < yamls->len; j++) {
			const gchar *val;
			val = (const gchar *) g_ptr_array_index (yamls, j);
			g_ptr_array_add (yaml_files, g_strdup (val));
		}

		g_ptr_array_unref (yamls);
	}

	if (yaml_files->len == 0) {
		g_ptr_array_unref (yaml_files);
		return FALSE;
	}

	ret = TRUE;
	for (i = 0; i < yaml_files->len; i++) {
		gchar *fname;
		GFileInfo *info = NULL;
		fname = (gchar*) g_ptr_array_index (yaml_files, i);
		infile = g_file_new_for_path (fname);
		if (!g_file_query_exists (infile, NULL)) {
			g_warning ("File '%s' does not exist.", fname);
			g_object_unref (infile);
			continue;
		}

		info = g_file_query_info (infile,
				G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
				G_FILE_QUERY_INFO_NONE,
				NULL, NULL);
		if (info == NULL) {
			g_debug ("No info for file '%s' found, file was skipped.", fname);
			g_object_unref (infile);
			continue;
		}
		content_type = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
		if (g_strcmp0 (content_type, "application/x-yaml") == 0) {
			ret = as_provider_dep11_process_file (dprov, infile);
		} else if (g_strcmp0 (content_type, "application/gzip") == 0 ||
				g_strcmp0 (content_type, "application/x-gzip") == 0) {
			ret = as_provider_dep11_process_compressed_file (dprov, infile);
		} else {
			g_warning ("Invalid file of type '%s' found. File '%s' was skipped.", content_type, fname);
		}
		g_object_unref (info);
		g_object_unref (infile);

		if (!ret)
			break;
	}
	g_ptr_array_unref (yaml_files);

	return ret;
}

static void
as_provider_dep11_class_init (AsProviderDEP11Class * klass)
{
	as_provider_dep11_parent_class = g_type_class_peek_parent (klass);
	AS_DATA_PROVIDER_CLASS (klass)->execute = as_provider_dep11_real_execute;
}

static void
as_provider_dep11_instance_init (AsProviderDEP11 *dprov)
{
}

/**
 * as_provider_dep11_get_type:
 */
GType
as_provider_dep11_get_type (void)
{
	static volatile gsize as_provider_dep11_type_id__volatile = 0;
	if (g_once_init_enter (&as_provider_dep11_type_id__volatile)) {
		static const GTypeInfo g_define_type_info = {
					sizeof (AsProviderDEP11Class),
					(GBaseInitFunc) NULL,
					(GBaseFinalizeFunc) NULL,
					(GClassInitFunc) as_provider_dep11_class_init,
					(GClassFinalizeFunc) NULL,
					NULL,
					sizeof (AsProviderDEP11),
					0,
					(GInstanceInitFunc) as_provider_dep11_instance_init,
					NULL
		};
		GType as_provider_dep11_type_id;
		as_provider_dep11_type_id = g_type_register_static (AS_TYPE_DATA_PROVIDER, "AsProviderDEP11", &g_define_type_info, 0);
		g_once_init_leave (&as_provider_dep11_type_id__volatile, as_provider_dep11_type_id);
	}
	return as_provider_dep11_type_id__volatile;
}
