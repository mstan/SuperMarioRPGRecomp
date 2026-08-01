#include "host_report.h"
#include "spc_player.h"
#include "types.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

bool g_new_ppu = true;

static void headless_spc_initialize(SpcPlayer *player) { (void)player; }

static void headless_spc_upload(SpcPlayer *player, const uint8_t *data) {
  (void)player;
  (void)data;
}

static SpcPlayer g_headless_spc_player = {
    .initialize = headless_spc_initialize,
    .upload = headless_spc_upload,
};

SpcPlayer *g_spc_player = &g_headless_spc_player;

void NORETURN Die(const char *error) {
  fprintf(stderr, "fatal: %s\n", error ? error : "unknown error");
  exit(EXIT_FAILURE);
}

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

void host_report_init(const char *game_name, const char *build_version) {
  (void)game_name;
  (void)build_version;
}

void host_report_breadcrumb(const char *format, ...) { (void)format; }

void host_report_fatal(const char *message) {
  if (message) fprintf(stderr, "fatal: %s\n", message);
}

int host_report_has_fatal(void) { return 0; }
void host_report_dump_json(FILE *stream) { (void)stream; }
const char *host_report_write_minidump(void *info) {
  (void)info;
  return NULL;
}
const char *host_report_preserve_crash_copy(const char *path) {
  (void)path;
  return NULL;
}
void host_report_crash_test_tick(void) {}

#ifdef _WIN32
static LONG WINAPI headless_exception_filter(EXCEPTION_POINTERS *exception) {
  const CONTEXT *context =
      exception && exception->ContextRecord ? exception->ContextRecord : NULL;
  const EXCEPTION_RECORD *record =
      exception ? exception->ExceptionRecord : NULL;
  const uintptr_t image_base = (uintptr_t)GetModuleHandleW(NULL);
  const uintptr_t instruction =
      context ? (uintptr_t)context->Rip : (uintptr_t)0;
  fprintf(stderr,
          "headless exception: code=0x%08lx address=%p rip=0x%llx "
          "image=0x%llx rva=0x%llx "
          "rsp=0x%llx rbp=0x%llx\n",
          record ? (unsigned long)record->ExceptionCode : 0ul,
          record ? record->ExceptionAddress : NULL,
          context ? (unsigned long long)context->Rip : 0ull,
          (unsigned long long)image_base,
          instruction >= image_base
              ? (unsigned long long)(instruction - image_base)
              : 0ull,
          context ? (unsigned long long)context->Rsp : 0ull,
          context ? (unsigned long long)context->Rbp : 0ull);
  fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void headless_install_exception_filter(void) {
#ifdef _WIN32
  SetUnhandledExceptionFilter(headless_exception_filter);
#endif
}
