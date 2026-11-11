#include "mod_runtime.h"
#include "smrpg_replay.h"
static void reset(void){SmrpgReplayEnable(false);}
static void activate(void){SmrpgReplayEnable(true);}
SNES_MOD_CONSTRUCTOR(smrpg_register_diagnostics_plugin){
  (void)snes_mod_register_reset_callback(reset);
  (void)snes_mod_register_activation_plugin("super-mario-rpg.diagnostics",activate);
}
