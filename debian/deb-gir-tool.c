/*
 * Copyright 2025 Simon McVittie
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "deb-gi-tool"

#include <locale.h>
#include <stdio.h>

#include <glib.h>
#include <gio/gio.h>

/*
 * @error: (out) (transfer full): Set to the same error that is in @src
 * @src: (inout) (transfer full): Error to move to @error
 * Returns: %FALSE
 */
static gboolean
throw_take_error (GError **error,
                  GError **src)
{
  g_propagate_error (error, g_steal_pointer (src));
  return FALSE;
}

/*
 * @error: (out) (transfer full): Set to the same error that is in @src
 * @src: (inout) (transfer full): Error to move to @dest
 * Returns: %FALSE
 */
static gboolean
throw_prefix_markup_position (GError **error,
                              const char *path,
                              GMarkupParseContext *context)
{
  int line, column;

  g_markup_parse_context_get_position (context, &line, &column);
  g_prefix_error (error, "\n%s:%d:%d: ", path, line, column);
  return FALSE;
}

/*
 * A GCopyFunc that does the same thing as g_strdup()
 */
static void *
strdup_copy_func (const void *source,
                  G_GNUC_UNUSED void *user_data)
{
  return g_strdup (source);
}

/*
 * @upstream_paths: If %TRUE, return the full search path.
 *  If %FALSE, only search locations appropriate for .deb packages.
 *
 * Returns: (element-type filename): The directories searched for .gir files
 */
static GPtrArray *
get_search_path (gboolean upstream_paths)
{
  g_autoptr(GPtrArray) search_path = g_ptr_array_new_with_free_func (g_free);
  const char *deb_host_multiarch = g_getenv ("DEB_HOST_MULTIARCH");
  const char *const standard_system_dirs[] = { "/usr/share", NULL };
  const char *const *system_dirs;
  const char *env;
  const char *user_dir;
  size_t i;

#ifdef DEB_HOST_MULTIARCH
  if (deb_host_multiarch == NULL)
    deb_host_multiarch = DEB_HOST_MULTIARCH;
#endif

  if (upstream_paths)
    {
      g_debug ("Searching all locations used by upstream tools");
      env = g_getenv ("GI_GIR_PATH");
    }
  else
    {
      g_debug ("Only searching locations appropriate for a .deb file");
      env = NULL;
    }

  if (env != NULL)
    {
      g_auto(GStrv) entries = g_strsplit (env, G_SEARCHPATH_SEPARATOR_S, -1);

      for (i = 0; entries[i] != NULL; i++)
        {
          g_debug ("Search path entry from $GI_GIR_PATH: %s", entries[i]);
          g_ptr_array_add (search_path, g_strdup (entries[i]));
        }
    }

  if (upstream_paths)
    user_dir = g_get_user_data_dir ();
  else
    user_dir = NULL;

  if (user_dir != NULL)
    {
      g_autofree char *path = g_build_filename (user_dir, "gir-1.0", NULL);

      g_debug ("Search path entry from user data dir: %s", path);
      g_ptr_array_add (search_path, g_steal_pointer (&path));
    }

  if (upstream_paths)
    system_dirs = g_get_system_data_dirs ();
  else
    system_dirs = standard_system_dirs;

  for (i = 0; system_dirs[i] != NULL; i++)
    {
      g_autofree char *path = g_build_filename (system_dirs[i], "gir-1.0", NULL);

      g_debug ("Search path entry from system data dirs: %s", path);
      g_ptr_array_add (search_path, g_steal_pointer (&path));
    }

  if (deb_host_multiarch != NULL)
    {
      g_autofree char *path = g_build_filename ("/usr", "lib", deb_host_multiarch, "gir-1.0", NULL);

      g_debug ("Debian multiarch search path entry: %s", path);
      g_ptr_array_add (search_path, g_steal_pointer (&path));
    }

  g_debug ("Hard-coded last resort: /usr/share/gir-1.0");
  g_ptr_array_add (search_path, g_strdup ("/usr/share/gir-1.0"));

  return g_steal_pointer (&search_path);
}

typedef struct
{
  const char *ns_and_ver;
  GHashTable *seen;
  GPtrArray *todo;
} DependsParserData;

