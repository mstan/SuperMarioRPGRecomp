#include "mod_runtime.h"
#include "recomp_launcher.h"
#include "smrpg_widescreen_plugin.h"

#include <string.h>

#define SMRPG_WS_PACKAGE "super-mario-rpg.enhancement.widescreen"
#define SMRPG_WS_FEATURE "widescreen"
#define SMRPG_WS_PLUGIN "super-mario-rpg.widescreen"

static int g_smrpg_widescreen_enabled;
static int g_smrpg_aspect;

int smrpg_widescreen_aspect(void) { return g_smrpg_aspect; }

int smrpg_widescreen_enabled(void) {
  return g_smrpg_widescreen_enabled != 0;
}

static void smrpg_widescreen_reset(void) {
  g_smrpg_widescreen_enabled = 0;
  g_smrpg_aspect = 0;
}

static void smrpg_widescreen_activate(void) {
  g_smrpg_widescreen_enabled = 1;
  g_smrpg_aspect = 0;

  const RecompLauncherCModProvider *provider =
      snes_mod_runtime_launcher_provider_c();
  if (!provider || !provider->feature_option_get)
    return;

  RecompLauncherCModOption option;
  for (int i = 0; i < 16; i++) {
    memset(&option, 0, sizeof(option));
    if (!provider->feature_option_get(provider->ctx, SMRPG_WS_PACKAGE,
                                      SMRPG_WS_FEATURE, i, &option)) {
      break;
    }
    if (strcmp(option.id, "aspect") == 0)
      g_smrpg_aspect = strcmp(option.value, "16:9") == 0 ? 1 :
                      strcmp(option.value, "21:9") == 0 ? 2 : 0;
  }
}

SNES_MOD_CONSTRUCTOR(smrpg_register_widescreen_plugin) {
  (void)snes_mod_register_reset_callback(smrpg_widescreen_reset);
  (void)snes_mod_register_activation_plugin(SMRPG_WS_PLUGIN,
                                            smrpg_widescreen_activate);
}
