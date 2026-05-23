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
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* sys-clk: unified system clock scaling backend.
 *
 * This module replaces the previous misc/cpufreq and misc/gpufreq
 * modules. CPU and GPU scaling share the same sysfs-driven structure
 * (a list of policy drivers, a per-domain "mode" describing how RA
 * steers the policy, and lifecycle hooks tied to focus events), so
 * the two have been folded into one driver indexed by an
 * enum sys_clk_domain.
 *
 * The CPU domain is wired to /sys/devices/system/cpu/cpufreq/. The
 * GPU domain on Lakka Switch is wired to a fixed Tegra devfreq path;
 * on any other HAVE_LAKKA build, sys-clk probes /sys/class/devfreq/
 * at runtime. Per-domain quirks (governor list semantics, sysfs leaf
 * names, frequency persistence policy, "menu vs core" steering) are
 * isolated in a small ops table inside sys-clk.c, so the public API
 * is identical for both domains.
 */

#ifndef _MISC_SYS_CLK_H
#define _MISC_SYS_CLK_H

#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define MAX_GOV_STRLEN   32

/* Which clock domain a call applies to. */
enum sys_clk_domain
{
   SYS_CLK_DOMAIN_CPU = 0,
   SYS_CLK_DOMAIN_GPU,
   SYS_CLK_DOMAIN_COUNT
};

/* Events from frontend to the driver to drive policies. */
enum sys_clk_event
{
   SYS_CLK_EVENT_FOCUS_CORE,
   SYS_CLK_EVENT_FOCUS_MENU,
   SYS_CLK_EVENT_FOCUS_SCREENSAVER
};

/* Scaling mode selected by the user.
 *
 * Numeric values are CPU-stable on disk: they match the historical
 * cpu_scaling_mode enum so existing cpu_scaling_mode settings keep
 * the same meaning. _PER_CONTEXT and _MANUAL are CPU-only and are
 * never set for the GPU domain.
 *
 * The GPU domain's saved-config integer follows the historical
 * gpu_scaling_mode ordering (MANAGED=0, MAX=1, MIN=2, BALANCED=3)
 * and is translated at the I/O boundary by sys_clk_driver_init.
 */
enum sys_clk_mode
{
   SYS_CLK_MODE_MANAGED_PERFORMANCE = 0, /* Performance while running core */
   SYS_CLK_MODE_MANAGED_PER_CONTEXT = 1, /* Per-context policies (CPU only) */
   SYS_CLK_MODE_MAX_PERFORMANCE     = 2, /* Always max */
   SYS_CLK_MODE_MIN_POWER           = 3, /* Always min */
   SYS_CLK_MODE_BALANCED            = 4, /* Ondemand-style */
   SYS_CLK_MODE_MANUAL              = 5  /* User tweaks directly (CPU only) */
};

/* Per-mode options.
 *
 * main_policy/menu_policy are governor names and are only consulted
 * when domain is CPU and mode is _MANAGED_PER_CONTEXT.
 */
typedef struct sys_clk_opts
{
   uint32_t min_freq, max_freq;
   char main_policy[MAX_GOV_STRLEN];
   char menu_policy[MAX_GOV_STRLEN];
} sys_clk_opts_t;

/* Per-policy driver. Fields shared by both domains live at the top;
 * affected_cpus and scaling_governor are populated for the CPU
 * domain only. */
typedef struct sys_clk_driver
{
   unsigned int policy_id;
   /* Comma-separated list of affected CPU IDs (CPU only). */
   char *affected_cpus;
   /* Current governor name and the list of selectable governors. */
   char *scaling_governor;
   struct string_list *available_governors;
   /* Current frequency (snapshot, may be slightly stale). */
   uint32_t current_frequency;
   /* Hardware and policy frequency bounds. */
   uint32_t min_hw_freq, max_hw_freq;
   uint32_t min_policy_freq, max_policy_freq;
   /* Optional discrete frequency table, NULL-terminated with 0. */
   uint32_t *available_freqs;
} sys_clk_driver_t;

/* Lifecycle. Init and free are called once per domain at startup
 * and shutdown respectively. They are no-ops on platforms where the
 * domain is not supported. */
void sys_clk_driver_init(enum sys_clk_domain dom);
void sys_clk_driver_free(enum sys_clk_domain dom);

/* Returns true iff sys_clk_driver_init successfully resolved a
 * sysfs device for this domain and at least one driver was
 * populated. The menu uses this to hide UI entries for domains
 * that no usable hardware backs (e.g. a desktop build with no
 * devfreq node for the GPU). Cheap: no sysfs access. */
bool sys_clk_domain_available(enum sys_clk_domain dom);

/* Get the (NULL-terminated) list of per-policy drivers for a
 * domain. Pass can_update=true to refresh from sysfs (rate-limited
 * internally). */
sys_clk_driver_t **sys_clk_get_drivers(enum sys_clk_domain dom,
      bool can_update);

/* Per-driver setters: write min/max policy frequency and governor
 * back through sysfs. Return true on success. */
bool sys_clk_set_min_frequency(enum sys_clk_domain dom,
      sys_clk_driver_t *driver, uint32_t min_freq);
bool sys_clk_set_max_frequency(enum sys_clk_domain dom,
      sys_clk_driver_t *driver, uint32_t max_freq);
bool sys_clk_set_governor(enum sys_clk_domain dom,
      sys_clk_driver_t *driver, const char *governor);

/* Return the user's last-set endpoint-sentinel intent for a given
 * driver's min/max policy frequency, or 0 if the user has not
 * parked the slider at an endpoint. The menu uses this so a
 * slider sitting at 'Min.' / 'Max.' keeps rendering that label
 * across sysfs cache refreshes (which always report a concrete
 * frequency, never a sentinel). The return value is one of
 * 0, 1 (= 'Min.'), or ~0U (= 'Max.'). */
uint32_t sys_clk_get_min_intent(enum sys_clk_domain dom,
      const sys_clk_driver_t *driver);
uint32_t sys_clk_get_max_intent(enum sys_clk_domain dom,
      const sys_clk_driver_t *driver);

/* Step to the next/previous frequency. The "table" variant uses
 * driver->available_freqs when present and produces sentinel values
 * (1, ~0U) for "Min." / "Max." so the UI can render them. The
 * "limit" variant always works against the domain's absolute
 * min/max and is suitable for drivers without a discrete table. */
uint32_t sys_clk_get_next_frequency(enum sys_clk_domain dom,
      sys_clk_driver_t *driver, uint32_t freq, int step);
uint32_t sys_clk_get_next_frequency_limit(enum sys_clk_domain dom,
      uint32_t freq, int step);

/* Signal a frontend focus event so the driver can re-steer the
 * policy (e.g. drop to min while paused in the menu). */
void sys_clk_set_signal(enum sys_clk_domain dom,
      enum sys_clk_event event);

/* Get/set the current scaling mode. opts may be NULL on get/set.
 * On set, the mode is applied immediately and persisted to config. */
enum sys_clk_mode sys_clk_get_mode(enum sys_clk_domain dom,
      sys_clk_opts_t *opts);
void sys_clk_set_mode(enum sys_clk_domain dom,
      enum sys_clk_mode mode, const sys_clk_opts_t *opts);

RETRO_END_DECLS

#endif