/* This is a fairly simple parser: it doesn't validate the file or care
 * about the nesting/structure, it just looks for <include> elements
 * with name and version attributes. */
static void
depends_parser_start_element (GMarkupParseContext *context,
                              const char *name,
                              const char **attributes,
                              const char **values,
                              void *user_data,
                              GError **error)
{
  DependsParserData *data = user_data;
  g_autofree char *ns_and_ver = NULL;
  size_t i;
  const char *ns = NULL;
  const char *version = NULL;

  if (!g_str_equal (name, "include"))
    return;

  for (i = 0; attributes[i] != NULL; i++)
    {
      g_return_if_fail (values[i] != NULL);

      if (g_str_equal (attributes[i], "name"))
        ns = values[i];
      else if (g_str_equal (attributes[i], "version"))
        version = values[i];
    }

  if (ns == NULL || version == NULL)
    {
      g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                   "<include> missing attribute “name” or “version”");
      return;
    }

  ns_and_ver = g_strdup_printf ("%s-%s", ns, version);

  if (g_hash_table_contains (data->seen, ns_and_ver))
    {
      g_info ("%s depends on %s which was already processed",
              data->ns_and_ver, ns_and_ver);
    }
  else
    {
      g_info ("%s depends on %s", data->ns_and_ver, ns_and_ver);
      g_ptr_array_add (data->todo, g_steal_pointer (&ns_and_ver));
    }
}

static const GMarkupParser depends_parser =
{
  .start_element = depends_parser_start_element
};

static gboolean opt_upstream_paths = FALSE;
static gboolean opt_zero_terminated = FALSE;
static unsigned verbosity = 0;

static GLogWriterOutput
info_log_writer_cb (GLogLevelFlags log_level,
                    const GLogField *fields,
                    size_t n_fields,
                    void *user_data)
{
  if (verbosity == 1 &&
      (log_level & G_LOG_LEVEL_INFO))
    {
      size_t i;

      for (i = 0; i < n_fields; i++)
        {
          if (g_str_equal (fields[i].key, "MESSAGE")
              && fields[i].length == -1)
            {
              g_printerr ("%s: %s\n",
                          g_get_prgname (),
                          (const char *) fields[i].value);
              return G_LOG_WRITER_HANDLED;
            }
        }
    }

  return g_log_writer_default (log_level, fields, n_fields, NULL);
}

static gboolean
opt_verbose_cb (G_GNUC_UNUSED const char *name,
                G_GNUC_UNUSED const char *value,
                G_GNUC_UNUSED void *data,
                G_GNUC_UNUSED GError **error)
{
  verbosity++;

  if (verbosity >= 2)
    g_log_set_debug_enabled (TRUE);
  else if (verbosity == 1)
    g_log_set_writer_func (info_log_writer_cb, NULL, NULL);

  return TRUE;
}

static const GOptionEntry common_option_entries[] =
{
  { "upstream-paths", '\0', G_OPTION_FLAG_NONE,
    G_OPTION_ARG_NONE, &opt_upstream_paths,
    "Search the same paths as gi-compile-repository(1)",
    NULL },
  { "verbose", 'v', G_OPTION_FLAG_NO_ARG,
    G_OPTION_ARG_CALLBACK, opt_verbose_cb,
    "Produce more output",
    NULL },
  { "zero-terminated", 'z', G_OPTION_FLAG_NONE,
    G_OPTION_ARG_NONE, &opt_zero_terminated,
    "Line delimiter is NUL, not newline",
    NULL },
  { NULL }
};

static gboolean
print_search_path_cb (GOptionGroup *common_options,
                      int argc,
                      char **argv,
                      GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) search_path = NULL;
  size_t i;
  char eol = '\n';

  context = g_option_context_new (NULL);
  g_option_context_add_group (context, g_option_group_ref (common_options));
  g_option_context_set_summary (context,
      "Print the search path for GIR XML files.");

  if (!g_option_context_parse (context, &argc, &argv, error))
    return FALSE;

  if (argc > 1)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   "Unexpected non-option argument");
      return FALSE;
    }

  if (opt_zero_terminated)
    eol = 0;

  search_path = get_search_path (opt_upstream_paths);

  for (i = 0; i < search_path->len; i++)
    {
      const char *path_entry = g_ptr_array_index (search_path, i);

      printf ("%s", path_entry);
      putc (eol, stdout);
    }

  return TRUE;
}

