/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2021 - David Guillen Fandos
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <file/file_path.h>
#include <lists/string_list.h>
#include <lists/dir_list.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <retro_miscellaneous.h>

#include "sys-clk.h"
#include "../../configuration.h"
#include "../../verbosity.h"

#define REFRESH_TIMEOUT  2

/* ------------------------------------------------------------------ */
/* Per-domain backend description                                     */
/* ------------------------------------------------------------------ */

/* Sysfs leaf names differ between the CPU cpufreq layer and the GPU
 * devfreq layer. The CPU layer also exposes per-policy subdirectories
 * (policy0/, policy1/...) under one root, while devfreq has a flat
 * directory of files per device.
 *
 * Everything that differs between the two backends is collected here
 * so the rest of the file can be domain-agnostic. */
typedef struct sys_clk_backend
{
   /* Resolved sysfs directory for this domain. CPU: the cpufreq
    * root that contains policy*/  /* subdirs. GPU: the devfreq
    * device directory itself (one device per domain). Always
    * slash-terminated. Empty until probed. */
   char root[PATH_MAX_LENGTH];

   /* Filenames within a per-policy/per-device directory. */
   const char *file_cur_freq;
   const char *file_min_policy;
   const char *file_max_policy;
   const char *file_min_hw;        /* may be NULL if the domain has no separate "hw" range */
   const char *file_max_hw;        /* may be NULL */
   const char *file_avail_freqs;
   const char *file_avail_govs;
   const char *file_governor;      /* may be NULL: GPU governor is read-only here */
   const char *file_affected;      /* may be NULL */

   /* CPU has many policy subdirs to enumerate; GPU has exactly one
    * device per resolved root. */
   bool        per_policy_subdirs;
   /* Filename prefix that marks a policy subdir for per_policy_subdirs
    * domains (e.g. "policy" for cpufreq). */
   const char *policy_prefix;

   /* CPU defaults strings used by SYS_CLK_MODE_MANAGED_* and friends.
    * GPU steers by frequency only, so these are NULL there. */
   const char *gov_perf;
   const char *gov_ondemand;
   const char *gov_powersave;
} sys_clk_backend_t;

/* Per-domain state. Holds the live driver list, current user-selected
 * mode and options, and a precomputed absolute frequency range
 * across all drivers in the domain. */
typedef struct sys_clk_state
{
   const sys_clk_backend_t *be;
   sys_clk_driver_t       **drivers;
   time_t                   last_update;
   enum sys_clk_mode        cur_mode;
   sys_clk_opts_t           cur_opts;
   uint32_t                 abs_min_freq;
   uint32_t                 abs_max_freq;
} sys_clk_state_t;

/* CPU backend: standard Linux cpufreq sysfs. */
static const sys_clk_backend_t be_cpu =
{
   /* .root              */ "/sys/devices/system/cpu/cpufreq/",
   /* .file_cur_freq     */ "scaling_cur_freq",
   /* .file_min_policy   */ "scaling_min_freq",
   /* .file_max_policy   */ "scaling_max_freq",
   /* .file_min_hw       */ "cpuinfo_min_freq",
   /* .file_max_hw       */ "cpuinfo_max_freq",
   /* .file_avail_freqs  */ "scaling_available_frequencies",
   /* .file_avail_govs   */ "scaling_available_governors",
   /* .file_governor     */ "scaling_governor",
   /* .file_affected     */ "affected_cpus",
   /* .per_policy_subdirs*/ true,
   /* .policy_prefix     */ "policy",
   /* .gov_perf          */ "performance",
   /* .gov_ondemand      */ "ondemand",
   /* .gov_powersave     */ "powersave"
};

/* GPU backend: Tegra devfreq on Lakka Switch, generic devfreq elsewhere. */
static const sys_clk_backend_t be_gpu =
{
#ifdef HAVE_LAKKA_SWITCH
   /* .root              */ "/sys/devices/gpu.0/devfreq/57000000.gpu/",
#else
   /* .root              */ "",   /* resolved at runtime */
#endif
   /* .file_cur_freq     */ "cur_freq",
   /* .file_min_policy   */ "min_freq",
   /* .file_max_policy   */ "max_freq",
   /* .file_min_hw       */ NULL,
   /* .file_max_hw       */ NULL,
   /* .file_avail_freqs  */ "available_frequencies",
   /* .file_avail_govs   */ "available_governors",
   /* .file_governor     */ NULL,
   /* .file_affected     */ NULL,
   /* .per_policy_subdirs*/ false,
   /* .policy_prefix     */ NULL,
   /* .gov_perf          */ NULL,
   /* .gov_ondemand      */ NULL,
   /* .gov_powersave     */ NULL
};

