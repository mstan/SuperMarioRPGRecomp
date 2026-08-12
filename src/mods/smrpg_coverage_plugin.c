#include "mod_runtime.h"
#include "smrpg_coverage_plugin.h"

#define SMRPG_COVERAGE_PLUGIN "super-mario-rpg.coverage-proof"

static int g_smrpg_coverage_proof_capture;

int smrpg_coverage_proof_capture_enabled(void) {
  return g_smrpg_coverage_proof_capture != 0;
}

static void smrpg_coverage_reset(void) {
  g_smrpg_coverage_proof_capture = 0;
}

static void smrpg_coverage_activate(void) {
  g_smrpg_coverage_proof_capture = 1;
}

SNES_MOD_CONSTRUCTOR(smrpg_register_coverage_plugin) {
  (void)snes_mod_register_reset_callback(smrpg_coverage_reset);
  (void)snes_mod_register_activation_plugin(SMRPG_COVERAGE_PLUGIN,
                                            smrpg_coverage_activate);
}