static gboolean
find_cb (GOptionGroup *common_options,
         int argc,
         char **argv,
         GError **error)
{
  gboolean opt_if_exists = FALSE;
  gboolean opt_keep = FALSE;
  gboolean opt_overwrite = FALSE;
  gboolean opt_with_dependencies = FALSE;
  g_autofree char *opt_target_directory = NULL;
  const GOptionEntry entries[] =
    {
        { "if-exists", '\0', G_OPTION_FLAG_NONE,
          G_OPTION_ARG_NONE, &opt_if_exists,
          "Silently ignore any nonexistent NAMESPACE-VER",
          NULL },
        { "keep", '\0', G_OPTION_FLAG_NONE,
          G_OPTION_ARG_NONE, &opt_keep,
          "Skip copying if the destination file already exists",
          NULL },
        { "overwrite", '\0', G_OPTION_FLAG_NONE,
          G_OPTION_ARG_NONE, &opt_overwrite,
          "Overwrite if the destination file already exists",
          NULL },
        { "target-directory", '\0', G_OPTION_FLAG_NONE,
          G_OPTION_ARG_FILENAME, &opt_target_directory,
          "Copy selected GIR XML to DIR instead of printing filenames",
          "DIR" },
        { "with-dependencies", '\0', G_OPTION_FLAG_NONE,
          G_OPTION_ARG_NONE, &opt_with_dependencies,
          "Also process each module's recursive dependencies",
          NULL },
        { NULL }
    };
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) search_path = NULL;
  g_autoptr(GPtrArray) todo = NULL;
  g_autoptr(GHashTable) seen = NULL;
  size_t i;
  char eol = '\n';

  context = g_option_context_new ("NAMESPACE-VER [NAMESPACE-VER…]");
  g_option_context_add_group (context, g_option_group_ref (common_options));
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_set_summary (context,
      "Print the paths to the GIR XML files for the given namespaces.");

  if (!g_option_context_parse (context, &argc, &argv, error))
    return FALSE;

  if (argc < 2)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   "At least one NAMESPACE-VER pair is required (try Gio-2.0)");
      return FALSE;
    }

  if (opt_zero_terminated)
    eol = 0;

  search_path = get_search_path (opt_upstream_paths);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  todo = g_ptr_array_new_from_array ((void **) &argv[1],
                                     ((size_t) argc) - 1,
                                     strdup_copy_func,
                                     NULL,
                                     g_free);

  for (i = 0; i < todo->len; i++)
    {
      const char *ns_and_ver = g_ptr_array_index (todo, i);
      gboolean found = FALSE;
      size_t j;

      if (g_hash_table_contains (seen, ns_and_ver))
        {
          g_debug ("Already processed %s", ns_and_ver);
          continue;
        }

      g_hash_table_add (seen, g_strdup (ns_and_ver));

      for (j = 0; j < search_path->len; j++)
        {
          const char *path_entry = g_ptr_array_index (search_path, j);
          g_autofree char *path = NULL;

          path = g_strdup_printf ("%s/%s.gir", path_entry, ns_and_ver);
          g_debug ("Trying %s", path);

          if (g_file_test (path, G_FILE_TEST_EXISTS))
            {
              g_autoptr(GFile) file = g_file_new_for_path (path);

              found = TRUE;

              if (opt_target_directory != NULL)
                {
                  GFileCopyFlags copy_flags = G_FILE_COPY_TARGET_DEFAULT_PERMS;
                  g_autofree char *base = NULL;
                  g_autoptr(GError) local_error = NULL;
                  g_autoptr(GFile) dest = NULL;

                  base = g_strdup_printf ("%s.gir", ns_and_ver);
                  dest = g_file_new_build_filename (opt_target_directory,
                                                    base, NULL);

                  if (opt_overwrite)
                    copy_flags |= G_FILE_COPY_OVERWRITE;

                  if (g_file_copy (file, dest, copy_flags,
                                   NULL, NULL, NULL, &local_error))
                    {
                      g_info ("Copied “%s” to “%s/%s”",
                              path, opt_target_directory, base);
                    }
                  else if (opt_keep
                           && g_error_matches (local_error, G_IO_ERROR,
                                               G_IO_ERROR_EXISTS))
                    {
                      g_info ("Not overwriting “%s/%s”",
                              opt_target_directory, base);
                      g_clear_error (&local_error);
                    }
                  else
                    {
                      return throw_take_error (error, &local_error);
                    }

                }
              else
                {
                  printf ("%s", path);
                  putc (eol, stdout);
                }

              if (opt_with_dependencies)
                {
                  g_autoptr(GInputStream) istream = NULL;
                  g_autoptr(GMarkupParseContext) context = NULL;
                  char buf[4096];
                  ssize_t bytes_read;
                  DependsParserData dpd =
                  {
                    .ns_and_ver = ns_and_ver,
                    .seen = seen,
                    .todo = todo,
                  };

                  istream = G_INPUT_STREAM (g_file_read (file, NULL, error));

                  if (istream == NULL)
                    return FALSE;

                  context = g_markup_parse_context_new (&depends_parser,
                                                        G_MARKUP_DEFAULT_FLAGS,
                                                        &dpd,
                                                        NULL);

                  while (TRUE)
                    {
                      bytes_read = g_input_stream_read (istream, buf, sizeof (buf), NULL, error);

                      if (bytes_read < 0)
                        {
                          g_prefix_error (error, "\n%s: ", path);
                          return FALSE;
                        }

                      if (bytes_read == 0)
                        break;

                      if (!g_markup_parse_context_parse (context, buf, bytes_read, error))
                        return throw_prefix_markup_position (error, path, context);
                    }

                  if (!g_markup_parse_context_end_parse (context, error))
                    return throw_prefix_markup_position (error, path, context);
                }

              break;
            }
        }

      if (!found && !opt_if_exists)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "%s.gir not found in any search path entry", ns_and_ver);
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean help_cb (GOptionGroup *common_options,
                         int argc,
                         char **argv,
                         GError **error);