static sys_clk_state_t g_state[SYS_CLK_DOMAIN_COUNT] = {
   { &be_cpu, NULL, 0, SYS_CLK_MODE_MANAGED_PERFORMANCE,
     { 1, ~0U, "performance", "ondemand" }, 1, ~0U },
   { &be_gpu, NULL, 0, SYS_CLK_MODE_MANAGED_PERFORMANCE,
     { 1, ~0U, "", "" }, 1, ~0U }
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static bool readparse_uint32(const char *path, uint32_t *value)
{
   char *tmpbuf;
   char *endp;
   unsigned long v;
   if (!filestream_read_file(path, (void**)&tmpbuf, NULL))
      return false;
   string_remove_all_chars(tmpbuf, '\n');
   v = strtoul(tmpbuf, &endp, 10);
   if (endp == tmpbuf)
   {
      free(tmpbuf);
      return false;
   }
   *value = (uint32_t)v;
   free(tmpbuf);
   return true;
}

static struct string_list *readparse_list(const char *path)
{
   char *tmpbuf;
   struct string_list *ret;
   if (!filestream_read_file(path, (void**)&tmpbuf, NULL))
      return NULL;
   string_remove_all_chars(tmpbuf, '\n');
   ret = string_split(tmpbuf, " ");
   free(tmpbuf);
   return ret;
}

static void free_drivers(sys_clk_driver_t **d)
{
   sys_clk_driver_t **it;
   if (!d)
      return;
   for (it = d; *it; it++)
   {
      sys_clk_driver_t *drv = *it;
      if (drv->affected_cpus)
         free(drv->affected_cpus);
      if (drv->scaling_governor)
         free(drv->scaling_governor);
      if (drv->available_freqs)
         free(drv->available_freqs);
      string_list_free(drv->available_governors);
      free(drv);
   }
   free(d);
}

/* In-place ascending sort of a NULL-terminated u32 list of length n. */
static void sort_freqs(uint32_t *arr, int n)
{
   int a, b;
   for (a = 0; a < n - 1; a++)
   {
      for (b = 0; b < n - 1 - a; b++)
      {
         if (arr[b] > arr[b + 1])
         {
            uint32_t tmp = arr[b];
            arr[b]       = arr[b + 1];
            arr[b + 1]   = tmp;
         }
      }
   }
}

/* Read one policy/device directory into a freshly allocated driver.
 * Updates the domain's abs_min/max_freq window. Returns NULL on
 * failure (caller frees nothing). */
static sys_clk_driver_t *read_one_driver(sys_clk_state_t *st,
      const char *dirpath, unsigned policy_id)
{
   const sys_clk_backend_t *be = st->be;
   sys_clk_driver_t        *drv;
   char                     fpath[PATH_MAX_LENGTH];
   struct string_list      *tmplst;
   int                      j;

   drv = (sys_clk_driver_t*)calloc(1, sizeof(*drv));
   if (!drv)
      return NULL;
   drv->policy_id = policy_id;

   fill_pathname_join(fpath, dirpath, be->file_cur_freq, sizeof(fpath));
   readparse_uint32(fpath, &drv->current_frequency);

   fill_pathname_join(fpath, dirpath, be->file_min_policy, sizeof(fpath));
   readparse_uint32(fpath, &drv->min_policy_freq);
   fill_pathname_join(fpath, dirpath, be->file_max_policy, sizeof(fpath));
   readparse_uint32(fpath, &drv->max_policy_freq);

   if (be->file_min_hw)
   {
      fill_pathname_join(fpath, dirpath, be->file_min_hw, sizeof(fpath));
      readparse_uint32(fpath, &drv->min_hw_freq);
   }
   else
      drv->min_hw_freq = drv->min_policy_freq;
   if (be->file_max_hw)
   {
      fill_pathname_join(fpath, dirpath, be->file_max_hw, sizeof(fpath));
      readparse_uint32(fpath, &drv->max_hw_freq);
   }
   else
      drv->max_hw_freq = drv->max_policy_freq;

   /* Track the absolute range across all drivers in the domain. */
   if (drv->min_hw_freq &&
         (st->abs_min_freq == 1 || drv->min_hw_freq < st->abs_min_freq))
      st->abs_min_freq = drv->min_hw_freq;
   if (drv->max_hw_freq &&
         (st->abs_max_freq == ~0U || drv->max_hw_freq > st->abs_max_freq))
      st->abs_max_freq = drv->max_hw_freq;

   if (be->file_avail_govs)
   {
      fill_pathname_join(fpath, dirpath, be->file_avail_govs, sizeof(fpath));
      drv->available_governors = readparse_list(fpath);
   }

   if (be->file_affected)
   {
      fill_pathname_join(fpath, dirpath, be->file_affected, sizeof(fpath));
      if (filestream_read_file(fpath, (void**)&drv->affected_cpus, NULL)
            && drv->affected_cpus)
         string_remove_all_chars(drv->affected_cpus, '\n');
   }

   if (be->file_governor)
   {
      fill_pathname_join(fpath, dirpath, be->file_governor, sizeof(fpath));
      if (filestream_read_file(fpath, (void**)&drv->scaling_governor, NULL)
            && drv->scaling_governor)
         string_remove_all_chars(drv->scaling_governor, '\n');
   }

   /* Optional discrete frequency table. */
   fill_pathname_join(fpath, dirpath, be->file_avail_freqs, sizeof(fpath));
   tmplst = readparse_list(fpath);
   if (tmplst)
   {
      int n = 0;
      drv->available_freqs = (uint32_t*)calloc(tmplst->size + 1,
            sizeof(uint32_t));
      if (drv->available_freqs)
      {
         for (j = 0; j < (int)tmplst->size; j++)
         {
            uint32_t f = (uint32_t)atol(tmplst->elems[j].data);
            if (!f)
               continue;
            drv->available_freqs[n++] = f;
            if (st->abs_min_freq == 1 || f < st->abs_min_freq)
               st->abs_min_freq = f;
            if (st->abs_max_freq == ~0U || f > st->abs_max_freq)
               st->abs_max_freq = f;
         }
         sort_freqs(drv->available_freqs, n);
      }
      string_list_free(tmplst);
   }

   return drv;
}

/* Resolve the sysfs root for a domain. CPU is compile-time fixed.
 * GPU on Switch is also compile-time fixed; on any other build we
 * scan /sys/class/devfreq/ for the first device that exposes
 * cur_freq + max_freq. Returns false if no usable device exists. */
static bool resolve_root(sys_clk_state_t *st)
{
   if (st->be->root[0])
      return path_is_directory(st->be->root);

#if defined(HAVE_LAKKA) && !defined(HAVE_LAKKA_SWITCH)
   if (st == &g_state[SYS_CLK_DOMAIN_GPU])
   {
      int                 i;
      struct string_list *devs = dir_list_new("/sys/class/devfreq/", NULL,
            true, false, false, false);
      if (!devs)
         return false;
      dir_list_sort(devs, false);
      for (i = 0; i < (int)devs->size; i++)
      {
         char probe[PATH_MAX_LENGTH];
         fill_pathname_join(probe, devs->elems[i].data,
               st->be->file_cur_freq, sizeof(probe));
         if (!path_is_valid(probe))
            continue;
         fill_pathname_join(probe, devs->elems[i].data,
               st->be->file_max_policy, sizeof(probe));
         if (!path_is_valid(probe))
            continue;
         /* be->root is compile-time const in the static instance, but
          * here we deliberately patch the per-domain copy. The struct
          * is mutable because g_state[] holds it by value indirectly
          * through the pointer; we instead write to a separate buffer
          * by casting away the pointer constness on the state's
          * backend reference. */
         {
            sys_clk_backend_t *mb = (sys_clk_backend_t*)st->be;
            size_t             l;
            strlcpy(mb->root, devs->elems[i].data, sizeof(mb->root));
            l = strlen(mb->root);
            if (l && l + 1 < sizeof(mb->root) && mb->root[l - 1] != '/')
            {
               mb->root[l]     = '/';
               mb->root[l + 1] = '\0';
            }
            RARCH_LOG("[sys-clk]: using GPU devfreq device %s\n", mb->root);
         }
         dir_list_free(devs);
         return true;
      }
      dir_list_free(devs);
   }
#endif
   return false;
}

/* Build/refresh the list of drivers for a domain. */
sys_clk_driver_t **sys_clk_get_drivers(enum sys_clk_domain dom,
      bool can_update)
{
   sys_clk_state_t *st;
   if (dom >= SYS_CLK_DOMAIN_COUNT)
      return NULL;
   st = &g_state[dom];

   if (!can_update
         || (st->drivers
            && time(NULL) <= st->last_update + REFRESH_TIMEOUT))
      return st->drivers;

   if (!resolve_root(st))
      return st->drivers; /* could be NULL or stale; nothing else to do */

   free_drivers(st->drivers);
   st->drivers = NULL;

   if (st->be->per_policy_subdirs)
   {
      /* CPU: enumerate policy*/  /* subdirs under root. */
      struct string_list *policy_dir = dir_list_new(st->be->root, NULL,
            true, false, false, false);
      int                 i, pc;
      size_t              prefix_len;
      if (!policy_dir)
         return NULL;
      dir_list_sort(policy_dir, false);

      st->drivers = (sys_clk_driver_t**)calloc(
            policy_dir->size + 1, sizeof(sys_clk_driver_t*));
      if (!st->drivers)
      {
         dir_list_free(policy_dir);
         return NULL;
      }
      prefix_len = strlen(st->be->policy_prefix);
      for (i = 0, pc = 0; i < (int)policy_dir->size; i++)
      {
         const char       *fname = strrchr(policy_dir->elems[i].data, '/');
         sys_clk_driver_t *drv;
         char             *endp;
         unsigned long     v;
         if (!fname)
            continue;
         fname++;   /* skip the leading slash */
         if (strncmp(fname, st->be->policy_prefix, prefix_len) != 0)
            continue;
         v = strtoul(fname + prefix_len, &endp, 10);
         if (endp == fname + prefix_len)
            continue;
         drv = read_one_driver(st, policy_dir->elems[i].data, (unsigned)v);
         if (drv)
            st->drivers[pc++] = drv;
      }
      dir_list_free(policy_dir);
   }
   else
   {
      /* GPU: exactly one driver per resolved root. */
      sys_clk_driver_t *drv;
      st->drivers = (sys_clk_driver_t**)calloc(2,
            sizeof(sys_clk_driver_t*));
      if (!st->drivers)
         return NULL;
      drv = read_one_driver(st, st->be->root, 0);
      if (drv)
         st->drivers[0] = drv;
   }

   st->last_update = time(NULL);
   return st->drivers;
}

/* ------------------------------------------------------------------ */
/* Per-driver setters                                                 */
/* ------------------------------------------------------------------ */

/* Build the path for writing a leaf inside the driver's policy dir. */
static void build_leaf_path(const sys_clk_state_t *st,
      const sys_clk_driver_t *drv, const char *leaf,
      char *out, size_t out_sz)
{
   if (st->be->per_policy_subdirs)
      snprintf(out, out_sz, "%s%s%u/%s",
            st->be->root, st->be->policy_prefix,
            drv->policy_id, leaf);
   else
      snprintf(out, out_sz, "%s%s", st->be->root, leaf);
}

static bool write_uint32_leaf(enum sys_clk_domain dom,
      sys_clk_driver_t *driver, const char *leaf, uint32_t value)
{
   sys_clk_state_t *st;
   char             fpath[PATH_MAX_LENGTH];
   char             buf[16];
   if (dom >= SYS_CLK_DOMAIN_COUNT)
      return false;
   st = &g_state[dom];
   build_leaf_path(st, driver, leaf, fpath, sizeof(fpath));
   snprintf(buf, sizeof(buf), "%" PRIu32 "\n", value);
   if (!filestream_write_file(fpath, buf, strlen(buf)))
      return false;
   st->last_update = 0;
   return true;
}

bool sys_clk_set_min_frequency(enum sys_clk_domain dom,
      sys_clk_driver_t *driver, uint32_t min_freq)
{
   sys_clk_state_t *st = &g_state[dom];
   if (!write_uint32_leaf(dom, driver, st->be->file_min_policy, min_freq))
      return false;
   driver->min_policy_freq = min_freq;
   return true;
}

bool sys_clk_set_max_frequency(enum sys_clk_domain dom,
      sys_clk_driver_t *driver, uint32_t max_freq)
{
   sys_clk_state_t *st = &g_state[dom];
   if (!write_uint32_leaf(dom, driver, st->be->file_max_policy, max_freq))
      return false;
   driver->max_policy_freq = max_freq;
   return true;
}

bool sys_clk_set_governor(enum sys_clk_domain dom,
      sys_clk_driver_t *driver, const char *governor)
{
   sys_clk_state_t *st;
   char             fpath[PATH_MAX_LENGTH];
   if (dom >= SYS_CLK_DOMAIN_COUNT || !governor)
      return false;
   st = &g_state[dom];
   if (!st->be->file_governor)
      return false;   /* domain has no writable governor (GPU) */
   build_leaf_path(st, driver, st->be->file_governor, fpath, sizeof(fpath));
   if (!filestream_write_file(fpath, governor, strlen(governor)))
      return false;
   if (driver->scaling_governor)
      free(driver->scaling_governor);
   driver->scaling_governor = strdup(governor);
   st->last_update = 0;
   return true;
}

/* ------------------------------------------------------------------ */
/* Frequency stepping                                                 */
/* ------------------------------------------------------------------ */

uint32_t sys_clk_get_next_frequency(enum sys_clk_domain dom,
      sys_clk_driver_t *driver, uint32_t freq, int step)
{
   sys_clk_state_t *st;
   uint32_t         abs_min, abs_max;
   if (dom >= SYS_CLK_DOMAIN_COUNT || !driver)
      return freq;
   st      = &g_state[dom];
   abs_min = st->abs_min_freq;
   abs_max = st->abs_max_freq;

   if (driver->available_freqs && driver->available_freqs[0])
   {
      uint32_t *fr;

      /* At/below the bottom while stepping down: "Min." sentinel. */
      if (freq <= driver->available_freqs[0] && step < 0)
         return 1;
      /* At/above the top while stepping up: "Max." sentinel. */
      if (freq >= abs_max && step > 0)
         return ~0U;

      /* Translate sentinels back to real numbers for the search. */
      if (freq < abs_min && step > 0)
         freq = abs_min;
      if (freq == ~0U && step < 0)
         freq = abs_max;

      for (fr = driver->available_freqs; *fr; fr++)
      {
         if (fr[0] <= freq && fr[1] > freq && step > 0)
            return (fr[1] != abs_max) ? fr[1] : ~0U;
         if (fr[0] < freq && fr[1] >= freq && step < 0)
            return (fr[0] != abs_min) ? fr[0] : 1;
      }
      /* Walked off the end. */
      return (step > 0) ? driver->max_hw_freq : driver->min_hw_freq;
   }

   /* No discrete list: arbitrary 100MHz steps, clamped per-driver. */
   freq = freq + step * 100000;
   freq = MIN(freq, driver->max_hw_freq);
   freq = MAX(freq, driver->min_hw_freq);
   return freq;
}

uint32_t sys_clk_get_next_frequency_limit(enum sys_clk_domain dom,
      uint32_t freq, int step)
{
   sys_clk_state_t *st;
   unsigned         fstep = 100000;
   if (dom >= SYS_CLK_DOMAIN_COUNT)
      return freq;
   st = &g_state[dom];

   if ((st->abs_max_freq - st->abs_min_freq) / 20 < fstep)
      fstep = 50000;

   if (freq <= st->abs_min_freq && step < 0)
      return 1;
   if (freq >= st->abs_max_freq && step > 0)
      return ~0U;

   freq = freq + step * fstep;
   freq = MIN(freq, st->abs_max_freq);
   freq = MAX(freq, st->abs_min_freq);
   return freq;
}

/* ------------------------------------------------------------------ */
/* Mode application                                                   */
/* ------------------------------------------------------------------ */

/* Steer every driver in a domain. The GPU domain has no writable
 * governor and the CPU domain always wants one; passing NULL for
 * `governor` selects the per-domain default behavior. */
static void steer_all(enum sys_clk_domain dom,
      const char *governor, uint32_t minfreq, uint32_t maxfreq)
{
   sys_clk_state_t   *st = &g_state[dom];
   sys_clk_driver_t **it = sys_clk_get_drivers(dom, false);
   if (!it)
      return;
   for (; *it; it++)
   {
      sys_clk_driver_t *d = *it;
      if (minfreq)
      {
         /* When the driver advertises a discrete table, the
          * sentinels 1 and ~0U mean "absolute min/max". */
         if (d->available_freqs)
         {
            if (minfreq == 1)
               sys_clk_set_min_frequency(dom, d, st->abs_min_freq);
            else
               sys_clk_set_min_frequency(dom, d, minfreq);
         }
         else
            sys_clk_set_min_frequency(dom, d, MAX(minfreq, d->min_hw_freq));
      }
      if (maxfreq)
      {
         if (d->available_freqs)
         {
            if (maxfreq == ~0U)
               sys_clk_set_max_frequency(dom, d, st->abs_max_freq);
            else
               sys_clk_set_max_frequency(dom, d, maxfreq);
         }
         else
            sys_clk_set_max_frequency(dom, d, MIN(maxfreq, d->max_hw_freq));
      }
      if (governor && st->be->file_governor)
         sys_clk_set_governor(dom, d, governor);
   }
}

void sys_clk_set_signal(enum sys_clk_domain dom, enum sys_clk_event event)
{
   sys_clk_state_t *st;
   if (dom >= SYS_CLK_DOMAIN_COUNT)
      return;
   st = &g_state[dom];

   switch (st->cur_mode)
   {
      case SYS_CLK_MODE_MANAGED_PERFORMANCE:
         if (dom == SYS_CLK_DOMAIN_CPU)
         {
            /* CPU: bump to performance during a core, fall back to
             * ondemand otherwise (and let the governor pick the
             * frequency, so 1/~0U as the range). */
            if (event == SYS_CLK_EVENT_FOCUS_CORE)
               steer_all(dom, st->be->gov_perf,
                     st->cur_opts.min_freq, st->cur_opts.max_freq);
            else
               steer_all(dom, st->be->gov_ondemand, 1, ~0U);
         }
         else
         {
            /* GPU: user-selected range while a core is in focus,
             * drop to absolute min otherwise. */
            if (event == SYS_CLK_EVENT_FOCUS_CORE)
               steer_all(dom, NULL,
                     st->cur_opts.min_freq, st->cur_opts.max_freq);
            else
               steer_all(dom, NULL,
                     st->abs_min_freq, st->abs_min_freq);
         }
         break;

      case SYS_CLK_MODE_MANAGED_PER_CONTEXT:
         /* CPU-only. */
         if (dom == SYS_CLK_DOMAIN_CPU)
         {
            if (event == SYS_CLK_EVENT_FOCUS_CORE)
               steer_all(dom, st->cur_opts.main_policy,
                     st->cur_opts.min_freq, st->cur_opts.max_freq);
            else
               steer_all(dom, st->cur_opts.menu_policy, 1, ~0U);
         }
         break;

      default:
         break;
   }
}

enum sys_clk_mode sys_clk_get_mode(enum sys_clk_domain dom,
      sys_clk_opts_t *opts)
{
   if (dom >= SYS_CLK_DOMAIN_COUNT)
      return SYS_CLK_MODE_MANAGED_PERFORMANCE;
   if (opts)
      *opts = g_state[dom].cur_opts;
   return g_state[dom].cur_mode;
}

/* Persist the current state to settings_t. CPU and GPU live in
 * different fields. */
static void persist_mode(enum sys_clk_domain dom)
{
   settings_t      *settings = config_get_ptr();
   sys_clk_state_t *st       = &g_state[dom];
   if (!settings)
      return;
   if (dom == SYS_CLK_DOMAIN_CPU)
   {
      settings->uints.cpu_scaling_mode = (int)st->cur_mode;
      settings->uints.cpu_min_freq     = st->cur_opts.min_freq;
      settings->uints.cpu_max_freq     = st->cur_opts.max_freq;
      strlcpy(settings->arrays.cpu_main_gov, st->cur_opts.main_policy,
            sizeof(settings->arrays.cpu_main_gov));
      strlcpy(settings->arrays.cpu_menu_gov, st->cur_opts.menu_policy,
            sizeof(settings->arrays.cpu_menu_gov));
   }
   else
   {
      /* Translate the unified enum back to the historical GPU value
       * order before writing. See gpu_mode_from_config_value below. */
      unsigned wire;
      switch (st->cur_mode)
      {
         case SYS_CLK_MODE_MANAGED_PERFORMANCE: wire = 0; break;
         case SYS_CLK_MODE_MAX_PERFORMANCE:     wire = 1; break;
         case SYS_CLK_MODE_MIN_POWER:           wire = 2; break;
         case SYS_CLK_MODE_BALANCED:            wire = 3; break;
         default:                               wire = 0; break;
      }
      settings->uints.gpu_scaling_mode = wire;
      settings->uints.gpu_min_freq     = st->cur_opts.min_freq;
      settings->uints.gpu_max_freq     = st->cur_opts.max_freq;
   }
}

void sys_clk_set_mode(enum sys_clk_domain dom,
      enum sys_clk_mode mode, const sys_clk_opts_t *opts)
{
   sys_clk_state_t *st;
   if (dom >= SYS_CLK_DOMAIN_COUNT)
      return;
   st           = &g_state[dom];
   st->cur_mode = mode;
   if (opts)
      st->cur_opts = *opts;

   switch (mode)
   {
      case SYS_CLK_MODE_MANUAL:
         /* Nothing to enforce: the UI tweaks the policy directly. */
         break;
      case SYS_CLK_MODE_MANAGED_PERFORMANCE:
      case SYS_CLK_MODE_MANAGED_PER_CONTEXT:
         /* Re-trigger the focus path. */
         sys_clk_set_signal(dom, SYS_CLK_EVENT_FOCUS_MENU);
         break;
      case SYS_CLK_MODE_MAX_PERFORMANCE:
         if (dom == SYS_CLK_DOMAIN_CPU)
            steer_all(dom, st->be->gov_perf, 1, ~0U);
         else
            steer_all(dom, NULL, st->abs_max_freq, st->abs_max_freq);
         break;
      case SYS_CLK_MODE_MIN_POWER:
         if (dom == SYS_CLK_DOMAIN_CPU)
            steer_all(dom, st->be->gov_powersave, 1, ~0U);
         else
            steer_all(dom, NULL, st->abs_min_freq, st->abs_min_freq);
         break;
      case SYS_CLK_MODE_BALANCED:
         if (dom == SYS_CLK_DOMAIN_CPU)
            steer_all(dom, st->be->gov_ondemand, 1, ~0U);
         else
            steer_all(dom, NULL, st->abs_min_freq, st->abs_max_freq);
         break;
   }

   persist_mode(dom);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/* Translate a saved GPU mode integer (which uses the historical
 * gpu_scaling_mode ordering) into the unified enum. */
static enum sys_clk_mode gpu_mode_from_config_value(unsigned wire)
{
   switch (wire)
   {
      case 0:  return SYS_CLK_MODE_MANAGED_PERFORMANCE;
      case 1:  return SYS_CLK_MODE_MAX_PERFORMANCE;
      case 2:  return SYS_CLK_MODE_MIN_POWER;
      case 3:  return SYS_CLK_MODE_BALANCED;
      default: return SYS_CLK_MODE_MANAGED_PERFORMANCE;
   }
}

void sys_clk_driver_init(enum sys_clk_domain dom)
{
   settings_t      *settings = config_get_ptr();
   sys_clk_state_t *st;
   if (dom >= SYS_CLK_DOMAIN_COUNT || !settings)
      return;
   st = &g_state[dom];

   if (dom == SYS_CLK_DOMAIN_CPU)
   {
      unsigned mode             = settings->uints.cpu_scaling_mode;
      st->cur_opts.min_freq     = settings->uints.cpu_min_freq;
      st->cur_opts.max_freq     = settings->uints.cpu_max_freq;
      if (mode <= (unsigned)SYS_CLK_MODE_MANUAL)
         st->cur_mode = (enum sys_clk_mode)mode;
      if (settings->arrays.cpu_main_gov[0])
         strlcpy(st->cur_opts.main_policy,
               settings->arrays.cpu_main_gov,
               sizeof(st->cur_opts.main_policy));
      if (settings->arrays.cpu_menu_gov[0])
         strlcpy(st->cur_opts.menu_policy,
               settings->arrays.cpu_menu_gov,
               sizeof(st->cur_opts.menu_policy));
   }
   else
   {
      st->cur_opts.min_freq = settings->uints.gpu_min_freq;
      st->cur_opts.max_freq = settings->uints.gpu_max_freq;
      st->cur_mode = gpu_mode_from_config_value(
            settings->uints.gpu_scaling_mode);
   }

   /* Populate the driver tree, then enforce the mode. */
   sys_clk_get_drivers(dom, true);
   sys_clk_set_mode(dom, st->cur_mode, NULL);
}

void sys_clk_driver_free(enum sys_clk_domain dom)
{
   sys_clk_state_t *st;
   if (dom >= SYS_CLK_DOMAIN_COUNT)
      return;
   st = &g_state[dom];
   free_drivers(st->drivers);
   st->drivers     = NULL;
   st->last_update = 0;
}