typedef typeof(&help_cb) Implementation;

static const struct
  {
    const char *name;
    Implementation callback;
    const char *help;
  }
implementations[] =
  {
      { "--help", help_cb, NULL },
      { "help", help_cb, "This help" },
      { "find", find_cb,
        "Print the paths to the GIR XML for NAMESPACE-VER pairs" },
      { "print-search-path", print_search_path_cb,
        "Print the search path for GIR XML" },
  };

static gboolean
help_cb (GOptionGroup *common_options,
         int argc,
         char **argv,
         GError **error)
{
  size_t i;

  g_print ("Usage: %s SUBCOMMAND [ARGUMENT...]\n", g_get_prgname ());
  g_print ("Available commands:\n");

  for (i = 0; i < G_N_ELEMENTS (implementations); i++)
    {
      const char *name = implementations[i].name;
      const char *help = implementations[i].help;

      if (help != NULL)
        g_print ("%s: %s\n", name, help);
    }

  return TRUE;
}

static gboolean
run (int argc,
     char **argv,
     GError **error)
{
  g_autoptr(GOptionGroup) common_options = NULL;
  size_t i;

  if (argc < 2)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   "A subcommand is required");
      return FALSE;
    }

  common_options = g_option_group_new ("common",
                                       "Common Options:",
                                       "Options used by multiple commands",
                                       NULL,
                                       NULL);
  g_option_group_add_entries (common_options, common_option_entries);

  for (i = 0; i < G_N_ELEMENTS (implementations); i++)
    {
      if (g_str_equal (argv[1], implementations[i].name))
        return implementations[i].callback (common_options,
                                            argc - 1,
                                            argv + 1,
                                            error);
    }

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
               "Unknown subcommand");
  return FALSE;
}

int
main(int argc, char **argv)
{
  g_autoptr(GError) local_error = NULL;

  setlocale (LC_ALL, "");
  g_set_prgname ("deb-gi-tool");
  g_log_writer_default_set_use_stderr (TRUE);
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  if (run (argc, argv, &local_error))
    return 0;

  g_printerr ("%s: error: %s\n", g_get_prgname (), local_error->message);

  if (local_error->domain == G_OPTION_ERROR)
    return 2;
  else
    return 1;
}
